#include "esp/esp_machine.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("ESP ERI check failed %d: %s\n",__LINE__,#x); return false; } } while (0)
bool debug_registers() {
    EspMachine m(esp::CHIP_ESP32);u32 v;
    CHECK(m.external_read(0x10200c,v) && v==0); // no attached debugger
    CHECK(m.external_write(0x10200c,0x10000) && m.external_read(0x102008,v) && v==0x10000);
    CHECK(m.external_write(0x102008,0x10000) && m.external_read(0x10200c,v) && v==0);
    CHECK(m.external_write(0x102014,0x12345678) && m.external_read(0x102014,v) && v==0x12345678);
    CHECK(m.external_read(0x102010,v) && v==0xc0000c00);
    CHECK(m.external_write(0x102010,0x400) && m.external_read(0x102010,v) && v==0xc0000800);
    CHECK(m.external_read(0x103028,v) && v==0);
    CHECK(!m.external_read(0x10200d,v) && !m.external_read(0x00102004,v));
    CHECK(!m.external_write(0x10200c,2)); // debug instruction execution is unsupported, explicitstop
    CHECK(!m.read(0x102014,4,v,false)); // ERI neveraliasesnormaldata memory
    m.cpu().set_processor_id(0xabab);
    CHECK(m.external_read(0x102014,v) && v==0); // percoreexternaldebugstate
    EspMachine n(esp::CHIP_ESP8266);
    CHECK(!n.external_read(0x10200c,v));
    return true;
}
bool cpu_external_instruction() {
    EspMachine m(esp::CHIP_ESP32);
    CHECK(m.write(0x40080000,4,0x406220));
    m.cpu().set_pc(0x40080000);m.cpu().set_reg(2,0x10200c);
    CHECK(m.step()==XTENSA_OK && m.cpu().reg(2)==0 && m.cpu().pc()==0x40080003);
    CHECK(m.write(0x40080004,4,0x407320));
    m.cpu().set_pc(0x40080004);m.cpu().set_reg(3,0x102014);m.cpu().set_reg(2,0x99887766);
    CHECK(m.step()==XTENSA_OK);
    u32 v;CHECK(m.external_read(0x102014,v) && v==0x99887766);
    return true;
}
}
int esp_eri_tests() {
    checks=0;if(!debug_registers() || !cpu_external_instruction())return -1;
    printf("All ESP ERI tests passed (%d checks).\n",checks);return checks;
}
#ifdef ESP_ERI_TEST_MAIN
int main() { return esp_eri_tests()<0?1:0; }
#endif
