#include "esp8266/esp8266_wifi_dma.h"
#include "esp8266/esp8266_wdev.h"
#include "esp/esp_state.h"
namespace {
const u32 TX_START=1u<<18,TX_COMPLETE=1u<<19,TX_EVENTS=TX_START|TX_COMPLETE;
const u32 MASK=0x3ff20c18;
const u32 TX_STATUS=0x3ff20c84,TX_STATE=0x3ff20410;
const u32 RX_COMPLETE=1u<<8,RX_CONTROL=0x3ff20004,RX_HEAD=0x3ff20008;
const u32 RX_EOF=0x3ff20014,RX_TIME=0x3ff210fc,RX_STATE=0x3ff201d0;
const u32 RX_RATE=0x3ff2003c,RX_PENDING_RATE=esp_state::WDEV_8266+12;
u32 reg(const Esp8266WifiDma::Registers &regs,u32 address)
{
    Esp8266WifiDma::Registers::const_iterator it=regs.find(address);
    return it==regs.end()?0:it->second;
}
bool fail(std::string *error,const char *message){if(error)*error=message;return false;}
bool ram_address(u32 address,unsigned length)
{
    return address>=0x3ffe0000 && u64(address)+length<=0x40000000ull;
}
unsigned queue_index(u32 address){return (0x3ff20dc4-address)/24;}
unsigned airtime_us(unsigned rate,unsigned length)
{
    /* SDK FIXED_RATE defines the OFDM hardware codes8..15. IEEE802.11
     * legacy OFDM includes16 service bits,6 tail bits,4us symbols and the
     * 2.4GHz6us signal extension. CCK codes0..3/5..7 select long/short PLCP. */
    static const unsigned ofdm[]={48,24,12,6,54,36,18,9};
    static const unsigned cck_twice[]={2,4,11,22};
    const unsigned bits=(length+4)*8; // hardware-generated FCS
    if(rate>=8) {
        const unsigned per_symbol=ofdm[rate-8]*4;
        return 26+4*((16+bits+6+per_symbol-1)/per_symbol);
    }
    const unsigned twice=cck_twice[rate&3];
    return (rate>=4?96:192)+(bits*2+twice-1)/twice;
}
u32 crc32(const Bytes &bytes)
{
    u32 crc=0xffffffffu;
    for(size_t i=0;i<bytes.size();++i) {
        crc^=u8(bytes[i]);
        for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
void append_word(Bytes &bytes,u32 value)
{
    for(unsigned byte=0;byte<4;++byte)bytes+=char(value>>(byte*8));
}
bool comparator(const Esp8266WifiDma::Registers &regs,u32 address,u32 mask,
                const Bytes &frame,unsigned offset)
{
    const u32 high=reg(regs,mask+4);
    if(!(high&0x10000))return false;
    for(unsigned i=0;i<6;++i) {
        const unsigned shift=(i&3)*8;
        const u32 target=reg(regs,address+(i&4)),bits=reg(regs,mask+(i&4));
        if((u8(frame[offset+i])^u8(target>>shift))&u8(bits>>shift))return false;
    }
    return true;
}
struct RxPiece {
    u32 descriptor,flags,buffer,next;
    unsigned count;
};
bool overlaps(u32 first,unsigned count,u32 second,unsigned size)
{
    return u64(first)<u64(second)+size && u64(second)<u64(first)+count;
}
}

Esp8266WifiDma::Esp8266WifiDma(XtensaBus &bus):bus_(bus),air_(0),ack_timeout_us_(0){reset();}
void Esp8266WifiDma::reset()
{
    stage_=IDLE;queue_.clear();completed_.clear();deadline_=transmissions_=0;pending_events_=0;
    rx_pending_=rx_reported_=false;rx_cursor_=rx_eof_=rx_next_=0;
    rx_deadline_=rx_timestamp_=receptions_=0;rx_writes_.clear();rx_error_.clear();
}
bool Esp8266WifiDma::read(const Registers &regs,u32 address,u32 &value)const
{
    /* Both SDK wDev_ProcessFiq and the RTOS handler read these auxiliary
     * snapshots, then discard them before the ordinary legacy handoff.
     * Auxiliary state is unmodeled; neutral values suffice for that observed
     * legacy path. Actual completion is solely EOF14, the descriptor and event8;
     * reading these words cannot create a packet or an interrupt. */
    if(address==0x3ff20018 || address==0x3ff2001c){value=0;return true;}
    /* phy_get_bb_freqoffset selects its CCK/OFDM measurement path from
     * bits19:16 of the received PHY snapshot. Publish actual RX metadata
     * only after a successful DMA transaction, independently of IRQ ack. */
    if(address==RX_RATE){value=receptions_?reg(regs,RX_RATE):0;return true;}
    if(address!=RX_EOF && address!=RX_TIME)return false;
    value=reg(regs,address);return true;
}
bool Esp8266WifiDma::handles(u32 address)
{
    if(address==RX_CONTROL || address==RX_HEAD)return true;
    if(address>0x3ff20dc4 || address<0x3ff20cd4 || (0x3ff20dc4-address)%24)return false;
    const unsigned queue=queue_index(address);return queue<8 || queue==10;
}

bool Esp8266WifiDma::capture(const Registers &regs,unsigned queue,u32 control,
                           Pending &pending,std::string *error)
{
    const u32 base=0x3ff20dc0-24*queue,phy=reg(regs,base+8);
    const unsigned length=phy&4095,policy=(control>>24)&7;
    if(length<10)return fail(error,"Wi-Fi TX frame is shorter than its MAC header");
    if(phy&(1u<<24))return fail(error,"unsupported HT Wi-Fi TX modulation");
    if(policy>1 || (control&0x18000000u))return fail(error,"unsupported protected or block-ACK Wi-Fi TX exchange");
    pending=Pending();pending.frame.queue=queue;pending.frame.rate=(phy>>12)&15;
    if(pending.frame.rate==4)return fail(error,"unsupported CCK Wi-Fi TX rate code4");
    pending.frame.descriptor=0x3ffc0000u|(control&0x3ffffu);
    u32 descriptor=pending.frame.descriptor;
    std::set<u32> visited;
    while(pending.frame.bytes.size()<length) {
        if((descriptor&3) || !ram_address(descriptor,12))return fail(error,"Wi-Fi TX descriptor is outside data RAM or unaligned");
        if(!visited.insert(descriptor).second || visited.size()>256)return fail(error,"Wi-Fi TX descriptor chain cycles or exceeds256 nodes");
        u32 flags,buffer,next;
        if(!bus_.read(descriptor,4,flags,false) || !bus_.read(descriptor+4,4,buffer,false) ||
           !bus_.read(descriptor+8,4,next,false))return fail(error,"Wi-Fi TX descriptor memory fault");
        /* ESP8266 TRM3.3.2: MAC does not use owner31. The software/hardware
         * owner contract of other DMA peripherals must not be imposed here. */
        /* TX uses length, not the RX buffer-capacity field. The SDK's
         * ppProcTxSecFrame updates only length/EOF, and real SDK TX chains
         * intentionally retain size0. Bound the actual read against RAM
         * and the programmed PHY frame length instead. */
        const unsigned size=flags&4095,count=(flags>>12)&4095;
        if(count>length-pending.frame.bytes.size()) {
            return fail(error,strf(
                "Wi-Fi TX descriptor length exceeds its frame (queue%u descriptor%08x flags%08x size%u length%u captured%u PHY%08x control%08x)",
                queue,descriptor,flags,size,count,(unsigned)pending.frame.bytes.size(),phy,control).c_str());
        }
        /* ROM lldesc_build_chain and SDK allocation preserve bits28:24.
         * They are software metadata, not an offset applied by the MAC. */
        if(!ram_address(buffer,count))return fail(error,"Wi-Fi TX buffer is outside data RAM");
        for(unsigned i=0;i<count;) {
            const unsigned amount=((buffer+i)&3) || count-i<4?1:4;u32 value;
            if(!bus_.read(buffer+i,amount,value,false))return fail(error,"Wi-Fi TX buffer memory fault");
            for(unsigned j=0;j<amount;++j)pending.frame.bytes+=char(value>>(j*8));
            i+=amount;
        }
        if(pending.frame.bytes.size()==length) {
            if(!(flags&0x40000000u))return fail(error,"Wi-Fi TX frame has no EOF descriptor");
            break;
        }
        if((flags&0x40000000u) || !next)return fail(error,"Wi-Fi TX descriptor chain ends before the frame");
        descriptor=next;
    }
    if((u8(pending.frame.bytes[0])&3)!=0)return fail(error,"unsupported Wi-Fi MAC protocol version");
    if(u8(pending.frame.bytes[1])&0x40)return fail(error,"encrypted Wi-Fi TX exchange is not implemented");
    const unsigned type=(u8(pending.frame.bytes[0])>>2)&3,subtype=u8(pending.frame.bytes[0])>>4;
    const bool no_response=type==1 && (subtype==12 || subtype==13); // CTS and ACK
    pending.frame.ack_requested=policy==1 && !(u8(pending.frame.bytes[4])&1) && !no_response;
    const unsigned slot=pending.frame.rate<8?20:9;
    pending.backoff_ticks=u64((reg(regs,base)>>12)&1023)*slot*80;
    pending.airtime_ticks=u64(airtime_us(pending.frame.rate,length))*80;
    pending.ack_ticks=u64(10+airtime_us(pending.frame.rate<8?0:11,10))*80;
    return true;
}

Esp8266WifiDma::Result Esp8266WifiDma::write(Registers &regs,u32 address,u32 value,u64 now,std::string *error)
{
    if(!handles(address))return UNHANDLED;
    if(error)error->clear();
    advance(regs,now);
    if(address==RX_CONTROL) {
        regs[address]=value;
        if(!(value&0x80000000u) && rx_pending_) {
            rx_pending_=false;rx_writes_.clear();
            regs.erase(RX_PENDING_RATE);
            regs[RX_STATE]=reg(regs,RX_STATE)&~0xf000u;
        }
        return ACCEPTED;
    }
    if(address==RX_HEAD) {
        /* A completed frame still references its captured EOF snapshot.
         * Replenishing the free ring affects the next reception only. */
        if(rx_pending_) {fail(error,"cannot replace Wi-Fi RX head during a reception");return REJECTED;}
        regs[address]=value;rx_cursor_=value;return ACCEPTED;
    }
    const unsigned queue=queue_index(address);
    if((value&0xc0000000u)!=0xc0000000u) {
        for(std::deque<Pending>::iterator it=queue_.begin();it!=queue_.end();) {
            if(it->frame.queue==queue)it=queue_.erase(it);else ++it;
        }
        /* Firmware disables a queue after its start interrupt. That stops
         * future submissions, not a frame already on the air. */
        if(stage_==BACKOFF && current_.frame.queue==queue){stage_=IDLE;begin(regs,now);}
        regs[address]=value;return ACCEPTED;
    }
    if(stage_!=IDLE && current_.frame.queue==queue) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Wi-Fi TX queue is already active (stage=%d, pending=0x%x, now=%llu, deadline=%llu)",
                 (int)stage_, (unsigned)pending_events_, (unsigned long long)now, (unsigned long long)deadline_);
        fail(error, buf);
        return REJECTED;
    }
    for(size_t i=0;i<queue_.size();++i)if(queue_[i].frame.queue==queue) {
        fail(error,"Wi-Fi TX queue already has a pending frame");return REJECTED;
    }
    Pending pending;
    if(!capture(regs,queue,value,pending,error))return REJECTED;
    regs[address]=value;queue_.push_back(pending);begin(regs,now);advance(regs,now);return ACCEPTED;
}

void Esp8266WifiDma::begin(Registers &regs,u64 now)
{
    if(stage_!=IDLE || queue_.empty())return;
    current_=queue_.front();queue_.pop_front();stage_=BACKOFF;
    deadline_=now+current_.backoff_ticks;
    regs[TX_STATE]=reg(regs,TX_STATE)&~15u;
}

void Esp8266WifiDma::finish(Registers &regs,u64 now)
{
    current_.frame.completed=now;
    regs[TX_STATUS]=(current_.frame.queue<<12)|
        (current_.frame.outcome==ACK_TIMEOUT?5u<<28:0);
    regs[TX_STATE]=reg(regs,TX_STATE)&~15u;
    esp8266_wdev::raise(regs,TX_COMPLETE);pending_events_|=TX_COMPLETE;
    ++transmissions_;
    if(completed_.size()==256)completed_.pop_front();
    completed_.push_back(current_.frame);stage_=REPORTED;
    const u32 required=reg(regs,MASK)&TX_EVENTS;
    if(!required) {pending_events_=0;stage_=IDLE;begin(regs,now);advance(regs,now);}
}

void Esp8266WifiDma::advance(Registers &regs,u64 now)
{
    while(stage_!=IDLE && stage_!=REPORTED && now>=deadline_) {
        const u64 when=deadline_;
        if(stage_==BACKOFF) {
            current_.frame.started=when;regs[TX_STATUS]=current_.frame.queue<<12;
            regs[TX_STATE]=(reg(regs,TX_STATE)&~15u)|1;
            esp8266_wdev::raise(regs,TX_START);pending_events_|=TX_START;
            deadline_=when+current_.airtime_ticks;stage_=ON_AIR;
        } else if(stage_==ON_AIR) {
            current_.frame.air_finished=when;
            const bool acknowledged=air_ && air_->transmit(current_.frame);
            current_.frame.outcome=current_.frame.ack_requested && !acknowledged?ACK_TIMEOUT:SUCCESS;
            if(!current_.frame.ack_requested)finish(regs,when);
            else {
                const u64 timeout=ack_timeout_us_?u64(ack_timeout_us_)*80:
                    current_.ack_ticks+(current_.frame.rate<8?20:9)*80;
                deadline_=when+(acknowledged?current_.ack_ticks:timeout);stage_=WAIT_ACK;
            }
        } else finish(regs,when);
    }
    if(rx_pending_ && now>=rx_deadline_)finish_receive(regs);
}

void Esp8266WifiDma::acknowledge(Registers &regs,u32 events,u64 now)
{
    pending_events_&=~(events&TX_EVENTS);
    if(events&RX_COMPLETE)rx_reported_=false;
    const u32 required=reg(regs,MASK)&TX_EVENTS;
    if(stage_==REPORTED && !(pending_events_&required)) {
        pending_events_&=required;
        stage_=IDLE;begin(regs,now);advance(regs,now);
    }
}

bool Esp8266WifiDma::next_event_ticks(u64 now,u64 &ticks)const
{
    const bool tx=stage_!=IDLE && stage_!=REPORTED;
    if(!tx && !rx_pending_)return false;
    const u64 deadline=!tx?rx_deadline_:!rx_pending_?deadline_:std::min(deadline_,rx_deadline_);
    ticks=deadline>now?deadline-now:0;return true;
}
bool Esp8266WifiDma::inflight()const{return stage_!=IDLE && stage_!=REPORTED;}

Esp8266WifiDma::ReceiveResult Esp8266WifiDma::receive(Registers &regs,const Bytes &frame,
                                                     const RxInfo &info,u64 now,std::string *error)
{
    if(error)error->clear();
    /* The caller may inject from the TX air callback. Do not recursively
     * advance TX here: that would transmit the same frame twice. */
    if(rx_pending_ && now>=rx_deadline_)finish_receive(regs);
    if(!rx_error_.empty()){if(error)*error=rx_error_;return RX_INVALID;}
    if(!(reg(regs,RX_CONTROL)&0x80000000u))return RX_DISABLED;
    if(rx_pending_ || rx_reported_)return RX_BUSY;
    if(info.rate>15 || info.rate==4 || info.channel<1 || info.channel>14 ||
       info.rssi_dbm<-128 || info.rssi_dbm>31 ||
       info.noise_floor_dbm<-128 || info.noise_floor_dbm>0) {
        fail(error,"unsupported legacy Wi-Fi RX radio metadata");return RX_INVALID;
    }
    if(frame.size()<10 || frame.size()>4091) {
        fail(error,"Wi-Fi RX frame length is outside the legacy MAC bounds");return RX_INVALID;
    }
    const u64 duration=u64(airtime_us(info.rate,(unsigned)frame.size()))*80;
    if(now>~u64(0)-duration){fail(error,"Wi-Fi RX deadline overflows the radio clock");return RX_INVALID;}
    const ReceiveResult result=prepare_receive(regs,frame,info,error);
    if(result!=RX_ACCEPTED)return result;
    rx_deadline_=now+duration;rx_timestamp_=now/80;rx_pending_=true;
    regs[RX_PENDING_RATE]=info.rate;
    regs[RX_STATE]=(reg(regs,RX_STATE)&~0xf000u)|0x1000u;
    return RX_ACCEPTED;
}

Esp8266WifiDma::ReceiveResult Esp8266WifiDma::prepare_receive(const Registers &regs,
                                  const Bytes &frame,const RxInfo &info,std::string *error)
{
    const unsigned fc=u8(frame[0]),direction=u8(frame[1])&3,type=(fc>>2)&3,subtype=fc>>4;
    if((fc&3) || type==3 || (u8(frame[1])&0x40) || direction==3 ||
       (type==1 && subtype<10)) {
        fail(error,"unsupported encrypted, aggregate or nonlegacy Wi-Fi RX frame");return RX_INVALID;
    }
    const unsigned header=type==1?((subtype==12 || subtype==13)?10:16):
        (type==2 && (subtype&8)?26:24);
    if(frame.size()<header){fail(error,"Wi-Fi RX frame is shorter than its MAC header");return RX_INVALID;}
    if(type==2 && (subtype&8) && ((u8(frame[24])&0x80) || (u8(frame[1])&0x80))) {
        fail(error,"Wi-Fi RX A-MSDU and HT-control formats are unsupported");return RX_INVALID;
    }
    const unsigned bssid=direction==1?4:direction==2?10:16;
    u32 matches=0;
    for(unsigned iface=0;iface<2;++iface) {
        if(comparator(regs,0x3ff20c48+iface*8,0x3ff20c58+iface*8,frame,4))matches|=1u<<(28+iface);
        if(type!=1 && comparator(regs,0x3ff20c28+iface*8,0x3ff20c38+iface*8,frame,bssid))matches|=1u<<(30+iface);
    }
    /* This bounded model supports explicit comparator matches. It does not
     * invent disabled-comparator wildcard or promiscuous-policy behavior. */
    if(!matches)return RX_FILTERED;
    Bytes payload;
    append_word(payload,u32(u8(info.rssi_dbm+96))|(info.rate<<8)|
        ((u8(frame[4])&1)?1u<<12:0)|(u32(frame.size()+4)<<16)|matches);
    append_word(payload,0); // legacy PHY, no HT or aggregation metadata
    append_word(payload,(info.channel<<16)|(u32(u8(info.noise_floor_dbm))<<24));
    payload+=frame;append_word(payload,crc32(frame));

    std::vector<RxPiece> pieces;
    std::set<u32> visited;
    u32 descriptor=rx_cursor_?rx_cursor_:reg(regs,RX_HEAD);
    unsigned remaining=(unsigned)payload.size();
    while(remaining) {
        if(!descriptor)return RX_NO_BUFFER;
        if((descriptor&3) || !ram_address(descriptor,12)) {
            fail(error,"Wi-Fi RX descriptor is outside data RAM or unaligned");return RX_INVALID;
        }
        if(!visited.insert(descriptor).second || visited.size()>256) {
            fail(error,"Wi-Fi RX descriptor chain cycles or exceeds256 nodes");return RX_INVALID;
        }
        RxPiece piece;piece.descriptor=descriptor;
        if(!bus_.read(descriptor,4,piece.flags,false) || !bus_.read(descriptor+4,4,piece.buffer,false) ||
           !bus_.read(descriptor+8,4,piece.next,false)) {
            fail(error,"Wi-Fi RX descriptor memory fault");return RX_INVALID;
        }
        if(piece.flags&0x40000000u)return RX_NO_BUFFER; // completed, not recycled
        const unsigned capacity=piece.flags&4095;
        if(!capacity) {
            fail(error,"Wi-Fi RX descriptor has zero capacity");return RX_INVALID;
        }
        if(!ram_address(piece.buffer,capacity)) {
            fail(error,"Wi-Fi RX buffer is outside data RAM");return RX_INVALID;
        }
        piece.count=std::min(capacity,remaining);remaining-=piece.count;
        pieces.push_back(piece);descriptor=piece.next;
    }
    /* SDK ISR requires a successor after EOF, even for a single-node frame.
     * It owns the subsequent detach/replenish operations on the linked list. */
    if(!descriptor)return RX_NO_BUFFER;
    if((descriptor&3) || !ram_address(descriptor,12) || visited.count(descriptor)) {
        fail(error,"Wi-Fi RX EOF successor is invalid or cycles");return RX_INVALID;
    }
    for(size_t i=0;i<pieces.size();++i) {
        if(overlaps(pieces[i].buffer,pieces[i].count,descriptor,12)) {
            fail(error,"Wi-Fi RX buffer overlaps its sentinel descriptor");return RX_INVALID;
        }
        for(size_t j=0;j<pieces.size();++j) {
            if(overlaps(pieces[i].buffer,pieces[i].count,pieces[j].descriptor,12) ||
               (i!=j && overlaps(pieces[i].buffer,pieces[i].count,pieces[j].buffer,pieces[j].count))) {
                fail(error,"Wi-Fi RX buffers overlap descriptors or one another");return RX_INVALID;
            }
        }
    }
    std::vector<RxWrite> writes;unsigned offset=0;
    for(size_t i=0;i<pieces.size();++i) {
        const RxPiece &piece=pieces[i];
        for(unsigned position=0;position<piece.count;) {
            const u32 address=piece.buffer+position;
            const unsigned amount=(address&3) || piece.count-position<4?1:4;
            u32 before,after=0;
            if(!bus_.read(address,amount,before,false)) {
                fail(error,"Wi-Fi RX buffer memory fault during validation");return RX_INVALID;
            }
            for(unsigned byte=0;byte<amount;++byte)after|=u32(u8(payload[offset+position+byte]))<<(byte*8);
            writes.push_back(RxWrite(address,before,after,amount));position+=amount;
        }
        offset+=piece.count;
    }
    for(size_t i=0;i<pieces.size();++i) {
        const RxPiece &piece=pieces[i];
        /* Keep owner/sub_sof/software metadata/size unchanged. EOF is the final publication
         * write, after every byte and every earlier node length is ready. */
        u32 flags=(piece.flags&~0x40fff000u)|(piece.count<<12);
        if(i+1==pieces.size())flags|=0x40000000u;
        writes.push_back(RxWrite(piece.descriptor,piece.flags,flags,4));
    }
    rx_writes_.swap(writes);rx_eof_=pieces.back().descriptor;rx_next_=descriptor;
    return RX_ACCEPTED;
}

void Esp8266WifiDma::finish_receive(Registers &regs)
{
    rx_pending_=false;
    regs[RX_STATE]=reg(regs,RX_STATE)&~0xf000u;
    for(size_t i=0;i<rx_writes_.size();++i) {
        const RxWrite &write=rx_writes_[i];
        if(!bus_.write(write.address,write.size,write.after)) {
            bool restored=true;
            while(i) {
                const RxWrite &previous=rx_writes_[--i];
                if(!bus_.write(previous.address,previous.size,previous.before))restored=false;
            }
            rx_error_=restored?"Wi-Fi RX DMA memory write fault; transaction rolled back":
                "Wi-Fi RX DMA memory write fault; RAM rollback also failed";
            rx_writes_.clear();regs.erase(RX_PENDING_RATE);return;
        }
    }
    rx_writes_.clear();rx_cursor_=rx_next_;
    regs[RX_EOF]=rx_eof_;regs[RX_TIME]=(u32)rx_timestamp_;
    regs[RX_RATE]=(reg(regs,RX_PENDING_RATE)&15)<<16;regs.erase(RX_PENDING_RATE);
    ++receptions_;regs[0x3ff201c0]=reg(regs,0x3ff201c0)+1;rx_reported_=true;esp8266_wdev::raise(regs,RX_COMPLETE);
}
