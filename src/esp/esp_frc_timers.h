#ifndef BK_ESP_FRC_TIMERS_H
#define BK_ESP_FRC_TIMERS_H
#include "common.h"

/* Shared legacy FRC1 down-counter and FRC2 up-counter hardware. Register
 * offsets follow ESP8266 TRM Appendix 4 and ESP32 soc/frc_timer_reg.h.
 * Chip adapters supply clock ticks, interrupt routing and acknowledge rules. */
class EspFrcTimers {
public:
    EspFrcTimers();
    void reset(u64 now=0);
    bool handles(u32 offset) const;
    bool read(u32 offset,u32 &value) const;
    bool write(u32 offset,u32 value);
    unsigned advance(u64 absolute_ticks);
    bool next_event_ticks(unsigned index,u64 &ticks) const;
    u32 control(unsigned index) const { return index<2?timers_[index].control:0; }
    bool pending(unsigned index) const { return index<2 && timers_[index].pending; }
private:
    struct Timer {
        u32 load,count,control,alarm;
        unsigned phase;
        bool pending;
    } timers_[2];
    u64 last_;
    u64 distance(unsigned index) const;
};
#endif
