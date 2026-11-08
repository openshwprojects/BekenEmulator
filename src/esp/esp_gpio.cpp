#include "esp/esp_machine.h"
#include "esp32/esp32_rtc_io.h"

/* GPIO register semantics shared by both chips. ESP32 INT_ENA selects
 * per-core normal/NMI source levels before the interrupt matrix. Edge status
 * latches until W1TC; an active level immediately reasserts after W1TC. */
namespace {
bool gpio_offset(bool esp32,u32 offset) {
    if(offset&3)return false;
    if(!esp32)return offset<=0x68;
    return (offset<=0x124 && offset!=0x5c) || (offset>=0x130 && offset<=0x5cc);
}
}

bool EspMachine::gpio_state(unsigned pin,bool &input,bool &output,bool &enabled) const
{
    if(pin>(chip_==esp::CHIP_ESP32?39u:16u) ||
       (chip_==esp::CHIP_ESP32 && (pin==24 || (pin>=28 && pin<=31))))return false;
    u32 output_address,enable_address,pad_address,pullup,opendrain;
    bool rtc_pull=false,has_rtc_pad=false,held=false,held_output=false,held_enabled=false;
    unsigned bit=pin&31;
    if(chip_==esp::CHIP_ESP32) {
        static const u8 mux[]={0x44,0x88,0x40,0x84,0x48,0x6c,0x60,0x64,0x68,0x54,0x58,0x5c,0x34,0x38,0x30,0x3c,
            0x4c,0x50,0x70,0x74,0x78,0x7c,0x80,0x8c,0x90,0x24,0x28,0x2c,0,0,0,0,0x1c,0x20,0x14,0x18,4,8,12,16};
        output_address=pin<32?0x3ff44004:0x3ff44010;
        enable_address=pin<32?0x3ff44020:0x3ff4402c;
        pad_address=0x3ff49000+mux[pin];pullup=pin<34?0x100:0;
        opendrain=register_value(0x3ff44088+pin*4)&4;
        esp32_rtc_io::Pad rtc;
        if(esp32_rtc_io::pad(registers_,pin,rtc)) {
            has_rtc_pad=true;rtc_pull=rtc.pullup;
            if(rtc.selected) {
                output_address=0x3ff48400;enable_address=0x3ff4840c;
                bit=rtc.channel+14;opendrain=register_value(0x3ff48428+rtc.channel*4)&4;
            }
            if(rtc.held) {
                held=true;held_output=rtc.output;held_enabled=rtc.enabled;opendrain=rtc.opendrain?4:0;
            }
        }
    } else if(pin==16) {
        output_address=0x60000768;enable_address=0x60000774;bit=0;
        pad_address=0x600007a0;pullup=0;opendrain=0;
    } else {
        static const u8 mux[]={0x34,0x18,0x38,0x14,0x3c,0x40,0x1c,0x20,0x24,0x28,0x2c,0x30,4,8,12,16};
        output_address=0x60000300;enable_address=0x6000030c;
        pad_address=0x60000800+mux[pin];pullup=0x80;
        opendrain=register_value(0x60000328+pin*4)&4;
    }
    output=(register_value(output_address)&(1u<<bit))!=0;
    enabled=(register_value(enable_address)&(1u<<bit))!=0 && !(chip_==esp::CHIP_ESP32 && pin>=34);
    if(held){output=held_output;enabled=held_enabled;}
    std::map<unsigned,int>::const_iterator external=gpio_external_.find(pin);
    if(external!=gpio_external_.end())input=external->second!=0;
    else if(enabled && !(opendrain && output))input=output;
    else input=has_rtc_pad?rtc_pull:(register_value(pad_address)&pullup)!=0;
    return true;
}

bool EspMachine::set_gpio_input(int pin,int level)
{
    bool input,output,enabled;
    if(pin<0 || level<-1 || level>1 || !gpio_state(pin,input,output,enabled))return false;
    update_gpio_interrupts();
    if(level<0)gpio_external_.erase(pin);else gpio_external_[pin]=level;
    update_gpio_interrupts();
    return true;
}

u32 EspMachine::gpio_interrupt_status(unsigned bank,unsigned enable_mask) const
{
    u32 status=0;
    const u32 raw=register_value(bank?0x3ff44050:0x3ff44044);
    for(unsigned bit=0;bit<(bank?8u:32u);++bit) {
        const unsigned pin=bank*32+bit;
        if((raw&(1u<<bit)) && ((register_value(0x3ff44088+pin*4)>>13)&enable_mask))status|=1u<<bit;
    }
    return status;
}

void EspMachine::update_gpio_interrupts()
{
    const bool esp32=chip_==esp::CHIP_ESP32;
    const u32 base=esp32?0x3ff44000:0x60000300;
    u64 levels=0;
    for(unsigned pin=0;pin<(esp32?40u:16u);++pin) {
        bool input,output,enabled;
        if(!gpio_state(pin,input,output,enabled))continue;
        if(esp32) {
            esp32_rtc_io::Pad rtc;
            if(esp32_rtc_io::pad(registers_,pin,rtc) && rtc.selected)continue;
        }
        const u64 bit=u64(1)<<pin;
        if(input)levels|=bit;
        const bool old=(gpio_levels_&bit)!=0;
        const unsigned type=(register_value(base+(esp32?0x88:0x28)+pin*4)>>7)&7;
        const bool triggered=(type==1 && !old && input) || (type==2 && old && !input) ||
            (type==3 && old!=input) || (type==4 && !input) || (type==5 && input);
        if(triggered)registers_[base+(esp32?(pin<32?0x44:0x50):0x1c)]|=1u<<(pin&31);
    }
    gpio_levels_=levels;
    if(esp32) {
        for(unsigned core=0;core<2;++core) {
            const unsigned normal=core?1:4,nmi=core?2:8;
            interrupts_.set_core_source(core,22,(gpio_interrupt_status(0,normal)|gpio_interrupt_status(1,normal))!=0);
            interrupts_.set_core_source(core,23,(gpio_interrupt_status(0,nmi)|gpio_interrupt_status(1,nmi))!=0);
        }
    } else cpu_.set_irq(4,(register_value(base+0x1c)&0xffff)!=0);
}

bool EspMachine::gpio_mmio_read(u32 address,u32 &value)
{
    const bool esp32=chip_==esp::CHIP_ESP32;
    const u32 base=esp32?0x3ff44000:0x60000300,offset=address-base;
    if(!gpio_offset(esp32,offset))return false;
    update_gpio_interrupts();
    if(esp32 && offset==0x38) { value=0x13;return true; }
    if((esp32 && (offset==0x3c || offset==0x40)) || (!esp32 && offset==0x18)) {
        value=u32(gpio_levels_>>((esp32 && offset==0x40)?32:0));return true;
    }
    if(esp32 && offset>=0x60 && offset<=0x84) {
        const unsigned slot=(offset-0x60)/4;
        static const unsigned masks[]={1,2,4,8,16};
        value=gpio_interrupt_status(slot/5,masks[slot%5]);return true;
    }
    value=register_value(address);return true;
}

bool EspMachine::gpio_mmio_write(u32 address,u32 value)
{
    const bool esp32=chip_==esp::CHIP_ESP32;
    const u32 base=esp32?0x3ff44000:0x60000300,offset=address-base;
    if(!gpio_offset(esp32,offset))return false;
    if((esp32 && (offset==0x38 || offset==0x3c || offset==0x40 || (offset>=0x60 && offset<=0x84))) ||
       (!esp32 && offset==0x18))return false;
    update_gpio_interrupts();
    static const u32 roots8266[]={0,0xc,0x1c};
    static const u32 roots32[]={4,0x10,0x20,0x2c,0x44,0x50};
    const u32 *roots=esp32?roots32:roots8266;
    for(unsigned i=0;i<(esp32?6u:3u);++i) {
        const u32 root=roots[i];
        if(offset>=root && offset<=root+8) {
            u32 mask=esp32?(i&1?0xff:0x0effffffu):0xffff;
            if(esp32 && (i==2 || i==3))mask&=i==3?3u:~0u; // GPIO34..39 inputonly
            u32 next=offset==root?value:offset==root+4?register_value(base+root)|value:register_value(base+root)&~value;
            if(!esp32 && root<=0xc) {
                const u32 selectors=root?0x003f0000:0xffff0000;
                next=(next&mask)|(offset==root?(value&selectors):(register_value(base+root)&selectors));
            } else next&=mask;
            registers_[base+root]=next;update_gpio_interrupts();return true;
        }
    }
    if(esp32 && offset>=0x130)value&=offset<0x530?0xff:0xfff;
    else if(esp32 && offset>=0x88)value&=0x3ffff;
    else if(!esp32 && offset>=0x28 && offset<=0x64)value&=0x1f85;
    else if(!esp32 && offset==0x68)value&=0x1ffff;
    registers_[address]=value;update_gpio_interrupts();return true;
}
