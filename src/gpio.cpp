#include "gpio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdexcept>

/* Python raises ValueError for the invalid arguments below; the C++ port
 * throws std::invalid_argument in the same places. */
GpioModel::GpioModel(int pc) : pin_count(pc), int_enable(0), int_status(0)
{
    if (pc <= 0 || pc > 64)
        throw std::invalid_argument("GPIO pin count must be a positive integer");
    int_modes.assign((size_t)pc, INT_LEVEL_LOW);
}

int GpioModel::check_pin(int pin) const
{
    if (pin < 0 || pin >= pin_count) {
        char msg[64];
        snprintf(msg, sizeof msg, "GPIO pin must be in the range 0-%d", pin_count - 1);
        throw std::invalid_argument(msg);
    }
    return pin;
}

void GpioModel::check_first_pin(int first_pin) const
{
    if (first_pin < 0 || first_pin >= pin_count)
        throw std::invalid_argument("bad first GPIO pin for interrupt mode register");
}

int GpioModel::check_level(int level)
{
    if (level < 0)
        return -1; /* None */
    if (level == 0 || level == 1)
        return level;
    return -2; /* invalid */
}

int GpioModel::effective_level(int pin, u32 config) const
{
    check_pin(pin);
    if (!inputs.empty()) {
        std::map<int, int>::const_iterator it = inputs.find(pin);
        if (it != inputs.end())
            return it->second;
    }
    if (config & GPIO_PULL_ENABLE_BIT)
        return (config & GPIO_PULL_MODE_BIT) ? 1 : 0;
    return (config & GPIO_INPUT_BIT) ? 1 : 0;
}

u32 GpioModel::read_config(int pin, u32 config) const
{
    int level = effective_level(pin, config);
    return (config & ~GPIO_INPUT_BIT) | (u32)level;
}

bool GpioModel::mode_matches_transition(int pin, int before, int after) const
{
    int mode = int_modes[(size_t)pin];
    return (mode == INT_EDGE_RISING && before == 0 && after == 1)
           || (mode == INT_EDGE_FALLING && before == 1 && after == 0);
}

bool GpioModel::mode_matches_level(int pin, int level) const
{
    int mode = int_modes[(size_t)pin];
    return (mode == INT_LEVEL_LOW && level == 0) || (mode == INT_LEVEL_HIGH && level == 1);
}

void GpioModel::latch_change(int pin, int before, int after)
{
    u64 bit = 1ULL << pin;
    if (!(int_enable & bit))
        return;
    if (mode_matches_transition(pin, before, after) || mode_matches_level(pin, after))
        int_status |= bit;
}

void GpioModel::drive(int pin, int level, u32 config, int *before_out, int *after_out)
{
    check_pin(pin);
    level = check_level(level);
    if (level < -1)
        throw std::invalid_argument("GPIO input level must be 0, 1, or None (released)");
    int before = effective_level(pin, config);
    if (level < 0)
        inputs.erase(pin);
    else
        inputs[pin] = level;
    int after = effective_level(pin, config);
    latch_change(pin, before, after);
    if (before_out)
        *before_out = before;
    if (after_out)
        *after_out = after;
}

void GpioModel::config_changed(int pin, u32 old_config, u32 new_config)
{
    check_pin(pin);
    if (!(int_enable & (1ULL << pin)))
        return;
    int before = effective_level(pin, old_config);
    int after = effective_level(pin, new_config);
    latch_change(pin, before, after);
}

void GpioModel::write_enable(u64 value)
{
    int_enable = value & pin_mask();
    int_status &= pin_mask();
}

void GpioModel::write_mode_word(int first_pin, u32 value, int count)
{
    check_first_pin(first_pin);
    if (count > pin_count - first_pin)
        count = pin_count - first_pin;
    for (int i = 0; i < count; i++)
        int_modes[(size_t)(first_pin + i)] = (int)((value >> (2 * i)) & 0x3);
}

u32 GpioModel::read_mode_word(int first_pin, int count) const
{
    check_first_pin(first_pin);
    if (count > pin_count - first_pin)
        count = pin_count - first_pin;
    u32 value = 0;
    for (int i = 0; i < count; i++)
        value |= (u32)(int_modes[(size_t)(first_pin + i)] & 0x3) << (2 * i);
    return value;
}

void GpioModel::clear_status(u64 value)
{
    int_status &= ~(value & pin_mask());
}

void GpioModel::refresh_level_interrupts(ConfigFn fn, void *ctx)
{
    u64 enabled = int_enable;
    if (!enabled)
        return;
    while (enabled) {
        u64 bit = enabled & (~enabled + 1);
        enabled ^= bit;
        int pin = __builtin_ctzll(bit);
        int mode = int_modes[(size_t)pin];
        if (mode != INT_LEVEL_LOW && mode != INT_LEVEL_HIGH)
            continue;
        if (mode_matches_level(pin, effective_level(pin, fn(ctx, pin))))
            int_status |= bit;
    }
}

/* ------------------------------------------------------------- self-tests */
static int g_checks;
static void gcheck(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
    g_checks++;
    printf("  [PASS] %s\n", msg);
}

static u32 cfg_pullup(void *, int pin) { return pin == 3 ? 0x3C : 0x2C; }
static u32 cfg_const_pullup(void *, int) { return 0x3C; }

int gpio_selftests()
{
    g_checks = 0;
    const u32 pull_up = 0x3C, pull_down = 0x2C;

    printf("== external drive and pull fallback ==\n");
    {
        GpioModel g(32);
        gcheck(g.read_config(9, pull_up) & 1, "a released pull-up input reads high");
        g.drive(9, 0, pull_up);
        gcheck((g.read_config(9, pull_up) & 1) == 0, "an external low wins over the pull-up");
        g.drive(9, -1, pull_up);
        gcheck(g.read_config(9, pull_up) & 1, "releasing the external drive falls back to the pull-up");
        g.drive(10, 1, pull_down);
        gcheck(g.read_config(10, pull_down) & 1, "an external high wins over a pull-down");
        g.drive(10, -1, pull_down);
        gcheck((g.read_config(10, pull_down) & 1) == 0, "release falls back to the pull-down as well");
        gcheck(g.read_config(9, pull_up | 0xA00) == (pull_up | 0xA01), "sampling changes only input bit 0");
    }

    printf("== interrupt mode registers and INTEN gate ==\n");
    {
        GpioModel g(32);
        u32 modes = (u32)(INT_EDGE_FALLING << (2 * 3)) | (u32)(INT_EDGE_RISING << (2 * 4));
        g.write_mode_word(0, modes);
        gcheck(g.read_mode_word(0) == modes, "INTLV packs one two-bit mode per pin");
        g.drive(3, 0, pull_up);
        gcheck(g.int_status == 0, "a disabled edge does not latch INTSTA");
        g.drive(3, -1, pull_up);
        g.write_enable((1u << 3) | (1u << 4));
        g.drive(3, 0, pull_up);
        gcheck(g.int_status == (1u << 3) && g.irq_asserted(),
               "an enabled falling edge latches its pin and asserts GPIO IRQ");
        g.clear_status(1u << 3);
        g.refresh_level_interrupts(cfg_pullup, 0);
        gcheck(g.int_status == 0, "a held edge input does not reassert after W1C");
        g.drive(4, 1, pull_down);
        gcheck(g.int_status == (1u << 4), "rising mode latches only the rising edge");
    }

    printf("== low/high level interrupts and W1C ==\n");
    {
        GpioModel g(32);
        g.write_mode_word(0, (u32)(INT_LEVEL_LOW << (2 * 6)));
        g.write_enable(1u << 6);
        g.refresh_level_interrupts(cfg_const_pullup, 0);
        gcheck(g.int_status == 0, "low-level mode stays quiet while its pin is high");
        g.drive(6, 0, pull_up);
        gcheck(g.int_status == (1u << 6), "low-level mode asserts when the pin is low");
        g.clear_status(1u << 6);
        gcheck(g.int_status == 0, "INTSTA clears only the bits written as one");
        g.refresh_level_interrupts(cfg_const_pullup, 0);
        gcheck(g.int_status == (1u << 6), "a held level condition reasserts after status is cleared");
        g.drive(6, -1, pull_up);
        g.clear_status(1u << 6);
        g.refresh_level_interrupts(cfg_const_pullup, 0);
        gcheck(g.int_status == 0, "release removes the low-level condition");

        GpioModel g2(32);
        g2.write_mode_word(0, (u32)(INT_LEVEL_HIGH << (2 * 7)));
        g2.write_enable(1u << 7);
        g2.refresh_level_interrupts(cfg_const_pullup, 0);
        gcheck(g2.int_status == (1u << 7), "high-level mode asserts immediately when enabled on a high pin");
    }

    printf("== validation and extended bank ==\n");
    {
        GpioModel g(32);
        bool rejected = false;
        try {
            g.drive(32, 0, pull_up);
        } catch (std::invalid_argument &) {
            rejected = true;
        }
        gcheck(rejected, "a 32-pin model rejects an interrupt-register index as a pin");
        GpioModel ext(40);
        ext.write_mode_word(32, (u32)(INT_EDGE_RISING << (2 * 7)), 8);
        ext.write_enable(1ULL << 39);
        ext.drive(39, 1, pull_down);
        gcheck(ext.int_status == (1ULL << 39),
               "the BK7231U/BK7252 upper GPIO bank keeps logical pins 32-39 separate");
    }

    printf("\nAll %d GPIO model self-tests passed.\n", g_checks);
    return g_checks;
}
