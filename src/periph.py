"""Decode the GPIO and PWM registers the emulator observed during a run.

The emulator records the last value written to each peripheral register and
prints them on `[EMU_GPIO]` / `[EMU_PWM]` lines; this turns those into
something readable for the report.

GPIO layout (beken378/driver/gpio/gpio.h):
    per-pin config at GPIO_BASE + n*4 for n = 0..31
        bit0 input level   bit1 output level   bit2 input enable
        bit3 output enable bit4 pull up/down   bit5 pull enable
        bit6 second function enable            bit7 input monitor
    index 32 is REG_GPIO_FUNC_CFG, 46 is REG_GPIO_FUNC_CFG_2 - function
    selects, not pins, so they are reported separately rather than decoded as
    if they were.

A pin the firmware never wrote is left as "factory": on real silicon that is
whatever reset state the pad has, and the emulator has not been told otherwise.
"""

import os
import re

PIN_COUNT = 32
FUNC_REGS = {32: "REG_GPIO_FUNC_CFG", 46: "REG_GPIO_FUNC_CFG_2"}



# Special-function pin names for BK7231N, from OpenBeken's
# HAL_PIN_GetPinNameAlias (src/hal/bk7231/hal_pins_bk7231.c). Without these a
# pin in "second function" mode is unreadable - it is precisely these aliases
# that say WHICH peripheral took the pin.
PIN_ALIAS = {
    0: "TXD2", 1: "RXD2",           # UART2 - the debug/log port
    11: "TXD1", 10: "RXD1",         # UART1 - the TuyaMCU / programming port
    6: "PWM0", 7: "PWM1", 8: "PWM2", 9: "PWM3",
    24: "PWM4/ADC2", 26: "PWM5/ADC1",
    21: "ADC6", 22: "ADC5", 28: "ADC4",
}

# What a pin in second-function mode is most likely doing, by peripheral group.
PIN_ROLE = {
    0: "UART2 TX (log)", 1: "UART2 RX (log)",
    11: "UART1 TX (TuyaMCU / programming)", 10: "UART1 RX (TuyaMCU / programming)",
    6: "PWM channel 0", 7: "PWM channel 1", 8: "PWM channel 2", 9: "PWM channel 3",
    24: "PWM channel 4 / ADC2", 26: "PWM channel 5 / ADC1",
}


def pin_alias(pin):
    return PIN_ALIAS.get(pin, "")


def pin_role(pin):
    return PIN_ROLE.get(pin, "")


# Mode words the SDK's gpio_config() actually writes, from
# beken378/driver/gpio/gpio.c - byte-identical in the BK7231T and BK7231N
# trees, so this table is not family-specific:
#
#     GMODE_OUTPUT               0x00        GMODE_SECOND_FUNC          0x48
#     GMODE_INPUT                0x0C        GMODE_SECOND_FUNC_PULL_UP  0x78
#     GMODE_INPUT_PULLUP         0x3C
#     GMODE_INPUT_PULLDOWN       0x2C
#
# Bit 3 is the trap. gpio.h names it GCFG_OUTPUT_ENABLE_POS, but the table
# above shows it SET for every input and second-function mode and CLEAR for
# GMODE_OUTPUT - it is an output *disable*. Decoding it as an active-high
# enable (as this file first did) inverts the whole picture: every
# peripheral-owned pin prints "OUTPUT = 0" and every genuinely driven output
# is missed. That is why a pin reading 0x02 is not an idle latch but a GPIO
# actively driving HIGH.
KNOWN_MODES = {
    0x00: "GMODE_OUTPUT",
    0x0C: "GMODE_INPUT",
    0x2C: "GMODE_INPUT_PULLDOWN",
    0x3C: "GMODE_INPUT_PULLUP",
    0x48: "GMODE_SECOND_FUNC",
    0x78: "GMODE_SECOND_FUNC_PULL_UP",
}


def decode_pin(value):
    """Return a readable description of one pin config word."""
    second = bool(value & (1 << 6))
    # Bit 3 clear == the GPIO output driver is active. See the note above.
    driving = not (value & (1 << 3))
    in_en = bool(value & (1 << 2))
    level = 1 if value & (1 << 1) else 0
    bits = []
    if second:
        # A peripheral owns the pad; naming which one is the whole point,
        # otherwise every UART and PWM pin looks identical.
        bits.append("second function")
    elif driving:
        bits.append("OUTPUT = %d" % level)
    if in_en:
        bits.append("input")
    if value & (1 << 5):
        bits.append("pull-%s" % ("up" if value & (1 << 4) else "down"))
    if value & (1 << 7):
        bits.append("monitor")
    if not bits:
        bits.append("configured, no direction enabled")
    return ", ".join(bits)


def mode_name(value):
    """The SDK mode constant this word corresponds to, if it is an exact match.

    gpio_config() writes these whole-word, but gpio_output() then flips bit 1
    in place, so a driven pin is "GMODE_OUTPUT with bit 1 set" rather than an
    exact hit - hence the retry with the output level masked off.
    """
    if value in KNOWN_MODES:
        return KNOWN_MODES[value]
    base = value & ~(1 << 1)
    if base in KNOWN_MODES:
        return "%s + output %d" % (KNOWN_MODES[base], 1 if value & (1 << 1) else 0)
    return ""


def parse_lines(lines):
    """Collect [EMU_GPIO]/[EMU_PWM] values from harness-captured lines."""
    gpio, pwm = {}, {}
    for line in lines:
        if line.startswith("[EMU_GPIO]"):
            parts = line.split()
            if len(parts) >= 3:
                try:
                    gpio[int(parts[1])] = int(parts[2], 16)
                except ValueError:
                    pass
        elif line.startswith("[EMU_PWM]"):
            parts = line.split()
            if len(parts) >= 3:
                try:
                    pwm[int(parts[1], 16)] = int(parts[2], 16)
                except ValueError:
                    pass
    return gpio, pwm


def gpio_table(gpio):
    """[(pin, alias, state, detail)] for every pin, touched or not."""
    rows = []
    for pin in range(PIN_COUNT):
        alias = pin_alias(pin)
        if pin in gpio:
            detail = decode_pin(gpio[pin])
            role = pin_role(pin)
            if role and "second function" in detail:
                detail = detail.replace("second function", role)
            rows.append((pin, alias, "set by firmware", detail))
        else:
            rows.append((pin, alias, "factory", "not configured"))
    return rows


def func_rows(gpio):
    return [(FUNC_REGS[i], "0x%08X" % v) for i, v in sorted(gpio.items())
            if i in FUNC_REGS]


# PWM. pwm.h selects PWM_BASE as either PWM_NEW_BASE (0x802A00) or
# PWM_NEW_BASE + 0x80 via a build switch, so the capture covers both and the
# layout is inferred from which offsets the firmware actually wrote.
#
# In the new layout each channel has a period ("counter") and a duty
# ("capture") register:
#     period(n) = PWM_BASE + 0x08 + 8*n      duty(n) = PWM_BASE + 0x0C + 8*n
#
# Verified against a real device: a Woox CW bulb wrote period 0x54A2 (21666),
# and 26 MHz / 21666 = 1200.04 Hz - exactly the pwmhz:1200 its own stored Tuya
# config declares. Two independent sources agreeing pins the layout down.
PWM_CLOCK_HZ = 26000000


def infer_base(pwm):
    """Return the offset PWM_BASE sits at within the captured window."""
    # Writes at or above +0x80 mean the shifted layout is in use. Offsets at
    # 0x100+ belong to a DIFFERENT peripheral (pwm_new on BK7231N, audio on
    # the 7252 family) and must not influence the old-block base inference.
    return 0x80 if any(0x80 <= off < 0x100 for off in pwm) else 0x00


# ---------------------------------------------------------------------------
# BK7231N "pwm_new" block (beken378/driver/pwm/pwm_new.c/h in the N SDK).
#
# A different peripheral from the old block above: REG_PWM_BASE_ADDR is
# 0x00802B00, i.e. offset 0x100 in our capture window, with three GROUPS of
# 0x40 bytes. Each group holds one shared CTRL word and two sub-channels of
# four edge-time registers T1..T4:
#
#     group g at 0x100 + 0x40*g:
#         +0x00 CTRL      +0x04..0x10 sub0 T1..T4    +0x14..0x20 sub1 T1..T4
#
# Logical channel ch (0..5) = group ch//2, sub ch%2; pad map is the same
# 6,7,8,9,24,26. CTRL packs per-sub fields at bit 8*s: mode[2:0] (1 = PWM),
# enable at +3, init level at +6, cfg-update strobe at +7; pre-divider is
# bits[23:16] (never set by the SDK - 0 means divide-by-1); int-status W1C at
# bits 30/31. The strobe and int bits read back sticky in a last-value capture
# and must be masked before interpreting.
#
# T4 is the period. T1..T3 are LEVEL-TOGGLE times: output starts at the init
# level and flips at each nonzero T < T4 (a toggle at >= T4 coincides with
# reload and never happens - the SDK's CW code relies on that). Duty therefore
# comes from replaying the toggles, not from a duty register.
#
# Why the gating on which decoder to use is by CHIP: pwm_new.c is compiled
# only for SOC_BK7231N and the old pwm.c only for everything else, and on the
# 7252 family 0x802B00 is the AUDIO block - so the same captured offsets mean
# different things per chip, and only the harness knows which chip a run is.
PWM_NEW_OFF = 0x100


def pwm_new_channels(pwm):
    """[(channel, period, high_ticks, freq_hz, duty_pct)] from pwm_new writes."""
    rows = []
    for ch in range(6):
        g, s = ch // 2, ch % 2
        base = PWM_NEW_OFF + 0x40 * g
        ctrl = pwm.get(base)
        if ctrl is None:
            continue
        if not (ctrl >> (8 * s + 3)) & 1:        # enable
            continue
        if (ctrl >> (8 * s)) & 0x7 != 1:         # mode: 1 = PWM (2 = timer)
            continue
        toff = base + 0x04 + 0x10 * s
        t = [pwm.get(toff + 4 * i) or 0 for i in range(4)]
        period = t[3]
        if not period:
            continue                              # cleared = channel stopped
        pre = (ctrl >> 16) & 0xFF                 # SDK never sets it; 0 = /1
        freq = PWM_CLOCK_HZ / (max(1, pre) * period)
        if not (PWM_FREQ_MIN_HZ <= freq <= PWM_FREQ_MAX_HZ):
            continue
        level = (ctrl >> (8 * s + 6)) & 1         # init level
        high, prev = 0, 0
        for tog in sorted(x for x in t[:3] if 0 < x < period):
            high += (tog - prev) * level
            level ^= 1
            prev = tog
        high += (period - prev) * level
        rows.append((ch, period, high, freq, 100.0 * high / period))
    return rows


# A real PWM output lands somewhere between mains-flicker and switching-supply
# territory. Anything outside this is not a period register being read as one:
# a firmware doing NO pwm still writes 0x173EED80 (390,000,000 -> 0.067 Hz) into
# this window for the FreeRTOS tick, and decoding that as a light produced a
# confident "0.1 Hz" in the report. Verified plausible values sit well inside:
# a Woox CW bulb uses 0x54A2 -> 1200 Hz, matching its own stored pwmhz:1200.
PWM_FREQ_MIN_HZ = 20
PWM_FREQ_MAX_HZ = 100000


def pwm_channels(pwm):
    """[(channel, period, duty, freq_hz, duty_pct)] for plausible channels.

    Two register layouts exist, and beken378/driver/pwm/pwm.h selects BOTH the
    base and the layout from the same switch, so they always go together:

      CFG_SOC_NAME == SOC_BK7231      other SOCs
      PWM_BASE = PWM_NEW_BASE         PWM_BASE = PWM_NEW_BASE + 0x20*4 (+0x80)
      PWM0_COUNTER = BASE + 2*4       PWM0_END       = BASE + 2*4
      PWM1_COUNTER = BASE + 4*4       PWM0_DUTY_CYCLE= BASE + 3*4
        -> stride 8                   PWM1_COUNTER   = BASE + 5*4
      END is bits 0-15 and DC is        -> stride 12, END and DUTY are
      bits 16-31 of that ONE word       SEPARATE 32-bit registers

    This function previously used stride 8 together with a separate duty
    register - a hybrid matching NEITHER layout, which is why decoded channels
    came out as nonsense. Confirmed against hardware: an OpenBeken image told
    "PWMFrequency 1400; SetPinRole 9 PWM; SetChannel 1 50" wrote 0x488B (18571
    = 26 MHz / 1400) at +0xAC and 0x2445 (9285 = 50%) at +0xB0, which is
    exactly channel 3 under the +0x80 / stride-12 reading.
    """
    base = infer_base(pwm)
    rows = []
    if not base:
        # Only the +0x80 / stride-12 layout is confirmed against hardware (see
        # the 1400 Hz case above). With every write below +0x80 there is no way
        # to tell an old-layout channel from the timer registers that share
        # this page: a firmware doing no PWM at all writes 0x173EED80 at +0x08,
        # whose low 16 bits (0xED80) read back as a perfectly believable 427 Hz
        # at 9.8% duty. Declining to decode is the honest answer - the raw
        # registers are still reported, so nothing is hidden.
        return rows, pwm.get(0, 0), base
    for ch in range(6):
        period = pwm.get(base + 0x08 + 12 * ch)
        duty = pwm.get(base + 0x0C + 12 * ch)
        if not period:
            continue
        freq = PWM_CLOCK_HZ / period
        if not (PWM_FREQ_MIN_HZ <= freq <= PWM_FREQ_MAX_HZ):
            continue
        pct = (100.0 * duty / period) if duty is not None else None
        rows.append((ch, period, duty, freq, pct))
    return rows, pwm.get(base, 0), base


def ctl_channels(ctl):
    """Channels PWM_CTL says are enabled.

    pwm.h gives each channel a 4-bit field in PWM_CTL: PWMn_EN_BIT is
    1 << (4*n), with int-enable at 4*n+1 and a 2-bit mode at 4*n+2. This is an
    INDEPENDENT witness to the channel numbering, which is the thing this
    decoder has historically got wrong - it comes from a different register
    than the period/duty pair. Confirmed on hardware: driving pin 9 (PWM3) left
    CTL = 0x1000 (1 << 12) and pin 7 (PWM1) left CTL = 0x10 (1 << 4).
    """
    return [n for n in range(6) if ctl & (1 << (4 * n))]


# PWM-capable pads, from OpenBeken's HAL_PIN_GetPinNameAlias table.
PWM_PINS = {6: 0, 7: 1, 8: 2, 9: 3, 24: 4, 26: 5}


def pwm_output_pins(gpio):
    """Pins actually handed to the PWM peripheral.

    PWM register writes on their own prove nothing: fclk_init() runs a PWM
    channel in PMODE_TIMER with duty_cycle 0 as the FreeRTOS tick, on every
    boot, bound to no pad at all (beken378/func/misc/fake_clock.c). So a
    "wrote PWM registers" test tags literally every firmware. A PWM-capable
    pad switched to second function is the thing that distinguishes a bulb
    driving its channels from the OS keeping time.
    """
    return sorted(pin for pin, val in gpio.items()
                  if pin in PWM_PINS and (val & (1 << 6)))


def pwm_registers(pwm):
    """[(offset, value)] for every PWM register the firmware wrote."""
    return [("+0x%02X" % off, "0x%08X" % val) for off, val in sorted(pwm.items())]


# Same captured offsets mean different peripherals per chip: 0x100+ is pwm_new
# on the N family but AUDIO on 7252, so the chip selects the decoder. This
# lives here rather than in the caller because it is a decoding decision, and
# there are now two callers - the self-test report and the GUI, which reads the
# emulator's live register dicts instead of captured [EMU_*] lines.
PWM_NEW_CHIPS = {"BK7231N", "BK7231M", "BL2028N"}


def decode_state(gpio, pwm, chip=""):
    """Decode raw {pin: value} / {offset: value} register maps.

    Returns the dict the report and the GUI both render, or None when the
    firmware has not touched either peripheral yet.
    """
    if not gpio and not pwm:
        return None
    if chip in PWM_NEW_CHIPS and any(off >= 0x100 for off in pwm):
        channels = pwm_new_channels(pwm)
        ctl, base, layout = 0, None, "new"
    else:
        channels, ctl, base = pwm_channels(pwm)
        layout = "old"
    return {"gpio": gpio_table(gpio),
            "pwm_layout": layout,
            "raw": gpio,
            "pwm_pins": pwm_output_pins(gpio),
            "func": func_rows(gpio),
            "pwm_ctl": ctl,
            "pwm_base": base,
            "pwm_channels": channels,
            "pwm_regs": pwm_registers(pwm)}


# Silicon name implied by a dump's filename. Report chip names (BK7231N,
# BK7231T, ...) are finer-grained than the four -chip CLI identities, and
# decode_state above needs the fine-grained one, so this lives beside it and
# is shared by the self-test harness and the GUI.
CHIP_RE = re.compile(r"(BL2028N|BK7252N|BK7231[TUNMQ]|BK7238|BK7252|BK7236|BK7258|BK3231)", re.I)


def chip_from_name(filename):
    """Chip a dump's filename names, or "" when it says nothing."""
    m = CHIP_RE.search(os.path.basename(filename or ""))
    return m.group(1).upper() if m else ""
