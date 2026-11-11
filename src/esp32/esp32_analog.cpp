#include "esp32/esp32_analog.h"
#include "esp/esp_state.h"
#include "esp/esp_rf_math.h"
#include <stdio.h>

namespace {
const u32 BASE=0x3ff4e000,GATE=BASE+0x44,TXDC_CONTROL=BASE+0x4c,CONFIG=BASE+0xc4,READ_DATA=BASE+0xc0,WRITE_DATA=BASE+0x148;
const u32 SAR_DOUT_BASE=BASE+0x80,SAR_DOUT_END=BASE+0x9c;
const u32 FREQUENCY_STATE=BASE+0x168;
const u32 TRIM=esp_state::ANALOG+0x1000;
const u32 RFPLL=esp_state::ANALOG+0x2000,SDM=esp_state::ANALOG+0x2100;
const u32 RFTX=esp_state::ANALOG+0x2200;
const u32 BBTOP=esp_state::ANALOG+0x2300,RXBB=esp_state::ANALOG+0x2400,BIAS=esp_state::ANALOG+0x2500;
const u32 RFRX=esp_state::ANALOG+0x2600;
const u32 PLL_ARMED=RFPLL+0x80,PLL_DEADLINE=RFPLL+0x84,PLL_DEADLINE_HIGH=RFPLL+0x88,PLL_TARGET=RFPLL+0x8c;
const u32 RC_DEADLINE=BIAS+0x80,RC_DEADLINE_HIGH=BIAS+0x84;
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
    /* Named libphy ram_en_pwdet/ram_tx_pwctrl_bg_init/ram_get_fm_sar_dout fields. */
    if(address==BASE+0x50)return 0xd3;
    if(address==BASE+0x54)return 0xffffffff;
    if(address==BASE+0x5c)return 0x00a00000;
    if(address==BASE+0x60)return 2;
    return 0;
}
bool sar_dout_word(u32 address) {
    return address>=SAR_DOUT_BASE && address<=SAR_DOUT_END && !(address&3);
}
u32 canonical(u32 address) {
    return address>=0x6000e000 && address<=0x6000e168?BASE+(address-0x6000e000):address;
}
u32 stored(const std::map<u32,u32> &r,u32 address,u32 initial=0) {
    std::map<u32,u32>::const_iterator i=r.find(address);return i==r.end()?initial:i->second;
}
bool command(u32 address){return address>=BASE && address<=BASE+16 && !(address&3);}
esp32_analog::Result fail(std::string &error,const std::string &message) {
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
    } else if( !(data&0x40) && (data&0x20) && (stored(r,RFPLL+2*4)&0x80)) {
        u32 khz=0;unsigned code=0;
        if((stored(r,SDM,0x17)&16) && r.find(SDM+12)!=r.end() && r.find(SDM+16)!=r.end() && r.find(SDM+20)!=r.end() &&
           esp_rf_math::pll_frequency_khz((u8)stored(r,SDM+12),(u8)stored(r,SDM+16),
               (u8)stored(r,SDM+20),xtal_khz,khz) &&
           esp_rf_math::vco_capacitor_code(khz,2437000,2048,250,code,4095)) {
            const unsigned target_cap=code>>4,ext_cap=stored(r,RFPLL+1*4)&255;
            r[RFPLL+5*4]=ext_cap>target_cap?2u:(ext_cap<target_cap?1u:0u);
            r[RFPLL+6*4]=code&15;r[RFPLL+7*4]=0x80;
        }
    }
    r[RFPLL]=data;
}
bool run_sequence(std::map<u32,u32> &r,u32 config,u64 now,u32 xtal_khz) {
    const unsigned count=(config>>10)&31;
    if(!(config&0x10000000u) || !count || count>20)return false;
    for(unsigned i=0;i<count;++i) {
        const unsigned host=(stored(r,BASE+0x100+(i>>3)*4)>>((i&7)*4))&15;
        const u32 pair_addr=BASE+(i<16?0xd8+(i>>1)*4:0x10c+((i-16)>>1)*4);
        const unsigned pair=(stored(r,pair_addr)>>((i&1)*16))&0xffff;
        const unsigned block=pair&255,reg=(pair>>8)&255;
        const u32 data=(stored(r,BASE+0x130+(i>>2)*4)>>((i&3)*8))&255;
        if(host==0 && block==0x63 && (reg<=1 || (reg>=3 && reg<=5))) {
            if(reg==0 && (data&~0x17u))return false;
            r[SDM+reg*4]=data;
        } else if(host==1 && block==0x62 && reg<=2) {
            r[RFPLL+reg*4]=data;
            if(reg==0)pll_control(r,data,now,xtal_khz);
        } else if(host==0 && block==0x64 && (reg==4 || reg==7)) {
            r[RFRX+reg*4]=data;
        } else return false;
    }
    return true;
}
void rc_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,RC_DEADLINE)|(u64(stored(r,RC_DEADLINE_HIGH))<<32);
    if(deadline && now>=deadline) {
        r[BIAS+5*4]=0x20;
        r.erase(RC_DEADLINE);r.erase(RC_DEADLINE_HIGH);
    }
}
void rc_control(std::map<u32,u32> &r,u32 data,u64 now) {
    const u32 old=stored(r,BIAS+4*4);
    if(!(data&1) || !(data&8)) {
        r.erase(RC_DEADLINE);r.erase(RC_DEADLINE_HIGH);
        if(!(data&8))r[BIAS+5*4]=0;
    } else if(!(old&8) && (stored(r,RXBB+4)&0x20) && (stored(r,BIAS+6*4)&0x10) &&
              ((stored(r,BIAS+4)>>4)&3)==2) {
        const u64 deadline=now+50;
        r[RC_DEADLINE]=(u32)deadline;r[RC_DEADLINE_HIGH]=(u32)(deadline>>32);
    }
    r[BIAS+4*4]=data;
}

void sar_convert(std::map<u32,u32> &r,u32 control) {
    if((control&0x10) && (stored(r,RFTX+9*4)&0x80)) {
        for(unsigned i=0;i<8;++i)r[SAR_DOUT_BASE+i*4]=(~0x0d33u)&0xfff;
        return;
    }
    const u32 tone=stored(r,0x3ff460b8);
    const int raw_atten=(tone&0x40000)?((0-int((tone>>10)&255))&255):80;
    const int atten=raw_atten>128?0:(raw_atten>120?120:raw_atten);
    const unsigned target_db1=atten<=49?414u:unsigned(610-4*atten);
    const unsigned a8=target_db1/48<1?1:(target_db1/48>8?8:target_db1/48);
    const unsigned rem=target_db1%48;
    const unsigned m=(rem*16+24)/48>15?15:(rem*16+24)/48;
    const unsigned p1=(16+m)<<(a8-1);
    const unsigned dout[8]={100,100,100+p1/2,100+(p1-p1/2),2100,2100,100,100};
    for(unsigned i=0;i<8;++i)r[SAR_DOUT_BASE+i*4]=(~dout[i])&0xfff;
}

}

namespace esp32_analog {
Result read(const std::map<u32,u32> &r,u32 address,u32 &value)
{
    address=canonical(address);
    if(address==READ_DATA){value=stored(r,TRIM+(stored(r,CONFIG)&255)*4);return OK;}
    if(address==GATE){value=stored(r,address,0x1ff00);return OK;}
    if(address==TXDC_CONTROL){value=stored(r,address);return OK;}
    if(sar_dout_word(address)) {
        if(r.find(address)==r.end())return UNHANDLED;
        value=stored(r,address);return OK;
    }
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
    if(sar_dout_word(address))return fail(error,"ESP32 SAR readout is read-only");
    if(address==GATE){r[address]=value&0x3ff00;return OK;}
    if(address==TXDC_CONTROL) {
        const u32 old=stored(r,address)&0x00ffffffu;
        value&=0x00ffffffu;
        if(value && (value&~3u)!=0x00113cf0u)return fail(error,"unsupported ESP32 TXDC calibration control");
        if((value&3)==3 && (old&3)==1) {
            const u32 b2=stored(r,0x3ff460ac),b3=stored(r,0x3ff460b0);
            const int i_dac=int(((b2>>9)&511)-256)*8+int((b2&511)-256);
            const int q_dac=int(((b3>>18)&511)-256)*8+int(((b3>>9)&511)-256);
            value|=0x01000000u|(i_dac>=19?0x80000000u:0u)|(q_dac>=-13?0x40000000u:0u);
        }
        r[address]=value;return OK;
    }
    if(sequence_word(address)){r[address]=value;return OK;}
    const u32 detector_mask=detector_config_mask(address);
    if(detector_mask) {
        if(value&~detector_mask)return fail(error,"unsupported ESP32 power-detector control");
        if(address==BASE+0x50 && (value&2))sar_convert(r,value);
        r[address]=value;return OK;
    }
    if(address==WRITE_DATA){r[address]=value;return OK;}
    if(address==CONFIG) {
        /* Fields exercised by Espressif libphy i2cmst_reg_init, and the
         * indexed trim-memory transaction in wr_rf_freq_mem/bt_opt_write_mem.
         * Unknown controls fail instead of silently enabling unmodeled work. */
        if(value&~0x7bff7fffu)return fail(error,"unsupported ESP32 analog controller configuration");
        if(value&0x100) {
            if(!run_sequence(r,value,now,xtal_khz))
                return fail(error,"ESP32 hardware frequency transition is unsupported");
            value&=~0x100u;
        }
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
    else if(address==BASE+8 && block==0x6b && ((reg>=1 && reg<=7) || reg==9 || reg==10))cell=RFTX+reg*4;
    /* i2c_bbtop_init/filter setup, RXBB power/override configuration and
     * i2c_bias_init/rc_cal trims. */
    else if(address==BASE+4 && block==0x67 && (reg<=12 || reg==14 || reg==15))cell=BBTOP+reg*4;
    else if(address==BASE+12 && block==0x68 && reg<=1)cell=RXBB+reg*4;
    else if(address==BASE+8 && block==0x6a && (reg<=2 || (reg>=4 && reg<=6))) {
        if((value&0x01000000) && reg==5)return fail(error,"ESP32 BIAS RC calibration result is read-only");
        cell=BIAS+reg*4;
    }
    /* get_rf_freq_init programs manual capacitor controls1/2 independently
     * of the reset/trigger in0; merely selecting a cap cannot report lock. */
    else if(address==BASE+4 && block==0x62 && reg<=10) {
        if((value&0x01000000) && (reg==5 || reg==6 || reg==7))return fail(error,"ESP32 RFPLL calibration result is read-only");
        cell=RFPLL+reg*4;
    } else {
        return fail(error,strf("unsupported ESP32 analog register host%u block0x%02x reg%u",(address-BASE)/4,block,reg));
    }
    pll_update(r,now);rc_update(r,now);
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
        if(cell==RXBB+4 && !(data&0x20)){r.erase(RC_DEADLINE);r.erase(RC_DEADLINE_HIGH);}
        if(cell==RFPLL)pll_control(r,data,now,xtal_khz);
        else if(cell==BIAS+4*4)rc_control(r,data,now);
        else r[cell]=data;
    } else value=(value&~0xff0000u)|(stored(r,cell,cell==SDM?0x17:0)<<16);
    r[address]=value&0x01ffffff;return OK;
}
}
