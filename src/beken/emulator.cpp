#include "beken/emulator.h"
#include "beken/periph.h"
#include "beken/blecore.h"
#include "beken/wifimac.h"
#include "wifipeer.h"
#include "netstack.h"

#include <string.h>

/* ----- chip identities ----- */
bool chip_family(const std::string &name, ChipId &out)
{
    const Chip *c = find_chip(upper(pyc::py_strip(name)));
    if (!c)
        return false;
    out.chip = c->chip_id;
    out.device = c->dev_id;
    return true;
}
StrList chip_family_names()
{
    return chip_names();
}

SimState::SimState()
    : icu_int_enable(0), icu_global_int_en(0), pending_irqs(0), uart1_int_enable(0), uart2_int_enable(0),
      insn_count(0), pwm_status(0), timer0_2_ctl(0), timer3_5_ctl(0), efuse_ctrl(0), saradc_cfg(0),
      saradc_cfg2(0), saradc_state_reg(0), saradc_pending(0), saradc_channel(-1), saradc_last_raw(-1),
      saradc_conversions(0), saradc_reads(0), last_pc(0), last_slow(0), last_fast(0), last_emit(0), gpio_hw(32),
      i2c_last_pin(-1), bl0942_polls(0), bl0942_active(false), uart1_tx_prev(-1), uart_at_line_start(true), last_uart_insn(0),
      last_periph_sync(0),
      idle_task_valid(false), idle_task_lo(0), idle_task_hi(0), pwmn_seen(false), pwmn_stat(0),
      wdt_first_period_valid(false), wdt_first_period(0), wdt_period_ms(0), wdt_clock_enabled(true),
      wdt_deadline_valid(false), wdt_deadline(0), sctrl_rosc_timer(0), sctrl_rosc_timer_h(0),
      sctrl_rosc_int_status(false), sctrl_gpio_wakeup_en(0), sctrl_gpio_wakeup_type(0),
      sctrl_gpio_wakeup_int_status(0), sctrl_gpio_wakeup_en1(0), sctrl_gpio_wakeup_type1(0),
      sctrl_gpio_wakeup_int_status1(0), aon_gpio_wakeup_status(0), sctrl_reset_retention(0),
      in_deep_sleep(false), deep_sleep_count(0), deep_sleep_rtc_sec(0.0), last_wake_pin(-1),
      wake_pulse_pin(-1), wake_pulse_restore(0), wake_pulse_reads_left(0), wake_reg_sampled(false),
      rtt_boot_seen(false), obk_boot_seen(false),
      msh_prompt_seen(false), msh_uart_port(-1), rx_next_line_insn(0), audio_fifo_conf(0),
      audio_dac_writes(0), camera_sensor_probed(false), camera_i2c_writes(0), camera_last_i2c_insn(0),
      ejpeg_reg0(0), ejpeg_reg1(0), ejpeg_reg2(0), ejpeg_reg3(0), ejpeg_int_stat(0), ejpeg_frame_len(0),
      ejpeg_dma_ch(-1), ejpeg_dma_dst(0), ejpeg_dma_node_len(1460), ejpeg_tick_div(0), camera_frame_count(0),
      camera_width(640), camera_height(480)
{
    for (int i = 0; i < 6; i++) {
        hw_timer_counter[i] = 0;
        hw_timer_due[i] = 0;
    }
    hw_i2c[1] = HwI2cBus();
    hw_i2c[2] = HwI2cBus();
    hw_i2c[2].v2 = true;
}

/* ----- BLE/Wi-Fi guest-memory accessors (what Python sets as model.mem) ----- */
Bytes ble_mem_read(void *ctx, u32 addr, u32 size)
{
    return ((BekenEmulator *)ctx)->memory().read_bytes(addr, size);
}
void ble_mem_write(void *ctx, u32 addr, const Bytes &data)
{
    ((BekenEmulator *)ctx)->memory().write_bytes(addr, data);
}

/* ----- MMIO trampolines ----- */
static void mem_on_read(void *owner, Memory *, u32 addr, int size)
{
    ((BekenEmulator *)owner)->mmio_read(addr, size);
}
static void mem_on_write(void *owner, Memory *, u32 addr, int size, u32 value)
{
    ((BekenEmulator *)owner)->mmio_write(addr, size, value);
}
static bool mem_on_unmapped(void *owner, Memory *, u32 addr, int size, int type)
{
    return ((BekenEmulator *)owner)->mmio_unmapped(addr, size, type);
}

static bool decode_flash_uid_b64(const Bytes &src, Bytes &out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string tag("flash_uid_save=");
    size_t pos = src.find(tag);
    if (pos == std::string::npos)
        return false;
    pos += tag.size();
    std::string b64;
    for (size_t i = pos; i < src.size() && i < pos + 48; i++) {
        u8 ch = (u8)src[i];
        if (ch == 0 || ch == 0xFF)
            break;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
            || ch == '+' || ch == '/' || ch == '=')
            b64 += (char)ch;
        else
            break;
    }
    if (b64.size() < 20)
        return false;
    int rev[256];
    for (int i = 0; i < 256; i++)
        rev[i] = -1;
    for (int i = 0; i < 64; i++)
        rev[(u8)tbl[i]] = i;
    out.clear();
    u32 acc = 0;
    int q = 0;
    for (size_t i = 0; i < b64.size(); i++) {
        if (b64[i] == '=')
            break;
        int v = rev[(u8)b64[i]];
        if (v < 0)
            break;
        acc = (acc << 6) | (u32)v;
        if (q == 1) out += (char)(u8)(acc >> 4);
        else if (q == 2) out += (char)(u8)(acc >> 2);
        else if (q == 3) out += (char)(u8)acc;
        q = (q + 1) & 3;
    }
    if (out.size() >= 16) {
        out.resize(16);
        return true;
    }
    return false;
}

BekenEmulator::BekenEmulator(const Bytes &raw_flash, const Bytes &bootloader, const Bytes &app,
                             const BuildOptions &opt, const ChipId &chip_in, const Bytes *physical_flash,
                             const Chip *chip_def)
    : uart_sink(0), uart_sink_ctx(0), net_sink(0), net_sink_ctx(0), reset_count(0), has_run_error(false),
      chip(0), cpu_(mem_), raw_flash_(raw_flash), bootloader_(bootloader), app_(app),
      flash_page0_word0_(0), opt_(opt),
      exception_vector_base_(0), tuyamcu_(0), blecore_(0), wifimac_(0), wifi_peer_(0), trng_state_(0),
      have_trng_(false), network_insn_offset_(0), stopping_(0), watchdog_reset_pending_(false),
      paused_(false), resume_(true), uart_src_(0), have_resume_at_(false), resume_at_(0),
      have_power_on_ctx_(false), deep_sleep_wake_pending_valid_(false), deep_sleep_wake_pending_pin_(-1),
      snapshot_at_insns_valid_(false), snapshot_at_insns_(0), snapshot_due_(false),
      ip_pcap_(0)
{
    have_physical_ = physical_flash != 0;
    net_frame_sink = 0;
    net_frame_sink_ctx = 0;
    ui_view_req_ = 0;
    ui_wake_req_valid_ = false;
    ui_wake_req_pin_ = -1;
    deep_sleep_wake_queue_ = opt.deep_sleep_wakes;
    if (have_physical_)
        physical_flash_ = *physical_flash;

    flash_uid_ = Bytes("BP12637\x07\x00\xb9#'C\x01\xf0x", 16);
    {
        Bytes uid;
        if (decode_flash_uid_b64(have_physical_ ? physical_flash_ : raw_flash_, uid))
            flash_uid_ = uid;
    }

    only_uart_ = opt.only_uart;
    uart1_hex_ = opt.uart1_hex;
    uart1_rx_ = opt.uart1_rx;
    chip_variant = upper(strip(opt.chip));
    chip = chip_def ? chip_def : find_chip(chip_variant);
    if (!chip)
        chip = &default_chip();
    chip_variant = chip->name;
    exception_vector_base_ = opt.with_boot ? 0 : 0; /* app_window sets 0x10000 via build_emulator */

    chip_id_value = chip_in.chip ? chip_in.chip : chip->chip_id;
    device_id_value = chip_in.device ? chip_in.device : chip->dev_id;

    is_bk7252 = (chip_variant == "BK7252");
    is_bk7252n = (chip_variant == "BK7252N");
    gpio_pin_count = chip->gpio_pins;
    has_extended_gpio = (gpio_pin_count > 32);
    has_pwm_new = chip->is_pwm_v2();
    is_bk7231 = (chip_variant == "BK7231" || chip_variant == "BK7231U" || chip_variant == "BK7231N");

    emit_insns_ = env_has("EMU_REPORT");
    block_pacing_ = env_has("EMU_BLOCK_PACING");
    {
        std::string c = env_get("EMU_CHUNK_DEVICE_INSNS");
        long long n;
        chunk_device_insns_ = (!c.empty() && parse_int(c, 10, n)) ? (u64)n : CHUNK_DEVICE_INSNS;
        /* a negative grain is Python's max(1, round(negative)): one-instruction chunks */
        if (!c.empty() && parse_int(c, 10, n) && n < 0)
            chunk_device_insns_ = 0;
    }
    {
        std::string c = env_get("EMU_MAX_INSNS");
        long long n;
        max_insns_ = (!c.empty() && parse_int(c, 10, n)) ? (u64)n : 0;
    }
    idle_skip_ = env_get("EMU_IDLE_SKIP") != "0";
    idle_chunks_skipped_ = chunks_run_ = 0;
    exc_epoch_ = 0;

    /* Python's constructor default is CONSOLE_RX_HOLDOFF_INSNS whatever the
     * peer; build_emulator is what drops it to 0 for a TuyaMCU run. */
    uart1_rx_delay_ = opt.uart1_rx_delay_set ? opt.uart1_rx_delay : CONSOLE_RX_HOLDOFF_INSNS;

    /* SimState gpio width */
    if (gpio_pin_count != state.gpio_hw.pin_count)
        state.gpio_hw = GpioModel(gpio_pin_count);
    state.hw_i2c[1].v2 = chip->is_i2c_v2();
    for (std::map<int, int>::const_iterator it = opt.gpio_inputs.begin(); it != opt.gpio_inputs.end(); ++it) {
        /* Python: gpio_hw.check_pin / check_level raise ValueError */
        if (it->first < 0 || it->first >= state.gpio_hw.pin_count) {
            if (init_error.empty())
                init_error = strf("GPIO pin must be in the range 0-%d", state.gpio_hw.pin_count - 1);
            continue;
        }
        int lvl = GpioModel::check_level(it->second);
        if (lvl < -1) {
            if (init_error.empty())
                init_error = "GPIO input level must be 0, 1, or None (released)";
            continue;
        }
        if (lvl >= 0)
            state.gpio_hw.inputs[it->first] = lvl;
    }

    for (std::map<int, int>::const_iterator it = opt.adc_inputs.begin(); it != opt.adc_inputs.end(); ++it)
        set_adc_input(it->first, it->second);

    /* energy meter defaults */
    meter_v_ = 230.0;
    meter_a_ = 1.5;
    meter_w_ = 345.0;
    meter_cf_pin_ = meter_cf1_pin_ = meter_sel_pin_ = -1;
    meter_sel_lv_ = 1;
    meter_cf_ticks_ = meter_cf1_ticks_ = meter_cf_lv_ = meter_cf1_lv_ = 0;

    /* TuyaMCU peer */
    if (opt.tuyamcu) {
        std::string pid = opt.tuyamcu_pid.empty() ? "bekenemulator000" : opt.tuyamcu_pid;
        tuyamcu_ = new tuyamcu::Slave(pid, "1.0.0", opt.tuyamcu_raw);
        for (size_t i = 0; i < opt.tuyamcu_dps.size(); i++)
            tuyamcu_->set_dp(opt.tuyamcu_dps[i].first, opt.tuyamcu_dps[i].second.type,
                             opt.tuyamcu_dps[i].second.ivalue, opt.tuyamcu_dps[i].second.svalue);
        for (size_t i = 0; i < opt.tuyamcu_injects.size(); i++)
            tuyamcu_->add_inject(opt.tuyamcu_injects[i]);
    }

    /* BLE core selection */
    ble_layout_ = chip->ble_core ? chip->ble_core : "";
    bool auto_ble = chip->ble_auto;
    bool want_ble = opt.ble_core == 1 || (opt.ble_core == -1 && auto_ble);
    if (opt.ble_core == 1 && ble_layout_.empty()) {
        if (init_error.empty())
            init_error = "--ble-core models the BLE 5.x core of BK7231N/M (5.1) and BK7238 (5.2); "
                         "this chip has no such core.";
    }
    if (want_ble && !ble_layout_.empty()) {
        blecore_ = new BleCore(XVR_BASE, (u64)SLOW_TICK_INSNS * 1000 / SLOW_TICK_MS, ble_layout_.c_str());
        MemAccess ma;
        ma.ctx = this;
        ma.read = ble_mem_read;
        ma.write = ble_mem_write;
        blecore_->set_mem(ma);
    }

    /* Wi-Fi MAC */
    if (opt.wifi) {
        wifimac_ = new MacHw((u64)SLOW_TICK_INSNS / (SLOW_TICK_MS * 1000));
        MemAccess ma;
        ma.ctx = this;
        ma.read = ble_mem_read;
        ma.write = ble_mem_write;
        wifimac_->mem = ma;
        wifimac_->have_mem = true;
        wifimac_->log = &BekenEmulator::wifi_net_log;
        wifimac_->log_ctx = this;
        trng_state_ = 0x2545F491;
        have_trng_ = true;
    }

    mem_.owner = this;
    mem_.on_read = mem_on_read;
    mem_.on_write = mem_on_write;
    mem_.on_unmapped = mem_on_unmapped;
}

BekenEmulator::~BekenEmulator()
{
    close_network();
    delete tuyamcu_;
    delete blecore_;
    delete wifimac_;
    delete ip_pcap_;
}

/* --------------------------------------------------------------- printing */
void BekenEmulator::print_line(const std::string &s)
{
    if (!only_uart_)
        out_write(s + "\n");
}

static bool bytes_contains(const Bytes &hay, const char *needle)
{
    return hay.find(needle) != std::string::npos;
}

void BekenEmulator::net_log(const std::string &msg)
{
    if (net_sink) {
        net_sink(net_sink_ctx, msg);
        return;
    }
    Bytes data = Bytes(msg) + "\n";
    if (uart_sink) {
        Bytes all = Bytes("\n") + data;
        for (size_t i = 0; i < all.size(); i++)
            uart_sink(uart_sink_ctx, 2, (u8)all[i]);
        return;
    }
    if (!state.uart_at_line_start)
        data = Bytes("\n") + data;
    out_write(data);
    out_flush();
    state.uart_at_line_start = true;
}

void BekenEmulator::uart_write(const Bytes &data)
{
    out_write(data);
    if (!data.empty()) {
        state.uart_at_line_start = (u8)data[data.size() - 1] == 0x0A;
        state.last_uart_insn = state.insn_count;
        if (state.uart_at_line_start || uart1_hex_)
            out_flush();
        if (!snapshot_marker_.empty())
            snapshot_check_uart(data);
    }
}

void BekenEmulator::note_uart_tx(int port, u8 b)
{
    state.last_uart_insn = state.insn_count;
    if (b == 0x0A) {
        state.uart_at_line_start = true;
        state.uart_line_tail.clear();
    } else {
        state.uart_at_line_start = false;
        state.uart_line_tail += (char)b;
        if (state.uart_line_tail.size() > 24)
            state.uart_line_tail.erase(0, state.uart_line_tail.size() - 24);
        const Bytes &t = state.uart_line_tail;
        if (!state.obk_boot_seen && b == ':' && (bytes_contains(t, "Info:MAIN:") || bytes_contains(t, "Warn:CFG:")))
            state.obk_boot_seen = true;
        if (!state.rtt_boot_seen && (b == ' ' || b == 'd' || b == ']')
            && (bytes_contains(t, "RT -     ") || bytes_contains(t, "rt-thread") || bytes_contains(t, "[I/FAL]")
                || bytes_contains(t, "[D/FAL]")))
            state.rtt_boot_seen = true;
        if (b == '>' && bytes_contains(t, "msh />")) {
            state.rtt_boot_seen = true;
            state.msh_prompt_seen = true;
            state.msh_uart_port = port;
        }
        if ((b == ':' || b == 'n') && (bytes_contains(t, "[THD]cli_main:") || bytes_contains(t, "power on"))) {
            state.msh_uart_port = port;
            if (!uart1_rx_.empty() && !tuyamcu_) {
                u64 due = state.insn_count + 2500000ULL;
                if (due > state.rx_next_line_insn)
                    state.rx_next_line_insn = due;
            }
        }
    }
}

/* --------------------------------------------------------------- flash */
Bytes &BekenEmulator::flash_backing()
{
    return have_physical_ ? physical_flash_ : raw_flash_;
}

void BekenEmulator::flash_program(u32 addr, const std::vector<u32> &words)
{
    if (words.empty())
        return;
    Bytes &buf = flash_backing();
    Bytes data;
    for (size_t i = 0; i < words.size(); i++)
        data += p32le(words[i]);
    u32 base = addr & ~0x1Fu;
    for (size_t i = 0; i < data.size(); i++) {
        u32 pos = base + (u32)i;
        if (pos < buf.size())
            buf[pos] = (char)((u8)buf[pos] & (u8)data[i]);
    }
}

void BekenEmulator::flash_program_page(u32 addr, const Bytes &data)
{
    if (data.empty())
        return;
    Bytes &buf = flash_backing();
    u32 base = addr & ~0xFFu;
    size_t n = data.size() < 256 ? data.size() : 256;
    for (size_t i = 0; i < n; i++) {
        u32 pos = base + (u32)i;
        if (pos < buf.size())
            buf[pos] = (char)((u8)buf[pos] & (u8)data[i]);
    }
}

void BekenEmulator::flash_erase(u32 addr, u32 size)
{
    Bytes &buf = flash_backing();
    u32 start = addr & ~(size - 1);
    u32 end = start + size;
    if (end > buf.size())
        end = (u32)buf.size();
    for (u32 i = start; i < end; i++)
        buf[i] = (char)0xFF;
}

bool BekenEmulator::range_mapped(u32 addr, u32 len)
{
    if (!len)
        return true;
    u64 end = (u64)addr + len; /* exclusive */
    if (end > 0x100000000ULL)
        return false;
    for (u64 page = addr & ~0xFFFu; page < end; page += 0x1000)
        if (!mem_.is_mapped((u32)page))
            return false;
    return true;
}

/* --------------------------------------------------------------- GPIO glue */
int BekenEmulator::gpio_slot(int pin)
{
    if (pin < 32)
        return pin;
    return 48 + (pin - 32);
}
u32 BekenEmulator::gpio_config_word(int pin)
{
    std::map<int, u32>::iterator it = state.gpio_cfg.find(gpio_slot(pin));
    return it == state.gpio_cfg.end() ? 0 : it->second;
}
int BekenEmulator::gpio_pin_at(u32 address)
{
    u32 GPIO_BASE = 0x00802800;
    u32 end = has_extended_gpio ? (GPIO_BASE + (48 + 8) * 4) : (GPIO_BASE + 32 * 4);
    if (address < GPIO_BASE || address >= end)
        return -1;
    if (address & 3)
        return -1;
    u32 slot = (address - GPIO_BASE) / 4;
    if (slot < 32)
        return (int)slot;
    if (has_extended_gpio && slot >= 48 && slot < 48 + 8)
        return 32 + (int)(slot - 48);
    return -1;
}

void BekenEmulator::apply_gpio_events()
{
    if (gpio_events_.empty())
        return;
    std::vector<std::pair<int, int> > events;
    {
        ScopedLock lk(gpio_lock_);
        if (gpio_events_.empty())
            return;
        events.swap(gpio_events_);
    }
    for (size_t i = 0; i < events.size(); i++)
        state.gpio_hw.drive(events[i].first, events[i].second, gpio_config_word(events[i].first));
}

void BekenEmulator::sync_gpio_irq()
{
    /* Resample enabled level-mode pins (inline, since the config lookup needs
     * this emulator instance). Then drive the GPIO controller's ICU line. */
    u64 enabled = state.gpio_hw.int_enable;
    if (!enabled) {
        state.pending_irqs &= ~(1u << 7);
        return;
    }
    while (enabled) {
        u64 bit = enabled & (~enabled + 1);
        enabled ^= bit;
        int pin = __builtin_ctzll(bit);
        int mode = state.gpio_hw.int_modes[(size_t)pin];
        if (mode != INT_LEVEL_LOW && mode != INT_LEVEL_HIGH)
            continue;
        int lvl = state.gpio_hw.effective_level(pin, gpio_config_word(pin));
        if ((mode == INT_LEVEL_LOW && lvl == 0) || (mode == INT_LEVEL_HIGH && lvl == 1))
            state.gpio_hw.int_status |= bit;
    }
    if (state.gpio_hw.irq_asserted())
        state.pending_irqs |= (1u << 7);
    else
        state.pending_irqs &= ~(1u << 7);
}

u32 BekenEmulator::old_pwm_timer_status_mask(u32 pwm_ctl)
{
    u32 mask = 0;
    for (int channel = 0; channel < 6; channel++)
        if (((pwm_ctl >> (4 * channel)) & 0xF) == 0x7)
            mask |= 1u << channel;
    return mask;
}

u32 BekenEmulator::trng_next()
{
    u32 x = trng_state_;
    x ^= (x << 13);
    x ^= x >> 17;
    x ^= (x << 5);
    trng_state_ = x;
    return x;
}

u64 BekenEmulator::network_now_us()
{
    u64 insns = network_insn_offset_ + state.insn_count;
    return wifimac_ ? wifimac_->now_us(insns) : 0;
}

/* --------------------------------------------------------------- stop/pause */
void BekenEmulator::stop()
{
    atomic_set(&stopping_, 1);
    pause(false);
    cpu_.request_stop();
}
bool BekenEmulator::is_paused() { return paused_; }
void BekenEmulator::pause(bool p)
{
    if (p) {
        resume_.clear();
        paused_ = true;
    } else {
        paused_ = false;
        resume_.set();
    }
}

void BekenEmulator::feed_uart1(const Bytes &data)
{
    ScopedLock lk(rx_lock_);
    uart1_rx_ += data;
    state.msh_prompt_seen = true;
}

bool BekenEmulator::set_gpio_input(int pin, int level)
{
    /* Python raises ValueError for a pin outside the chip's logical range
     * (a controller-register slot is not an injectable GPIO32) or a level
     * other than 0/1/None. */
    if (pin < 0 || pin >= gpio_pin_count)
        return false;
    int lvl = GpioModel::check_level(level);
    if (lvl < -1)
        return false;
    ScopedLock lk(gpio_lock_);
    gpio_events_.push_back(std::make_pair(pin, lvl));
    return true;
}

void BekenEmulator::set_adc_input(int channel, int value)
{
    int ch = channel & 0xF;
    int clamped = value < 0 ? 0 : (value > 0xFFFF ? 0xFFFF : value);
    ScopedLock lk(adc_lock_);
    adc_inputs[ch] = clamped;
    state.adc_samples[ch] = clamped;
}

bool BekenEmulator::set_adc_input(const std::string &target, int value, std::string *err)
{
    int ch = 0;
    if (!periph::parse_adc_channel(target, chip->name, ch, err))
        return false;
    set_adc_input(ch, value);
    return true;
}

int BekenEmulator::get_adc_input(int channel) const
{
    int ch = channel & 0xF;
    ScopedLock lk(adc_lock_);
    std::map<int, int>::const_iterator it = adc_inputs.find(ch);
    return it == adc_inputs.end() ? periph::DEFAULT_ADC_RAW : it->second;
}

u32 BekenEmulator::saradc_sample_for(int channel)
{
    int ch = channel & 0xF;
    int raw = get_adc_input(ch) & 0xFFFF;
    state.saradc_channel = ch;
    state.saradc_last_raw = raw;
    state.adc_samples[ch] = raw;
    return (u32)raw;
}

void BekenEmulator::set_energy_meter(double v, double a, double w, int cf_pin, int cf1_pin, int sel_pin, int sel_lv)
{
    meter_v_ = v < 0 ? 0 : v;
    meter_a_ = a < 0 ? 0 : a;
    meter_w_ = w < 0 ? 0 : w;
    if (cf_pin >= 0)
        meter_cf_pin_ = cf_pin;
    if (cf1_pin >= 0)
        meter_cf1_pin_ = cf1_pin;
    if (sel_pin >= 0)
        meter_sel_pin_ = sel_pin;
    if (sel_lv >= 0)
        meter_sel_lv_ = sel_lv ? 1 : 0;
}

/* ------------------------------------------------------- GUI front-end hooks */
size_t BekenEmulator::uart1_rx_queued()
{
    ScopedLock lk(rx_lock_);
    return uart1_rx_.size();
}

void BekenEmulator::wake_from_deep_sleep(const std::string &reason, int pin)
{
    {
        ScopedLock lk(gpio_lock_);
        ui_wake_req_reason_ = reason.empty() ? std::string("auto") : reason;
        ui_wake_req_pin_ = (reason == "gpio") ? pin : -1;
        ui_wake_req_valid_ = true;
    }
    pause(false);
}

void BekenEmulator::ui_view(UiView &out)
{
    atomic_set(&ui_view_req_, 1);
    ScopedLock lk(ui_view_lock_);
    out = ui_view_;
}

/* Emulation thread only: copy the displayed fields while nothing writes them. */
void BekenEmulator::publish_ui_view()
{
    atomic_set(&ui_view_req_, 0);
    UiView v;
    v.valid = true;
    v.insn_count = state.insn_count;
    v.last_pc = state.last_pc;
    v.gpio_cfg = state.gpio_cfg;
    v.pwm_regs = state.pwm_regs;
    v.i2c_frames = state.i2c_frames;
    v.i2c_last_on = state.i2c_last_on;
    v.ws2812_spi = state.ws2812_spi;
    v.bl0942_polls = state.bl0942_polls;
    v.in_deep_sleep = state.in_deep_sleep;
    v.deep_sleep_count = state.deep_sleep_count;
    v.deep_sleep_rtc_sec = state.deep_sleep_rtc_sec;
    v.deep_sleep_wake_pins = state.deep_sleep_wake_pins;
    v.last_wake_reason = state.last_wake_reason;
    v.last_wake_pin = state.last_wake_pin;
    v.camera_sensor = state.camera_sensor;
    v.camera_width = state.camera_width;
    v.camera_height = state.camera_height;
    v.camera_i2c_writes = state.camera_i2c_writes;
    v.camera_frame_count = state.camera_frame_count;
    v.camera_last_jpeg = state.camera_last_jpeg;
    v.camera_frame_rgb = state.camera_frame_rgb;
    v.saradc_channel = state.saradc_channel;
    v.saradc_last_raw = state.saradc_last_raw;
    v.saradc_conversions = state.saradc_conversions;
    v.saradc_reads = state.saradc_reads;
    v.adc_samples = state.adc_samples;
    {
        ScopedLock lk(adc_lock_);
        v.adc_inputs = adc_inputs;
    }
    v.forward_listening = wifi_peer_ && wifi_peer_->net && wifi_peer_->net->listener_count() > 0;
    ScopedLock lk(ui_view_lock_);
    ui_view_ = v;
}

static void trace_pc_trampoline(void *ctx, u32 addr)
{
    ((BekenEmulator *)ctx)->on_trace_pc_hit(addr);
}

void BekenEmulator::on_trace_pc_hit(u32 addr)
{
    if (pc_trace_seen_.count(addr))
        return;
    pc_trace_seen_.insert(addr);
    net_log(strf("[EMU_PC] reached 0x%08x at %llu instructions", addr,
                 (unsigned long long)state.insn_count));
}

void BekenEmulator::trace_pc(u32 address)
{
    u32 a = address & 0xFFFFFFFEu;
    if (pc_trace_addrs_.count(a))
        return;
    pc_trace_addrs_.insert(a);
    cpu_.watch_pcs.push_back(a);
    cpu_.watch_cb = trace_pc_trampoline;
    cpu_.watch_ctx = this;
}
