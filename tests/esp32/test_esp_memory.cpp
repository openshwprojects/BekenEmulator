#include "esp/esp_machine.h"
#include <stdio.h>

int esp_memory_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP memory line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP32;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40080000;
    esp::Segment code;code.address=image.entry_point;code.data=H("00 70 00");image.segments.push_back(code);
    layout.images.push_back(image);
    EspMachine machine(layout.chip);const Bytes flash(0x100000,(char)0xff);
    CHECK(machine.load(flash,layout,false));
    u32 value=0;
    /* ESP-IDF SOC_DIRAM_INVERTED: words run in opposite directions in the
     * two bus views, but bytes retain their order within each word. */
    CHECK(machine.write(0x3ffe0000,4,0x11223344));
    CHECK(machine.read(0x400bfffc,4,value,false) && value==0x11223344);
    CHECK(machine.write(0x3ffe0004,4,0x55667788));
    CHECK(machine.read(0x400bfff8,4,value,false) && value==0x55667788);
    CHECK(machine.read(0x3ffe0003,4,value,false) && value==0x66778811);
    CHECK(machine.write(0x3ffe0003,4,0xbbaa9988));
    CHECK(machine.read(0x400bfffc,4,value,false) && value==0x88223344);
    CHECK(machine.read(0x400bfff8,4,value,false) && value==0x55bbaa99);
    CHECK(machine.write(0x400a0000,4,0xaabbccdd));
    CHECK(machine.read(0x3ffffffc,4,value,false) && value==0xaabbccdd);
    CHECK(!machine.write(0x3fffffff,2,0x1234));
    CHECK(machine.read(0x400a0000,4,value,false) && value==0xaabbccdd);
    CHECK(!machine.read(0x3fffffff,2,value,false));
    CHECK(!machine.read(0x3ffffffc,1,value,true));
    /* Code written through the DRAM view executes from the IRAM view. */
    CHECK(machine.write(0x3ffffdfc,4,0x002aa022)); /* MOVI a2,42; first WAITI byte */
    CHECK(machine.write(0x3ffffdf8,4,0x00000070));
    machine.cpu().set_pc(0x400a0200);
    CHECK(machine.run(10)==XTENSA_WAIT && machine.cpu().reg(2)==42);
    /* Image and mask-ROM initializers must use the same physical storage. */
    esp::Segment data;data.address=0x3ffe0040;data.data="RAM!";
    layout.images[0].segments.push_back(data);
    CHECK(machine.load(flash,layout,false));
    CHECK(machine.read(0x400bffbc,4,value,false) && value==0x214d4152);
    RomSegment rom;rom.address=rom.physical_address=0x40000500;rom.executable=true;rom.data=H("00 70 00");
    RomSegment init;init.address=init.physical_address=0x3ffe0080;init.initialize_ram=true;init.data="ROM!";
    std::vector<RomSegment> segments;segments.push_back(rom);segments.push_back(init);
    CHECK(machine.load_rom(segments));
    CHECK(machine.read(0x400bff7c,4,value,false) && value==0x214d4f52);
    /* RTC RAM keeps its documented direct alias and remains a different pool. */
    CHECK(machine.write(0x3ff80000,4,0x12345678));
    CHECK(machine.read(0x400c0000,4,value,false) && value==0x12345678);
    CHECK(machine.read(0x3ffe0040,4,value,false) && value==0x214d4152);
    /* The first32KiB ROM window can expose SRAM1 independently per core.
     * Its old instruction-bus alias is then hidden; DRAM access remains. */
    CHECK(machine.load(flash,layout,false));
    rom.address=rom.physical_address=0x40000000;rom.data.assign(0x8004,'\0');
    rom.data.replace(0,4,"ROM!");wr32le(&rom.data[0x8000],0xaabbccdd);
    CHECK(machine.load_rom(std::vector<RomSegment>(1,rom)));
    CHECK(machine.write(0x400b0000,4,0x11223344));
    CHECK(machine.write(0x400b7ffc,4,0x55667788));
    CHECK(machine.read(0x40000000,4,value,false) && value==0x214d4f52);
    CHECK(machine.write(0x3ff00000,4,1));
    CHECK(machine.read(0x40000000,4,value,false) && value==0x11223344);
    CHECK(!machine.read(0x400b0000,4,value,false));
    CHECK(!machine.write(0x400b0000,4,0));
    CHECK(machine.write(0x40000000,4,0x12345678));
    CHECK(machine.read(0x3ffefffc,4,value,false) && value==0x12345678);
    CHECK(machine.write(0x3ffefffc,4,0x87654321));
    CHECK(machine.read(0x40000000,4,value,false) && value==0x87654321);
    CHECK(machine.read(0x40007fff,2,value,false) && value==0xdd55);
    CHECK(!machine.write(0x40007fff,2,0));
    CHECK(machine.read(0x40007ffc,4,value,false) && value==0x55667788);
    /* Remapped code at a known ROM-service address must execute as RAM. */
    CHECK(machine.write(0x40007cf8,4,0x004da022)); /* MOVI a2,77; WAITI 0 */
    CHECK(machine.write(0x40007cfc,2,0x0070));
    machine.cpu().set_pc(0x40007cf8);
    CHECK(machine.run(10)==XTENSA_WAIT && machine.cpu().reg(2)==77 && machine.rom_calls()==0);
    /* APP still reads the old SRAM address until its own remap bit is set. */
    CHECK(machine.write(0x40080100,4,0x22002322)); /* L32I a2,a3,0; S32I a2,a4,0 */
    CHECK(machine.write(0x40080104,4,0x70000064));
    CHECK(machine.write(0x40080108,1,0));
    CHECK(machine.write(0x3ff00030,4,1) && machine.write(0x3ff0002c,4,0));
    machine.cpu(1).set_pc(0x40080100);machine.cpu(1).set_reg(3,0x400b0000);machine.cpu(1).set_reg(4,0x3ffb1000);
    CHECK(machine.run(20)==XTENSA_WAIT);
    CHECK(machine.read(0x3ffb1000,4,value,false) && value==0x87654321);
    CHECK(machine.write(0x3ff00004,4,1));
    machine.cpu(1).set_pc(0x40007cf8);
    CHECK(machine.run(10)==XTENSA_WAIT && machine.cpu(1).reg(2)==77 && machine.rom_calls()==0);
    CHECK(machine.write(0x3ff00000,4,0) && machine.write(0x3ff00004,4,0));
    CHECK(machine.read(0x400b0000,4,value,false) && value==0x87654321);
    CHECK(machine.read(0x40000000,4,value,false) && value==0x214d4f52);
    CHECK(machine.active_core()==1);
    CHECK(!machine.read(0x3ff80000,4,value,false)); /* RTC FAST belongs to PRO */
    CHECK(!machine.read(0x400c0000,1,value,true));
    CHECK(!machine.write(0x3ff80000,4,1));
    CHECK(machine.write(0x50000000,4,0x11223344)); /* RTC SLOW is shared */
    CHECK(machine.read(0x50000000,4,value,false) && value==0x11223344);
    /* Direct application entry reproduces the bootloader's guest-visible
     * MMU setup. The SDK allocator must see occupied DROM/IROM pages, not
     * treat mapped constants as free and replace them with flash page0. */
    esp::FlashImage mapped_layout; mapped_layout.chip=esp::CHIP_ESP32;
    mapped_layout.app_index=mapped_layout.boot_index=0;
    esp::Image mapped_image;mapped_image.chip=mapped_layout.chip;
    mapped_image.entry_point=0x400d0020;
    esp::Segment mapped_data;mapped_data.address=0x3f40fff0;mapped_data.file_offset=0x6fff0;
    mapped_data.mapped=true;mapped_data.data.assign(32,'\xa5');
    esp::Segment mapped_code;mapped_code.address=mapped_image.entry_point;mapped_code.file_offset=0x90020;
    mapped_code.mapped=true;mapped_code.data=H("22 a0 2a 00 70 00"); /* MOVI a2,42; WAITI0 */
    mapped_image.segments.push_back(mapped_data);mapped_image.segments.push_back(mapped_code);
    mapped_layout.images.push_back(mapped_image);
    Bytes mapped_flash(0x100000,'\xff');
    mapped_flash.replace(mapped_data.file_offset,mapped_data.data.size(),mapped_data.data);
    mapped_flash.replace(mapped_code.file_offset,mapped_code.data.size(),mapped_code.data);
    CHECK(machine.load(mapped_flash,mapped_layout,false));
    for(unsigned core=0;core<2;++core) {
        const u32 table=0x3ff10000+core*0x2000;
        CHECK(machine.read(table,4,value,false) && value==6);
        CHECK(machine.read(table+4,4,value,false) && value==7);
        CHECK(machine.read(table+77*4,4,value,false) && value==9);
        CHECK(machine.read(table+8,4,value,false) && value==0x100);
    }
    CHECK(machine.read(0x3f40fffe,4,value,false) && value==0xa5a5a5a5);
    CHECK(machine.run(10)==XTENSA_WAIT && machine.cpu().reg(2)==42);
    /* Page tables remain authoritative when the guest deliberately remaps. */
    CHECK(machine.write(0x3ff10000,4,0));
    CHECK(machine.read(0x3f40fff0,4,value,false) && value==0xffffffffu);
    CHECK(machine.load(mapped_flash,mapped_layout,true));
    CHECK(machine.read(0x3ff10000,4,value,false) && value==0x100);
    mapped_layout.images[0].segments[0].file_offset++;
    std::string map_error;
    CHECK(!machine.load(mapped_flash,mapped_layout,false,&map_error) && !map_error.empty());
    mapped_layout.images[0].segments[0].file_offset=0x16fff0;
    CHECK(!machine.load(mapped_flash,mapped_layout,false,&map_error));
    mapped_layout.images[0].segments[0].file_offset=mapped_data.file_offset;
    mapped_data.file_offset=0x8fff0;
    mapped_flash.replace(mapped_data.file_offset,mapped_data.data.size(),mapped_data.data);
    mapped_layout.images[0].segments.push_back(mapped_data);
    CHECK(!machine.load(mapped_flash,mapped_layout,false,&map_error));
#undef CHECK
    return checks;
}
