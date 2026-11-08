#include "esp/esp_machine.h"
#include "esp/esp_spi.h"
#include "esp32/esp32_rtc_io.h"

void EspMachine::request_reset(ResetDomain domain,unsigned mask,unsigned reason)
{
    mask&=(1u<<core_count())-1;
    if(!mask || domain==RESET_NONE)return;
    if(domain>reset_domain_) {reset_domain_=domain;reset_mask_=mask;reset_reason_=reason;}
    else if(domain==reset_domain_) {reset_mask_|=mask;reset_reason_=reason;}
}

bool EspMachine::apply_pending_reset()
{
    if(reset_domain_==RESET_NONE)return false;
    const ResetDomain domain=reset_domain_;
    const unsigned mask=reset_mask_,reason=reset_reason_&63;
    reset_domain_=RESET_NONE;reset_mask_=reset_reason_=0;
    u32 causes=registers_.count(0x3ff48034)?registers_[0x3ff48034]:0x41;
    for(unsigned core=0;core<core_count();++core) if(mask&(1u<<core)) {
        interrupts_.attach_cpu(core,0);
        retired_cpu_instructions_+=cpu(core).instructions();
        cpu(core).reset(chip_==esp::CHIP_ESP32?0x40000400:0x40000080);
        causes=(causes&~(63u<<(core*6)))|(reason<<(core*6));
    }
    if(domain>=RESET_DIGITAL) {
        /* Reset controller state, not physical SRAM or external devices.
         * A digital reset retains the RTC domain, including its watchdog's
         * next stage; the wider RTC reset also restarts the RTC counter. */
        std::map<u32,u32> retained;
        for(std::map<u32,u32>::const_iterator i=registers_.begin();i!=registers_.end();++i) {
            const u32 address=i->first;
            const bool rtc=chip_==esp::CHIP_ESP32?
                ((address>=0x3ff48000 && address<0x3ff49000) ||
                 (address>=0xffff1200 && address<=0xffff1208) || esp32_rtc_io::retained_state_register(address)):
                (address>=0x60000700 && address<0x60000800);
            if((domain==RESET_DIGITAL && rtc) || esp_spi::external_state_register(address))
                retained[address]=i->second;
        }
        registers_.swap(retained);
        wifi_dma_.reset();
        interrupts_.reset();
        for(unsigned i=0;i<3;++i)uarts_[i].reset(chip_==esp::CHIP_ESP32,true);
        /* The RTC clock mux survives a digital reset. Reuse the clock
         * controller's divider reset instead of assuming a PLL clock. */
        if(chip_==esp::CHIP_ESP32)esp32_mmio_write(0x3ff0003c,0);
        else {
            set_cpu_frequency(80);
            frc_timers_.reset(esp32_apb_ticks());
        }
        app_reset_=true;gpio_levels_=0;
        putc1_=0;output_stack_.clear();flash_window_=0;cache_enabled_=true;
        if(domain==RESET_RTC) {
            rtc_epoch_us_=elapsed_us();rtc_wake_us_=rtc_calibration_us_=0;
        }
        reset_clocked_devices();
    }
    if(chip_==esp::CHIP_ESP32)registers_[0x3ff48034]=causes;
    else registers_[0x60000714]=(register_value(0x60000714)&~15u)|(reason&15);
    active_core_=next_core_=0;
    diagnostic_.clear();stopped_=XTENSA_OK;++reset_count_;
    update_core_controls();update_gpio_interrupts();update_uart_interrupts();
    return true;
}
