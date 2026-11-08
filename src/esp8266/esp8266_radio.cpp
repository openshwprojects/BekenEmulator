#include "esp8266/esp8266_radio.h"
#include "esp/esp_rf_math.h"
#include "esp/esp_state.h"

namespace esp8266_radio {
namespace {
u32 get(const std::map<u32,u32> &regs,u32 address)
{
    std::map<u32,u32>::const_iterator found=regs.find(address);
    return found==regs.end()?0:found->second;
}
u64 wide(const std::map<u32,u32> &regs,u32 address)
{
    return get(regs,address)|(u64(get(regs,address+4))<<32);
}
}

void advance(std::map<u32,u32> &regs,u64 now_us)
{
    /* Shared with the analog I2C status/result reads: elapsed guest time,
     * rather than reading LOCK, completes an already-triggered calibration. */
    const u64 deadline=wide(regs,0xffff2004);
    if(deadline && now_us>=deadline) {
        regs[0xfe040762]=0x80;
        regs[0xfe040562]=get(regs,0xffff2014);
        regs[0xffff2004]=regs[0xffff2008]=0;
    }
}

State snapshot(std::map<u32,u32> &regs,unsigned xtal_mhz,u64 now_us)
{
    advance(regs,now_us);
    State state;
    state.generation=wide(regs,esp_state::RADIO_8266);
    if(regs.count(0xfe000363) && regs.count(0xfe000463) && regs.count(0xfe000563)) {
        /* ROM rom_write_rfpll_sdm400078dc programs host0/block63/reg3..5.
         * SDK ram_rfpll_set_freq includes small crystal-frequency trims.
         * Classify only within half a MHz of a standard channel center;
         * this is channel identification, not a receiver bandwidth model. */
        esp_rf_math::pll_frequency_khz((u8)get(regs,0xfe000363),
            (u8)get(regs,0xfe000463),(u8)get(regs,0xfe000563),xtal_mhz*1000,state.frequency_khz);
        for(unsigned channel=1;channel<=14;++channel) {
            const u32 center=channel==14?2484000:2407000+channel*5000;
            const u32 distance=state.frequency_khz>center?state.frequency_khz-center:center-state.frequency_khz;
            if(distance<=500) { state.channel=channel;break; }
        }
    }
    state.pll_enabled=(get(regs,0xfe000063)&16)!=0; // ROM SDM control7 ->23
    const u32 rf=get(regs,0x600005c8);
    state.power_blocked=(rf&0x00030000)!=0; // pm_open_rf clears17:16
    state.channel_tuning=(rf&0x00f00000)!=0; // ram_set_channel_freq brackets23:20
    state.calibration_held=(get(regs,0xfe040062)&64)!=0;
    state.calibration_pending=wide(regs,0xffff2004)!=0;
    /* SDK stop_dig_rx sets9b08[27] and clears9a2c[19]; start_dig_rx
     * clears the stop gate and restores the saved enable/configuration. */
    state.digital_rx_enabled=!(get(regs,0x60009b08)&(1u<<27)) && (get(regs,0x60009a2c)&(1u<<19));
    state.mac_rx_enabled=(get(regs,0x3ff20004)&(1u<<31))!=0;
    return state;
}

void note_write(std::map<u32,u32> &regs,u32 address,u32 old_value,u32 value)
{
    u32 mask=0;
    switch(address) {
    case 0xfe000063:mask=16;break;
    case 0xfe000363:case 0xfe000463:case 0xfe000563:mask=255;break;
    case 0xfe040062:mask=0x60;break;
    case 0x600005c8:mask=0x00f30000;break;
    case 0x60009b08:mask=1u<<27;break;
    case 0x60009a2c:mask=1u<<19;break;
    case 0x3ff20004:mask=1u<<31;break;
    }
    if((old_value^value)&mask) {
        const u64 generation=wide(regs,esp_state::RADIO_8266)+1;
        regs[esp_state::RADIO_8266]=(u32)generation;
        regs[esp_state::RADIO_8266+4]=(u32)(generation>>32);
    }
}
}
