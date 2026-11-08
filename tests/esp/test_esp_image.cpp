#include "esp/esp_image.h"
#include "crypto_prims.h"
#include "esp/esp_fixtures.h"
#include <stdio.h>
#include <fstream>
#include <iterator>

static int esp_checks;
#define ESP_CHECK(test) do { ++esp_checks; if (!(test)) { \
    printf("[FAIL] esp_image line %d: %s\n", __LINE__, #test); return false; } } while (0)

static void word(Bytes &b, u32 v)
{
    char w[4]; wr32le(w, v); b.append(w, 4);
}

static Bytes header(u8 magic, u8 count, u32 entry)
{
    Bytes b; b += (char)magic; b += (char)count; b += '\0'; b += '\0';
    word(b, entry); return b;
}

static void segment(Bytes &b, u32 address, const Bytes &payload, u8 &checksum)
{
    word(b, address); word(b, (u32)payload.size()); b += payload;
    for (size_t n = 0; n < payload.size(); ++n) checksum ^= (u8)payload[n];
}

static void footer(Bytes &b, u8 checksum)
{
    b.append(15 - b.size() % 16, '\0'); b += (char)checksum;
}

static u32 fixture_crc(const Bytes &b)
{
    u32 crc = 0xffffffff;
    for (size_t i = 0; i < b.size(); ++i) {
        crc ^= (u8)b[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc ^= 0xffffffff;
    return (crc & 0x80000000) ? (crc ^ 0xffffffff) : crc + 1;
}

static Bytes fixture8266(bool v2)
{
    Bytes b;
    u8 sum = 0xef;
    if (v2) {
        b = header(0xea, 4, 0x40100000);
        u8 ignored = 0;
        segment(b, 0, Bytes(32, '\x5a'), ignored);
    }
    b += header(0xe9, 2, 0x40100000);
    segment(b, 0x40100000, Bytes("\x36\x41\0\x08", 4), sum);
    segment(b, 0x3ffe8000, Bytes("DATA", 4), sum);
    footer(b, sum);
    if (v2) word(b, fixture_crc(b));
    return b;
}

static Bytes fixture32(bool hash)
{
    Bytes b = header(0xe9, 2, 0x40080000);
    b.append(16, '\0'); b[23] = hash ? 1 : 0;
    u8 sum = 0xef;
    segment(b, 0x40080000, Bytes("\x36\x41\0\x08", 4), sum);
    segment(b, 0x3ffb0000, Bytes("DATA", 4), sum);
    footer(b, sum);
    if (hash) b += sha256(b);
    return b;
}

static Bytes partition(u8 type, u8 subtype, u32 offset, u32 size, const char *name)
{
    Bytes b(32, '\0'); wr16le(&b[0], 0x50aa); b[2] = type; b[3] = subtype;
    wr32le(&b[4], offset); wr32le(&b[8], size);
    b.replace(12, std::string(name).size(), name); return b;
}

static bool test_valid_images()
{
    esp::Image im; std::string err;
    ESP_CHECK(esp::parse_image(fixture8266(false), 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(im.chip == esp::CHIP_ESP8266 && im.version == 1 && im.checksum_valid);
    ESP_CHECK(im.entry_point == 0x40100000 && im.segments.size() == 2);
    ESP_CHECK(im.segments[0].file_offset == 16 && im.segments[1].data == "DATA");
    Bytes v2 = Bytes(0x1000, (char)0xff) + fixture8266(true);
    ESP_CHECK(esp::parse_image(v2, 0x1000, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(im.version == 2 && im.segments.size() == 3 && im.crc_valid);
    ESP_CHECK(im.segments[0].mapped && im.segments[0].address == 0x40201010);
    ESP_CHECK(im.segments[0].file_offset == 0x1010 && im.segments[0].data == Bytes(32, '\x5a'));
    v2 = Bytes(0x101000, (char)0xff) + fixture8266(true);
    ESP_CHECK(esp::parse_image(v2, 0x101000, esp::CHIP_ESP8266, im, &err));
    ESP_CHECK(im.segments[0].address == 0x40201010 && im.segments[0].file_offset == 0x101010);
    ESP_CHECK(esp::parse_image(fixture32(true), 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(im.chip == esp::CHIP_ESP32 && im.hash_appended && im.hash_valid);
    ESP_CHECK(im.segments[0].file_offset == 32 && im.image_size == fixture32(true).size());
    ESP_CHECK(esp::parse_image(fixture32(false), 0, esp::CHIP_ESP32, im, &err));
    ESP_CHECK(!im.hash_appended && !im.hash_valid);
    Bytes mapped_entry = fixture32(false); wr32le(&mapped_entry[4], 0x40100000);
    ESP_CHECK(esp::parse_image(mapped_entry, 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(im.chip == esp::CHIP_ESP32); /* IROM entry ranges overlap */
    Bytes unaligned = Bytes("pad") + fixture32(true);
    ESP_CHECK(esp::parse_image(unaligned, 3, esp::CHIP_ESP32, im, &err));
    ESP_CHECK(im.hash_valid && im.segments[0].file_offset == 35);
    return true;
}

static bool test_corruption()
{
    esp::Image im; std::string err;
    const Bytes images[] = { fixture8266(false), fixture8266(true), fixture32(true) };
    for (int n = 0; n < 3; ++n) {
        for (size_t len = 0; len < images[n].size(); ++len) {
            ESP_CHECK(!esp::parse_image(images[n].substr(0, len), 0, esp::CHIP_UNKNOWN, im, &err));
            ESP_CHECK(im.segments.empty() && !err.empty());
        }
    }
    Bytes b = fixture8266(false); b[16] ^= 1;
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_ESP8266, im, &err));
    ESP_CHECK(err.find("checksum") != std::string::npos);
    b = fixture8266(true); b[16] ^= 1; /* v2 IROM excluded from XOR, covered by CRC */
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(err.find("CRC") != std::string::npos);
    b = fixture32(true); b[b.size() - 1] ^= 1;
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_ESP32, im, &err));
    ESP_CHECK(err.find("SHA-256") != std::string::npos);
    b = fixture8266(false); wr32le(&b[12], 0xffffffff);
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    b = fixture8266(false); wr32le(&b[8], 0xfffffffc);
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    b = fixture32(false); b[1] = 17;
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(!esp::parse_image(fixture32(false), (size_t)-1, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(!esp::parse_image(fixture8266(false), 0, esp::CHIP_ESP32, im, &err));
    b = fixture32(false); wr16le(&b[12], 5); wr32le(&b[4], 0x40380000);
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(err.find("chip ID 5") != std::string::npos);
    b = fixture32(false); wr16le(&b[12], 9);
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_ESP32, im, &err));
    ESP_CHECK(err.find("chip ID 9") != std::string::npos);
    b = fixture32(false); b[23] = 2;
    ESP_CHECK(!esp::parse_image(b, 0, esp::CHIP_UNKNOWN, im, &err));
    ESP_CHECK(err.find("hash-appended") != std::string::npos);
    return true;
}

static bool test_flash_layout()
{
    esp::FlashImage flash; std::string err;
    Bytes b(0x20000, (char)0xff), boot = fixture32(false), app = fixture32(true);
    b.replace(0x1000, boot.size(), boot); b.replace(0x10000, app.size(), app);
    Bytes table = partition(1, 2, 0x9000, 0x6000, "nvs") + partition(0, 0, 0x10000, 0x10000, "factory");
    Bytes digest(16, (char)0xff); wr16le(&digest[0], 0xebeb); digest += md5(table);
    table += digest; b.replace(0x8000, table.size(), table);
    ESP_CHECK(esp::parse_flash(b, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(flash.chip == esp::CHIP_ESP32 && flash.images.size() == 2);
    ESP_CHECK(flash.boot_index == 0 && flash.app_index == 1 && flash.partitions.size() == 2);
    ESP_CHECK(flash.images[flash.app_index].file_offset == 0x10000);
    ESP_CHECK(flash.partitions[1].label == "factory" && flash.partition_table_offset == 0x8000);
    Bytes invalid = b; wr32le(&invalid[0x8028], 0xffffffff);
    ESP_CHECK(!esp::parse_flash(invalid, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(err.find("partition exceeds") != std::string::npos && flash.images.empty());
    invalid = b; wr32le(&invalid[0x8024], 0xa000);
    ESP_CHECK(!esp::parse_flash(invalid, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(err.find("overlapping") != std::string::npos);
    b[0x8050] ^= 1;
    ESP_CHECK(!esp::parse_flash(b, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(err.find("MD5") != std::string::npos);
    b = Bytes(0x1000, (char)0xff); boot = fixture8266(false);
    b.replace(0, boot.size(), boot); b += fixture8266(true);
    ESP_CHECK(esp::parse_flash(b, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(flash.boot_index == 0 && flash.app_index == 1 && flash.images.size() == 2);
    b[0x1010] ^= 1;
    ESP_CHECK(!esp::parse_flash(b, esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(flash.images.empty() && err.find("CRC") != std::string::npos);
    ESP_CHECK(esp::parse_flash(fixture8266(true), esp::CHIP_UNKNOWN, flash, &err));
    ESP_CHECK(flash.app_index == 0 && flash.boot_index == 0);
    ESP_CHECK(!esp::parse_flash(Bytes(4096, (char)0xff), esp::CHIP_UNKNOWN, flash, &err));
    return true;
}

int esp_image_tests()
{
    esp_checks = 0;
    if (!test_valid_images() || !test_corruption() || !test_flash_layout()) return -1;
    printf("[PASS] ESP image bounds, integrity, architecture and flash layout\n");
    return esp_checks;
}

static bool dump_fixture(const EspFixture &fixture)
{
    const char *base = getenv("ESP_FLASH_DUMPS");
    std::string path = std::string(base ? base : "../FlashDumps") + "/" + fixture.relative_path;
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        /* Portable CI never needs a private flash dump. Supplying the env var
         * requests this integration test explicitly and makes absence fail. */
        ESP_CHECK(!base);
        printf("[SKIP] external %s image fixture unavailable (set ESP_FLASH_DUMPS)\n", fixture.label);
        return true;
    }
    Bytes raw((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    esp::FlashImage flash; std::string err;
    ESP_CHECK(raw.size() == fixture.flash_size);
    ESP_CHECK(hexstr(sha256(raw)) == fixture.sha256);
    if (!esp::parse_flash(raw, esp::CHIP_UNKNOWN, flash, &err)) {
        printf("[FAIL] external %s image: %s\n", fixture.label, err.c_str());
        return false;
    }
    ESP_CHECK(flash.chip == fixture.chip && flash.images.size() >= 2);
    ESP_CHECK(flash.boot_index >= 0 && flash.app_index >= 0);
    const esp::Image &boot = flash.images[flash.boot_index];
    const esp::Image &app = flash.images[flash.app_index];
    ESP_CHECK(boot.entry_point == fixture.boot_entry && app.entry_point == fixture.app_entry);
    ESP_CHECK(app.file_offset == fixture.app_offset && app.segments.size() == fixture.app_segments);
    ESP_CHECK(boot.checksum_valid && app.checksum_valid);
    if (fixture.chip == esp::CHIP_ESP8266) {
        ESP_CHECK(app.version == 2 && app.crc_valid && app.segments[0].mapped);
        ESP_CHECK(app.segments[0].address == 0x40201010);
    } else {
        ESP_CHECK(boot.hash_valid && app.hash_valid && flash.partitions.size() == 5);
        ESP_CHECK(flash.partitions[3].subtype == 0x10 && flash.partitions[3].offset == 0x20000);
        ESP_CHECK(app.segments[0].mapped && app.segments[4].mapped);
    }
    for (size_t i = 0; i < app.segments.size(); ++i) {
        const esp::Segment &s = app.segments[i];
        ESP_CHECK(s.data == raw.substr(s.file_offset, s.data.size()));
    }
    printf("[PASS] external %s image segments and integrity (execution tested separately)\n", fixture.label);
    return true;
}

int esp_image_dump_tests()
{
    esp_checks = 0;
    for (size_t i = 0; i < ESP_FIXTURE_COUNT; ++i)
        if (!dump_fixture(ESP_FIXTURES[i])) return -1;
    return esp_checks;
}

#ifdef ESP_IMAGE_TEST_MAIN
int main() { return esp_image_tests() < 0 || esp_image_dump_tests() < 0 ? 1 : 0; }
#endif
