#ifndef BK_ESP_RF_MATH_H
#define BK_ESP_RF_MATH_H
#include "common.h"

/* Register-independent arithmetic shared by the ESP8266/ESP32 radio models.
 * The PLL divider encoding is from Espressif ram_rfpll_set_freq. The tuning
 * curve is supplied by each model, not claimed to describe silicon process. */
namespace esp_rf_math {
bool pll_frequency_khz(u8 integer,u8 fraction_high,u8 fraction_low,
                       u32 xtal_khz,u32 &frequency_khz);
bool vco_capacitor_code(u32 frequency_khz,u32 center_khz,unsigned center_code,
                        u32 step_khz,unsigned &code,unsigned maximum_code=255);
/* Encode 10*log10(power/reference_power) around a caller-selected code.
 * Both noise detectors use this relationship with different resolutions. */
unsigned logarithmic_power_code(double power,double reference_power,
                                 unsigned reference_code,unsigned units_per_db,
                                 unsigned maximum_code);
}
#endif
