#ifndef BK_ESP8266_RADIO_H
#define BK_ESP8266_RADIO_H
#include "common.h"

namespace esp8266_radio {
/* Observations of the modeled RF and baseband gates, not SDK variables.
 * Channel0 means unprogrammed or outside a standard channel center. */
struct State {
    u32 frequency_khz;
    unsigned channel;
    u64 generation;
    bool pll_enabled, power_blocked, channel_tuning;
    bool calibration_held, calibration_pending;
    bool digital_rx_enabled, mac_rx_enabled;
    State() : frequency_khz(0), channel(0), generation(0), pll_enabled(false),
        power_blocked(false), channel_tuning(false), calibration_held(false),
        calibration_pending(false), digital_rx_enabled(false), mac_rx_enabled(false) {}
    bool tx_ready() const {
        return channel && pll_enabled && !power_blocked && !channel_tuning &&
            !calibration_held && !calibration_pending;
    }
    bool rx_ready() const { return tx_ready() && digital_rx_enabled && mac_rx_enabled; }
};

void advance(std::map<u32,u32> &registers,u64 now_us);
State snapshot(std::map<u32,u32> &registers,unsigned xtal_mhz,u64 now_us);
/* Called only for accepted guest writes, including decoded analog I2C keys.
 * A generation change detects tuning/gating away and back during a frame. */
void note_write(std::map<u32,u32> &registers,u32 address,u32 old_value,u32 value);
}
#endif
