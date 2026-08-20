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


def decode_pin(value):
    """Return a readable description of one pin config word."""
    out_en = bool(value & (1 << 3))
    in_en = bool(value & (1 << 2))
    pull_en = bool(value & (1 << 5))
    level = 1 if value & (1 << 1) else 0
    bits = []
    if value & (1 << 6):
        bits.append("second function")
    if out_en:
        bits.append("OUTPUT = %d" % level)
    elif value & (1 << 1):
        # The output latch is set but the driver is not enabled - worth showing
        # rather than calling the pin "disabled".
        bits.append("output latch = %d (not driven)" % level)
    if in_en:
        bits.append("input")
    if pull_en:
        bits.append("pull-%s" % ("up" if value & (1 << 4) else "down"))
    if value & (1 << 7):
        bits.append("monitor")
    if not bits:
        bits.append("configured, all functions off")
    return ", ".join(bits)


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
    """[(pin, state, detail)] for every pin, touched or not."""
    rows = []
    for pin in range(PIN_COUNT):
        if pin in gpio:
            rows.append((pin, "set by firmware", decode_pin(gpio[pin])))
        else:
            rows.append((pin, "factory", "not configured"))
    return rows


def func_rows(gpio):
    return [(FUNC_REGS[i], "0x%08X" % v) for i, v in sorted(gpio.items())
            if i in FUNC_REGS]


# PWM. PWM_CTL at +0x00 carries four bits per channel (enable, interrupt
# enable, two mode bits). The period/duty register spacing differs between the
# two layouts pwm.h selects with #if (PWM_BASE is either PWM_NEW_BASE or
# PWM_NEW_BASE + 0x80), and which one a given image uses has not been
# confirmed here - so the individual registers are reported at their observed
# offsets rather than guessed into period/duty pairs.
def pwm_channels(pwm):
    """[(channel, enabled, mode)] decoded from PWM_CTL, which is unambiguous."""
    ctl = pwm.get(0x00, 0)
    rows = []
    for ch in range(6):
        base = ch * 4
        rows.append((ch,
                     bool(ctl & (1 << base)),
                     (ctl >> (base + 2)) & 0x03))
    return rows, ctl


def pwm_registers(pwm):
    """[(offset, value)] for every PWM register the firmware wrote."""
    return [("+0x%02X" % off, "0x%08X" % val) for off, val in sorted(pwm.items())]
