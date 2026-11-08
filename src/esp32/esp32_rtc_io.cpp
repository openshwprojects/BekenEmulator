#include "esp32/esp32_rtc_io.h"
#include "esp/esp_state.h"

namespace {
const u32 BASE=0x3ff48400;
struct Register {u32 offset,reset,mask,hold,unsupported;};
/* ESP-IDF classic ESP32 rtc_io_reg.h and rtc_io_periph.c. Only pad and GPIO
 * registers are decoded: touch conversion, ADC/DAC output and RTC
 * wake interrupts require separate active device models. */
const Register pads[]={
    {0x7c,0,0xfffffff0,0xf0000000,0},
    {0x80,0,0xfffc0000,0xc0000000,0},
    {0x84,0x80000000,0xfffffc00,0x20000000,0x00040000},
    {0x88,0x80000000,0xfffffc00,0x20000000,0x00040000},
    {0x8c,0x84100010,0xfffffffe,0x21000000,0x00080000},
    {0x94,0x52000000,0xfbfff000,0x80000000,0x00400000},
    {0x98,0x4a000000,0xfbfff000,0x80000000,0x00400000},
    {0x9c,0x52000000,0xfbfff000,0x80000000,0x00400000},
    {0xa0,0x4a000000,0xfbfff000,0x80000000,0x00400000},
    {0xa4,0x52000000,0xfbfff000,0x80000000,0x00400000},
    {0xa8,0x52000000,0xfbfff000,0x80000000,0x00400000},
    {0xac,0x4a000000,0xfbfff000,0x80000000,0x00400000},
    {0xb0,0x42000000,0xfbfff000,0x80000000,0x00400000}
};
const Register *find(u32 address) {
    for(unsigned i=0;i<sizeof(pads)/sizeof(pads[0]);++i)
        if(address==BASE+pads[i].offset)return &pads[i];
    return 0;
}
u32 stored(const std::map<u32,u32> &r,u32 address,u32 initial=0) {
    std::map<u32,u32>::const_iterator it=r.find(address);
    return it==r.end()?initial:it->second;
}
struct Descriptor {unsigned gpio,offset,mux,input,pull,func;};
const Descriptor pin[]={
    {36,0x7c,27,19,0,22},{37,0x7c,26,14,0,17},
    {38,0x7c,25,9,0,12},{39,0x7c,24,4,0,7},
    {34,0x80,29,23,0,26},{35,0x80,28,18,0,21},
    {25,0x84,17,11,27,15},{26,0x88,17,11,27,15},
    {33,0x8c,18,11,27,15},{32,0x8c,17,5,22,9},
    {4,0x94,19,13,27,17},{0,0x98,19,13,27,17},
    {2,0x9c,19,13,27,17},{15,0xa0,19,13,27,17},
    {13,0xa4,19,13,27,17},{12,0xa8,19,13,27,17},
    {14,0xac,19,13,27,17},{27,0xb0,19,13,27,17}
};
const unsigned hold_shift[]={31,30,29,28,31,30,29,29,29,24,31,31,31,31,31,31,31,31};
const unsigned force_shift[]={4,5,6,7,0,1,2,3,17,16,8,9,10,11,12,13,14,15};
const u32 FORCE=0x3ff480c8,VALID=esp_state::RTC_IO,CONFIG=esp_state::RTC_IO+4,DRIVE=esp_state::RTC_IO+0x4c;

/* Capture before the register write so a hold edge latches the existing pad.
 * Later writes remain visible in registers, but do not alter the held pad.
 * HOLD and HOLD_FORCE are independent reasons to retain the same snapshot. */
void update_holds(std::map<u32,u32> &registers,u32 address,u32 value)
{
    u32 valid=stored(registers,VALID);
    for(unsigned i=0;i<18;++i) {
        const Descriptor &p=pin[i];const Register *r=find(BASE+p.offset);
        const u32 old=stored(registers,BASE+p.offset,r->reset);
        const u32 config=address==BASE+p.offset?value:old;
        const u32 force=address==FORCE?value:stored(registers,FORCE);
        const bool held=(config&(1u<<hold_shift[i])) || (force&(1u<<force_shift[i]));
        const u32 bit=1u<<i;
        if(held && !(valid&bit)) {
            const bool selected=(old&(1u<<p.mux))!=0;
            const unsigned shift=selected?i+14:p.gpio&31;
            const u32 output=selected?BASE:p.gpio<32?0x3ff44004:0x3ff44010;
            const u32 enable=selected?BASE+12:p.gpio<32?0x3ff44020:0x3ff4402c;
            const u32 mode=selected?BASE+0x28+i*4:0x3ff44088+p.gpio*4;
            registers[CONFIG+i*4]=old;
            registers[DRIVE+i*4]=((stored(registers,output)>>shift)&1) |
                (p.gpio<34?((stored(registers,enable)>>shift)&1)<<1:0) |
                (stored(registers,mode)&4);
            valid|=bit;
        } else if(!held)valid&=~bit;
    }
    registers[VALID]=valid;
}
}

namespace esp32_rtc_io {
unsigned gpio(unsigned channel){return channel<18?pin[channel].gpio:40;}
bool retained_state_register(u32 address)
{return !(address&3) && address>=VALID && address<DRIVE+18*4;}

bool pad(const std::map<u32,u32> &registers,unsigned gpio,Pad &result)
{
    for(unsigned i=0;i<18;++i)if(pin[i].gpio==gpio) {
        const Descriptor &p=pin[i];const Register *r=find(BASE+p.offset);
        result.held=(stored(registers,VALID)&(1u<<i))!=0;
        const u32 value=result.held?stored(registers,CONFIG+i*4):stored(registers,BASE+p.offset,r->reset);
        const u32 drive=stored(registers,DRIVE+i*4);
        result.output=(drive&1)!=0;result.enabled=(drive&2)!=0;result.opendrain=(drive&4)!=0;
        result.channel=i;result.selected=(value&(1u<<p.mux))!=0;
        result.input_enabled=(value&(1u<<p.input))!=0;
        result.pullup=p.pull && (value&(1u<<p.pull));return true;
    }
    return false;
}

Result read(const std::map<u32,u32> &registers,u32 address,u32 inputs,u32 &value)
{
    if(address&3)return UNHANDLED;
    if(address==FORCE){value=stored(registers,address);return OK;}
    const Register *r=find(address);
    if(r){value=stored(registers,address,r->reset);return OK;}
    const u32 offset=address-BASE;
    if(offset<=0x14){value=offset%12?0:stored(registers,address);return OK;}
    if(offset==0x24) {
        value=0;
        for(unsigned i=0;i<18;++i) {
            Pad p;pad(registers,pin[i].gpio,p);
            if(p.selected && p.input_enabled)value|=inputs&(1u<<(i+14));
        }
        return OK;
    }
    if(offset>=0x28 && offset<=0x6c){value=stored(registers,address);return OK;}
    return UNHANDLED;
}

Result write(std::map<u32,u32> &registers,u32 address,u32 value,std::string &error)
{
    if(address&3)return UNHANDLED;
    if(address==FORCE){update_holds(registers,address,value);registers[address]=value&0x3ffff;return OK;}
    const Register *r=find(address);
    if(r) {
        if(value&r->unsupported){error="ESP32 RTC analog pad operation is not implemented";return ERROR;}
        for(unsigned i=0;i<18;++i)if(BASE+pin[i].offset==address && (value&(1u<<pin[i].mux)) && ((value>>pin[i].func)&3)) {
            error="ESP32 RTC pad alternate function is not implemented";return ERROR;
        }
        update_holds(registers,address,value);registers[address]=value&r->mask;return OK;
    }
    const u32 offset=address-BASE;
    if(offset<=0x14) {
        const u32 root=offset<12?0:12,op=offset-root;
        const u32 mask=root?0xfff00000u:0xffffc000u;
        u32 next=op==0?value:op==4?stored(registers,BASE+root)|value:stored(registers,BASE+root)&~value;
        registers[BASE+root]=next&mask;return OK;
    }
    if(offset>=0x28 && offset<=0x6c) {
        if(value&0x780){error="ESP32 RTC GPIO interrupt/wakeup is not implemented";return ERROR;}
        registers[address]=value&4;return OK;
    }
    return UNHANDLED;
}
}
