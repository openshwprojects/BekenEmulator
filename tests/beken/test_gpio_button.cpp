/* Port of tests/gpio_button.py: real-firmware regression for host-driven GPIO
 * button input.
 *
 * This deliberately uses stock Tuya wall-switch images rather than a synthetic
 * guest. Their own hardware tables identify the active-low buttons and
 * active-high relays, so a successful press must travel through the emulated
 * GPIO input, each firmware's key scanner, and its relay-output code. The
 * BK7231Q case additionally proves that its channel-3 RTOS timer drives the
 * polling-based scanner.
 *
 * Mapping note: Python polls the live emu.state from the test thread (safe
 * under the GIL). Here every poll first parks the emulation thread at a chunk
 * boundary (PyTestAccess::park) so the std::maps are never read mid-update. */
#include "beken/pytests.h"
#include "beken/beken_crypto.h"

static int pt_checks;

typedef PyTestAccess A;

static const int BUTTON_PIN = 7;
static const int RELAY_PIN = 6;
static const u64 PRESS_INSNS = 1000000; /* 200 ms of guest time (5M insns == one second) */

struct ButtonCtx {
    Bytes tail;
    size_t keep;
    const char *marker;
    volatile long ready;
};
static void uart_sink(void *ctx, int uart, u8 byte)
{
    ButtonCtx *c = (ButtonCtx *)ctx;
    if (uart != 2)
        return;
    c->tail += (char)byte;
    if (c->tail.size() > c->keep)
        c->tail.erase(0, c->tail.size() - c->keep);
    if (c->tail.find(c->marker) != std::string::npos)
        atomic_set(&c->ready, 1);
}

static void run_emu(void *arg) { ((BekenEmulator *)arg)->run(); }

/* A predicate over the parked emulator. */
typedef bool (*Pred)(BekenEmulator &emu, void *arg);

static u64 parked_insns(BekenEmulator &emu, Thread &worker)
{
    A::park(emu, worker);
    u64 n = emu.state.insn_count;
    A::unpark(emu);
    return n;
}

/* Wait on live model state with both guest-time and wall-time bounds. */
static bool wait_for(BekenEmulator &emu, Thread &worker, Pred pred, void *arg, const std::string &description,
                     u64 max_insns, double max_seconds, std::string &failure)
{
    u64 start_insns = parked_insns(emu, worker);
    double deadline = mono_time() + max_seconds;
    for (;;) {
        bool parked = A::park(emu, worker);
        bool ok = parked && pred(emu, arg);
        u64 now_insns = emu.state.insn_count;
        A::unpark(emu);
        if (ok)
            return true;
        if (!parked || !worker.is_alive()) {
            failure = "emulator stopped before " + description;
            return false;
        }
        u64 elapsed = now_insns - start_insns;
        if (elapsed > max_insns) {
            failure = strf("%s not reached after %llu guest instructions", description.c_str(),
                           (unsigned long long)elapsed);
            return false;
        }
        if (mono_time() > deadline) {
            failure = strf("%s not reached within %.1f seconds", description.c_str(), max_seconds);
            return false;
        }
        sleep_ms(5);
    }
}

static bool cfg_is(BekenEmulator &emu, int slot, u32 value)
{
    std::map<int, u32>::const_iterator it = emu.state.gpio_cfg.find(slot);
    return it != emu.state.gpio_cfg.end() && it->second == value;
}
static bool pwm_is(BekenEmulator &emu, u32 reg, u32 value)
{
    std::map<u32, u32>::const_iterator it = emu.state.pwm_regs.find(reg);
    return it != emu.state.pwm_regs.end() && it->second == value;
}
static bool input_is(BekenEmulator &emu, int pin, int level)
{
    std::map<int, int>::const_iterator it = emu.state.gpio_hw.inputs.find(pin);
    return it != emu.state.gpio_hw.inputs.end() && it->second == level;
}

static bool p_ready(BekenEmulator &, void *arg) { return atomic_get(&((ButtonCtx *)arg)->ready) != 0; }
struct GuestTime {
    u64 start, insns;
};
static bool p_guest_time(BekenEmulator &emu, void *arg)
{
    GuestTime *g = (GuestTime *)arg;
    return emu.state.insn_count - g->start >= g->insns;
}
struct PinArg {
    int pin;
    int level; /* -1 = "not in inputs" */
};
static bool p_input(BekenEmulator &emu, void *arg)
{
    PinArg *p = (PinArg *)arg;
    if (p->level < 0)
        return emu.state.gpio_hw.inputs.find(p->pin) == emu.state.gpio_hw.inputs.end();
    return input_is(emu, p->pin, p->level);
}
struct CfgArg {
    int slot;
    u32 value;
};
static bool p_cfg(BekenEmulator &emu, void *arg) { return cfg_is(emu, ((CfgArg *)arg)->slot, ((CfgArg *)arg)->value); }
static bool p_t_setup(BekenEmulator &emu, void *)
{
    return cfg_is(emu, RELAY_PIN, 0x02) && cfg_is(emu, BUTTON_PIN, 0x3C);
}
static bool p_q_setup(BekenEmulator &emu, void *)
{
    return pwm_is(emu, 0, 0x7000) && pwm_is(emu, 0x20, 0x6590) && cfg_is(emu, 18, 0x02) && cfg_is(emu, 6, 0x3C);
}

static bool wait_guest_time(BekenEmulator &emu, Thread &worker, u64 insns, std::string &failure)
{
    GuestTime g;
    g.start = parked_insns(emu, worker);
    g.insns = insns;
    return wait_for(emu, worker, p_guest_time, &g, strf("%llu instructions of pressed guest time", (unsigned long long)insns),
                    insns + 1000000, 10.0, failure);
}

/* The press/release scenario shared by both firmwares. */
struct Scenario {
    const char *image;
    const char *key;      /* 0 = no -key */
    const char *marker;
    size_t keep;
    const char *ready_desc;
    u64 ready_insns;
    Pred setup_pred;
    const char *setup_desc;
    int button, relay;
    const char *press_desc, *release_desc, *toggle_desc, *pullup_fail;
};

static bool run_scenario(const Scenario &s)
{
    std::string image = pt_firmware(s.image);
    PT_CHECK(file_exists(image), "firmware image is missing: " + image);
    KeyCoefs key;
    std::string err;
    if (s.key)
        PT_CHECK(parse_key(s.key, key, &err), "parse_key: " + err);
    BuildOptions opt;
    opt.only_uart = true;
    BekenEmulator *emu = build_emulator(image, opt, s.key ? &key : 0, &err, 0, false);
    PT_CHECK(emu != 0, "build_emulator: " + err);
    ButtonCtx ctx;
    ctx.keep = s.keep;
    ctx.marker = s.marker;
    ctx.ready = 0;
    emu->uart_sink = uart_sink;
    emu->uart_sink_ctx = &ctx;
    emu->setup();
    Thread worker;
    worker.start(run_emu, emu);

    std::string failure;
    bool ok = false;
    int released_level = -1;
    do {
        if (!wait_for(*emu, worker, p_ready, &ctx, s.ready_desc, s.ready_insns, 45.0, failure))
            break;
        /* Confirm that it reached its real pin configuration before injecting
         * an input, and establish the relay's pre-press state. */
        if (!wait_for(*emu, worker, s.setup_pred, 0, s.setup_desc, 2000000, 5.0, failure))
            break;

        emu->set_gpio_input(s.button, 0);
        PinArg pressed = {s.button, 0};
        if (!wait_for(*emu, worker, p_input, &pressed, s.press_desc, 2000000, 5.0, failure))
            break;
        if (!wait_guest_time(*emu, worker, PRESS_INSNS, failure))
            break;

        /* Release (Python None) is physical release, not an artificial
         * drive-high. Waiting for the model to remove its override also
         * verifies that the pin falls back to the firmware's pull-up. */
        emu->set_gpio_input(s.button, -1);
        PinArg released = {s.button, -1};
        if (!wait_for(*emu, worker, p_input, &released, s.release_desc, 2000000, 5.0, failure))
            break;
        if (A::park(*emu, worker)) {
            released_level = emu->state.gpio_hw.effective_level(s.button, emu->state.gpio_cfg[s.button]);
            A::unpark(*emu);
        }
        if (released_level != 1) {
            failure = s.pullup_fail;
            break;
        }
        CfgArg relay_low = {s.relay, 0x00};
        if (!wait_for(*emu, worker, p_cfg, &relay_low, s.toggle_desc, 8000000, 15.0, failure))
            break;
        ok = true;
    } while (0);
    emu->stop();
    bool stopped = worker.join(5000);
    if (stopped)
        delete emu;
    PT_CHECK(ok, failure);
    PT_CHECK(stopped, "emulator worker did not stop");
    return true;
}

static bool test_stock_tuya_button_toggles_relay()
{
    /* The dump itself reports this map on UART:
     *   IO - relay[0]:  pin-6  type high
     *   IO - button[0]: pin-7  type low */
    Scenario s = {"BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin", "TUYA", "device_init ok", 256,
                  "the stock firmware's device_init checkpoint", 80000000, p_t_setup,
                  "P6 high relay and P7 pull-up button configuration", BUTTON_PIN, RELAY_PIN,
                  "the queued active-low press", "button release", "stock firmware toggling relay P6 low",
                  "released P7 did not return to its pull-up level"};
    return run_scenario(s);
}

static bool test_moes_q_button_toggles_relay()
{
    /* Prove the Q SDK's channel-3 RTOS timer services its polled key. This
     * SDK puts its FreeRTOS tick on old-PWM channel 3, not channel 0: nibble
     * 3 of PWM_CTL is 0b0111 (enabled, interrupt enabled, timer mode), and
     * PWM3's period is 0x6590. The button is deliberately polling-driven by a
     * 30 ms Tuya timer, so its relay response is also an end-to-end
     * regression for dispatching the correct PWM status bit. */
    Scenario s = {"BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin", 0, "Initializing TCP/IP stack", 512,
                  "the MOES firmware's TCP/IP checkpoint", 60000000, p_q_setup,
                  "MOES channel-3 OS timer, P18 relay, and P6 button setup", 6, 18,
                  "the MOES active-low press", "MOES button release", "MOES firmware toggling relay P18 low",
                  "released MOES P6 did not return to its pull-up level"};
    return run_scenario(s);
}

int gpio_button_tests()
{
    pt_checks = 0;
    printf("== stock Tuya GPIO button integration ==\n");
    if (!test_stock_tuya_button_toggles_relay())
        return -1;
    pt_pass("active-low P7 press toggles active-high relay P6");
    if (!test_moes_q_button_toggles_relay())
        return -1;
    pt_pass("MOES Q active-low P6 press toggles active-high relay P18");
    printf("\nGPIO button firmware regression passed.\n");
    return pt_checks;
}
