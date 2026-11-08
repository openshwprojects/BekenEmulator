#ifndef BK_ESP8266_INTERRUPTS_H
#define BK_ESP8266_INTERRUPTS_H

#include "esp8266/esp8266_wdev.h"
#include "esp/esp_state.h"
#include "arch/xtensa/xtensa_cpu.h"

namespace esp8266_interrupts {

/* The RTOS MAC driver masks native IRQ0 and enables the DPORT NMI gate
 * with low five bits=1. Its NMI vector calls wDev_ProcessFiq, which clears
 * DPORT0, and rearms it immediately before RFI3. NONOS uses native IRQ0.
 * Low five bits=15 selects the independent FRC1 NMI path instead. */
inline void update_mac(esp8266_wdev::Registers &registers,XtensaCpu &cpu)
{
    const bool level=esp8266_wdev::interrupt(registers);
    cpu.set_irq(0,level);
    const u32 key=esp_state::WDEV_8266+8;
    const esp8266_wdev::Registers::const_iterator saved=registers.find(key);
    const bool previous=saved!=registers.end() && saved->second!=0;
    if(!level && !previous)return;
    const esp8266_wdev::Registers::const_iterator entry=registers.find(0x3ff00000);
    const u32 select=entry==registers.end()?0:entry->second&31;
    const bool nmi=select==1 && level;
    /* NMI is edge-triggered and is not masked even inside its handler.
     * Reasserting its CPU latch every instruction would nest forever before
     * wDev_ProcessFiq can acknowledge the source. */
    if(nmi!=previous) {
        cpu.set_irq(14,nmi);
        if(nmi)registers[key]=1;else registers.erase(key);
    }
}

}
#endif
