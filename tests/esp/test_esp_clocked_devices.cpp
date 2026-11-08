#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
struct ClockProbe : EspMachine::ClockedDevice {
    EspMachine &machine;
    unsigned resets,events;
    u64 deadline,period,last_us;
    bool scheduled,irq;
    ClockProbe(EspMachine &m):machine(m),resets(0),events(0),deadline(0),
        period(0),last_us(0),scheduled(false),irq(false) {}
    void reset(u64 now) {++resets;events=0;scheduled=false;last_us=now;}
    bool next_event_us(u64 &out) const {out=deadline;return scheduled;}
    void advance(u64 now) {
        ++events;last_us=now;
        if(period)deadline=now+period;else scheduled=false;
        if(irq)machine.cpu().set_irq(6);
    }
    void arm(u64 when,u64 repeat=0,bool interrupt=false) {
        deadline=when;period=repeat;irq=interrupt;scheduled=true;
    }
};
}

int esp_clocked_device_tests()
{
    int checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP clocked devices line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    for(unsigned chip=0;chip<2;++chip) {
        esp::FlashImage layout;layout.chip=chip?esp::CHIP_ESP32:esp::CHIP_ESP8266;
        layout.app_index=layout.boot_index=0;
        esp::Image image;image.chip=layout.chip;image.entry_point=chip?0x40080000:0x40100000;
        esp::Segment code;code.address=image.entry_point;code.data.assign(0x400,'\0');
        code.data.replace(0,3,"\x00\x70\x00",3); // WAITI 0
        code.data.replace(chip?0x300:0x30,3,"\x22\xa0\x17",3); // MOVI a2,23
        image.segments.push_back(code);layout.images.push_back(image);
        EspMachine machine(layout.chip);ClockProbe probe(machine),second(machine);
        CHECK(!machine.attach_clocked_device(0));
        CHECK(machine.attach_clocked_device(&probe) && probe.resets==1 && probe.last_us==0);
        CHECK(!machine.attach_clocked_device(&probe) && probe.resets==1);
        CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
        CHECK(probe.resets==2 && probe.last_us==0);
        machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
        machine.cpu().set_special(XT_SR_PS,chip?0x40000:0);
        machine.cpu().set_special(XT_SR_INTENABLE,1u<<6);
        probe.arm(10,0,true);
        CHECK(machine.step()==XTENSA_OK);
        CHECK(probe.events==1 && probe.last_us==10 && machine.elapsed_us()==10);
        CHECK(machine.instructions()==1 && machine.cpu().special(XT_SR_CCOUNT)==800);
        CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==23);

        // A repeating external event with no guest wake must yield to the
        // host. It must not retire instructions or trap pause/stop forever.
        CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
        probe.arm(1000,1000);
        CHECK(machine.step()==XTENSA_OK);
        CHECK(probe.events>0 && probe.events<=1024);
        CHECK(machine.instructions()==1 && machine.cpu().waiting());
        CHECK(machine.elapsed_us()>0 && machine.elapsed_us()<1000000);
        const u64 first=machine.elapsed_us();const unsigned events=probe.events;
        CHECK(machine.step()==XTENSA_OK);
        CHECK(machine.elapsed_us()>first && probe.events>events && machine.instructions()==1);
        CHECK(machine.detach_clocked_device(&probe));
        CHECK(!machine.detach_clocked_device(&probe) && !machine.detach_clocked_device(0));
        CHECK(machine.step()==XTENSA_WAIT && machine.elapsed_us()>first);

        // Attach uses the current epoch, then earliest deadline wins across
        // devices. An event can wake the CPU even if another is later.
        CHECK(machine.attach_clocked_device(&probe) && probe.last_us==machine.elapsed_us());
        CHECK(machine.attach_clocked_device(&second));
        machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
        machine.cpu().set_special(XT_SR_PS,chip?0x40000:0);
        machine.cpu().set_special(XT_SR_INTENABLE,1u<<6);
        const u64 start=machine.elapsed_us();
        probe.arm(start+40);second.arm(start+20,0,true);
        CHECK(machine.step()==XTENSA_OK);
        CHECK(second.events==1 && second.last_us==start+20 && probe.events==0);
        CHECK(machine.instructions()==2 && machine.cpu().reg(2)==23);

        // Digital reset cancels old external transactions and reports the
        // retained guest clock; reloading an image starts a new time epoch.
        const unsigned before_reset=probe.resets;
        CHECK(machine.write(chip?0x3ff48000:0x60000700,4,0x80000000));
        CHECK(machine.step()==XTENSA_OK && machine.reset_count()==1);
        CHECK(probe.resets==before_reset+1 && !probe.scheduled && !second.scheduled);
        CHECK(probe.last_us==machine.elapsed_us() && probe.last_us>=start+20);
        CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
        CHECK(probe.last_us==0 && probe.resets==before_reset+2);

        // An already-due event is dispatched without advancing guest time.
        // Detaching another pending device prevents its callback entirely.
        probe.arm(0);second.arm(0);
        CHECK(machine.detach_clocked_device(&second));
        CHECK(machine.step()==XTENSA_WAIT);
        CHECK(probe.events==1 && probe.last_us==0 && second.events==0);
        CHECK(machine.attach_clocked_device(&second));
        CHECK(machine.detach_clocked_device(&probe) && machine.detach_clocked_device(&second));
    }
#undef CHECK
    return checks;
}
