#include "esp_machine.h"
#include "esp_psram.h"
#include "esp32_cache.h"

namespace {
u32 saved(const std::map<u32,u32> &regs,u32 address,u32 initial)
{
    std::map<u32,u32>::const_iterator it=regs.find(address);return it==regs.end()?initial:it->second;
}

const char *translate(std::map<u32,u32> &regs,const Bytes &memory,unsigned core,
                      u32 address,unsigned size,bool write,size_t physical[4])
{
    if(memory.empty())return "ESP32 PSRAM is not installed";
    const u32 control=core?0x3ff00058:0x3ff00040;
    if(!(saved(regs,control,core?0x10:8)&8))return "ESP32 PSRAM cache disabled";
    const u32 config=saved(regs,control+4,0x8ff);
    if(config&32)return "ESP32 PSRAM address window masked";
    if(!esp_psram::qpi_enabled(regs))return "ESP32 PSRAM cache requires QPI mode";
    if(saved(regs,0x3ff43034,6)&2)return "ESP32 PSRAM cache chip select disabled";
    const u32 spi_config=saved(regs,0x3ff43054,0x15c04830);
    if((spi_config&6)!=4)return "ESP32 PSRAM cache requires quad I/O";
    if(((spi_config>>22)&63)!=23)return "ESP32 PSRAM cache requires 24-bit addresses";
    if(spi_config&(write?(1u<<28):32)) {
        const u32 command=saved(regs,write?0x3ff43060:0x3ff4305c,0);
        const u32 opcode=write?0x38:0xeb;
        /* IDF selects either a direct 8-bit QPI opcode or a 16-bit DCLK
         * frame with the leading zero byte providing clock guard cycles. */
        if(command!=(0x70000000|opcode) && command!=(0xf0000000|(opcode<<8)))
            return "unsupported ESP32 PSRAM cache command";
    }
    const unsigned mode=(config>>6)&7;
    if(mode>4)return "reserved ESP32 SRAM MMU page mode";
    const unsigned shift=15-mode;
    if((u64)address+size>0x3fc00000ull)return "ESP32 PSRAM access crosses address window";
    for(unsigned i=0;i<size;++i) {
        const u32 offset=address+i-0x3f800000;
        const unsigned index=offset>>shift;
        if(index>=128)return "ESP32 SRAM MMU page outside configured window";
        /* ROM cache_sram_mmu_set uses entry 0x480 for PID0/1, then 128
         * entries of 32/16/8/4/2 KiB selected by CACHE_CTRL1 bits8:6. */
        const u32 mapping=saved(regs,0x3ff11200+core*0x2000+index*4,0x100);
        esp32_cache::record_mmu_entry(regs,core,mapping);
        if(mapping&0x100)return "invalid ESP32 SRAM MMU entry";
        physical[i]=((size_t)(mapping&255)<<shift)|(offset&((1u<<shift)-1));
        if(physical[i]>=memory.size())return "ESP32 SRAM MMU mapping exceeds PSRAM";
    }
    return 0;
}
}

bool EspMachine::esp32_psram_read(u32 address,unsigned size,u32 &value,bool instruction,bool &handled)
{
    handled=address>=0x3f800000 && address<0x3fc00000;
    if(!handled)return false;
    if(instruction)return fault("ESP32 PSRAM is not executable",address);
    if(!psram_.empty() && !(saved(registers_,active_core_?0x3ff00058:0x3ff00040,active_core_?0x10:8)&8))
        esp32_cache::record_disabled_access(registers_,interrupts_,active_core_,address);
    size_t physical[4];const char *error=translate(registers_,psram_,active_core_,address,size,false,physical);
    if(error)return fault(error,address);
    value=0;for(unsigned i=0;i<size;++i)value|=(u32)(u8)psram_[physical[i]]<<(i*8);
    return true;
}

bool EspMachine::esp32_psram_write(u32 address,unsigned size,u32 value,bool &handled)
{
    handled=address>=0x3f800000 && address<0x3fc00000;
    if(!handled)return false;
    if(!psram_.empty() && !(saved(registers_,active_core_?0x3ff00058:0x3ff00040,active_core_?0x10:8)&8))
        esp32_cache::record_disabled_access(registers_,interrupts_,active_core_,address);
    size_t physical[4];const char *error=translate(registers_,psram_,active_core_,address,size,true,physical);
    if(error)return fault(error,address);
    for(unsigned i=0;i<size;++i)psram_[physical[i]]=(char)(value>>(i*8));
    return true;
}
