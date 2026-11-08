#ifndef BK_ESP32_PHY_H
#define BK_ESP32_PHY_H
#include "common.h"
#include <map>
#include <string>
namespace esp32_phy {
enum Result { UNHANDLED, OK, ERROR };
Result read(std::map<u32,u32> &registers,u32 address,u32 &value,u64 now_us=0);
Result write(std::map<u32,u32> &registers,u32 address,u32 value,std::string &error,u64 now_us=0);
void reset_baseband(std::map<u32,u32> &registers);
void reset_frontend(std::map<u32,u32> &registers);
void reset_mac(std::map<u32,u32> &registers);
bool agc_enabled(const std::map<u32,u32> &registers);
int cca_threshold(const std::map<u32,u32> &registers);
/* Two-bit FE force/value fields at shifts8,10,12,14,16. Unforced controls
 * use the caller's receiver/transmitter state, rather than forcing activity. */
bool frontend_signal(const std::map<u32,u32> &registers,unsigned shift,bool automatic);
/* Virtual receiver input: symmetric idle I/Q noise amplitude, 0..511.
 * It changes later acquisitions, never an already latched measurement. */
bool set_noise_amplitude(std::map<u32,u32> &registers,unsigned amplitude);
}
#endif
