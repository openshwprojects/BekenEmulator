#include "esp8266_watchdog.h"
#include "esp_state.h"

/* Register fields: Espressif ESP8266_RTOS_SDK eagle_soc.h/task_wdt.c.
 * Exact counter reload and additional stage/count registers: Cesanta's
 * original hardware measurements, mongoose-os.com/blog/esp8266-watchdog-timer/.
 * The SDK's 0.8ms notation rounds the 65536 / 80MHz base period (819.2us).
 * ROM ets_wdt_enable also confirms bit2 disables the second stage. */
namespace esp8266_watchdog {
namespace {
const u32 BASE=0x60000900,DEADLINE=esp_state::WATCHDOG_8266;
u32 get(const Registers &r,u32 address) {
    Registers::const_iterator i=r.find(address);return i==r.end()?0:i->second;
}
u64 deadline(const Registers &r) {return get(r,DEADLINE)|(u64(get(r,DEADLINE+4))<<32);}
void set_deadline(Registers &r,u64 value) {r[DEADLINE]=u32(value);r[DEADLINE+4]=u32(value>>32);}
u64 period(const Registers &r,unsigned stage) {return u64(1)<<(16+(get(r,BASE+4+stage*4)&15));}
void feed(Registers &r,u64 now) {
    r[BASE+0x10]=0;r[BASE+0xc]=u32(period(r,0));
    set_deadline(r,(get(r,BASE)&1)?now+period(r,0):0);
}
}
bool handles(u32 address) {return address>=BASE && address<=BASE+0x18 && !(address&3);}
bool read(const Registers &r,u32 address,u64 now,u32 &value) {
    if(!handles(address))return false;
    if(address==BASE+0xc && (get(r,BASE)&1)) {
        const u64 end=deadline(r);value=end>now?u32(end-now):0;
    } else value=address>=BASE+0x14?0:get(r,address);
    return true;
}
bool write(Registers &r,u32 address,u32 value,u64 now) {
    if(!handles(address) || address==BASE+0xc || address==BASE+0x10)return false;
    if(address==BASE) {
        const u32 old=get(r,BASE);
        if(!(value&1) && (old&1)) {
            u32 count;read(r,BASE+0xc,now,count);r[BASE+0xc]=count;set_deadline(r,0);
        }
        r[address]=value&63;
        if((value&1) && !(old&1))feed(r,now);
    } else if(address==BASE+4 || address==BASE+8)r[address]=value&15;
    else if((value&255)==0x73) {
        if(address==BASE+0x14)feed(r,now);
        else r[BASE+0x10]=0;
    }
    return true;
}
unsigned advance(Registers &r,u64 now) {
    const u32 config=get(r,BASE);
    if(!(config&1))return NO_EVENT;
    u64 end=deadline(r);
    if(!end || now<end)return NO_EVENT;
    unsigned events=0;
    if(config&4) {set_deadline(r,0);return RESET;}
    if(!get(r,BASE+0x10)) {
        events|=INTERRUPT;r[BASE+0x10]=1;
        end+=period(r,(config&2)?0:1);set_deadline(r,end);
    }
    if(now>=end) {
        if(!(config&2)) {set_deadline(r,0);return events|RESET;}
        const u64 interval=period(r,0);
        set_deadline(r,now+interval-(now-end)%interval);
    }
    return events;
}
bool next_event_ticks(const Registers &r,u64 now,u64 &ticks) {
    const u32 config=get(r,BASE);
    if(!(config&1))return false;
    const u64 end=deadline(r);
    if(!end || ((config&6)==2 && get(r,BASE+0x10)))return false;
    ticks=end>now?end-now:0;return true;
}
}
