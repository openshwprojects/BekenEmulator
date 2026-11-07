/* Port of tests/timers.py: Hardware timer banks (0x802A00 / 0x802A40). */
#include "pytests.h"
#include "chips.h"

static int pt_checks;

typedef PyTestAccess A;

static const u32 ICU_BIT_TIMER = 1u << 8;
static const u32 BANK0_COUNTER = 0x00802A00u, BANK0_CTL = 0x00802A0Cu;
static const u32 BANK1_COUNTER = 0x00802A40u, BANK1_CTL = 0x00802A4Cu;
static const u32 STATUS0 = 1u << 7;
static const u64 SECOND = BekenEmulator::TIMER_INSNS_PER_SEC;

static BekenEmulator *make_emu(const char *name = "BK7252")
{
    BuildOptions opt;
    opt.only_uart = true;
    opt.chip = name;
    const Chip *c = find_chip(name);
    ChipId cid;
    cid.chip = c ? c->chip_id : default_chip().chip_id;
    cid.device = c ? c->dev_id : default_chip().dev_id;
    BekenEmulator *emu = new BekenEmulator(Bytes(0x20000, (char)0xFF), Bytes(), Bytes(), opt, cid, 0, c);
    emu->setup();
    emu->state.icu_int_enable |= ICU_BIT_TIMER;
    return emu;
}

static void wr(BekenEmulator &emu, u32 address, u32 value)
{
    emu.mmio_write(address, 4, value);
}

static u32 ctl_of(BekenEmulator &emu, int bank)
{
    return bank == 0 ? emu.state.timer0_2_ctl : emu.state.timer3_5_ctl;
}

static void step(BekenEmulator &emu, u64 insns)
{
    emu.state.insn_count += insns;
    A::pacing(emu);
}

static bool test_period_follows_the_counter()
{
    BekenEmulator *emu = make_emu();
    u64 period = SECOND / 1000;
    wr(*emu, BANK0_COUNTER, 26000);
    wr(*emu, BANK0_CTL, 0x1);
    step(*emu, period - 1);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) == 0, "not fired before period");
    step(*emu, 1);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) != 0, "fired at period");
    PT_CHECK((emu->state.pending_irqs & ICU_BIT_TIMER) != 0, "ICU_BIT_TIMER pending");
    delete emu;
    return true;
}

static bool test_one_second_gives_a_thousand_ticks()
{
    BekenEmulator *emu = make_emu();
    wr(*emu, BANK0_COUNTER, 26000);
    wr(*emu, BANK0_CTL, 0x1);
    int ticks = 0;
    for (u64 i = 0; i < SECOND / 1000; i++) {
        step(*emu, 1000);
        if (ctl_of(*emu, 0) & STATUS0) {
            ticks++;
            wr(*emu, BANK0_CTL, 0x1 | STATUS0);
        }
    }
    PT_CHECK(ticks == 1000, "ticks == 1000");
    delete emu;
    return true;
}

static bool test_divider_and_32k_bank()
{
    BekenEmulator *emu = make_emu();
    wr(*emu, BANK1_COUNTER, 32);
    wr(*emu, BANK1_CTL, 0x1 | (1u << 3));
    u64 period = 32ULL * 2ULL * SECOND / 32000ULL;
    step(*emu, period - 1);
    PT_CHECK((ctl_of(*emu, 1) & STATUS0) == 0, "bank 1 not fired before period");
    step(*emu, 1);
    PT_CHECK((ctl_of(*emu, 1) & STATUS0) != 0, "bank 1 fired at period");
    delete emu;
    return true;
}

static bool test_timers_are_independent()
{
    BekenEmulator *emu = make_emu();
    wr(*emu, BANK0_COUNTER + 8, 26000);
    wr(*emu, BANK0_CTL, 0x4);
    step(*emu, SECOND / 1000);
    PT_CHECK((ctl_of(*emu, 0) & (1u << 9)) != 0, "timer 2 status set");
    PT_CHECK((ctl_of(*emu, 0) & (STATUS0 | (1u << 8))) == 0, "timers 0 & 1 status clear");
    delete emu;
    return true;
}

static bool test_disabled_timer_never_fires()
{
    BekenEmulator *emu = make_emu();
    wr(*emu, BANK0_COUNTER, 26000);
    step(*emu, SECOND);
    PT_CHECK((ctl_of(*emu, 0) & (0x7u << 7)) == 0, "disabled timer never fires");
    wr(*emu, BANK0_CTL, 0x1);
    step(*emu, SECOND / 2000);
    wr(*emu, BANK0_CTL, 0x0);
    step(*emu, SECOND);
    PT_CHECK((ctl_of(*emu, 0) & (0x7u << 7)) == 0, "stopped timer never fires");
    delete emu;
    return true;
}

static bool test_missed_periods_coalesce_into_one_tick()
{
    BekenEmulator *emu = make_emu();
    u64 period = SECOND / 1000;
    wr(*emu, BANK0_COUNTER, 26000);
    wr(*emu, BANK0_CTL, 0x1);
    step(*emu, 10 * period);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) != 0, "fired after 10 periods");
    wr(*emu, BANK0_CTL, 0x1 | STATUS0);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) == 0, "W1C cleared status");
    step(*emu, period - 1);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) == 0, "coalesced: not fired before next period");
    step(*emu, 1);
    PT_CHECK((ctl_of(*emu, 0) & STATUS0) != 0, "fires at next period");
    delete emu;
    return true;
}

static bool test_acknowledged_status_drops_the_interrupt()
{
    BekenEmulator *emu = make_emu();
    wr(*emu, BANK0_COUNTER, 26000);
    wr(*emu, BANK0_CTL, 0x1);
    step(*emu, SECOND / 1000);
    PT_CHECK((emu->state.pending_irqs & ICU_BIT_TIMER) != 0, "IRQ pending");
    wr(*emu, BANK0_CTL, 0x1 | STATUS0);
    step(*emu, 1);
    PT_CHECK((emu->state.pending_irqs & ICU_BIT_TIMER) == 0, "IRQ dropped after W1C");
    delete emu;
    return true;
}

static bool test_next_timer_due_bounds_idle_skipping()
{
    BekenEmulator *emu = make_emu();
    u64 due = 0;
    PT_CHECK(!A::next_timer_due(*emu, emu->state, due), "next_timer_due is None initially");
    wr(*emu, BANK0_COUNTER, 26000);
    wr(*emu, BANK0_CTL, 0x1);
    PT_CHECK(A::next_timer_due(*emu, emu->state, due) && due == emu->state.insn_count + SECOND / 1000,
             "next_timer_due == insn_count + SECOND/1000");
    wr(*emu, BANK0_CTL, 0x0);
    PT_CHECK(!A::next_timer_due(*emu, emu->state, due), "next_timer_due is None after disable");
    delete emu;
    return true;
}

static bool test_other_chips_keep_the_fixed_tick()
{
    const char *names[3] = {"BK7231", "BK7231N", "BK7238"};
    for (int i = 0; i < 3; i++) {
        BekenEmulator *emu = make_emu(names[i]);
        step(*emu, SLOW_TICK_INSNS);
        PT_CHECK((emu->state.timer3_5_ctl & STATUS0) != 0, names[i]);
        delete emu;
    }
    BekenEmulator *emu = make_emu("BK7252");
    step(*emu, 10 * SLOW_TICK_INSNS);
    PT_CHECK((emu->state.timer3_5_ctl & (0x7u << 7)) == 0, "BK7252 needs an enabled timer");
    delete emu;
    return true;
}

int timers_tests()
{
    pt_checks = 0;
    struct T {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"test_period_follows_the_counter", test_period_follows_the_counter},
        {"test_one_second_gives_a_thousand_ticks", test_one_second_gives_a_thousand_ticks},
        {"test_divider_and_32k_bank", test_divider_and_32k_bank},
        {"test_timers_are_independent", test_timers_are_independent},
        {"test_disabled_timer_never_fires", test_disabled_timer_never_fires},
        {"test_missed_periods_coalesce_into_one_tick", test_missed_periods_coalesce_into_one_tick},
        {"test_acknowledged_status_drops_the_interrupt", test_acknowledged_status_drops_the_interrupt},
        {"test_next_timer_due_bounds_idle_skipping", test_next_timer_due_bounds_idle_skipping},
        {"test_other_chips_keep_the_fixed_tick", test_other_chips_keep_the_fixed_tick},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++) {
        if (!tests[i].fn())
            return -1;
        pt_pass(tests[i].name);
    }
    printf("All timer tests passed.\n");
    return pt_checks;
}
