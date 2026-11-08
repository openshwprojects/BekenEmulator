#include "esp_image.h"
#include "crypto_prims.h"

/* Format references (independent implementation): Espressif esptool image
 * format documentation and ESP8266V2FirmwareImage in esptool/bin_image.py.
 * https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/firmware-image-format.html
 * https://github.com/espressif/esptool/blob/master/esptool/bin_image.py
 */

namespace esp {
namespace {

bool fail(std::string *err, const std::string &message)
{
    if (err) *err = message;
    return false;
}

bool fits(const Bytes &data, size_t offset, size_t length)
{
    return offset <= data.size() && length <= data.size() - offset;
}

bool within(u32 address, size_t length, u32 start, u32 end)
{
    return address >= start && address < end && length <= (size_t)(end - address);
}

bool mapped_address(Chip chip, u32 address)
{
    if (chip == CHIP_ESP8266) return address >= 0x40200000 && address < 0x40300000;
    return (address >= 0x3f400000 && address < 0x3f800000) ||
           (address >= 0x400d0000 && address < 0x40400000);
}

bool valid_segment(Chip chip, u32 address, size_t length)
{
    if (address == 0) return chip == CHIP_ESP32; /* MMU alignment padding */
    if (chip == CHIP_ESP8266)
        return within(address, length, 0x3ffe0000, 0x40000000) ||
               within(address, length, 0x40100000, 0x40110000) ||
               within(address, length, 0x40200000, 0x40300000);
    return within(address, length, 0x3f400000, 0x40000000) ||
           within(address, length, 0x40000000, 0x40400000) ||
           within(address, length, 0x50000000, 0x50002000);
}

Chip detect_chip(const Bytes &data, size_t offset)
{
    if (!fits(data, offset, 8)) return CHIP_UNKNOWN;
    if (bat(data, offset) == 0xea) return CHIP_ESP8266;
    if (bat(data, offset) != 0xe9) return CHIP_UNKNOWN;
    /* ESP32 IROM overlaps ESP8266's entry range, so the entry point alone
     * cannot identify the chip. ESP8266 immediately stores a RAM segment;
     * ESP32 stores its SPI-pin/drive header before the first segment. */
    if (fits(data, offset, 16)) {
        u32 first_address = rd32le(data.data() + offset + 8);
        if (valid_segment(CHIP_ESP8266, first_address, 0)) return CHIP_ESP8266;
    }
    return CHIP_ESP32;
}

u32 esp8266_crc(const Bytes &data, size_t start, size_t length)
{
    u32 crc = 0xffffffff;
    for (size_t n = 0; n < length; ++n) {
        crc ^= bat(data, start + n);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc ^= 0xffffffff;
    return (crc & 0x80000000) ? (crc ^ 0xffffffff) : crc + 1;
}

bool load_segment(const Bytes &data, size_t &cursor, Image &im, u8 *checksum,
                  bool v2_irom, std::string *err)
{
    if (!fits(data, cursor, 8)) return fail(err, "truncated ESP segment header");
    Segment s;
    s.address = rd32le(data.data() + cursor);
    u32 length = rd32le(data.data() + cursor + 4);
    cursor += 8;
    if (!fits(data, cursor, length)) return fail(err, "ESP segment exceeds image bounds");
    if (length >= 0x1000000) return fail(err, "ESP segment exceeds 16 MiB limit");
    s.file_offset = cursor;
    if (v2_irom) {
        if (s.address != 0) return fail(err, "ESP8266 v2 IROM load address must be zero");
        /* The cache maps one megabyte of flash at a time. OTA images in later
         * megabytes use the same virtual window after cache remapping. */
        s.address = 0x40200000 + (u32)(cursor & 0xfffff);
        s.mapped = true;
    } else s.mapped = mapped_address(im.chip, s.address);
    if (!valid_segment(im.chip, s.address, length))
        return fail(err, "ESP segment address is outside supported chip memory");
    s.data.assign(data, cursor, length);
    if (checksum)
        for (size_t n = 0; n < length; ++n) *checksum ^= bat(data, cursor + n);
    im.segments.push_back(s);
    cursor += length;
    return true;
}

bool parse_partition_table(const Bytes &data, size_t offset,
                           std::vector<Partition> &parts, std::string *err)
{
    size_t cursor = offset;
    for (unsigned n = 0; n < 96; ++n, cursor += 32) {
        if (!fits(data, cursor, 32)) return fail(err, "truncated ESP32 partition table");
        u16 magic = rd16le(data.data() + cursor);
        if (magic == 0xffff) return !parts.empty();
        if (magic == 0xebeb) {
            if (md5(data.substr(offset, cursor - offset)) != data.substr(cursor + 16, 16))
                return fail(err, "ESP32 partition table MD5 mismatch");
            return !parts.empty();
        }
        if (magic != 0x50aa) return fail(err, "invalid ESP32 partition table entry");
        Partition p;
        p.type = bat(data, cursor + 2); p.subtype = bat(data, cursor + 3);
        p.offset = rd32le(data.data() + cursor + 4);
        p.size = rd32le(data.data() + cursor + 8);
        p.flags = rd32le(data.data() + cursor + 28);
        if (!p.size || !fits(data, p.offset, p.size))
            return fail(err, "ESP32 partition exceeds flash bounds");
        if (p.offset & 0xfff) return fail(err, "ESP32 partition offset is not sector aligned");
        for (size_t i = 0; i < parts.size(); ++i)
            if ((u64)p.offset < (u64)parts[i].offset + parts[i].size &&
                (u64)parts[i].offset < (u64)p.offset + p.size)
                return fail(err, "overlapping ESP32 partitions");
        size_t len = 0;
        while (len < 16 && data[cursor + 12 + len]) ++len;
        p.label.assign(data, cursor + 12, len);
        parts.push_back(p);
    }
    return fail(err, "unterminated ESP32 partition table");
}

int image_at(const FlashImage &flash, size_t offset)
{
    for (size_t i = 0; i < flash.images.size(); ++i)
        if (flash.images[i].file_offset == offset) return (int)i;
    return -1;
}

} // namespace

const char *chip_name(Chip chip)
{
    switch (chip) {
    case CHIP_ESP8266: return "ESP8266";
    case CHIP_ESP32: return "ESP32";
    default: return "unknown";
    }
}

bool parse_image(const Bytes &data, size_t offset, Chip expected, Image &out, std::string *err)
{
    out = Image();
    if (err) err->clear();
    if (!fits(data, offset, 8)) return fail(err, "truncated ESP image header");
    Image im;
    im.chip = detect_chip(data, offset);
    if (im.chip == CHIP_UNKNOWN) return fail(err, "invalid ESP image magic");
    if (expected != CHIP_UNKNOWN && expected != im.chip)
        return fail(err, "ESP image architecture does not match selected chip");
    im.file_offset = offset;
    im.version = bat(data, offset) == 0xea ? 2 : 1;
    im.entry_point = rd32le(data.data() + offset + 4);
    unsigned count = bat(data, offset + 1);
    size_t cursor = offset + 8;
    if (im.chip == CHIP_ESP32) {
        if (!fits(data, offset, 24)) return fail(err, "truncated ESP32 extended header");
        unsigned chip_id = rd16le(data.data() + offset + 12);
        if (chip_id != 0)
            return fail(err, strf("unsupported ESP32-family chip ID %u (classic ESP32 requires ID 0)", chip_id));
        unsigned hashed = bat(data, offset + 23);
        if (hashed > 1) return fail(err, "invalid ESP32 hash-appended flag");
        im.hash_appended = hashed != 0;
        cursor = offset + 24;
    } else if (im.version == 2) {
        if (count != 4) return fail(err, "invalid ESP8266 v2 header marker");
        if (!load_segment(data, cursor, im, 0, true, err)) return false;
        if (!fits(data, cursor, 8)) return fail(err, "truncated ESP8266 v2 inner header");
        if (bat(data, cursor) != 0xe9) return fail(err, "invalid ESP8266 v2 inner header magic");
        /* Espressif's loader takes the entry point from the second header. */
        count = bat(data, cursor + 1);
        im.entry_point = rd32le(data.data() + cursor + 4);
        cursor += 8;
    }
    if (!count || count > 16 || count + im.segments.size() > 16)
        return fail(err, "invalid ESP image segment count");
    bool entry_ok = im.chip == CHIP_ESP8266 ?
        ((im.entry_point >= 0x40100000 && im.entry_point < 0x40110000) ||
         (im.entry_point >= 0x40200000 && im.entry_point < 0x40300000)) :
        ((im.entry_point >= 0x40000000 && im.entry_point < 0x40400000) ||
         (im.entry_point >= 0x50000000 && im.entry_point < 0x50002000));
    if (!entry_ok) return fail(err, "ESP entry point is outside executable chip memory");
    u8 checksum = 0xef;
    for (unsigned n = 0; n < count; ++n)
        if (!load_segment(data, cursor, im, &checksum, false, err)) return false;
    size_t padding = 15 - ((cursor - offset) % 16);
    if (!fits(data, cursor, padding + 1)) return fail(err, "truncated ESP checksum footer");
    cursor += padding;
    if (bat(data, cursor) != checksum) return fail(err, "ESP image checksum mismatch");
    im.checksum_valid = true;
    ++cursor;
    if (im.hash_appended) {
        if (!fits(data, cursor, 32)) return fail(err, "truncated ESP32 SHA-256 digest");
        Sha256 hash; hash.update((const u8 *)data.data() + offset, cursor - offset);
        if (hash.digest() != data.substr(cursor, 32)) return fail(err, "ESP32 image SHA-256 mismatch");
        im.hash_valid = true;
        cursor += 32;
    }
    if (im.version == 2) {
        im.crc_appended = true;
        if (!fits(data, cursor, 4)) return fail(err, "truncated ESP8266 v2 CRC footer");
        if (rd32le(data.data() + cursor) != esp8266_crc(data, offset, cursor - offset))
            return fail(err, "ESP8266 v2 image CRC mismatch");
        im.crc_valid = true;
        cursor += 4;
    }
    im.image_size = cursor - offset;
    out = im;
    return true;
}

bool parse_flash(const Bytes &data, Chip expected, FlashImage &out, std::string *err)
{
    out = FlashImage();
    if (err) err->clear();
    FlashImage flash;
    std::string first_error;
    for (size_t offset = 0; fits(data, offset, 8); offset += 0x1000) {
        if (bat(data, offset) != 0xe9 && bat(data, offset) != 0xea) continue;
        Image im; std::string image_error;
        Chip wanted = flash.chip == CHIP_UNKNOWN ? expected : flash.chip;
        if (!parse_image(data, offset, wanted, im, &image_error)) {
            if (first_error.empty()) first_error = image_error;
            /* A malformed primary header must not silently become an app-only
             * boot. The rest of flash may contain stale or coincidental E9. */
            if (offset == 0 || (offset == 0x1000 &&
                (flash.images.empty() || flash.chip == CHIP_ESP8266)))
                return fail(err, image_error);
            continue;
        }
        if (flash.chip == CHIP_UNKNOWN) flash.chip = im.chip;
        flash.images.push_back(im);
        size_t image_end = offset + im.image_size;
        offset = (image_end - 1) & ~(size_t)0xfff;
    }
    if (flash.images.empty()) return fail(err, first_error.empty() ? "no supported ESP image found" : first_error);
    if (flash.chip == CHIP_ESP32) {
        /* CONFIG_PARTITION_TABLE_OFFSET can move the default 0x8000 table.
         * Tables occur outside validated images and before application data. */
        for (size_t offset = 0x8000; fits(data, offset, 32) && offset < 0x100000; offset += 0x1000) {
            if (rd16le(data.data() + offset) != 0x50aa) continue;
            bool in_image = false;
            for (size_t i = 0; i < flash.images.size(); ++i)
                if (offset >= flash.images[i].file_offset &&
                    offset - flash.images[i].file_offset < flash.images[i].image_size)
                    in_image = true;
            if (in_image) continue;
            if (!parse_partition_table(data, offset, flash.partitions, err)) return false;
            flash.partition_table_offset = offset;
            break;
        }
        flash.boot_index = image_at(flash, 0x1000);
        int ota = -1;
        for (size_t p = 0; p < flash.partitions.size(); ++p) {
            const Partition &part = flash.partitions[p];
            if (part.type != 0) continue;
            int index = image_at(flash, part.offset);
            if (index < 0) {
                if (bat(data, part.offset) == 0xe9) {
                    Image unused;
                    if (!parse_image(data, part.offset, flash.chip, unused, err)) return false;
                }
                continue;
            }
            if (flash.images[index].image_size > part.size)
                return fail(err, "ESP32 application image exceeds its partition");
            if (part.subtype == 0) flash.app_index = index;
            else if (part.subtype >= 0x10 && part.subtype <= 0x1f && ota < 0) ota = index;
        }
        if (flash.app_index < 0) flash.app_index = ota;
    } else flash.boot_index = image_at(flash, 0);
    if (flash.app_index < 0) {
        for (size_t i = 0; i < flash.images.size(); ++i)
            if ((int)i != flash.boot_index) { flash.app_index = (int)i; break; }
    }
    if (flash.boot_index < 0) flash.boot_index = 0;
    if (flash.app_index < 0) flash.app_index = flash.boot_index;
    out = flash;
    return true;
}

} // namespace esp
