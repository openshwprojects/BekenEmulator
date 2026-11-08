#ifndef BK_ESP32_RNG_H
#define BK_ESP32_RNG_H
#include "common.h"

/* ESP32 bootloader ADC/I2S entropy routing and hardware PRNG. State stays in
 * the owning machine; no host device or host randomness is accessed. */
namespace esp32_rng {
enum Result { UNHANDLED, OK, ERROR };
Result read(std::map<u32,u32> &registers,u32 address,u32 &value);
Result write(std::map<u32,u32> &registers,u32 address,u32 value,std::string *error=0);
void reset_i2s(std::map<u32,u32> &registers,unsigned controller=0);
}
#endif
