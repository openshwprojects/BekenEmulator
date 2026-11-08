/* Port of tests/gpio_emulator.py: fast integration tests for GPIO injection
 * and emulator register routing.
 *
 * Mapping notes. The Python file builds a BekenEmulator without Unicorn and
 * drives hook_mem_write_mmio / hook_mem_read_mmio against a FakeMemory. Here
 * a real emulator is set up (no firmware runs) and the same accesses go
 * through the hooked bus (Memory::bus_write / bus_read), i.e. exactly the
 * path a guest load/store takes. pickle round-trips of the state become the
 * C++ snapshot round trip (save_snapshot -> load_snapshot), and the legacy
 * pre-GpioModel snapshot is a C++ snapshot without its state section. */
#include "beken/pytests.h"
#include "beken/periph.h"
#include "beken/wifimac.h"
#include "wifipeer.h"

static int pt_checks;

static const u32 GPIO_BASE = 0x00802800;
static const u32 GPIO_FUNC_CFG = GPIO_BASE + 32 * 4;
static const u32 GPIO_INTEN = GPIO_BASE + 33 * 4;
static const u32 GPIO_INTLV0 = GPIO_BASE + 34 * 4;
static const u32 GPIO_INTSTA = GPIO_BASE + 36 * 4;
static const u32 GPIO_INTEN2 = GPIO_BASE + 56 * 4;
static const u32 GPIO_INTLV3 = GPIO_BASE + 57 * 4;
static const u32 GPIO_INTSTA2 = GPIO_BASE + 58 * 4;
static const u32 GPIO_EXT_PIN_END = GPIO_BASE + (48 + 8) * 4;
static const int GPIO_EXT_SLOT_FIRST = 48;
static const int GPIO_EXT_PIN_FIRST = 32;

typedef PyTestAccess A;

static void quiet_sink(void *, const std::string &) {}

/* make_emulator(extended): BK7231 (32 pins) or BK7252 (40 pins, second bank). */
static BekenEmulator *make_emulator(bool extended = false, bool wifi = false)
{
    BuildOptions opt;
    opt.chip = extended ? "BK7252" : "BK7231";
    opt.only_uart = true;
    opt.wifi = wifi;
    BekenEmulator *emu = new BekenEmulator(Bytes(0x1000, (char)0xFF), Bytes(), Bytes(), opt,
                                           pt_chip(opt.chip.c_str()), 0);
    emu->net_sink = quiet_sink;
    emu->setup();
    return emu;
}

static void wr(BekenEmulator &emu, u32 address, u32 value) { emu.memory().bus_write(address, 4, value); }
static u32 rd(BekenEmulator &emu, u32 address)
{
    u32 v = 0;
    emu.memory().bus_read(address, 4, v);
    return v;
}

static bool test_pin_routing_and_queued_drive()
{
    BekenEmulator *e = make_emulator();
    BekenEmulator &emu = *e;
    u32 pull_up = 0x3C;
    emu.state.gpio_cfg[9] = pull_up;

    PT_CHECK(A::gpio_pin_at(emu, GPIO_BASE + 9 * 4) == 9, "P9 config word routes to pin 9");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_BASE + 31 * 4) == 31, "P31 config word routes to pin 31");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_FUNC_CFG) == -1, "FUNC_CFG is not a pin");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_INTEN) == -1, "INTEN is not a pin");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_EXT_PIN_END) == -1, "EXT_PIN_END is not a pin");

    emu.set_gpio_input(9, 0);
    PT_CHECK(emu.state.gpio_hw.inputs.empty(), "host calls must only enqueue");
    A::apply_gpio_events(emu);
    PT_CHECK(emu.state.gpio_hw.inputs.size() == 1 && emu.state.gpio_hw.inputs[9] == 0, "inputs == {9: 0}");
    PT_CHECK(!(emu.state.gpio_hw.read_config(9, pull_up) & 1), "driven-low P9 reads 0");

    emu.set_gpio_input(9, -1);
    A::apply_gpio_events(emu);
    PT_CHECK(emu.state.gpio_hw.inputs.empty(), "release empties inputs");
    PT_CHECK(emu.state.gpio_hw.read_config(9, pull_up) & 1, "released P9 reads its pull-up");

    /* Python: set_gpio_input(32, 0) raises ValueError on a 32-pin chip. */
    PT_CHECK(!emu.set_gpio_input(32, 0), "a controller-register slot must not be injectable as GPIO32");
    PT_CHECK(A::queued_gpio_events(emu) == 0, "the rejected GPIO32 drive queued nothing");
    delete e;
    return true;
}

static bool test_old_pwm_timer_status_uses_configured_channel()
{
    /* One nibble per channel: enable, interrupt-enable, then mode=timer (1).
     * The Q SDK's 0x7000 therefore names channel 3, and must never synthesize
     * the channel-0 status bit that has no handler in that firmware. */
    BekenEmulator *e = make_emulator();
    u32 status = A::old_pwm_timer_status_mask(*e, 0x7000);
    PT_CHECK(status == 1u << 3, "0x7000 -> channel 3 status");
    PT_CHECK(!(status & 1), "no channel-0 status bit");
    PT_CHECK(A::old_pwm_timer_status_mask(*e, 0x0007) == 1, "0x0007 -> channel 0");
    PT_CHECK(A::old_pwm_timer_status_mask(*e, 0x0001) == 0, "0x0001 is a PWM output, not a timer");
    delete e;
    return true;
}

static bool test_interrupt_register_hooks_and_icu_line()
{
    BekenEmulator *e = make_emulator();
    BekenEmulator &emu = *e;
    int pin = 6;
    u32 bit = 1u << pin;
    u32 pull_up = 0x3C;
    emu.state.gpio_cfg[pin] = pull_up;

    /* Mode 3 is falling edge. INTLV and INTEN are controller registers, not
     * extra GPIO config words, even though they share the same MMIO window. */
    wr(emu, GPIO_INTLV0, 3u << (2 * pin));
    wr(emu, GPIO_INTEN, bit);
    PT_CHECK(emu.state.gpio_cfg.size() == 1 && emu.state.gpio_cfg.count(pin), "gpio_cfg keys == {6}");
    PT_CHECK(rd(emu, GPIO_INTLV0) == 3u << (2 * pin), "INTLV0 reads back");
    PT_CHECK(rd(emu, GPIO_INTEN) == bit, "INTEN reads back");

    emu.set_gpio_input(pin, 0);
    A::apply_gpio_events(emu);
    A::sync_gpio_irq(emu);
    PT_CHECK(rd(emu, GPIO_INTSTA) == bit, "falling edge latched in INTSTA");
    PT_CHECK(emu.state.pending_irqs & (1u << 7), "GPIO must drive ICU IRQ bit 7");

    /* INTSTA is W1C. A held edge does not re-latch until release + press. */
    wr(emu, GPIO_INTSTA, bit);
    PT_CHECK(rd(emu, GPIO_INTSTA) == 0, "W1C clears INTSTA");
    PT_CHECK(!(emu.state.pending_irqs & (1u << 7)), "ICU line drops with the status");
    emu.set_gpio_input(pin, -1);
    emu.set_gpio_input(pin, 0);
    A::apply_gpio_events(emu);
    A::sync_gpio_irq(emu);
    PT_CHECK(rd(emu, GPIO_INTSTA) == bit, "release + press latches again");
    wr(emu, GPIO_INTSTA, bit);
    delete e;

    /* Level interrupt semantics are visible immediately through MMIO. The
     * guest need not wait for the emulator's next pacing chunk after enabling
     * a source or clearing a still-active level. */
    e = make_emulator();
    BekenEmulator &emu2 = *e;
    int high_pin = 7;
    u32 high_bit = 1u << high_pin;
    emu2.state.gpio_cfg[high_pin] = pull_up;
    wr(emu2, GPIO_INTLV0, 1u << (2 * high_pin)); /* high level */
    wr(emu2, GPIO_INTEN, high_bit);
    PT_CHECK(rd(emu2, GPIO_INTSTA) == high_bit, "enabled high level latches at once");
    PT_CHECK(emu2.state.pending_irqs & (1u << 7), "and raises ICU bit 7");
    wr(emu2, GPIO_INTSTA, high_bit);
    PT_CHECK(rd(emu2, GPIO_INTSTA) == high_bit, "held level must reassert now");
    delete e;
    return true;
}

/* Bytes of a save_snapshot blob before its state section (what a snapshot
 * written before the GPIO model / state serializer contained). */
static size_t snapshot_prefix_size(BekenEmulator &emu)
{
    std::vector<std::pair<u32, u32> > regs;
    emu.memory().regions(regs);
    size_t n = 8 + sizeof(CpuContext) + 8 + 4;
    for (size_t i = 0; i < regs.size(); i++)
        n += 8 + regs[i].second;
    return n;
}

static bool test_legacy_extended_snapshot_migration()
{
    BekenEmulator *e = make_emulator(true);
    BekenEmulator &emu = *e;
    int pin = 39;
    int slot = GPIO_EXT_SLOT_FIRST + pin - GPIO_EXT_PIN_FIRST;
    emu.memory().phys_write(GPIO_BASE + (u32)slot * 4, 4, 0x3C);
    emu.memory().phys_write(GPIO_INTEN2, 4, 1u << 7);
    emu.memory().phys_write(GPIO_INTLV3, 4, 1u << (2 * 7));
    A::restore_legacy_gpio_state(emu);

    PT_CHECK(emu.state.gpio_cfg.count(slot) && emu.state.gpio_cfg[slot] == 0x3C, "slot 55 config recovered");
    PT_CHECK(emu.state.gpio_hw.pin_count == 40, "40-pin model");
    PT_CHECK(emu.state.gpio_hw.int_enable == 1ULL << pin, "INTEN2 bit 7 -> P39 enabled");
    PT_CHECK(emu.state.gpio_hw.int_modes[pin] == 1, "INTLV3 -> P39 high level");
    A::sync_gpio_irq(emu);
    PT_CHECK(emu.state.gpio_hw.int_status == 1ULL << pin, "level latched after sync");
    PT_CHECK(emu.state.pending_irqs & (1u << 7), "ICU bit 7 raised");

    /* The same through load_snapshot: a C++ snapshot without its state
     * section is the legacy format; loading it must take this path. */
    std::string path = make_temp_file("pytests_legacy_", ".snap");
    A::save_snapshot(emu, path);
    Bytes blob;
    read_file(path, blob);
    size_t prefix = snapshot_prefix_size(emu);
    PT_CHECK(blob.size() > prefix, "snapshot has a state section to strip");
    write_file(path, blob.substr(0, prefix));
    BekenEmulator *f = make_emulator(true);
    std::string err;
    bool loaded = f->load_snapshot(path, &err);
    remove_file(path);
    PT_CHECK(loaded, "legacy snapshot loads: " + err);
    PT_CHECK(f->state.gpio_cfg.count(slot) && f->state.gpio_cfg[slot] == 0x3C, "load: slot 55 config recovered");
    PT_CHECK(f->state.gpio_hw.pin_count == 40 && f->state.gpio_hw.int_enable == 1ULL << pin &&
                 f->state.gpio_hw.int_modes[pin] == 1,
             "load: P39 enable + level mode recovered");
    A::sync_gpio_irq(*f);
    PT_CHECK(f->state.gpio_hw.int_status == 1ULL << pin && (f->state.pending_irqs & (1u << 7)),
             "load: level interrupt latches");
    delete f;
    delete e;
    return true;
}

static bool test_extended_peripheral_decode_uses_logical_pins()
{
    std::map<int, u32> raw;
    raw[32] = 0x11111111; /* FUNC_CFG, not logical P32 */
    raw[46] = 0x22222222;
    raw[47] = 0x33333333;
    raw[48] = 0x3C;       /* logical P32 */
    raw[55] = 0x2C;       /* logical P39 */
    const char *variants[] = {"BK7231U", "BK7252"};
    for (int v = 0; v < 2; v++) {
        std::map<u32, u32> pwm;
        periph::DecodedState d = periph::decode_state(raw, pwm, variants[v], 0, 0, 0, 0, 0, 0, 0);
        std::map<int, u32> want;
        want[32] = 0x3C;
        want[39] = 0x2C;
        PT_CHECK(d.raw == want, strf("%s: raw == {32: 0x3C, 39: 0x2C}", variants[v]));
        PT_CHECK(d.gpio.size() == 40, strf("%s: 40 GPIO rows", variants[v]));
        std::map<std::string, std::string> func(d.func.begin(), d.func.end());
        std::map<std::string, std::string> want_func;
        want_func["REG_GPIO_FUNC_CFG"] = "0x11111111";
        want_func["REG_GPIO_FUNC_CFG_2"] = "0x22222222";
        want_func["REG_GPIO_FUNC_CFG_3"] = "0x33333333";
        PT_CHECK(func == want_func && d.func.size() == 3, strf("%s: three function registers", variants[v]));
    }
    return true;
}

static bool test_extended_bank_and_snapshot_payload()
{
    BekenEmulator *e = make_emulator(true);
    BekenEmulator &emu = *e;
    PT_CHECK(A::gpio_pin_at(emu, GPIO_BASE + 48 * 4) == 32, "slot 48 is P32");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_BASE + 55 * 4) == 39, "slot 55 is P39");
    PT_CHECK(A::gpio_pin_at(emu, GPIO_INTEN2) == -1, "INTEN2 is not a pin");

    int pin = 39;
    u64 bit = 1ULL << pin;
    emu.state.gpio_cfg[55] = 0x2C; /* pull-down at physical slot 55 */
    wr(emu, GPIO_INTLV3, 2u << (2 * 7)); /* rising edge */
    wr(emu, GPIO_INTEN2, 1u << 7);
    emu.set_gpio_input(pin, 1);
    A::apply_gpio_events(emu);
    A::sync_gpio_irq(emu);
    PT_CHECK(emu.state.gpio_hw.int_status == bit, "P39 rising edge latched");
    PT_CHECK(rd(emu, GPIO_INTSTA2) == 1u << 7, "INTSTA2 bit 7");

    /* The live model has no locks/callbacks and remains snapshot-safe
     * (Python: pickle round trip of emu.state). */
    std::string path = make_temp_file("pytests_gpio_", ".snap");
    A::save_snapshot(emu, path);
    BekenEmulator *f = make_emulator(true);
    std::string err;
    bool loaded = f->load_snapshot(path, &err);
    remove_file(path);
    PT_CHECK(loaded, "snapshot loads: " + err);
    PT_CHECK(f->state.gpio_hw.pin_count == 40, "restored pin_count == 40");
    PT_CHECK(f->state.gpio_hw.inputs.size() == 1 && f->state.gpio_hw.inputs[pin] == 1, "restored inputs == {39: 1}");
    PT_CHECK(f->state.gpio_hw.int_status == bit, "restored int_status");
    delete f;
    delete e;
    return true;
}

static bool test_close_network_detaches_peer_callback()
{
    /* Python fakes the peer and the MAC; here a real peer is attached and
     * closed. The C++ close_network() also frees the peer, so "peer.closed"
     * is covered by wifi_peer == 0 (its close() runs before the delete). */
    BekenEmulator *e = make_emulator(false, true);
    std::vector<std::pair<int, int> > forwards;
    WifiPeer *peer = e->attach_wifi_peer("ap", "HomeNet", 1, Bytes(), "", forwards, false, "", StrList(),
                                         std::vector<std::pair<std::string, Bytes> >(), std::vector<Bytes>(),
                                         false);
    PT_CHECK(peer && A::wifi_peer(*e) == peer, "peer attached");
    PT_CHECK(A::wifimac(*e)->on_tx != 0, "MAC transmits to the peer");
    e->close_network();
    PT_CHECK(A::wifi_peer(*e) == 0, "wifi_peer is None after close_network");
    PT_CHECK(A::wifimac(*e)->on_tx == 0, "closed peer callback still retains the network stack");
    delete e;
    return true;
}

int gpio_emulator_tests()
{
    pt_checks = 0;
    struct T {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"test_pin_routing_and_queued_drive", test_pin_routing_and_queued_drive},
        {"test_old_pwm_timer_status_uses_configured_channel", test_old_pwm_timer_status_uses_configured_channel},
        {"test_interrupt_register_hooks_and_icu_line", test_interrupt_register_hooks_and_icu_line},
        {"test_extended_bank_and_snapshot_payload", test_extended_bank_and_snapshot_payload},
        {"test_legacy_extended_snapshot_migration", test_legacy_extended_snapshot_migration},
        {"test_extended_peripheral_decode_uses_logical_pins", test_extended_peripheral_decode_uses_logical_pins},
        {"test_close_network_detaches_peer_callback", test_close_network_detaches_peer_callback},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++) {
        if (!tests[i].fn())
            return -1;
        pt_pass(tests[i].name);
    }
    printf("\nAll %d GPIO/emulator integration tests passed.\n", n);
    return pt_checks;
}
