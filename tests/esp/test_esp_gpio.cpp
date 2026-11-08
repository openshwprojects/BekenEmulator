#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("ESP GPIO check failed %d: %s\n",__LINE__,#x); return false; } } while (0)
bool edges_and_levels() {
    for(unsigned chip=0;chip<2;++chip) {
        EspMachine m(chip?esp::CHIP_ESP32:esp::CHIP_ESP8266);u32 v;
        const u32 base=chip?0x3ff44000:0x60000300,pin=base+(chip?0x88:0x28)+5*4;
        const u32 status=base+(chip?0x44:0x1c),clear=status+8,mask=chip?(4u<<13):0,irq=chip?8:16;
        if(chip)CHECK(m.write(0x3ff00104+22*4,4,3));
        CHECK(m.set_gpio_input(5,0) && m.write(pin,4,mask|(1u<<7)));
        CHECK(m.set_gpio_input(5,1) && (m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.read(status,4,v,false) && v==32);
        CHECK(m.write(clear,4,32) && !(m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.set_gpio_input(5,1) && !(m.cpu().special(XT_SR_INTERRUPT)&irq)); // no inventededge
        CHECK(m.set_gpio_input(5,0) && !(m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.write(pin,4,mask|(2u<<7)) && m.set_gpio_input(5,1));
        CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.set_gpio_input(5,0) && (m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.write(clear,4,32) && m.write(pin,4,mask|(3u<<7)));
        CHECK(m.set_gpio_input(5,1) && (m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.write(clear,4,32) && m.set_gpio_input(5,0) && (m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.write(clear,4,32) && m.write(pin,4,mask|(4u<<7)));
        CHECK(m.cpu().special(XT_SR_INTERRUPT)&irq); // lowlevel fires immediately
        CHECK(m.write(clear,4,32) && (m.cpu().special(XT_SR_INTERRUPT)&irq)); // W1C cannot suppressactivelevel
        CHECK(m.set_gpio_input(5,1) && m.write(clear,4,32) && !(m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.write(pin,4,mask|(5u<<7)) && (m.cpu().special(XT_SR_INTERRUPT)&irq));
        CHECK(m.set_gpio_input(5,0) && m.write(clear,4,32) && !(m.cpu().special(XT_SR_INTERRUPT)&irq));
    }
    return true;
}
bool output_and_pull_edges() {
    for(unsigned chip=0;chip<2;++chip) {
        EspMachine m(chip?esp::CHIP_ESP32:esp::CHIP_ESP8266);u32 v;
        const u32 b=chip?0x3ff44000:0x60000300,pin=b+(chip?0x88:0x28),mask=chip?(4u<<13):0;
        const u32 status=b+(chip?0x44:0x1c),out=b+(chip?4:0),enable=b+(chip?0x20:0xc);
        const u32 pad=chip?0x3ff48498:0x60000834;
        CHECK(m.write(pad,4,0)); // classic ESP32 GPIO0 resets with its RTC pullup enabled
        CHECK(m.write(pin,4,mask|(3u<<7)));
        CHECK(m.write(pad,4,chip?0x08000000:0x80));
        CHECK(m.read(status,4,v,false) && v==1); // pullup createsrisingedge
        CHECK(m.write(status+8,4,1));
        CHECK(m.write(enable+4,4,1) && m.read(status,4,v,false) && v==1); // outputlowoverridespull
        CHECK(m.write(status+8,4,1) && m.write(out+4,4,1));
        CHECK(m.read(status,4,v,false) && v==1);
        CHECK(m.write(status+8,4,1) && m.write(pin,4,mask|(3u<<7)|4)); // opendrainhighreleasespin
        CHECK(m.write(pad,4,0) && m.read(status,4,v,false) && v==1); // releasedpinlosespullup
        CHECK(m.set_gpio_input(0,1) && m.write(status+8,4,1));
        CHECK(m.write(out+8,4,1) && m.read(status,4,v,false) && v==0); // externaldrivewins
        CHECK(m.set_gpio_input(0,-1) && m.read(status,4,v,false) && v==1);
    }
    return true;
}
bool masked_views_and_software_status() {
    EspMachine m(esp::CHIP_ESP32);u32 v;
    const u32 b=0x3ff44000,pin=b+0x88+33*4;
    CHECK(m.write(0x3ff00104+22*4,4,3) && m.write(0x3ff00104+23*4,4,14));
    CHECK(m.write(pin,4,(1u<<13)|(1u<<7)) && m.set_gpio_input(33,1)); // APPonly
    CHECK(m.read(b+0x50,4,v,false) && v==2);
    CHECK(m.read(b+0x74,4,v,false) && v==2);
    CHECK(m.read(b+0x7c,4,v,false) && v==0 && !(m.cpu().special(XT_SR_INTERRUPT)&8));
    CHECK(m.read(0x3ff000ec,4,v,false) && !(v&(1u<<22))); // PROmatrixinput
    CHECK(m.read(0x3ff000f8,4,v,false) && (v&(1u<<22))); // APPmatrixinput
    CHECK(m.write(pin,4,(4u<<13)|(1u<<7)) && (m.cpu().special(XT_SR_INTERRUPT)&8));
    CHECK(m.write(pin,4,(8u<<13)|(1u<<7)) && !(m.cpu().special(XT_SR_INTERRUPT)&8));
    CHECK(m.cpu().special(XT_SR_INTERRUPT)&(1u<<14));
    CHECK(m.read(b+0x80,4,v,false) && v==2);
    CHECK(m.write(b+0x58,4,2) && (m.cpu().special(XT_SR_INTERRUPT)&(1u<<14))); // CPU NMI edge remainslatched
    CHECK(m.cpu().service_interrupts() && !(m.cpu().special(XT_SR_INTERRUPT)&(1u<<14)));
    CHECK(m.write(pin,4,4u<<13) && m.write(b+0x54,4,2)); // softwarestatusworkswithoutedge
    CHECK(m.cpu().special(XT_SR_INTERRUPT)&8);
    CHECK(!m.set_gpio_input(24,1) && !m.set_gpio_input(28,1) && !m.set_gpio_input(40,1));
    CHECK(!m.write(b+0x3c,4,1) && !m.read(b+0x128,4,v,false));
    CHECK(m.write(b+0x130,4,0xffffffff) && m.read(b+0x130,4,v,false) && v==0xff);
    CHECK(m.write(b+0x530,4,0xffffffff) && m.read(b+0x530,4,v,false) && v==0xfff);
    EspMachine n(esp::CHIP_ESP8266);
    CHECK(n.write(0x60000320,4,8) && (n.cpu().special(XT_SR_INTERRUPT)&16));
    CHECK(n.write(0x60000324,4,8) && !(n.cpu().special(XT_SR_INTERRUPT)&16));
    CHECK(n.set_gpio_input(16,1) && !(n.cpu().special(XT_SR_INTERRUPT)&16));
    return true;
}
}
int esp_gpio_tests() {
    checks=0;
    if(!edges_and_levels() || !output_and_pull_edges() || !masked_views_and_software_status())return -1;
    printf("All ESP GPIO tests passed (%d checks).\n",checks);return checks;
}
#ifdef ESP_GPIO_TEST_MAIN
int main() { return esp_gpio_tests()<0?1:0; }
#endif
