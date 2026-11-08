#include "esp_spi.h"
#include "esp_psram.h"

/* ESP32 soc/spi_reg.h and ESP8266 esp8266/spi_register.h define the same
 * command engine with different data windows and transfer-length fields.
 * NOR operations complete synchronously but obey WEL, page wrapping, and
 * one-to-zero programming. Unsupported transactions fail explicitly. */
namespace esp_spi {
namespace {
const u32 STATUS = 0xff800000, STATUS2 = 0xff800004, POWER_DOWN = 0xff800008;
struct Reg { u32 offset, reset, mask; };
const Reg common[] = {
    {0x04,0,0xffffffff}, {0x08,0x0020a400,0x07f0fc00},
    {0x0c,0x5fff0000,0xffff0000}, {0x10,0,0xffff},
    {0x14,0x11,0xffff00ff}, {0x18,0x80003043,0xffffffff},
    {0x1c,0x80000040,0xfffffcf5}, {0x20,0x5c000007,0xffffffff},
    {0x24,0x70000000,0xf000ffff}, {0xfc,0,3}
};
const Reg extra32[] = {
    {0x28,0,0xffffff}, {0x2c,0,0xffffff}, {0x30,0,0xffffffff},
    {0x34,6,0x600039e7}, {0x38,0x20,0xf8000fff},
    {0x3c,0x02000000,0xfe00ffff}, {0x40,0,0xffffffff}, {0x44,0,0xffffffff},
    {0x48,0,0xffffff}, {0x4c,0,0xffffff}, {0x50,0,0xf},
    {0x54,0x15c04830,0x1ffffffe}, {0x58,0,0x13},
    {0x5c,0,0xf000ffff}, {0x60,0,0xf000ffff}, {0x64,0,0xffffff},
    /* spi_reg.h defines DATE[27:0] as read-only. esp_psram_impl_quad.c
     * uses SPI0 bits 31/30 for the flash/SRAM clock-divider selection. */
    {0x3fc,0x01604270,0xc0000000}
};
const Reg extra8266[] = {
    {0x28,0,0xffffffff}, {0x2c,6,0x20000007},
    {0x30,0,0xff8003ff}, {0x34,0,0xffffffff},
    {0x38,0,0xffffffff}, {0x3c,0,0xffffffff}
};

u32 get(const std::map<u32,u32> &regs,u32 address,u32 initial=0)
{
    std::map<u32,u32>::const_iterator it=regs.find(address);
    return it==regs.end()?initial:it->second;
}

bool decode(esp::Chip chip,u32 address,u32 &base)
{
    /* General-purpose SPI2/3 expose the same read-only revision word.
     * SDK DPORT synchronization reads SPI3 DATE even without using SPI3. */
    if(chip==esp::CHIP_ESP32 && (address==0x3ff643fc || address==0x3ff653fc)) {
        base=address-0x3fc;return true;
    }
    const u32 first=chip==esp::CHIP_ESP32?0x3ff42000:0x60000100;
    const u32 second=chip==esp::CHIP_ESP32?0x3ff43000:0x60000200;
    if (chip!=esp::CHIP_ESP32 && chip!=esp::CHIP_ESP8266) return false;
    if ((address>=first && address<first+0x100) || (chip==esp::CHIP_ESP32 && address==first+0x3fc)) base=first;
    else if ((address>=second && address<second+0x100) || (chip==esp::CHIP_ESP32 && address==second+0x3fc)) base=second;
    else return false;
    return !(address&3);
}

const Reg *reg_for(esp::Chip chip,u32 offset)
{
    for (size_t i=0;i<sizeof(common)/sizeof(common[0]);++i)
        if (common[i].offset==offset) return &common[i];
    const Reg *table=chip==esp::CHIP_ESP32?extra32:extra8266;
    size_t count=chip==esp::CHIP_ESP32?sizeof(extra32)/sizeof(extra32[0]):sizeof(extra8266)/sizeof(extra8266[0]);
    for (size_t i=0;i<count;++i) if (table[i].offset==offset) return &table[i];
    return 0;
}

Result error(std::string *err,const char *message)
{
    if(err) *err=message;
    return ERROR;
}

u8 buffer_byte(const std::map<u32,u32> &regs,u32 buffer,unsigned byte)
{
    return (u8)(get(regs,buffer+(byte&~3u))>>((byte&3)*8));
}

void set_byte(std::map<u32,u32> &regs,u32 buffer,unsigned byte,u8 value)
{
    unsigned shift=(byte&3)*8;
    u32 &word=regs[buffer+(byte&~3u)];
    word=(word&~(0xffu<<shift))|((u32)value<<shift);
}

Result psram_command(std::map<u32,u32> &regs,Bytes &memory,u32 base,u32 trigger,std::string *err)
{
    const bool user=trigger==(1u<<18);
    const u32 config=get(regs,base+0x1c,0x80000040),user1=get(regs,base+0x20,0x5c000007);
    const u32 command=get(regs,base+0x24,0x70000000),address=get(regs,base+4);
    const unsigned tx_offset=user && (config&(1u<<25))?32:0,rx_offset=user && (config&(1u<<24))?32:0;
    const unsigned tx_size=!user || (config&(1u<<27))?(get(regs,base+0x28)>>3)+1:0;
    unsigned rx_size=!user || (config&(1u<<28))?(get(regs,base+0x2c)>>3)+1:0;
    if(tx_size>64-tx_offset || rx_size>64-rx_offset)return error(err,"PSRAM transfer exceeds controller buffer");
    Bytes tx,rx;for(unsigned i=0;i<tx_size;++i)tx+=(char)buffer_byte(regs,base+0x80,tx_offset+i);
    const bool quad=(config&0x8000)!=0 || (get(regs,base+8,0x20a400)&0x01000000)!=0;
    u32 opcode=0,physical=0;unsigned address_bits=0;
    if(!user) {
        if(trigger==(1u<<31))opcode=3;
        else if(trigger==(1u<<25))opcode=2;
        else if(trigger==(1u<<28)){opcode=0x9f;rx_size=3;}
        else return error(err,"unsupported SPI PSRAM controller command");
        physical=address&0xffffff;address_bits=24;
    } else {
        const unsigned command_bits=(config&(1u<<31))?((command>>28)&15)+1:0;
        address_bits=(config&(1u<<30))?(user1>>26)+1:0;
        if(address_bits>32)return error(err,"unsupported PSRAM address bit length");
        if(command_bits==8) {
            opcode=command&255;
            if(address_bits)physical=address_bits==32?address:address>>(32-address_bits);
        } else if(command_bits==0 || (command_bits==2 && !(command&3))) {
            /* IDF's DCLK mode uses two guard cycles, then sends the actual
             * opcode in the leading address byte (or raw quad MOSI byte). */
            if(address_bits>=8) {
                opcode=address>>24;address_bits-=8;
                physical=address_bits?(address>>(24-address_bits))&((1u<<address_bits)-1):0;
            } else if(!tx.empty()) {
                if(quad && tx.size()>1 && tx[0]==0)tx.erase(0,1);
                opcode=(u8)tx[0];tx.erase(0,1);
                if((opcode==2 || opcode==0x38 || opcode==3 || opcode==0x0b || opcode==0xeb) && tx.size()>=3) {
                    physical=((u32)(u8)tx[0]<<16)|((u32)(u8)tx[1]<<8)|(u8)tx[2];
                    address_bits=24;tx.erase(0,3);
                }
            } else return error(err,"PSRAM transfer has no command phase");
        } else return error(err,"unsupported PSRAM command bit length");
    }
    if((opcode==2 || opcode==0x38 || opcode==3 || opcode==0x0b || opcode==0xeb) && address_bits!=24)
        return error(err,"PSRAM memory command requires a 24-bit address");
    if(!esp_psram::command(regs,memory,(u8)opcode,physical,quad,tx,rx_size,rx,err))return ERROR;
    for(unsigned i=0;i<rx.size();++i)set_byte(regs,base+0x80,rx_offset+i,(u8)rx[i]);
    return OK;
}

Result command(esp::Chip chip,std::map<u32,u32> &regs,Bytes &flash,
               u32 base,u32 trigger,std::string *err,Bytes *psram)
{
    if (!trigger) return OK;
    if ((trigger&0xfffc0000u)!=trigger || (trigger&(trigger-1)))
        return error(err,"unsupported or simultaneous SPI command bits");
    bool user=trigger==(1u<<18);
    const u32 pin=base+(chip==esp::CHIP_ESP32?0x34:0x2c);
    if(chip==esp::CHIP_ESP32 && psram && !psram->empty() && !(get(regs,pin,6)&2)) {
        if(!(get(regs,pin,6)&1))return error(err,"simultaneous NOR and PSRAM chip selects");
        return psram_command(regs,*psram,base,trigger,err);
    }
    if(get(regs,pin,6)&1) {
        /* NOR is wired to CS0. The default machine has no optional device
         * on CS1/CS2. Unanswered transfers leave MISO high and never mutate
         * flash or its status latches, including raw PSRAM QPI commands. */
        const u32 user_reg=get(regs,base+0x1c,0x80000040);
        if(!user || (user_reg&(1u<<28))) {
            const u32 buffer=base+(chip==esp::CHIP_ESP32?0x80:0x40);
            const unsigned offset=user && chip==esp::CHIP_ESP32 && (user_reg&(1u<<24))?32:0;
            const u32 user1=get(regs,base+0x20,0x5c000007),addr=get(regs,base+4);
            const unsigned rx=chip==esp::CHIP_ESP32?(get(regs,base+0x2c)>>3)+1:
                user?(((user1>>8)&0x1ff)>>3)+1:(addr>>24)?(addr>>24):32;
            if(rx>64-offset)return error(err,"SPI transfer exceeds controller data buffer");
            for(unsigned i=0;i<rx;++i)set_byte(regs,buffer,offset+i,0xff);
        }
        return OK;
    }
    u32 opcode=0;
    if (user) {
        u32 cmd=get(regs,base+0x24,0x70000000);
        if (((cmd>>28)&15)!=7) return error(err,"SPI flash requires an 8-bit command");
        opcode=cmd&0xff;
    } else {
        switch(trigger) {
        case 1u<<31: opcode=3;break;
        case 1u<<30: opcode=6;break;
        case 1u<<29: opcode=4;break;
        case 1u<<28: opcode=0x9f;break;
        case 1u<<27: opcode=5;break;
        case 1u<<26: opcode=1;break;
        case 1u<<25: opcode=2;break;
        case 1u<<24: opcode=0x20;break;
        case 1u<<23: opcode=0xd8;break;
        case 1u<<22: opcode=0xc7;break;
        case 1u<<21: opcode=0xb9;break;
        case 1u<<20: opcode=0xab;break;
        default:return error(err,"unsupported SPI hardware command");
        }
    }
    u32 user_reg=get(regs,base+0x1c,0x80000040), addr=get(regs,base+4);
    u32 user1=get(regs,base+0x20,0x5c000007);
    size_t physical=addr&0xffffff;
    if (user && chip==esp::CHIP_ESP32 && (user_reg&(1u<<30))) {
        unsigned bits=(user1>>26)+1;
        physical=bits>=32?addr:(addr>>8)&0xffffff;
    }
    u32 buffer=base+(chip==esp::CHIP_ESP32?0x80:0x40);
    unsigned rx_offset=user && chip==esp::CHIP_ESP32 && (user_reg&(1u<<24))?32:0;
    unsigned tx_offset=user && chip==esp::CHIP_ESP32 && (user_reg&(1u<<25))?32:0;
    unsigned rx,tx;
    if (chip==esp::CHIP_ESP32) {
        rx=(get(regs,base+0x2c)>>3)+1;
        tx=(get(regs,base+0x28)>>3)+1;
    } else if (user) {
        rx=(((user1>>8)&0x1ff)>>3)+1;
        tx=(((user1>>17)&0x1ff)>>3)+1;
    } else rx=tx=(addr>>24)?(addr>>24):32;
    if (rx>64-rx_offset || tx>64-tx_offset)
        return error(err,"SPI transfer exceeds controller data buffer");
    if (get(regs,POWER_DOWN) && opcode!=0xab) {
        for (unsigned i=0;i<rx;++i) set_byte(regs,buffer,rx_offset+i,0xff);
        return OK; /* a powered-down NOR does not drive MISO */
    }
    switch(opcode) {
    case 3: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
        if (physical>flash.size() || rx>flash.size()-physical)
            return error(err,"SPI read exceeds flash bounds");
        for (unsigned i=0;i<rx;++i) set_byte(regs,buffer,rx_offset+i,(u8)flash[physical+i]);
        return OK;
    case 6: regs[STATUS]|=2;return OK;
    case 4: regs[STATUS]&=~2u;return OK;
    case 0x9f: case 0x90: case 0xab: {
        unsigned capacity=0; size_t bytes=flash.size();
        while(bytes>1) { ++capacity; bytes>>=1; }
        regs[POWER_DOWN]=0;
        /* JEDEC capacity is log2(bytes); legacy ID/signature use capacity-1.
         * Match the supplied NOR size instead of claiming every dump is 4MB. */
        regs[buffer+rx_offset]=opcode==0x9f ? 0x40ef | (capacity<<16) :
            opcode==0x90 ? 0xef | ((capacity-1)<<8) : capacity-1;
        return OK;
    }
    case 5:
        regs[base+0x10]=get(regs,STATUS)&0xfe;
        if(user) for (unsigned i=0;i<rx;++i) set_byte(regs,buffer,rx_offset+i,(u8)get(regs,STATUS));
        return OK;
    case 0x35:
        for (unsigned i=0;i<rx;++i) set_byte(regs,buffer,rx_offset+i,(u8)get(regs,STATUS2));
        return OK;
    case 1: case 0x31:
        if (!(get(regs,STATUS)&2)) return OK;
        if (opcode==0x31) regs[STATUS2]=buffer_byte(regs,buffer,tx_offset);
        else if(user) {
            regs[STATUS]=buffer_byte(regs,buffer,tx_offset)&0xfc;
            if(tx>1) regs[STATUS2]=buffer_byte(regs,buffer,tx_offset+1);
        } else {
            regs[STATUS]=get(regs,base+0x10)&0xfc;
            regs[STATUS2]=(get(regs,base+0x10)>>8)&0xff;
        }
        regs[STATUS]&=~2u;return OK;
    case 2: case 0x32:
        if (!(get(regs,STATUS)&2)) return OK;
        if (get(regs,STATUS)&0x1c) return error(err,"SPI protected-area programming is unsupported");
        for (unsigned i=0;i<tx;++i) {
            size_t at=(physical&~(size_t)255)|((physical+i)&255);
            if(at>=flash.size())return error(err,"SPI program exceeds flash bounds");
        }
        for (unsigned i=0;i<tx;++i) {
            size_t at=(physical&~(size_t)255)|((physical+i)&255);
            flash[at]=(char)((u8)flash[at]&buffer_byte(regs,buffer,tx_offset+i));
        }
        regs[STATUS]&=~2u;return OK;
    case 0x20: case 0x52: case 0xd8: case 0xc7: case 0x60: {
        if (!(get(regs,STATUS)&2)) return OK;
        if (get(regs,STATUS)&0x1c) return error(err,"SPI protected-area erase is unsupported");
        size_t length=opcode==0x20?0x1000:opcode==0x52?0x8000:opcode==0xd8?0x10000:flash.size();
        size_t start=(opcode==0xc7 || opcode==0x60)?0:physical&~(length-1);
        if(start>flash.size() || length>flash.size()-start)return error(err,"SPI erase exceeds flash bounds");
        flash.replace(start,length,length,'\xff');
        regs[STATUS]&=~2u;return OK;
    }
    case 0xb9:regs[POWER_DOWN]=1;return OK;
    default:return error(err,"unsupported SPI NOR opcode");
    }
}
}

bool external_state_register(u32 address)
{
    return address==STATUS || address==STATUS2 || address==POWER_DOWN || esp_psram::external_state_register(address);
}

Result read(esp::Chip chip,std::map<u32,u32> &regs,const Bytes &flash,
            u32 address,u32 &value,std::string *err)
{
    (void)flash; (void)err;
    u32 base;
    if(!decode(chip,address,base))return UNHANDLED;
    u32 offset=address-base, buffer=chip==esp::CHIP_ESP32?0x80:0x40;
    if(offset==0 || offset==0xf8) {value=0;return OK;}
    if(offset>=buffer && offset<buffer+64) {value=get(regs,address);return OK;}
    const Reg *r=reg_for(chip,offset);
    if(!r)return UNHANDLED;
    value=get(regs,address,r->reset);return OK;
}

Result write(esp::Chip chip,std::map<u32,u32> &regs,Bytes &flash,
             u32 address,u32 value,std::string *err,Bytes *psram)
{
    u32 base;
    if(!decode(chip,address,base))return UNHANDLED;
    u32 offset=address-base, buffer=chip==esp::CHIP_ESP32?0x80:0x40;
    if(offset==0)return command(chip,regs,flash,base,value,err,psram);
    if(offset==0xf8)return error(err,"SPI controller state is read-only");
    if(offset>=buffer && offset<buffer+64) {regs[address]=value;return OK;}
    const Reg *r=reg_for(chip,offset);
    if(!r)return UNHANDLED;
    u32 mask=r->mask;
    if(chip==esp::CHIP_ESP32 && offset==0x20)mask=0xfc0000ff;
    if(chip==esp::CHIP_ESP32 && offset==0x3fc && base!=0x3ff43000)mask=0;
    regs[address]=(get(regs,address,r->reset)&~mask)|(value&mask);
    return OK;
}
}
