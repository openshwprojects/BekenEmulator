/* Beken GPIO input and interrupt register model. Port of src/gpio.py. */
#ifndef BK_GPIO_H
#define BK_GPIO_H

#include "common.h"

static const u32 GPIO_INPUT_BIT = 1u << 0;
static const u32 GPIO_PULL_MODE_BIT = 1u << 4;   /* 1 = up, 0 = down */
static const u32 GPIO_PULL_ENABLE_BIT = 1u << 5;

static const int INT_LEVEL_LOW = 0;
static const int INT_LEVEL_HIGH = 1;
static const int INT_EDGE_RISING = 2;
static const int INT_EDGE_FALLING = 3;

class GpioModel {
public:
    explicit GpioModel(int pin_count = 32);

    int pin_count;
    std::map<int, int> inputs;   /* pin -> externally driven 0/1; absent = released */
    u64 int_enable;              /* up to 40 pins */
    u64 int_status;
    std::vector<int> int_modes;

    u64 pin_mask() const { return pin_count >= 64 ? ~0ULL : ((1ULL << pin_count) - 1); }

    /* throws std::invalid_argument (Python ValueError) unless 0 <= pin < pin_count */
    int check_pin(int pin) const;

    /* level helpers (effective_level/read_config/drive/config_changed validate the pin) */
    int effective_level(int pin, u32 config) const;
    u32 read_config(int pin, u32 config) const;

    /* returns (before, after). level < 0 means release (None). */
    void drive(int pin, int level, u32 config, int *before = 0, int *after = 0);
    void config_changed(int pin, u32 old_config, u32 new_config);

    void write_enable(u64 value);
    void write_mode_word(int first_pin, u32 value, int count = 16);
    u32 read_mode_word(int first_pin, int count = 16) const;
    void clear_status(u64 value);

    /* config_for_pin: a function the caller supplies to resample config words. */
    typedef u32 (*ConfigFn)(void *ctx, int pin);
    void refresh_level_interrupts(ConfigFn fn, void *ctx);

    bool irq_asserted() const { return (int_status & int_enable) != 0; }

    /* check_level: validate a level (0/1/None), return -1 for None, -2 invalid. */
    static int check_level(int level);

private:
    bool mode_matches_transition(int pin, int before, int after) const;
    bool mode_matches_level(int pin, int level) const;
    void latch_change(int pin, int before, int after);
    void check_first_pin(int first_pin) const;
};

int gpio_selftests(); /* returns number of checks; aborts on failure */

#endif
