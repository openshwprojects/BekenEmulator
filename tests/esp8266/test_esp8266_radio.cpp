#include "esp8266/esp8266_radio.h"
#include <stdio.h>

namespace {
void write(std::map<u32,u32> &regs,u32 address,u32 value)
{
    esp8266_radio::note_write(regs,address,regs[address],value);
    regs[address]=value;
}
void tune(std::map<u32,u32> &regs,u32 divider)
{
    write(regs,0xfe000063,7); // ROM rom_write_rfpll_sdm disables then enables SDM
    write(regs,0xfe000363,divider>>16);
    write(regs,0xfe000463,(divider>>8)&255);
    write(regs,0xfe000563,divider&255);
    write(regs,0xfe000063,23);
}
}

int esp8266_radio_tests()
{
    int checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { printf("[FAIL] ESP8266 radio line%d: %s\n",__LINE__,#x);return -1; } } while(0)
    std::map<u32,u32> regs;
    esp8266_radio::State state=esp8266_radio::snapshot(regs,26,0);
    CHECK(!state.frequency_khz && !state.channel && !state.tx_ready() && !state.rx_ready());
    CHECK(regs.empty()); // inspection does not manufacture programmed registers
    tune(regs,0x5bb13b); // official SDK channel1 divider with26MHz XTAL
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.frequency_khz==2412000 && state.channel==1 && state.tx_ready());
    CHECK(!state.digital_rx_enabled && !state.mac_rx_enabled && !state.rx_ready());
    write(regs,0x60009a2c,1u<<19);
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.digital_rx_enabled && !state.rx_ready());
    write(regs,0x3ff20004,1u<<31);
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.mac_rx_enabled && state.rx_ready());
    u64 generation=state.generation;
    write(regs,0x60009b08,1u<<27); // stop_dig_rx, independent of MAC enable
    CHECK(!esp8266_radio::snapshot(regs,26,0).rx_ready());
    write(regs,0x60009b08,0);
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.rx_ready() && state.generation>generation);
    generation=state.generation;
    write(regs,0x60009b08,0x1234); // unrelated receive configuration
    write(regs,0x60009a2c,(1u<<19)|0x1234);
    write(regs,0x3ff20004,(1u<<31)|0x1234);
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.generation==generation && state.rx_ready());
    write(regs,0x600005c8,0x00300000); // ram_set_channel_freq inhibit bits23:20
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.channel_tuning && !state.tx_ready() && !state.rx_ready());
    write(regs,0x600005c8,0x00010000); // pm_open_rf clears power gates17:16
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(!state.channel_tuning && state.power_blocked && !state.tx_ready());
    write(regs,0x600005c8,0);
    write(regs,0xfe040062,0x40); // guest holds the RFPLL calibration in reset
    state=esp8266_radio::snapshot(regs,26,0);
    CHECK(state.calibration_held && !state.tx_ready());
    write(regs,0xfe040062,0);
    CHECK(esp8266_radio::snapshot(regs,26,0).tx_ready()); // fast tune need not recalibrate
    regs[0xffff2004]=20;regs[0xffff2008]=0;regs[0xffff2014]=134; // actual calibration engine deadline
    state=esp8266_radio::snapshot(regs,26,19);
    CHECK(state.calibration_pending && !state.tx_ready() && !regs.count(0xfe040762));
    state=esp8266_radio::snapshot(regs,26,20);
    CHECK(!state.calibration_pending && state.tx_ready());
    CHECK(regs[0xfe040762]==0x80 && regs[0xfe040562]==134);
    CHECK(regs[0xffff2004]==0 && regs[0xffff2008]==0);
    generation=state.generation;
    tune(regs,0x5f6276); // channel14, not channel15 on the5MHz grid
    state=esp8266_radio::snapshot(regs,26,20);
    CHECK(state.channel==14 && state.frequency_khz==2484000 && state.generation>generation);
    tune(regs,0x306666); // channel1 with40MHz XTAL
    state=esp8266_radio::snapshot(regs,40,20);
    CHECK(state.channel==1 && state.frequency_khz==2412000 && state.tx_ready());
    CHECK(esp8266_radio::snapshot(regs,26,20).channel==0); // cannot assume board XTAL
    tune(regs,0x67638e);state=esp8266_radio::snapshot(regs,24,20);
    CHECK(state.channel==6 && state.frequency_khz==2437000); // third supported board crystal
    tune(regs,0x5ec4ec);state=esp8266_radio::snapshot(regs,26,20);
    CHECK(state.channel==13 && state.frequency_khz==2472000);
    tune(regs,0x3068f6);state=esp8266_radio::snapshot(regs,40,20);
    CHECK(state.channel==1 && state.frequency_khz==2412300); // fractional crystal trim
    tune(regs,0x306c60);state=esp8266_radio::snapshot(regs,40,20);
    CHECK(state.channel==0 && !state.tx_ready()); // no guessed channel far from its center
    tune(regs,0);state=esp8266_radio::snapshot(regs,26,20);
    CHECK(state.channel==0 && !state.tx_ready());
    tune(regs,0x5bb13b);write(regs,0xfe000063,7);
    state=esp8266_radio::snapshot(regs,26,20);
    CHECK(!state.pll_enabled && !state.tx_ready());
    regs.clear();state=esp8266_radio::snapshot(regs,26,20);
    CHECK(state.generation==0 && state.channel==0 && !state.rx_ready());
#undef CHECK
    return checks;
}
