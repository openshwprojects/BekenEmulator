#ifndef BK_ESP32_CACHE_H
#define BK_ESP32_CACHE_H
#include "common.h"
#include <map>
class Esp32Interrupts;

namespace esp32_cache {
bool read(const std::map<u32,u32> &registers,u32 address,u32 &value);
void record_mmu_entry(std::map<u32,u32> &registers,unsigned core,u32 entry);
void record_disabled_access(std::map<u32,u32> &registers,Esp32Interrupts &interrupts,unsigned core,u32 address);
void update_interrupt(const std::map<u32,u32> &registers,Esp32Interrupts &interrupts);
}
#endif
