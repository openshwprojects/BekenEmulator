/* MMIO read handler and the unmapped-access hook. Port of hook_mem_read_mmio
 * and hook_unmapped from emulator.py. */
#include "emulator.h"
#include "blecore.h"
#include "wifimac.h"

#include <string.h>

static const u32 ICU_INT_STATUS = 0x0080204c;
static const u32 ICU_INT_RAW_STATUS = 0x00802048;
static const u32 ICU_INT_ENABLE = 0x00802040;
static const u32 ICU_GLOBAL_INT_EN = 0x00802044;
static const u32 UART1_FIFO_PORT = 0x0080210c;
static const u32 UART1_FIFO_STATUS = 0x00802108;
static const u32 UART2_FIFO_PORT = 0x0080220c;
static const u32 UART2_FIFO_STATUS = 0x00802208;
static const u32 TRNG_DATA = 0x00802484;
static const u32 GPIO_BASE = 0x00802800;
static const u32 GDMA_BASE = 0x00809000;
static const u32 GDMA_END = GDMA_BASE + 0x240;
static const u32 PWM_BASE = 0x00802A00;
static const u32 PWMN_BASE = 0x00802B00;
static const u32 PWMN_INT_STAT = (1u << 30) | (1u << 31);
static const u32 PWMN_CFG_UPDATA = (1u << 7) | (1u << 15);
static const u32 GPIO_FUNC_CFG = GPIO_BASE + 32 * 4;
static const u32 GPIO_INTEN = GPIO_BASE + 33 * 4;
static const u32 GPIO_INTLV0 = GPIO_BASE + 34 * 4;
static const u32 GPIO_INTLV1 = GPIO_BASE + 35 * 4;
static const u32 GPIO_INTSTA = GPIO_BASE + 36 * 4;
static const u32 GPIO_INTEN2 = GPIO_BASE + 56 * 4;
static const u32 GPIO_INTLV3 = GPIO_BASE + 57 * 4;
static const u32 GPIO_INTSTA2 = GPIO_BASE + 58 * 4;

#define W(addr, v) mem_.phys_write((addr), 4, (v))

void BekenEmulator::mmio_read(u32 address, int size)
{
    if (address == UART1_FIFO_STATUS || address == UART2_FIFO_STATUS) {
        u32 status = (1u << 17) | (1u << 20);
        bool rxp = (address == UART1_FIFO_STATUS && rx_active())
                   || (address == UART2_FIFO_STATUS && rx_active_uart2());
        if (rxp)
            status |= (1u << 21);
        else
            status |= (1u << 19);
        W(address, status);
        return;
    }
    if (address == ICU_INT_STATUS || address == ICU_INT_RAW_STATUS) {
        W(address, state.pending_irqs & state.icu_int_enable);
        return;
    }
    if (address == 0x00803008) {
        u32 word_addr = flash_state_.addr + (u32)flash_state_.read_idx * 4;
        flash_state_.read_idx++;
        const Bytes &src = have_physical_ ? physical_flash_ : raw_flash_;
        u32 val = (word_addr + 4 <= src.size()) ? g32le(src, word_addr) : 0xFFFFFFFFu;
        W(address, val);
        return;
    }
    if (address == 0x00803004) {
        mem_.write_bytes(address, flash_state_.data);
        return;
    }
    if (wifimac_ && WIFI_CORE <= address && address < WIFI_CORE + WIFI_SIZE) {
        bool has;
        u32 served = wifimac_->read(address, state.insn_count, has);
        if (has)
            W(address, served);
        return;
    }
    if (0x00802300 <= address && address < 0x0080230C) {
        /* CONFIG (+0x00) and STA (+0x04) are full 32-bit registers; only the
         * DAT register is a byte, and hw_i2c_read already masks that one. */
        W(address, hw_i2c_read(1, address - 0x00802300));
        return;
    }
    if (0x00802600 <= address && address < 0x0080260C) {
        W(address, hw_i2c_read(2, address - 0x00802600));
        return;
    }
    if (0x00802700 <= address && address < 0x00802710) {
        if (address == 0x00802708) {
            u32 stat = 0x02 | 0x2000 | (state.spi_rx.empty() ? 0 : 0x04);
            W(address, stat);
        } else if (address == 0x0080270C) {
            u8 b = 0xFF;
            if (!state.spi_rx.empty()) {
                b = (u8)state.spi_rx[0];
                state.spi_rx.erase(0, 1);
            }
            W(address, b);
        }
        return;
    }
    if (chip->is_ejpeg_v1() && 0x0080A000 <= address && address < 0x0080A100) {
        u32 off = address - 0x0080A000;
        if (off == 0x00) W(address, state.ejpeg_reg0);
        else if (off == 0x04) W(address, state.ejpeg_reg1);
        else if (off == 0x08) W(address, state.ejpeg_reg2);
        else if (off == 0x0C) W(address, state.ejpeg_reg3);
        else if (off == 0x18) W(address, state.ejpeg_int_stat);
        else if (off == 0x1C) W(address, state.ejpeg_frame_len);
        return;
    }
    if (chip->is_ejpeg_v2() && 0x00A03000 <= address && address < 0x00A05000) {
        if (address == 0x00A03008) W(address, state.ejpeg_reg0);
        else if (address == 0x00A03018) W(address, state.ejpeg_int_stat);
        else if (address == 0x00A0301C) W(address, state.ejpeg_frame_len > 0 ? state.ejpeg_frame_len - 1 : 0);
        return;
    }
    if (address == TRNG_DATA && have_trng_) {
        W(address, trng_next());
        return;
    }
    if (address == 0x00800000) { W(address, chip_id_value); return; }
    if (address == 0x00800004) { W(address, device_id_value); return; }
    if (address == 0x00800070) { W(address, state.sctrl_reset_retention); return; }
    if (address == 0x0080011c) {
        u32 val = (state.sctrl_rosc_timer & ~0x100u) | (state.sctrl_rosc_int_status ? 0x100 : 0);
        W(address, val);
        return;
    }
    if (address == 0x00800140) { W(address, state.sctrl_rosc_timer_h & 0xFFFF); return; }
    if (address == 0x00800120) { W(address, state.sctrl_gpio_wakeup_en); return; }
    if (address == 0x00800124) { state.wake_reg_sampled = true; W(address, state.sctrl_gpio_wakeup_type); return; }
    if (address == 0x00800128) { state.wake_reg_sampled = true; W(address, state.sctrl_gpio_wakeup_int_status); return; }
    if (address == 0x00800144) { W(address, state.sctrl_gpio_wakeup_en1); return; }
    if (address == 0x00800148) { state.wake_reg_sampled = true; W(address, state.sctrl_gpio_wakeup_type1); return; }
    if (address == 0x0080014c) { state.wake_reg_sampled = true; W(address, state.sctrl_gpio_wakeup_int_status1); return; }
    if (address == 0x00808004) { state.wake_reg_sampled = true; W(address, state.aon_gpio_wakeup_status); return; }
    if (address == 0x00800084 && chip->usb_plug_wake) {
        u32 cur = mem_.phys_read(address, 4);
        W(address, cur | (1u << 21));
        return;
    }

    int pin = gpio_pin_at(address);
    if (pin >= 0) {
        int slot = gpio_slot(pin);
        std::map<int, u32>::const_iterator git = state.gpio_cfg.find(slot);
        u32 cfg = (git != state.gpio_cfg.end()) ? git->second : mem_.phys_read(address, 4);
        cfg = state.gpio_hw.read_config(pin, cfg);
        if (pin == state.wake_pulse_pin && state.wake_reg_sampled && state.wake_pulse_reads_left > 0) {
            state.wake_pulse_reads_left--;
            if (state.wake_pulse_reads_left == 0) {
                state.gpio_hw.inputs[pin] = state.wake_pulse_restore;
                state.wake_pulse_pin = -1;
            }
        }
        if (!state.i2c_buses.empty() && (cfg & 0x08)) {
            for (std::map<std::pair<int, int>, SimState::I2cBus>::iterator it = state.i2c_buses.begin();
                 it != state.i2c_buses.end(); ++it) {
                const SimState::I2cBus &bs = it->second;
                if (pin != it->first.second || !bs.in_tx)
                    continue;
                if (bs.in_ack || bs.bits == 8) {
                    if (bs.frame.empty() || (((u8)bs.frame[0]) & 1) == 0 || bs.frame.size() == 1) {
                        cfg &= ~1u;
                        break;
                    }
                } else if (!bs.frame.empty() && (((u8)bs.frame[0]) & 1) != 0) {
                    u8 addr_byte = (u8)bs.frame[0];
                    if (addr_byte == 0x81 || addr_byte == 0x89) {
                        size_t idx = bs.frame.size() - 1;
                        u8 rx_byte = 0xFF;
                        if (addr_byte == 0x81) {
                            if (bs.last_reg == 0xFE) {
                                static const u8 id_fe[2] = {0x59, 0x59};
                                rx_byte = idx < 2 ? id_fe[idx] : 0xFF;
                            } else if (bs.last_reg == 0xFF) {
                                static const u8 id_ff[2] = {0x83, 0x05};
                                rx_byte = idx < 2 ? id_ff[idx] : 0xFF;
                            } else if (bs.last_reg == 0x01) {
                                static const u8 hum[2] = {0x40, 0x00};
                                rx_byte = idx < 2 ? hum[idx] : 0xFF;
                            } else {
                                static const u8 th[4] = {0x1A, 0x00, 0x40, 0x00};
                                rx_byte = idx < 4 ? th[idx] : 0xFF;
                            }
                        } else {
                            static const u8 sht30[6] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xA2};
                            rx_byte = idx < 6 ? sht30[idx] : 0xFF;
                        }
                        int bit_idx = (bs.bits <= 0) ? 7 : (8 - bs.bits);
                        if (((rx_byte >> bit_idx) & 1) == 0)
                            cfg &= ~1u;
                        else
                            cfg |= 1u;
                        break;
                    }
                }
            }
        }
        W(address, cfg);
        return;
    }
    if (address == GPIO_INTEN) { W(address, (u32)(state.gpio_hw.int_enable & 0xFFFFFFFFULL)); return; }
    if (address == GPIO_INTLV0) { W(address, state.gpio_hw.read_mode_word(0)); return; }
    if (address == GPIO_INTLV1) { W(address, state.gpio_hw.read_mode_word(16)); return; }
    if (address == GPIO_INTSTA) { W(address, (u32)(state.gpio_hw.int_status & 0xFFFFFFFFULL)); return; }
    if (has_extended_gpio && address == GPIO_INTEN2) { W(address, (u32)((state.gpio_hw.int_enable >> 32) & 0xFF)); return; }
    if (has_extended_gpio && address == GPIO_INTLV3) { W(address, state.gpio_hw.read_mode_word(32, 8)); return; }
    if (has_extended_gpio && address == GPIO_INTSTA2) { W(address, (u32)((state.gpio_hw.int_status >> 32) & 0xFF)); return; }

    if (address == 0x00802A04) { W(address, state.pwm_status); return; }
    if ((address == PWMN_BASE + 0x24 || address == PWMN_BASE + 0x64 || address == PWMN_BASE + 0xA4) && has_pwm_new) {
        u32 cur = mem_.phys_read(address, 4);
        W(address, cur & ~0x3u);
        return;
    }
    if ((address == PWMN_BASE || address == PWMN_BASE + 0x40 || address == PWMN_BASE + 0x80) && has_pwm_new) {
        u32 group = (address - PWMN_BASE) >> 6;
        u32 cur = mem_.phys_read(address, 4);
        u32 stat = ((state.pwmn_stat >> (2 * group)) & 0x3) << 30;
        W(address, (cur & ~(PWMN_INT_STAT | PWMN_CFG_UPDATA)) | stat);
        return;
    }
    if (address == 0x00802A0C) { W(address, state.timer0_2_ctl); return; }
    if (address == 0x00802A10 || address == 0x00802A50) {
        u32 cur = mem_.phys_read(address, 4);
        W(address, cur & ~0x1u);
        return;
    }
    if (address == 0x00802A4C) { W(address, state.timer3_5_ctl); return; }

    if (address == 0x00802c00) {
        u32 base_cfg = state.saradc_cfg & ~((1u << 28) | (1u << 30) | 0x180u);
        u32 cfg;
        if ((chip->is_saradc_v1() || chip->is_saradc_v2()) && state.saradc_pending)
            cfg = base_cfg;
        else
            cfg = base_cfg | (1u << 28) | (1u << 30);
        W(address, cfg);
        return;
    }
    if (address == 0x00802c08 && (chip->is_saradc_v1() || chip->is_saradc_v2())) {
        u32 cur = mem_.phys_read(address, 4);
        W(address, cur & ~0x100u);
        return;
    }
    if (address == 0x00802c0c && chip->is_saradc_v3()) {
        u32 stat = state.saradc_state_reg & ~((1u << 30) | (1u << 31) | 0x28010u);
        if (!state.saradc_pending)
            stat |= (1u << 30);
        W(address, stat);
        return;
    }
    if (address == 0x00802d24) {
        u32 cmd_ctrl = mem_.phys_read(0x00802d00, 4);
        W(address, (cmd_ctrl & 0x2) ? 0x24 : 0x01);
        return;
    }
    if (address == 0x00802d34) {
        u32 cur = mem_.phys_read(address, 4);
        W(address, cur | (3u << 18));
        return;
    }
    if (address == 0x00800074) { W(address, state.efuse_ctrl); return; }
    if (address == 0x00900100 && !blecore_) { W(address, 0); return; }
    if (blecore_ && XVR_BASE <= address && address < XVR_BASE + blecore_->window) {
        bool has;
        u32 served = blecore_->read(address, state.insn_count, has);
        if (has) {
            W(address, served);
            return;
        }
    }
    if (opt_.xvr_selfclear && (address == 0x009000f8 || address == 0x00900000)) {
        if (state.xvr_armed.count(address)) {
            state.xvr_armed.erase(address);
            u32 cur = mem_.phys_read(address, 4);
            W(address, cur & ~0x80000000u);
        }
        return;
    }
    if (address == 0x00810000 || address == 0x0081001c) { W(address, 0); return; }
    if (address == 0x00810020) { W(address, 0x200); return; }
    if (address == 0x00800078) { W(address, 1u << 8); return; }
    if (address == 0xc0008050) { W(address, 0); return; }
    if (address == 0x00802c04 || address == 0x00802c10) {
        if (chip->is_saradc_v1() || chip->is_saradc_v2() || address == 0x00802c10) {
            if (state.saradc_pending)
                state.saradc_pending--;
            W(address, saradc_sample_for(state.saradc_channel));
            return;
        }
    }
    if (address == 0x00803004) {
        mem_.write_bytes(address, flash_state_.data);
        return;
    }
    if (address == ICU_INT_STATUS || address == ICU_INT_RAW_STATUS) {
        W(address, state.pending_irqs & state.icu_int_enable);
        return;
    }
    if (GDMA_BASE <= address && address < GDMA_END) {
        int ch = gdma_channel(address);
        if (ch >= 0) {
            std::map<int, u32>::const_iterator cit = state.gdma_conf.find(ch);
            if (cit != state.gdma_conf.end()) {
                W(address, cit->second);
                return;
            }
        }
        if (chip->is_gdma_v2() && 0x00809040 <= address && address < 0x00809240) {
            u32 within = (address - 0x00809040) % 0x40;
            int c = (int)((address - 0x00809040) / 0x40);
            if (within == 0x30) {
                std::map<int, u32>::const_iterator rit = state.gdma_remain.find(c);
                W(address, rit != state.gdma_remain.end() ? (rit->second & 0x1FFFF) : 0);
                return;
            }
        } else if (0x008090C0 <= address && address < 0x008090D8) {
            int c = (int)((address - 0x008090C0) / 4);
            std::map<int, u32>::const_iterator rit = state.gdma_remain.find(c);
            W(address, rit != state.gdma_remain.end() ? (rit->second & 0x1FFFF) : 0);
            return;
        } else if (address < GDMA_BASE + 6 * 0x20) {
            u32 within = (address - GDMA_BASE) % 0x20;
            int c = (int)((address - GDMA_BASE) / 0x20);
            if (within == 0x1C) {
                std::map<int, u32>::const_iterator rit = state.gdma_remain.find(c);
                W(address, rit != state.gdma_remain.end() ? (rit->second & 0x1FFFF) : 0);
                return;
            }
        }
    }
    if (address == ICU_INT_ENABLE) { W(address, state.icu_int_enable); return; }
    if (address == ICU_GLOBAL_INT_EN) { W(address, state.icu_global_int_en); return; }
    if (address == 0x00802110) { W(address, state.uart1_int_enable); return; }
    if (address == 0x00802210) { W(address, state.uart2_int_enable); return; }
    if (address == 0x00802114) { W(address, rx_active() ? ((1u << 1) | (1u << 6)) : 0); return; }
    if (address == 0x00802214) { W(address, rx_active_uart2() ? ((1u << 1) | (1u << 6)) : 0); return; }

    if (address == UART1_FIFO_PORT && rx_active()) {
        u8 b;
        {
            /* Python pops lock-free under the GIL; a std::string append from
             * the GUI thread (feed_uart1) may reallocate, so lock here. */
            ScopedLock lk(rx_lock_);
            b = (u8)uart1_rx_[0];
            uart1_rx_.erase(0, 1);
        }
        if (b == 0x0A && !uart1_rx_.empty() && !tuyamcu_
            && (state.rtt_boot_seen || (chip->rtthread_console && !state.obk_boot_seen))) {
            state.msh_prompt_seen = false;
            state.rx_next_line_insn = state.insn_count + 3000000;
        }
        if (emit_insns_) {
            SimState::U1Evt e;
            e.insn = state.insn_count;
            e.dir = 'R';
            e.byte = b;
            state.uart1_events.push_back(e);
        }
        W(address, (u32)(b & 0xFF) << 8);
        return;
    }
    if (address == UART2_FIFO_PORT && rx_active_uart2()) {
        u8 b;
        {
            /* Python pops lock-free under the GIL; a std::string append from
             * the GUI thread (feed_uart1) may reallocate, so lock here. */
            ScopedLock lk(rx_lock_);
            b = (u8)uart1_rx_[0];
            uart1_rx_.erase(0, 1);
        }
        if (b == 0x0A && !uart1_rx_.empty() && !tuyamcu_ && !state.obk_boot_seen) {
            state.msh_prompt_seen = false;
            state.rx_next_line_insn = state.insn_count + 3000000;
        }
        W(address, (u32)(b & 0xFF) << 8);
        return;
    }
    if (address == UART1_FIFO_STATUS || address == UART2_FIFO_STATUS) {
        u32 status = (1u << 17) | (1u << 20);
        bool rxp = (address == UART1_FIFO_STATUS && rx_active())
                   || (address == UART2_FIFO_STATUS && rx_active_uart2());
        if (rxp)
            status |= (1u << 21);
        else
            status |= (1u << 19);
        W(address, status);
        return;
    }
    if (chip->has_audio() && address == 0x00802B38) {
        u32 sta = (state.audio_fifo_conf & 0x3C) ? ((1u << 18) | (1u << 19)) : 0;
        if (state.audio_dac_writes >= 256)
            sta |= 0x4000 | 0x8000;
        W(address, sta);
        return;
    }
    if (address == 0x00803008) {
        u32 word_addr = flash_state_.addr + (u32)flash_state_.read_idx * 4;
        flash_state_.read_idx++;
        const Bytes &src = have_physical_ ? physical_flash_ : raw_flash_;
        u32 val = (word_addr + 4 <= src.size()) ? g32le(src, word_addr) : 0xFFFFFFFFu;
        W(address, val);
        return;
    }
    /* unhandled: leave backing memory as-is, and log the read (Python's
     * fall-through _print; silent under --only-uart) */
    if (only_uart_ || address == UART1_FIFO_STATUS || address == UART2_FIFO_STATUS || address == 0x00802c00)
        return;
    if (size == 4 || size == 2 || size == 1)
        print_line(strf("[MMIO] Read 0x%08x from 0x%08x", mem_.phys_read(address, size), address));
}

bool BekenEmulator::mmio_unmapped(u32 address, int size, int type)
{
    /* Python hook_unmapped: map a zeroed page for a data read or write and
     * retry; decline a fetch (UC_ERR_FETCH_UNMAPPED). */
    print_line(strf("[!] Unmapped access at 0x%08x (type=%d)", address, type));
    u32 page = address & ~0xFFFu;
    if (type == Memory::UNMAPPED_READ || type == Memory::UNMAPPED_WRITE) {
        if (mem_.map(page, 0x1000))
            return true;
        print_line(strf("Failed to map page 0x%08x: Invalid memory mapping (UC_ERR_MAP)", page));
    }
    return false;
}

/* -------------------------------------------------------------- snapshots */
/* The C++ snapshot format is self-contained; it is not interchangeable with
 * the Python pickle, but round-trips within this binary (that is what the
 * snapshot_roundtrip test checks). A full binary serializer would be large;
 * since the self-test only needs save+reload determinism, we serialize the
 * mapped memory, CPU context, and the scalar device state. */
/* ---- SimState (de)serialization for deterministic snapshot resume ---- */
static void sp_u32(Bytes &b, u32 v) { b += p32le(v); }
static void sp_u64(Bytes &b, u64 v) { b += p64le(v); }
static void sp_i(Bytes &b, int v) { b += p32le((u32)v); }
static void sp_b(Bytes &b, bool v) { b += (char)(v ? 1 : 0); }
static void sp_bytes(Bytes &b, const Bytes &s) { b += p32le((u32)s.size()); b += s; }
static u32 sg_u32(const Bytes &b, size_t &p) { u32 v = g32le(b, p); p += 4; return v; }
static u64 sg_u64(const Bytes &b, size_t &p) { u64 v = g64le(b, p); p += 8; return v; }
static int sg_i(const Bytes &b, size_t &p) { return (int)sg_u32(b, p); }
static bool sg_b(const Bytes &b, size_t &p) { bool v = b[p] != 0; p += 1; return v; }
static Bytes sg_bytes(const Bytes &b, size_t &p) { u32 n = sg_u32(b, p); Bytes s = b.substr(p, n); p += n; return s; }

template <class K, class V> static void sp_map(Bytes &b, const std::map<K, V> &m) {
    sp_u32(b, (u32)m.size());
    for (typename std::map<K, V>::const_iterator it = m.begin(); it != m.end(); ++it) { sp_i(b, (int)it->first); sp_u32(b, (u32)it->second); }
}
template <class K, class V> static void sg_map(const Bytes &b, size_t &p, std::map<K, V> &m) {
    m.clear(); u32 n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) { int k = sg_i(b, p); u32 v = sg_u32(b, p); m[(K)k] = (V)v; }
}

static void serialize_state(const SimState &s, Bytes &b) {
    sp_u32(b, s.icu_int_enable); sp_u32(b, s.icu_global_int_en); sp_u32(b, s.pending_irqs);
    sp_u32(b, s.uart1_int_enable); sp_u32(b, s.uart2_int_enable); sp_u64(b, s.insn_count);
    sp_u32(b, s.pwm_status); sp_u32(b, s.timer0_2_ctl); sp_u32(b, s.timer3_5_ctl); sp_u32(b, s.efuse_ctrl);
    sp_u32(b, s.saradc_cfg); sp_i(b, s.saradc_pending); sp_u32(b, s.last_pc);
    sp_u64(b, s.last_slow); sp_u64(b, s.last_fast); sp_u64(b, s.last_emit);
    sp_map(b, s.gpio_cfg); sp_map(b, s.pwm_regs); sp_map(b, s.gdma_conf); sp_map(b, s.gdma_remain);
    /* GpioModel */
    sp_i(b, s.gpio_hw.pin_count); sp_u64(b, s.gpio_hw.int_enable); sp_u64(b, s.gpio_hw.int_status);
    sp_u32(b, (u32)s.gpio_hw.int_modes.size());
    for (size_t i = 0; i < s.gpio_hw.int_modes.size(); i++) sp_i(b, s.gpio_hw.int_modes[i]);
    sp_map(b, s.gpio_hw.inputs);
    sp_i(b, s.i2c_last_pin); sp_i(b, s.bl0942_polls); sp_i(b, s.uart1_tx_prev);
    sp_b(b, s.uart_at_line_start); sp_u64(b, s.last_uart_insn); sp_bytes(b, s.uart_line_tail);
    sp_b(b, s.idle_task_valid); sp_u32(b, s.idle_task_lo); sp_u32(b, s.idle_task_hi);
    sp_u32(b, (u32)s.xvr_armed.size());
    for (std::set<u32>::const_iterator it = s.xvr_armed.begin(); it != s.xvr_armed.end(); ++it) sp_u32(b, *it);
    sp_b(b, s.pwmn_seen); sp_u32(b, s.pwmn_stat);
    sp_b(b, s.wdt_first_period_valid); sp_u32(b, s.wdt_first_period); sp_u32(b, s.wdt_period_ms);
    sp_b(b, s.wdt_clock_enabled); sp_b(b, s.wdt_deadline_valid); sp_u64(b, s.wdt_deadline);
    sp_u32(b, s.sctrl_rosc_timer); sp_u32(b, s.sctrl_rosc_timer_h); sp_b(b, s.sctrl_rosc_int_status);
    sp_u32(b, s.sctrl_gpio_wakeup_en); sp_u32(b, s.sctrl_gpio_wakeup_type); sp_u32(b, s.sctrl_gpio_wakeup_int_status);
    sp_u32(b, s.sctrl_gpio_wakeup_en1); sp_u32(b, s.sctrl_gpio_wakeup_type1); sp_u32(b, s.sctrl_gpio_wakeup_int_status1);
    sp_u32(b, s.aon_gpio_wakeup_status); sp_u32(b, s.sctrl_reset_retention);
    sp_b(b, s.in_deep_sleep); sp_i(b, s.deep_sleep_count);
    sp_b(b, s.rtt_boot_seen); sp_b(b, s.obk_boot_seen); sp_b(b, s.msh_prompt_seen); sp_i(b, s.msh_uart_port);
    sp_u64(b, s.rx_next_line_insn); sp_u32(b, s.audio_fifo_conf); sp_i(b, s.audio_dac_writes);
    /* camera / ejpeg */
    sp_bytes(b, Bytes(s.camera_sensor)); sp_b(b, s.camera_sensor_probed); sp_i(b, s.camera_i2c_writes);
    sp_u64(b, s.camera_last_i2c_insn);
    sp_u32(b, s.ejpeg_reg0); sp_u32(b, s.ejpeg_reg1); sp_u32(b, s.ejpeg_reg2); sp_u32(b, s.ejpeg_reg3);
    sp_u32(b, s.ejpeg_int_stat); sp_u32(b, s.ejpeg_frame_len); sp_i(b, s.ejpeg_dma_ch);
    sp_u32(b, s.ejpeg_dma_dst); sp_u32(b, s.ejpeg_dma_node_len); sp_i(b, s.ejpeg_tick_div);
    sp_i(b, s.camera_frame_count); sp_i(b, s.camera_width); sp_i(b, s.camera_height);
}

static void deserialize_state(SimState &s, const Bytes &b, size_t &p) {
    s.icu_int_enable = sg_u32(b, p); s.icu_global_int_en = sg_u32(b, p); s.pending_irqs = sg_u32(b, p);
    s.uart1_int_enable = sg_u32(b, p); s.uart2_int_enable = sg_u32(b, p); s.insn_count = sg_u64(b, p);
    s.pwm_status = sg_u32(b, p); s.timer0_2_ctl = sg_u32(b, p); s.timer3_5_ctl = sg_u32(b, p); s.efuse_ctrl = sg_u32(b, p);
    s.saradc_cfg = sg_u32(b, p); s.saradc_pending = sg_i(b, p); s.last_pc = sg_u32(b, p);
    s.last_slow = sg_u64(b, p); s.last_fast = sg_u64(b, p); s.last_emit = sg_u64(b, p);
    sg_map(b, p, s.gpio_cfg); sg_map(b, p, s.pwm_regs); sg_map(b, p, s.gdma_conf); sg_map(b, p, s.gdma_remain);
    s.gpio_hw.pin_count = sg_i(b, p); s.gpio_hw.int_enable = sg_u64(b, p); s.gpio_hw.int_status = sg_u64(b, p);
    { u32 n = sg_u32(b, p); s.gpio_hw.int_modes.assign(n, 0); for (u32 i = 0; i < n; i++) s.gpio_hw.int_modes[i] = sg_i(b, p); }
    sg_map(b, p, s.gpio_hw.inputs);
    s.i2c_last_pin = sg_i(b, p); s.bl0942_polls = sg_i(b, p); s.uart1_tx_prev = sg_i(b, p);
    s.uart_at_line_start = sg_b(b, p); s.last_uart_insn = sg_u64(b, p); s.uart_line_tail = sg_bytes(b, p);
    s.idle_task_valid = sg_b(b, p); s.idle_task_lo = sg_u32(b, p); s.idle_task_hi = sg_u32(b, p);
    { u32 n = sg_u32(b, p); s.xvr_armed.clear(); for (u32 i = 0; i < n; i++) s.xvr_armed.insert(sg_u32(b, p)); }
    s.pwmn_seen = sg_b(b, p); s.pwmn_stat = sg_u32(b, p);
    s.wdt_first_period_valid = sg_b(b, p); s.wdt_first_period = sg_u32(b, p); s.wdt_period_ms = sg_u32(b, p);
    s.wdt_clock_enabled = sg_b(b, p); s.wdt_deadline_valid = sg_b(b, p); s.wdt_deadline = sg_u64(b, p);
    s.sctrl_rosc_timer = sg_u32(b, p); s.sctrl_rosc_timer_h = sg_u32(b, p); s.sctrl_rosc_int_status = sg_b(b, p);
    s.sctrl_gpio_wakeup_en = sg_u32(b, p); s.sctrl_gpio_wakeup_type = sg_u32(b, p); s.sctrl_gpio_wakeup_int_status = sg_u32(b, p);
    s.sctrl_gpio_wakeup_en1 = sg_u32(b, p); s.sctrl_gpio_wakeup_type1 = sg_u32(b, p); s.sctrl_gpio_wakeup_int_status1 = sg_u32(b, p);
    s.aon_gpio_wakeup_status = sg_u32(b, p); s.sctrl_reset_retention = sg_u32(b, p);
    s.in_deep_sleep = sg_b(b, p); s.deep_sleep_count = sg_i(b, p);
    s.rtt_boot_seen = sg_b(b, p); s.obk_boot_seen = sg_b(b, p); s.msh_prompt_seen = sg_b(b, p); s.msh_uart_port = sg_i(b, p);
    s.rx_next_line_insn = sg_u64(b, p); s.audio_fifo_conf = sg_u32(b, p); s.audio_dac_writes = sg_i(b, p);
    s.camera_sensor = sg_bytes(b, p); s.camera_sensor_probed = sg_b(b, p); s.camera_i2c_writes = sg_i(b, p);
    s.camera_last_i2c_insn = sg_u64(b, p);
    s.ejpeg_reg0 = sg_u32(b, p); s.ejpeg_reg1 = sg_u32(b, p); s.ejpeg_reg2 = sg_u32(b, p); s.ejpeg_reg3 = sg_u32(b, p);
    s.ejpeg_int_stat = sg_u32(b, p); s.ejpeg_frame_len = sg_u32(b, p); s.ejpeg_dma_ch = sg_i(b, p);
    s.ejpeg_dma_dst = sg_u32(b, p); s.ejpeg_dma_node_len = sg_u32(b, p); s.ejpeg_tick_div = sg_i(b, p);
    s.camera_frame_count = sg_i(b, p); s.camera_width = sg_i(b, p); s.camera_height = sg_i(b, p);
}

/* ---- "BKX2" extension: the rest of what Python's pickle carries ----
 * Appended after the Wi-Fi MAC section, so snapshots written before it (which
 * end there) still load, with these fields at their reset values. */
static const char SNAP_EXT_TAG[] = "BKX2";
/* guest-memory accessors for the radio models (emulator.cpp) */
Bytes ble_mem_read(void *ctx, u32 addr, u32 size);
void ble_mem_write(void *ctx, u32 addr, const Bytes &data);
static void sp_dbl(Bytes &b, double d) { u64 v; memcpy(&v, &d, 8); sp_u64(b, v); }
static double sg_dbl(const Bytes &b, size_t &p) { u64 v = sg_u64(b, p); double d; memcpy(&d, &v, 8); return d; }
static void sp_pair(Bytes &b, const std::pair<int, int> &k) { sp_i(b, k.first); sp_i(b, k.second); }
static std::pair<int, int> sg_pair(const Bytes &b, size_t &p) { int a = sg_i(b, p); int c = sg_i(b, p); return std::make_pair(a, c); }
static void sp_pbytes(Bytes &b, const std::map<std::pair<int, int>, Bytes> &m) {
    sp_u32(b, (u32)m.size());
    for (std::map<std::pair<int, int>, Bytes>::const_iterator it = m.begin(); it != m.end(); ++it) { sp_pair(b, it->first); sp_bytes(b, it->second); }
}
static void sg_pbytes(const Bytes &b, size_t &p, std::map<std::pair<int, int>, Bytes> &m) {
    m.clear(); u32 n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) { std::pair<int, int> k = sg_pair(b, p); m[k] = sg_bytes(b, p); }
}

static void serialize_state_ext(const SimState &s, Bytes &b) {
    /* report dirty sets + UART1 event log (not yet emitted at the save point) */
    sp_u32(b, (u32)s.gpio_dirty.size());
    for (std::set<int>::const_iterator it = s.gpio_dirty.begin(); it != s.gpio_dirty.end(); ++it) sp_i(b, *it);
    sp_u32(b, (u32)s.pwm_dirty.size());
    for (std::set<u32>::const_iterator it = s.pwm_dirty.begin(); it != s.pwm_dirty.end(); ++it) sp_u32(b, *it);
    sp_u32(b, (u32)s.i2c_dirty.size());
    for (std::set<std::pair<int, int> >::const_iterator it = s.i2c_dirty.begin(); it != s.i2c_dirty.end(); ++it) sp_pair(b, *it);
    sp_u32(b, (u32)s.uart1_events.size());
    for (size_t i = 0; i < s.uart1_events.size(); i++) { sp_u64(b, s.uart1_events[i].insn); b += s.uart1_events[i].dir; b += (char)s.uart1_events[i].byte; }
    /* bit-banged I2C capture */
    sp_u32(b, (u32)s.i2c_pair_counts.size());
    for (std::map<std::pair<int, int>, int>::const_iterator it = s.i2c_pair_counts.begin(); it != s.i2c_pair_counts.end(); ++it) { sp_pair(b, it->first); sp_i(b, it->second); }
    sp_u32(b, (u32)s.i2c_buses.size());
    for (std::map<std::pair<int, int>, SimState::I2cBus>::const_iterator it = s.i2c_buses.begin(); it != s.i2c_buses.end(); ++it) {
        sp_pair(b, it->first); sp_b(b, it->second.in_tx); sp_i(b, it->second.cur_byte); sp_i(b, it->second.bits); sp_bytes(b, it->second.frame);
    }
    sp_pbytes(b, s.i2c_frames); sp_pbytes(b, s.i2c_last_on);
    sp_bytes(b, s.ws2812_spi);
    /* deep sleep bookkeeping */
    const DeepSleepInfo &d = s.deep_sleep_info;
    sp_b(b, d.valid); sp_u32(b, d.sleep_reg); sp_b(b, d.rtc_enabled); sp_u32(b, d.rtc_ticks);
    sp_b(b, d.rtc_seconds_valid); sp_dbl(b, d.rtc_seconds); sp_u64(b, d.wake_gpio_mask); sp_u64(b, d.wake_gpio_type);
    sp_u32(b, (u32)d.wake_pins.size());
    for (size_t i = 0; i < d.wake_pins.size(); i++) sp_pair(b, d.wake_pins[i]);
    sp_u64(b, d.insn_count);
    sp_dbl(b, s.deep_sleep_rtc_sec);
    sp_map(b, s.deep_sleep_wake_pins);
    sp_bytes(b, Bytes(s.last_wake_reason)); sp_i(b, s.last_wake_pin);
    sp_i(b, s.wake_pulse_pin); sp_i(b, s.wake_pulse_restore); sp_i(b, s.wake_pulse_reads_left); sp_b(b, s.wake_reg_sampled);
    /* hardware I2C buses */
    sp_u32(b, (u32)s.hw_i2c.size());
    for (std::map<int, HwI2cBus>::const_iterator it = s.hw_i2c.begin(); it != s.hw_i2c.end(); ++it) {
        const HwI2cBus &h = it->second;
        sp_i(b, it->first); sp_u32(b, h.cfg); sp_u32(b, h.sta); sp_u32(b, h.last_dat); sp_u32(b, h.read_dat);
        sp_u32(b, h.slave); sp_u32(b, h.reg); sp_u32(b, h.phase); sp_b(b, h.v2);
    }
    /* camera */
    sp_u32(b, (u32)s.camera_sensor_regs.size());
    for (std::map<std::pair<int, int>, u8>::const_iterator it = s.camera_sensor_regs.begin(); it != s.camera_sensor_regs.end(); ++it) { sp_pair(b, it->first); b += (char)it->second; }
    sp_bytes(b, s.camera_last_jpeg); sp_bytes(b, s.camera_frame_rgb);
    /* hardware timers + SARADC + SPI */
    for (int i = 0; i < 6; i++) { sp_u32(b, s.hw_timer_counter[i]); sp_u64(b, s.hw_timer_due[i]); }
    sp_u32(b, s.saradc_cfg2); sp_u32(b, s.saradc_state_reg);
    sp_i(b, s.saradc_channel); sp_i(b, s.saradc_last_raw);
    sp_i(b, s.saradc_conversions); sp_i(b, s.saradc_reads);
    sp_map(b, s.adc_samples);
    sp_bytes(b, s.spi_tx); sp_bytes(b, s.spi_rx);
}

static void deserialize_state_ext(SimState &s, const Bytes &b, size_t &p) {
    u32 n;
    s.gpio_dirty.clear(); n = sg_u32(b, p); for (u32 i = 0; i < n; i++) s.gpio_dirty.insert(sg_i(b, p));
    s.pwm_dirty.clear(); n = sg_u32(b, p); for (u32 i = 0; i < n; i++) s.pwm_dirty.insert(sg_u32(b, p));
    s.i2c_dirty.clear(); n = sg_u32(b, p); for (u32 i = 0; i < n; i++) s.i2c_dirty.insert(sg_pair(b, p));
    s.uart1_events.clear(); n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) { SimState::U1Evt e; e.insn = sg_u64(b, p); e.dir = b[p++]; e.byte = (u8)b[p++]; s.uart1_events.push_back(e); }
    s.i2c_pair_counts.clear(); n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) { std::pair<int, int> k = sg_pair(b, p); s.i2c_pair_counts[k] = sg_i(b, p); }
    s.i2c_buses.clear(); n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) {
        std::pair<int, int> k = sg_pair(b, p); SimState::I2cBus bus;
        bus.in_tx = sg_b(b, p); bus.cur_byte = sg_i(b, p); bus.bits = sg_i(b, p); bus.frame = sg_bytes(b, p);
        s.i2c_buses[k] = bus;
    }
    sg_pbytes(b, p, s.i2c_frames); sg_pbytes(b, p, s.i2c_last_on);
    s.ws2812_spi = sg_bytes(b, p);
    DeepSleepInfo d;
    d.valid = sg_b(b, p); d.sleep_reg = sg_u32(b, p); d.rtc_enabled = sg_b(b, p); d.rtc_ticks = sg_u32(b, p);
    d.rtc_seconds_valid = sg_b(b, p); d.rtc_seconds = sg_dbl(b, p); d.wake_gpio_mask = sg_u64(b, p); d.wake_gpio_type = sg_u64(b, p);
    n = sg_u32(b, p); for (u32 i = 0; i < n; i++) d.wake_pins.push_back(sg_pair(b, p));
    d.insn_count = sg_u64(b, p);
    s.deep_sleep_info = d;
    s.deep_sleep_rtc_sec = sg_dbl(b, p);
    sg_map(b, p, s.deep_sleep_wake_pins);
    s.last_wake_reason = sg_bytes(b, p); s.last_wake_pin = sg_i(b, p);
    s.wake_pulse_pin = sg_i(b, p); s.wake_pulse_restore = sg_i(b, p); s.wake_pulse_reads_left = sg_i(b, p); s.wake_reg_sampled = sg_b(b, p);
    s.hw_i2c.clear(); n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) {
        int k = sg_i(b, p); HwI2cBus h;
        h.cfg = sg_u32(b, p); h.sta = sg_u32(b, p); h.last_dat = sg_u32(b, p); h.read_dat = sg_u32(b, p);
        h.slave = sg_u32(b, p); h.reg = sg_u32(b, p); h.phase = sg_u32(b, p); h.v2 = sg_b(b, p);
        s.hw_i2c[k] = h;
    }
    s.camera_sensor_regs.clear(); n = sg_u32(b, p);
    for (u32 i = 0; i < n; i++) { std::pair<int, int> k = sg_pair(b, p); s.camera_sensor_regs[k] = (u8)b[p++]; }
    s.camera_last_jpeg = sg_bytes(b, p); s.camera_frame_rgb = sg_bytes(b, p);
    for (int i = 0; i < 6; i++) { s.hw_timer_counter[i] = sg_u32(b, p); s.hw_timer_due[i] = sg_u64(b, p); }
    s.saradc_cfg2 = sg_u32(b, p); s.saradc_state_reg = sg_u32(b, p);
    s.saradc_channel = sg_i(b, p); s.saradc_last_raw = sg_i(b, p);
    s.saradc_conversions = sg_i(b, p); s.saradc_reads = sg_i(b, p);
    sg_map(b, p, s.adc_samples);
    s.spi_tx = sg_bytes(b, p); s.spi_rx = sg_bytes(b, p);
}

void BekenEmulator::save_snapshot(const std::string &path)
{
    /* Layout: magic, CPU context, insn_count, mapped regions, SimState, the
     * Wi-Fi MAC, then the BKX2 extension with everything else Python pickles
     * (chip identity, rest of the state, flash controller + backing flash,
     * queued UART RX, TuyaMCU, BLE core, [BLE_ADV] dedupe, TRNG, network clock). */
    Bytes blob;
    blob += "BKSNAP01";
    CpuContext ctx;
    cpu_.context_save(ctx);
    blob += Bytes((const char *)&ctx, sizeof(ctx));
    blob += p64le(state.insn_count);
    std::vector<std::pair<u32, u32> > regs;
    mem_.regions(regs);
    blob += p32le((u32)regs.size());
    for (size_t i = 0; i < regs.size(); i++) {
        blob += p32le(regs[i].first);
        blob += p32le(regs[i].second);
        blob += mem_.read_bytes(regs[i].first, regs[i].second);
    }
    serialize_state(state, blob);
    blob += (char)(wifimac_ ? 1 : 0);
    if (wifimac_)
        wifimac_->serialize(blob);

    blob += Bytes(SNAP_EXT_TAG, 4);
    sp_u32(blob, chip_id_value);
    sp_u32(blob, device_id_value);
    sp_bytes(blob, Bytes(chip_variant));
    serialize_state_ext(state, blob);
    sp_u32(blob, flash_state_.addr);
    sp_bytes(blob, flash_state_.data);
    sp_i(blob, flash_state_.read_idx);
    sp_u32(blob, (u32)flash_state_.write_buf.size());
    for (size_t i = 0; i < flash_state_.write_buf.size(); i++)
        sp_u32(blob, flash_state_.write_buf[i]);
    sp_bytes(blob, flash_state_.pw_buf);
    sp_b(blob, have_physical_);
    if (have_physical_)
        sp_bytes(blob, physical_flash_);
    sp_bytes(blob, raw_flash_);
    {
        ScopedLock lk(rx_lock_);
        sp_bytes(blob, uart1_rx_);
    }
    blob += (char)(uart_src_ == 0 ? 0 : (strcmp(uart_src_, "u1") == 0 ? 1 : 2));
    sp_b(blob, tuyamcu_ != 0);
    if (tuyamcu_)
        tuyamcu_->serialize(blob);
    sp_b(blob, blecore_ != 0);
    if (blecore_)
        blecore_->serialize(blob);
    sp_u32(blob, (u32)ble_adv_seen_.size());
    for (std::set<Bytes>::const_iterator it = ble_adv_seen_.begin(); it != ble_adv_seen_.end(); ++it)
        sp_bytes(blob, *it);
    sp_b(blob, have_trng_);
    sp_u32(blob, trng_state_);
    sp_u64(blob, network_insn_offset_);
    write_file(path, blob);
    net_log(strf("[SNAPSHOT] saved %s at %llu instructions (%llu bytes)", path.c_str(),
                 (unsigned long long)state.insn_count, (unsigned long long)blob.size()));
}

/* Python _restore_legacy_gpio_state: build the GPIO hardware model for a
 * snapshot made before it existed, from the controller/pin words in mapped
 * MMIO. Upper P32..P39 (physical slots 48..55) are recovered explicitly. */
void BekenEmulator::restore_legacy_gpio_state()
{
    GpioModel gpio_hw(gpio_pin_count);
    u64 enabled = mem_.phys_read(GPIO_INTEN, 4);
    if (has_extended_gpio)
        enabled |= (u64)(mem_.phys_read(GPIO_INTEN2, 4) & 0xFF) << 32;
    gpio_hw.write_enable(enabled);
    gpio_hw.write_mode_word(0, mem_.phys_read(GPIO_INTLV0, 4));
    gpio_hw.write_mode_word(16, mem_.phys_read(GPIO_INTLV1, 4));
    if (has_extended_gpio) {
        gpio_hw.write_mode_word(32, mem_.phys_read(GPIO_INTLV3, 4), 8);
        for (int pin = 32; pin < 40; pin++) {
            int slot = gpio_slot(pin);
            if (state.gpio_cfg.count(slot))
                continue;
            u32 config = mem_.phys_read(GPIO_BASE + (u32)slot * 4, 4);
            /* zero is also the untouched/reset value: nothing to recover */
            if (config)
                state.gpio_cfg[slot] = config;
        }
    }
    state.gpio_hw = gpio_hw;
}

bool BekenEmulator::load_snapshot(const std::string &path, std::string *err)
{
    Bytes blob;
    if (!read_file(path, blob) || blob.size() < 8 || blob.compare(0, 8, "BKSNAP01") != 0) {
        if (err)
            *err = strf("%s is not a snapshot of this emulator", path.c_str());
        return false;
    }
    /* Parse everything first, so a mismatched snapshot is refused (like
     * Python's checks before it touches the device) without half-loading. */
    size_t p = 8;
    CpuContext ctx;
    memcpy(&ctx, blob.data() + p, sizeof(ctx));
    p += sizeof(ctx);
    u64 insn_count = g64le(blob, p);
    p += 8;
    u32 n = g32le(blob, p);
    p += 4;
    std::vector<std::pair<u32, std::pair<size_t, u32> > > regions; /* begin, (offset, size) */
    for (u32 i = 0; i < n; i++) {
        u32 begin = g32le(blob, p);
        p += 4;
        u32 sz = g32le(blob, p);
        p += 4;
        regions.push_back(std::make_pair(begin, std::make_pair(p, sz)));
        p += sz;
    }
    SimState st = state;
    bool have_state = p < blob.size();
    if (have_state)
        deserialize_state(st, blob, p);
    bool have_mac_section = p < blob.size();
    bool snap_mac = false;
    size_t mac_pos = 0;
    if (have_mac_section) {
        snap_mac = blob[p++] != 0;
        mac_pos = p;
        if (snap_mac) {
            MacHw skip(1); /* only to step over the section */
            skip.deserialize(blob, p);
        }
    }
    bool have_ext = p + 4 <= blob.size() && blob.compare(p, 4, SNAP_EXT_TAG) == 0;
    if (have_ext) {
        p += 4;
        u32 sc = sg_u32(blob, p), device = sg_u32(blob, p);
        std::string variant = sg_bytes(blob, p);
        if (sc != chip_id_value || device != device_id_value || variant != chip_variant) {
            if (err)
                *err = strf("snapshot was taken with a different -chip (%s, not %s)",
                            variant.c_str(), chip_variant.c_str());
            return false;
        }
    }
    if (have_mac_section && snap_mac != (wifimac_ != 0)) {
        if (err)
            *err = strf("snapshot was taken %s the Wi-Fi MAC model (--wifi); use the same option",
                        snap_mac ? "with" : "without");
        return false;
    }
    if (have_state && st.gpio_hw.pin_count != gpio_pin_count) {
        if (err)
            *err = "snapshot GPIO width does not match its chip identity";
        return false;
    }

    for (size_t i = 0; i < regions.size(); i++) {
        u32 begin = regions[i].first, sz = regions[i].second.second;
        /* pages the run mapped on demand (unmapped accesses) come back too */
        for (u32 a = begin; a - begin < sz; a += 0x1000)
            if (!mem_.is_mapped(a))
                mem_.map(a, 0x1000);
        mem_.write_bytes(begin, blob.substr(regions[i].second.first, sz));
    }
    if (have_state) {
        state = st;
    } else {
        /* A snapshot without the state section predates the GPIO model: its
         * INTEN/INTLV and pin words are only ordinary mapped MMIO words. */
        restore_legacy_gpio_state();
    }
    state.insn_count = insn_count;
    /* Host-side events are outside snapshots: a press queued before the load
     * must not leak into the restored device on its next pacing boundary. */
    {
        ScopedLock lk(gpio_lock_);
        gpio_events_.clear();
    }
    if (snap_mac && wifimac_) {
        size_t mp = mac_pos;
        wifimac_->deserialize(blob, mp);
    }
    if (have_ext) {
        deserialize_state_ext(state, blob, p);
        {
            ScopedLock lk(adc_lock_);
            for (std::map<int, int>::const_iterator it = adc_inputs.begin(); it != adc_inputs.end(); ++it)
                state.adc_samples[it->first] = it->second;
        }
        flash_state_ = FlashState();
        flash_state_.addr = sg_u32(blob, p);
        flash_state_.data = sg_bytes(blob, p);
        flash_state_.read_idx = sg_i(blob, p);
        n = sg_u32(blob, p);
        for (u32 i = 0; i < n; i++)
            flash_state_.write_buf.push_back(sg_u32(blob, p));
        flash_state_.pw_buf = sg_bytes(blob, p);
        if (sg_b(blob, p)) {
            physical_flash_ = sg_bytes(blob, p);
            have_physical_ = true;
        }
        raw_flash_ = sg_bytes(blob, p);
        {
            ScopedLock lk(rx_lock_);
            uart1_rx_ = sg_bytes(blob, p);
        }
        u8 src = (u8)blob[p++];
        uart_src_ = src == 0 ? 0 : (src == 1 ? "u1" : "u2");
        /* Python: the snapshot's MCU replaces this run's, even with none. */
        delete tuyamcu_;
        tuyamcu_ = sg_b(blob, p) ? tuyamcu::Slave::deserialize(blob, p) : 0;
        /* Python: a snapshot BLE core replaces this run's; without one, keep it. */
        if (sg_b(blob, p)) {
            delete blecore_;
            blecore_ = BleCore::deserialize(blob, p);
            ble_layout_ = blecore_->layout;
            MemAccess ma;
            ma.ctx = this;
            ma.read = ble_mem_read;
            ma.write = ble_mem_write;
            blecore_->set_mem(ma);
        }
        ble_adv_seen_.clear();
        n = sg_u32(blob, p);
        for (u32 i = 0; i < n; i++)
            ble_adv_seen_.insert(sg_bytes(blob, p));
        bool snap_trng = sg_b(blob, p);
        u32 trng = sg_u32(blob, p);
        if (snap_trng) {
            trng_state_ = trng;
            have_trng_ = true;
        }
        network_insn_offset_ = sg_u64(blob, p);
    }
    cpu_.context_restore(ctx);
    u32 pc = cpu_.reg_read(REG_PC);
    u32 cpsr = cpu_.reg_read(REG_CPSR);
    resume_at_ = pc | ((cpsr & PSR_T) ? 1 : 0);
    have_resume_at_ = true;
    net_log(strf("[SNAPSHOT] restored %s: %llu instructions, pc 0x%08x", path.c_str(),
                 (unsigned long long)state.insn_count, pc));
    return true;
}
