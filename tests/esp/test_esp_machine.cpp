#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("[FAIL] ESP machine %s:%d: %s\n",__FILE__,__LINE__,#x); return false; } } while(0)

esp::FlashImage image_for(esp::Chip chip,const Bytes &code=Bytes("\xf0\x20\x00",3))
{
    esp::FlashImage layout;layout.chip=chip;layout.boot_index=layout.app_index=0;
    esp::Image image;image.chip=chip;image.checksum_valid=true;
    image.entry_point=chip==esp::CHIP_ESP8266?0x40100000u:0x40080000u;
    esp::Segment segment;segment.address=image.entry_point;segment.data=code;
    image.segments.push_back(segment);layout.images.push_back(image);return layout;
}

bool memory_maps()
{
    const esp::Chip chips[]={esp::CHIP_ESP8266,esp::CHIP_ESP32};
    for(unsigned i=0;i<2;++i) {
        EspMachine m(chips[i]);std::string error;u32 value=0;
        CHECK(m.load(Bytes(0x100000,'\xff'),image_for(chips[i]),false,&error));
        CHECK(m.read(m.cpu().pc(),3,value,true) && value==0x0020f0);
        CHECK(m.write(0x3ffff000,4,0x12345678));
        CHECK(m.read(0x3ffff001,2,value,false) && value==0x3456);
        CHECK(!m.read(0x3ffff000,3,value,true));
        CHECK(!m.write(0xfffffffeu,4,1));
        CHECK(!m.read(0x12345678,4,value,false));
        CHECK(!m.diagnostic().empty());
        CHECK(m.load(Bytes(0x100000,'\xff'),image_for(chips[i]),false,&error));
        CHECK(m.diagnostic().empty() && m.instructions()==0);
        CHECK(m.read(0x3ffff000,4,value,false) && value==0);
    }
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.write(0x3ff80000,4,0xabcdef01));
    CHECK(m.read(0x400c0000,4,value,false) && value==0xabcdef01);
    return true;
}

bool uart_mmio()
{
    const esp::Chip chips[]={esp::CHIP_ESP8266,esp::CHIP_ESP32};
    const u32 bases[]={0x60000000u,0x3ff40000u};
    for(unsigned i=0;i<2;++i) {
        EspMachine m(chips[i]);u32 value;
        CHECK(m.write(bases[i],4,'A'));
        CHECK(m.write(bases[i],4,'\n'));
        CHECK(m.uart()=="A\n");
        m.receive_uart(0,"xyz");
        CHECK(m.read(bases[i]+0x1c,4,value,false) && (value&255)==3);
        CHECK(m.read(bases[i],4,value,false) && value=='x');
        CHECK(m.read(bases[i]+0x1c,4,value,false) && (value&255)==2);
        CHECK(m.write(bases[i]+0x24,4,0x6002));
        CHECK(m.write(bases[i]+0x0c,4,3));
        CHECK(m.read(bases[i]+0x08,4,value,false) && value==3);
    }
    return true;
}

bool rom_memory_and_printf()
{
    const esp::Chip chips[]={esp::CHIP_ESP8266,esp::CHIP_ESP32};
    const u32 copies[]={0x400018b4u,0x4000c2c8u};
    const u32 prints[]={0x400024ccu,0x40007d54u};
    for(unsigned i=0;i<2;++i) {
        EspMachine m(chips[i]);CHECK(m.load(Bytes(0x100000,'\xff'),image_for(chips[i]),false));
        u32 pc=m.cpu().pc(),dst=0x3ffff000,src=0x3ffff100,value;
        CHECK(m.write(src,4,0x64636261));
        m.cpu().set_reg(0,pc);m.cpu().set_reg(2,dst);m.cpu().set_reg(3,src);m.cpu().set_reg(4,4);
        m.cpu().set_pc(copies[i]);
        CHECK(m.step()==XTENSA_OK && m.cpu().pc()==pc && m.cpu().reg(2)==dst);
        CHECK(m.instructions()==1 && m.rom_calls()==1);
        CHECK(m.read(dst,4,value,false) && value==0x64636261);
        const char format[]="test %08x %d %%\n";
        for(unsigned n=0;n<sizeof(format);++n) CHECK(m.write(src+n,1,(u8)format[n]));
        m.cpu().set_reg(0,pc);m.cpu().set_reg(2,src);m.cpu().set_reg(3,0x123a);m.cpu().set_reg(4,(u32)-7);
        m.cpu().set_pc(prints[i]);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.uart()=="test 0000123a -7 %\n");
        CHECK(m.cpu().reg(2)==m.uart().size());
    }
    return true;
}

bool flash_and_unknown_service()
{
    Bytes flash(0x100000,'\xff');flash.replace(0x1234,4,"test");
    EspMachine m(esp::CHIP_ESP8266);CHECK(m.load(flash,image_for(esp::CHIP_ESP8266),false));
    u32 value;CHECK(m.read(0x40201234,4,value,true) && value==0x74736574);
    CHECK(!m.write(0x40201234,4,0));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,0x1234);m.cpu().set_reg(3,0x3ffff000);m.cpu().set_reg(4,4);
    m.cpu().set_pc(0x40004b1c);CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x3ffff000,4,value,false) && value==0x74736574);
    CHECK(m.write(0x3ffff104,4,0x100000));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,0x3ffff100);
    m.cpu().set_reg(3,0x1234);m.cpu().set_reg(4,0x3ffff010);m.cpu().set_reg(5,4);
    m.cpu().set_pc(0x400042ac);CHECK(m.step()==XTENSA_OK && m.cpu().reg(2)==0);
    CHECK(m.read(0x3ffff010,4,value,false) && value==0x74736574);
    CHECK(m.write(0x3ffff104,4,0x1237));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,0x3ffff100);
    m.cpu().set_reg(3,0x1234);m.cpu().set_reg(4,0x3ffff010);m.cpu().set_reg(5,4);
    m.cpu().set_pc(0x400042ac);CHECK(m.step()==XTENSA_OK && m.cpu().reg(2)==1);
    m.cpu().set_pc(0x40001234);CHECK(m.step()==XTENSA_EXCEPTION);
    CHECK(m.diagnostic().find("unsupported ESP8266 ROM service")!=std::string::npos);
    CHECK(m.step()==XTENSA_EXCEPTION);
    CHECK(m.uart().empty());
    return true;
}

bool mapped_image_and_rejection()
{
    EspMachine m(esp::CHIP_ESP32);esp::FlashImage layout=image_for(esp::CHIP_ESP32);
    esp::Segment s;s.address=0x3f400020;s.file_offset=0x20;s.data="hello";s.mapped=true;
    layout.images[0].segments.push_back(s);
    Bytes flash(0x100000,'\xff');flash.replace(s.file_offset,s.data.size(),s.data);
    CHECK(m.load(flash,layout,false));u32 value;
    CHECK(m.read(0x3f400020,4,value,false) && value==0x6c6c6568);
    CHECK(!m.read(0x3f400020,3,value,true));
    CHECK(!m.write(0x3f400020,1,0));
    s.address=0x12340000;s.mapped=false;
    layout.images[0].segments.push_back(s);std::string error;
    CHECK(!m.load(Bytes(),layout,false,&error) && !error.empty());
    CHECK(!m.load(Bytes(),image_for(esp::CHIP_ESP8266),false,&error));
    return true;
}

bool printf_callback_and_stack_arguments()
{
    /* The callback changes every character before writing UART. This proves
     * printf invokes guest code, instead of inferring output from a pointer. */
    const Bytes callback("\x22\xc2\x01\x22\x63\x00\x0d\xf0",8); // addi a2,1; s32i a2,a3,0; ret.n
    EspMachine m(esp::CHIP_ESP8266);
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266,callback),false));
    m.cpu().set_reg(0,0x40100020);m.cpu().set_reg(2,0x40100000);m.cpu().set_pc(0x4000242c);
    CHECK(m.step()==XTENSA_OK && m.cpu().pc()==0x40100020);
    CHECK(m.write(0x3ffff000,4,0x00434241));
    m.cpu().set_reg(0,0x40100020);m.cpu().set_reg(2,0x3ffff000);m.cpu().set_reg(3,0x60000000);
    m.cpu().set_pc(0x400024cc);
    CHECK(m.step()==XTENSA_OK && m.uart().empty());
    unsigned steps=0;
    while(m.cpu().pc()!=0x40100020 && steps++<32) CHECK(m.step()==XTENSA_OK);
    CHECK(steps<32 && m.cpu().reg(2)==3 && m.uart()=="BCD");
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    const char format[]="%u %u %u %u %u %u %u";
    for(unsigned n=0;n<sizeof(format);++n) CHECK(m.write(0x3ffff000+n,1,format[n]));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,0x3ffff000);
    for(unsigned n=1;n<6;++n) m.cpu().set_reg(2+n,n);
    CHECK(m.write(m.cpu().reg(1),4,6));CHECK(m.write(m.cpu().reg(1)+4,4,7));
    m.cpu().set_pc(0x400024cc);CHECK(m.step()==XTENSA_OK);
    CHECK(m.uart()=="1 2 3 4 5 6 7");
    return true;
}

bool banked_flash_and_frequency()
{
    Bytes flash(0x200000,'\xff');flash[0]='A';flash[0x80000]='B';flash[0x100000]='C';
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(flash,image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x3fffc714,4,value,false) && value==0x3fffc718);
    CHECK(m.read(0x3fffc718,4,value,false) && value==0x1540ef);
    CHECK(m.read(0x40200000,1,value,false) && value=='A');
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_reg(3,0);m.cpu().set_pc(0x40004678);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x40200000,1,value,false) && value=='B');
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,0);m.cpu().set_reg(3,1);m.cpu().set_pc(0x40004678);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x40200000,1,value,false) && value=='C');
    m.cpu().set_reg(0,0x40100000);m.cpu().set_pc(0x400047f0);CHECK(m.step()==XTENSA_OK);
    CHECK(!m.read(0x40200000,1,value,false));
    /* ROM delay advances cycles, and a frequency change preserves elapsed
     * wall time rather than rescaling all previously executed instructions. */
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,123);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK && m.elapsed_us()==123);
    CHECK(m.write(0x3ff00014,4,1) && m.elapsed_us()==123);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,10);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK && m.elapsed_us()==133);
    CHECK(m.cpu().special(234)==123*80+10*160);
    CHECK(m.write(0x3ff0000c,4,0x10001));
    CHECK(m.read(0x3ff0000c,4,value,false) && (value&0x303)==3);
    CHECK(!m.read(0x40200000,1,value,false));
    CHECK(m.write(0x3ff0000c,4,0x10100));
    CHECK(m.read(0x3ff0000c,4,value,false) && (value&0x303)==0x100);
    CHECK(m.read(0x40200000,1,value,false) && value=='C');
    CHECK(m.write(0x3ff0000c,4,0x2000100));
    CHECK(m.read(0x40200000,1,value,false) && value=='B');
    return true;
}

bool rtc_alarm_sequence()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60000704,4,150)); // 1ms in the deterministic RC clock model
    CHECK(m.write(0x60000708,4,1u<<20));
    CHECK(m.read(0x60000728,4,value,false) && value==0);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,999);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000728,4,value,false) && value==0);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000728,4,value,false) && value==1);
    CHECK(m.read(0x6000071c,4,value,false) && value==150);
    CHECK(m.read(0x60000708,4,value,false) && !(value&(1u<<20)));
    CHECK(m.write(0x60000724,4,1));
    CHECK(m.read(0x60000728,4,value,false) && value==0);
    CHECK(m.write(0x6000036c,4,150));
    CHECK(m.write(0x6000036c,4,0x80000096u));
    CHECK(m.read(0x60000370,4,value,false) && value==0);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1000);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000370,4,value,false) && value==0x80006590u);
    CHECK(m.xtal_frequency()==26 && !m.set_xtal_frequency(25));
    CHECK(m.set_xtal_frequency(40));
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.xtal_frequency()==40);
    CHECK(m.write(0x6000036c,4,0x80000096u));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1000);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000370,4,value,false) && value==0x80009c40u);
    CHECK(m.set_xtal_frequency(24));
    EspMachine n(esp::CHIP_ESP32);
    CHECK(n.xtal_frequency()==40 && n.set_xtal_frequency(26));
    CHECK(!n.set_xtal_frequency(24));
    return true;
}

bool gpio_registers()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.set_gpio_input(3,1));
    CHECK(m.read(0x60000318,4,value,false) && value==8);
    CHECK(m.set_gpio_input(3,0));
    CHECK(m.read(0x60000318,4,value,false) && value==0);
    CHECK(m.set_gpio_input(16,1));
    CHECK(m.read(0x6000078c,4,value,false) && value==1);
    CHECK(m.set_gpio_input(16,-1));
    CHECK(m.read(0x6000078c,4,value,false) && value==0);
    CHECK(m.write(0x60000304,4,7));
    CHECK(m.write(0x60000308,4,2));
    CHECK(m.read(0x60000300,4,value,false) && value==5);
    CHECK(m.read(0x60000d48,4,value,false) && value==0);
    CHECK(m.write(0x60000d48,4,1));
    CHECK(m.read(0x60000d48,4,value,false) && value==1);
    CHECK(m.write(0x60000d48,4,0));
    CHECK(!m.write(0x60000d48,4,2));
    CHECK(!m.read(0x60000d44,4,value,false));
    CHECK(m.read(0x3ff00058,4,value,false) && (value&0x9000)==0x9000);
    CHECK(!m.write(0x3ff00058,4,0));
    CHECK(m.read(0x3ff00050,4,value,false) && value==0x01000000);
    CHECK(!m.read(0x60009d74,4,value,false));
    CHECK(m.write(0x60009d74,4,0xeab4d027));
    CHECK(m.read(0x60009d74,4,value,false) && value==0xeab4d027);
    CHECK(!m.write(0x60009d78,4,0));
    CHECK(m.read(0x60009800,4,value,false) && value==0);
    CHECK(!m.write(0x60009800,4,0xffffffff));
    CHECK(m.write(0x60000800,4,0x100));
    CHECK(m.read(0x60000800,4,value,false) && value==0x100);
    CHECK(m.write(0x60000834,4,0x80));
    CHECK(m.read(0x60000834,4,value,false) && value==0x80);
    CHECK(!m.read(0x60000844,4,value,false));
    CHECK(m.write(0x60000224,4,0x700000ff));
    CHECK(!m.write(0x60000200,4,1u<<18));
    CHECK(m.diagnostic().find("SPI")!=std::string::npos && m.diagnostic().find("0x60000200")!=std::string::npos);
    CHECK(m.diagnostic().find("write=0x00040000")!=std::string::npos);
    CHECK(m.write(0x60000d08,4,0x015a086a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x015a086a);
    CHECK(m.write(0x60000d08,4,0x0000086a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x005a086a);
    CHECK(!m.write(0x60000d08,4,0x0000096a));
    CHECK(m.diagnostic().find("block 0x6a register 0x09")!=std::string::npos);
    CHECK(m.write(0x60000d10,4,0x01880167));
    CHECK(m.write(0x60000d10,4,0x00000167));
    CHECK(m.read(0x60000d10,4,value,false) && value==0x00880167);
    CHECK(!m.write(0x60000d00,4,0x00000167));
    CHECK(!m.write(0x60000d20,4,0));
    CHECK(m.write(0x60000d10,4,0x00000467));
    CHECK(m.read(0x60000d10,4,value,false) && value==0x00000467);
    CHECK(m.write(0x60000d10,4,0x01930467)); // PLL divider plus I2S gate
    CHECK(m.write(0x60000d10,4,0x00000467));
    CHECK(m.read(0x60000d10,4,value,false) && value==0x00930467);
    CHECK(m.write(0x60000d00,4,0x00000f77));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00000f77);
    CHECK(!m.write(0x60000d00,4,0x00007f77));
    CHECK(m.write(0x60000d00,4,0x00001077));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00001077);
    CHECK(m.write(0x60000d08,4,0x0000046a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000046a);
    CHECK(m.write(0x60000d0c,4,0x00000168));
    CHECK(m.read(0x60000d0c,4,value,false) && value==0x00000168);
    CHECK(m.write(0x60000d04,4,0x00000261));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000261);
    CHECK(m.write(0x60000d0c,4,0x00000166));
    CHECK(m.read(0x60000d0c,4,value,false) && value==0x00000166);
    CHECK(m.read(0x60009b00,4,value,false) && value==0);
    CHECK(m.write(0x60009b00,4,0x800000));
    CHECK(m.read(0x60009b00,4,value,false) && value==0x800000);
    CHECK(m.write(0x60009d60,4,0x01010101));
    CHECK(m.read(0x60009d60,4,value,false) && value==0x01010101);
    CHECK(!m.read(0x60009b04,4,value,false));
    CHECK(m.write(0x600005e8,4,0x800000));
    CHECK(m.read(0x600005e8,4,value,false) && value==0x800000);
    CHECK(!m.read(0x600005ec,4,value,false));
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(2<<2)|(0x181<<5)|(1<<14)));
    CHECK(m.read(0x600005a8,4,value,false) && ((value>>9)&0x1ff)==0x181);
    CHECK(m.read(0x600005a0,4,value,false) && !(value&0x80000000u));
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(3<<2)|(6<<5)|(2<<14)));
    CHECK(m.read(0x600005ac,4,value,false) && ((value>>18)&0x1ff)==6);
    CHECK(m.write(0x60000594,4,1));
    CHECK(!m.write(0x60000594,4,1|2));
    CHECK(!m.write(0x600005a8,4,0)); // readback bank cannot be overwritten directly
    CHECK(m.write(0x60000d04,4,0x00000062));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000062);
    CHECK(m.write(0x60000d08,4,0x0000006c));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000006c);
    CHECK(m.write(0x60000d08,4,0x0000016c));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000016c);
    CHECK(m.write(0x60000d00,4,0x00001a77));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00001a77);
    CHECK(m.write(0x60000d00,4,0x00000464));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00000464);
    CHECK(m.write(0x60000d04,4,0x00000561));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000561);
    CHECK(!m.write(0x60000d04,4,0x00000464));
    CHECK(m.write(0x60009a2c,4,1u<<19));
    CHECK(m.read(0x60009a2c,4,value,false) && value==(1u<<19));
    bool input,output,enabled;
    CHECK(m.gpio_state(0,input,output,enabled) && input && output && !enabled);
    CHECK(m.set_gpio_input(0,0));
    CHECK(m.gpio_state(0,input,output,enabled) && !input);
    CHECK(m.set_gpio_input(0,-1));
    CHECK(m.gpio_state(0,input,output,enabled) && input);
    CHECK(m.write(0x6000030c,4,1));CHECK(m.write(0x60000300,4,0));
    CHECK(m.gpio_state(0,input,output,enabled) && !input && !output && enabled);
    CHECK(m.write(0x60000724,4,0xffffffff));
    CHECK(m.read(0x60000720,4,value,false) && value==0);
    CHECK(m.read(0x60000724,4,value,false) && value==0);
    EspMachine n(esp::CHIP_ESP32);
    CHECK(n.set_gpio_input(39,1));
    CHECK(n.read(0x3ff44040,4,value,false) && value==128);
    CHECK(!n.set_gpio_input(40,1));
    CHECK(n.write(0x3ff44008,4,15));
    CHECK(n.write(0x3ff4400c,4,3));
    CHECK(n.read(0x3ff44004,4,value,false) && value==12);
    CHECK(n.read(0x3ff44038,4,value,false) && value==0x13);
    CHECK(n.set_gpio_input(39,-1));
    CHECK(n.read(0x3ff44040,4,value,false) && value==0);
    CHECK(n.write(0x3ff49044,4,0x100));
    CHECK(n.gpio_state(0,input,output,enabled) && input && !enabled);
    CHECK(n.set_gpio_input(0,0));
    CHECK(n.gpio_state(0,input,output,enabled) && !input);
    CHECK(n.set_gpio_input(0,-1));
    CHECK(n.gpio_state(0,input,output,enabled) && input);
    CHECK(!n.set_gpio_input(28,1));
    return true;
}

bool rom_uart_handoff()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    RomSegment rom;rom.address=rom.physical_address=0x40000000;
    rom.executable=true;rom.data=Bytes(0x10000,'\0');
    rom.data.replace(0x242c,7,H("31 21 fe 29 33 0d f0"));
    rom.data.replace(0x2438,3,H("21 ff ff"));
    wr32le(&rom.data[0x1cb0],0x3fffdd3c);
    wr32le(&rom.data[0x2434],0x40001dcc);
    std::vector<RomSegment> segments(1,rom);
    CHECK(m.load_rom(segments));
    CHECK(m.read(0x3fffdd48,4,value,false) && value==0x40001dcc);
    CHECK(m.cpu().pc()==0x40100000 && m.instructions()==0 && m.uart().empty());
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    segments[0].data[0x242c]=0; // Unknown ROM revision cannot inherit guessed globals.
    CHECK(m.load_rom(segments));
    CHECK(m.read(0x3fffdd48,4,value,false) && value==0);
    return true;
}

bool rfpll_calibration()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000762);
    // Releasing reset without the start edge cannot complete calibration.
    CHECK(m.write(0x60000d04,4,0x01480062));
    CHECK(m.write(0x60000d04,4,0x01080062));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,100);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000762);
    // SDK restart sequence, then observations on each side of the deadline.
    CHECK(m.write(0x60000d00,4,0x015b0363)); // channel1, 26MHz XTAL
    CHECK(m.write(0x60000d00,4,0x01b10463));
    CHECK(m.write(0x60000d00,4,0x013b0563));
    CHECK(m.write(0x60000d04,4,0x01480062));
    CHECK(m.write(0x60000d04,4,0x01680062));
    CHECK(m.write(0x60000d04,4,0x01280062));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,19);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000762);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00800762);
    CHECK(m.write(0x60000d04,4,0x00000562));
    CHECK(m.read(0x60000d04,4,value,false) && ((value>>16)&255)==134);
    CHECK(!m.write(0x60000d04,4,0x01000562));
    CHECK(!m.write(0x60000d04,4,0x01800762));
    CHECK(!m.write(0x60000d00,4,0x00000762)); // Different I2C host is independent.
    CHECK(m.write(0x60000d04,4,0x01680062)); // Reset clears the completion latch.
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000762);
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x00000762);
    return true;
}

bool rf_channel_and_noise()
{
    EspMachine a(esp::CHIP_ESP8266),b(esp::CHIP_ESP8266);u32 x,y,first=0;
    CHECK(a.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(b.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(a.write(0x60009b14,4,0xa9d26000)); // channel-dependent divider/control
    CHECK(a.read(0x60009b14,4,x,false) && x==0xa9d26000);
    CHECK(a.write(0x600098a0,4,0x12345678)); // programmable spur coefficients
    CHECK(a.write(0x600098d4,4,0x400));
    CHECK(a.read(0x600098a0,4,x,false) && x==0x12345678);
    CHECK(a.read(0x600098d4,4,x,false) && x==0x400);
    CHECK(a.write(0x60009b60,4,1));
    CHECK(a.read(0x60009b60,4,x,false) && x==1);
    CHECK(a.write(0x60009b60,4,0));
    CHECK(a.read(0x60009b60,4,x,false) && x==0);
    CHECK(a.write(0x60000d60,4,0x101));
    CHECK(a.read(0x60000d60,4,x,false) && x==0x101);
    CHECK(!a.read(0x60000d64,4,x,false));
    CHECK(a.write(0x6000059c,4,0x01800000));
    CHECK(a.read(0x6000059c,4,x,false) && x==0x01800000);
    CHECK(a.write(0x60000504,4,0x000bf007));
    CHECK(a.write(0x60000560,4,0x0008f013));
    CHECK(a.read(0x60000504,4,x,false) && x==0x000bf007);
    CHECK(a.read(0x60000560,4,x,false) && x==0x0008f013);
    CHECK(a.write(0x60000430,4,0x01234567));
    CHECK(a.read(0x60000430,4,x,false) && x==0x01234567);
    CHECK(a.write(0x600005b8,4,0x71040));
    CHECK(a.write(0x600005bc,4,0x60080));
    CHECK(a.write(0x600005c4,4,0x10040040));
    CHECK(a.read(0x600005b8,4,x,false) && x==0x71040);
    CHECK(a.read(0x600005bc,4,x,false) && x==0x60080);
    CHECK(a.read(0x600005c4,4,x,false) && x==0x10040040);
    CHECK(a.write(0x600005fc,4,0x01020304));
    CHECK(a.read(0x600005fc,4,x,false) && x==0x01020304);
    CHECK(a.write(0x60009860,4,0x010e0000));
    CHECK(a.write(0x6000983c,4,2));
    CHECK(a.read(0x60009860,4,x,false) && x==0x010e0000);
    CHECK(a.read(0x6000983c,4,x,false) && x==2);
    CHECK(a.write(0x60009b68,4,0x5ac64198)); // RX gain/timing configuration
    CHECK(a.write(0x60009b6c,4,0x0914bc81));
    CHECK(a.read(0x60009b68,4,x,false) && x==0x5ac64198);
    CHECK(a.read(0x60009b6c,4,x,false) && x==0x0914bc81);
    CHECK(a.write(0x600005c0,4,1));
    CHECK(a.read(0x600005c0,4,x,false) && x==1);
    CHECK(!a.read(0x600005cc,4,x,false) && !a.read(0x60009b70,4,x,false));
    CHECK(!a.read(0x60000564,4,x,false) && !a.write(0x60000434,4,0));
    CHECK(!a.read(0x600098a4,4,x,false));
    CHECK(!a.write(0x60009830,4,0)); // receiver samples are read-only
    bool changed=false;
    for(unsigned i=0;i<32;++i) {
        CHECK(a.read(0x60009830,4,x,false));
        CHECK(b.read(0x60009830,4,y,false) && x==y);
        CHECK(!(x&0xfff00000u)); // two packed ten-bit samples
        int inphase=int(x&1023),quadrature=int((x>>10)&1023);
        if(inphase&512)inphase-=1024;
        if(quadrature&512)quadrature-=1024;
        CHECK(inphase>=-3 && inphase<=3 && quadrature>=-3 && quadrature<=3);
        if(i==0)first=x;else if(x!=first)changed=true;
    }
    CHECK(changed);
    CHECK(a.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(a.read(0x60009830,4,x,false) && x==first);
    CHECK(a.read(0x60009b14,4,x,false) && x==0);
    return true;
}

bool rfpll_tuning()
{
    // Two divider/XTAL pairs for channel1, plus channel14 and an invalid VCO target.
    const unsigned xtals[]={26,40,26,26};
    const u32 dividers[]={0x5bb13b,0x306666,0x5f6276,0};
    const u32 expected_codes[]={134,134,116,0};
    for(unsigned i=0;i<4;++i) {
        EspMachine m(esp::CHIP_ESP8266);u32 value;
        CHECK(m.set_xtal_frequency(xtals[i]));
        CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
        CHECK(m.write(0x60000d00,4,0x01000363|((dividers[i]>>16)<<16)));
        CHECK(m.write(0x60000d00,4,0x01000463|(((dividers[i]>>8)&255)<<16)));
        CHECK(m.write(0x60000d00,4,0x01000563|((dividers[i]&255)<<16)));
        CHECK(m.write(0x60000d04,4,0x015f0062)); // genuine ROM restart sequence
        CHECK(m.write(0x60000d04,4,0x017f0062));
        CHECK(m.write(0x60000d04,4,0x013f0062));
        m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,20);m.cpu().set_pc(0x40002ecc);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.write(0x60000d04,4,0x00000762));
        CHECK(m.read(0x60000d04,4,value,false) && ((value>>16)&255)==(i==3?0:128));
        CHECK(m.write(0x60000d04,4,0x00000562));
        CHECK(m.read(0x60000d04,4,value,false) && ((value>>16)&255)==expected_codes[i]);
    }
    return true;
}

bool receive_11b_configuration()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x60009820,4,value,false) && value==0);
    // Official libphy phy_set_rx11b_reg uses this exact read/modify/write
    // sequence. Preserve the other receiver fields when toggling 11b mode.
    const u32 original=0xa5a55affu;
    CHECK(m.write(0x60009820,4,original));
    CHECK(m.read(0x60009820,4,value,false) && value==original);
    CHECK(m.write(0x60009820,4,(value&0xfffff030u)|0x48au));
    CHECK(m.read(0x60009820,4,value,false) && value==0xa5a554bau);
    CHECK(m.write(0x60009820,4,value&0xffffff3fu));
    CHECK(m.read(0x60009820,4,value,false) && value==0xa5a5543au);
    CHECK(!m.read(0x6000981c,4,value,false));
    CHECK(!m.write(0x60009828,4,1));
    CHECK(!m.write(0x60009824,4,1)); // receiver power result remains read-only
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x60009820,4,value,false) && value==0);
    return true;
}

bool rc_filter_calibration()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60000d08,4,0x0108066a)); // reference8
    CHECK(m.write(0x60000d08,4,0x0119046a)); // no power or enable, so no measurement
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,100);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d08,4,0x0000056a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000056a);
    CHECK(m.write(0x60000d0c,4,0x01010168));
    CHECK(m.write(0x60000d08,4,0x0140036a));
    CHECK(m.write(0x60000d08,4,0x0111046a));
    CHECK(m.write(0x60000d08,4,0x0119046a));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,99);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d08,4,0x0000056a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000056a);
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d08,4,0x0000056a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0020056a);
    CHECK(!m.write(0x60000d08,4,0x013f056a));
    CHECK(m.write(0x60000d08,4,0x0104066a)); // changing the programmed reference changes the result
    CHECK(m.write(0x60000d08,4,0x0111046a));
    CHECK(m.write(0x60000d08,4,0x0119046a));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,100);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.write(0x60000d08,4,0x0000056a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0010056a);
    CHECK(m.write(0x60000d0c,4,0x01000168));
    CHECK(m.write(0x60000d08,4,0x0000056a));
    CHECK(m.read(0x60000d08,4,value,false) && value==0x0000056a);
    return true;
}

bool dc_comparator_calibration()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    // PBUS0 register2=64, PBUS1 register2=64. The two virtual offsets differ.
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(64<<5)|(2<<14)));
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(1<<2)|(64<<5)|(2<<14)));
    CHECK(m.write(0x60000d4c,4,0xc1113cf1)); // status bits cannot be set by software
    CHECK(m.read(0x60000d4c,4,value,false) && !(value&0xc1000000u));
    CHECK(m.write(0x60000d4c,4,0x01113cf3));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d4c,4,value,false) && !(value&0xc1000000u));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d4c,4,value,false) && (value&0xc1000000u)==0x41000000u);
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(70<<5)|(2<<14)));
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,1|2|(1<<2)|(50<<5)|(2<<14)));
    CHECK(m.read(0x60000d4c,4,value,false) && (value&0xc1000000u)==0x41000000u);
    CHECK(m.write(0x60000d4c,4,0x01113cf1));
    CHECK(m.write(0x60000d4c,4,0x01113cf3));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,2);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d4c,4,value,false) && (value&0xc1000000u)==0x81000000u);
    CHECK(m.write(0x60000d4c,4,0x01113cf0));
    CHECK(m.read(0x60000d4c,4,value,false) && !(value&0xc1000000u));
    CHECK(!m.write(0x60000d4c,4,0x12345678));
    return true;
}

bool sar_supply_conversion()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x60000d50,4,value,false) && value==0x1c); // default eight-sample batch
    CHECK(m.write(0x60000d54,4,0x112233));
    CHECK(m.write(0x60000d58,4,0x445566));
    CHECK(m.read(0x60000d54,4,value,false) && value==0x112233);
    CHECK(m.read(0x60000d58,4,value,false) && value==0x445566);
    CHECK(!m.write(0x60000d80,4,0) && !m.read(0x60000da0,4,value,false));
    CHECK(!m.write(0x60000d50,4,0x1e)); // unpowered ADC is not a successful sample
    CHECK(m.write(0x60000d08,4,0x0120006c)); // SAR power override
    CHECK(m.write(0x60000d08,4,0x0180096b)); // connect supply divider
    CHECK(m.write(0x60000d5c,4,0x800000));
    CHECK(m.write(0x60000d50,4,0x0700001e)); // software cannot supply BUSY/status
    CHECK(m.read(0x60000d50,4,value,false) && (value&0x07000000u)==0x01000000u);
    CHECK(m.read(0x60000d80,4,value,false) && value==0xffffffffu); // old result during conversion
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,23);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d50,4,value,false) && (value&0x07000000u));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d50,4,value,false) && !(value&0x07000000u));
    unsigned sum=0;
    for(unsigned i=0;i<8;++i) {
        CHECK(m.read(0x60000d80+4*i,4,value,false));
        unsigned raw=(~value)&2047,low=raw&255;
        unsigned calibrated=(raw&0x700)+((low>21?(low-21)*279:0)>>8);
        sum+=calibrated;
    }
    CHECK((sum+2)/4>=3378 && (sum+2)/4<=3380); // approximately3.3V in SDK units
    // Changing the input does not alter the held batch; a new single-sample
    // conversion updates only its first slot, preserving the other seven.
    CHECK(m.write(0x60000d08,4,0x0100096b));
    CHECK(m.write(0x60000d5c,4,0x200000)); // external TOUT, grounded by default
    CHECK(m.read(0x60000d80,4,value,false) && ((~value)&2047)>1600);
    CHECK(m.write(0x60000d50,4,0x200)); // divide-by2, one sample
    CHECK(m.write(0x60000d50,4,0x202));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60000d80,4,value,false) && value==0xffffffffu);
    CHECK(m.read(0x60000d84,4,value,false) && ((~value)&2047)>1600);
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x60000d84,4,value,false) && value==0xffffffffu);
    CHECK(m.write(0x60000d04,4,0x0000157c)); // independently retained power gates
    CHECK(m.read(0x60000d04,4,value,false) && value==0x0000157c);
    CHECK(m.write(0x60000d04,4,0x0103157c));
    CHECK(m.write(0x60000d04,4,0x0000157c));
    CHECK(m.read(0x60000d04,4,value,false) && value==0x0003157c);
    CHECK(m.write(0x60000d00,4,0x00000977));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00000977);
    CHECK(m.write(0x60000d00,4,0x01240977));
    CHECK(m.write(0x60000d00,4,0x00000977));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00240977);
    CHECK(m.write(0x60000d00,4,0x00001177)); // signed calibration trim magnitude
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00001177);
    CHECK(m.write(0x60000d00,4,0x01151177));
    CHECK(m.write(0x60000d00,4,0x00001177));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00151177);
    CHECK(m.write(0x60000d00,4,0x00001277)); // receiver calibration override
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00001277);
    CHECK(m.write(0x60000d00,4,0x01a51277));
    CHECK(m.write(0x60000d00,4,0x00001277));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00a51277);
    CHECK(m.write(0x60000d00,4,0x00001977)); // RX IQ magnitude/phase trim
    CHECK(m.read(0x60000d00,4,value,false) && value==0x00001977);
    CHECK(m.write(0x60000d00,4,0x013b1977));
    CHECK(m.write(0x60000d00,4,0x00001977));
    CHECK(m.read(0x60000d00,4,value,false) && value==0x003b1977);
    return true;
}

bool sar_rf_detector()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;unsigned samples[3];
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60000d08,4,0x0108016b)); // nominal first capacitor trim
    CHECK(m.write(0x60000d08,4,0x0148026b)); // nominal second/third trims
    for(unsigned n=0;n<3;++n) {
        CHECK(m.write(0x600005b8,4,0x40000|(n?32:64)));
        if(n==2)CHECK(m.write(0x60000d08,4,0x0100016b)); // detune the filter
        CHECK(m.write(0x60000d50,4,0x1c));
        CHECK(m.write(0x60000d50,4,0x1e)); // internal detector: no external power override
        m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,24);m.cpu().set_pc(0x40002ecc);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(0x60000d84,4,value,false));
        unsigned raw=(~value)&2047,low=raw&255;
        samples[n]=(raw&0x700)+((low>21?(low-21)*279:0)>>8);
        CHECK(m.read(0x60000d98,4,value,false));
        raw=(~value)&2047;low=raw&255;
        unsigned baseline=(raw&0x700)+((low>21?(low-21)*279:0)>>8);
        CHECK(samples[n]>baseline);
        samples[n]-=baseline;
    }
    CHECK(samples[0]>=4*samples[1]-2 && samples[0]<=4*samples[1]+2);
    CHECK(samples[2]<samples[1]);
    CHECK(m.write(0x600005b8,4,0));
    CHECK(m.write(0x60000d50,4,0x1c));
    CHECK(!m.write(0x60000d50,4,0x1e)); // no powered source is not a successful measurement
    return true;
}

bool receive_gain_table()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60009a68,4,0x1e));
    CHECK(m.write(0x60009e00,4,0x11223344));
    CHECK(m.write(0x60009f64,4,0x55667788));
    CHECK(m.write(0x60009a68,4,0x1e0));
    CHECK(m.read(0x60009e00,4,value,false) && value==0);
    CHECK(m.write(0x60009e00,4,0xaabbccdd));
    CHECK(m.read(0x60009e00,4,value,false) && value==0xaabbccdd);
    CHECK(m.write(0x60009a68,4,0x1e));
    CHECK(m.read(0x60009e00,4,value,false) && value==0x11223344);
    CHECK(m.read(0x60009f64,4,value,false) && value==0x55667788);
    CHECK(m.write(0x60009a68,4,2)); // only the low byte write gate
    CHECK(m.write(0x60009e00,4,0xdeadbeef));
    CHECK(m.read(0x60009e00,4,value,false) && value==0x112233ef);
    CHECK(m.write(0x60009a68,4,0));
    CHECK(m.write(0x60009e00,4,0));
    CHECK(m.read(0x60009e00,4,value,false) && value==0x112233ef);
    CHECK(!m.write(0x60009a68,4,0x200));
    CHECK(!m.write(0x60009f68,4,0));
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x60009e00,4,value,false) && value==0);
    return true;
}

bool iq_dc_estimator()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.read(0x6000057c,4,value,false) && value==0);
    for(unsigned block=4;block<=5;++block)for(unsigned reg=1;reg<=2;++reg) {
        CHECK(m.write(0x60000594,4,1));
        CHECK(m.write(0x60000594,4,3|(block<<2)|(256<<5)|(reg<<14)));
    }
    CHECK(m.write(0x6000057c,4,0x80000001u)); // DONE cannot be asserted by software
    CHECK(m.read(0x6000057c,4,value,false) && value==1);
    CHECK(m.write(0x6000057c,4,0x40003u|(79<<2))); // eighty baseband samples
    CHECK(m.read(0x6000057c,4,value,false) && !(value&0x80000000u));
    CHECK(m.write(0x60000594,4,1));
    CHECK(m.write(0x60000594,4,3|(4<<2)|(270<<5)|(1<<14)));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,3);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x6000057c,4,value,false) && !(value&0x80000000u));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x6000057c,4,value,false) && (value&0x80000000u));
    CHECK(m.read(0x600005dc,4,value,false) && value==u32(-4*80*64));
    CHECK(m.read(0x600005e0,4,value,false) && value==3*80*64);
    CHECK(m.read(0x600005e4,4,value,false) && value==(4*4+3*3+8)*80);
    CHECK(!m.write(0x600005e4,4,0));
    CHECK(!m.write(0x600005dc,4,0));
    CHECK(m.write(0x6000057c,4,1));
    CHECK(m.write(0x6000057c,4,0x40003u|(19<<2))); // new batch samples the changed trim
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x600005dc,4,value,false) && value==10*20*64);
    CHECK(m.write(0x6000057c,4,0));
    CHECK(m.read(0x6000057c,4,value,false) && value==0);
    CHECK(m.write(0x6000057c,4,0x40002)); // disabled power cannot complete a batch
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,10);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x6000057c,4,value,false) && !(value&0x80000000u));
    return true;
}

bool iq_correlation()
{
    EspMachine m(esp::CHIP_ESP8266);u32 ii,iq,qi,qq;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x600005b8,4,0x40008));
    for(unsigned pass=0;pass<3;++pass) {
        if(pass==1) {
            CHECK(m.write(0x60000d00,4,0x01e21977)); // gain+2, phase-3
            CHECK(m.write(0x60000d00,4,0x01051a77));
        }
        if(pass==2)CHECK(m.write(0x600005b8,4,0));
        CHECK(m.write(0x6000057c,4,1));
        CHECK(m.write(0x6000057c,4,0x40003u|(79<<2)));
        m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,4);m.cpu().set_pc(0x40002ecc);
        CHECK(m.step()==XTENSA_OK);
        CHECK(m.read(0x60000580,4,ii,false));
        CHECK(m.read(0x60000584,4,iq,false));
        CHECK(m.read(0x60000588,4,qi,false));
        CHECK(m.read(0x6000058c,4,qq,false));
        if(pass==0)CHECK(ii>qq && iq==qi && iq>0);
        if(pass==1)CHECK(ii==qq && ii>0 && iq==0 && qi==0);
        if(pass==2)CHECK(ii==0 && qq==0 && iq==0 && qi==0);
    }
    CHECK(!m.write(0x60000580,4,123));
    CHECK(!m.read(0x60000578,4,ii,false));
    return true;
}

bool mac_nmi_routing()
{
    // RTOS Wi-Fi startup selects DPORT0=1 and masks normal WDEV IRQ0.
    // Its real NMI vector calls wDev_ProcessFiq and rearms DPORT0 before RFI3.
    Bytes code(0x100,'\0');code.replace(0,3,"\x00\x70\x00",3); // WAITI 0
    code.replace(0x20,2,"\x0c\x72",2); // MOVI.N a2,7
    code.replace(0x22,3,"\x10\x33\x00",3); // RFI 3
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266,code),false));
    m.cpu().set_special(XT_SR_VECBASE,0x40100000);
    m.cpu().set_special(XT_SR_INTENABLE,0);
    CHECK(m.write(0x3ff00000,4,1));
    CHECK(m.write(0x3ff21058,4,0x80000000));
    CHECK(m.write(0x3ff210cc,4,5) && m.write(0x3ff210d0,4,0));
    CHECK(m.write(0x3ff210c8,4,0x80000000));
    CHECK(m.write(0x3ff20c18,4,1u<<26));
    CHECK(m.step()==XTENSA_OK && m.cpu().pc()==0x40100020);
    CHECK(m.elapsed_us()==5 && m.cpu().special(XT_SR_EPC1+2)==0x40100003);
    CHECK(m.cpu().special(XT_SR_INTENABLE)==0); // NMI ignores INTENABLE
    CHECK(m.step()==XTENSA_OK && m.cpu().reg(2)==7);
    CHECK(m.write(0x3ff20c18,4,0)); // no stale NMI after peripheral masking
    CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&(1u<<14)));
    CHECK(m.read(0x3ff20c1c,4,value,false) && value==(1u<<26));
    CHECK(m.write(0x3ff00000,4,0));
    CHECK(m.write(0x3ff20c18,4,1u<<26));
    CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&(1u<<14)));
    CHECK(m.cpu().special(XT_SR_INTERRUPT)&1); // native MAC level stays observable
    CHECK(m.step()==XTENSA_OK && m.cpu().pc()==0x40100003);
    CHECK(m.write(0x3ff00000,4,1)); // rearming a still-pending source delivers it
    CHECK(m.step()==XTENSA_OK && m.cpu().reg(2)==7 && m.cpu().pc()==0x40100022);
    CHECK(m.write(0x3ff20c24,4,1u<<26));
    CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&((1u<<14)|1)));
    CHECK(m.step()==XTENSA_OK && m.cpu().pc()==0x40100003);
    CHECK(m.write(0x3ff00000,4,0x100)); // unrelated DPORT bits do not enable NMI
    CHECK(m.read(0x3ff00000,4,value,false) && value==0x100);
    return true;
}

bool receiver_noise_measurement()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value,quiet;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.write(0x60009b64,4,0xfff001a0));
    CHECK(m.read(0x60009b64,4,value,false) && value==0x1a0); // measurement is read-only
    CHECK(m.write(0x60009b60,4,0x28002));
    CHECK(m.read(0x60009b60,4,value,false) && (value&2));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,6);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60009b60,4,value,false) && (value&2));
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,1);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60009b60,4,value,false) && !(value&2));
    CHECK(m.read(0x60009b64,4,value,false) && (value&0xfffff)==0x1a0);
    quiet=value>>20;
    CHECK(quiet>3312 && quiet<3344); // virtual idle receiver around-96dBm
    CHECK(m.read(0x60009824,4,value,false) && value==quiet);
    CHECK(!m.write(0x60009824,4,0));
    CHECK(m.write(0x600005b8,4,0x40040)); // an enabled tone raises measured noise power
    CHECK(m.write(0x60009b60,4,0x28002));
    CHECK(m.write(0x600005b8,4,0)); // capture is already in progress
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,7);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60009824,4,value,false) && value>quiet+100);
    CHECK(m.write(0x60009b60,4,0x28002));
    CHECK(m.write(0x60009b60,4,0)); // cancellation retains the preceding result
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,20);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    CHECK(m.read(0x60009824,4,value,false) && value>quiet+100);
    return true;
}

bool receiver_frequency_acknowledgement()
{
    EspMachine m(esp::CHIP_ESP8266);u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    // libphy phy_get_bb_freqoffset consumes9800 only when bit0 is valid,
    // then always performs read/OR15/write at98dc, even for unavailable data.
    CHECK(m.read(0x60009800,4,value,false) && !(value&1));
    CHECK(m.read(0x600098dc,4,value,false) && value==0);
    CHECK(m.write(0x600098dc,4,value|15));
    CHECK(m.read(0x60009800,4,value,false) && value==0);
    CHECK(m.read(0x600098dc,4,value,false) && !(value&~15u));
    CHECK(m.write(0x600098dc,4,value|15)); // repeated unavailable sample
    CHECK(m.write(0x600098dc,4,0));
    CHECK(m.read(0x60009800,4,value,false) && value==0);
    CHECK(!m.write(0x600098dc,4,1)); // individual clear-bit meanings are unknown
    CHECK(!m.write(0x600098dc,4,0x10)); // no inferred adjacent control fields
    CHECK(!m.write(0x600098dc,4,0xffffffff));
    CHECK(!m.write(0x60009800,4,1)); // guest cannot manufacture a valid sample
    CHECK(!m.read(0x600098d8,4,value,false));
    CHECK(!m.read(0x600098e0,4,value,false));
    CHECK(!m.write(0x600098d8,4,15));
    CHECK(!m.write(0x600098e0,4,15));
    return true;
}

bool radio_snapshot()
{
    EspMachine m(esp::CHIP_ESP8266);EspMachine::WifiRadioState state;u32 value;
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.wifi_radio_state(state) && !state.channel && !state.tx_ready());
    CHECK(m.write(0x60000d00,4,0x01070063));
    CHECK(m.write(0x60000d00,4,0x015b0363));
    CHECK(m.write(0x60000d00,4,0x01b10463));
    CHECK(m.write(0x60000d00,4,0x013b0563));
    CHECK(m.write(0x60000d00,4,0x01170063));
    CHECK(m.wifi_radio_state(state) && state.channel==1 && state.tx_ready());
    CHECK(m.write(0x60009a2c,4,1u<<19));
    CHECK(m.write(0x3ff20004,4,1u<<31));
    CHECK(m.wifi_radio_state(state) && state.rx_ready());
    const u64 generation=state.generation;
    CHECK(m.write(0x60009b08,4,1u<<27));
    CHECK(m.write(0x60009b08,4,0));
    CHECK(m.wifi_radio_state(state) && state.rx_ready() && state.generation>generation);
    CHECK(m.write(0x60000d04,4,0x01480062));
    CHECK(m.write(0x60000d04,4,0x01680062));
    CHECK(m.write(0x60000d04,4,0x01280062));
    CHECK(m.wifi_radio_state(state) && state.calibration_pending && !state.rx_ready());
    m.cpu().set_reg(0,0x40100000);m.cpu().set_reg(2,20);m.cpu().set_pc(0x40002ecc);
    CHECK(m.step()==XTENSA_OK);
    // No guest LOCK/result read is needed for the adapter to see completion.
    CHECK(m.wifi_radio_state(state) && !state.calibration_pending && state.rx_ready());
    CHECK(m.write(0x60000d04,4,0x00000762));
    CHECK(m.read(0x60000d04,4,value,false) && ((value>>16)&255)==128);
    CHECK(m.load(Bytes(0x100000,'\xff'),image_for(esp::CHIP_ESP8266),false));
    CHECK(m.wifi_radio_state(state) && !state.generation && !state.channel && !state.rx_ready());
    EspMachine esp32(esp::CHIP_ESP32);
    CHECK(!esp32.wifi_radio_state(state) && !state.channel && !state.rx_ready());
    return true;
}
}

int run_esp_machine_tests()
{
    checks=0;
    if(!memory_maps() || !uart_mmio() || !rom_memory_and_printf() ||
       !flash_and_unknown_service() || !mapped_image_and_rejection() || !gpio_registers() ||
       !printf_callback_and_stack_arguments() || !banked_flash_and_frequency() || !rtc_alarm_sequence() ||
       !rom_uart_handoff() || !rfpll_calibration() || !rf_channel_and_noise() || !rfpll_tuning() ||
       !rc_filter_calibration() || !dc_comparator_calibration() || !sar_supply_conversion() ||
       !sar_rf_detector() || !receive_gain_table() || !iq_dc_estimator() || !iq_correlation() ||
       !receiver_noise_measurement() || !receiver_frequency_acknowledgement() ||
       !receive_11b_configuration() || !mac_nmi_routing() || !radio_snapshot()) return -1;
    return checks;
}
