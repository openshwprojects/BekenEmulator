#include "esp32_rng.h"

namespace esp32_rng {
namespace {
/* Register definitions and this supported route come from Espressif's
 * sens_reg.h/syscon_reg.h/i2s_reg.h and bootloader_random_esp32.c.
 * Conversion, audio and DMA requests outside the entropy route fail. */
struct Reg {u32 address,reset,mask;};
const Reg configuration[]={
    {0x3ff48800,0x70902,0x1fffffff}, {0x3ff4880c,0x20000a,0x0fffffff},
    {0x3ff4882c,15,0x01ffffff}, {0x3ff48890,0x70902,0x3fffffff},
    /* sens_reg.h: per-pad attenuation and ADC2 RTC route. DONE/DATA are
     * read-only; configuration alone must not manufacture a conversion. */
    {0x3ff48838,0xffffffff,0xffffffff}, {0x3ff48894,0,0xfffe0000},
    {0x3ff66010,0x007f8240,0x07ffffff}, {0x3ff66014,0x1fe,0x7ff},
    {0x3ff66018,0x0208ff08,0xffffffff},
    {0x3ff6601c,0x0f0f0f0f,0xffffffff}, {0x3ff66020,0x0f0f0f0f,0xffffffff},
    {0x3ff66024,0x0f0f0f0f,0xffffffff}, {0x3ff66028,0x0f0f0f0f,0xffffffff},
    {0x3ff6602c,0x0f0f0f0f,0xffffffff}, {0x3ff66030,0x0f0f0f0f,0xffffffff},
    {0x3ff66034,0x0f0f0f0f,0xffffffff}, {0x3ff66038,0x0f0f0f0f,0xffffffff},
    {0x3ff4f008,0x30300,0x7ffff}, {0x3ff4f0a8,0,0xff},
    {0x3ff4f0ac,4,0x3fffff}, {0x3ff4f0b0,0x410186,0xffffff}
};
const u32 RNG_STATE=0xffff1280;

u32 get(const std::map<u32,u32> &regs,u32 address,u32 initial=0)
{
    std::map<u32,u32>::const_iterator it=regs.find(address);
    return it==regs.end()?initial:it->second;
}
const Reg *find(u32 address)
{
    /* The two I2S controllers share the documented register layout, while
     * the entropy input route exists only on I2S0. Keep their state separate. */
    if(address>=0x3ff6d000 && address<0x3ff6d100)address-=0x1e000;
    for(unsigned i=0;i<sizeof(configuration)/sizeof(configuration[0]);++i)
        if(configuration[i].address==address)return &configuration[i];
    return 0;
}
bool is_rng(u32 address){return address==0x60035144 || address==0x3ff75144;}
Result fail(std::string *error,const char *message)
{
    if(error)*error=message;
    return ERROR;
}
bool entropy_route(const std::map<u32,u32> &regs)
{
    return (get(regs,0x3ff480a8)&0xe0000000)==0xa0000000 &&
        (get(regs,0x3ff4882c,15)&16) &&
        (get(regs,0x3ff4880c,0x20000a)&0xc0000)==0xc0000 &&
        (get(regs,0x3ff48890,0x70902)&0x10000000) &&
        (get(regs,0x3ff66010,0x7f8240)&0x04000024)==0x04000024 &&
        (get(regs,0x3ff4f0a8)&0x39)==0x38;
}
}

Result read(std::map<u32,u32> &regs,u32 address,u32 &value)
{
    if(is_rng(address)) {
        value=get(regs,RNG_STATE,0x32febabe);
        if(get(regs,0x3ff000cc,0xfffce030)&0x8000) {
            /* A deterministic PRNG makes guest execution reproducible. It
             * models the readable stream, not physical or secure entropy. */
            value^=value<<13;value^=value>>17;value^=value<<5;
            regs[RNG_STATE]=value;
        }
        return OK;
    }
    const Reg *reg=find(address);
    if(!reg)return UNHANDLED;
    value=get(regs,address,reg->reset);return OK;
}

Result write(std::map<u32,u32> &regs,u32 address,u32 value,std::string *error)
{
    if(is_rng(address))return fail(error,"ESP32 random data is read-only");
    const Reg *reg=find(address);
    if(!reg)return UNHANDLED;
    if(address==0x3ff4882c && (value&0x300)==0x300)
        return fail(error,"ESP32 ULP execution is not implemented");
    if(address==0x3ff66010 && (value&3)==3)
        return fail(error,"ESP32 software ADC conversion is not implemented");
    if(address==0x3ff48894 && (value&0x60000)==0x60000)
        return fail(error,"ESP32 ADC2 RTC software conversion is not implemented");
    if(address==0x3ff4f008 || address==0x3ff6d008) {
        if(value&16)return fail(error,"ESP32 I2S audio transmit is not implemented");
        if((value&32) && !(value&2)) {
            const u32 clock=address==0x3ff4f008?16:1u<<21;
            if(!(get(regs,0x3ff000c0,0xf9c1e06f)&clock) || (get(regs,0x3ff000c4)&clock))
                return fail(error,"ESP32 I2S clock disabled or held in reset");
            if(address!=0x3ff4f008 || !entropy_route(regs))return fail(error,"ESP32 I2S input route is not implemented");
        }
        if(value&2)value&=~32u; /* receive reset stops the internal stream */
    }
    regs[address]=value&reg->mask;return OK;
}

void reset_i2s(std::map<u32,u32> &regs,unsigned controller)
{
    if(controller>1)return;
    for(unsigned i=0;i<sizeof(configuration)/sizeof(configuration[0]);++i)
        if(configuration[i].address>=0x3ff4f000 && configuration[i].address<0x3ff4f100)
            regs.erase(configuration[i].address+controller*0x1e000);
}
}
