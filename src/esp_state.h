#ifndef BK_ESP_STATE_H
#define BK_ESP_STATE_H
#include "common.h"

/* Private register-map storage, never decoded as guest memory. Reserve one
 * 64 KiB domain per device here so unrelated device models cannot collide.
 * Add new domains here instead of choosing another module's unused offset. */
namespace esp_state {
const u32 ANALOG=0xff900000;
const u32 SHA=0xff910000;
const u32 PSRAM=0xff920000;
const u32 AES=0xff930000;
const u32 WATCHDOG_8266=0xff960000;
const u32 WDEV_8266=0xff940000;
const u32 MAC_TIMERS_8266=0xff950000;
const u32 RTC_IO=0xff970000;
const u32 PHY_ESP32=0xff980000;
const u32 RADIO_8266=0xff990000;
}
#endif
