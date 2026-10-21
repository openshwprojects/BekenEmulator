import sys
import os
import struct
import io
import threading
import time
from unicorn import *
from unicorn.arm_const import *

# TuyaMCU MCU-peer simulator (optional). Imported defensively so emulator.py
# works whether it is loaded as src.emulator (via main.py) or as a top-level
# module with src/ on the path.
try:
    from . import tuyamcu as _tuyamcu
    from . import blecore as _blecore
    from . import wifimac as _wifimac
    from . import gpio as _gpio
    from . import periph as _periph
    from .chips import DEFAULT_CHIP
except ImportError:
    import tuyamcu as _tuyamcu
    import blecore as _blecore
    import wifimac as _wifimac
    import gpio as _gpio
    import periph as _periph
    from chips import DEFAULT_CHIP

# Instructions between raises of the guest's periodic timer (see hook_block),
# and the slice of DEVICE time one such tick stands for. Together they are the
# whole emulated clock: guest seconds = insn_count / (TICK_INSNS * 1000/TICK_MS).
# Exported because the GUI's status bar reports device uptime from them, and a
# private copy there would quietly start lying the day this changes.
SLOW_TICK_INSNS = 10000
SLOW_TICK_MS = 2

# The finer pacing grain, for UART bytes and the Wi-Fi MAC's in-flight
# transmissions (see the "fast tick" in _pacing). Also the device-clock step
# the chunked execution loop aims at, so that tick lands on its nominal
# period rather than whenever a chunk happens to end.
FAST_TICK_INSNS = 1000

# Device-clock units one emu_start() call covers. All the emulator's periodic
# work happens between two of them (see _run_chunked), so this is the grain
# everything is paced on: one fast tick per chunk, ten chunks per slow tick,
# which is the cadence the old per-block pacing produced. It also happens to
# sit at the knee of the speed curve - best of three runs to a fixed 10M
# device-clock count on a mathDemo boot: 250 -> 4.1s, 500 -> 3.8s,
# 1000 -> 3.1s, 2000 -> 3.3s, 4000 -> 3.1s, 16000 -> 2.6s. Going past a fast
# tick buys nothing until the step is many ticks long, and then it buys it by
# delivering fewer interrupts per unit of device time, which is a behaviour
# change rather than a speed-up.
CHUNK_DEVICE_INSNS = FAST_TICK_INSNS

# What one REAL guest instruction is worth on the insn_count device clock,
# which is defined by the old per-block estimator (see hook_block): the ARM
# instruction width, `(size >> 2) or 1` per basic block. In ARM state that is
# exactly one per instruction. In Thumb state the instructions are 2 bytes, so
# it would be one per two - except that the floor bites on every odd-length
# block (a 3-instruction block is 6 bytes and counts 1, not 1.5), which pulls
# the real figure well below a half. 0.42 is measured, not assumed:
# scratch/calib.py runs the chunked loop with an extra counting-only block
# hook and reports what the old estimator would have produced for the same
# code. On an OpenBeken BK7231T boot it gives 0.4164 over 18.7M Thumb
# instructions, stable at 0.40-0.44 across boot phases, and it is a property
# of Thumb block lengths rather than of this firmware.
CHUNK_SCALE_ARM = 1.0
CHUNK_SCALE_THUMB = 0.42

# Idle skipping (see _run_chunked). A settled OpenBeken spends ~85% of its
# instructions in the RTOS idle task, polling its power-save hook with
# interrupts open - on the chip that is waiting, here it was the bulk of the
# emulation cost. Once the CPU has spent IDLE_STREAK whole chunks in the idle
# task, the loop stops executing it and only advances the device clock, a
# chunk at a time with all the periodic work, until an interrupt is taken.
# At most IDLE_MAX_SKIP chunks go by without the idle loop running at all, so
# anything it polls is still seen at least every ~10 ms of device time.
# EMU_IDLE_SKIP=0 turns it off.
IDLE_STREAK = 2
IDLE_MAX_SKIP = 50
# The idle loop is usually inside its interrupts-off window when a chunk ends;
# it is walked out of it IDLE_STEP_INSNS instructions at a time, at most
# IDLE_STEP_TRIES times, before a skip may start (see _idle_step_to_open).
IDLE_STEP_INSNS = 8
IDLE_STEP_TRIES = 64
# How often to look for the idle task until it exists (device-clock units).
IDLE_SCAN_INSNS = 10_000_000


def _fast_reg_access(mu):
    """(read, write) for 32-bit registers, bypassing the binding's per-call cost.

    Uc.reg_read re-derives the register class and builds a ctypes object on
    every call - ~10 us, 3x what the C call itself costs, and the chunk loop
    and the interrupt entry make a few thousand of them per device-second.
    Falls back to the binding if this Unicorn's internals differ.
    """
    try:
        import ctypes
        from unicorn.unicorn_py3.unicorn import uclib
        handle = mu._uch
        c_read, c_write = uclib.uc_reg_read, uclib.uc_reg_write
        val = ctypes.c_uint32()
        ref = ctypes.byref(val)
        slow_read, slow_write = mu.reg_read, mu.reg_write

        def read(reg):
            if c_read(handle, reg, ref):
                return slow_read(reg)
            return val.value

        def write(reg, value):
            val.value = value & 0xFFFFFFFF
            if c_write(handle, reg, ref):
                slow_write(reg, value)

        read(UC_ARM_REG_PC)             # prove the path works before trusting it
        return read, write
    except Exception:
        return mu.reg_read, mu.reg_write

# How long UART1 receive bytes are held before the guest may read them. OpenBeken
# enables UART1 RX early (~140k insns) but only registers its console callback
# about a million instructions later in CMD_Init_Delayed, and the ISR discards
# what it reads until then - so bytes handed over sooner are simply lost. This is
# the default; build_emulator drops it to 0 with a TuyaMCU peer, and the GUI's
# "console ready" indicator reads the running emulator's own uart1_rx_delay.
CONSOLE_RX_HOLDOFF_INSNS = 2_000_000

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
        self.hw_timer_counter = [0] * 6    # counter[] of both banks (Chip.timer "v2")
        self.hw_timer_due = [None] * 6     # insn_count of each running timer's next expiry
        self.efuse_ctrl = 0
        self.saradc_cfg = 0        # shadow of SARADC_ADC_CONFIG (0x802c00)
        self.saradc_cfg2 = 0       # shadow of SARADC_ADC_CONFIG2 (0x802c08, BK7238/BK7252N)
        self.saradc_state_reg = 0  # shadow of SARADC_ADC_STATE (0x802c0c, BK7238/BK7252N)
        self.saradc_pending = 0    # samples waiting in the emulated ADC FIFO
        self.saradc_channel = None # active SARADC channel (0..15)
        self.saradc_last_raw = None
        self.saradc_conversions = 0
        self.saradc_reads = 0
        self.adc_samples = {}      # channel -> latest raw sample
        self.last_pc = 0           # start of the last basic block, for the GUI status bar
        self.last_slow = 0         # insn_count at last slow (timer) tick
        self.last_fast = 0         # insn_count at last ~1000-insn (UART) tick
        self.last_emit = 0         # insn_count at last [EMU_INSNS] report line
        # Peripheral state observed during the run, for the report's GPIO tab.
        # Both live in MMIO ranges the write hook already covers, and a boot
        # performs only tens of GPIO writes (a couple of thousand PWM ones),
        # so recording them costs nothing measurable.
        self.gpio_cfg = {}         # pin -> last value written to its config reg
        # Physical input levels and the GPIO controller's INTEN/INTLV/INTSTA
        # registers.  The model itself is lock-free and pickleable; host-thread
        # changes reach it through BekenEmulator's queue.
        self.gpio_hw = _gpio.GpioModel()
        self.pwm_regs = {}         # register offset -> last value written
        # Keys changed since the last report emission. The snapshot used to go
        # out only on the 1,000,000-instruction [EMU_INSNS] tick, which races
        # the harness: it stops as soon as its log markers match, so a register
        # written after the last tick was never reported and an assertion saw a
        # stale value. Tracking dirty keys lets the fast tick emit just what
        # changed, which removes the race and prints less than the snapshot.
        self.gpio_dirty = set()
        self.pwm_dirty = set()
        # Bit-banged two-wire LED driver capture (SM2135, SM2235, BP5758D, BP1658CJ)
        # and SPI-DMA WS2812 pixel buffer + BL0942 UART energy meter state.
        self.i2c_last_pin = None
        self.i2c_pair_counts = {}
        self.i2c_buses = {}        # (scl, sda) -> [in_tx, cur_byte, bits, cur_frame]
        self.i2c_frames = {}       # (scl, sda) -> latest raw frame bytes
        self.i2c_last_on = {}      # (scl, sda) -> latest non-zero LED frame bytes
        self.i2c_dirty = set()
        self.ws2812_spi = b""
        self.bl0942_polls = 0
        self.uart1_tx_prev = -1
        # True while stdout sits at a line boundary. Report lines may only be
        # written then, so they never land inside a UART log line.
        self.uart_at_line_start = True
        self.last_uart_insn = 0
        self.uart_line_tail = b""
        # The RTOS idle task's stack as (low, high), once found; see idle
        # skipping in _run_chunked. Here, not on the emulator, so a snapshot
        # carries it and a resumed run skips exactly as the original did.
        self.idle_task = None
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
        # (Now understood: on the BLE 5.1 layout these are the RW core's
        # IP_SLOTCLK SAMP bit and RWDMCNTL MASTER_SOFT_RST - the same block
        # --ble-core models fully for the 5.2 layout; see src/blecore.py.)
        self.xvr_armed = set()
        # General-DMA channel config shadows: channel -> last CONF value, with
        # the enable bit already cleared because the transfer is done by the
        # time the guest can look. Reads of CONF are served from here.
        self.gdma_conf = {}
        # pwm_new interrupt status (BK7231N/BK7238 PWM block at 0x802B00).
        # pwmn_seen flips the first time the firmware programs a group; from
        # then on ICU bit 9 (IRQ_PWM) follows pwmn_stat rather than the
        # T-family PWM status at 0x802A04, which an N-family ISR never clears.
        # pwmn_stat holds each group's INT_STAT bits (30/31) at their own bit
        # positions shifted down by 30 - 2*group. Nothing raises them yet (no
        # signal is ever driven onto a capture pin), so they only get cleared.
        self.pwmn_seen = False
        self.pwmn_stat = 0
        # Watchdog controller.  The Beken block accepts a period only after
        # the 0x5a/0xa5 key pair has been written to WDT_CTRL.  Its clock is
        # gated by ICU_PERI_CLK_PWD bit 8; while gated, a programmed period is
        # retained but cannot expire.
        self.wdt_first_period = None
        self.wdt_period_ms = 0
        self.wdt_clock_enabled = True
        self.wdt_deadline = None
        # SCTRL / AON Deep Sleep & Wakeup registers:
        # 0x00800104 SCTRL_SLEEP (deep sleep triggered when low 16 bits == 0xADC1)
        # 0x0080011C SCTRL_ROSC_TIMER (bits 31:16 low ticks, bit 8 W1C int status, bit 0 enable)
        # 0x00800120 SCTRL_GPIO_WAKEUP_EN (pins 0..31 wake mask)
        # 0x00800124 SCTRL_GPIO_WAKEUP_TYPE (pins 0..31 wake polarity: 0=high/rising, 1=low/falling)
        # 0x00800128 SCTRL_GPIO_WAKEUP_INT_STATUS (pins 0..31 W1C wake status)
        # 0x00800140 SCTRL_ROSC_TIMER_H (bits 15:0 high ticks)
        # 0x00800144 SCTRL_GPIO_WAKEUP_EN1 (pins 32..39 wake mask)
        # 0x00800148 SCTRL_GPIO_WAKEUP_TYPE1 (pins 32..39 wake polarity)
        # 0x0080014C SCTRL_GPIO_WAKEUP_INT_STATUS1 (pins 32..39 W1C wake status)
        # 0x00808004 AON_PMU_WAKEUP_GPIO_STATUS (pins 0..31 wake status on BK7231N/BK7238)
        self.sctrl_rosc_timer = 0
        self.sctrl_rosc_timer_h = 0
        self.sctrl_rosc_int_status = False
        self.sctrl_gpio_wakeup_en = 0
        self.sctrl_gpio_wakeup_type = 0
        self.sctrl_gpio_wakeup_int_status = 0
        self.sctrl_gpio_wakeup_en1 = 0
        self.sctrl_gpio_wakeup_type1 = 0
        self.sctrl_gpio_wakeup_int_status1 = 0
        self.aon_gpio_wakeup_status = 0
        self.sctrl_reset_retention = 0
        self.in_deep_sleep = False
        self.deep_sleep_count = 0
        self.deep_sleep_info = None
        self.deep_sleep_rtc_sec = 0.0
        self.deep_sleep_wake_pins = {}
        self.last_wake_reason = None
        self.last_wake_pin = None
        self.wake_pulse_pin = None
        self.wake_pulse_restore = None
        self.wake_pulse_reads_left = 0
        self.wake_reg_sampled = False
        self.rtt_boot_seen = False
        self.obk_boot_seen = False
        self.msh_prompt_seen = False
        self.msh_uart_port = None
        self.rx_next_line_insn = 0
        self.audio_fifo_conf = 0
        self.audio_dac_writes = 0
        # Hardware I2C1 (0x00802300, IRQ bit 2) / I2C2 (0x00802600, IRQ bit 5),
        # GC0328C DVP camera sensor, and EJPEG encoder (0x0080A000, FIQ bit 29/23).
        self.gdma_remain = {}
        self.hw_i2c = {
            1: {"cfg": 0, "sta": 0, "last_dat": 0, "read_dat": 0, "slave": 0,
                "reg": 0, "phase": 0, "v2": False},
            2: {"cfg": 0, "sta": 0, "last_dat": 0, "read_dat": 0, "slave": 0,
                "reg": 0, "phase": 0, "v2": True},
        }
        self.camera_sensor = None
        self.camera_sensor_probed = False
        self.camera_sensor_regs = {}
        self.camera_i2c_writes = 0
        self.camera_last_i2c_insn = 0
        self.ejpeg_reg0 = 0
        self.ejpeg_reg1 = 0
        self.ejpeg_reg2 = 0
        self.ejpeg_reg3 = 0
        self.ejpeg_int_stat = 0
        self.ejpeg_frame_len = 0
        self.ejpeg_dma_ch = None
        self.ejpeg_dma_dst = 0
        self.ejpeg_dma_node_len = 1460
        self.ejpeg_tick_div = 0
        self.camera_frame_count = 0
        self.camera_last_jpeg = b""
        self.camera_frame_rgb = b""
        self.camera_width = 640
        self.camera_height = 480


class FlashState:
    def __init__(self):
        self.addr = 0
        self.data = b'\xff\xff\xff\xff'
        self.read_idx = 0   # word index within the current 32-byte flash page read
        self.write_buf = []  # words staged via REG_FLASH_DATA_SW_FLASH for a page program
        self.pw_buf = bytearray()  # bytes staged via REG_FLASH_PW_CONF (0x803024) for a 256-byte page program

class BekenEmulator:
    # Hardware base addresses
    FLASH_BASE = 0x00000000
    FLASH_SIZE = 2 * 1024 * 1024
    MMIO_BASE = 0x00800000
    MMIO_SIZE = 0x010000
    PERIPH_BASE = 0xc0000000
    PERIPH_SIZE = 0x100000
    SPI_FLASH_BASE = 0x00200000
    XVR_BASE = 0x00900000
    EJPEG_FIFO = {"v1": 0x0080A014, "v2": 0x00A03014}   # RX FIFO per EJPEG variant
    EJPEG2_BASE = 0x00A00000      # EJPEG v2 block (registers at 0x00A03000)
    EJPEG2_SIZE = 0x10000
    ACCEL_BASE = 0x00810000
    PWM_BASE = 0x00802A00
    # pwm_new (0x802B00, three groups of 0x40): REG_PWM_GROUP_CTRL and
    # REG_GROUP_PWM_CPU of each group (pwm_bk7231n.h).
    PWMN_BASE = 0x00802B00
    PWMN_CTRL_REGS = (0x00802B00, 0x00802B40, 0x00802B80)
    PWM_CPU_REGS = (0x00802B24, 0x00802B64, 0x00802BA4)
    PWMN_INT_STAT = (1 << 30) | (1 << 31)     # per-channel status, write 1 to clear
    PWMN_CFG_UPDATA = (1 << 7) | (1 << 15)    # per-channel config latch, self-clearing
    # General DMA (beken378/driver/general_dma/general_dma.h). Six channels on
    # v1 (0x00809000 + ch*0x20, remain at 0x008090C0 + ch*4), eight channels on
    # v2 (BK7252N: 0x00809040 + ch*0x40, remain at 0x00809070 + ch*0x40).
    GDMA_BASE = 0x00809000
    GDMA_CHANNELS = 6
    GDMA_CH_STRIDE = 0x20
    GDMA_END = GDMA_BASE + 0x240
    GDMA_EN = 1 << 0            # GDMA_X_DMA_EN, cleared by hardware when done
    GDMA_SRCADDR_INC = 1 << 8
    GDMA_DSTADDR_INC = 1 << 9
    GDMA_TRANS_LEN_POSI = 16    # length is stored as (bytes - 1)
    GPIO_BASE = 0x00802800
    GPIO_PIN_COUNT = 32
    GPIO_PIN_END = GPIO_BASE + GPIO_PIN_COUNT * 4
    GPIO_FUNC_CFG = GPIO_BASE + 32 * 4
    GPIO_INTEN = GPIO_BASE + 33 * 4
    GPIO_INTLV0 = GPIO_BASE + 34 * 4
    GPIO_INTLV1 = GPIO_BASE + 35 * 4
    GPIO_INTSTA = GPIO_BASE + 36 * 4
    GPIO_FUNC_CFG_2 = GPIO_BASE + 46 * 4
    GPIO_FUNC_CFG_3 = GPIO_BASE + 47 * 4
    # BK7221U silicon (selected here as BK7252) has GPIO32..39 after the two
    # function-select words, followed by a second interrupt bank.  Logical pin
    # 32 is therefore physical register slot 48 -- never slot 32 (FUNC_CFG).
    GPIO_EXT_PIN_FIRST = 32
    GPIO_EXT_SLOT_FIRST = 48
    GPIO_EXT_PIN_COUNT = 8
    GPIO_EXT_PIN_END = GPIO_BASE + (GPIO_EXT_SLOT_FIRST + GPIO_EXT_PIN_COUNT) * 4
    GPIO_INTEN2 = GPIO_BASE + 56 * 4
    GPIO_INTLV3 = GPIO_BASE + 57 * 4
    GPIO_INTSTA2 = GPIO_BASE + 58 * 4
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
    ICU_PERI_CLK_PWD = 0x00802008
    ICU_WDT_CLK_PWD = 1 << 8

    # Watchdog: wdt.h defines an 8-bit key at bit 16 and a 16-bit period.
    # The SDK passes milliseconds (and uses period 5 for bk_reboot()).
    WDT_CTRL = 0x00802900
    WDT_FIRST_KEY = 0x5A
    WDT_SECOND_KEY = 0xA5
    WDT_INSNS_PER_MS = SLOW_TICK_INSNS // SLOW_TICK_MS
    # Hardware timers (Chip.timer "v2", soc/timer.h): two banks of three, each
    # counter[3] then a ctl word (enable [2:0], clk_divider [6:3], irq_status
    # [9:7], write-1-to-clear). Bank 0 counts the 26 MHz crystal, bank 1 the
    # 32 kHz low-power clock; the period is counter * (divider + 1) / clock.
    TIMER_BANK_BASE = (0x00802A00, 0x00802A40)
    TIMER_SOURCE_HZ = (26_000_000, 32_000)
    TIMER_INSNS_PER_SEC = SLOW_TICK_INSNS * 1000 // SLOW_TICK_MS

    def __init__(self, raw_flash, bootloader, app, with_boot=False, only_uart=False, chip=None, uart1_hex=False, physical_flash=None, uart1_rx=b"", uart1_rx_delay=CONSOLE_RX_HOLDOFF_INSNS, tuyamcu_enabled=False, tuyamcu_pid=None, tuyamcu_raw=False, xvr_selfclear=False, tuyamcu_dps=None, tuyamcu_injects=None, uart_sink=None, ble_core=None, wifi=False, exception_vector_base=0, deep_sleep_wakes=None, gpio_inputs=None, adc_inputs=None):
        self.raw_flash = raw_flash
        self.xvr_selfclear = xvr_selfclear
        self.chip = chip or DEFAULT_CHIP
        # Opt-in RivieraWaves Wi-Fi MAC model over 0xC0000000 (src/wifimac.py).
        # Off, the block is plain memory and the firmware's Wi-Fi stack comes up
        # but can never move a frame or see time pass; on, TX/RX descriptors,
        # the 1 MHz MAC clock, the ABS timers and the six MAC FIQ lines work,
        # and a peer (src/wifipeer.py) can be attached to talk 802.11 to it.
        self.wifimac = None
        self.wifi_peer = None
        # On-chip timers restart from zero on watchdog reset, while the access
        # point/router and packet-capture clock outside the chip do not.  Add
        # each completed boot here to give that external side monotonic time.
        self._network_insn_offset = 0
        # Why run() ended early, as text, when it was an error rather than a
        # stop or the end of the program. run() only prints it; a front end
        # with its own error display reads it from here.
        self.run_error = None
        # Hardware TRNG (TRNG_DATA, irda.h - same address on every family).
        # None = plain memory, i.e. the same value on every read. The network
        # needs real randomness: LwIP's LWIP_RAND() is bk_rand() -> TRNG, and
        # its DNS client loops `do { port = LWIP_RAND(); } while (port < 1024)`
        # to pick a source port - with a constant 0 that loop never ends, the
        # TCP/IP thread hangs on the first lookup and every task waiting on it
        # (OBK's main loop, MQTT) with it. Deterministic xorshift32, so runs
        # stay reproducible. Tied to --wifi so non-network runs are unchanged.
        self.trng_state = None
        if wifi:
            # net_log, not _print: these are [WIFI] lines like the peer's, and
            # every real run passes --only-uart, which silences _print. Sent
            # there they were invisible exactly when they mattered.
            self.wifimac = _wifimac.MacHw(SLOW_TICK_INSNS // (SLOW_TICK_MS * 1000),
                                          log=self.net_log)
            self.wifimac.mem = self._guest_mem_callbacks()
            self.trng_state = 0x2545F491
        # RivieraWaves BLE core model over the 0x900000 block (src/blecore.py).
        # Real BLE 5.x silicon selected by an unambiguous chip identity gets
        # this hardware automatically.  None means that automatic policy,
        # True is the legacy forced mode for N/M firmware run with the generic
        # BK7231 identity, and False is an explicit programmatic opt-out useful
        # for diagnostics.  A modelled controller gets a slot clock, wake-ups,
        # interrupt registers and the BLE/BTDM FIQ lines (ICU bits 29/30).
        self.blecore = None
        layout = self.chip.ble_core
        self._ble_layout = layout
        if ble_core is True and layout is None:
            raise ValueError("--ble-core models the BLE 5.x core of BK7231N/M (5.1) and "
                             "BK7238 (5.2); this chip has no such core.")
        want_ble_core = (ble_core is True or
                         (ble_core is None and self.chip.ble_auto))
        if want_ble_core:
            # Auto chips must have a BLE layout; keep this assertion close to
            # selection so a future chip file cannot accidentally instantiate
            # a layout-less core.
            if layout is None:
                raise ValueError("--ble-core models the BLE 5.x core of BK7231N/M (5.1) and "
                                 "BK7238 (5.2); this chip has no such core.")
            self.blecore = _blecore.BleCore(self.XVR_BASE, SLOW_TICK_INSNS * 1000 // SLOW_TICK_MS,
                                            layout=layout)
            # Exchange memory is ordinary guest RAM, reached through Unicorn.
            self.blecore.mem = self._guest_mem_callbacks()
        self.bootloader = bootloader
        self.app = app
        self.with_boot = with_boot
        # ARM968E-S firmware can relocate its exception table.  Unicorn does
        # not deliver the emulator's synthetic IRQ/FIQ lines for us, so the
        # manual exception entry below must apply the same base explicitly.
        self.exception_vector_base = exception_vector_base
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
        # Optional callback taking (port, byte) for every byte the firmware
        # transmits, port being 1 or 2. A GUI sets this to route each UART to
        # its own pane; when it is None (every CLI run) nothing changes and the
        # bytes go to stdout exactly as before.
        self.uart_sink = uart_sink
        # Optional callback(str) for [WIFI]/[NET] lines; see net_log().
        self.net_sink = None
        # Optional callback(direction, frame_bytes, now_us) for every frame the
        # IP bridge carried, so a front end can show the bytes themselves.
        self.net_frame_sink = None
        # Optional exact-PC diagnostics installed by trace_pc().  Narrow
        # Unicorn code hooks are effectively free compared with the old
        # all-code pacing hook, and each address is reported only once.
        self._pc_trace_addresses = set()
        self._pc_trace_seen = set()
        self._pc_trace_hooks = []
        # Appends to uart1_rx may come from another thread (a GUI typing into
        # the console) while the emulation thread pops from it. Only appends
        # are locked: the emulator is the sole consumer, and an append can only
        # ever GROW the buffer, so a lock-free pop can never miss or overrun.
        # That keeps the lock off the hot read path, which polls UART1 status
        # constantly during boot.
        self._rx_lock = threading.Lock()
        # Host/UI threads enqueue physical GPIO changes here.  Only _pacing(),
        # which runs between guest instructions on the emulation thread, drains
        # them into state.gpio_hw.  In particular, set_gpio_input() never calls
        # Unicorn from a foreign thread.
        self._gpio_lock = threading.Lock()
        self._gpio_events = []
        # Pause support for a GUI. Checked once per basic block, so it must
        # stay a plain attribute test - the Event is only touched once actually
        # paused. Pausing also hands the GIL back, which is what keeps a UI
        # responsive while a boot is running.
        self._paused = False
        self._resume = threading.Event()
        self._resume.set()
        self.stopping = False
        # A watchdog expiry ends the current Unicorn run, but unlike stop() it
        # immediately starts a fresh device boot.  reset_count is intentionally
        # public: the GUI exposes it so a reboot cannot look like a frozen PC.
        self._watchdog_reset_pending = False
        self.reset_count = 0
        self._power_on_context = None
        # Snapshot request (see arm_snapshot); None until asked for.
        self._snapshot_path = None
        self._snapshot_at_insns = None
        self._snapshot_marker = None
        self._snapshot_due = False
        self._uart_tail = b""
        self._resume_at = None
        # Execution granularity. Normally the emulator drives Unicorn in
        # chunks worth CHUNK_DEVICE_INSNS of device clock and does its
        # periodic work between them (_run_chunked); EMU_BLOCK_PACING=1
        # switches back to the old UC_HOOK_BLOCK callback, which is ~20x
        # slower but defines the insn_count scale the chunked path
        # reproduces. EMU_CHUNK_DEVICE_INSNS overrides the grain, for
        # measuring it.
        self._block_pacing = bool(os.environ.get("EMU_BLOCK_PACING"))
        self.chunk_device_insns = int(os.environ.get("EMU_CHUNK_DEVICE_INSNS")
                                      or CHUNK_DEVICE_INSNS)
        # Idle skipping, on unless EMU_IDLE_SKIP=0 (see IDLE_STREAK). The
        # counters say how much of a run was skipped, for measuring it.
        self.idle_skip = os.environ.get("EMU_IDLE_SKIP", "1") != "0"
        self.idle_chunks_skipped = 0
        self.chunks_run = 0
        # Bumped whenever trigger_irq/trigger_fiq actually takes an exception,
        # i.e. moves PC out from under the chunk loop.
        self._exc_epoch = 0
        # Advertising payloads already printed, so each is reported once.
        self._ble_adv_seen = set()
        # When EMU_REPORT is set (the self-test harness does this), periodically
        # emit the approximate executed-instruction count on a distinct
        # [EMU_INSNS] line so the report can show it. Off by default so normal
        # runs are not polluted, and the emit is throttled + piggybacks the
        # existing slow tick, so it never touches the per-block hot path.
        self._emit_insns = bool(os.environ.get("EMU_REPORT"))
        self.chip_id_value, self.device_id_value = self.chip.chip_id, self.chip.dev_id
        self.ram_base, self.ram_size = self.chip.ram_base, self.chip.ram_size
        self.gpio_pin_count = self.chip.gpio_pins

        self.state = SimulatorState()
        if self.chip.i2c == "v2":
            self.state.hw_i2c[1]["v2"] = True
        if self.gpio_pin_count != self.state.gpio_hw.pin_count:
            self.state.gpio_hw = _gpio.GpioModel(self.gpio_pin_count)
        if gpio_inputs:
            for pin, level in gpio_inputs.items():
                self.state.gpio_hw.check_pin(int(pin))
                lvl = _gpio.GpioModel.check_level(level)
                if lvl is not None:
                    self.state.gpio_hw.inputs[int(pin)] = lvl
        self.adc_inputs = {}
        self._adc_inputs = self.adc_inputs
        if adc_inputs:
            for ch, val in adc_inputs.items():
                self.set_adc_input(ch, val)
        self._deep_sleep_wake_queue = list(deep_sleep_wakes or [])
        self._deep_sleep_wake_pending = None
        self.flash_state = FlashState()
        # Simulated Energy Meter (BL0942 UART1 / BL0937 GPIO pulse) readings.
        self.meter_v = 230.0
        self.meter_a = 1.5
        self.meter_w = 345.0
        self.meter_cf_pin = None
        self.meter_cf1_pin = None
        self.meter_sel_pin = None
        self.meter_sel_lv = 1
        self._meter_cf_ticks = 0
        self._meter_cf1_ticks = 0
        self._meter_cf_lv = 0
        self._meter_cf1_lv = 0
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
        self._reg_read, self._reg_write = _fast_reg_access(self.mu)
            
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

    def _flash_program_page(self, addr, data):
        """256-byte page program (BK7238 REG_FLASH_PW_CONF at 0x00803024)."""
        if not data:
            return
        buf = self._flash_backing()
        base = addr & ~0xFF
        for i, byte in enumerate(data[:256]):
            pos = base + i
            if pos < len(buf):
                buf[pos] &= byte

    def _flash_erase(self, addr, size):
        """Sector/block erase: restore the region to erased (0xFF)."""
        buf = self._flash_backing()
        start = addr & ~(size - 1)
        end = min(start + size, len(buf))
        if start < end:
            buf[start:end] = b"\xff" * (end - start)

    def _use_uart2_rx(self):
        """True when RT-Thread FinSH (msh />) listens on UART2 instead of UART1."""
        state = self.state
        if self.tuyamcu is not None:
            return False
        if state.msh_uart_port in (1, 2):
            return state.msh_uart_port == 2
        if not ((state.uart2_int_enable & 0x42) and (state.icu_int_enable & (1 << 1))):
            return False
        return not ((state.uart1_int_enable & 0x42) and (state.icu_int_enable & (1 << 0)))

    def _rx_ready_for_console(self):
        """Hold back console commands on RT-Thread until FinSH prints msh />."""
        state = self.state
        if state.rtt_boot_seen or (self.chip.rtthread_console and not state.obk_boot_seen):
            if state.msh_prompt_seen:
                return True
            if state.rx_next_line_insn:
                return state.insn_count >= state.rx_next_line_insn
            return state.insn_count >= max(self.uart1_rx_delay, 25_000_000)
        return state.insn_count >= self.uart1_rx_delay

    def _rx_active(self):
        """True while queued UART1 RX bytes should be delivered to UART1."""
        if not self.uart1_rx or self._use_uart2_rx():
            return False
        if self.tuyamcu is not None:
            return self.state.insn_count >= self.uart1_rx_delay
        return self._rx_ready_for_console()

    def _rx_active_uart2(self):
        """True while queued console RX bytes should be delivered to UART2."""
        if not self.uart1_rx or not self._use_uart2_rx():
            return False
        return self._rx_ready_for_console()

    def feed_uart1(self, data):
        """Queue bytes for the guest to read back on UART1. Thread-safe.

        This is how a GUI types into the firmware's command console: the bytes
        join the same RX FIFO / RX interrupt path that --uart1-rx and the
        simulated MCU's replies use, so the guest cannot tell them apart.
        """
        with self._rx_lock:
            self.uart1_rx += bytes(data)
            self.state.msh_prompt_seen = True

    def set_energy_meter(self, voltage_v=230.0, current_a=1.5, power_w=345.0,
                         cf_pin=None, cf1_pin=None, sel_pin=None, sel_lv=1):
        """Update simulated BL0942 (UART1) / BL0937 (GPIO pulse) readings.

        Safe to call from a GUI thread while the emulator is running.
        """
        self.meter_v = max(0.0, float(voltage_v))
        self.meter_a = max(0.0, float(current_a))
        self.meter_w = max(0.0, float(power_w))
        if cf_pin is not None:
            self.meter_cf_pin = int(cf_pin)
        if cf1_pin is not None:
            self.meter_cf1_pin = int(cf1_pin)
        if sel_pin is not None:
            self.meter_sel_pin = int(sel_pin)
        if sel_lv is not None:
            self.meter_sel_lv = 1 if int(sel_lv) else 0

    def _bl0942_reply_packet(self):
        """Build a 23-byte BL0942 full-data packet (0x55 header + checksum).

        Scale factors match OpenBeken's ``drv_bl0942.c`` defaults
        (``BL0942_VREF = 15188``, ``BL0942_IREF = 251210``, ``BL0942_PREF = 598``).
        """
        v_rms = max(0, min(0xFFFFFF, int(round(self.meter_v * 15188.0))))
        i_rms = max(0, min(0xFFFFFF, int(round(self.meter_a * 251210.0))))
        watt = max(0, min(0x7FFFFF, int(round(self.meter_w * 598.0))))
        cf_cnt = max(1, min(0xFFFFFF, self.state.bl0942_polls * 10))
        freq = int(1000000 // 50)
        pkt = bytearray(23)
        pkt[0] = 0x55
        pkt[1:4] = i_rms.to_bytes(3, "little")
        pkt[4:7] = v_rms.to_bytes(3, "little")
        pkt[7:10] = i_rms.to_bytes(3, "little")
        pkt[10:13] = watt.to_bytes(3, "little")
        pkt[13:16] = cf_cnt.to_bytes(3, "little")
        pkt[16:18] = freq.to_bytes(2, "little")
        pkt[22] = ((0x58 + sum(pkt[:22])) & 0xFF) ^ 0xFF
        return bytes(pkt)

    def _tick_energy_meter_pulses(self, state):
        """Drive simulated BL0937/HLW8012 CF (power) and CF1 (V/I) pulses."""
        cf_pin = self.meter_cf_pin
        cf1_pin = self.meter_cf1_pin
        if cf_pin is not None and 0 <= cf_pin < self.gpio_pin_count and self.meter_w > 0.1:
            self._meter_cf_ticks += 1
            period = max(1, int(round(600.0 / max(5.0, min(3000.0, self.meter_w)))))
            if self._meter_cf_ticks >= period:
                self._meter_cf_ticks = 0
                self._meter_cf_lv ^= 1
                state.gpio_hw.drive(cf_pin, self._meter_cf_lv, self._gpio_config_word(cf_pin))
        if cf1_pin is not None and 0 <= cf1_pin < self.gpio_pin_count:
            self._meter_cf1_ticks += 1
            sel_v = True
            if self.meter_sel_pin is not None and 0 <= self.meter_sel_pin < self.gpio_pin_count:
                sel_cfg = self._gpio_config_word(self.meter_sel_pin)
                sel_out = 0 if (not (sel_cfg & 0x08) and not (sel_cfg & 0x02)) else 1
                sel_v = (sel_out == self.meter_sel_lv)
            rate = self.meter_v if sel_v else (self.meter_a * 100.0)
            if rate > 0.1:
                period = max(1, int(round(300.0 / max(5.0, min(1500.0, rate)))))
                if self._meter_cf1_ticks >= period:
                    self._meter_cf1_ticks = 0
                    self._meter_cf1_lv ^= 1
                    state.gpio_hw.drive(cf1_pin, self._meter_cf1_lv, self._gpio_config_word(cf1_pin))

    def _i2c_pin_edge(self, state, pin, old_cfg, new_cfg):
        """Track bit-banged two-wire LED driver frames (SM2135/SM2235/BP5758D/BP1658CJ)."""
        old_lv = 0 if (not (old_cfg & 0x08) and not (old_cfg & 0x02)) else 1
        new_lv = 0 if (not (new_cfg & 0x08) and not (new_cfg & 0x02)) else 1
        if pin != state.i2c_last_pin:
            if state.i2c_last_pin is not None:
                pair = (min(pin, state.i2c_last_pin), max(pin, state.i2c_last_pin))
                cnt = state.i2c_pair_counts.get(pair, 0) + 1
                state.i2c_pair_counts[pair] = cnt
                if cnt == 32:
                    state.i2c_buses[(pair[0], pair[1])] = [False, 0, 0, bytearray()]
                    state.i2c_buses[(pair[1], pair[0])] = [False, 0, 0, bytearray()]
            state.i2c_last_pin = pin
        if not state.i2c_buses or old_lv == new_lv:
            return
        for (scl, sda), bstate in state.i2c_buses.items():
            if pin == sda:
                scl_cfg = state.gpio_cfg.get(self._gpio_slot(scl), 0x3C)
                scl_lv = 0 if (not (scl_cfg & 0x08) and not (scl_cfg & 0x02)) else 1
                if scl_lv == 1:
                    if old_lv == 1 and new_lv == 0:
                        bstate[0] = True
                        bstate[1] = 0
                        bstate[2] = 0
                        bstate[3] = bytearray()
                    elif old_lv == 0 and new_lv == 1:
                        if bstate[0] and len(bstate[3]) >= 2:
                            raw = bytes(bstate[3])
                            dec = _periph.decode_i2c_led(scl, sda, raw)
                            if dec is not None:
                                state.i2c_frames[(scl, sda)] = raw
                                if dec["is_lit"]:
                                    state.i2c_last_on[(scl, sda)] = raw
                                if self._emit_insns:
                                    state.i2c_dirty.add((scl, sda))
                        bstate[0] = False
            elif pin == scl and bstate[0]:
                if old_lv == 0 and new_lv == 1:
                    sda_cfg = state.gpio_cfg.get(self._gpio_slot(sda), 0x3C)
                    sda_lv = 0 if (not (sda_cfg & 0x08) and not (sda_cfg & 0x02)) else 1
                    if bstate[2] < 8:
                        bstate[1] = ((bstate[1] << 1) | sda_lv) & 0xFF
                        bstate[2] += 1
                    else:
                        bstate[3].append(bstate[1])
                        bstate[1] = 0
                        bstate[2] = 0

    @property
    def chip_family_name(self):
        return self.chip.name

    def set_adc_input(self, channel, value):
        """Set or clear a simulated SARADC input channel (0..15 or ``'P23'``).

        ``value`` is a raw ADC sample (0..65535), a voltage string (``'1.65V'``,
        ``'1650mV'``), or ``None`` to revert to the default sample (330).
        Safe to call from a GUI thread while running.
        """
        ch = _periph.parse_adc_channel(channel, chip=self.chip_family_name)
        if value is None:
            self.adc_inputs.pop(ch, None)
            self.state.adc_samples.pop(ch, None)
            return
        if isinstance(value, str):
            _, value = _periph.parse_adc_spec("0:%s" % value, chip=self.chip_family_name)
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFF:
            raise ValueError("ADC sample value must be in 0..65535 (got %r)" % (value,))
        self.adc_inputs[ch] = value
        self.state.adc_samples[ch] = value

    def get_adc_input(self, channel):
        """Return the raw sample value configured for ``channel``."""
        ch = _periph.parse_adc_channel(channel, chip=self.chip_family_name)
        return self.state.adc_samples.get(
            ch, self.adc_inputs.get(ch, _periph.DEFAULT_ADC_RAW))

    def _saradc_sample_for(self, channel=None):
        """Consume and record one sample read for the active SARADC channel."""
        state = self.state
        ch = state.saradc_channel if channel is None else channel
        if ch is None:
            ch = 0
        val = self.get_adc_input(ch) & 0xFFFF
        state.saradc_last_raw = val
        state.adc_samples[ch] = val
        state.saradc_reads += 1
        return val

    def _guest_mem_callbacks(self):
        """Return (read, write) callables giving a peripheral model guest RAM access."""
        return (
            lambda addr, size: self.mu.mem_read(addr, size),
            lambda addr, data: self.mu.mem_write(addr, bytes(data)),
        )

    def _bind_wifi_peer(self, peer, reset_device=False):
        """Wire a WifiPeer to the current MacHw instance."""
        if peer is None or self.wifimac is None:
            return
        self.wifimac.on_tx = peer.on_device_tx
        peer.inject = lambda f, decr=None: self.wifimac.inject(
            f, self.state.insn_count, decr=decr)
        peer.now_us = self._network_now_us
        if reset_device:
            peer.device_reset(self._network_now_us())

    def _write_mmio_reset_defaults(self):
        """Write power-on reset values for UART FIFOs, SARADC, and WDT into MMIO."""
        fifo_default = struct.pack("<I", 1 << 17 | 1 << 19 | 1 << 20)
        self.mu.mem_write(self.UART1_FIFO_STATUS, fifo_default)
        self.mu.mem_write(self.UART2_FIFO_STATUS, fifo_default)
        self.mu.mem_write(0x00802c00, struct.pack("<I", 1 << 28 | 1 << 30))
        self.mu.mem_write(self.WDT_CTRL, struct.pack("<I", 0))

    def _clear_reset_memory(self):
        """Zero RAM/MMIO/peripheral windows and restore power-on CPU context."""
        self.mu.mem_write(self.ram_base, b"\x00" * self.ram_size)
        self.mu.mem_write(self.MMIO_BASE, b"\x00" * self.MMIO_SIZE)
        self.mu.mem_write(self.PERIPH_BASE, b"\x00" * self.PERIPH_SIZE)
        for base, size, _hooked in self._chip_windows():
            self.mu.mem_write(base, b"\x00" * size)
        self.mu.mem_write(self.ACCEL_BASE, b"\x00" * 0x1000)
        self._write_mmio_reset_defaults()
        self.mu.context_restore(self._power_on_context)

    def set_gpio_input(self, pin, level):
        """Drive one GPIO input from a host thread, or release it with ``None``.

        ``level`` is 0 or 1 while an external switch/sensor drives the pad.
        ``None`` disconnects that external drive, after which the guest's pull
        configuration decides the sampled level.  Calls are queued and applied
        between guest instructions by the emulation thread; this method never
        reads or writes Unicorn and is safe for a GUI callback.
        """
        if (not isinstance(pin, int) or isinstance(pin, bool)
                or not 0 <= pin < self.gpio_pin_count):
            raise ValueError("GPIO pin must be in the range 0-%d"
                             % (self.gpio_pin_count - 1))
        level = _gpio.GpioModel.check_level(level)
        with self._gpio_lock:
            self._gpio_events.append((pin, level))

    def _gpio_slot(self, pin):
        """Physical register slot for a logical GPIO number."""
        return _periph.gpio_slot(pin, self.gpio_pin_count)

    def _gpio_pin_at(self, address):
        """Logical GPIO at an MMIO address, excluding controller registers."""
        # This helper is reached from the hooks for every MMIO access in the
        # system.  Reject the overwhelmingly common non-GPIO case before doing
        # alignment or division work.
        end = self.GPIO_EXT_PIN_END if self.chip.gpio_pins > self.GPIO_PIN_COUNT else self.GPIO_PIN_END
        if address < self.GPIO_BASE or address >= end:
            return None
        if address & 3:
            return None
        slot = (address - self.GPIO_BASE) // 4
        if 0 <= slot < self.GPIO_PIN_COUNT:
            return slot
        if (self.chip.gpio_pins > self.GPIO_PIN_COUNT and self.GPIO_EXT_SLOT_FIRST <= slot
                < self.GPIO_EXT_SLOT_FIRST + self.GPIO_EXT_PIN_COUNT):
            return self.GPIO_EXT_PIN_FIRST + (slot - self.GPIO_EXT_SLOT_FIRST)
        return None

    def _gpio_config_word(self, pin):
        # gpio_cfg keys are physical register slots.  That preserves the
        # report's historical 0..31/function-register representation while
        # keeping BK7231U/BK7252 logical P32 distinct from FUNC_CFG at slot 32.
        return self.state.gpio_cfg.get(self._gpio_slot(pin), 0)

    def _apply_gpio_events(self, state):
        # A racy false here can only defer a just-enqueued event to the next
        # pacing boundary.  It avoids acquiring a mutex on every normal chunk.
        if not self._gpio_events:
            return
        with self._gpio_lock:
            if not self._gpio_events:
                return
            events, self._gpio_events = self._gpio_events, []
        for pin, level in events:
            state.gpio_hw.drive(pin, level, self._gpio_config_word(pin))

    def _sync_gpio_irq(self, state):
        """Resample level sources and drive the GPIO controller's ICU line."""
        state.gpio_hw.refresh_level_interrupts(self._gpio_config_word)
        if state.gpio_hw.irq_asserted():
            state.pending_irqs |= (1 << 7)
        else:
            state.pending_irqs &= ~(1 << 7)

    def _restore_legacy_gpio_state(self):
        """Build GPIO hardware state for a snapshot made before ``gpio_hw``.

        Old snapshots still contain the controller/pin words in Unicorn's
        mapped MMIO memory.  Their report-side ``gpio_cfg`` capture stopped at
        slot 47, however, so upper P32..P39 (physical slots 48..55) must be
        recovered explicitly for pull/level interrupt behaviour after resume.
        """
        def word(address):
            try:
                return struct.unpack("<I", self.mu.mem_read(address, 4))[0]
            except Exception:
                return 0

        gpio_hw = _gpio.GpioModel(self.gpio_pin_count)
        enabled = word(self.GPIO_INTEN)
        if self.chip.gpio_pins > self.GPIO_PIN_COUNT:
            enabled |= (word(self.GPIO_INTEN2) & 0xFF) << 32
        gpio_hw.write_enable(enabled)
        gpio_hw.write_mode_word(0, word(self.GPIO_INTLV0))
        gpio_hw.write_mode_word(16, word(self.GPIO_INTLV1))
        if self.chip.gpio_pins > self.GPIO_PIN_COUNT:
            gpio_hw.write_mode_word(32, word(self.GPIO_INTLV3), count=8)
            for pin in range(self.GPIO_EXT_PIN_FIRST,
                             self.GPIO_EXT_PIN_FIRST + self.GPIO_EXT_PIN_COUNT):
                slot = self._gpio_slot(pin)
                if slot not in self.state.gpio_cfg:
                    config = word(self.GPIO_BASE + slot * 4)
                    # Zero is also the untouched/reset value.  It has no pull
                    # or input sampling semantics to recover, so do not report
                    # it as a firmware-configured pin merely because RAM read 0.
                    if config:
                        self.state.gpio_cfg[slot] = config
        self.state.gpio_hw = gpio_hw

    def is_paused(self):
        return self._paused

    def pause(self, paused=True):
        """Hold the emulation between basic blocks, or let it run on."""
        if paused:
            self._resume.clear()
            self._paused = True
        else:
            self._paused = False
            self._resume.set()

    def stop(self):
        """Ask the emulation to finish. Safe to call from another thread."""
        self.stopping = True
        self.pause(False)          # a paused run must wake up to see the stop
        try:
            self.mu.emu_stop()
        except Exception:
            pass

    def _arm_watchdog(self):
        """Start/reload the watchdog after a valid key pair or clock enable."""
        state = self.state
        if not state.wdt_clock_enabled or not state.wdt_period_ms:
            state.wdt_deadline = None
            return
        state.wdt_deadline = (state.insn_count
                              + state.wdt_period_ms * self.WDT_INSNS_PER_MS)

    def _watchdog_expired(self, state):
        """Stop the current CPU slice when the watchdog reaches its deadline."""
        deadline = state.wdt_deadline
        if deadline is None or state.insn_count < deadline:
            return False
        # Disarm before emu_stop: block-paced execution can otherwise revisit
        # this boundary while Unicorn unwinds and report the same expiry twice.
        state.wdt_deadline = None
        self._watchdog_reset_pending = True
        try:
            self.mu.emu_stop()
        except Exception:
            pass
        return True

    def _reset_peripheral_models(self):
        """Reset on-chip radio engines while retaining their external peers."""
        if self.wifimac is not None:
            peer = self.wifi_peer
            self.wifimac = _wifimac.MacHw(
                SLOW_TICK_INSNS // (SLOW_TICK_MS * 1000), log=self.net_log)
            self.wifimac.mem = self._guest_mem_callbacks()
            if peer is not None:
                # The access point/router is outside the emulated chip, so it
                # survives the reset.  Only the MAC endpoint is replaced.
                self._bind_wifi_peer(peer, reset_device=True)
        if self.blecore is not None:
            self.blecore = _blecore.BleCore(
                self.XVR_BASE, SLOW_TICK_INSNS * 1000 // SLOW_TICK_MS,
                layout=self._ble_layout)
            self.blecore.mem = self._guest_mem_callbacks()

    def _chip_windows(self):
        """(base, size, hooked) of the regions this chip adds to the common map.

        The 0x900000 window is external SDRAM where the chip has it (BK7252: the
        RT-Thread system heap lives there, so it is plain RAM and must not get
        the XVR hooks, which would answer heap reads with fake register values
        and wreck the free list). Otherwise it is the XVR (RF transceiver)
        register block: one hooked page, so the 0x900100 transaction trigger
        reads back as always-complete. The EJPEG v2 block is registers.
        """
        windows = []
        if self.chip.sdram:
            windows.append((self.chip.sdram[0], self.chip.sdram[1], False))
        elif self.chip.xvr:
            windows.append((self.XVR_BASE, 0x1000, True))
        if self.chip.ejpeg == "v2":
            windows.append((self.EJPEG2_BASE, self.EJPEG2_SIZE, True))
        return windows

    def _watchdog_reset(self):
        """Apply a chip reset and leave the emulator ready for a fresh boot."""
        if self._power_on_context is None:
            raise RuntimeError("watchdog reset requested before emulator setup")

        # A button held across reset is still electrically held afterwards.
        # Preserve only those external pad drives; controller configuration,
        # pending interrupts, RAM, and on-chip peripheral engines reset.
        gpio_inputs = dict(self.state.gpio_hw.inputs)
        self._network_insn_offset += self.state.insn_count
        self._clear_reset_memory()

        self.state = SimulatorState()
        if self.chip.i2c == "v2":
            self.state.hw_i2c[1]["v2"] = True
        if self.gpio_pin_count != self.state.gpio_hw.pin_count:
            self.state.gpio_hw = _gpio.GpioModel(self.gpio_pin_count)
        self.state.gpio_hw.inputs.update(gpio_inputs)
        self.state.adc_samples.update(self._adc_inputs)
        self.flash_state = FlashState()
        self._uart_src = None
        self._resume_at = None
        self._ble_adv_seen.clear()
        self._watchdog_reset_pending = False
        self.run_error = None
        if self.wifimac is not None:
            self.trng_state = 0x2545F491
        self._reset_peripheral_models()
        self.reset_count += 1
        self._exc_epoch += 1
        self.net_log("[WDT] watchdog expired; device reset #%d" % self.reset_count)

    def _decode_deep_sleep_info(self, state, sleep_reg_val):
        """Decode SCTRL RTC timer and GPIO wakeup configuration at deep sleep entry."""
        rosc_en = bool(state.sctrl_rosc_timer & 1)
        ticks = (((state.sctrl_rosc_timer_h & 0xFFFF) << 16)
                 | ((state.sctrl_rosc_timer >> 16) & 0xFFFF))
        if not rosc_en or ticks == 0:
            rtc_seconds = None
        elif ticks % 32000 == 0:
            rtc_seconds = ticks / 32000.0
        elif ticks % 32768 == 0:
            rtc_seconds = ticks / 32768.0
        elif (state.sctrl_rosc_timer_h & 0xFFFF) == 0 and ticks < 3200:
            rtc_seconds = round(ticks / 32.7, 1)
        else:
            rtc_seconds = round(ticks / 32768.0, 1)

        wake_mask = ((state.sctrl_gpio_wakeup_en & 0xFFFFFFFF)
                     | ((state.sctrl_gpio_wakeup_en1 & 0xFF) << 32))
        wake_type = ((state.sctrl_gpio_wakeup_type & 0xFFFFFFFF)
                     | ((state.sctrl_gpio_wakeup_type1 & 0xFF) << 32))
        wake_pins = []
        for p in range(self.gpio_pin_count):
            if (wake_mask >> p) & 1:
                active_lv = 0 if ((wake_type >> p) & 1) else 1
                wake_pins.append((p, active_lv))
        return {
            "sleep_reg": sleep_reg_val & 0xFFFFFFFF,
            "rtc_enabled": bool(rosc_en and ticks > 0),
            "rtc_ticks": ticks,
            "rtc_seconds": rtc_seconds,
            "wake_gpio_mask": wake_mask,
            "wake_gpio_type": wake_type,
            "wake_pins": wake_pins,
            "insn_count": state.insn_count,
        }

    def _resolve_wake_spec(self, spec, info=None):
        """Resolve a wake specification ('rtc', 'gpio', 'gpio:16', 'auto') to (reason, pin)."""
        if info is None:
            info = self.state.deep_sleep_info or {}
        if isinstance(spec, tuple) and len(spec) == 2:
            reason, pin = spec
        else:
            text = str(spec or "auto").strip().lower()
            if text.startswith("gpio:"):
                reason = "gpio"
                pin = int(text.split(":", 1)[1], 0)
            elif text in ("rtc", "gpio", "auto"):
                reason = text
                pin = None
            else:
                raise ValueError("bad deep-sleep wake spec %r (want rtc, gpio, gpio:PIN, or auto)" % spec)
        wake_pins = info.get("wake_pins") or []
        if reason == "auto":
            if info.get("rtc_enabled"):
                return ("rtc", None)
            if wake_pins:
                return ("gpio", wake_pins[0][0])
            return ("rtc", None)
        if reason == "gpio" and pin is None:
            pin = wake_pins[0][0] if wake_pins else 0
        return (reason, pin)

    def wake_from_deep_sleep(self, reason="auto", pin=None):
        """Request a wakeup from deep sleep (callable from GUI/host thread)."""
        spec = ("gpio", int(pin)) if (reason == "gpio" and pin is not None) else reason
        self._deep_sleep_wake_pending = self._resolve_wake_spec(spec)
        self.pause(False)

    def _flush_report_periph(self, state):
        """Flush any pending [EMU_GPIO]/[EMU_PWM]/[EMU_I2C]/[EMU_UART1] lines."""
        if not self._emit_insns:
            return
        if not (state.gpio_dirty or state.pwm_dirty or state.i2c_dirty or state.uart1_events):
            return
        buf = sys.__stdout__.buffer
        if not state.uart_at_line_start:
            buf.write(b"\n")
            state.uart_at_line_start = True
        periph_changed = bool(state.gpio_dirty or state.pwm_dirty or state.i2c_dirty)
        for pin in sorted(state.gpio_dirty):
            buf.write(b"[EMU_GPIO] %d %08x\n" % (pin, state.gpio_cfg[pin]))
        for off in sorted(state.pwm_dirty):
            buf.write(b"[EMU_PWM] %03x %08x\n" % (off, state.pwm_regs[off]))
        for scl, sda in sorted(state.i2c_dirty):
            raw = state.i2c_frames.get((scl, sda), b"")
            if raw:
                buf.write(b"[EMU_I2C] %d %d %s\n" % (scl, sda, raw.hex().encode("ascii")))
        if periph_changed:
            buf.write(b"[EMU_PERIPH]\n")
        for _t, _d, _b in state.uart1_events:
            buf.write(b"[EMU_UART1] %d %c %02x\n" % (_t, ord(_d), _b))
        state.gpio_dirty.clear()
        state.pwm_dirty.clear()
        state.i2c_dirty.clear()
        state.uart1_events.clear()
        buf.flush()

    def _enter_deep_sleep(self, sleep_reg_val):
        """Transition into deep sleep when SCTRL_SLEEP (0x00800104) is written with 0xADC1."""
        state = self.state
        if state.in_deep_sleep:
            return
        self._flush_report_periph(state)
        state.in_deep_sleep = True
        state.deep_sleep_count += 1
        info = self._decode_deep_sleep_info(state, sleep_reg_val)
        state.deep_sleep_info = info
        state.deep_sleep_rtc_sec = float(info["rtc_seconds"] or 0.0)
        state.deep_sleep_wake_pins = dict(info["wake_pins"])
        parts = []
        if info["rtc_enabled"] and info["rtc_seconds"] is not None:
            parts.append("RTC=%gs" % info["rtc_seconds"])
        if info["wake_pins"]:
            pins_str = ", ".join("P%d:%s" % (p, "HIGH" if lv else "LOW")
                                 for p, lv in info["wake_pins"])
            parts.append("GPIO=[%s]" % pins_str)
        summary = ", ".join(parts) if parts else "no wake source armed"
        self.net_log("[SLEEP] entered deep sleep (%s)" % summary)
        if self._deep_sleep_wake_queue:
            spec = self._deep_sleep_wake_queue.pop(0)
            self._deep_sleep_wake_pending = self._resolve_wake_spec(spec, info)
        try:
            self.mu.emu_stop()
        except Exception:
            pass

    def _wait_in_deep_sleep(self):
        """Park while in deep sleep until a wake event (RTC/GPIO) or stop occurs."""
        while self.state.in_deep_sleep and not self.stopping:
            if self._gpio_events:
                with self._gpio_lock:
                    events, self._gpio_events = self._gpio_events, []
                info = self.state.deep_sleep_info or {}
                wake_map = dict(info.get("wake_pins") or ())
                for pin, level in events:
                    self.state.gpio_hw.drive(pin, level, self._gpio_config_word(pin))
                    if pin in wake_map and level is not None and level == wake_map[pin]:
                        self._deep_sleep_wake_pending = ("gpio", pin)
            if self._deep_sleep_wake_pending is not None:
                reason, pin = self._deep_sleep_wake_pending
                self._deep_sleep_wake_pending = None
                self._deep_sleep_reset(reason, pin)
                return True
            time.sleep(0.01)
        return False

    def _deep_sleep_reset(self, reason="rtc", pin=None):
        """Wake the chip from deep sleep via warm SCTRL/AON reset."""
        if self._power_on_context is None:
            raise RuntimeError("deep sleep reset requested before emulator setup")

        old = self.state
        gpio_inputs = dict(old.gpio_hw.inputs)
        info = old.deep_sleep_info or self._decode_deep_sleep_info(old, 0xADC1)
        reason, pin = self._resolve_wake_spec((reason, pin), info)
        self._network_insn_offset += old.insn_count
        self._clear_reset_memory()

        new_state = SimulatorState()
        if self.chip.i2c == "v2":
            new_state.hw_i2c[1]["v2"] = True
        if self.gpio_pin_count != new_state.gpio_hw.pin_count:
            new_state.gpio_hw = _gpio.GpioModel(self.gpio_pin_count)
        new_state.gpio_hw.inputs.update(gpio_inputs)
        new_state.adc_samples.update(self._adc_inputs)

        # Always-On (AON) SCTRL wakeup configuration survives deep sleep reset.
        new_state.sctrl_rosc_timer = old.sctrl_rosc_timer
        new_state.sctrl_rosc_timer_h = old.sctrl_rosc_timer_h
        new_state.sctrl_gpio_wakeup_en = old.sctrl_gpio_wakeup_en
        new_state.sctrl_gpio_wakeup_type = old.sctrl_gpio_wakeup_type
        new_state.sctrl_gpio_wakeup_en1 = old.sctrl_gpio_wakeup_en1
        new_state.sctrl_gpio_wakeup_type1 = old.sctrl_gpio_wakeup_type1
        new_state.sctrl_reset_retention = old.sctrl_reset_retention
        new_state.deep_sleep_count = old.deep_sleep_count
        new_state.deep_sleep_info = info
        new_state.deep_sleep_rtc_sec = float(info.get("rtc_seconds") or 0.0)
        new_state.deep_sleep_wake_pins = dict(info.get("wake_pins") or ())

        if reason == "rtc":
            new_state.sctrl_rosc_int_status = True
            new_state.last_wake_reason = "rtc"
            new_state.last_wake_pin = None
            wake_desc = "RTC timer"
        else:
            pin = int(pin or 0)
            wake_type = info.get("wake_gpio_type", 0)
            active_lv = 0 if ((wake_type >> pin) & 1) else 1
            if pin < 32:
                new_state.sctrl_gpio_wakeup_int_status = 1 << pin
                new_state.aon_gpio_wakeup_status = 1 << pin
            else:
                new_state.sctrl_gpio_wakeup_int_status1 = 1 << (pin - 32)
            prev_lv = gpio_inputs.get(pin)
            new_state.gpio_hw.inputs[pin] = active_lv
            new_state.wake_pulse_pin = pin
            new_state.wake_pulse_restore = (prev_lv if prev_lv is not None
                                            and prev_lv != active_lv
                                            else (1 - active_lv))
            new_state.wake_pulse_reads_left = 4
            new_state.wake_reg_sampled = False
            new_state.last_wake_reason = "gpio"
            new_state.last_wake_pin = pin
            wake_desc = "GPIO P%d=%d" % (pin, active_lv)

        # Mirror retained SCTRL words into mapped MMIO memory as well.
        self.mu.mem_write(0x0080011c, struct.pack("<I",
            (new_state.sctrl_rosc_timer & ~0x100)
            | (0x100 if new_state.sctrl_rosc_int_status else 0)))
        self.mu.mem_write(0x00800120, struct.pack("<I", new_state.sctrl_gpio_wakeup_en))
        self.mu.mem_write(0x00800124, struct.pack("<I", new_state.sctrl_gpio_wakeup_type))
        self.mu.mem_write(0x00800128, struct.pack("<I", new_state.sctrl_gpio_wakeup_int_status))
        self.mu.mem_write(0x00800140, struct.pack("<I", new_state.sctrl_rosc_timer_h))
        self.mu.mem_write(0x00800144, struct.pack("<I", new_state.sctrl_gpio_wakeup_en1))
        self.mu.mem_write(0x00800148, struct.pack("<I", new_state.sctrl_gpio_wakeup_type1))
        self.mu.mem_write(0x0080014c, struct.pack("<I", new_state.sctrl_gpio_wakeup_int_status1))
        self.mu.mem_write(0x00808004, struct.pack("<I", new_state.aon_gpio_wakeup_status))

        self.state = new_state
        self.flash_state = FlashState()
        self._uart_src = None
        self._resume_at = None
        self._ble_adv_seen.clear()
        self._watchdog_reset_pending = False
        self.run_error = None
        if self.wifimac is not None:
            self.trng_state = 0x2545F491
        self._reset_peripheral_models()
        self.reset_count += 1
        self._exc_epoch += 1
        self.net_log("[SLEEP] waking from deep sleep (%s); device reset #%d"
                     % (wake_desc, self.reset_count))

    def net_log(self, msg):
        """Print a [WIFI]/[NET] line of our own without splitting a UART line.

        Shares stdout with the guest's UART output, so it goes out on a line
        boundary (a leading newline when the UART is mid-line) and leaves the
        stream at a line start.

        A front end that has somewhere better to put these - its own network
        pane rather than the middle of the firmware's debug log - sets
        net_sink and gets whole lines instead of a byte stream.
        """
        if self.net_sink is not None:
            try:
                self.net_sink(msg)
            except Exception:
                pass
            return
        data = msg.encode("utf-8", "replace") + b"\n"
        if self.uart_sink is not None:
            # A GUI shows these in its log pane, where the user is looking,
            # rather than on the console window behind it.
            for b in b"\n" + data:
                self.uart_sink(2, b)
            return
        out = sys.__stdout__.buffer
        if not self.state.uart_at_line_start:
            data = b"\n" + data
        out.write(data)
        out.flush()
        self.state.uart_at_line_start = True

    def _network_now_us(self):
        """Monotonic time for the AP/router outside a rebooting device."""
        insns = self._network_insn_offset + self.state.insn_count
        return self.wifimac.now_us(insns) if self.wifimac is not None else 0

    def trace_pc(self, address):
        """Report the first execution of an exact guest PC.

        This is a diagnostic/coverage primitive for opaque firmware where the
        last UART message is not the last code that ran.  Bit zero is accepted
        as a Thumb-state tag but Unicorn's code-hook address is always even.
        Call after setup(), before run().
        """
        address = int(address) & 0xFFFFFFFE
        if address in self._pc_trace_addresses:
            return
        self._pc_trace_addresses.add(address)

        def reached(mu, pc, size, user_data):
            if address in self._pc_trace_seen:
                return
            self._pc_trace_seen.add(address)
            self.net_log("[EMU_PC] reached 0x%08x at %d instructions"
                         % (address, self.state.insn_count))

        handle = self.mu.hook_add(UC_HOOK_CODE, reached,
                                  begin=address, end=address)
        self._pc_trace_hooks.append(handle)

    def attach_wifi_peer(self, role="auto", ssid=None, channel=1, peer_mac=None,
                         forwards=(), net_out=False, pcap=None, http_gets=(), mqtt_pubs=(),
                         passphrase=None, tls_psks=(), tuya_legacy_identity_psk=False):
        """Put a virtual station/access point and an IP network behind the MAC model.

        role "auto" lets the peer take the opposite role of whatever the
        device turns out to be (it beacons -> we are a station; it probes ->
        we are an access point). forwards is [(local_port, device_port)] for
        TCP connections from the host into the device; net_out lets the
        device's own connections reach real hosts instead of a logging
        sinkhole; pcap is a file path for an 802.11 capture of the exchange;
        http_gets are paths requested from the device's port 80 once it is
        reachable, each reply logged as a [NET] line; mqtt_pubs are (topic,
        payload) messages the offline broker pushes to the device once it
        subscribes to a matching filter.
        """
        if self.wifimac is None:
            raise ValueError("a Wi-Fi peer needs the MAC model: pass wifi=True / --wifi")
        try:
            from . import wifipeer as _wifipeer, netstack as _netstack
        except ImportError:
            import wifipeer as _wifipeer
            import netstack as _netstack
        mac = peer_mac or b"\x02\x00\x00\x00\x00\x01"
        net = _netstack.NetStack(mac, log=self.net_log, gateway=(role == "ap"),
                                 ip=_netstack.str2ip("192.168.1.1") if role == "ap" else None,
                                 net_out=net_out, forwards=forwards, http_gets=http_gets,
                                 mqtt_pubs=mqtt_pubs, tls_psks=tls_psks,
                                 tuya_legacy_identity_psk=tuya_legacy_identity_psk)
        # Late binding on purpose: a front end may attach its frame sink after
        # the peer, and the snapshot path rebuilds this stack.
        net.on_frame = lambda d, f, us: (self.net_frame_sink(d, f, us)
                                         if self.net_frame_sink is not None else None)
        try:
            peer = _wifipeer.WifiPeer(
                mac, net,
                # decr: what the device's MAC reports about decryption, set on a
                # protected link so the firmware sees a frame its hardware decrypted.
                inject=lambda f, decr=None: self.wifimac.inject(
                    f, self.state.insn_count, decr=decr),
                log=self.net_log, role=role, ssid=ssid, channel=channel, pcap=pcap,
                now_us=self._network_now_us,
                latency_us=self.NET_LATENCY_US, passphrase=passphrase)
        except BaseException:
            # The stack has already bound its forward ports; a peer that
            # failed to start (an unwritable pcap path) must not keep them.
            net.close()
            raise
        self._bind_wifi_peer(peer)
        self.wifi_peer = peer
        return peer

    def close_network(self):
        """Release what attach_wifi_peer opened on the host: the forward
        listeners, relayed sockets and the capture file.

        The command line exits instead. A front end that runs again in the
        same process (the GUI) calls this after run(), or the next run's
        forward finds its port still taken.
        """
        peer, self.wifi_peer = self.wifi_peer, None
        if peer is not None:
            # MacHw lives as long as the emulator object (the GUI keeps that
            # object to display its final state).  Do not let its bound method
            # keep the closed WifiPeer -> NetStack -> TLS credentials alive.
            self.wifimac.on_tx = None
            peer.close()

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
            self.state.last_uart_insn = self.state.insn_count
            if self._snapshot_marker is not None:
                self._snapshot_check_uart(data)

    def _note_uart_tx(self, port, b_val):
        state = self.state
        state.last_uart_insn = state.insn_count
        if b_val == 0x0A:
            state.uart_at_line_start = True
            state.uart_line_tail = b""
        else:
            state.uart_at_line_start = False
            state.uart_line_tail = (state.uart_line_tail + bytes([b_val]))[-24:]
            if any(m in state.uart_line_tail for m in (b"Info:MAIN:", b"Warn:CFG:")):
                state.obk_boot_seen = True
            if any(m in state.uart_line_tail for m in (b"RT -     ", b"rt-thread", b"[I/FAL]", b"[D/FAL]")):
                state.rtt_boot_seen = True
            if b"msh />" in state.uart_line_tail:
                state.rtt_boot_seen = True
                state.msh_prompt_seen = True
                state.msh_uart_port = port

    def _camera_i2c_read(self, slave_id, reg_addr):
        """Read one register from the simulated DVP camera sensor (GC0328C at 0x21)."""
        state = self.state
        state.camera_last_i2c_insn = state.insn_count
        reg_addr &= 0xFF
        if slave_id in (0x21, 0x42):
            if reg_addr == 0xF0:
                if state.camera_sensor is None:
                    state.camera_sensor = "GC0328C"
                state.camera_sensor_probed = True
                return 0x9D
            if reg_addr == 0xF1:
                return 0x28
            if reg_addr == 0x00:
                return 0x00
        return state.camera_sensor_regs.get((slave_id, reg_addr), 0x00)

    def _camera_i2c_write(self, slave_id, reg_addr, data_byte):
        """Write one register to the simulated DVP camera sensor."""
        state = self.state
        state.camera_last_i2c_insn = state.insn_count
        if state.camera_sensor is None and slave_id in (0x21, 0x42):
            state.camera_sensor = "GC0328C"
            state.camera_sensor_probed = True
        state.camera_sensor_regs[(slave_id, reg_addr & 0xFF)] = data_byte & 0xFF
        state.camera_i2c_writes += 1

    def _hw_i2c_write(self, bus_num, offset, value):
        """Handle MMIO writes to I2C1 (0x00802300, IRQ 2) or I2C2 (0x00802600, IRQ 5)."""
        state = self.state
        bus = state.hw_i2c[bus_num]
        irq_bit = (1 << 2) if bus_num == 1 else (1 << 5)
        if offset == 0x08 or (offset == 0x00 and (value & 0x80000000)):
            bus["v2"] = True

        if not bus["v2"]:
            # 2-register I2C1 layout (BK7252 / BK7231): +0x00 CONFIG, +0x04 DAT
            if offset == 0x04:
                bus["last_dat"] = value & 0xFF
            elif offset == 0x00:
                if not (value & 1) or (value & (1 << 2)):
                    bus["cfg"] = value & ~((1 << 1) | (1 << 2) | (1 << 16) | (1 << 19))
                    bus["phase"] = 0
                    state.pending_irqs &= ~irq_bit
                elif value & (1 << 1):
                    slave_id = (bus["last_dat"] >> 1) & 0x7F
                    is_read = bool(bus["last_dat"] & 1)
                    bus["slave"] = slave_id
                    bus["phase"] = 3 if is_read else 1
                    bus["cfg"] = (value & ~(1 << 19)) | (1 << 16) | (1 << 17) | (1 << 1)
                    state.pending_irqs |= irq_bit
                elif bus["phase"] != 0:
                    if bus["phase"] == 1:
                        bus["reg"] = bus["last_dat"] & 0xFF
                        bus["phase"] = 2
                    elif bus["phase"] == 2:
                        self._camera_i2c_write(bus["slave"], bus["reg"], bus["last_dat"])
                        bus["reg"] = (bus["reg"] + 1) & 0xFF
                    bus["cfg"] = (value & ~((1 << 1) | (1 << 19))) | (1 << 16) | (1 << 17)
                    state.pending_irqs |= irq_bit
                else:
                    bus["cfg"] = value & ~((1 << 16) | (1 << 19))
        else:
            # 3-register I2C2 / BK7252N-I2C1 layout: +0x00 CONFIG, +0x04 STA, +0x08 DAT
            if offset == 0x00:
                bus["cfg"] = value & 0xFFFFFFFF
                if not (value & 0x80000000):
                    bus["phase"] = 0
                    bus["sta"] = 0
                    state.pending_irqs &= ~irq_bit
            elif offset == 0x08:
                bus["last_dat"] = value & 0xFF
            elif offset == 0x04:
                if value & (1 << 9):
                    bus["sta"] = value & ~((1 << 0) | (1 << 9) | (1 << 10) | (1 << 14) | (1 << 15))
                    bus["phase"] = 0
                    state.pending_irqs &= ~irq_bit
                elif value & (1 << 10):
                    slave_id = (bus["last_dat"] >> 1) & 0x7F
                    is_read = bool(bus["last_dat"] & 1)
                    bus["slave"] = slave_id
                    bus["phase"] = 3 if is_read else 1
                    bus["sta"] = (value & ~(1 << 15)) | (1 << 0) | (1 << 8) | (1 << 10) | (1 << 14)
                    state.pending_irqs |= irq_bit
                elif bus["phase"] != 0:
                    if bus["phase"] == 1:
                        bus["reg"] = bus["last_dat"] & 0xFF
                        bus["phase"] = 2
                    elif bus["phase"] == 2:
                        self._camera_i2c_write(bus["slave"], bus["reg"], bus["last_dat"])
                        bus["reg"] = (bus["reg"] + 1) & 0xFF
                    bus["sta"] = (value & ~((1 << 10) | (1 << 15))) | (1 << 0) | (1 << 8) | (1 << 14)
                    state.pending_irqs |= irq_bit
                else:
                    bus["sta"] = value & ~((1 << 0) | (1 << 15))

    def _hw_i2c_read(self, bus_num, offset):
        """Handle MMIO reads from I2C1 (0x00802300) or I2C2 (0x00802600)."""
        bus = self.state.hw_i2c[bus_num]
        if not bus["v2"]:
            if offset == 0x00:
                return bus["cfg"]
            if offset == 0x04:
                val = self._camera_i2c_read(bus["slave"], bus["reg"])
                bus["reg"] = (bus["reg"] + 1) & 0xFF
                return val & 0xFF
            return 0
        if offset == 0x00:
            return bus["cfg"]
        if offset == 0x04:
            return bus["sta"]
        if offset == 0x08:
            val = self._camera_i2c_read(bus["slave"], bus["reg"])
            bus["reg"] = (bus["reg"] + 1) & 0xFF
            return val & 0xFF
        return 0

    def _build_camera_frame(self, frame_idx, target_len=1180):
        """Synthesize a baseline JFIF JPEG frame and matching 64x48 RGB preview."""
        w_prev, h_prev = 64, 48
        rgb = bytearray(w_prev * h_prev * 3)
        sun_x = 14 + (frame_idx * 3) % 36
        sun_y = 12
        bars = (
            (235, 235, 235), (235, 220, 40), (40, 210, 220), (45, 205, 75),
            (215, 55, 205), (225, 50, 50), (45, 75, 230), (35, 35, 45),
        )
        for y in range(h_prev):
            for x in range(w_prev):
                idx = (y * w_prev + x) * 3
                if y >= 38:
                    r, g, b = bars[(x * len(bars)) // w_prev]
                else:
                    dx = x - sun_x
                    dy = y - sun_y
                    if dx * dx + dy * dy <= 25:
                        r, g, b = 255, 225, 70
                    elif y < 24:
                        r = 35 + (y * 3)
                        g = 115 + (y * 4)
                        b = 215 + (y * 1)
                    else:
                        hill = 24 + ((x * 5 + frame_idx * 2) % 7)
                        if y < hill:
                            r, g, b = 95, 175, 235
                        else:
                            r = 35 + ((x + y) % 25)
                            g = 140 + ((y - 24) * 4)
                            b = 55 + ((x * 2) % 30)
                rgb[idx] = r & 0xFF
                rgb[idx + 1] = g & 0xFF
                rgb[idx + 2] = b & 0xFF

        width = self.state.camera_width or 640
        height = self.state.camera_height or 480
        # Standard baseline JFIF header + quantization + SOF0 + Huffman tables.
        # Byte offsets 0x2B..0x2E fall inside DQT table entries (all non-zero),
        # satisfying Naxclow_MiniCam's non-zero header check at 0x59572.
        q_val = max(4, 16 - (frame_idx % 8))
        dqt = bytes([q_val] * 64)
        header = (
            b"\xff\xd8"
            b"\xff\xe0\x00\x10JFIF\x00\x01\x01\x01\x00\x48\x00\x48\x00\x00"
            b"\xff\xdb\x00\x43\x00" + dqt +
            b"\xff\xc0\x00\x0b\x08" + struct.pack(">HH", height, width) + b"\x01\x01\x11\x00"
            b"\xff\xc4\x00\x1f\x00\x00\x01\x05\x01\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00"
            b"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b"
            b"\xff\xc4\x00\x14\x10\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        )
        sos_and_scan = (
            b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00"
            + bytes([0x7F ^ ((frame_idx + i) & 0x3F) for i in range(64)])
            + b"\xff\xd9"
        )
        meta = ("BekenSimulator %s %dx%d Frame #%04d "
                % (self.state.camera_sensor or "GC0328C", width, height, frame_idx)).encode("ascii")
        pad_needed = max(32, target_len - len(header) - len(sos_and_scan) - 4)
        if len(meta) < pad_needed:
            pattern = bytes([0x41 + ((frame_idx + i) % 26) for i in range(pad_needed - len(meta))])
            com_payload = meta + pattern
        else:
            com_payload = meta[:pad_needed]
        com_seg = b"\xff\xfe" + struct.pack(">H", len(com_payload) + 2) + com_payload
        return header + com_seg + sos_and_scan, bytes(rgb)

    def _tick_camera_ejpeg(self, state):
        """Deliver periodic EJPEG DMA frames while the camera encoder is active."""
        enc_en = bool((state.ejpeg_reg0 & 0x3)
                      or (state.ejpeg_reg0 & ((1 << 4) | (1 << 31)))
                      or (state.ejpeg_reg1 & (1 << 4)))
        if not enc_en:
            return
        fiq_mask = (1 << 23) if self.chip.ejpeg == "v2" else ((1 << 23) | (1 << 29))
        fiq_bits = state.icu_int_enable & fiq_mask
        if not fiq_bits:
            return
        if state.ejpeg_dma_ch is None or state.ejpeg_dma_dst < 0x00400000:
            return
        if state.camera_last_i2c_insn and (state.insn_count - state.camera_last_i2c_insn) < 20_000:
            return
        state.ejpeg_tick_div += 1
        if state.ejpeg_tick_div < 5:
            return
        state.ejpeg_tick_div = 0

        node_len = max(512, state.ejpeg_dma_node_len or 1460)
        target_len = min(1180, max(1050, node_len - 64))
        jpeg_bytes, rgb_bytes = self._build_camera_frame(state.camera_frame_count, target_len)
        state.camera_frame_count += 1
        state.camera_last_jpeg = jpeg_bytes
        state.camera_frame_rgb = rgb_bytes

        if state.ejpeg_reg1 & (1 << 16):
            dma_payload = jpeg_bytes + b"\x00" + struct.pack("<I", len(jpeg_bytes))
        else:
            dma_payload = jpeg_bytes
        try:
            self.mu.mem_write(state.ejpeg_dma_dst, dma_payload)
        except UcError:
            return
        state.ejpeg_frame_len = len(jpeg_bytes)
        state.gdma_remain[state.ejpeg_dma_ch] = max(0, node_len - len(dma_payload))
        state.ejpeg_int_stat |= (1 << 1)
        if state.ejpeg_reg0 & (1 << 2):
            state.ejpeg_int_stat |= (1 << 0)
        state.pending_irqs |= fiq_bits
        if state.camera_frame_count == 1:
            self.net_log(
                "[CAMERA] %s streaming %dx%d JPEG (%d bytes/frame, DMA ch%d -> 0x%08x)"
                % (state.camera_sensor or "GC0328C", state.camera_width,
                   state.camera_height, len(jpeg_bytes), state.ejpeg_dma_ch,
                   state.ejpeg_dma_dst))

    def trigger_irq(self):
        if self.state.icu_global_int_en == 0:
            return # Global interrupts disabled
            
        read, write = self._reg_read, self._reg_write
        cpsr = read(UC_ARM_REG_CPSR)
        if (cpsr & 0x80) == 0:  # I bit is clear (IRQs enabled)
            pc = read(UC_ARM_REG_PC)

            # Switch to IRQ mode (0x12), disable IRQs (0x80), clear Thumb bit (0x20)
            new_cpsr = (cpsr & ~0x3F) | 0x92
            write(UC_ARM_REG_CPSR, new_cpsr)
            write(UC_ARM_REG_SPSR, cpsr)
            write(UC_ARM_REG_LR, pc + 4)
            write(UC_ARM_REG_PC, self.exception_vector_base + 0x18)
            # Tells _run_chunked that PC/CPSR moved under it, so the next
            # chunk must start from the vector rather than where the guest
            # was. Cheaper than re-reading both registers every chunk.
            self._exc_epoch += 1

    def trigger_fiq(self):
        """Take the FIQ exception, if the ICU and the CPSR F bit allow it.

        Mirror of trigger_irq for the fast-interrupt line: the ICU's global
        enable has a separate FIQ bit (GINTR_FIQ_EN, bit 1), the CPU masks
        FIQ with CPSR.F (0x40) rather than .I, entry mode is FIQ (0x11) with
        both F and I set, and the vector is 0x1C. The firmware's FIQ stub
        expects LR = return address + 4 exactly like its IRQ stub does.
        """
        if not (self.state.icu_global_int_en & 0x2):
            return
        read, write = self._reg_read, self._reg_write
        cpsr = read(UC_ARM_REG_CPSR)
        if cpsr & 0x40:
            return
        pc = read(UC_ARM_REG_PC)
        new_cpsr = (cpsr & ~0x3F) | 0x11 | 0xC0
        write(UC_ARM_REG_CPSR, new_cpsr)
        write(UC_ARM_REG_SPSR, cpsr)
        write(UC_ARM_REG_LR, pc + 4)
        write(UC_ARM_REG_PC, self.exception_vector_base + 0x1C)
        self._exc_epoch += 1            # see trigger_irq

    def hook_intr(self, mu, intno, user_data):
        pc = mu.reg_read(UC_ARM_REG_PC)
        if intno == 2: # EXCP_SWI
            cpsr = mu.reg_read(UC_ARM_REG_CPSR)
            new_cpsr = (cpsr & ~0x3F) | 0x13 | 0x80
            mu.reg_write(UC_ARM_REG_CPSR, new_cpsr)
            mu.reg_write(UC_ARM_REG_SPSR, cpsr)
            mu.reg_write(UC_ARM_REG_LR, pc)
            mu.reg_write(UC_ARM_REG_PC, self.exception_vector_base + 0x08)

    # Interrupt/pacing bookkeeping does NOT run per instruction. A
    # per-instruction UC_HOOK_CODE callback dominated runtime (measured ~3.7x
    # slower than a block hook on a real boot): Unicorn must leave the
    # translated block to call into Python for every instruction, which
    # defeats block execution.
    #
    # It used to run once per BASIC BLOCK (UC_HOOK_BLOCK -> hook_block), which
    # is still available under EMU_BLOCK_PACING=1 and is what the insn_count
    # scale is defined by. That too turned out to be the whole cost of the
    # emulator: ~1.9 counted instructions per block means a ctypes callback
    # into Python every two instructions, and a profile of a 15 s boot put
    # ~80% of the time in hook_block plus Unicorn's callback glue. _run_chunked
    # does the same work between counted emu_start() calls instead, so nothing
    # calls into Python between chunks (see run()).
    #
    # insn_count is an ESTIMATE either way, and deliberately the SAME estimate:
    # block byte size >> 2, i.e. the ARM instruction width. Pure-ARM code
    # counts exactly; Thumb blocks undercount by ~2.4x (slightly slowing the
    # device clock there). The chunked path reproduces that by scaling each
    # chunk by the CPU state it ended in - see CHUNK_SCALE_THUMB.
    # Device time is already a fiction paced off this counter, so the
    # approximation only nudges the wall<->device-time ratio - it never changes
    # what the firmware computes.
    # The periodic sources fire on threshold crossing (>= since last tick)
    # rather than exact modulo, because the per-chunk increment is variable.
    IRQ_CHECK_STRIDE = 20  # retained for reference; pacing is now per-chunk

    @staticmethod
    def _old_pwm_timer_status_mask(pwm_ctl):
        """Status bits for enabled+interrupting old-PWM timer channels."""
        mask = 0
        for channel in range(6):
            if ((pwm_ctl >> (4 * channel)) & 0xF) == 0x7:
                mask |= 1 << channel
        return mask

    def hook_block(self, mu, address, size, user_data):
        """Legacy per-basic-block pacing (EMU_BLOCK_PACING=1); see _run_chunked."""
        state = self.state
        state.insn_count += (size >> 2) or 1
        # Where execution is right now. Read by the GUI status bar from another
        # thread, which is why it is captured here rather than via reg_read -
        # Unicorn is not safe to query while it is running.
        state.last_pc = address
        if self.stopping:
            # The chunked loop checks this between chunks; here it is the only
            # check, and it also catches a stop() made before emu_start, which
            # Unicorn itself would ignore.
            mu.emu_stop()
            return
        self._pacing(state)

    def _pacing(self, state):
        """All the periodic device work, run once per block or once per chunk.

        Called with state.insn_count already advanced for the piece of
        execution that just happened, from a point where the CPU is between
        instructions (a basic-block boundary, or an emu_start() chunk end).
        """
        # GUI pause. One attribute test per chunk on the normal path; the
        # blocking wait is only reached once actually paused, and it releases
        # the GIL so the UI thread gets it all.
        if self._paused:
            self._resume.wait()

        if self._watchdog_expired(state):
            return

        # External GPIO changes are queued by a GUI/host thread and become
        # hardware state only here, between guest instructions.  Resampling
        # level modes on every pacing boundary also gives held-low/high inputs
        # their real level-triggered behaviour after the ISR clears INTSTA.
        self._apply_gpio_events(state)
        self._sync_gpio_irq(state)

        # Slow tick: raise the periodic sources' pending.
        if state.insn_count - state.last_slow >= SLOW_TICK_INSNS:
            state.last_slow = state.insn_count
            if self.meter_cf_pin is not None or self.meter_cf1_pin is not None:
                self._tick_energy_meter_pulses(state)
                self._sync_gpio_irq(state)
            # Report-only: emit the running instruction estimate a few times per
            # boot. Guarded by _emit_insns (off unless the harness asks) and
            # throttled, so this costs nothing on a normal run.
            if self._emit_insns:
                # An RT-Thread shell prompt ('msh />') ends without a newline
                # and then parks waiting for console input forever. Once the
                # UART has been silent for 500k instructions (10 slow ticks),
                # close the prompt line so pending [EMU_GPIO]/[EMU_PERIPH]/
                # [EMU_INSNS] lines can flush without splitting any other stream.
                if (not state.uart_at_line_start
                        and (state.uart_line_tail.rstrip(b" ").endswith(b"msh />")
                             or b"WSL02810" in state.uart_line_tail)
                        and state.insn_count - state.last_uart_insn >= 500_000):
                    buf = sys.__stdout__.buffer
                    buf.write(b"\n")
                    buf.flush()
                    state.uart_at_line_start = True
                    state.uart_line_tail = b""
                # Peripheral changes go out on THIS tick (the slow one)
                # rather than the 1M-instruction one, and only for registers
                # that actually changed. The emulator never exits on its own -
                # it is killed once the harness has what it needs - so whatever
                # has not been emitted by then is simply lost. The harness
                # filters these lines out of the displayed log.
                if state.uart_at_line_start:
                    self._flush_report_periph(state)
                # Same line-boundary rule as the lines above, and for the same
                # reason: this one used to open with "\n" unconditionally, so
                # when the 1M-instruction tick landed mid-line it cut a guest
                # log line in two - and since the harness filters [EMU_INSNS]
                # out, what the test saw was a bare line break inside a string
                # it was asserting on. Holding the emit until the stream is at
                # a line start costs nothing: the next slow tick takes it.
                if (state.insn_count - state.last_emit >= 1_000_000
                        and state.uart_at_line_start):
                    state.last_emit = state.insn_count
                    buf = sys.__stdout__.buffer
                    buf.write(b"[EMU_INSNS] %d\n" % state.insn_count)
                    buf.flush()
            # PWM/Timer (Tuya T/Q-family RTOS tick through 0x802A04).  Each old
            # PWM channel owns one nibble in PWM_CTL: enable, interrupt-enable,
            # then the two-bit mode.  PMODE_TIMER is 1, hence an active clock
            # channel has nibble 0b0111.  Do not hard-code channel zero here:
            # the BK7231Q SDK uses channel 3 (CTL=0x7000, period at +0x20), and
            # its ISR dispatch table quite correctly has no callback in slot 0.
            # Raising bit 0 made the ISR acknowledge a source with no handler,
            # leaving the FreeRTOS tick -- and every Tuya software timer -- at
            # zero forever.
            #
            # Not on a pwm_new firmware: that block has no such status register,
            # its ISR never acks 0x802A04, and the bit would stay pending.
            if state.icu_int_enable & (1 << 9) and not state.pwmn_seen:
                pwm_ctl = state.pwm_regs.get(0, 0)
                state.pwm_status |= self._old_pwm_timer_status_mask(pwm_ctl)
            if state.icu_int_enable & (1 << 8) and self.chip.timer == "v1":
                # BKTIMER (OpenBK + BK7252N camera Timer 5): a fixed tick whatever is programmed
                if self.chip.bktimer_v2:
                    state.timer3_5_ctl |= (((state.timer3_5_ctl & 0x7) or 0x1) << 7)
                else:
                    state.timer3_5_ctl |= (1 << 7) # Set Timer 3 Int Status
            if (self.chip.audio is not None
                    and (state.icu_int_enable & (1 << 10))
                    and (state.audio_fifo_conf & 0x3C)):
                state.audio_dac_writes = 0
                state.pending_irqs |= (1 << 10)
            self._tick_camera_ejpeg(state)
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
            if self.blecore is not None:
                # FIQ_BTDM (29) carries the core-side interrupts (sleep wake,
                # timers, SW request) into rwip_isr; FIQ_BLE (30) the BLE
                # event side into rwble_isr. Both are levels: they stay up
                # until the firmware acks them in INTACK0/1.
                core_irq, ble_irq = self.blecore.tick(state.insn_count)
                self._report_ble_adv()
                state.pending_irqs = ((state.pending_irqs & ~((1 << 29) | (1 << 30)))
                                      | ((1 << 29) if core_irq else 0)
                                      | ((1 << 30) if ble_irq else 0))
            if self.wifimac is not None:
                # The peer first (it may queue frames), then the MAC: ABS
                # timers, TX completions, RX delivery, TBTT. The six MAC FIQ
                # lines (ICU bits 16..22) are levels recomputed by the model.
                if self.wifi_peer is not None:
                    self.wifi_peer.tick(state.insn_count)
                lines = self.wifimac.tick(state.insn_count)
                state.pending_irqs = (state.pending_irqs & ~_wifimac.LINE_MASK) | lines

        if (state.pwmn_stat if state.pwmn_seen else state.pwm_status & 0x3F):
            state.pending_irqs |= (1 << 9)
        else:
            state.pending_irqs &= ~(1 << 9)

        if self.chip.timer == "v2":
            self._tick_hw_timers(state)
        if (state.timer0_2_ctl & (0x7 << 7)) or (state.timer3_5_ctl & (0x7 << 7)):
            state.pending_irqs |= (1 << 8)
        else:
            state.pending_irqs &= ~(1 << 8)

        ready = state.pending_irqs & state.icu_int_enable
        if ready & 0x0000FFFF:
            self.trigger_irq()
        if ready & 0xFFFF0000:
            self.trigger_fiq()

        # Fast tick (~every 1000 insns): UART RX/TX and SARADC interrupts.
        if state.insn_count - state.last_fast >= FAST_TICK_INSNS:
            state.last_fast = state.insn_count
            # A transmission in flight completes on this finer grain (~200 us
            # of device time), so a TX confirmation is not held for a whole
            # slow tick. Nothing to do on the common path: `busy` is one
            # attribute test.
            if self.wifimac is not None and self.wifimac.busy:
                lines = self.wifimac.tick(state.insn_count)
                state.pending_irqs = (state.pending_irqs & ~_wifimac.LINE_MASK) | lines
            uart1_due, uart2_due = self._uart_irqs_due(state)
            if uart1_due:
                state.pending_irqs |= (1 << 0)
                self.trigger_irq()
            if uart2_due:
                state.pending_irqs |= (1 << 1)
                self.trigger_irq()

        # Snapshot request: take it only after every periodic action due at
        # this device-time boundary has completed.  Saving before the slow and
        # fast ticks made the continued run perform work that was absent from
        # the image; a restored run then executed its next chunk first and ran
        # that work one boundary late.  We are still between guest
        # instructions here, never inside a memory hook where one is half done.
        if self._snapshot_path is not None:
            if self._snapshot_due or (self._snapshot_at_insns is not None
                                      and state.insn_count >= self._snapshot_at_insns):
                path, self._snapshot_path = self._snapshot_path, None
                self.save_snapshot(path)

    def _uart_irqs_due(self, state):
        """(UART1, UART2): whether the fast tick raises each UART's interrupt.

        UART1: the firmware's TX path arms bit 0 (TX_FIFO_NEED_WRITE_EN); the
        RX console arms bit 1 (RX_FIFO_NEED_READ_EN) / bit 6 (RX_STOP_END_EN).
        It fires for TX always-ready, and for RX only while there are queued
        bytes to deliver - otherwise a spurious RX interrupt storm.
        """
        uart1 = bool(state.icu_int_enable & (1 << 0)) and bool(
            (state.uart1_int_enable & 0x01)
            or (self._rx_active() and (state.uart1_int_enable & 0x42)))
        uart2 = bool(state.icu_int_enable & (1 << 1)) and bool(
            (state.uart2_int_enable & 0x01)
            or (self._rx_active_uart2() and (state.uart2_int_enable & 0x42)))
        return uart1, uart2

    def _timer_period(self, idx):
        """Instructions between expiries of hardware timer idx (0-2 bank 0, 3-5 bank 1)."""
        bank = idx // 3
        ctl = self.state.timer3_5_ctl if bank else self.state.timer0_2_ctl
        divider = ((ctl >> 3) & 0xF) + 1
        return max(1, self.state.hw_timer_counter[idx] * divider
                   * self.TIMER_INSNS_PER_SEC // self.TIMER_SOURCE_HZ[bank])

    def _timer_enable_changed(self, bank, old_enable):
        """A bank's ctl was written: start the timers just enabled, stop the ones cleared."""
        if self.chip.timer != "v2":
            return
        state = self.state
        enable = (state.timer3_5_ctl if bank else state.timer0_2_ctl) & 0x7
        for n in range(3):
            idx = 3 * bank + n
            if enable & (1 << n):
                if not old_enable & (1 << n):
                    state.hw_timer_due[idx] = state.insn_count + self._timer_period(idx)
            else:
                state.hw_timer_due[idx] = None

    def _tick_hw_timers(self, state):
        """Raise the irq_status of every timer whose period has elapsed.

        Runs at the pacing grain, so a period shorter than one chunk expires
        once per chunk: the missed expiries merge into a single pending status,
        as they would for a firmware that was not scheduled for a while.
        """
        now = state.insn_count
        for idx, due in enumerate(state.hw_timer_due):
            if due is None or now < due:
                continue
            status = 1 << (7 + idx % 3)
            if idx < 3:
                state.timer0_2_ctl |= status
            else:
                state.timer3_5_ctl |= status
            period = self._timer_period(idx)
            state.hw_timer_due[idx] = due + period if due + period > now else now + period

    def _next_timer_due(self, state):
        """insn_count of the soonest hardware timer expiry, or None."""
        dues = [due for due in state.hw_timer_due if due is not None]
        return min(dues) if dues else None

    def _fast_tick_idle(self, state):
        """True when the fast tick would do nothing at all: no transmission in
        flight in the Wi-Fi MAC and no UART interrupt due."""
        if self.wifimac is not None and self.wifimac.busy:
            return False
        uart1_due, uart2_due = self._uart_irqs_due(state)
        return not (uart1_due or uart2_due)

    def _report_ble_adv(self):
        """Print any advertising payload the controller has staged in EM.

        Where EM lives comes from the core model (BleCore.em_base): on the 5.2
        core the firmware hands the hardware its runtime pointer (`extern
        uint8_t *ex_mem`, different in every build) through a base register;
        on 5.1 EM sits at a fixed address. Either way no RAM-wide scan.
        Emitted on a line boundary like the other capture lines, and only when
        a payload is new - a boot produces one or two, not a stream.
        """
        if not self.state.uart_at_line_start:
            return
        found = self.blecore.find_adv(
            lambda addr, size: bytes(self.mu.mem_read(addr, size)))
        if not found:
            return
        base = self.blecore.em_base()
        for offset, payload, fields in found:
            if payload in self._ble_adv_seen:
                continue
            self._ble_adv_seen.add(payload)
            # net_log, so a GUI shows these in its network pane like the
            # other capture lines rather than on the console behind it.
            self.net_log("[BLE_ADV] EM 0x%08x +0x%04x (%d bytes): %s"
                         % (base, offset, len(payload), payload.hex(" ")))
            for line in _blecore.BleCore.describe_ad(fields):
                self.net_log("[BLE_ADV]   %s" % line.rstrip())

    def _gdma_channel(self, address):
        """Channel number if address is a GDMA CONF register, else None."""
        if self.chip.gdma == "v2":
            if not (0x00809040 <= address < 0x00809240):
                return None
            channel, within = divmod(address - 0x00809040, 0x40)
            return channel if within == 0 else None
        if not (self.GDMA_BASE <= address
                < self.GDMA_BASE + self.GDMA_CHANNELS * self.GDMA_CH_STRIDE):
            return None
        offset = address - self.GDMA_BASE
        channel, within = divmod(offset, self.GDMA_CH_STRIDE)
        return channel if within == 0 else None

    def _gdma_start(self, mu, channel, conf):
        """Run one general-DMA transfer to completion, immediately.

        The firmware kicks a channel by setting GDMA_X_DMA_EN in its CONF
        register and then spins on that same bit waiting for the hardware to
        clear it - see gdma_enable()/gdma_memcpy() in the vendor SDK, which
        wrap the whole thing in GLOBAL_INT_DISABLE(). With no DMA engine here
        the bit stayed set forever and the firmware wedged in that spin with
        interrupts masked, which is exactly where an OpenBeken boot stopped
        once it reached Wi-Fi AP setup.

        Real hardware transfers in the background; doing it inline is both
        simpler and indistinguishable to the guest, because the guest cannot
        run again until the copy is finished anyway.
        """
        length = ((conf >> self.GDMA_TRANS_LEN_POSI) & 0xFFFF) + 1
        base = (0x00809040 + channel * 0x40
                if self.chip.gdma == "v2"
                else self.GDMA_BASE + channel * self.GDMA_CH_STRIDE)
        try:
            dst = struct.unpack("<I", mu.mem_read(base + 0x04, 4))[0]
            src = struct.unpack("<I", mu.mem_read(base + 0x08, 4))[0]
        except UcError:
            return

        # Only a straight incrementing block copy is modelled. A channel with a
        # fixed source or destination is streaming to or from a peripheral FIFO
        # that this emulator does not implement, so inventing bytes for it would
        # be worse than leaving the buffer alone - the enable bit is still
        # cleared either way, so the firmware is never left spinning.
        both_inc = (conf & self.GDMA_SRCADDR_INC) and (conf & self.GDMA_DSTADDR_INC)
        if both_inc and length:
            try:
                mu.mem_write(dst, bytes(mu.mem_read(src, length)))
            except UcError as e:
                self._print("[GDMA] ch%d copy 0x%08x -> 0x%08x (%d bytes) failed: %s"
                            % (channel, src, dst, length, e))
        elif ((conf & self.GDMA_SRCADDR_INC) and not (conf & self.GDMA_DSTADDR_INC)
              and dst == 0x0080270c and 24 <= length <= 8192):
            # SPI_DAT (0x0080270c) fixed-destination DMA: used by WS2812 /
            # SM16703P addressable RGB LED strip drivers (1 SPI byte per bit).
            try:
                self.state.ws2812_spi = bytes(mu.mem_read(src, length))
            except UcError:
                pass
        elif (not (conf & self.GDMA_SRCADDR_INC) and (conf & self.GDMA_DSTADDR_INC)
              and (src == self.EJPEG_FIFO.get(self.chip.ejpeg) or dst >= 0x00400000)):
            # EJPEG RX FIFO -> circular RAM/SDRAM destination buffer.
            self.state.ejpeg_dma_ch = channel
            self.state.ejpeg_dma_dst = dst
            self.state.ejpeg_dma_node_len = length
            self.state.gdma_remain[channel] = length

    def hook_mem_write_mmio(self, mu, access, address, size, value, user_data):
        # Bounds test inlined rather than calling _gdma_channel(): this hook
        # runs on every MMIO write in the system, so the common path must not
        # pay for an extra Python call.
        if self.GDMA_BASE <= address < self.GDMA_END:
            channel = self._gdma_channel(address)
            if channel is not None:
                if value & self.GDMA_EN:
                    self._gdma_start(mu, channel, value)
                # Whatever the guest reads back, the enable bit is clear: the
                # transfer above already happened.
                self.state.gdma_conf[channel] = value & ~self.GDMA_EN
                return
        if 0x00802300 <= address < 0x0080230C:
            self._hw_i2c_write(1, address - 0x00802300, value)
            return
        if 0x00802600 <= address < 0x0080260C:
            self._hw_i2c_write(2, address - 0x00802600, value)
            return
        if self.chip.ejpeg == "v1" and 0x0080A000 <= address < 0x0080A100:
            off = address - 0x0080A000
            if off == 0x00:
                self.state.ejpeg_reg0 = value & 0xFFFFFFFF
                x_code = (value >> 8) & 0x3
                if x_code:
                    self.state.camera_width = {1: 640, 2: 320, 3: 1280}.get(x_code, 640)
            elif off == 0x04:
                self.state.ejpeg_reg1 = value & 0xFFFFFFFF
                x_pix = (value >> 8) & 0xFF
                y_pix = (value >> 24) & 0xFF
                if x_pix >= 20:
                    self.state.camera_width = x_pix * 8
                if y_pix >= 15:
                    self.state.camera_height = y_pix * 8
                elif y_pix in (1, 2, 3):
                    self.state.camera_height = {1: 480, 2: 240, 3: 720}[y_pix]
            elif off == 0x08:
                self.state.ejpeg_reg2 = value & 0xFFFFFFFF
            elif off == 0x0C:
                self.state.ejpeg_reg3 = value & 0xFFFFFFFF
            elif off == 0x18:
                self.state.ejpeg_int_stat &= ~(value & 0xFFFFFFFF)
                if not self.state.ejpeg_int_stat:
                    self.state.pending_irqs &= ~((1 << 23) | (1 << 29))
            try:
                mu.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))
            except Exception:
                pass
            return
        if self.chip.ejpeg == "v2" and 0x00A03000 <= address < 0x00A05000:
            if address < 0x00A03100:
                off = address - 0x00A03000
                if off == 0x08:
                    self.state.ejpeg_reg0 = value & 0xFFFFFFFF
                elif off == 0x18:
                    self.state.ejpeg_int_stat &= ~(value & 0xFFFFFFFF)
                    if not self.state.ejpeg_int_stat:
                        self.state.pending_irqs &= ~((1 << 23) | (1 << 29))
                elif off == 0x34:
                    if value & 0x2:
                        self.state.ejpeg_reg1 |= (1 << 16)
                    else:
                        self.state.ejpeg_reg1 &= ~(1 << 16)
            elif 0x00A04000 <= address < 0x00A04100:
                off = address - 0x00A04000
                if off == 0x14:
                    x_pix = value & 0xFF
                    y_pix = (value >> 8) & 0xFF
                    if x_pix >= 20:
                        self.state.camera_width = x_pix * 8
                    if y_pix >= 15:
                        self.state.camera_height = y_pix * 8
            try:
                mu.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))
            except Exception:
                pass
            return
        if self.wifimac is not None and _wifimac.CORE <= address < _wifimac.CORE + _wifimac.SIZE:
            self.wifimac.write(address, value, self.state.insn_count)
            self.state.pending_irqs = ((self.state.pending_irqs & ~_wifimac.LINE_MASK)
                                       | self.wifimac.lines)
        if address == self.ICU_INT_ENABLE:
            self.state.icu_int_enable = value
            self._print(f"[DEBUG] ICU_INT_ENABLE set to: 0x{value:08x}")
        if address == self.ICU_GLOBAL_INT_EN: self.state.icu_global_int_en = value
        if address == self.ICU_PERI_CLK_PWD:
            enabled = not bool(value & self.ICU_WDT_CLK_PWD)
            if enabled != self.state.wdt_clock_enabled:
                self.state.wdt_clock_enabled = enabled
                if enabled:
                    self._arm_watchdog()
                else:
                    self.state.wdt_deadline = None
        if address == self.WDT_CTRL:
            key = (value >> 16) & 0xFF
            period = value & 0xFFFF
            if key == self.WDT_FIRST_KEY:
                self.state.wdt_first_period = period
            elif (key == self.WDT_SECOND_KEY
                  and self.state.wdt_first_period == period):
                self.state.wdt_first_period = None
                self.state.wdt_period_ms = period
                self._arm_watchdog()
            else:
                # An incomplete or mismatched unlock sequence has no effect on
                # the running timer and cannot be completed by a later key.
                self.state.wdt_first_period = None
        if address == self.ICU_INT_STATUS:
            self.state.pending_irqs &= ~value
            # The MAC FIQ lines are levels: an ICU clear does not take them
            # down while their status is still pending in the MAC itself.
            if self.wifimac is not None:
                self.state.pending_irqs |= self.wifimac.lines
        if address == 0x00802110: self.state.uart1_int_enable = value
        if address == 0x00802210: self.state.uart2_int_enable = value
        # Arm the XVR RF/BLE busy self-clear (opt-in) when the firmware sets bit
        # 31 to start an operation. The write still lands in mapped memory.
        if self.xvr_selfclear and address in (0x009000f8, 0x00900000) and (value & 0x80000000):
            self.state.xvr_armed.add(address)
        if self.blecore is not None and self.XVR_BASE <= address < self.XVR_BASE + self.blecore.window:
            self.blecore.write(address, value, self.state.insn_count)

        # GPIO input/interrupt controller.  Keep actual pin config slots apart
        # from FUNC_CFG and INTEN/INTLV/INTSTA: the old broad range treated all
        # of those controller words as imaginary pins 32+ and even applied pull
        # synthesis to their reads.
        _pin = self._gpio_pin_at(address)
        if _pin is not None:
            _slot = self._gpio_slot(_pin)
            _old = self.state.gpio_cfg.get(_slot, 0)
            _new = value & 0xFFFFFFFF
            self.state.gpio_hw.config_changed(_pin, _old, _new)
            self.state.gpio_cfg[_slot] = _new
            self._i2c_pin_edge(self.state, _pin, _old, _new)
            if self._emit_insns:
                self.state.gpio_dirty.add(_slot)
            self._sync_gpio_irq(self.state)
        elif address == self.GPIO_INTEN:
            self.state.gpio_hw.write_enable(
                (self.state.gpio_hw.int_enable & ~0xFFFFFFFF) | (value & 0xFFFFFFFF))
            self._sync_gpio_irq(self.state)
        elif address == self.GPIO_INTLV0:
            self.state.gpio_hw.write_mode_word(0, value)
            self._sync_gpio_irq(self.state)
        elif address == self.GPIO_INTLV1:
            self.state.gpio_hw.write_mode_word(16, value)
            self._sync_gpio_irq(self.state)
        elif address == self.GPIO_INTSTA:
            self.state.gpio_hw.clear_status(value & 0xFFFFFFFF)
            self._sync_gpio_irq(self.state)
        elif self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTEN2:
            low = self.state.gpio_hw.int_enable & 0xFFFFFFFF
            self.state.gpio_hw.write_enable(low | ((value & 0xFF) << 32))
            self._sync_gpio_irq(self.state)
        elif self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTLV3:
            self.state.gpio_hw.write_mode_word(32, value, count=8)
            self._sync_gpio_irq(self.state)
        elif self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTSTA2:
            self.state.gpio_hw.clear_status((value & 0xFF) << 32)
            self._sync_gpio_irq(self.state)
        elif address in (self.GPIO_FUNC_CFG, self.GPIO_FUNC_CFG_2, self.GPIO_FUNC_CFG_3):
            _slot = (address - self.GPIO_BASE) // 4
            self.state.gpio_cfg[_slot] = value & 0xFFFFFFFF
            if self._emit_insns:
                self.state.gpio_dirty.add(_slot)

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
        if address in self.PWMN_CTRL_REGS and self.chip.pwm == "v2":
            # Write-1-to-clear the group's interrupt status (see the read hook).
            group = (address - self.PWMN_BASE) >> 6
            self.state.pwmn_seen = True
            self.state.pwmn_stat &= ~(((value >> 30) & 0x3) << (2 * group))

        if self.PWM_BASE <= address < self.PWM_BASE + 0x1C0:
            _off = address - self.PWM_BASE
            self.state.pwm_regs[_off] = value & 0xFFFFFFFF
            if self._emit_insns:
                self.state.pwm_dirty.add(_off)

        if self.chip.audio is not None:
            if address == 0x00802B08:
                self.state.audio_fifo_conf = value & 0xFFFFFFFF
                if not (value & 0x3C):
                    self.state.pending_irqs &= ~(1 << 10)
            elif address == 0x00802B1C:
                self.state.audio_dac_writes += 1

        if address == 0x00802A04:
            w1c_mask = value & 0x3F
            self.state.pwm_status = (self.state.pwm_status & ~0x3F) | (value & ~0x3F)
            self.state.pwm_status &= ~w1c_mask
            
        if address == 0x00802A0C:
            old_enable = self.state.timer0_2_ctl & 0x7
            w1c_mask = value & (0x7 << 7)
            self.state.timer0_2_ctl = (value & ~(0x7 << 7)) | (self.state.timer0_2_ctl & (0x7 << 7))
            self.state.timer0_2_ctl &= ~w1c_mask
            self._timer_enable_changed(0, old_enable)

        if address == 0x00802A4C:
            old_enable = self.state.timer3_5_ctl & 0x7
            w1c_mask = value & (0x7 << 7)
            self.state.timer3_5_ctl = (value & ~(0x7 << 7)) | (self.state.timer3_5_ctl & (0x7 << 7))
            self.state.timer3_5_ctl &= ~w1c_mask
            self._timer_enable_changed(1, old_enable)

        if self.chip.timer == "v2":
            for bank, base in enumerate(self.TIMER_BANK_BASE):
                if base <= address < base + 12:
                    self.state.hw_timer_counter[3 * bank + (address - base) // 4] = value & 0xFFFFFFFF

        # UART output. UART2 is the firmware's debug/log port (text). UART1 is
        # the TuyaMCU link to the external device MCU; with --uart1-hex it is
        # shown as tagged hex so the 55 AA protocol frames are readable instead
        # of printing as garbage characters mixed into the log.
        if address == self.UART1_FIFO_PORT:
            b_val = value & 0xFF
            self._note_uart_tx(1, b_val)
            if self._emit_insns:
                self.state.uart1_events.append((self.state.insn_count, 'T', b_val))
            if self.tuyamcu is not None:
                # Feed this transmitted byte to the simulated MCU; when it has a
                # whole frame it answers, and the answer is queued for the guest
                # to read back over UART1 RX (delivered by the RX interrupt path).
                reply = self.tuyamcu.react_stream(bytes([b_val]))
                if reply:
                    with self._rx_lock:
                        self.uart1_rx += reply
            elif self.state.uart1_tx_prev == 0x58 and b_val == 0xAA:
                # BL0942 UART energy-meter read-all request (0x58 0xAA).
                self.state.bl0942_polls += 1
                self.uart1_rx_delay = 0
                reply = self._bl0942_reply_packet()
                with self._rx_lock:
                    self.uart1_rx += reply
            self.state.uart1_tx_prev = b_val
            if self.uart_sink is not None:
                # A GUI shows each UART in its own pane and formats it there,
                # so none of the stdout interleaving below applies.
                self.uart_sink(1, b_val)
                return
            if self.uart1_hex:
                if self._uart_src != 'u1':
                    self._uart_write(b'\n[UART1/MCU] ')
                    self._uart_src = 'u1'
                self._uart_write(b'%02x ' % b_val)
            else:
                self._uart_write(bytes([b_val]))
            return
        if address == self.UART2_FIFO_PORT:
            b_val = value & 0xFF
            self._note_uart_tx(2, b_val)
            if self.uart_sink is not None:
                self.uart_sink(2, b_val)
                return
            if self.uart1_hex and self._uart_src == 'u1':
                self._uart_write(b'\n')
                self._uart_src = 'u2'
            self._uart_write(bytes([b_val]))
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

        # REG_FLASH_PW_CONF (BK7238 256-byte page-write FIFO). For writes >= 256
        # bytes, BK7238's flash_page_write() writes 0x80000000 here to clear the
        # FIFO, pushes 256 individual bytes (one per word write), then triggers
        # FLASH_OPCODE_PP at 0x00803000 once per 256-byte page instead of every
        # 32 bytes. Without this, 4K sector writes only programmed the first 32
        # bytes of each 256-byte page and failed read-back verification.
        if address == 0x00803024:
            if value & 0x80000000:
                self.flash_state.pw_buf.clear()
            else:
                self.flash_state.pw_buf.append(value & 0xFF)
                del self.flash_state.pw_buf[:-256]
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

            if op_type == 12:        # FLASH_OPCODE_PP - program one 256B or 32B page
                if self.flash_state.pw_buf:
                    self._flash_program_page(self.flash_state.addr, self.flash_state.pw_buf)
                    self.flash_state.pw_buf.clear()
                else:
                    self._flash_program(self.flash_state.addr, self.flash_state.write_buf)
            elif op_type == 13:      # FLASH_OPCODE_SE - erase one 4K sector
                self._flash_erase(self.flash_state.addr, 0x1000)
            elif op_type in (14, 15):  # FLASH_OPCODE_BE1 / BE2 - block erase
                self._flash_erase(self.flash_state.addr, 0x10000)

            value = value & ~(1 << 31)
            
        # SARADC_ADC_CONFIG (BK7231T/U/N/M & BK7252 layout, verified against
        # the SDK saradc.c). A write with CHNL_EN (bit 2) set starts a conversion;
        # fill the emulated FIFO with a batch of samples. The driver's ISR /
        # poll loops read DATA / DAT_AFTER_STA until FIFO_EMPTY (config bit 28/30)
        # is set, so each sample read drains one. Arming only from idle (pending==0)
        # avoids an ever-full FIFO. BK7231N's temp read blocks forever without
        # this - it waits on the SARADC interrupt (ICU bit 11).
        if address == 0x00802c00:
            self.state.saradc_cfg = value
            # The BK7231(T/U/N/M) and BK7252 (SOC_BK7221U) SARADC layouts use
            # this model; the 7238/7252N layout differs and is served via 0x802c08/0c.
            if self.chip.saradc in ("v1", "v2"):
                if value & (1 << 2):                # CHNL_EN: start converting
                    if self.state.saradc_pending == 0:
                        ch_mask = 0xF if self.chip.saradc == "v2" else 0x7
                        ch = (value >> 3) & ch_mask
                        self.state.saradc_channel = ch
                        self.state.saradc_conversions += 1
                        raw = self.get_adc_input(ch)
                        self.state.saradc_last_raw = raw
                        self.state.adc_samples[ch] = raw
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
            elif value & 0x2:                       # BK7238/BK7252N FORCE_QUIT
                self.state.saradc_pending = 0

        if address == 0x00802c08 and self.chip.saradc == "v3":
            # BK7238/BK7252N SARADC_ADC_CONFIG2: bits [3:0] select channel 0..12.
            self.state.saradc_cfg2 = value & 0xFFFFFFFF
            self.state.saradc_channel = value & 0xF

        if address == 0x00802c0c and self.chip.saradc == "v3":
            # BK7238/BK7252N SARADC_ADC_STATE: CHNL_EN (0x14008) enables FIFO/
            # conversion interrupts; INT_CLR (0x28010) is write-1-to-clear.
            self.state.saradc_state_reg = value & ~0x28010
            if (value & 0x14008) and not (value & 0x28010):
                ch = self.state.saradc_channel if self.state.saradc_channel is not None else 0
                self.state.saradc_channel = ch
                self.state.saradc_conversions += 1
                raw = self.get_adc_input(ch)
                self.state.saradc_last_raw = raw
                self.state.adc_samples[ch] = raw
                if ch < 7 or ch in self.adc_inputs:
                    if self.state.saradc_pending == 0:
                        self.state.saradc_pending = 32
            elif not (value & 0x14008):
                self.state.saradc_pending = 0

        # SCTRL_EFUSE_CTRL: EFUSE_OPER_EN (bit 0) self-clears when the efuse
        # operation completes; complete it instantly or sctrl_read_efuse spins
        # forever on it. Kept in shadow state because Unicorn commits the
        # original value to memory after this hook, so reads are served from
        # the shadow in hook_mem_read_mmio.
        if address == 0x00800074:
            self.state.efuse_ctrl = value & ~1

        # SCTRL / AON Deep Sleep & Wakeup registers.
        if address == 0x00800070:
            self.state.sctrl_reset_retention = value & 0xFFFFFFFF
        elif address == 0x0080011c:
            # Bit 8 (0x100) is ROSC_TIMER_INT_STATUS_BIT (write-1-to-clear).
            if value & 0x100:
                self.state.sctrl_rosc_int_status = False
            self.state.sctrl_rosc_timer = value & ~0x100
        elif address == 0x00800140:
            self.state.sctrl_rosc_timer_h = value & 0xFFFF
        elif address == 0x00800120:
            self.state.sctrl_gpio_wakeup_en = value & 0xFFFFFFFF
        elif address == 0x00800124:
            self.state.sctrl_gpio_wakeup_type = value & 0xFFFFFFFF
        elif address == 0x00800128:
            # Write-1-to-clear GPIO 0..31 wakeup status.
            self.state.sctrl_gpio_wakeup_int_status &= ~(value & 0xFFFFFFFF)
        elif address == 0x00800144:
            self.state.sctrl_gpio_wakeup_en1 = value & 0xFFFFFFFF
        elif address == 0x00800148:
            self.state.sctrl_gpio_wakeup_type1 = value & 0xFFFFFFFF
        elif address == 0x0080014c:
            # Write-1-to-clear GPIO 32..39 wakeup status.
            self.state.sctrl_gpio_wakeup_int_status1 &= ~(value & 0xFFFFFFFF)
        elif address == 0x00808004:
            # AON_PMU wakeup GPIO status (write-1-to-clear on BK7231N/BK7238).
            self.state.aon_gpio_wakeup_status &= ~(value & 0xFFFFFFFF)
        elif address == 0x00800104:
            if (value & 0xFFFF) == 0xADC1:
                try:
                    mu.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))
                except Exception:
                    pass
                self._enter_deep_sleep(value)
                return


        try:
            mu.mem_write(address, struct.pack("<I" if size == 4 else "<H" if size == 2 else "B", value))
        except Exception:
            pass

    TRNG_DATA = 0x00802484
    # Device time between the peer handing a frame to the air and the MAC
    # receiving it (see WifiPeer.latency_us): a few ms, like a LAN round trip.
    NET_LATENCY_US = 3000

    def hook_mem_read_mmio(self, mu, access, address, size, value, user_data):
        if self.wifimac is not None and _wifimac.CORE <= address < _wifimac.CORE + _wifimac.SIZE:
            served = self.wifimac.read(address, self.state.insn_count)
            if served is not None:
                mu.mem_write(address, struct.pack("<I", served & 0xFFFFFFFF))
            return
        if 0x00802300 <= address < 0x0080230C:
            val = self._hw_i2c_read(1, address - 0x00802300)
            mu.mem_write(address, struct.pack("<I", val & 0xFFFFFFFF))
            return
        if 0x00802600 <= address < 0x0080260C:
            val = self._hw_i2c_read(2, address - 0x00802600)
            mu.mem_write(address, struct.pack("<I", val & 0xFFFFFFFF))
            return
        if self.chip.ejpeg == "v1" and 0x0080A000 <= address < 0x0080A100:
            off = address - 0x0080A000
            if off == 0x00:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_reg0))
            elif off == 0x04:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_reg1))
            elif off == 0x08:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_reg2))
            elif off == 0x0C:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_reg3))
            elif off == 0x18:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_int_stat))
            elif off == 0x1C:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_frame_len))
            return
        if self.chip.ejpeg == "v2" and 0x00A03000 <= address < 0x00A05000:
            if address == 0x00A03008:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_reg0))
            elif address == 0x00A03018:
                mu.mem_write(address, struct.pack("<I", self.state.ejpeg_int_stat))
            elif address == 0x00A0301C:
                mu.mem_write(address, struct.pack("<I", max(0, self.state.ejpeg_frame_len - 1)))
            return
        if address == self.TRNG_DATA and self.trng_state is not None:
            x = self.trng_state
            x ^= (x << 13) & 0xFFFFFFFF
            x ^= x >> 17
            x ^= (x << 5) & 0xFFFFFFFF
            self.trng_state = x
            mu.mem_write(address, struct.pack("<I", x))
            return
        # SCTRL identity registers; served per the selected chip family.
        if address == 0x00800000:
            mu.mem_write(address, struct.pack("<I", self.chip_id_value))
            return
        if address == 0x00800004:
            mu.mem_write(address, struct.pack("<I", self.device_id_value))
            return
        if address == 0x00800070:
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_reset_retention))
            return
        if address == 0x0080011c:
            val = ((self.state.sctrl_rosc_timer & ~0x100)
                   | (0x100 if self.state.sctrl_rosc_int_status else 0))
            mu.mem_write(address, struct.pack("<I", val & 0xFFFFFFFF))
            return
        if address == 0x00800140:
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_rosc_timer_h & 0xFFFF))
            return
        if address == 0x00800120:
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_en))
            return
        if address == 0x00800124:
            self.state.wake_reg_sampled = True
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_type))
            return
        if address == 0x00800128:
            self.state.wake_reg_sampled = True
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_int_status))
            return
        if address == 0x00800144:
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_en1))
            return
        if address == 0x00800148:
            self.state.wake_reg_sampled = True
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_type1))
            return
        if address == 0x0080014c:
            self.state.wake_reg_sampled = True
            mu.mem_write(address, struct.pack("<I", self.state.sctrl_gpio_wakeup_int_status1))
            return
        if address == 0x00808004:
            self.state.wake_reg_sampled = True
            mu.mem_write(address, struct.pack("<I", self.state.aon_gpio_wakeup_status))
            return
        # BK7252 SCTRL_USB_PLUG_WAKEUP (0x00800084): bit 21 (USB_PLUG_IN_STATUS)
        # reports USB 5V power present so battery-powered cameras (CYCAM/T99)
        # stay awake and boot their application instead of shutting down into
        # deep sleep when the power button is not held.
        if address == 0x00800084 and self.chip.usb_plug_wake:
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            mu.mem_write(address, struct.pack("<I", cur | (1 << 21)))
            return
        # GPIO config reads combine firmware-owned config with the external pad
        # level.  Only real pin slots come through this path; the adjacent
        # interrupt controller words have their own semantics below.
        _pin = self._gpio_pin_at(address)
        if _pin is not None:
            _slot = self._gpio_slot(_pin)
            if _slot in self.state.gpio_cfg:
                cfg = self.state.gpio_cfg[_slot]
            else:
                try:
                    cfg = struct.unpack("<I", mu.mem_read(address, 4))[0]
                except Exception:
                    cfg = 0
            cfg = self.state.gpio_hw.read_config(_pin, cfg)
            if (_pin == self.state.wake_pulse_pin
                    and self.state.wake_reg_sampled
                    and self.state.wake_pulse_reads_left > 0):
                self.state.wake_pulse_reads_left -= 1
                if self.state.wake_pulse_reads_left == 0:
                    self.state.gpio_hw.inputs[_pin] = self.state.wake_pulse_restore
                    self.state.wake_pulse_pin = None
            if self.state.i2c_buses and (cfg & 0x08):
                for (_scl, sda), bstate in self.state.i2c_buses.items():
                    if _pin == sda and bstate[0] and bstate[2] == 8:
                        cfg &= ~1
                        break
            mu.mem_write(address, struct.pack("<I", cfg & 0xFFFFFFFF))
            return
        if address == self.GPIO_INTEN:
            mu.mem_write(address, struct.pack("<I", self.state.gpio_hw.int_enable & 0xFFFFFFFF))
            return
        if address == self.GPIO_INTLV0:
            mu.mem_write(address, struct.pack("<I", self.state.gpio_hw.read_mode_word(0)))
            return
        if address == self.GPIO_INTLV1:
            mu.mem_write(address, struct.pack("<I", self.state.gpio_hw.read_mode_word(16)))
            return
        if address == self.GPIO_INTSTA:
            mu.mem_write(address, struct.pack("<I", self.state.gpio_hw.int_status & 0xFFFFFFFF))
            return
        if self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTEN2:
            mu.mem_write(address, struct.pack("<I",
                                              (self.state.gpio_hw.int_enable >> 32) & 0xFF))
            return
        if self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTLV3:
            mu.mem_write(address, struct.pack("<I",
                                              self.state.gpio_hw.read_mode_word(32, count=8)))
            return
        if self.chip.gpio_pins > self.GPIO_PIN_COUNT and address == self.GPIO_INTSTA2:
            mu.mem_write(address, struct.pack("<I",
                                              (self.state.gpio_hw.int_status >> 32) & 0xFF))
            return

        if address == 0x00802A04:
            mu.mem_write(address, struct.pack("<I", self.state.pwm_status))
            return

        # pwm_new REG_GROUP_PWM_CPU, one per group (0x802B24/64/A4). Reading a
        # channel's counter is a request: the driver sets CPU_RD0/1 (bits 0/1)
        # and spins until hardware clears it, having latched the count into
        # RD_DATA (pwm_bk7231n.c: `while ((value & (1 << post)) != 0)`). Served
        # as plain memory the bit never clears - the VeSync plug firmware wedges
        # there right after putting its PWM channels into capture mode (how it
        # counts the metering chip's pulses). Read back with the request
        # bits clear, i.e. the count already latched; RD_DATA itself stays
        # plain memory. Gated to the chips whose 0x802B00 block is pwm_new; on
        # the 7252 family it is the audio block.
        if address in self.PWM_CPU_REGS and self.chip.pwm == "v2":
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            mu.mem_write(address, struct.pack("<I", cur & ~0x3))
            return
        # pwm_new REG_PWM_GROUP_CTRL: the interrupt status bits (30/31) come
        # from the shadow, since the firmware writes them as 1 to CLEAR them
        # and plain memory would keep them set - its PWM ISR then sees a
        # phantom capture interrupt, acks it and spins forever waiting for the
        # ack to take. CFG_UPDATA reads back clear: the new config is latched.
        if address in self.PWMN_CTRL_REGS and self.chip.pwm == "v2":
            group = (address - self.PWMN_BASE) >> 6
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            stat = ((self.state.pwmn_stat >> (2 * group)) & 0x3) << 30
            mu.mem_write(address, struct.pack(
                "<I", (cur & ~(self.PWMN_INT_STAT | self.PWMN_CFG_UPDATA)) | stat))
            return
        if address == 0x00802A0C:
            mu.mem_write(address, struct.pack("<I", self.state.timer0_2_ctl))
            return
        if address == 0x00802A10 or address == 0x00802A50:
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            mu.mem_write(address, struct.pack("<I", cur & ~0x1))
            return
        if address == 0x00802A4C:
            mu.mem_write(address, struct.pack("<I", self.state.timer3_5_ctl))
            return

        if address == 0x00802c00:
            # Config readback: preserve firmware-configured mode/channel/enable
            # bits from saradc_cfg so saradc_int_clr()'s read-modify-write does
            # not accidentally clear CHNL_EN (bit 2) and flush the FIFO before
            # saradc_isr() can read a single sample. FIFO_EMPTY (bit 28 on
            # SOC_BK7231 Q/T/U, bit 30 on SOC_BK7231N/BK7252) is set when
            # drained and clear while samples pend; INT_CLR (bits 7/8) always
            # reads back clear so saradc_int_clr()'s do/while loop exits.
            base_cfg = self.state.saradc_cfg & ~((1 << 28) | (1 << 30) | 0x180)
            if (self.chip.saradc in ("v1", "v2")) and self.state.saradc_pending:
                cfg = base_cfg
            else:
                cfg = base_cfg | (1 << 28) | (1 << 30)
            mu.mem_write(address, struct.pack("<I", cfg & 0xFFFFFFFF))
            return

        # BK7231N/BK7252 SARADC_ADC_CTRL_CFG (0x00802c08). Newer SDK builds
        # (SDK Rev 3.0.56+, e.g. VeSync BSD33 / WHOGPLUG 1.0.02, ChinaMobile
        # HeJiaQin) set bit 8 (0x100) here during calibration_main's SARADC
        # reads and spin on `while (REG_READ(0x802c08) & 0x100)` waiting for
        # hardware to clear the trigger bit.
        if address == 0x00802c08 and (self.chip.saradc in ("v1", "v2")):
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            mu.mem_write(address, struct.pack("<I", cur & ~0x100))
            return

        # BK7238/BK7252N SARADC state register (SARADC_ADC_STATE). Its driver
        # drains the sample FIFO with `while(!(STATE & FIFO_EMPTY)) read DATA;`
        # spin loops; report FIFO_EMPTY (bit 30) once saradc_pending == 0, and
        # clear while samples pend so saradc_isr drains them.
        if address == 0x00802c0c and self.chip.saradc == "v3":
            stat = self.state.saradc_state_reg & ~((1 << 30) | (1 << 31) | 0x28010)
            if not self.state.saradc_pending:
                stat |= (1 << 30)
            mu.mem_write(address, struct.pack("<I", stat))
            return

        # BK7251/BK7252 SDCard controller (0x00802d00..0x00802d3c). Camera
        # firmwares probe for an SD card during boot by writing CMD_SEND_CTRL
        # (0x00802d00) and spinning on SDCARD_CMDRSP_INT_SEL (0x00802d24) until
        # one of NORSP_END_INT (bit 0), RSP_END_INT (bit 1), or TIMEOUT_INT
        # (bit 2) asserts. Report NORSP_END_INT when the command does not
        # expect a response (CMD_FLAGS_RSP bit 1 clear, e.g. CMD0) and
        # TIMEOUT_INT | DATA_TIME_OUT_INT (bits 2 and 5) when a response is
        # expected from the absent card. Also report RX/TX FIFO ready (bits
        # 18..19) on SDCARD_FIFO_THRESHOLD (0x00802d34).
        if address == 0x00802d24:
            cmd_ctrl = struct.unpack("<I", mu.mem_read(0x00802d00, 4))[0]
            stat = 0x24 if (cmd_ctrl & 0x2) else 0x01
            mu.mem_write(address, struct.pack("<I", stat))
            return
        if address == 0x00802d34:
            cur = struct.unpack("<I", mu.mem_read(address, 4))[0]
            mu.mem_write(address, struct.pack("<I", cur | (3 << 18)))
            return

        # SCTRL_EFUSE_CTRL readback: shadow with EFUSE_OPER_EN self-cleared.
        if address == 0x00800074:
            mu.mem_write(address, struct.pack("<I", self.state.efuse_ctrl))
            return

        # XVR (RF transceiver) transaction register. RF init sets bit 31 to
        # start a transfer and spins until hardware clears it; report it always
        # complete (bit 31 low) so the wait loop exits. On the RW BLE 5.x
        # layout this same offset is SLOTCLK, the controller's slot counter,
        # which --ble-core serves properly - so that model takes precedence.
        if address == 0x00900100 and self.blecore is None:
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
        if self.blecore is not None and self.XVR_BASE <= address < self.XVR_BASE + self.blecore.window:
            served = self.blecore.read(address, self.state.insn_count)
            if served is not None:
                mu.mem_write(address, struct.pack("<I", served))
                return
            # anything else in the block: plain memory, fall through
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
            # SARADC sample registers: DATA (0x802c04 on BK7231/BK7252) and
            # DAT_AFTER_STA / BK7238 DATA (0x802c10). Each read consumes one
            # pending FIFO sample and returns the configured channel's raw
            # value (default 330 -> roughly +30C on BK7231N ch7).
            if self.chip.saradc in ("v1", "v2") or address == 0x00802c10:
                if self.state.saradc_pending:
                    self.state.saradc_pending -= 1
                val = self._saradc_sample_for(self.state.saradc_channel)
                mu.mem_write(address, struct.pack("<I", val))
                return

        if address == 0x00803004:
            if hasattr(self.flash_state, 'data'):
                mu.mem_write(address, self.flash_state.data)
            return
            
        if address == self.ICU_INT_STATUS or address == self.ICU_INT_RAW_STATUS:
            val = self.state.pending_irqs & self.state.icu_int_enable
            mu.mem_write(address, struct.pack("<I", val))
            return
            
        if self.GDMA_BASE <= address < self.GDMA_END:
            channel = self._gdma_channel(address)
            if channel is not None and channel in self.state.gdma_conf:
                # The enable bit reads back clear, which is how the firmware's
                # wait-for-completion spin exits.
                mu.mem_write(address, struct.pack("<I", self.state.gdma_conf[channel]))
                return
            if self.chip.gdma == "v2" and 0x00809040 <= address < 0x00809240:
                ch, within = divmod(address - 0x00809040, 0x40)
                if within == 0x30:
                    mu.mem_write(address, struct.pack("<I", self.state.gdma_remain.get(ch, 0) & 0x1FFFF))
                    return
            elif 0x008090C0 <= address < 0x008090D8:
                ch = (address - 0x008090C0) // 4
                mu.mem_write(address, struct.pack("<I", self.state.gdma_remain.get(ch, 0) & 0x1FFFF))
                return
            elif address < self.GDMA_BASE + self.GDMA_CHANNELS * self.GDMA_CH_STRIDE:
                ch, within = divmod(address - self.GDMA_BASE, self.GDMA_CH_STRIDE)
                if within == 0x1C:
                    mu.mem_write(address, struct.pack("<I", self.state.gdma_remain.get(ch, 0) & 0x1FFFF))
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

        if address == 0x00802214:
            sta = ((1 << 1) | (1 << 6)) if self._rx_active_uart2() else 0
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
            if (b == 0x0A and self.uart1_rx and self.tuyamcu is None
                    and (self.state.rtt_boot_seen
                         or (self.chip.rtthread_console and not self.state.obk_boot_seen))):
                self.state.msh_prompt_seen = False
                self.state.rx_next_line_insn = self.state.insn_count + 3_000_000
            if self._emit_insns:
                self.state.uart1_events.append((self.state.insn_count, 'R', b & 0xFF))
            mu.mem_write(address, struct.pack("<I", (b & 0xFF) << 8))
            return

        if address == self.UART2_FIFO_PORT and self._rx_active_uart2():
            b = self.uart1_rx.pop(0)
            if (b == 0x0A and self.uart1_rx and self.tuyamcu is None
                    and (self.state.rtt_boot_seen
                         or (self.chip.rtthread_console and not self.state.obk_boot_seen))):
                self.state.msh_prompt_seen = False
                self.state.rx_next_line_insn = self.state.insn_count + 3_000_000
            mu.mem_write(address, struct.pack("<I", (b & 0xFF) << 8))
            return

        if address == self.UART1_FIFO_STATUS or address == self.UART2_FIFO_STATUS:
            # TX bits: TX_FIFO_EMPTY (17), FIFO_WR_READY (20) - always ready.
            status = (1 << 17) | (1 << 20)
            if ((address == self.UART1_FIFO_STATUS and self._rx_active())
                    or (address == self.UART2_FIFO_STATUS and self._rx_active_uart2())):
                # RX pending: FIFO_RD_READY (21) set, RX_FIFO_EMPTY (19) clear.
                status |= (1 << 21)
            else:
                status |= (1 << 19)   # RX_FIFO_EMPTY: nothing to receive
            mu.mem_write(address, struct.pack("<I", status))
            return

        if self.chip.audio is not None and address == 0x00802B38:
            sta = ((1 << 18) | (1 << 19)) if (self.state.audio_fifo_conf & 0x3C) else 0
            if self.state.audio_dac_writes >= 256:
                sta |= 0x4000 | 0x8000
            mu.mem_write(address, struct.pack("<I", sta))
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

        if self.only_uart or address in (self.UART1_FIFO_STATUS, self.UART2_FIFO_STATUS, 0x00802c00):
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

    @staticmethod
    def _patch_ldm_caret_stubs(img):
        """Normalize word-aligned `ldmfd sp!, {r0, r1}^` (0xe8fd0003) in the
        first 2 KB (boot_handlers.S IRQ/FIQ entry stubs in AliOS Things / SDK
        3.0.65) to `ldmfd sp!, {r0, r1}` (0xe8bd0003).  R0 and R1 are unbanked
        across all ARM modes so the S-bit has no architectural effect, but
        QEMU/Unicorn's ARMv5 decoder rejects LDM(2) (S=1, PC=0) with writeback
        (W=1) as UC_ERR_INSN_INVALID.
        """
        head = img[:0x800]
        if b"\x03\x00\xfd\xe8" not in head:
            return bytes(img)
        buf = bytearray(img)
        for off in range(0, min(len(buf), 0x800) - 3, 4):
            if buf[off:off + 4] == b"\x03\x00\xfd\xe8":
                buf[off + 2] = 0xbd
        return bytes(buf)

    def setup(self):
        self.mu.mem_map(self.FLASH_BASE, self.FLASH_SIZE)
        self.mu.mem_map(self.ram_base, self.ram_size)
        self.mu.mem_map(self.MMIO_BASE, self.MMIO_SIZE)
        self.mu.mem_map(self.PERIPH_BASE, self.PERIPH_SIZE)
        
        self.mu.mem_write(self.FLASH_BASE, self.raw_flash[:self.FLASH_SIZE])
        
        if self.bootloader:
            boot_img = self._patch_ldm_caret_stubs(self.bootloader)
            self.mu.mem_write(self.chip.bootloader_addr, boot_img)
        if self.app:
            app_img = self._patch_ldm_caret_stubs(self.app)
            self.mu.mem_write(self.chip.app_addr, app_img)

        self._write_mmio_reset_defaults()

        # Mirror the raw flash at the SPI-flash window. Cap the size so it can
        # never run into the RAM region: dumps larger than 2MB (e.g. 4MB BK7238
        # plug images) would otherwise map past RAM_BASE and fail with UC_ERR_MAP
        # before any code runs. Code/data live in the low 2MB, so the upper part
        # of the mirror is not needed for boot.
        spi_flash_size = (len(self.raw_flash) + 0xFFF) & ~0xFFF
        spi_flash_size = min(spi_flash_size, self.ram_base - self.SPI_FLASH_BASE)
        self.mu.mem_map(self.SPI_FLASH_BASE, spi_flash_size)
        self.mu.mem_write(self.SPI_FLASH_BASE, self.raw_flash[:spi_flash_size])

        windows = self._chip_windows()
        for base, size, _hooked in windows:
            self.mu.mem_map(base, size)
        # Accelerator block just past the main MMIO window (0x810000); mapped
        # and hooked so the 0x81001c transaction trigger can be served.
        self.mu.mem_map(self.ACCEL_BASE, 0x1000)

        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.MMIO_BASE, end=self.MMIO_BASE + self.MMIO_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.PERIPH_BASE, end=self.PERIPH_BASE + self.PERIPH_SIZE)
        for base, size, hooked in windows:
            if hooked:
                self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=base, end=base + size)
                self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=base, end=base + size)
        self.mu.hook_add(UC_HOOK_MEM_WRITE, self.hook_mem_write_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_READ, self.hook_mem_read_mmio, begin=self.ACCEL_BASE, end=self.ACCEL_BASE + 0x1000)
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self.hook_unmapped)
        # Only under EMU_BLOCK_PACING: the chunked loop in run() does this
        # work itself, and leaving the hook registered would cost the whole
        # win (Unicorn calls it between every two guest instructions).
        self._tick_hook = (self.mu.hook_add(UC_HOOK_BLOCK, self.hook_block)
                           if self._block_pacing else None)
        self.mu.hook_add(UC_HOOK_INTR, self.hook_intr)
        # A watchdog reset restores every banked CPU register, not just PC.
        # Unicorn contexts include those banked registers, so the pristine
        # post-setup context is the exact reset value and is reusable.
        self._power_on_context = self.mu.context_save()

    # ------------------------------------------------------------ snapshots
    # A boot to the point where a firmware's Wi-Fi is up costs minutes of wall
    # time; iterating on anything after it (a network exchange, a console
    # command) should not. A snapshot is the complete device: every mapped
    # memory region, the CPU context (all modes' banked registers, via
    # Unicorn), and the emulator's own peripheral state. What it deliberately
    # leaves out is the OTHER side of the peripherals - host sockets, the GUI,
    # a pcap writer - which are attached fresh after a load.
    SNAPSHOT_MAGIC = b"BKSNAP01"

    def arm_snapshot(self, path, at):
        """Take a snapshot to `path` at `at`: an instruction count, or a UART marker string."""
        self._snapshot_path = path
        if isinstance(at, int):
            self._snapshot_at_insns = at
            self._snapshot_marker = None
        else:
            self._snapshot_at_insns = None
            self._snapshot_marker = at.encode("utf-8", "replace")
            self._uart_tail = b""
        self._snapshot_due = False

    def _snapshot_check_uart(self, data):
        """Called with every UART byte written; arms the snapshot when the marker shows."""
        tail = self._uart_tail + data
        if self._snapshot_marker in tail:
            self._snapshot_due = True
            self._snapshot_marker = None
        self._uart_tail = tail[-len(self._snapshot_marker or b"") - 1:] if self._snapshot_marker else b""

    def save_snapshot(self, path):
        import pickle
        import zlib
        regions = []
        for begin, end, perms in self.mu.mem_regions():
            size = end - begin + 1
            regions.append((begin, size, perms, zlib.compress(bytes(self.mu.mem_read(begin, size)), 1)))
        ctx = self.mu.context_save()
        # Detach the callables the models hold into this emulator before pickling.
        detached = []
        for obj, names in ((self.wifimac, ("mem", "on_tx", "log")), (self.blecore, ("mem",))):
            if obj is None:
                continue
            saved = {n: getattr(obj, n) for n in names}
            for n in names:
                setattr(obj, n, None)
            detached.append((obj, saved))
        try:
            blob = pickle.dumps({
                "magic": self.SNAPSHOT_MAGIC,
                "regions": regions,
                "ctx": ctx,
                "state": self.state,
                "flash_state": self.flash_state,
                "physical_flash": bytes(self.physical_flash) if self.physical_flash is not None else None,
                "raw_flash": bytes(self.raw_flash),
                "uart1_rx": bytes(self.uart1_rx),
                "uart_src": self._uart_src,
                "tuyamcu": self.tuyamcu,
                "blecore": self.blecore,
                "wifimac": self.wifimac,
                "chip": self.chip.name,
                "ble_adv_seen": self._ble_adv_seen,
                "trng_state": self.trng_state,
                "network_insn_offset": self._network_insn_offset,
            }, protocol=4)
        finally:
            for obj, saved in detached:
                for n, v in saved.items():
                    setattr(obj, n, v)
        with open(path, "wb") as f:
            f.write(blob)
        self.net_log("[SNAPSHOT] saved %s at %d instructions (%d bytes)" % (path, self.state.insn_count, len(blob)))

    def load_snapshot(self, path):
        """Restore a snapshot taken by save_snapshot; call after setup(), before run()."""
        import pickle
        import zlib
        with open(path, "rb") as f:
            snap = pickle.load(f)
        if snap.get("magic") != self.SNAPSHOT_MAGIC:
            raise ValueError("%s is not a snapshot of this emulator" % path)
        if snap["chip"] != self.chip.name:
            raise ValueError("snapshot was taken with a different -chip (%s, not %s)"
                             % (snap["chip"], self.chip.name))
        if (snap["wifimac"] is None) != (self.wifimac is None):
            raise ValueError("snapshot was taken %s the Wi-Fi MAC model (--wifi); use the same option"
                             % ("with" if snap["wifimac"] is not None else "without"))
        mapped = {(b, e - b + 1) for b, e, p in self.mu.mem_regions()}
        for begin, size, perms, data in snap["regions"]:
            if (begin, size) not in mapped:
                try:
                    self.mu.mem_map(begin, size, perms)
                except UcError:
                    pass
            self.mu.mem_write(begin, zlib.decompress(data))
        self.mu.context_restore(snap["ctx"])
        self.state = snap["state"]
        if not hasattr(self.state, "idle_task"):
            self.state.idle_task = None     # a snapshot from before idle skipping
        # BKSNAP01 is intentionally forward-readable.  Snapshots created before
        # the watchdog model simply resume with a reset-value, idle watchdog.
        for name, value in (("wdt_first_period", None), ("wdt_period_ms", 0),
                            ("wdt_clock_enabled", True), ("wdt_deadline", None),
                            ("last_uart_insn", 0), ("uart_line_tail", b""),
                            ("hw_timer_counter", [0] * 6), ("hw_timer_due", [None] * 6),
                            ("i2c_last_pin", None), ("i2c_pair_counts", {}),
                            ("i2c_buses", {}), ("i2c_frames", {}),
                            ("i2c_last_on", {}), ("i2c_dirty", set()),
                            ("ws2812_spi", b""), ("bl0942_polls", 0),
                            ("uart1_tx_prev", -1),
                            ("sctrl_rosc_timer", 0), ("sctrl_rosc_timer_h", 0),
                            ("sctrl_rosc_int_status", False),
                            ("sctrl_gpio_wakeup_en", 0), ("sctrl_gpio_wakeup_type", 0),
                            ("sctrl_gpio_wakeup_int_status", 0),
                            ("sctrl_gpio_wakeup_en1", 0), ("sctrl_gpio_wakeup_type1", 0),
                            ("sctrl_gpio_wakeup_int_status1", 0),
                            ("aon_gpio_wakeup_status", 0), ("sctrl_reset_retention", 0),
                            ("in_deep_sleep", False), ("deep_sleep_count", 0),
                            ("deep_sleep_info", None), ("last_wake_reason", None),
                            ("wake_pulse_pin", None), ("wake_pulse_restore", None),
                            ("wake_pulse_reads_left", 0), ("wake_reg_sampled", False),
                            ("rtt_boot_seen", False), ("msh_prompt_seen", False),
                            ("rx_next_line_insn", 0), ("hw_i2c", {}),
                            ("camera_sensor", None), ("camera_sensor_probed", False),
                            ("camera_sensor_regs", {}), ("camera_i2c_writes", 0),
                            ("camera_last_i2c_insn", 0), ("ejpeg_reg0", 0),
                            ("ejpeg_reg1", 0), ("ejpeg_reg2", 0), ("ejpeg_reg3", 0),
                            ("ejpeg_int_stat", 0), ("ejpeg_frame_len", 0),
                            ("ejpeg_dma_ch", None), ("ejpeg_dma_dst", 0),
                            ("ejpeg_dma_node_len", 0), ("ejpeg_tick_div", 0),
                            ("camera_frame_count", 0), ("camera_last_jpeg", b""),
                            ("camera_frame_rgb", b""), ("camera_width", 640),
                            ("camera_height", 480), ("gdma_remain", {}),
                            ("saradc_cfg2", 0), ("saradc_state_reg", 0),
                            ("saradc_channel", None), ("saradc_last_raw", None),
                            ("saradc_conversions", 0), ("saradc_reads", 0),
                            ("adc_samples", {})):
            if not hasattr(self.state, name):
                setattr(self.state, name, value)
        self.state.adc_samples.update(self.adc_inputs)
        if not hasattr(self.state, "gpio_hw"):
            # BKSNAP01 predates the GPIO model.  Preserve its register state as
            # far as that format permits: INTEN/INTLV were formerly ordinary
            # mapped words.  INTSTA starts clear because old snapshots could
            # contain only the guest's last W1C write, not hardware status.
            self._restore_legacy_gpio_state()
        elif self.state.gpio_hw.pin_count != self.gpio_pin_count:
            raise ValueError("snapshot GPIO width does not match its chip identity")
        # Host-side events are deliberately outside snapshots, like socket and
        # GUI state.  Do not let a press queued before the load leak into the
        # restored device on its next pacing boundary.
        with self._gpio_lock:
            self._gpio_events = []
        self.flash_state = snap["flash_state"]
        if snap["physical_flash"] is not None:
            self.physical_flash = bytearray(snap["physical_flash"])
        self.raw_flash = bytearray(snap["raw_flash"])
        self.uart1_rx = bytearray(snap["uart1_rx"])
        self._uart_src = snap["uart_src"]
        self.tuyamcu = snap["tuyamcu"]
        self._ble_adv_seen = snap["ble_adv_seen"]
        if snap.get("trng_state") is not None:
            self.trng_state = snap["trng_state"]
        self._network_insn_offset = snap.get("network_insn_offset", 0)
        if snap["blecore"] is not None:
            self.blecore = snap["blecore"]
            self.blecore.mem = self._guest_mem_callbacks()
        if snap["wifimac"] is not None:
            peer = self.wifi_peer
            self.wifimac = snap["wifimac"]
            self.wifimac.mem = self._guest_mem_callbacks()
            self.wifimac.log = self.net_log      # not _print: --only-uart silences that
            self.wifimac.debug = bool(os.environ.get("WIFIDBG"))
            if peer is not None:
                # a peer attached before the load binds to the restored MAC
                self._bind_wifi_peer(peer)
        # The run starts where the snapshot was taken: a block boundary.
        pc = self.mu.reg_read(UC_ARM_REG_PC)
        cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
        self._resume_at = pc | (1 if cpsr & 0x20 else 0)
        self.net_log("[SNAPSHOT] restored %s: %d instructions, pc 0x%08x" % (path, self.state.insn_count, pc))

    def _run_chunked(self, start):
        """Drive Unicorn one chunk of device time at a time, pacing between.

        Unicorn's emu_start(count=N) runs exactly N guest instructions and
        returns, so all the periodic device work can happen in this loop
        instead of in a UC_HOOK_BLOCK callback. That matters because the
        callback was the emulator: at ~1.9 counted instructions per basic
        block, Unicorn was crossing the ctypes boundary into Python between
        every two guest instructions. Nothing calls into Python between chunks
        now except the MMIO hooks, which fire a few thousand times a second
        rather than a hundred thousand.

        Resuming is the same trick load_snapshot() uses: a chunk ends between
        two instructions, so PC plus the CPSR Thumb bit is the whole of where
        to carry on from.

        The device clock (state.insn_count) deliberately does NOT become the
        exact count Unicorn gives us here. Everything paced off it - UART
        timing, the Wi-Fi MAC, interrupt rates, SLOW_TICK_INSNS - is
        calibrated against the old per-block ESTIMATE, which counted one
        instruction per 4 bytes of block and so ran at ~0.42 speed through
        Thumb code. Feeding the exact count would make the device clock run
        2.4x fast wherever the firmware is Thumb, which is most of an
        OpenBeken app. So a chunk is converted back onto that scale by the
        CPU state it ran in - CHUNK_SCALE_ARM / CHUNK_SCALE_THUMB.

        That conversion also decides how LONG a chunk is: enough instructions
        to advance the device clock by CHUNK_DEVICE_INSNS, which is one fast
        tick. Sizing it in device units rather than instructions is what
        keeps the pacing faithful - a fixed instruction count would land the
        1000-unit fast tick on whatever multiple of the chunk's device step
        came first (a 2048-instruction chunk steps 860 units in Thumb, so the
        tick would fire every 1720 units, at 58% of its proper rate and with
        the UART interrupts it drives thinned out to match). Only chunks that
        straddle a mode switch step by something other than CHUNK_DEVICE_INSNS,
        a few percent of them; measured end to end, a mathDemo boot reaches
        its last log line within 3% of the device time the per-block pacing
        gave it, having printed the same log - only the periodic "quicktick"
        line moves, by one position, which is the tick phase shifting.
        """
        mu = self.mu
        state = self.state
        # Indexed by the Thumb bit: how many instructions to run, and - once
        # the state the chunk ENDED in is known - what that is worth on the
        # device clock. Both precomputed; the loop below only indexes.
        device_step = self.chunk_device_insns
        chunk_for = (max(1, round(device_step / CHUNK_SCALE_ARM)),
                     max(1, round(device_step / CHUNK_SCALE_THUMB)))
        step_for = tuple(tuple(max(1, int(chunk_for[entry] * scale))
                               for scale in (CHUNK_SCALE_ARM, CHUNK_SCALE_THUMB))
                         for entry in (0, 1))
        emu_start = mu.emu_start
        # Separate register reads, not reg_read_batch (whose wrapper builds
        # ctypes arrays), and the direct C call rather than the binding's.
        reg_read = self._reg_read
        pacing = self._pacing
        pc = start & ~1
        thumb = start & 1
        # Idle skipping (IDLE_STREAK): how many chunks in a row have ended in
        # the idle task, and which IDLE_SCAN_INSNS step of device time was
        # last searched for it. The grid is absolute, not counted from this
        # run's start, so a run resumed from a snapshot searches when the
        # original did.
        skip = self.idle_skip
        grid = state.insn_count // IDLE_SCAN_INSNS
        streak = 0
        while True:
            emu_start(pc | thumb, 0xFFFFFFFF, 0, chunk_for[thumb])
            self.chunks_run += 1
            pc = reg_read(UC_ARM_REG_PC)
            cpsr = reg_read(UC_ARM_REG_CPSR)
            was_thumb, thumb = thumb, 1 if cpsr & 0x20 else 0
            state.insn_count += step_for[was_thumb][thumb]
            # Where execution is, for the GUI status bar in another thread.
            state.last_pc = pc
            epoch = self._exc_epoch
            armed = self._snapshot_path is not None
            pacing(state)
            if self.stopping or self._watchdog_reset_pending or state.in_deep_sleep:
                break
            if self._exc_epoch != epoch:
                # The pacing work took an IRQ or FIQ: PC is now the vector and
                # the mode/Thumb bit changed under us.
                pc = reg_read(UC_ARM_REG_PC)
                thumb = 1 if reg_read(UC_ARM_REG_CPSR) & 0x20 else 0
                streak = 0
            elif skip and armed:
                # Nothing is skipped or searched for while a snapshot is armed
                # (that includes the chunk it was just taken after), so the run
                # up to it is exactly a skip-off run and the skipping restarts
                # from it the way a resumed run's does. A resume then reproduces
                # the original byte for byte, which the harness checks.
                streak = 0
                grid = state.insn_count // IDLE_SCAN_INSNS
            elif skip:
                idle = state.idle_task
                if idle is None:
                    if state.insn_count // IDLE_SCAN_INSNS != grid:
                        grid = state.insn_count // IDLE_SCAN_INSNS
                        state.idle_task = self._find_idle_task()
                # In the idle task, in task context (not an IRQ/FIQ handler),
                # with IRQs on at the ICU, so the interrupt that ends the wait
                # can be delivered once the CPU has them open.
                elif ((cpsr & 0x1F) == 0x1F or (cpsr & 0x1F) == 0x13) and \
                        state.icu_global_int_en & 1 and \
                        idle[0] <= reg_read(UC_ARM_REG_SP) < idle[1]:
                    streak += 1
                    if streak >= IDLE_STREAK:
                        streak = 0
                        pc, thumb, ready = self._idle_step_to_open(pc, thumb, cpsr, idle)
                        if ready:
                            if self._skip_idle(state, step_for[thumb][thumb]):
                                break       # stopped while skipping
                            pc = reg_read(UC_ARM_REG_PC)
                            thumb = 1 if reg_read(UC_ARM_REG_CPSR) & 0x20 else 0
                else:
                    streak = 0
            if pc == 0xFFFFFFFF:
                break                   # ran into `until`: emulation is over

    def _idle_step_to_open(self, pc, thumb, cpsr, idle):
        """Walk the idle loop out of its critical section, a few instructions
        at a time, so skipping starts with interrupts open.

        Beken's idle hook spends most of each pass with IRQ and FIQ masked
        (it disables both around its power-save check), so a chunk usually
        ends inside that window; skipping from there would wait on an
        interrupt the CPU cannot take. The instructions run here are put on
        the device clock like any others. Returns (pc, thumb, ready): ready
        is False if the CPU left the idle task or did not open in time.
        """
        read = self._reg_read
        emu_start = self.mu.emu_start
        real = [0, 0]                   # instructions run, by ARM / Thumb
        ready = False
        for _ in range(IDLE_STEP_TRIES):
            if cpsr & 0xC0 == 0:
                ready = True
                break
            emu_start(pc | thumb, 0xFFFFFFFF, 0, IDLE_STEP_INSNS)
            real[thumb] += IDLE_STEP_INSNS
            pc = read(UC_ARM_REG_PC)
            cpsr = read(UC_ARM_REG_CPSR)
            thumb = 1 if cpsr & 0x20 else 0
            if not (((cpsr & 0x1F) == 0x1F or (cpsr & 0x1F) == 0x13)
                    and idle[0] <= read(UC_ARM_REG_SP) < idle[1]):
                break                   # left the idle task: nothing to skip
        self.state.insn_count += int(real[0] * CHUNK_SCALE_ARM + real[1] * CHUNK_SCALE_THUMB)
        self.state.last_pc = pc
        return pc, thumb, ready

    def _skip_idle(self, state, step):
        """Advance device time without executing, while the CPU only idles.

        Exactly what running the idle loop would do to everything outside the
        CPU: the clock moves a chunk at a time and the periodic work runs at
        its usual grain, so interrupts are raised at the same device times.
        Ends when one is taken (PC is then the vector), after IDLE_MAX_SKIP
        chunks so the idle loop still gets to run now and then, or on stop.
        Returns True if the run was stopped.
        """
        pacing = self._pacing
        epoch = self._exc_epoch
        # _idle_step_to_open has just opened I and F, so an interrupt raised
        # while the idle hook had them masked is deliverable now - on the chip
        # it would be taken here. Do the periodic work once before the clock
        # moves, or a pending timer tick would wait for the next one and the
        # two merge, losing the guest RTOS a tick.
        pacing(state)
        if self.stopping or self._watchdog_reset_pending or state.in_deep_sleep:
            return True
        if self._exc_epoch != epoch:
            return False
        left = IDLE_MAX_SKIP
        while left > 0:
            # When the fast tick has nothing to do, nothing can happen before
            # the next slow tick, so go straight there in one step rather than
            # one chunk at a time. (No snapshot point can be crossed: skipping
            # is off while one is armed.)
            n = 1
            if self._fast_tick_idle(state):
                target = state.last_slow + SLOW_TICK_INSNS
                # A running hardware timer can expire before the next slow tick.
                timer_due = self._next_timer_due(state)
                if timer_due is not None:
                    target = min(target, timer_due)
                # Queued UART1 bytes held back by uart1_rx_delay become due
                # at that point; land on it so the fast tick sees them then.
                if self.uart1_rx and state.insn_count < self.uart1_rx_delay:
                    target = min(target, self.uart1_rx_delay)
                n = min(left, max(1, -(-(target - state.insn_count) // step)))
            state.insn_count += step * n
            left -= n
            self.idle_chunks_skipped += n
            pacing(state)
            if self.stopping or self._watchdog_reset_pending or state.in_deep_sleep:
                return True
            if self._exc_epoch != epoch:
                break
        return False

    @property
    def idle_task(self):
        """The RTOS idle task's stack as (low, high) once found, else None."""
        return self.state.idle_task

    def _find_idle_task(self):
        """The RTOS idle task's stack, as (low, high), or None if not there yet.

        FreeRTOS (v8 to v10, Beken's SDKs) lays a task control block out as
        pxTopOfStack, two list items whose owner fields (+0x10, +0x24) point
        back at the block, uxPriority (+0x2c), pxStack (+0x30), then the name
        (+0x34). A NUL-terminated "IDLE" - Beken spells it "idle" - where all
        of that holds, at priority 0, is the idle task. Its stack runs from
        pxStack up to the block, which the heap allocates right after it;
        anything else and idle skipping simply stays off for this firmware.
        """
        ram = bytes(self.mu.mem_read(self.ram_base, self.ram_size))
        for name in (b"IDLE\x00", b"idle\x00"):
            at = ram.find(name)
            while at >= 0:
                tcb_off = at - 0x34
                if tcb_off >= 0 and tcb_off % 4 == 0:
                    tcb = self.ram_base + tcb_off
                    own1 = struct.unpack_from("<I", ram, tcb_off + 0x10)[0]
                    own2 = struct.unpack_from("<I", ram, tcb_off + 0x24)[0]
                    prio, stack = struct.unpack_from("<II", ram, tcb_off + 0x2C)
                    if (own1 == tcb and own2 == tcb and prio == 0
                            and self.ram_base <= stack < tcb and tcb - stack <= 0x4000):
                        return (stack, tcb)
                at = ram.find(name, at + 1)
        return None

    def _cold_vector_base(self):
        if self.with_boot:
            return self.chip.bootloader_addr
        app_addr = self.chip.app_addr
        vector_base = app_addr
        try:
            val = struct.unpack("<I", self.mu.mem_read(vector_base, 4))[0]
            if (val & 0xFE000000) != 0xEA000000 and (val & 0xFFFFF000) != 0xE59FF000:
                val100 = struct.unpack("<I", self.mu.mem_read(app_addr + 0x100, 4))[0]
                if (val100 & 0xFE000000) == 0xEA000000 or (val100 & 0xFFFFF000) == 0xE59FF000:
                    vector_base = app_addr + 0x100
                elif val == 0xFFFFFFFF or val == 0x94b5072f:
                    vector_base = app_addr + 0x1000
        except Exception:
            pass
        return vector_base

    def run(self):
        if getattr(self, "_resume_at", None) is not None:
            vector_base = self._resume_at
            self._print(f"Resuming snapshot at: 0x{vector_base:08x}")
        else:
            vector_base = self._cold_vector_base()
            if self.with_boot:
                self._print(f"Booting from bootloader at: 0x{vector_base:08x}")
            else:
                self._print(f"Detected app vector table at: 0x{vector_base:08x}")

        if self.app:
            self._print("App header:", self.app[:32].hex())
        
        self._print(f"Starting emulation at 0x{vector_base:08x}...")
        start_time = time.time()
        try:
            while True:
                if self._block_pacing:
                    self.mu.emu_start(vector_base, 0xFFFFFFFF, count=1000000000)
                else:
                    self._run_chunked(vector_base)
                if self.stopping:
                    break
                if self.state.in_deep_sleep:
                    if not self._wait_in_deep_sleep():
                        break
                    vector_base = self._cold_vector_base()
                    self._print(f"Restarting emulation after deep sleep at 0x{vector_base:08x}...")
                    continue
                if not self._watchdog_reset_pending:
                    break
                self._watchdog_reset()
                if self.stopping:
                    break
                vector_base = self._cold_vector_base()
                self._print(f"Restarting emulation at 0x{vector_base:08x}...")
        except UcError as e:
            pc = self.mu.reg_read(UC_ARM_REG_PC)
            cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
            self.run_error = "%s at pc 0x%08x" % (e, pc)
            print(f"\n[!] Emulation finished with error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
        except Exception as e:
            pc = self.mu.reg_read(UC_ARM_REG_PC)
            cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
            self.run_error = "%s: %s (pc 0x%08x)" % (type(e).__name__, e, pc)
            print(f"\n[!] Emulation finished with python error: {e}. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x}")
        except KeyboardInterrupt:
            print("\nEmulation stopped by user.")

        pc = self.mu.reg_read(UC_ARM_REG_PC)
        cpsr = self.mu.reg_read(UC_ARM_REG_CPSR)
        elapsed = time.time() - start_time
        print(f"Emulation finished. PC: 0x{pc:08x}, CPSR: 0x{cpsr:08x} (T-bit: {(cpsr & 0x20) >> 5})", flush=True)
        print(f"Time taken: {elapsed:.2f} seconds.")

        print("\nDone.")

