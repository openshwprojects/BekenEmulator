"""Decode the GPIO and PWM registers the emulator observed during a run.

The emulator records the last value written to each peripheral register and
prints them on `[EMU_GPIO]` / `[EMU_PWM]` lines; this turns those into
something readable for the report.

GPIO layout (beken378/driver/gpio/gpio.h):
    per-pin config at GPIO_BASE + n*4 for n = 0..31
        bit0 input level   bit1 output level   bit2 input enable
        bit3 output enable bit4 pull up/down   bit5 pull enable
        bit6 second function enable            bit7 input monitor
    index 32 is REG_GPIO_FUNC_CFG, 46/47 are REG_GPIO_FUNC_CFG_2/_3 - function
    selects, not pins, so they are reported separately rather than decoded as
    if they were.

BK7231U/BK7252 P32..P39 are not contiguous with P0..P31: their physical
register slots are 48..55, after those function-select words.  The decoder
translates those slots back to logical pin numbers for front ends while
keeping the function words separate.

A pin the firmware never wrote is left as "factory": on real silicon that is
whatever reset state the pad has, and the emulator has not been told otherwise.
"""

import os
import re

try:
    from .chips import CHIPS
except ImportError:
    from chips import CHIPS

PIN_COUNT = 32
BK7252_EXT_SLOT = 48


def chip_pin_count(chip):
    """GPIO pins of a chip name. Names with no chip file (report-only, such as
    BK7231M or BL2028N from a firmware's file name) have the base bank."""
    found = CHIPS.get(chip)
    return found.gpio_pins if found else PIN_COUNT
FUNC_REGS = {32: "REG_GPIO_FUNC_CFG", 46: "REG_GPIO_FUNC_CFG_2",
             47: "REG_GPIO_FUNC_CFG_3"}



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


def parse_i2c_lines(lines):
    """Collect {(scl, sda): raw_bytes} from [EMU_I2C] lines."""
    frames = {}
    for line in lines:
        if line.startswith("[EMU_I2C]"):
            parts = line.split()
            if len(parts) >= 4:
                try:
                    scl, sda = int(parts[1]), int(parts[2])
                    frames[(scl, sda)] = bytes.fromhex(parts[3])
                except ValueError:
                    pass
    return frames


def gpio_slot(pin, pin_count=PIN_COUNT):
    """GPIO register slot for logical ``pin`` on a chip with ``pin_count`` pins.

    ``None`` if the chip has no such pin."""
    if 0 <= pin < PIN_COUNT:
        return pin
    if PIN_COUNT <= pin < pin_count:
        return BK7252_EXT_SLOT + (pin - PIN_COUNT)
    return None


def gpio_pin_values(gpio, chip=""):
    """Return a logical ``pin -> config`` map, excluding controller words."""
    pins = {pin: value for pin, value in gpio.items() if 0 <= pin < PIN_COUNT}
    pin_count = chip_pin_count(chip)
    if pin_count > PIN_COUNT:
        for pin in range(PIN_COUNT, pin_count):
            slot = gpio_slot(pin, pin_count)
            if slot in gpio:
                pins[pin] = gpio[slot]
    return pins


def gpio_table(gpio, pin_count=PIN_COUNT):
    """[(pin, alias, state, detail)] for every pin, touched or not."""
    rows = []
    for pin in range(pin_count):
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
PWM_CHANNEL_PINS = {ch: pin for pin, ch in PWM_PINS.items()}

# ---------------------------------------------------------------------------
# SARADC (Successive Approximation ADC) channel <-> GPIO pad mapping & decoding
# ---------------------------------------------------------------------------
# Default raw sample value returned when no per-channel override is configured
# (330 corresponds to ~+30 C on BK7231N's internal temperature formula and
# satisfies battery-monitor thresholds on BK7252 camera firmwares).
DEFAULT_ADC_RAW = 330

ADC_CHANNEL_PINS_BK7231 = {1: 26, 2: 24, 3: 23, 4: 28, 5: 22, 6: 21, 7: 13}
ADC_CHANNEL_PINS_BK7231N = {1: 26, 2: 24, 3: 23, 4: 28, 5: 22, 6: 21, 7: 13}
ADC_CHANNEL_PINS_BK7238 = {1: 20, 2: 22, 3: 23, 4: 26, 5: 24, 6: 28}
ADC_CHANNEL_PINS_BK7252N = {1: 26, 2: 24, 3: 20, 4: 28, 5: 1, 6: 10, 7: 13}

# Fallback pad -> channel table covering legacy BK7231Q and all newer families.
ADC_PIN_TO_CHANNEL = {
    4: 1, 26: 1,
    5: 2, 24: 2,
    23: 3, 20: 3,
    2: 4, 28: 4,
    3: 5, 22: 5, 1: 5,
    12: 6, 21: 6, 10: 6,
    13: 7,
}


def adc_channel_pins(chip=""):
    """Return ``{channel: gpio_pin}`` for external ADC channels on ``chip``."""
    c = (chip or "").strip().upper()
    if c in ("BK7231N", "BK7231M", "BL2028N"):
        return dict(ADC_CHANNEL_PINS_BK7231N)
    if c == "BK7238":
        return dict(ADC_CHANNEL_PINS_BK7238)
    if c == "BK7252N":
        return dict(ADC_CHANNEL_PINS_BK7252N)
    return dict(ADC_CHANNEL_PINS_BK7231)


def adc_channel_label(channel, chip=""):
    """Human-readable label for a SARADC channel (e.g. ``'ch3(P23)'``)."""
    ch = int(channel)
    if ch == 0:
        return "ch0(VBAT)"
    c = (chip or "").strip().upper()
    pin = adc_channel_pins(c).get(ch)
    if ch == 7 and c in ("BK7231N", "BK7231M", "BL2028N", "BK7238"):
        return "ch7(TEMP/P13)" if pin is not None else "ch7(TEMP)"
    if pin is not None:
        return "ch%d(P%d)" % (ch, pin)
    if ch == 8:
        return "ch8(TEMP/TSSI)"
    if ch == 9:
        return "ch9(TEMP)"
    return "ch%d" % ch


def parse_adc_channel(target, chip=""):
    """Resolve a channel number, ``'ADCn'``, ``'CHn'``, ``'Pn'``, ``'vbat'`` or ``'temp'`` to 0..15."""
    if isinstance(target, int):
        if 0 <= target <= 15:
            return target
        raise ValueError("ADC channel %d out of range 0..15" % target)
    text = str(target or "").strip()
    if not text:
        raise ValueError("empty ADC channel specification")
    # Drop trailing parenthetical label from GUI combobox items like "3 (P23/P20)"
    head = text.split()[0].strip().lower()
    if "(" in head:
        head = head.split("(", 1)[0].strip()
    if head == "vbat":
        return 0
    if head == "temp":
        c = (chip or "").strip().upper()
        if c == "BK7252":
            return 8
        if c == "BK7252N":
            return 9
        return 7
    if head.startswith(("p", "gpio")):
        pin_s = head[4:] if head.startswith("gpio") else head[1:]
        try:
            pin = int(pin_s, 0)
        except ValueError:
            raise ValueError("bad ADC GPIO pin %r (want e.g. P23, P26)" % target)
        pin_map = {p: ch for ch, p in adc_channel_pins(chip).items()}
        ch = pin_map.get(pin, ADC_PIN_TO_CHANNEL.get(pin))
        if ch is None:
            raise ValueError("GPIO P%d is not an ADC input pad" % pin)
        return ch
    if head.startswith("adc"):
        head = head[3:]
    elif head.startswith("ch"):
        head = head[2:]
    try:
        ch = int(head, 0)
    except ValueError:
        raise ValueError("bad ADC channel %r (want 0..15, ADC1..ADC7, P23, vbat, or temp)" % target)
    if not 0 <= ch <= 15:
        raise ValueError("ADC channel %d out of range 0..15" % ch)
    return ch


def parse_adc_spec(spec, chip=""):
    """Parse ``'CH:VALUE'`` or ``'PIN:VALUE'`` into ``(channel, raw_value)``.

    ``VALUE`` may be an integer sample ``0..65535`` (decimal or ``0x`` hex), or
    a voltage with ``V`` / ``mV`` suffix scaled against 3.3V full-scale (1023).
    """
    text = str(spec or "").strip()
    sep = ":" if ":" in text else ("=" if "=" in text else None)
    if not sep:
        raise ValueError("bad --adc %r (want CH:VALUE or P<pin>:VALUE, e.g. 3:512 or P23:1.65V)" % spec)
    lhs, rhs = text.split(sep, 1)
    ch = parse_adc_channel(lhs, chip=chip)
    rhs_s = rhs.strip()
    rhs_low = rhs_s.lower()
    try:
        if rhs_low.endswith("mv"):
            mv = float(rhs_s[:-2].strip())
            val = int(round(mv * 1023.0 / 3300.0))
        elif rhs_low.endswith("v") and not rhs_low.startswith("0x"):
            volts = float(rhs_s[:-1].strip())
            val = int(round(volts * 1023.0 / 3.3))
        else:
            val = int(rhs_s, 0)
    except ValueError:
        raise ValueError("bad --adc value in %r (want integer 0..65535 or voltage like 1.65V / 1650mV)" % spec)
    if not 0 <= val <= 0xFFFF:
        raise ValueError("bad --adc value %d in %r (must be 0..65535)" % (val, spec))
    return ch, val


def parse_adc_specs(specs, chip=""):
    """Parse a list of ``--adc`` specifications into ``{channel: raw_value}``."""
    out = {}
    for spec in specs or ():
        ch, val = parse_adc_spec(spec, chip=chip)
        out[ch] = val
    return out


def describe_adc(saradc_channel=None, saradc_conversions=0, saradc_reads=0,
                 adc_samples=None, adc_inputs=None, last_raw=None, chip=""):
    """Summarize SARADC activity and configured channel sample values."""
    convs = (sum(saradc_conversions.values())
             if isinstance(saradc_conversions, dict)
             else int(saradc_conversions or 0))
    reads = (sum(saradc_reads.values())
             if isinstance(saradc_reads, dict)
             else int(saradc_reads or 0))
    samples = {}
    if adc_inputs:
        samples.update((int(k), int(v)) for k, v in adc_inputs.items())
    if adc_samples:
        samples.update((int(k), int(v)) for k, v in adc_samples.items())
    active = bool(saradc_channel is not None or convs > 0 or reads > 0)
    if not active and not samples:
        return {
            "active": False,
            "active_channel": None,
            "conversions": convs,
            "reads": reads,
            "samples": samples,
            "last_raw": last_raw,
            "summary": "ADC: idle (default %d / %.2fV)" % (
                DEFAULT_ADC_RAW, DEFAULT_ADC_RAW * 3.3 / 1023.0),
        }
    parts = []
    if saradc_channel is not None:
        raw = int(last_raw if last_raw is not None else samples.get(saradc_channel, DEFAULT_ADC_RAW))
        volts = raw * 3.3 / 1023.0
        parts.append("%s=%d (%.2fV)" % (adc_channel_label(saradc_channel, chip), raw, volts))
    other_samples = [
        "%s=%d (%.2fV)" % (adc_channel_label(ch, chip), val, val * 3.3 / 1023.0)
        for ch, val in sorted(samples.items())
        if ch != saradc_channel
    ]
    if other_samples:
        parts.append(", ".join(other_samples))
    if convs or reads:
        parts.append("%d conv, %d reads" % (convs, reads))
    return {
        "active": active,
        "active_channel": saradc_channel,
        "conversions": convs,
        "reads": reads,
        "samples": samples,
        "last_raw": last_raw,
        "summary": "ADC: " + " — ".join(parts),
    }


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


# ---------------------------------------------------------------------------
# LED driver & Energy Meter peripheral decoding (SM2135, SM2235/SM2135EJ,
# BP5758D, BP1658CJ, WS2812 SPI, PWM lights, BL0942 UART, BL0937 Pulse).
# ---------------------------------------------------------------------------

LED_OFF_HEX = "#181C22"

# Tuya user_param_key dmod values observed across TuyaOS/Tuya SDK dumps.
TUYA_DMOD_NAMES = {
    "0": "PWM",
    "1": "SM16726B",
    "2": "SM2135",
    "3": "SM2135EH",
    "4": "SM2135EJ",
    "5": "BP1658CJ",
    "6": "BP5758D",
    "7": "SM2235",
}


def rgbcw_to_hex(r=0.0, g=0.0, b=0.0, c=0.0, w=0.0):
    """Blend normalized (0.0..1.0) R, G, B, Cool-White, Warm-White into (#RRGGBB, is_lit).

    Warm White (~2700K) contributes a warm golden-amber tint; Cool White
    (~6500K) contributes a crisp daylight tint.  A gentle perceptual curve
    keeps low-duty dimming visible on a UI swatch without clipping full scale.
    """
    r = max(0.0, min(1.0, float(r)))
    g = max(0.0, min(1.0, float(g)))
    b = max(0.0, min(1.0, float(b)))
    c = max(0.0, min(1.0, float(c)))
    w = max(0.0, min(1.0, float(w)))
    level = max(r, g, b, c, w)
    if level <= 0.001:
        return LED_OFF_HEX, False

    # Additive contribution from RGB + Cool White (6500K) + Warm White (2700K)
    lin_r = r + 0.90 * c + 1.00 * w
    lin_g = g + 0.96 * c + 0.78 * w
    lin_b = b + 1.00 * c + 0.42 * w
    peak = max(lin_r, lin_g, lin_b, 1.0)
    lin_r /= peak
    lin_g /= peak
    lin_b /= peak

    # Brightness envelope so 10% dimming is still clearly visible on screen
    bright = 0.25 + 0.75 * (level ** 0.65)
    out_r = int(round(max(0.0, min(1.0, lin_r * bright)) * 255))
    out_g = int(round(max(0.0, min(1.0, lin_g * bright)) * 255))
    out_b = int(round(max(0.0, min(1.0, lin_b * bright)) * 255))
    return "#%02X%02X%02X" % (out_r, out_g, out_b), True


def decode_i2c_led(scl, sda, raw_bytes, channel_map=None):
    """Decode a bit-banged two-wire LED driver frame (SM2135, SM2235, BP5758D, BP1658CJ).

    Returns a dict with driver name, raw channel values, mapped R/G/B/C/W
    fractions, and blended ``hex`` color, or ``None`` when ``raw_bytes`` does
    not match any known LED driver frame format.
    """
    b = bytes(raw_bytes or b"")
    if len(b) < 2:
        return None
    b0 = b[0]
    driver = None
    max_val = 1023
    cur_info = ""
    outs = [0, 0, 0, 0, 0]

    # 1. BP5758D: command byte 10_xx_xxxx (0x80 sleep, 0x90/0xB0 active)
    if (b0 & 0xC0) == 0x80 and (len(b) == 17 or b0 == 0x80):
        driver = "BP5758D"
        max_val = 1023
        if b0 == 0x80 or len(b) < 17:
            cur_info = "sleep"
        else:
            en = b[1] & 0x1F
            curs = []
            for c_byte in b[2:7]:
                # Bit 6 selects the upper current range (+30 mA above 0x40-0x1E)
                ma = (c_byte & 0x3F) + 30 if (c_byte & 0x40) else (c_byte & 0x3F)
                curs.append(ma)
            cur_info = "%s mA" % "/".join(str(x) for x in curs)
            for i in range(5):
                if en & (1 << i):
                    lo = b[7 + 2 * i] & 0x1F
                    hi = b[8 + 2 * i] & 0x1F
                    outs[i] = lo | (hi << 5)
    # 2. SM2235 / SM2135EJ / SM2335: command byte 0xD0..0xEF, 12 bytes total from reg 0
    elif (b0 & 0xF0) in (0xD0, 0xE0) and (b0 & 0x07) == 0 and len(b) == 12:
        driver = "SM2235"
        max_val = 1023
        cur_info = "I=0x%02X" % b[1]
        for i in range(5):
            outs[i] = ((b[2 + 2 * i] & 0x03) << 8) | b[3 + 2 * i]
    # 3. BP1658CJ: command byte 0xB0..0xBF (or 0x80 sleep), 12 bytes total from reg 0
    elif (b0 & 0xF0) == 0xB0 and (b0 & 0x07) == 0 and len(b) == 12:
        driver = "BP1658CJ"
        max_val = 1023
        cur_info = "I=0x%02X" % b[1]
        for i in range(5):
            lo = b[2 + 2 * i] & 0x1F
            hi = b[3 + 2 * i] & 0x1F
            outs[i] = lo | (hi << 5)
    # 4. SM2135 / SM2135E: command byte 0xC0..0xC7, 8-bit channels at regs 2..6
    elif (b0 & 0xF8) == 0xC0 and 3 <= len(b) <= 8:
        driver = "SM2135"
        max_val = 255
        start_reg = b0 & 0x07
        regs = {}
        for idx, val in enumerate(b[1:]):
            regs[start_reg + idx] = val
        if 0 in regs:
            cur_info = "I=0x%02X" % regs[0]
        for i in range(5):
            outs[i] = regs.get(2 + i, 0)
    else:
        return None

    # Default channel ordering when no Tuya iicr..iicw mapping is supplied:
    # BP5758D RGBCCT bulbs typically wire OUT1..5 as R, G, B, C, W;
    # SM2235/SM2135 Tuya bulbs frequently wire OUT0..4 as R, G, B, W, C or R, G, B, C, W.
    cmap = channel_map or {"r": 0, "g": 1, "b": 2, "c": 3, "w": 4}
    def ch_frac(name):
        idx = cmap.get(name)
        if idx is None or not (0 <= idx < 5):
            return 0.0
        return min(1.0, max(0.0, outs[idx] / float(max_val)))

    r, g, bl, c, w = ch_frac("r"), ch_frac("g"), ch_frac("b"), ch_frac("c"), ch_frac("w")
    hex_color, is_lit = rgbcw_to_hex(r, g, bl, c, w)
    return {
        "driver": driver,
        "scl": scl,
        "sda": sda,
        "raw_channels": outs,
        "max_val": max_val,
        "cur_info": cur_info,
        "r": r, "g": g, "b": bl, "c": c, "w": w,
        "hex": hex_color,
        "is_lit": is_lit,
        "raw_hex": b.hex(" "),
    }


def decode_ws2812_spi(buf):
    """Decode SPI-DMA WS2812 bitstream (1 SPI byte per WS2812 bit, GRB order).

    Returns a list of ``(r, g, b)`` tuples (0..255).
    """
    data = bytes(buf or b"")
    if len(data) < 24:
        return []
    n_leds = len(data) // 24
    pixels = []
    for i in range(n_leds):
        chunk = data[i * 24:(i + 1) * 24]
        val = 0
        for byte in chunk:
            bit = 1 if bin(byte).count("1") >= 4 else 0
            val = (val << 1) | bit
        g = (val >> 16) & 0xFF
        r = (val >> 8) & 0xFF
        bl = val & 0xFF
        pixels.append((r, g, bl))
    return pixels


def _tuya_pairs_dict(tuya_config):
    if not tuya_config:
        return {}
    return dict((str(k), str(v)) for k, v in (tuya_config.get("pairs") or []))


def tuya_i2c_channel_map(tuya_config):
    """Extract {r,g,b,c,w -> 0..4} from Tuya user_param_key iicr/g/b/c/w."""
    pairs = _tuya_pairs_dict(tuya_config)
    cmap = {}
    for ch in ("r", "g", "b", "c", "w"):
        val = pairs.get("iic" + ch)
        if val is not None:
            try:
                cmap[ch] = int(val, 0)
            except ValueError:
                pass
    return cmap if len(cmap) >= 3 else None


def describe_peripherals(tuya_config=None, decoded_state=None, i2c_frames=None,
                         i2c_last_on=None, ws2812_spi=None, bl0942_polls=0,
                         meter_values=None, saradc_channel=None,
                         saradc_conversions=None, saradc_reads=None,
                         adc_samples=None, saradc_last_raw=None,
                         adc_inputs=None, chip=""):
    """Synthesize LED driver, Energy Meter, and SARADC state from Tuya config + live hardware.

    Returns ``{"led": dict | None, "meter": dict | None, "adc": dict | None}``.
    """
    pairs = _tuya_pairs_dict(tuya_config)
    cmap = tuya_i2c_channel_map(tuya_config)
    i2c_frames = dict(i2c_frames or {})
    i2c_last_on = dict(i2c_last_on or {})

    # --- 1. LED Driver detection & live color ---
    led = None
    # Check live I2C LED frames first
    best_i2c = None
    best_on = None
    for (scl, sda), raw in sorted(i2c_frames.items()):
        dec = decode_i2c_led(scl, sda, raw, cmap)
        if dec is not None:
            best_i2c = dec
            on_raw = i2c_last_on.get((scl, sda))
            if on_raw:
                best_on = decode_i2c_led(scl, sda, on_raw, cmap)
            break

    ws_pixels = decode_ws2812_spi(ws2812_spi)
    pwm_chans = (decoded_state or {}).get("pwm_channels") or []
    pwm_by_ch = {ch: pct for ch, _per, _duty, _freq, pct in pwm_chans if pct is not None}

    # Map PWM pins from Tuya config (r_pin, g_pin, b_pin, c_pin, w_pin)
    pwm_pin_map = {}
    for ch_name in ("r", "g", "b", "c", "w"):
        p_str = pairs.get("%s_pin" % ch_name)
        if p_str is not None:
            try:
                p_num = int(p_str, 0)
                if p_num in PWM_PINS:
                    pwm_pin_map[ch_name] = PWM_PINS[p_num]
            except ValueError:
                pass

    if best_i2c is not None:
        # If the bulb is blinking in pairing mode and currently on an OFF frame,
        # keep the last-lit color accessible and note that it is blinking.
        active = best_i2c
        blinking_off = (not best_i2c["is_lit"]) and (best_on is not None and best_on["is_lit"])
        shown = best_on if blinking_off else best_i2c
        dmod_name = TUYA_DMOD_NAMES.get(pairs.get("dmod", ""), shown["driver"])
        if dmod_name in ("SM2235", "SM2135EJ", "SM2135EH") and shown["driver"] == "SM2235":
            drv_label = dmod_name
        else:
            drv_label = shown["driver"]
        cur_part = (", %s" % shown["cur_info"]) if shown["cur_info"] else ""
        pcts = "R:%.0f%% G:%.0f%% B:%.0f%% C:%.0f%% W:%.0f%%" % (
            shown["r"] * 100, shown["g"] * 100, shown["b"] * 100,
            shown["c"] * 100, shown["w"] * 100)
        if blinking_off:
            pcts += " (blink paused/off)"
        led = {
            "kind": drv_label,
            "summary": "%s (SCL=P%d SDA=P%d%s)  %s  %s" % (
                drv_label, shown["scl"], shown["sda"], cur_part, pcts, shown["hex"]),
            "hex": shown["hex"],
            "live_hex": active["hex"],
            "is_lit": shown["is_lit"],
            "pixels": [shown["hex"]],
        }
    elif ws_pixels:
        # Addressable RGB strip (WS2812 over SPI DMA), optionally with PWM CW channels
        hex_pixels = []
        r_avg = sum(p[0] for p in ws_pixels) / (255.0 * len(ws_pixels))
        g_avg = sum(p[1] for p in ws_pixels) / (255.0 * len(ws_pixels))
        b_avg = sum(p[2] for p in ws_pixels) / (255.0 * len(ws_pixels))
        for pr, pg, pb in ws_pixels:
            hx, _ = rgbcw_to_hex(pr / 255.0, pg / 255.0, pb / 255.0)
            hex_pixels.append(hx)
        c_val = pwm_by_ch.get(pwm_pin_map.get("c", -1), 0.0) / 100.0
        w_val = pwm_by_ch.get(pwm_pin_map.get("w", -1), 0.0) / 100.0
        hex_color, is_lit = rgbcw_to_hex(r_avg, g_avg, b_avg, c_val, w_val)
        mosi = pairs.get("mosi", "16")
        extra_pwm = ""
        if "c" in pwm_pin_map or "w" in pwm_pin_map:
            extra_pwm = " + PWM C:%.0f%% W:%.0f%%" % (c_val * 100, w_val * 100)
        led = {
            "kind": "WS2812 SPI",
            "summary": "WS2812 SPI (MOSI=P%s, %d LEDs)%s  R:%.0f%% G:%.0f%% B:%.0f%%  %s" % (
                mosi, len(ws_pixels), extra_pwm, r_avg * 100, g_avg * 100, b_avg * 100, hex_color),
            "hex": hex_color,
            "live_hex": hex_color,
            "is_lit": is_lit,
            "pixels": hex_pixels,
        }
    elif "iicscl" in pairs and "iicsda" in pairs:
        drv_label = TUYA_DMOD_NAMES.get(pairs.get("dmod", ""), "I2C LED")
        scl_s, sda_s = pairs.get("iicscl", "?"), pairs.get("iicsda", "?")
        cur_notes = []
        for k in ("2135ccur", "2135wcur", "2235ccur", "2235wcur", "5758ccur", "5758wcur", "rgbcur", "wcur"):
            if k in pairs:
                cur_notes.append("%s=%s" % (k, pairs[k]))
        cur_str = (", " + " ".join(cur_notes)) if cur_notes else ""
        led = {
            "kind": drv_label,
            "summary": "%s (SCL=P%s SDA=P%s%s) — waiting for I2C frames" % (
                drv_label, scl_s, sda_s, cur_str),
            "hex": LED_OFF_HEX,
            "live_hex": LED_OFF_HEX,
            "is_lit": False,
            "pixels": [LED_OFF_HEX],
        }
    elif "mosi" in pairs or "LedNum" in pairs:
        mosi = pairs.get("mosi", "16")
        led_num = pairs.get("LedNum", "?")
        c_val = pwm_by_ch.get(pwm_pin_map.get("c", -1), 0.0) / 100.0
        w_val = pwm_by_ch.get(pwm_pin_map.get("w", -1), 0.0) / 100.0
        hex_color, is_lit = rgbcw_to_hex(0.0, 0.0, 0.0, c_val, w_val)
        cw_note = ("  PWM C:%.0f%% W:%.0f%%  %s" % (c_val * 100, w_val * 100, hex_color)
                   if (c_val > 0 or w_val > 0) else " — waiting for SPI/PWM output")
        led = {
            "kind": "WS2812 SPI",
            "summary": "WS2812 SPI (MOSI=P%s, %s LEDs)%s" % (mosi, led_num, cw_note),
            "hex": hex_color,
            "live_hex": hex_color,
            "is_lit": is_lit,
            "pixels": [hex_color],
        }
    elif pwm_pin_map or pwm_chans:
        # Pure PWM light (either with Tuya r/g/b/c/w_pin mapping or generic PWM channels)
        if pwm_pin_map:
            r = pwm_by_ch.get(pwm_pin_map.get("r", -1), 0.0) / 100.0
            g = pwm_by_ch.get(pwm_pin_map.get("g", -1), 0.0) / 100.0
            bl = pwm_by_ch.get(pwm_pin_map.get("b", -1), 0.0) / 100.0
            c = pwm_by_ch.get(pwm_pin_map.get("c", -1), 0.0) / 100.0
            w = pwm_by_ch.get(pwm_pin_map.get("w", -1), 0.0) / 100.0
            pin_desc = " ".join("%s=P%s" % (k.upper(), pairs["%s_pin" % k])
                                for k in ("r", "g", "b", "c", "w") if "%s_pin" % k in pairs)
            hex_color, is_lit = rgbcw_to_hex(r, g, bl, c, w)
            led = {
                "kind": "PWM Light",
                "summary": "PWM Light (%s)  R:%.0f%% G:%.0f%% B:%.0f%% C:%.0f%% W:%.0f%%  %s" % (
                    pin_desc, r * 100, g * 100, bl * 100, c * 100, w * 100, hex_color),
                "hex": hex_color,
                "live_hex": hex_color,
                "is_lit": is_lit,
                "pixels": [hex_color],
            }
        elif pwm_chans:
            # Generic PWM channels (e.g. OpenBeken or non-Tuya PWM bulb): map ch0..4 -> R,G,B,C,W
            # or single channel -> Warm White
            if len(pwm_chans) == 1:
                r = g = bl = c = 0.0
                w = (pwm_chans[0][4] or 0.0) / 100.0
            elif len(pwm_chans) == 2:
                r = g = bl = 0.0
                c = (pwm_chans[0][4] or 0.0) / 100.0
                w = (pwm_chans[1][4] or 0.0) / 100.0
            else:
                r = pwm_by_ch.get(0, 0.0) / 100.0
                g = pwm_by_ch.get(1, 0.0) / 100.0
                bl = pwm_by_ch.get(2, 0.0) / 100.0
                c = pwm_by_ch.get(3, 0.0) / 100.0
                w = pwm_by_ch.get(4, 0.0) / 100.0
            hex_color, is_lit = rgbcw_to_hex(r, g, bl, c, w)
            ch_str = " ".join("ch%d:%.0f%%" % (ch[0], ch[4] or 0.0) for ch in pwm_chans)
            led = {
                "kind": "PWM Output",
                "summary": "PWM Output (%s)  %s" % (ch_str, hex_color),
                "hex": hex_color,
                "live_hex": hex_color,
                "is_lit": is_lit,
                "pixels": [hex_color],
            }

    # --- 2. Energy Meter detection (BL0942 UART / BL0937 Pulse) ---
    meter = None
    v_val, a_val, w_val = meter_values if meter_values else (230.0, 1.5, 345.0)
    if bl0942_polls > 0:
        meter = {
            "kind": "BL0942 (UART1)",
            "summary": "BL0942 UART1 (%d polls) — %.1fV  %.2fA  %.1fW" % (
                bl0942_polls, v_val, a_val, w_val),
            "cf_pin": None,
            "cf1_pin": None,
            "sel_pin": None,
            "sel_lv": 1,
        }
    elif "ele_pin" in pairs or "vi_pin" in pairs:
        def _pin_int(key):
            try:
                return int(pairs[key], 0)
            except (KeyError, ValueError):
                return None
        cf_pin = _pin_int("ele_pin")
        cf1_pin = _pin_int("vi_pin")
        sel_pin = _pin_int("sel_pin_pin")
        sel_lv = _pin_int("sel_pin_lv")
        meter = {
            "kind": "BL0937 (Pulse)",
            "summary": "BL0937 Pulse (CF=P%s CF1=P%s SEL=P%s) — %.1fV  %.2fA  %.1fW" % (
                pairs.get("ele_pin", "?"), pairs.get("vi_pin", "?"),
                pairs.get("sel_pin_pin", "?"), v_val, a_val, w_val),
            "cf_pin": cf_pin,
            "cf1_pin": cf1_pin,
            "sel_pin": sel_pin,
            "sel_lv": 1 if sel_lv is None else sel_lv,
        }

    adc = describe_adc(
        saradc_channel=saradc_channel,
        saradc_conversions=saradc_conversions,
        saradc_reads=saradc_reads,
        adc_samples=adc_samples,
        adc_inputs=adc_inputs,
        last_raw=saradc_last_raw,
        chip=chip,
    )
    return {"led": led, "meter": meter, "adc": adc}


# Same captured offsets mean different peripherals per chip: 0x100+ is pwm_new
# on the N family but AUDIO on 7252, so the chip selects the decoder. This
# lives here rather than in the caller because it is a decoding decision, and
# there are now two callers - the self-test report and the GUI, which reads the
# emulator's live register dicts instead of captured [EMU_*] lines.
PWM_NEW_CHIPS = {"BK7231N", "BK7231M", "BL2028N", "BK7238"}


def decode_state(gpio, pwm, chip="", i2c_frames=None, i2c_last_on=None,
                 ws2812_spi=None, tuya_config=None, bl0942_polls=0,
                 meter_values=None, saradc_channel=None,
                 saradc_conversions=None, saradc_reads=None,
                 adc_samples=None, saradc_last_raw=None, adc_inputs=None):
    """Decode raw {pin: value} / {offset: value} register maps.

    Returns the dict the report and the GUI both render, or None when the
    firmware has not touched any peripheral yet.
    """
    if (not gpio and not pwm and not i2c_frames and not ws2812_spi
            and not bl0942_polls and not saradc_conversions):
        return None
    if chip in PWM_NEW_CHIPS and any(off >= 0x100 for off in pwm):
        channels = pwm_new_channels(pwm)
        ctl, base, layout = 0, None, "new"
    else:
        channels, ctl, base = pwm_channels(pwm)
        layout = "old"
    logical_gpio = gpio_pin_values(gpio, chip)
    pin_count = chip_pin_count(chip)
    out = {"gpio": gpio_table(logical_gpio, pin_count),
           "pwm_layout": layout,
           # Logical pin keys are what GUI actions and report assertions need;
           # controller/function registers remain available through ``func``.
           "raw": logical_gpio,
           "pwm_pins": pwm_output_pins(gpio),
           "func": func_rows(gpio),
           "pwm_ctl": ctl,
           "pwm_base": base,
           "pwm_channels": channels,
           "pwm_regs": pwm_registers(pwm)}
    periphs = describe_peripherals(
        tuya_config=tuya_config, decoded_state=out,
        i2c_frames=i2c_frames, i2c_last_on=i2c_last_on,
        ws2812_spi=ws2812_spi, bl0942_polls=bl0942_polls,
        meter_values=meter_values, saradc_channel=saradc_channel,
        saradc_conversions=saradc_conversions, saradc_reads=saradc_reads,
        adc_samples=adc_samples, saradc_last_raw=saradc_last_raw,
        adc_inputs=adc_inputs, chip=chip)
    out["led"] = periphs["led"]
    out["meter"] = periphs["meter"]
    out["adc"] = periphs["adc"]
    return out


# Silicon name implied by a dump's filename. Report chip names (BK7231N,
# BK7231T, ...) are finer-grained than the four -chip CLI identities, and
# decode_state above needs the fine-grained one, so this lives beside it and
# is shared by the self-test report and the GUI.
CHIP_RE = re.compile(r"(BL2028N|BK7252N|BK7231[TUNMQ]|BK7238|BK7252|BK7236|BK7258|BK3231)", re.I)


def chip_from_name(filename):
    """Chip a dump's filename names, or "" when it says nothing."""
    m = CHIP_RE.search(os.path.basename(filename or ""))
    return m.group(1).upper() if m else ""


def _self_test():
    """Unit tests for peripheral decoders and SARADC emulation."""
    import sys as _sys
    _sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    from src.emulator import BekenEmulator

    # 1. GPIO slot & pin decoding
    assert gpio_slot(10, 40) == 10
    assert gpio_slot(32, 40) == 48
    assert gpio_slot(39, 40) == 55
    assert gpio_slot(40, 40) is None
    assert gpio_slot(32, 32) is None
    assert "OUTPUT = 0" in decode_pin(0x00)
    assert "OUTPUT = 1" in decode_pin(0x02)
    assert "pull-up" in decode_pin(0x3C)
    assert "pull-down" in decode_pin(0x2C)
    assert "second function" in decode_pin(0x48)

    # 2. PWM old & pwm_new decoders
    old_ch, old_ctl, old_base = pwm_channels({0x80: 0x11, 0x88: 26000, 0x8C: 13000})
    assert old_base == 0x80 and old_ctl == 0x11
    assert len(old_ch) == 1 and old_ch[0][:3] == (0, 26000, 13000)
    assert abs(old_ch[0][3] - 1000.0) < 0.1 and abs(old_ch[0][4] - 50.0) < 0.1

    new_ch = pwm_new_channels({0x100: 0x09, 0x104: 0x171B, 0x108: 0x4E75, 0x110: 0x6590})
    assert len(new_ch) == 1 and new_ch[0][0] == 0 and new_ch[0][1] == 0x6590
    assert abs(new_ch[0][4] - 54.5) < 0.2

    # 3. I2C LED decoders (SM2135, SM2235, BP5758D, BP1658CJ) & WS2812 SPI
    sm2135 = decode_i2c_led(24, 26, bytes.fromhex("c01402ff00000000"))
    assert sm2135 is not None and sm2135["driver"] == "SM2135" and sm2135["r"] == 1.0
    sm2235 = decode_i2c_led(26, 24, bytes.fromhex("d82400000000000003a20000"))
    assert sm2235 is not None and sm2235["driver"] == "SM2235" and sm2235["c"] > 0.9
    bp5758 = decode_i2c_led(24, 26, bytes.fromhex("b01f0808081e1ebcbfbcbfbcbfa0a0a0a0"))
    assert bp5758 is not None and bp5758["driver"] == "BP5758D" and bp5758["is_lit"]
    bp1658 = decode_i2c_led(24, 26, bytes.fromhex("b8231f1f0000000000000000"))
    assert bp1658 is not None and bp1658["driver"] == "BP1658CJ" and bp1658["r"] == 1.0

    ws_spi = (b"\xf8" * 8 + b"\xc0" * 16) * 3   # 3 pure green GRB pixels
    ws_px = decode_ws2812_spi(ws_spi)
    assert ws_px == [(0, 255, 0), (0, 255, 0), (0, 255, 0)]

    # 4. SARADC parser & descriptor helpers
    assert parse_adc_channel("3") == 3
    assert parse_adc_channel("ch5") == 5
    assert parse_adc_channel("P26", chip="BK7231T") == 1
    assert parse_adc_channel("P26", chip="BK7238") == 4
    assert parse_adc_spec("3:512") == (3, 512)
    assert parse_adc_spec("P23:1.65V", chip="BK7231T") == (3, 512)
    assert parse_adc_spec("ch0:1650mV") == (0, 512)
    assert parse_adc_specs(["0:420", "P26:777"], chip="BK7231N") == {0: 420, 1: 777}
    adc_desc = describe_adc(saradc_channel=3, saradc_conversions=2, saradc_reads=2,
                            adc_samples={3: 512}, last_raw=512, chip="BK7231T")
    assert adc_desc["active"] and "ch3(P23)=512" in adc_desc["summary"]

    # 5. BekenEmulator SARADC MMIO emulation (BK7231T/N and BK7238 register layouts)
    import struct as _struct
    emu_t = BekenEmulator(raw_flash=b"\xff" * 0x1000, bootloader=None, app=b"\x00" * 64,
                          only_uart=True, chip=CHIPS["BK7231"],
                          adc_inputs={1: 700, 3: 512})
    emu_t.setup()
    assert emu_t.get_adc_input(1) == 700 and emu_t.get_adc_input(3) == 512
    assert emu_t.get_adc_input(2) == DEFAULT_ADC_RAW
    emu_t.set_adc_input("P24", 888)   # P24 -> ch2 on BK7231T
    assert emu_t.get_adc_input(2) == 888
    # Trigger BK7231T SARADC on channel 3:
    # mode=1 (bits 1:0), chn_en=1 (bit 2), chn_sel=3 (bits 5:3 -> 0x18), 4clk_delay (bit 6 -> 0x40)
    emu_t.hook_mem_write_mmio(emu_t.mu, 0, 0x00802c00, 4, 0x01 | 0x04 | (3 << 3) | 0x40, None)
    assert emu_t.state.saradc_channel == 3
    assert emu_t.state.saradc_last_raw == 512
    assert emu_t.state.saradc_pending == 32
    samples = []
    for _ in range(32):
        emu_t.hook_mem_read_mmio(emu_t.mu, 0, 0x00802c04, 4, 0, None)
        samples.append(_struct.unpack("<I", emu_t.mu.mem_read(0x00802c04, 4))[0])
    assert samples == [512] * 32
    emu_t.hook_mem_read_mmio(emu_t.mu, 0, 0x00802c00, 4, 0, None)
    cfg_read = _struct.unpack("<I", emu_t.mu.mem_read(0x00802c00, 4))[0]
    assert (cfg_read & (1 << 28)) != 0   # FIFO_EMPTY asserted after draining batch

    # BK7238 SARADC layout (0x00802c08 CFG2, 0x00802c0c STATE, 0x00802c10 DATA)
    emu_38 = BekenEmulator(raw_flash=b"\xff" * 0x1000, bootloader=None, app=b"\x00" * 64,
                           only_uart=True, chip=CHIPS["BK7238"],
                           adc_inputs={4: 640})
    emu_38.setup()
    emu_38.hook_mem_write_mmio(emu_38.mu, 0, 0x00802c08, 4, 4, None)        # channel 4 (P26 on BK7238)
    emu_38.hook_mem_write_mmio(emu_38.mu, 0, 0x00802c0c, 4, 0x00004008, None)  # CHNL_EN
    assert emu_38.state.saradc_channel == 4
    emu_38.hook_mem_read_mmio(emu_38.mu, 0, 0x00802c10, 4, 0, None)
    assert _struct.unpack("<I", emu_38.mu.mem_read(0x00802c10, 4))[0] == 640
    print("periph.py: all self-tests passed")


if __name__ == "__main__":
    _self_test()


