#include "esp/esp_machine.h"
#include <stdio.h>

int esp_reset_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP reset line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.boot_index=layout.app_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40080000;
    esp::Segment code;code.address=image.entry_point;code.data=H("22 a0 11 00 70 00");
    image.segments.push_back(code);layout.images.push_back(image);
    const Bytes flash(0x100000,(char)0xff);
    EspMachine machine(layout.chip);
    CHECK(machine.load(flash,layout,false));
    RomSegment rom;rom.address=rom.physical_address=0x40000400;rom.executable=true;
    rom.data=H("22 a0 2a 00 70 00");
    CHECK(machine.load_rom(std::vector<RomSegment>(1,rom)));
    CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==17);
    CHECK(machine.write(0x3ffb0100,4,0x11223344));
    CHECK(machine.write(0x50000000,4,0x55667788));
    CHECK(machine.write(0x3ff480b8,4,0x12345678));
    CHECK(machine.write(0x3ff40000,4,'A'));
    CHECK(machine.write(0x3ff40014,4,123));
    machine.receive_uart(0,"RX");
    CHECK(machine.set_gpio_input(18,1));
    const u64 before=machine.instructions();
    CHECK(machine.write(0x3ff48000,4,0x80000000));
    CHECK(machine.reset_count()==0 && machine.cpu().reg(2)==17); /* deferred */
    CHECK(machine.step()==XTENSA_OK && machine.reset_count()==1);
    CHECK(machine.cpu().pc()==0x40000400 && machine.cpu().reg(2)==0);
    CHECK(machine.instructions()==before && machine.rom_calls()==0);
    u32 value=0;
    CHECK(machine.read(0x3ff48034,4,value,false) && (value&0xfff)==(3u|(3u<<6)));
    CHECK(machine.read(0x3ff480b8,4,value,false) && value==0x12345678);
    CHECK(machine.read(0x3ffb0100,4,value,false) && value==0x11223344);
    CHECK(machine.read(0x50000000,4,value,false) && value==0x55667788);
    CHECK(machine.flash()==flash && machine.uart()=="A");
    CHECK(machine.read(0x3ff40014,4,value,false) && value==0x2b6);
    CHECK(machine.read(0x3ff4001c,4,value,false) && (value&255)==0); /* RX reset */
    bool input,output,enabled;
    CHECK(machine.gpio_state(18,input,output,enabled) && input && !enabled);
    CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==42);
    CHECK(machine.instructions()==before+1 && machine.rom_calls()==0);
    /* A CPU-only reset retains device configuration and the other core. */
    CHECK(machine.write(0x3ff40014,4,456));
    machine.cpu(1).set_reg(7,99);
    CHECK(machine.write(0x3ff48000,4,1u<<5));
    CHECK(machine.step()==XTENSA_OK && machine.reset_count()==2);
    CHECK(machine.cpu().pc()==0x40000400 && machine.cpu(1).reg(7)==99);
    CHECK(machine.read(0x3ff40014,4,value,false) && value==456);
    CHECK(machine.read(0x3ff48034,4,value,false) && (value&0xfff)==(12u|(3u<<6)));
    machine.cpu().set_reg(7,88);
    CHECK(machine.write(0x3ff48000,4,1u<<4));
    CHECK(machine.step()==XTENSA_OK && machine.reset_count()==3);
    CHECK(machine.cpu().reg(7)==88 && machine.cpu(1).reg(7)==0);
    CHECK(machine.cpu(1).special(XT_SR_PRID)==0xabab);
    CHECK(machine.read(0x3ff48034,4,value,false) && (value&0xfff)==(12u|(12u<<6)));
    machine.cpu().set_pc(0x400081d4);machine.cpu().set_reg(2,1);
    machine.cpu().set_reg(0,image.entry_point);
    CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==12); /* ROM API agrees */
    /* A new image load remains a fresh machine, including its host transcript. */
    CHECK(machine.load(flash,layout,false));
    CHECK(machine.reset_count()==0 && machine.instructions()==0 && machine.uart().empty());
    /* Digital reset retains the RTC-selected source, while resetting the
     * digital divider. ROM's frequency API must describe the resulting clock. */
    const unsigned sources[]={0,2,1},expected[]={26,8,80};
    CHECK(machine.set_xtal_frequency(26));
    for(unsigned i=0;i<3;++i) {
        CHECK(machine.write(0x3ff48070,4,sources[i]<<27));
        CHECK(machine.write(0x3ff0003c,4,2)); /* 240 MHz before PLL reset */
        CHECK(machine.write(0x3ff48000,4,0x80000000));
        CHECK(machine.step()==XTENSA_OK);
        machine.cpu().set_pc(0x4000855c);machine.cpu().set_reg(0,image.entry_point);
        CHECK(machine.step()==XTENSA_OK && machine.cpu().reg(2)==expected[i]);
    }
    /* The LX106 software-reset register resets the digital domain and
     * resumes the ROM reset vector. RTC scratch/RAM and the external flash
     * survive; the serial transcript belongs to the host, not the FIFO. */
    layout.chip=image.chip=esp::CHIP_ESP8266;
    image.entry_point=code.address=0x40100000;
    image.segments.clear();image.segments.push_back(code);
    layout.images.clear();layout.images.push_back(image);
    EspMachine lx106(layout.chip);
    CHECK(lx106.load(flash,layout,false));
    rom.address=rom.physical_address=0x40000080;
    CHECK(lx106.load_rom(std::vector<RomSegment>(1,rom)));
    CHECK(lx106.read(0x60000714,4,value,false) && (value&15)==1);
    bool running=true;
    for(unsigned i=0;i<800 && running;++i) {
        lx106.cpu().set_pc(image.entry_point);
        running=lx106.step()==XTENSA_OK;
    }
    CHECK(running);
    CHECK(lx106.write(0x3ffe9000,4,0x12345678));
    CHECK(lx106.write(0x60001200,4,0x88776655));
    CHECK(lx106.write(0x60000730,4,0x11224488));
    CHECK(lx106.write(0x60000774,4,1) && lx106.write(0x60000768,4,1));
    CHECK(lx106.write(0x6000030c,4,16) && lx106.set_gpio_input(4,1));
    CHECK(lx106.write(0x60000000,4,'L') && lx106.write(0x60000014,4,333));
    lx106.receive_uart(0,"RX");
    CHECK(lx106.write(0x3ff00014,4,1)); /* 160 MHz digital divider */
    u32 rtc_before;
    CHECK(lx106.read(0x6000071c,4,rtc_before,false) && rtc_before>0);
    const u64 lx_before=lx106.instructions(),us_before=lx106.elapsed_us();
    CHECK(lx106.write(0x60000700,4,0x80000200));
    CHECK(lx106.reset_count()==0 && lx106.cpu().reg(2)==17);
    CHECK(lx106.step()==XTENSA_OK && lx106.reset_count()==1);
    CHECK(lx106.cpu().pc()==0x40000080 && lx106.cpu().reg(2)==0);
    CHECK(lx106.instructions()==lx_before && lx106.elapsed_us()==us_before);
    CHECK(lx106.read(0x60000714,4,value,false) && (value&15)==3);
    CHECK(lx106.read(0x60000700,4,value,false) && value==0x200); /* reset strobe clears */
    CHECK(lx106.read(0x60000730,4,value,false) && value==0x11224488);
    CHECK(lx106.read(0x60001200,4,value,false) && value==0x88776655);
    CHECK(lx106.read(0x3ffe9000,4,value,false) && value==0x12345678);
    CHECK(lx106.read(0x6000071c,4,value,false) && value==rtc_before);
    CHECK(lx106.read(0x60000014,4,value,false) && value==0x2b6);
    CHECK(lx106.read(0x6000001c,4,value,false) && (value&255)==0);
    CHECK(lx106.read(0x3ff00014,4,value,false) && value==0);
    CHECK(lx106.gpio_state(4,input,output,enabled) && input && !enabled);
    CHECK(lx106.gpio_state(16,input,output,enabled) && input && output && enabled);
    CHECK(lx106.flash()==flash && lx106.uart()=="L");
    CHECK(lx106.step()==XTENSA_OK && lx106.cpu().reg(2)==42);
    CHECK(lx106.instructions()==lx_before+1 && lx106.rom_calls()==0);
    CHECK(lx106.load(flash,layout,false));
    CHECK(lx106.reset_count()==0 && lx106.uart().empty());
    CHECK(lx106.read(0x60000714,4,value,false) && (value&15)==1);
#undef CHECK
    return checks;
}
