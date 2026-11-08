#include "esp_machine.h"
#include "esp8266_wdev.h"
#include "esp_test_images.h"
#include <stdio.h>

int esp8266_wdev_tests()
{
    int checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("[FAIL] ESP8266 WDEV line %d: %s\n",__LINE__,#x);return -1; } } while(0)
    esp8266_wdev::Registers registers;u32 status=0;
    CHECK(!esp8266_wdev::interrupt(registers));
    esp8266_wdev::raise(registers,0x0c000000);
    CHECK(!esp8266_wdev::interrupt(registers));
    CHECK(esp8266_wdev::read(registers,0x3ff20c1c,0,status) && status==0x0c000000);
    CHECK(esp8266_wdev::write(registers,0x3ff20c18,0x08000000));
    CHECK(esp8266_wdev::interrupt(registers));
    CHECK(esp8266_wdev::read(registers,0x3ff20c20,0,status) && status==0x08000000);
    CHECK(esp8266_wdev::write(registers,0x3ff20c24,0x08000000));
    CHECK(!esp8266_wdev::interrupt(registers));
    CHECK(esp8266_wdev::read(registers,0x3ff20c1c,0,status) && status==0x04000000);
    CHECK(esp8266_wdev::write(registers,0x3ff20c18,0x0c000000));
    CHECK(esp8266_wdev::read(registers,0x3ff20c20,0,status) && status==0x04000000);
    CHECK(esp8266_wdev::write(registers,0x3ff20c24,0xffffffff));
    CHECK(!esp8266_wdev::interrupt(registers));
    CHECK(!esp8266_wdev::write(registers,0x3ff20c20,1));
    CHECK(!esp8266_wdev::write(registers,0x3ff20c00,1));
    CHECK(!esp8266_wdev::read(registers,0x3ff20c24,0,status));
    CHECK(esp8266_wdev::handles(0x3ff20c1c));
    CHECK(!esp8266_wdev::write(registers,0x3ff20c1c,0xffffffff));
    CHECK(esp8266_wdev::read(registers,0x3ff20c00,0x100000007ull,status) && status==7);
    /* A TX completion need not have a measured ACK signal. The SDK tests
     * bit24 before consuming its RSSI byte; an absent measurement stays
     * invalid even when a functional peer supplies an ACK outcome. */
    CHECK(esp8266_wdev::handles(0x3ff20e04));
    CHECK(esp8266_wdev::read(registers,0x3ff20e04,0,status) && status==0);
    CHECK(!esp8266_wdev::write(registers,0x3ff20e04,0x0100d600));
    esp8266_wdev::raise(registers,1u<<19);
    CHECK(esp8266_wdev::read(registers,0x3ff20e04,0,status) && status==0);
    CHECK(esp8266_wdev::read(registers,0x3ff20c1c,0,status) && status==(1u<<19));
    CHECK(esp8266_wdev::write(registers,0x3ff20c24,1u<<19));
    const u32 options[]={0x3ff20c14,0x3ff20c68,0x3ff20c6c,0x3ff20c70,0x3ff20c74,0x3ff20c88,0x3ff20c90,0x3ff20c94,0x3ff20de0,0x3ff20e08};
    for(unsigned i=0;i<sizeof(options)/sizeof(options[0]);++i) {
        CHECK(esp8266_wdev::write(registers,options[i],0x12345678));
        CHECK(esp8266_wdev::read(registers,options[i],0,status) && status==0x12345678);
    }
    /* wDev_AppendRxBlocks sets the CA8/CAC high nibbles to 1/5 when at
     * least two buffers remain. Configuration must not publish a packet. */
    esp8266_wdev::Registers receive_options;
    const u32 receive_words[]={0x3ff20ca8,0x3ff20cac};
    for(unsigned i=0;i<2;++i) {
        const u32 address=receive_words[i],high=(i?5u:1u)<<28;
        CHECK(esp8266_wdev::handles(address));
        CHECK(esp8266_wdev::read(receive_options,address,0,status) && status==0);
        CHECK(esp8266_wdev::write(receive_options,address,0x0abc1234));
        CHECK(esp8266_wdev::read(receive_options,address,0,status));
        CHECK(esp8266_wdev::write(receive_options,address,(status&0x0fffffff)|high));
        CHECK(esp8266_wdev::read(receive_options,address,0,status) && status==(high|0x0abc1234));
    }
    CHECK(esp8266_wdev::read(receive_options,0x3ff20c1c,0,status) && status==0);
    CHECK(!esp8266_wdev::interrupt(receive_options));
    CHECK(!esp8266_wdev::handles(0x3ff20ca4));
    CHECK(!esp8266_wdev::handles(0x3ff20cb0));
    /* lmacSetTxFrame programs the remaining frame lifetime, in 1024-us
     * units, into bits23:12 of each ordinary queue's sixth word. */
    for(unsigned queue=0;queue<8;++queue) {
        const u32 lifetime=0x3ff20dd4-queue*24,value=(queue+5)<<12;
        CHECK(esp8266_wdev::handles(lifetime));
        CHECK(esp8266_wdev::write(registers,lifetime,value));
        CHECK(esp8266_wdev::read(registers,lifetime,0,status) && status==value);
        CHECK(!esp8266_wdev::write(registers,lifetime,0x1000000));
        CHECK(!esp8266_wdev::write(registers,lifetime,1));
        CHECK(esp8266_wdev::read(registers,lifetime,0,status) && status==value);
    }
    CHECK(!esp8266_wdev::handles(0x3ff20dd8));
    CHECK(!esp8266_wdev::handles(0x3ff20d14));
    CHECK(!esp8266_wdev::handles(0x3ff20cfc));
    esp8266_wdev::Registers replay;u32 first=0;
    CHECK(esp8266_wdev::handles(0x3ff20e44));
    for(unsigned i=0;i<16;++i) {
        u32 other;
        CHECK(esp8266_wdev::read(registers,0x3ff20e44,0,status));
        CHECK(esp8266_wdev::read(replay,0x3ff20e44,0,other) && status==other);
        if(!i)first=status;else CHECK(status!=first);
    }
    CHECK(!esp8266_wdev::write(registers,0x3ff20e44,0));
    CHECK(!esp8266_wdev::handles(0x3ff20e40));
    CHECK(!esp8266_wdev::handles(0x3ff20e48));
    CHECK(!esp8266_wdev::interrupt(replay)); // RNG reads never raise MAC events
    esp8266_wdev::Registers queues;
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0);
    CHECK(esp8266_wdev::write(queues,0x3ff20c18,(1u<<20)|(1u<<29)));
    esp8266_wdev::raise_queue_events(queues,1u<<3,(1u<<0)|(1u<<7));
    CHECK(esp8266_wdev::interrupt(queues));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0x00810008);
    CHECK(esp8266_wdev::read(queues,0x3ff20c20,0,status) && status==((1u<<20)|(1u<<29)));
    // The IRQ is acknowledged separately from the per-queue flags. The
    // official handler preserves collisions while clearing expired queues.
    CHECK(esp8266_wdev::write(queues,0x3ff20c24,1u<<29));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0x00810008);
    CHECK(esp8266_wdev::write(queues,0x3ff20cc0,status&0xf000ffffu));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==8);
    CHECK(esp8266_wdev::read(queues,0x3ff20c20,0,status) && status==(1u<<20));
    esp8266_wdev::raise_queue_events(queues,1u<<4,1u<<10);
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0x04000018);
    CHECK(esp8266_wdev::write(queues,0x3ff20cc0,status&0xfffff000u));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0x04000000);
    CHECK(esp8266_wdev::write(queues,0x3ff20cc0,0xffffffff));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0x04000000);
    CHECK(esp8266_wdev::write(queues,0x3ff20cc0,0));
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0);
    CHECK(esp8266_wdev::interrupt(queues)); // status clear does not acknowledge IRQ
    CHECK(esp8266_wdev::write(queues,0x3ff20c24,0xffffffff));
    CHECK(!esp8266_wdev::interrupt(queues));
    esp8266_wdev::raise_queue_events(queues,0xfffff000,0xfffff000);
    CHECK(esp8266_wdev::read(queues,0x3ff20cc0,0,status) && status==0);
    CHECK(!esp8266_wdev::interrupt(queues));
    const Bytes image=esp_test::uart_image(false,true);
    esp::FlashImage layout;std::string error;u32 value=0;
    CHECK(esp::parse_flash(image,esp::CHIP_ESP8266,layout,&error));
    EspMachine machine(esp::CHIP_ESP8266);
    CHECK(machine.load(image,layout,false,&error));
    CHECK(machine.write(0x3ff20008,4,0x3ffe9000));
    CHECK(machine.write(0x3ff20ca8,4,0x12345678));
    CHECK(machine.write(0x3ff20cac,4,0x5abcdef0));
    CHECK(machine.read(0x3ff20ca8,4,value,false) && value==0x12345678);
    CHECK(machine.read(0x3ff20cac,4,value,false) && value==0x5abcdef0);
    CHECK(machine.read(0x3ff20008,4,value,false) && value==0x3ffe9000);
    CHECK(machine.read(0x3ff20014,4,value,false) && value==0);
    CHECK(machine.wifi_receptions()==0);
    CHECK(machine.read(0x3ff20c1c,4,value,false) && value==0);
    CHECK(machine.load(image,layout,false,&error));
    CHECK(machine.read(0x3ff20ca8,4,value,false) && value==0);
    CHECK(machine.read(0x3ff20cac,4,value,false) && value==0);
    CHECK(machine.read(0x3ff20e04,4,value,false) && value==0);
    CHECK(!machine.write(0x3ff20e04,4,0x01000000));
    CHECK(machine.read(0x3ff20e44,4,value,false) && value==first);
    CHECK(machine.read(0x3ff20e44,4,value,false) && value!=first);
    CHECK(!machine.write(0x3ff20e44,4,0));
    CHECK(machine.write(0x3ff20dd4,4,5<<12));
    CHECK(machine.read(0x3ff20dd4,4,value,false) && value==(5<<12));
    CHECK(machine.load(image,layout,false,&error));
    CHECK(machine.read(0x3ff20dd4,4,value,false) && value==0);
    CHECK(machine.read(0x3ff20e44,4,value,false) && value==first);
    /* The SDK starts by masking the MAC and acknowledging all old events. */
    CHECK(machine.write(0x3ff20c18,4,0));
    CHECK(machine.write(0x3ff20c24,4,0xffffffff));
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
    CHECK(machine.read(0x3ff20c1c,4,value,false) && value==0);
    CHECK(machine.write(0x3ff20c18,4,0x2c9f0300));
    CHECK(machine.read(0x3ff20c18,4,value,false) && value==0x2c9f0300);
    CHECK(machine.read(0x3ff20c00,4,value,false) && value==0);
    CHECK(machine.run(160)==XTENSA_OK);
    CHECK(machine.read(0x3ff20c00,4,value,false) && value==machine.elapsed_us() && value>=2);
    CHECK(machine.load(image,layout,false,&error));
    CHECK(machine.read(0x3ff20c18,4,value,false) && value==0);
    CHECK(machine.write(0x3ff20800,4,0x00030303));
    CHECK(machine.write(0x3ff20804,4,0x10030303));
    CHECK(machine.write(0x3ff2080c,4,0x01000001));
    CHECK(machine.read(0x3ff2080c,4,value,false) && value==0x01000001);
    /* Each hardware key slot contains eight metadata bytes and 32 key
     * bytes. ROM memcpy uses narrow stores for a partial last word. */
    for(unsigned slot=0;slot<25;++slot) {
        const u32 address=0x3ff21400+slot*40;
        CHECK(machine.write(address,4,0xdead0000+slot));
        CHECK(machine.write(address+4,4,0x6000ffff));
        for(unsigned i=0;i<32;++i)CHECK(machine.write(address+8+i,1,(slot+i)&255));
        CHECK(machine.read(address,4,value,false) && value==0xdead0000+slot);
        CHECK(machine.read(address+4,2,value,false) && value==0xffff);
        CHECK(machine.read(address+8,4,value,false) && value==(slot|((slot+1)<<8)|((slot+2)<<16)|((slot+3)<<24)));
        CHECK(machine.read(address+39,1,value,false) && value==slot+31);
    }
    CHECK(!machine.read(0x3ff217e8,4,value,false));
    CHECK(!machine.write(0x3ff213ff,1,0));
    CHECK(machine.load(image,layout,false,&error));
    CHECK(machine.read(0x3ff21400,4,value,false) && value==0);
    /* wDev_Initialize publishes the RX descriptor rings, then enables the
     * receiver. An idle RF channel must leave their heads undisturbed. */
    const u32 rx_registers[]={0x3ff20080,0x3ff2007c,0x3ff20088,0x3ff20084,
        0x3ff20000,0x3ff20008,0x3ff2000c,0x3ff20010,0x3ff2006c,0x3ff20178,0x3ff20004};
    for(unsigned i=0;i<sizeof(rx_registers)/sizeof(rx_registers[0]);++i) {
        CHECK(machine.write(rx_registers[i],4,0x3ffe8000+4*i));
        CHECK(machine.read(rx_registers[i],4,value,false) && value==0x3ffe8000+4*i);
    }
    CHECK(machine.run(100)==XTENSA_OK);
    CHECK(machine.read(0x3ff20008,4,value,false) && value==0x3ffe8014);
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
    CHECK(machine.read(0x3ff20410,4,value,false) && value==0);
    CHECK(machine.read(0x3ff201d0,4,value,false) && value==0);
    CHECK(!machine.write(0x3ff20410,4,1));
    CHECK(!machine.write(0x3ff201d0,4,0x1000));
#undef CHECK
    return checks;
}
