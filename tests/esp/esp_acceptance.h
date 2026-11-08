#ifndef BK_TEST_ESP_ACCEPTANCE_H
#define BK_TEST_ESP_ACCEPTANCE_H

#include "common.h"

/* Observe guest UART output incrementally. A query-based application must
 * answer a fresh query after its first recognizable banner/reply; startup
 * text alone therefore cannot satisfy its runtime acceptance criterion. */
class EspAcceptance {
    std::string boot_marker_,app_marker_;
    bool boot_required_,query_required_,banner_seen_,query_sent_;
    bool boot_seen_,app_seen_;
    unsigned resets_;
    size_t scanned_,epoch_start_,query_start_;
    u64 app_instructions_,app_us_;

    bool find_new(const std::string &output,const std::string &marker,size_t floor) const {
        size_t start=scanned_>=marker.size()?scanned_-marker.size()+1:0;
        if(start<floor)start=floor;
        return output.find(marker,start)!=std::string::npos;
    }
public:
    EspAcceptance(const char *boot,const char *app,bool bootloader,bool query)
        : boot_marker_(boot),app_marker_(app),boot_required_(bootloader),query_required_(query),
          banner_seen_(false),query_sent_(false),boot_seen_(!bootloader),app_seen_(false),
          resets_(0),scanned_(0),epoch_start_(0),query_start_(0),app_instructions_(0),app_us_(0) {}

    void observe(const std::string &output,u64 instructions,u64 us,unsigned resets) {
        if(resets!=resets_ || output.size()<scanned_) {
            resets_=resets;scanned_=epoch_start_=output.size();query_start_=0;
            boot_seen_=!boot_required_;app_seen_=banner_seen_=query_sent_=false;
            app_instructions_=app_us_=0;
        }
        if(output.size()==scanned_)return;
        if(!boot_seen_ && find_new(output,boot_marker_,epoch_start_))boot_seen_=true;
        if(!app_seen_ && find_new(output,app_marker_,epoch_start_)) {
            banner_seen_=true;
            if(!query_required_ || (query_sent_ && find_new(output,app_marker_,query_start_))) {
                app_seen_=true;app_instructions_=instructions;app_us_=us;
            }
        }
        scanned_=output.size();
    }
    void query_sent(size_t output_size) {
        if(banner_seen_ && !query_sent_) {query_sent_=true;query_start_=output_size;}
    }
    bool boot_seen() const {return boot_seen_;}
    bool app_seen() const {return app_seen_;}
    bool complete(u64 instructions,u64 us) const {
        return boot_seen_ && app_seen_ && instructions>=app_instructions_ && us>=app_us_ &&
            instructions-app_instructions_>=100000 && us-app_us_>=1000000;
    }
};

/* Drive a physical short press only after the guest configures both pins.
 * Startup relay changes restart settling; only a response to the press counts. */
class EspButtonAcceptance {
    enum State { WAITING, SETTLING, PRESSED, RELEASED, COMPLETE, FAILED } state_;
    bool baseline_,changed_;
    u64 since_,last_us_;
    unsigned resets_;
public:
    enum Action { NONE, PRESS, RELEASE };
    EspButtonAcceptance():state_(WAITING),baseline_(false),changed_(false),
        since_(0),last_us_(0),resets_(0) {}
    Action observe(bool application,bool input_ready,bool relay_enabled,
                   bool relay_output,u64 now_us,unsigned resets) {
        if(resets!=resets_ || now_us<last_us_) {
            bool release=state_==PRESSED;
            state_=WAITING;changed_=false;since_=now_us;
            resets_=resets;last_us_=now_us;
            if(release)return RELEASE;
        }
        last_us_=now_us;
        if(state_==COMPLETE || state_==FAILED)return NONE;
        if(state_==WAITING || state_==SETTLING) {
            if(!application || !input_ready || !relay_enabled) {
                state_=WAITING;return NONE;
            }
            if(state_==WAITING || baseline_!=relay_output) {
                state_=SETTLING;baseline_=relay_output;since_=now_us;
                return NONE;
            }
            if(now_us-since_>=200000) {
                state_=PRESSED;since_=now_us;changed_=false;return PRESS;
            }
            return NONE;
        }
        if(relay_enabled && relay_output!=baseline_)changed_=true;
        if(state_==PRESSED) {
            if(now_us-since_>=200000) {
                state_=RELEASED;since_=now_us;return RELEASE;
            }
        } else if(now_us-since_>=1000000) {
            state_=input_ready && relay_enabled && changed_?COMPLETE:FAILED;
        }
        return NONE;
    }
    bool complete() const {return state_==COMPLETE;}
    bool failed() const {return state_==FAILED;}
};
#endif
