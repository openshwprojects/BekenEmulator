#include "esp32_cache.h"
#include "esp32_interrupts.h"

/* ESP32 DPORT_DCACHE_DBUG3 and CACHE_IA_INT_EN fields, as documented in
 * soc/dport_reg.h and used by esp_system/port/soc/esp32/cache_err_int.c.
 * Error latches survive interrupt masking and cache re-enabling; a digital
 * reset clears the controller registers. A rejected access stays rejected. */
namespace esp32_cache {
namespace {
u32 get(const std::map<u32,u32> &regs,u32 address)
{
    std::map<u32,u32>::const_iterator i=regs.find(address);return i==regs.end()?0:i->second;
}
u32 debug_register(unsigned core){return core?0x3ff00424:0x3ff003fc;}
}

bool read(const std::map<u32,u32> &regs,u32 address,u32 &value)
{
    if(address!=0x3ff003fc && address!=0x3ff00424)return false;
    value=get(regs,address)&0xffff;return true;
}

void update_interrupt(const std::map<u32,u32> &regs,Esp32Interrupts &interrupts)
{
    const u32 enables=get(regs,0x3ff005a0);
    bool pending=false;
    for(unsigned core=0;core<2;++core) {
        const u32 flags=get(regs,debug_register(core));
        for(unsigned reason=0;reason<6;++reason)
            if((flags&(1u<<(14-reason))) && (enables&(1u<<((core?0:14)+reason))))pending=true;
    }
    interrupts.set_source(68,pending);
}

void record_mmu_entry(std::map<u32,u32> &regs,unsigned core,u32 entry)
{
    if(core>1)return;
    const u32 address=debug_register(core);
    regs[address]=(get(regs,address)&0xfe00)|(entry&0x1ff);
}

void record_disabled_access(std::map<u32,u32> &regs,Esp32Interrupts &interrupts,unsigned core,u32 address)
{
    if(core>1)return;
    unsigned bit;
    if(address>=0x3f400000 && address<0x3f800000)bit=14; /* DROM0 */
    else if(address>=0x3f800000 && address<0x3fc00000)bit=10; /* DRAM1 / PSRAM */
    else if(address>=0x40000000 && address<0x40400000)bit=13; /* IRAM0 */
    else if(address>=0x40400000 && address<0x40800000)bit=12; /* IRAM1 */
    else if(address>=0x40800000 && address<0x40c00000)bit=11; /* IROM0 */
    else return;
    regs[debug_register(core)]|=1u<<bit;
    update_interrupt(regs,interrupts);
}
}
