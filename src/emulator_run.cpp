/* Execution engine for BekenEmulator: interrupt entry, the device-clock pacing,
 * the chunked run loop with idle-skip, setup, run, and snapshots. Same class as
 * emulator.cpp, split for file size. */
#include "emulator.h"
#include "blecore.h"
#include "wifimac.h"
#include "wifipeer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdexcept>

/* Ctrl-C during run(): Python's KeyboardInterrupt ends run()'s loop with
 * "Emulation stopped by user." and still prints the closing lines. The
 * handler only flags the run; run() reports it. */
static BekenEmulator *volatile g_sigint_emu = 0;
static volatile sig_atomic_t g_sigint_hit = 0;
static void on_sigint(int)
{
    g_sigint_hit = 1;
    BekenEmulator *e = g_sigint_emu;
    if (e)
        e->stop();
}

/* EMU_PHASE_TIMING diagnostic accumulators (interpreter vs pacing wall time). */
static double g_cpu_secs = 0.0;
static double g_pace_secs = 0.0;

static const u64 IDLE_STREAK = 2;
static const u64 IDLE_MAX_SKIP = 50;
static const u64 IDLE_STEP_INSNS = 8;
static const u64 IDLE_STEP_TRIES = 64;
static const u64 IDLE_SCAN_INSNS = 10000000;
static const double CHUNK_SCALE_ARM = 1.0;
static const double CHUNK_SCALE_THUMB = 0.42;

/* -------------------------------------------------------------- interrupts */
void BekenEmulator::ensure_vector_page()
{
    if (!flash_page0_.empty() && state.msh_uart_port == 2) {
        if (mem_.phys_read(0, 4) != flash_page0_word0_)
            mem_.write_bytes(0, flash_page0_);
    }
}

void BekenEmulator::on_swi_cb(void *ctx)
{
    static_cast<BekenEmulator *>(ctx)->ensure_vector_page();
}

void BekenEmulator::trigger_irq()
{
    if (state.icu_global_int_en == 0)
        return;
    u32 cpsr = cpu_.reg_read(REG_CPSR);
    if (cpsr & PSR_I)
        return;
    ensure_vector_page();
    u32 pc = cpu_.reg_read(REG_PC);
    u32 new_cpsr = (cpsr & ~0x3Fu) | 0x92;
    cpu_.reg_write(REG_CPSR, new_cpsr);
    cpu_.reg_write(REG_SPSR, cpsr);
    cpu_.reg_write(REG_LR, pc + 4);
    cpu_.reg_write(REG_PC, cpu_.vector_base + 0x18);
    exc_epoch_++;
}

void BekenEmulator::trigger_fiq()
{
    if (!(state.icu_global_int_en & 0x2))
        return;
    u32 cpsr = cpu_.reg_read(REG_CPSR);
    if (cpsr & PSR_F)
        return;
    ensure_vector_page();
    u32 pc = cpu_.reg_read(REG_PC);
    u32 new_cpsr = (cpsr & ~0x3Fu) | 0x11 | 0xC0;
    cpu_.reg_write(REG_CPSR, new_cpsr);
    cpu_.reg_write(REG_SPSR, cpsr);
    cpu_.reg_write(REG_LR, pc + 4);
    cpu_.reg_write(REG_PC, cpu_.vector_base + 0x1C);
    exc_epoch_++;
}

/* -------------------------------------------------------------- watchdog */
void BekenEmulator::arm_watchdog()
{
    if (!state.wdt_clock_enabled || !state.wdt_period_ms) {
        state.wdt_deadline_valid = false;
        return;
    }
    state.wdt_deadline_valid = true;
    state.wdt_deadline = state.insn_count + (u64)state.wdt_period_ms * (SLOW_TICK_INSNS / SLOW_TICK_MS);
}

bool BekenEmulator::watchdog_expired()
{
    if (!state.wdt_deadline_valid || state.insn_count < state.wdt_deadline)
        return false;
    state.wdt_deadline_valid = false;
    watchdog_reset_pending_ = true;
    cpu_.request_stop();
    return true;
}

/* -------------------------------------------------------------- UART rx */
bool BekenEmulator::use_uart2_rx()
{
    if (tuyamcu_)
        return false;
    if (state.msh_uart_port == 1 || state.msh_uart_port == 2)
        return state.msh_uart_port == 2;
    if (!((state.uart2_int_enable & 0x42) && (state.icu_int_enable & (1u << 1))))
        return false;
    return !((state.uart1_int_enable & 0x42) && (state.icu_int_enable & (1u << 0)));
}

bool BekenEmulator::rx_ready_for_console()
{
    if (state.rtt_boot_seen || (chip->rtthread_console && !state.obk_boot_seen)) {
        if (state.msh_prompt_seen)
            return true;
        if (state.rx_next_line_insn)
            return state.insn_count >= state.rx_next_line_insn;
        u64 lim = uart1_rx_delay_ > 25000000 ? uart1_rx_delay_ : 25000000;
        return state.insn_count >= lim;
    }
    if (state.rx_next_line_insn && state.insn_count < state.rx_next_line_insn)
        return false;
    return state.insn_count >= uart1_rx_delay_;
}

bool BekenEmulator::rx_active()
{
    if (uart1_rx_.empty() || use_uart2_rx())
        return false;
    if (tuyamcu_)
        return state.insn_count >= uart1_rx_delay_;
    return rx_ready_for_console();
}

bool BekenEmulator::rx_active_uart2()
{
    if (uart1_rx_.empty() || !use_uart2_rx())
        return false;
    return rx_ready_for_console();
}

void BekenEmulator::uart_irqs_due(bool &u1, bool &u2)
{
    u1 = (state.icu_int_enable & (1u << 0)) &&
         ((state.uart1_int_enable & 0x01) || (rx_active() && (state.uart1_int_enable & 0x42)));
    u2 = (state.icu_int_enable & (1u << 1)) &&
         ((state.uart2_int_enable & 0x01) || (rx_active_uart2() && (state.uart2_int_enable & 0x42)));
}

bool BekenEmulator::fast_tick_idle()
{
    if (wifimac_ && wifimac_->busy())
        return false;
    bool u1, u2;
    uart_irqs_due(u1, u2);
    return !(u1 || u2);
}

/* -------------------------------------------------------------- energy meter */
void BekenEmulator::tick_energy_meter_pulses()
{
    if (meter_cf_pin_ >= 0 && meter_cf_pin_ < gpio_pin_count && meter_w_ > 0.1) {
        meter_cf_ticks_++;
        double wclamp = meter_w_ < 5.0 ? 5.0 : (meter_w_ > 3000.0 ? 3000.0 : meter_w_);
        int period = (int)py_round(600.0 / wclamp);
        if (period < 1)
            period = 1;
        if (meter_cf_ticks_ >= period) {
            meter_cf_ticks_ = 0;
            meter_cf_lv_ ^= 1;
            state.gpio_hw.drive(meter_cf_pin_, meter_cf_lv_, gpio_config_word(meter_cf_pin_));
        }
    }
    if (meter_cf1_pin_ >= 0 && meter_cf1_pin_ < gpio_pin_count) {
        meter_cf1_ticks_++;
        bool sel_v = true;
        if (meter_sel_pin_ >= 0 && meter_sel_pin_ < gpio_pin_count) {
            u32 sel_cfg = gpio_config_word(meter_sel_pin_);
            int sel_out = (!(sel_cfg & 0x08) && !(sel_cfg & 0x02)) ? 0 : 1;
            sel_v = (sel_out == meter_sel_lv_);
        }
        double rate = sel_v ? meter_v_ : (meter_a_ * 100.0);
        if (rate > 0.1) {
            double rc = rate < 5.0 ? 5.0 : (rate > 1500.0 ? 1500.0 : rate);
            int period = (int)py_round(300.0 / rc);
            if (period < 1)
                period = 1;
            if (meter_cf1_ticks_ >= period) {
                meter_cf1_ticks_ = 0;
                meter_cf1_lv_ ^= 1;
                state.gpio_hw.drive(meter_cf1_pin_, meter_cf1_lv_, gpio_config_word(meter_cf1_pin_));
            }
        }
    }
}

/* -------------------------------------------------------------- pacing */
void BekenEmulator::flush_report_periph()
{
    if (!emit_insns_)
        return;
    if (state.gpio_dirty.empty() && state.pwm_dirty.empty() && state.i2c_dirty.empty() && state.uart1_events.empty())
        return;
    Bytes out;
    if (!state.uart_at_line_start) {
        out += "\n";
        state.uart_at_line_start = true;
    }
    bool periph_changed = !state.gpio_dirty.empty() || !state.pwm_dirty.empty() || !state.i2c_dirty.empty();
    for (std::set<int>::iterator it = state.gpio_dirty.begin(); it != state.gpio_dirty.end(); ++it)
        out += strf("[EMU_GPIO] %d %08x\n", *it, state.gpio_cfg[*it]);
    for (std::set<u32>::iterator it = state.pwm_dirty.begin(); it != state.pwm_dirty.end(); ++it)
        out += strf("[EMU_PWM] %03x %08x\n", *it, state.pwm_regs[*it]);
    for (std::set<std::pair<int, int> >::iterator it = state.i2c_dirty.begin(); it != state.i2c_dirty.end(); ++it) {
        std::map<std::pair<int, int>, Bytes>::iterator f = state.i2c_frames.find(*it);
        if (f != state.i2c_frames.end() && !f->second.empty())
            out += strf("[EMU_I2C] %d %d %s\n", it->first, it->second, hexstr(f->second).c_str());
    }
    if (periph_changed)
        out += "[EMU_PERIPH]\n";
    for (size_t i = 0; i < state.uart1_events.size(); i++)
        out += strf("[EMU_UART1] %llu %c %02x\n", (unsigned long long)state.uart1_events[i].insn,
                    state.uart1_events[i].dir, state.uart1_events[i].byte);
    state.gpio_dirty.clear();
    state.pwm_dirty.clear();
    state.i2c_dirty.clear();
    state.uart1_events.clear();
    out_write(out);
    out_flush();
}

void BekenEmulator::pacing()
{
    pacing_inner();
    /* EMU_MAX_INSNS (the C++ twin of the differential runner's pywrap.py):
     * stop right after the pacing call that crosses the cap - every pacing
     * call, including the ones idle skipping makes, exactly where the Python
     * wrapper around _pacing sets `stopping`. */
    if (max_insns_ && state.insn_count >= max_insns_)
        atomic_set(&stopping_, 1);
}

void BekenEmulator::pacing_inner()
{
    if (ui_view_req_)
        publish_ui_view();
    /* Keep answering view requests while paused: a reader that pauses first
     * and requests a view second (test park(), the GUI) would otherwise wait
     * forever on a worker that already blocked here before seeing the request. */
    while (paused_) {
        if (ui_view_req_)
            publish_ui_view();
        if (resume_.wait(5))
            break;
    }

    if (watchdog_expired())
        return;

    apply_gpio_events();
    sync_gpio_irq();

    if (state.insn_count - state.last_slow >= SLOW_TICK_INSNS) {
        state.last_slow = state.insn_count;
        if (!state.uart_at_line_start)
            out_flush();
        if (meter_cf_pin_ >= 0 || meter_cf1_pin_ >= 0) {
            tick_energy_meter_pulses();
            sync_gpio_irq();
        }
        if (emit_insns_) {
            /* close an RT-Thread prompt line that has parked, so report lines flush */
            bool parked = !state.uart_at_line_start
                          && state.insn_count - state.last_uart_insn >= 500000
                          && (endswith(rstrip(state.uart_line_tail, " "), "msh />")
                              || state.uart_line_tail.find("WSL02810") != std::string::npos);
            if (parked) {
                out_write("\n");
                out_flush();
                state.uart_at_line_start = true;
                state.uart_line_tail.clear();
            }
            bool anydirty = !state.gpio_dirty.empty() || !state.pwm_dirty.empty() || !state.i2c_dirty.empty()
                            || !state.uart1_events.empty();
            if (anydirty && state.uart_at_line_start) {
                Bytes out;
                bool periph_changed = !state.gpio_dirty.empty() || !state.pwm_dirty.empty() || !state.i2c_dirty.empty();
                for (std::set<int>::iterator it = state.gpio_dirty.begin(); it != state.gpio_dirty.end(); ++it)
                    out += strf("[EMU_GPIO] %d %08x\n", *it, state.gpio_cfg[*it]);
                for (std::set<u32>::iterator it = state.pwm_dirty.begin(); it != state.pwm_dirty.end(); ++it)
                    out += strf("[EMU_PWM] %03x %08x\n", *it, state.pwm_regs[*it]);
                for (std::set<std::pair<int, int> >::iterator it = state.i2c_dirty.begin(); it != state.i2c_dirty.end(); ++it) {
                    std::map<std::pair<int, int>, Bytes>::iterator f = state.i2c_frames.find(*it);
                    if (f != state.i2c_frames.end() && !f->second.empty())
                        out += strf("[EMU_I2C] %d %d %s\n", it->first, it->second, hexstr(f->second).c_str());
                }
                if (periph_changed) {
                    out += "[EMU_PERIPH]\n";
                    state.last_periph_sync = state.insn_count;
                }
                for (size_t i = 0; i < state.uart1_events.size(); i++)
                    out += strf("[EMU_UART1] %llu %c %02x\n", (unsigned long long)state.uart1_events[i].insn,
                                state.uart1_events[i].dir, state.uart1_events[i].byte);
                state.gpio_dirty.clear();
                state.pwm_dirty.clear();
                state.i2c_dirty.clear();
                state.uart1_events.clear();
                out_write(out);
                out_flush();
            } else if (state.uart_at_line_start && state.last_uart_insn > state.last_periph_sync
                       && state.insn_count - state.last_uart_insn >= 50000) {
                state.last_periph_sync = state.insn_count;
                out_write("[EMU_PERIPH]\n");
                out_flush();
            }
            if (state.insn_count - state.last_emit >= 1000000 && state.uart_at_line_start) {
                state.last_emit = state.insn_count;
                out_write(strf("[EMU_INSNS] %llu\n", (unsigned long long)state.insn_count));
                out_flush();
            }
        }
        if ((state.icu_int_enable & (1u << 9)) && !state.pwmn_seen) {
            std::map<u32, u32>::const_iterator pit = state.pwm_regs.find(0);
            u32 pwm_ctl = pit != state.pwm_regs.end() ? pit->second : 0;
            state.pwm_status |= old_pwm_timer_status_mask(pwm_ctl);
        }
        if ((state.icu_int_enable & (1u << 8)) && chip->is_timer_v1()) {
            if (chip->bktimer_v2)
                state.timer3_5_ctl |= (((state.timer3_5_ctl & 0x7) ? (state.timer3_5_ctl & 0x7) : 0x1) << 7);
            else
                state.timer3_5_ctl |= (1u << 7);
        }
        if (chip->has_audio() && (state.icu_int_enable & (1u << 10)) && (state.audio_fifo_conf & 0x3C)) {
            state.audio_dac_writes = 0;
            state.pending_irqs |= (1u << 10);
        }
        tick_camera_ejpeg();
        if (state.saradc_pending && (state.icu_int_enable & (1u << 11)))
            state.pending_irqs |= (1u << 11);
        if (blecore_) {
            bool core_irq, ble_irq;
            blecore_->tick(state.insn_count, core_irq, ble_irq);
            report_ble_adv();
            state.pending_irqs = (state.pending_irqs & ~((1u << 29) | (1u << 30)))
                                 | (core_irq ? (1u << 29) : 0) | (ble_irq ? (1u << 30) : 0);
        }
        if (wifimac_) {
            if (wifi_peer_)
                wifi_peer_->tick(state.insn_count);
            u32 lines = wifimac_->tick(state.insn_count);
            state.pending_irqs = (state.pending_irqs & ~WIFI_LINE_MASK) | lines;
        }
    }

    if ((state.pwmn_seen ? state.pwmn_stat : (state.pwm_status & 0x3F)))
        state.pending_irqs |= (1u << 9);
    else
        state.pending_irqs &= ~(1u << 9);

    if (chip->is_timer_v2())
        tick_hw_timers(state);
    if ((state.timer0_2_ctl & (0x7u << 7)) || (state.timer3_5_ctl & (0x7u << 7)))
        state.pending_irqs |= (1u << 8);
    else
        state.pending_irqs &= ~(1u << 8);

    u32 ready = state.pending_irqs & state.icu_int_enable;
    if (ready & 0x0000FFFF)
        trigger_irq();
    if (ready & 0xFFFF0000)
        trigger_fiq();

    if (state.insn_count - state.last_fast >= FAST_TICK_INSNS) {
        state.last_fast = state.insn_count;
        if (wifimac_ && wifimac_->busy()) {
            u32 lines = wifimac_->tick(state.insn_count);
            state.pending_irqs = (state.pending_irqs & ~WIFI_LINE_MASK) | lines;
        }
        bool u1, u2;
        uart_irqs_due(u1, u2);
        if (u1) {
            state.pending_irqs |= (1u << 0);
            trigger_irq();
        }
        if (u2) {
            state.pending_irqs |= (1u << 1);
            trigger_irq();
        }
    }

    if (!snapshot_path_.empty()) {
        if (snapshot_due_ || (snapshot_at_insns_valid_ && state.insn_count >= snapshot_at_insns_)) {
            std::string path = snapshot_path_;
            snapshot_path_.clear();
            save_snapshot(path);
        }
    }

    static u64 max_insns = ~(u64)0;
    static bool max_insns_checked = false;
    if (!max_insns_checked) {
        max_insns_checked = true;
        std::string s = env_get("EMU_MAX_INSNS");
        if (!s.empty()) {
            long long v = 0;
            if (parse_int(s, 0, v) && v > 0)
                max_insns = (u64)v;
        }
    }
    if (network_insn_offset_ + state.insn_count >= max_insns) {
        flush_report_periph();
        stop();
    }
}

/* -------------------------------------------------------------- idle task */
void BekenEmulator::find_idle_task()
{
    const u8 *p0 = mem_.fast_ptr(RAM_BASE, 1, false);
    const u8 *pe = mem_.fast_ptr(RAM_BASE + RAM_SIZE - 1, 1, false);
    Bytes ram_copy;
    const u8 *ram = 0;
    if (p0 && pe == p0 + RAM_SIZE - 1) {
        ram = p0;
    } else {
        ram_copy = mem_.read_bytes(RAM_BASE, RAM_SIZE);
        ram = (const u8 *)ram_copy.data();
    }
    const char *names[2] = {"IDLE\x00", "idle\x00"};
    for (int ni = 0; ni < 2; ni++) {
        const char *needle = names[ni];
        const u8 *cur = ram;
        const u8 *end = ram + RAM_SIZE - 5;
        while (cur <= end) {
            const u8 *hit = (const u8 *)memchr(cur, needle[0], (size_t)(end - cur + 1));
            if (!hit)
                break;
            if (memcmp(hit, needle, 5) == 0) {
                long tcb_off = (long)(hit - ram) - 0x34;
                if (tcb_off >= 0 && tcb_off % 4 == 0) {
                    u32 tcb = RAM_BASE + (u32)tcb_off;
                    u32 own1 = rd32le(ram + (size_t)tcb_off + 0x10);
                    u32 own2 = rd32le(ram + (size_t)tcb_off + 0x24);
                    u32 prio = rd32le(ram + (size_t)tcb_off + 0x2C);
                    u32 stack = rd32le(ram + (size_t)tcb_off + 0x30);
                    if (own1 == tcb && own2 == tcb && prio == 0 && RAM_BASE <= stack && stack < tcb
                        && tcb - stack <= 0x4000) {
                        state.idle_task_valid = true;
                        state.idle_task_lo = stack;
                        state.idle_task_hi = tcb;
                        return;
                    }
                }
            }
            cur = hit + 1;
        }
    }
    state.idle_task_valid = false;
}

bool BekenEmulator::idle_step_to_open(u32 &pc, u32 &thumb, u32 cpsr, u32 lo, u32 hi)
{
    u64 real_arm = 0, real_thumb = 0;
    bool ready = false;
    for (u64 t = 0; t < IDLE_STEP_TRIES; t++) {
        if ((cpsr & 0xC0) == 0) {
            ready = true;
            break;
        }
        RunResult rr = cpu_.run(pc | thumb, 0xFFFFFFFF, IDLE_STEP_INSNS);
        if (rr == RUN_ERR_INVALID || rr == RUN_ERR_MEM)
            return false; /* UcError: run_chunked unwinds to run() */
        if (thumb)
            real_thumb += IDLE_STEP_INSNS;
        else
            real_arm += IDLE_STEP_INSNS;
        pc = cpu_.reg_read(REG_PC);
        cpsr = cpu_.reg_read(REG_CPSR);
        thumb = (cpsr & PSR_T) ? 1 : 0;
        u32 mode = cpsr & 0x1F;
        u32 sp = cpu_.reg_read(REG_SP);
        if (!(((mode == 0x1F) || (mode == 0x13)) && lo <= sp && sp < hi))
            break;
    }
    state.insn_count += (u64)(real_arm * CHUNK_SCALE_ARM + real_thumb * CHUNK_SCALE_THUMB);
    state.last_pc = pc;
    return ready;
}

bool BekenEmulator::skip_idle(u64 step)
{
    u32 epoch = exc_epoch_;
    pacing();
    if (atomic_get(&stopping_) || watchdog_reset_pending_ || state.in_deep_sleep)
        return true;
    if (exc_epoch_ != epoch)
        return false;
    u64 left = IDLE_MAX_SKIP;
    while (left > 0) {
        u64 n = 1;
        if (fast_tick_idle()) {
            u64 target = state.last_slow + SLOW_TICK_INSNS;
            u64 timer_due = next_timer_due(state);
            if (timer_due && timer_due < target)
                target = timer_due;
            if (!uart1_rx_.empty() && state.insn_count < uart1_rx_delay_ && uart1_rx_delay_ < target)
                target = uart1_rx_delay_;
            u64 need = target > state.insn_count ? target - state.insn_count : 1;
            n = (need + step - 1) / step;
            if (n < 1)
                n = 1;
            if (n > left)
                n = left;
        }
        state.insn_count += step * n;
        left -= n;
        idle_chunks_skipped_ += n;
        pacing();
        if (atomic_get(&stopping_) || watchdog_reset_pending_ || state.in_deep_sleep)
            return true;
        if (exc_epoch_ != epoch)
            break;
    }
    return false;
}

void BekenEmulator::run_chunked(u32 start)
{
    u64 device_step = chunk_device_insns_;
    u64 chunk_for[2];
    chunk_for[0] = (u64)py_round(device_step / CHUNK_SCALE_ARM);
    if (chunk_for[0] < 1)
        chunk_for[0] = 1;
    chunk_for[1] = (u64)py_round(device_step / CHUNK_SCALE_THUMB);
    if (chunk_for[1] < 1)
        chunk_for[1] = 1;
    u64 step_for[2][2];
    double scales[2] = {CHUNK_SCALE_ARM, CHUNK_SCALE_THUMB};
    for (int entry = 0; entry < 2; entry++)
        for (int e2 = 0; e2 < 2; e2++) {
            step_for[entry][e2] = (u64)(chunk_for[entry] * scales[e2]);
            if (step_for[entry][e2] < 1)
                step_for[entry][e2] = 1;
        }

    u32 pc = start & ~1u;
    u32 thumb = start & 1;
    bool skip = idle_skip_;
    u64 grid = state.insn_count / IDLE_SCAN_INSNS;
    u64 streak = 0;
    u64 masked_streak = 0;

    static int phase_timing = -1;
    if (phase_timing < 0)
        phase_timing = env_has("EMU_PHASE_TIMING") ? 1 : 0;
    for (;;) {
        double t0 = phase_timing ? mono_time() : 0.0;
        RunResult rr = cpu_.run(pc | thumb, 0xFFFFFFFF, chunk_for[thumb]);
        /* A CPU fault is Python's UcError out of emu_start: it propagates
         * straight to run() - no clock step, no pacing, no more chunks. */
        if (rr == RUN_ERR_INVALID || rr == RUN_ERR_MEM)
            return;
        double t1 = phase_timing ? mono_time() : 0.0;
        chunks_run_++;
        pc = cpu_.reg_read(REG_PC);
        u32 cpsr = cpu_.reg_read(REG_CPSR);
        u32 was_thumb = thumb;
        thumb = (cpsr & PSR_T) ? 1 : 0;
        state.insn_count += step_for[was_thumb][thumb];
        state.last_pc = pc;
        ensure_vector_page();
        u32 epoch = exc_epoch_;
        bool armed = !snapshot_path_.empty();
        pacing();
        if (phase_timing) {
            g_cpu_secs += t1 - t0;
            g_pace_secs += mono_time() - t1;
        }
        if (atomic_get(&stopping_) || watchdog_reset_pending_ || state.in_deep_sleep)
            break;
        if (exc_epoch_ != epoch) {
            pc = cpu_.reg_read(REG_PC);
            thumb = (cpu_.reg_read(REG_CPSR) & PSR_T) ? 1 : 0;
            streak = 0;
            masked_streak = 0;
        } else if (skip && armed) {
            streak = 0;
            masked_streak = 0;
            grid = state.insn_count / IDLE_SCAN_INSNS;
        } else if (skip) {
            if (!state.idle_task_valid) {
                if (state.insn_count / IDLE_SCAN_INSNS != grid) {
                    grid = state.insn_count / IDLE_SCAN_INSNS;
                    find_idle_task();
                }
                if (chip->rtthread_console && (cpsr & 0x80) && (state.icu_global_int_en & 1)) {
                    masked_streak++;
                    if (masked_streak >= 8) {
                        masked_streak = 0;
                        RunResult mr = cpu_.run(pc | thumb, 0xFFFFFFFF, 31);
                        if (mr == RUN_ERR_INVALID || mr == RUN_ERR_MEM)
                            return;
                        pc = cpu_.reg_read(REG_PC);
                        cpsr = cpu_.reg_read(REG_CPSR);
                        thumb = (cpsr & PSR_T) ? 1 : 0;
                    }
                } else {
                    masked_streak = 0;
                }
            } else {
                u32 mode = cpsr & 0x1F;
                u32 sp = cpu_.reg_read(REG_SP);
                if (((mode == 0x1F) || (mode == 0x13)) && (state.icu_global_int_en & 1)
                    && state.idle_task_lo <= sp && sp < state.idle_task_hi) {
                    streak++;
                    if (streak >= IDLE_STREAK) {
                        streak = 0;
                        u32 ipc = pc, ithumb = thumb;
                        bool ready = idle_step_to_open(ipc, ithumb, cpsr, state.idle_task_lo, state.idle_task_hi);
                        if (!cpu_.error().empty())
                            return; /* faulted while stepping: UcError */
                        pc = ipc;
                        thumb = ithumb;
                        if (ready) {
                            if (skip_idle(step_for[thumb][thumb]))
                                break;
                            pc = cpu_.reg_read(REG_PC);
                            thumb = (cpu_.reg_read(REG_CPSR) & PSR_T) ? 1 : 0;
                        }
                    }
                } else {
                    streak = 0;
                }
            }
        }
        if (pc == 0xFFFFFFFF)
            break;
    }
}

/* EMU_BLOCK_PACING=1: Python's legacy UC_HOOK_BLOCK pacing (hook_block). The
 * device clock advances by the block's byte size / 4 (at least 1) and the
 * periodic work runs once per block instead of once per chunk; no idle skip.
 * Unicorn's translation blocks are approximated: a block ends at a non-
 * sequential PC (branch, exception, IRQ entry), a change of the CPSR's
 * mode/T/I/F bits, a 4 KB page boundary or 512 instructions (TCG_MAX_INSNS).
 * Python paces at the START of each block, already counting it (Unicorn knows
 * the translated size up front); here a block's accounting and pacing follow
 * its execution, at the next block's start, so the clock at a boundary lags
 * Python's by one block. Unicorn's exact block cuts cannot be reproduced, so
 * this mode is close to, not byte-identical with, Python's. */
void BekenEmulator::run_block_paced(u32 start)
{
    static const u64 EMU_START_COUNT = 1000000000ULL; /* run()'s emu_start(count=1e9) */
    u32 pc = start & ~1u;
    u32 thumb = start & 1;
    u64 executed = 0;
    for (;;) {
        u32 block_pc = pc;
        u32 page = pc & ~0xFFFu;
        u32 bytes = 0, insns = 0;
        for (;;) {
            u32 cpsr0 = cpu_.reg_read(REG_CPSR);
            u32 len = 4;
            if (thumb) {
                bool ok = false;
                u16 hw = cpu_.mem.fetch16(pc, ok);
                len = (ok && (hw >> 11) >= 0x1D) ? 4 : 2;
            }
            RunResult rr = cpu_.run(pc | thumb, 0xFFFFFFFF, 1);
            if (rr == RUN_ERR_INVALID || rr == RUN_ERR_MEM)
                return; /* UcError */
            executed++;
            bytes += len;
            insns++;
            u32 npc = cpu_.reg_read(REG_PC);
            u32 cpsr = cpu_.reg_read(REG_CPSR);
            thumb = (cpsr & PSR_T) ? 1 : 0;
            bool seq = npc == pc + len && (cpsr & 0xFF) == (cpsr0 & 0xFF);
            pc = npc;
            if (!seq || (npc & ~0xFFFu) != page || insns >= 512)
                break;
            /* an MMIO write that stopped the CPU (deep sleep, watchdog) */
            if (state.in_deep_sleep || watchdog_reset_pending_ || atomic_get(&stopping_)
                || executed >= EMU_START_COUNT)
                break;
        }
        state.insn_count += (bytes >> 2) ? (bytes >> 2) : 1;
        state.last_pc = block_pc;
        if (atomic_get(&stopping_))
            return;
        if (!state.in_deep_sleep && !watchdog_reset_pending_)
            pacing();
        if (atomic_get(&stopping_) || watchdog_reset_pending_ || state.in_deep_sleep
            || executed >= EMU_START_COUNT)
            return;
        /* pacing may have taken an IRQ/FIQ: carry on from wherever PC is */
        pc = cpu_.reg_read(REG_PC);
        thumb = (cpu_.reg_read(REG_CPSR) & PSR_T) ? 1 : 0;
        if (pc == 0xFFFFFFFF)
            return;
    }
}

/* -------------------------------------------------------------- vectors */
u32 BekenEmulator::cold_vector_base()
{
    if (opt_.with_boot)
        return chip->bootloader_addr;
    u32 app_addr = chip->app_addr;
    u32 vector_base = app_addr;
    u32 val = mem_.phys_read(vector_base, 4);
    if ((val & 0xFE000000u) != 0xEA000000u && (val & 0xFFFFF000u) != 0xE59FF000u) {
        u32 val100 = mem_.phys_read(app_addr + 0x100, 4);
        if ((val100 & 0xFE000000u) == 0xEA000000u || (val100 & 0xFFFFF000u) == 0xE59FF000u)
            vector_base = app_addr + 0x100;
        else if (val == 0xFFFFFFFFu || val == 0x94b5072fu)
            vector_base = app_addr + 0x1000;
    }
    return vector_base;
}

/* -------------------------------------------------------------- setup/run */
static Bytes patch_ldm_caret_stubs(const Bytes &img)
{
    Bytes head = bhead(img, 0x800);
    const std::string pat("\x03\x00\xfd\xe8", 4);
    if (head.find(pat) == std::string::npos)
        return img;
    Bytes buf = img;
    u32 lim = (u32)(buf.size() < 0x800 ? buf.size() : 0x800);
    for (u32 off = 0; off + 4 <= lim; off += 4)
        if (buf.compare(off, 4, pat) == 0)
            buf[off + 2] = (char)0xbd;
    return buf;
}

void BekenEmulator::setup()
{
    cpu_.vector_base = exception_vector_base_;
    cpu_.on_swi = &BekenEmulator::on_swi_cb;
    cpu_.watch_ctx = this;
    mem_.map(FLASH_BASE, FLASH_SIZE);
    mem_.map(RAM_BASE, RAM_SIZE);
    mem_.map(MMIO_BASE, MMIO_SIZE);
    mem_.map(PERIPH_BASE, PERIPH_SIZE);

    mem_.write_bytes(FLASH_BASE, bhead(raw_flash_, FLASH_SIZE));
    if (!bootloader_.empty())
        mem_.write_bytes(chip->bootloader_addr, patch_ldm_caret_stubs(bootloader_));
    if (!app_.empty())
        mem_.write_bytes(chip->app_addr, patch_ldm_caret_stubs(app_));

    mem_.phys_write(0x00802108, 4, (1u << 17) | (1u << 19) | (1u << 20)); /* UART1 status */
    mem_.phys_write(0x00802208, 4, (1u << 17) | (1u << 19) | (1u << 20)); /* UART2 status */
    mem_.phys_write(0x00802c00, 4, (1u << 28) | (1u << 30));              /* SARADC */
    mem_.phys_write(0x00802900, 4, 0);                                   /* WDT */

    u32 spi_size = (u32)((raw_flash_.size() + 0xFFF) & ~0xFFFu);
    if (spi_size > RAM_BASE - SPI_FLASH_BASE)
        spi_size = RAM_BASE - SPI_FLASH_BASE;
    mem_.map(SPI_FLASH_BASE, spi_size);
    mem_.write_bytes(SPI_FLASH_BASE, bhead(raw_flash_, spi_size));

    if (chip->sdram_size)
        mem_.map(chip->sdram_base, chip->sdram_size);
    else if (chip->xvr)
        mem_.map(XVR_BASE, 0x1000);
    if (chip->is_ejpeg_v2())
        mem_.map(0x00A00000, 0x10000);
    mem_.map(ACCEL_BASE, 0x1000);

    /* MMIO hooks */
    mem_.add_read_hook(MMIO_BASE, MMIO_BASE + MMIO_SIZE);
    mem_.add_write_hook(MMIO_BASE, MMIO_BASE + MMIO_SIZE);
    mem_.add_read_hook(PERIPH_BASE, PERIPH_BASE + PERIPH_SIZE);
    mem_.add_write_hook(PERIPH_BASE, PERIPH_BASE + PERIPH_SIZE);
    if (!chip->sdram_size && chip->xvr) {
        mem_.add_read_hook(XVR_BASE, XVR_BASE + 0x1000);
        mem_.add_write_hook(XVR_BASE, XVR_BASE + 0x1000);
    }
    if (chip->is_ejpeg_v2()) {
        mem_.add_read_hook(0x00A00000, 0x00A10000);
        mem_.add_write_hook(0x00A00000, 0x00A10000);
    }
    mem_.add_read_hook(ACCEL_BASE, ACCEL_BASE + 0x1000);
    mem_.add_write_hook(ACCEL_BASE, ACCEL_BASE + 0x1000);

    if (!uart1_rx_.empty() && chip->rtthread_console && !tuyamcu_) {
        u32 scan_len = FLASH_SIZE - 0x10000 < 0x180000 ? FLASH_SIZE - 0x10000 : 0x180000;
        Bytes app_mem = mem_.read_bytes(0x10000, scan_len);
        const std::string tshell_pat("tshell\x00", 7);
        const std::string nouart_pat("no_uart\x00", 8);
        const std::string uart1_pat("uart1\x00\x00\x00", 8);
        if (app_mem.find(tshell_pat) != std::string::npos
            && app_mem.find(nouart_pat) != std::string::npos) {
            size_t idx = app_mem.find(nouart_pat);
            while (idx != std::string::npos) {
                mem_.write_bytes((u32)(0x10000 + idx), uart1_pat);
                idx = app_mem.find(nouart_pat, idx + 1);
            }
        }
    }
    flash_page0_ = mem_.read_bytes(0, 0x800);
    flash_page0_word0_ = mem_.phys_read(0, 4);

    cpu_.context_save(power_on_ctx_);
    have_power_on_ctx_ = true;
}

void BekenEmulator::run()
{
    u32 vector_base;
    if (have_resume_at_) {
        vector_base = resume_at_;
        print_line(strf("Resuming snapshot at: 0x%08x", vector_base));
    } else {
        vector_base = cold_vector_base();
        if (opt_.with_boot)
            print_line(strf("Booting from bootloader at: 0x%08x", vector_base));
        else
            print_line(strf("Detected app vector table at: 0x%08x", vector_base));
    }
    if (!app_.empty())
        print_line("App header: " + hexstr(bhead(app_, 32)));
    print_line(strf("Starting emulation at 0x%08x...", vector_base));

    double start_time = mono_time();
    g_sigint_hit = 0;
    g_sigint_emu = this;
    void (*prev_sigint)(int) = signal(SIGINT, on_sigint);
    try {
    for (;;) {
        cpu_.clear_stop();
        if (block_pacing_) {
            run_block_paced(vector_base);
        } else {
            run_chunked(vector_base);
        }
        if (!cpu_.error().empty())
            break; /* Python: the UcError leaves run()'s loop at once */
        if (atomic_get(&stopping_))
            break;
        if (state.in_deep_sleep) {
            if (!wait_in_deep_sleep())
                break; /* stopped while parked, or no wake source */
            vector_base = cold_vector_base();
            print_line(strf("Restarting emulation after deep sleep at 0x%08x...", vector_base));
            continue;
        }
        if (!watchdog_reset_pending_)
            break;
        watchdog_reset();
        if (atomic_get(&stopping_))
            break;
        vector_base = cold_vector_base();
        print_line(strf("Restarting emulation at 0x%08x...", vector_base));
    }
    } catch (const std::exception &e) {
        /* Python's `except Exception` arm: a model raised (e.g. GpioModel's
         * ValueError for a pin out of range) - report it and finish normally. */
        u32 epc = cpu_.reg_read(REG_PC);
        u32 ecpsr = cpu_.reg_read(REG_CPSR);
        const char *kind = dynamic_cast<const std::invalid_argument *>(&e) ? "ValueError" : "RuntimeError";
        run_error = strf("%s: %s (pc 0x%08x)", kind, e.what(), epc);
        has_run_error = true;
        out_write(strf("\n[!] Emulation finished with python error: %s. PC: 0x%08x, CPSR: 0x%08x\n",
                       e.what(), epc, ecpsr));
    }

    signal(SIGINT, prev_sigint == SIG_ERR ? SIG_DFL : prev_sigint);
    g_sigint_emu = 0;
    if (g_sigint_hit) {
        g_sigint_hit = 0;
        out_write("\nEmulation stopped by user.\n");
    }
    publish_ui_view();
    u32 pc = cpu_.reg_read(REG_PC);
    u32 cpsr = cpu_.reg_read(REG_CPSR);
    double elapsed = mono_time() - start_time;
    if (!cpu_.error().empty() && !has_run_error) {
        run_error = strf("%s at pc 0x%08x", cpu_.error().c_str(), pc);
        has_run_error = true;
        out_write(strf("\n[!] Emulation finished with error: %s. PC: 0x%08x, CPSR: 0x%08x\n",
                       cpu_.error().c_str(), pc, cpsr));
    }
    out_write(strf("Emulation finished. PC: 0x%08x, CPSR: 0x%08x (T-bit: %u)\n", pc, cpsr, (cpsr & 0x20) >> 5));
    out_write(strf("Time taken: %.2f seconds.\n", elapsed));
    if (env_has("EMU_PHASE_TIMING")) {
        double tot = g_cpu_secs + g_pace_secs;
        fprintf(stderr, "[PHASE] cpu=%.3fs (%.1f%%) pacing=%.3fs (%.1f%%) chunks=%llu\n",
                g_cpu_secs, tot > 0 ? 100.0 * g_cpu_secs / tot : 0.0,
                g_pace_secs, tot > 0 ? 100.0 * g_pace_secs / tot : 0.0,
                (unsigned long long)chunks_run_);
    }
    out_write("\nDone.\n");
    out_flush();
}

void BekenEmulator::watchdog_reset()
{
    std::map<int, int> gpio_inputs = state.gpio_hw.inputs;
    network_insn_offset_ += state.insn_count;
    mem_.fill(RAM_BASE, 0, RAM_SIZE);
    mem_.fill(MMIO_BASE, 0, MMIO_SIZE);
    mem_.fill(PERIPH_BASE, 0, PERIPH_SIZE);
    if (chip->sdram_size)
        mem_.fill(chip->sdram_base, 0, chip->sdram_size);
    else if (chip->xvr)
        mem_.fill(XVR_BASE, 0, 0x1000);
    if (chip->is_ejpeg_v2())
        mem_.fill(0x00A00000, 0, 0x10000);
    mem_.fill(ACCEL_BASE, 0, 0x1000);
    mem_.phys_write(0x00802108, 4, (1u << 17) | (1u << 19) | (1u << 20));
    mem_.phys_write(0x00802208, 4, (1u << 17) | (1u << 19) | (1u << 20));
    mem_.phys_write(0x00802c00, 4, (1u << 28) | (1u << 30));
    mem_.phys_write(0x00802900, 4, 0);
    cpu_.context_restore(power_on_ctx_);

    int pin_count = gpio_pin_count;
    state = SimState();
    if (chip->is_i2c_v2())
        state.hw_i2c[1].v2 = true;
    if (pin_count != state.gpio_hw.pin_count)
        state.gpio_hw = GpioModel(pin_count);
    state.gpio_hw.inputs = gpio_inputs;
    state.adc_samples = adc_inputs;
    flash_state_ = FlashState();
    uart_src_ = 0;
    have_resume_at_ = false;
    ble_adv_seen_.clear();
    watchdog_reset_pending_ = false;
    has_run_error = false;
    run_error.clear();
    if (wifimac_)
        trng_state_ = 0x2545F491;
    reset_peripheral_models();
    reset_count++;
    exc_epoch_++;
    net_log(strf("[WDT] watchdog expired; device reset #%llu", (unsigned long long)reset_count));
}

/* --------------------------------------------------------------- deep sleep */

/* Parse a --deep-sleep-wake spec string into (reason, pin). pin=-1 means None. */
static bool parse_wake_spec_str(const std::string &spec, std::string &reason, int &pin)
{
    std::string text = lower(strip(spec.empty() ? std::string("auto") : spec));
    pin = -1;
    if (text.size() > 5 && text.substr(0, 5) == "gpio:") {
        reason = "gpio";
        long long v;
        if (!parse_int(text.substr(5), 0, v))
            return false;
        pin = (int)v;
        return true;
    }
    if (text == "rtc" || text == "gpio" || text == "auto") {
        reason = text;
        return true;
    }
    return false;
}

bool BekenEmulator::resolve_wake_spec(const std::string &reason_in, int pin_in,
                                      const DeepSleepInfo &info,
                                      std::string &reason_out, int &pin_out)
{
    std::string reason = reason_in;
    int pin = pin_in;
    const std::vector<std::pair<int, int> > &wake_pins = info.wake_pins;
    if (reason == "auto") {
        if (info.rtc_enabled) {
            reason_out = "rtc";
            pin_out = -1;
            return true;
        }
        if (!wake_pins.empty()) {
            reason_out = "gpio";
            pin_out = wake_pins[0].first;
            return true;
        }
        reason_out = "rtc";
        pin_out = -1;
        return true;
    }
    if (reason == "gpio" && pin < 0)
        pin = wake_pins.empty() ? 0 : wake_pins[0].first;
    reason_out = reason;
    pin_out = pin;
    return true;
}

/* Python's round(x, 1): correctly rounded to one decimal of x's exact binary
 * value (round-half-even only on exact ties), not round(x * 10) / 10 - which
 * rounds twice. printf's %.1f does exactly that conversion. */
static double py_round1(double x)
{
    return strtod(strf("%.1f", x).c_str(), 0);
}

DeepSleepInfo BekenEmulator::decode_deep_sleep_info(SimState &st, u32 sleep_reg_val)
{
    DeepSleepInfo info;
    info.valid = true;
    info.sleep_reg = sleep_reg_val;
    bool rosc_en = (st.sctrl_rosc_timer & 1) != 0;
    u32 ticks = (((st.sctrl_rosc_timer_h & 0xFFFF) << 16)
                 | ((st.sctrl_rosc_timer >> 16) & 0xFFFF));
    info.rtc_ticks = ticks;
    if (!rosc_en || ticks == 0) {
        info.rtc_seconds_valid = false;
    } else if (ticks % 32000 == 0) {
        info.rtc_seconds_valid = true;
        info.rtc_seconds = ticks / 32000.0;
    } else if (ticks % 32768 == 0) {
        info.rtc_seconds_valid = true;
        info.rtc_seconds = ticks / 32768.0;
    } else if ((st.sctrl_rosc_timer_h & 0xFFFF) == 0 && ticks < 3200) {
        info.rtc_seconds_valid = true;
        info.rtc_seconds = py_round1(ticks / 32.7);
    } else {
        info.rtc_seconds_valid = true;
        info.rtc_seconds = py_round1(ticks / 32768.0);
    }
    info.rtc_enabled = rosc_en && ticks > 0;

    u64 wake_mask = ((u64)(st.sctrl_gpio_wakeup_en & 0xFFFFFFFFu)
                     | ((u64)(st.sctrl_gpio_wakeup_en1 & 0xFF) << 32));
    u64 wake_type = ((u64)(st.sctrl_gpio_wakeup_type & 0xFFFFFFFFu)
                     | ((u64)(st.sctrl_gpio_wakeup_type1 & 0xFF) << 32));
    info.wake_gpio_mask = wake_mask;
    info.wake_gpio_type = wake_type;
    for (int p = 0; p < gpio_pin_count; p++) {
        if ((wake_mask >> p) & 1) {
            int active_lv = ((wake_type >> p) & 1) ? 0 : 1;
            info.wake_pins.push_back(std::make_pair(p, active_lv));
        }
    }
    info.insn_count = st.insn_count;
    return info;
}

void BekenEmulator::enter_deep_sleep(u32 sleep_reg_val)
{
    if (state.in_deep_sleep)
        return;
    flush_report_periph();
    state.in_deep_sleep = true;
    state.deep_sleep_count++;
    DeepSleepInfo info = decode_deep_sleep_info(state, sleep_reg_val);
    state.deep_sleep_info = info;
    state.deep_sleep_rtc_sec = info.rtc_seconds_valid ? info.rtc_seconds : 0.0;
    state.deep_sleep_wake_pins.clear();
    for (size_t i = 0; i < info.wake_pins.size(); i++)
        state.deep_sleep_wake_pins[info.wake_pins[i].first] = info.wake_pins[i].second;

    std::vector<std::string> parts;
    if (info.rtc_enabled && info.rtc_seconds_valid)
        parts.push_back(strf("RTC=%gs", info.rtc_seconds));
    if (!info.wake_pins.empty()) {
        std::string pins_str;
        for (size_t i = 0; i < info.wake_pins.size(); i++) {
            if (i)
                pins_str += ", ";
            pins_str += strf("P%d:%s", info.wake_pins[i].first,
                             info.wake_pins[i].second ? "HIGH" : "LOW");
        }
        parts.push_back("GPIO=[" + pins_str + "]");
    }
    std::string summary;
    if (parts.empty())
        summary = "no wake source armed";
    else
        for (size_t i = 0; i < parts.size(); i++)
            summary += (i ? ", " : "") + parts[i];
    net_log("[SLEEP] entered deep sleep (" + summary + ")");

    if (!deep_sleep_wake_queue_.empty()) {
        std::string spec = deep_sleep_wake_queue_.front();
        deep_sleep_wake_queue_.erase(deep_sleep_wake_queue_.begin());
        std::string reason;
        int pin;
        if (parse_wake_spec_str(spec, reason, pin)) {
            std::string r;
            int p;
            resolve_wake_spec(reason, pin, info, r, p);
            deep_sleep_wake_pending_valid_ = true;
            deep_sleep_wake_pending_reason_ = r;
            deep_sleep_wake_pending_pin_ = p;
            /* Python's wake_from_deep_sleep stores straight into the same
             * pending slot this assignment overwrites: a GUI request made
             * before this sleep began is superseded by the queued spec. */
            ScopedLock lk(gpio_lock_);
            ui_wake_req_valid_ = false;
        }
    }
    cpu_.request_stop();
}

bool BekenEmulator::wait_in_deep_sleep()
{
    publish_ui_view();
    while (state.in_deep_sleep && !atomic_get(&stopping_)) {
        if (ui_view_req_)
            publish_ui_view();
        {
            ScopedLock lk(gpio_lock_);
            if (ui_wake_req_valid_) {
                ui_wake_req_valid_ = false;
                DeepSleepInfo info = state.deep_sleep_info.valid
                                         ? state.deep_sleep_info
                                         : decode_deep_sleep_info(state, 0xADC1);
                std::string r;
                int p;
                resolve_wake_spec(ui_wake_req_reason_, ui_wake_req_pin_, info, r, p);
                deep_sleep_wake_pending_valid_ = true;
                deep_sleep_wake_pending_reason_ = r;
                deep_sleep_wake_pending_pin_ = p;
            }
            if (!gpio_events_.empty()) {
                std::vector<std::pair<int, int> > events;
                events.swap(gpio_events_);
                for (size_t i = 0; i < events.size(); i++) {
                    int pin = events[i].first, level = events[i].second;
                    state.gpio_hw.drive(pin, level, gpio_config_word(pin));
                    std::map<int, int>::iterator it = state.deep_sleep_wake_pins.find(pin);
                    if (it != state.deep_sleep_wake_pins.end() && level >= 0 && level == it->second) {
                        deep_sleep_wake_pending_valid_ = true;
                        deep_sleep_wake_pending_reason_ = "gpio";
                        deep_sleep_wake_pending_pin_ = pin;
                    }
                }
            }
        }
        if (deep_sleep_wake_pending_valid_) {
            std::string reason = deep_sleep_wake_pending_reason_;
            int pin = deep_sleep_wake_pending_pin_;
            deep_sleep_wake_pending_valid_ = false;
            deep_sleep_reset(reason, pin);
            return true;
        }
        sleep_ms(10);
    }
    return false;
}

void BekenEmulator::deep_sleep_reset(const std::string &reason_in, int pin_in)
{
    if (!have_power_on_ctx_)
        return;
    std::map<int, int> gpio_inputs = state.gpio_hw.inputs;
    DeepSleepInfo info = state.deep_sleep_info.valid
                             ? state.deep_sleep_info
                             : decode_deep_sleep_info(state, 0xADC1);
    std::string reason;
    int pin;
    resolve_wake_spec(reason_in, pin_in, info, reason, pin);
    network_insn_offset_ += state.insn_count;

    mem_.fill(RAM_BASE, 0, RAM_SIZE);
    mem_.fill(MMIO_BASE, 0, MMIO_SIZE);
    mem_.fill(PERIPH_BASE, 0, PERIPH_SIZE);
    if (chip->sdram_size)
        mem_.fill(chip->sdram_base, 0, chip->sdram_size);
    else if (chip->xvr)
        mem_.fill(XVR_BASE, 0, 0x1000);
    if (chip->is_ejpeg_v2())
        mem_.fill(0x00A00000, 0, 0x10000);
    mem_.fill(ACCEL_BASE, 0, 0x1000);
    mem_.phys_write(0x00802108, 4, (1u << 17) | (1u << 19) | (1u << 20));
    mem_.phys_write(0x00802208, 4, (1u << 17) | (1u << 19) | (1u << 20));
    mem_.phys_write(0x00802c00, 4, (1u << 28) | (1u << 30));
    mem_.phys_write(0x00802900, 4, 0);
    cpu_.context_restore(power_on_ctx_);

    int pin_count = gpio_pin_count;
    SimState ns;
    if (chip->is_i2c_v2())
        ns.hw_i2c[1].v2 = true;
    if (pin_count != ns.gpio_hw.pin_count)
        ns.gpio_hw = GpioModel(pin_count);
    ns.gpio_hw.inputs = gpio_inputs;
    ns.adc_samples = adc_inputs;

    /* Always-On (AON) SCTRL wakeup configuration survives the deep sleep reset. */
    ns.sctrl_rosc_timer = state.sctrl_rosc_timer;
    ns.sctrl_rosc_timer_h = state.sctrl_rosc_timer_h;
    ns.sctrl_gpio_wakeup_en = state.sctrl_gpio_wakeup_en;
    ns.sctrl_gpio_wakeup_type = state.sctrl_gpio_wakeup_type;
    ns.sctrl_gpio_wakeup_en1 = state.sctrl_gpio_wakeup_en1;
    ns.sctrl_gpio_wakeup_type1 = state.sctrl_gpio_wakeup_type1;
    ns.sctrl_reset_retention = state.sctrl_reset_retention;
    ns.deep_sleep_count = state.deep_sleep_count;
    ns.deep_sleep_info = info;
    ns.deep_sleep_rtc_sec = info.rtc_seconds_valid ? info.rtc_seconds : 0.0;
    for (size_t i = 0; i < info.wake_pins.size(); i++)
        ns.deep_sleep_wake_pins[info.wake_pins[i].first] = info.wake_pins[i].second;

    std::string wake_desc;
    if (reason == "rtc") {
        ns.sctrl_rosc_int_status = true;
        ns.last_wake_reason = "rtc";
        ns.last_wake_pin = -1;
        wake_desc = "RTC timer";
    } else {
        if (pin < 0)
            pin = 0;
        u64 wake_type = info.wake_gpio_type;
        int active_lv = ((wake_type >> pin) & 1) ? 0 : 1;
        if (pin < 32) {
            ns.sctrl_gpio_wakeup_int_status = 1u << pin;
            ns.aon_gpio_wakeup_status = 1u << pin;
        } else {
            ns.sctrl_gpio_wakeup_int_status1 = 1u << (pin - 32);
        }
        bool have_prev = gpio_inputs.count(pin) != 0;
        int prev_lv = have_prev ? gpio_inputs[pin] : -1;
        ns.gpio_hw.inputs[pin] = active_lv;
        ns.wake_pulse_pin = pin;
        ns.wake_pulse_restore = (have_prev && prev_lv != active_lv) ? prev_lv : (1 - active_lv);
        ns.wake_pulse_reads_left = 4;
        ns.wake_reg_sampled = false;
        ns.last_wake_reason = "gpio";
        ns.last_wake_pin = pin;
        wake_desc = strf("GPIO P%d=%d", pin, active_lv);
    }

    /* Mirror retained SCTRL words into mapped MMIO memory as well. */
    mem_.phys_write(0x0080011c, 4,
                    (ns.sctrl_rosc_timer & ~0x100u) | (ns.sctrl_rosc_int_status ? 0x100 : 0));
    mem_.phys_write(0x00800120, 4, ns.sctrl_gpio_wakeup_en);
    mem_.phys_write(0x00800124, 4, ns.sctrl_gpio_wakeup_type);
    mem_.phys_write(0x00800128, 4, ns.sctrl_gpio_wakeup_int_status);
    mem_.phys_write(0x00800140, 4, ns.sctrl_rosc_timer_h);
    mem_.phys_write(0x00800144, 4, ns.sctrl_gpio_wakeup_en1);
    mem_.phys_write(0x00800148, 4, ns.sctrl_gpio_wakeup_type1);
    mem_.phys_write(0x0080014c, 4, ns.sctrl_gpio_wakeup_int_status1);
    mem_.phys_write(0x00808004, 4, ns.aon_gpio_wakeup_status);

    state = ns;
    flash_state_ = FlashState();
    uart_src_ = 0;
    have_resume_at_ = false;
    ble_adv_seen_.clear();
    watchdog_reset_pending_ = false;
    has_run_error = false;
    run_error.clear();
    if (wifimac_)
        trng_state_ = 0x2545F491;
    reset_peripheral_models();
    reset_count++;
    exc_epoch_++;
    net_log(strf("[SLEEP] waking from deep sleep (%s); device reset #%llu",
                 wake_desc.c_str(), (unsigned long long)reset_count));
}

/* -------------------------------------------------------------- snapshots */
void BekenEmulator::arm_snapshot(const std::string &path, bool at_is_insns, u64 at_insns, const std::string &marker)
{
    snapshot_path_ = path;
    if (at_is_insns) {
        snapshot_at_insns_valid_ = true;
        snapshot_at_insns_ = at_insns;
        snapshot_marker_.clear();
    } else {
        snapshot_at_insns_valid_ = false;
        snapshot_marker_ = marker;
        uart_tail_.clear();
    }
    snapshot_due_ = false;
}

void BekenEmulator::snapshot_check_uart(const Bytes &data)
{
    Bytes tail = uart_tail_ + data;
    if (!snapshot_marker_.empty() && tail.find(snapshot_marker_) != std::string::npos) {
        snapshot_due_ = true;
        snapshot_marker_.clear();
    }
    size_t keep = snapshot_marker_.empty() ? 0 : snapshot_marker_.size() + 1;
    uart_tail_ = keep && tail.size() > keep ? tail.substr(tail.size() - keep) : tail;
}
