#include "esp/esp_rf_math.h"
#include <math.h>

namespace esp_rf_math {
bool pll_frequency_khz(u8 integer,u8 fraction_high,u8 fraction_low,
                       u32 xtal_khz,u32 &frequency_khz)
{
    if(!xtal_khz)return false;
    const u32 divider=((u32(integer)+32)<<16)|(u32(fraction_high)<<8)|fraction_low;
    const u64 khz=(u64(divider)*xtal_khz*3+131072)/262144;
    if(khz>0xffffffffu)return false;
    frequency_khz=(u32)khz;return true;
}

bool vco_capacitor_code(u32 frequency_khz,u32 center_khz,unsigned center_code,
                        u32 step_khz,unsigned &code,unsigned maximum_code)
{
    if(!step_khz || center_code>maximum_code)return false;
    const bool higher=center_khz>=frequency_khz;
    const u64 difference=higher?u64(center_khz)-frequency_khz:u64(frequency_khz)-center_khz;
    const u64 steps=(difference+step_khz/2)/step_khz;
    if(higher) {
        if(steps>maximum_code-center_code)return false;
        code=center_code+(unsigned)steps;
    } else {
        if(steps>center_code)return false;
        code=center_code-(unsigned)steps;
    }
    return true;
}

unsigned logarithmic_power_code(double power,double reference_power,
                                 unsigned reference_code,unsigned units_per_db,
                                 unsigned maximum_code)
{
    if(!(power>0) || !(reference_power>0))return 0;
    const double code=reference_code+10.0*units_per_db*(log10(power)-log10(reference_power));
    if(code<=0)return 0;
    if(code>=maximum_code)return maximum_code;
    return (unsigned)(code+0.5);
}
}
