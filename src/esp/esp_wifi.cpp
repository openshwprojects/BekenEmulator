#include "esp/esp_machine.h"
#include "esp8266/esp8266_interrupts.h"
#include "esp8266/esp8266_wdev.h"

bool EspMachine::wifi_radio_state(WifiRadioState &state)
{
    state=WifiRadioState();
    if(chip_==esp::CHIP_ESP32) {
        if(flash_.empty())return false;
        state.pll_enabled=true;
        state.calibration_pending=false;
        state.frequency_khz=2412000;
        state.channel=1;
        state.digital_rx_enabled=(register_value(0x3ff73084)&0x80000000u)!=0;
        state.mac_rx_enabled=state.digital_rx_enabled;
        state.generation=register_value(0xffff3010u);
        return true;
    }
    if(chip_!=esp::CHIP_ESP8266)return false;
    state=esp8266_radio::snapshot(registers_,xtal_mhz_,elapsed_us());
    return true;
}

bool EspMachine::set_wifi_air(Esp8266WifiDma::Air *air)
{
    if(chip_==esp::CHIP_ESP32) {
        if(flash_.empty())return false;
        wifi_dma_.set_air(air);return true;
    }
    if(chip_!=esp::CHIP_ESP8266)return false;
    wifi_dma_.set_air(air);return true;
}

Esp8266WifiDma::ReceiveResult EspMachine::receive_wifi(const Bytes &frame,
    const Esp8266WifiDma::RxInfo &info,std::string *error)
{
    if(chip_==esp::CHIP_ESP32) {
        if(flash_.empty()) {
            if(error)*error="ESP32 Wi-Fi receive DMA is not implemented yet";
            return Esp8266WifiDma::RX_INVALID;
        }
        if(!(register_value(0x3ff73084)&0x80000000u)) {
            if(error)*error="ESP32 MAC RX is disabled";
            return Esp8266WifiDma::RX_DISABLED;
        }
        if(frame.size()<10 || frame.size()>2048) {
            if(error)*error="invalid 802.11 frame length for ESP32 RX DMA";
            return Esp8266WifiDma::RX_INVALID;
        }
        u32 desc=register_value(0x3ff7308c);
        if(desc<0x3ffae000u || desc>=0x40000000u) {
            const u32 last=register_value(0x3ff73090);
            u32 next=0;
            if(last>=0x3ffae000u && last<0x40000000u && read(last+8,4,next,false) &&
               next>=0x3ffae000u && next<0x40000000u)
                desc=next;
        }
        if(desc<0x3ffae000u || desc>=0x40000000u) {
            if(error)*error="no active ESP32 RX DMA descriptor";
            return Esp8266WifiDma::RX_BUSY;
        }
        u32 w0=0,buf=0,next=0;
        if(!read(desc,4,w0,false) || !read(desc+4,4,buf,false) || !read(desc+8,4,next,false) ||
           !(w0&0x80000000u)) {
            if(error)*error="ESP32 RX DMA descriptor is not owned by hardware";
            return Esp8266WifiDma::RX_BUSY;
        }
        if(next<0x3ffae000u || next>=0x40000000u) {
            if(error)*error="ESP32 RX DMA sentinel descriptor reached";
            return Esp8266WifiDma::RX_NO_BUFFER;
        }
        const unsigned cap=w0&0xfffu,total_len=28u+(unsigned)frame.size()+4u;
        if(buf<0x3ffae000u || buf>=0x40000000u || total_len>cap) {
            if(error)*error="ESP32 RX DMA buffer too small or out of range";
            return Esp8266WifiDma::RX_BUSY;
        }
        const u32 m0_lo=register_value(0x3ff73000),m0_hi=register_value(0x3ff73004)&0xffffu;
        const u32 m1_lo=register_value(0x3ff73008),m1_hi=register_value(0x3ff7300c)&0xffffu;
        const bool if0_en=(register_value(0x3ff73024)&0x10000u)!=0;
        const bool if1_en=(register_value(0x3ff7302c)&0x10000u)!=0;
        const u32 a1_lo=u32(u8(frame[4]))|(u32(u8(frame[5]))<<8)|(u32(u8(frame[6]))<<16)|(u32(u8(frame[7]))<<24);
        const u32 a1_hi=u32(u8(frame[8]))|(u32(u8(frame[9]))<<8);
        u32 match_bit=1u<<28;
        if(if1_en && a1_lo==m1_lo && a1_hi==m1_hi)match_bit=1u<<29;
        else if(if0_en && a1_lo==m0_lo && a1_hi==m0_hi)match_bit=1u<<28;
        else if(if1_en && ((u8(frame[0])&0xfcu)==0x40u || (u8(frame[0])&0xfcu)==0x00u ||
                           ((u8(frame[0])&0x0cu)==0x08u && (u8(frame[1])&3u)==1u)))match_bit=1u<<29;
        else if(if0_en)match_bit=1u<<28;
        else if(if1_en)match_bit=1u<<29;
        const u8 rssi_byte=u8(info.rssi_dbm+96);
        const u32 mpdu_fcs_len=(u32)frame.size()+4u;
        const u32 hdr[7]={
            u32(rssi_byte)|(11u<<8)|match_bit,
            0u,
            u32(elapsed_us()),
            0u,
            0u,
            0u,
            (mpdu_fcs_len&0xfffu)|((mpdu_fcs_len&0xfffu)<<12)
        };
        for(unsigned i=0;i<7;++i)write(buf+i*4,4,hdr[i]);
        for(size_t i=0;i<frame.size();++i)write(buf+28+(u32)i,1,u8(frame[i]));
        write(buf+28+(u32)frame.size(),4,0u);
        const u32 new_w0=(w0&0xfffu)|((total_len&0xfffu)<<12)|0x40000000u;
        write(desc,4,new_w0);
        registers_[0x3ff73090]=desc;
        registers_[0x3ff73094]=desc;
        registers_[0x3ff73098]=buf;
        registers_[0x3ff7308c]=(next>=0x3ffae000u && next<0x40000000u)?next:0x3ff00000u;
        registers_[0xffff3004u]=register_value(0xffff3004u)+1;
        registers_[0x3ff73c48]=register_value(0x3ff73c48)|0x04u;
        const u32 ena=register_value(0x3ff73c40);
        interrupts_.set_source(0,(register_value(0x3ff73c48)&(ena|(ena?0x04u:0u)))!=0);
        return Esp8266WifiDma::RX_ACCEPTED;
    }
    if(chip_!=esp::CHIP_ESP8266) {
        if(error)*error="ESP32 Wi-Fi receive DMA is not implemented yet";
        return Esp8266WifiDma::RX_INVALID;
    }
    const Esp8266WifiDma::ReceiveResult result=wifi_dma_.receive(registers_,frame,info,esp32_apb_ticks(),error);
    esp8266_timer_check_ticks_=0;
    esp8266_interrupts::update_mac(registers_,cpu_);return result;
}
