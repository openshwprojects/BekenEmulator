/* RivieraWaves BLE 5.x core register model (the 0x900000 block). Port of
 * src/blecore.py. */
#ifndef BK_BLECORE_H
#define BK_BLECORE_H

#include "common.h"

/* A memory accessor the core uses to reach exchange memory (guest RAM). */
struct MemAccess {
    void *ctx;
    Bytes (*read)(void *ctx, u32 addr, u32 size);
    void (*write)(void *ctx, u32 addr, const Bytes &data);
};

struct AdField {
    u8 type;
    Bytes value;
};
struct AdvFound {
    u32 offset;
    Bytes raw;
    std::vector<AdField> fields;
};

class BleCore {
public:
    BleCore(u32 base, u64 insns_per_second, const std::string &layout = "5.2", u64 max_sleep_us = 10000);

    /* register access: read returns served value (has=true), or has=false for plain memory */
    u32 read(u32 address, u64 insns, bool &has);
    void write(u32 address, u32 value, u64 insns);
    /* tick: returns core_irq / ble_irq level flags */
    void tick(u64 insns, bool &core_irq, bool &ble_irq);

    u32 window;
    std::string layout;

    void set_mem(const MemAccess &m) { mem_ = m; have_mem_ = true; }

    /* Snapshot: the full core state, as Python pickles it (mem is re-bound by
     * the emulator after a load). deserialize builds a new core. */
    void serialize(Bytes &out) const;
    static BleCore *deserialize(const Bytes &in, size_t &p);
    u32 em_base();
    std::vector<AdvFound> find_adv();
    static std::vector<AdField> parse_ad(const Bytes &blob);
    static StrList describe_ad(const std::vector<AdField> &fields);

    /* events log for self-tests: (name, insns, extra) */
    struct Event { std::string name; u64 insns; u64 extra; };
    std::vector<Event> events;

    /* expose a few internals the self-tests read */
    bool raw_core_has(u32 bit) const { return (raw_core_ & bit) != 0; }
    u64 wake_at() const { return wake_at_; }
    u64 max_sleep_insns() const { return max_sleep_insns_; }
    u64 insns_per_lp_cycle() const { return insns_per_lp_cycle_; }
    u32 raw_core_value() const { return raw_core_; }
    u32 slot(u64 insns) const;
    u32 fine(u64 insns) const;

private:
    u32 base_;
    u32 slotclk_, finetimecnt_, version_;
    bool em_fixed_valid_;
    u32 em_fixed_;
    bool ble_version_valid_;
    u32 ble_version_off_, ble_version_val_;
    bool ble_control_valid_;
    u32 ble_control_;
    std::vector<u32> clkntgt_, hmicrosectgt_;
    u64 max_sleep_insns_, insns_per_slot_, insns_per_lp_cycle_;
    std::map<u32, u32> regs_;
    u32 raw_evt_, raw_core_;
    u32 slot_latch_;
    u32 slot_offset_;
    bool last_clkn_slot_valid_;
    u32 last_clkn_slot_;
    bool sleeping_;
    u64 wake_at_, slept_from_;
    std::map<u32, u32> fired_tgt_;
    MemAccess mem_;
    bool have_mem_;

    void aes_run();
    static bool reached(u32 now, u32 target);
};

int blecore_selftests();

#endif
