#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("[FAIL] ESP32 MMIO line %d: %s\n", __LINE__, #x); return false; } } while (0)

bool cache_controls()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff00040, 4, v, false) && (v & 8));
    CHECK(m.write(0x3ff00040, 4, 0x18)); /* enable + request invalidate */
    CHECK(m.read(0x3ff00040, 4, v, false) && (v & 0x38) == 0x38);
    CHECK(m.write(0x3ff00040, 4, 8));
    CHECK(m.read(0x3ff00040, 4, v, false) && (v & 0x38) == 8);
    CHECK(m.write(0x3ff00040, 4, 0xffffffff));
    CHECK(m.read(0x3ff00040, 4, v, false) && (v & 0xe0000000) == 0);
    CHECK(!m.read(0x3ff00ffc, 4, v, false));
    CHECK(!m.write(0x3ff00ffc, 4, 1));
    CHECK(m.read(0x3ff430f8,4,v,false) && v == 0);
    CHECK(!m.write(0x3ff430f8,4,1)); /* read-only SPI0 state machine */
    CHECK(m.write(0x3ff430fc,4,0xffffffff));
    CHECK(m.read(0x3ff430fc,4,v,false) && v == 3);
    return true;
}

bool clock_configuration()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff66000,4,v,false) && v==0x2000);
    CHECK(m.write(0x3ff66000,4,0xffffffff));
    CHECK(m.read(0x3ff66000,4,v,false) && v==0x3fff);
    CHECK(m.read(0x3ff66004,4,v,false) && v==39);
    CHECK(m.read(0x3ff66008,4,v,false) && v==79);
    CHECK(m.read(0x3ff6600c,4,v,false) && v==11);
    CHECK(m.write(0x3ff66004,4,0xffffffff));
    CHECK(m.read(0x3ff66004,4,v,false) && v==255);
    CHECK(m.read(0x3ff6607c,4,v,false) && v==0x96042000);
    CHECK(m.write(0x3ff6607c,4,0));
    CHECK(m.read(0x3ff6607c,4,v,false) && v==0x80000000); /* chip ECO bit is fixed */
    CHECK(!m.read(0x3ff660e0,4,v,false));
    CHECK(m.read(0x3ff48000,4,v,false) && v==0x1c492000);
    CHECK(m.write(0x3ff48000,4,0x1c492c00)); /* PLL and analog power controls */
    CHECK(m.read(0x3ff48000,4,v,false) && v==0x1c492c00);
    CHECK(m.read(0x3ff4807c,4,v,false) && v==0xa9002400);
    CHECK(m.write(0x3ff4807c,4,0xffffffff));
    CHECK(m.read(0x3ff4807c,4,v,false) && v==0xffffff80);
    CHECK(m.read(0x3ff000cc,4,v,false) && v==0xfffce030);
    CHECK(m.write(0x3ff000cc,4,0x8000)); /* bootloader enables RNG clock */
    CHECK(m.read(0x3ff000cc,4,v,false) && v==0x8000);
    CHECK(m.read(0x3ff000d0,4,v,false) && v==0); /* modem/SDIO/EMAC reset controls */
    CHECK(m.write(0x3ff000d0,4,0xffffffff));
    CHECK(m.read(0x3ff000d0,4,v,false) && v==0x7ff); /* eleven documented reset lines */
    CHECK(m.write(0x3ff000d0,4,v&~0x24u)); /* SDK periph_module_enable releases reset */
    CHECK(m.read(0x3ff000d0,4,v,false) && v==0x7db);
    CHECK(m.read(0x3ff000cc,4,v,false) && v==0x8000); /* independent clock register */
    CHECK(m.read(0x3ff480d4,4,v,false) && v==0x13ff0000); /* brownout default threshold/wait */
    CHECK(m.write(0x3ff480d4,4,0x40028000)); /* real SDK setup: detect on, RF powerdown enabled */
    CHECK(m.read(0x3ff480d4,4,v,false) && v==0x40028000);
    CHECK(m.write(0x3ff480d4,4,0xffffffff));
    CHECK(m.read(0x3ff480d4,4,v,false) && v==0x7fffc000); /* DET is read-only; supply is healthy */
    CHECK(m.read(0x3ff48040,4,v,false) && !(v&128) && m.reset_count()==0);
    CHECK(m.read(0x3ff480c8,4,v,false) && v==0); /* RTC pads boot without forced hold */
    CHECK(m.write(0x3ff480c8,4,v&0xfffc0000u)); /* rtc_gpio_force_hold_dis_all */
    CHECK(m.read(0x3ff480c8,4,v,false) && v==0);
    CHECK(m.read(0x3ff005a0,4,v,false) && v==0); /* cache access interrupt enables */
    CHECK(m.write(0x3ff005a0,4,0xfc000)); /* PRO startup enables its six cache access reasons */
    CHECK(m.read(0x3ff005a0,4,v,false) && v==0xfc000);
    CHECK(m.write(0x3ff005a0,4,v|0x3f)); /* APP startup must preserve PRO enables */
    CHECK(m.read(0x3ff005a0,4,v,false) && v==0xfc03f);
    CHECK(m.write(0x3ff005a0,4,0xffffffff));
    CHECK(m.read(0x3ff005a0,4,v,false) && v==0x0fffffff);
    CHECK(m.write(0x3ff480c8,4,1)); /* separate RTCIO tests verify the physical pad latch */
    CHECK(m.read(0x3ff480c8,4,v,false) && v==1);
    CHECK(m.read(0x3ff480a8,4,v,false) && v==0);
    CHECK(m.write(0x3ff480a8,4,0xffffffff));
    CHECK(m.read(0x3ff480a8,4,v,false) && v==0xe0000000);
    CHECK(m.write(0x3ff4f0ac,4,0x100008) && m.write(0x3ff6d0ac,4,0x10000a));
    CHECK(m.write(0x3ff000c4,4,1u<<21));
    CHECK(m.read(0x3ff6d0ac,4,v,false) && v==4);
    CHECK(m.read(0x3ff4f0ac,4,v,false) && v==0x100008);
    CHECK(m.write(0x3ff000c4,4,1u<<4));
    CHECK(m.read(0x3ff4f0ac,4,v,false) && v==4);
    return true;
}

bool analog_pll_bus()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff000bc,4,v,false) && v==0);
    CHECK(m.write(0x3ff000bc,4,0xffffffff));
    CHECK(m.read(0x3ff000bc,4,v,false) && v==0xffff);
    CHECK(m.read(0x6000e044,4,v,false) && (v&0x20000)==0); /* boot ROM enabled BBPLL */
    CHECK(m.write(0x6000e044,4,0));
    CHECK(m.write(0x6000e010,4,0x015a0366)); /* BBPLL register3 = 0x5a */
    CHECK(m.read(0x6000e010,4,v,false) && !(v&0x02000000));
    CHECK(m.write(0x6000e010,4,0x00000366));
    CHECK(m.read(0x6000e010,4,v,false) && ((v>>16)&255)==0x5a);
    CHECK(m.write(0x6000e010,4,0x00000466));
    CHECK(m.read(0x6000e010,4,v,false) && ((v>>16)&255)==0);
    CHECK(!m.write(0x6000e010,4,0x010000ff)); /* unknown analog block */
    CHECK(!m.read(0x6000e024,4,v,false)); /* reserved host is not zero memory */
    return true;
}

bool rtc_startup_power_controls()
{
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.read(0x3ff48030,4,value,false) && value==0x00800000);
    CHECK(m.write(0x3ff48030,4,value&0x23ffffff)); /* rtc_init clears unused analog power-up overrides */
    CHECK(m.read(0x3ff48030,4,value,false) && value==0x00800000);
    CHECK(m.write(0x3ff48030,4,0xddffffff));
    CHECK(m.read(0x3ff48030,4,value,false) && value==0xdd800000);
    CHECK(!m.write(0x3ff48030,4,0x02000000)); /* sleep PLL calibration requires its own state machine */
    CHECK(m.read(0x3ff4801c,4,value,false) && value==0x28140403);
    CHECK(m.write(0x3ff4801c,4,0x14050503)); /* PLL/XTAL/RC wait count20, preserve stall settings */
    CHECK(m.read(0x3ff4801c,4,value,false) && value==0x14050503);
    CHECK(m.read(0x3ff48078,4,value,false) && value==0);
    CHECK(m.write(0x3ff48078,4,0x63000000)); /* bias heartbeat and attenuation configuration */
    CHECK(m.read(0x3ff48078,4,value,false) && value==0x63000000);
    CHECK(m.write(0x3ff48078,4,0x63ffffff));
    CHECK(m.read(0x3ff48078,4,value,false) && value==0x63000000);
    CHECK(m.read(0x3ff00094,4,value,false) && value==3);
    CHECK(m.write(0x3ff00094,4,0xffffff00));
    CHECK(m.read(0x3ff00094,4,value,false) && value==0);
    CHECK(m.read(0x3ff000a0,4,value,false) && value==0xffffffff);
    CHECK(m.write(0x3ff000a0,4,0));
    CHECK(m.read(0x3ff000a0,4,value,false) && value==0);
    CHECK(m.read(0x3ff000a4,4,value,false) && value==1);
    CHECK(m.write(0x3ff000a4,4,0xfffffffe));
    CHECK(m.read(0x3ff000a4,4,value,false) && value==0);
    CHECK(m.read(0x3ff000ac,4,value,false) && value==0x101);
    CHECK(m.write(0x3ff000ac,4,0xfffffcfc));
    CHECK(m.read(0x3ff000ac,4,value,false) && value==0);
    CHECK(!m.write(0x3ff000ac,4,2)); /* power-down is not silently reduced to config storage */
    CHECK(m.read(0x3ff48080,4,value,false) && value==0x00012925);
    CHECK(m.read(0x3ff48084,4,value,false) && value==0x00155550);
    CHECK(m.read(0x3ff48088,4,value,false) && value==0xaaaa5000);
    /* Actual rtc_init sequence hands power/isolation back to the awake
     * state machine. Removing a forced-on override does not power RAM off. */
    CHECK(m.write(0x3ffb0000,4,0x12345678));
    CHECK(m.write(0x3ff48084,4,0x00155550&0xffffffef&0xffefffff&0xfffbffff&0xfffeaabf));
    CHECK(m.read(0x3ff48084,4,value,false) && value==0);
    CHECK(m.write(0x3ff48080,4,0x00012925&0xfffedfff&0xfff7ffff&0xfffffffa&0xffffffdf));
    CHECK(m.read(0x3ff48080,4,value,false) && value==0x900);
    CHECK(m.write(0x3ff48088,4,0xaaaa5000&0x7fffffff&0xdfffffff&0xf555ffff&0xffffbfff&0xffffefff));
    CHECK(m.read(0x3ff48088,4,value,false) && value==0);
    CHECK(m.read(0x3ffb0000,4,value,false) && value==0x12345678);
    CHECK(!m.write(0x3ff48080,4,1u<<18));
    CHECK(!m.write(0x3ff48084,4,1u<<19));
    CHECK(!m.write(0x3ff48088,4,1u<<30));
    return true;
}

bool sha256_accelerator()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    /* NIST SHA256.pdf two-block example, including the intermediate state:
     * csrc.nist.gov/CSRC/media/Projects/Cryptographic-Standards-and-Guidelines/documents/examples/SHA256.pdf */
    Bytes block("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(block.size()==56);
    block += (char)0x80; block.resize(64,0);
    static const u32 intermediate[]={0x85e655d6,0x417a1795,0x3363376a,0x624cde5c,0x76e09589,0xcac5f811,0xcc4b32c1,0xf20e533a};
    static const u32 digest[]={0x248d6a61,0xd20638b8,0xe5c02693,0x0c3e6039,0xa33ce459,0x64ff2167,0xf6ecedd4,0x19db06c1};
    CHECK(!m.write(0x3ff03094,4,1)); /* CONTINUE needs a running context */
    CHECK(m.write(0x3ff0001c,4,2)); /* enable SHA clock */
    for(unsigned i=0;i<16;++i) CHECK(m.write(0x3ff03000+i*4,4,rd32be(block.data()+i*4)));
    CHECK(m.write(0x3ff03090,4,1));
    CHECK(m.read(0x3ff0309c,4,v,false) && v==0);
    CHECK(m.write(0x3ff03098,4,1));
    for(unsigned i=0;i<8;++i) CHECK(m.read(0x3ff03000+i*4,4,v,false) && v==intermediate[i]);
    for(unsigned i=0;i<16;++i) CHECK(m.write(0x3ff03000+i*4,4,i==15?448:0));
    CHECK(m.write(0x3ff03094,4,1));
    CHECK(m.write(0x3ff03098,4,1));
    for(unsigned i=0;i<8;++i) CHECK(m.read(0x3ff03000+i*4,4,v,false) && v==digest[i]);
    CHECK(!m.write(0x3ff0309c,4,1)); /* busy is read-only */
    CHECK(!m.write(0x3ff030a0,4,1)); /* unsupported algorithms do not fake a digest */
    CHECK(m.write(0x3ff00020,4,2)); /* reset SHA context */
    CHECK(m.write(0x3ff00020,4,0));
    CHECK(!m.write(0x3ff03094,4,1));
    return true;
}

bool rtc_time_and_storage()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff48074, 4, v, false) && v == 0x02a00000);
    CHECK(m.write(0x3ff48074, 4, 0xffffffff));
    CHECK(m.read(0x3ff48074, 4, v, false) && v == 0xfee00000);
    CHECK(m.read(0x3ff48034, 4, v, false) && (v & 0xfff) == 0x41);
    CHECK(m.write(0x3ff480b0, 4, 0x12345678));
    CHECK(m.read(0x3ff480b0, 4, v, false) && v == 0x12345678);
    CHECK(m.read(0x3ff480c, 4, v, false) == false); /* unrelated address */
    CHECK(m.write(0x3ff4800c, 4, 0x80000000));
    CHECK(m.read(0x3ff4800c, 4, v, false) && v == 0x40000000);
    CHECK(m.read(0x3ff48040, 4, v, false) && (v & 16));
    CHECK(m.write(0x3ff4803c, 4, 16));
    CHECK(m.read(0x3ff48044, 4, v, false) && v == 16);
    CHECK(m.write(0x3ff48048, 4, 16));
    CHECK(m.read(0x3ff48040, 4, v, false) && v == 0);
    u32 before;
    CHECK(m.read(0x3ff48010, 4, before, false));
    m.cpu().set_pc(0x40008534); m.cpu().set_reg(0, 0x40080000); m.cpu().set_reg(2, 1000);
    CHECK(m.step() == XTENSA_OK);
    CHECK(m.read(0x3ff48010, 4, v, false) && v == before); /* latch stable */
    CHECK(m.write(0x3ff4800c, 4, 0x80000000));
    CHECK(m.read(0x3ff48010, 4, v, false) && v - before == 150); /* 150 kHz RC */
    CHECK(!m.write(0x3ff48010, 4, 0)); /* counter is read-only */
    return true;
}

bool rtc_calibration()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff5f068,4,v,false) && v==0x13000);
    CHECK(m.write(0x3ff5f068,4,0x80000000u | (150u<<16)));
    CHECK(m.read(0x3ff5f068,4,v,false) && !(v&0x8000));
    m.cpu().set_pc(0x40008534); m.cpu().set_reg(0,0x40080000); m.cpu().set_reg(2,500);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff5f068,4,v,false) && !(v&0x8000));
    m.cpu().set_pc(0x40008534); m.cpu().set_reg(2,500);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff5f068,4,v,false) && (v&0x8000));
    CHECK(m.read(0x3ff5f06c,4,v,false) && (v>>7)==40000); /* 150 slow cycles /150kHz *40MHz */
    CHECK(!m.write(0x3ff5f06c,4,0));
    CHECK(m.write(0x3ff60068,4,0));
    CHECK(m.write(0x3ff60068,4,0x80002000u | (1u<<16)));
    m.cpu().set_pc(0x40008534); m.cpu().set_reg(2,32);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff60068,4,v,false) && (v&0x8000));
    CHECK(m.read(0x3ff6006c,4,v,false) && (v>>7)==1280); /* 8MHz/256 */
    CHECK(m.read(0x3ff5f06c,4,v,false) && (v>>7)==40000); /* independent groups */
    EspMachine other(esp::CHIP_ESP32);
    CHECK(other.set_xtal_frequency(26));
    CHECK(other.read(0x3ff480b0,4,v,false) && v==0x001a001a);
    CHECK(other.write(0x3ff5f068,4,0x80000000u | (150u<<16)));
    other.cpu().set_pc(0x40008534);other.cpu().set_reg(0,0x40080000);other.cpu().set_reg(2,1000);
    CHECK(other.step()==XTENSA_OK);
    CHECK(other.read(0x3ff5f06c,4,v,false) && (v>>7)==26000);
    return true;
}

bool rtc_watchdog()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.write(0x3ff480a4,4,0)); /* lock */
    CHECK(m.write(0x3ff48090,4,150));
    CHECK(m.read(0x3ff48090,4,v,false) && v==128000);
    CHECK(m.write(0x3ff480a4,4,0x50d83aa1));
    CHECK(m.write(0x3ff48090,4,150));
    CHECK(m.write(0x3ff4808c,4,0x90020080)); /* stage0 interrupt, 150 slow ticks */
    CHECK(m.write(0x3ff4803c,4,8));
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40080000);m.cpu().set_reg(2,500);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff48044,4,v,false) && v==0);
    CHECK(m.write(0x3ff480a0,4,0x80000000)); /* feed restarts stage0 */
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,500);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff48044,4,v,false) && v==0);
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,500);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff48044,4,v,false) && v==8);
    CHECK(m.write(0x3ff48048,4,8));
    CHECK(m.read(0x3ff48044,4,v,false) && v==0);
    CHECK(m.write(0x3ff4808c,4,0xb0000080)); /* stage0 system reset */
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,1000);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff48044,4,v,false)); /* expiry queues a reset without interrupting this bus access */
    CHECK(m.reset_count()==0);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.reset_count()==1 && m.cpu().pc()==0x40000400);
    CHECK(m.read(0x3ff48034,4,v,false) && (v&0xfff)==(9u|(9u<<6)));
    return true;
}

bool timer_group_watchdog()
{
    for(unsigned group=0;group<2;++group) {
        EspMachine m(esp::CHIP_ESP32); u32 v;
        const u32 base=group?0x3ff60000:0x3ff5f000;
        CHECK(m.read(base+0x48,4,v,false) && v==0x4c000);
        CHECK(m.read(base+0x4c,4,v,false) && v==0x10000);
        CHECK(m.write(base+0x64,4,0));
        CHECK(m.write(base+0x50,4,1000));
        CHECK(m.read(base+0x50,4,v,false) && v==26000000);
        CHECK(m.write(base+0x64,4,0x50d83aa1));
        CHECK(m.write(base+0x4c,4,0));
        CHECK(!m.write(base+0x48,4,0x80000000)); /* prescaler zero is undocumented */
        CHECK(m.write(base+0x4c,4,80u<<16)); /* one watchdog tick per microsecond */
        CHECK(m.write(base+0x50,4,1000));
        CHECK(m.write(base+0x48,4,0xa024c000)); /* stage0 level interrupt */
        CHECK(m.write(base+0x98,4,4)); /* WDT shares the controller's interrupt bank */
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40080000);m.cpu().set_reg(2,500);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.write(base+0x60,4,0)); /* any feed write restarts stage0 */
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,500);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(base+0x48,4,v,false));
        CHECK(m.read(base+0xa0,4,v,false) && v==0);
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,500);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(base+0x48,4,v,false));
        CHECK(m.read(base+0xa0,4,v,false) && v==4);
        CHECK(m.write(base+0xa4,4,4));
        CHECK(m.read(base+0x48,4,v,false));
        CHECK(m.read(base+0xa0,4,v,false) && v==0); /* acknowledgement stays cleared */
        CHECK(m.write(base+0x48,4,0xe004c000)); /* stage0 system reset */
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,1000);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(base+0x48,4,v,false));
        CHECK(m.reset_count()==0 && m.step()==XTENSA_OK);
        CHECK(m.reset_count()==1 && m.cpu().pc()==0x40000400);
        CHECK(m.read(0x3ff48034,4,v,false) && (v&0xfff)==((7u+group)|((7u+group)<<6)));
    }
    return true;
}

bool watchdog_idle_deadline()
{
    EspMachine m(esp::CHIP_ESP32);
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=esp::CHIP_ESP32;image.entry_point=0x40080000;
    esp::Segment segment;segment.address=image.entry_point;segment.data=Bytes("\x00\x70\x00",3);
    image.segments.push_back(segment);layout.images.push_back(image);
    CHECK(m.load(Bytes(0x100000,'\xff'),layout,false));
    CHECK(m.write(0x3ff5f04c,4,80u<<16));
    CHECK(m.write(0x3ff5f050,4,10));
    CHECK(m.write(0x3ff5f048,4,0xe004c000));
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.elapsed_us()==10 && m.instructions()==1);
    CHECK(m.reset_count()==1 && m.cpu().pc()==0x40000400);
    return true;
}

bool software_reset_requests()
{
    for(unsigned mode=0;mode<4;++mode) {
        EspMachine m(esp::CHIP_ESP32);u32 value;
        const unsigned targets=mode==0?1:mode==1?2:3;
        const u32 request=mode==3?0x80000000u:((targets&1)<<5)|((targets&2)<<3);
        m.cpu().set_pc(0x40081234);m.cpu(1).set_pc(0x40085678);
        CHECK(m.write(0x3ff48054,4,0x12345678));
        CHECK(m.write(0x3ff000bc,4,0x5678));
        CHECK(m.write(0x3ff48000,4,0x1c492000|request));
        CHECK(m.reset_count()==0 && m.cpu().pc()==0x40081234 && m.cpu(1).pc()==0x40085678);
        CHECK(m.read(0x3ff48000,4,value,false) && !(value&0x80000030u));
        CHECK(m.step()==XTENSA_OK && m.reset_count()==1);
        CHECK(m.cpu().pc()==((targets&1)?0x40000400u:0x40081234u));
        CHECK(m.cpu(1).pc()==((targets&2)?0x40000400u:0x40085678u));
        const unsigned reason=mode==3?3:12;
        CHECK(m.read(0x3ff48034,4,value,false) && (value&0xfff)==(((targets&1)?reason:1)|(((targets&2)?reason:1)<<6)));
        CHECK(m.read(0x3ff48054,4,value,false) && value==0x12345678); /* RTC retained on digital/CPU reset */
        CHECK(m.read(0x3ff000bc,4,value,false) && value==(mode==3?0:0x5678));
    }
    return true;
}

bool watchdog_reset_domains()
{
    for(unsigned targets=0;targets<4;++targets) {
        EspMachine m(esp::CHIP_ESP32);u32 value;
        CHECK(m.write(0x3ff48090,4,150));
        CHECK(m.write(0x3ff4808c,4,0xa0000080u|((targets&1)<<9)|((targets&2)<<7)));
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,1000);
        m.cpu(1).set_pc(0x40085678);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(0x3ff48044,4,value,false) && m.reset_count()==0);
        if(!targets) {
            m.cpu().set_pc(0x40008534);m.cpu().set_reg(2,0);
            CHECK(m.step()==XTENSA_OK && m.reset_count()==0); /* both CPU reset enables are clear */
        } else {
            CHECK(m.step()==XTENSA_OK && m.reset_count()==1);
            CHECK(m.cpu().pc()==((targets&1)?0x40000400u:0x40081234u));
            CHECK(m.cpu(1).pc()==((targets&2)?0x40000400u:0x40085678u));
            CHECK(m.read(0x3ff48034,4,value,false) && (value&0xfff)==(((targets&1)?13u:1u)|(((targets&2)?13u:1u)<<6)));
        }
        CHECK(m.read(0x3ff4808c,4,value,false) && (value&0x80000000u)); /* CPU reset retains watchdog */
        CHECK(m.read(0x3ff48044,4,value,false)); /* next stage not immediately requeued */
        CHECK(!m.write(0x3ff4808c,4,0xd0000080)); /* reserved action does not alias RTC reset */
    }
    for(unsigned group=0;group<2;++group) {
        EspMachine m(esp::CHIP_ESP32);u32 value;const u32 base=group?0x3ff60000:0x3ff5f000;
        CHECK(m.write(base+0x4c,4,80u<<16) && m.write(base+0x50,4,1000));
        CHECK(m.write(base+0x48,4,0xc004c000));
        m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,1000);
        m.cpu(1).set_pc(0x40085678);
        CHECK(m.step()==XTENSA_OK && m.read(base+0x48,4,value,false));
        CHECK(m.step()==XTENSA_OK && m.reset_count()==1);
        CHECK(m.cpu(group).pc()==0x40000400);
        CHECK(m.cpu(1-group).pc()==(group?0x40081234u:0x40085678u));
        CHECK(m.read(0x3ff48034,4,value,false) && ((value>>(group*6))&63)==11);
        CHECK(m.read(base+0x48,4,value,false) && (value&0x80000000u));
    }
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.write(0x3ff48054,4,0x12345678));
    CHECK(m.write(0x3ff48090,4,150) && m.write(0x3ff4808c,4,0xc0000080));
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,1000);
    CHECK(m.step()==XTENSA_OK && m.read(0x3ff48044,4,value,false));
    CHECK(m.step()==XTENSA_OK && m.reset_count()==1);
    CHECK(m.read(0x3ff48034,4,value,false) && (value&0xfff)==(16u|(16u<<6)));
    CHECK(m.read(0x3ff48054,4,value,false) && value==0); /* RTC-domain reset clears scratch words */
    CHECK(m.elapsed_us()>=1000 && m.write(0x3ff4800c,4,0x80000000));
    CHECK(m.read(0x3ff48010,4,value,false) && value==0); /* RTC counter restarted; machine clock did not */
    return true;
}

bool efuse_read_controller()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    CHECK(m.read(0x3ff5a010, 4, v, false) && v == 0); /* no flash-pad override */
    CHECK(m.read(0x3ff5a00c, 4, v, false) && (v & 0x8000)); /* revision 3 */
    CHECK(!m.write(0x3ff5a010, 4, 1)); /* OTP read window is read-only */
    CHECK(m.write(0x3ff5a110, 4, 1));
    CHECK(m.write(0x3ff5a104, 4, 1));
    CHECK(m.read(0x3ff5a104, 4, v, false) && v == 1); /* ROM requires busy first */
    m.cpu().set_pc(0x40008534); m.cpu().set_reg(0, 0x40080000); m.cpu().set_reg(2, 4);
    CHECK(m.step() == XTENSA_OK);
    CHECK(m.read(0x3ff5a104, 4, v, false) && v == 0);
    CHECK(m.read(0x3ff5a108, 4, v, false) && v == 1);
    CHECK(m.read(0x3ff5a10c, 4, v, false) && v == 1);
    CHECK(m.write(0x3ff5a114, 4, 1));
    CHECK(m.read(0x3ff5a10c, 4, v, false) && v == 0);
    CHECK(!m.write(0x3ff5a104, 4, 2)); /* programming not silently emulated */
    CHECK(!m.read(0x3ff5a180, 4, v, false)); /* reserved registers are not RAM */
    return true;
}

bool flash_mmu()
{
    EspMachine m(esp::CHIP_ESP32); u32 v;
    esp::FlashImage layout; layout.chip = esp::CHIP_ESP32; layout.app_index = layout.boot_index = 0;
    esp::Image im; im.chip = esp::CHIP_ESP32; im.entry_point = 0x40080000;
    esp::Segment code; code.address = im.entry_point; code.data = Bytes("\xf0\x20\x00",3);
    im.segments.push_back(code); layout.images.push_back(im);
    Bytes flash(0x30000,'\xff'); flash.replace(0x10000,4,"page"); flash.replace(0x20000,4,"next");
    CHECK(m.load(flash,layout,true));
    CHECK(m.read(0x3ff10000,4,v,false) && v == 0x100);
    CHECK(m.write(0x3ff10000,4,1));
    CHECK(m.read(0x3f400000,4,v,false) && v == 0x65676170);
    CHECK(!m.read(0x3f400000,3,v,true));
    CHECK(m.write(0x3ff10000,4,2));
    CHECK(m.read(0x3f400000,4,v,false) && v == 0x7478656e); /* no stale cache */
    CHECK(m.write(0x3ff10000 + 77*4,4,1)); /* IRAM0 page for 0x400d0000 */
    CHECK(m.read(0x400d0000,3,v,true) && v == 0x676170);
    CHECK(m.write(0x3ff00040,4,0));
    CHECK(!m.read(0x400d0000,3,v,true));
    CHECK(m.write(0x3ff00040,4,8));
    CHECK(m.write(0x3ff10000,4,0x100));
    CHECK(!m.read(0x3f400000,4,v,false));
    CHECK(m.write(0x3ff12000,4,1));
    CHECK(m.read(0x3ff10000,4,v,false) && v == 0x100); /* APP table independent */
    CHECK(m.write(0x3ff10000,4,8));
    CHECK(!m.read(0x3f400000,4,v,false)); /* physical bounds enforced */
    return true;
}

bool per_core_flash_mmu()
{
    EspMachine m(esp::CHIP_ESP32);u32 value;
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=esp::CHIP_ESP32;image.entry_point=0x40080000;
    esp::Segment segment;segment.address=image.entry_point;
    segment.data=Bytes("\xf0\x20\x00\xf0\x20\x00\xf0\x20\x00",9);
    segment.data.resize(0x100,0);segment.data+=Bytes("\x38\x02\xf0\x20\x00",5); /* APP: L32I.N a3,a2,0 */
    image.segments.push_back(segment);layout.images.push_back(image);
    Bytes flash(0x30000,'\xff');flash.replace(0x10000,4,"PRO0");flash.replace(0x20000,4,"APP1");
    CHECK(m.load(flash,layout,false));
    CHECK(m.write(0x3ff10000,4,1) && m.write(0x3ff12000,4,2));
    CHECK(m.read(0x3f400000,4,value,false) && value==0x304f5250);
    CHECK(m.write(0x3ff00058,4,8));
    CHECK(m.write(0x3ff00038,4,0xc0080100));
    CHECK(m.write(0x3ff00030,4,1) && m.write(0x3ff0002c,4,0));
    m.cpu(1).set_reg(2,0x3f400000);
    CHECK(m.step()==XTENSA_OK && m.step()==XTENSA_OK);
    CHECK(m.step()==XTENSA_OK && m.step()==XTENSA_OK);
    CHECK(m.cpu(1).reg(3)==0x31505041);
    CHECK(m.write(0x3ff00040,4,0)); /* PRO cache does not gate APP reads */
    CHECK(m.read(0x3f400000,4,value,false) && value==0x31505041);
    CHECK(m.write(0x3ff00058,4,0));
    CHECK(!m.read(0x3f400000,4,value,false));
    return true;
}

bool psram_cache_mapping()
{
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.set_psram_size(0x200000));
    CHECK(!m.read(0x3f800000,4,value,false)); /* masked and device still in SPI mode */
    CHECK(m.write(0x3ff42034,4,5));
    CHECK(m.write(0x3ff4201c,4,0x80000040));
    CHECK(m.write(0x3ff42024,4,0x70000035));
    CHECK(m.write(0x3ff42000,4,1u<<18)); /* enter QPI on the actual SPI device */
    CHECK(m.write(0x3ff00044,4,0)); /* 32 KiB pages, SRAM address window enabled */
    CHECK(!m.read(0x3f800000,4,value,false)); /* MMU entries still invalid */
    CHECK(m.write(0x3ff11200,4,0));
    CHECK(!m.read(0x3f800000,4,value,false)); /* SPI0 CS1 and cache command must be configured */
    CHECK(m.write(0x3ff43034,4,4));
    CHECK(m.write(0x3ff43054,4,0x15c04834));
    CHECK(!m.read(0x3f800000,4,value,false)); /* invalid default read opcode */
    CHECK(m.write(0x3ff4305c,4,0x700000eb));
    CHECK(m.write(0x3ff43060,4,0x70000038));
    CHECK(m.read(0x3f800000,4,value,false) && value==0);
    CHECK(m.write(0x3f800000,4,0x12345678));
    CHECK(m.read(0x3f800000,4,value,false) && value==0x12345678);
    CHECK(m.write(0x3ff43060,4,0x700000ff));
    CHECK(!m.write(0x3f800000,4,0)); /* bad write opcode cannot modify backing RAM */
    CHECK(m.read(0x3f800000,4,value,false) && value==0x12345678);
    CHECK(m.write(0x3ff43060,4,0xf0003800)); /* IDF DCLK guarded QPI commands */
    CHECK(m.write(0x3ff4305c,4,0xf000eb00));
    CHECK(m.write(0x3f800004,4,0x44332211));
    CHECK(m.read(0x3f800004,4,value,false) && value==0x44332211);
    CHECK(m.write(0x3ff43054,4,0x15c04832));
    CHECK(!m.read(0x3f800000,4,value,false)); /* DIO cannot talk to a QPI device */
    CHECK(m.write(0x3ff43054,4,0x15c04834));
    CHECK(m.write(0x3ff43034,4,6));
    CHECK(!m.read(0x3f800000,4,value,false));
    CHECK(m.write(0x3ff43034,4,4));
    CHECK(!m.read(0x3f800000,3,value,true)); /* data memory is not executable */
    CHECK(!m.write(0x3f807ffe,4,0xaabbccdd)); /* no partial write across invalid page */
    CHECK(m.read(0x3f807ffc,4,value,false) && value==0);
    CHECK(m.write(0x3ff11204,4,1));
    CHECK(m.write(0x3f807ffe,4,0xaabbccdd));
    CHECK(m.read(0x3f807ffe,4,value,false) && value==0xaabbccdd);
    CHECK(m.write(0x3ff11200,4,1)); /* remap sees the same backing bytes */
    CHECK(m.read(0x3f800000,2,value,false) && value==0xaabb);
    CHECK(m.write(0x3ff11200,4,64));
    CHECK(!m.read(0x3f800000,4,value,false)); /* physical page beyond configured 2 MiB */
    CHECK(m.write(0x3ff11200,4,0));
    CHECK(m.write(0x3ff00040,4,0));
    CHECK(!m.write(0x3f800000,4,0));
    CHECK(m.write(0x3ff00040,4,8));
    CHECK(m.write(0x3ff00044,4,0x20));
    CHECK(!m.read(0x3f800000,4,value,false));
    CHECK(m.write(0x3ff00044,4,4u<<6)); /* 2 KiB pages, same 128-entry table */
    CHECK(!m.read(0x3f840000,4,value,false)); /* reduced mapped window does not alias */
    CHECK(m.read(0x3f800000,4,value,false) && value==0x12345678);
    CHECK(m.write(0x3ff42034,4,6) && m.write(0x3ff42000,4,1u<<30)); /* arm NOR write enable */
    CHECK(m.write(0x3ff48000,4,0x80000000) && m.step()==XTENSA_OK);
    CHECK(m.reset_count()==1);
    CHECK(m.write(0x3ff42000,4,1u<<27));
    CHECK(m.read(0x3ff42010,4,value,false) && (value&2)); /* external NOR latch survives SoC reset */
    CHECK(!m.read(0x3f800000,4,value,false)); /* controller/MMU configuration is reset */
    CHECK(m.read(0x3ff43054,4,value,false) && value==0x15c04830);
    CHECK(m.write(0x3ff00044,4,0) && m.write(0x3ff11200,4,0));
    CHECK(m.write(0x3ff43034,4,4) && m.write(0x3ff43054,4,0x15c04834));
    CHECK(m.write(0x3ff4305c,4,0x700000eb) && m.write(0x3ff43060,4,0x70000038));
    CHECK(m.read(0x3f800000,4,value,false) && value==0x12345678); /* external RAM contents and QPI mode survive */
    CHECK(m.write(0x3ff42034,4,5));
    CHECK(m.write(0x3ff4201c,4,0x80008040));
    CHECK(m.write(0x3ff42024,4,0x700000f5));
    CHECK(m.write(0x3ff42000,4,1u<<18));
    CHECK(!m.read(0x3f800000,4,value,false)); /* cache cannot access a SPI-mode PSRAM */
    return true;
}

bool per_core_psram_mapping()
{
    EspMachine m(esp::CHIP_ESP32);u32 value;
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.boot_index=layout.app_index=0;
    esp::Image image;image.chip=esp::CHIP_ESP32;image.entry_point=0x40080000;
    esp::Segment segment;segment.address=image.entry_point;
    segment.data=Bytes("\xf0\x20\x00\xf0\x20\x00\xf0\x20\x00",9);
    segment.data.resize(0x100,0);segment.data+=Bytes("\x38\x02\xf0\x20\x00",5);
    image.segments.push_back(segment);layout.images.push_back(image);
    CHECK(m.set_psram_size(0x200000) && m.load(Bytes(0x10000,'\xff'),layout,false));
    CHECK(m.write(0x3ff42034,4,5) && m.write(0x3ff4201c,4,0x80000040));
    CHECK(m.write(0x3ff42024,4,0x70000035) && m.write(0x3ff42000,4,1u<<18));
    CHECK(m.write(0x3ff43034,4,4) && m.write(0x3ff43054,4,0x15c04834));
    CHECK(m.write(0x3ff4305c,4,0x700000eb) && m.write(0x3ff43060,4,0x70000038));
    CHECK(m.write(0x3ff00044,4,0) && m.write(0x3ff0005c,4,0));
    CHECK(m.write(0x3ff11200,4,0) && m.write(0x3ff11204,4,1));
    CHECK(m.write(0x3ff13200,4,1)); /* APP maps the same virtual page to a different physical page */
    CHECK(m.write(0x3f800000,4,0x304f5250) && m.write(0x3f808000,4,0x31505041));
    CHECK(m.write(0x3ff00058,4,8) && m.write(0x3ff00038,4,0xc0080100));
    CHECK(m.write(0x3ff00030,4,1) && m.write(0x3ff0002c,4,0));
    m.cpu(1).set_reg(2,0x3f800000);
    CHECK(m.step()==XTENSA_OK && m.step()==XTENSA_OK);
    CHECK(m.step()==XTENSA_OK && m.step()==XTENSA_OK);
    CHECK(m.cpu(1).reg(3)==0x31505041);
    CHECK(m.write(0x3f800000,4,0xaabbccdd));
    CHECK(m.step()==XTENSA_OK); /* PRO runs next and observes its own mapping */
    CHECK(m.read(0x3f800000,4,value,false) && value==0x304f5250);
    CHECK(m.read(0x3f808000,4,value,false) && value==0xaabbccdd); /* shared physical storage */
    CHECK(m.write(0x3ff11200,4,1));
    CHECK(m.read(0x3f800000,4,value,false) && value==0xaabbccdd);
    return true;
}
}

static bool wifi_timestamp()
{
    EspMachine m(esp::CHIP_ESP32);u32 value=0,alias=0;
    CHECK(m.read(0x3ff73c00,4,value,false) && value==0);
    CHECK(m.read(0x60033c00,4,alias,false) && alias==value);
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,125);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60033c00,4,value,false) && value==125);
    CHECK(m.read(0x3ff73c00,4,alias,false) && alias==value);
    CHECK(m.write(0x3ff0003c,4,2)); /* changing CPU speed cannot change MAC time units */
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,75);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ff73c00,4,value,false) && value==200);
    m.cpu().set_pc(0x40008534);m.cpu().set_reg(0,0x40081234);m.cpu().set_reg(2,0xffffffffu);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60033c00,4,value,false) && value==199); /* hardware's 32-bit microsecond wrap */
    CHECK(!m.write(0x60033c00,4,0) && !m.write(0x3ff73c00,4,0));
    CHECK(!m.read(0x60033c04,4,value,false) && !m.read(0x3ff73c04,4,value,false)); /* no anonymous MAC registers */
    return true;
}

static bool wifi_baseband_reset()
{
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.read(0x3ff00024,4,value,false) && value==0);
    CHECK(m.read(0x3ff00028,4,value,false) && value==0);
    CHECK(m.write(0x3ff00024,4,0xa55a5aa7));
    CHECK(m.read(0x3ff00024,4,value,false) && value==0xa55a5aa7);
    CHECK(m.write(0x3ff00024,4,value&~2u)); /* PHY resets BB after disabling this selector */
    CHECK(m.read(0x3ff00024,4,value,false) && value==0xa55a5aa5);
    CHECK(m.write(0x3ff00028,4,0x12345678));
    CHECK(m.write(0x3ff5c01c,4,0x000c00bf));
    CHECK(m.read(0x6001c01c,4,value,false) && value==0x000c00bf);
    CHECK(m.write(0x3ff000d0,4,8)); /* Bluetooth reset leaves Wi-Fi AGC intact */
    CHECK(m.read(0x3ff5c01c,4,value,false) && value==0x000c00bf);
    CHECK(m.write(0x3ff000d0,4,1));
    CHECK(m.read(0x6001c01c,4,value,false) && value==0);
    CHECK(m.read(0x3ff00024,4,value,false) && value==0xa55a5aa5);
    CHECK(m.read(0x3ff00028,4,value,false) && value==0x12345678);
    CHECK(!m.write(0x3ff5c01c,4,0x7f0000));
    CHECK(m.diagnostic().find("held in reset")!=std::string::npos);
    CHECK(m.write(0x3ff000d0,4,0));
    CHECK(m.write(0x6001c01c,4,0x7f0000));
    CHECK(m.read(0x3ff5c01c,4,value,false) && value==0x7f0000);
    CHECK(m.write(0x3ff460a0,4,0x3c000));
    CHECK(m.write(0x3ff000d0,4,1));
    CHECK(m.read(0x600060a0,4,value,false) && value==0x3c000);
    CHECK(m.write(0x3ff000d0,4,2));
    CHECK(m.read(0x3ff460a0,4,value,false) && value==0);
    CHECK(!m.write(0x600060a0,4,0x3c000));
    CHECK(m.write(0x3ff5c030,4,0x10)); /* FE reset does not hold the baseband */
    CHECK(m.write(0x60033d38,4,0x0c800000));
    CHECK(m.write(0x3ff000d0,4,4));
    CHECK(m.read(0x3ff73d38,4,value,false) && value==0);
    CHECK(!m.write(0x60033d38,4,0x0c800000));
    CHECK(m.read(0x3ff5c030,4,value,false) && value==0x10);
    CHECK(m.write(0x3ff000d0,4,0));
    CHECK(m.write(0x60033d38,4,0x0c800000));
    CHECK(m.read(0x3ff73d38,4,value,false) && value==0x0c800000);
    return true;
}

int esp32_mmio_tests()
{
    checks = 0;
    if (!cache_controls() || !clock_configuration() || !analog_pll_bus() || !rtc_startup_power_controls() || !sha256_accelerator() || !rtc_time_and_storage() || !rtc_calibration() || !rtc_watchdog() || !timer_group_watchdog() || !watchdog_idle_deadline() || !software_reset_requests() || !watchdog_reset_domains() || !efuse_read_controller() || !flash_mmu() || !per_core_flash_mmu() || !psram_cache_mapping() || !per_core_psram_mapping() || !wifi_timestamp() || !wifi_baseband_reset()) return -1;
    return checks;
}

#ifdef ESP32_MMIO_TEST_MAIN
int main() { int n = esp32_mmio_tests(); printf("ESP32 MMIO: %d checks\n", n); return n < 0 ? 1 : 0; }
#endif
