#include "esp_psram.h"
#include "esp_state.h"
namespace esp_psram {
namespace {
const u32 QPI=esp_state::PSRAM,RESET_ENABLED=esp_state::PSRAM+4,WRAP32=esp_state::PSRAM+8;
u32 get(const std::map<u32,u32> &regs,u32 key)
{
    std::map<u32,u32>::const_iterator it=regs.find(key);return it==regs.end()?0:it->second;
}
bool fail(std::string *error,const char *message)
{
    if(error)*error=message;
    return false;
}
}

bool qpi_enabled(const std::map<u32,u32> &regs){return get(regs,QPI)!=0;}
bool external_state_register(u32 address){return address==QPI || address==RESET_ENABLED || address==WRAP32;}

bool command(std::map<u32,u32> &regs,Bytes &memory,u8 opcode,u32 address,
             bool quad_command,const Bytes &tx,unsigned rx_size,Bytes &rx,std::string *error)
{
    /* ESP-PSRAM64/64H datasheet, sections 4..8. Espressif's IDF PSRAM driver
     * also specifies the capacity bits for compatible 16/32-Mbit devices.
     * Linear bursts cross 1 KiB rows; C0 toggles a 32-byte address wrap. */
    if(memory.size()!=0x200000 && memory.size()!=0x400000 && memory.size()!=0x800000)
        return fail(error,"unsupported SPI PSRAM capacity");
    if(tx.size()>64 || rx_size>64)return fail(error,"PSRAM transfer exceeds controller buffer");
    rx.assign(rx_size,'\xff');
    if(quad_command!=qpi_enabled(regs))return true; /* chip cannot decode the mismatched bus width */
    const bool reset_enabled=get(regs,RESET_ENABLED)!=0;
    regs[RESET_ENABLED]=0;
    switch(opcode) {
    case 0x9f:
        if(quad_command)return true; /* Read ID is a SPI-only command */
        for(unsigned i=0;i<rx_size;++i)
            rx[i]=(char)(i==0?0x0d:i==1?0x5d:i==2?(memory.size()==0x200000?0:memory.size()==0x400000?0x20:0x40):0);
        return true;
    case 0x35:if(!quad_command)regs[QPI]=1;return true;
    case 0xf5:if(quad_command)regs[QPI]=0;return true;
    case 0x66:regs[RESET_ENABLED]=1;return true;
    case 0x99:
        if(reset_enabled){regs[QPI]=0;regs[WRAP32]=0;}return true;
    case 0xc0:regs[WRAP32]=!get(regs,WRAP32);return true;
    case 3:case 0x0b:case 0xeb:case 2:case 0x38: {
        if(quad_command && (opcode==3 || opcode==0x0b))return true;
        const bool write=opcode==2 || opcode==0x38;
        const unsigned length=write?(unsigned)tx.size():rx_size;
        for(unsigned i=0;i<length;++i) {
            u32 at=get(regs,WRAP32)?(address&~31u)|((address+i)&31):(address+i);
            at&=(u32)memory.size()-1;
            if(write)memory[at]=tx[i];else rx[i]=memory[at];
        }
        return true;
    }
    default:return fail(error,"unsupported SPI PSRAM opcode");
    }
}
}
