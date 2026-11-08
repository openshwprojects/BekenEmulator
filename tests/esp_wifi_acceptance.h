#ifndef BK_TEST_ESP_WIFI_ACCEPTANCE_H
#define BK_TEST_ESP_WIFI_ACCEPTANCE_H
#include "esp_machine.h"

/* A real DMA completion must be observed before the guest can recycle it.
 * This monitor uses public device state, never firmware-private RAM globals. */
class EspWifiRxMonitor {
    enum State { IDLE, WAIT_DMA, WAIT_RECYCLE, CONTINUING, COMPLETE, FAILED } state_;
    u64 baseline_,last_instructions_,last_us_,continued_instructions_,continued_us_;
    unsigned resets_;
    u32 descriptor_,capacity_;
    bool received_,recycled_;
    std::string error_;
public:
    struct Observation {
        Observation():received(0),instructions(0),microseconds(0),resets(0),
            eof(0),flags(0),raw_events(0) {}
        u64 received,instructions,microseconds;
        unsigned resets;
        u32 eof,flags,raw_events;
    };
    EspWifiRxMonitor():state_(IDLE),baseline_(0),last_instructions_(0),last_us_(0),
        continued_instructions_(0),continued_us_(0),resets_(0),descriptor_(0),capacity_(0),
        received_(false),recycled_(false) {}
    void reject(const std::string &error) {state_=FAILED;error_=error;}
    void arm(const Observation &sample) {
        if(state_!=IDLE){reject("Wi-Fi RX acceptance was armed more than once");return;}
        if(sample.received==~u64(0)){reject("Wi-Fi RX count cannot advance");return;}
        state_=WAIT_DMA;baseline_=sample.received;resets_=sample.resets;
        last_instructions_=sample.instructions;last_us_=sample.microseconds;
    }
    void observe(const Observation &sample) {
        if(state_==IDLE || state_==FAILED)return;
        if(sample.resets!=resets_ || sample.instructions<last_instructions_ || sample.microseconds<last_us_) {
            reject("reset or clock rollback interrupted Wi-Fi RX acceptance");return;
        }
        last_instructions_=sample.instructions;last_us_=sample.microseconds;
        if(sample.received<baseline_ || sample.received>baseline_+1) {
            reject("Wi-Fi RX count differs from the single injected frame");return;
        }
        if(state_==WAIT_DMA) {
            if(sample.received==baseline_)return;
            capacity_=sample.flags&4095;
            if(sample.eof<0x3ffe0000 || sample.eof>0x3ffffff4 || (sample.eof&3) ||
               !(sample.flags&0x40000000u) || !capacity_ ||
               !((sample.flags>>12)&4095) || ((sample.flags>>12)&4095)>capacity_) {
                reject("new Wi-Fi RX completion did not publish a valid EOF descriptor");return;
            }
            descriptor_=sample.eof;received_=true;state_=WAIT_RECYCLE;return;
        }
        if(sample.received!=baseline_+1) {
            reject("completed Wi-Fi RX count was lost");return;
        }
        if(state_==WAIT_RECYCLE) {
            if(sample.eof!=descriptor_ || (sample.flags&4095)!=capacity_) {
                reject("Wi-Fi RX completion descriptor was replaced before recycling");return;
            }
            if((sample.flags&0x40000000u) || ((sample.flags>>12)&4095)!=capacity_ ||
               (sample.raw_events&(1u<<8)))return;
            recycled_=true;state_=CONTINUING;
            continued_instructions_=sample.instructions;continued_us_=sample.microseconds;
        }
        if(state_==CONTINUING && sample.instructions-continued_instructions_>=100000 &&
           sample.microseconds-continued_us_>=10000)state_=COMPLETE;
    }
    bool armed() const {return state_!=IDLE;}
    bool received() const {return received_;}
    bool recycled() const {return recycled_;}
    bool complete() const {return state_==COMPLETE;}
    bool failed() const {return state_==FAILED;}
    bool needs_descriptor() const {return state_==WAIT_DMA || state_==WAIT_RECYCLE;}
    const std::string &error() const {return error_;}
};

/* A deterministic isolated radio fixture. The guest's first probe request
 * selects its station address; no dump credentials are inspected or printed.
 * Delivery waits for actual TX completion, an idle MAC, and a 10us turnaround.
 * Channel1 is the explicit test radio channel, not inferred hardware tuning. */
class EspWifiRxAcceptance:public Esp8266WifiDma::Air {
    EspMachine &machine_;
    EspWifiRxMonitor monitor_;
    Bytes response_;
    bool selected_,injected_;
    u64 tx_baseline_,after_us_;
    unsigned resets_;

    EspWifiRxMonitor::Observation snapshot() const {
        EspWifiRxMonitor::Observation sample;
        sample.received=machine_.wifi_receptions();
        sample.instructions=machine_.cpu().instructions();
        sample.microseconds=machine_.elapsed_us();sample.resets=machine_.reset_count();
        return sample;
    }
public:
    explicit EspWifiRxAcceptance(EspMachine &machine):machine_(machine),selected_(false),injected_(false),
        tx_baseline_(0),after_us_(0),resets_(machine.reset_count()) {
        if(machine.chip()!=esp::CHIP_ESP8266)monitor_.reject("Wi-Fi RX acceptance requires ESP8266");
    }
    static bool response_for(const Bytes &request,Bytes &response) {
        if(request.size()<24 || u8(request[0])!=0x40 || (u8(request[1])&0x43))return false;
        response.assign(36,'\0');response[0]=0x50;
        response.replace(4,6,request.substr(10,6));
        response.replace(10,6,"\x02\x77\x88\x99\xaa\xbb",6);
        response.replace(16,6,response.substr(10,6));
        response[32]=100;response[34]=1; // 100TU beacon interval, ESS capability
        response.append("\0\x08sim-test",10);
        response.append("\x01\x04\x82\x84\x8b\x96\x03\x01\x01",9);
        return true;
    }
    bool transmit(const Esp8266WifiDma::Frame &frame) {
        if(!selected_ && !monitor_.failed() && response_for(frame.bytes,response_)) {
            selected_=true;tx_baseline_=machine_.wifi_transmissions();
            after_us_=std::max(machine_.elapsed_us(),frame.air_finished/80)+10;
            resets_=machine_.reset_count();
        }
        return false; // this radio fixture never claims to have sent a MAC ACK
    }
    void observe() {
        if(monitor_.failed())return;
        EspWifiRxMonitor::Observation sample=snapshot();
        if(selected_ && sample.resets!=resets_) {
            monitor_.reject("reset interrupted the radio exchange");return;
        }
        if(selected_ && !injected_ && machine_.wifi_transmissions()>tx_baseline_ &&
           sample.microseconds>=after_us_) {
            u32 tx_state=0;
            if(!machine_.read(0x3ff20410,4,tx_state,false)) {
                monitor_.reject("cannot read TX state before RX injection");return;
            }
            if(tx_state&15)return;
            monitor_.arm(sample);injected_=true;
            Esp8266WifiDma::RxInfo info;info.rate=0;info.channel=1;
            std::string error;
            Esp8266WifiDma::ReceiveResult result=machine_.receive_wifi(response_,info,&error);
            if(result!=Esp8266WifiDma::RX_ACCEPTED) {
                if(error.empty())error="synthetic probe response was not accepted by RX DMA";
                monitor_.reject(error);return;
            }
        }
        if(!injected_)return;
        if(monitor_.needs_descriptor() && monitor_.received()) {
            if(!machine_.read(0x3ff20014,4,sample.eof,false) ||
               !machine_.read(sample.eof,4,sample.flags,false) ||
               !machine_.read(0x3ff20c1c,4,sample.raw_events,false)) {
                monitor_.reject("cannot observe completed RX descriptor");return;
            }
        } else if(monitor_.needs_descriptor() && sample.received>0) {
            if(!machine_.read(0x3ff20014,4,sample.eof,false) ||
               (sample.eof && !machine_.read(sample.eof,4,sample.flags,false))) {
                monitor_.reject("cannot observe RX EOF publication");return;
            }
        }
        monitor_.observe(sample);
    }
    bool injected() const {return injected_;}
    bool received() const {return monitor_.received();}
    bool recycled() const {return monitor_.recycled();}
    bool complete() const {return monitor_.complete();}
    bool failed() const {return monitor_.failed();}
    const std::string &error() const {return monitor_.error();}
};
#endif
