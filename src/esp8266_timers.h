#ifndef BK_ESP8266_TIMERS_H
#define BK_ESP8266_TIMERS_H

#include "xtensa_cpu.h"
#include "esp_frc_timers.h"

/* ESP8266 FRC counters run from the fixed 80 MHz APB clock. The caller
 * supplies monotonic APB ticks, independently of CPU frequency and WAITI. */
class Esp8266Timers {
public:
    explicit Esp8266Timers(XtensaCpu &cpu);
    void reset(u64 now=0);
    bool handles(u32 address) const;
    bool read(u32 address,u64 now,u32 &value);
    bool write(u32 address,u64 now,u32 value);
    void set_routing(u32 edge_enable,u32 nmi_enable,u64 now);
    void advance(u64 now);
    bool next_event_ticks(u64 &ticks) const;
private:
    EspFrcTimers counters_;
    XtensaCpu &cpu_;
    u32 edge_enable_,nmi_enable_;
    void route(unsigned events);
    bool can_wake(unsigned timer) const;
};
#endif
