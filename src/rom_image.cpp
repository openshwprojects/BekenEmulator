#include "rom_image.h"

namespace {
bool fail(std::vector<RomSegment> &segments, std::string *error, const char *message)
{
    segments.clear();
    if (error) *error = message;
    return false;
}
bool overlap(u32 a, size_t as, u32 b, size_t bs)
{
    return (u64)a < (u64)b + bs && (u64)b < (u64)a + as;
}
}

bool parse_elf_rom(const Bytes &bytes, u16 machine, std::vector<RomSegment> &segments,
                   std::string *error)
{
    segments.clear();
    if (error) error->clear();
    if (bytes.size() < 52 || bytes.compare(0, 4, "\x7f" "ELF", 4) != 0)
        return fail(segments, error, "ROM is not an ELF image.");
    const u8 *b = (const u8 *)bytes.data();
    if (b[4] != 1 || b[5] != 1 || b[6] != 1 || rd32le(b + 20) != 1)
        return fail(segments, error, "ROM requires ELF32 little-endian version 1.");
    if (rd16le(b + 16) != 2 || rd16le(b + 18) != machine)
        return fail(segments, error, "ROM ELF executable architecture does not match the CPU.");
    u32 phoff = rd32le(b + 28);
    unsigned phsize = rd16le(b + 42), count = rd16le(b + 44);
    if (rd16le(b + 40) < 52 || phsize < 32 || !count || count > 1024
        || (u64)phoff + (u64)phsize * count > bytes.size())
        return fail(segments, error, "Invalid ROM ELF program header table.");
    size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        const u8 *p = b + phoff + (size_t)i * phsize;
        if (rd32le(p) != 1) continue; /* PT_LOAD */
        u32 offset = rd32le(p + 4), address = rd32le(p + 8), physical = rd32le(p + 12);
        u32 size = rd32le(p + 16), memory = rd32le(p + 20), flags = rd32le(p + 24);
        if (size > memory || (u64)offset + size > bytes.size()
            || (u64)address + memory > 0x100000000ull || (u64)physical + memory > 0x100000000ull)
            return fail(segments, error, "ROM ELF segment exceeds file or address bounds.");
        if ((flags & 2) || !size) continue; /* Skip RAM and symbols-only loads. */
        if (!(flags & 4) || size != memory || size > 16 * 1024 * 1024 || total + size > 16 * 1024 * 1024)
            return fail(segments, error, "Invalid read-only ROM ELF segment.");
        for (size_t n = 0; n < segments.size(); ++n) {
            const RomSegment &s = segments[n];
            if (overlap(address, size, s.address, s.data.size())
                || overlap(physical, size, s.physical_address, s.data.size())
                || overlap(address, size, s.physical_address, s.data.size())
                || overlap(physical, size, s.address, s.data.size()))
                return fail(segments, error, "ROM ELF segments overlap.");
        }
        RomSegment segment;
        segment.address = address; segment.physical_address = physical;
        segment.executable = (flags & 1) != 0;
        segment.data.assign(bytes, offset, size);
        segments.push_back(segment);
        total += size;
    }
    if (segments.empty()) return fail(segments, error, "ROM ELF contains no instruction or ROM data bytes.");
    /* The published Espressif ROM ELF carries initialized ROM-library RAM as
     * non-ALLOC .data_* sections. Unlike writable PT_LOAD headers, these are
     * actual initializer bytes (function tables, libc and Xtensa state). */
    u32 shoff = rd32le(b + 32);
    unsigned shsize = rd16le(b + 46), shcount = rd16le(b + 48), shstrings = rd16le(b + 50);
    if (shcount) {
        if (shsize < 40 || shstrings >= shcount || shcount > 4096
            || (u64)shoff + (u64)shsize * shcount > bytes.size())
            return fail(segments, error, "Invalid ROM ELF section header table.");
        const u8 *strings = b + shoff + (size_t)shstrings * shsize;
        u32 names = rd32le(strings + 16), names_size = rd32le(strings + 20);
        if (rd32le(strings + 4) != 3 || (u64)names + names_size > bytes.size())
            return fail(segments, error, "Invalid ROM ELF section names.");
        for (unsigned i = 0; i < shcount; ++i) {
            const u8 *s = b + shoff + (size_t)i * shsize;
            u32 name = rd32le(s), address = rd32le(s + 12), offset = rd32le(s + 16), size = rd32le(s + 20);
            if (name >= names_size)
                return fail(segments, error, "Invalid ROM ELF section name offset.");
            size_t end = bytes.find('\0', (size_t)names + name);
            if (end == Bytes::npos || end >= (u64)names + names_size)
                return fail(segments, error, "Unterminated ROM ELF section name.");
            std::string label = bytes.substr((size_t)names + name, end - names - name);
            if (label != ".data" && label.compare(0, 6, ".data_") != 0) continue;
            if (rd32le(s + 4) != 1 || !size) continue; /* PROGBITS only */
            if (!address || (rd32le(s + 8) & 4) || (u64)offset + size > bytes.size()
                || (u64)address + size > 0x100000000ull || size > 0x100000 || total + size > 0x1000000)
                return fail(segments, error, "Invalid ROM ELF RAM initializer.");
            for (size_t n = 0; n < segments.size(); ++n)
                if (overlap(address, size, segments[n].address, segments[n].data.size()))
                    return fail(segments, error, "ROM ELF RAM initializers overlap.");
            RomSegment initializer;
            initializer.address = initializer.physical_address = address;
            initializer.initialize_ram = true;
            initializer.data.assign(bytes, offset, size);
            segments.push_back(initializer);
            total += size;
        }
    }
    return true;
}
