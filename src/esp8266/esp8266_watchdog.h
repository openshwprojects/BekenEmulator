#ifndef BK_ESP8266_WATCHDOG_H
#define BK_ESP8266_WATCHDOG_H
#include "common.h"
namespace esp8266_watchdog {
typedef std::map<u32,u32> Registers;
enum Event { NO_EVENT=0, INTERRUPT=1, RESET=2 };
bool handles(u32 address);
bool read(const Registers &registers,u32 address,u64 ticks,u32 &value);
bool write(Registers &registers,u32 address,u32 value,u64 ticks);
unsigned advance(Registers &registers,u64 ticks);
bool next_event_ticks(const Registers &registers,u64 now,u64 &ticks);
}
#endif
