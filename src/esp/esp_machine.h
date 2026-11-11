#ifndef BK_ESP_MACHINE_H
#define BK_ESP_MACHINE_H

#include "esp/esp_image.h"
#include "arch/xtensa/xtensa_cpu.h"
#include "esp32/esp32_interrupts.h"
#include "esp8266/esp8266_timers.h"
#include "esp8266/esp8266_wifi_dma.h"
#include "esp8266/esp8266_radio.h"
#include "esp/esp_uart.h"
#include "rom_image.h"

/* A chip owns its address map and peripherals; XtensaCpu only sees a bus.
 * ROM services are explicit ABI implementations. Unknown ROM addresses and
 * peripherals fail closed instead of silently returning fabricated values. */
class EspMachine : public XtensaBus {
public:
    explicit EspMachine(esp::Chip chip);
    bool load(const Bytes &flash, const esp::FlashImage &layout, bool bootloader,
              std::string *error = 0);
    bool load_rom(const std::vector<RomSegment> &segments, std::string *error = 0);
    bool load_rom(const Bytes &image, std::string *error = 0);
    XtensaResult step();
    XtensaResult run(u64 max_steps);
    virtual bool read(u32 address, unsigned size, u32 &value, bool instruction);
    virtual bool write(u32 address, unsigned size, u32 value);
    virtual bool external_read(u32 address, u32 &value);
    virtual bool external_write(u32 address, u32 value);
    XtensaCpu &cpu(unsigned core = 0) { return core == 1 ? cpu1_ : cpu_; }
    const XtensaCpu &cpu(unsigned core = 0) const { return core == 1 ? cpu1_ : cpu_; }
    unsigned active_core() const { return active_core_; }
    unsigned core_count() const { return chip_ == esp::CHIP_ESP32 ? 2 : 1; }
    const std::string &uart() const { return uarts_[0].output(); }
    const std::string &uart(unsigned index) const;
    void receive_uart(unsigned index, const Bytes &data);
    void feed_uart(int port, const Bytes &data) { if (port >= 0) receive_uart((unsigned)port, data); }
    bool set_gpio_input(int pin, int level);
    bool gpio_state(unsigned pin, bool &input, bool &output, bool &enabled) const;
    const std::string &diagnostic() const { return diagnostic_; }
    u64 instructions() const { return cpu_instructions()+rom_calls_; }
    u64 rom_calls() const { return rom_calls_; }
    u64 elapsed_us() const;
    bool set_xtal_frequency(unsigned mhz);
    unsigned xtal_frequency() const { return xtal_mhz_; }
    bool set_psram_size(unsigned bytes);
    unsigned psram_size() const { return (unsigned)psram_.size(); }
    unsigned reset_count() const { return reset_count_; }
    esp::Chip chip() const { return chip_; }
    const Bytes &flash() const { return flash_; }
    /* Backend-owned external devices share the guest clock. Deadlines are
     * absolute microseconds, including time skipped by WAIT. Callbacks must
     * not attach/detach devices or recursively step the machine. Detach before
     * destroying a device; the machine never takes ownership. */
    class ClockedDevice {
    public:
        virtual ~ClockedDevice() {}
        virtual void reset(u64 now_us)=0;
        virtual bool next_event_us(u64 &deadline) const=0;
        virtual void advance(u64 now_us)=0;
    };
    bool attach_clocked_device(ClockedDevice *device);
    bool detach_clocked_device(ClockedDevice *device);
    /* Radio adapters pass actual frame bytes through hardware descriptors.
     * Callers own the peer lifetime and enforce channel reception policy. */
    bool set_wifi_air(Esp8266WifiDma::Air *air);
    typedef esp8266_radio::State WifiRadioState;
    bool wifi_radio_state(WifiRadioState &state);
    Esp8266WifiDma::ReceiveResult receive_wifi(const Bytes &frame,
        const Esp8266WifiDma::RxInfo &info,std::string *error=0);
    u64 wifi_transmissions() const { return chip_ == esp::CHIP_ESP32 ? register_value(0xffff3000u) : wifi_dma_.transmissions(); }
    u64 wifi_receptions() const { return chip_ == esp::CHIP_ESP32 ? register_value(0xffff3004u) : wifi_dma_.receptions(); }

private:
    struct Region {
        u32 base;
        Bytes bytes;
        bool writable, executable, flash_mapped;
        unsigned core_mask;
        Region(u32 start, size_t size, bool w, bool x, unsigned cores = 3)
            : base(start), bytes(size, '\0'), writable(w), executable(x), flash_mapped(false), core_mask(cores) {}
    };
    esp::Chip chip_;
    XtensaCpu cpu_, cpu1_;
    Esp32Interrupts interrupts_;
    Esp8266Timers frc_timers_;
    Esp8266WifiDma wifi_dma_;
    std::vector<ClockedDevice *> clocked_devices_;
    unsigned active_core_, next_core_;
    bool app_reset_;
    enum ResetDomain { RESET_NONE, RESET_CPU, RESET_DIGITAL, RESET_RTC };
    ResetDomain reset_domain_;
    unsigned reset_mask_, reset_reason_, reset_count_;
    u64 rtc_epoch_us_;
    u64 retired_cpu_instructions_;
    Bytes flash_, psram_;
    std::vector<Region> regions_;
    std::map<u32, u32> registers_;
    EspUart uarts_[3];
    std::map<unsigned,int> gpio_external_;
    u64 gpio_levels_;
    std::string diagnostic_;
    struct RomOutput {
        u32 return_pc, callback;
        std::string text;
        size_t position;
        RomOutput() : return_pc(0), callback(0), position(0) {}
    };
    std::vector<RomOutput> output_stack_;
    u32 putc1_;
    u64 rom_calls_, delay_us_, clock_origin_instructions_, clock_skipped_cycles_;
    u64 apb_origin_ticks_, apb_origin_delay_us_;
    u64 rtc_wake_us_, rtc_calibration_us_, esp8266_timer_check_ticks_;
    u32 cpu_mhz_, xtal_mhz_, flash_window_, random_;
    bool cache_enabled_;
    XtensaResult stopped_;

    void initialize_memory();
    void request_reset(ResetDomain domain,unsigned cpu_mask,unsigned reason);
    bool apply_pending_reset();
    void initialize_flash_descriptor();
    Region *region(u32 address, unsigned size);
    bool load_segment(const esp::Segment &segment, std::string *error);
    bool uart_address(u32 address, unsigned &index, u32 &offset) const;
    void uart_transmit(unsigned index, u8 byte);
    void update_uart_interrupts();
    bool cycles_to_uart_interrupt(u64 &cycles) const;
    bool gpio_mmio_read(u32 address, u32 &value);
    bool gpio_mmio_write(u32 address, u32 value);
    void update_gpio_interrupts();
    u32 gpio_interrupt_status(unsigned bank, unsigned enable_mask) const;
    bool mmio_read(u32 address, u32 &value);
    bool mmio_write(u32 address, u32 value);
    u64 esp32_apb_ticks() const;
    unsigned apb_frequency() const;
    bool cycles_for_apb_ticks(u64 ticks, u64 &cycles) const;
    bool update_esp32_rtc();
    bool cycles_to_esp32_rtc(u64 &cycles) const;
    void update_esp32_interrupts();
    bool cycles_to_esp32_interrupt(u64 &cycles) const;
    bool update_esp8266_timers();
    bool cycles_to_esp8266_timer(u64 &cycles) const;
    bool esp8266_timer_read(u32 address, u32 &value);
    bool esp8266_timer_write(u32 address, u32 value);
    bool esp32_mmio_read(u32 address, u32 &value);
    bool esp32_mmio_write(u32 address, u32 value);
    bool esp32_mmu_read(u32 address, unsigned size, u32 &value,
                       bool instruction, bool &handled);
    bool esp32_psram_read(u32 address, unsigned size, u32 &value,
                         bool instruction, bool &handled);
    bool esp32_psram_write(u32 address, unsigned size, u32 value, bool &handled);
    bool esp8266_mmio_read(u32 address, u32 &value);
    bool esp8266_mmio_write(u32 address, u32 value);
    bool fault(const char *operation, u32 address);
    bool rom_service(u32 address, XtensaResult &result);
    bool rom_string(u32 address, std::string &out, size_t limit = 65536);
    bool rom_printf(std::string &out);
    bool rom_vprintf(u32 format_addr, u32 va_stack, u32 va_reg, u32 va_ndx, std::string &out);
    bool rom_argument(unsigned index, u32 &value);
    bool rom_copy(u32 dst, u32 src, u32 length);
    void set_cpu_frequency(u32 mhz);
    XtensaResult idle_until_event();
    void reset_clocked_devices();
    void advance_clocked_devices();
    bool cycles_to_clocked_device(u64 &cycles) const;
    XtensaResult step_core();
    XtensaCpu &active_cpu() { return cpu(active_core_); }
    const XtensaCpu &active_cpu() const { return cpu(active_core_); }
    u64 cpu_instructions() const { return retired_cpu_instructions_ + cpu_.instructions() + cpu1_.instructions(); }
    bool core_clocked(unsigned core) const;
    bool core_available(unsigned core) const;
    bool core_runnable(unsigned core);
    bool select_runnable_core();
    void update_core_controls();
    void advance_clocked_cores(u64 cycles, int except_core = -1);
    bool app_rom_bootstrap();
    u32 register_value(u32 address) const;
};

#endif
