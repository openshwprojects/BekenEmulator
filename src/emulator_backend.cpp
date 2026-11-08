#include "emulator_backend.h"
#include "beken/chips.h"
#include <algorithm>

EmulatorBackend *build_beken_backend(const std::string &, const BuildOptions &,
    const KeyCoefs *, std::string *, void (*)(const std::string &), bool, std::string *);
EmulatorBackend *build_esp_backend(const std::string &, const BuildOptions &,
    const KeyCoefs *, std::string *, void (*)(const std::string &), bool, std::string *);

namespace {
typedef EmulatorBackend *(*BackendBuilder)(const std::string &, const BuildOptions &,
    const KeyCoefs *, std::string *, void (*)(const std::string &), bool, std::string *);
struct BackendRegistration {
    const char *chip, *family, *architecture;
    unsigned gpio_pins, console_uart, uart_rx_mask;
    BackendBuilder build;
};
/* Beken variants share their existing descriptor table; additional families
 * register here without adding their hardware fields to Beken's Chip. */
const BackendRegistration extra_backends[] = {
    {"ESP8266", "Espressif", "Xtensa LX106", 17, 0, 1, build_esp_backend},
    {"ESP32", "Espressif", "Xtensa LX6", 40, 0, 7, build_esp_backend}
};

BackendBuilder lookup(const std::string &name, BackendInfo &info)
{
    std::string chip = upper(strip(name));
    const Chip *beken = find_chip(chip);
    if (beken) {
        info = BackendInfo(chip, "Beken", "ARM", beken->gpio_pins, 1, 2);
        return build_beken_backend;
    }
    for (size_t i = 0; i < sizeof(extra_backends) / sizeof(extra_backends[0]); ++i) {
        const BackendRegistration &r = extra_backends[i];
        if (chip == r.chip) {
            info = BackendInfo(r.chip, r.family, r.architecture, r.gpio_pins, r.console_uart, r.uart_rx_mask);
            return r.build;
        }
    }
    return 0;
}
}

StrList backend_chip_names()
{
    StrList names = chip_names();
    for (size_t i = 0; i < sizeof(extra_backends) / sizeof(extra_backends[0]); ++i)
        names.push_back(extra_backends[i].chip);
    std::sort(names.begin(), names.end());
    return names;
}

bool backend_info(const std::string &name, BackendInfo &info) { return lookup(name, info) != 0; }

EmulatorBackend *build_backend(const std::string &path, const BuildOptions &options,
    const KeyCoefs *key, std::string *err, void (*log)(const std::string &),
    bool save_decrypted, std::string *exception_type)
{
    if (err) err->clear();
    if (exception_type) *exception_type = "ValueError";
    BackendInfo info;
    BackendBuilder builder = lookup(options.chip, info);
    if (!builder) {
        if (err) *err = "Unknown -chip value: '" + options.chip + "'"
            + ". Known chips: " + join(backend_chip_names(), ", ") + ".";
        return 0;
    }
    BuildOptions normalized = options;
    normalized.chip = info.chip;
    return builder(path, normalized, key, err, log, save_decrypted, exception_type);
}
