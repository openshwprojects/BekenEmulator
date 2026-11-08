#include "esp8266/esp8266_timers.h"

Esp8266Timers::Esp8266Timers(XtensaCpu &cpu):cpu_(cpu){reset();}
void Esp8266Timers::reset(u64 now) {
    counters_.reset(now);edge_enable_=nmi_enable_=0;
    cpu_.set_irq(9,false);cpu_.set_irq(10,false);cpu_.set_irq(14,false);
}
bool Esp8266Timers::handles(u32 address) const {
    return address>=0x60000600 && counters_.handles(address-0x60000600);
}
void Esp8266Timers::advance(u64 now) { route(counters_.advance(now)); }
void Esp8266Timers::route(unsigned events) {
    for(unsigned index=0;index<2;++index) {
        const bool edge=(counters_.control(index)&1)==0;
        const bool enabled=!edge || (edge_enable_&(2u<<index));
        /* Espressif NONOS hw_timer_init(NMI_SOURCE) uses the ordinary FRC1
         * edge gate and NmiTimSetFunc selects DPORT low five bits = 15.
         * The LX106 NMI input is IRQ14, not the SDK handler's RTC IRQ3. */
        const bool nmi=!index && (nmi_enable_&31)==15;
        if(!edge && !nmi)cpu_.set_irq(9+index,enabled && counters_.pending(index));
        else if(enabled && (events&(1u<<index)))cpu_.set_irq(nmi?14:9+index);
    }
}
void Esp8266Timers::set_routing(u32 edge_enable,u32 nmi_enable,u64 now) {
    advance(now);edge_enable_=edge_enable;nmi_enable_=nmi_enable;route(0);
}
bool Esp8266Timers::read(u32 address,u64 now,u32 &value) {
    if(!handles(address))return false;
    advance(now);return counters_.read(address-0x60000600,value);
}
bool Esp8266Timers::write(u32 address,u64 now,u32 value) {
    if(!handles(address))return false;
    advance(now);
    const u32 offset=address-0x60000600;
    /* The 8266 SDK/Arduino acknowledge level mode by writing zero.
     * ESP32's adapter retains the shared engine's W1C convention. */
    if((offset&31)==12)value=1;
    if((offset&31)==8)value&=0xcd;
    const bool ok=counters_.write(offset,value);
    route(0);return ok;
}
bool Esp8266Timers::can_wake(unsigned index) const {
    const bool edge=(counters_.control(index)&1)==0;
    if(edge && !(edge_enable_&(2u<<index)))return false;
    if(!index && (nmi_enable_&31)==15)return (cpu_.special(XT_SR_PS)&15)<3;
    return (cpu_.special(XT_SR_INTENABLE)&(1u<<(9+index))) && !(cpu_.special(XT_SR_PS)&31);
}
bool Esp8266Timers::next_event_ticks(u64 &ticks) const {
    bool found=false;u64 candidate;
    for(unsigned index=0;index<2;++index) {
        if(can_wake(index) && counters_.next_event_ticks(index,candidate) && (!found || candidate<ticks)) {
            ticks=candidate;found=true;
        }
    }
    return found;
}
