#ifndef ESP32_RTC_IO_H
#define ESP32_RTC_IO_H
#include "common.h"
#include <map>
#include <string>

/* Classic ESP32 RTC pads share their pull resistors with digital GPIO. */
namespace esp32_rtc_io {
enum Result { UNHANDLED, OK, ERROR };
struct Pad {
    unsigned channel;
    bool selected, input_enabled, pullup, held, output, enabled, opendrain;
};
bool pad(const std::map<u32,u32> &registers,unsigned gpio,Pad &result);
unsigned gpio(unsigned channel);
bool retained_state_register(u32 address);
Result read(const std::map<u32,u32> &registers,u32 address,u32 inputs,u32 &value);
Result write(std::map<u32,u32> &registers,u32 address,u32 value,std::string &error);
}
#endif
