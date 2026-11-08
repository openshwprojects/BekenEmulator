#include "esp32/esp32_rng.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if(!(x)){printf("[FAIL] ESP32 RNG line %d: %s\n",__LINE__,#x);return false;} } while(0)

bool bootloader_entropy_route()
{
    std::map<u32,u32> regs;u32 value;std::string error;
    CHECK(esp32_rng::read(regs,0x3ff4882c,value)==esp32_rng::OK && value==15);
    CHECK(esp32_rng::read(regs,0x3ff48800,value)==esp32_rng::OK && value==0x70902);
    CHECK(esp32_rng::read(regs,0x3ff4f0b0,value)==esp32_rng::OK && value==0x410186);
    CHECK(esp32_rng::write(regs,0x3ff4f008,0x30320,&error)==esp32_rng::ERROR);
    /* Official bootloader_random_esp32.c routes ADC2's internal reference
     * through I2S0 to the random generator, without a DMA or audio stream. */
    regs[0x3ff480a8]=0xa0000000;regs[0x3ff000c0]=0xf9c1e07f;
    CHECK(esp32_rng::write(regs,0x3ff4882c,31)==esp32_rng::OK);
    for(unsigned i=0;i<4;++i)CHECK(esp32_rng::write(regs,0x3ff6602c+4*i,0xadadadad)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff4880c,0x2c000a)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff48800,0x08070902)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff48890,0x10070902)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff66010,0x047f8264)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff66018,0x020aff08)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff4f0b0,0x410506)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff4f0a8,0x38)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff4f008,0x30320)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff4f008,value)==esp32_rng::OK && (value&32));
    CHECK(esp32_rng::write(regs,0x3ff4f008,0x30310,&error)==esp32_rng::ERROR); /* no fake audio TX */
    CHECK(esp32_rng::write(regs,0x3ff4882c,0x30f,&error)==esp32_rng::ERROR); /* ULP execution needs its CPU */
    CHECK(esp32_rng::write(regs,0x3ff4f0a8,0xfffffffe)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff4f0a8,value)==esp32_rng::OK && value==0xfe);
    esp32_rng::reset_i2s(regs);
    CHECK(esp32_rng::read(regs,0x3ff4f008,value)==esp32_rng::OK && value==0x30300);
    CHECK(esp32_rng::read(regs,0x3ff4f0a8,value)==esp32_rng::OK && value==0);
    CHECK(esp32_rng::read(regs,0x3ff488fc,value)==esp32_rng::UNHANDLED);
    return true;
}

bool deterministic_random_stream()
{
    std::map<u32,u32> first,second;u32 a,b,previous=0;
    for(unsigned i=0;i<32;++i) {
        CHECK(esp32_rng::read(first,0x60035144,a)==esp32_rng::OK);
        CHECK(esp32_rng::read(second,0x3ff75144,b)==esp32_rng::OK && a==b && a!=previous);
        previous=a;
    }
    first[0x3ff000cc]=0; /* gating the RNG clock freezes its state */
    CHECK(esp32_rng::read(first,0x60035144,a)==esp32_rng::OK && a==previous);
    CHECK(esp32_rng::read(first,0x60035144,b)==esp32_rng::OK && b==a);
    CHECK(esp32_rng::write(first,0x60035144,0)==esp32_rng::ERROR);
    CHECK(esp32_rng::read(first,0x60035140,a)==esp32_rng::UNHANDLED);
    return true;
}

bool independent_i2s_clocks()
{
    std::map<u32,u32> regs;u32 value;
    CHECK(esp32_rng::read(regs,0x3ff6d0ac,value)==esp32_rng::OK && value==4);
    CHECK(esp32_rng::write(regs,0x3ff6d0ac,0xffffffff)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff6d0ac,value)==esp32_rng::OK && value==0x3fffff);
    CHECK(esp32_rng::read(regs,0x3ff4f0ac,value)==esp32_rng::OK && value==4);
    CHECK(esp32_rng::write(regs,0x3ff4f0ac,0x100008)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff6d0ac,0x10000a)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff6d0b0,value)==esp32_rng::OK && value==0x410186);
    CHECK(esp32_rng::write(regs,0x3ff6d008,16)==esp32_rng::ERROR);
    regs[0x3ff000c0]=1u<<21;
    CHECK(esp32_rng::write(regs,0x3ff6d008,32)==esp32_rng::ERROR); /* no invented audio/DMA receiver */
    CHECK(esp32_rng::read(regs,0x3ff6d010,value)==esp32_rng::UNHANDLED);
    esp32_rng::reset_i2s(regs,1);
    CHECK(esp32_rng::read(regs,0x3ff6d0ac,value)==esp32_rng::OK && value==4);
    CHECK(esp32_rng::read(regs,0x3ff4f0ac,value)==esp32_rng::OK && value==0x100008);
    esp32_rng::reset_i2s(regs,0);
    CHECK(esp32_rng::read(regs,0x3ff4f0ac,value)==esp32_rng::OK && value==4);
    return true;
}

bool power_detector_sar_route()
{
    std::map<u32,u32> regs;u32 value=0;std::string error;
    CHECK(esp32_rng::read(regs,0x3ff48838,value)==esp32_rng::OK && value==0xffffffff);
    CHECK(esp32_rng::read(regs,0x3ff48894,value)==esp32_rng::OK && value==0);
    /* Espressif pwdet_sar2_init selects the internal RF detector instead
     * of pads/DMA. This is routing setup, not a software conversion. */
    CHECK(esp32_rng::write(regs,0x3ff4880c,0x2c000a)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff48890,0x08070902)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff66010,0x007f8240)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff48894,0x80000000)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff48838,0x3ffffffc)==esp32_rng::OK);
    CHECK(esp32_rng::write(regs,0x3ff4882c,0x8f)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff48894,value)==esp32_rng::OK && value==0x80000000);
    CHECK(esp32_rng::read(regs,0x3ff48838,value)==esp32_rng::OK && value==0x3ffffffc);
    CHECK(esp32_rng::write(regs,0x3ff48894,0x8001ffff)==esp32_rng::OK);
    CHECK(esp32_rng::read(regs,0x3ff48894,value)==esp32_rng::OK && value==0x80000000); /* DONE/data are read-only */
    CHECK(esp32_rng::write(regs,0x3ff48894,0x80020000)==esp32_rng::OK); /* start ignored unless forced */
    CHECK(esp32_rng::write(regs,0x3ff48894,0x80060000,&error)==esp32_rng::ERROR);
    CHECK(error.find("conversion")!=std::string::npos);
    CHECK(esp32_rng::read(regs,0x3ff48894,value)==esp32_rng::OK && value==0x80020000); /* rejected command is atomic */
    CHECK(esp32_rng::read(regs,0x3ff48834,value)==esp32_rng::UNHANDLED);
    return true;
}
}

int esp32_rng_tests()
{
    checks=0;if(!bootloader_entropy_route() || !deterministic_random_stream() || !independent_i2s_clocks() || !power_detector_sar_route())return -1;return checks;
}
#ifdef ESP32_RNG_TEST_MAIN
int main(){int n=esp32_rng_tests();printf("ESP32 RNG: %d checks\n",n);return n<0?1:0;}
#endif
