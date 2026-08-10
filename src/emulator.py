import sys
import struct
import io
import time
from unicorn import *
from unicorn.arm_const import *

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

class FlashState:
    def __init__(self):
        self.addr = 0
        self.data = b'\xff\xff\xff\xff'

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
    
    # UART Registers
    UART1_FIFO_PORT = 0x0080210c
    UART1_FIFO_STATUS = 0x00802108
    UART2_FIFO_PORT = 0x0080220c
    UART2_FIFO_STATUS = 0x00802208
    
    # ICU Registers
    ICU_INT_STATUS = 0x0080204c
    ICU_INT_RAW_STATUS = 0x00802048
    ICU_INT_ENABLE = 0x00802050
    ICU_GLOBAL_INT_EN = 0x00802044

    def __init__(self, raw_flash, bootloader, app, with_boot=False, only_uart=False):
        self.raw_flash = raw_flash
        self.bootloader = bootloader
        self.app = app
        self.with_boot = with_boot
        self.only_uart = only_uart
        
        self.state = SimulatorState()
        self.flash_state = FlashState()
        
        try:
            self.mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        except UcError as e:
            print("Unicorn error:", e)
            sys.exit(1)
            
    def _print(self, *args, **kwargs):
        if not self.only_uart:
            print(*args, **kwargs)

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

    def hook_code(self, mu, address, size, user_data):
        self.state.insn_count += 1
        
        if self.state.insn_count % 10000 == 0:
            if self.state.icu_int_enable & (1 << 9): # PWM/Timer (Tuya)
                self.state.pwm_status |= (1 << 0) 
            if self.state.icu_int_enable & (1 << 8): # BKTIMER (OpenBK)
                self.state.timer3_5_ctl |= (1 << 7) 
                
        if self.state.pwm_status & 0x3F:
            self.state.pending_irqs |= (1 << 9)
        else:
            self.state.pending_irqs &= ~(1 << 9)
            
        if (self.state.timer0_2_ctl & (0x7 << 7)) or (self.state.timer3_5_ctl & (0x7 << 7)):
            self.state.pending_irqs |= (1 << 8)
        else:
            self.state.pending_irqs &= ~(1 << 8)
            
        if self.state.pending_irqs & self.state.icu_int_enable:
            self.trigger_irq()
                
        if self.state.insn_count % 1000 == 0:
            if (self.state.uart1_int_enable & 0x01) and (self.state.icu_int_enable & (1 << 0)):
                self.state.pending_irqs |= (1 << 0)
                self.trigger_irq()
            if (self.state.uart2_int_enable & 0x01) and (self.state.icu_int_enable & (1 << 1)):
                self.state.pending_irqs |= (1 << 1)
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

        if address == self.UART1_FIFO_PORT or address == self.UART2_FIFO_PORT:
            sys.__stdout__.write(chr(value))
            sys.__stdout__.flush()
            return
            
        if address == 0x00803000:
            self.flash_state.addr = value & 0x00FFFFFF
            op_type = (value >> 28) & 0xF
            if op_type == 6:
                try:
                    self.flash_state.data = mu.mem_read(self.FLASH_BASE + self.flash_state.addr, 4)
                except Exception:
                    self.flash_state.data = b'\xff\xff\xff\xff'
            value = value & ~(1 << 31)
            
        if address == 0x00802c00:
            value = 0
            
        try:
            mu.mem_write(address, struct.pack("<I" if size == 4 else "<H" if size == 2 else "B", value))
        except Exception:
            pass

    def hook_mem_read_mmio(self, mu, access, address, size, value, user_data):
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
            mu.mem_write(address, struct.pack("<I", 1 << 30))
            return
            
        if address == 0xc0008050:
            mu.mem_write(address, b'\x00\x00\x00\x00')
            return
            
        if address == 0x00802c04:
            mu.mem_write(address, struct.pack("<I", 0xFFFFFFFF))
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
            if self.flash_state.addr + 4 <= len(self.raw_flash):
                val = struct.unpack("<I", self.raw_flash[self.flash_state.addr : self.flash_state.addr + 4])[0]
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

        spi_flash_size = (len(self.raw_flash) + 0xFFF) & ~0xFFF
        self.mu.mem_map(self.SPI_FLASH_BASE, spi_flash_size)
        self.mu.mem_write(self.SPI_FLASH_BASE, self.raw_flash)

        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
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
            self.mu.emu_start(vector_base, 0xFFFFFFFF, count=500000000)
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
