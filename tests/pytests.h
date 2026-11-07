/* C++ ports of the Python unit tests in ../tests (the ones that are not firmware
 * self-test cases: gpio_button.py, gpio_emulator.py, image_loader.py and
 * watchdog.py. Each suite runs the same scenarios with the same inputs and
 * assertions as its Python file, prints "  [PASS] <test>" per test, and
 * returns the number of checks it made (< 0 on a failure).
 *
 * PyTestAccess is the one friend of BekenEmulator / netstack::NetStack that
 * these tests use to reach what Python reads as plain attributes (emu.state,
 * emu._gpio_pin_at, emu.wifi_peer, net.conns, ...). */
#ifndef BK_PYTESTS_H
#define BK_PYTESTS_H

#include "common.h"
#include "platform.h"
#include "emulator.h"
#include "netstack.h"

#include <stdio.h>

int gpio_button_tests();
int gpio_emulator_tests();
int image_loader_tests();
int watchdog_tests();
int chips_tests();
int timers_tests();

struct PyTestAccess {
    typedef BekenEmulator E;
    /* GPIO glue */
    static int gpio_pin_at(E &e, u32 address) { return e.gpio_pin_at(address); }
    static void apply_gpio_events(E &e) { e.apply_gpio_events(); }
    static void sync_gpio_irq(E &e) { e.sync_gpio_irq(); }
    static u32 old_pwm_timer_status_mask(E &e, u32 ctl) { return e.old_pwm_timer_status_mask(ctl); }
    static void restore_legacy_gpio_state(E &e) { e.restore_legacy_gpio_state(); }
    static size_t queued_gpio_events(E &e)
    {
        ScopedLock lk(e.gpio_lock_);
        return e.gpio_events_.size();
    }
    /* snapshots */
    static void save_snapshot(E &e, const std::string &path) { e.save_snapshot(path); }
    /* exceptions & resets */
    static void trigger_irq(E &e) { e.trigger_irq(); }
    static void trigger_fiq(E &e) { e.trigger_fiq(); }
    static void watchdog_reset(E &e) { e.watchdog_reset(); }
    /* flash, gdma, timers & pacing */
    static void flash_erase(E &e, u32 offset, u32 size) { e.flash_erase(offset, size); }
    static void flash_program(E &e, u32 offset, const u32 *words, int n)
    {
        std::vector<u32> v(words, words + n);
        e.flash_program(offset, v);
    }
    static int gdma_channel(E &e, u32 offset) { return e.gdma_channel(offset); }
    static void pacing(E &e) { e.pacing(); }
    static bool next_timer_due(E &e, const SimState &state, u64 &due)
    {
        due = e.next_timer_due(state);
        return due != 0;
    }
    /* image layout */
    static const Bytes &raw_flash(E &e) { return e.raw_flash_; }
    static const Bytes &bootloader(E &e) { return e.bootloader_; }
    static const Bytes &app(E &e) { return e.app_; }
    static const Bytes &physical_flash(E &e) { return e.physical_flash_; }
    static bool have_physical(E &e) { return e.have_physical_; }
    static u32 exception_vector_base(E &e) { return e.exception_vector_base_; }
    /* optional models */
    static BleCore *blecore(E &e) { return e.blecore_; }
    static MacHw *wifimac(E &e) { return e.wifimac_; }
    static WifiPeer *wifi_peer(E &e) { return e.wifi_peer_; }
    static u64 &network_insn_offset(E &e) { return e.network_insn_offset_; }

    /* Park the emulation thread between chunks so its state may be read
     * (Python reads it live under the GIL; a C++ map must not be read while
     * the worker may be inserting into it). pause() holds the worker at its
     * next pacing boundary; a ui_view request is answered at that same
     * boundary just before it parks, so once the request is cleared the
     * worker is parked. Returns false if the worker finished instead. */
    static bool park(E &e, Thread &worker)
    {
        e.pause(true);
        atomic_set(&e.ui_view_req_, 1);
        while (atomic_get(&e.ui_view_req_)) {
            if (!worker.is_alive())
                return false;
            sleep_ms(1);
        }
        return true;
    }
    static void unpark(E &e) { e.pause(false); }

    /* netstack::NetStack internals that tests/watchdog.py primes and reads */
    typedef netstack::NetStack N;
    static bool &link(N &n) { return n.link_; }
    static bool &link_up_valid(N &n) { return n.link_up_valid_; }
    static u64 &link_up_us(N &n) { return n.link_up_us_; }
    static Bytes &device_mac(N &n) { return n.device_mac_; }
    static Bytes &device_ip(N &n) { return n.device_ip_; }
    static size_t conn_count(N &n) { return n.conns_.size(); }
    static const std::vector<sock_t> &listeners(N &n) { return n.listeners_; }
    static const void *io(N &n) { return n.io_; }
    /* the stack's own connection constructor (device -> remote SYN) */
    static void tcp_new(N &n, const Bytes &src, int sport, const Bytes &dst, int dport)
    {
        n.tcp_new(src, sport, dst, dport, 1000, 65535);
    }
};

/* ---- shared check helpers (per translation unit) ---- */
#define PT_CHECK(cond, what)                                                              \
    do {                                                                                  \
        pt_checks++;                                                                      \
        if (!(cond)) {                                                                    \
            printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, std::string(what).c_str()); \
            fflush(stdout);                                                               \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

static inline void pt_pass(const char *name)
{
    out_flush();
    printf("  [PASS] %s\n", name);
    fflush(stdout);
}

/* firmwares/<name> from the repo root, wherever the test is started. */
static inline std::string pt_firmware(const std::string &name)
{
    const char *roots[] = {"firmwares", "../firmwares", "../../firmwares", "../../../firmwares"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        std::string p = path_join(roots[i], name);
        if (file_exists(p))
            return p;
    }
    return path_join("../firmwares", name);
}

static inline ChipId pt_chip(const char *name)
{
    ChipId c;
    c.chip = c.device = 0;
    chip_family(name, c);
    return c;
}

#endif
