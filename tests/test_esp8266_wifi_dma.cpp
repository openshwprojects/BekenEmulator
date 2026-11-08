#include "esp8266_wifi_dma.h"
#include "esp8266_wdev.h"
#ifndef ESP8266_WIFI_DMA_TEST_MAIN
#include "esp_machine.h"
#endif
#include <stdio.h>

namespace {
class DmaRam:public XtensaBus {
public:
    Bytes memory;u32 fail_read,fail_write;unsigned writes;
    DmaRam():memory(0x20000,'\0'),fail_read(0),fail_write(0),writes(0){}
    bool read(u32 address,unsigned size,u32 &value,bool instruction) {
        if(instruction || address==fail_read || address<0x3ffe0000 ||
            u64(address)+size>0x40000000)return false;
        value=0;for(unsigned i=0;i<size;++i)value|=u32((u8)memory[address-0x3ffe0000+i])<<(i*8);return true;
    }
    bool write(u32 address,unsigned size,u32 value) {
        if(address==fail_write || address<0x3ffe0000 || u64(address)+size>0x40000000)return false;
        ++writes;for(unsigned i=0;i<size;++i)memory[address-0x3ffe0000+i]=char(value>>(i*8));return true;
    }
    void descriptor(u32 address,u32 buffer,unsigned length,u32 next,bool eof=true) {
        write(address,4,0x80000000u|(eof?0x40000000u:0)|length|(length<<12));
        write(address+4,4,buffer);write(address+8,4,next);
    }
};
class AckPeer:public Esp8266WifiDma::Air {
public:
    unsigned count;bool ack;Bytes bytes;
    AckPeer():count(0),ack(true){}
    bool transmit(const Esp8266WifiDma::Frame &frame){++count;bytes=frame.bytes;return ack;}
};
void setup(DmaRam &ram,Esp8266WifiDma::Registers &regs,unsigned queue=0,bool multicast=true)
{
    const u32 block=0x3ff20dc0-24*queue;
    Bytes frame(24,'\0');frame[0]=0x40;frame[4]=multicast?char(255):2;
    ram.memory.replace(0x10000,frame.size(),frame);
    ram.descriptor(0x3ffe8000,0x3fff0000,24,0);
    regs[block]=0;regs[block+8]=24|(11u<<12); // 6Mbps OFDM
}
Bytes receive_setup(DmaRam &ram,Esp8266WifiDma::Registers &regs)
{
    regs.clear();
    regs[0x3ff20004]=0x80000000u;regs[0x3ff20008]=0x3ffe8000;
    regs[0x3ff20c48]=2;regs[0x3ff20c4c]=0x100;
    regs[0x3ff20c58]=0xffffffff;regs[0x3ff20c5c]=0x1ffff;
    regs[0x3ff20c28]=2;regs[0x3ff20c2c]=0x300;
    regs[0x3ff20c38]=0xffffffff;regs[0x3ff20c3c]=0x1ffff;
    esp8266_wdev::write(regs,0x3ff20c18,1u<<8);
    ram.memory.assign(0x20000,char(0xa5));
    ram.descriptor(0x3ffe8000,0x3fff0000,64,0x3ffe8010,false);
    ram.descriptor(0x3ffe8010,0x3fff0100,64,0x3ffe8020,false);
    ram.descriptor(0x3ffe8020,0x3fff0200,4,0,false);
    Bytes frame(24,'\0');frame[0]=0x40;
    for(unsigned i=0;i<3;++i){frame[4+i*6]=2;frame[9+i*6]=char(i+1);}
    return frame;
}
int receive_tests()
{
    int checks=0;
#define RX_CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP8266 Wi-Fi RX line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    DmaRam ram;Esp8266WifiDma dma(ram);Esp8266WifiDma::Registers regs;
    Esp8266WifiDma::RxInfo info;info.rate=11;info.channel=6;
    std::string error;u32 value=0;u64 ticks=0;Bytes frame=receive_setup(ram,regs);
    const Bytes original=ram.memory;const unsigned writes=ram.writes;
    RX_CHECK(dma.read(regs,0x3ff20018,value) && value==0);
    RX_CHECK(dma.read(regs,0x3ff2001c,value) && value==0);
    RX_CHECK(dma.read(regs,0x3ff20014,value) && value==0 && dma.receptions()==0);
    RX_CHECK(ram.memory==original && ram.writes==writes && !esp8266_wdev::interrupt(regs));
    RX_CHECK(dma.write(regs,0x3ff20018,1,0)==Esp8266WifiDma::UNHANDLED);
    RX_CHECK(dma.write(regs,0x3ff2001c,1,0)==Esp8266WifiDma::UNHANDLED);
    RX_CHECK(dma.receive(regs,frame,info,800,&error)==Esp8266WifiDma::RX_ACCEPTED);
    RX_CHECK(ram.memory==original && ram.writes==writes && !esp8266_wdev::interrupt(regs));
    RX_CHECK(dma.read(regs,0x3ff20018,value) && value==0 && dma.receptions()==0);
    RX_CHECK(dma.read(regs,0x3ff2001c,value) && value==0 && dma.receptions()==0);
    RX_CHECK(dma.next_event_ticks(800,ticks) && ticks==70*80);
    dma.advance(regs,800+70*80-1);RX_CHECK(dma.receptions()==0 && ram.memory==original);
    dma.advance(regs,800+70*80);
    RX_CHECK(dma.receptions()==1 && dma.receive_error().empty());
    RX_CHECK(esp8266_wdev::read(regs,0x3ff20c20,0,value) && value==(1u<<8));
    RX_CHECK(dma.read(regs,0x3ff20014,value) && value==0x3ffe8000);
    RX_CHECK(dma.read(regs,0x3ff210fc,value) && value==10);
    RX_CHECK(dma.read(regs,0x3ff20018,value) && value==0);
    RX_CHECK(dma.read(regs,0x3ff2001c,value) && value==0);
    RX_CHECK(ram.read(0x3ffe8000,4,value,false) && value==0xc0028040u);
    RX_CHECK(ram.read(0x3ffe8008,4,value,false) && value==0x3ffe8010);
    RX_CHECK(ram.read(0x3fff0000,4,value,false) && value==0x501c0b36);
    RX_CHECK(ram.read(0x3fff0004,4,value,false) && value==0);
    RX_CHECK(ram.read(0x3fff0008,4,value,false) && value==0xa0060000u);
    RX_CHECK(ram.memory.substr(0x1000c,24)==frame);
    /* Independent CRC fixture computed with Python zlib.crc32, IEEE CRC32. */
    RX_CHECK(ram.read(0x3fff0024,4,value,false) && value==0x79d3c8fe);
    RX_CHECK((u8)ram.memory[0x10028]==0xa5 && !dma.next_event_ticks(6400,ticks));
    RX_CHECK(dma.receive(regs,frame,info,6400,&error)==Esp8266WifiDma::RX_BUSY);
    esp8266_wdev::write(regs,0x3ff20c24,1u<<8);dma.acknowledge(regs,1u<<8,6400);
    RX_CHECK(dma.receive(regs,frame,info,6400,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,12000);
    RX_CHECK(dma.read(regs,0x3ff20014,value) && value==0x3ffe8010 && dma.receptions()==2);
    esp8266_wdev::write(regs,0x3ff20c24,1u<<8);dma.acknowledge(regs,1u<<8,12000);
    const Bytes exhausted=ram.memory;
    RX_CHECK(dma.receive(regs,frame,info,12000,&error)==Esp8266WifiDma::RX_NO_BUFFER);
    RX_CHECK(ram.memory==exhausted && !esp8266_wdev::interrupt(regs));

    /* Split input across descriptor buffers; owner is ignored and preserved. */
    dma.reset();frame=receive_setup(ram,regs);
    ram.descriptor(0x3ffe8000,0x3fff0000,16,0x3ffe8010,false);
    ram.write(0x3ffe8000,4,16|(16u<<12));
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,5600);
    RX_CHECK(ram.read(0x3ffe8000,4,value,false) && value==(16|(16u<<12)));
    RX_CHECK(ram.read(0x3ffe8010,4,value,false) && value==0xc0018040u);
    RX_CHECK(ram.memory.substr(0x1000c,4)+ram.memory.substr(0x10100,20)==frame);
    RX_CHECK(ram.read(0x3fff0114,4,value,false) && value==0x79d3c8fe);

    /* ROM lldesc_build_chain preserves bits28:24 in fresh descriptors.
     * They are software metadata, not a DMA offset into the RX buffer. */
    dma.reset();frame=receive_setup(ram,regs);
    ram.write(0x3ffe8000,4,0x9f040040u);
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,5600);
    RX_CHECK(ram.read(0x3ffe8000,4,value,false) && value==0xdf028040u);
    RX_CHECK(ram.memory.substr(0x1000c,24)==frame);
    RX_CHECK(ram.read(0x3fff0024,4,value,false) && value==0x79d3c8fe);

    /* Validation must leave every descriptor, payload byte and IRQ untouched. */
    for(unsigned fault=0;fault<10;++fault) {
        dma.reset();frame=receive_setup(ram,regs);ram.fail_read=0;
        if(fault==0)ram.write(0x3ffe8004,4,0x60000000);
        if(fault==1)ram.write(0x3ffe8004,4,0x3ffffff0);
        if(fault==2)regs[0x3ff20008]=0x3ffe8001;
        if(fault==3)ram.write(0x3ffe8008,4,0x3ffe8011);
        if(fault==4)ram.write(0x3ffe8000,4,0x80000000);
        if(fault==5){ram.descriptor(0x3ffe8000,0x3fff0000,16,0x3ffe8000,false);}
        if(fault==6)ram.write(0x3ffe8004,4,0x3ffe8000);
        if(fault==7){ram.descriptor(0x3ffe8000,0x3fff0000,16,0x3ffe8010,false);ram.write(0x3ffe8014,4,0x3fff0000);}
        if(fault==8)ram.fail_read=0x3ffe8004;
        if(fault==9)ram.fail_read=0x3fff0000;
        const Bytes before=ram.memory;const unsigned before_writes=ram.writes;
        RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID && !error.empty());
        RX_CHECK(ram.memory==before && ram.writes==before_writes && !esp8266_wdev::interrupt(regs));
    }
    ram.fail_read=0;
    dma.reset();frame=receive_setup(ram,regs);regs[0x3ff20004]=0;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_DISABLED);
    regs[0x3ff20004]=0x80000000;frame[9]=9;frame[21]=9;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_FILTERED && dma.receptions()==0);
    frame[0]=0x41;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    frame=receive_setup(ram,regs);frame[1]=0x40;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    RX_CHECK(dma.read(regs,0x3ff20018,value) && value==0);
    RX_CHECK(dma.read(regs,0x3ff2001c,value) && value==0);
    RX_CHECK(dma.read(regs,0x3ff20014,value) && value==0 && dma.receptions()==0);
    RX_CHECK(!esp8266_wdev::interrupt(regs));
    frame[1]=0;info.rate=4;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    info.rate=11;info.channel=0;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    info.channel=6;info.rssi_dbm=-200;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    info.rssi_dbm=-42;
    RX_CHECK(dma.receive(regs,Bytes(4092,'\0'),info,0,&error)==Esp8266WifiDma::RX_INVALID);
    RX_CHECK(dma.receive(regs,Bytes(9,'\0'),info,0,&error)==Esp8266WifiDma::RX_INVALID);

    /* QoS carries aggregation and optional HT-control bits independently of
     * the PHY rate. Reject those formats before changing any guest memory. */
    dma.reset();frame=receive_setup(ram,regs);frame[0]=(char)0x88;
    frame+=Bytes(2,'\0');frame[24]=(char)0x80;
    const Bytes before_qos=ram.memory;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    RX_CHECK(ram.memory==before_qos && !esp8266_wdev::interrupt(regs));
    frame[24]=0;frame[1]=(char)0x80;frame+=Bytes(4,'\0');
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_INVALID);
    RX_CHECK(ram.memory==before_qos && !esp8266_wdev::interrupt(regs));
    frame[1]=0;frame.resize(26);
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);

    /* A late RAM write fault rolls back completed writes and publishes no IRQ. */
    dma.reset();frame=receive_setup(ram,regs);const Bytes before_fault=ram.memory;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    ram.fail_write=0x3fff0010;dma.advance(regs,5600);
    RX_CHECK(!dma.receive_error().empty() && dma.receptions()==0 && !esp8266_wdev::interrupt(regs));
    RX_CHECK(ram.memory==before_fault && !dma.next_event_ticks(5600,ticks));
    ram.fail_write=0;
    /* Restart and cancellation are driven by actual guest register writes. */
    dma.reset();frame=receive_setup(ram,regs);const Bytes before_cancel=ram.memory;
    RX_CHECK(Esp8266WifiDma::handles(0x3ff20004) && Esp8266WifiDma::handles(0x3ff20008));
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    RX_CHECK((regs[0x3ff201d0]&0xf000)==0x1000);
    RX_CHECK(dma.write(regs,0x3ff20008,0x3ffe8010,1,&error)==Esp8266WifiDma::REJECTED);
    RX_CHECK(dma.write(regs,0x3ff20004,0,2,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,10000);
    RX_CHECK(ram.memory==before_cancel && dma.receptions()==0 && !dma.next_event_ticks(10000,ticks));
    RX_CHECK((regs[0x3ff201d0]&0xf000)==0 && !esp8266_wdev::interrupt(regs));
    RX_CHECK(dma.write(regs,0x3ff20008,0x3ffe8010,10000,&error)==Esp8266WifiDma::ACCEPTED);
    RX_CHECK(dma.write(regs,0x3ff20004,0x80000000u,10000,&error)==Esp8266WifiDma::ACCEPTED);
    RX_CHECK(dma.receive(regs,frame,info,10000,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,15600);
    RX_CHECK(dma.read(regs,0x3ff20014,value) && value==0x3ffe8010);
    RX_CHECK((u8)ram.memory[0x10000]==0xa5 && dma.receptions()==1);
    dma.reset();frame=receive_setup(ram,regs);
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    const Bytes before_reset=ram.memory;dma.reset();dma.advance(regs,5600);
    RX_CHECK(ram.memory==before_reset && dma.receptions()==0 && dma.receive_error().empty());

    /* Real comparator values select interface1. Broadcast still needs an
     * enabled BSSID match; it cannot invent a match for a disabled filter. */
    frame=receive_setup(ram,regs);
    regs[0x3ff20c5c]=regs[0x3ff20c3c]=0;
    regs[0x3ff20c50]=2;regs[0x3ff20c54]=0x100;
    regs[0x3ff20c60]=0xffffffff;regs[0x3ff20c64]=0x1ffff;
    regs[0x3ff20c30]=2;regs[0x3ff20c34]=0x300;
    regs[0x3ff20c40]=0xffffffff;regs[0x3ff20c44]=0x1ffff;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,5600);
    RX_CHECK(ram.read(0x3fff0000,4,value,false) && (value&0xf0000000u)==0xa0000000u);
    dma.reset();frame=receive_setup(ram,regs);frame.replace(4,6,Bytes(6,char(0xff)));
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,5600);
    RX_CHECK(ram.read(0x3fff0000,4,value,false) && (value&0xf0001000u)==0x40001000u);
    dma.reset();frame=receive_setup(ram,regs);regs[0x3ff20c58]=0xffffff00;
    frame[4]=6; // guest mask deliberately ignores the first MAC octet
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    dma.advance(regs,5600);
    RX_CHECK(ram.read(0x3fff0000,4,value,false) && (value&(1u<<28)));

    /* To-DS and From-DS frames select their BSSID from the actual802.11
     * address layout, while DA/receiver matching uses address1. */
    for(unsigned direction=1;direction<=2;++direction) {
        dma.reset();frame=receive_setup(ram,regs);frame[0]=8;frame[1]=char(direction);
        const unsigned bssid=direction==1?4:10;
        frame[bssid+5]=3;regs[0x3ff20c5c]=0; // only BSSID0 enabled
        RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
        dma.advance(regs,5600);
        RX_CHECK(ram.read(0x3fff0000,4,value,false) && (value&0xf0000000u)==0x40000000u);
    }
    /* No free successor and unrecycledEOF are explicit buffer exhaustion. */
    for(unsigned missing=0;missing<2;++missing) {
        dma.reset();frame=receive_setup(ram,regs);
        ram.write(missing?0x3ffe8000:0x3ffe8008,4,missing?0xc0040040u:0);
        const Bytes before=ram.memory;
        RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_NO_BUFFER);
        RX_CHECK(ram.memory==before && !esp8266_wdev::interrupt(regs));
    }
    /* Rollback also covers a fault while publishing descriptor lengths. */
    dma.reset();frame=receive_setup(ram,regs);
    ram.descriptor(0x3ffe8000,0x3fff0000,16,0x3ffe8010,false);
    const Bytes before_publish=ram.memory;
    RX_CHECK(dma.receive(regs,frame,info,0,&error)==Esp8266WifiDma::RX_ACCEPTED);
    ram.fail_write=0x3ffe8010;dma.advance(regs,5600);ram.fail_write=0;
    RX_CHECK(ram.memory==before_publish && !dma.receive_error().empty() && !esp8266_wdev::interrupt(regs));
    RX_CHECK(dma.receive(regs,frame,info,6000,&error)==Esp8266WifiDma::RX_INVALID && !error.empty());
    dma.reset();frame=receive_setup(ram,regs);
    RX_CHECK(dma.receive(regs,frame,info,~u64(0)-1,&error)==Esp8266WifiDma::RX_INVALID);
#undef RX_CHECK
    return checks;
}
}
int esp8266_wifi_dma_tests()
{
    int checks=0;
#define CHECK(x) do{++checks;if(!(x)){printf("[FAIL] ESP8266 Wi-Fi DMA line %d: %s\n",__LINE__,#x);return -1;}}while(0)
    DmaRam ram;Esp8266WifiDma dma(ram);Esp8266WifiDma::Registers regs;std::string error;
    u64 ticks=0;u32 value=0;
    CHECK(Esp8266WifiDma::handles(0x3ff20dc4) && Esp8266WifiDma::handles(0x3ff20d1c));
    CHECK(!Esp8266WifiDma::handles(0x3ff20dc0) && !Esp8266WifiDma::handles(0x3ff20dc5));
    CHECK(dma.write(regs,0x3ff20dc0,0,0)==Esp8266WifiDma::UNHANDLED);
    setup(ram,regs);const unsigned writes=ram.writes;
    CHECK(esp8266_wdev::write(regs,0x3ff20c18,(1u<<18)|(1u<<19)));
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    CHECK(dma.inflight() && dma.queued()==0 && dma.transmissions()==0);
    CHECK(esp8266_wdev::read(regs,0x3ff20c20,0,value) && value==(1u<<18));
    CHECK(dma.next_event_ticks(0,ticks) && ticks==70*80); // 24 bytes + FCS, eleven OFDM symbols
    ram.memory[0x10000]=0; // descriptor DMA has already captured the frame
    dma.advance(regs,70*80-1);CHECK(dma.completed_frames().empty());
    dma.advance(regs,70*80);
    CHECK(dma.transmissions()==1 && dma.completed_frames().size()==1);
    CHECK(dma.completed_frames().back().bytes[0]==0x40 && dma.completed_frames().back().outcome==Esp8266WifiDma::SUCCESS);
    CHECK(ram.writes==writes); // descriptor ownership semantics are deliberately preserved
    CHECK(esp8266_wdev::read(regs,0x3ff20c20,0,value) && value==((1u<<18)|(1u<<19)));
    CHECK(regs[0x3ff20c84]==0 && !dma.next_event_ticks(70*80,ticks));
    CHECK(esp8266_wdev::write(regs,0x3ff20c24,(1u<<18)|(1u<<19)));
    dma.acknowledge(regs,(1u<<18)|(1u<<19),70*80);
    setup(ram,regs,0,false);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,10000,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,10000+70*80);
    CHECK(dma.completed_frames().size()==1 && dma.next_event_ticks(10000+70*80,ticks) && ticks>0);
    dma.advance(regs,10000+70*80+ticks);
    CHECK(dma.completed_frames().back().outcome==Esp8266WifiDma::ACK_TIMEOUT && (regs[0x3ff20c84]>>28)==5);
    dma.reset();regs.clear();setup(ram,regs,0,false);AckPeer peer;dma.set_air(&peer);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,70*80);CHECK(peer.count==1 && peer.bytes.size()==24);
    CHECK(dma.next_event_ticks(70*80,ticks));dma.advance(regs,70*80+ticks);
    CHECK(dma.completed_frames().back().outcome==Esp8266WifiDma::SUCCESS);
    /* ESP8266 TRM3.3.2 explicitly says the MAC ignores descriptor owner. */
    dma.reset();regs.clear();setup(ram,regs);
    ram.write(0x3ffe8000,4,0x40000000u|24|(24u<<12));
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,70*80);
    CHECK(dma.transmissions()==1 && dma.completed_frames().back().outcome==Esp8266WifiDma::SUCCESS);
    CHECK(ram.read(0x3ffe8000,4,value,false) && value==(0x40000000u|24|(24u<<12)));
    /* Real SDK TX descriptors set length but leave RX capacity at zero.
     * Neither zero nor a smaller nonzero capacity constrains a TX read. */
    dma.set_air(0);
    for(unsigned capacity=0;capacity<2;++capacity) {
        dma.reset();regs.clear();setup(ram,regs);
        ram.write(0x3ffe8000,4,0xc0000000u|capacity|(24u<<12));
        CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::ACCEPTED);
        dma.advance(regs,70*80);
        CHECK(dma.transmissions()==1 && dma.completed_frames().back().bytes.size()==24);
    }
    dma.set_air(&peer);
    /* Actual Gosund TX descriptor: all five hardware-reserved bits are1.
     * The ROM and SDK preserve them; DMA must use buf_ptr unchanged. */
    dma.reset();regs.clear();setup(ram,regs);dma.set_air(0);
    Bytes reused_frame(51,'\0');reused_frame[0]=0x40;reused_frame[4]=(char)0xff;
    ram.memory.replace(0x10000,reused_frame.size(),reused_frame);
    ram.write(0x3ffe8000,4,0xdf0335b8u);regs[0x3ff20dc8]=51|(11u<<12);
    CHECK(dma.write(regs,0x3ff20dc4,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,106*80);
    CHECK(dma.transmissions()==1 && dma.completed_frames().back().bytes==reused_frame);
    CHECK(ram.read(0x3ffe8000,4,value,false) && value==0xdf0335b8u);
    dma.set_air(&peer);
    /* DMA errors reject atomically, without publishing an interrupt or frame. */
    dma.reset();regs.clear();setup(ram,regs);
    ram.descriptor(0x3ffe8000,0x60000000,24,0);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    ram.descriptor(0x3ffe8000,0x3fff0000,12,0x3ffe8000,false);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    setup(ram,regs);ram.fail_read=0x3ffe8004;
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    ram.fail_read=0;ram.write(0x3ffe8000,4,0xc001900c);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    setup(ram,regs);ram.write(0x3ffe8000,4,0x80018000);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    setup(ram,regs);regs[0x3ff20dc8]|=1u<<24;
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    setup(ram,regs);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028001,0,&error)==Esp8266WifiDma::REJECTED);
    ram.descriptor(0x3ffe8000,0x3ffffff8,24,0);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    ram.descriptor(0x3ffe8000,0x3fff0000,12,0);
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    setup(ram,regs);ram.memory[0x10001]=0x40;
    CHECK(dma.write(regs,0x3ff20dc4,0xc1028000,0,&error)==Esp8266WifiDma::REJECTED);
    CHECK(peer.count==2 && dma.transmissions()==0);
    /* Two descriptors concatenate real data; no-ACK policy also applies
     * to a unicast destination explicitly configured without an ACK. */
    dma.set_air(0);setup(ram,regs,0,false);
    ram.descriptor(0x3ffe8000,0x3fff0000,12,0x3ffe8010,false);
    ram.descriptor(0x3ffe8010,0x3fff000c,12,0);
    ram.memory[0x10017]=0x5a;
    CHECK(dma.write(regs,0x3ff20dc4,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,70*80);
    CHECK(dma.completed_frames().back().bytes.size()==24 && dma.completed_frames().back().bytes[23]==0x5a);
    CHECK(dma.completed_frames().back().outcome==Esp8266WifiDma::SUCCESS);
    /* Backoff and serialized queues cannot overwrite the shared completion
     * status before the guest has acknowledged its pending event bits. */
    dma.reset();regs.clear();setup(ram,regs);
    regs[0x3ff20dc0]=2u<<12;
    CHECK(esp8266_wdev::write(regs,0x3ff20c18,(1u<<18)|(1u<<19)));
    CHECK(dma.write(regs,0x3ff20dc4,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    CHECK(dma.next_event_ticks(0,ticks) && ticks==18*80);
    CHECK(!esp8266_wdev::interrupt(regs) && (regs[0x3ff20410]&15)==0);
    setup(ram,regs,1);
    CHECK(dma.write(regs,0x3ff20dac,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED && dma.queued()==1);
    CHECK(dma.write(regs,0x3ff20dac,0xc0028000,0,&error)==Esp8266WifiDma::REJECTED && dma.queued()==1);
    dma.advance(regs,18*80-1);CHECK(!esp8266_wdev::interrupt(regs));
    dma.advance(regs,18*80);CHECK(esp8266_wdev::interrupt(regs) && (regs[0x3ff20410]&15)==1);
    CHECK(dma.write(regs,0x3ff20dc4,0,18*80,&error)==Esp8266WifiDma::ACCEPTED && dma.inflight());
    dma.advance(regs,88*80);
    CHECK(dma.completed_frames().size()==1 && dma.queued()==1 && regs[0x3ff20c84]==0);
    CHECK((regs[0x3ff20410]&15)==0);
    CHECK(esp8266_wdev::write(regs,0x3ff20c24,1u<<19));dma.acknowledge(regs,1u<<19,88*80);
    CHECK(dma.queued()==1 && !dma.inflight());
    CHECK(esp8266_wdev::write(regs,0x3ff20c24,1u<<18));dma.acknowledge(regs,1u<<18,88*80);
    CHECK(dma.queued()==0 && dma.inflight() && regs[0x3ff20c84]==(1u<<12));
    dma.advance(regs,158*80);
    CHECK(dma.completed_frames().size()==2 && dma.completed_frames().back().queue==1);
    CHECK(dma.completed_frames().back().started==88*80 && dma.completed_frames().back().completed==158*80);
    /* Disabling a queued/backoff frame prevents any radio transmission. */
    dma.reset();regs.clear();setup(ram,regs);regs[0x3ff20dc0]=3u<<12;
    CHECK(dma.write(regs,0x3ff20dc4,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    CHECK(dma.write(regs,0x3ff20dc4,0,1,&error)==Esp8266WifiDma::ACCEPTED);
    dma.advance(regs,1000000);
    CHECK(dma.transmissions()==0 && !dma.inflight() && !dma.next_event_ticks(1000000,ticks));
    /* CCK1Mbps includes its long192us PLCP. */
    dma.reset();regs.clear();setup(ram,regs);regs[0x3ff20dc8]=24;
    CHECK(dma.write(regs,0x3ff20dc4,0xc0028000,0,&error)==Esp8266WifiDma::ACCEPTED);
    CHECK(dma.next_event_ticks(0,ticks) && ticks==(192+28*8)*80);
#ifndef ESP8266_WIFI_DMA_TEST_MAIN
    esp::FlashImage layout;layout.chip=esp::CHIP_ESP8266;layout.app_index=layout.boot_index=0;
    esp::Image image;image.chip=layout.chip;image.entry_point=0x40100000;
    esp::Segment code;code.address=image.entry_point;code.data.assign(0x100,'\0');
    code.data.replace(0,3,"\x00\x70\x00",3); // WAITI0
    image.segments.push_back(code);layout.images.push_back(image);
    EspMachine machine(layout.chip);
    CHECK(machine.load(Bytes(0x100000,(char)0xff),layout,false));
    CHECK(machine.read(0x3ff20018,4,value,false) && value==0);
    CHECK(machine.read(0x3ff2001c,4,value,false) && value==0);
    CHECK(!machine.write(0x3ff20018,4,1) && !machine.write(0x3ff2001c,4,1));
    machine.cpu().set_special(XT_SR_VECBASE,image.entry_point);
    machine.cpu().set_special(XT_SR_PS,0);
    machine.cpu().set_special(XT_SR_INTENABLE,1);
    CHECK(machine.write(0x3ffe8000,4,0xc0018018) && machine.write(0x3ffe8004,4,0x3fff0000) && machine.write(0x3ffe8008,4,0));
    CHECK(machine.write(0x3fff0000,4,0x40) && machine.write(0x3fff0004,4,0xffffffff));
    CHECK(machine.write(0x3ff20dc8,4,24|(11u<<12)));
    CHECK(machine.write(0x3ff20c18,4,1u<<19)); // start masked; completion wakes CPU
    CHECK(machine.write(0x3ff20dc4,4,0xc0028000));
    CHECK(machine.read(0x3ff20410,4,value,false) && (value&15)==1);
    CHECK(machine.step()==XTENSA_OK && machine.cpu().pc()==image.entry_point+0x30);
    CHECK(machine.elapsed_us()==70 && machine.cpu().special(XT_SR_CCOUNT)==5600);
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==(1u<<19));
    CHECK(machine.read(0x3ff20410,4,value,false) && value==0);
    CHECK(machine.write(0x3ff20c24,4,(1u<<18)|(1u<<19)));
    CHECK(!(machine.cpu().special(XT_SR_INTERRUPT)&1));
    CHECK(machine.write(0x3fff0004,4,2)); // new real unicast packet requires ACK
    CHECK(machine.write(0x3ff20dc4,4,0xc1028000));
    CHECK(machine.write(0x60000700,4,0x80000000) && machine.step()==XTENSA_OK);
    CHECK(machine.reset_count()==1 && machine.cpu().pc()==0x40000080);
    CHECK(machine.read(0x3ff20c20,4,value,false) && value==0);
#endif
#undef CHECK
    const int receive_checks=receive_tests();if(receive_checks<0)return -1;checks+=receive_checks;
    return checks;
}
#ifdef ESP8266_WIFI_DMA_TEST_MAIN
int main(){int n=esp8266_wifi_dma_tests();if(n>=0)printf("ESP8266 Wi-Fi DMA: %d checks passed.\n",n);return n<0;}
#endif
