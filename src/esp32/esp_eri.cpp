#include "esp/esp_machine.h"

/* External Register Interface is a separate Xtensa bus, not a RAM alias.
 * ESP32 uses OCD DCR to detect an attached debugger during early startup.
 * Definitions: ESP-IDF components/xtensa/include/xtensa/xdm-regs.h.
 * We model idle debugger control/data state, not host instruction injection. */
bool EspMachine::external_read(u32 address,u32 &value)
{
    if(chip_!=esp::CHIP_ESP32)return fault("unsupported Xtensa external read",address);
    const u32 bank=(active_cpu().special(XT_SR_PRID)&0x2000)?0x01000000:0;
    const u32 control=bank|0x10200c,status=bank|0x102010,data=bank|0x102014;
    switch(address) {
    case 0x102008:case 0x10200c:value=register_value(control);return true;
    case 0x102010:value=0xc0000000u|register_value(status);return true; // core/debug powered, nohostpending
    case 0x102014:
        value=register_value(data);registers_[status]|=0x800;return true;
    case 0x103028:value=0;return true; // no outstanding synchronous ERI transaction
    default:return fault("unsupported Xtensa external read",address);
    }
}

bool EspMachine::external_write(u32 address,u32 value)
{
    if(chip_!=esp::CHIP_ESP32)return fault("unsupported Xtensa external write",address);
    const u32 bank=(active_cpu().special(XT_SR_PRID)&0x2000)?0x01000000:0;
    const u32 control=bank|0x10200c,status=bank|0x102010,data=bank|0x102014;
    switch(address) {
    case 0x102008:registers_[control]=register_value(control)&~value;return true;
    case 0x10200c:
        if(value&0x4a)return fault("unsupported Xtensa debugger run control",address);
        registers_[control]=register_value(control)|(value&0x03770035u);return true;
    case 0x102010:registers_[status]=register_value(status)&~(value&0xcc0b);return true;
    case 0x102014:registers_[data]=value;registers_[status]|=0x400;return true;
    default:return fault("unsupported Xtensa external write",address);
    }
}
