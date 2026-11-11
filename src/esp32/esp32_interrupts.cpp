#include "esp32/esp32_interrupts.h"
#include "esp/esp_machine.h"
#include <string.h>

namespace {
const u32 ENABLE=0x80000000u,UP=0x40000000u,RELOAD=0x20000000u,ALARM=0x400,LEVEL=0x800,EDGE=0x1000;
const u32 LEVEL_IRQS=0x8fbe333fu,EDGE_IRQS=0x50404400u;
const u32 MATRIX0=0x3ff00104u,MATRIX1=0x3ff00218u;
unsigned divider(u32 config) {
    unsigned d=(config>>13)&65535;
    return !d?65536:d<2?2:d;
}
bool timer_address(u32 address,unsigned &group,u32 &offset) {
    if (address>=0x3ff5f000u && address<0x3ff5f100u) group=0;
    else if (address>=0x3ff60000u && address<0x3ff60100u) group=1;
    else return false;
    offset=address-(0x3ff5f000u+group*0x1000);
    return !(offset&3) && (offset<=0x44 || (offset>=0x70 && offset<=0xa4));
}
bool matrix_address(u32 address,unsigned &core,unsigned &source) {
    if (address>=MATRIX0 && address<MATRIX1) { core=0; source=(address-MATRIX0)/4; }
    else if (address>=MATRIX1 && address<MATRIX1+69*4) { core=1; source=(address-MATRIX1)/4; }
    else return false;
    return !(address&3);
}
unsigned irq_level(unsigned irq) {
    static const u32 masks[8]={0,0x000637ff,0x00380000,0x28c08800,0x53000000,0x84010000,0,0x4000};
    for (unsigned n=1;n<8;++n) if (masks[n]&(1u<<irq)) return n;
    return 0;
}
void set_word(u64 &target,u32 value,bool high) {
    target=high?((target&0xffffffffu)|(u64(value)<<32)):((target&~u64(0xffffffffu))|value);
}
}

Esp32Interrupts::Esp32Interrupts(XtensaCpu &cpu) {
    cpus_[0]=&cpu;cpus_[1]=0;reset();
}

void Esp32Interrupts::attach_cpu(unsigned core,XtensaCpu *cpu) {
    if(core>=2 || cpus_[core]==cpu)return;
    if(cpus_[core]) for(unsigned i=0;i<32;++i)
        if((LEVEL_IRQS|EDGE_IRQS)&(1u<<i))cpus_[core]->set_irq(i,false);
    cpus_[core]=cpu;previous_edges_[core]=0;
    route();
}

void Esp32Interrupts::reset() {
    legacy_timers_.reset();
    memset(timers_,0,sizeof(timers_));
    for(unsigned group=0;group<2;++group) {
        for(unsigned i=0;i<2;++i) timers_[group][i].config=0x60002000;
        timers_[group][2].config=0xffff0000u;
        lact_rtc_[group]=0;
    }
    memset(matrix_,16,sizeof(matrix_)); // documented reset route: internal timer2, no external input
    memset(sources_,0,sizeof(sources_));
    memset(group_modes_,0,sizeof(group_modes_));
    enabled_[0]=enabled_[1]=raw_[0]=raw_[1]=previous_edges_[0]=previous_edges_[1]=0;
    for(unsigned core=0;core<2;++core) if(cpus_[core])
        for(unsigned i=0;i<32;++i) if((LEVEL_IRQS|EDGE_IRQS)&(1u<<i)) cpus_[core]->set_irq(i,false);
}

bool Esp32Interrupts::handles(u32 address) const {
    unsigned a,b;u32 offset;
    return matrix_address(address,a,b) || timer_address(address,a,offset) ||
        legacy_timers_.handles(address-0x3ff47000u) ||
        (!(address&3) && address>=0x3ff000dc && address<=0x3ff00100);
}

void Esp32Interrupts::route() {
    /* CPU timer/software/profiling inputs cannot be driven by the matrix.
     * Level inputs remain asserted until every sharing source deasserts;
     * edge inputs latch once and are acknowledged in CPU INTCLEAR. */
    for(unsigned core=0;core<2;++core) if(cpus_[core]) {
        u32 routed=0;
        for(unsigned i=0;i<69;++i)if(sources_[core][i])routed|=1u<<matrix_[core][i];
        for(unsigned i=0;i<32;++i) {
            const u32 bit=1u<<i;
            if(LEVEL_IRQS&bit)cpus_[core]->set_irq(i,(routed&bit)!=0);
            else if((EDGE_IRQS&bit) && (routed&bit) && !(previous_edges_[core]&bit))cpus_[core]->set_irq(i);
        }
        previous_edges_[core]=routed&EDGE_IRQS;
    }
}

void Esp32Interrupts::set_source(unsigned source,bool active) {
    if(source>=69 || (sources_[0][source]==active && sources_[1][source]==active))return;
    sources_[0][source]=sources_[1][source]=active;
    route();
}

void Esp32Interrupts::set_core_source(unsigned core,unsigned source,bool active) {
    if(core>=2 || source>=69 || sources_[core][source]==active)return;
    sources_[core][source]=active;
    route();
}

void Esp32Interrupts::update_sources() {
    bool changed=false;
    for(unsigned group=0;group<2;++group) for(unsigned index=0;index<4;++index) {
        const u32 conf=index<2?timers_[group][index].config:
                       index==3?timers_[group][2].config:
                       u32(group_modes_[group][index])<<11;
        const bool pending=(raw_[group]&enabled_[group]&(1u<<index))!=0;
        const unsigned level_source=14+group*4+index,edge_source=58+group*4+index;
        const bool level=pending && (conf&LEVEL),edge=pending && (conf&EDGE);
        for(unsigned core=0;core<2;++core) {
            changed=changed || sources_[core][level_source]!=level || sources_[core][edge_source]!=edge;
            sources_[core][level_source]=level;
            sources_[core][edge_source]=edge;
        }
    }
    if(changed) route();
}

void Esp32Interrupts::configure_group_interrupt(unsigned group,unsigned index,bool level,bool edge) {
    if(group>=2 || index<2 || index>=4) return;
    group_modes_[group][index]=u8((level?1:0)|(edge?2:0));
    update_sources();
}

void Esp32Interrupts::raise_group_interrupt(unsigned group,unsigned index) {
    if(group>=2 || index>=4) return;
    raw_[group]|=1u<<index;
    update_sources();
}

void Esp32Interrupts::update_legacy_sources(unsigned events) {
    for(unsigned index=0;index<2;++index) {
        const unsigned source=56+index;
        if(legacy_timers_.control(index)&1) set_source(source,legacy_timers_.pending(index));
        else {
            /* An edge-mode timer emits a pulse, rather than holding the
             * matrix source high until a level-style acknowledgement. */
            set_source(source,false);
            if(events&(1u<<index)) {set_source(source,true);set_source(source,false);}
        }
    }
}

void Esp32Interrupts::advance(u64 now) {
    update_legacy_sources(legacy_timers_.advance(now));
    for(unsigned group=0;group<2;++group) for(unsigned index=0;index<3;++index) {
        Timer &timer=timers_[group][index];
        const unsigned irq_bit=index==2?3:index;
        const u64 elapsed=now>=timer.timestamp?now-timer.timestamp:0;
        timer.timestamp=now;
        if(!(timer.config&ENABLE)) continue;
        const unsigned div=divider(timer.config);
        u64 whole=elapsed/div;
        timer.phase+=elapsed%div;
        whole+=timer.phase/div; timer.phase%=div;
        const bool up=(timer.config&UP)!=0;
        const u64 distance=(index==2 && up && timer.alarm<=timer.counter)?0:
                           (up?timer.alarm-timer.counter:timer.counter-timer.alarm);
        if((timer.config&ALARM) && distance<=whole) {
            timer.config&=~ALARM;
            raw_[group]|=1u<<irq_bit;
            if(timer.config&RELOAD) {
                timer.counter=timer.load;
                whole-=distance;
            }
        }
        timer.counter+=up?whole:u64(0)-whole;
    }
    update_sources();
}

bool Esp32Interrupts::read(u32 address,u64 now,u32 &value) {
    if(!handles(address)) return false;
    advance(now);
    if(legacy_timers_.handles(address-0x3ff47000u))return legacy_timers_.read(address-0x3ff47000u,value);
    unsigned core,source,group;u32 offset;
    if(matrix_address(address,core,source)) { value=matrix_[core][source]; return true; }
    if(address>=0x3ff000dc && address<=0x3ff000e8) { value=sources_[0][24+(address-0x3ff000dc)/4]?1:0; return true; }
    if(address>=0x3ff000ec && address<=0x3ff00100) {
        const unsigned first=((address-0x3ff000ec)/4%3)*32;
        core=(address-0x3ff000ec)/12;
        value=0;for(unsigned i=first;i<first+32 && i<69;++i) if(sources_[core][i]) value|=1u<<(i-first);
        return true;
    }
    if(!timer_address(address,group,offset)) return false;
    if(offset>=0x98) {
        value=offset==0x98?enabled_[group]:offset==0x9c?raw_[group]:offset==0xa0?(enabled_[group]&raw_[group]):0;
        return true;
    }
    if(offset==0x74) { value=lact_rtc_[group]; return true; }
    Timer &timer=timers_[group][offset>=0x70?2:(offset/0x24)];
    const u32 rel=offset>=0x70?(offset==0x70?0:(offset-0x74)):(offset%0x24);
    switch(rel) {
    case 0: value=timer.config; break;
    case 4: value=u32(timer.captured); break;
    case 8: value=u32(timer.captured>>32); break;
    case 0x10: value=u32(timer.alarm); break;
    case 0x14: value=u32(timer.alarm>>32); break;
    case 0x18: value=u32(timer.load); break;
    case 0x1c: value=u32(timer.load>>32); break;
    default: value=0; break; // write-only update/load triggers
    }
    return true;
}

bool Esp32Interrupts::write(u32 address,u64 now,u32 value) {
    if(!handles(address)) return false;
    advance(now);
    if(legacy_timers_.handles(address-0x3ff47000u)) {
        const bool result=legacy_timers_.write(address-0x3ff47000u,value);
        update_legacy_sources(0);
        return result;
    }
    unsigned core,source,group;u32 offset;
    if(matrix_address(address,core,source)) { matrix_[core][source]=u8(value&31); route(); return true; }
    if(address>=0x3ff000dc && address<=0x3ff000e8) { set_source(24+(address-0x3ff000dc)/4,(value&1)!=0); return true; }
    if(!timer_address(address,group,offset)) return false; // matrix status is read-only
    if(offset>=0x98) {
        if(offset==0x98) enabled_[group]=value&15;
        else if(offset==0xa4) raw_[group]&=~value;
        else return false;
    } else if(offset==0x74) {
        lact_rtc_[group]=value;
    } else {
        Timer &timer=timers_[group][offset>=0x70?2:(offset/0x24)];
        const u32 rel=offset>=0x70?(offset==0x70?0:(offset-0x74)):(offset%0x24);
        switch(rel) {
        case 0:
            if((timer.config^value)&0x1fffe000u) timer.phase=0;
            timer.config=value&0xfffffc00u;
            if(offset>=0x70 && (timer.config&(ENABLE|ALARM))==(ENABLE|ALARM) &&
               ((timer.config&UP)?(timer.counter>=timer.alarm):(timer.counter<=timer.alarm))) {
                timer.config&=~ALARM;
                raw_[group]|=1u<<3;
                if(timer.config&RELOAD) timer.counter=timer.load;
            }
            break;
        case 0xc: timer.captured=timer.counter; break;
        case 0x10: set_word(timer.alarm,value,false); break;
        case 0x14: set_word(timer.alarm,value,true); break;
        case 0x18: set_word(timer.load,value,false); break;
        case 0x1c: set_word(timer.load,value,true); break;
        case 0x20: timer.counter=timer.load;timer.phase=0;break;
        default: return false;
        }
    }
    update_sources();
    return true;
}

bool Esp32Interrupts::source_can_wake(unsigned source) const {
    return source_can_wake(source,0) || source_can_wake(source,1);
}

bool Esp32Interrupts::source_can_wake(unsigned source,unsigned core) const {
    if(source>=69 || core>=2 || !cpus_[core])return false;
    const XtensaCpu &cpu=*cpus_[core];
    const unsigned irq=matrix_[core][source];
    const u32 bit=1u<<irq;
    if(!((LEVEL_IRQS|EDGE_IRQS)&bit)) return false;
    if((EDGE_IRQS&bit) && (previous_edges_[core]&bit)) return false;
    if(irq==14) return true;
    if(!(cpu.special(XT_SR_INTENABLE)&bit)) return false;
    unsigned mask=cpu.special(XT_SR_PS)&15;
    if((cpu.special(XT_SR_PS)&16) && mask<3) mask=3;
    return irq_level(irq)>mask;
}

bool Esp32Interrupts::next_event_ticks(u64 &ticks) const {
    bool found=false;
    for(unsigned index=0;index<2;++index) {
        u64 delta;
        if(source_can_wake(56+index) && legacy_timers_.next_event_ticks(index,delta) && (!found || delta<ticks)) {
            ticks=delta;found=true;
        }
    }
    for(unsigned group=0;group<2;++group) for(unsigned index=0;index<3;++index) {
        const Timer &timer=timers_[group][index];
        const unsigned irq_bit=index==2?3:index;
        if((timer.config&(ENABLE|ALARM))!=(ENABLE|ALARM) || !(enabled_[group]&(1u<<irq_bit))) continue;
        if(!((timer.config&LEVEL) && source_can_wake(14+group*4+irq_bit)) &&
           !((timer.config&EDGE) && source_can_wake(58+group*4+irq_bit))) continue;
        const u64 distance=(index==2 && (timer.config&UP) && timer.alarm<=timer.counter)?0:
                           ((timer.config&UP)?timer.alarm-timer.counter:timer.counter-timer.alarm);
        const unsigned div=divider(timer.config);
        if(distance>~u64(0)/div) continue; // distant 64-bit wrap cannot fit our time horizon
        u64 delta=distance*div;
        delta=delta>timer.phase?delta-timer.phase:1;
        if(!found || delta<ticks) { ticks=delta;found=true; }
    }
    return found;
}

unsigned EspMachine::apb_frequency() const {
    return chip_==esp::CHIP_ESP32 && cpu_mhz_<80?cpu_mhz_:80;
}

u64 EspMachine::esp32_apb_ticks() const {
    const u64 cycles=cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_;
    const unsigned apb_mhz=apb_frequency();
    return apb_origin_ticks_+(delay_us_-apb_origin_delay_us_)*apb_mhz+
        (cycles/cpu_mhz_)*apb_mhz+(cycles%cpu_mhz_)*apb_mhz/cpu_mhz_;
}

void EspMachine::update_esp32_interrupts() {
    if(chip_==esp::CHIP_ESP32) interrupts_.advance(esp32_apb_ticks());
}

bool EspMachine::cycles_to_esp32_interrupt(u64 &cycles) const {
    u64 ticks;
    if(chip_!=esp::CHIP_ESP32 || !interrupts_.next_event_ticks(ticks)) return false;
    return cycles_for_apb_ticks(ticks,cycles);
}

bool EspMachine::cycles_for_apb_ticks(u64 ticks,u64 &cycles) const {
    const unsigned apb_mhz=apb_frequency();
    if(ticks/apb_mhz>~u64(0)/cpu_mhz_) return false;
    cycles=(ticks/apb_mhz)*cpu_mhz_;
    const u64 elapsed_cycles=cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_;
    const u64 phase=((elapsed_cycles%cpu_mhz_)*apb_mhz)%cpu_mhz_;
    const u64 remainder=(ticks%apb_mhz)*cpu_mhz_;
    if(remainder>=phase) {
        const u64 extra=(remainder-phase+apb_mhz-1)/apb_mhz;
        if(cycles>~u64(0)-extra) return false;
        cycles+=extra;
    } else cycles-=(phase-remainder)/apb_mhz;
    if(!cycles) cycles=1;
    return true;
}
