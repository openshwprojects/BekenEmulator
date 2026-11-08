#include "esp_machine.h"
#include "esp8266_interrupts.h"
#include "esp_spi.h"
#include "esp8266_watchdog.h"
#include "esp8266_wdev.h"
#include "esp8266_mac_timers.h"
#include "esp_rf_math.h"
#include <math.h>

namespace {
/* Private device state, outside the guest address map and analog register
 * storage. Resetting the machine clears these with the other devices. */
const u32 RFPLL_ARMED=0xffff2000u, RFPLL_DEADLINE_LO=0xffff2004u,
          RFPLL_DEADLINE_HI=0xffff2008u;
const u32 RFPLL_CONTROL=0xfe040062u, RFPLL_STATUS=0xfe040762u,
          RFPLL_RESULT=0xfe040562u, RFPLL_TARGET=0xffff2014u;
const u32 RX_NOISE_STATE=0xffff2010u;
const u32 RC_CONFIG=0xfe08046au, RC_ENABLE=0xfe08036au,
          RC_REFERENCE=0xfe08066au, RC_POWER=0xfe0c0168u,
          RC_RESULT=0xfe08056au, RC_DEADLINE=0xffff2020u, RC_TARGET=0xffff2028u;
const u32 DC_COMMAND=0x60000d4cu, DC_RESULT=0xffff2030u, DC_DEADLINE=0xffff2034u;
const u32 SAR_CONTROL=0x60000d50u, SAR_DEADLINE=0xffff2040u,
          SAR_PENDING_COUNT=0xffff204cu, SAR_PENDING_SAMPLE=0xffff2050u;
const u32 RX_GAIN_SELECT=0x60009a68u, RX_GAIN_RAM=0xfd000000u;
const u32 IQ_CONTROL=0x6000057cu, IQ_DEADLINE=0xffff2080u,
          IQ_PENDING_I=0xffff2088u, IQ_PENDING_Q=0xffff208cu, IQ_PENDING_POWER=0xffff2090u,
          IQ_PENDING_CORRELATION=0xffff2094u;
const u32 NF_CONTROL=0x60009b60u, NF_RESULT=0x60009b64u,
          NF_DEADLINE=0xffff20b0u, NF_PENDING=0xffff20b8u;

unsigned rf_detector_power(std::map<u32,u32> &regs,u32 tone);
u32 receiver_sample(std::map<u32,u32> &regs);

bool rx_gain_address(u32 address)
{
    return address>=0x60009e00u && address<=0x60009f64u && !(address&3);
}

bool rx_gain_read(std::map<u32,u32> &regs,u32 address,u32 &value,std::string &error)
{
    u32 select=(regs[RX_GAIN_SELECT]>>1)&255;
    if((select&15) && (select&240)) {
        error="RX gain RAM read has both word banks selected";return false;
    }
    u32 key=RX_GAIN_RAM+((address-0x60009e00u)/4)*8+(select&240?4:0);
    value=regs[key];return true;
}

void rx_gain_write(std::map<u32,u32> &regs,u32 address,u32 value)
{
    /* set_rx_gain_testchip_50 writes90 gain entries through one32-bit
     * window. Selector0x1e writes the first four byte lanes,0x1e0 the
     * second four. Preserve both words and honor individual write gates;
     * a plain address->word latch would silently lose half the table. */
    u32 select=regs[RX_GAIN_SELECT]>>1;
    u32 key=RX_GAIN_RAM+((address-0x60009e00u)/4)*8;
    for(unsigned bank=0;bank<2;++bank) {
        u32 mask=0;
        for(unsigned byte=0;byte<4;++byte)
            if(select&(1u<<(bank*4+byte)))mask|=255u<<(byte*8);
        if(mask)regs[key+bank*4]=(regs[key+bank*4]&~mask)|(value&mask);
    }
}

void set_analog_deadline(std::map<u32,u32> &regs,u32 key,u64 time)
{
    regs[key]=(u32)time;regs[key+4]=(u32)(time>>32);
}

u64 analog_deadline(std::map<u32,u32> &regs,u32 key)
{
    return regs[key]|(u64(regs[key+4])<<32);
}

void noise_update(std::map<u32,u32> &regs,u64 now)
{
    const u64 deadline=analog_deadline(regs,NF_DEADLINE);
    if(deadline && now>=deadline) {
        regs[NF_RESULT]=(regs[NF_RESULT]&0xfffffu)|(regs[NF_PENDING]<<20);
        regs[0x60009824u]=regs[NF_PENDING];regs[NF_CONTROL]&=~2u;
        set_analog_deadline(regs,NF_DEADLINE,0);
    }
}

void noise_start(std::map<u32,u32> &regs,u32 value,u64 now)
{
    noise_update(regs,now);
    const u32 old=regs[NF_CONTROL];
    if((value&1) && !(old&1))regs[0x60009800u]=0;
    if(!(value&2))set_analog_deadline(regs,NF_DEADLINE,0);
    else if(!(old&2)) {
        unsigned duration=1;
        regs[NF_PENDING]=regs[NF_RESULT]>>20;
        /* SDK ram_start_noisefloor enables automatic acquisition with
         * bits15/17 and starts it with bit1; completion clears bit1. Its
         * manual mode only applies the programmed threshold. The result
         * is a12-bit offset-binary value in eighth-dBm units, also exposed
         * at9824; SDK versions return either dBm or quarter-dBm from it. */
        if((value&0x28000u)==0x28000u) {
            const unsigned count=128u<<((regs[NF_RESULT]>>9)&7);
            unsigned energy=0;
            for(unsigned i=0;i<count;++i) {
                const u32 sample=receiver_sample(regs);
                int a=sample&1023,b=(sample>>10)&1023;
                if(a&512)a-=1024;
                if(b&512)b-=1024;
                energy+=a*a+b*b;
            }
            const unsigned tones=rf_detector_power(regs,regs[0x600005b8u])+
                rf_detector_power(regs,regs[0x600005bcu])+rf_detector_power(regs,regs[0x600005c4u]);
            const double power=double(energy)/count+tones*16;
            /* The virtual board's nominal idle variance8 corresponds to
             * -96dBm. Accumulate actual deterministic ADC noise and tone
             * power, then convert their ratio to logarithmic RF units. */
            int result=power>0?int(3328+80*log10(power/8)+0.5):0;
            if(result<0)result=0;
            if(result>4095)result=4095;
            regs[NF_PENDING]=u32(result);duration=(count+19)/20;
        }
        set_analog_deadline(regs,NF_DEADLINE,now+duration);
    }
    regs[NF_CONTROL]=value;
}

unsigned pbus_value(std::map<u32,u32> &regs,unsigned block,unsigned reg)
{
    static const u8 offsets[]={0,2,4,5,7,9,11,12};
    const unsigned index=offsets[block]+reg-1;
    const unsigned shift=index==12?0:18-9*(index%3);
    return (regs[0x600005a4u+(index/3)*4]>>shift)&511;
}

u32 iq_saturate(s64 value)
{
    if(value>0x7fffffff)return 0x7fffffffu;
    if(value<-s64(0x7fffffff)-1)return 0x80000000u;
    return u32(value);
}

void iq_correlations(std::map<u32,u32> &regs,unsigned count)
{
    const u32 packed=regs[0xfe001977u],sign=regs[0xfe001877u];
    int gain=packed&31,phase=((packed>>2)&56)|(regs[0xfe001a77u]&7);
    gain=(sign&32)?-gain:(gain&16?gain-32:gain);
    phase=(sign&8)?-phase:(phase&32?phase-64:phase);
    unsigned amplitude=0;
    const u32 tones[]={0x600005b8u,0x600005bcu,0x600005c4u};
    for(unsigned i=0;i<3;++i)if(regs[tones[i]]&0x40000u)amplitude+=regs[tones[i]]&1023;
    /* The calibration tone projects onto I*cos,Q*sin and the two cross
     * terms. A virtual receiver begins with a small gain/phase imbalance;
     * BIAS25/26 trims correct it. ROM rxiq_get_mis derives its correction
     * from the sums/differences of these four signed accumulators. This
     * deterministic transfer model preserves the guest's calibration loop. */
    const s64 base=s64(amplitude)*count*32;
    const int gain_error=2-gain,phase_error=phase+3;
    regs[IQ_PENDING_CORRELATION]=iq_saturate(base*(256+gain_error)/256);
    regs[IQ_PENDING_CORRELATION+4]=iq_saturate(base*phase_error/256);
    regs[IQ_PENDING_CORRELATION+8]=iq_saturate(base*phase_error/256);
    regs[IQ_PENDING_CORRELATION+12]=iq_saturate(base*(256-gain_error)/256);
}

u32 iq_control(std::map<u32,u32> &regs,u64 now)
{
    const u64 deadline=analog_deadline(regs,IQ_DEADLINE);
    if(deadline && now>=deadline) {
        regs[0x600005dcu]=regs[IQ_PENDING_I];regs[0x600005e0u]=regs[IQ_PENDING_Q];
        regs[0x600005e4u]=regs[IQ_PENDING_POWER];
        for(unsigned i=0;i<4;++i)regs[0x60000580u+i*4]=regs[IQ_PENDING_CORRELATION+i*4];
        regs[IQ_CONTROL]|=0x80000000u;set_analog_deadline(regs,IQ_DEADLINE,0);
    }
    return regs[IQ_CONTROL];
}

bool iq_start(std::map<u32,u32> &regs,u32 value,u64 now,std::string &error)
{
    const u32 old=iq_control(regs,now),control=value&0x7fffffffu;
    if(!(control&1)) {
        regs[IQ_CONTROL]=control;set_analog_deadline(regs,IQ_DEADLINE,0);return true;
    }
    if((control&2) && !(old&2)) {
        if(!(control&0x40000u)) {error="unsupported RF IQ correlation mode";return false;}
        /* ROM iq_est_enable uses power0, START1, sample count-1[16:2],
         * DC mode18 and DONE31. dc_iq_est divides the signed sums by64
         * and by the requested sample count. The virtual receiver has
         * small I/Q offsets at its nominal two-stage PBUS DC trims;
         * firmware must search those trims using the measured residual. */
        const unsigned count=((control>>2)&32767)+1;
        int inphase=int(pbus_value(regs,4,1))-260+int(pbus_value(regs,4,2))-256;
        int quadrature=int(pbus_value(regs,5,1))-253+int(pbus_value(regs,5,2))-256;
        if(inphase<-511)inphase=-511;
        if(inphase>511)inphase=511;
        if(quadrature<-511)quadrature=-511;
        if(quadrature>511)quadrature=511;
        regs[IQ_PENDING_I]=u32(s64(inphase)*count*64);
        regs[IQ_PENDING_Q]=u32(s64(quadrature)*count*64);
        /* Sum squared sample magnitudes, including the variance of the
         * two idle-noise channels and any enabled calibration tones. */
        const unsigned tones=rf_detector_power(regs,regs[0x600005b8u])+
            rf_detector_power(regs,regs[0x600005bcu])+rf_detector_power(regs,regs[0x600005c4u]);
        const u64 power=u64(inphase*inphase+quadrature*quadrature+8+tones*16)*count;
        regs[IQ_PENDING_POWER]=power>0x7fffffffu?0x7fffffffu:u32(power);
        iq_correlations(regs,count);
        /* Functional20Msample/s baseband acquisition, independent of the
         * CPU clock. Results latch at START and appear only at completion. */
        set_analog_deadline(regs,IQ_DEADLINE,now+(count+19)/20);
        regs[IQ_CONTROL]=control;
    } else regs[IQ_CONTROL]=control|(old&0x80000000u);
    return true;
}

u32 sar_control(std::map<u32,u32> &regs,u64 now)
{
    /* SDK readvdd33/read_sar_dout use an eight-sample reset batch. Fast
     * ADC acquisition explicitly reduces COUNT[4:2] to zero (one sample).
     * BUSY[26:24] and the inverted eleven-bit result bank are read-only.
     * Register layout is also documented by pvvx/esp8266web esp8266.h. */
    if(regs.find(SAR_CONTROL)==regs.end())regs[SAR_CONTROL]=0x1c;
    u64 deadline=analog_deadline(regs,SAR_DEADLINE);
    if(deadline && now>=deadline) {
        for(unsigned i=0;i<regs[SAR_PENDING_COUNT];++i)
            regs[0x60000d80u+i*4]=~regs[SAR_PENDING_SAMPLE+i*4];
        regs[SAR_CONTROL]&=~0x07000000u;
        set_analog_deadline(regs,SAR_DEADLINE,0);
    }
    return regs[SAR_CONTROL];
}

u32 sar_encode(unsigned corrected)
{
    if(corrected>2047)corrected=2047;
    unsigned low=corrected&255;
    unsigned rawlow=low?21+(low*256+278)/279:0;
    if(rawlow>255)rawlow=255;
    return (corrected&0x700)|rawlow;
}

unsigned rf_detector_power(std::map<u32,u32> &regs,u32 tone)
{
    if(!(tone&0x40000u))return 0;
    unsigned amplitude=tone&1023;
    /* A deterministic virtual three-stage TX filter: nominal trims8,8,4,
     * with a smooth loss away from resonance. Its square-law detector
     * measures actual programmed tone amplitude; firmware must perform its
     * capacitor search. This is a functional RF model, not silicon physics. */
    int first=int(regs[0xfe08016bu]&15)-8;
    int second=int(regs[0xfe08026bu]&15)-8;
    int third=int((regs[0xfe08026bu]>>4)&7)-4;
    unsigned loss=64+first*first+second*second+third*third;
    return amplitude*amplitude*4/loss;
}

bool sar_start(std::map<u32,u32> &regs,u32 value,u64 now,std::string &error)
{
    const u32 old=sar_control(regs,now),control=value&~0x07000000u;
    if((control&2) && !(old&2)) {
        const u32 route=regs[0x60000d5cu]&0x00a00000u;
        const bool external_power=(regs[0xfe08006cu]&0x20)!=0;
        unsigned corrected[8];
        if(external_power && route==0x00800000u && (regs[0xfe08096bu]&0x87)==0x80) {
            /* Nominal virtual board supply:3.3V. The SDK returns VDD in
             * units of1/1024V after summing eight corrected samples/4.
             * Model the corresponding11-bit SAR transfer, including its
             * subrange offset/slope that read_sar_dout compensates. */
            for(unsigned i=0;i<8;++i)corrected[i]=(3300u*512+500)/1000;
        } else if(external_power && route==0x00200000u && !(regs[0xfe08096bu]&0x80)) {
            for(unsigned i=0;i<8;++i)corrected[i]=0; // Default board ties TOUT low.
        } else if(!external_power && !route &&
                  ((regs[0x600005b8u]|regs[0x600005bcu]|regs[0x600005c4u])&0x40000u)) {
            /* ram_get_fm_sar_dout subtracts the reference slots6/7 from
             * three in-phase samples1..3 and quadrature slots4/5. Keep
             * those measured powers and the detector pedestal separate. */
            unsigned inphase=rf_detector_power(regs,regs[0x600005b8u]);
            unsigned quadrature=rf_detector_power(regs,regs[0x600005bcu]);
            unsigned common=rf_detector_power(regs,regs[0x600005c4u]);
            for(unsigned i=0;i<8;++i)
                corrected[i]=128+common+(i>=1 && i<=3?inphase:(i==4 || i==5?quadrature:0));
        } else {
            error="unsupported SAR analog input route";return false;
        }
        for(unsigned i=0;i<8;++i)regs[SAR_PENDING_SAMPLE+i*4]=sar_encode(corrected[i]);
        unsigned count=((control>>2)&7)+1,divider=(control>>8)&255;
        if(!divider)divider=20;
        /* Functional timing:12 SAR clocks/sample on the80MHz domain;
         * divide-by20 gives the SDK's24us eight-sample settling interval. */
        unsigned duration=(count*12*divider+79)/80;
        regs[SAR_PENDING_COUNT]=count;
        set_analog_deadline(regs,SAR_DEADLINE,now+duration);
        regs[SAR_CONTROL]=control|0x01000000u;
    } else regs[SAR_CONTROL]=control|(old&0x07000000u);
    return true;
}

void rc_calibration_write(std::map<u32,u32> &regs,u32 key,u32 value,u64 now)
{
    if((key==RC_POWER && !(value&1)) || (key==RC_ENABLE && !(value&0x40)) ||
       (key==RC_CONFIG && (value&9)!=9)) {
        regs[RC_RESULT]=0;set_analog_deadline(regs,RC_DEADLINE,0);
    }
    if(key==RC_CONFIG && (value&9)==9 && !(regs[key]&8) &&
       (regs[RC_POWER]&1) && (regs[RC_ENABLE]&0x40)) {
        /* SDK rc_cal enables the calibration oscillator, programs the
         * reference in6a/6, then pulses6a/4 bit3 and samples6a/5 after100us.
         * Model a nominal RC process with four measured counts per reference
         * step (the SDK's reference8 yields32). Keep this result dependent on
         * the programmed reference and the enabled measurement interval. */
        u32 count=(regs[RC_REFERENCE]&31)*4;
        regs[RC_TARGET]=count>63?63:count;
        regs[RC_RESULT]=0;set_analog_deadline(regs,RC_DEADLINE,now+100);
    }
}

u32 rc_calibration_result(std::map<u32,u32> &regs,u64 now)
{
    u64 deadline=analog_deadline(regs,RC_DEADLINE);
    if(deadline && now>=deadline) {
        regs[RC_RESULT]=regs[RC_TARGET];set_analog_deadline(regs,RC_DEADLINE,0);
    }
    return regs[RC_RESULT];
}

bool dc_measurement_write(std::map<u32,u32> &regs,u32 value,u64 now,std::string &error)
{
    const u32 control=value&0x00ffffffu;
    if(control && (control&~3u)!=0x00113cf0u) {
        error="unsupported SAR DC comparator configuration";return false;
    }
    const u32 old=regs[DC_COMMAND];
    if((control&3)!=3) {
        regs[DC_RESULT]=0;set_analog_deadline(regs,DC_DEADLINE,0);
    } else if(!(old&2)) {
        /* ram_cal_tos_v60 trims PBUS0/1 register2 with a binary search using
         * comparator bits31/30. A nominal virtual receiver has DC offsets
         * +3/-2 around midpoint64. Sample-and-hold captures both comparisons
         * on START; conversion takes the SDK's2us settling interval. */
        u32 i=(regs[0x600005a4u]>>9)&127,q=(regs[0x600005a8u]>>18)&127;
        regs[DC_RESULT]=(i>=67?0x80000000u:0)|(q>=62?0x40000000u:0);
        set_analog_deadline(regs,DC_DEADLINE,now+2);
    }
    regs[DC_COMMAND]=control;return true;
}

u32 dc_measurement_read(std::map<u32,u32> &regs,u64 now)
{
    u64 deadline=analog_deadline(regs,DC_DEADLINE);
    if(deadline && now>=deadline) {
        regs[DC_COMMAND]|=0x01000000u|regs[DC_RESULT];
        set_analog_deadline(regs,DC_DEADLINE,0);
    }
    return regs[DC_COMMAND];
}

bool rf_configuration(u32 address)
{
    /* set_rfanagain_dc_reg and ram_set_txbb_atten address exactly24 RF gain
     * entries and12 packed pairs of I/Q DC trims. These are coefficient RAM,
     * not peripheral status; neighboring words remain unimplemented. */
    if(!(address&3) && ((address>=0x60000504u && address<=0x60000560u) ||
                       (address>=0x60000404u && address<=0x60000430u)))return true;
    switch(address) {
    /* chip_60_set_channel divider/control, followed by the coefficient and
     * subcarrier mask words written by phy_dig_spur_set/phy_dig_spur_prot. */
    case 0x60009b14u:case 0x600098a0u:case 0x600098d4u:case 0x6000989cu:
    case 0x600099b0u:case 0x60009884u:case 0x6000988cu:case 0x60009888u:
    case 0x60009d0cu:case 0x60009d08u:case 0x600099bcu:case 0x600099b8u:
    case 0x600099acu:case 0x600099a8u:
        return true;
    case 0x60009864u: /* ram_cal_tos_v60 stores the resulting DC trim fields. */
    case 0x60009860u:case 0x6000983cu: /* ram_rfcal_txiq coefficient/enable fields */
    /* Calibration tone configuration: enable[18], signed phase[17:10],
     * amplitude[9:0]. PHY helpers enable/disable these three generators;
     * txiq_get_mis_pwr changes their fields before reading receiver power. */
    case 0x600005b8u:case 0x600005bcu:case 0x600005c4u:
    case 0x600005fcu: /* tx_pwctrl_init_cal packs four channel power offsets. */
        return true;
    /* The bounded configuration set programmed by SDK phy_bb_rx_cfg:
     * receive thresholds, gain/timing coefficients and feature enables.
     * These are read/modify/write latches; receiver samples and event/status
     * registers are intentionally absent from this list. */
    case 0x60009b0cu:case 0x60009d40u:case 0x60009b6cu:case 0x60009b68u:
    case 0x60009b50u:case 0x60009d18u:case 0x600098ecu:case 0x60009988u:
    case 0x60009b48u:case 0x60009b28u:case 0x60009a34u:case 0x60009b44u:
    case 0x60009d70u:case 0x60009b5cu:case 0x60009d10u:
    case 0x60009d24u:case 0x60009b58u:case 0x60009d4cu:case 0x60009d20u:
    case 0x60009d44u:case 0x60009838u:case 0x60009c48u:case 0x60009c4cu:
    case 0x60009d1cu:case 0x600005c0u:case 0x60009c04u:
    case 0x60000590u: /* chip_v6_rxmax_ext_dig maximum gain selection, bit4 */
    case 0x60009d68u: /* chip_v6_set_chan_rx_cmp channel gain compensation */
    case 0x60009c28u: /* PHY gain threshold, bits16:10 */
    /* Official libphy phy_set_rx11b_reg selects the 11b receiver fields with
     * (value & 0xfffff030) | 0x48a and disables them by clearing bits7:6.
     * This is configuration; the adjacent receiver-power latch is separate. */
    case 0x60009820u:
        return true;
    }
    return false;
}

u32 receiver_sample(std::map<u32,u32> &regs)
{
    /* SDK get_adc_rand samples bits1:0 and11:10 from the packed I/Q ADC
     * latch. Model an idle receiver with small, zero-centered sample noise.
     * The fixed seed is reproducible and does not provide secure entropy. */
    u32 state=regs[RX_NOISE_STATE];
    if(!state)state=0x8266adc1u;
    state^=state<<13;state^=state>>17;state^=state<<5;
    regs[RX_NOISE_STATE]=state;
    int inphase=int(state%7)-3,quadrature=int((state>>16)%7)-3;
    return (u32(inphase)&1023u)|((u32(quadrature)&1023u)<<10);
}

void rfpll_control(std::map<u32,u32> &regs,u32 value,u64 now,unsigned xtal_mhz)
{
    u32 old=regs[RFPLL_CONTROL];
    if(value&0x40) {
        regs[RFPLL_STATUS]=regs[RFPLL_RESULT]=0;
        regs[RFPLL_DEADLINE_LO]=regs[RFPLL_DEADLINE_HI]=0;
        if(!(old&0x40))regs[RFPLL_ARMED]=0;
        if((value&0x20) && !(old&0x20))regs[RFPLL_ARMED]=1;
    } else if((old&0x40) && regs[RFPLL_ARMED]) {
        /* Both ROM restart_cal and SDK ram_restart_cal pulse bit5 while
         * holding bit6, then release bit6. The virtual oscillator completes
         * this calibration after one SDK polling interval (20 us). This is
         * a deterministic functional timing model, not silicon RF timing. */
        if(regs.find(0xfe000363u)!=regs.end() && regs.find(0xfe000463u)!=regs.end() &&
           regs.find(0xfe000563u)!=regs.end()) {
            /* ram_rfpll_set_freq programs N = RF_MHz/(XTAL_MHz*3/4)-32,
             * as integer and two fractional bytes in SDM registers3..5.
             * The virtual VCO is centered at2437MHz/code128 with a4MHz
             * capacitor step. Search that deterministic tuning curve; do
             * not report lock when an unconfigured/out-of-range PLL starts.
             * This functional oscillator is not a silicon process model. */
            u32 khz=0;unsigned code=0;
            if(esp_rf_math::pll_frequency_khz((u8)regs[0xfe000363u],(u8)regs[0xfe000463u],
                    (u8)regs[0xfe000563u],xtal_mhz*1000,khz) &&
               esp_rf_math::vco_capacitor_code(khz,2437000,128,4000,code)) {
                regs[RFPLL_TARGET]=code;
                u64 deadline=now+20;
                regs[RFPLL_DEADLINE_LO]=(u32)deadline;
                regs[RFPLL_DEADLINE_HI]=(u32)(deadline>>32);
            }
        }
        regs[RFPLL_ARMED]=0;
    }
}

u32 rfpll_status(std::map<u32,u32> &regs,u64 now)
{
    esp8266_radio::advance(regs,now);
    return regs[RFPLL_STATUS];
}

bool analog_control_reset(unsigned host,u32 reg,u32 &value)
{
    /* The virtual analog domain begins with optional clocks/power overrides
     * disabled. These are configuration latches, not measurement or LOCK
     * status: SDK change_bbpll160_sleep writes them using read/modify/write.
     * BBPLL4 bit7 is the documented I2S clock gate; bits4:0 are programmed
     * to19/20 by the SDK. BIAS15/24/26/28 select RF power overrides. RXBB0
     * bits0/2 are enabled by chip_v6_rf_init after setting the channel. No other
     * unwritten analog register receives an assumed measurement value. */
    if((host==4 && reg==0x0467) ||
       (host==1 && reg==0x0062) || /* RFPLL calibration control */
       (host==2 && (reg==0x006c || reg==0x016c || reg==0x096b)) || /* SAR input configuration */
       /* chip_v6_rxmax_ext_ana derives these RX gain trims from RFPLL5. */
       (host==0 && (reg==0x0464 || reg==0x0764)) ||
       (host==1 && (reg==0x0261 || reg==0x0561 || reg==0x0761)) ||
       (host==3 && reg==0x0166) || /* RC-derived receive/transmit filter trim */
       /* rc_cal configures the filter calibration clock and reference. */
       (host==2 && (reg==0x036a || reg==0x046a || reg==0x066a)) ||
       (host==3 && reg==0x0168) ||
       /* Newer PHY power-gating helpers mirror BIAS28 bits6/5 into7c/21
        * bits0/1. BIAS9 and BIAS18 are saved, cleared and restored around
        * RX/TX calibration; they are control overrides, not ADC results. */
       (host==1 && reg==0x157c) ||
       (host==0 && (reg==0x0977 || reg==0x0f77 || reg==0x1077 || reg==0x1177 || reg==0x1277 ||
                    reg==0x1877 || reg==0x1977 || reg==0x1a77 || reg==0x1c77))) {
        value=0;return true;
    }
    return false;
}
}

/* Chip-specific blocks live here; UART/GPIO and ROM ABI utilities are shared.
 * The analog reset latch at SAR+0x48 is used by Espressif SDK startup as a
 * set/clear bit-0 pulse (also identified by esp-open-rtos sar_regs.h).
 * Analog measurements below have explicit input, trigger and completion
 * behavior; unsupported modes remain visible faults. */
bool EspMachine::esp8266_mmio_read(u32 address,u32 &value)
{
    if(wifi_dma_.read(registers_,address,value))return true;
    if(esp8266_mac_timers::handles(address)) {
        const bool ok=esp8266_mac_timers::read(registers_,address,esp32_apb_ticks(),value);
        esp8266_interrupts::update_mac(registers_,cpu_);return ok;
    }
    if(esp8266_wdev::handles(address))return esp8266_wdev::read(registers_,address,elapsed_us(),value);
    if(frc_timers_.handles(address) || esp8266_watchdog::handles(address))
        return esp8266_timer_read(address,value);
    esp_spi::Result spi=esp_spi::read(chip_,registers_,flash_,address,value,&diagnostic_);
    if(spi!=esp_spi::UNHANDLED) return spi==esp_spi::OK;
    if(address==NF_CONTROL || address==NF_RESULT || address==0x60009824u) {
        noise_update(registers_,elapsed_us());value=registers_[address];return true;
    }
    if(rf_configuration(address)) { value=registers_[address];return true; }
    if(address==0x60009830u) { value=receiver_sample(registers_);return true; }
    if(address==DC_COMMAND) { value=dc_measurement_read(registers_,elapsed_us());return true; }
    if(address==SAR_CONTROL) { value=sar_control(registers_,elapsed_us());return true; }
    if(address==IQ_CONTROL) { value=iq_control(registers_,elapsed_us());return true; }
    if(address==0x600005dcu || address==0x600005e0u || address==0x600005e4u) {
        iq_control(registers_,elapsed_us());value=registers_[address];return true;
    }
    if(address>=0x60000580u && address<=0x6000058cu && !(address&3)) {
        iq_control(registers_,elapsed_us());value=registers_[address];return true;
    }
    if(address==RX_GAIN_SELECT) { value=registers_[address];return true; }
    if(rx_gain_address(address))return rx_gain_read(registers_,address,value,diagnostic_);
    if(address>=0x60000d80u && address<=0x60000d9cu && !(address&3)) {
        sar_control(registers_,elapsed_us());
        std::map<u32,u32>::const_iterator it=registers_.find(address);
        value=it==registers_.end()?0xffffffffu:it->second;return true;
    }
    if(address>=0x60000d00u && address<=0x60000d1cu && !(address&3)) {
        value=registers_[address];return true;
    }
    /* IO_MUX_CONF plus the sixteen documented pin function registers.
     * Startup selects the SPI clock source here; GPIO mux configuration is
     * retained independently from the GPIO input/output data registers. */
    if(address>=0x60000800u && address<=0x60000840u && !(address&3)) {
        value=registers_[address];return true;
    }
    /* OTP is chip identity, not part of an SPI flash dump. Use a fixed local
     * unicast MAC 02:00:00:82:66:01; the vendor-present flag selects MAC2.
     * Field layout: esp-open-rtos core/include/esp/dport_regs.h and app_main.c. */
    switch(address) {
    case 0x60000594u:case 0x600005a4u:case 0x600005a8u:
    case 0x600005acu:case 0x600005b0u:case 0x600005b4u:
        value=registers_[address];return true; /* PBUS control/configuration banks */
    case 0x60009b00u:case 0x60009b08u:case 0x60009a28u:case 0x60009a2cu:
    case 0x60009d60u:case 0x60009d64u:
        /* SDK ant_switch_init programs the antenna select enable, GPIO
         * selection and RX/TX switch patterns here. No RF samples or
         * calibration-ready flags are part of these configuration words.
         * 0x9a2c bit19 enables digital receive (start_dig_rx/stop_dig_rx). */
        value=registers_[address];return true;
    case 0x6000036cu:value=registers_[address];return true;
    case 0x60000370u:
        if(rtc_calibration_us_ && elapsed_us()>=rtc_calibration_us_) {
            u32 periods=registers_[0x6000036cu]&0x3ff;
            registers_[address]=0x80000000u|(u32)((u64)periods*xtal_mhz_*1000000/150000);
            rtc_calibration_us_=0;
        }
        value=registers_[address];return true;
    case 0x600005c8u:
        /* RF-domain power selection. pm_open_rf clears bits16..17 when
         * bringing the analog radio domain out of sleep. */
        value=registers_[address];return true;
    case 0x60000598u:case 0x6000059cu:case 0x600005a0u:case 0x600005e8u:
        /* chip_v6_rf_init configures RF clock/power routing in these exact
         * words before invoking the analog calibration routines. */
        value=registers_[address];return true;
    case 0x60000d40u:
        /* SDK set_crystal_uart selects the baseband/UART clock in bits2..3. */
        value=registers_[address];return true;
    case 0x60000d54u:case 0x60000d58u:case 0x60000d5cu:case 0x60000d60u:
        /* SAR sampling/force configuration; chip_v6_set_chan toggles bit0
         * after restoring digital receive. This is not a conversion result. */
        value=registers_[address];return true;
    case 0x60000700u:case 0x60000704u:case 0x60000708u:case 0x6000070cu:case 0x60000710u:
        value=registers_[address];return true;
    case 0x60000714u:
        value=registers_.count(address)?registers_[address]:1;return true;
    case 0x60000718u:value=registers_[address];return true;
    case 0x6000071cu:value=(u32)(elapsed_us()*150000/1000000);return true;
    case 0x60000720u:value=registers_[address];return true;
    case 0x60000724u:value=0;return true; /* write-one-to-clear */
    case 0x60000728u:
        /* The SDK's pm_wait4wakeup samples bits0..1 here after starting the
         * sleep state machine. Complete only when its RTC alarm expires,
         * never by returning an unconditional ready bit. The modeled RC
         * clock runs deterministically at150kHz, also used by calibration. */
        if(rtc_wake_us_ && elapsed_us()>=rtc_wake_us_) {
            registers_[address]|=1;registers_[0x60000720u]|=1;
            registers_[0x60000708u]&=~(1u<<20);rtc_wake_us_=0;
        }
        value=registers_[address];return true;
    case 0x60000730u:case 0x60000734u:case 0x60000738u:
    case 0x6000073cu:case 0x60000768u:case 0x60000774u:case 0x60000790u:
    case 0x60000794u:case 0x60000798u:case 0x6000079cu:case 0x600007a0u:case 0x600007a4u:case 0x600007a8u:
        value=registers_[address];return true;
    case 0x60000740u:case 0x60000744u:case 0x60000748u:case 0x6000074cu:
    case 0x60000750u:case 0x60000754u:case 0x60000758u:case 0x6000075cu:
    case 0x60000760u:case 0x60000764u:case 0x6000076cu:case 0x60000770u:
    case 0x60000778u:case 0x6000077cu:case 0x60000780u:case 0x60000784u:case 0x60000788u:
    case 0x600007acu:case 0x600007b0u:case 0x600007b4u:
        /* Sleep timing/control words are initialized by the SDK before use.
         * Preserve an explicitly written configuration, not an invented
         * power-on measurement or ready indication. */
        if(registers_.find(address)==registers_.end())return false;
        value=registers_[address];return true;
    case 0x6000078cu: {
        bool input,output,enabled;gpio_state(16,input,output,enabled);value=input?1:0;return true;
    }
    case 0x3ff00050u:value=0x01000000u;return true;
    case 0x3ff00054u:value=0x00008266u;return true;
    case 0x3ff00058u:value=0x00009000u;return true;
    case 0x3ff0005cu:value=0x00020000u;return true;
    case 0x3ff0000cu:value=(registers_[address]&~0x300u)|(cache_enabled_?0x100u:0);return true;
    case 0x3ff00000u:case 0x3ff00004u:case 0x3ff00014u:
    case 0x3ff00018u:case 0x3ff00020u:case 0x3ff00024u:case 0x3ff00028u:
        value=registers_[address];return true;
    case 0x60009800u:
        /* libphy phy_get_bb_evm reads EVM from bits16..28. The measurement
         * latch is zero before receiving any baseband samples, and the
         * SAR reset pulse clears it. SDK user_start discards this first
         * result; the call initializes the analog/baseband block. */
        value=registers_[address];return true;
    case 0x600098dcu:
        /* Espressif libphy (NONOSDK221/305) phy_get_bb_freqoffset reads
         * 9800's valid bit, then always RMWs98dc with OR15, including its
         * unavailable-result path. No frequency samples are generated by
         * this receiver yet, so there is nothing pending to acknowledge.
         * This does not infer the other fields or individual clear bits. */
        value=0;return true;
    }
    if(address==0x60000d48u) { value=registers_[address];return true; }
    /* The SDK writes a silicon-variant bootstrap word here immediately after
     * the analog reset pulse. Retain only an explicitly written word; its
     * undocumented power-on value and neighboring RF registers are not
     * guessed. This models register retention, not analog RF behavior. */
    if(address==0x60009d74u) {
        std::map<u32,u32>::const_iterator it=registers_.find(address);
        if(it!=registers_.end()) { value=it->second;return true; }
    }
    return false;
}

bool EspMachine::esp8266_mmio_write(u32 address,u32 value)
{
    if(Esp8266WifiDma::handles(address)) {
        const u32 old=address==0x3ff20004?register_value(address):0;
        const Esp8266WifiDma::Result result=wifi_dma_.write(registers_,address,value,esp32_apb_ticks(),&diagnostic_);
        if(result==Esp8266WifiDma::ACCEPTED && address==0x3ff20004)
            esp8266_radio::note_write(registers_,address,old,register_value(address));
        esp8266_interrupts::update_mac(registers_,cpu_);
        return result==Esp8266WifiDma::ACCEPTED;
    }
    if(esp8266_mac_timers::handles(address)) {
        const bool ok=esp8266_mac_timers::write(registers_,address,value,esp32_apb_ticks());
        esp8266_interrupts::update_mac(registers_,cpu_);return ok;
    }
    if(esp8266_wdev::handles(address)) {
        bool ok=esp8266_wdev::write(registers_,address,value);
        if(ok && address==0x3ff20c24)wifi_dma_.acknowledge(registers_,value,esp32_apb_ticks());
        esp8266_interrupts::update_mac(registers_,cpu_);return ok;
    }
    if(frc_timers_.handles(address) || esp8266_watchdog::handles(address) ||
       address==0x3ff00000u || address==0x3ff00004u)
        return esp8266_timer_write(address,value);
    esp_spi::Result spi=esp_spi::write(chip_,registers_,flash_,address,value,&diagnostic_);
    if(spi!=esp_spi::UNHANDLED) return spi==esp_spi::OK;
    if(address==DC_COMMAND)return dc_measurement_write(registers_,value,elapsed_us(),diagnostic_);
    if(address==SAR_CONTROL)return sar_start(registers_,value,elapsed_us(),diagnostic_);
    if(address==IQ_CONTROL)return iq_start(registers_,value,elapsed_us(),diagnostic_);
    if(address==NF_CONTROL) {noise_start(registers_,value,elapsed_us());return true;}
    if(address==NF_RESULT) {
        noise_update(registers_,elapsed_us());
        registers_[address]=(registers_[address]&0xfff00000u)|(value&0xfffffu);return true;
    }
    if(address==RX_GAIN_SELECT) {
        if(value&~0x1feu) { diagnostic_="unsupported RX gain RAM selector bits";return false; }
        registers_[address]=value;return true;
    }
    if(rx_gain_address(address)) { rx_gain_write(registers_,address,value);return true; }
    if(rf_configuration(address)) {
        registers_[address]=value;return true;
    }
    if(address>=0x60000d00u && address<=0x60000d1cu && !(address&3)) {
        /* Internal analog I2C command engine, as used by ROM readReg/writeReg:
         * block[7:0], register[15:8], data[23:16], WRITE[24], BUSY[25]. */
        if(value&0xfc000000u) { diagnostic_="unsupported internal I2C command bits";return false; }
        u32 key=0xfe000000u|((address&28u)<<16)|(value&0xffffu);
        if(value&(1u<<24)) {
            if(key==RFPLL_STATUS || key==RFPLL_RESULT || key==RC_RESULT) {
                diagnostic_="analog calibration result is read-only";return false;
            }
            if(key==RFPLL_CONTROL)rfpll_control(registers_,(value>>16)&255,elapsed_us(),xtal_mhz_);
            if(key==RC_CONFIG || key==RC_ENABLE || key==RC_POWER)
                rc_calibration_write(registers_,key,(value>>16)&255,elapsed_us());
            esp8266_radio::note_write(registers_,key,register_value(key),(value>>16)&255);
            registers_[key]=(value>>16)&255;
        }
        else if(key==RFPLL_STATUS)registers_[key]=rfpll_status(registers_,elapsed_us());
        else if(key==RFPLL_RESULT) { rfpll_status(registers_,elapsed_us());registers_[key]&=255; }
        else if(key==RC_RESULT)registers_[key]=rc_calibration_result(registers_,elapsed_us());
        else if(registers_.find(key)==registers_.end()) {
            u32 initial;
            if(!analog_control_reset((address-0x60000d00u)/4,value&0xffffu,initial)) {
                diagnostic_=strf("unmodeled analog I2C reset value for block 0x%02x register 0x%02x",value&255,(value>>8)&255);
                return false;
            }
            registers_[key]=initial;
        }
        registers_[address]=(value&0x0100ffffu)|(registers_[key]<<16);
        return true;
    }
    if(address>=0x60000800u && address<=0x60000840u && !(address&3)) {
        registers_[address]=value;update_gpio_interrupts();return true;
    }
    switch(address) {
    case 0x60000594u: {
        /* ROM pbus_force_test encodes block[4:2], nine data bits[13:5],
         * register[15:14], START[1], DEBUG[0]. Its pbus_rd exposes the
         * configuration in packed nine-bit banks beginning at0x5a4.
         * The block offsets are the ROM's own eight-entry PBUS layout. */
        if((value&2) && !(registers_[address]&2)) {
            static const u8 offsets[]={0,2,4,5,7,9,11,12};
            unsigned block=(value>>2)&7,reg=(value>>14)&3;
            if(!reg) { diagnostic_="unsupported PBUS register selector";return false; }
            unsigned index=offsets[block]+reg-1;
            if(index>12) { diagnostic_="PBUS register exceeds configuration bank";return false; }
            unsigned shift=index==12?0:18-9*(index%3);
            u32 target=0x600005a4u+(index/3)*4;
            registers_[target]=(registers_[target]&~(0x1ffu<<shift))|(((value>>5)&0x1ffu)<<shift);
        }
        registers_[address]=value;
        /* Synchronous transfers have no pending BUSY operation. */
        registers_[0x600005a0u]&=~0x80000000u;
        return true;
    }
    case 0x60009b00u:case 0x60009b08u:case 0x60009a28u:case 0x60009a2cu:
    case 0x60009d60u:case 0x60009d64u:
        esp8266_radio::note_write(registers_,address,register_value(address),value);
        registers_[address]=value;return true;
    case 0x6000036cu:
        /* GPIO_RTC_CALIB_SYNC: rising START measures XTAL cycles over the
         * requested number of RC-clock periods, using the board's XTAL and
         * the deterministic 150kHz RC oscillator. */
        if((value&0x80000000u) && !(registers_[address]&0x80000000u)) {
            u32 periods=value&0x3ff;
            if(!periods)return false;
            registers_[0x60000370u]=0;
            rtc_calibration_us_=elapsed_us()+((u64)periods*1000000+149999)/150000;
        }
        registers_[address]=value&0x800003ffu;return true;
    case 0x60000370u:
        registers_[address]&=value|0x7fffffffu;return true; /* clear READY */
    case 0x600005c8u:
        esp8266_radio::note_write(registers_,address,register_value(address),value);
        registers_[address]=value;return true;
    case 0x60000598u:case 0x6000059cu:case 0x600005a0u:case 0x600005e8u:
        registers_[address]=value;return true;
    case 0x60000d40u:case 0x60000d54u:case 0x60000d58u:case 0x60000d5cu:
    case 0x60000d60u:registers_[address]=value;return true;
    case 0x60000700u:
        /* ROM software_reset() sets bit31. RTC_STATE1 then reports SW_RESET
         * (3), as defined by Espressif's ESP8266_RTOS_SDK rtc_register.h. */
        registers_[address]=value&0x7fffffffu;
        if(value&0x80000000u)request_reset(RESET_DIGITAL,1,3);
        return true;
    case 0x60000704u:case 0x6000070cu:case 0x60000710u:case 0x60000718u:case 0x60000720u:
        registers_[address]=value;return true;
    case 0x60000708u:
        if((value&(1u<<20)) && !(registers_[address]&(1u<<20))) {
            u32 current=(u32)(elapsed_us()*150000/1000000);
            u32 cycles=registers_[0x60000704u]-current;
            rtc_wake_us_=elapsed_us()+((u64)cycles*1000000+149999)/150000;
        }
        registers_[address]=value;return true;
    case 0x60000724u:registers_[0x60000720u]&=~value;registers_[0x60000728u]&=~value;return true;
    case 0x60000728u:case 0x60000730u:case 0x60000734u:case 0x60000738u:case 0x6000073cu:
    case 0x60000768u:case 0x60000774u:case 0x60000790u:
    case 0x60000794u:case 0x60000798u:case 0x6000079cu:case 0x600007a0u:case 0x600007a4u:case 0x600007a8u:
    case 0x60000740u:case 0x60000744u:case 0x60000748u:case 0x6000074cu:
    case 0x60000750u:case 0x60000754u:case 0x60000758u:case 0x6000075cu:
    case 0x60000760u:case 0x60000764u:case 0x6000076cu:case 0x60000770u:
    case 0x60000778u:case 0x6000077cu:case 0x60000780u:case 0x60000784u:case 0x60000788u:
    case 0x600007acu:case 0x600007b0u:case 0x600007b4u:
        registers_[address]=value;return true;
    case 0x3ff0000cu:
        /* ROM SPIWaitIdle waits for BUSY bit9 to clear. Cache invalidation
         * starts on bit0 and reports completion in bit1. All memory reads
         * are synchronous, so there is no pending fill to drain. */
        cache_enabled_=(value&0x100u)!=0;
        registers_[address]=(value&~0x202u)|(value&1u?2u:0u);
        flash_window_=((value>>16)&7u)*0x100000u+((value&(1u<<25))?0x80000u:0);
        return true;
    case 0x3ff00014u:set_cpu_frequency((value&1u)?160:80);registers_[address]=value;return true;
    case 0x3ff00000u:case 0x3ff00004u:case 0x3ff00018u:
    case 0x3ff00020u:case 0x3ff00024u:case 0x3ff00028u:
        registers_[address]=value;return true;
    }
    if(address==0x60000d48u && !(value&~1u)) {
        registers_[address]=value;
        if(value&1u) registers_[0x60009800u]=0;
        return true;
    }
    if(address==0x600098dcu && (value==0 || value==15)) {
        /* Accept the SDK's empty-measurement acknowledgement without
         * manufacturing a valid sample, EVM or carrier-frequency offset. */
        return true;
    }
    if(address==0x60009d74u) { registers_[address]=value;return true; }
    return false;
}
