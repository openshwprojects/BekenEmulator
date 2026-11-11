#ifndef BK_ESP8266_WIFI_DMA_H
#define BK_ESP8266_WIFI_DMA_H
#include "arch/xtensa/xtensa_cpu.h"

/* Functional ESP8266 legacy Wi-Fi DMA. The guest supplies real 802.11 frame
 * bytes; an optional radio peer supplies actual ACK outcomes. No peer means
 * no received ACK. The class has no dependency on Beken's network device. */
class Esp8266WifiDma {
public:
    typedef std::map<u32,u32> Registers;
    enum Result { UNHANDLED, ACCEPTED, REJECTED };
    enum Outcome { PENDING, SUCCESS, ACK_TIMEOUT };
    enum ReceiveResult { RX_ACCEPTED, RX_DISABLED, RX_FILTERED, RX_BUSY,
                         RX_NO_BUFFER, RX_INVALID };
    struct RxInfo {
        RxInfo():rate(0),channel(1),rssi_dbm(-42),noise_floor_dbm(-96){}
        unsigned rate,channel;
        int rssi_dbm,noise_floor_dbm;
    };
    struct Frame {
        Frame():queue(0),rate(0),descriptor(0),ack_requested(false),
            started(0),air_finished(0),completed(0),outcome(PENDING){}
        unsigned queue,rate;
        u32 descriptor;
        bool ack_requested;
        u64 started,air_finished,completed; // 80MHz APB ticks
        Outcome outcome;
        Bytes bytes; // MAC frame without the hardware-generated four-byte FCS
    };
    class Air {
    public:
        virtual ~Air(){}
        virtual bool transmit(const Frame &frame)=0; // true only if a peer ACKs
    };
    explicit Esp8266WifiDma(XtensaBus &bus);
    void reset();
    void set_air(Air *air){air_=air;}
    Air *air() const{return air_;}
    void set_ack_timeout_us(unsigned value){ack_timeout_us_=value;}
    static bool handles(u32 address);
    bool read(const Registers &registers,u32 address,u32 &value) const;
    Result write(Registers &registers,u32 address,u32 value,u64 apb_ticks,std::string *error=0);
    /* Input contains a complete, unencrypted legacy MAC frame without FCS.
     * The caller supplies radio channel filtering; channel is RX metadata.
     * Only enabled guest MAC/BSSID comparators are supported. Scan wildcard
     * and promiscuous policies, aggregation and PHY errors remain unsupported.
     * Auxiliary RX snapshots18/1c are unmodeled and read as neutral values;
     * observed legacy handlers discard them before consuming the real EOF.
     * At most one receive is pending until its completion IRQ is acknowledged. */
    ReceiveResult receive(Registers &,const Bytes &,const RxInfo &,u64 apb_ticks,std::string *error=0);
    const std::string &receive_error() const{return rx_error_;}
    u64 receptions() const{return receptions_;}
    void advance(Registers &registers,u64 apb_ticks);
    void acknowledge(Registers &registers,u32 events,u64 apb_ticks);
    bool next_event_ticks(u64 apb_ticks,u64 &ticks) const;
    bool inflight() const;
    size_t queued() const{return queue_.size();}
    const std::deque<Frame> &completed_frames() const{return completed_;}
    void clear_completed(){completed_.clear();}
    u64 transmissions() const{return transmissions_;}
private:
    struct Pending {
        Frame frame;
        u64 backoff_ticks,airtime_ticks,ack_ticks;
    };
    struct RxWrite {
        u32 address,before,after;
        unsigned size;
        RxWrite(u32 a,u32 b,u32 v,unsigned s):address(a),before(b),after(v),size(s){}
    };
    enum Stage { IDLE, BACKOFF, ON_AIR, WAIT_ACK, REPORTED };
    XtensaBus &bus_;
    Air *air_;
    unsigned ack_timeout_us_;
    Stage stage_;
    Pending current_;
    std::deque<Pending> queue_;
    std::deque<Frame> completed_;
    u64 deadline_,transmissions_;
    u32 pending_events_;
    bool rx_pending_,rx_reported_;
    u32 rx_cursor_,rx_eof_,rx_next_;
    u64 rx_deadline_,rx_timestamp_,receptions_;
    std::vector<RxWrite> rx_writes_;
    std::string rx_error_;
    bool capture(const Registers &,unsigned queue,u32 control,Pending &,std::string *);
    void begin(Registers &,u64 now);
    void finish(Registers &,u64 now);
    ReceiveResult prepare_receive(const Registers &,const Bytes &,const RxInfo &,std::string *);
    void finish_receive(Registers &);
};
#endif
