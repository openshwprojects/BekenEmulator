/* Espressif on-flash images. Parsing never executes firmware or reads outside
 * the supplied dump. Offsets are physical offsets in that dump, not addresses. */
#ifndef BK_ESP_IMAGE_H
#define BK_ESP_IMAGE_H

#include "common.h"

namespace esp {

enum Chip { CHIP_UNKNOWN, CHIP_ESP8266, CHIP_ESP32 };

struct Segment {
    u32 address;
    size_t file_offset; /* first data byte, excluding the segment header */
    Bytes data;
    bool mapped;       /* flash cache mapping, rather than RAM initialization */
    Segment() : address(0), file_offset(0), mapped(false) {}
};

struct Image {
    Chip chip;
    size_t file_offset, image_size;
    u32 entry_point;
    unsigned version;
    bool checksum_valid, hash_appended, hash_valid, crc_appended, crc_valid;
    std::vector<Segment> segments;
    Image() : chip(CHIP_UNKNOWN), file_offset(0), image_size(0), entry_point(0),
        version(0), checksum_valid(false), hash_appended(false), hash_valid(false),
        crc_appended(false), crc_valid(false) {}
};

struct Partition {
    u8 type, subtype;
    u32 offset, size, flags;
    std::string label;
    Partition() : type(0), subtype(0), offset(0), size(0), flags(0) {}
};

struct FlashImage {
    Chip chip;
    std::vector<Image> images;
    std::vector<Partition> partitions;
    int boot_index, app_index;
    size_t partition_table_offset;
    FlashImage() : chip(CHIP_UNKNOWN), boot_index(-1), app_index(-1),
        partition_table_offset(0) {}
};

const char *chip_name(Chip chip);

/* CHIP_UNKNOWN detects ESP8266 versus classic ESP32. Other ESP32 family
 * chip IDs are rejected explicitly, rather than executed on the wrong ISA.
 * Checksums, optional SHA-256 and ESP8266 v2 CRC are mandatory when present.
 * On failure output is reset and err contains a reason, never firmware data. */
bool parse_image(const Bytes &data, size_t offset, Chip expected, Image &out,
                 std::string *err = 0);

/* Accepts full flash or a standalone image. Full-flash app_index is a valid
 * factory app, or the first valid OTA app; OTA selection state is interpreted
 * by the real bootloader, not guessed by this parser. A standalone image is
 * both boot_index and app_index. ESP32 zero-address padding is retained as
 * segments; a loader must skip it. ESP8266 v1 raw IROM is not part of the
 * segment list and must be mapped from the original flash by the machine. */
bool parse_flash(const Bytes &data, Chip expected, FlashImage &out,
                 std::string *err = 0);

} // namespace esp
#endif
