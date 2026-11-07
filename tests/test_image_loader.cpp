/* Port of tests/image_loader.py: firmware-image layout detection and mapping.
 *
 * Mapping notes. Python's _build_from_bytes patches builtins.open with
 * mock_open; the C++ build_emulator reads a path, so the synthetic image is
 * written to a temporary file instead. Python's ValueError from
 * build_emulator is the C++ null return with the message in *err. The
 * manually injected SWI (emu.hook_intr(mu, 2)) is the C++ CPU's own SWI
 * entry: one SVC instruction is executed at the same PC. */
#include "pytests.h"
#include "beken_crypto.h"
#include "blecore.h"

#include <string.h>

static int pt_checks;

typedef PyTestAccess A;

/* Bit-for-bit reference for the table-driven production implementation. */
static u16 slow_crc16(const Bytes &data, u16 initial)
{
    u32 reg = initial;
    for (size_t i = 0; i < data.size(); i++) {
        u8 octet = (u8)data[i];
        for (int bit = 0; bit < 8; bit++) {
            u32 top = reg & 0x8000;
            if (octet & (0x80 >> bit))
                top ^= 0x8000;
            reg <<= 1;
            if (top)
                reg ^= 0x8005;
            reg &= 0xFFFF;
        }
    }
    return (u16)reg;
}

static Bytes crc_encode(const Bytes &data)
{
    Bytes encoded;
    for (size_t off = 0; off + 32 <= data.size(); off += 32) {
        Bytes block = data.substr(off, 32);
        encoded += block;
        u16 c = crc16(block, 0xFFFF);
        encoded += (char)(c >> 8);
        encoded += (char)(c & 0xFF);
    }
    return encoded;
}

/* A BK-style ARM vector table plus its seven LDR-PC literals. */
static Bytes vectors(bool app_linked = true)
{
    Bytes out(0x40, '\0');
    wr32le(&out[0], 0xEA00000E); /* B base+0x40 */
    for (int i = 1; i < 8; i++)
        wr32le(&out[i * 4], 0xE59FF014);
    u32 base = app_linked ? 0x10100 : 0x500;
    for (int i = 0; i < 7; i++)
        wr32le(&out[0x20 + i * 4], base + (u32)i * 0x20);
    wr32le(&out[0x3C], 0xDEADBEEF);
    return out;
}

static void synthetic_app_window(Bytes &app, Bytes &raw)
{
    app.assign(0x10000, (char)0xFF);
    app.replace(0, 0x40, vectors(true));
    /* Absolute data/code pointers must survive mapping without relocation. */
    wr32le(&app[0x200], 0x00080BDD);
    raw = crc_encode(app);
    /* A raw-storage tail starts immediately after the CRC-framed application. */
    raw += Bytes("raw-config-tail") + Bytes(64, (char)0xFF);
}

static StrList g_logs;
static void collect_log(const std::string &line) { g_logs.push_back(line); }

/* _build_from_bytes: build_emulator over these bytes (via a temp file). */
static BekenEmulator *build_from_bytes(const Bytes &raw, BuildOptions opt, const KeyCoefs *key, std::string *err,
                                       void (*log)(const std::string &) = 0)
{
    opt.only_uart = true;
    std::string path = make_temp_file("pytests_synthetic_", ".bin");
    write_file(path, raw);
    std::string e;
    BekenEmulator *emu = build_emulator(path, opt, key, &e, log, false);
    remove_file(path);
    if (err)
        *err = e;
    return emu;
}

static BuildOptions chip_opts(const char *chip, int ble_core = -1, bool with_boot = false)
{
    BuildOptions opt;
    opt.chip = chip;
    opt.ble_core = ble_core;
    opt.with_boot = with_boot;
    return opt;
}

/* _assert_raises(fragment, ...): the build fails and names the problem. */
static bool raises(const Bytes &raw, const BuildOptions &opt, const KeyCoefs *key, const char *fragment)
{
    std::string err;
    BekenEmulator *emu = build_from_bytes(raw, opt, key, &err);
    if (emu) {
        delete emu;
        return false;
    }
    return contains(err, fragment);
}

static bool test_crc_prefix()
{
    Bytes samples[5];
    samples[0] = Bytes();
    samples[1] = Bytes("a");
    for (int i = 0; i < 32; i++)
        samples[2] += (char)i;
    for (int i = 0; i < 255; i++)
        samples[3] += (char)i;
    for (int i = 0; i < 211; i++)
        samples[4] += "Beken";
    for (int i = 0; i < 5; i++)
        PT_CHECK(crc16(samples[i], 0xFFFF) == slow_crc16(samples[i], 0xFFFF),
                 strf("crc16 matches the bitwise reference (sample %d)", i));

    Bytes first = samples[2], second, later(32, 'L');
    for (int i = 31; i >= 0; i--)
        second += (char)i;
    Bytes encoded = crc_encode(first + second);
    encoded += Bytes(32, 'X') + Bytes(2, '\0'); /* deliberately invalid third block */
    encoded += crc_encode(later);               /* must not resume after the gap */
    size_t consumed = 0;
    Bytes decoded = decode_crc_prefix(encoded, 0, -1, consumed);
    PT_CHECK(decoded == first + second, "prefix decode stops at the first bad block");
    PT_CHECK(consumed == 68, "consumed == 68");
    size_t limited_consumed = 0;
    Bytes limited = decode_crc_prefix(encoded, 0, 1, limited_consumed);
    PT_CHECK(limited == first && limited_consumed == 34, "max_blocks=1 decodes one block");
    return true;
}

static bool test_app_window_detection_and_mapping()
{
    Bytes app, raw;
    synthetic_app_window(app, raw);
    Bytes detected_app;
    size_t detected_size = 0;
    PT_CHECK(detect_bk7252_app_only(raw, detected_app, detected_size), "app-only window detected");
    PT_CHECK(detected_app == app && detected_size == crc_encode(app).size(), "detected == (app, len(encoded app))");
    std::vector<u32> dest = arm_vector_destinations(app, 0, 0x10000);
    PT_CHECK(!dest.empty() && dest[0] == 0x10040, "reset vector resolves to 0x10040");

    g_logs.clear();
    std::string err;
    BekenEmulator *emu = build_from_bytes(raw, chip_opts("BK7252"), 0, &err, collect_log);
    PT_CHECK(emu != 0, "app window builds: " + err);
    const u32 FLASH_SIZE = BekenEmulator::FLASH_SIZE;
    PT_CHECK(A::bootloader(*emu).empty(), "bootloader is None");
    PT_CHECK(A::app(*emu) == app, "app == decoded window");
    PT_CHECK(A::exception_vector_base(*emu) == 0x10000, "exception_vector_base == 0x10000");
    const Bytes &rf = A::raw_flash(*emu);
    PT_CHECK(rf.size() == FLASH_SIZE, "raw_flash spans FLASH_SIZE");
    PT_CHECK(rf.substr(0, 0x10000) == Bytes(0x10000, (char)0xFF), "raw_flash[:0x10000] erased");
    PT_CHECK(rf.substr(0x10000, 0x10000) == app, "raw_flash[0x10000:0x20000] == app");
    PT_CHECK(rf.substr(0x20000) == Bytes(FLASH_SIZE - 0x20000, (char)0xFF), "raw_flash[0x20000:] erased");
    const Bytes &pf = A::physical_flash(*emu);
    PT_CHECK(A::have_physical(*emu) && pf.substr(0, 0x11000) == Bytes(0x11000, (char)0xFF),
             "physical_flash[:0x11000] erased");
    PT_CHECK(pf.substr(0x11000, raw.size()) == raw, "physical_flash[0x11000:0x11000+len(raw)] == file");
    PT_CHECK(g32le(A::app(*emu), 0x200) == 0x00080BDD, "absolute pointer not relocated");
    bool logged = false;
    for (size_t i = 0; i < g_logs.size(); i++)
        if (contains(g_logs[i], "physical 0x11000"))
            logged = true;
    PT_CHECK(logged, "the layout is logged as physical 0x11000");

    PT_CHECK(raises(raw, chip_opts("BK7252", -1, true), 0, "has no bootloader"), "--with-boot is rejected");
    KeyCoefs zero_key;
    zero_key.has = true; /* key=(0, 0, 0, 0) */
    PT_CHECK(raises(raw, chip_opts("BK7252"), &zero_key, "do not pass -key"), "-key is rejected");

    /* The layout alone must not silently change another requested chip. */
    BekenEmulator *other = build_from_bytes(raw, chip_opts("BK7252N"), 0, &err);
    PT_CHECK(other != 0, "BK7252N builds: " + err);
    PT_CHECK(A::exception_vector_base(*other) == 0, "BK7252N keeps vector base 0");
    PT_CHECK(A::physical_flash(*other).substr(0, raw.size()) == raw, "BK7252N physical_flash[:len(raw)] == file");
    delete other;

    /* Exercise all three manually injected exception types at the relocated
     * table; this is the behaviour the real app needs after its first tick. */
    emu->setup();
    emu->state.icu_global_int_en = 3;
    ArmCpu &cpu = emu->cpu();
    cpu.reg_write(REG_CPSR, 0x13);
    cpu.reg_write(REG_PC, 0x10100);
    A::trigger_irq(*emu);
    PT_CHECK(cpu.reg_read(REG_PC) == 0x10018, "IRQ enters the relocated vector 0x10018");
    cpu.reg_write(REG_CPSR, 0x13);
    cpu.reg_write(REG_PC, 0x10100);
    A::trigger_fiq(*emu);
    PT_CHECK(cpu.reg_read(REG_PC) == 0x1001C, "FIQ enters the relocated vector 0x1001C");
    cpu.reg_write(REG_CPSR, 0x13);
    emu->memory().phys_write(0x10100, 4, 0xEF000000); /* svc #0 */
    cpu.run(0x10100, 0xFFFFFFFFu, 1);
    PT_CHECK(cpu.reg_read(REG_PC) == 0x10008, "SWI enters the relocated vector 0x10008");
    delete emu;
    return true;
}

static bool test_full_image_precedence_and_false_positive_guard()
{
    std::string full_path = pt_firmware("OpenBK7252_QIO_1.18.300.bin");
    Bytes full;
    PT_CHECK(read_file(full_path, full), "firmware image is present: " + full_path);
    Bytes a;
    size_t s;
    PT_CHECK(!detect_bk7252_app_only(full, a, s), "a full image is not an app window");
    BuildOptions opt = chip_opts("BK7252");
    opt.only_uart = true;
    std::string err;
    BekenEmulator *emu = build_emulator(full_path, opt, 0, &err, 0, false);
    PT_CHECK(emu != 0, "full image builds: " + err);
    PT_CHECK(A::exception_vector_base(*emu) == 0, "full image keeps vector base 0");
    const Bytes &pf = A::physical_flash(*emu);
    PT_CHECK(pf.substr(0, full.size()) == full, "physical_flash[:len(full)] == file");
    PT_CHECK(pf.substr(full.size()) == Bytes(pf.size() - full.size(), (char)0xFF), "physical_flash[len(full):] erased");
    delete emu;

    Bytes low_app(0x10000, (char)0xFF);
    low_app.replace(0, 0x40, vectors(false));
    PT_CHECK(!detect_bk7252_app_only(crc_encode(low_app), a, s), "bootloader-linked vectors are not an app window");
    return true;
}

static bool test_flash_size()
{
    const size_t MiB = 1024 * 1024;
    Bytes app, raw;
    synthetic_app_window(app, raw);

    std::string err;
    BekenEmulator *emu = build_from_bytes(raw, chip_opts("BK7252"), 0, &err);
    PT_CHECK(emu != 0, "BK7252 app window builds: " + err);
    const Bytes &pf = A::physical_flash(*emu);
    PT_CHECK(pf.size() == 4 * MiB, "len(physical_flash) == 4 MiB");
    PT_CHECK(pf.substr(0x11000, raw.size()) == raw, "physical_flash[0x11000:...] == raw");
    PT_CHECK(pf.substr(0x11000 + raw.size()) == Bytes(4 * MiB - 0x11000 - raw.size(), (char)0xFF),
             "physical_flash tail erased to 0xFF");
    A::flash_erase(*emu, 0x1FF000, 0x1000);
    const u32 words[2] = {0x8C47C8A5u, 0xC6B4508Cu};
    A::flash_program(*emu, 0x1FF000, words, 2);
    PT_CHECK(A::physical_flash(*emu).substr(0x1FF000, 8) == unhex("a5c8478c8c50b4c6"),
             "flash_program at 0x1FF000 writes expected bytes");
    delete emu;

    const size_t sizes[3] = {2 * MiB, 2 * MiB + 0x11000, 4 * MiB};
    for (int i = 0; i < 3; i++) {
        BekenEmulator *e = build_from_bytes(Bytes(sizes[i], (char)0xA5), chip_opts("BK7252"), 0, &err);
        PT_CHECK(e != 0 && A::physical_flash(*e).size() == 4 * MiB, "BK7252 physical_flash is 4 MiB");
        delete e;
    }
    PT_CHECK(raises(Bytes(4 * MiB + 1, (char)0xA5), chip_opts("BK7252"), 0, "does not fit"),
             "4 MiB + 1 byte is rejected on BK7252");

    const size_t pairs[4][2] = {{1 * MiB, 2 * MiB}, {2 * MiB, 2 * MiB}, {3 * MiB, 4 * MiB}, {4 * MiB, 4 * MiB}};
    for (int i = 0; i < 4; i++) {
        BekenEmulator *e = build_from_bytes(Bytes(pairs[i][0], (char)0xFF), chip_opts("BK7231N"), 0, &err);
        PT_CHECK(e != 0 && A::physical_flash(*e).size() == pairs[i][1], "BK7231N auto-sizes physical_flash");
        delete e;
    }
    return true;
}

static bool layout_is(BekenEmulator *emu, const char *layout)
{
    return emu && A::blecore(*emu) && A::blecore(*emu)->layout == layout;
}

static bool test_ble_core_selection()
{
    /* Real BLE 5.x silicon is automatic; the legacy override stays narrow. */
    Bytes raw(68, (char)0xFF);
    std::string err;
    BekenEmulator *bk7231n = build_from_bytes(raw, chip_opts("BK7231N"), 0, &err);
    BekenEmulator *bk7238 = build_from_bytes(raw, chip_opts("BK7238"), 0, &err);
    PT_CHECK(layout_is(bk7231n, "5.1"), "BK7231N gets the BLE 5.1 core");
    PT_CHECK(layout_is(bk7238, "5.2"), "BK7238 gets the BLE 5.2 core");

    /* Generic BK7231 may mean T/U (BLE 4.2) or N/M (BLE 5.1), so it cannot be
     * enabled from chip identity alone. The retained flag is an explicit
     * assertion that this particular image is N/M-family firmware. */
    BekenEmulator *ambiguous = build_from_bytes(raw, chip_opts("BK7231"), 0, &err);
    BekenEmulator *forced = build_from_bytes(raw, chip_opts("BK7231", 1), 0, &err);
    PT_CHECK(ambiguous && A::blecore(*ambiguous) == 0, "generic BK7231 has no automatic BLE core");
    PT_CHECK(layout_is(forced, "5.1"), "--ble-core forces the 5.1 core on BK7231");

    /* BK7231U serves the generic family's same silicon ID, but an explicit
     * variant selection exposes its second GPIO bank and keeps its BLE 4.2
     * hardware out of the unrelated BLE 5.1 model. */
    BekenEmulator *bk7231u = build_from_bytes(raw, chip_opts("BK7231U"), 0, &err);
    PT_CHECK(bk7231u && bk7231u->has_extended_gpio && bk7231u->gpio_pin_count == 40, "BK7231U has 40 GPIOs");
    PT_CHECK(A::blecore(*bk7231u) == 0, "BK7231U has no BLE 5.x core");
    PT_CHECK(!ambiguous->has_extended_gpio && ambiguous->gpio_pin_count == 32, "generic BK7231 has 32 GPIOs");
    PT_CHECK(raises(raw, chip_opts("BK7231U", 1), 0, "this chip has no such core"), "BK7231U rejects --ble-core");

    /* False remains a deliberate API-level diagnostic opt-out, while chips
     * without this BLE generation stay off in automatic mode and reject a
     * nonsensical force exactly as the CLI has always done. */
    BekenEmulator *disabled = build_from_bytes(raw, chip_opts("BK7231N", 0), 0, &err);
    BekenEmulator *unsupported = build_from_bytes(raw, chip_opts("BK7252"), 0, &err);
    PT_CHECK(disabled && A::blecore(*disabled) == 0, "ble_core=False disables it on BK7231N");
    PT_CHECK(unsupported && A::blecore(*unsupported) == 0, "BK7252 has no BLE 5.x core");
    PT_CHECK(raises(raw, chip_opts("BK7252", 1), 0, "this chip has no such core"), "BK7252 rejects --ble-core");
    delete bk7231n;
    delete bk7238;
    delete ambiguous;
    delete forced;
    delete bk7231u;
    delete disabled;
    delete unsupported;
    return true;
}

/* optional_real_bk7252u_smoke: run the public A9 dump when BK7252U_DUMP
 * names it; CI stays synthetic. */
struct SmokeCtx {
    Bytes out1, out2;
    BekenEmulator *emu;
};
static bool ends_with(const Bytes &b, const char *s)
{
    size_t n = strlen(s);
    return b.size() >= n && b.compare(b.size() - n, n, s) == 0;
}
static void smoke_sink(void *ctx, int port, u8 byte)
{
    SmokeCtx *c = (SmokeCtx *)ctx;
    if (port != 1 && port != 2)
        return;
    (port == 1 ? c->out1 : c->out2) += (char)byte;
    if (ends_with(c->out1, "msh />") || ends_with(c->out2, "msh />"))
        c->emu->stop();
}
static void run_emu(void *arg) { ((BekenEmulator *)arg)->run(); }

static bool optional_real_bk7252u_smoke()
{
    std::string path = env_get("BK7252U_DUMP");
    if (path.empty()) {
        printf("SKIP real BK7252U smoke (set BK7252U_DUMP to enable)\n");
        return true;
    }
    BuildOptions opt = chip_opts("BK7252");
    opt.only_uart = true;
    std::string err;
    BekenEmulator *emu = build_emulator(path, opt, 0, &err, 0, false);
    PT_CHECK(emu != 0, "BK7252U dump builds: " + err);
    SmokeCtx ctx;
    ctx.emu = emu;
    emu->uart_sink = smoke_sink;
    emu->uart_sink_ctx = &ctx;
    emu->setup();
    Thread worker;
    worker.start(run_emu, emu);
    if (!worker.join(120000)) {
        emu->stop();
        worker.join(5000);
        PT_CHECK(false, "BK7252U firmware did not reach its shell in 120 seconds");
    }
    Bytes text = ctx.out1 + "\n" + ctx.out2;
    const char *markers[] = {"- RT -     Thread Operating System", "3.1.0 build May 14 2022", "OSK Rev: R-3.0.22",
                             "SDK Rev: 3.0.33", "lwIP-2.0.2 initialized!", "beken wlan hw init", "drv_pm_init",
                             "msh />"};
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
        PT_CHECK(text.find(markers[i]) != std::string::npos, strf("missing UART marker: %s", markers[i]));
    PT_CHECK(!emu->has_run_error, emu->run_error);
    delete emu;
    pt_pass("optional_real_bk7252u_smoke");
    return true;
}

int image_loader_tests()
{
    pt_checks = 0;
    struct T {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"test_crc_prefix", test_crc_prefix},
        {"test_app_window_detection_and_mapping", test_app_window_detection_and_mapping},
        {"test_full_image_precedence_and_false_positive_guard", test_full_image_precedence_and_false_positive_guard},
        {"test_flash_size", test_flash_size},
        {"test_ble_core_selection", test_ble_core_selection},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++) {
        if (!tests[i].fn())
            return -1;
        pt_pass(tests[i].name);
    }
    if (!optional_real_bk7252u_smoke())
        return -1;
    printf("All image-loader self-tests passed.\n");
    return pt_checks;
}
