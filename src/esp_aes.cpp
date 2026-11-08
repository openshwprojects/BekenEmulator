#include "esp_aes.h"
#include "crypto_prims.h"
#include "esp_state.h"

/* ESP32 TRM chapter14 and ESP-IDF hal/esp32/include/hal/aes_ll.h.
 * https://documentation.espressif.com/esp32_technical_reference_manual_en.pdf
 * Key/input/output byte and word ordering are independently programmable.
 * The timing model uses the published upper bounds:15 encryption clocks,
 * 22 decryption clocks. It is functional timing, not a cycle-accurate core. */
namespace esp_aes {
namespace {
const u32 BASE=0x3ff01000,REMAINING=esp_state::AES,LAST=esp_state::AES+4,RESULT=esp_state::AES+0x20;
u32 get(const std::map<u32,u32> &registers,u32 address,u32 initial=0) {
    std::map<u32,u32>::const_iterator i=registers.find(address);
    return i==registers.end()?initial:i->second;
}
bool configured(u32 address) {
    return address==BASE+8 || address==BASE+0x40 ||
        (address>=BASE+0x10 && address<=BASE+0x3c && !(address&3));
}
bool held_reset(const std::map<u32,u32> &registers) {return (get(registers,0x3ff00020)&0x19)!=0;}
void read_bytes(const std::map<u32,u32> &registers,u32 base,u8 *bytes,unsigned length,unsigned order) {
    for(unsigned i=0;i<length;++i) {
        const unsigned word=(order&2)?i/4:length/4-1-i/4;
        const unsigned byte=(order&1)?i%4:3-i%4;
        bytes[i]=(u8)(get(registers,base+word*4)>>(byte*8));
    }
}
}

void reset(std::map<u32,u32> &registers) {
    for(u32 offset=0;offset<=0x40;offset+=4)registers.erase(BASE+offset);
    registers.erase(REMAINING);registers.erase(LAST);registers.erase(LAST+4);
    for(unsigned i=0;i<4;++i)registers.erase(RESULT+i*4);
}

void synchronize(std::map<u32,u32> &registers,u64 ticks) {
    const u32 remaining=get(registers,REMAINING);
    if(!remaining)return;
    const u64 last=(u64(get(registers,LAST+4))<<32)|get(registers,LAST);
    registers[LAST]=u32(ticks);registers[LAST+4]=u32(ticks>>32);
    if(held_reset(registers) || !(get(registers,0x3ff0001c)&1) || ticks<last)return;
    const u64 elapsed=ticks-last;
    if(elapsed<remaining) {registers[REMAINING]=remaining-u32(elapsed);return;}
    for(unsigned i=0;i<4;++i) {
        registers[BASE+0x30+i*4]=get(registers,RESULT+i*4);
        registers.erase(RESULT+i*4);
    }
    registers.erase(REMAINING);registers.erase(LAST);registers.erase(LAST+4);
}

bool read(std::map<u32,u32> &registers,u32 address,u64 ticks,u32 &value) {
    if(address!=BASE+4 && !configured(address))return false;
    synchronize(registers,ticks);
    if(address==BASE+4)value=get(registers,REMAINING)?0:1;
    else value=get(registers,address,address==BASE+0x40?63:0);
    return true;
}

bool write(std::map<u32,u32> &registers,u32 address,u64 ticks,u32 value,std::string &error) {
    if(address!=BASE && !configured(address))return false;
    synchronize(registers,ticks);
    if(!(get(registers,0x3ff0001c)&1) || held_reset(registers)) {
        error="ESP32 AES clock disabled or engine held in reset";return false;
    }
    if(address==BASE && !(value&1))return true;
    if(get(registers,REMAINING)) {error="ESP32 AES write while operation is busy";return false;}
    if(address!=BASE) {
        registers[address]=value&(address==BASE+8?7u:address==BASE+0x40?63u:0xffffffffu);
        return true;
    }
    const unsigned mode=get(registers,BASE+8);
    if((mode&3)==3) {error="reserved ESP32 AES operation mode";return false;}
    const unsigned length=16+(mode&3)*8,endian=get(registers,BASE+0x40,63);
    u8 key[32],input[16],output[16];
    read_bytes(registers,BASE+0x10,key,length,endian&3);
    read_bytes(registers,BASE+0x30,input,16,(endian>>2)&3);
    Aes cipher(Bytes((const char *)key,length));
    if(mode&4)cipher.decrypt_block(input,output);else cipher.encrypt_block(input,output);
    for(unsigned i=0;i<4;++i) {
        const unsigned index=(endian&32)?i:3-i;
        registers[RESULT+index*4]=(endian&16)?rd32le(output+i*4):rd32be(output+i*4);
    }
    registers[REMAINING]=(mode&4)?22:15;
    registers[LAST]=u32(ticks);registers[LAST+4]=u32(ticks>>32);
    return true;
}
}
