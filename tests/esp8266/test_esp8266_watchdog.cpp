#include "esp8266/esp8266_watchdog.h"
#ifndef ESP8266_WATCHDOG_TEST_MAIN
#include "esp/esp_machine.h"
#endif
#include <stdio.h>
int esp8266_watchdog_tests() {
    int checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP8266 watchdog line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    using namespace esp8266_watchdog;
    Registers registers;u32 value=0;u64 ticks=0;
    CHECK(handles(0x60000900) && handles(0x60000918) && !handles(0x6000091c) && !handles(0x60000901));
    CHECK(read(registers,0x60000900,0,value) && value==0);
    CHECK(!next_event_ticks(registers,0,ticks));
    CHECK(write(registers,0x60000904,0x11,0) && write(registers,0x60000908,2,0));
    CHECK(read(registers,0x60000904,0,value) && value==1);
    CHECK(write(registers,0x60000900,0x39,100));
    CHECK(next_event_ticks(registers,100,ticks) && ticks==131072);
    CHECK(read(registers,0x6000090c,101,value) && value==131071);
    CHECK(esp8266_watchdog::advance(registers,131171)==NO_EVENT);
    CHECK(esp8266_watchdog::advance(registers,131172)==INTERRUPT);
    CHECK(read(registers,0x60000910,131172,value) && value==1);
    CHECK(next_event_ticks(registers,131172,ticks) && ticks==262144);
    CHECK(write(registers,0x60000914,0x72,131172));
    CHECK(next_event_ticks(registers,131172,ticks) && ticks==262144);
    CHECK(write(registers,0x60000914,0x73,131173));
    CHECK(read(registers,0x60000910,131173,value) && value==0);
    CHECK(next_event_ticks(registers,131173,ticks) && ticks==131072);
    CHECK(esp8266_watchdog::advance(registers,262245)==INTERRUPT);
    CHECK(write(registers,0x60000918,0x73,262246));
    CHECK(read(registers,0x60000910,262246,value) && value==0);
    CHECK(next_event_ticks(registers,262246,ticks) && ticks==262143);
    CHECK(esp8266_watchdog::advance(registers,524389)==INTERRUPT);
    CHECK(esp8266_watchdog::advance(registers,786533)==RESET);
    CHECK(!next_event_ticks(registers,786533,ticks));
    registers.clear();
    CHECK(write(registers,0x60000900,0x3d,0));
    CHECK(esp8266_watchdog::advance(registers,65535)==NO_EVENT && esp8266_watchdog::advance(registers,65536)==RESET);
    CHECK(!write(registers,0x6000090c,1,0) && !write(registers,0x60000910,1,0));
    registers.clear();
    CHECK(write(registers,0x60000900,0x3b,0));
    CHECK(esp8266_watchdog::advance(registers,65536)==INTERRUPT);
    CHECK(!next_event_ticks(registers,65536,ticks));
    CHECK(esp8266_watchdog::advance(registers,0x100000123ull)==NO_EVENT);
    CHECK(read(registers,0x6000090c,0x100000123ull,value) && value==65536-0x123);
    CHECK(write(registers,0x60000900,0x3a,0x100000123ull));
    CHECK(!next_event_ticks(registers,0x100000123ull,ticks));
    CHECK(read(registers,0x6000090c,0x200000123ull,value) && value==65536-0x123);
#ifndef ESP8266_WATCHDOG_TEST_MAIN
    /* Masked stage0 still advances to a real reset during WAITI. */
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP8266;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40100000;
    esp::Segment segment;segment.address=image.entry_point;segment.data.assign(0x100,'\0');
    segment.data.replace(0,3,"\x00\x70\x00",3);
    image.segments.push_back(segment);layout.images.push_back(image);
    EspMachine machine(layout.chip);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    CHECK(machine.write(0x60000900,4,0x39));
    CHECK(machine.step()==XTENSA_OK && machine.reset_count()==1);
    CHECK(machine.cpu().pc()==0x40000080 && machine.instructions()==1);
    CHECK(machine.elapsed_us()==131072/80);
    CHECK(machine.read(0x60000714,4,value,false) && (value&15)==4);
    CHECK(machine.read(0x60000900,4,value,false) && value==0);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_PS,0); // fixture installs a kernel-mode IRQ vector
    machine.cpu().set_special(XT_SR_INTENABLE,1u<<8);
    CHECK(machine.write(0x3ff00004,4,1) && machine.write(0x60000900,4,0x39));
    CHECK(machine.step()==XTENSA_OK && machine.reset_count()==0);
    CHECK(machine.cpu().pc()==image.entry_point+0x30 && machine.cpu().special(XT_SR_CCOUNT)==65536);
#endif
#undef CHECK
    return checks;
}
#ifdef ESP8266_WATCHDOG_TEST_MAIN
int main(){int count=esp8266_watchdog_tests();if(count>=0)printf("ESP8266 watchdog: %d checks passed.\n",count);return count<0;}
#endif
