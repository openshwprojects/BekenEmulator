#ifndef BK_ESP_AES_H
#define BK_ESP_AES_H
#include "common.h"

namespace esp_aes {
/* Register-backed state keeps CPU cores independent from this bus device.
 * Synchronize before changing the shared crypto clock/reset registers. */
void synchronize(std::map<u32,u32> &registers,u64 apb_ticks);
void reset(std::map<u32,u32> &registers);
bool read(std::map<u32,u32> &registers,u32 address,u64 apb_ticks,u32 &value);
bool write(std::map<u32,u32> &registers,u32 address,u64 apb_ticks,u32 value,std::string &error);
}
#endif
