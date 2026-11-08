#ifndef BK_ESP8266_MAC_TIMERS_H
#define BK_ESP8266_MAC_TIMERS_H
#include "common.h"
#include <map>

/* Wi-Fi TSF time and the independent MacTim1 alarm, measured in microseconds.
 * APB timestamps keep the fractional microsecond across reads and clock stops. */
namespace esp8266_mac_timers {
typedef std::map<u32,u32> Registers;
bool handles(u32 address);
bool read(Registers &registers,u32 address,u64 apb_ticks,u32 &value);
bool write(Registers &registers,u32 address,u32 value,u64 apb_ticks);
void advance(Registers &registers,u64 apb_ticks);
bool next_event_ticks(const Registers &registers,u64 apb_ticks,u64 &ticks);
}
#endif
