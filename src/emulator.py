import sys
import os
import struct
import io
import time
from unicorn import *
from unicorn.arm_const import *

# TuyaMCU MCU-peer simulator (optional). Imported defensively so emulator.py
# works whether it is loaded as src.emulator (via main.py) or as a top-level
# module with src/ on the path.
try:
    from . import tuyamcu as _tuyamcu
except ImportError:
    import tuyamcu as _tuyamcu

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
        self.last_slow = 0         # insn_count at last ~10000-insn (timer) tick
        self.last_fast = 0         # insn_count at last ~1000-insn (UART) tick
        self.last_emit = 0         # insn_count at last [EMU_INSNS] report line
        # Peripheral state observed during the run, for the report's GPIO tab.
        # Both live in MMIO ranges the write hook already covers, and a boot
        # performs only tens of GPIO writes (a couple of thousand PWM ones),
        # so recording them costs nothing measurable.
        self.gpio_cfg = {}         # pin -> last value written to its config reg
        self.pwm_regs = {}         # register offset -> last value written
        # Keys changed since the last report emission. The snapshot used to go
        # out only on the 1,000,000-instruction [EMU_INSNS] tick, which races
        # the harness: it stops as soon as its log markers match, so a register
        # written after the last tick was never reported and an assertion saw a
        # stale value. Tracking dirty keys lets the fast tick emit just what
        # changed, which removes the race and prints less than the snapshot.
        self.gpio_dirty = set()
        self.pwm_dirty = set()
        # True while stdout sits at a line boundary. Report lines may only be
        # written then, so they never land inside a UART log line.
        self.uart_at_line_start = True
        # UART1 (the TuyaMCU serial link) byte log for the report's dedicated
        # UART1 tab: a list of (insn_count, 'T'|'R', byte). 'T' = the device
        # transmitted it (device -> MCU); 'R' = it was delivered to the device
        # (MCU -> device, e.g. --uart1-rx). Drained on the same line-start tick
        # as gpio/pwm below and filtered out of the log by the harness. Only
        # populated when EMU_REPORT is set, so a normal run pays nothing.
        self.uart1_events = []
        # XVR RF/BLE busy registers (0x9000F8, 0x900000) the firmware has armed
        # by writing bit 31; the next read of an armed one reports the bit clear
        # ("op done") and disarms. Only used under the opt-in --xvr-selfclear.
        self.xvr_armed = set()

class FlashState:
    def __init__(self):
        self.addr = 0
        self.data = b'\xff\xff\xff\xff'
        self.read_idx = 0   # word index within the current 32-byte flash page read
        self.write_buf = []  # words staged via REG_FLASH_DATA_SW_FLASH for a page program

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
    PWM_BASE = 0x00802A00
    GPIO_BASE = 0x00802800
    GPIO_END = 0x008028A0
    # Capture-only upper bound for the WRITE hook. Wider than GPIO_END so the
    # function-select registers REG_GPIO_FUNC_CFG_2/_3 (0x8028B8/0x8028BC) are
    # recorded - they carry the P24/P26 pin-mux on BK7231N. GPIO_END itself
    # must stay at 0x8028A0: the READ hook pull-simulates bit 0 for everything
    # below it, which would corrupt read-modify-writes of a mux register.
    GPIO_CAP_END = 0x008028C0
    
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

    def __init__(self, raw_flash, bootloader, app, with_boot=False, only_uart=False, chip_identity=None, uart1_hex=False, physical_flash=None, uart1_rx=b"", uart1_rx_delay=2_000_000, tuyamcu_enabled=False, tuyamcu_pid=None, tuyamcu_raw=False, xvr_selfclear=False, tuyamcu_dps=None, tuyamcu_injects=None):
        self.raw_flash = raw_flash
        self.xvr_selfclear = xvr_selfclear
        self.bootloader = bootloader
        self.app = app
        self.with_boot = with_boot
        self.only_uart = only_uart
        self.uart1_hex = uart1_hex
        # Bytes to feed into UART1's receive FIFO, as if a host typed them. The
        # firmware only drains this once it has enabled UART1 RX (OBK does that
        # in CMD_UARTConsole_Init when OBK_FLAG_CMD_ACCEPT_UART_COMMANDS is set),
        # so queued bytes simply wait until the console is up. Reads of the RX
        # FIFO port dequeue from here; the FIFO status reports "data ready" while
        # it is non-empty, and the existing UART1 RX interrupt drives the ISR
        # that copies these into OBK's command ring buffer.
        self.uart1_rx = bytearray(uart1_rx)
        # Hold the RX bytes back until the boot has run this many instructions.
        # OBK enables UART1 RX very early (~140k insns, for the Wi-Fi/early UART
        # path) but only registers its console receive callback about a million
        # instructions later, when CMD_Init_Delayed runs. Delivering bytes in
        # that window is pointless: the UART ISR, finding no callback, drains
        # the FIFO and throws them away. Waiting also matches reality - a human
        # types a command after the device has finished booting, not during its
        # UART init. Tunable; the default clears the observed console-init point
        # with margin.
        self.uart1_rx_delay = uart1_rx_delay
        # Optional simulated MCU on the other end of UART1. When enabled, every
        # TuyaMCU frame the firmware transmits is fed to this peer, and its
        # replies are appended to uart1_rx above - so the same RX FIFO / RX
        # interrupt path that carries a typed console command also carries an
        # MCU's answers. This is what lets a TuyaMCU dump advance past its
        # heartbeat loop (which stalls forever with nothing attached).
        # A real Tuya module compares the product key the MCU reports against
        # the one in its own license and rejects a mismatch, so tuyamcu_pid lets
        # the peer answer with the device's actual 16-char product id. TuyaOS
        # 3.x additionally wants that id as a raw 16-byte payload rather than
        # JSON (it rejects other lengths with "prod len = .."), which tuyamcu_raw
        # selects - the caller knows which form a given dump expects, exactly as
        # the real MCU wired to it would.
        if tuyamcu_enabled:
            kw = {}
            if tuyamcu_pid:
                kw["product_key"] = tuyamcu_pid
            if tuyamcu_raw:
                kw["raw_product"] = True
            if tuyamcu_dps:
                # {dp_id: (dp_type, value)} reported for a query-state (0x08),
                # so a device advances past working-mode into DP reporting.
                kw["dps"] = tuyamcu_dps
            if tuyamcu_injects:
                # Raw byte blobs the peer sends unprompted after the handshake,
                # so the link is not only "firmware asks, MCU answers" - the
                # firmware's own reply and error paths get exercised too.
                kw["injects"] = tuyamcu_injects
            self.tuyamcu = _tuyamcu.TuyaMCUSlave(**kw)
        else:
            self.tuyamcu = None
        self._uart_src = None   # last UART shown, for interleaving text/hex
        # When EMU_REPORT is set (the self-test harness does this), periodically
        # emit the approximate executed-instruction count on a distinct
        # [EMU_INSNS] line so the report can show it. Off by default so normal
        # runs are not polluted, and the emit is throttled + piggybacks the
        # existing slow tick, so it never touches the per-block hot path.
        self._emit_insns = bool(os.environ.get("EMU_REPORT"))
        self.chip_id_value, self.device_id_value = chip_identity or CHIP_FAMILIES["BK7231"]
        # BK7252 (BK7221U silicon, the A9-style WiFi cameras) is the odd one
        # out of the Beken family: it has external SDRAM at 0x00900000, and its
        # RT-Thread firmware puts the whole system heap there
        # (rt_system_heap_init(RT_HW_SDRAM_BEGIN=0x900000, +256K)). On every
        # other supported chip 0x900000 is the XVR register block instead. So
        # this flag switches that window between "RAM" and "XVR registers".
        self.is_bk7252 = (self.chip_id_value == CHIP_FAMILIES["BK7252"][0])

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

    def _flash_backing(self):
        """The image flash reads are served from, as a mutable buffer.

        Addresses are physical, so writes land in the unstripped dump when we
        have it (see the 0x803008 read handler for why).
        """
        if self.physical_flash is not None:
            if not isinstance(self.physical_flash, bytearray):
                self.physical_flash = bytearray(self.physical_flash)
            return self.physical_flash
        if not isinstance(self.raw_flash, bytearray):
            self.raw_flash = bytearray(self.raw_flash)
        return self.raw_flash

    def _flash_program(self, addr, words):
        """Page program: AND the staged words into flash (NOR bits only clear)."""
        if not words:
            return
        buf = self._flash_backing()
        data = b"".join(struct.pack("<I", w) for w in words)
        base = addr & ~0x1F                      # programs are page aligned
        for i, byte in enumerate(data):
            pos = base + i
            if pos < len(buf):
                # Real NOR flash can only turn 1 bits into 0; an erase is what
                # restores them. Modelling that keeps program-without-erase
                # behaving as the firmware expects rather than silently working.
                buf[pos] &= byte

    def _flash_erase(self, addr, size):
        """Sector/block erase: restore the region to erased (0xFF)."""
        buf = self._flash_backing()
        start = addr & ~(size - 1)
        end = min(start + size, len(buf))
        if start < end:
            buf[start:end] = b"\xff" * (end - start)

    def _rx_active(self):
        """True while queued UART1 RX bytes should be delivered to the guest."""
        return bool(self.uart1_rx) and self.state.insn_count >= self.uart1_rx_delay

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
        # Remember whether the stream is mid-line. The report's peripheral
        # lines share this stdout, and injecting one between two UART bytes
        # splits the log line in half - which silently broke an asserted
        # marker string once the emission rate went up.
        if data:
            self.state.uart_at_line_start = data.endswith(b"\n")

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

    # Interrupt/pacing bookkeeping runs once per BASIC BLOCK, not per
    # instruction. A per-instruction UC_HOOK_CODE callback dominated runtime
    # (measured ~3.7x slower than a block hook on a real boot): Unicorn must
    # leave the translated block to call into Python for every instruction,
    # which defeats block execution. UC_HOOK_BLOCK fires once per ~5
    # instructions, so whole blocks run natively between callbacks.
    #
    # insn_count is therefore an ESTIMATE: block byte size >> 2, i.e. the ARM
    # instruction width. Pure-ARM code counts exactly; Thumb blocks undercount
    # (slightly slowing the device clock there). Device time is already a
    # fiction paced off this counter, so the approximation only nudges the
    # wall<->device-time ratio - it never changes what the firmware computes.
    # The periodic sources fire on threshold crossing (>= since last tick)
    # rather than exact modulo, because the per-block increment is variable.
    IRQ_CHECK_STRIDE = 20  # retained for reference; pacing is now per-block

    def hook_block(self, mu, address, size, user_data):
        state = self.state
        state.insn_count += (size >> 2) or 1

        # Slow tick (~every 10000 insns): raise the periodic sources' pending.
        if state.insn_count - state.last_slow >= 10000:
            state.last_slow = state.insn_count
            # Report-only: emit the running instruction estimate a few times per
            # boot. Guarded by _emit_insns (off unless the harness asks) and
            # throttled, so this costs nothing on a normal run.
            if self._emit_insns:
                # Peripheral changes go out on THIS tick (~every 10000 insns)
                # rather than the 1M-instruction one, and only for registers
                # that actually changed. The emulator never exits on its own -
                # it is killed once the harness has what it needs - so whatever
                # has not been emitted by then is simply lost. The harness
                # filters these lines out of the displayed log.
                if ((state.gpio_dirty or state.pwm_dirty or state.uart1_events)
                        and state.uart_at_line_start):
                    buf = sys.__stdout__.buffer
                    for pin in sorted(state.gpio_dirty):
                        buf.write(b"[EMU_GPIO] %d %08x\n" % (pin, state.gpio_cfg[pin]))
                    for off in sorted(state.pwm_dirty):
                        buf.write(b"[EMU_PWM] %03x %08x\n" % (off, state.pwm_regs[off]))
                    # One line per UART1 byte: insn timestamp, direction, value.
                    # The report coalesces consecutive same-direction bytes into
                    # Sent:/Recv: frames; keeping raw bytes here means no framing
                    # decision is baked into the capture.
                    for _t, _d, _b in state.uart1_events:
                        buf.write(b"[EMU_UART1] %d %c %02x\n" % (_t, ord(_d), _b))
                    state.gpio_dirty.clear()
                    state.pwm_dirty.clear()
                    state.uart1_events.clear()
                    buf.flush()
                if state.insn_count - state.last_emit >= 1_000_000:
                    state.last_emit = state.insn_count
                    buf = sys.__stdout__.buffer
                    buf.write(b"\n[EMU_INSNS] %d\n" % state.insn_count)
                    buf.flush()
            if state.icu_int_enable & (1 << 9): # PWM/Timer (Tuya)
                state.pwm_status |= (1 << 0)
            if state.icu_int_enable & (1 << 8): # BKTIMER (OpenBK)
                state.timer3_5_ctl |= (1 << 7)
            # Pulse the SARADC interrupt (ICU bit 11) while samples pend, paced
            # here - NOT re-raised every block - so it costs one interrupt per
            # tick period like the timer, never an IRQ storm. The generic
            # `pending_irqs & icu_int_enable` check below delivers it. Raising
            # it on the faster 1000-instruction tick was measured to be a net
            # loss: the extra interrupt overhead slowed device-time progress
            # (4 fewer device-seconds per 400s run) without unblocking anything.
            # The guest's ISR drains the FIFO (each read decrements pending) and
            # the ICU W1C ack retires the pending bit. BK7231N/M block in the
            # per-second temperature read without this; firmwares that never
            # unmask bit 11 (T and the 7238/7252 families) are unaffected.
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

        # Fast tick (~every 1000 insns): UART RX/TX and SARADC interrupts.
        if state.insn_count - state.last_fast >= 1000:
            state.last_fast = state.insn_count
            # UART1 interrupt: the firmware's TX path arms bit 0
            # (TX_FIFO_NEED_WRITE_EN); the RX console arms bit 1
            # (RX_FIFO_NEED_READ_EN) / bit 6 (RX_STOP_END_EN). Fire for TX
            # always-ready, and for RX only while we actually have queued bytes
            # to deliver - otherwise a spurious RX interrupt storm.
            if state.icu_int_enable & (1 << 0):
                rx_pending = self._rx_active() and (state.uart1_int_enable & 0x42)
                if (state.uart1_int_enable & 0x01) or rx_pending:
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
        # Arm the XVR RF/BLE busy self-clear (opt-in) when the firmware sets bit
        # 31 to start an operation. The write still lands in mapped memory.
        if self.xvr_selfclear and address in (0x009000f8, 0x00900000) and (value & 0x80000000):
            self.state.xvr_armed.add(address)

        # GPIO config registers - one 32-bit word per pin at GPIO_BASE + n*4.
        # Bit 1 is the driven output level (GCFG_OUTPUT), bit 3 output-enable.
        if self.GPIO_BASE <= address < self.GPIO_CAP_END:
            _pin = (address - self.GPIO_BASE) // 4
            self.state.gpio_cfg[_pin] = value & 0xFFFFFFFF
            if self._emit_insns:
                self.state.gpio_dirty.add(_pin)

        # PWM block. pwm.h places PWM_BASE at either PWM_NEW_BASE (0x802A00)
        # or PWM_NEW_BASE + 0x80 depending on a build switch, so capture the
        # whole window and let the report show offsets rather than guess which
        # layout an image uses. Recorded for the report only.
        # Window covers BOTH PWM generations: the old block at 0x802A00
        # (BK7231T; period/duty pairs) and pwm_new at 0x802B00 (BK7231N;
        # three groups of 0x40 with T1..T4 edge registers). Offsets 0x100+
        # in pwm_regs are therefore pwm_new registers. On 7252-family chips
        # 0x802B00 is the audio block instead, so the DECODER gates on chip
        # identity - capture here just records writes.
        if self.PWM_BASE <= address < self.PWM_BASE + 0x1C0:
            _off = address - self.PWM_BASE
            self.state.pwm_regs[_off] = value & 0xFFFFFFFF
            if self._emit_insns:
                self.state.pwm_dirty.add(_off)

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
            if self._emit_insns:
                self.state.uart1_events.append((self.state.insn_count, 'T', value & 0xFF))
            if self.tuyamcu is not None:
                # Feed this transmitted byte to the simulated MCU; when it has a
                # whole frame it answers, and the answer is queued for the guest
                # to read back over UART1 RX (delivered by the RX interrupt path).
                reply = self.tuyamcu.react_stream(bytes([value & 0xFF]))
                if reply:
                    self.uart1_rx += reply
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

        # REG_FLASH_DATA_SW_FLASH: words staged for the next page program.
        # flash_write_data() pushes eight of them (one 32-byte page) before
        # triggering the PP opcode below.
        if address == 0x00803004:
            self.flash_state.write_buf.append(value & 0xFFFFFFFF)
            del self.flash_state.write_buf[:-8]      # keep only the last page
            mu.mem_write(address, struct.pack("<I", value))
            return

        if address == 0x00803000:
            self.flash_state.addr = value & 0x00FFFFFF
            self.flash_state.read_idx = 0   # new page read starts at word 0
            # Opcode field is 5 bits at OP_TYPE_SW_POSI (24) - see flash.h. The
            # previous (value >> 28) & 0xF never matched a real opcode, so page
            # programs and sector erases were silently ignored: firmware that
            # persists data at boot then read back stale bytes, failed its own
            # verify ("Err write addr:...") and rebooted in a loop.
            op_type = (value >> 24) & 0x1F
            if os.environ.get("FLASHDBG") and self.flash_state.addr >= 0x001E0000:
                sys.__stderr__.write("FLASHDBG op addr=0x%06x op=%d reg=0x%08x\n"
                                     % (self.flash_state.addr, op_type, value))
                sys.__stderr__.flush()

            if op_type == 12:        # FLASH_OPCODE_PP - program one 32-byte page
                self._flash_program(self.flash_state.addr, self.flash_state.write_buf)
            elif op_type == 13:      # FLASH_OPCODE_SE - erase one 4K sector
                self._flash_erase(self.flash_state.addr, 0x1000)
            elif op_type in (14, 15):  # FLASH_OPCODE_BE1 / BE2 - block erase
                self._flash_erase(self.flash_state.addr, 0x10000)

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
            if os.environ.get("ADCDBG"):
                sys.__stderr__.write(
                    "ADCDBG cfg write=0x%08x chnl_en=%d chip=%#x\n"
                    % (value, (value >> 2) & 1, self.chip_id_value))
                sys.__stderr__.flush()
            # Only the BK7231(T/U/N/M) SARADC layout uses this model; the
            # 7238/7252/7252N layout differs and is served via 0x802c0c.
            if self.chip_id_value == 0x0007231A:
                if value & (1 << 2):                # CHNL_EN: start converting
                    if self.state.saradc_pending == 0:
                        self.state.saradc_pending = 32
                else:
                    # CHNL_EN cleared - the channel is stopped (saradc_pause /
                    # ddev_close / saradc_ensure_close all end up here), so the
                    # sample FIFO is flushed with it.
                    #
                    # This matters: saradc_isr drains at most data_buff_size
                    # samples (10 for BK7231N temperature) and then breaks,
                    # leaving the rest queued. Without flushing on stop, those
                    # leftovers kept saradc_pending non-zero, so the emulator
                    # re-asserted the SARADC interrupt forever - an interrupt
                    # storm that starved the temp_detect thread, filled its
                    # queue ("temp_detect_send_msg failed") and wedged the boot.
                    self.state.saradc_pending = 0

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

        # XVR RF/BLE busy registers 0x9000F8 (RF-cal) and 0x900000 (BLE
        # llm_init). Both use the identical "set bit 31, spin until hardware
        # clears it" busy pattern, at app PCs ~0x93F1C and ~0x93BD0:
        #     ldr r1,[r]; orr r2,r1,#0x80000000; str r2,[r]   ; set bit 31
        #     ldr r2,[r]; <test bit 31>; branch-if-still-set  ; wait bit31==0
        # Serving them is OPT-IN (--xvr-selfclear), NOT default: dumps that do
        # real BLE init (TempHum/Plug/zmai90) also write and read bit 31 here but
        # for the OPPOSITE meaning and hang if it is cleared - the two uses are
        # irreconcilable, so a per-dump flag is the only safe way. When set,
        # model bit 31 as self-clearing, and only after the firmware itself set
        # it (state.xvr_armed). Measured to walk the stock 2.x / ATORCH (2.1.17)
        # RF and BLE init past BOTH spins all the way to the TuyaMCU link.
        # Disassembly + measurements: scratch/ble_stall.py, scratch/ble_crack.py.
        if self.xvr_selfclear and address in (0x009000f8, 0x00900000):
            if address in self.state.xvr_armed:
                self.state.xvr_armed.discard(address)
                cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
                mu.mem_write(address, struct.pack("<I", cur & ~0x80000000))
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

        # Result register of that same block (0x810020). The Woox bk7231s light
        # triggers an op at 0x81001c, waits (handled above), reads this result,
        # and repeats the whole thing until the value exceeds 0x1d3 - so with
        # the register left at 0, the threshold is never met and boot spins here
        # forever, right after the protected-key read. The firmware immediately
        # reduces the value to a boolean (result > 0x1d3 ? 1 : 0) and discards
        # the number, so the exact value does not matter as long as it clears
        # the threshold; 0x200 does. Serving it lets Woox run on to mf_init,
        # the SDK banner, BLE advertising and the Wi-Fi scan.
        if address == 0x00810020:
            mu.mem_write(address, struct.pack("<I", 0x200))
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

        if address == 0x00802114:
            # UART1 interrupt-status. The RX ISR reads this to decide whether to
            # drain the FIFO; report RX_FIFO_NEED_READ_STA (1) and
            # RX_STOP_END_STA (6) exactly while we have queued RX bytes, so the
            # ISR reads them and then stops. Reads are self-clearing here.
            sta = ((1 << 1) | (1 << 6)) if self._rx_active() else 0
            mu.mem_write(address, struct.pack("<I", sta))
            return

        if address == self.UART1_FIFO_PORT and self._rx_active():
            # RX path: hand the firmware the next queued byte. (The write side of
            # this same address, TX, is in the write hook.) Only intercept the
            # read when we actually have RX bytes, so a normal read still returns
            # the mapped value otherwise. Received data sits in bits [15:8] of
            # the FIFO port (UART_RX_FIFO_DOUT_POSI = 8) - the firmware reads it
            # as (reg >> 8) & 0xFF - so the byte is shifted up here. TX, in
            # contrast, is bits [7:0].
            b = self.uart1_rx.pop(0)
            if self._emit_insns:
                self.state.uart1_events.append((self.state.insn_count, 'R', b & 0xFF))
            mu.mem_write(address, struct.pack("<I", (b & 0xFF) << 8))
            return

        if address == self.UART1_FIFO_STATUS or address == self.UART2_FIFO_STATUS:
            # TX bits: TX_FIFO_EMPTY (17), FIFO_WR_READY (20) - always ready.
            status = (1 << 17) | (1 << 20)
            if address == self.UART1_FIFO_STATUS and self._rx_active():
                # RX pending: FIFO_RD_READY (21) set, RX_FIFO_EMPTY (19) clear.
                status |= (1 << 21)
            else:
                status |= (1 << 19)   # RX_FIFO_EMPTY: nothing to receive
            mu.mem_write(address, struct.pack("<I", status))
            return

        if address == 0x00803008:
            # REG_FLASH_DATA_FLASH_SW. The SDK's flash_read_data reads this
            # register 8 times after one operate-write to pull a whole 32-byte
            # page (buf[8]); serve successive words so bulk reads (e.g. the
            # net_param config partition) get the full page, not word 0 repeated.
            #
            # Addresses here are PHYSICAL offsets into the dump, not offsets into
            # the CRC-stripped image. Verified against a real OpenBeken BK7231N
            # dump: its config carries a valid CRC at physical 0x1D1000, exactly
            # the BK_PARTITION_NET_PARAM address from that SDK's partition table,
            # while the address a logical mapping implies (0x1EE000) holds
            # garbage. Serving stripped bytes here used to agree with our own
            # config injector and with nothing else - reading Tuya's mirror KV at
            # 0x1CF000 as blank flash, for instance, when the dump plainly has it.
            word_addr = self.flash_state.addr + self.flash_state.read_idx * 4
            self.flash_state.read_idx += 1
            src = self.physical_flash if self.physical_flash is not None else self.raw_flash
            if word_addr + 4 <= len(src):
                val = struct.unpack("<I", src[word_addr : word_addr + 4])[0]
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

        # The 0x00900000 window. On BK7252 this is external SDRAM holding the
        # RT-Thread heap, so back it with 256K of real RAM and DO NOT install
        # the XVR register hooks below (they would answer heap reads with fake
        # register values and wreck the free list). On every other chip it is
        # the XVR (RF transceiver) register block: map one page and hook it so
        # the 0x900100 transaction trigger reads back as always-complete.
        if self.is_bk7252:
            self.mu.mem_map(self.XVR_BASE, 0x40000)  # 256K SDRAM (heap)
        else:
            self.mu.mem_map(self.XVR_BASE, 0x1000)
        # Accelerator block just past the main MMIO window (0x810000); mapped
        # and hooked so the 0x81001c transaction trigger can be served.
        self.mu.mem_map(self.ACCEL_BASE, 0x1000)

        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        if not self.is_bk7252:
            self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.XVR_BASE, end=self.XVR_BASE + 0x1000)
            self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.XVR_BASE, end=self.XVR_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self.hook_unmapped)
        self._tick_hook = self.mu.hook_add(UC_HOOK_BLOCK, self.hook_block)
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
