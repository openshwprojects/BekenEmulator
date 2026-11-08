#include "esp_frc_timers.h"
#include <string.h>

namespace {
unsigned divider(u32 control) {
    const unsigned selection=(control>>2)&3;
    return selection==0?1:selection==1?16:256;
}
}

EspFrcTimers::EspFrcTimers(){reset();}
void EspFrcTimers::reset(u64 now) {
    memset(timers_,0,sizeof(timers_));
    timers_[0].count=0x7fffff;timers_[1].count=0xffffffffu;
    last_=now;
}
bool EspFrcTimers::handles(u32 address) const {
    return address==0x00 || address==0x04 || address==0x08 || address==0x0c ||
        address==0x20 || address==0x24 || address==0x28 || address==0x2c || address==0x30;
}
u64 EspFrcTimers::distance(unsigned index) const {
    const Timer &timer=timers_[index];
    if(!index)return timer.count?timer.count:0x800000ull;
    if(timer.alarm>timer.count)return u64(timer.alarm)-timer.count;
    const u32 base=(timer.control&0x40)?timer.load:0;
    if(timer.alarm<base)return 0;
    return 0x100000000ull-timer.count+timer.alarm-base;
}

unsigned EspFrcTimers::advance(u64 now) {
    if(now<last_){last_=now;return 0;}
    const u64 elapsed=now-last_;last_=now;
    unsigned events=0;
    for(unsigned index=0;index<2;++index) {
        Timer &timer=timers_[index];
        if(!(timer.control&0x80))continue;
        const unsigned div=divider(timer.control);
        u64 steps=elapsed/div;
        const unsigned fractional=unsigned(elapsed%div)+timer.phase;
        steps+=fractional/div;timer.phase=fractional%div;
        if(!steps)continue;
        const u64 until=distance(index);
        if(until && steps>=until){timer.pending=true;events|=1u<<index;}
        if(index) {
            const u64 base=(timer.control&0x40)?timer.load:0;
            const u64 until_wrap=0x100000000ull-timer.count;
            if(steps<until_wrap)timer.count+=u32(steps);
            else timer.count=u32(base+(steps-until_wrap)%(0x100000000ull-base));
        } else if((timer.control&0x40) && steps>=until) {
            const u64 period=timer.load?timer.load:0x800000ull;
            const u64 remainder=(steps-until)%period;
            timer.count=u32((period-remainder)&0x7fffff);
        } else timer.count=(timer.count-u32(steps))&0x7fffff;
    }
    return events;
}

bool EspFrcTimers::read(u32 address,u32 &value) const {
    if(!handles(address))return false;
    const unsigned index=(address>=0x20);
    const Timer &timer=timers_[index];
    switch(address&0x1f) {
    case 0:value=timer.load;break;
    case 4:value=timer.count;break;
    case 8:value=timer.control|(timer.pending?0x100:0);break;
    case 12:value=0;break;
    case 16:value=timer.alarm;break;
    default:return false;
    }
    return true;
}

bool EspFrcTimers::write(u32 address,u32 value) {
    if(!handles(address))return false;
    const unsigned index=(address>=0x20);
    Timer &timer=timers_[index];
    switch(address&0x1f) {
    case 0:timer.load=timer.count=value&(index?0xffffffffu:0x7fffff);timer.phase=0;break;
    case 8:
        if((timer.control^value)&0x8c)timer.phase=0;
        timer.control=value&0xcf;break;
    case 12:if((timer.control&1) && (value&1))timer.pending=false;break;
    case 16:timer.alarm=value;break;
    default:return false;
    }
    return true;
}

bool EspFrcTimers::next_event_ticks(unsigned index,u64 &ticks) const {
    if(index>=2 || !(timers_[index].control&0x80))return false;
    const u64 until=distance(index);
    if(!until)return false;
    ticks=until*divider(timers_[index].control)-timers_[index].phase;
    return true;
}
