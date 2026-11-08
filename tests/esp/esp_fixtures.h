/* External firmware fixtures are deliberately referenced, never copied into
 * the repository: dumps may include device credentials. Hashes pin the exact
 * tested bytes. Markers must be observed on emulated UART during execution;
 * finding them in a binary is not evidence that its application runs. */
#ifndef BK_TEST_ESP_FIXTURES_H
#define BK_TEST_ESP_FIXTURES_H

#include "esp/esp_image.h"

struct EspFixture {
    const char *label;
    esp::Chip chip;
    const char *relative_path; /* relative to ESP_FLASH_DUMPS */
    const char *sha256;
    const char *app_marker;
    const char *boot_marker;
    const char *uart_command;
    size_t flash_size;
    u32 boot_entry;
    size_t app_offset;
    u32 app_entry;
    size_t app_segments;
    unsigned psram_mb; /* board memory required by this firmware */
    int button_pin,relay_pin; /* active-low button / active-high relay; -1 absent */
};

static const EspFixture ESP_FIXTURES[] = {
    {
        "Tuya BW-SHP6", esp::CHIP_ESP8266,
        "IoT/ESP8266/Tuya_BW-SHP6_(schemaID-0000004cy4)_ab4kwxlrok0yvxuo_1.0.4.bin",
        "26556d53971a639c5b76bc97ddb13a4ab94358fc6d45af3197785c9ea9ada28b",
        "tuya sdk version:", "2nd boot version :", "",
        0x100000, 0x4010057c, 0x1000, 0x40100004, 4, 0, 13, 15
    },
    {
        "Sonoff NSPanel", esp::CHIP_ESP32,
        "IoT/ESP32/Sonoff-NSPanel.bin",
        "a047dcdb5fac92d10d956cd09b0100df4aae82ef61b5df9bf71aed7270c89596",
        "AT+SDK_VER=ESP32-SONOFF-", "2nd stage bootloader", "AT+SDK_VER?\r\n",
        0x400000, 0x40080688, 0x20000, 0x400815a4, 6, 8, -1, -1
    },
    {
        "Tuya Gosund SP1", esp::CHIP_ESP8266,
        "IoT/ESP8266/Tuya_Gosund_SP1_EUPlug_(schemaID-000000075o)_qxJSyTLEtX5WrzA9_1.0.0.bin",
        "1a5df50bf99252c9ae46bc36ae3439f35d846733d7b3967dffa220664d2584a9",
        "tuya sdk version:", "2nd boot version :", "",
        0x100000, 0x40100294, 0x1000, 0x40100004, 4, 0, 13, 15
    }
};

static const size_t ESP_FIXTURE_COUNT = sizeof(ESP_FIXTURES) / sizeof(ESP_FIXTURES[0]);

#endif
