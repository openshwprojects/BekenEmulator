#include "esp32/esp32_interrupts.h"
#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("ESP interrupt check failed %d: %s\n",__LINE__,#x); return false; } } while (0)
struct Bus : XtensaBus {
    virtual bool read(u32,unsigned,u32&,bool) { return false; }
    virtual bool write(u32,unsigned,u32) { return false; }
};
const u32 T0=0x3ff5f000,T1=0x3ff60000,MAP=0x3ff00104;
u32 cfg(unsigned divider,bool increase=true,bool reload=false,bool alarm=false,bool edge=false) {
    return 0x80000000u|(increase?0x40000000u:0)|(reload?0x20000000u:0)|(divider<<13)|
        (alarm?0x400u:0)|(edge?0x1000u:0x800u);
}

bool registers_and_counters() {
    Bus bus; XtensaCpu cpu(bus,true); Esp32Interrupts p(cpu); u32 v;
    CHECK(p.read(T0,0,v) && v==0x60002000);
    CHECK(p.read(MAP+14*4,0,v) && v==16); // reset routes to an internal IRQ, disconnected
    CHECK(!p.handles(T0+0x68) && !p.handles(T0+0x48)); // calibration/watchdog belong to separate devices
    CHECK(!p.read(0x3ff0032c,0,v));
    CHECK(!p.write(T0+4,0,123)); // captured counter is read only
    CHECK(p.write(T0,0,cfg(2)));
    p.advance(9);
    CHECK(p.read(T0+4,9,v) && v==0); // counter does not refresh until UPDATE
    CHECK(p.write(T0+0xc,9,0));
    CHECK(p.read(T0+4,9,v) && v==4);
    CHECK(p.write(T0+0xc,10,0));
    CHECK(p.read(T0+4,10,v) && v==5); // prescaler phase survives small time steps
    CHECK(p.write(T0,10,cfg(2)&~0x80000000u));
    CHECK(p.write(T0+0xc,100,0));
    CHECK(p.read(T0+4,100,v) && v==5); // disabled counter freezes
    CHECK(p.write(T0+0x18,100,0xffffffffu) && p.write(T0+0x1c,100,7) && p.write(T0+0x20,100,0));
    CHECK(p.write(T0,100,cfg(2)) && p.write(T0+0xc,102,0));
    CHECK(p.read(T0+4,102,v) && v==0);
    CHECK(p.read(T0+8,102,v) && v==8); // low word carries into high
    CHECK(p.write(T1+0x24,102,cfg(0))); // divider zero represents65536
    CHECK(p.write(T1+0x30,102+65535,0));
    CHECK(p.read(T1+0x28,102+65535,v) && v==0);
    CHECK(p.write(T1+0x30,102+65536,0));
    CHECK(p.read(T1+0x28,102+65536,v) && v==1);
    return true;
}

bool level_alarm_and_reload() {
    Bus bus; XtensaCpu cpu(bus,true); Esp32Interrupts p(cpu); u32 v; u64 next=0;
    cpu.set_special(XT_SR_INTENABLE,1); cpu.set_special(XT_SR_VECBASE,0x1000); cpu.set_pc(0x1234);
    CHECK(p.write(MAP+14*4,0,0));
    CHECK(p.write(T0+0x98,0,1));
    CHECK(p.write(T0+0x10,0,5));
    CHECK(p.write(T0+0x18,0,2));
    CHECK(p.write(T0,0,cfg(80,true,true,true)));
    CHECK(p.next_event_ticks(next) && next==400);
    p.advance(399);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&1));
    CHECK(p.next_event_ticks(next) && next==1);
    p.advance(400);
    CHECK((cpu.special(XT_SR_INTERRUPT)&1) && p.read(T0+0x9c,400,v) && v==1);
    CHECK(p.read(T0,400,v) && !(v&0x400)); // hardware disarms even in auto-reload mode
    CHECK(p.write(T0+0xc,400,0) && p.read(T0+4,400,v) && v==2);
    CHECK(cpu.service_interrupts() && cpu.pc()==0x1300 && cpu.special(XT_SR_EPC1)==0x1234);
    CHECK(p.write(T0+0xa4,400,1) && !(cpu.special(XT_SR_INTERRUPT)&1));
    CHECK(!p.next_event_ticks(next));
    CHECK(p.write(T0,400,cfg(80,true,true,true)));
    CHECK(!p.next_event_ticks(next)); // EXCM masks level1
    cpu.set_special(XT_SR_PS,0x40000);
    CHECK(p.next_event_ticks(next) && next==240);
    p.advance(640);
    CHECK(cpu.special(XT_SR_INTERRUPT)&1);
    CHECK(p.write(MAP+14*4,640,6)); // route to an internal CPU timer means disconnect
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&1) && !(cpu.special(XT_SR_INTERRUPT)&(1u<<6)));
    CHECK(p.read(0x3ff000ec,640,v) && (v&(1u<<14))); // source status is independent of routing
    return true;
}

bool shared_edge_and_masked_deadlines() {
    Bus bus; XtensaCpu cpu(bus,true); Esp32Interrupts p(cpu); u64 next; u32 v;
    CHECK(p.write(MAP+34*4,0,2) && p.write(MAP+35*4,0,2));
    p.set_source(34,true); p.set_source(35,true);
    CHECK(cpu.special(XT_SR_INTERRUPT)&4);
    p.set_source(34,false); CHECK(cpu.special(XT_SR_INTERRUPT)&4);
    p.set_source(35,false); CHECK(!(cpu.special(XT_SR_INTERRUPT)&4));
    CHECK(p.write(MAP+58*4,0,10)); // timer0 edge maps to CPU edgeIRQ10
    CHECK(p.write(T0+0x98,0,1) && p.write(T0+0x10,0,2) && p.write(T0,0,cfg(2,true,false,true,true)));
    cpu.set_special(XT_SR_INTENABLE,1u<<10);
    p.advance(4); CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<10));
    cpu.set_special(XT_SR_INTCLEAR,1u<<10);
    p.advance(6); CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<10))); // edge latch does not continuously reassert
    CHECK(p.write(T0+0xa4,6,1) && p.write(T0+0x10,6,4) && p.write(T0,6,cfg(2,true,false,true,true)));
    p.advance(8); CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<10));
    p.reset(); cpu.set_special(XT_SR_INTENABLE,1u<<0); cpu.set_special(XT_SR_PS,0x40000);
    CHECK(p.write(MAP+14*4,0,2) && p.write(MAP+15*4,0,0));
    CHECK(p.write(T0+0x98,0,3));
    CHECK(p.write(T0+0x10,0,2) && p.write(T0+0x34,0,10));
    CHECK(p.write(T0,0,cfg(2,true,false,true)) && p.write(T0+0x24,0,cfg(2,true,false,true)));
    CHECK(p.next_event_ticks(next) && next==20); // disabled earlier IRQ must not hide enabled later event
    CHECK(p.write(0x3ff000dc,0,1) && p.read(0x3ff000dc,0,v) && v==1);
    CHECK(p.write(MAP+24*4,0,3) && (cpu.special(XT_SR_INTERRUPT)&8)); // software cross-core source
    CHECK(p.write(0x3ff000dc,0,0) && !(cpu.special(XT_SR_INTERRUPT)&8));
    return true;
}

bool down_counter_and_core_isolation() {
    Bus bus; XtensaCpu cpu(bus,true); Esp32Interrupts p(cpu);u32 v;
    CHECK(p.write(T1+0x3c,0,2) && p.write(T1+0x40,0,0) && p.write(T1+0x44,0,0));
    CHECK(p.write(T1+0x34,0,0) && p.write(T1+0x98,0,2));
    CHECK(p.write(T1+0x24,0,cfg(2,false,false,true)));
    CHECK(p.write(0x3ff00218+19*4,0,0)); // APP_CPU route does not assert PRO_CPU
    p.advance(4);
    CHECK(p.read(T1+0x9c,4,v) && v==2 && !(cpu.special(XT_SR_INTERRUPT)&1));
    CHECK(p.write(MAP+19*4,4,0) && (cpu.special(XT_SR_INTERRUPT)&1));
    CHECK(p.write(T1+0x30,6,0) && p.read(T1+0x28,6,v) && v==0xffffffffu);
    CHECK(p.read(T1+0x2c,6,v) && v==0xffffffffu); // down-count wrap is64bits
    CHECK(p.write(T1+0xa4,6,2) && !(cpu.special(XT_SR_INTERRUPT)&1));
    CHECK(p.read(T0+0x9c,6,v) && v==0); // groups remain independent
    return true;
}

bool two_cpu_matrix_routing() {
    Bus bus;XtensaCpu pro(bus,true),app(bus,true);Esp32Interrupts p(pro);u64 next;
    p.attach_cpu(1,&app);
    CHECK(p.write(MAP+34*4,0,3) && p.write(0x3ff00218+34*4,0,19));
    p.set_source(34,true);
    CHECK((pro.special(XT_SR_INTERRUPT)&8) && (app.special(XT_SR_INTERRUPT)&(1u<<19)));
    pro.set_special(XT_SR_INTENABLE,0);app.set_special(XT_SR_INTENABLE,1u<<19);
    CHECK(!p.source_can_wake(34,0) && p.source_can_wake(34,1) && p.source_can_wake(34));
    p.set_source(34,false);
    CHECK(!(pro.special(XT_SR_INTERRUPT)&8) && !(app.special(XT_SR_INTERRUPT)&(1u<<19)));
    CHECK(p.write(MAP+24*4,0,10) && p.write(0x3ff00218+24*4,0,10));
    p.set_source(24,true);
    CHECK((pro.special(XT_SR_INTERRUPT)&1024) && (app.special(XT_SR_INTERRUPT)&1024));
    pro.set_special(XT_SR_INTCLEAR,1024);
    CHECK(!(pro.special(XT_SR_INTERRUPT)&1024) && (app.special(XT_SR_INTERRUPT)&1024));
    p.attach_cpu(0,&pro);
    CHECK(!(pro.special(XT_SR_INTERRUPT)&1024)); // idempotentattachcannotreplayanedge
    p.set_source(25,true); // unrelatedsourcechange mustnotrelatchtheoldedge
    CHECK(!(pro.special(XT_SR_INTERRUPT)&1024));
    CHECK(p.write(0x3ff00218+14*4,0,19) && p.write(T0+0x98,0,1));
    CHECK(p.write(T0+0x10,0,10) && p.write(T0,0,cfg(2,true,false,true)));
    CHECK(p.next_event_ticks(next) && next==20); // APP-only timer can wake sharedmachineidle
    p.attach_cpu(1,0);
    CHECK(!p.source_can_wake(34) && !p.next_event_ticks(next));
    p.attach_cpu(1,&app);p.advance(20);
    CHECK(app.special(XT_SR_INTERRUPT)&(1u<<19));
    CHECK(!(pro.special(XT_SR_INTERRUPT)&(1u<<19)));
    p.reset();
    CHECK(!(app.special(XT_SR_INTERRUPT)&(1u<<19)) && !(pro.special(XT_SR_INTERRUPT)&8));
    return true;
}

bool watchdog_group_sources() {
    Bus bus; XtensaCpu cpu(bus,true); Esp32Interrupts p(cpu);u32 v;
    CHECK(p.write(MAP+16*4,0,3) && p.write(T0+0x98,0,4));
    p.configure_group_interrupt(0,2,true,false); p.raise_group_interrupt(0,2);
    CHECK((cpu.special(XT_SR_INTERRUPT)&8) && p.read(T0+0x9c,0,v) && v==4);
    CHECK(p.write(T0+0xa4,0,4) && !(cpu.special(XT_SR_INTERRUPT)&8));
    p.advance(100);
    CHECK(p.read(T0+0x9c,100,v) && v==0); // W1C stays acknowledged until another stage event
    p.configure_group_interrupt(0,2,false,true);
    CHECK(p.write(MAP+60*4,100,10));
    p.raise_group_interrupt(0,2); CHECK(cpu.special(XT_SR_INTERRUPT)&(1u<<10));
    cpu.set_special(XT_SR_INTCLEAR,1u<<10); p.advance(101);
    CHECK(!(cpu.special(XT_SR_INTERRUPT)&(1u<<10)));
    return true;
}

bool clock_cpu_cycles(EspMachine &m,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        m.cpu().set_pc(0x40080000);
        if(m.cpu().step()!=XTENSA_OK) return false;
    }
    return true;
}
bool machine_apb_clock_continuity() {
    EspMachine m(esp::CHIP_ESP32);u32 v;
    CHECK(m.write(0x40080000,4,0x0020f0)); // NOP, one modeled CPU cycle
    CHECK(m.write(T0,4,cfg(2)) && clock_cpu_cycles(m,16));
    CHECK(m.write(T0+0xc,4,0) && m.read(T0+4,4,v,false) && v==8);
    CHECK(m.write(0x3ff48070,4,0x00002210)); // switch80MHzPLL to40MHzXTAL
    CHECK(clock_cpu_cycles(m,16));
    CHECK(m.write(T0+0xc,4,0) && m.read(T0+4,4,v,false) && v==16); // APB tracksXTAL, preserving earlier count
    CHECK(m.write(0x3ff0003c,4,2) && m.write(0x3ff48070,4,0x08002210)); // PLL240MHz, APB80MHz
    CHECK(clock_cpu_cycles(m,6));
    CHECK(m.write(T0+0xc,4,0) && m.read(T0+4,4,v,false) && v==17);
    CHECK(m.write(0x3ff48070,4,0x00002210) && clock_cpu_cycles(m,16));
    CHECK(m.write(T0+0xc,4,0) && m.read(T0+4,4,v,false) && v==25);
    return true;
}
}

int esp_interrupt_tests() {
    checks=0;
    if(!two_cpu_matrix_routing())return -1;
    if (!registers_and_counters() || !level_alarm_and_reload() || !shared_edge_and_masked_deadlines() || !down_counter_and_core_isolation() || !watchdog_group_sources() || !machine_apb_clock_continuity()) return -1;
    printf("All ESP32 interrupt/timer tests passed (%d checks).\n",checks);
    return checks;
}
#ifdef ESP_INTERRUPTS_TEST_MAIN
int main() { return esp_interrupt_tests()<0?1:0; }
#endif
