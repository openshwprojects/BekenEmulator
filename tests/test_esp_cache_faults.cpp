#include "esp_machine.h"
#include <stdio.h>

int esp_cache_fault_tests()
{
    int checks=0;
#define CACHE_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP32 cache faults line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspMachine m(esp::CHIP_ESP32);u32 value=0;
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && value==0);
    CACHE_CHECK(m.read(0x3ff00424,4,value,false) && value==0);
    CACHE_CHECK(!m.write(0x3ff003fc,4,0xffff)); /* status is read-only */
    CACHE_CHECK(m.write(0x40080000,4,0x0020f0)); /* NOP establishes both CPU matrix attachments */
    m.cpu().set_pc(0x40080000);
    CACHE_CHECK(m.write(0x3ff00030,4,1) && m.write(0x3ff0002c,4,0) && m.step()==XTENSA_OK);
    CACHE_CHECK(m.write(0x3ff00104+68*4,4,25));
    CACHE_CHECK(m.write(0x3ff00218+68*4,4,24));
    CACHE_CHECK(m.write(0x3ff00040,4,0));
    CACHE_CHECK(!m.read(0x3f400000,4,value,false));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x7e00)==0x4000);
    CACHE_CHECK(m.read(0x3ff00424,4,value,false) && value==0); /* per-core status */
    CACHE_CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&(1u<<25))); /* enable mask initially off */
    CACHE_CHECK(m.write(0x3ff005a0,4,1u<<14));
    CACHE_CHECK((m.cpu().special(XT_SR_INTERRUPT)&(1u<<25)) && (m.cpu(1).special(XT_SR_INTERRUPT)&(1u<<24)));
    CACHE_CHECK(m.read(0x3ff000f4,4,value,false) && (value&16)); /* matrix source68 */
    CACHE_CHECK(!m.read(0x400d0120,3,value,true));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x7e00)==0x6000);
    CACHE_CHECK(!m.read(0x40400000,4,value,false) && !m.read(0x40800000,4,value,false));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x7e00)==0x7800);
    CACHE_CHECK(m.write(0x3ff005a0,4,1)); /* APP DROM enable cannot select PRO's fault */
    CACHE_CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&(1u<<25)) && !(m.cpu(1).special(XT_SR_INTERRUPT)&(1u<<24)));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x7e00)==0x7800); /* masking does not erase cause */
    CACHE_CHECK(m.write(0x3ff00040,4,8));
    CACHE_CHECK(m.write(0x3ff10000,4,0x123));
    CACHE_CHECK(!m.read(0x3f400000,4,value,false));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x1ff)==0x123); /* last MMU lookup, including invalid bit */
    CACHE_CHECK(m.set_psram_size(0x200000));
    CACHE_CHECK(m.write(0x3ff00040,4,0));
    CACHE_CHECK(!m.write(0x3f800000,4,0x12345678));
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && (value&0x7e00)==0x7c00);
    CACHE_CHECK(m.write(0x3ff005a0,4,1u<<18));
    CACHE_CHECK(m.cpu().special(XT_SR_INTERRUPT)&(1u<<25));
    CACHE_CHECK(m.write(0x3ff48000,4,0x80000000u) && m.step()==XTENSA_OK); /* digital reset clears cache diagnostics */
    CACHE_CHECK(m.read(0x3ff003fc,4,value,false) && value==0 && m.read(0x3ff00424,4,value,false) && value==0);
    CACHE_CHECK(m.read(0x3ff005a0,4,value,false) && value==0);
#undef CACHE_CHECK
    return checks;
}
