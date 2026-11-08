#ifndef BK_ESP8266_WDEV_H
#define BK_ESP8266_WDEV_H
#include "common.h"
#include <map>

/* ESP8266 Wi-Fi MAC registers. The device owns event latches separately from
 * its interrupt mask; the machine routes the resulting line to native IRQ0
 * and, when selected by DPORT, the edge-latched MAC NMI path. */
namespace esp8266_wdev {
typedef std::map<u32,u32> Registers;
bool handles(u32 address);
bool read(Registers &registers,u32 address,u64 microseconds,u32 &value);
bool write(Registers &registers,u32 address,u32 value);
void raise(Registers &registers,u32 events);
/* Hardware producers latch per-queue collision/expiry state and its MAC IRQ.
 * Masks contain queue numbers in bits0..11; guest writes only clear flags. */
void raise_queue_events(Registers &registers,u32 collisions,u32 expirations);
bool interrupt(const Registers &registers);
bool key_memory(u32 address,unsigned size);
bool read_key(const Registers &registers,u32 address,unsigned size,u32 &value);
bool write_key(Registers &registers,u32 address,unsigned size,u32 value);
}
#endif
