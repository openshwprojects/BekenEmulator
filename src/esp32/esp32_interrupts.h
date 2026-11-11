#ifndef BK_ESP32_INTERRUPTS_H
#define BK_ESP32_INTERRUPTS_H

#include "arch/xtensa/xtensa_cpu.h"
#include "esp/esp_frc_timers.h"

/* ESP32 peripheral interrupt routing and the four general-purpose timers.
 * Time is supplied by the machine in APB clock ticks, never wall-clock time.
 * Register addresses/fields follow Espressif's ESP32 soc headers. */
class Esp32Interrupts {
public:
    explicit Esp32Interrupts(XtensaCpu &cpu);
    void attach_cpu(unsigned core, XtensaCpu *cpu);
    void reset();
    bool handles(u32 address) const;
    bool read(u32 address, u64 apb_ticks, u32 &value);
    bool write(u32 address, u64 apb_ticks, u32 value);
    void advance(u64 apb_ticks);
    void set_source(unsigned source, bool active);
    void set_core_source(unsigned core, unsigned source, bool active);
    void configure_group_interrupt(unsigned group, unsigned index, bool level, bool edge);
    void raise_group_interrupt(unsigned group, unsigned index);
    /* Only return an event that can wake the current CPU interrupt mask. */
    bool next_event_ticks(u64 &ticks) const;
    bool source_can_wake(unsigned source) const;
    bool source_can_wake(unsigned source, unsigned core) const;

private:
    EspFrcTimers legacy_timers_;
    struct Timer {
        u64 counter, captured, load, alarm, phase, timestamp;
        u32 config;
    } timers_[2][3];
    u32 lact_rtc_[2];
    XtensaCpu *cpus_[2];
    u8 matrix_[2][69];
    bool sources_[2][69];
    u8 group_modes_[2][4];
    u32 enabled_[2], raw_[2], previous_edges_[2];
    void update_sources();
    void update_legacy_sources(unsigned events);
    void route();
};

#endif
