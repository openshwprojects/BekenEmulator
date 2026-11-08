#include "esp32/esp32_phy.h"
#include <stdio.h>

int esp32_phy_tests()
{
    using namespace esp32_phy;
    int checks=0;std::map<u32,u32> r;std::string error;u32 value;
#define PHY_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP32 PHY line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    PHY_CHECK(read(r,0x3ff5c01c,value)==OK);
    PHY_CHECK(!agc_enabled(r));
    /* Real libphy disable_wifi_agc: threshold127, latch bit26, mode0, hold. */
    PHY_CHECK(write(r,0x3ff5c01c,0x007f00bf,error)==OK);
    PHY_CHECK(write(r,0x6001c038,0x04000000,error)==OK);
    PHY_CHECK(write(r,0x3ff5c030,0,error)==OK);
    PHY_CHECK(write(r,0x6001c080,1,error)==OK);
    PHY_CHECK(read(r,0x6001c01c,value)==OK && value==0x007f00bf);
    PHY_CHECK(!agc_enabled(r) && cca_threshold(r)==-65);
    /* enable_wifi_agc reverses hold/mode independently of the CCA byte. */
    PHY_CHECK(write(r,0x3ff5c080,0,error)==OK);
    PHY_CHECK(!agc_enabled(r));
    PHY_CHECK(write(r,0x6001c030,0x10,error)==OK);
    PHY_CHECK(agc_enabled(r));
    PHY_CHECK(write(r,0x3ff5c01c,0x000c00bf,error)==OK);
    PHY_CHECK(read(r,0x6001c01c,value)==OK && value==0x000c00bf);
    PHY_CHECK(cca_threshold(r)==-65);
    PHY_CHECK(write(r,0x3ff5c018,0x40000004,error)==OK);
    PHY_CHECK(read(r,0x6001c018,value)==OK && value==0x40000004);
    PHY_CHECK(write(r,0x3ff5c01c,0x000c00a6,error)==OK && cca_threshold(r)==-90);
    PHY_CHECK(write(r,0x3ff5c080,1,error)==OK && !agc_enabled(r));
    PHY_CHECK(write(r,0x3ff5c080,2,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5c080,value)==OK && value==1); /* failed write is atomic */
    PHY_CHECK(write(r,0x3ff5c038,0x80000000,error)==ERROR);
    PHY_CHECK(read(r,0x6001c038,value)==OK && value==0x04000000);
    PHY_CHECK(read(r,0x3ff5c020,value)==UNHANDLED);
    PHY_CHECK(write(r,0x6001c020,0,error)==UNHANDLED);
    PHY_CHECK(read(r,0x3ff5c01d,value)==UNHANDLED);
    r[0x3ff4e010]=0x12345678; /* baseband reset must not erase analog controller */
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff5c01c,value)==OK && value==0);
    PHY_CHECK(read(r,0x6001c038,value)==OK && value==0);
    PHY_CHECK(!agc_enabled(r) && r[0x3ff4e010]==0x12345678);
    r[0x3ff000d0]=1;
    PHY_CHECK(write(r,0x3ff5c030,0x10,error)==ERROR);
    PHY_CHECK(!agc_enabled(r));
    r[0x3ff000d0]=0;
    PHY_CHECK(write(r,0x3ff5c030,0x10,error)==OK && agc_enabled(r));
    /* ram_disable_agc overrides the mode without changing its selection. */
    PHY_CHECK(write(r,0x3ff5c030,0x110,error)==OK && !agc_enabled(r));
    PHY_CHECK(write(r,0x3ff5c030,0x10,error)==OK && agc_enabled(r));
    /* force_txrxoff programs three force/value pairs in separate writes. */
    PHY_CHECK(read(r,0x3ff460a0,value)==OK && value==0);
    PHY_CHECK(frontend_signal(r,10,true) && !frontend_signal(r,10,false));
    PHY_CHECK(write(r,0x600060a0,0x2000,error)==OK);
    PHY_CHECK(write(r,0x3ff460a0,0x2800,error)==OK);
    PHY_CHECK(write(r,0x3ff460a0,0x2a00,error)==OK);
    PHY_CHECK(!frontend_signal(r,8,true) && !frontend_signal(r,10,true) && !frontend_signal(r,12,true));
    PHY_CHECK(write(r,0x600060a0,0x3ea00,error)==OK); /* force both RX/TX clocks on */
    PHY_CHECK(frontend_signal(r,14,false) && frontend_signal(r,16,false));
    PHY_CHECK(!frontend_signal(r,10,true)); /* enabling clocks cannot enable the PA */
    PHY_CHECK(read(r,0x3ff460a0,value)==OK && value==0x3ea00);
    PHY_CHECK(write(r,0x3ff460a0,0x8003ea00,error)==OK); /* PBUS busy is not writable */
    PHY_CHECK(read(r,0x600060a0,value)==OK && value==0x3ea00);
    PHY_CHECK(read(r,0x3ff460a4,value)==UNHANDLED);
    PHY_CHECK(write(r,0x3ff46094,2,error)==ERROR); /* invalid PBUS transfer cannot complete */
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff460a0,value)==OK && value==0x3ea00);
    PHY_CHECK(write(r,0x3ff5c01c,0x000c00bf,error)==OK);
    reset_frontend(r);
    PHY_CHECK(read(r,0x600060a0,value)==OK && value==0);
    PHY_CHECK(read(r,0x3ff5c01c,value)==OK && value==0x000c00bf);
    /* Complete agc_reg_init sequence. Literal configuration words and RMW
     * fields come from the official classic ESP32 PHY implementation. */
    reset_baseband(r);
    const u32 agc_addresses[]={0x3ff5c004,0x3ff5c0a4,0x3ff5c0c4,0x3ff5c030,0x3ff5c094,
        0x3ff5c02c,0x3ff5c018,0x3ff5c01c,0x3ff5c028,0x3ff5c0f8,0x3ff5c038,0x3ff5c088,0x3ff5c104,0x3ff5c07c};
    const u32 agc_values[]={0,0x1e3c,0x6450403f,0x03c1a4d5,0x01b8dd03,
        0x324048fe,0x40000004,0xbf,0x1ba,0x37400,0,5,0,0x009c0000};
    for(unsigned i=0;i<14;++i) {
        PHY_CHECK(read(r,agc_addresses[i],value)==OK);
        PHY_CHECK(write(r,agc_addresses[i],agc_values[i],error)==OK);
        PHY_CHECK(read(r,0x60000000+(agc_addresses[i]-0x3ff40000),value)==OK && value==agc_values[i]);
    }
    PHY_CHECK(agc_enabled(r) && cca_threshold(r)==-65);
    PHY_CHECK(write(r,0x3ff5c02c,0x32c048fe,error)==OK);
    PHY_CHECK(write(r,0x3ff5c02c,0x324048fe,error)==OK);
    PHY_CHECK(read(r,0x6001c02c,value)==OK && value==0x324048fe);
    /* Older SDK agc_reg_init initializes the whole gain-configuration word
     * before later versions update just its two seven-bit table endpoints. */
    PHY_CHECK(write(r,0x3ff5c0a4,0x00e21e3c,error)==OK);
    PHY_CHECK(read(r,0x6001c0a4,value)==OK && value==0x00e21e3c);
    PHY_CHECK(write(r,0x3ff5c028,0x200,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5c028,value)==OK && value==0x1ba);
    PHY_CHECK(read(r,0x3ff5c008,value)==UNHANDLED);
    reset_baseband(r);
    for(unsigned i=0;i<14;++i)PHY_CHECK(read(r,agc_addresses[i],value)==OK && value==0);
    /* Baseband digital filters and timing: both SDK generations program
     * whole filter words, with independent RMW threshold fields. */
    const u32 bb_addresses[]={0x3ff5cc48,0x3ff5cce4,0x3ff5c400,0x3ff5cd04,0x3ff5cd08,
        0x3ff5cc0c,0x3ff5cc08,0x3ff5ccdc,0x3ff5c044,0x3ff5c024,0x3ff5cc04,0x3ff5ccd8};
    const u32 bb_values[]={0x170433af,0,0x001ae0a5,0x51510556,0x51510556,
        0x001241fc,0x090b4d20,0x0195c83c,0x01fb0000,0x36800,0x2c,0x186};
    for(unsigned i=0;i<12;++i) {
        PHY_CHECK(read(r,bb_addresses[i],value)==OK);
        PHY_CHECK(write(r,bb_addresses[i],bb_values[i],error)==OK);
        PHY_CHECK(read(r,0x60000000+(bb_addresses[i]-0x3ff40000),value)==OK && value==bb_values[i]);
    }
    PHY_CHECK(write(r,0x3ff5c044,0x02000000,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5cc4c,value)==UNHANDLED);
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff5cc48,value)==OK && value==0x170433af);
    r[0x3ff000d0]=1;
    PHY_CHECK(write(r,0x3ff5cc48,0,error)==ERROR);
    r[0x3ff000d0]=0;
    reset_baseband(r);
    for(unsigned i=0;i<12;++i)PHY_CHECK(read(r,bb_addresses[i],value)==OK && value==0);
    /* PA-on timing belongs to baseband; the analog-facing FE delay has a
     * separate reset line. Both SDK parameter sets use the same fields. */
    PHY_CHECK(write(r,0x3ff5d000,0x1fe48000,error)==OK);
    PHY_CHECK(read(r,0x6001d000,value)==OK && value==0x1fe48000);
    PHY_CHECK(write(r,0x60006048,0xa400,error)==OK);
    PHY_CHECK(write(r,0x3ff5c400,0x20000,error)==OK);
    PHY_CHECK(write(r,0x3ff5d000,0x0f7e8000,error)==OK);
    PHY_CHECK(write(r,0x3ff5d000,1,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5d000,value)==OK && value==0x0f7e8000);
    PHY_CHECK(read(r,0x3ff5d004,value)==UNHANDLED);
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff5d000,value)==OK && value==0);
    PHY_CHECK(read(r,0x3ff46048,value)==OK && value==0xa400);
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff46048,value)==OK && value==0);
    /* The BB watchdog protects radio state-machine activity. Configuration
     * alone does not constitute a pending transmit/receive operation. */
    PHY_CHECK(read(r,0x3ff5d040,value)==OK && value==0);
    PHY_CHECK(write(r,0x3ff5d040,0x80000000,error)==OK);
    PHY_CHECK(write(r,0x3ff5d03c,0x401800aa,error)==OK);
    PHY_CHECK(read(r,0x6001d03c,value)==OK && value==0x401800aa);
    PHY_CHECK(write(r,0x6001d040,0xe0000000,error)==OK);
    PHY_CHECK(read(r,0x3ff5d040,value)==OK && value==0xc0000000); /* timeout-clear is a strobe */
    PHY_CHECK(read(r,0x6001d008,value)==OK && value==0); /* no active baseband request */
    PHY_CHECK(write(r,0x3ff5d008,1,error)==ERROR);
    PHY_CHECK(write(r,0x3ff5d040,1,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5d040,value)==OK && value==0xc0000000);
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff5d03c,value)==OK && value==0x401800aa);
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff5d040,value)==OK && value==0);
    PHY_CHECK(read(r,0x3ff5d03c,value)==OK && value==0);
    /* phy_set_bbfreq_init, phy_ant_init and opt_11b_resart configure
     * independent clock selection, antenna and CCK receiver thresholds. */
    const u32 rx_addresses[]={0x3ff5ccb8,0x3ff5c85c,0x3ff5c11c,0x3ff5c120,0x3ff5c0a0,0x3ff5c0f0,0x3ff5c0b8};
    const u32 rx_values[]={0x20000000,0,0xe000,0x1e101e1c,0x00800000,0x80,0x800};
    for(unsigned i=0;i<7;++i) {
        PHY_CHECK(read(r,rx_addresses[i],value)==OK);
        PHY_CHECK(write(r,rx_addresses[i],rx_values[i],error)==OK);
        PHY_CHECK(read(r,0x60000000+(rx_addresses[i]-0x3ff40000),value)==OK && value==rx_values[i]);
    }
    PHY_CHECK(write(r,0x3ff5c85c,0x10,error)==ERROR);
    PHY_CHECK(write(r,0x3ff5c0f0,0x100,error)==ERROR);
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff5ccb8,value)==OK && value==0x20000000);
    reset_baseband(r);
    for(unsigned i=0;i<7;++i)PHY_CHECK(read(r,rx_addresses[i],value)==OK && value==0);
    r[0x3ff000d0]=2;
    PHY_CHECK(write(r,0x3ff460a0,0x3c000,error)==ERROR);
    PHY_CHECK(write(r,0x3ff5c030,0x10,error)==OK);
    r[0x3ff000d0]=0;
    /* Complete Espressif fe_reg_init configuration sequence, including both
     * FE register banks. No measurement/result/status register is invented. */
    PHY_CHECK(write(r,0x3ff5c01c,0x000c00bf,error)==OK);
    const u32 fe_addresses[]={0x3ff460a0,0x3ff45114,0x3ff450dc,0x3ff450d8,0x3ff460b8,0x3ff4609c,0x3ff4604c};
    const u32 fe_values[]={0x01000000,0x200,0xc0000000,0x02000000,0x20000000,0x4000,0xc800};
    for(unsigned i=0;i<7;++i) {
        PHY_CHECK(read(r,fe_addresses[i],value)==OK);
        PHY_CHECK(write(r,fe_addresses[i],value|fe_values[i],error)==OK);
        PHY_CHECK(read(r,0x60000000+(fe_addresses[i]-0x3ff40000),value)==OK && value==fe_values[i]);
    }
    PHY_CHECK(write(r,0x6000609c,0x4014,error)==OK); /* phy_close_pa release controls */
    PHY_CHECK(read(r,0x3ff4609c,value)==OK && value==0x4014);
    PHY_CHECK(write(r,0x3ff45114,0x201,error)==ERROR);
    PHY_CHECK(read(r,0x60005114,value)==OK && value==0x200);
    reset_frontend(r);
    for(unsigned i=0;i<7;++i)PHY_CHECK(read(r,fe_addresses[i],value)==OK && value==0);
    PHY_CHECK(read(r,0x3ff5c01c,value)==OK && value==0x000c00bf);
    PHY_CHECK(read(r,0x3ff460c0,value)==OK && value==0);
    PHY_CHECK(write(r,0x3ff460c0,0x80,error)==OK); /* ram_en_pwdet front-end route */
    PHY_CHECK(read(r,0x600060c0,value)==OK && value==0x80);
    PHY_CHECK(write(r,0x3ff460c0,0x81,error)==ERROR);
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff460c0,value)==OK && value==0x80);
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff460c0,value)==OK && value==0);
    r[0x3ff000d0]=2;
    PHY_CHECK(write(r,0x3ff460c0,0x80,error)==ERROR);
    r[0x3ff000d0]=0;
    reset_baseband(r);
    /* Real ROM rom_noise_floor_auto_set configuration and explicit sample
     * acknowledgement. Status/data follow elapsed acquisition, not writes. */
    PHY_CHECK(write(r,0x6001c018,0x40800004,error,100)==OK);
    PHY_CHECK(write(r,0x6001c018,0x50800004,error,100)==OK);
    PHY_CHECK(write(r,0x6001d044,1,error,100)==OK);
    PHY_CHECK(write(r,0x6001d050,1,error,100)==OK);
    PHY_CHECK(read(r,0x3ff5d050,value,100)==OK && value==0); /* acknowledge is a strobe */
    PHY_CHECK(read(r,0x6001c018,value,106)==OK && !(value&0x01000000));
    PHY_CHECK(read(r,0x6001c050,value,106)==OK && value==0);
    PHY_CHECK(read(r,0x6001c018,value,107)==OK && (value&0x01000000));
    PHY_CHECK(read(r,0x6001c050,value,107)==OK && value>=636 && value<=644);
    const u32 quiet=value;
    PHY_CHECK(write(r,0x3ff5c050,0x3ff,error,107)==ERROR);
    PHY_CHECK(read(r,0x3ff5c050,value,107)==OK && value==quiet);
    PHY_CHECK(set_noise_amplitude(r,12));
    PHY_CHECK(write(r,0x3ff5d050,1,error,200)==OK);
    PHY_CHECK(read(r,0x3ff5c018,value,206)==OK && !(value&0x01000000));
    PHY_CHECK(read(r,0x3ff5c018,value,207)==OK && (value&0x01000000));
    PHY_CHECK(read(r,0x3ff5c050,value,207)==OK && value>quiet+36);
    PHY_CHECK(!set_noise_amplitude(r,512));
    PHY_CHECK(set_noise_amplitude(r,0));
    const u64 later=0x100000010ULL;
    PHY_CHECK(write(r,0x3ff5d050,1,error,later)==OK);
    PHY_CHECK(read(r,0x3ff5c018,value,later+6)==OK && !(value&0x01000000));
    PHY_CHECK(read(r,0x3ff5c050,value,later+7)==OK && value==0);
    PHY_CHECK(read(r,0x3ff5c018,value,later+7)==OK && (value&0x01000000));
    /* rom_check_noise_floor resets with bit25 then resumes with bit23. */
    PHY_CHECK(write(r,0x3ff5c018,0x52000004,error,later+8)==OK);
    PHY_CHECK(read(r,0x3ff5c018,value,later+100)==OK && !(value&0x01000000));
    PHY_CHECK(write(r,0x3ff5c018,0x50000004,error,later+101)==OK);
    PHY_CHECK(write(r,0x3ff5c018,0x50800004,error,later+102)==OK);
    PHY_CHECK(read(r,0x3ff5c018,value,later+108)==OK && !(value&0x01000000));
    PHY_CHECK(read(r,0x3ff5c018,value,later+109)==OK && (value&0x01000000));
    PHY_CHECK(write(r,0x3ff5d050,1,error,later+110)==OK);
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff5c018,value,later+200)==OK && value==0);
    PHY_CHECK(read(r,0x3ff5c050,value,later+200)==OK && value==0);
    PHY_CHECK(write(r,0x3ff5c018,0x01000000,error,later+201)==OK);
    PHY_CHECK(read(r,0x3ff5c018,value,later+300)==OK && value==0); /* DONE is read-only */
    PHY_CHECK(write(r,0x3ff5d044,2,error)==ERROR);
    PHY_CHECK(read(r,0x3ff5d04c,value)==UNHANDLED);
    /* PBUS format and readback packing are from real ROM pbus_force_test,
     * pbus_rd_addr and pbus_rd_shift, including its six-block table. */
    r[0x3ff000d0]=0;reset_frontend(r);
    PHY_CHECK(read(r,0x3ff46094,value)==OK && value==0);
    PHY_CHECK(write(r,0x3ff46094,1,error,0)==OK);
    const u32 command=1|2|(1u<<2)|(0x155u<<6)|(2u<<15);
    PHY_CHECK(write(r,0x60006094,command,error,100)==OK);
    PHY_CHECK(read(r,0x600060a0,value,100)==OK && (value&0x80000000));
    PHY_CHECK(read(r,0x3ff460ac,value,100)==OK && value==0);
    PHY_CHECK(write(r,0x3ff46094,command^(1u<<6),error,100)==ERROR); /* in flight command is immutable */
    PHY_CHECK(read(r,0x3ff46094,value,100)==OK && value==command);
    PHY_CHECK(read(r,0x3ff460a0,value,101)==OK && !(value&0x80000000));
    PHY_CHECK(read(r,0x600060ac,value,101)==OK && value==(0x155u<<18));
    PHY_CHECK(write(r,0x3ff46094,command&~2u,error,101)==OK);
    const unsigned blocks[]={0,1,2,2,3,3,4,5};
    const unsigned selectors[]={1,1,1,2,1,2,1,1};
    const u32 addresses[]={0x3ff460a8,0x3ff460a8,0x3ff460ac,0x3ff460ac,0x3ff460b0,0x3ff460b0,0x3ff460b0,0x3ff460b4};
    const unsigned shifts[]={9,0,9,0,18,9,0,0};
    for(unsigned i=0;i<8;++i) {
        const u32 cmd=3|(blocks[i]<<2)|((i+10)<<6)|(selectors[i]<<15);
        PHY_CHECK(write(r,0x3ff46094,cmd,error,200+i*2)==OK);
        PHY_CHECK(read(r,addresses[i],value,201+i*2)==OK && ((value>>shifts[i])&511)==i+10);
        PHY_CHECK(write(r,0x60006094,cmd&~2u,error,201+i*2)==OK);
    }
    PHY_CHECK(read(r,0x3ff460ac,value,220)==OK && (value>>18)==0x155); /* neighboring cells survive */
    PHY_CHECK(write(r,0x3ff460ac,0,error,220)==ERROR);
    PHY_CHECK(write(r,0x3ff46094,3|(6<<2)|(1<<15),error,220)==ERROR);
    PHY_CHECK(write(r,0x3ff46094,3|(4<<2)|(2<<15),error,220)==ERROR);
    PHY_CHECK(write(r,0x3ff46094,3|(2<<2),error,220)==ERROR);
    PHY_CHECK(write(r,0x3ff46094,2|(1<<15),error,220)==ERROR); /* no debug bus ownership */
    PHY_CHECK(write(r,0x3ff46094,0x20001,error,220)==ERROR);
    PHY_CHECK(write(r,0x3ff460a0,0x81000000,error,220)==OK); /* BUSY is read-only */
    PHY_CHECK(read(r,0x3ff460a0,value,220)==OK && value==0x01000000);
    const u64 transfer_time=0x200000010ULL;
    PHY_CHECK(write(r,0x3ff46094,command,error,transfer_time)==OK);
    reset_baseband(r);
    PHY_CHECK(read(r,0x3ff460a0,value,transfer_time)==OK && (value&0x80000000));
    reset_frontend(r);
    PHY_CHECK(read(r,0x3ff460a0,value,transfer_time+100)==OK && value==0);
    PHY_CHECK(read(r,0x3ff460ac,value,transfer_time+100)==OK && value==0);
    PHY_CHECK(read(r,0x3ff46094,value,transfer_time+100)==OK && value==0);
    /* set_chan_freq_hw_init sets the MAC channel-transition timing word.
     * Unknown low-half controls and neighboring MAC registers stay strict. */
    PHY_CHECK(read(r,0x60033d38,value)==OK && value==0);
    PHY_CHECK(write(r,0x60033d38,0x0c800000,error)==OK);
    PHY_CHECK(read(r,0x3ff73d38,value)==OK && value==0x0c800000);
    PHY_CHECK(write(r,0x3ff73d38,0x0c800001,error)==ERROR);
    PHY_CHECK(read(r,0x60033d38,value)==OK && value==0x0c800000);
    PHY_CHECK(read(r,0x60033d3c,value)==UNHANDLED);
    reset_baseband(r);reset_frontend(r);
    PHY_CHECK(read(r,0x3ff73d38,value)==OK && value==0x0c800000);
    r[0x3ff000d0]=4;
    PHY_CHECK(write(r,0x60033d38,0,error)==ERROR);
    r[0x3ff000d0]=0;
    reset_mac(r);
    PHY_CHECK(read(r,0x60033d38,value)==OK && value==0);
#undef PHY_CHECK
    return checks;
}
#ifdef ESP32_PHY_TEST_MAIN
int main(){int n=esp32_phy_tests();if(n<0)return 1;printf("ESP32 PHY: %d checks passed\n",n);return 0;}
#endif
