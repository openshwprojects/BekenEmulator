#include "esp_rf_math.h"
#include <stdio.h>

int esp_rf_math_tests()
{
    int checks=0;u32 khz=0;unsigned code=0;
#define RF_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] RF math line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    using namespace esp_rf_math;
    /* N = RF/(XTAL*3/4)-32, with a 16-bit fraction. */
    RF_CHECK(pll_frequency_khz(48,0,0,40000,khz) && khz==2400000);
    RF_CHECK(pll_frequency_khz(49,0x80,0,40000,khz) && khz==2445000);
    RF_CHECK(pll_frequency_khz(93,0,0,26000,khz) && khz==2437500);
    RF_CHECK(pll_frequency_khz(0,0,0,40000,khz) && khz==960000);
    RF_CHECK(pll_frequency_khz(0xff,0xff,0xff,40000,khz) && khz==8640000);
    RF_CHECK(pll_frequency_khz(48,0,1,40000,khz) && khz==2400000);
    RF_CHECK(pll_frequency_khz(48,0,2,40000,khz) && khz==2400001);
    khz=123;
    RF_CHECK(!pll_frequency_khz(48,0,0,0,khz) && khz==123);
    RF_CHECK(!pll_frequency_khz(0xff,0xff,0xff,0xffffffff,khz) && khz==123);
    RF_CHECK(vco_capacitor_code(2437000,2437000,128,4000,code) && code==128);
    RF_CHECK(vco_capacitor_code(2412000,2437000,128,4000,code) && code==134);
    RF_CHECK(vco_capacitor_code(2462000,2437000,128,4000,code) && code==122);
    RF_CHECK(vco_capacitor_code(2435000,2437000,128,4000,code) && code==129);
    RF_CHECK(vco_capacitor_code(2439000,2437000,128,4000,code) && code==127);
    RF_CHECK(vco_capacitor_code(2435001,2437000,128,4000,code) && code==128);
    RF_CHECK(vco_capacitor_code(2438999,2437000,128,4000,code) && code==128);
    RF_CHECK(vco_capacitor_code(2949000,2437000,128,4000,code) && code==0);
    RF_CHECK(vco_capacitor_code(1929000,2437000,128,4000,code) && code==255);
    code=42;
    RF_CHECK(!vco_capacitor_code(2953000,2437000,128,4000,code) && code==42);
    RF_CHECK(!vco_capacitor_code(1925000,2437000,128,4000,code) && code==42);
    RF_CHECK(!vco_capacitor_code(2437000,2437000,128,0,code) && code==42);
    RF_CHECK(!vco_capacitor_code(2437000,2437000,256,4000,code) && code==42);
    RF_CHECK(vco_capacitor_code(0xffffffff,0xffffffff,128,0xffffffff,code) && code==128);
    RF_CHECK(!vco_capacitor_code(0xffffffff,0,0,1,code));
    /* A different virtual oscillator can reuse the arithmetic without
     * inheriting either chip's calibration curve or register addresses. */
    RF_CHECK(vco_capacitor_code(1000,1020,10,10,code) && code==12);
    /* Reference power8 corresponds to -96dBm. Offset encoding and resolution
     * differ between chips, but their physical logarithmic ratio is shared. */
    RF_CHECK(logarithmic_power_code(8,8,640,4,1023)==640);
    RF_CHECK(logarithmic_power_code(80,8,640,4,1023)==680);
    RF_CHECK(logarithmic_power_code(0.8,8,640,4,1023)==600);
    RF_CHECK(logarithmic_power_code(16,8,640,4,1023)==652);
    RF_CHECK(logarithmic_power_code(8,8,3328,8,4095)==3328);
    RF_CHECK(logarithmic_power_code(0,8,640,4,1023)==0);
    RF_CHECK(logarithmic_power_code(8,0,640,4,1023)==0);
    RF_CHECK(logarithmic_power_code(1e100,8,640,4,1023)==1023);
    RF_CHECK(logarithmic_power_code(1e-100,8,640,4,1023)==0);
    RF_CHECK(vco_capacitor_code(2400000,2437000,2048,250,code,4095) && code==2196);
    RF_CHECK(vco_capacitor_code(2437500,2437000,2048,250,code,4095) && code==2046);
    RF_CHECK(!vco_capacitor_code(0,2437000,2048,250,code,4095));
#undef RF_CHECK
    return checks;
}
#ifdef ESP_RF_MATH_TEST_MAIN
int main(){int n=esp_rf_math_tests();if(n<0)return 1;printf("ESP RF math: %d checks passed\n",n);return 0;}
#endif
