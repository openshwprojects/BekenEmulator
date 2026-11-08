#include "esp/esp_uart.h"
#ifndef ESP_UART_TEST_MAIN
#include "esp/esp_machine.h"
#endif
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("ESP UART check failed %d: %s\n",__LINE__,#x); return false; } } while (0)

bool fifo_and_interrupts() {
    EspUart u;u32 v;u64 delta;
    CHECK(u.read(0x14,0,80,v) && v==0x2b6);
    CHECK(u.read(0x20,0,80,v) && v==0x0800001c);
    CHECK(u.read(0x24,0,80,v) && v==0x6060);
    CHECK(!u.read(0x70,0,80,v) && !u.write(4,0,80,3));
    CHECK(u.write(0x24,0,80,0x6003) && u.write(0xc,0,80,1));
    u.receive("ab",0,80);
    CHECK(!u.interrupt() && !u.next_event_ticks(0,80,delta));
    u.receive("c",1,80);
    CHECK(u.interrupt() && u.read(8,1,80,v) && v==1);
    CHECK(u.read(0,1,80,v) && v=='a');
    CHECK(u.write(0x10,1,80,1) && !u.interrupt());
    CHECK(u.read(0x1c,1,80,v) && (v&255)==2);
    CHECK(u.read(0x60,1,80,v) && ((v>>13)&0x7ff)==2);
    CHECK(u.read(0x64,1,80,v) && v==0);
    CHECK(u.write(0xc,1,80,2) && u.interrupt());
    CHECK(u.write(0x10,1,80,2));
    u.advance(2,80);
    CHECK(u.interrupt()); // TXempty is a levelcondition, neededwhen driverenablesTX afterclearingstartupflags
    u.transmit('X',2,80);
    CHECK(u.output()=="X" && u.interrupt());
    CHECK(u.read(4,2,80,v) && (v&0x4002)==0x4002);
    CHECK(u.write(0x20,2,80,0x0802001c) && u.read(0x1c,2,80,v) && (v&255)==0);
    u.receive("discarded while reset",3,80);
    CHECK(u.read(0x1c,3,80,v) && (v&255)==0);
    CHECK(u.write(0x20,3,80,0x0800001c));
    u.receive(Bytes(130,'z'),4,80);
    CHECK(u.read(0x1c,4,80,v) && (v&255)==128);
    CHECK(u.read(4,4,80,v) && (v&16));
    CHECK(u.write(0x10,4,80,16) && u.read(4,4,80,v) && !(v&16));
    return true;
}

bool receive_timeout() {
    EspUart u;u32 v;u64 delta;
    CHECK(u.write(0x14,0,80,10) && u.write(0x24,0,80,0x82006060));
    CHECK(u.write(0xc,0,80,0x100));
    u.receive("AT",100,80); // threshold2 *8 baudbits *divider10 =160APBticks
    CHECK(u.next_event_ticks(100,80,delta) && delta==160);
    u.advance(259,80);CHECK(!u.interrupt());
    u.advance(260,80);CHECK(u.interrupt());
    CHECK(!u.next_event_ticks(260,80,delta));
    CHECK(u.write(0x10,260,80,0x100) && !u.interrupt());
    CHECK(u.next_event_ticks(260,80,delta) && delta==160);
    CHECK(u.read(0,270,80,v) && v=='A' && u.read(0,270,80,v) && v=='T');
    CHECK(!u.next_event_ticks(270,80,delta));
    u.advance(1000,80);CHECK(!u.interrupt());
    u.receive("a",1000,80);u.receive("b",1100,80);
    CHECK(u.next_event_ticks(1100,80,delta) && delta==160);
    CHECK(u.write(0xc,1100,80,0) && !u.next_event_ticks(1100,80,delta));
    u.advance(1260,80);
    CHECK(u.read(4,1260,80,v) && (v&0x100)); // RAW runs even while masked
    CHECK(u.write(0xc,1260,80,0x100) && u.interrupt());
    return true;
}

bool chip_variants_and_fractional_clock() {
    EspUart u;u32 v;u64 delta;
    CHECK(u.write(0x14,0,80,0x80000a) && u.write(0x24,0,80,0x81006060) && u.write(0xc,0,80,0x100));
    u.receive("x",0,80);
    CHECK(u.next_event_ticks(0,80,delta) && delta==84); // divider10.5 *8bits
    u.reset(true);
    CHECK(u.write(0x20,0,80,0x1c) && u.write(0x14,0,80,10)); // REF_TICK1MHz
    CHECK(u.write(0x24,0,80,0x88006060) && u.write(0xc,0,80,0x100));
    u.receive("x",0,80);
    CHECK(u.next_event_ticks(0,80,delta) && delta==800); // threshold8 =onebit with REF workaround
    u.reset(false);
    CHECK(u.read(0x20,0,80,v) && v==0x1c);
    CHECK(!u.read(0x34,0,80,v));
    CHECK(u.write(0x14,0,80,0x80000a) && u.read(0x14,0,80,v) && v==10);
    CHECK(u.write(0x24,0,80,0x81006060) && u.write(0xc,0,80,0x100));
    u.receive("x",0,80);
    CHECK(u.next_event_ticks(0,80,delta) && delta==100); // one8N1symbol
    u.transmit('y',0,80);
    CHECK(u.read(4,0,80,v) && !(v&0x4000)); // TX_DONE is ESP32 only
    CHECK(u.write(0x20,0,80,0x401c));
    u.transmit('z',0,80); // local loopback also reachesRX
    CHECK(u.read(0,0,80,v) && v=='x' && u.read(0,0,80,v) && v=='z');
    return true;
}
#ifndef ESP_UART_TEST_MAIN
bool machine_fifo_byte_access() {
    /* NSPanel's UART writer uses S8I a3,a10,0 at 0x400d8738.
     * The FIFO contains one octet per transaction, unlike config registers. */
    const u32 bases[]={0x3ff40000,0x3ff50000,0x3ff6e000};
    EspMachine m(esp::CHIP_ESP32);u32 value;
    CHECK(m.write(0x40080000,4,0x004a32));
    m.cpu().set_special(XT_SR_PS,0);
    for(unsigned i=0;i<3;++i) {
        m.cpu().set_pc(0x40080000);m.cpu().set_reg(10,bases[i]);m.cpu().set_reg(3,0x10d);
        CHECK(m.step()==XTENSA_OK && m.cpu().pc()==0x40080003);
        CHECK(m.uart(i)=="\r"); // exactly the low byte, once
        CHECK(m.write(bases[i]+0x24,4,0x6001) && m.write(bases[i]+0xc,4,1));
        CHECK(m.write(0x3ff00104+(34+i)*4,4,3));
        m.receive_uart(i,"ab");
        CHECK(m.cpu().special(XT_SR_INTERRUPT)&8);
        CHECK(m.read(bases[i],1,value,false) && value=='a');
        CHECK(m.read(bases[i],4,value,false) && value=='b');
        CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&8));
        CHECK(!m.write(bases[i]+1,1,'x') && !m.read(bases[i]+1,1,value,false));
        CHECK(!m.write(bases[i]+0x14,1,3) && !m.read(bases[i]+0x14,1,value,false));
        CHECK(m.read(bases[i]+0x14,4,value,false) && value==0x2b6);
        CHECK(!m.read(bases[i],1,value,true)); // never executable device memory
    }
    return true;
}

bool machine_uart_routing() {
    const esp::Chip chips[]={esp::CHIP_ESP8266,esp::CHIP_ESP32};
    const u32 bases[]={0x60000000,0x3ff40000};
    for(unsigned c=0;c<2;++c) {
        EspMachine m(chips[c]);u32 v;const u32 b=bases[c];
        CHECK(m.write(b+0x24,4,0x6002) && m.write(b+0xc,4,1));
        if(c)CHECK(m.write(0x3ff00104+34*4,4,3));
        const u32 irq=c?8u:32u;
        m.receive_uart(0,"x");CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&irq));
        m.receive_uart(0,"y");CHECK(m.cpu().special(XT_SR_INTERRUPT)&irq);
        CHECK(m.read(b,4,v,false) && v=='x' && m.write(b+0x10,4,1));
        CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&irq));
        const u32 b1=c?0x3ff50000u:0x60000f00u;
        CHECK(m.write(b1+0x24,4,0x6001) && m.write(b1+0xc,4,1));
        if(c)CHECK(m.write(0x3ff00104+35*4,4,3));
        m.receive_uart(1,"z");CHECK(m.cpu().special(XT_SR_INTERRUPT)&irq);
        CHECK(m.write(b+0x10,4,1) && (m.cpu().special(XT_SR_INTERRUPT)&irq)); // sharedline survivesotherportack
        CHECK(m.read(b1,4,v,false) && v=='z' && m.write(b1+0x10,4,1));
        CHECK(!(m.cpu().special(XT_SR_INTERRUPT)&irq));
    }
    return true;
}

bool machine_timeout_wait() {
    const esp::Chip chips[]={esp::CHIP_ESP8266,esp::CHIP_ESP32};
    const u32 bases[]={0x60000000,0x3ff40000};
    const u32 pcs[]={0x40100000,0x40080000};
    for(unsigned c=0;c<2;++c) {
        EspMachine m(chips[c]);const u32 b=bases[c],pc=pcs[c];
        CHECK(m.write(pc,4,0x007000)); // WAITI0
        m.cpu().set_pc(pc);m.cpu().set_special(XT_SR_PS,0);
        m.cpu().set_special(XT_SR_VECBASE,pc+0x1000);
        m.cpu().set_special(XT_SR_INTENABLE,c?8:32);
        if(c)CHECK(m.write(0x3ff00104+34*4,4,3));
        CHECK(m.write(b+0x14,4,10) && m.write(b+0x24,4,0x81006060) && m.write(b+0xc,4,0x100));
        m.receive_uart(0,"AT");
        CHECK(m.step()==XTENSA_OK && !m.cpu().waiting());
        CHECK(m.cpu().special(XT_SR_INTERRUPT)&(c?8:32));
        CHECK(m.cpu().special(XT_SR_CCOUNT)==(c?80u:100u)); // idle deadline followschiptimeoutunits
    }
    return true;
}
#endif
}
int esp_uart_tests() {
    checks=0;
    if(!fifo_and_interrupts() || !receive_timeout() || !chip_variants_and_fractional_clock()) return -1;
#ifndef ESP_UART_TEST_MAIN
    if(!machine_fifo_byte_access() || !machine_uart_routing() || !machine_timeout_wait())return -1;
#endif
    printf("All ESP UART tests passed (%d checks).\n",checks);
    return checks;
}
#if defined(ESP_UART_TEST_MAIN) || defined(ESP_UART_MACHINE_TEST_MAIN)
int main() { return esp_uart_tests()<0?1:0; }
#endif
