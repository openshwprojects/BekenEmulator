#include "esp/esp_machine.h"
#include "esp8266/esp8266_wdev.h"

#include <stdio.h>
#include <string.h>

namespace {
struct MemoryRange { u32 base, size; bool executable; };
/* ESP8266EX technical reference; ESP-IDF components/soc/esp32/include/soc/soc.h. */
const MemoryRange esp8266_ram[] = {
    {0x3ffe8000u, 0x18000u, false}, {0x40100000u, 0x10000u, true},
    {0x60001000u, 0x400u, false}
};
const MemoryRange esp32_ram[] = {
    {0x3ffae000u, 0x52000u, false}, {0x40070000u, 0x50000u, true},
    {0x400c0000u, 0x2000u, true}, {0x50000000u, 0x2000u, true}
};
/* ESP32 SRAM1 is dual ported. Its DRAM and IRAM buses address words in
 * opposite order, while retaining byte order within a word. Keep the IRAM
 * view as the physical storage (ESP-IDF SOC_DIRAM_INVERTED). */
u32 esp32_dram_alias(u32 address)
{
    return address>=0x3ffe0000u && address<0x40000000u
        ? 0x400a0000u+(0x3ffffffcu-(address&~3u))+(address&3u) : address;
}

bool boot_remap_range(u32 address,unsigned size)
{
    return (address>=0x40000000u && address<0x40008000u) ||
        (address<0x400b8000u && (u64)address+size>0x400b0000u);
}
enum RomOp {
    ROM_COPY, ROM_SET, ROM_ZERO, ROM_COMPARE, ROM_LENGTH, ROM_STRCMP,
    ROM_STRNCMP, ROM_STRCPY, ROM_STRNCPY, ROM_PUTC, ROM_PRINTF,
    ROM_FLASH_READ, ROM_FLASH_WRITE, ROM_FLASH_ERASE, ROM_DELAY,
    ROM_CPU_FREQ, ROM_SET_FREQ, ROM_CACHE_ON, ROM_CACHE_OFF,
    ROM_UART_DIV, ROM_GET_CCOUNT, ROM_SRAND, ROM_RAND, ROM_RESET_REASON,
    ROM_UDIV, ROM_UMOD, ROM_SDIV, ROM_INSTALL_PUTC, ROM_UART_PRINTF,
    ROM_FLASH_READ_DATA
};
struct RomEntry { u32 address; RomOp operation; };
/* Addresses from Espressif's eagle.rom.addr.v6.ld and esp32.rom*.ld.
 * https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/ld/eagle.rom.addr.v6.ld
 * https://github.com/espressif/esp-idf/tree/v4.4/components/esp_rom/esp32/ld
 */
const RomEntry esp8266_rom[] = {
    {0x400018b4,ROM_COPY},{0x400018c4,ROM_COPY},{0x4000df48,ROM_COPY},
    {0x4000e04c,ROM_COPY},{0x400006c4,ROM_COPY},
    {0x400018a4,ROM_SET},{0x4000e190,ROM_SET},
    {0x40002ae8,ROM_ZERO},{0x4000de84,ROM_ZERO},
    {0x400018d4,ROM_COMPARE},{0x4000dea8,ROM_COMPARE},
    {0x40002ac8,ROM_LENGTH},{0x4000bf4c,ROM_LENGTH},
    {0x40002aa8,ROM_STRCMP},{0x4000bdc8,ROM_STRCMP},
    {0x40002ab8,ROM_STRNCMP},{0x4000bfa8,ROM_STRNCMP},
    {0x40002a88,ROM_STRCPY},{0x4000bec8,ROM_STRCPY},
    {0x40002a98,ROM_STRNCPY},{0x4000c0a0,ROM_STRNCPY},
    {0x40003b30,ROM_PUTC},{0x40002be8,ROM_PUTC},
    {0x400024cc,ROM_PRINTF},{0x40002544,ROM_PRINTF},
    {0x4000242c,ROM_INSTALL_PUTC},{0x40002438,ROM_UART_PRINTF},
    {0x40004b1c,ROM_FLASH_READ},{0x40004a4c,ROM_FLASH_WRITE},
    {0x400042ac,ROM_FLASH_READ_DATA},
    {0x40004a00,ROM_FLASH_ERASE},{0x40002ecc,ROM_DELAY},
    {0x40002f0c,ROM_CPU_FREQ},{0x40002f04,ROM_SET_FREQ},
    {0x40004678,ROM_CACHE_ON},{0x400047f0,ROM_CACHE_OFF},
    {0x4000dd38,ROM_GET_CCOUNT},{0x400005f0,ROM_SRAND},
    {0x40000600,ROM_RAND},{0x4000e21c,ROM_UDIV},
    {0x4000e268,ROM_UMOD},{0x4000dc88,ROM_SDIV}
};
const RomEntry esp32_rom[] = {
    {0x4000c2c8,ROM_COPY},{0x4000c3c0,ROM_COPY},{0x4000c44c,ROM_SET},
    {0x4000c1f4,ROM_ZERO},{0x4000c260,ROM_COMPARE},
    {0x400014c0,ROM_LENGTH},{0x40001274,ROM_STRCMP},
    {0x4000c5f4,ROM_STRNCMP},{0x400013ac,ROM_STRCPY},
    {0x400015d4,ROM_STRNCPY},{0x40009200,ROM_PUTC},
    {0x4000922c,ROM_PUTC},{0x40007cf8,ROM_PUTC},
    {0x40007d54,ROM_PRINTF},{0x40008534,ROM_DELAY},
    {0x4000855c,ROM_CPU_FREQ},{0x40008550,ROM_SET_FREQ},
    {0x40009a84,ROM_CACHE_ON},{0x40009ab8,ROM_CACHE_OFF},
    {0x400090cc,ROM_UART_DIV},{0x400081d4,ROM_RESET_REASON},
    {0x40001004,ROM_SRAND},{0x40001058,ROM_RAND}
};
}

EspMachine::EspMachine(esp::Chip chip)
    : chip_(chip), cpu_(*this, chip == esp::CHIP_ESP32), cpu1_(*this, true), interrupts_(cpu_), frc_timers_(cpu_), wifi_dma_(*this),
      active_core_(0), next_core_(0), app_reset_(true), reset_domain_(RESET_NONE), reset_mask_(0), reset_reason_(0), reset_count_(0), rtc_epoch_us_(0), retired_cpu_instructions_(0), gpio_levels_(0), putc1_(0), rom_calls_(0),
      delay_us_(0), clock_origin_instructions_(0), clock_skipped_cycles_(0), apb_origin_ticks_(0), apb_origin_delay_us_(0), rtc_wake_us_(0), rtc_calibration_us_(0), cpu_mhz_(80), xtal_mhz_(chip==esp::CHIP_ESP8266?26:40), flash_window_(0), random_(1),
      cache_enabled_(true), stopped_(XTENSA_OK)
{
    initialize_memory();
    cpu1_.set_processor_id(0xabab);
    for(unsigned i=0;i<3;++i) uarts_[i].reset(chip_==esp::CHIP_ESP32);
}

bool EspMachine::set_xtal_frequency(unsigned mhz)
{
    if(mhz!=26 && mhz!=40 && !(chip_==esp::CHIP_ESP8266 && mhz==24))return false;
    xtal_mhz_=mhz;
    return true;
}

bool EspMachine::set_psram_size(unsigned bytes)
{
    if(bytes && (chip_!=esp::CHIP_ESP32 || (bytes!=0x200000 && bytes!=0x400000 && bytes!=0x800000)))
        return false;
    psram_.assign(bytes,'\0');
    return true;
}

void EspMachine::initialize_memory()
{
    regions_.clear();
    const MemoryRange *ranges = chip_ == esp::CHIP_ESP32 ? esp32_ram : esp8266_ram;
    size_t n = chip_ == esp::CHIP_ESP32 ? sizeof(esp32_ram)/sizeof(esp32_ram[0])
                                      : sizeof(esp8266_ram)/sizeof(esp8266_ram[0]);
    for (size_t i=0; i<n; ++i)
        regions_.push_back(Region(ranges[i].base, ranges[i].size, true, ranges[i].executable,
            chip_==esp::CHIP_ESP32 && ranges[i].base==0x400c0000?1:3)); /* RTC FAST: PRO only */
}

EspMachine::Region *EspMachine::region(u32 address, unsigned size)
{
    if (chip_ == esp::CHIP_ESP32 && address >= 0x3ff80000 && address < 0x3ff82000)
        address = 0x400c0000 + (address - 0x3ff80000);
    for (size_t i=0; i<regions_.size(); ++i) {
        Region &r = regions_[i];
        if ((r.core_mask & (1u << active_core_)) && address >= r.base &&
            (u64)address + size <= (u64)r.base + r.bytes.size())
            return &r;
    }
    return 0;
}

bool EspMachine::load_segment(const esp::Segment &s, std::string *error)
{
    if (!s.address || s.data.empty()) return true;
    if (s.data.size() > 0x1000000u || (u64)s.address+s.data.size() > 0x100000000ull) {
        if (error) *error = "ESP segment exceeds address space";
        return false;
    }
    if (s.mapped) {
        /* LX106 has a single banked flash window, not independent segment
         * mappings. Read it from flash_ so writes and bank selection stay
         * coherent; the parser's file offset determines its virtual address. */
        if(chip_==esp::CHIP_ESP8266) return true;
        Region mapped(s.address, s.data.size(), false,
                      chip_ == esp::CHIP_ESP8266 || s.address >= 0x400d0000);
        mapped.bytes = s.data;
        mapped.flash_mapped = true;
        regions_.push_back(mapped);
        return true;
    }
    Region *r = region(s.address, (unsigned)s.data.size());
    if (!r || !r->writable) {
        if (error) *error = strf("ESP segment at 0x%08x is outside chip RAM", s.address);
        return false;
    }
    u32 addr = s.address;
    if (chip_ == esp::CHIP_ESP32 && addr >= 0x3ff80000 && addr < 0x3ff82000)
        addr = 0x400c0000 + (addr - 0x3ff80000);
    if(chip_==esp::CHIP_ESP32 && addr<0x40000000u && (u64)addr+s.data.size()>0x3ffe0000u) {
        for(size_t i=0;i<s.data.size();++i) write(addr+(u32)i,1,(u8)s.data[i]);
    } else r->bytes.replace(addr-r->base, s.data.size(), s.data);
    return true;
}

bool EspMachine::load(const Bytes &flash, const esp::FlashImage &layout,
                      bool bootloader, std::string *error)
{
    if (layout.chip != chip_ || (chip_ != esp::CHIP_ESP8266 && chip_ != esp::CHIP_ESP32)) {
        if (error) *error = "ESP image/chip mismatch";
        return false;
    }
    int index = bootloader ? layout.boot_index : layout.app_index;
    if (index < 0 || (size_t)index >= layout.images.size()) {
        if (error) *error = bootloader ? "ESP bootloader image is absent" : "ESP application image is absent";
        return false;
    }
    initialize_memory();
    registers_.clear();
    wifi_dma_.reset();
    gpio_external_.clear();
    gpio_levels_=0;
    for (unsigned i=0;i<3;++i) uarts_[i].reset(chip_==esp::CHIP_ESP32);
    diagnostic_.clear(); flash_ = flash;
    psram_.assign(psram_.size(),'\0');
    active_core_=next_core_=0; app_reset_=true; retired_cpu_instructions_=0;
    reset_domain_=RESET_NONE;reset_mask_=reset_reason_=reset_count_=0;rtc_epoch_us_=0;
    putc1_=0;output_stack_.clear();
    rom_calls_=delay_us_=clock_origin_instructions_=clock_skipped_cycles_=0; cpu_mhz_=80; flash_window_=0; random_=1;
    apb_origin_ticks_=apb_origin_delay_us_=0;
    rtc_wake_us_=rtc_calibration_us_=0;
    cache_enabled_=true; stopped_=XTENSA_OK;
    const esp::Image &image=layout.images[index];
    for (size_t i=0; i<image.segments.size(); ++i)
        if (!load_segment(image.segments[i],error)) return false;
    if(chip_==esp::CHIP_ESP32 && !bootloader) {
        /* Direct app entry starts after the bootloader mapped the image.
         * Populate the real tables too: the SDK's mmap allocator reads them
         * to reserve existing pages. A private Region alone is insufficient. */
        for(size_t i=0;i<image.segments.size();++i) {
            const esp::Segment &s=image.segments[i];
            if(!s.mapped || s.data.empty())continue;
            const bool drom=s.address>=0x3f400000 && s.address<0x3f800000;
            const bool irom=s.address>=0x400d0000 && s.address<0x40c00000;
            const u64 end=u64(s.address)+s.data.size();
            if((!drom && !irom) || end>(drom?0x3f800000ull:0x40c00000ull) ||
                (s.address&0xffff)!=(s.file_offset&0xffff) ||
                s.file_offset>flash_.size() || s.data.size()>flash_.size()-s.file_offset ||
                s.file_offset+s.data.size()>0x1000000u ||
                flash_.compare(s.file_offset,s.data.size(),s.data)!=0) {
                if(error)*error="ESP32 mapped segment has invalid physical flash placement";
                return false;
            }
            const u32 first=s.address&~0xffffu;
            for(u64 address=first;address<end;address+=0x10000) {
                const unsigned entry=drom?unsigned((address-0x3f400000)>>16):
                    64+unsigned((address-0x40000000)>>16);
                const u32 physical=u32((s.file_offset>>16)+((address-first)>>16));
                for(unsigned core=0;core<2;++core) {
                    const u32 cell=0x3ff10000+core*0x2000+entry*4;
                    std::map<u32,u32>::const_iterator prior=registers_.find(cell);
                    if(prior!=registers_.end() && prior->second!=physical) {
                        if(error)*error="ESP32 mapped segments require conflicting MMU pages";
                        return false;
                    }
                    registers_[cell]=physical;
                }
            }
        }
    }
    cpu_.reset(image.entry_point);
    cpu1_.reset(0x40000400);
    interrupts_.attach_cpu(1,0);
    interrupts_.reset();
    frc_timers_.reset();
    cpu_.set_reg(1, chip_ == esp::CHIP_ESP32 ? 0x3ffe3f20u : 0x3ffffff0u);
    if (chip_ == esp::CHIP_ESP32) {
        /* Flash entry is a windowed C call from the ROM/bootloader. Keep a
         * caller window live so ENTRY and the real overflow vectors create
         * the normal stack back-links. Starting ENTRY with CALLINC=0 loses
         * that caller and its first spill reads an uninitialized sp-12. */
        cpu_.set_special(XT_SR_PS, 0x00060020u); /* WOE | CALLINC=2 | UM */
        cpu_.set_reg(8, 0x80000740u); /* ROM _start continuation, call8 encoding */
        write(cpu_.reg(1) - 12, 4, cpu_.reg(1) + 64);
    } else {
        /* LX106 ROM _start at 0x40000431 sets PS=0x20 before main and
         * flash entry. Preserve UM so the first IRQ uses the user vector. */
        cpu_.set_special(XT_SR_PS,0x20);
    }
    initialize_flash_descriptor();
    reset_clocked_devices();
    return true;
}

void EspMachine::initialize_flash_descriptor()
{
    /* ROM probes the NOR capacity before transferring control. Restore this
     * handover state after loading ROM-library RAM initializers as well. */
    u32 descriptor=chip_==esp::CHIP_ESP32 ? 0x3ffae270u : 0x3fffc718u;
    if(chip_==esp::CHIP_ESP8266)write(0x3fffc714,4,descriptor);
    unsigned capacity=0;for(size_t bytes=flash_.size();bytes>1;bytes>>=1) ++capacity;
    write(descriptor,4,(capacity<<16)|0x40efu); /* Winbond identity with actual NOR capacity */
    write(descriptor+4,4,(u32)flash_.size());
    write(descriptor+8,4,0x10000); write(descriptor+12,4,0x1000);
    write(descriptor+16,4,0x100); write(descriptor+20,4,0xffff);
}

bool EspMachine::fault(const char *operation, u32 address)
{
    diagnostic_=strf("%s at 0x%08x (PC 0x%08x)",operation,address,active_cpu().pc());
    return false;
}

bool EspMachine::uart_address(u32 address, unsigned &index, u32 &offset) const
{
    static const u32 bases8266[]={0x60000000u,0x60000f00u};
    static const u32 bases32[]={0x3ff40000u,0x3ff50000u,0x3ff6e000u};
    const u32 *bases=chip_==esp::CHIP_ESP32 ? bases32 : bases8266;
    unsigned count=chip_==esp::CHIP_ESP32 ? 3 : 2;
    for (unsigned i=0;i<count;++i) if (address>=bases[i] && address<bases[i]+0x80) {
        index=i;offset=address-bases[i];return true;
    }
    return false;
}

bool EspMachine::mmio_read(u32 address,u32 &value)
{
    if(chip_==esp::CHIP_ESP32 && interrupts_.handles(address))
        return interrupts_.read(address,esp32_apb_ticks(),value);
    if(chip_==esp::CHIP_ESP8266 && (address==0x6000036c || address==0x60000370))
        return esp8266_mmio_read(address,value);
    if(chip_==esp::CHIP_ESP32 && address>=0x3ff49000u && address<=0x3ff49090u && !(address&3)) {
        value=registers_[address];return true;
    }
    unsigned index; u32 offset;
    if (uart_address(address,index,offset)) {
        const bool ok=uarts_[index].read(offset,esp32_apb_ticks(),apb_frequency(),value);
        update_uart_interrupts();
        return ok;
    }
    u32 gpio=chip_==esp::CHIP_ESP32?0x3ff44000u:0x60000300u;
    u32 span=chip_==esp::CHIP_ESP32?0x700u:0xb0u;
    if(address>=gpio && address<gpio+span)return gpio_mmio_read(address,value);
    return chip_==esp::CHIP_ESP32 ? esp32_mmio_read(address,value) : esp8266_mmio_read(address,value);
}

bool EspMachine::mmio_write(u32 address,u32 value)
{
    if(chip_==esp::CHIP_ESP32 && interrupts_.handles(address))
        return interrupts_.write(address,esp32_apb_ticks(),value);
    if(chip_==esp::CHIP_ESP8266 && (address==0x6000036c || address==0x60000370))
        return esp8266_mmio_write(address,value);
    if(chip_==esp::CHIP_ESP32 && address>=0x3ff49000u && address<=0x3ff49090u && !(address&3)) {
        update_gpio_interrupts();registers_[address]=value&0x7fffu;update_gpio_interrupts();return true;
    }
    unsigned index; u32 offset;
    if (uart_address(address,index,offset)) {
        const bool ok=uarts_[index].write(offset,esp32_apb_ticks(),apb_frequency(),value);
        update_uart_interrupts();
        return ok;
    }
    u32 gpio=chip_==esp::CHIP_ESP32?0x3ff44000u:0x60000300u;
    u32 span=chip_==esp::CHIP_ESP32?0x700u:0xb0u;
    if(address>=gpio && address<gpio+span)return gpio_mmio_write(address,value);
    return chip_==esp::CHIP_ESP32 ? esp32_mmio_write(address,value) : esp8266_mmio_write(address,value);
}

bool EspMachine::read(u32 address,unsigned size,u32 &value,bool instruction)
{
    if(!instruction && chip_==esp::CHIP_ESP8266 && esp8266_wdev::key_memory(address,size))
        return esp8266_wdev::read_key(registers_,address,size,value);
    value=0;
    if (!(size==1 || size==2 || size==3 || size==4)) return fault("invalid read size",address);
    if(chip_==esp::CHIP_ESP32) {
        bool handled=false;
        bool ram_ok=esp32_psram_read(address,size,value,instruction,handled);
        if(handled)return ram_ok;
        bool ok=esp32_mmu_read(address,size,value,instruction,handled);
        if(handled)return ok;
    }
    u32 physical=address;
    const bool boot_remap=chip_==esp::CHIP_ESP32 && boot_remap_range(address,size) &&
        (register_value(0x3ff00000+active_core_*4)&1);
    if(boot_remap && address<0x400b8000 && (u64)address+size>0x400b0000)
        return fault("SRAM instruction alias hidden by boot remap",address);
    if(boot_remap && address>=0x40000000 && address<0x40008000) {
        if((u64)address+size>0x40008000) {
            /* A density instruction can straddle the remap boundary. */
            for(unsigned i=0;i<size;++i) {
                u32 byte;if(!read(address+i,1,byte,instruction))return false;
                value|=byte<<(8*i);
            }
            return true;
        }
        physical=0x400b0000+(address-0x40000000);
    }
    if (chip_==esp::CHIP_ESP32 && address>=0x3ff80000 && address<0x3ff82000)
        physical=0x400c0000+(address-0x3ff80000);
    Region *r=region(physical,size);
    if (r && (!instruction || r->executable)) {
        if(chip_==esp::CHIP_ESP8266 && r->flash_mapped && !cache_enabled_) return fault("flash cache disabled",address);
        if(chip_==esp::CHIP_ESP32 && physical<0x40000000u && (u64)physical+size>0x3ffe0000u) {
            Region *alias=region(0x400a0000u,0x20000u);
            for(unsigned i=0;i<size;++i) {
                u32 a=esp32_dram_alias(physical+i);
                Region *source=a==physical+i?r:alias;
                value|=(u32)(u8)source->bytes[a-source->base]<<(i*8);
            }
            return true;
        }
        for (unsigned i=0;i<size;++i) value|=(u32)(u8)r->bytes[physical-r->base+i]<<(i*8);
        return true;
    }
    if (chip_==esp::CHIP_ESP8266 && address>=0x40200000 && (u64)address+size<=0x40300000ull) {
        size_t offset=(size_t)(address-0x40200000)+flash_window_;
        if (!cache_enabled_) return fault("flash cache disabled",address);
        if ((u64)offset+size>flash_.size()) return fault("flash read beyond image",address);
        for (unsigned i=0;i<size;++i) value|=(u32)(u8)flash_[offset+i]<<(i*8);
        return true;
    }
    unsigned uart_index;u32 uart_offset;
    const bool uart_byte=chip_==esp::CHIP_ESP32 && size==1 &&
        uart_address(address,uart_index,uart_offset) && uart_offset==0;
    if (!instruction && ((size==4 && !(address&3)) || uart_byte)) {
        diagnostic_.clear();
        if(mmio_read(address,value))return true;
        if(!diagnostic_.empty()) {
            diagnostic_+=strf(" at 0x%08x (PC 0x%08x)",address,active_cpu().pc());return false;
        }
    }
    return fault(instruction?"unmapped instruction fetch":"unsupported memory read",address);
}

bool EspMachine::write(u32 address,unsigned size,u32 value)
{
    if(chip_==esp::CHIP_ESP8266 && esp8266_wdev::key_memory(address,size))
        return esp8266_wdev::write_key(registers_,address,size,value);
    if (!(size==1 || size==2 || size==4)) return fault("invalid write size",address);
    if(chip_==esp::CHIP_ESP32) {
        bool handled=false;
        const bool ok=esp32_psram_write(address,size,value,handled);
        if(handled)return ok;
    }
    u32 physical=address;
    const bool boot_remap=chip_==esp::CHIP_ESP32 && boot_remap_range(address,size) &&
        (register_value(0x3ff00000+active_core_*4)&1);
    if(boot_remap && address<0x400b8000 && (u64)address+size>0x400b0000)
        return fault("SRAM instruction alias hidden by boot remap",address);
    if(boot_remap && address>=0x40000000 && address<0x40008000) {
        if((u64)address+size>0x40008000)return fault("write crosses remapped SRAM boundary",address);
        physical=0x400b0000+(address-0x40000000);
    }
    if (chip_==esp::CHIP_ESP32 && address>=0x3ff80000 && address<0x3ff82000)
        physical=0x400c0000+(address-0x3ff80000);
    Region *r=region(physical,size);
    if (r && r->writable) {
        if(chip_==esp::CHIP_ESP32 && physical<0x40000000u && (u64)physical+size>0x3ffe0000u) {
            Region *alias=region(0x400a0000u,0x20000u);
            for(unsigned i=0;i<size;++i) {
                u32 a=esp32_dram_alias(physical+i);
                Region *target=a==physical+i?r:alias;
                target->bytes[a-target->base]=(char)(value>>(8*i));
            }
            return true;
        }
        for (unsigned i=0;i<size;++i) r->bytes[physical-r->base+i]=(char)(value>>(8*i));
        return true;
    }
    /* ESP32 firmware also accesses the UART FIFO with L8UI/S8I. One
     * transaction consumes or emits one byte; adjacent config registers
     * retain their word-access requirement. */
    unsigned uart_index;u32 uart_offset;
    const bool uart_byte=chip_==esp::CHIP_ESP32 && size==1 &&
        uart_address(address,uart_index,uart_offset) && uart_offset==0;
    if ((size==4 && !(address&3)) || uart_byte) {
        diagnostic_.clear();
        if(mmio_write(address,value))return true;
        if(!diagnostic_.empty()) {
            diagnostic_+=strf(" at 0x%08x (write=0x%08x, PC 0x%08x)",address,value,active_cpu().pc());return false;
        }
    }
    return fault("unsupported memory write",address);
}

const std::string &EspMachine::uart(unsigned index) const
{
    static const std::string empty;
    return index<(chip_==esp::CHIP_ESP32?3u:2u) ? uarts_[index].output() : empty;
}

void EspMachine::receive_uart(unsigned index,const Bytes &data)
{
    if(index>=(chip_==esp::CHIP_ESP32?3u:2u))return;
    uarts_[index].receive(data,esp32_apb_ticks(),apb_frequency());
    update_uart_interrupts();
}

void EspMachine::uart_transmit(unsigned index,u8 byte)
{
    if(index>=(chip_==esp::CHIP_ESP32?3u:2u))return;
    uarts_[index].transmit(byte,esp32_apb_ticks(),apb_frequency());
    update_uart_interrupts();
}

void EspMachine::update_uart_interrupts()
{
    const u64 ticks=esp32_apb_ticks();
    const unsigned count=chip_==esp::CHIP_ESP32?3:2;
    bool shared=false;
    for(unsigned i=0;i<count;++i) {
        uarts_[i].advance(ticks,apb_frequency());
        const bool active=uarts_[i].interrupt();
        if(chip_==esp::CHIP_ESP32)interrupts_.set_source(34+i,active);
        else shared=shared||active;
    }
    if(chip_==esp::CHIP_ESP8266)active_cpu().set_irq(5,shared);
}

bool EspMachine::cycles_to_uart_interrupt(u64 &cycles) const
{
    const u64 ticks=esp32_apb_ticks();
    const unsigned count=chip_==esp::CHIP_ESP32?3:2;
    bool found=false;u64 minimum=0;
    for(unsigned i=0;i<count;++i) {
        if(chip_==esp::CHIP_ESP32) {
            if(!interrupts_.source_can_wake(34+i))continue;
        } else if(!(active_cpu().special(XT_SR_INTENABLE)&32) || (active_cpu().special(XT_SR_PS)&31))continue;
        u64 delta;
        if(uarts_[i].next_event_ticks(ticks,apb_frequency(),delta) && (!found || delta<minimum)) {
            minimum=delta;found=true;
        }
    }
    return found && cycles_for_apb_ticks(minimum,cycles);
}

u32 EspMachine::register_value(u32 address) const
{
    std::map<u32,u32>::const_iterator found=registers_.find(address);
    return found==registers_.end()?0:found->second;
}

u64 EspMachine::elapsed_us() const {
    return delay_us_+(cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_)/cpu_mhz_;
}

void EspMachine::set_cpu_frequency(u32 mhz)
{
    if(!mhz || mhz==cpu_mhz_)return;
    const u64 apb_ticks=esp32_apb_ticks();
    update_esp32_interrupts();
    update_uart_interrupts();
    delay_us_=elapsed_us();
    clock_origin_instructions_=cpu_instructions();
    clock_skipped_cycles_=0;
    cpu_mhz_=mhz;
    apb_origin_ticks_=apb_ticks;
    apb_origin_delay_us_=delay_us_;
}

bool EspMachine::rom_copy(u32 dst,u32 src,u32 length)
{
    if (length>0x1000000u || (u64)src+length>0x100000000ull || (u64)dst+length>0x100000000ull)
        return fault("ROM copy range invalid",dst);
    Bytes data(length,'\0');
    u32 value;
    for (u32 i=0;i<length;++i) { if (!read(src+i,1,value,false)) return false; data[i]=(char)value; }
    for (u32 i=0;i<length;++i) if (!write(dst+i,1,(u8)data[i])) return false;
    return true;
}

bool EspMachine::rom_string(u32 address,std::string &out,size_t limit)
{
    out.clear();
    for (size_t i=0;i<limit;++i) {
        u32 value;
        if ((u64)address+i>0xffffffffull || !read(address+(u32)i,1,value,false)) return false;
        if (!value) return true;
        out+=(char)value;
    }
    return fault("unterminated ROM string",address);
}

bool EspMachine::rom_argument(unsigned index,u32 &value)
{
    /* Both ABIs pass six integer arguments in registers; further arguments
     * are in the caller's stack frame, before the callee executes ENTRY. */
    if(index<6) { value=active_cpu().rom_argument(index);return true; }
    u64 address=(u64)active_cpu().reg(1)+(index-6)*4;
    if(address>0xffffffffull) return fault("ROM argument stack overflow",active_cpu().reg(1));
    return read((u32)address,4,value,false);
}

bool EspMachine::rom_printf(std::string &out)
{
    std::string format;
    if (!rom_string(active_cpu().rom_argument(0),format)) return false;
    out.clear(); unsigned arg=1;
    for (size_t i=0;i<format.size();++i) {
        if (format[i]!='%') { out+=format[i];continue; }
        ++i; if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        if (format[i]=='%') { out+='%';continue; }
        bool left=false,zero=false;
        if (format[i]=='-') { left=true;++i; }
        if (format[i]=='0') { zero=true;++i; }
        unsigned width=0;
        while (i<format.size() && format[i]>='0' && format[i]<='9') {
            width=width*10+(format[i++]-'0');if (width>4096) return fault("ROM printf width too large",active_cpu().pc());
        }
        int precision=-1;
        if (i<format.size() && format[i]=='.') {
            ++i;precision=0;
            while (i<format.size() && format[i]>='0' && format[i]<='9') {
                precision=precision*10+(format[i++]-'0');if (precision>65536) return fault("ROM printf precision too large",active_cpu().pc());
            }
        }
        if (i<format.size() && format[i]=='l') ++i;
        if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        u32 value;
        if(!rom_argument(arg++,value)) return false;
        std::string piece;
        char temp[48];
        switch (format[i]) {
        case 's': if (!value) piece="(null)";else if (!rom_string(value,piece)) return false;
                  if (precision>=0 && piece.size()>(size_t)precision) piece.resize(precision);
                  break;
        case 'c': piece+=(char)value;break;
        case 'd': case 'i': sprintf(temp,"%d",(s32)value);piece=temp;break;
        case 'u': sprintf(temp,"%u",value);piece=temp;break;
        case 'x': sprintf(temp,"%x",value);piece=temp;break;
        case 'X': sprintf(temp,"%X",value);piece=temp;break;
        case 'p': sprintf(temp,"0x%08x",value);piece=temp;break;
        default:return fault("unsupported ROM printf conversion",active_cpu().pc());
        }
        if (piece.size()<width) {
            std::string pad(width-piece.size(),zero?'0':' ');
            if (left) piece+=pad;
            else if (zero && !piece.empty() && piece[0]=='-') piece="-"+pad+piece.substr(1);
            else piece=pad+piece;
        }
        out+=piece;
    }
    return true;
}

bool EspMachine::rom_service(u32 address,XtensaResult &result)
{
    /* HLE callback return bridge, used only while an ets_printf output
     * callback is active. Firmware callbacks execute on the real CPU/bus;
     * output is never inferred from which callback was installed. */
    const u32 output_return_bridge=0x4000fff0u;
    if(chip_==esp::CHIP_ESP8266 && address==output_return_bridge && !output_stack_.empty()) {
        RomOutput &output=output_stack_.back();
        if(output.position<output.text.size()) {
            active_cpu().set_reg(2,(u8)output.text[output.position++]);
            active_cpu().set_reg(0,output_return_bridge);active_cpu().set_pc(output.callback);
        } else {
            u32 target=output.return_pc,length=(u32)output.text.size();
            output_stack_.pop_back();
            active_cpu().set_reg(0,target);active_cpu().set_reg(2,length);active_cpu().set_pc(target);
        }
        ++rom_calls_;result=XTENSA_OK;return true;
    }
    const RomEntry *table=chip_==esp::CHIP_ESP32?esp32_rom:esp8266_rom;
    size_t count=chip_==esp::CHIP_ESP32?sizeof(esp32_rom)/sizeof(esp32_rom[0]):sizeof(esp8266_rom)/sizeof(esp8266_rom[0]);
    const RomEntry *entry=0;
    for (size_t i=0;i<count;++i) if (table[i].address==address) { entry=&table[i];break; }
    if (!entry) return false;
    u32 a=active_cpu().rom_argument(0),b=active_cpu().rom_argument(1),c=active_cpu().rom_argument(2),ret=0;
    bool ok=true,redirected=false;u32 x,y;std::string text;
    switch (entry->operation) {
    case ROM_COPY: ok=rom_copy(a,b,c);ret=a;break;
    case ROM_ZERO: c=b;b=0; /* fall through */
    case ROM_SET:
        if (c>0x1000000u || (u64)a+c>0x100000000ull) { ok=fault("ROM fill range invalid",a);break; }
        for (u32 i=0;i<c && ok;++i) ok=write(a+i,1,b);
        ret=a;break;
    case ROM_COMPARE: case ROM_STRCMP: case ROM_STRNCMP: {
        u32 limit=entry->operation==ROM_STRCMP?65536:c;
        if (limit>0x1000000u) { ok=fault("ROM compare range invalid",a);break; }
        for (u32 i=0;i<limit;++i) {
            if (!read(a+i,1,x,false) || !read(b+i,1,y,false)) { ok=false;break; }
            if (x!=y) { ret=(u32)((s32)x-(s32)y);break; }
            if (!x && entry->operation!=ROM_COMPARE) break;
        }
        break;
    }
    case ROM_LENGTH: ok=rom_string(a,text);ret=(u32)text.size();break;
    case ROM_STRCPY: case ROM_STRNCPY: {
        bool bounded=entry->operation==ROM_STRNCPY;u32 limit=bounded?c:65536;
        if (limit>0x1000000u) { ok=fault("ROM string copy range invalid",a);break; }
        bool ended=false;
        for (u32 i=0;i<limit && ok;++i) {
            x=0;if (!ended) ok=read(b+i,1,x,false);
            if (ok) ok=write(a+i,1,x);
            if (!x) { ended=true;if (!bounded) break; }
        }
        ret=a;break;
    }
    case ROM_PUTC: uart_transmit(0,(u8)a);break;
    case ROM_PRINTF:
        ok=rom_printf(text);
        if(ok) {
            ret=(u32)text.size();
            if(chip_==esp::CHIP_ESP8266 && putc1_ && !text.empty()) {
                RomOutput output;output.return_pc=active_cpu().reg(0);output.callback=putc1_;output.text=text;
                output_stack_.push_back(output);active_cpu().set_pc(output_return_bridge);redirected=true;
            } else for(size_t i=0;i<text.size();++i)uart_transmit(0,(u8)text[i]);
        }
        break;
    case ROM_INSTALL_PUTC:putc1_=a;break;
    case ROM_UART_PRINTF:putc1_=0;break;
    case ROM_FLASH_READ_DATA:
        /* Lower-level ROM entry takes the caller's SpiFlashChip descriptor
         * before the normal (offset, destination, length) arguments. */
        if(!read(a+4,4,x,false)) { ok=false;break; }
        a=b;b=c;c=active_cpu().rom_argument(3);
        if((u64)a+c>x) { ret=1;break; }
        /* fall through */
    case ROM_FLASH_READ:
        if ((u64)a+c>flash_.size()) { ret=1;break; }
        for (u32 i=0;i<c && ok;++i) ok=write(b+i,1,(u8)flash_[a+i]);
        break;
    case ROM_FLASH_WRITE:
        if ((u64)a+c>flash_.size()) { ret=1;break; }
        for (u32 i=0;i<c && ok;++i) { ok=read(b+i,1,x,false);if(ok) flash_[a+i]=(char)((u8)flash_[a+i]&x); }break;
    case ROM_FLASH_ERASE:
        if ((u64)a*4096+4096>flash_.size()) ret=1;
        else flash_.replace((size_t)a*4096,4096,4096,(char)0xff);
        break;
    case ROM_DELAY: delay_us_+=a;advance_clocked_cores((u64)a*cpu_mhz_);break;
    case ROM_CPU_FREQ: ret=cpu_mhz_;break;
    case ROM_SET_FREQ: set_cpu_frequency(a);break;
    case ROM_CACHE_ON:
        if(chip_==esp::CHIP_ESP8266) {
            cache_enabled_=true;flash_window_=(b&3)*0x100000u+(a&1)*0x80000u;
        } else if(a>1)ok=fault("invalid ESP32 cache core",a);
        else {
            u32 control=0x3ff00040+a*0x18;
            ok=mmio_read(control,x) && mmio_write(control,x|8);
        }
        break;
    case ROM_CACHE_OFF:
        if(chip_==esp::CHIP_ESP8266)cache_enabled_=false;
        else if(a>1)ok=fault("invalid ESP32 cache core",a);
        else {
            u32 control=0x3ff00040+a*0x18;
            ok=mmio_read(control,x) && mmio_write(control,x&~8u);
        }
        break;
    case ROM_UART_DIV: ok=mmio_write((a==1?0x3ff50000u:0x3ff40000u)+0x14,b);break;
    case ROM_GET_CCOUNT:ret=active_cpu().special(234);break;
    case ROM_SRAND:random_=a;break;
    case ROM_RAND:random_=random_*1103515245u+12345u;ret=(random_>>1)&0x7fffffffu;break;
    case ROM_RESET_REASON:
        x=registers_.count(0x3ff48034)?registers_[0x3ff48034]:0x41;
        ret=a<2?(x>>(a*6))&63:0;
        break;
    case ROM_UDIV:if(!b)ok=fault("ROM divide by zero",address);else ret=a/b;break;
    case ROM_UMOD:if(!b)ok=fault("ROM divide by zero",address);else ret=a%b;break;
    case ROM_SDIV:if(!b)ok=fault("ROM divide by zero",address);else ret=(a==0x80000000u && b==0xffffffffu)?a:(u32)((s32)a/(s32)b);break;
    }
    ++rom_calls_;
    if(ok) { if(!redirected)active_cpu().rom_return(ret);result=XTENSA_OK; }
    else result=XTENSA_MEMORY_FAULT;
    return true;
}

XtensaResult EspMachine::run(u64 max_steps)
{
    for(u64 i=0;i<max_steps;++i) { XtensaResult r=step();if(r!=XTENSA_OK)return r; }
    return XTENSA_OK;
}
