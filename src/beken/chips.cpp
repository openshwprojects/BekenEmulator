#include "beken/chips.h"
#include "beken/beken_crypto.h"

#include <algorithm>

static const Chip CHIP_BK7231 = {
    "BK7231",
    0x0007231Au, 0x18520001u, 32,
    "5.1", false,
    0, 0x00000000u, 0x00010000u, -1,
    0x00400000u, 1 * MIB,
    0, 0,
    "v1", "v1", "v1", "v1", "v2", "", "v1", "v1",
    false, false, false
};

static const Chip CHIP_BK7231U = {
    "BK7231U",
    0x0007231Au, 0x18520001u, 40,
    "", false,
    0, 0x00000000u, 0x00010000u, -1,
    0x00400000u, 1 * MIB,
    0, 0,
    "v1", "v1", "v1", "v1", "v2", "", "v1", "v1",
    false, false, false
};

static const Chip CHIP_BK7231N = {
    "BK7231N",
    0x0007231Cu, 0x20521023u, 32,
    "5.1", true,
    0, 0x00000000u, 0x00010000u, -1,
    0x00400000u, 1 * MIB,
    0, 0,
    "v1", "v1", "v1", "v1", "v2", "", "v1", "v1",
    false, false, false
};

static const Chip CHIP_BK7238 = {
    "BK7238",
    0x00007238u, 0x21128000u, 32,
    "5.2", true,
    0, 0x00000000u, 0x00010000u, -1,
    0x00400000u, 1 * MIB,
    0, 0,
    "v1", "v1", "v1", "v1", "v2", "", "v3", "v1",
    false, false, false
};

static const Chip CHIP_BK7252 = {
    "BK7252",
    0x0007221Au, 0x18221020u, 40,
    "", false,
    4 * MIB, 0x00000000u, 0x00010000u, 0x11000,
    0x00400000u, 1 * MIB,
    0x00900000u, 0x40000u,
    "", "v1", "v1", "v1", "v1", "v1", "v2", "v2",
    false, true, true
};

static const Chip CHIP_BK7252N = {
    "BK7252N",
    0x0007252Au, 0x23A18000u, 32,
    "", false,
    0, 0x00000000u, 0x00010000u, -1,
    0x00400000u, 1 * MIB,
    0, 0,
    "v1", "v2", "v2", "v2", "v1", "v1", "v3", "v1",
    true, true, false
};

static const Chip *const ALL_CHIPS[] = {
    &CHIP_BK7231, &CHIP_BK7231U, &CHIP_BK7231N,
    &CHIP_BK7238, &CHIP_BK7252, &CHIP_BK7252N, 0
};

const Chip &default_chip()
{
    return CHIP_BK7231;
}

const std::map<std::string, const Chip *> &chips_map()
{
    static std::map<std::string, const Chip *> m;
    if (m.empty()) {
        for (int i = 0; ALL_CHIPS[i]; i++)
            m[ALL_CHIPS[i]->name] = ALL_CHIPS[i];
    }
    return m;
}

const Chip *find_chip(const std::string &name)
{
    const std::map<std::string, const Chip *> &m = chips_map();
    std::map<std::string, const Chip *>::const_iterator it = m.find(name);
    return it == m.end() ? 0 : it->second;
}

StrList chip_names()
{
    StrList out;
    const std::map<std::string, const Chip *> &m = chips_map();
    for (std::map<std::string, const Chip *>::const_iterator it = m.begin(); it != m.end(); ++it)
        out.push_back(it->first);
    std::sort(out.begin(), out.end());
    return out;
}

const Chip *get_chip(const std::string &name, std::string *err)
{
    std::string key = upper(pyc::py_strip(name));
    const Chip *chip = find_chip(key);
    if (!chip) {
        if (err)
            *err = "Unknown -chip value: " + pyc::py_repr(name) + ". Known chips: " + join(chip_names(), ", ") + ".";
        return 0;
    }
    return chip;
}

bool fit_flash(const Bytes &physical, const Chip &chip, Bytes &out, std::string *err)
{
    size_t flash_size = chip.flash_size;
    if (flash_size == 0) {
        flash_size = DEFAULT_FLASH_SIZE;
        while (flash_size < physical.size())
            flash_size *= 2;
    }
    if (physical.size() > flash_size) {
        if (err)
            *err = strf("The dump (%llu bytes) does not fit the %llu-byte flash of %s.",
                        (unsigned long long)physical.size(), (unsigned long long)flash_size, chip.name);
        return false;
    }
    out = physical;
    if (out.size() < flash_size)
        out.append(flash_size - out.size(), (char)0xFF);
    return true;
}
