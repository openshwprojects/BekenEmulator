#include "esp_machine.h"
#include <algorithm>

bool EspMachine::attach_clocked_device(ClockedDevice *device)
{
    if(!device || std::find(clocked_devices_.begin(),clocked_devices_.end(),device)!=clocked_devices_.end())return false;
    clocked_devices_.push_back(device);device->reset(elapsed_us());return true;
}

bool EspMachine::detach_clocked_device(ClockedDevice *device)
{
    std::vector<ClockedDevice *>::iterator found=std::find(clocked_devices_.begin(),clocked_devices_.end(),device);
    if(found==clocked_devices_.end())return false;
    clocked_devices_.erase(found);return true;
}

void EspMachine::reset_clocked_devices()
{
    for(size_t i=0;i<clocked_devices_.size();++i)clocked_devices_[i]->reset(elapsed_us());
}

void EspMachine::advance_clocked_devices()
{
    const u64 now=elapsed_us();
    for(size_t i=0;i<clocked_devices_.size();++i) {
        u64 deadline;
        if(clocked_devices_[i]->next_event_us(deadline) && deadline<=now)
            clocked_devices_[i]->advance(now);
    }
}

bool EspMachine::cycles_to_clocked_device(u64 &cycles) const
{
    bool found=false;u64 earliest=0;
    for(size_t i=0;i<clocked_devices_.size();++i) {
        u64 deadline;
        if(clocked_devices_[i]->next_event_us(deadline) && (!found || deadline<earliest)) {
            earliest=deadline;found=true;
        }
    }
    if(!found)return false;
    const u64 now=elapsed_us();
    if(earliest<=now) {cycles=0;return true;}
    const u64 delta=earliest-now;
    /* Limit each jump to one CPU counter turn. This also avoids multiplying
     * an arbitrary host deadline beyond the clock's integer range. */
    if(delta>0xffffffffull/cpu_mhz_)cycles=0xffffffffull;
    else {
        const u64 phase=(cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_)%cpu_mhz_;
        cycles=delta*cpu_mhz_-phase;
    }
    return true;
}
