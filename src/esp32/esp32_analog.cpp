#include "esp32/esp32_analog.h"
#include "esp/esp_state.h"
#include "esp/esp_rf_math.h"
#include <stdio.h>

namespace {
const u32 BASE=0x3ff4e000,GATE=BASE+0x44,CONFIG=BASE+0xc4,READ_DATA=BASE+0xc0,WRITE_DATA=BASE+0x148;
const u32 FREQUENCY_STATE=BASE+0x168;
const u32 TRIM=esp_state::ANALOG+0x1000;
const u32 RFPLL=esp_state::ANALOG+0x2000,SDM=esp_state::ANALOG+0x2100;
const u32 RFTX=esp_state::ANALOG+0x2200;
const u32 BBTOP=esp_state::ANALOG+0x2300,RXBB=esp_state::ANALOG+0x2400,BIAS=esp_state::ANALOG+0x2500;
const u32 RFRX=esp_state::ANALOG+0x2600;
const u32 PLL_ARMED=RFPLL+0x80,PLL_DEADLINE=RFPLL+0x84,PLL_DEADLINE_HIGH=RFPLL+0x88,PLL_TARGET=RFPLL+0x8c;
bool sequence_word(u32 address) {
    /* Espressif bt_i2c_write_set packs addresses, host selectors and masks
     * into these words; bt_i2c_set_wifi_data packs the five payload words.
     * They are program storage. Writing them does not start a transaction. */
    switch(address-BASE) {
    case 0xc8:case 0xcc:case 0xd0:case 0xd4:case 0xd8:case 0xdc:
    case 0xe0:case 0xe4:case 0xe8:case 0xec:case 0xf0:case 0xf4:
    case 0x100:case 0x104:case 0x108:case 0x10c:case 0x110:
    case 0x114:case 0x118:case 0x11c:case 0x120:case 0x124:case 0x128:case 0x12c:
    case 0x130:case 0x134:case 0x138:case 0x13c:case 0x140:case 0x164:
        return true;
    default:return false;
    }
}
u32 detector_config_mask(u32 address) {
    /* Named libphy ram_en_pwdet/ram_tx_pwctrl_bg_init fields. The separate
     * SAR request bits are deliberately excluded until conversion is modeled. */
    if(address==BASE+0x50)return 0xc1;
    if(address==BASE+0x5c)return 0x00a00000;
    if(address==BASE+0x60)return 2;
    return 0;
}
u32 canonical(u32 address) {
    return address>=0x6000e000 && address<=0x6000e168?BASE+(address-0x6000e000):address;
}
u32 stored(const std::map<u32,u32> &r,u32 address,u32 initial=0) {
    std::map<u32,u32>::const_iterator i=r.find(address);return i==r.end()?initial:i->second;
}
bool command(u32 address){return address>=BASE && address<=BASE+16 && !(address&3);}
esp32_analog::Result fail(std::string &error,const char *message) {
    error=message;return esp32_analog::ERROR;
}
void pll_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,PLL_DEADLINE)|(u64(stored(r,PLL_DEADLINE_HIGH))<<32);
    if(deadline && now>=deadline) {
        r[RFPLL+7*4]=0x80;r[RFPLL+5*4]=stored(r,PLL_TARGET)>>4;
        r[RFPLL+6*4]=stored(r,PLL_TARGET)&15;
        r.erase(PLL_DEADLINE);r.erase(PLL_DEADLINE_HIGH);
    }
}
void pll_control(std::map<u32,u32> &r,u32 data,u64 now,u32 xtal_khz) {
    const u32 old=stored(r,RFPLL);
    if(data&0x40) {
        r[RFPLL+5*4]=r[RFPLL+6*4]=r[RFPLL+7*4]=0;
        r.erase(PLL_DEADLINE);r.erase(PLL_DEADLINE_HIGH);
        if(!(old&0x40))r[PLL_ARMED]=0;
        if((data&0x20) && !(old&0x20))r[PLL_ARMED]=1;
    } else if((old&0x40) && stored(r,PLL_ARMED)) {
        u32 khz=0;unsigned code=0;
        /* ROM restart_cal holds reset6, pulses trigger5, then releases reset.
         * The shared functional oscillator finishes in one 20us SDK polling
         * interval. Unconfigured/out-of-range dividers cannot report lock. */
        if((stored(r,SDM,0x17)&16) && r.find(SDM+12)!=r.end() && r.find(SDM+16)!=r.end() && r.find(SDM+20)!=r.end() &&
           esp_rf_math::pll_frequency_khz((u8)stored(r,SDM+12),(u8)stored(r,SDM+16),
               (u8)stored(r,SDM+20),xtal_khz,khz) &&
           esp_rf_math::vco_capacitor_code(khz,2437000,2048,250,code,4095)) {
            /* get_rf_freq_cap reads coarse byte5 and fine nibble6. The
             * virtual oscillator's4MHz coarse step has sixteen250kHz trims. */
            r[PLL_TARGET]=code;const u64 deadline=now+20;
            r[PLL_DEADLINE]=(u32)deadline;r[PLL_DEADLINE_HIGH]=(u32)(deadline>>32);
        }
        r[PLL_ARMED]=0;
    }
    r[RFPLL]=data;
}
}

namespace esp32_analog {
Result read(const std::map<u32,u32> &r,u32 address,u32 &value)
{
    address=canonical(address);
    if(address==READ_DATA){value=stored(r,TRIM+(stored(r,CONFIG)&255)*4);return OK;}
    if(address==GATE){value=stored(r,address,0x1ff00);return OK;}
    if(detector_config_mask(address) || sequence_word(address)){value=stored(r,address);return OK;}
    /* phy_dis_hw_set_freq polls BUSY31 before disabling automatic changes.
     * No accepted command currently starts the frequency sequencer: explicit
     * requests fail below, so this is idle state, not fabricated completion. */
    if(address==FREQUENCY_STATE){value=0;return OK;}
    if(command(address) || address==CONFIG || address==WRITE_DATA){value=stored(r,address);return OK;}
    return UNHANDLED;
}

Result write(std::map<u32,u32> &r,u32 address,u32 value,std::string &error,u64 now,u32 xtal_khz)
{
    address=canonical(address);
    if(address==READ_DATA)return fail(error,"ESP32 analog trim read port is read-only");
    if(address==FREQUENCY_STATE)return fail(error,"ESP32 frequency sequencer status is read-only");
    if(address==GATE){r[address]=value&0x3ff00;return OK;}
    if(sequence_word(address)){r[address]=value;return OK;}
    const u32 detector_mask=detector_config_mask(address);
    if(detector_mask) {
        if((address==BASE+0x50 && (value&2)) || (address==BASE+0x60 && (value&1)))
            return fail(error,"ESP32 power-detector SAR conversion is unsupported");
        if(value&~detector_mask)return fail(error,"unsupported ESP32 power-detector control");
        r[address]=value;return OK;
    }
    if(address==WRITE_DATA){r[address]=value;return OK;}
    if(address==CONFIG) {
        /* Fields exercised by Espressif libphy i2cmst_reg_init, and the
         * indexed trim-memory transaction in wr_rf_freq_mem/bt_opt_write_mem.
         * Unknown controls fail instead of silently enabling unmodeled work. */
        if(value&~0x63ff7fffu)return fail(error,"unsupported ESP32 analog controller configuration");
        if(value&0x100)return fail(error,"ESP32 hardware frequency transition is unsupported");
        if((value&0x200) && !(stored(r,address)&0x200))r[TRIM+(value&255)*4]=stored(r,WRITE_DATA);
        r[address]=value;return OK;
    }
    if(!command(address))return UNHANDLED;
    if(value&0xfc000000u)return fail(error,"unsupported ESP32 analog command bits");
    /* ROM i2c_master_reset clears a host command independently of its cells.
     * Other commands encode block[7:0], register[15:8], data[23:16], WRITE24
     * and BUSY25. Exact device controls are decoded below; measurements use
     * explicit models instead of treating unknown cells as byte stores. */
    if(!value){r[address]=0;return OK;}
    const unsigned block=value&255,reg=(value>>8)&255;
    u32 cell=0;
    if(address==BASE+16 && block==0x66 && reg<=12) {
        if(stored(r,GATE,0x1ff00)&0x20000)return fail(error,"ESP32 BBPLL analog bus disabled");
        cell=esp_state::ANALOG+reg*4;
    } else if(address==BASE && block==0x63 && (reg<=1 || (reg>=3 && reg<=5)))cell=SDM+reg*4;
    /* chip_v7_rxmax_ext_ana supplies LNA/VGA capacitor trims; it does not
     * request a measurement or provide a hardware-generated result. */
    else if(address==BASE && block==0x64 && (reg==4 || reg==7))cell=RFRX+reg*4;
    /* i2c_rftx_init and ram_set_txcap_reg: named transmitter configuration
     * and capacitor controls. Other cells/measurement modes are rejected. */
    else if(address==BASE+8 && block==0x6b && ((reg>=1 && reg<=7) || reg==10))cell=RFTX+reg*4;
    /* i2c_bbtop_init/filter setup, RXBB power/override configuration and
     * i2c_bias_init voltage trims. Do not accept continuous-TX or RC-measurement
     * registers just because they share an analog device address. */
    else if(address==BASE+4 && block==0x67 && ((reg>=5 && reg<=9) || reg==11 || reg==12 || reg==15))cell=BBTOP+reg*4;
    else if(address==BASE+12 && block==0x68 && reg<=1) {
        if((value&0x01000000) && reg==1 && (value&0x00200000))return fail(error,"ESP32 RXBB RC calibration is unsupported");
        cell=RXBB+reg*4;
    }
    else if(address==BASE+8 && block==0x6a && (reg==0 || reg==2))cell=BIAS+reg*4;
    /* get_rf_freq_init programs manual capacitor controls1/2 independently
     * of the reset/trigger in0; merely selecting a cap cannot report lock. */
    else if(address==BASE+4 && block==0x62 && reg<=10) {
        if((value&0x01000000) && (reg==5 || reg==6 || reg==7))return fail(error,"ESP32 RFPLL calibration result is read-only");
        cell=RFPLL+reg*4;
    } else {
        char detail[100];snprintf(detail,sizeof(detail),"unsupported ESP32 analog register host%u block0x%02x reg%u",(address-BASE)/4,block,reg);
        return fail(error,detail);
    }
    pll_update(r,now);
    if(value&0x01000000) {
        const u32 data=(value>>16)&255;
        if(cell==SDM) {
            /* ROM writes0x07, divider bytes, then0x17. Holding the divider
             * cannot produce RFPLL lock. Other SDM modes are not modeled. */
            if(data&~0x17u)return fail(error,"unsupported ESP32 RFSDM control");
            if(!(data&16)) {
                r[RFPLL+7*4]=0;r.erase(PLL_DEADLINE);r.erase(PLL_DEADLINE_HIGH);
            }
        }
        if(cell==RFPLL)pll_control(r,data,now,xtal_khz);else r[cell]=data;
    } else value=(value&~0xff0000u)|(stored(r,cell,cell==SDM?0x17:0)<<16);
    r[address]=value&0x01ffffff;return OK;
}
}
