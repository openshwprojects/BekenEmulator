#include "rom_image.h"
#include "esp_machine.h"
#include <stdio.h>

namespace {
Bytes elf_fixture()
{
    Bytes b(88, '\0');
    b.replace(0, 7, "\x7f" "ELF\x01\x01\x01", 7);
    wr16le(&b[16], 2); wr16le(&b[18], 94); wr32le(&b[20], 1);
    wr32le(&b[28], 52); wr16le(&b[40], 52); wr16le(&b[42], 32); wr16le(&b[44], 1);
    wr32le(&b[52], 1); wr32le(&b[56], 84);
    wr32le(&b[60], 0x40000000); wr32le(&b[64], 0x40000000);
    wr32le(&b[68], 4); wr32le(&b[72], 4); wr32le(&b[76], 5);
    b.replace(84, 4, "ROM!");
    return b;
}
Bytes elf_with_initializer()
{
    Bytes b = elf_fixture(); b.resize(0x200 + 3 * 40, '\0');
    wr32le(&b[32], 0x200); wr16le(&b[46], 40); wr16le(&b[48], 3); wr16le(&b[50], 1);
    b.replace(0x100, 22, "\0.shstrtab\0.data_test\0", 22);
    b.replace(0x120, 4, "INIT");
    wr32le(&b[0x228], 1); wr32le(&b[0x22c], 3);
    wr32le(&b[0x238], 0x100); wr32le(&b[0x23c], 22);
    wr32le(&b[0x250], 11); wr32le(&b[0x254], 1);
    wr32le(&b[0x25c], 0x3ffae030); wr32le(&b[0x260], 0x120); wr32le(&b[0x264], 4);
    return b;
}
}

int rom_image_tests()
{
    int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { printf("[FAIL] ROM ELF line %d: %s\n", __LINE__, #x); return -1; } } while (0)
    std::vector<RomSegment> segments;
    std::string error;
    Bytes b = elf_fixture();
    CHECK(parse_elf_rom(b, 94, segments, &error));
    CHECK(segments.size() == 1 && segments[0].data == "ROM!" && segments[0].executable);
    CHECK(segments[0].address == 0x40000000 && segments[0].physical_address == 0x40000000);
    CHECK(!parse_elf_rom(b, 40, segments, &error) && segments.empty());
    for (size_t size = 0; size < b.size(); ++size)
        CHECK(!parse_elf_rom(b.substr(0, size), 94, segments));
    b[5] = 2; CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr32le(&b[68], 5); CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr32le(&b[60], 0xfffffffe); CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr32le(&b[64], 0xfffffffe); CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr16le(&b[42], 0); CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr32le(&b[76], 6); CHECK(!parse_elf_rom(b, 94, segments));
    b = elf_fixture(); wr32le(&b[64], 0x3ff96000); wr32le(&b[76], 4);
    error = "stale";
    CHECK(parse_elf_rom(b, 94, segments, &error));
    CHECK(!segments[0].executable && segments[0].physical_address == 0x3ff96000);
    CHECK(error.empty());
    b = elf_with_initializer();
    CHECK(parse_elf_rom(b, 94, segments, &error));
    CHECK(segments.size() == 2 && segments[1].initialize_ram && segments[1].data == "INIT");
    CHECK(segments[1].address == 0x3ffae030);
    wr32le(&b[0x264], 0xfffffffe);
    CHECK(!parse_elf_rom(b, 94, segments) && segments.empty());
    b = elf_with_initializer(); wr32le(&b[0x250], 22);
    CHECK(!parse_elf_rom(b, 94, segments));
    esp::FlashImage layout;
    layout.chip = esp::CHIP_ESP32;
    layout.app_index = layout.boot_index = 0;
    esp::Image image;
    image.chip = esp::CHIP_ESP32; image.entry_point = 0x40080000;
    esp::Segment code;
    code.address = image.entry_point; code.data.assign("\xf0\x20\x00", 3);
    image.segments.push_back(code); layout.images.push_back(image);
    EspMachine machine(esp::CHIP_ESP32);
    CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
    RomSegment rom;
    rom.address = rom.physical_address = 0x40000500;
    rom.executable = true;
    rom.data.assign("\x22\xa0\x07\x0d\xf0", 5); /* movi a2,7; ret.n */
    segments.clear(); segments.push_back(rom);
    RomSegment data;
    data.address = 0x3ff96000; data.physical_address = 0x40066000;
    data.data = "ROM!"; segments.push_back(data);
    RomSegment initializer;
    initializer.address = initializer.physical_address = 0x3ffae030;
    initializer.initialize_ram = true; initializer.data = "INIT";
    segments.push_back(initializer);
    CHECK(machine.load_rom(segments, &error));
    u32 value;
    CHECK(machine.read(0x3ffae030, 4, value, false) && value == 0x54494e49);
    CHECK(machine.write(0x3ffae030, 4, 0)); /* RAM remains mutable. */
    CHECK(machine.read(0x3ff96000, 4, value, false) && value == 0x214d4f52);
    CHECK(machine.read(0x40066000, 4, value, false) && value == 0x214d4f52);
    CHECK(!machine.write(0x40000500, 1, 0));
    CHECK(!machine.read(0x3ff96000, 1, value, true));
    machine.cpu().set_reg(0, image.entry_point);
    machine.cpu().set_pc(rom.address);
    CHECK(machine.step() == XTENSA_OK && machine.cpu().reg(2) == 7);
    CHECK(machine.step() == XTENSA_OK && machine.cpu().pc() == image.entry_point);
    CHECK(machine.rom_calls() == 0); /* Actual ROM code, no host replacement. */
    CHECK(!machine.load_rom(segments, &error)); /* Overlap is rejected. */
    CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
    segments[1].address = 0x12345678;
    CHECK(!machine.load_rom(segments, &error));
    CHECK(!machine.read(rom.address, 1, value, true)); /* Rejection was atomic. */
    /* Eleven nested CALL8 frames overflow the physical register file, then
     * RETW restores them through the real Xtensa ABI spill/fill sequences.
     * The image entry itself must have a valid caller frame from the loader. */
    layout.images[0].segments[0].data = H(
        "36 81 00 a2 a0 0a a5 00 00 a0 2a 20 00 70 00 00"
        "36 81 00 16 a2 00 a2 c2 ff 65 ff ff 22 ca 01 1d f0 22 a0 07 1d f0");
    CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
    rom.address = rom.physical_address = 0x40000000;
    rom.data.assign(0x100, '\0');
    rom.data.replace(0x80, 30, H("00 c9 49 00 d1 09 10 d9 49 20 e9 49 30 f9 49"
        "40 80 49 50 90 49 60 a0 49 70 b0 49 00 34 00"));
    rom.data.replace(0xc0, 30, H("00 c9 09 10 d9 09 20 e9 09 70 d1 09 30 f9 09"
        "40 87 09 50 97 09 60 a7 09 70 b7 09 00 35 00"));
    segments.clear(); segments.push_back(rom);
    CHECK(machine.load_rom(segments, &error));
    CHECK(machine.run(1000) == XTENSA_WAIT);
    CHECK(machine.cpu().reg(2) == 17);
    CHECK(machine.cpu().reg(1) == 0x3ffe3ee0);
    CHECK(machine.rom_calls() == 0 && machine.instructions() > 100);
    /* Published LX6 ELFs keep ROM-library initializer bytes in RAM sections,
     * but the real reset code also reads their original ROM source addresses.
     * Empty entries alias a later section and still read one source word. */
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    rom.address=rom.physical_address=0x40000400;rom.data.assign(0x200,'\0');
    wr32le(&rom.data[0x24],0x40000580);wr32le(&rom.data[0x28],0x400005a0);
    rom.data.replace(0xfb,6,H("41 ca ff 51 ca ff"));
    wr32le(&rom.data[0x180],0x3ffe0010);wr32le(&rom.data[0x184],0x3ffe0010);
    wr32le(&rom.data[0x188],0x4000f0d0);wr32le(&rom.data[0x18c],1);
    wr32le(&rom.data[0x190],0x3ffe01e0);wr32le(&rom.data[0x194],0x3ffe01e4);
    wr32le(&rom.data[0x198],0x4000f0d0);wr32le(&rom.data[0x19c],1);
    initializer.address=initializer.physical_address=0x3ffe01e0;initializer.data="ROM!PAD!";
    segments.clear();segments.push_back(rom);segments.push_back(initializer);
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.read(0x4000f0d0,4,value,false) && value==0x214d4f52);
    CHECK(machine.read(0x4000f0d4,4,value,false) && value==0x21444150);
    CHECK(!machine.write(0x4000f0d0,4,0));
    CHECK(!machine.read(0x4000f0d0,1,value,true));
    CHECK(machine.read(0x3ffe01e0,4,value,false) && value==0x214d4f52);
    CHECK(machine.write(0x3ffe01e0,4,0));
    CHECK(machine.read(0x4000f0d0,4,value,false) && value==0x214d4f52);
    /* Malformed table bounds are rejected before mapping any ROM bytes. */
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    wr32le(&segments[0].data[0x28],0x400005a1);
    CHECK(!machine.load_rom(segments,&error));
    CHECK(!machine.read(0x400004fb,1,value,true));
    /* Classic mask ROM has a distinct APP view of private-library pointers.
     * Execute guest loads on APP, rather than inspecting internal regions. */
    layout.images[0].segments[0].data.assign(0x120,'\0');
    layout.images[0].segments[0].data.replace(0,3,H("00 70 00"));
    layout.images[0].segments[0].data.replace(0x100,15,H("22 23 00 22 64 00 52 26 00 52 67 00 00 70 00"));
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    rom.address=rom.physical_address=0x40000000;rom.data.assign(0x10000,'\0');
    rom.data.replace(0x4fb,6,H("41 ca ff 51 ca ff"));
    wr32le(&rom.data[0x424],0x4000d4f8);wr32le(&rom.data[0x428],0x4000d5c8);
    wr32le(&rom.data[0x560],0x3ffe3f20);wr32le(&rom.data[0x580],0x3ffe0548);
    wr32le(&rom.data[0x584],0x3ffe0448);
    for(unsigned p=0xd4f8;p<0xd5c8;p+=16) {
        wr32le(&rom.data[p],0x3ffae010);wr32le(&rom.data[p+4],0x3ffae010);
        wr32le(&rom.data[p+8],0x4000d670);
    }
    wr32le(&rom.data[0xd528],0x3ffe0010);wr32le(&rom.data[0xd52c],0x3ffe0010);
    wr32le(&rom.data[0xd534],1);
    segments.clear();segments.push_back(rom);
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.read(0x40000560,4,value,false) && value==0x3ffe3f20);
    CHECK(machine.write(0x3ff00030,4,1) && machine.write(0x3ff0002c,4,0));
    machine.cpu(1).set_pc(0x40080100);
    machine.cpu(1).set_reg(3,0x40000560);machine.cpu(1).set_reg(4,0x3ffb1000);
    machine.cpu(1).set_reg(6,0x4000d528);machine.cpu(1).set_reg(7,0x3ffb1004);
    CHECK(machine.run(20)==XTENSA_WAIT);
    CHECK(machine.read(0x3ffb1000,4,value,false) && value==0x3ffe7e30);
    CHECK(machine.read(0x3ffb1004,4,value,false) && value==0x3ffe3f20);
    CHECK(machine.cpu(0).instructions()==1 && machine.cpu(1).instructions()==5 && machine.rom_calls()==0);
    CHECK(!machine.write(0x40000560,4,0));
    CHECK(!machine.load_rom(segments,&error));
    /* An arbitrary ROM with the same pointer is not automatically relocated. */
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    segments[0].data[0x4fb]=0;
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.write(0x3ff00030,4,1) && machine.write(0x3ff0002c,4,0));
    machine.cpu(1).set_pc(0x40080100);
    machine.cpu(1).set_reg(3,0x40000560);machine.cpu(1).set_reg(4,0x3ffb1000);
    machine.cpu(1).set_reg(6,0x4000d528);machine.cpu(1).set_reg(7,0x3ffb1004);
    CHECK(machine.run(20)==XTENSA_WAIT);
    CHECK(machine.read(0x3ffb1000,4,value,false) && value==0x3ffe3f20);
    /* LX106 mask ROM supplies a reset copy table rather than ELF .data
     * metadata. Its end pointer need not be word aligned. */
    EspMachine lx106(esp::CHIP_ESP8266);
    layout.chip = layout.images[0].chip = esp::CHIP_ESP8266;
    layout.images[0].entry_point = layout.images[0].segments[0].address = 0x40100000;
    layout.images[0].segments[0].data = H("00 70 00");
    CHECK(lx106.load(Bytes(0x100000, (char)0xff), layout, false));
    rom.address=rom.physical_address=0x40000000;
    rom.data.assign(0x200, '\0');
    rom.data.replace(0x80, 3, H("06 08 00"));
    wr32le(&rom.data[0x98], 0x40000100);
    wr32le(&rom.data[0x100], 0x3fffc000);
    wr32le(&rom.data[0x104], 0x3fffc005);
    wr32le(&rom.data[0x108], 0x40000180);
    wr32le(&rom.data[0x10c], 0x3fffc008);
    wr32le(&rom.data[0x110], 0x3fffc008); /* empty section, followed by terminator */
    wr32le(&rom.data[0x114], 0x40000188);
    rom.data.replace(0x180, 8, "ROMSTATE");
    segments.clear(); segments.push_back(rom);
    CHECK(lx106.load_rom(segments, &error));
    CHECK(lx106.read(0x3fffc000, 4, value, false) && value == 0x534d4f52);
    CHECK(lx106.read(0x3fffc004, 4, value, false) && value == 0x45544154);
    CHECK(lx106.read(0x3fffc714, 4, value, false) && value == 0x3fffc718);
    CHECK(lx106.read(0x3fffc71c, 4, value, false) && value == 0x100000);
    CHECK(lx106.load(Bytes(0x100000, (char)0xff), layout, false));
    wr32le(&segments[0].data[0x108], 0x40030000);
    CHECK(!lx106.load_rom(segments, &error));
    CHECK(!lx106.read(0x40000080, 1, value, true));
    CHECK(lx106.read(0x3fffc000, 4, value, false) && value == 0); /* atomic failure */
    Bytes raw=rom.data;raw.resize(0x10000,'\0');
    raw.replace(0x1c0,6,H("22 a0 2a 00 70 00")); /* actual MOVI/WAIT in raw ROM */
    CHECK(lx106.load_rom(raw,&error));
    CHECK(lx106.read(0x3fffc000,4,value,false) && value==0x534d4f52);
    lx106.cpu().set_pc(0x400001c0);
    CHECK(lx106.run(10)==XTENSA_WAIT && lx106.cpu().reg(2)==42);
    CHECK(lx106.rom_calls()==0);
    CHECK(lx106.load(Bytes(0x100000,(char)0xff),layout,false));
    CHECK(!lx106.load_rom(raw.substr(0,0xffff),&error));
    raw[0x80]=0;
    CHECK(!lx106.load_rom(raw,&error) && contains(error,"reset vector"));
    CHECK(!lx106.read(0x400001c0,1,value,true));
    /* Flash entry skips the ROM's clock handoff. Recognize the real updater
     * and use its literal, even when the test relocates the RAM variable. */
    layout.chip=esp::CHIP_ESP32;layout.images[0].chip=esp::CHIP_ESP32;
    layout.images[0].entry_point=layout.images[0].segments[0].address=0x40080000;
    layout.images[0].segments[0].data=H("00 70 00");
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    rom.address=rom.physical_address=0x40008530;rom.executable=true;
    rom.data.assign(0x30,'\0');wr32le(&rom.data[0],0x3ffb1200);
    rom.data.replace(0x20,10,H("36 41 00 81 f7 ff 29 08 1d f0"));
    initializer.address=initializer.physical_address=0x3ffb1200;
    initializer.data.assign(8,'\0');wr32le(&initializer.data[0],13);wr32le(&initializer.data[4],13);
    segments.clear();segments.push_back(rom);segments.push_back(initializer);
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.read(0x3ffb1200,4,value,false) && value==80);
    CHECK(machine.read(0x3ffb1204,4,value,false) && value==13); // Only the recognized PRO variable changes.
    CHECK(machine.rom_calls()==0);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,true));
    segments[0].data[0x26]=0; // Different updater body must not modify RAM.
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.read(0x3ffb1200,4,value,false) && value==13);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,true));
    segments[0]=rom;
    CHECK(machine.load_rom(segments,&error));
    CHECK(machine.read(0x3ffb1200,4,value,false) && value==80); // Second-stage bootloader handoff too.
    EspMachine lx6(esp::CHIP_ESP32);
    CHECK(!lx6.load_rom(raw,&error));
#undef CHECK
    return checks;
}
