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
    },
    {
        "Sonoff S26", esp::CHIP_ESP8266,
        "IoT/ESP8266/Sonoff_S26.bin",
        "8ce784a3f2ef3cb938aa8c94e063c891b11f0d8401908c1574943b9812fe9698",
        "SDK ver:", "2nd boot version :", "",
        0x100000, 0x4010057c, 0x1000, 0x40100004, 4, 0, 0, 12
    },
    {
        "Sonoff POWR316D", esp::CHIP_ESP32,
        "IoT/ESP32/Sonoff-Powr316D.bin",
        "d31118361c0bfdece510eed23ae4fa7ad5ebad2e7b76204d9a7bc09a66f9187e",
        "AT+SDK_VER=ESP32-SONOFF-", "2nd stage bootloader", "AT+SDK_VER?\r\n",
        0x400000, 0x40080688, 0x20000, 0x40081390, 6, 0, -1, -1
    },
    {
        "Sonoff POWCT", esp::CHIP_ESP32,
        "IoT/ESP32/Sonoff-POWCT-20250518.bin",
        "295cd9b7791d6a088216bfafc03d63bbb4622df5e68d2f26f499ed64161b3625",
        "AT+SDK_VER=ESP32-SONOFF-", "2nd stage bootloader", "AT+SDK_VER?\r\n",
        0x400000, 0x40080688, 0x20000, 0x4008135c, 6, 0, -1, -1
    },
    {
        "Sonoff THR320D", esp::CHIP_ESP32,
        "IoT/ESP32/Sonoff-THR320D.bin",
        "538d8e6244388abe6f758e821a2c58ccdfe3a30badd248f1c64b72c23d6cdf6d",
        "AT+SDK_VER=ESP32-SONOFF-", "2nd stage bootloader", "AT+SDK_VER?\r\n",
        0x400000, 0x40080688, 0x60000, 0x4008135c, 6, 0, -1, -1
    },
    {
        "Tuya A60-WIFI", esp::CHIP_ESP8266,
        "IoT/ESP8266/Tuya_A60-WIFI_DY.0410_(schemaID-0000001bli)_key4fv3xs8twchhy_bqkefojbmb8ovvws_1.6.1.bin",
        "223138bff6b4b9633c0d1f5c76eac1aa050d2f2b4dcd5567e27a641d53bd89e1",
        "tuya sdk version:", "2nd boot version :", "",
        0x200000, 0x4010057c, 0x1000, 0x40100004, 4, 0, -1, -1
    },
    {
        "Tuya BW-SHP10", esp::CHIP_ESP8266,
        "IoT/ESP8266/Tuya_BW-SHP10_(schemaID-0000005kwi)_vuyw3gn9lcaq4wmv_1.0.0.bin",
        "349d02993835a93537585c105a9c8b1d4a43cffdb870640dcdf458a2f2abbca5",
        "tuya sdk version:", "2nd boot version :", "",
        0x100000, 0x4010057c, 0x1000, 0x40100004, 4, 0, -1, -1
    },
    {
        "OpenBeken ESP8266", esp::CHIP_ESP8266,
        "OpenBeken/OpenESP8266_2MB_1.18.321.bin",
        "cddc3357c80ac665b4912df7ef38cfae1636deb592bb12e798506721483574d6",
        "OpenESP8266", "2nd stage bootloader", "",
        0x200000, 0x40100290, 0x10000, 0x40214328, 3, 0, -1, -1
    },
    {
        "OpenBeken ESP32", esp::CHIP_ESP32,
        "OpenBeken/OpenESP32_4MB_1.18.321.bin",
        "ef601995a68d43a6218f4634fd9321c334e1fe11ecc613269f1a554d9505a4f0",
        "OpenESP32", "boot: ESP-IDF", "",
        0x400000, 0x40080644, 0x10000, 0x4008158c, 6, 0, -1, -1
    },
    {
        "Emporia Vue Gen3", esp::CHIP_ESP32,
        "IoT/ESP32/Emporia_Vue_Gen3_(ESP32-WROVER-IE)_(ESP32-D0WD-V3).bin",
        "8a4cfab62d7a3f9a2b9dc038384256371ac5d19cd17022bb017f226e85be148e",
        "App cpu up.", "2nd stage bootloader", "",
        0x800000, 0x40080698, 0x60000, 0x40081720, 6, 8, -1, -1
    },
    {
        "Tuya BW-SHP9", esp::CHIP_ESP8266,
        "IoT/ESP8266/Tuya_BW-SHP9_4xSocketCord_(schemaID-0000003dde)_4es44udrj48pqtlb_1.0.2.bin",
        "0794ddd85d3c829ea8fe1b6a0badccbfb729d8844659d4a5d3247bb85d81ae6b",
        "tuya sdk version:", "2nd boot version :", "",
        0x100000, 0x4010057c, 0x1000, 0x40100004, 4, 0, -1, -1
    }
};

static const size_t ESP_FIXTURE_COUNT = sizeof(ESP_FIXTURES) / sizeof(ESP_FIXTURES[0]);

#endif
