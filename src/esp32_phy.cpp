#include "esp32_phy.h"
#include "esp_state.h"
#include "esp_rf_math.h"
namespace {
const u32 NF_CONTROL=0x3ff5c018,NF_RESULT=0x3ff5c050;
const u32 NF_STATE=esp_state::PHY_ESP32,NF_DEADLINE=NF_STATE+4;
const u32 NF_PENDING=NF_STATE+12,NF_AMPLITUDE=NF_STATE+16;
const u32 PBUS_COMMAND=0x3ff46094,PBUS_STATUS=0x3ff460a0;
const u32 PBUS_DEADLINE=esp_state::PHY_ESP32+0x100,PBUS_PENDING=PBUS_DEADLINE+8;
struct Register { u32 address, supported, reset_line; };
/* Exact controls established by Espressif's classic ESP32 libphy routines
 * disable_wifi_agc, enable_wifi_agc, set_cca and agc_reg_init. They configure
 * the receiver, not measurement-completion or packet-reception status.
 * Unconfigured controls start clear in this functional receiver model;
 * these values are not a claim about undocumented silicon reset defaults. */
const Register controls[] = {
    {0x3ff5c004,0x00010000,1},
    {0x3ff5c018,0x52800007,1}, /* CCA/AGC plus noise enable23/reset25/auto28; DONE24 is RO */
    {0x3ff5c01c,0x00ff00ff,1}, /* AGC threshold[23:16], signed CCA threshold[7:0] */
    /* agc_reg_init supplies a complete configuration word here; mode[5:4]
     * and force-disable8 are also independently controlled by ram_*_agc. */
    {0x3ff5c030,0xffffffff,1},
    {0x3ff5c038,0x0c000000,1}, /* AGC control bits used by enable/init */
    {0x3ff5c080,0x00000001,1}, /* force gain-control hold */
    {0x3ff5c0a4,0xffffffff,1}, /* older agc_reg_init writes complete gain config */
    {0x3ff5c0c4,0xffffffff,1}, /* four AGC thresholds, programmed as one word */
    {0x3ff5c094,0xffffffff,1},
    {0x3ff5c02c,0xffffffff,1}, /* receiver configuration including init toggle23 */
    {0x3ff5c028,0x000001ff,1},
    {0x3ff5c0f8,0x0003fe00,1},
    {0x3ff5c088,0x000000ff,1},
    {0x3ff5c104,0x00008000,1},
    {0x3ff5c07c,0x00ff0000,1},
    /* bb_reg_init programs complete digital filter words. The remaining
     * entries expose only fields read/modified by that routine. */
    {0x3ff5cc48,0xffffffff,1},
    {0x3ff5cce4,0x00400000,1},
    {0x3ff5c400,0xffffffff,1},
    {0x3ff5cd04,0xffffffff,1},
    {0x3ff5cd08,0xffffffff,1},
    {0x3ff5cc0c,0xffffffff,1},
    {0x3ff5cc08,0xffffffff,1},
    {0x3ff5ccdc,0xffffffff,1},
    {0x3ff5c044,0x01ff0000,1},
    {0x3ff5c024,0x0003fe00,1},
    {0x3ff5cc04,0x000001fc,1},
    {0x3ff5ccd8,0xffffffff,1},
    {0x3ff5d000,0x1ffff800,1}, /* tx_paon_set PA-on timing fields */
    {0x3ff5d03c,0xffffffff,1}, /* bb_wdg_cfg supplies complete threshold word */
    {0x3ff5d040,0xe0000000,1}, /* watchdog reset/IRQ enables, timeout-clear */
    {0x3ff5d044,1,1}, /* rom_noise_floor_auto_set acquisition mode */
    {0x3ff5d050,1,1}, /* sample acknowledgement/rearm strobe */
    {0x3ff5ccb8,0xe0000000,1}, /* phy_set_bbfreq_init clock selection */
    {0x3ff5c85c,0x0000000f,1},
    {0x3ff5c11c,0xffffffff,1}, /* phy_ant_init complete antenna config words */
    {0x3ff5c120,0xffffffff,1},
    {0x3ff5c0a0,0x00ff0000,1}, /* opt_11b_resart receiver thresholds */
    {0x3ff5c0f0,0x000000ff,1},
    {0x3ff5c0b8,0x00000800,1},
    /* force_txrxoff, ram_set_rxclk_en/txclk_en, fe_reg_init: force/value
     * pairs[17:8] and configuration bit24. PBUS busy31 is read-only and
     * supplied by the timed command engine, not a retained control field. */
    {0x3ff460a0,0x0103ff00,2},
    {PBUS_COMMAND,0x0001ffff,2},
    /* Remaining exact fields programmed by fe_reg_init; FE and FE2 share
     * DPORT_FE_RST, which is independent of the Wi-Fi baseband reset. */
    {0x3ff45114,0x00000200,2},
    {0x3ff450dc,0xc0000000,2},
    {0x3ff450d8,0x02000000,2},
    {0x3ff460b8,0x20000000,2},
    {0x3ff4609c,0x00004014,2}, /* bit2/4 also used by phy_close_pa */
    {0x3ff4604c,0x0000ff00,2},
    {0x3ff46048,0x0000ff00,2}, /* tx_paon_set front-end delay */
    {0x3ff460c0,0x00000080,2}, /* ram_en_pwdet front-end route enable */
    /* set_chan_freq_hw_init programs upper16 to0x0c80 after preserving
     * lower16. No active MAC request or status is accepted here. */
    {0x3ff73d38,0xffff0000,4}
};
u32 canonical(u32 address) {
    if(address==0x60033d38)return 0x3ff73d38;
    if(address>=0x60005000 && address<0x60007000)return address-0x60005000+0x3ff45000;
    return address>=0x6001c000 && address<0x6001e000?address-0x6001c000+0x3ff5c000:address;
}
const Register *reg(u32 address) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].address==address)return &controls[i];
    return 0;
}
u32 stored(const std::map<u32,u32> &r,u32 address) {
    std::map<u32,u32>::const_iterator it=r.find(address);return it==r.end()?0:it->second;
}
void noise_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,NF_DEADLINE)|(u64(stored(r,NF_DEADLINE+4))<<32);
    if(deadline && now>=deadline) {
        r[NF_RESULT]=stored(r,NF_PENDING);r[NF_CONTROL]|=0x01000000;
        r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);
    }
}
void noise_cancel(std::map<u32,u32> &r) {
    r[NF_CONTROL]&=~0x01000000u;
    r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);
}
void noise_start(std::map<u32,u32> &r,u64 now) {
    noise_cancel(r);
    if((stored(r,NF_CONTROL)&0x02800000)!=0x00800000 || (stored(r,0x3ff000d0)&1))return;
    /* Functional 20MHz receiver: integrate128 deterministic I/Q samples.
     * The virtual board's variance8 is -96dBm. Timing/idle noise are explicit
     * model parameters, not undocumented silicon reset/calibration values. */
    const unsigned amplitude=r.find(NF_AMPLITUDE)==r.end()?3:stored(r,NF_AMPLITUDE);
    const unsigned range=amplitude*2+1;u64 energy=0;
    u32 state=stored(r,NF_STATE);if(!state)state=0x32adc001;
    for(unsigned i=0;i<128;++i) {
        state^=state<<13;state^=state>>17;state^=state<<5;
        const int a=int(state%range)-int(amplitude),b=int((state>>16)%range)-int(amplitude);
        energy+=a*a+b*b;
    }
    r[NF_STATE]=state;
    r[NF_PENDING]=esp_rf_math::logarithmic_power_code(double(energy)/128,8,640,4,1023);
    const u64 deadline=now+7;r[NF_DEADLINE]=(u32)deadline;r[NF_DEADLINE+4]=(u32)(deadline>>32);
}
bool pbus_cell(unsigned block,unsigned selector,u32 &address,unsigned &shift) {
    /* Real mask-ROM pbus_rd_addr/pbus_rd_shift tables. Blocks0/4/5 have
     * one cell; blocks1..3 have two. Do not alias invalid selectors. */
    if(selector<1 || selector>2 || block>5 || (selector==2 && (block==0 || block>=4)))return false;
    if(block==0){address=0x3ff460a8;shift=9;}
    else if(block==1){address=selector==1?0x3ff460a8:0x3ff460ac;shift=selector==1?0:18;}
    else if(block==2){address=0x3ff460ac;shift=selector==1?9:0;}
    else if(block==3){address=0x3ff460b0;shift=selector==1?18:9;}
    else {address=block==4?0x3ff460b0:0x3ff460b4;shift=0;}
    return true;
}
bool pbus_readback(u32 address) {
    return address>=0x3ff460a8 && address<=0x3ff460b4 && !(address&3);
}
bool pbus_update(std::map<u32,u32> &r,u64 now) {
    const u64 deadline=stored(r,PBUS_DEADLINE)|(u64(stored(r,PBUS_DEADLINE+4))<<32);
    if(!deadline)return false;
    if(now<deadline)return true;
    const u32 command=stored(r,PBUS_PENDING);u32 address=0;unsigned shift=0;
    if(pbus_cell((command>>2)&15,(command>>15)&3,address,shift))
        r[address]=(stored(r,address)&~(511u<<shift))|(((command>>6)&511)<<shift);
    r.erase(PBUS_DEADLINE);r.erase(PBUS_DEADLINE+4);r.erase(PBUS_PENDING);
    return false;
}
}
namespace esp32_phy {
Result read(std::map<u32,u32> &r,u32 address,u32 &value,u64 now) {
    address=canonical(address);
    if(address==PBUS_STATUS || address==PBUS_COMMAND || pbus_readback(address)) {
        const bool busy=pbus_update(r,now);value=stored(r,address);
        if(address==PBUS_STATUS && busy)value|=0x80000000;
        return OK;
    }
    if(address==NF_CONTROL || address==NF_RESULT) {
        noise_update(r,now);value=stored(r,address);return OK;
    }
    /* bb_wdt_get_status: this controller currently accepts configuration,
     * not active RX/TX state-machine commands. It therefore has no pending
     * operation to time out. Unsupported active operations still fail at
     * their command register; this is not an always-successful completion. */
    if(address==0x3ff5d008){value=0;return OK;}
    if(!reg(address))return UNHANDLED;
    value=stored(r,address);return OK;
}
Result write(std::map<u32,u32> &r,u32 address,u32 value,std::string &error,u64 now) {
    address=canonical(address);
    if(address==NF_RESULT){error="ESP32 receiver noise result is read-only";return ERROR;}
    if(pbus_readback(address)){error="ESP32 PBUS readback is read-only";return ERROR;}
    if(address==0x3ff5d008){error="ESP32 baseband watchdog status is read-only";return ERROR;}
    const Register *definition=reg(address);
    if(!definition)return UNHANDLED;
    if(stored(r,0x3ff000d0)&definition->reset_line){error="ESP32 radio controller is held in reset";return ERROR;}
    if(address==NF_CONTROL)value&=~0x01000000u; /* status cannot be supplied by guest */
    if(address==PBUS_STATUS)value&=~0x80000000u;
    if(value&~definition->supported){error="unsupported ESP32 radio control";return ERROR;}
    if(address==PBUS_COMMAND) {
        const bool busy=pbus_update(r,now);const u32 old=stored(r,address);
        if(busy && value!=old){error="ESP32 PBUS command is busy";return ERROR;}
        if((value&2) && !(old&2)) {
            u32 target=0;unsigned shift=0;
            if(!(value&1)){error="ESP32 PBUS command requires debug mode";return ERROR;}
            if(!pbus_cell((value>>2)&15,(value>>15)&3,target,shift)) {
                error="unsupported ESP32 PBUS block/register";return ERROR;
            }
            /* A functional serial configuration transfer occupies1us. The
             * receiver/transmitter cell changes only at completion. */
            const u64 deadline=now+1;r[PBUS_PENDING]=value;
            r[PBUS_DEADLINE]=(u32)deadline;r[PBUS_DEADLINE+4]=(u32)(deadline>>32);
        }
        r[address]=value;return OK;
    }
    if(address==NF_CONTROL) {
        noise_update(r,now);const u32 old=stored(r,address);
        r[address]=value|(old&0x01000000);
        if((value&0x02000000) || !(value&0x00800000))noise_cancel(r);
        else if(!(old&0x00800000) || (old&0x02000000))noise_start(r,now);
        return OK;
    }
    if(address==0x3ff5d050) {
        noise_update(r,now);if(value&1)noise_start(r,now);
        r[address]=0;return OK;
    }
    /* bb_wdt_timeout_clear writes a one-shot clear command without a second
     * software write. Keep its reset/interrupt enables independently. */
    if(address==0x3ff5d040)value&=~0x20000000u;
    r[address]=value;return OK;
}
void reset_baseband(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==1)r.erase(controls[i].address);
    r.erase(NF_RESULT);r.erase(NF_STATE);r.erase(NF_DEADLINE);r.erase(NF_DEADLINE+4);r.erase(NF_PENDING);
}
bool set_noise_amplitude(std::map<u32,u32> &r,unsigned amplitude) {
    if(amplitude>511)return false;
    r[NF_AMPLITUDE]=amplitude;return true;
}
bool agc_enabled(const std::map<u32,u32> &r) {
    return !(stored(r,0x3ff000d0)&1) && !(stored(r,0x3ff5c080)&1) && (stored(r,0x3ff5c030)&0x130)==0x10;
}
int cca_threshold(const std::map<u32,u32> &r) {
    unsigned threshold=stored(r,0x3ff5c01c)&255;return threshold>=128?(int)threshold-256:(int)threshold;
}
void reset_frontend(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==2)r.erase(controls[i].address);
    for(u32 address=0x3ff460a8;address<=0x3ff460b4;address+=4)r.erase(address);
    r.erase(PBUS_DEADLINE);r.erase(PBUS_DEADLINE+4);r.erase(PBUS_PENDING);
}
void reset_mac(std::map<u32,u32> &r) {
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);++i)if(controls[i].reset_line==4)r.erase(controls[i].address);
}
bool frontend_signal(const std::map<u32,u32> &r,unsigned shift,bool automatic) {
    if(shift<8 || shift>16 || (shift&1) || (stored(r,0x3ff000d0)&2))return false;
    const u32 field=(stored(r,0x3ff460a0)>>shift)&3;
    return field&2?(field&1)!=0:automatic;
}
}
