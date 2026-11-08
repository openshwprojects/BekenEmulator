#include "esp8266/esp8266_mac_timers.h"
#include "esp8266/esp8266_wdev.h"
#ifndef ESP8266_MAC_TIMERS_TEST_MAIN
#include "esp/esp_machine.h"
#endif
#include <stdio.h>

int esp8266_mac_timers_tests()
{
    int checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP8266 MAC timers line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    using namespace esp8266_mac_timers;
    Registers regs;u32 value=0;u64 ticks=0;
    CHECK(handles(0x3ff21048) && handles(0x3ff2104c) && handles(0x3ff210d0));
    CHECK(!handles(0x3ff21050) && !handles(0x3ff21049) && !handles(0x3ff210d4));
    CHECK(read(regs,0x3ff21048,800,value) && value==0);
    esp8266_mac_timers::advance(regs,1600);
    CHECK(regs.empty());
    CHECK(!write(regs,0x3ff21048,1,800));
    CHECK(write(regs,0x3ff21058,0x80000000,800));
    CHECK(read(regs,0x3ff21048,879,value) && value==0);
    CHECK(read(regs,0x3ff21048,880,value) && value==1);
    CHECK(read(regs,0x3ff21048,959,value) && value==1);
    CHECK(read(regs,0x3ff21048,960,value) && value==2);
    CHECK(write(regs,0x3ff21058,0,999));
    CHECK(read(regs,0x3ff21048,8000,value) && value==2);
    CHECK(write(regs,0x3ff21058,0x80000000,8000));
    CHECK(read(regs,0x3ff21048,8040,value) && value==2);
    CHECK(read(regs,0x3ff21048,8041,value) && value==3);
    CHECK(write(regs,0x3ff21058,0,8041));
    CHECK(write(regs,0x3ff2105c,0xfffffffe,8041) && write(regs,0x3ff21060,5,8041));
    CHECK(write(regs,0x3ff21058,0xc4000000,8100));
    CHECK(read(regs,0x3ff21048,8340,value) && value==1);
    CHECK(read(regs,0x3ff2104c,8340,value) && value==6);
    CHECK(!next_event_ticks(regs,8340,ticks));
    CHECK(write(regs,0x3ff210cc,4,8340) && write(regs,0x3ff210d0,6,8340));
    CHECK(write(regs,0x3ff210c8,0x80000000,8340));
    CHECK(next_event_ticks(regs,8341,ticks) && ticks==239);
    esp8266_mac_timers::advance(regs,8579);
    CHECK(!esp8266_wdev::interrupt(regs));
    CHECK(esp8266_wdev::write(regs,0x3ff20c18,1u<<26));
    CHECK(!esp8266_wdev::interrupt(regs));
    esp8266_mac_timers::advance(regs,8580);
    CHECK(esp8266_wdev::interrupt(regs));
    CHECK(esp8266_wdev::read(regs,0x3ff20c20,0,value) && value==(1u<<26));
    CHECK(!next_event_ticks(regs,8580,ticks));
    CHECK(esp8266_wdev::write(regs,0x3ff20c24,1u<<26));
    esp8266_mac_timers::advance(regs,9000);
    CHECK(!esp8266_wdev::interrupt(regs));
    /* Re-arm at the same deadline is due immediately; clearing enable
     * cancels it. An alarm that occurs masked remains latched. */
    CHECK(write(regs,0x3ff210c8,0x80000000,9000));
    CHECK(esp8266_wdev::interrupt(regs));
    CHECK(esp8266_wdev::write(regs,0x3ff20c24,1u<<26));
    CHECK(write(regs,0x3ff210cc,100,9000) && write(regs,0x3ff210c8,0x80000000,9000));
    CHECK(write(regs,0x3ff210c8,0,9001) && !next_event_ticks(regs,9001,ticks));
    regs.clear();
    CHECK(write(regs,0x3ff210cc,1,0) && write(regs,0x3ff210c8,0x80000000,0));
    CHECK(!next_event_ticks(regs,80,ticks));
    CHECK(write(regs,0x3ff21058,0x80000000,80));
    CHECK(next_event_ticks(regs,80,ticks) && ticks==80);
    esp8266_mac_timers::advance(regs,160);
    CHECK(!esp8266_wdev::interrupt(regs));
    CHECK(esp8266_wdev::write(regs,0x3ff20c18,1u<<26) && esp8266_wdev::interrupt(regs));
    regs.clear();
    CHECK(write(regs,0x3ff2105c,0xfffffffe,0) && write(regs,0x3ff21060,0xffffffff,0));
    CHECK(write(regs,0x3ff210cc,1,0) && write(regs,0x3ff210d0,0,0));
    CHECK(write(regs,0x3ff210c8,0x80000000,0) && write(regs,0x3ff21058,0x80000000,0));
    CHECK(next_event_ticks(regs,0,ticks) && ticks==240);
    CHECK(esp8266_wdev::write(regs,0x3ff20c18,1u<<26));
    esp8266_mac_timers::advance(regs,240);
    CHECK(esp8266_wdev::interrupt(regs));
#ifndef ESP8266_MAC_TIMERS_TEST_MAIN
    /* Execute WAITI and wake on the modeled MAC alarm, including a masked
     * event that becomes visible only when firmware enables its source. */
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP8266;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40100000;
    esp::Segment segment;segment.address=image.entry_point;segment.data.assign(0x100,'\0');
    segment.data.replace(0,3,"\x00\x70\x00",3);
    segment.data.replace(0x30,2,"\x0c\x72",2); // MOVI.N a2,7
    image.segments.push_back(segment);layout.images.push_back(image);
    EspMachine machine(layout.chip);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    machine.cpu().set_special(XT_SR_PS,0); // fixture installs a kernel-mode IRQ vector
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_INTENABLE,1);
    CHECK(machine.write(0x3ff21058,4,0x80000000));
    CHECK(machine.write(0x3ff210cc,4,5) && machine.write(0x3ff210d0,4,0));
    CHECK(machine.write(0x3ff210c8,4,0x80000000));
    CHECK(machine.write(0x3ff20c18,4,1u<<26));
    CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==image.entry_point+0x30);
    CHECK(machine.elapsed_us()==5 && machine.cpu().special(XT_SR_CCOUNT)==400);
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==(1u<<26));
    CHECK(machine.write(0x3ff20c24,4,1u<<26));
    CHECK(!(machine.cpu().special(XT_SR_INTERRUPT)&1));
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    machine.cpu().set_special(XT_SR_PS,0);
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_INTENABLE,1);
    CHECK(machine.write(0x3ff21058,4,0x80000000));
    CHECK(machine.write(0x3ff210cc,4,7) && machine.write(0x3ff210c8,4,0x80000000));
    CHECK(machine.step()==XTENSA_WAIT && machine.elapsed_us()==7);
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
    CHECK(machine.write(0x3ff20c18,4,1u<<26));
    CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==7);
    CHECK(machine.write(0x3ff20c24,4,1u<<26));
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
#endif
#undef CHECK
    return checks;
}
#ifdef ESP8266_MAC_TIMERS_TEST_MAIN
int main(){int count=esp8266_mac_timers_tests();if(count>=0)printf("ESP8266 MAC timers: %d checks passed.\n",count);return count<0;}
#endif
