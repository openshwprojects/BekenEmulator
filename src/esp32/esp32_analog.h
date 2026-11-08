#ifndef BK_ESP32_ANALOG_H
#define BK_ESP32_ANALOG_H
#include "common.h"
#include <map>
#include <string>
namespace esp32_analog {
enum Result { UNHANDLED, OK, ERROR };
Result read(const std::map<u32,u32> &registers,u32 address,u32 &value);
Result write(std::map<u32,u32> &registers,u32 address,u32 value,std::string &error,
             u64 now_us=0,u32 xtal_khz=40000);
}
#endif
