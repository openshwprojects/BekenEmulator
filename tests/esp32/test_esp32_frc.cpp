#include "esp32/esp32_interrupts.h"
#include <stdio.h>

namespace {
struct FrcBus : XtensaBus {
    virtual bool read(u32,unsigned,u32 &,bool) { return false; }
    virtual bool write(u32,unsigned,u32) { return false; }
};
}

int esp32_frc_tests()
{
    int checks=0;
#define FRC_CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP32 FRC line %d: %s\n",__LINE__,#x);return -1;}} while(0)
    const u32 base=0x3ff47000,map=0x3ff00104,appmap=0x3ff00218;
    FrcBus bus;XtensaCpu pro(bus,true),app(bus,true);Esp32Interrupts p(pro);p.attach_cpu(1,&app);
    u32 value=0;u64 ticks=0;
    FRC_CHECK(p.read(base+4,0,value) && value==0x7fffff);
    FRC_CHECK(p.read(base+0x24,0,value) && value==0xffffffffu);
    FRC_CHECK(!p.handles(base+0x10) && !p.handles(base+0x14) && !p.handles(base+1));
    FRC_CHECK(!p.write(base+0x24,0,0)); /* current count is read-only */
    /* The real NSPanel esp_timer startup uses legacy FRC2, not TIMG0/1. */
    FRC_CHECK(p.write(base+0x30,0,0xefffffffu));
    FRC_CHECK(p.write(base+0x20,0,0) && p.write(base+0x28,0,0x81));
    FRC_CHECK(p.write(base+0x2c,0,1));
    FRC_CHECK(p.read(base+0x24,80,value) && value==80);
    FRC_CHECK(!p.next_event_ticks(ticks)); /* disconnected reset matrix route */
    FRC_CHECK(p.write(map+57*4,80,0) && p.write(appmap+57*4,80,2));
    pro.set_special(XT_SR_INTENABLE,1);app.set_special(XT_SR_INTENABLE,4);
    FRC_CHECK(p.write(base+0x30,80,100));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==20);
    p.advance(99);
    FRC_CHECK(!(pro.special(XT_SR_INTERRUPT)&1) && !(app.special(XT_SR_INTERRUPT)&4));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==1);
    p.advance(100);
    FRC_CHECK((pro.special(XT_SR_INTERRUPT)&1) && (app.special(XT_SR_INTERRUPT)&4));
    FRC_CHECK(p.read(base+0x28,100,value) && value==0x181);
    FRC_CHECK(p.write(base+0x2c,100,0) && (pro.special(XT_SR_INTERRUPT)&1));
    FRC_CHECK(p.write(base+0x2c,100,1) && !(pro.special(XT_SR_INTERRUPT)&1) && !(app.special(XT_SR_INTERRUPT)&4));
    FRC_CHECK(p.read(base+0x28,100,value) && value==0x81);
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==0x100000000ull); /* match occurs again only after wrap */
    FRC_CHECK(p.write(base+0x30,100,105));
    pro.set_special(XT_SR_PS,1);app.set_special(XT_SR_INTENABLE,0);
    FRC_CHECK(!p.next_event_ticks(ticks));
    app.set_special(XT_SR_INTENABLE,4);
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==5); /* either core can wake */
    p.advance(105);
    FRC_CHECK(app.special(XT_SR_INTERRUPT)&4);
    FRC_CHECK(p.write(base+0x2c,105,1));
    /* Prescaler phase survives repeated reads and the APB counter wraps. */
    p.reset();pro.set_special(XT_SR_PS,0);pro.set_special(XT_SR_INTENABLE,1);
    FRC_CHECK(p.write(map+57*4,0,0));
    FRC_CHECK(p.write(base+0x20,0,0xfffffffeu) && p.write(base+0x30,0,1));
    FRC_CHECK(p.write(base+0x28,0,0x85));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==48);
    FRC_CHECK(p.read(base+0x24,15,value) && value==0xfffffffeu);
    FRC_CHECK(p.read(base+0x24,16,value) && value==0xffffffffu);
    FRC_CHECK(p.read(base+0x24,32,value) && value==0);
    FRC_CHECK(p.read(base+0x24,47,value) && value==0 && !(pro.special(XT_SR_INTERRUPT)&1));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==1);
    FRC_CHECK(p.read(base+0x24,48,value) && value==1 && (pro.special(XT_SR_INTERRUPT)&1));
    FRC_CHECK(p.write(base+0x28,48,5) && p.write(base+0x2c,48,1));
    FRC_CHECK(p.read(base+0x24,1000,value) && value==1 && !p.next_event_ticks(ticks));
    /* FRC1 is a 23-bit down counter. Edge delivery must not stay asserted. */
    p.reset();pro.set_special(XT_SR_INTENABLE,1u<<10);
    FRC_CHECK(p.write(map+56*4,0,10));
    FRC_CHECK(p.write(base,0,0xff800004) && p.read(base,0,value) && value==4);
    FRC_CHECK(p.write(base+8,0,0xc0));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==4);
    p.advance(4);
    FRC_CHECK((pro.special(XT_SR_INTERRUPT)&(1u<<10)) && p.read(base+4,4,value) && value==4);
    pro.set_special(XT_SR_INTCLEAR,1u<<10);p.advance(5);
    FRC_CHECK(!(pro.special(XT_SR_INTERRUPT)&(1u<<10)));
    FRC_CHECK(p.next_event_ticks(ticks) && ticks==3);
    p.advance(8);
    FRC_CHECK(pro.special(XT_SR_INTERRUPT)&(1u<<10));
    p.reset();
    FRC_CHECK(p.read(base+8,1000,value) && value==0 && !p.next_event_ticks(ticks));
#undef FRC_CHECK
    return checks;
}

#ifdef ESP32_FRC_TEST_MAIN
int main(){int count=esp32_frc_tests();if(count>=0)printf("ESP32 FRC: %d checks passed.\n",count);return count<0;}
#endif
