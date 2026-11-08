#include "esp8266_mac_timers.h"
#include "esp8266_wdev.h"
#include "esp_state.h"
namespace esp8266_mac_timers {
namespace {
const u32 COUNT_LO=0x3ff21048,COUNT_HI=0x3ff2104c,CONTROL=0x3ff21058;
const u32 LOAD_LO=0x3ff2105c,LOAD_HI=0x3ff21060;
const u32 ALARM_CONTROL=0x3ff210c8,ALARM_LO=0x3ff210cc,ALARM_HI=0x3ff210d0;
const u32 ENABLE=0x80000000u,EVENT=1u<<26;
const u32 COUNT=esp_state::MAC_TIMERS_8266,LAST=COUNT+8,PHASE=COUNT+16,ARMED=COUNT+20;
u32 get(const Registers &regs,u32 address)
{
    Registers::const_iterator it=regs.find(address);
    return it==regs.end()?0:it->second;
}
u64 pair(const Registers &regs,u32 address){return get(regs,address)|(u64(get(regs,address+4))<<32);}
void store(Registers &regs,u32 address,u64 value){regs[address]=(u32)value;regs[address+4]=(u32)(value>>32);}
u64 forward_distance(u64 count,u64 target)
{
    const u64 delta=target-count;
    /* The SDK arms within a 32-bit delay, including a carry across the
     * 64-bit TSF wrap. A deadline more than half a turn behind is due. */
    return delta>0x7fffffffffffffffull?0:delta;
}
u64 elapsed(const Registers &regs,u64 now)
{
    if(regs.find(LAST)==regs.end())return 0;
    const u64 last=pair(regs,LAST);
    return now>=last?now-last:0;
}
u64 counter(const Registers &regs,u64 now,unsigned &phase)
{
    const u64 delta=(get(regs,CONTROL)&ENABLE)?elapsed(regs,now):0;
    phase=get(regs,PHASE)+unsigned(delta%80);
    const u64 value=pair(regs,COUNT)+delta/80+phase/80;
    phase%=80;return value;
}
}

bool handles(u32 address)
{
    return address==COUNT_LO || address==COUNT_HI || address==CONTROL ||
        address==LOAD_LO || address==LOAD_HI || address==ALARM_CONTROL ||
        address==ALARM_LO || address==ALARM_HI;
}

void advance(Registers &regs,u64 now)
{
    /* Clock origins change only when software loads or gates the counter.
     * The scheduler calls this after every instruction: an idle device must
     * neither allocate map entries nor continuously rewrite its counter. */
    if(!(get(regs,CONTROL)&ENABLE) || !get(regs,ARMED))return;
    const u64 delta=elapsed(regs,now);
    const unsigned phase=get(regs,PHASE)+unsigned(delta%80);
    const u64 increments=delta/80+phase/80;
    const u64 count=pair(regs,COUNT);
    if(forward_distance(count,pair(regs,ALARM_LO))<=increments) {
        regs[ARMED]=0;
        esp8266_wdev::raise(regs,EVENT);
    }
}

bool read(Registers &regs,u32 address,u64 now,u32 &value)
{
    if(!handles(address))return false;
    advance(regs,now);
    unsigned phase=0;
    if(address==COUNT_LO)value=(u32)counter(regs,now,phase);
    else if(address==COUNT_HI)value=(u32)(counter(regs,now,phase)>>32);
    else value=get(regs,address);
    return true;
}

bool write(Registers &regs,u32 address,u32 value,u64 now)
{
    if(!handles(address) || address==COUNT_LO || address==COUNT_HI)return false;
    advance(regs,now);
    if(address==CONTROL || address==LOAD_LO || address==LOAD_HI || address==ALARM_CONTROL) {
        unsigned phase=0;const u64 count=counter(regs,now,phase);
        store(regs,COUNT,count);regs[PHASE]=phase;store(regs,LAST,now);
    }
    regs[address]=value;
    /* wDev_Reset_TBTT pauses the TSF, writes both load words, then enables
     * it. wDev_MacTim1Arm writes an absolute 64-bit deadline and sets bit31
     * of its control word. Each arm produces one event; W1C at WDEV c24
     * acknowledges that event independently of counter and alarm state. */
    if(address==LOAD_LO || address==LOAD_HI) {
        regs[COUNT+(address==LOAD_HI?4:0)]=value;regs[PHASE]=0;
    }
    if(address==ALARM_CONTROL)regs[ARMED]=(value&ENABLE)?1:0;
    advance(regs,now);return true;
}

bool next_event_ticks(const Registers &regs,u64 now,u64 &ticks)
{
    if(!(get(regs,CONTROL)&ENABLE) || !get(regs,ARMED))return false;
    unsigned phase=0;const u64 count=counter(regs,now,phase);
    const u64 distance=forward_distance(count,pair(regs,ALARM_LO));
    if(distance>~u64(0)/80)return false;
    ticks=distance?distance*80-phase:0;return true;
}
}
