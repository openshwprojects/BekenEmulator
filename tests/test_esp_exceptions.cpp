#include "esp_machine.h"
#include <stdio.h>

int esp_exception_tests()
{
    int checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("[FAIL] ESP exception line %d: %s\n",__LINE__,#x); return -1; } } while(0)
    esp::FlashImage layout;
    layout.chip=esp::CHIP_ESP32; layout.app_index=layout.boot_index=0;
    esp::Image image; image.chip=layout.chip; image.entry_point=0x40080000;
    esp::Segment segment; segment.address=image.entry_point; segment.data.assign(0x400,'\0');
    segment.data.replace(0,6,H("20 43 0a 00 70 00")); /* ADD.S f4,f3,f2; WAITI */
    image.segments.push_back(segment); layout.images.push_back(image);
    EspMachine machine(layout.chip);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_PS,0x40000);
    CHECK(machine.step()==XTENSA_EXCEPTION); /* No handler installed. */
    CHECK(machine.cpu().pc()==image.entry_point && machine.cpu().special(XT_SR_EXCCAUSE)==32);
    for(unsigned user=0;user<2;++user) {
        const u32 vector=image.entry_point+(user?0x340:0x300);
        layout.images[0].segments[0].data.replace(vector-image.entry_point,9,
            H("22 a0 01 20 e0 13 00 30 00")); /* MOVI a2,1; WSR CPENABLE; RFE */
        CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
        machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
        machine.cpu().set_special(XT_SR_PS,0x40000|(user?32:0));
        machine.cpu().set_float_bits(2,0x3fc00000);
        machine.cpu().set_float_bits(3,0x40100000);
        CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==vector);
        CHECK(machine.cpu().special(XT_SR_EPC1)==image.entry_point);
        CHECK(machine.run(20)==XTENSA_WAIT);
        CHECK(machine.cpu().float_bits(4)==0x40700000); /* Retried operation produces3.75. */
        CHECK(machine.instructions()==5 && machine.rom_calls()==0);
        CHECK(machine.diagnostic().empty());
    }
    /* A remapped ROM vector is executable SRAM, including exception entry. */
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    CHECK(machine.write(0x3ff00000,4,1));
    const Bytes handler=H("22 a0 01 20 e0 13 00 30 00");
    for(unsigned i=0;i<handler.size();++i)CHECK(machine.write(0x40000300+i,1,(u8)handler[i]));
    machine.cpu().set_special(XT_SR_VECBASE,0x40000000);
    machine.cpu().set_special(XT_SR_PS,0x40000);
    machine.cpu().set_float_bits(2,0x3fc00000);machine.cpu().set_float_bits(3,0x40100000);
    CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==0x40000300);
    CHECK(machine.run(20)==XTENSA_WAIT && machine.cpu().float_bits(4)==0x40700000);
    CHECK(machine.rom_calls()==0);
    layout.chip=layout.images[0].chip=esp::CHIP_ESP8266;
    layout.images[0].entry_point=layout.images[0].segments[0].address=0x40100000;
    layout.images[0].segments[0].data.assign(0x100,'\0');
    layout.images[0].segments[0].data.replace(0,9,H("22 23 00 42 a0 11 00 70 00"));
    /* Test a firmware alignment handler that records the fault by advancing
     * EPC1 past the offending load, then returns to the following MOVI. */
    layout.images[0].segments[0].data.replace(0x30,12,H("20 b1 03 22 c2 03 20 b1 13 00 30 00"));
    EspMachine lx106(layout.chip);
    CHECK(lx106.load(Bytes(0x100000,(char)0xff),layout,false));
    /* ROM _start sets PS.UM before main calls either the flash bootloader
     * or application. An interrupt during startup must therefore use the
     * user vector, not the kernel vector's BREAK1,0 fallback. */
    CHECK(lx106.cpu().special(XT_SR_PS)==0x20);
    lx106.cpu().set_special(XT_SR_VECBASE,0x40100000);
    lx106.cpu().set_special(XT_SR_INTENABLE,1u<<7);
    lx106.cpu().set_irq(7);
    CHECK(lx106.cpu().service_interrupts() && lx106.cpu().pc()==0x40100050);
    CHECK(lx106.cpu().special(XT_SR_EXCCAUSE)==4 && lx106.cpu().special(XT_SR_EPC1)==0x40100000);
    CHECK(lx106.load(Bytes(0x100000,(char)0xff),layout,true));
    CHECK(lx106.cpu().special(XT_SR_PS)==0x20); // bootloader entry has the same ROM handoff
    lx106.cpu().set_special(XT_SR_PS,0); // this fixture explicitly tests a kernel handler
    lx106.cpu().set_special(XT_SR_VECBASE,0x40100000);
    lx106.cpu().set_reg(3,0x3ffe8001);
    CHECK(lx106.step()==XTENSA_OK && lx106.cpu().pc()==0x40100030);
    CHECK(lx106.cpu().special(XT_SR_EXCVADDR)==0x3ffe8001);
    CHECK(lx106.run(20)==XTENSA_WAIT && lx106.cpu().reg(4)==17);
    CHECK(lx106.cpu().special(XT_SR_EXCCAUSE)==9 && lx106.diagnostic().empty());
#undef CHECK
    return checks;
}
