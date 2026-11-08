#include "esp/esp_machine.h"

bool EspMachine::load_rom(const Bytes &image,std::string *error)
{
    std::vector<RomSegment> segments;
    if(chip_==esp::CHIP_ESP8266 && image.size()==0x10000 && image.compare(0,4,"\x7f" "ELF")!=0) {
        if(image.compare(0x80,3,"\x06\x08\x00",3)!=0) {
            if(error)*error="Unrecognized ESP8266 raw mask-ROM reset vector.";
            return false;
        }
        RomSegment segment;
        segment.address=segment.physical_address=0x40000000;
        segment.executable=true;segment.data=image;segments.push_back(segment);
    } else if(!parse_elf_rom(image,94,segments,error)) return false;
    return load_rom(segments,error);
}

namespace {
bool inside(u32 address, size_t size, u32 low, u32 high)
{
    return address >= low && (u64)address + size <= high;
}
bool valid_rom_address(esp::Chip chip, u32 address, size_t size)
{
    if (chip == esp::CHIP_ESP32)
        return inside(address, size, 0x40000000, 0x40070000)
            || inside(address, size, 0x3ff90000, 0x3ffa0000);
    return inside(address, size, 0x40000000, 0x40010000);
}

bool rom_slice(const std::vector<RomSegment> &segments, u32 address, size_t size, Bytes &out)
{
    for (size_t i = 0; i < segments.size(); ++i) {
        const RomSegment &s = segments[i];
        if (s.initialize_ram) continue;
        const u32 bases[] = {s.address, s.physical_address};
        for (unsigned n = 0; n < 2; ++n)
            if (address >= bases[n] && (u64)address + size <= (u64)bases[n] + s.data.size()) {
                out.assign(s.data, address - bases[n], size);
                return true;
            }
    }
    return false;
}

void lx6_store_section(std::vector<RomSegment> &segments,u32 start,u32 end,u32 source)
{
    if(start>=end) return;
    for(size_t i=0;i<segments.size();++i) {
        const RomSegment &s=segments[i];
        if(!s.initialize_ram || s.address!=start || (u64)start+s.data.size()<end) continue;
        Bytes existing;
        if(rom_slice(segments,source,s.data.size(),existing))return;
        RomSegment stored;
        stored.address=stored.physical_address=source;stored.data=s.data;
        segments.push_back(stored);
        return;
    }
}

bool lx6_rom_stores(std::vector<RomSegment> &segments,std::string *error)
{
    Bytes bytes;
    /* Espressif's ELF represents ROM initializer storage as non-ALLOC RAM
     * sections. Recover the original read-only bus addresses from the ROM's
     * own reset copy table, so either core executes the actual unpack loop.
     * The two L32R instructions identify the table-bound literals. */
    if(!rom_slice(segments,0x400004fb,6,bytes) || bytes!=H("41 ca ff 51 ca ff"))return true;
    if(!rom_slice(segments,0x40000424,8,bytes))return false;
    const u32 table=rd32le((const u8 *)bytes.data()),end=rd32le((const u8 *)bytes.data()+4);
    if(table>end || ((end-table)&15) || end-table>16384 ||
       !inside(table,end-table,0x40000000,0x40070000)) {
        if(error)*error="Invalid LX6 ROM initializer table bounds.";
        return false;
    }
    for(u32 p=table;p<end;p+=16) {
        if(!rom_slice(segments,p,16,bytes)) {
            if(error)*error="Missing LX6 ROM initializer table bytes.";
            return false;
        }
        const u8 *b=(const u8 *)bytes.data();
        u32 first=rd32le(b),last=rd32le(b+4),source=rd32le(b+8),always=rd32le(b+12);
        if(first>last || ((first|source)&3) || always>1 ||
           !inside(first,last-first,0x3ffae000,0x40000000) ||
           !inside(source,last-first,0x40000000,0x40070000)) {
            if(error)*error="Invalid LX6 ROM initializer table entry.";
            return false;
        }
        /* Empty entries share the following section's source. Do not invent
         * padding: mapping that section supplies their real first word. */
        lx6_store_section(segments,first,last,source);
    }
    /* BTDM initializes separately. Its (start,end,source) descriptor directly
     * precedes the reset table and identifies the remaining ROM store bytes. */
    if(table>=0x4000000c && rom_slice(segments,table-12,12,bytes)) {
        const u8 *b=(const u8 *)bytes.data();
        u32 first=rd32le(b),last=rd32le(b+4),source=rd32le(b+8);
        if(first<last && inside(first,last-first,0x3ffae000,0x40000000) &&
           inside(source,last-first,0x40000000,0x40070000))
            lx6_store_section(segments,first,last,source);
    }
    return true;
}

bool lx6_private_rom_views(const std::vector<RomSegment> &segments)
{
    /* The classic ESP32 exposes core-specific mask-ROM literals. Espressif's
     * official PRO/APP ROM pair differs only in private-RAM pointer words:
     * every aligned word in 3ffe0010..3ffe3f20 gains 3f10 on APP. Recognize the
     * shared rev0/rev3 startup layout before applying that hardware view to
     * ELF files, which publish only the PRO instruction image. */
    Bytes b;
    if(!rom_slice(segments,0x400004fb,6,b) || b!=H("41 ca ff 51 ca ff"))return false;
    if(!rom_slice(segments,0x40000424,8,b) ||
       rd32le((const u8 *)b.data())!=0x4000d4f8 ||
       rd32le((const u8 *)b.data()+4)!=0x4000d5c8)return false;
    if(!rom_slice(segments,0x40000560,4,b) || rd32le((const u8 *)b.data())!=0x3ffe3f20)return false;
    if(!rom_slice(segments,0x40000580,8,b) ||
       rd32le((const u8 *)b.data())!=0x3ffe0548 ||
       rd32le((const u8 *)b.data()+4)!=0x3ffe0448)return false;
    return rom_slice(segments,0x4000d528,8,b) &&
        rd32le((const u8 *)b.data())==0x3ffe0010 &&
        rd32le((const u8 *)b.data()+4)==0x3ffe0010;
}

void lx6_app_rom_view(u32 address,Bytes &bytes)
{
    for(size_t i=(0u-address)&3;i+4<=bytes.size();i+=4) {
        const u32 value=rd32le((const u8 *)bytes.data()+i);
        if(value>=0x3ffe0010 && value<=0x3ffe3f20)wr32le(&bytes[i],value+0x3f10);
    }
}

void lx6_flash_handoff(std::vector<RomSegment> &segments,u32 cpu_mhz)
{
    Bytes code,literal;
    /* We enter flash after ROM clock setup, whereas ELF .data initializers
     * describe mask-reset state. Keep ets_delay_us consistent with that
     * handoff. Identify ets_update_cpu_frequency (ENTRY; L32R; S32I; RETW)
     * and derive its PRO RAM variable from the actual ROM literal. APP reset
     * still executes its own initialization and later frequency updates. */
    const u32 updater=0x40008550;
    if(!rom_slice(segments,updater,10,code) || code.compare(0,4,H("36 41 00 81")) ||
       code.compare(6,4,H("29 08 1d f0")))return;
    const u32 displacement=0xfffc0000u | (u32(rd16le((const u8 *)code.data()+4))<<2);
    if(!rom_slice(segments,((updater+6)&~3u)+displacement,4,literal))return;
    const u32 variable=rd32le((const u8 *)literal.data());
    if((variable&3) || !inside(variable,4,0x3ffae000,0x40000000))return;
    RomSegment initializer;
    initializer.address=initializer.physical_address=variable;
    initializer.initialize_ram=true;initializer.data.assign(4,'\0');
    wr32le(&initializer.data[0],cpu_mhz);
    segments.push_back(initializer);
}

bool lx106_initializers(std::vector<RomSegment> &segments, std::string *error)
{
    Bytes bytes;
    /* The LX106 reset vector jumps to its startup code at 0x400000a4.
     * That code reads this literal and copies (start,end,source) triples.
     * Decode the ROM's own table, including its word-rounded last copy. */
    if (!rom_slice(segments, 0x40000080, 3, bytes) || bytes != H("06 08 00")) return true;
    if (!rom_slice(segments, 0x40000098, 4, bytes)) return false;
    u32 table = rd32le((const u8 *)bytes.data());
    for (unsigned i = 0; i < 32; ++i) {
        if ((u64)table + i * 12 + 12 > 0x100000000ull || !rom_slice(segments, table + i * 12, 12, bytes)) break;
        const u8 *p = (const u8 *)bytes.data();
        u32 start = rd32le(p), end = rd32le(p + 4), source = rd32le(p + 8);
        if (!start && !end && !source) return true;
        if (start == end) continue; /* Empty ROM sections still occupy a table entry. */
        if (start > end || ((start | source) & 3)) break;
        u64 size = ((u64)end - start + 3) & ~3ull;
        if (size > 0x18000 || !inside(start, (size_t)size, 0x3ffe8000, 0x40000000)
            || !rom_slice(segments, source, (size_t)size, bytes)) break;
        RomSegment initializer;
        initializer.address = initializer.physical_address = start;
        initializer.initialize_ram = true;
        initializer.data = bytes;
        segments.push_back(initializer);
    }
    if (error) *error = "Invalid LX106 ROM RAM initialization table.";
    return false;
}

void lx106_flash_handoff(std::vector<RomSegment> &segments)
{
    Bytes code, literal;
    /* Before jumping into SPI flash the mask ROM calls ets_uart_printf.
     * Its actual implementation installs the UART character callback in
     * the ROM printf state. Reproduce that handoff only for a recognized
     * implementation, and derive both pointers from the supplied ROM. */
    if(!rom_slice(segments,0x4000242c,7,code) || code!=H("31 21 fe 29 33 0d f0") ||
       !rom_slice(segments,0x40002438,3,code) || code!=H("21 ff ff") ||
       !rom_slice(segments,0x40001cb0,4,literal))return;
    u32 state=rd32le((const u8 *)literal.data());
    if(!inside(state,16,0x3ffe8000,0x40000000) ||
       !rom_slice(segments,0x40002434,4,literal))return;
    u32 callback=rd32le((const u8 *)literal.data());
    if(!inside(callback,1,0x40000000,0x40010000) ||
       !rom_slice(segments,callback,1,code))return;
    RomSegment initializer;
    initializer.address=initializer.physical_address=state+12;
    initializer.initialize_ram=true;initializer.data=literal;
    segments.push_back(initializer);
}
}

bool EspMachine::load_rom(const std::vector<RomSegment> &input_segments, std::string *error)
{
    std::vector<RomSegment> segments(input_segments);
    if (segments.empty()) {
        if (error) *error = "The supplied ROM contains no data.";
        return false;
    }
    if (chip_ == esp::CHIP_ESP8266 && !lx106_initializers(segments, error)) return false;
    if (chip_ == esp::CHIP_ESP8266) lx106_flash_handoff(segments);
    if (chip_ == esp::CHIP_ESP32 && !lx6_rom_stores(segments,error)) return false;
    if (chip_ == esp::CHIP_ESP32) lx6_flash_handoff(segments,cpu_mhz_);
    const bool core_views=chip_==esp::CHIP_ESP32 && lx6_private_rom_views(segments);
    /* Validate the entire mapping before changing machine state. */
    std::vector<Region> mapped;
    for (size_t i = 0; i < segments.size(); ++i) {
        const RomSegment &s = segments[i];
        if (s.initialize_ram) {
            Region *ram = region(s.address, (unsigned)s.data.size());
            if (s.data.empty() || s.executable || s.address != s.physical_address || !ram || !ram->writable) {
                if (error) *error = "ROM initializer is outside writable chip RAM.";
                return false;
            }
            continue;
        }
        const u32 addresses[] = {s.address, s.physical_address};
        unsigned count = s.address == s.physical_address ? 1 : 2;
        for (unsigned n = 0; n < count; ++n) {
            u32 address = addresses[n];
            if (s.data.empty() || !valid_rom_address(chip_, address, s.data.size())) {
                if (error) *error = strf("ROM segment 0x%08x is outside %s mask ROM.", address, esp::chip_name(chip_));
                return false;
            }
            for(unsigned core=0;core<(core_views?2u:1u);++core) {
                const unsigned mask=core_views?(1u<<core):3u;
                for (size_t j = 0; j < regions_.size() + mapped.size(); ++j) {
                    const Region &r = j < regions_.size() ? regions_[j] : mapped[j - regions_.size()];
                    if ((r.core_mask&mask) && (u64)address < (u64)r.base + r.bytes.size() &&
                        (u64)r.base < (u64)address + s.data.size()) {
                        if (error) *error = "ROM segment overlaps an existing memory region.";
                        return false;
                    }
                }
                Region region(address, s.data.size(), false, s.executable,mask);
                region.bytes = s.data;
                if(core_views && core)lx6_app_rom_view(address,region.bytes);
                mapped.push_back(region);
            }
        }
    }
    regions_.insert(regions_.end(), mapped.begin(), mapped.end());
    for (size_t i = 0; i < segments.size(); ++i) {
        const RomSegment &s = segments[i];
        if (!s.initialize_ram) continue;
        /* Use the bus so RTC and dual-ported SRAM aliases share the same
         * storage as guest loads, stores and image initializers. */
        for(size_t j=0;j<s.data.size();++j) write(s.address+(u32)j,1,(u8)s.data[j]);
    }
    initialize_flash_descriptor();
    if (error) error->clear();
    return true;
}
