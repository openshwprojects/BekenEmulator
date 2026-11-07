/* The Beken SoC emulator: wires the ARM core to the peripheral register model,
 * UART, ICU/interrupts, timers, flash controller, GPIO, PWM, SARADC, watchdog,
 * deep sleep, camera, DMA and the optional Wi-Fi/BLE/network models. Port of
 * src/emulator.py, driven by the chunked execution loop. */
#ifndef BK_EMULATOR_H
#define BK_EMULATOR_H

#include <set>
#include "common.h"
#include "platform.h"
#include "arm_cpu.h"
#include "chips.h"
#include "gpio.h"
#include "tuyamcu.h"
#include "beken_crypto.h"

/* Forward declarations for the optional peripheral models. */
class BleCore;
class MacHw;
class WifiPeer;
struct TxInfo;
namespace netstack {
class NetStack;
class EthPcap;
}

/* ----- chip identities (SCTRL id registers) ----- */
struct ChipId {
    u32 chip, device;
};
bool chip_family(const std::string &name, ChipId &out);
StrList chip_family_names();

/* Pacing constants (the device-clock model). */
static const u64 SLOW_TICK_INSNS = 10000;
static const u64 SLOW_TICK_MS = 2;
static const u64 FAST_TICK_INSNS = 1000;
static const u64 CHUNK_DEVICE_INSNS = FAST_TICK_INSNS;
static const u64 CONSOLE_RX_HOLDOFF_INSNS = 2000000;

struct FlashState {
    u32 addr;
    Bytes data;
    int read_idx;
    std::vector<u32> write_buf;
    Bytes pw_buf;
    FlashState() : addr(0), data(H("ffffffff")), read_idx(0) {}
};

/* Hardware I2C bus shadow */
struct HwI2cBus {
    u32 cfg, sta;
    u32 last_dat, read_dat;
    u32 slave, reg, phase;
    bool v2;
    HwI2cBus() : cfg(0), sta(0), last_dat(0), read_dat(0), slave(0), reg(0), phase(0), v2(false) {}
};

/* Decoded SCTRL/AON configuration captured when the chip enters deep sleep. */
struct DeepSleepInfo {
    bool valid;
    u32 sleep_reg;
    bool rtc_enabled;
    u32 rtc_ticks;
    bool rtc_seconds_valid; /* false == Python None */
    double rtc_seconds;
    u64 wake_gpio_mask;
    u64 wake_gpio_type;
    std::vector<std::pair<int, int> > wake_pins; /* (pin, active_level) */
    u64 insn_count;
    DeepSleepInfo()
        : valid(false), sleep_reg(0), rtc_enabled(false), rtc_ticks(0),
          rtc_seconds_valid(false), rtc_seconds(0.0), wake_gpio_mask(0),
          wake_gpio_type(0), insn_count(0) {}
};

/* The whole mutable device state (what a snapshot carries). */
struct SimState {
    u32 icu_int_enable, icu_global_int_en;
    u32 pending_irqs;
    u32 uart1_int_enable, uart2_int_enable;
    u64 insn_count;
    u32 pwm_status, timer0_2_ctl, timer3_5_ctl, efuse_ctrl;
    u32 hw_timer_counter[6];
    u64 hw_timer_due[6]; /* 0 = None */
    u32 saradc_cfg;
    u32 saradc_cfg2;
    u32 saradc_state_reg;
    int saradc_pending;
    int saradc_channel;  /* -1 = None */
    int saradc_last_raw; /* -1 = None */
    int saradc_conversions;
    int saradc_reads;
    std::map<int, int> adc_samples;
    Bytes spi_tx;
    Bytes spi_rx;
    u32 last_pc;
    u64 last_slow, last_fast, last_emit;
    std::map<int, u32> gpio_cfg;
    GpioModel gpio_hw;
    std::map<u32, u32> pwm_regs;
    std::set<int> gpio_dirty;
    std::set<u32> pwm_dirty;
    /* bit-banged LED I2C capture */
    int i2c_last_pin;
    std::map<std::pair<int, int>, int> i2c_pair_counts;
    /* bus state: (scl,sda) -> [in_tx,cur_byte,bits, frame] */
    struct I2cBus {
        bool in_tx;
        int cur_byte, bits;
        Bytes frame;
        I2cBus() : in_tx(false), cur_byte(0), bits(0) {}
    };
    std::map<std::pair<int, int>, I2cBus> i2c_buses;
    std::map<std::pair<int, int>, Bytes> i2c_frames;
    std::map<std::pair<int, int>, Bytes> i2c_last_on;
    std::set<std::pair<int, int> > i2c_dirty;
    Bytes ws2812_spi;
    int bl0942_polls;
    bool bl0942_active;
    Bytes bl0942_cmd_buf;
    std::map<u8, u32> bl0942_regs;
    int uart1_tx_prev;
    bool uart_at_line_start;
    u64 last_uart_insn;
    Bytes uart_line_tail;
    /* idle task stack (low,high); valid flag */
    bool idle_task_valid;
    u32 idle_task_lo, idle_task_hi;
    /* UART1 event log for the report */
    struct U1Evt { u64 insn; char dir; u8 byte; };
    std::vector<U1Evt> uart1_events;
    std::set<u32> xvr_armed;
    std::map<int, u32> gdma_conf;
    bool pwmn_seen;
    u32 pwmn_stat;
    /* watchdog */
    bool wdt_first_period_valid;
    u32 wdt_first_period;
    u32 wdt_period_ms;
    bool wdt_clock_enabled;
    bool wdt_deadline_valid;
    u64 wdt_deadline;
    /* SCTRL / deep sleep */
    u32 sctrl_rosc_timer, sctrl_rosc_timer_h;
    bool sctrl_rosc_int_status;
    u32 sctrl_gpio_wakeup_en, sctrl_gpio_wakeup_type, sctrl_gpio_wakeup_int_status;
    u32 sctrl_gpio_wakeup_en1, sctrl_gpio_wakeup_type1, sctrl_gpio_wakeup_int_status1;
    u32 aon_gpio_wakeup_status, sctrl_reset_retention;
    bool in_deep_sleep;
    int deep_sleep_count;
    DeepSleepInfo deep_sleep_info;
    double deep_sleep_rtc_sec;
    std::map<int, int> deep_sleep_wake_pins; /* pin -> active level */
    std::string last_wake_reason; /* "" / "rtc" / "gpio" */
    int last_wake_pin;            /* -1 none */
    int wake_pulse_pin;           /* -1 none */
    int wake_pulse_restore;
    int wake_pulse_reads_left;
    bool wake_reg_sampled;
    bool rtt_boot_seen, obk_boot_seen, msh_prompt_seen;
    int msh_uart_port; /* -1 = none */
    u64 rx_next_line_insn;
    u32 audio_fifo_conf;
    int audio_dac_writes;
    std::map<int, u32> gdma_remain;
    std::map<int, HwI2cBus> hw_i2c;
    /* camera / ejpeg */
    std::string camera_sensor;
    bool camera_sensor_probed;
    std::map<std::pair<int, int>, u8> camera_sensor_regs;
    int camera_i2c_writes;
    u64 camera_last_i2c_insn;
    u32 ejpeg_reg0, ejpeg_reg1, ejpeg_reg2, ejpeg_reg3, ejpeg_int_stat;
    u32 ejpeg_frame_len;
    int ejpeg_dma_ch; /* -1 = none */
    u32 ejpeg_dma_dst, ejpeg_dma_node_len;
    int ejpeg_tick_div;
    int camera_frame_count;
    Bytes camera_last_jpeg, camera_frame_rgb;
    int camera_width, camera_height;

    SimState();
};

struct BuildOptions {
    std::string chip;    /* family name */
    bool with_boot;
    bool only_uart;
    bool uart1_hex;
    Bytes uart1_rx;
    u64 uart1_rx_delay;
    bool uart1_rx_delay_set;
    bool tuyamcu;
    std::string tuyamcu_pid;
    bool tuyamcu_raw;
    std::vector<std::pair<int, tuyamcu::DpSpec> > tuyamcu_dps;
    std::vector<Bytes> tuyamcu_injects;
    bool xvr_selfclear;
    int ble_core; /* -1 auto, 0 off, 1 on */
    bool wifi;
    std::map<int, int> gpio_inputs;
    std::map<int, int> adc_inputs;
    StrList deep_sleep_wakes;
    BuildOptions()
        : chip("BK7231"), with_boot(false), only_uart(false), uart1_hex(false), uart1_rx_delay(CONSOLE_RX_HOLDOFF_INSNS),
          uart1_rx_delay_set(false), tuyamcu(false), tuyamcu_raw(false), xvr_selfclear(false), ble_core(-1), wifi(false)
    {
    }
};

class BekenEmulator {
public:
    /* Hardware base addresses (public like the Python class attributes). */
    static const u32 FLASH_BASE = 0x00000000, FLASH_SIZE = 2 * 1024 * 1024;
    static const u32 RAM_BASE = 0x00400000, RAM_SIZE = 1 * 1024 * 1024;
    static const u32 MMIO_BASE = 0x00800000, MMIO_SIZE = 0x010000;
    static const u32 PERIPH_BASE = 0xC0000000, PERIPH_SIZE = 0x100000;
    static const u32 SPI_FLASH_BASE = 0x00200000;
    static const u32 XVR_BASE = 0x00900000;
    static const u32 ACCEL_BASE = 0x00810000;
    static const u64 TIMER_INSNS_PER_SEC = SLOW_TICK_INSNS * 1000ULL / SLOW_TICK_MS;

    BekenEmulator(const Bytes &raw_flash, const Bytes &bootloader, const Bytes &app,
                  const BuildOptions &opt, const ChipId &chip, const Bytes *physical_flash,
                  const Chip *chip_def = 0);
    ~BekenEmulator();

    void setup();
    /* setup() maps the SPI-flash mirror with the dump's size; Unicorn rejects
     * a zero-size map (UC_ERR_ARG), so an empty dump cannot be set up. */
    bool flash_empty() const { return raw_flash_.empty(); }
    void run();
    void stop();

    /* UART sink: a front end (GUI) sets this; CLI leaves it null. */
    void (*uart_sink)(void *ctx, int port, u8 byte);
    void *uart_sink_ctx;
    void (*net_sink)(void *ctx, const std::string &line);
    void *net_sink_ctx;

    SimState state;
    u64 reset_count;
    std::string run_error;
    bool has_run_error;

    /* diagnostics / GUI accessors */
    bool is_paused();
    void pause(bool p = true);
    void feed_uart1(const Bytes &data);
    /* level<0 = release; false (nothing queued) for a pin outside
     * 0..gpio_pin_count-1 or a level other than 0/1/release (Python raises). */
    bool set_gpio_input(int pin, int level);
    void set_adc_input(int channel, int value);
    bool set_adc_input(const std::string &target, int value, std::string *err = 0);
    int get_adc_input(int channel) const;
    void set_energy_meter(double v, double a, double w, int cf_pin, int cf1_pin, int sel_pin, int sel_lv);

    /* --- GUI front-end hooks (tools/sim_ui) --- */
    /* Every frame the IP bridge carried: ("from device"/"to device", bytes, us). */
    void (*net_frame_sink)(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us);
    void *net_frame_sink_ctx;
    /* Request a wake from deep sleep (GUI thread); reason rtc/gpio/auto, pin -1 = none. */
    void wake_from_deep_sleep(const std::string &reason, int pin);
    double meter_v() const { return meter_v_; }
    double meter_a() const { return meter_a_; }
    double meter_w() const { return meter_w_; }
    u64 uart1_rx_delay() const { return uart1_rx_delay_; }
    size_t uart1_rx_queued();
    /* A consistent copy of what the GUI displays, published by the emulation
     * thread between chunks whenever ui_view() asked for one (the maps in
     * SimState must never be read while the worker may be writing them). */
    struct UiView {
        bool valid;
        u64 insn_count;
        u32 last_pc;
        std::map<int, u32> gpio_cfg;
        std::map<u32, u32> pwm_regs;
        std::map<std::pair<int, int>, Bytes> i2c_frames, i2c_last_on;
        Bytes ws2812_spi;
        int bl0942_polls;
        bool in_deep_sleep;
        int deep_sleep_count;
        double deep_sleep_rtc_sec;
        std::map<int, int> deep_sleep_wake_pins;
        std::string last_wake_reason;
        int last_wake_pin;
        std::string camera_sensor;
        int camera_width, camera_height, camera_i2c_writes, camera_frame_count;
        Bytes camera_last_jpeg, camera_frame_rgb;
        int saradc_channel;
        int saradc_last_raw;
        int saradc_conversions;
        int saradc_reads;
        std::map<int, int> adc_samples;
        std::map<int, int> adc_inputs;
        bool forward_listening;
        UiView()
            : valid(false), insn_count(0), last_pc(0), bl0942_polls(0), in_deep_sleep(false),
              deep_sleep_count(0), deep_sleep_rtc_sec(0.0), last_wake_pin(-1), camera_width(640),
              camera_height(480), camera_i2c_writes(0), camera_frame_count(0),
              saradc_channel(-1), saradc_last_raw(-1), saradc_conversions(0), saradc_reads(0),
              forward_listening(false) {}
    };
    void ui_view(UiView &out); /* last published copy; also asks for a fresh one */

    /* snapshots */
    void arm_snapshot(const std::string &path, bool at_is_insns, u64 at_insns, const std::string &marker);
    bool load_snapshot(const std::string &path, std::string *err);

    /* trace a PC */
    void trace_pc(u32 address);
    void on_trace_pc_hit(u32 addr);

    /* wifi peer attach */
    WifiPeer *attach_wifi_peer(const std::string &role, const std::string &ssid, int channel,
                               const Bytes &peer_mac, const std::string &passphrase,
                               const std::vector<std::pair<int, int> > &forwards, bool net_out,
                               const std::string &pcap, const StrList &http_gets,
                               const std::vector<std::pair<std::string, Bytes> > &mqtt_pubs,
                               const std::vector<Bytes> &tls_psks, bool tuya_legacy_identity_psk);
    void close_network();
    void set_pcap_ip(const std::string &path);

    Memory &memory() { return mem_; }
    ArmCpu &cpu() { return cpu_; }
    void set_exception_vector_base(u32 v) { exception_vector_base_ = v; }

    /* --- MMIO callbacks (invoked by Memory) --- */
    void mmio_read(u32 addr, int size);
    void mmio_write(u32 addr, int size, u32 value);
    /* type: Unicorn's UC_MEM_*_UNMAPPED code (19 read, 20 write, 21 fetch) */
    bool mmio_unmapped(u32 addr, int size, int type);
    /* Non-empty when the constructor rejected its options (Python raises
     * ValueError there); build_emulator reports it and discards the object. */
    std::string init_error;

    const Chip *chip;
    std::map<int, int> adc_inputs;
    bool is_bk7252, is_bk7252n, is_bk7231, has_pwm_new, has_extended_gpio;
    std::string chip_variant;
    int gpio_pin_count;
    u32 chip_id_value, device_id_value;

private:
    friend struct PyTestAccess; /* tests/pytests.h: the ported Python unit-test files */
    Memory mem_;
    ArmCpu cpu_;
    Bytes raw_flash_, bootloader_, app_, physical_flash_;
    bool have_physical_;
    Bytes flash_uid_;
    Bytes flash_page0_;
    u32 flash_page0_word0_;
    BuildOptions opt_;
    u32 exception_vector_base_;
    bool only_uart_, uart1_hex_;
    Bytes uart1_rx_;
    Mutex rx_lock_;
    mutable Mutex adc_lock_;
    u64 uart1_rx_delay_;
    bool emit_insns_;
    bool block_pacing_;
    u64 chunk_device_insns_;
    u64 max_insns_; /* EMU_MAX_INSNS: stop after this many device insns (0 = unlimited) */
    bool idle_skip_;
    u64 idle_chunks_skipped_, chunks_run_;
    u32 exc_epoch_;

    tuyamcu::Slave *tuyamcu_;
    BleCore *blecore_;
    std::string ble_layout_;
    MacHw *wifimac_;
    WifiPeer *wifi_peer_;
    u32 trng_state_;
    bool have_trng_;
    u64 network_insn_offset_;

    volatile long stopping_;
    bool watchdog_reset_pending_;
    volatile bool paused_; /* written by the UI/test thread */
    Event resume_;

    Mutex gpio_lock_;
    std::vector<std::pair<int, int> > gpio_events_;

    /* energy meter */
    double meter_v_, meter_a_, meter_w_;
    int meter_cf_pin_, meter_cf1_pin_, meter_sel_pin_, meter_sel_lv_;
    int meter_cf_ticks_, meter_cf1_ticks_, meter_cf_lv_, meter_cf1_lv_;

    FlashState flash_state_;
    const char *uart_src_;

    bool have_resume_at_;
    /* advertising payloads already printed as [BLE_ADV] (Python _ble_adv_seen) */
    std::set<Bytes> ble_adv_seen_;
    u32 resume_at_;

    CpuContext power_on_ctx_;
    bool have_power_on_ctx_;

    /* deep sleep */
    StrList deep_sleep_wake_queue_;
    bool deep_sleep_wake_pending_valid_;
    std::string deep_sleep_wake_pending_reason_;
    int deep_sleep_wake_pending_pin_;

    /* snapshot request */
    std::string snapshot_path_;
    bool snapshot_at_insns_valid_;
    u64 snapshot_at_insns_;
    Bytes snapshot_marker_;
    bool snapshot_due_;
    Bytes uart_tail_;

    std::set<u32> pc_trace_addrs_;
    std::set<u32> pc_trace_seen_;

    netstack::EthPcap *ip_pcap_;

    /* GUI view + wake request (see ui_view / wake_from_deep_sleep) */
    Mutex ui_view_lock_;
    UiView ui_view_;
    volatile long ui_view_req_;
    bool ui_wake_req_valid_;
    std::string ui_wake_req_reason_;
    int ui_wake_req_pin_;
    void publish_ui_view();

    /* helpers */
    void print_line(const std::string &s); /* _print */
    void net_log(const std::string &msg);
    void uart_write(const Bytes &data);
    void note_uart_tx(int port, u8 b);
    void pacing();       /* pacing_inner + the EMU_MAX_INSNS cap */
    void pacing_inner(); /* Python _pacing */
    void run_chunked(u32 start);
    void run_block_paced(u32 start); /* EMU_BLOCK_PACING: Python's hook_block mode */
    void trigger_irq();
    void trigger_fiq();
    bool watchdog_expired();
    void arm_watchdog();
    void apply_gpio_events();
    void sync_gpio_irq();
    u32 gpio_config_word(int pin);
    int gpio_slot(int pin);
    int gpio_pin_at(u32 address);
    void i2c_pin_edge(int pin, u32 old_cfg, u32 new_cfg);
    void flush_report_periph();
    bool rx_active();
    bool rx_active_uart2();
    bool use_uart2_rx();
    bool rx_ready_for_console();
    void uart_irqs_due(bool &u1, bool &u2);
    bool fast_tick_idle();
    void flash_program(u32 addr, const std::vector<u32> &words);
    void flash_program_page(u32 addr, const Bytes &data);
    void flash_erase(u32 addr, u32 size);
    /* the register side of mmio_write (Python's hook body; the original store
     * that Unicorn commits after the hook is done by mmio_write itself) */
    void mmio_write_regs(u32 addr, int size, u32 value);
    /* whole [addr, addr+len) mapped, i.e. Unicorn's mem_read/mem_write would not
     * raise UC_ERR_*_UNMAPPED */
    bool range_mapped(u32 addr, u32 len);
    Bytes &flash_backing();
    u32 cold_vector_base();
    void find_idle_task();
    bool idle_step_to_open(u32 &pc, u32 &thumb, u32 cpsr, u32 lo, u32 hi);
    bool skip_idle(u64 step);
    void hw_i2c_write(int bus, u32 offset, u32 value);
    u32 hw_i2c_read(int bus, u32 offset);
    u32 camera_i2c_read(u32 slave, u32 reg);
    void camera_i2c_write(u32 slave, u32 reg, u32 data);
    int gdma_channel(u32 address);
    void gdma_start(int channel, u32 conf);
    void tick_energy_meter_pulses();
    Bytes bl0942_reply_packet();
    Bytes bl0942_reply_reg(u8 reg);
    Bytes bl0942_react_byte(u8 b_val);
    void tick_camera_ejpeg();
    void build_camera_frame(int frame_idx, int target_len, Bytes &jpeg_out, Bytes &rgb_out);
    void report_ble_adv();
    void reset_peripheral_models();
    void watchdog_reset();
    void snapshot_check_uart(const Bytes &data);
    void save_snapshot(const std::string &path);
    void restore_legacy_gpio_state();
    /* deep sleep */
    DeepSleepInfo decode_deep_sleep_info(SimState &st, u32 sleep_reg_val);
    bool resolve_wake_spec(const std::string &reason_in, int pin_in, const DeepSleepInfo &info,
                           std::string &reason_out, int &pin_out);
    void enter_deep_sleep(u32 sleep_reg_val);
    bool wait_in_deep_sleep();
    void deep_sleep_reset(const std::string &reason, int pin);
    u32 trng_next();
    u32 old_pwm_timer_status_mask(u32 pwm_ctl);
    u64 network_now_us();
    void ensure_vector_page();
    static void on_swi_cb(void *ctx);
    u32 saradc_sample_for(int channel);
    u64 timer_period(const SimState &s, int idx) const;
    void timer_enable_changed(SimState &s, int bank, u32 old_ctl, u32 new_ctl);
    void tick_hw_timers(SimState &s);
    u64 next_timer_due(const SimState &s) const; /* 0 = None */

    /* Wi-Fi peer <-> MAC <-> IP-stack callback trampolines (ctx = this). */
    static const u64 NET_LATENCY_US = 3000;
    static void wifi_on_tx(void *ctx, const Bytes &frame, const TxInfo &info);
    static void wifi_inject(void *ctx, const Bytes &frame, int decr);
    static void wifi_net_log(void *ctx, const std::string &msg);
    static u64 wifi_now_us(void *ctx);
    static void wifi_on_frame(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us);
};

/* Build an emulator from a dump file + options (the main.py build_emulator).
 * On failure *err is str(exception) and *exc_type its Python class name:
 * "ValueError" for a rejected option (main.py prints it), anything else
 * ("FileNotFoundError", "OverflowError", ...) is what Python leaves uncaught. */
BekenEmulator *build_emulator(const std::string &dump_path, const BuildOptions &opt,
                              const KeyCoefs *key, std::string *err,
                              void (*log)(const std::string &) = 0, bool save_decrypted = true,
                              std::string *exc_type = 0);

#endif
