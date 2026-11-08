#include "esp/esp_machine.h"
#include "esp8266/esp8266_interrupts.h"
#include "esp8266/esp8266_watchdog.h"
#include "esp8266/esp8266_mac_timers.h"
#include "esp8266/esp8266_wdev.h"

bool EspMachine::esp8266_timer_read(u32 address,u32 &value)
{
    if(esp8266_watchdog::handles(address)) {
        if(!update_esp8266_timers())return false;
        return esp8266_watchdog::read(registers_,address,esp32_apb_ticks(),value);
    }
    return frc_timers_.read(address,esp32_apb_ticks(),value);
}

bool EspMachine::esp8266_timer_write(u32 address,u32 value)
{
    if(esp8266_watchdog::handles(address)) {
        if(!update_esp8266_timers())return false;
        return esp8266_watchdog::write(registers_,address,value,esp32_apb_ticks());
    }
    if(address==0x3ff00000 || address==0x3ff00004) {
        if(address==0x3ff00000 && (register_value(address)&31)==1 && (value&31)!=1)cpu_.set_irq(14,false);
        registers_[address]=value;
        frc_timers_.set_routing(register_value(0x3ff00004),register_value(0x3ff00000),esp32_apb_ticks());
        esp8266_interrupts::update_mac(registers_,cpu_);
        return true;
    }
    return frc_timers_.write(address,esp32_apb_ticks(),value);
}

bool EspMachine::update_esp8266_timers()
{
    if(chip_!=esp::CHIP_ESP8266)return true;
    const u64 now=esp32_apb_ticks();frc_timers_.advance(now);
    esp8266_mac_timers::advance(registers_,now);
    wifi_dma_.advance(registers_,now);
    if(!wifi_dma_.receive_error().empty()) {
        diagnostic_=wifi_dma_.receive_error();return false;
    }
    esp8266_interrupts::update_mac(registers_,cpu_);
    const unsigned events=esp8266_watchdog::advance(registers_,now);
    if((events&esp8266_watchdog::INTERRUPT) && (register_value(0x3ff00004)&1))cpu_.set_irq(8);
    if(events&esp8266_watchdog::RESET)request_reset(RESET_DIGITAL,1,4);
    return true;
}

bool EspMachine::cycles_to_esp8266_timer(u64 &cycles) const
{
    if(chip_!=esp::CHIP_ESP8266)return false;
    u64 ticks,candidate;bool found=frc_timers_.next_event_ticks(ticks);
    if(wifi_dma_.next_event_ticks(esp32_apb_ticks(),candidate) && (!found || candidate<ticks)) {
        ticks=candidate;found=true;
    }
    if(esp8266_mac_timers::next_event_ticks(registers_,esp32_apb_ticks(),candidate) && (!found || candidate<ticks)) {
        ticks=candidate;found=true;
    }
    if(esp8266_watchdog::next_event_ticks(registers_,esp32_apb_ticks(),candidate) && (!found || candidate<ticks)) {
        ticks=candidate;found=true;
    }
    return found && cycles_for_apb_ticks(ticks,cycles);
}

/* Both LX6 cores share one bus and one deterministic clock. Instructions
 * interleave at instruction boundaries, so atomic instructions stay atomic
 * and both CCOUNTs keep ticking while either core executes or waits. */
bool EspMachine::core_clocked(unsigned core) const
{
    if (!core) return true;
    return chip_==esp::CHIP_ESP32 && !app_reset_ && (register_value(0x3ff00030)&1);
}

void EspMachine::update_core_controls()
{
    if(chip_!=esp::CHIP_ESP32) return;
    std::map<u32,u32>::const_iterator reset=registers_.find(0x3ff0002c);
    const bool asserted=reset==registers_.end() || (reset->second&1);
    if(asserted && !app_reset_) {
        retired_cpu_instructions_+=cpu1_.instructions();
        cpu1_.reset(0x40000400);
        u32 cause=registers_.count(0x3ff48034)?registers_[0x3ff48034]:0x41;
        registers_[0x3ff48034]=(cause&~(63u<<6))|(14u<<6);
        ++reset_count_;
    }
    app_reset_=asserted;
    interrupts_.attach_cpu(0,core_available(0)?&cpu_:0);
    interrupts_.attach_cpu(1,core_available(1)?&cpu1_:0);
}

bool EspMachine::core_available(unsigned core) const
{
    if(!core_clocked(core)) return false;
    if(chip_==esp::CHIP_ESP32) {
        if(core && (register_value(0x3ff00034)&1)) return false;
        const unsigned c0=core?0:2,c1=core?20:26;
        const unsigned stall=((register_value(0x3ff48000)>>c0)&3) |
            (((register_value(0x3ff480ac)>>c1)&63)<<2);
        if(stall==0x86) return false;
    }
    return true;
}

bool EspMachine::core_runnable(unsigned core)
{
    if(!core_available(core)) return false;
    if(chip_==esp::CHIP_ESP32) {
        /* A ROM-less APP reset service waits for the same boot-address word
         * as ROM main(). A supplied mask ROM executes its own polling loop. */
        if(core && cpu1_.pc()==0x40000400 && !register_value(0x3ff00038)) {
            Region *reset=region(0x40000400,3);
            if(!reset || !reset->executable || reset->writable) return false;
        }
    }
    cpu(core).service_interrupts();
    return !cpu(core).waiting();
}

bool EspMachine::select_runnable_core()
{
    for(unsigned i=0;i<core_count();++i) {
        unsigned core=(next_core_+i)%core_count();
        if(core_runnable(core)) { active_core_=core;return true; }
    }
    return false;
}

void EspMachine::advance_clocked_cores(u64 cycles,int except_core)
{
    for(unsigned core=0;core<core_count();++core) {
        if((int)core==except_core || !core_clocked(core)) continue;
        /* Full counter turns latch all compares once. Subsequent turns do
         * not change their sticky state, and need no host-side loop. */
        if(cycles>0xffffffffull) {
            cpu(core).advance_cycles(0xffffffffu);cpu(core).advance_cycles(1);
        }
        cpu(core).advance_cycles((u32)cycles);
    }
}

bool EspMachine::app_rom_bootstrap()
{
    if(chip_!=esp::CHIP_ESP32 || active_core_!=1 || cpu1_.pc()!=0x40000400) return false;
    const u32 boot=register_value(0x3ff00038);
    if(!boot) return false;
    /* ROM's fast-boot path is CALLX0; the normal path prepares a windowed
     * call after initializing its stack. The supplied entry still executes
     * all application initialization and inter-core synchronization. */
    if(boot&0x80000000u) {
        cpu1_.set_reg(0,0x40000471);
        cpu1_.set_pc(boot&0x7fffffffu);
    } else {
        const u32 stack=0x3ffe3eb0;
        cpu1_.set_reg(1,stack);
        cpu1_.set_special(XT_SR_PS,0x60020);
        cpu1_.set_reg(8,0x80007c18);
        write(stack-12,4,stack+112);
        cpu1_.set_pc(boot);
    }
    ++rom_calls_;
    return true;
}

XtensaResult EspMachine::idle_until_event()
{
    /* A masked periodic peripheral or external radio may keep scheduling
     * events indefinitely. Yield without retiring a guest instruction so
     * the backend can process pause, stop and host input between batches. */
    for (unsigned events=0;events<128;++events) {
        advance_clocked_devices();
        if(!update_esp32_rtc()) return XTENSA_EXCEPTION;
        if(apply_pending_reset())return XTENSA_OK;
        update_esp32_interrupts();
        if(!update_esp8266_timers())return XTENSA_MEMORY_FAULT;
        update_uart_interrupts();
        if(apply_pending_reset())return XTENSA_OK;
        if(select_runnable_core()) return XTENSA_OK;
        bool found=false;u64 cycles=0,candidate;
        for(unsigned core=0;core<core_count();++core) {
            u32 delta;
            if(core_available(core) && cpu(core).cycles_to_next_timer(delta) && (!found || delta<cycles)) {
                cycles=delta;found=true;
            }
        }
        if(cycles_to_esp32_interrupt(candidate) && (!found || candidate<cycles)) {cycles=candidate;found=true;}
        if(cycles_to_esp8266_timer(candidate) && (!found || candidate<cycles)) {cycles=candidate;found=true;}
        if(cycles_to_esp32_rtc(candidate) && (!found || candidate<cycles)) {cycles=candidate;found=true;}
        if(cycles_to_uart_interrupt(candidate) && (!found || candidate<cycles)) {cycles=candidate;found=true;}
        if(cycles_to_clocked_device(candidate) && (!found || candidate<cycles)) {cycles=candidate;found=true;}
        if(!found) return XTENSA_WAIT;
        clock_skipped_cycles_+=cycles;
        advance_clocked_cores(cycles);
    }
    return XTENSA_OK;
}

XtensaResult EspMachine::step_core()
{
    XtensaCpu &current=active_cpu();
    XtensaResult result;
    const u32 pc=current.pc();
    const bool ram_boot=chip_==esp::CHIP_ESP32 && pc>=0x40000000 && pc<0x40008000 &&
        (register_value(0x3ff00000+active_core_*4)&1);
    Region *code=region(ram_boot?0x400b0000+(pc-0x40000000):pc,1);
    bool mapped_rom=code && code->executable && !code->flash_mapped && !code->writable;
    if(!ram_boot && !mapped_rom && app_rom_bootstrap()) return XTENSA_OK;
    if(!ram_boot && !mapped_rom && rom_service(pc,result)) return result;
    const u32 rom_end=chip_==esp::CHIP_ESP32?0x40070000u:0x40010000u;
    if(!ram_boot && !mapped_rom && pc>=0x40000000u && pc<rom_end) {
        diagnostic_=strf("unsupported %s ROM service at 0x%08x",esp::chip_name(chip_),pc);
        return XTENSA_EXCEPTION;
    }
    result=current.step();
    if(result==XTENSA_EXCEPTION) {
        const bool remap=chip_==esp::CHIP_ESP32 && (register_value(0x3ff00000+active_core_*4)&1);
        const u32 ps=current.special(XT_SR_PS);
        const u32 offset=(ps&16)?(chip_==esp::CHIP_ESP32?0x3c0:0x70):
            (ps&32)?(chip_==esp::CHIP_ESP32?0x340:0x50):(chip_==esp::CHIP_ESP32?0x300:0x30);
        const u32 vector=current.special(XT_SR_VECBASE)+offset;
        const u32 physical=remap && vector>=0x40000000 && vector<0x40008000?
            0x400b0000+(vector-0x40000000):vector;
        Region *handler=remap && vector>=0x400b0000 && vector<0x400b8000?0:region(physical,3);
        if(handler && handler->executable) {
            size_t start=physical-handler->base;
            if(handler->bytes[start] || handler->bytes[start+1] || handler->bytes[start+2]) {
                current.enter_exception(current.special(XT_SR_EXCCAUSE));result=XTENSA_OK;
            }
        }
    }
    if(result!=XTENSA_OK && result!=XTENSA_WAIT && diagnostic_.empty())
        diagnostic_=strf("Xtensa core %u stopped with status %d at 0x%08x, instruction 0x%06x",
            active_core_,(int)result,pc,current.last_instruction());
    return result;
}

XtensaResult EspMachine::step()
{
    if(apply_pending_reset())return XTENSA_OK;
    if(stopped_!=XTENSA_OK && stopped_!=XTENSA_WAIT) return stopped_;
    advance_clocked_devices();
    update_core_controls();
    if(!update_esp32_rtc()) return stopped_=XTENSA_EXCEPTION;
    if(apply_pending_reset())return XTENSA_OK;
    update_esp32_interrupts();
    if(!update_esp8266_timers())return stopped_=XTENSA_MEMORY_FAULT;
    update_uart_interrupts();
    if(apply_pending_reset())return XTENSA_OK;
    if(!select_runnable_core()) {
        const unsigned previous_resets=reset_count_;
        const XtensaResult idle=idle_until_event();
        if(idle!=XTENSA_OK) return stopped_=idle;
        if(reset_count_!=previous_resets)return XTENSA_OK;
        if(!select_runnable_core())return stopped_=XTENSA_OK;
    }
    const u64 before=active_cpu().instructions();
    XtensaResult result=step_core();
    advance_clocked_cores(active_cpu().instructions()-before,(int)active_core_);
    if(apply_pending_reset())return XTENSA_OK;
    next_core_=(active_core_+1)%core_count();
    if(result==XTENSA_WAIT) result=idle_until_event();
    return stopped_=result;
}
