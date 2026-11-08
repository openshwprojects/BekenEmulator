#include "esp/esp_spi.h"
#include <stdio.h>

namespace {
int checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("[FAIL] ESP SPI line %d: %s\n", __LINE__, #x); return false; } } while(0)

bool controller(esp::Chip chip)
{
    std::map<u32,u32> regs; Bytes flash(0x20000,'\xff'); u32 value;
    const u32 base = chip == esp::CHIP_ESP32 ? 0x3ff42000 : 0x60000100;
    const u32 buffer = base + (chip == esp::CHIP_ESP32 ? 0x80 : 0x40);
    std::string error;
    CHECK(esp_spi::read(chip,regs,flash,base+0x1c,value) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<28) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && value == 0x001140ef);
    CHECK(esp_spi::read(chip,regs,flash,base,value) == esp_spi::OK && value == 0);
    CHECK(esp_spi::write(chip,regs,flash,base+4,chip==esp::CHIP_ESP32 ? 0x1234 : 0x04001234) == esp_spi::OK);
    if (chip == esp::CHIP_ESP32) {
        CHECK(esp_spi::write(chip,regs,flash,base+0x28,31) == esp_spi::OK);
        CHECK(esp_spi::write(chip,regs,flash,base+0x2c,31) == esp_spi::OK);
    }
    CHECK(esp_spi::write(chip,regs,flash,buffer,0x44332211) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<25) == esp_spi::OK);
    CHECK((u8)flash[0x1234] == 0xff); /* programming requires WEL */
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<25) == esp_spi::OK);
    CHECK(flash.substr(0x1234,4) == Bytes("\x11\x22\x33\x44",4));
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<27) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x10,value) == esp_spi::OK && !(value & 3));
    CHECK(esp_spi::write(chip,regs,flash,buffer,0xffffffff) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<25) == esp_spi::OK);
    CHECK((u8)flash[0x1234] == 0x11); /* NOR cannot program zeroes back to one */
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<31) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && value == 0x44332211);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<24) == esp_spi::OK);
    CHECK(flash.substr(0x1000,0x1000) == Bytes(0x1000,'\xff'));
    CHECK(esp_spi::write(chip,regs,flash,base+4,0x00ffffff) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<31,&error) == esp_spi::ERROR);
    CHECK(!error.empty());
    CHECK(esp_spi::write(chip,regs,flash,base,1,&error) == esp_spi::ERROR);
    CHECK(esp_spi::read(chip,regs,flash,base+0xe0,value) == esp_spi::UNHANDLED);
    CHECK(esp_spi::write(chip,regs,flash,base+0xe0,1) == esp_spi::UNHANDLED);
    return true;
}

bool esp32_address_phase()
{
    std::map<u32,u32> regs; Bytes flash(0x20000,'\xff'); flash.replace(0x1234,4,"real");
    const u32 base=0x3ff42000; u32 value;
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x1c,(1u<<31)|(1u<<30)|(1u<<28)) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x20,23u<<26) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x24,0x70000003) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+4,0x123400) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x2c,31) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<18) == esp_spi::OK);
    CHECK(esp_spi::read(esp::CHIP_ESP32,regs,flash,base+0x80,value) == esp_spi::OK && value == 0x6c616572);
    /* Fixed hardware commands use an unshifted address even when USER_ADDR
     * remains set by a preceding user transaction (spi_flash_ll.h). */
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+4,0x1234) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<31) == esp_spi::OK);
    CHECK(esp_spi::read(esp::CHIP_ESP32,regs,flash,base+0x80,value) == esp_spi::OK && value == 0x6c616572);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x28,31) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x80,0) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<25) == esp_spi::OK);
    CHECK(flash.substr(0x1234,4)==Bytes(4,0) && (u8)flash[0x12]==0xff);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<24) == esp_spi::OK);
    CHECK(flash.substr(0x1234,4)==Bytes(4,'\xff'));
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base+0x24,0x700000ff) == esp_spi::OK);
    CHECK(esp_spi::write(esp::CHIP_ESP32,regs,flash,base,1u<<18) == esp_spi::ERROR);
    return true;
}

bool nor_protocol()
{
    std::map<u32,u32> regs; Bytes flash(0x100000,'\xff'); u32 value;
    const esp::Chip chip=esp::CHIP_ESP8266; const u32 base=0x60000200,buffer=base+0x40;
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<28) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && value == 0x1440ef);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<21) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<28) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && value == 0xffffffff);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<20) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && (value&0xff) == 0x13);
    /* Page program wraps inside a page, and consumes the write enable latch. */
    CHECK(esp_spi::write(chip,regs,flash,base+4,0x040000fe) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,buffer,0x44332211) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<30) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<25) == esp_spi::OK);
    CHECK((u8)flash[0xfe]==0x11 && (u8)flash[0xff]==0x22 && (u8)flash[0]==0x33 && (u8)flash[1]==0x44);
    /* ESP8266 cache-map bits are not the ESP32 high-buffer selectors. */
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0x83000040) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x20,(31u<<8)) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x24,0x70000003) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+4,0) == esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18) == esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,buffer,value) == esp_spi::OK && value == 0xffff4433);
    return true;
}

bool esp32_chip_select()
{
    std::map<u32,u32> regs; Bytes flash(0x100000,'\xff'); u32 value;
    const esp::Chip chip=esp::CHIP_ESP32; const u32 base=0x3ff42000;
    /* ESP-IDF psram_disable_qio_mode sends F5 in raw MOSI bits on CS1.
     * No PSRAM is installed by default, so the transaction is electrically
     * unanswered; it must not reach the NOR command decoder on CS0. */
    CHECK(esp_spi::write(chip,regs,flash,base+0x34,5)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0x08008040)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x24,0)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x28,15)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x80,0xf500)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x80,value)==esp_spi::OK && value==0xf500);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,1u<<28)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x2c,31)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x80,value)==esp_spi::OK && value==0xffffffff);
    CHECK(flash==Bytes(0x100000,'\xff'));
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<30)==esp_spi::OK); /* WREN on CS1 does not enable NOR */
    CHECK(esp_spi::write(chip,regs,flash,base+0x34,6)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<27)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x10,value)==esp_spi::OK && !(value&2));
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<28)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x80,value)==esp_spi::OK && value==0x1440ef);
    return true;
}

bool esp32_psram_transactions()
{
    std::map<u32,u32> regs;Bytes flash(0x400000,'\xff'),psram(0x800000,0);u32 value;
    const esp::Chip chip=esp::CHIP_ESP32;const u32 base=0x3ff42000;
    /* Exact DCLK startup transactions observed in the real firmware. */
    CHECK(esp_spi::write(chip,regs,flash,base+0x34,5)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0x08008040)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x28,15)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x80,0xf500)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18,0,&psram)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0xd0000040)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x20,31u<<26)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x24,0x10000000)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+4,0x9f000000)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x2c,63)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18,0,&psram)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,base+0x80,value)==esp_spi::OK && value==0x405d0d);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0x48000040)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x20,7u<<26)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+4,0x35000000)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18,0,&psram)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x1c,0xc8008040)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x20,23u<<26)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x24,0x70000002)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+4,0x123400)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x28,31)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base+0x80,0x44332211)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18,0,&psram)==esp_spi::OK);
    CHECK(psram.substr(0x1234,4)==Bytes("\x11\x22\x33\x44",4));
    CHECK(flash==Bytes(0x400000,'\xff'));
    CHECK(esp_spi::write(chip,regs,flash,base+0x34,4)==esp_spi::OK);
    CHECK(esp_spi::write(chip,regs,flash,base,1u<<18,0,&psram)==esp_spi::ERROR); /* bus contention */
    return true;
}

bool esp32_psram_clock_control()
{
    std::map<u32,u32> regs; Bytes flash(0x400000,'\xff'); u32 value;
    const esp::Chip chip=esp::CHIP_ESP32;
    /* The low 28 DATE bits identify the silicon register revision. IDF's
     * ESP32 PSRAM clock setup also uses the two high SPI0 clock-divider bits. */
    CHECK(esp_spi::read(chip,regs,flash,0x3ff433fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::write(chip,regs,flash,0x3ff433fc,0xffffffff)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff433fc,value)==esp_spi::OK && value==0xc1604270);
    CHECK(esp_spi::write(chip,regs,flash,0x3ff433fc,value&~(1u<<30))==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff433fc,value)==esp_spi::OK && value==0x81604270);
    CHECK(esp_spi::write(chip,regs,flash,0x3ff433fc,0)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff433fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff423fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff643fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff653fc,value)==esp_spi::OK && value==0x01604270); /* DPORT synchronization read */
    CHECK(esp_spi::write(chip,regs,flash,0x3ff653fc,0xffffffff)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff653fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff653f8,value)==esp_spi::UNHANDLED);
    CHECK(esp_spi::write(chip,regs,flash,0x3ff65000,1u<<18)==esp_spi::UNHANDLED); /* external SPI transfer remains explicit */
    CHECK(esp_spi::write(chip,regs,flash,0x3ff423fc,0xffffffff)==esp_spi::OK);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff423fc,value)==esp_spi::OK && value==0x01604270);
    CHECK(esp_spi::read(chip,regs,flash,0x3ff433f8,value)==esp_spi::UNHANDLED);
    CHECK(esp_spi::write(chip,regs,flash,0x3ff433f8,0)==esp_spi::UNHANDLED);
    CHECK(esp_spi::read(esp::CHIP_ESP8266,regs,flash,0x600005fc,value)==esp_spi::UNHANDLED);
    CHECK(esp_spi::external_state_register(0xff800000) && esp_spi::external_state_register(0xff800004) && esp_spi::external_state_register(0xff800008));
    CHECK(esp_spi::external_state_register(0xff920000) && esp_spi::external_state_register(0xff920004) && esp_spi::external_state_register(0xff920008));
    CHECK(!esp_spi::external_state_register(0xff80000c) && !esp_spi::external_state_register(0xff92000c));
    CHECK(!esp_spi::external_state_register(0x3ff42008) && !esp_spi::external_state_register(0xff910000));
    return true;
}
}

int esp_spi_tests()
{
    checks=0;
    if (!controller(esp::CHIP_ESP8266) || !controller(esp::CHIP_ESP32) || !esp32_address_phase() || !nor_protocol() || !esp32_chip_select() || !esp32_psram_transactions() || !esp32_psram_clock_control()) return -1;
    return checks;
}

#ifdef ESP_SPI_TEST_MAIN
int main() { int n=esp_spi_tests(); printf("ESP SPI: %d checks\n",n); return n<0?1:0; }
#endif
