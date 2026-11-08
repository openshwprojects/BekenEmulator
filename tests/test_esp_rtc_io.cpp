#include "esp_machine.h"
#include <stdio.h>

int esp_rtc_io_tests()
{
    int checks=0;
#define RTCIO_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP32 RTCIO line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspMachine m(esp::CHIP_ESP32);u32 value=0;bool input=false,output=false,enabled=false;
    static const u32 defaults[]={0x52000000,0x4a000000,0x52000000,0x4a000000,0x52000000,0x52000000,0x4a000000,0x42000000};
    for(unsigned i=0;i<8;++i) {
        const u32 address=0x3ff48494+i*4;
        RTCIO_CHECK(m.read(address,4,value,false) && value==defaults[i]);
        RTCIO_CHECK(m.write(address,4,value&~0x80000u)); /* real rtc_gpio_deinit RMW */
        RTCIO_CHECK(m.read(address,4,value,false) && value==defaults[i]);
    }
    RTCIO_CHECK(m.read(0x3ff4847c,4,value,false) && value==0);
    RTCIO_CHECK(m.read(0x3ff48480,4,value,false) && value==0);
    RTCIO_CHECK(m.read(0x3ff48484,4,value,false) && value==0x80000000u);
    RTCIO_CHECK(m.read(0x3ff48488,4,value,false) && value==0x80000000u);
    RTCIO_CHECK(m.read(0x3ff4848c,4,value,false) && value==0x84100010u);
    /* GPIO4's RTC pad pull controls its physical pin even in digital mode. */
    RTCIO_CHECK(m.write(0x3ff48494,4,0x4a000000));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input && !enabled);
    RTCIO_CHECK(m.write(0x3ff49048,4,0));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input);
    RTCIO_CHECK(m.write(0x3ff48494,4,0x52000000));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input);
    RTCIO_CHECK(m.set_gpio_input(4,1) && m.gpio_state(4,input,output,enabled) && input);
    RTCIO_CHECK(m.set_gpio_input(4,-1));
    /* RTC channel10 is GPIO4: data fields start at bit14, not at bit0. */
    RTCIO_CHECK(m.write(0x3ff48494,4,0x42082000));
    RTCIO_CHECK(m.write(0x3ff48404,4,1u<<24) && m.write(0x3ff48410,4,1u<<24));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input && output && enabled);
    RTCIO_CHECK(m.read(0x3ff48424,4,value,false) && (value&(1u<<24)));
    RTCIO_CHECK(m.write(0x3ff44098,4,4u<<7)); /* RTC-selected pin is disconnected from digital GPIO IRQ */
    RTCIO_CHECK(m.read(0x3ff44044,4,value,false) && !(value&16));
    RTCIO_CHECK(m.write(0x3ff44098,4,0));
    RTCIO_CHECK(m.write(0x3ff48408,4,1u<<24));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input && !output && enabled);
    RTCIO_CHECK(m.write(0x3ff48404,4,1u<<24) && m.write(0x3ff48450,4,4)); /* open drain */
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input && output && enabled);
    RTCIO_CHECK(m.write(0x3ff48494,4,0x4a082000));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input);
    RTCIO_CHECK(m.write(0x3ff48414,4,1u<<24));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !enabled && input);
    /* Returning to digital mode selects the independent digital OUT/ENABLE. */
    RTCIO_CHECK(m.write(0x3ff44004,4,0) && m.write(0x3ff44020,4,16));
    RTCIO_CHECK(m.write(0x3ff48494,4,0x4a000000));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input && !output && enabled);
    /* Input-only channels0..5 cannot become outputs through RTC aliases. */
    RTCIO_CHECK(m.write(0x3ff4840c,4,0xffffffff));
    RTCIO_CHECK(m.read(0x3ff4840c,4,value,false) && value==0xfff00000u);
    RTCIO_CHECK(!m.write(0x3ff48424,4,0)); /* input is read-only */
    RTCIO_CHECK(!m.write(0x3ff48494,4,0x00400000)); /* no synthetic touch conversion */
    RTCIO_CHECK(!m.write(0x3ff48450,4,0x80)); /* RTC interrupt routing not yet modeled */
    RTCIO_CHECK(!m.read(0x3ff48474,4,value,false)); /* reserved word */
    /* Digital-domain reset preserves RTCIO configuration. */
    RTCIO_CHECK(m.write(0x3ff48000,4,0x80000000u) && m.step()==XTENSA_OK);
    RTCIO_CHECK(m.read(0x3ff48494,4,value,false) && value==0x4a000000);
    /* A hold freezes the electrical pad while its configuration registers
     * remain writable. Releasing both hold sources applies current settings. */
    RTCIO_CHECK(m.write(0x3ff4840c,4,0));
    RTCIO_CHECK(m.write(0x3ff44004,4,16) && m.write(0x3ff44020,4,16));
    RTCIO_CHECK(m.write(0x3ff48494,4,0xca000000));
    RTCIO_CHECK(m.write(0x3ff44004,4,0) && m.write(0x3ff44020,4,0));
    RTCIO_CHECK(m.write(0x3ff48494,4,0xd2082000)); /* write RTC mux and pulldown while held */
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input && output && enabled);
    RTCIO_CHECK(m.read(0x3ff48494,4,value,false) && value==0xd2082000);
    RTCIO_CHECK(m.write(0x3ff480c8,4,1u<<8)); /* force map bit8 corresponds to channel10 */
    RTCIO_CHECK(m.write(0x3ff48494,4,0x52082000));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input && output && enabled);
    RTCIO_CHECK(m.set_gpio_input(4,0) && m.gpio_state(4,input,output,enabled) && !input && output);
    RTCIO_CHECK(m.set_gpio_input(4,-1));
    RTCIO_CHECK(m.write(0x3ff48000,4,0x80000000u) && m.step()==XTENSA_OK);
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && input && output && enabled); /* survives digital reset */
    RTCIO_CHECK(m.write(0x3ff480c8,4,0));
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input && !enabled); /* current RTC enable is off */
    /* Force hold alone also latches pull state, with separate channels. */
    RTCIO_CHECK(m.write(0x3ff48498,4,0x4a000000) && m.write(0x3ff480c8,4,1u<<9));
    RTCIO_CHECK(m.write(0x3ff48498,4,0x52000000));
    RTCIO_CHECK(m.gpio_state(0,input,output,enabled) && input && !enabled);
    RTCIO_CHECK(m.gpio_state(4,input,output,enabled) && !input);
    RTCIO_CHECK(m.write(0x3ff4848c,4,0x8c500010)); /* GPIO32 and33 pullups */
    RTCIO_CHECK(m.write(0x3ff480c8,4,(1u<<16)|(1u<<9))); /* XTAL pad force bits have a different order */
    RTCIO_CHECK(m.write(0x3ff4848c,4,0x84100010));
    RTCIO_CHECK(m.gpio_state(32,input,output,enabled) && input && !enabled);
    RTCIO_CHECK(m.gpio_state(33,input,output,enabled) && !input && !enabled);
    /* RTC watchdog action4 resets the RTC domain, including hold snapshots. */
    RTCIO_CHECK(m.write(0x3ff48090,4,150) && m.write(0x3ff4808c,4,0xc0000080));
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,1000);
    RTCIO_CHECK(m.step()==XTENSA_OK && m.read(0x3ff48044,4,value,false));
    RTCIO_CHECK(m.step()==XTENSA_OK && m.reset_count()==3);
    RTCIO_CHECK(m.read(0x3ff480c8,4,value,false) && value==0);
    RTCIO_CHECK(m.read(0x3ff48494,4,value,false) && value==0x52000000);
    RTCIO_CHECK(m.write(0x3ff48498,4,0) && m.gpio_state(0,input,output,enabled) && !input && !enabled);
#undef RTCIO_CHECK
    return checks;
}
#ifdef ESP_RTC_IO_TEST_MAIN
int main(){int n=esp_rtc_io_tests();if(n<0)return 1;printf("ESP32 RTCIO: %d checks passed\n",n);return 0;}
#endif
