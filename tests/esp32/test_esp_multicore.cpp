#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
esp::FlashImage firmware()
{
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.boot_index=layout.app_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40080000;
    esp::Segment segment;segment.address=image.entry_point;segment.data.assign(0x800,'\0');
    segment.data.replace(0,3,H("00 70 00")); /* PRO waits while APP initializes. */
    segment.data.replace(0x100,15,H("20 eb 03 22 65 00 42 a0 42 42 66 00 00 70 00"));
    /* APP reads its hardware identity, publishes to shared RAM and writes B
     * to the shared UART before waiting for an interrupt. */
    segment.data.replace(0x480,3,H("72 a0 23")); /* APP level2 handler: MOVI a7,35 */
    image.segments.push_back(segment);layout.images.push_back(image);return layout;
}
bool release(EspMachine &machine,u32 address)
{
    return machine.write(0x3ff00038,4,address) && machine.write(0x3ff00030,4,1)
        && machine.write(0x3ff0002c,4,0);
}
}

int esp_multicore_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP multicore line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspMachine machine(esp::CHIP_ESP32);
    esp::FlashImage layout=firmware();
    const Bytes flash(0x100000,(char)0xff);
    CHECK(machine.load(flash,layout,false));
    CHECK(machine.core_count()==2 && machine.cpu(0).special(XT_SR_PRID)==0xcdcd);
    CHECK(machine.cpu(1).special(XT_SR_PRID)==0xabab);
    CHECK(release(machine,0));
    CHECK(machine.step()==XTENSA_WAIT && machine.instructions()==1);
    CHECK(machine.cpu(1).instructions()==0); /* ROM-less reset waits for boot word. */
    CHECK(machine.write(0x3ff00038,4,0xc0080100)); /* documented CALLX0 fast boot */
    machine.cpu(1).set_reg(5,0x3ffb1000);machine.cpu(1).set_reg(6,0x3ff40000);
    CHECK(machine.run(20)==XTENSA_WAIT);
    u32 value=0;
    CHECK(machine.read(0x3ffb1000,4,value,false) && value==0xabab);
    CHECK(machine.uart()=="B" && machine.cpu(1).instructions()==5);
    CHECK(machine.cpu(0).reg(2)!=0xabab && machine.rom_calls()==1);
    CHECK(machine.cpu(0).special(XT_SR_CCOUNT)==6 && machine.cpu(1).special(XT_SR_CCOUNT)==6);
    /* A timer routed only to APP wakes APP while PRO remains in WAITI. */
    machine.cpu(1).set_special(XT_SR_VECBASE,0x40080300);
    machine.cpu(1).set_special(XT_SR_PS,0x40000);
    machine.cpu(1).set_special(XT_SR_INTENABLE,1u<<19);
    CHECK(machine.write(0x3ff00218+14*4,4,19));
    CHECK(machine.write(0x3ff5f098,4,1));
    CHECK(machine.write(0x3ff5f010,4,400));
    CHECK(machine.write(0x3ff5f000,4,0xc0004c00));
    CHECK(machine.step()==XTENSA_OK && machine.cpu(1).reg(7)==35);
    CHECK(machine.cpu(0).waiting() && machine.cpu(0).instructions()==1);
    CHECK(machine.cpu(0).special(XT_SR_CCOUNT)==807 && machine.cpu(1).special(XT_SR_CCOUNT)==807);
    CHECK(machine.elapsed_us()==10);
    /* Hardware reset clears APP architectural state while aggregate guest
     * progress/time stay monotonic and PRO's state remains untouched. */
    const u64 previous_instructions=machine.instructions();
    CHECK(machine.write(0x3ff0002c,4,1));
    CHECK(machine.write(0x3ff5f000,4,0));
    CHECK(machine.step()==XTENSA_WAIT);
    CHECK(machine.cpu(1).instructions()==0 && machine.cpu(1).pc()==0x40000400);
    CHECK(machine.instructions()==previous_instructions && machine.elapsed_us()==10);
    CHECK(machine.cpu(1).special(XT_SR_PRID)==0xabab && machine.cpu(1).reg(5)==0);
    /* Supplied ROM bytes take precedence on the second core too. */
    CHECK(machine.load(flash,layout,false));
    RomSegment reset;reset.address=reset.physical_address=0x40000400;reset.executable=true;
    reset.data=H("20 eb 03 22 65 00 00 70 00");
    CHECK(machine.load_rom(std::vector<RomSegment>(1,reset)));
    machine.cpu(1).set_reg(5,0x3ffb1000);
    CHECK(release(machine,0));
    CHECK(machine.run(20)==XTENSA_WAIT);
    CHECK(machine.read(0x3ffb1000,4,value,false) && value==0xabab);
    CHECK(machine.rom_calls()==0 && machine.cpu(1).instructions()==3);
    /* Clock gating freezes APP's cycle counter while PRO runs. Run-stall
     * stops instructions but leaves its clock/counter alive. */
    layout.images[0].segments[0].data.replace(0,3,H("06 ff ff")); /* J self */
    CHECK(machine.load(flash,layout,false));
    CHECK(release(machine,0xc0080100));
    CHECK(machine.write(0x3ff00034,4,1));
    CHECK(machine.run(20)==XTENSA_OK);
    CHECK(machine.cpu(0).instructions()==20 && machine.cpu(1).instructions()==0);
    CHECK(machine.cpu(1).special(XT_SR_CCOUNT)==20);
    CHECK(machine.write(0x3ff00030,4,0));
    CHECK(machine.run(20)==XTENSA_OK && machine.cpu(1).special(XT_SR_CCOUNT)==20);
    CHECK(machine.cpu(0).special(XT_SR_CCOUNT)==40);
    CHECK(machine.write(0x3ff00030,4,1) && machine.write(0x3ff00034,4,0));
    CHECK(machine.write(0x3ff48000,4,2) && machine.write(0x3ff480ac,4,0x02100000));
    CHECK(machine.run(20)==XTENSA_OK && machine.cpu(1).instructions()==0);
    CHECK(machine.cpu(1).special(XT_SR_CCOUNT)==40);
    CHECK(machine.write(0x3ff480ac,4,0));
    machine.cpu(1).set_reg(5,0x3ffb1000);machine.cpu(1).set_reg(6,0x3ff40000);
    CHECK(machine.run(20)==XTENSA_OK && machine.uart()=="B");
    CHECK(machine.cpu(1).waiting() && machine.cpu(0).instructions()>40);
#undef CHECK
    return checks;
}
