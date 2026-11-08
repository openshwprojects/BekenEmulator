#include "esp8266/esp8266_timers.h"
#ifndef ESP8266_TIMER_TEST_MAIN
#include "esp/esp_machine.h"
#endif
#include <stdio.h>

namespace {
struct TimerBus : XtensaBus {
    virtual bool read(u32,unsigned,u32 &,bool){return false;}
    virtual bool write(u32,unsigned,u32){return false;}
};
}
int esp8266_timer_tests() {
    int checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP8266 timers line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    TimerBus bus;XtensaCpu cpu(bus);Esp8266Timers timers(cpu);u32 value=0;u64 delta=0;
    CHECK(timers.read(0x60000604,0,value) && value==0x7fffff);
    CHECK(timers.read(0x60000624,0,value) && value==0xffffffffu);
    CHECK(!timers.handles(0x60000610) && !timers.handles(0x60000601));
    CHECK(timers.write(0x60000600,0,0xffffffffu));
    CHECK(timers.read(0x60000600,0,value) && value==0x7fffff);
    CHECK(!timers.write(0x60000604,0,0));
    CHECK(timers.write(0x60000600,0,10) && timers.write(0x60000608,0,0x84));
    cpu.set_special(XT_SR_INTENABLE,1u<<9);timers.set_routing(2,0,0);
    CHECK(timers.next_event_ticks(delta) && delta==160);
    timers.advance(159);
    CHECK(timers.read(0x60000604,159,value) && value==1);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<9)));
    CHECK(timers.next_event_ticks(delta) && delta==1);
    timers.advance(160);
    CHECK((cpu.special(XT_SR_INTERRUPT)&(1u<<9))!=0);
    CHECK(timers.read(0x60000608,160,value) && (value&0x100));
    cpu.set_special(XT_SR_INTCLEAR,1u<<9);timers.advance(161);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<9))); // Edge remains acknowledged until the next event.
    timers.reset();
    CHECK(timers.write(0x60000600,0,4) && timers.write(0x60000608,0,0xc0));
    timers.set_routing(2,0,0);timers.advance(11);
    CHECK(timers.read(0x60000604,11,value) && value==1);
    CHECK(timers.next_event_ticks(delta) && delta==1);
    cpu.set_special(XT_SR_INTCLEAR,1u<<9);timers.advance(12);
    CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<9));
    CHECK(timers.read(0x60000604,12,value) && value==4);
    timers.reset();
    CHECK(timers.write(0x60000620,0,0xfffffff0u) && timers.write(0x60000630,0,3));
    CHECK(timers.write(0x60000628,0,0x80));
    cpu.set_special(XT_SR_INTENABLE,1u<<10);timers.set_routing(4,0,0);
    CHECK(timers.next_event_ticks(delta) && delta==19);
    timers.advance(18);
    CHECK(timers.read(0x60000624,18,value) && value==2);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<10)));
    timers.advance(19);
    CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<10));
    CHECK(timers.read(0x60000624,19,value) && value==3);
    cpu.set_special(XT_SR_PS,1);
    CHECK(!timers.next_event_ticks(delta));
    cpu.set_special(XT_SR_PS,0);cpu.set_special(XT_SR_INTENABLE,0);
    CHECK(!timers.next_event_ticks(delta));

    /* Divider phase survives small advances and clock-independent pauses. */
    timers.reset(1000);cpu.set_special(XT_SR_INTENABLE,1u<<9);
    CHECK(timers.write(0x60000600,1000,2) && timers.write(0x60000608,1000,0x88));
    timers.set_routing(2,0,1000);timers.advance(1255);
    CHECK(timers.read(0x60000604,1255,value) && value==2);
    CHECK(timers.next_event_ticks(delta) && delta==257);
    timers.advance(1256);
    CHECK(timers.read(0x60000604,1256,value) && value==1);
    CHECK(timers.write(0x60000608,1256,8));
    timers.advance(2000);
    CHECK(timers.read(0x60000604,2000,value) && value==1 && !timers.next_event_ticks(delta));
    CHECK(timers.write(0x60000608,2000,0x88));
    timers.advance(2256);
    CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<9));

    /* Level status is acknowledged by an INT write, including the SDK's
     * zero write. It does not require the separately gated edge output. */
    timers.reset();cpu.set_special(XT_SR_PS,0);
    CHECK(timers.write(0x60000600,0,3) && timers.write(0x60000608,0,0x81));
    CHECK(timers.next_event_ticks(delta) && delta==3);
    timers.advance(3);
    CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<9));
    CHECK(timers.write(0x6000060c,3,0));
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<9)));
    CHECK(timers.read(0x60000608,3,value) && value==0x81);

    /* NONOS NmiTimSetFunc selects low DPORT bits15. NMI ignores INTENABLE
     * and preempts level1, but cannot recursively preempt itself. */
    timers.reset();cpu.set_special(XT_SR_INTENABLE,0);cpu.set_special(XT_SR_PS,1);
    CHECK(timers.write(0x60000600,0,5) && timers.write(0x60000608,0,0xc0));
    timers.set_routing(2,15,0);
    CHECK(timers.next_event_ticks(delta) && delta==5);
    timers.advance(5);
    CHECK((cpu.special(XT_SR_INTERRUPT)&((1u<<14)|(1u<<9)))==(1u<<14));
    CHECK(cpu.service_interrupts() && cpu.pc()==0x40000020);
    CHECK(!timers.next_event_ticks(delta));
    timers.advance(6);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<14)));

    /* The shared FRC2 engine handles wrap/reload and large jumps in O(1).
     * ESP32's adapter uses the documented W1C acknowledgement directly. */
    EspFrcTimers shared;
    CHECK(shared.write(0x20,0xfffffff0u) && shared.write(0x30,0xfffffff5u));
    CHECK(shared.write(0x28,0xc1));
    CHECK(shared.next_event_ticks(1,delta) && delta==5);
    CHECK(shared.advance(0x100000015ull)==2);
    CHECK(shared.read(0x24,value) && value==0xfffffff5u);
    CHECK(shared.pending(1) && shared.write(0x2c,0) && shared.pending(1));
    CHECK(shared.write(0x2c,1) && !shared.pending(1));
    CHECK(shared.write(0x30,7) && !shared.next_event_ticks(1,delta));
    CHECK(shared.advance(0x200000015ull)==0);
    CHECK(!shared.write(0x24,0) && !shared.read(0x10,value));
    /* Turning reload on after a free-running wrap changes the next wrap,
     * not the counter's current value or an alarm still ahead this turn. */
    shared.reset();
    CHECK(shared.write(0x20,0xfffffff0u) && shared.write(0x28,0x80));
    shared.advance(21);
    CHECK(shared.read(0x24,value) && value==5);
    CHECK(shared.write(0x28,0xc0) && shared.write(0x30,7));
    CHECK(shared.next_event_ticks(1,delta) && delta==2);
    CHECK(shared.advance(23)==2 && shared.read(0x24,value) && value==7);
    CHECK(!shared.next_event_ticks(1,delta));

#ifndef ESP8266_TIMER_TEST_MAIN
    /* Execute actual WAITI instructions through the machine. Peripheral
     * time advances without manufacturing guest instructions, at80/160MHz. */
    for(unsigned fast=0;fast<2;++fast) {
        esp::FlashImage layout;layout.chip=esp::CHIP_ESP8266;layout.app_index=layout.boot_index=0;
        esp::Image image;image.chip=layout.chip;image.entry_point=0x40100000;
        esp::Segment segment;segment.address=image.entry_point;segment.data.assign(0x100,'\0');
        segment.data.replace(0,3,"\x00\x70\x00",3); /* WAITI0 */
        segment.data.replace(0x30,3,"\x22\xa0\x17",3); /* MOVI a2,23 */
        image.segments.push_back(segment);layout.images.push_back(image);
        EspMachine machine(layout.chip);
        CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
        machine.cpu().set_special(XT_SR_PS,0); // fixture installs a kernel-mode IRQ vector
        machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
        machine.cpu().set_special(XT_SR_INTENABLE,1u<<10);
        CHECK(machine.write(0x3ff00014,4,fast));
        CHECK(machine.write(0x3ff00004,4,0x1004));
        CHECK(machine.write(0x60000620,4,0) && machine.write(0x60000630,4,800));
        CHECK(machine.write(0x60000628,4,0x80));
        CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==image.entry_point+0x30);
        CHECK(machine.cpu().special(XT_SR_CCOUNT)==800*(fast+1));
        CHECK(machine.elapsed_us()==10 && machine.instructions()==1);
        CHECK(machine.read(0x3ff00004,4,value,false) && value==0x1004);
        CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==23);
    }
#endif
#undef CHECK
    return checks;
}
#ifdef ESP8266_TIMER_TEST_MAIN
int main(){int count=esp8266_timer_tests();if(count>=0)printf("ESP8266 timers: %d checks passed.\n",count);return count<0;}
#endif
