/* Port of tests/chips.py: Chip registry, values, and what the emulator reads from them. */
#include "pytests.h"
#include "chips.h"
#include "periph.h"
#include "blecore.h"

#include <string.h>

static int pt_checks;

typedef PyTestAccess A;

static const u32 MiB = 1024 * 1024;

struct OldId {
    const char *name;
    u32 chip_id;
    u32 dev_id;
};

static const OldId OLD_IDS[6] = {
    {"BK7231",  0x0007231Au, 0x18520001u},
    {"BK7231U", 0x0007231Au, 0x18520001u},
    {"BK7231N", 0x0007231Cu, 0x20521023u},
    {"BK7238",  0x00007238u, 0x21128000u},
    {"BK7252",  0x0007221Au, 0x18221020u},
    {"BK7252N", 0x0007252Au, 0x23A18000u},
};

static BekenEmulator *make_emu(const char *name = 0, int ble_core = -1)
{
    BuildOptions opt;
    opt.only_uart = true;
    opt.chip = name ? name : "BK7231";
    opt.ble_core = ble_core;
    const Chip *c = name ? find_chip(name) : 0;
    ChipId cid;
    cid.chip = c ? c->chip_id : default_chip().chip_id;
    cid.device = c ? c->dev_id : default_chip().dev_id;
    return new BekenEmulator(Bytes(0x20000, (char)0xFF), Bytes(), Bytes(), opt, cid, 0, c);
}

static bool test_ids_match_old_table()
{
    const std::map<std::string, const Chip *> &m = chips_map();
    PT_CHECK(m.size() == 6, "len(CHIPS) == 6");
    for (int i = 0; i < 6; i++) {
        std::map<std::string, const Chip *>::const_iterator it = m.find(OLD_IDS[i].name);
        PT_CHECK(it != m.end() && it->second != 0, OLD_IDS[i].name);
        PT_CHECK(it->second->chip_id == OLD_IDS[i].chip_id && it->second->dev_id == OLD_IDS[i].dev_id,
                 OLD_IDS[i].name);
    }
    return true;
}

static bool test_registry()
{
    StrList names = chip_names();
    StrList want;
    want.push_back("BK7231");
    want.push_back("BK7231N");
    want.push_back("BK7231U");
    want.push_back("BK7238");
    want.push_back("BK7252");
    want.push_back("BK7252N");
    PT_CHECK(names == want, "chip_names() sorted");
    std::string err;
    PT_CHECK(get_chip(" bk7252 ", &err) == find_chip("BK7252"), "get_chip(' bk7252 ')");
    PT_CHECK(get_chip("nope", &err) == 0 && contains(err, "Known chips: BK7231, BK7231N"),
             "get_chip('nope') raises");
    return true;
}

static bool test_bk7252_values()
{
    const Chip *chip = find_chip("BK7252");
    PT_CHECK(chip != 0, "BK7252 exists");
    PT_CHECK(chip->flash_size == 4 * MiB, "flash_size == 4 MiB");
    PT_CHECK(chip->app_only_phys == 0x11000 && chip->app_only_cpu() == 0x10000, "app_only offsets");
    PT_CHECK(chip->sdram_base == 0x900000u && chip->sdram_size == 0x40000u, "sdram == (0x900000, 0x40000)");
    PT_CHECK(!chip->has_xvr(), "xvr is None");
    PT_CHECK(chip->gpio_pins == 40, "gpio_pins == 40");
    return true;
}

static bool test_bk7252n_is_not_bk7252()
{
    const Chip *chip = find_chip("BK7252N");
    PT_CHECK(chip != 0, "BK7252N exists");
    PT_CHECK(std::string(chip->gdma) == "v2" && std::string(chip->i2c) == "v2" && std::string(chip->ejpeg) == "v2",
             "gdma == i2c == ejpeg == v2");
    PT_CHECK(chip->has_audio() && std::string(chip->audio) == "v1", "audio == v1");
    PT_CHECK(!chip->has_sdram() && chip->has_xvr() && std::string(chip->xvr) == "v1", "sdram is None and xvr == v1");
    return true;
}

static bool test_name_matches_key()
{
    const std::map<std::string, const Chip *> &m = chips_map();
    for (std::map<std::string, const Chip *>::const_iterator it = m.begin(); it != m.end(); ++it)
        PT_CHECK(it->first == it->second->name, it->first);
    return true;
}

static bool test_emulator_identity()
{
    for (int i = 0; i < 6; i++) {
        BekenEmulator *emu = make_emu(OLD_IDS[i].name);
        PT_CHECK(emu->chip == find_chip(OLD_IDS[i].name), OLD_IDS[i].name);
        PT_CHECK(emu->chip_id_value == OLD_IDS[i].chip_id && emu->device_id_value == OLD_IDS[i].dev_id,
                 OLD_IDS[i].name);
        delete emu;
    }
    return true;
}

static bool test_default_chip()
{
    BekenEmulator *emu = make_emu();
    PT_CHECK(emu->chip == &default_chip(), "default_chip()");
    delete emu;
    return true;
}

static bool test_ble_selection()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        if (std::string(name) == "BK7231N")
            PT_CHECK(A::blecore(*emu) && A::blecore(*emu)->layout == "5.1", name);
        else if (std::string(name) == "BK7238")
            PT_CHECK(A::blecore(*emu) && A::blecore(*emu)->layout == "5.2", name);
        else
            PT_CHECK(A::blecore(*emu) == 0, name);
        delete emu;
    }
    BekenEmulator *forced = make_emu("BK7231", 1);
    PT_CHECK(A::blecore(*forced) != 0, "BK7231 ble_core=1");
    delete forced;
    const char *bad[3] = {"BK7231U", "BK7252", "BK7252N"};
    for (int i = 0; i < 3; i++) {
        BekenEmulator *emu = make_emu(bad[i], 1);
        PT_CHECK(!emu->init_error.empty() && contains(emu->init_error, "this chip has no such core"), bad[i]);
        delete emu;
    }
    return true;
}

static bool test_gpio_pins()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        int want = (std::string(name) == "BK7231U" || std::string(name) == "BK7252") ? 40 : 32;
        PT_CHECK(emu->gpio_pin_count == want && emu->state.gpio_hw.pin_count == want, name);
        delete emu;
    }
    return true;
}

static bool test_periph_pin_layout()
{
    struct Pair { const char *name; int want; } pairs[5] = {
        {"BK7252", 40}, {"BK7231U", 40}, {"BK7231N", 32}, {"BK7231M", 32}, {"", 32}
    };
    std::map<int, u32> g;
    g[0] = 1;
    std::map<u32, u32> p;
    for (int i = 0; i < 5; i++) {
        periph::DecodedState d = periph::decode_state(g, p, pairs[i].name, 0, 0, 0, 0, 0, 0, 0);
        PT_CHECK((int)d.gpio.size() == pairs[i].want, pairs[i].name);
    }
    return true;
}

static bool test_pwm_and_saradc_variants()
{
    const std::map<std::string, const Chip *> &m = chips_map();
    std::set<std::string> pwm_v2, want_v2;
    want_v2.insert("BK7231");
    want_v2.insert("BK7231U");
    want_v2.insert("BK7231N");
    want_v2.insert("BK7238");
    for (std::map<std::string, const Chip *>::const_iterator it = m.begin(); it != m.end(); ++it)
        if (std::string(it->second->pwm) == "v2")
            pwm_v2.insert(it->first);
    PT_CHECK(pwm_v2 == want_v2, "pwm v2 chips");
    PT_CHECK(std::string(find_chip("BK7231")->saradc) == "v1", "BK7231 saradc v1");
    PT_CHECK(std::string(find_chip("BK7231U")->saradc) == "v1", "BK7231U saradc v1");
    PT_CHECK(std::string(find_chip("BK7231N")->saradc) == "v1", "BK7231N saradc v1");
    PT_CHECK(std::string(find_chip("BK7252")->saradc) == "v2", "BK7252 saradc v2");
    PT_CHECK(std::string(find_chip("BK7238")->saradc) == "v3", "BK7238 saradc v3");
    PT_CHECK(std::string(find_chip("BK7252N")->saradc) == "v3", "BK7252N saradc v3");
    return true;
}

static bool find_region(BekenEmulator &emu, u32 address, u32 &begin_out, u32 &size_out)
{
    std::vector<std::pair<u32, u32> > regs;
    emu.memory().regions(regs);
    for (size_t i = 0; i < regs.size(); i++) {
        u32 b = regs[i].first, sz = regs[i].second;
        if (address >= b && address - b < sz) {
            begin_out = b;
            size_out = sz;
            return true;
        }
    }
    return false;
}

static bool test_memory_map()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        emu->setup();
        const Chip *chip = find_chip(name);
        u32 b = 0, sz = 0;
        PT_CHECK(find_region(*emu, 0x00400000u, b, sz) && b == 0x00400000u && sz == 1 * MiB, name);
        u32 want_9 = chip->sdram_size ? chip->sdram_size : 0x1000u;
        PT_CHECK(find_region(*emu, 0x00900000u, b, sz) && b == 0x00900000u && sz == want_9, name);
        if (std::string(chip->ejpeg) == "v2")
            PT_CHECK(find_region(*emu, 0x00A00000u, b, sz) && b == 0x00A00000u && sz == 0x10000u, name);
        else
            PT_CHECK(!find_region(*emu, 0x00A00000u, b, sz), name);
        delete emu;
    }
    return true;
}

static bool test_memory_map_after_reset()
{
    const char *names[3] = {"BK7252", "BK7252N", "BK7231N"};
    for (int i = 0; i < 3; i++) {
        BekenEmulator *emu = make_emu(names[i]);
        emu->setup();
        std::vector<std::pair<u32, u32> > before, after;
        emu->memory().regions(before);
        emu->memory().write_bytes(0x00900000u, Bytes(8, (char)0x55));
        A::watchdog_reset(*emu);
        emu->memory().regions(after);
        PT_CHECK(before == after, names[i]);
        PT_CHECK(emu->memory().read_bytes(0x00900000u, 8) == Bytes(8, '\0'), names[i]);
        delete emu;
    }
    return true;
}

static bool test_gdma_channel_layout()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        if (std::string(name) == "BK7252N") {
            PT_CHECK(A::gdma_channel(*emu, 0x00809040u) == 0, name);
            PT_CHECK(A::gdma_channel(*emu, 0x00809080u) == 1, name);
            PT_CHECK(A::gdma_channel(*emu, 0x00809000u) == -1, name);
        } else {
            PT_CHECK(A::gdma_channel(*emu, 0x00809000u) == 0, name);
            PT_CHECK(A::gdma_channel(*emu, 0x00809040u) == 2, name);
            PT_CHECK(A::gdma_channel(*emu, 0x00809010u) == -1, name);
        }
        delete emu;
    }
    return true;
}

static bool test_ejpeg_blocks()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        emu->setup();
        emu->mmio_write(0x0080A000u, 4, 0x5u);
        PT_CHECK(emu->state.ejpeg_reg0 == (std::string(name) == "BK7252N" ? 0u : 0x5u), name);
        delete emu;
        emu = make_emu(name);
        emu->setup();
        if (std::string(name) == "BK7252N") {
            emu->mmio_write(0x00A03008u, 4, 0x5u);
            PT_CHECK(emu->state.ejpeg_reg0 == 0x5u, name);
        }
        delete emu;
    }
    return true;
}

static bool test_audio_block()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        emu->setup();
        emu->mmio_write(0x00802B08u, 4, 0x3Cu);
        bool has_audio = (std::string(name) == "BK7252" || std::string(name) == "BK7252N");
        PT_CHECK(emu->state.audio_fifo_conf == (has_audio ? 0x3Cu : 0u), name);
        delete emu;
    }
    return true;
}

static bool test_i2c_v2()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        PT_CHECK(emu->state.hw_i2c[1].v2 == (std::string(name) == "BK7252N"), name);
        emu->setup();
        A::watchdog_reset(*emu);
        PT_CHECK(emu->state.hw_i2c[1].v2 == (std::string(name) == "BK7252N"), name);
        delete emu;
    }
    return true;
}

static bool test_usb_plug_wake()
{
    for (int i = 0; i < 6; i++) {
        const char *name = OLD_IDS[i].name;
        BekenEmulator *emu = make_emu(name);
        emu->setup();
        emu->mmio_read(0x00800084u, 4);
        u32 val = emu->memory().phys_read(0x00800084u, 4);
        PT_CHECK(((val & (1u << 21)) != 0) == (std::string(name) == "BK7252"), name);
        delete emu;
    }
    return true;
}

static bool test_snapshot_rejects_old()
{
    BekenEmulator *emu_a = make_emu("BK7252");
    emu_a->setup();
    std::string path = make_temp_file("pytests_chip_snap_", ".snap");
    A::save_snapshot(*emu_a, path);
    delete emu_a;

    BekenEmulator *emu_b = make_emu("BK7231");
    emu_b->setup();
    std::string err;
    PT_CHECK(!emu_b->load_snapshot(path, &err) && contains(err, "different -chip"),
             "load_snapshot rejects mismatched chip");
    delete emu_b;
    remove_file(path);
    return true;
}

int chips_tests()
{
    pt_checks = 0;
    struct T {
        const char *name;
        bool (*fn)();
    } tests[] = {
        {"test_ids_match_old_table", test_ids_match_old_table},
        {"test_registry", test_registry},
        {"test_bk7252_values", test_bk7252_values},
        {"test_bk7252n_is_not_bk7252", test_bk7252n_is_not_bk7252},
        {"test_name_matches_key", test_name_matches_key},
        {"test_emulator_identity", test_emulator_identity},
        {"test_default_chip", test_default_chip},
        {"test_ble_selection", test_ble_selection},
        {"test_gpio_pins", test_gpio_pins},
        {"test_periph_pin_layout", test_periph_pin_layout},
        {"test_pwm_and_saradc_variants", test_pwm_and_saradc_variants},
        {"test_memory_map", test_memory_map},
        {"test_memory_map_after_reset", test_memory_map_after_reset},
        {"test_gdma_channel_layout", test_gdma_channel_layout},
        {"test_ejpeg_blocks", test_ejpeg_blocks},
        {"test_audio_block", test_audio_block},
        {"test_i2c_v2", test_i2c_v2},
        {"test_usb_plug_wake", test_usb_plug_wake},
        {"test_snapshot_rejects_old", test_snapshot_rejects_old},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    for (int i = 0; i < n; i++) {
        if (!tests[i].fn())
            return -1;
        pt_pass(tests[i].name);
    }
    printf("All chip tests passed.\n");
    return pt_checks;
}
