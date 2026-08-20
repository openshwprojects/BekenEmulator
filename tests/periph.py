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
    # Writes at or above +0x80 mean the shifted layout is in use.
    return 0x80 if any(off >= 0x80 for off in pwm) else 0x00


def pwm_channels(pwm):
    """[(channel, period, duty, freq_hz, duty_pct)] for channels with a period."""
    base = infer_base(pwm)
    rows = []
    for ch in range(6):
        period = pwm.get(base + 0x08 + 8 * ch)
        duty = pwm.get(base + 0x0C + 8 * ch)
        if not period:
            continue
        freq = PWM_CLOCK_HZ / period if period else 0
        pct = (100.0 * duty / period) if duty is not None else None
        rows.append((ch, period, duty, freq, pct))
    return rows, pwm.get(base, 0), base


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
