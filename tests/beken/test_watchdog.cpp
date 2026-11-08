/* Port of tests/watchdog.py: watchdog register and full-chip reboot tests.
 *
 * Mapping notes. Register writes go through the hooked bus like a guest
 * store (Python calls hook_mem_write_mmio). Python's "wdt_deadline is None"
 * is wdt_deadline_valid == false. The network test primes the peer and the
 * IP stack with the same stale per-boot state Python assigns directly
 * (peer.stations/auth/_outq through WifiPeer's test-support calls, the
 * stack's link/device fields and a TCP connection made by the stack's own
 * tcp_new); "io thread alive" is the stack still owning the same HostIO. */
#include "beken/pytests.h"
#include "wifipeer.h"
#include "netstack.h"

static int pt_checks;

typedef PyTestAccess A;

static const u32 ICU_PERI_CLK_PWD = 0x00802008;
static const u32 ICU_WDT_CLK_PWD = 1u << 8;
static const u32 WDT_CTRL = 0x00802900;
static const u32 WDT_FIRST_KEY = 0x5A;
static const u32 WDT_SECOND_KEY = 0xA5;
static const u64 WDT_INSNS_PER_MS = SLOW_TICK_INSNS / SLOW_TICK_MS;

static void quiet_sink(void *, const std::string &) {}

static BekenEmulator *make_emulator(const Bytes &app = Bytes(), bool wifi = false)
{
    /* Only enough flash for the application window and SPI mirror is needed. */
    BuildOptions opt;
    opt.only_uart = true;
    opt.wifi = wifi;
    BekenEmulator *emu = new BekenEmulator(Bytes(0x20000, (char)0xFF), Bytes(), app, opt, pt_chip("BK7231"), 0);
    emu->setup();
    return emu;
}

static void wr(BekenEmulator &emu, u32 address, u32 value) { emu.memory().bus_write(address, 4, value); }

static bool test_key_sequence_and_clock_gate()
{
    BekenEmulator *e = make_emulator();
    BekenEmulator &emu = *e;
    emu.net_sink = quiet_sink;
    SimState &state = emu.state;

    /* A period programmed while the ICU has gated the watchdog clock is
     * retained, but cannot count until that clock is powered again. */
    wr(emu, ICU_PERI_CLK_PWD, ICU_WDT_CLK_PWD);
    wr(emu, WDT_CTRL, WDT_FIRST_KEY << 16 | 5);
    wr(emu, WDT_CTRL, WDT_SECOND_KEY << 16 | 5);
    PT_CHECK(emu.state.wdt_period_ms == 5, "gated period retained");
    PT_CHECK(!state.wdt_deadline_valid, "gated watchdog has no deadline");

    wr(emu, ICU_PERI_CLK_PWD, 0);
    PT_CHECK(state.wdt_deadline_valid && state.wdt_deadline == 5 * WDT_INSNS_PER_MS, "ungating arms 5 ms");

    /* A mismatched second key neither changes nor reloads an active watchdog. */
    u64 original = state.wdt_deadline;
    state.insn_count = 100;
    wr(emu, WDT_CTRL, WDT_FIRST_KEY << 16 | 9);
    wr(emu, WDT_CTRL, WDT_SECOND_KEY << 16 | 8);
    PT_CHECK(state.wdt_period_ms == 5, "mismatched pair keeps the period");
    PT_CHECK(state.wdt_deadline_valid && state.wdt_deadline == original, "mismatched pair does not reload");

    /* A complete pair is also the SDK's feed/reload operation. */
    wr(emu, WDT_CTRL, WDT_FIRST_KEY << 16 | 5);
    wr(emu, WDT_CTRL, WDT_SECOND_KEY << 16 | 5);
    PT_CHECK(state.wdt_deadline_valid && state.wdt_deadline == 100 + 5 * WDT_INSNS_PER_MS, "a full pair feeds");
    delete e;
    return true;
}

/* ARM code matching bk_reboot(): gate, program 5 ms, ungate, spin. */
static Bytes watchdog_firmware()
{
    static const u32 words[] = {
        0xE59F0024, /* ldr r0, [pc, #36]  ; ICU_PERI_CLK_PWD */
        0xE59F1024, /* ldr r1, [pc, #36]  ; bit 8 */
        0xE5801000, /* str r1, [r0]       ; watchdog power down */
        0xE59F2020, /* ldr r2, [pc, #32]  ; WDT_CTRL */
        0xE59F3020, /* ldr r3, [pc, #32]  ; first key + 5 ms */
        0xE5823000, /* str r3, [r2] */
        0xE59F301C, /* ldr r3, [pc, #28]  ; second key + 5 ms */
        0xE5823000, /* str r3, [r2] */
        0xE3A01000, /* mov r1, #0 */
        0xE5801000, /* str r1, [r0]       ; watchdog power up */
        0xEAFFFFFE, /* b .                 ; wait for hardware reset */
        0x00802008, 0x00000100, 0x00802900, 0x005A0005, 0x00A50005,
    };
    Bytes out;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        out += p32le(words[i]);
    return out;
}

static void run_emu(void *arg) { ((BekenEmulator *)arg)->run(); }

struct ExpiryCtx {
    BekenEmulator *emu;
    StrList messages;
};
static void expiry_logged(void *ctx, const std::string &message)
{
    ExpiryCtx *c = (ExpiryCtx *)ctx;
    c->messages.push_back(message);
    /* A second expiry proves that the first one did not merely stop the CPU:
     * the application restarted and programmed the watchdog again. */
    if (c->emu->reset_count == 2)
        c->emu->stop();
}

static bool test_expiry_reboots_and_restarts_firmware()
{
    BekenEmulator *emu = make_emulator(watchdog_firmware());
    ExpiryCtx ctx;
    ctx.emu = emu;

    /* An external button remains physically held through a chip reset. */
    emu->set_gpio_input(14, 0);

    emu->net_sink = expiry_logged;
    emu->net_sink_ctx = &ctx;
    Thread thread;
    thread.start(run_emu, emu);
    if (!thread.join(5000)) {
        emu->stop();
        thread.join(1000);
        PT_CHECK(false, "watchdog firmware did not reset within five seconds");
    }

    PT_CHECK(!emu->has_run_error, "run_error is None: " + emu->run_error);
    PT_CHECK(emu->reset_count == 2, "reset_count == 2");
    int expired = 0;
    for (size_t i = 0; i < ctx.messages.size(); i++)
        if (ctx.messages[i].compare(0, 22, "[WDT] watchdog expired") == 0)
            expired++;
    PT_CHECK(expired == 2, "two '[WDT] watchdog expired' lines");
    PT_CHECK(emu->state.gpio_hw.inputs.size() == 1 && emu->state.gpio_hw.inputs[14] == 0,
             "the held P14 input survives both resets");
    delete emu;
    return true;
}

struct NetCtx {
    BekenEmulator *emu;
    WifiPeer *peer;
    netstack::NetStack *net;
    sock_t listener;
    const void *io;
    bool observed;
    u64 now_us;
    size_t stations, auth, queued, conns;
    bool link, listener_alive, io_alive;
};
static void net_logged(void *ctx, const std::string &)
{
    NetCtx *c = (NetCtx *)ctx;
    if (c->emu->reset_count == 1 && !c->observed) {
        c->observed = true;
        c->now_us = c->peer->now_us();
        c->stations = c->peer->station_count();
        c->auth = c->peer->auth_count();
        c->queued = c->peer->outq_count();
        c->conns = A::conn_count(*c->net);
        c->link = A::link(*c->net);
        const std::vector<sock_t> &ls = A::listeners(*c->net);
        c->listener_alive = false;
        for (size_t i = 0; i < ls.size(); i++)
            if (ls[i] == c->listener)
                c->listener_alive = true;
        c->io_alive = A::io(*c->net) != 0 && A::io(*c->net) == c->io;
        c->emu->stop();
    }
}

static bool test_network_peer_survives_without_stale_device_sessions()
{
    BekenEmulator *emu = make_emulator(watchdog_firmware(), true);
    std::vector<std::pair<int, int> > forwards;
    forwards.push_back(std::make_pair(0, 80));
    NetCtx ctx;
    ctx.emu = emu;
    ctx.observed = false;
    emu->net_sink = quiet_sink; /* the attach logs "[NET] forwarding ..." */
    WifiPeer *peer = emu->attach_wifi_peer("ap", "HomeNet", 1, Bytes(), "", forwards, false, "", StrList(),
                                           std::vector<std::pair<std::string, Bytes> >(), std::vector<Bytes>(),
                                           false);
    PT_CHECK(peer && peer->net, "AP peer with an IP stack attached");
    netstack::NetStack *net = peer->net;
    PT_CHECK(!A::listeners(*net).empty(), "the forward is listening");
    ctx.peer = peer;
    ctx.net = net;
    ctx.listener = A::listeners(*net)[0];
    ctx.io = A::io(*net);

    /* Prime state that belongs to the old boot. A large external clock makes
     * a rollback obvious: beacon/TCP deadlines must not wait for that old time
     * to be reached again after the on-chip instruction counter restarts. */
    A::network_insn_offset(*emu) = 50000000;
    u64 before_us = peer->now_us();
    Bytes device_mac("\xc8\x47\x8c\x00\x00\x01", 6);
    Bytes device_ip = netstack::str2ip("192.168.1.100");
    Bytes remote_ip = netstack::str2ip("10.200.0.1");
    peer->prime_session_for_test(device_mac, before_us + 1000000);
    A::link(*net) = true;
    A::link_up_valid(*net) = true;
    A::link_up_us(*net) = before_us - 1000000;
    A::device_mac(*net) = device_mac;
    A::device_ip(*net) = device_ip;
    A::tcp_new(*net, device_ip, 49152, remote_ip, 443);
    PT_CHECK(peer->station_count() == 1 && peer->auth_count() == 1 && peer->outq_count() >= 1 &&
                 A::conn_count(*net) == 1,
             "stale per-boot state primed");

    emu->net_sink = net_logged;
    emu->net_sink_ctx = &ctx;
    Thread thread;
    thread.start(run_emu, emu);
    if (!thread.join(5000)) {
        emu->stop();
        thread.join(1000);
        emu->close_network();
        PT_CHECK(false, "networked watchdog firmware did not reset");
    }

    PT_CHECK(ctx.observed, "observed the first reset");
    PT_CHECK(ctx.now_us >= before_us, "network time does not roll back");
    PT_CHECK(ctx.stations == 0, "stations == {}");
    PT_CHECK(ctx.auth == 0, "auth == {}");
    PT_CHECK(ctx.queued == 0, "queued == []");
    PT_CHECK(ctx.conns == 0, "conns == {}");
    PT_CHECK(!ctx.link, "link is False");
    PT_CHECK(ctx.listener_alive, "the host forward listener survives");
    PT_CHECK(ctx.io_alive, "the host IO thread survives");
    emu->close_network();
    delete emu;
    return true;
}

int watchdog_tests()
{
    pt_checks = 0;
    struct T {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"test_key_sequence_and_clock_gate", test_key_sequence_and_clock_gate},
        {"test_expiry_reboots_and_restarts_firmware", test_expiry_reboots_and_restarts_firmware},
        {"test_network_peer_survives_without_stale_device_sessions",
         test_network_peer_survives_without_stale_device_sessions},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++) {
        if (!tests[i].fn())
            return -1;
        pt_pass(tests[i].name);
    }
    printf("Watchdog register and reboot self-tests passed.\n");
    return pt_checks;
}
