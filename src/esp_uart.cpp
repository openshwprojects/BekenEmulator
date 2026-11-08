#include "esp_uart.h"
#include <string.h>

namespace {
const u32 RX_FULL=1,TX_EMPTY=2,RX_OVERFLOW=16,RX_TIMEOUT=256,TX_DONE=0x4000;
}

EspUart::EspUart() { reset(true); }

void EspUart::reset(bool esp32,bool keep_output) {
    esp32_=esp32;timeout_armed_=false;received_at_=0;
    memset(registers_,0,sizeof(registers_));
    registers_[0x14/4]=0x2b6;
    registers_[0x20/4]=(esp32?0x08000000u:0)|0x1c;
    registers_[0x24/4]=0x6060;
    registers_[0x28/4]=registers_[0x2c/4]=0xfffff;
    if(esp32) {
        registers_[0x58/4]=0x88; // one128byteblock perFIFO
        registers_[0x68/4]=registers_[0x6c/4]=0xfffff;
        registers_[0x78/4]=0x15122500;registers_[0x7c/4]=0x500;
    }
    raw_=TX_EMPTY;rx_.clear();if(!keep_output)output_.clear();
}

bool EspUart::valid_offset(u32 offset) const {
    return !(offset&3) && (offset<=0x30 || (esp32_ && offset>=0x34 && offset<=0x6c) || offset==0x78 || offset==0x7c);
}

u64 EspUart::timeout_ticks(unsigned apb_mhz) const {
    const unsigned threshold=(registers_[0x24/4]>>24)&127;
    if(!(registers_[0x24/4]&0x80000000u) || !threshold) return 0;
    const u32 divider=registers_[0x14/4];
    const u64 div16=u64(divider&0xfffff)*16+(esp32_?(divider>>20)&15:0);
    if(!div16) return 0;
    if(!esp32_) {
        // ESP8266 RTOS SDK documents timeout in configured UART symbol periods.
        const u32 conf=registers_[0x20/4];
        const unsigned stop=(conf>>4)&3;
        const unsigned half_bits=2+2*(5+((conf>>2)&3)+((conf>>1)&1))+(stop==2?3:stop==3?4:2);
        return (div16*threshold*half_bits+31)/32;
    }
    // ESP-IDF esp32/uart_ll.h: APB counts baud/8. REF_TICK mode needs
    // threshold*8 for the same bit duration due to the silicon divider quirk.
    if(registers_[0x20/4]&0x08000000u) return (div16*threshold+1)/2;
    return (div16*threshold*apb_mhz+127)/128;
}

void EspUart::advance(u64 ticks,unsigned apb_mhz) {
    update_fifo_flags();
    if(!timeout_armed_ || rx_.empty()) return;
    const u64 duration=timeout_ticks(apb_mhz);
    if(duration && ticks>=received_at_ && ticks-received_at_>=duration) {
        raw_|=RX_TIMEOUT;timeout_armed_=false;
    }
}

bool EspUart::interrupt() const { return (raw_&registers_[0xc/4])!=0; }

bool EspUart::next_event_ticks(u64 ticks,unsigned apb_mhz,u64 &delta) const {
    if(!timeout_armed_ || rx_.empty() || !(registers_[0xc/4]&RX_TIMEOUT) || (raw_&RX_TIMEOUT)) return false;
    const u64 duration=timeout_ticks(apb_mhz);
    if(!duration) return false;
    const u64 elapsed=ticks>=received_at_?ticks-received_at_:0;
    delta=elapsed<duration?duration-elapsed:1;
    return true;
}

void EspUart::update_fifo_flags() {
    unsigned full=registers_[0x24/4]&127;
    if(!full) full=1;
    if(rx_.size()>=full) raw_|=RX_FULL;else raw_&=~RX_FULL;
    // The FIFO drains synchronously to the host, so TXempty stays asserted
    // while its threshold is enabled. W1C cannot remove an active condition.
    if((registers_[0x24/4]>>8)&127)raw_|=TX_EMPTY;else raw_&=~TX_EMPTY;
    if(rx_.empty()) timeout_armed_=false;
}

void EspUart::receive(const Bytes &bytes,u64 ticks,unsigned apb_mhz) {
    advance(ticks,apb_mhz);
    if(bytes.empty() || (registers_[0x20/4]&(1u<<17))) return;
    for(size_t i=0;i<bytes.size();++i) {
        if(rx_.size()<128) rx_.push_back((u8)bytes[i]);
        else raw_|=RX_OVERFLOW;
    }
    received_at_=ticks;timeout_armed_=true;update_fifo_flags();
}

void EspUart::transmit(u8 byte,u64 ticks,unsigned apb_mhz) {
    advance(ticks,apb_mhz);
    if(registers_[0x20/4]&(1u<<18)) return;
    output_+=(char)byte;
    if((registers_[0x24/4]>>8)&127) raw_|=TX_EMPTY;
    if(esp32_) raw_|=TX_DONE;
    if(registers_[0x20/4]&(1u<<14)) receive(Bytes(1,(char)byte),ticks,apb_mhz);
}

bool EspUart::read(u32 offset,u64 ticks,unsigned apb_mhz,u32 &value) {
    if(!valid_offset(offset)) return false;
    advance(ticks,apb_mhz);
    switch(offset) {
    case 0:
        value=rx_.empty()?0:rx_.front();
        if(!rx_.empty()) rx_.pop_front();
        update_fifo_flags();break;
    case 4:value=raw_;break;
    case 8:value=raw_&registers_[0xc/4];break;
    case 0x10:value=0;break;
    case 0x1c:value=0x80008000u|(u32)rx_.size();break; // idleRXD/TXD, synchronousTXempty
    case 0x60:value=u32(rx_.size()%128)<<13;break; // WR-RD pointerdifference for ESP-IDF FIFOworkaround
    case 0x64:value=0;break; // noadditional128byteblocks inthis FIFO
    default:value=registers_[offset/4];break;
    }
    return true;
}

bool EspUart::write(u32 offset,u64 ticks,unsigned apb_mhz,u32 value) {
    if(!valid_offset(offset)) return false;
    advance(ticks,apb_mhz);
    switch(offset) {
    case 0:transmit((u8)value,ticks,apb_mhz);return true;
    case 4:case 8:case 0x1c:case 0x28:case 0x2c:case 0x30:
    case 0x5c:case 0x60:case 0x64:case 0x68:case 0x6c:return false;
    case 0xc:registers_[offset/4]=value&(esp32_?0x7ffffu:0x1ffu);break;
    case 0x10:
        raw_&=~value;
        if(value&RX_TIMEOUT) { received_at_=ticks;timeout_armed_=!rx_.empty(); }
        break;
    case 0x14:registers_[offset/4]=value&(esp32_?0xffffffu:0xfffffu);break;
    case 0x20:
        registers_[offset/4]=value&(esp32_?0x0fffffffu:0x01ffffffu);
        if(value&(1u<<17)) { rx_.clear();raw_&=~(RX_FULL|RX_OVERFLOW|RX_TIMEOUT);timeout_armed_=false; }
        if(value&(1u<<18)) raw_&=~TX_DONE;
        break;
    case 0x24:
        registers_[offset/4]=value&0xffff7f7fu;
        update_fifo_flags();
        if((value>>8)&127) raw_|=TX_EMPTY;
        break;
    default:registers_[offset/4]=value;break;
    }
    return true;
}
