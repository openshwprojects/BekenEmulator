import sys
import os
import struct
import io
import time
from unicorn import *
from unicorn.arm_const import *

# Known chip identities served from the SCTRL id registers (0x00800000 chip
# id, 0x00800004 device id), selectable with --chip. Values extracted from
# each OpenBeken firmware's bk_check_chip_id (which hangs forever on a
# mismatch after printing "Unsupported chip or dev"). BK7231T-era firmwares
# do not check these registers at all, so the BK7231 default is safe for them.
CHIP_FAMILIES = {
    "BK7231": (0x0007231A, 0x18520001),   # BK7231T/U family (default)
    "BK7238": (0x00007238, 0x21128000),   # accepts dev 0x2112xxxx or 0x2206xxxx
    "BK7252": (0x0007221A, 0x18221020),   # BK7252 = BK7221U silicon
    "BK7252N": (0x0007252A, 0x23A18000),  # accepts dev 0x23A1xxxx or 0x2431xxxx
}

class SimulatorState:
    def __init__(self):
        self.icu_int_enable = 0
        self.icu_global_int_en = 0
        self.pending_irqs = 0
        self.uart1_int_enable = 0
        self.uart2_int_enable = 0
        self.insn_count = 0
        self.pwm_status = 0
        self.timer0_2_ctl = 0
        self.timer3_5_ctl = 0
        self.efuse_ctrl = 0
        self.saradc_cfg = 0        # shadow of SARADC_ADC_CONFIG (0x802c00)
        self.saradc_pending = 0    # samples waiting in the emulated ADC FIFO

class FlashState:
    def __init__(self):
        self.addr = 0
        self.data = b'\xff\xff\xff\xff'
        self.read_idx = 0   # word index within the current 32-byte flash page read

class BekenEmulator:
    # Hardware base addresses
    FLASH_BASE = 0x00000000
    FLASH_SIZE = 2 * 1024 * 1024
    RAM_BASE = 0x00400000
    RAM_SIZE = 1 * 1024 * 1024
    MMIO_BASE = 0x00800000
    MMIO_SIZE = 0x010000
    PERIPH_BASE = 0xc0000000
    PERIPH_SIZE = 0x100000
    SPI_FLASH_BASE = 0x00200000
    XVR_BASE = 0x00900000
    ACCEL_BASE = 0x00810000
    GPIO_BASE = 0x00802800
    GPIO_END = 0x008028A0
    
    # UART Registers
    UART1_FIFO_PORT = 0x0080210c
    UART1_FIFO_STATUS = 0x00802108
    UART2_FIFO_PORT = 0x0080220c
    UART2_FIFO_STATUS = 0x00802208
    
    # ICU Registers
    ICU_INT_STATUS = 0x0080204c
    ICU_INT_RAW_STATUS = 0x00802048
    ICU_INT_ENABLE = 0x00802040  # ICU_INTERRUPT_ENABLE (0x802050 is ICU_ARM_WAKEUP_EN)
    ICU_GLOBAL_INT_EN = 0x00802044

    def __init__(self, raw_flash, bootloader, app, with_boot=False, only_uart=False, chip_identity=None, uart1_hex=False, physical_flash=None):
        self.raw_flash = raw_flash
        self.bootloader = bootloader
        self.app = app
        self.with_boot = with_boot
        self.only_uart = only_uart
        self.uart1_hex = uart1_hex
        self._uart_src = None   # last UART shown, for interleaving text/hex
        self.chip_id_value, self.device_id_value = chip_identity or CHIP_FAMILIES["BK7231"]
        
        self.state = SimulatorState()
        self.flash_state = FlashState()
        # The unstripped dump. Flash reads are normally served from the
        # CRC-stripped (logical) image, but a 2MB physical dump only yields
        # ~1.88MB of logical space, and some firmware addresses data above
        # that. Keep the physical bytes so such reads can be served.
        self.physical_flash = physical_flash
        
        try:
            self.mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        except UcError as e:
            print("Unicorn error:", e)
            sys.exit(1)
            
    def _print(self, *args, **kwargs):
        if not self.only_uart:
            print(*args, **kwargs)

    def _uart_write(self, data):
        """Emit UART bytes verbatim.

        A UART carries arbitrary bytes, not console text. Writing them through
        sys.stdout in text mode raises UnicodeEncodeError for anything the
        console codepage cannot represent (cp1250 has no 0x96, for instance),
        which killed the whole emulation mid-boot. Go straight to the binary
        buffer so a stray non-ASCII byte in a log stream is just a byte.
        """
        out = sys.__stdout__.buffer
        out.write(data)
        out.flush()

    def trigger_irq(self):
        if self.state.icu_global_int_en == 0:
            return # Global interrupts disabled
            
        cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
        if (cpsr & 0x80) == 0:  # I bit is clear (IRQs enabled)
            pc = self.mu.reg_read(UC_ARM_REG_PC)
            
            # Switch to IRQ mode (0x12), disable IRQs (0x80), clear Thumb bit (0x20)
            new_cpsr = (cpsr & ~0x3F) | 0x92
            self.mu.reg_write(UC_ARM_REG_CPSR, new_cpsr)
            self.mu.reg_write(UC_ARM_REG_SPSR, cpsr)
            self.mu.reg_write(UC_ARM_REG_LR, pc + 4)
            self.mu.reg_write(UC_ARM_REG_PC, 0x00000018)

    def hook_intr(self, mu, intno, user_data):
        pc = mu.reg_read(UC_ARM_REG_PC)
        if intno == 2: # EXCP_SWI
            cpsr = mu.reg_read(UC_ARM_REG_CPSR)
            new_cpsr = (cpsr & ~0x3F) | 0x13 | 0x80
            mu.reg_write(UC_ARM_REG_CPSR, new_cpsr)
            mu.reg_write(UC_ARM_REG_SPSR, cpsr)
            mu.reg_write(UC_ARM_REG_LR, pc)
            mu.reg_write(UC_ARM_REG_PC, 0x00000008)

    # The per-instruction hook is the emulation bottleneck, so the interrupt
    # bookkeeping below only runs every IRQ_CHECK_STRIDE instructions. Pending
    # bits are sticky (cleared by W1C only), so interrupts are delayed by at
    # most IRQ_CHECK_STRIDE - 1 instructions, never lost. The stride must
    # divide the 10000/1000 pacing periods.
    IRQ_CHECK_STRIDE = 20

    def hook_code(self, mu, address, size, user_data):
        state = self.state
        state.insn_count += 1
        if state.insn_count % self.IRQ_CHECK_STRIDE:
            return

        if state.insn_count % 10000 == 0:
            if state.icu_int_enable & (1 << 9): # PWM/Timer (Tuya)
                state.pwm_status |= (1 << 0)
            if state.icu_int_enable & (1 << 8): # BKTIMER (OpenBK)
                state.timer3_5_ctl |= (1 << 7)
            # Pulse the SARADC interrupt (ICU bit 11) while samples pend, paced
            # here - NOT re-raised every stride - so it costs one interrupt per
            # tick period like the timer, never an IRQ storm. The guest's ISR
            # drains the FIFO (each read decrements pending) and the ICU W1C ack
            # retires the pending bit. BK7231N/M block in the per-second
            # temperature read without this; firmwares that never unmask bit 11
            # (T and the 7238/7252 families) are unaffected.
            if state.saradc_pending and (state.icu_int_enable & (1 << 11)):
                state.pending_irqs |= (1 << 11)

        if state.pwm_status & 0x3F:
            state.pending_irqs |= (1 << 9)
        else:
            state.pending_irqs &= ~(1 << 9)

        if (state.timer0_2_ctl & (0x7 << 7)) or (state.timer3_5_ctl & (0x7 << 7)):
            state.pending_irqs |= (1 << 8)
        else:
            state.pending_irqs &= ~(1 << 8)

        if state.pending_irqs & state.icu_int_enable:
            self.trigger_irq()

        if state.insn_count % 1000 == 0:
            if (state.uart1_int_enable & 0x01) and (state.icu_int_enable & (1 << 0)):
                state.pending_irqs |= (1 << 0)
                self.trigger_irq()
            if (state.uart2_int_enable & 0x01) and (state.icu_int_enable & (1 << 1)):
                state.pending_irqs |= (1 << 1)
                self.trigger_irq()

    def hook_mem_write_mmio(self, mu, access, address, size, value, user_data):
        if address == self.ICU_INT_ENABLE:
            self.state.icu_int_enable = value
            self._print(f"[DEBUG] ICU_INT_ENABLE set to: 0x{value:08x}")
        if address == self.ICU_GLOBAL_INT_EN: self.state.icu_global_int_en = value
        if address == self.ICU_INT_STATUS: self.state.pending_irqs &= ~value
        if address == 0x00802110: self.state.uart1_int_enable = value
        if address == 0x00802210: self.state.uart2_int_enable = value

        if address == 0x00802A04:
            w1c_mask = value & 0x3F
            self.state.pwm_status = (self.state.pwm_status & ~0x3F) | (value & ~0x3F)
            self.state.pwm_status &= ~w1c_mask
            
        if address == 0x00802A0C:
            w1c_mask = value & (0x7 << 7)
            self.state.timer0_2_ctl = (value & ~(0x7 << 7)) | (self.state.timer0_2_ctl & (0x7 << 7))
            self.state.timer0_2_ctl &= ~w1c_mask
            
        if address == 0x00802A4C:
            w1c_mask = value & (0x7 << 7)
            self.state.timer3_5_ctl = (value & ~(0x7 << 7)) | (self.state.timer3_5_ctl & (0x7 << 7))
            self.state.timer3_5_ctl &= ~w1c_mask

        # UART output. UART2 is the firmware's debug/log port (text). UART1 is
        # the TuyaMCU link to the external device MCU; with --uart1-hex it is
        # shown as tagged hex so the 55 AA protocol frames are readable instead
        # of printing as garbage characters mixed into the log.
        if address == self.UART1_FIFO_PORT:
            if self.uart1_hex:
                if self._uart_src != 'u1':
                    self._uart_write(b'\n[UART1/MCU] ')
                    self._uart_src = 'u1'
                self._uart_write(b'%02x ' % (value & 0xFF))
            else:
                self._uart_write(bytes([value & 0xFF]))
            return
        if address == self.UART2_FIFO_PORT:
            if self.uart1_hex and self._uart_src == 'u1':
                self._uart_write(b'\n')
                self._uart_src = 'u2'
            self._uart_write(bytes([value & 0xFF]))
            return
            
        # REG_FLASH_CONF. CRC_EN (bit 26) selects whether flash reads go through
        # the controller's 32-data + 2-CRC framing. The emulator always serves
        # the stripped image, so this is only observed, not acted on - logging it
        # shows whether firmware ever switches to raw physical addressing.
        if address == 0x0080301C and os.environ.get("FLASHDBG"):
            sys.__stderr__.write("FLASHDBG conf=0x%08x CRC_EN=%d\n" % (value, (value >> 26) & 1))
            sys.__stderr__.flush()

        if address == 0x00803000:
            self.flash_state.addr = value & 0x00FFFFFF
            self.flash_state.read_idx = 0   # new page read starts at word 0
            op_type = (value >> 28) & 0xF
            if os.environ.get("FLASHDBG") and self.flash_state.addr >= 0x001E0000:
                sys.__stderr__.write("FLASHDBG op addr=0x%06x reg=0x%08x\n"
                                     % (self.flash_state.addr, value))
                sys.__stderr__.flush()
            if op_type == 6:
                try:
                    self.flash_state.data = mu.mem_read(self.FLASH_BASE + self.flash_state.addr, 4)
                except Exception:
                    self.flash_state.data = b'\xff\xff\xff\xff'
            value = value & ~(1 << 31)
            
        # SARADC_ADC_CONFIG (BK7231N layout, verified against the OpenBK7231N
        # SDK saradc.c). A write with CHNL_EN (bit 2) set starts a conversion;
        # fill the emulated FIFO with a batch of samples. The driver's ISR /
        # poll loops read DAT_AFTER_STA until FIFO_EMPTY (config bit 30) is set,
        # so each sample read drains one. Arming only from idle (pending==0)
        # avoids an ever-full FIFO. BK7231N's temp read blocks forever without
        # this - it waits on the SARADC interrupt (ICU bit 11).
        if address == 0x00802c00:
            self.state.saradc_cfg = value
            # Only the BK7231(T/U/N/M) SARADC layout uses this model; the
            # 7238/7252/7252N layout differs and is served via 0x802c0c.
            if (self.chip_id_value == 0x0007231A
                    and (value & (1 << 2))          # CHNL_EN starts a conversion
                    and self.state.saradc_pending == 0):
                self.state.saradc_pending = 32

        # SCTRL_EFUSE_CTRL: EFUSE_OPER_EN (bit 0) self-clears when the efuse
        # operation completes; complete it instantly or sctrl_read_efuse spins
        # forever on it. Kept in shadow state because Unicorn commits the
        # original value to memory after this hook, so reads are served from
        # the shadow in hook_mem_read_mmio.
        if address == 0x00800074:
            self.state.efuse_ctrl = value & ~1


        try:
            mu.mem_write(address, struct.pack("<I" if size == 4 else "<H" if size == 2 else "B", value))
        except Exception:
            pass

    def hook_mem_read_mmio(self, mu, access, address, size, value, user_data):
        # SCTRL identity registers; served per the selected chip family.
        if address == 0x00800000:
            mu.mem_write(address, struct.pack("<I", self.chip_id_value))
            return
        if address == 0x00800004:
            mu.mem_write(address, struct.pack("<I", self.device_id_value))
            return
        # GPIO config registers (0x802800 + pin*4). Bit 0 is the input level,
        # which real silicon drives; with nothing connected the pull decides it.
        # Model that: a pin with the pull enabled reads back high for pull-up
        # and low for pull-down. Without this, bit 0 always reads 0 and any
        # firmware waiting for a pulled-up pin spins forever (BK7231U's
        # RT-Thread build does exactly that before printing anything).
        if self.GPIO_BASE <= address < self.GPIO_END:
            try:
                cfg = struct.unpack("<I", mu.mem_read(address, 4))[0]
            except Exception:
                cfg = 0
            if cfg & (1 << 5):  # GCFG_PULL_ENABLE
                if cfg & (1 << 4):  # GCFG_PULL_MODE: 1 = up
                    cfg |= 1
                else:
                    cfg &= ~1
            mu.mem_write(address, struct.pack("<I", cfg))
            return

        if address == 0x00802A04:
            mu.mem_write(address, struct.pack("<I", self.state.pwm_status))
            return
        if address == 0x00802A0C:
            mu.mem_write(address, struct.pack("<I", self.state.timer0_2_ctl))
            return
        if address == 0x00802A4C:
            mu.mem_write(address, struct.pack("<I", self.state.timer3_5_ctl))
            return

        if address == 0x00802c00:
            # Config readback: FIFO_EMPTY (bit 30) reflects the emulated FIFO -
            # set when drained, clear while samples pend so the ISR's
            # `while((cfg & FIFO_EMPTY)==0)` loop reads them. INT_CLR (bit 8) is
            # deliberately never reflected, so saradc_int_clr()'s do/while exits.
            # Identical to the historical constant (1<<30) whenever idle.
            cfg = 0 if (self.chip_id_value == 0x0007231A
                        and self.state.saradc_pending) else (1 << 30)
            mu.mem_write(address, struct.pack("<I", cfg))
            return

        # BK7238/BK7252N SARADC state register (SARADC_ADC_STATE). Its driver
        # drains the sample FIFO with `while(!(STATE & FIFO_EMPTY)) read DATA;`
        # spin loops during init; report FIFO_EMPTY (bit 30) so they terminate
        # immediately. Actual sampling is interrupt-driven, never polled here.
        if address == 0x00802c0c:
            mu.mem_write(address, struct.pack("<I", 1 << 30))
            return

        # SCTRL_EFUSE_CTRL readback: shadow with EFUSE_OPER_EN self-cleared.
        if address == 0x00800074:
            mu.mem_write(address, struct.pack("<I", self.state.efuse_ctrl))
            return

        # XVR (RF transceiver) transaction register. RF init sets bit 31 to
        # start a transfer and spins until hardware clears it; report it always
        # complete (bit 31 low) so the wait loop exits.
        if address == 0x00900100:
            mu.mem_write(address, struct.pack("<I", 0))
            return

        # Accelerator block just past the main MMIO window (0x810000) - a
        # crypto/hash engine used by original Tuya firmware during TCP/IP init.
        # Its status register 0x810000 and trigger 0x81001c are polled for busy
        # bits (bit 31 / bit 30) after each operation; report every operation
        # already complete (all bits clear) so the wait loops exit. There is no
        # real result to compute here (the emulator has no network), so the
        # sequence just needs to unblock.
        if address == 0x00810000 or address == 0x0081001c:
            mu.mem_write(address, struct.pack("<I", 0))
            return

        # SCTRL_EFUSE_OPTR: report a blank efuse byte (0x00) with
        # EFUSE_OPER_RD_DATA_VALID (bit 8) set.
        if address == 0x00800078:
            mu.mem_write(address, struct.pack("<I", 1 << 8))
            return

        if address == 0xc0008050:
            mu.mem_write(address, b'\x00\x00\x00\x00')
            return
            
        if address == 0x00802c04 or address == 0x00802c10:
            # SARADC sample registers: DATA (0x802c04) and DAT_AFTER_STA
            # (0x802c10, the one BK7231N's ISR actually reads). Each read
            # consumes one pending FIFO sample and returns a plausible raw value
            # (330 -> roughly +30C through the BK7231N temperature formula).
            if self.state.saradc_pending:
                self.state.saradc_pending -= 1
            mu.mem_write(address, struct.pack("<I", 330))
            return

        if address == 0x00803004:
            if hasattr(self.flash_state, 'data'):
                mu.mem_write(address, self.flash_state.data)
            return
            
        if address == self.ICU_INT_STATUS or address == self.ICU_INT_RAW_STATUS:
            val = self.state.pending_irqs & self.state.icu_int_enable
            mu.mem_write(address, struct.pack("<I", val))
            return
            
        if address == self.ICU_INT_ENABLE:
            mu.mem_write(address, struct.pack("<I", self.state.icu_int_enable))
            return
            
        if address == self.ICU_GLOBAL_INT_EN:
            mu.mem_write(address, struct.pack("<I", self.state.icu_global_int_en))
            return

        if address == 0x00802110:
            mu.mem_write(address, struct.pack("<I", self.state.uart1_int_enable))
            return
            
        if address == 0x00802210:
            mu.mem_write(address, struct.pack("<I", self.state.uart2_int_enable))
            return

        if address == self.UART1_FIFO_STATUS or address == self.UART2_FIFO_STATUS:
            mu.mem_write(address, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))
            return

        if address == 0x00803008:
            # REG_FLASH_DATA_FLASH_SW. The SDK's flash_read_data reads this
            # register 8 times after one operate-write to pull a whole 32-byte
            # page (buf[8]); serve successive words so bulk reads (e.g. the
            # net_param config partition) get the full page, not word 0 repeated.
            word_addr = self.flash_state.addr + self.flash_state.read_idx * 4
            self.flash_state.read_idx += 1
            if word_addr + 4 <= len(self.raw_flash):
                val = struct.unpack("<I", self.raw_flash[word_addr : word_addr + 4])[0]
            elif self.physical_flash is not None and word_addr + 4 <= len(self.physical_flash):
                # Past the end of logical (CRC-stripped) space. The dump still has
                # real bytes there, so serve them rather than erased flash.
                val = struct.unpack("<I", self.physical_flash[word_addr : word_addr + 4])[0]
            else:
                val = 0xFFFFFFFF
            mu.mem_write(address, struct.pack("<I", val))
            return

        if address in [self.UART1_FIFO_STATUS, self.UART2_FIFO_STATUS, 0x00802c00]:
            return
        
        try:
            mem_val = mu.mem_read(address, size)
            val = struct.unpack("<I" if size == 4 else "<H" if size == 2 else "B", mem_val)[0]
            self._print(f"[MMIO] Read 0x{val:08x} from 0x{address:08x}")
        except Exception:
            pass

    def hook_unmapped(self, mu, type, address, size, value, user_data):
        self._print(f"[!] Unmapped access at 0x{address:08x} (type={type})")
        page = address & ~0xFFF
        try:
            if type == UC_MEM_READ_UNMAPPED:
                mu.mem_map(page, 0x1000)
                mu.mem_write(page, b"\x00" * 0x1000)
                return True
            elif type == UC_MEM_WRITE_UNMAPPED:
                mu.mem_map(page, 0x1000)
                return True
        except Exception as e:
            self._print(f"Failed to map page 0x{page:08x}: {e}")
        return False

    def setup(self):
        self.mu.mem_map(self.FLASH_BASE, self.FLASH_SIZE)
        self.mu.mem_map(self.RAM_BASE, self.RAM_SIZE)
        self.mu.mem_map(self.MMIO_BASE, self.MMIO_SIZE)
        self.mu.mem_map(self.PERIPH_BASE, self.PERIPH_SIZE)
        
        self.mu.mem_write(self.FLASH_BASE, self.raw_flash[:self.FLASH_SIZE])
        
        if self.bootloader:
            self.mu.mem_write(0x00000000, self.bootloader)
        if self.app:
            self.mu.mem_write(0x00010000, self.app)

        self.mu.mem_write(self.UART1_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))
        self.mu.mem_write(self.UART2_FIFO_STATUS, struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20))

        SARADC_ADC_CONFIG = 0x00802c00
        self.mu.mem_write(SARADC_ADC_CONFIG, struct.pack("<I", 1 << 28 | 1 << 30))
        self.mu.mem_write(0x00802900, struct.pack("<I", 0))

        # Mirror the raw flash at the SPI-flash window. Cap the size so it can
        # never run into the RAM region: dumps larger than 2MB (e.g. 4MB BK7238
        # plug images) would otherwise map past RAM_BASE and fail with UC_ERR_MAP
        # before any code runs. Code/data live in the low 2MB, so the upper part
        # of the mirror is not needed for boot.
        spi_flash_size = (len(self.raw_flash) + 0xFFF) & ~0xFFF
        spi_flash_size = min(spi_flash_size, self.RAM_BASE - self.SPI_FLASH_BASE)
        self.mu.mem_map(self.SPI_FLASH_BASE, spi_flash_size)
        self.mu.mem_write(self.SPI_FLASH_BASE, self.raw_flash[:spi_flash_size])

        # BK7238/BK7252N XVR (RF transceiver) register block. Not in the main
        # MMIO window; map and hook it so the RF-init transaction trigger at
        # 0x900100 can be modelled as always-complete (see hook_mem_read_mmio).
        self.mu.mem_map(self.XVR_BASE, 0x1000)
        # Accelerator block just past the main MMIO window (0x810000); mapped
        # and hooked so the 0x81001c transaction trigger can be served.
        self.mu.mem_map(self.ACCEL_BASE, 0x1000)

        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.XVR_BASE, end=self.XVR_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.XVR_BASE, end=self.XVR_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self.hook_unmapped)
        self.mu.hook_add(UC_HOOK_CODE, self.hook_code)
        self.mu.hook_add(UC_HOOK_INTR, self.hook_intr)

    def run(self):
        if self.with_boot:
            vector_base = 0x00000000
            self._print(f"Booting from bootloader at: 0x{vector_base:08x}")
        else:
            vector_base = 0x00010000
            try:
                val = struct.unpack("<I", self.mu.mem_read(vector_base, 4))[0]
                if val == 0xFFFFFFFF or val == 0x94b5072f: 
                    vector_base = 0x00011000
            except Exception:
                pass
            self._print(f"Detected app vector table at: 0x{vector_base:08x}")

        if self.app:
            self._print("App header:", self.app[:32].hex())
        
        self._print(f"Starting emulation at 0x{vector_base:08x}...")
        start_time = time.time()
        try:
            self.mu.emu_start(vector_base, 0xFFFFFFFF, count=1000000000)
        except UcError as e:
            pc = self.mu.reg_read(UC_ARM_REG_PC)
            cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
            print(f"\n[!] Emulation finished with error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
        except Exception as e:
            pc = self.mu.reg_read(UC_ARM_REG_PC)
            cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
            print(f"\n[!] Emulation finished with python error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
        except KeyboardInterrupt:
            print("\nEmulation stopped by user.")

        pc = self.mu.reg_read(UC_ARM_REG_PC)
        cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
        elapsed = time.time() - start_time
        print(f"Emulation finished. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x} (T-bit: {(cpsr & 0x20) >> 5})", flush=True)
        print(f"Time taken: {elapsed:.2f} seconds.")
        print("\nDone.")
