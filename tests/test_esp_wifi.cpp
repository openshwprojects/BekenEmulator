#include "esp_machine.h"
#include <stdio.h>

namespace {
class FaultingDmaMachine:public EspMachine {
public:
    u32 fail_write;
    FaultingDmaMachine():EspMachine(esp::CHIP_ESP8266),fail_write(0){}
    bool write(u32 address,unsigned size,u32 value) {
        return address!=fail_write && EspMachine::write(address,size,value);
    }
};
}

int esp_wifi_tests()
{
    int checks=0;u32 value=0;std::string error;
#define CHECK(x) do { ++checks; if(!(x)) { printf("[FAIL] ESP Wi-Fi line %d: %s\n",__LINE__,#x);return -1; } } while(0)
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP8266;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40100000;
    esp::Segment code;code.address=image.entry_point;code.data.assign(0x100,'\0');
    code.data.replace(0,3,"\x00\x70\x00",3); // WAITI0
    image.segments.push_back(code);layout.images.push_back(image);
    FaultingDmaMachine machine;
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_PS,0);
    machine.cpu().set_special(XT_SR_INTENABLE,1);
    Bytes frame(24,'\0');frame[0]=(char)0x80; // beacon, broadcast destination
    frame.replace(4,6,6,(char)0xff);
    frame.replace(10,6,"\x02\x77\x88\x99\xaa\xbb",6);
    frame.replace(16,6,frame.substr(10,6));
    Esp8266WifiDma::RxInfo info;info.rate=11;info.channel=6;
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==0);
    CHECK(!machine.write(0x3ff2003c,4,11u<<16));
    CHECK(machine.receive_wifi(frame,info,&error)==Esp8266WifiDma::RX_DISABLED);
    CHECK(machine.write(0x3ffe8000,4,0x80080080)); // capacity128; owner is ignored
    CHECK(machine.write(0x3ffe8004,4,0x3fff0000));
    CHECK(machine.write(0x3ffe8008,4,0x3ffe8010)); // next node remains a sentinel
    CHECK(machine.write(0x3ffe8010,4,0x80080080));
    CHECK(machine.write(0x3ffe8014,4,0x3fff0100));
    CHECK(machine.write(0x3ffe8018,4,0));
    CHECK(machine.write(0x3ff20008,4,0x3ffe8000));
    CHECK(machine.write(0x3ff20004,4,0x80000000));
    CHECK(machine.write(0x3ff20c28,4,0x99887702));
    CHECK(machine.write(0x3ff20c2c,4,0xbbaa));
    CHECK(machine.write(0x3ff20c38,4,0xffffffff));
    CHECK(machine.write(0x3ff20c3c,4,0x1ffff));
    CHECK(machine.write(0x3ff20c18,4,1u<<8));
    CHECK(machine.receive_wifi(frame,info,&error)==Esp8266WifiDma::RX_ACCEPTED);
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==0); // frame is still on air
    CHECK(machine.read(0x3ff201d0,4,value,false) && (value&0xf000)!=0);
    CHECK(machine.read(0x3ffe8000,4,value,false) && !(value&(1u<<30)));
    CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==image.entry_point+0x30);
    CHECK(machine.elapsed_us()==70 && machine.cpu().special(XT_SR_CCOUNT)==70*80);
    CHECK(machine.wifi_receptions()==1);
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==(11u<<16));
    CHECK(machine.read(0x3ff20014,4,value,false) && value==0x3ffe8000);
    CHECK(machine.read(0x3ff210fc,4,value,false) && value==0); // timestamp at reception start
    CHECK(machine.read(0x3ffe8000,4,value,false) && (value&(1u<<30)) && ((value>>12)&0xfff)==40);
    CHECK(machine.read(0x3fff000c,4,value,false) && value==0x80);
    CHECK(machine.read(0x3ff20c1c,4,value,false) && value==(1u<<8));
    CHECK(machine.receive_wifi(frame,info,&error)==Esp8266WifiDma::RX_BUSY);
    CHECK(machine.write(0x3ff20c24,4,1u<<8));
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==(11u<<16));
    CHECK(!(machine.cpu().special(XT_SR_INTERRUPT)&1));
    /* A bus failure during completion must also leave WAIT with a fault;
     * otherwise the runner could keep executing after a failed DMA transfer. */
    CHECK(machine.write(0x3ffe8000,4,0x80080080));
    CHECK(machine.write(0x3ff20008,4,0x3ffe8000));
    u32 original_metadata=0;
    CHECK(machine.read(0x3fff0000,4,original_metadata,false));
    info.rssi_dbm=-33;info.rate=10;
    CHECK(machine.receive_wifi(frame,info,&error)==Esp8266WifiDma::RX_ACCEPTED);
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==(11u<<16));
    machine.fail_write=0x3fff0010;
    machine.cpu().set_pc(image.entry_point);machine.cpu().set_special(XT_SR_PS,0);
    CHECK(machine.step()==XTENSA_MEMORY_FAULT && !machine.diagnostic().empty());
    CHECK(machine.read(0x3fff0000,4,value,false) && value==original_metadata);
    CHECK(machine.wifi_receptions()==1);
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==(11u<<16)); // failed DMA cannot publish a new rate
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
    machine.fail_write=0;
    CHECK(machine.write(0x60000700,4,0x80000000) && machine.step()==XTENSA_OK);
    CHECK(machine.wifi_receptions()==0 && machine.wifi_transmissions()==0);
    CHECK(machine.read(0x3ff20014,4,value,false) && value==0);
    CHECK(machine.read(0x3ff2003c,4,value,false) && value==0);
    EspMachine esp32(esp::CHIP_ESP32);
    CHECK(!esp32.set_wifi_air(0));
    CHECK(esp32.receive_wifi(frame,info,&error)==Esp8266WifiDma::RX_INVALID && !error.empty());
#undef CHECK
    return checks;
}
