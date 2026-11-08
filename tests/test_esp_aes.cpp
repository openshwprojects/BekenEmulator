#include "esp_machine.h"
#include "esp_aes.h"
#include "esp_psram.h"
#include <stdio.h>

namespace {
bool advance(EspMachine &machine,unsigned clocks) {
    for(unsigned i=0;i<clocks;++i) {
        machine.cpu().set_pc(0x40080000);
        if(machine.step()!=XTENSA_OK)return false;
    }
    return true;
}
bool write_words(EspMachine &machine,u32 base,const Bytes &bytes,unsigned order) {
    const unsigned words=(unsigned)bytes.size()/4;
    for(unsigned i=0;i<words;++i) {
        const unsigned index=(order&2)?i:words-i-1;
        const u32 value=(order&1)?rd32le(bytes.data()+i*4):rd32be(bytes.data()+i*4);
        if(!machine.write(base+index*4,4,value))return false;
    }
    return true;
}
bool read_words(EspMachine &machine,u32 base,const Bytes &expected,unsigned order) {
    const unsigned words=(unsigned)expected.size()/4;
    for(unsigned i=0;i<words;++i) {
        const unsigned index=(order&2)?i:words-i-1;
        const u32 expected_word=(order&1)?rd32le(expected.data()+i*4):rd32be(expected.data()+i*4);
        u32 value;if(!machine.read(base+index*4,4,value,false) || value!=expected_word)return false;
    }
    return true;
}
}

int esp_aes_tests()
{
    int checks=0;
#define CHECK(x) do {++checks;if(!(x)){printf("[FAIL] ESP AES line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    EspMachine machine(esp::CHIP_ESP32);u32 value;
    CHECK(machine.write(0x40080000,4,0x0020f0));
    CHECK(machine.read(0x3ff01004,4,value,false) && value==1);
    CHECK(machine.read(0x3ff01040,4,value,false) && value==63);
    CHECK(!machine.write(0x3ff01008,4,0)); /* clock disabled */
    CHECK(machine.write(0x3ff0001c,4,1));
    CHECK(!machine.read(0x3ff0100c,4,value,false));
    CHECK(!machine.read(0x3ff01000,4,value,false)); /* START is write-only */
    CHECK(!machine.write(0x3ff01004,4,1)); /* IDLE is read-only */

    /* Crypto and an external RAM chip share the bus state map. Resetting
     * AES must never change QPI, burst-wrap or the RAM reset-enable latch. */
    std::map<u32,u32> devices;Bytes ram(0x200000,0),reply;std::string error;
    CHECK(esp_psram::command(devices,ram,0xc0,0,false,Bytes(),0,reply));
    CHECK(esp_psram::command(devices,ram,0x35,0,false,Bytes(),0,reply));
    CHECK(esp_aes::read(devices,0x3ff01004,0,value) && value==1);
    devices[0x3ff0001c]=1;
    CHECK(esp_aes::write(devices,0x3ff01000,0,1,error));
    esp_aes::synchronize(devices,32);
    CHECK(esp_psram::qpi_enabled(devices));
    esp_aes::reset(devices);
    CHECK(esp_psram::qpi_enabled(devices));
    CHECK(esp_psram::command(devices,ram,0x38,31,true,Bytes("abcd"),0,reply));
    CHECK(ram[31]=='a' && ram.substr(0,3)=="bcd");
    CHECK(esp_psram::command(devices,ram,0x66,0,true,Bytes(),0,reply));
    esp_aes::reset(devices);
    CHECK(esp_psram::command(devices,ram,0x99,0,true,Bytes(),0,reply));
    CHECK(!esp_psram::qpi_enabled(devices));

    /* FIPS197 Appendix C known-answer vectors, all key lengths and both
     * directions. Independently vary all three endianness fields from TRM14.3. */
    const Bytes key=H("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const Bytes plain=H("00112233445566778899aabbccddeeff");
    const Bytes encrypted[]={H("69c4e0d86a7b0430d8cdb78070b4c55a"),
        H("dda97ca4864cdfe06eaf70a0ec0d7191"),H("8ea2b7ca516745bfeafc49904b496089")};
    for(unsigned length=0;length<3;++length)for(unsigned decrypt=0;decrypt<2;++decrypt)
        for(unsigned endian=0;endian<64;++endian) {
            CHECK(machine.write(0x3ff01008,4,length+(decrypt?4:0)));
            CHECK(machine.write(0x3ff01040,4,endian));
            CHECK(write_words(machine,0x3ff01010,key.substr(0,16+length*8),endian&3));
            CHECK(write_words(machine,0x3ff01030,decrypt?encrypted[length]:plain,(endian>>2)&3));
            CHECK(machine.write(0x3ff01000,4,1));
            CHECK(machine.read(0x3ff01004,4,value,false) && value==0);
            CHECK(advance(machine,32));
            CHECK(machine.read(0x3ff01004,4,value,false) && value==1);
            CHECK(read_words(machine,0x3ff01030,decrypt?plain:encrypted[length],(endian>>4)&3));
        }
    CHECK(machine.write(0x3ff01008,4,3));
    CHECK(!machine.write(0x3ff01000,4,1)); /* reserved mode cannot invent ciphertext */
    CHECK(machine.read(0x3ff01004,4,value,false) && value==1);
    CHECK(machine.write(0x3ff01008,4,0) && machine.write(0x3ff01040,4,63));
    CHECK(write_words(machine,0x3ff01010,key.substr(0,16),3));
    CHECK(write_words(machine,0x3ff01030,plain,3));
    CHECK(machine.write(0x3ff01000,4,1));
    CHECK(!machine.write(0x3ff01000,4,1)); /* no second transaction while busy */
    CHECK(machine.write(0x3ff0001c,4,0));
    CHECK(advance(machine,100));
    CHECK(machine.read(0x3ff01004,4,value,false) && value==0); /* gated clock pauses work */
    CHECK(read_words(machine,0x3ff01030,plain,3));
    CHECK(machine.write(0x3ff0001c,4,1));
    CHECK(advance(machine,32));
    CHECK(read_words(machine,0x3ff01030,encrypted[0],3));
    const unsigned reset_bits[]={1,8,16};
    for(unsigned i=0;i<3;++i) {
        CHECK(machine.write(0x3ff01000,4,1));
        CHECK(machine.write(0x3ff00020,4,reset_bits[i]));
        CHECK(machine.read(0x3ff01004,4,value,false) && value==1);
        CHECK(machine.read(0x3ff01010,4,value,false) && value==0);
        CHECK(machine.read(0x3ff01040,4,value,false) && value==63);
        CHECK(!machine.write(0x3ff01000,4,1));
        CHECK(machine.write(0x3ff00020,4,0));
    }
    EspMachine lx106(esp::CHIP_ESP8266);
    CHECK(!lx106.read(0x3ff01004,4,value,false));
#undef CHECK
    return checks;
}
