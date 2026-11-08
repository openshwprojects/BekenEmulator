#include "esp32_analog.h"
#include <stdio.h>

int esp32_analog_tests()
{
    int checks=0;std::map<u32,u32> r;std::string error;u32 value=0;
#define ANALOG_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP32 analog line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    using namespace esp32_analog;
    ANALOG_CHECK(read(r,0x3ff4e0c4,value)==OK && value==0);
    ANALOG_CHECK(read(r,0x6000e044,value)==OK && value==0x1ff00);
    for(unsigned host=0;host<5;++host)ANALOG_CHECK(read(r,0x3ff4e000+host*4,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x6000e010,0x015a0366,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e010,0x00000366,error)==OK);
    ANALOG_CHECK(read(r,0x6000e010,value)==OK && ((value>>16)&255)==0x5a && !(value&0x02000000));
    ANALOG_CHECK(write(r,0x3ff4e010,0,error)==OK && read(r,0x6000e010,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e044,0x3ff00,error)==OK);
    ANALOG_CHECK(write(r,0x6000e010,0x00000366,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e044,0x1ff00,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e010,0x00000d66,error)==ERROR); /* no unknown analog cell */
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000366,error)==ERROR); /* BBPLL is on host4 */
    /* Espressif wr_rf_freq_mem selects a word, stages payload, pulses bit9. */
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x41422c17,error)==OK);
    ANALOG_CHECK(write(r,0x6000e148,0x12345678,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e0c0,value)==OK && value==0); /* staging alone cannot commit */
    ANALOG_CHECK(write(r,0x6000e0c4,0x41422e17,error)==OK);
    ANALOG_CHECK(read(r,0x6000e0c0,value)==OK && value==0x12345678);
    ANALOG_CHECK(write(r,0x3ff4e148,0xaabbccdd,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x41422e17,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e0c0,value)==OK && value==0x12345678); /* no new edge */
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x41422c18,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e0c0,value)==OK && value==0); /* separate address */
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x41422e18,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e0c0,value)==OK && value==0xaabbccdd);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x41422c17,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e0c0,value)==OK && value==0x12345678);
    ANALOG_CHECK(write(r,0x3ff4e0c0,0,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x80000000,error)==ERROR);
    ANALOG_CHECK(read(r,0x3ff4e0f8,value)==UNHANDLED && write(r,0x3ff4e0f8,0,error)==UNHANDLED);
    ANALOG_CHECK(read(r,0x3ff4e001,value)==UNHANDLED);
    /* phy_dis_hw_set_freq first waits for the frequency sequencer to be idle,
     * then inhibits automatic requests with CONFIG25 and clears REQUEST8. */
    ANALOG_CHECK(read(r,0x3ff4e168,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x43422c17,error)==OK);
    ANALOG_CHECK(read(r,0x6000e0c4,value)==OK && value==0x43422c17);
    ANALOG_CHECK(read(r,0x6000e168,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e168,0x80000000,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x43422d17,error)==ERROR); /* active frequency request is not a no-op */
    ANALOG_CHECK(read(r,0x3ff4e0c4,value)==OK && value==0x43422c17);
    ANALOG_CHECK(read(r,0x3ff4e160,value)==UNHANDLED);
    /* RF SDM and PLL are separate devices on hosts0/1. A configuration
     * write cannot manufacture a calibration result. */
    ANALOG_CHECK(write(r,0x3ff4e000,0x01f30163,error)==OK);
    ANALOG_CHECK(write(r,0x6000e000,0x00000163,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e000,value)==OK && value==0x00f30163);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000163,error)==ERROR); /* incorrect host */
    ANALOG_CHECK(write(r,0x3ff4e004,0x01770362,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && !(value&0x00800000));
    /* 2400MHz at40MHz XTAL: divider80 => SDM integer48, fraction0. */
    ANALOG_CHECK(write(r,0x3ff4e000,0x01300363,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x01000463,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x01000563,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x017f0062,error,100,40000)==OK); /* reset6, trigger5 already high */
    ANALOG_CHECK(write(r,0x3ff4e004,0x015f0062,error,101,40000)==OK); /* trigger low */
    ANALOG_CHECK(write(r,0x3ff4e004,0x017f0062,error,102,40000)==OK); /* trigger rising */
    ANALOG_CHECK(write(r,0x3ff4e004,0x013f0062,error,103,40000)==OK); /* release reset, start */
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error,122,40000)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && !(value&0x00800000));
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error,123,40000)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && (value&0x00800000));
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000562,error,123,40000)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && ((value>>16)&255)==137);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000662,error,123,40000)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && ((value>>16)&15)==4); /* remaining quarter-MHz trim */
    ANALOG_CHECK(write(r,0x3ff4e004,0x010f0662,error,123,40000)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e004,0x017f0062,error,124,40000)==OK); /* reset cancels result */
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error,200,40000)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && !(value&0x00800000));
    ANALOG_CHECK(write(r,0x3ff4e004,0x01ff0562,error)==ERROR); /* trim/result is not writable */
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000f62,error)==ERROR); /* no invented device registers */
    /* i2c_rftx_init programs only transmitter controls/capacitor settings;
     * it does not start a measurement or populate RFPLL result registers. */
    const unsigned tx_registers[]={1,2,3,4,5,6,7,10};
    const unsigned tx_values[]={0x61,0x23,0xa8,7,8,0xb8,0x5d,0x74};
    for(unsigned i=0;i<8;++i) {
        ANALOG_CHECK(write(r,0x3ff4e008,0x0100006b|(tx_registers[i]<<8)|(tx_values[i]<<16),error)==OK);
        ANALOG_CHECK(write(r,0x6000e008,0x6b|(tx_registers[i]<<8),error)==OK);
        ANALOG_CHECK(read(r,0x3ff4e008,value)==OK && ((value>>16)&255)==tx_values[i]);
    }
    ANALOG_CHECK(write(r,0x3ff4e008,0x0000086b,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e004,0x0000036b,error)==ERROR);
    const unsigned analog_config[][4]={
        {1,0x67,9,0x71},{1,0x67,11,0x61},{1,0x67,12,0x10},{1,0x67,15,0xea},
        {1,0x67,5,0x7f},{1,0x67,6,0x7f},{1,0x67,7,0x7f},{1,0x67,8,0x7f},
        {3,0x68,0,0x63},{3,0x68,1,0x80},{2,0x6a,2,0x68},{2,0x6a,0,0x26}};
    for(unsigned i=0;i<12;++i) {
        const u32 address=0x3ff4e000+analog_config[i][0]*4;
        const u32 command=analog_config[i][1]|(analog_config[i][2]<<8);
        ANALOG_CHECK(write(r,address,0x01000000|command|(analog_config[i][3]<<16),error)==OK);
        ANALOG_CHECK(write(r,address,command,error)==OK);
        ANALOG_CHECK(read(r,address,value)==OK && ((value>>16)&255)==analog_config[i][3]);
    }
    ANALOG_CHECK(write(r,0x3ff4e004,0x01000067,error)==ERROR); /* continuous TX is not configuration */
    ANALOG_CHECK(write(r,0x3ff4e00c,0x01a00168,error)==ERROR); /* active RC calibration remains explicit */
    ANALOG_CHECK(write(r,0x3ff4e008,0x0000046a,error)==ERROR);
    /* No lock for absent divider programming, or a frequency outside the
     * virtual oscillator curve. A different XTAL changes the decoded RF. */
    for(unsigned scenario=0;scenario<3;++scenario) {
        std::map<u32,u32> p;
        if(scenario) {
            ANALOG_CHECK(write(p,0x3ff4e000,0x01000363|(scenario==2?93u<<16:0),error)==OK);
            ANALOG_CHECK(write(p,0x3ff4e000,0x01000463,error)==OK);
            ANALOG_CHECK(write(p,0x3ff4e000,0x01000563,error)==OK);
        }
        const u64 start=0x100000010ULL;
        ANALOG_CHECK(write(p,0x3ff4e004,0x01400062,error,start,26000)==OK);
        ANALOG_CHECK(write(p,0x3ff4e004,0x01600062,error,start+1,26000)==OK);
        ANALOG_CHECK(write(p,0x3ff4e004,0x01200062,error,start+2,26000)==OK);
        ANALOG_CHECK(write(p,0x3ff4e004,0x00000762,error,start+22,26000)==OK);
        ANALOG_CHECK(read(p,0x3ff4e004,value)==OK && ((value>>23)&1)==(scenario==2));
        if(scenario==2) {
            ANALOG_CHECK(write(p,0x3ff4e004,0,error,start+30,26000)==OK); /* host reset retains device state */
            ANALOG_CHECK(write(p,0x3ff4e004,0x00000562,error,start+31,26000)==OK);
            ANALOG_CHECK(read(p,0x3ff4e004,value)==OK && ((value>>16)&255)==127);
            ANALOG_CHECK(write(p,0x3ff4e004,0x00000662,error,start+31,26000)==OK);
            ANALOG_CHECK(read(p,0x3ff4e004,value)==OK && ((value>>16)&15)==14);
        }
    }
    ANALOG_CHECK(write(r,0x3ff4e010,0x80000366,error)==ERROR);
    /* ram_en_pwdet / ram_tx_pwctrl_bg_init configure the detector path.
     * Neither configuration write produces a conversion or a result. */
    ANALOG_CHECK(read(r,0x3ff4e05c,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e05c,0x00a00000,error)==OK);
    ANALOG_CHECK(write(r,0x6000e05c,0x00200000,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e05c,0,error)==OK);
    ANALOG_CHECK(read(r,0x6000e05c,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e050,0xc1,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e050,0xc0,error)==OK);
    ANALOG_CHECK(read(r,0x6000e050,value)==OK && value==0xc0);
    ANALOG_CHECK(write(r,0x3ff4e060,2,error)==OK);
    ANALOG_CHECK(read(r,0x6000e060,value)==OK && value==2);
    ANALOG_CHECK(write(r,0x3ff4e050,0xc2,error)==ERROR); /* manual SAR trigger */
    ANALOG_CHECK(read(r,0x3ff4e050,value)==OK && value==0xc0);
    ANALOG_CHECK(write(r,0x3ff4e060,3,error)==ERROR); /* one-shot detector trigger */
    ANALOG_CHECK(read(r,0x3ff4e060,value)==OK && value==2);
    ANALOG_CHECK(write(r,0x3ff4e05c,0x00400000,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e060,0x01000002,error)==ERROR); /* rate is not writable */
    ANALOG_CHECK(read(r,0x3ff4e058,value)==UNHANDLED);
    ANALOG_CHECK(read(r,0x3ff4e080,value)==UNHANDLED); /* no invented SAR sample */
    /* get_rf_freq_init selects manual capacitor controls before it starts
     * the existing RFPLL calibration trigger sequence. */
    ANALOG_CHECK(write(r,0x3ff4e004,0x016e0162,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x01000262,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000162,error)==OK);
    ANALOG_CHECK(read(r,0x6000e004,value)==OK && ((value>>16)&255)==0x6e);
    ANALOG_CHECK(write(r,0x3ff4e004,0x01800262,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000262,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && ((value>>16)&255)==0x80);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && !(value&0x00800000));
    /* rom_write_rfpll_sdm holds divider loading with0x07 and releases it
     * with0x17 after programming all three divider bytes. */
    ANALOG_CHECK(write(r,0x3ff4e000,0x01070063,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000063,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e000,value)==OK && ((value>>16)&255)==7);
    ANALOG_CHECK(write(r,0x3ff4e004,0x01400062,error,1000)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x01600062,error,1001)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x01200062,error,1002)==OK);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000762,error,1022)==OK);
    ANALOG_CHECK(read(r,0x3ff4e004,value)==OK && !(value&0x00800000));
    ANALOG_CHECK(write(r,0x3ff4e000,0x01170063,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x01080063,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000063,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e000,value)==OK && ((value>>16)&255)==0x17);
    /* chip_v7_rxmax_ext_ana sets the LNA/VGA capacitor trims using
     * get_lna_vga_dcap_val; these cells are distinct from RFPLL results. */
    ANALOG_CHECK(write(r,0x3ff4e000,0x014c0464,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x01460764,error)==OK);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000464,error)==OK);
    ANALOG_CHECK(read(r,0x6000e000,value)==OK && ((value>>16)&255)==0x4c);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000764,error)==OK);
    ANALOG_CHECK(read(r,0x6000e000,value)==OK && ((value>>16)&255)==0x46);
    ANALOG_CHECK(write(r,0x3ff4e004,0x00000464,error)==ERROR);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000564,error)==ERROR);
    /* bt_i2c_write_set packs the sequencer program into exact data words.
     * Preparing a program does not execute its analog writes. */
    const u32 sequence_words[]={0xc8,0xcc,0xd0,0xd4,0xd8,0xdc,0xe0,0xe4,0xe8,0xec,0xf0,0xf4,
        0x100,0x104,0x108,0x10c,0x110,0x114,0x118,0x11c,0x120,0x124,0x128,0x12c,
        0x130,0x134,0x138,0x13c,0x140,0x164};
    for(unsigned i=0;i<30;++i) {
        ANALOG_CHECK(read(r,0x3ff4e000+sequence_words[i],value)==OK && value==0);
        const u32 pattern=0xa5037000u+i;
        ANALOG_CHECK(write(r,0x3ff4e000+sequence_words[i],pattern,error)==OK);
        ANALOG_CHECK(read(r,0x6000e000+sequence_words[i],value)==OK && value==pattern);
    }
    ANALOG_CHECK(read(r,0x3ff4e168,value)==OK && value==0);
    ANALOG_CHECK(write(r,0x3ff4e000,0x00000464,error)==OK);
    ANALOG_CHECK(read(r,0x3ff4e000,value)==OK && ((value>>16)&255)==0x4c);
    ANALOG_CHECK(write(r,0x3ff4e0c4,0x100,error)==ERROR); /* program execution remains explicit */
    ANALOG_CHECK(write(r,0x3ff4e144,0,error)==UNHANDLED);
#undef ANALOG_CHECK
    return checks;
}
#ifdef ESP32_ANALOG_TEST_MAIN
int main(){int n=esp32_analog_tests();if(n<0)return 1;printf("ESP32 analog: %d checks passed\n",n);return 0;}
#endif
