/* The big MMIO read/write handlers plus hardware-I2C, camera, GDMA and EJPEG
 * helpers. Port of hook_mem_read_mmio / hook_mem_write_mmio from emulator.py. */
#include "emulator.h"
#include "blecore.h"
#include "wifimac.h"
#include "wifipeer.h"
#include "periph.h"

#include <string.h>
#include <stdio.h>

static const u32 ICU_INT_STATUS = 0x0080204c;
static const u32 ICU_INT_ENABLE = 0x00802040;
static const u32 ICU_GLOBAL_INT_EN = 0x00802044;
static const u32 ICU_PERI_CLK_PWD = 0x00802008;
static const u32 ICU_WDT_CLK_PWD = 1u << 8;
static const u32 WDT_CTRL = 0x00802900;
static const u32 UART1_FIFO_PORT = 0x0080210c;
static const u32 UART2_FIFO_PORT = 0x0080220c;
static const u32 GPIO_BASE = 0x00802800;
static const u32 GDMA_BASE = 0x00809000;
static const u32 GDMA_END = GDMA_BASE + 0x240;
static const u32 GDMA_EN = 1u << 0;
static const u32 PWM_BASE = 0x00802A00;
static const u32 PWMN_BASE = 0x00802B00;
static const u32 PWMN_INT_STAT = (1u << 30) | (1u << 31);
static const u32 PWMN_CFG_UPDATA = (1u << 7) | (1u << 15);
static const u32 GPIO_FUNC_CFG = GPIO_BASE + 32 * 4;
static const u32 GPIO_INTEN = GPIO_BASE + 33 * 4;
static const u32 GPIO_INTLV0 = GPIO_BASE + 34 * 4;
static const u32 GPIO_INTLV1 = GPIO_BASE + 35 * 4;
static const u32 GPIO_INTSTA = GPIO_BASE + 36 * 4;
static const u32 GPIO_FUNC_CFG_2 = GPIO_BASE + 46 * 4;
static const u32 GPIO_FUNC_CFG_3 = GPIO_BASE + 47 * 4;
static const u32 GPIO_INTEN2 = GPIO_BASE + 56 * 4;
static const u32 GPIO_INTLV3 = GPIO_BASE + 57 * 4;
static const u32 GPIO_INTSTA2 = GPIO_BASE + 58 * 4;

/* ---------------------------------------------------------- hardware I2C */
u32 BekenEmulator::camera_i2c_read(u32 slave_id, u32 reg_addr)
{
    state.camera_last_i2c_insn = state.insn_count;
    reg_addr &= 0xFF;
    u32 ret;
    if ((slave_id == 0x21 || slave_id == 0x42) && reg_addr == 0xF0) {
        if (state.camera_sensor.empty())
            state.camera_sensor = "GC0328C";
        state.camera_sensor_probed = true;
        ret = 0x9D;
    } else if ((slave_id == 0x21 || slave_id == 0x42) && reg_addr == 0xF1) {
        ret = 0x28;
    } else if ((slave_id == 0x21 || slave_id == 0x42) && reg_addr == 0x00) {
        ret = 0x00;
    } else {
        std::map<std::pair<int, int>, u8>::iterator it =
            state.camera_sensor_regs.find(std::make_pair((int)slave_id, (int)reg_addr));
        ret = it == state.camera_sensor_regs.end() ? 0 : it->second;
    }
    return ret;
}

void BekenEmulator::camera_i2c_write(u32 slave_id, u32 reg_addr, u32 data_byte)
{
    state.camera_last_i2c_insn = state.insn_count;
    if (state.camera_sensor.empty() && (slave_id == 0x21 || slave_id == 0x42)) {
        state.camera_sensor = "GC0328C";
        state.camera_sensor_probed = true;
    }
    state.camera_sensor_regs[std::make_pair((int)slave_id, (int)(reg_addr & 0xFF))] = (u8)(data_byte & 0xFF);
    state.camera_i2c_writes++;
}

void BekenEmulator::hw_i2c_write(int bus_num, u32 offset, u32 value)
{
    HwI2cBus &bus = state.hw_i2c[bus_num];
    u32 irq_bit = bus_num == 1 ? (1u << 2) : (1u << 5);
    if (offset == 0x08 || (offset == 0x00 && (value & 0x80000000u)))
        bus.v2 = true;

    if (!bus.v2) {
        if (offset == 0x04) {
            bus.last_dat = value & 0xFF;
        } else if (offset == 0x00) {
            if (!(value & 1) || (value & (1u << 2))) {
                bus.cfg = value & ~((1u << 1) | (1u << 2) | (1u << 16) | (1u << 19));
                bus.phase = 0;
                state.pending_irqs &= ~irq_bit;
            } else if (value & (1u << 1)) {
                u32 slave_id = (bus.last_dat >> 1) & 0x7F;
                bool is_read = (bus.last_dat & 1) != 0;
                bus.slave = slave_id;
                bus.phase = is_read ? 3 : 1;
                bus.cfg = (value & ~(1u << 19)) | (1u << 16) | (1u << 17) | (1u << 1);
                state.pending_irqs |= irq_bit;
            } else if (bus.phase != 0) {
                if (bus.phase == 1) {
                    bus.reg = bus.last_dat & 0xFF;
                    bus.phase = 2;
                } else if (bus.phase == 2) {
                    camera_i2c_write(bus.slave, bus.reg, bus.last_dat);
                    bus.reg = (bus.reg + 1) & 0xFF;
                }
                bus.cfg = (value & ~((1u << 1) | (1u << 19))) | (1u << 16) | (1u << 17);
                state.pending_irqs |= irq_bit;
            } else {
                bus.cfg = value & ~((1u << 16) | (1u << 19));
            }
        }
    } else {
        if (offset == 0x00) {
            bus.cfg = value;
            if (!(value & 0x80000000u)) {
                bus.phase = 0;
                bus.sta = 0;
                state.pending_irqs &= ~irq_bit;
            }
        } else if (offset == 0x08) {
            bus.last_dat = value & 0xFF;
        } else if (offset == 0x04) {
            if (value & (1u << 9)) {
                bus.sta = value & ~((1u << 0) | (1u << 9) | (1u << 10) | (1u << 14) | (1u << 15));
                bus.phase = 0;
                state.pending_irqs &= ~irq_bit;
            } else if (value & (1u << 10)) {
                u32 slave_id = (bus.last_dat >> 1) & 0x7F;
                bool is_read = (bus.last_dat & 1) != 0;
                bus.slave = slave_id;
                bus.phase = is_read ? 3 : 1;
                bus.sta = (value & ~(1u << 15)) | (1u << 0) | (1u << 8) | (1u << 10) | (1u << 14);
                state.pending_irqs |= irq_bit;
            } else if (bus.phase != 0) {
                if (bus.phase == 1) {
                    bus.reg = bus.last_dat & 0xFF;
                    bus.phase = 2;
                } else if (bus.phase == 2) {
                    camera_i2c_write(bus.slave, bus.reg, bus.last_dat);
                    bus.reg = (bus.reg + 1) & 0xFF;
                }
                bus.sta = (value & ~((1u << 10) | (1u << 15))) | (1u << 0) | (1u << 8) | (1u << 14);
                state.pending_irqs |= irq_bit;
            } else {
                bus.sta = value & ~((1u << 0) | (1u << 15));
            }
        }
    }
}

u32 BekenEmulator::hw_i2c_read(int bus_num, u32 offset)
{
    HwI2cBus &bus = state.hw_i2c[bus_num];
    if (!bus.v2) {
        if (offset == 0x00)
            return bus.cfg;
        if (offset == 0x04) {
            u32 val = camera_i2c_read(bus.slave, bus.reg);
            bus.reg = (bus.reg + 1) & 0xFF;
            return val & 0xFF;
        }
        return 0;
    }
    if (offset == 0x00)
        return bus.cfg;
    if (offset == 0x04)
        return bus.sta;
    if (offset == 0x08) {
        u32 val = camera_i2c_read(bus.slave, bus.reg);
        bus.reg = (bus.reg + 1) & 0xFF;
        return val & 0xFF;
    }
    return 0;
}

/* ---------------------------------------------------------------- GDMA */
int BekenEmulator::gdma_channel(u32 address)
{
    if (chip->is_gdma_v2()) {
        if (!(0x00809040 <= address && address < 0x00809240))
            return -1;
        u32 off = address - 0x00809040;
        return (off % 0x40 == 0) ? (int)(off / 0x40) : -1;
    }
    if (!(GDMA_BASE <= address && address < GDMA_BASE + 6 * 0x20))
        return -1;
    u32 off = address - GDMA_BASE;
    return (off % 0x20 == 0) ? (int)(off / 0x20) : -1;
}

void BekenEmulator::gdma_start(int channel, u32 conf)
{
    u32 length = ((conf >> 16) & 0xFFFF) + 1;
    u32 base = chip->is_gdma_v2() ? (0x00809040 + (u32)channel * 0x40) : (GDMA_BASE + (u32)channel * 0x20);
    u32 dst = mem_.phys_read(base + 0x04, 4);
    u32 src = mem_.phys_read(base + 0x08, 4);
    bool both_inc = (conf & (1u << 8)) && (conf & (1u << 9));
    if (both_inc && length) {
        /* Unicorn checks the whole area before copying: nothing is written
         * when either side is unmapped, and the failure is logged */
        if (!range_mapped(src, length))
            print_line(strf("[GDMA] ch%d copy 0x%08x -> 0x%08x (%u bytes) failed: "
                            "Invalid memory read (UC_ERR_READ_UNMAPPED)", channel, src, dst, length));
        else if (!range_mapped(dst, length))
            print_line(strf("[GDMA] ch%d copy 0x%08x -> 0x%08x (%u bytes) failed: "
                            "Invalid memory write (UC_ERR_WRITE_UNMAPPED)", channel, src, dst, length));
        else
            mem_.write_bytes(dst, mem_.read_bytes(src, length));
    } else if ((conf & (1u << 8)) && !(conf & (1u << 9)) && dst == 0x0080270c && length >= 24 && length <= 8192) {
        if (range_mapped(src, length))
            state.ws2812_spi = mem_.read_bytes(src, length);
    } else if (!(conf & (1u << 8)) && (conf & (1u << 9))
               && ((chip->is_ejpeg_v1() && src == 0x0080A014)
                   || (chip->is_ejpeg_v2() && src == 0x00A03014)
                   || dst >= 0x00400000)) {
        state.ejpeg_dma_ch = channel;
        state.ejpeg_dma_dst = dst;
        state.ejpeg_dma_node_len = length;
        state.gdma_remain[channel] = length;
    }
}

/* ---------------------------------------------------------------- camera */
/* Synthesize a baseline JFIF JPEG frame and matching 64x48 RGB preview, byte
 * for byte like the Python _build_camera_frame. */
void BekenEmulator::build_camera_frame(int frame_idx, int target_len, Bytes &jpeg_out, Bytes &rgb_out)
{
    const int w_prev = 64, h_prev = 48;
    rgb_out.assign((size_t)(w_prev * h_prev * 3), (char)0);
    int sun_x = 14 + (frame_idx * 3) % 36;
    int sun_y = 12;
    static const int bars[8][3] = {
        {235, 235, 235}, {235, 220, 40}, {40, 210, 220}, {45, 205, 75},
        {215, 55, 205}, {225, 50, 50}, {45, 75, 230}, {35, 35, 45}};
    for (int y = 0; y < h_prev; y++) {
        for (int x = 0; x < w_prev; x++) {
            int idx = (y * w_prev + x) * 3;
            int r, g, b;
            if (y >= 38) {
                const int *c = bars[(x * 8) / w_prev];
                r = c[0]; g = c[1]; b = c[2];
            } else {
                int dx = x - sun_x, dy = y - sun_y;
                if (dx * dx + dy * dy <= 25) {
                    r = 255; g = 225; b = 70;
                } else if (y < 24) {
                    r = 35 + (y * 3); g = 115 + (y * 4); b = 215 + (y * 1);
                } else {
                    int hill = 24 + ((x * 5 + frame_idx * 2) % 7);
                    if (y < hill) {
                        r = 95; g = 175; b = 235;
                    } else {
                        r = 35 + ((x + y) % 25); g = 140 + ((y - 24) * 4); b = 55 + ((x * 2) % 30);
                    }
                }
            }
            rgb_out[idx] = (char)(r & 0xFF);
            rgb_out[idx + 1] = (char)(g & 0xFF);
            rgb_out[idx + 2] = (char)(b & 0xFF);
        }
    }

    int width = state.camera_width ? state.camera_width : 640;
    int height = state.camera_height ? state.camera_height : 480;
    int q_val = 16 - (frame_idx % 8);
    if (q_val < 4)
        q_val = 4;
    Bytes dqt = bytes_n(64, (u8)q_val);
    Bytes header;
    header += H("ffd8");                                  /* SOI */
    header += H("ffe00010");                              /* APP0 marker + len */
    header += Bytes("JFIF", 4);
    header += H("00010101004800480000");                 /* JFIF payload */
    header += H("ffdb004300") + dqt;                      /* DQT */
    header += H("ffc0000b08");                            /* SOF0 marker + len + precision */
    header += Bytes(1, (char)((height >> 8) & 0xFF));
    header += Bytes(1, (char)(height & 0xFF));
    header += Bytes(1, (char)((width >> 8) & 0xFF));
    header += Bytes(1, (char)(width & 0xFF));
    header += H("01011100");                              /* 1 component */
    header += H("ffc4001f0000010501010101010100000000000000000102030405060708090a0b"); /* DC Huffman */
    header += H("ffc40014100100000000000000000000000000000000");  /* AC Huffman */

    Bytes sos;
    sos += H("ffda0008010100003f00");
    for (int i = 0; i < 64; i++)
        sos += Bytes(1, (char)(0x7F ^ ((frame_idx + i) & 0x3F)));
    sos += H("ffd9");

    std::string meta = strf("BekenSimulator %s %dx%d Frame #%04d ",
                            (state.camera_sensor.empty() ? "GC0328C" : state.camera_sensor.c_str()),
                            width, height, frame_idx);
    int pad_needed = target_len - (int)header.size() - (int)sos.size() - 4;
    if (pad_needed < 32)
        pad_needed = 32;
    Bytes com_payload;
    if ((int)meta.size() < pad_needed) {
        com_payload = Bytes(meta.data(), meta.size());
        for (int i = 0; i < pad_needed - (int)meta.size(); i++)
            com_payload += Bytes(1, (char)(0x41 + ((frame_idx + i) % 26)));
    } else {
        com_payload = Bytes(meta.data(), (size_t)pad_needed);
    }
    Bytes com_seg = H("fffe");
    u32 clen = (u32)com_payload.size() + 2;
    com_seg += Bytes(1, (char)((clen >> 8) & 0xFF));
    com_seg += Bytes(1, (char)(clen & 0xFF));
    com_seg += com_payload;

    jpeg_out = header + com_seg + sos;
}

void BekenEmulator::tick_camera_ejpeg()
{
    bool enc_en = (state.ejpeg_reg0 & 0x3) || (state.ejpeg_reg0 & ((1u << 4) | (1u << 31))) || (state.ejpeg_reg1 & (1u << 4));
    if (!enc_en)
        return;
    u32 fiq_mask = chip->is_ejpeg_v2() ? (1u << 23) : ((1u << 23) | (1u << 29));
    u32 fiq_bits = state.icu_int_enable & fiq_mask;
    if (!fiq_bits)
        return;
    if (state.ejpeg_dma_ch < 0 || state.ejpeg_dma_dst < 0x00400000)
        return;
    if (state.camera_last_i2c_insn && (state.insn_count - state.camera_last_i2c_insn) < 20000)
        return;
    state.ejpeg_tick_div++;
    if (state.ejpeg_tick_div < 5)
        return;
    state.ejpeg_tick_div = 0;

    u32 node_len = state.ejpeg_dma_node_len ? state.ejpeg_dma_node_len : 1460;
    if (node_len < 512)
        node_len = 512;
    int target_len = (int)node_len - 64;
    if (target_len < 1050)
        target_len = 1050;
    if (target_len > 1180)
        target_len = 1180;
    Bytes jpeg, rgb;
    build_camera_frame(state.camera_frame_count, target_len, jpeg, rgb);
    state.camera_frame_count++;
    state.camera_last_jpeg = jpeg;
    state.camera_frame_rgb = rgb;
    Bytes payload = jpeg;
    if (state.ejpeg_reg1 & (1u << 16))
        payload += Bytes(1, '\0') + p32le((u32)jpeg.size());
    if (!range_mapped(state.ejpeg_dma_dst, (u32)payload.size()))
        return; /* Python: mem_write raised UcError -> frame not delivered */
    mem_.write_bytes(state.ejpeg_dma_dst, payload);
    state.ejpeg_frame_len = (u32)jpeg.size();
    state.gdma_remain[state.ejpeg_dma_ch] = node_len > payload.size() ? node_len - (u32)payload.size() : 0;
    state.ejpeg_int_stat |= (1u << 1);
    if (state.ejpeg_reg0 & (1u << 2))
        state.ejpeg_int_stat |= (1u << 0);
    state.pending_irqs |= fiq_bits;
    if (state.camera_frame_count == 1)
        net_log(strf("[CAMERA] %s streaming %dx%d JPEG (%d bytes/frame, DMA ch%d -> 0x%08x)",
                     (state.camera_sensor.empty() ? "GC0328C" : state.camera_sensor.c_str()),
                     state.camera_width, state.camera_height, (int)jpeg.size(),
                     state.ejpeg_dma_ch, state.ejpeg_dma_dst));
}

void BekenEmulator::report_ble_adv()
{
    if (!blecore_ || !state.uart_at_line_start)
        return;
    std::vector<AdvFound> found = blecore_->find_adv();
    if (found.empty())
        return;
    u32 base = blecore_->em_base();
    for (size_t i = 0; i < found.size(); i++) {
        if (ble_adv_seen_.count(found[i].raw))
            continue;
        ble_adv_seen_.insert(found[i].raw);
        net_log(strf("[BLE_ADV] EM 0x%08x +0x%04x (%d bytes): %s", base, found[i].offset,
                     (int)found[i].raw.size(), hexstr_sep(found[i].raw, " ").c_str()));
        StrList lines = BleCore::describe_ad(found[i].fields);
        for (size_t j = 0; j < lines.size(); j++)
            net_log("[BLE_ADV]   " + rstrip(lines[j]));
    }
}

/* guest-memory accessors for the radio models (emulator.cpp) */
Bytes ble_mem_read(void *ctx, u32 addr, u32 size);
void ble_mem_write(void *ctx, u32 addr, const Bytes &data);

void BekenEmulator::reset_peripheral_models()
{
    /* Python _reset_peripheral_models: reset the on-chip radio engines while
     * retaining their external peers. The access point/router is outside the
     * emulated chip, so it survives the reset; only the MAC endpoint is
     * replaced, and the peer forgets the rebooted radio's per-boot sessions.
     * (The peer's inject/now callbacks are trampolines through this emulator,
     * so they follow the new MAC without being re-pointed.) */
    if (wifimac_) {
        delete wifimac_;
        wifimac_ = new MacHw((u64)SLOW_TICK_INSNS / (SLOW_TICK_MS * 1000));
        MemAccess ma;
        ma.ctx = this;
        ma.read = ble_mem_read;
        ma.write = ble_mem_write;
        wifimac_->mem = ma;
        wifimac_->have_mem = true;
        wifimac_->log = &BekenEmulator::wifi_net_log;
        wifimac_->log_ctx = this;
        if (wifi_peer_) {
            wifimac_->on_tx = &BekenEmulator::wifi_on_tx;
            wifimac_->on_tx_ctx = this;
            wifi_peer_->device_reset(true, network_now_us());
        }
    }
    if (blecore_) {
        delete blecore_;
        blecore_ = new BleCore(XVR_BASE, (u64)SLOW_TICK_INSNS * 1000 / SLOW_TICK_MS, ble_layout_);
        MemAccess ma;
        ma.ctx = this;
        ma.read = ble_mem_read;
        ma.write = ble_mem_write;
        blecore_->set_mem(ma);
    }
}

/* =============================================================== WRITE */
/* Unicorn calls a UC_HOOK_MEM_WRITE hook BEFORE the store and then commits
 * the guest's original value at the original size, whatever the hook did (its
 * own mem_write of the same bytes included, which is why the efuse shadow
 * exists) - also after the hook's early returns (GDMA CONF, I2C, the UART TX
 * FIFOs, ...). Bytes the hook wrote outside the access stay written. */
void BekenEmulator::mmio_write(u32 address, int size, u32 value)
{
    mmio_write_regs(address, size, value);
    mem_.phys_write(address, size, value);
}

void BekenEmulator::mmio_write_regs(u32 address, int size, u32 value)
{
    value &= 0xFFFFFFFF;

    if (address == UART1_FIFO_PORT) {
        u8 b_val = value & 0xFF;
        note_uart_tx(1, b_val);
        if (emit_insns_) {
            SimState::U1Evt e;
            e.insn = state.insn_count;
            e.dir = 'T';
            e.byte = b_val;
            state.uart1_events.push_back(e);
        }
        if (tuyamcu_) {
            Bytes reply = tuyamcu_->react_stream(Bytes(1, (char)b_val));
            if (!reply.empty()) {
                ScopedLock lk(rx_lock_);
                uart1_rx_ += reply;
            }
        } else {
            Bytes reply = bl0942_react_byte(b_val);
            if (!reply.empty()) {
                ScopedLock lk(rx_lock_);
                uart1_rx_ += reply;
            }
        }
        state.uart1_tx_prev = b_val;
        if (uart_sink) {
            uart_sink(uart_sink_ctx, 1, b_val);
            return;
        }
        if (uart1_hex_) {
            if (uart_src_ == 0 || strcmp(uart_src_, "u1") != 0) {
                uart_write(Bytes("\n[UART1/MCU] "));
                uart_src_ = "u1";
            }
            uart_write(strf("%02x ", b_val));
        } else {
            uart_write(Bytes(1, (char)b_val));
        }
        return;
    }
    if (address == UART2_FIFO_PORT) {
        u8 b_val = value & 0xFF;
        note_uart_tx(2, b_val);
        if (uart_sink) {
            uart_sink(uart_sink_ctx, 2, b_val);
            return;
        }
        if (uart1_hex_ && uart_src_ && strcmp(uart_src_, "u1") == 0) {
            uart_write(Bytes("\n"));
            uart_src_ = "u2";
        }
        uart_write(Bytes(1, (char)b_val));
        return;
    }
    if (address == ICU_INT_STATUS) {
        state.pending_irqs &= ~value;
        if (wifimac_)
            state.pending_irqs |= wifimac_->lines;
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == WDT_CTRL) {
        u32 key = (value >> 16) & 0xFF;
        u32 period = value & 0xFFFF;
        if (key == 0x5A) {
            state.wdt_first_period = period;
            state.wdt_first_period_valid = true;
        } else if (key == 0xA5 && state.wdt_first_period_valid && state.wdt_first_period == period) {
            state.wdt_first_period_valid = false;
            state.wdt_period_ms = period;
            arm_watchdog();
        } else {
            state.wdt_first_period_valid = false;
        }
        mem_.phys_write(address, size, value);
        return;
    }
    if (GDMA_BASE <= address && address < GDMA_END) {
        int ch = gdma_channel(address);
        if (ch >= 0) {
            if (value & GDMA_EN)
                gdma_start(ch, value);
            state.gdma_conf[ch] = value & ~GDMA_EN;
            return;
        }
    }
    if (0x00802300 <= address && address < 0x0080230C) {
        hw_i2c_write(1, address - 0x00802300, value);
        return;
    }
    if (0x00802600 <= address && address < 0x0080260C) {
        hw_i2c_write(2, address - 0x00802600, value);
        return;
    }
    if (0x00802700 <= address && address < 0x00802710) {
        if (address == 0x00802704) {
            if ((value & 0x3) == 0) {
                state.spi_tx.clear();
                state.spi_rx.clear();
            }
        } else if (address == 0x00802708) {
            if (value & (1u << 16))
                state.spi_tx.clear();
            if (value & (1u << 17))
                state.spi_rx.clear();
        } else if (address == 0x0080270C) {
            u8 b = (u8)(value & 0xFF);
            state.spi_tx += (char)b;
            u8 cmd = (u8)state.spi_tx[0];
            if (cmd == 0x4B) {
                if (state.spi_tx.size() <= 5)
                    state.spi_rx += (char)0xFF;
                else
                    state.spi_rx += flash_uid_[(state.spi_tx.size() - 6) % 16];
            } else if (cmd == 0x9F) {
                if (state.spi_tx.size() == 1)
                    state.spi_rx += (char)0xFF;
                else {
                    static const u8 jedec[3] = {0x85, 0x60, 0x15};
                    state.spi_rx += (char)jedec[(state.spi_tx.size() - 2) % 3];
                }
            } else {
                state.spi_rx += (char)0xFF;
            }
        }
        mem_.phys_write(address, 4, value);
        return;
    }
    if (chip->is_ejpeg_v1() && 0x0080A000 <= address && address < 0x0080A100) {
        u32 off = address - 0x0080A000;
        if (off == 0x00) {
            state.ejpeg_reg0 = value;
            u32 x_code = (value >> 8) & 0x3;
            if (x_code)
                state.camera_width = x_code == 1 ? 640 : x_code == 2 ? 320 : x_code == 3 ? 1280 : 640;
        } else if (off == 0x04) {
            state.ejpeg_reg1 = value;
            u32 x_pix = (value >> 8) & 0xFF;
            u32 y_pix = (value >> 24) & 0xFF;
            if (x_pix >= 20)
                state.camera_width = x_pix * 8;
            if (y_pix >= 15)
                state.camera_height = y_pix * 8;
            else if (y_pix == 1 || y_pix == 2 || y_pix == 3)
                state.camera_height = y_pix == 1 ? 480 : y_pix == 2 ? 240 : 720;
        } else if (off == 0x08)
            state.ejpeg_reg2 = value;
        else if (off == 0x0C)
            state.ejpeg_reg3 = value;
        else if (off == 0x18) {
            state.ejpeg_int_stat &= ~value;
            if (!state.ejpeg_int_stat)
                state.pending_irqs &= ~((1u << 23) | (1u << 29));
        }
        mem_.phys_write(address, 4, value);
        return;
    }
    if (chip->is_ejpeg_v2() && 0x00A03000 <= address && address < 0x00A05000) {
        if (address < 0x00A03100) {
            u32 off = address - 0x00A03000;
            if (off == 0x08)
                state.ejpeg_reg0 = value;
            else if (off == 0x18) {
                state.ejpeg_int_stat &= ~value;
                if (!state.ejpeg_int_stat)
                    state.pending_irqs &= ~((1u << 23) | (1u << 29));
            } else if (off == 0x34) {
                if (value & 0x2)
                    state.ejpeg_reg1 |= (1u << 16);
                else
                    state.ejpeg_reg1 &= ~(1u << 16);
            }
        } else if (0x00A04000 <= address && address < 0x00A04100) {
            u32 off = address - 0x00A04000;
            if (off == 0x14) {
                u32 x_pix = value & 0xFF;
                u32 y_pix = (value >> 8) & 0xFF;
                if (x_pix >= 20)
                    state.camera_width = x_pix * 8;
                if (y_pix >= 15)
                    state.camera_height = y_pix * 8;
            }
        }
        mem_.phys_write(address, 4, value);
        return;
    }
    if (wifimac_ && WIFI_CORE <= address && address < WIFI_CORE + WIFI_SIZE) {
        wifimac_->write(address, value, state.insn_count);
        state.pending_irqs = (state.pending_irqs & ~WIFI_LINE_MASK) | wifimac_->lines;
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == ICU_INT_ENABLE) {
        state.icu_int_enable = value;
        print_line(strf("[DEBUG] ICU_INT_ENABLE set to: 0x%08x", value));
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == ICU_GLOBAL_INT_EN) {
        state.icu_global_int_en = value;
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == ICU_PERI_CLK_PWD) {
        bool enabled = !(value & ICU_WDT_CLK_PWD);
        if (enabled != state.wdt_clock_enabled) {
            state.wdt_clock_enabled = enabled;
            if (enabled)
                arm_watchdog();
            else
                state.wdt_deadline_valid = false;
        }
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == 0x00802110) {
        state.uart1_int_enable = value;
        mem_.phys_write(address, size, value);
        return;
    }
    if (address == 0x00802210) {
        state.uart2_int_enable = value;
        mem_.phys_write(address, size, value);
        return;
    }
    if (opt_.xvr_selfclear && (address == 0x009000f8 || address == 0x00900000) && (value & 0x80000000u))
        state.xvr_armed.insert(address);
    if (blecore_ && XVR_BASE <= address && address < XVR_BASE + blecore_->window)
        blecore_->write(address, value, state.insn_count);

    /* GPIO */
    int pin = gpio_pin_at(address);
    if (pin >= 0) {
        int slot = gpio_slot(pin);
        std::pair<std::map<int, u32>::iterator, bool> ins = state.gpio_cfg.insert(std::make_pair(slot, value));
        u32 old = ins.second ? 0 : ins.first->second;
        u32 nw = value;
        state.gpio_hw.config_changed(pin, old, nw);
        ins.first->second = nw;
        i2c_pin_edge(pin, old, nw);
        if (emit_insns_)
            state.gpio_dirty.insert(slot);
        sync_gpio_irq();
        mem_.phys_write(address, size, value);
        return;
    } else if (address == GPIO_INTEN) {
        state.gpio_hw.write_enable((state.gpio_hw.int_enable & ~0xFFFFFFFFULL) | (value & 0xFFFFFFFFu));
        sync_gpio_irq();
    } else if (address == GPIO_INTLV0) {
        state.gpio_hw.write_mode_word(0, value);
        sync_gpio_irq();
    } else if (address == GPIO_INTLV1) {
        state.gpio_hw.write_mode_word(16, value);
        sync_gpio_irq();
    } else if (address == GPIO_INTSTA) {
        state.gpio_hw.clear_status(value);
        sync_gpio_irq();
    } else if (has_extended_gpio && address == GPIO_INTEN2) {
        u64 low = state.gpio_hw.int_enable & 0xFFFFFFFFULL;
        state.gpio_hw.write_enable(low | ((u64)(value & 0xFF) << 32));
        sync_gpio_irq();
    } else if (has_extended_gpio && address == GPIO_INTLV3) {
        state.gpio_hw.write_mode_word(32, value, 8);
        sync_gpio_irq();
    } else if (has_extended_gpio && address == GPIO_INTSTA2) {
        state.gpio_hw.clear_status((u64)(value & 0xFF) << 32);
        sync_gpio_irq();
    } else if (address == GPIO_FUNC_CFG || address == GPIO_FUNC_CFG_2 || address == GPIO_FUNC_CFG_3) {
        int slot = (int)((address - GPIO_BASE) / 4);
        state.gpio_cfg[slot] = value;
        if (emit_insns_)
            state.gpio_dirty.insert(slot);
    }

    /* PWM new */
    if ((address == PWMN_BASE || address == PWMN_BASE + 0x40 || address == PWMN_BASE + 0x80) && has_pwm_new) {
        u32 group = (address - PWMN_BASE) >> 6;
        state.pwmn_seen = true;
        state.pwmn_stat &= ~(((value >> 30) & 0x3) << (2 * group));
    }
    if (PWM_BASE <= address && address < PWM_BASE + 0x1C0) {
        u32 off = address - PWM_BASE;
        state.pwm_regs[off] = value;
        if (emit_insns_)
            state.pwm_dirty.insert(off);
    }
    if (chip->has_audio()) {
        if (address == 0x00802B08) {
            state.audio_fifo_conf = value;
            if (!(value & 0x3C))
                state.pending_irqs &= ~(1u << 10);
        } else if (address == 0x00802B1C)
            state.audio_dac_writes++;
    }
    if (address == 0x00802A04) {
        u32 w1c = value & 0x3F;
        state.pwm_status = (state.pwm_status & ~0x3Fu) | (value & ~0x3Fu);
        state.pwm_status &= ~w1c;
    }
    if (address == 0x00802A0C) {
        u32 old_enable = state.timer0_2_ctl & 0x7;
        u32 w1c = value & (0x7u << 7);
        state.timer0_2_ctl = (value & ~(0x7u << 7)) | (state.timer0_2_ctl & (0x7u << 7));
        state.timer0_2_ctl &= ~w1c;
        timer_enable_changed(state, 0, old_enable, state.timer0_2_ctl);
    }
    if (address == 0x00802A4C) {
        u32 old_enable = state.timer3_5_ctl & 0x7;
        u32 w1c = value & (0x7u << 7);
        state.timer3_5_ctl = (value & ~(0x7u << 7)) | (state.timer3_5_ctl & (0x7u << 7));
        state.timer3_5_ctl &= ~w1c;
        timer_enable_changed(state, 1, old_enable, state.timer3_5_ctl);
    }
    if (chip->is_timer_v2()) {
        if (0x00802A00 <= address && address < 0x00802A0C)
            state.hw_timer_counter[(address - 0x00802A00) / 4] = value;
        else if (0x00802A40 <= address && address < 0x00802A4C)
            state.hw_timer_counter[3 + (address - 0x00802A40) / 4] = value;
    }

    /* UART TX */
    if (address == UART1_FIFO_PORT) {
        u8 b_val = value & 0xFF;
        note_uart_tx(1, b_val);
        if (emit_insns_) {
            SimState::U1Evt e;
            e.insn = state.insn_count;
            e.dir = 'T';
            e.byte = b_val;
            state.uart1_events.push_back(e);
        }
        if (tuyamcu_) {
            Bytes reply = tuyamcu_->react_stream(Bytes(1, (char)b_val));
            if (!reply.empty()) {
                ScopedLock lk(rx_lock_);
                uart1_rx_ += reply;
            }
        } else {
            Bytes reply = bl0942_react_byte(b_val);
            if (!reply.empty()) {
                ScopedLock lk(rx_lock_);
                uart1_rx_ += reply;
            }
        }
        state.uart1_tx_prev = b_val;
        if (uart_sink) {
            uart_sink(uart_sink_ctx, 1, b_val);
            return;
        }
        if (uart1_hex_) {
            if (uart_src_ == 0 || strcmp(uart_src_, "u1") != 0) {
                uart_write(Bytes("\n[UART1/MCU] "));
                uart_src_ = "u1";
            }
            uart_write(strf("%02x ", b_val));
        } else {
            uart_write(Bytes(1, (char)b_val));
        }
        return;
    }
    if (address == UART2_FIFO_PORT) {
        u8 b_val = value & 0xFF;
        note_uart_tx(2, b_val);
        if (uart_sink) {
            uart_sink(uart_sink_ctx, 2, b_val);
            return;
        }
        if (uart1_hex_ && uart_src_ && strcmp(uart_src_, "u1") == 0) {
            uart_write(Bytes("\n"));
            uart_src_ = "u2";
        }
        uart_write(Bytes(1, (char)b_val));
        return;
    }

    /* flash controller */
    /* REG_FLASH_CONF: CRC_EN (bit 26) is only observed (FLASHDBG). */
    static int flashdbg = -1;
    if (flashdbg < 0)
        flashdbg = env_has("FLASHDBG") ? 1 : 0;
    if (address == 0x0080301C && flashdbg) {
        fprintf(stderr, "FLASHDBG conf=0x%08x CRC_EN=%u\n", value, (value >> 26) & 1);
        fflush(stderr);
    }
    if (address == 0x00803004) {
        flash_state_.write_buf.push_back(value);
        while (flash_state_.write_buf.size() > 8)
            flash_state_.write_buf.erase(flash_state_.write_buf.begin());
        mem_.phys_write(address, 4, value);
        return;
    }
    if (address == 0x00803024) {
        if (value & 0x80000000u)
            flash_state_.pw_buf.clear();
        else {
            flash_state_.pw_buf += (char)(u8)(value & 0xFF);
            if (flash_state_.pw_buf.size() > 256)
                flash_state_.pw_buf = flash_state_.pw_buf.substr(flash_state_.pw_buf.size() - 256);
        }
        mem_.phys_write(address, 4, value);
        return;
    }
    if (address == 0x00803000) {
        flash_state_.addr = value & 0x00FFFFFF;
        flash_state_.read_idx = 0;
        u32 op_type = (value >> 24) & 0x1F;
        if (flash_state_.addr >= 0x001E0000 && flashdbg) {
            fprintf(stderr, "FLASHDBG op addr=0x%06x op=%u reg=0x%08x\n", flash_state_.addr, op_type, value);
            fflush(stderr);
        }
        if (op_type == 12) {
            if (!flash_state_.pw_buf.empty()) {
                flash_program_page(flash_state_.addr, flash_state_.pw_buf);
                flash_state_.pw_buf.clear();
            } else {
                flash_program(flash_state_.addr, flash_state_.write_buf);
            }
        } else if (op_type == 13)
            flash_erase(flash_state_.addr, 0x1000);
        else if (op_type == 14 || op_type == 15)
            flash_erase(flash_state_.addr, 0x10000);
        value = value & ~(1u << 31);
    }

    /* SARADC config */
    if (address == 0x00802c00) {
        state.saradc_cfg = value;
        static int adcdbg = -1;
        if (adcdbg < 0)
            adcdbg = env_has("ADCDBG") ? 1 : 0;
        if (adcdbg) {
            /* Python's "%#x" of the chip id ("0x0" for zero, unlike C's) */
            fprintf(stderr, "ADCDBG cfg write=0x%08x chnl_en=%u chip=0x%x\n", value, (value >> 2) & 1,
                    chip_id_value);
            fflush(stderr);
        }
        if (chip->is_saradc_v1() || chip->is_saradc_v2()) {
            if (value & (1u << 2)) {
                if (state.saradc_pending == 0) {
                    int ch_mask = chip->is_saradc_v2() ? 0xF : 0x7;
                    int ch = (int)((value >> 3) & ch_mask);
                    state.saradc_channel = ch;
                    state.saradc_conversions++;
                    int raw = get_adc_input(ch);
                    state.saradc_last_raw = raw;
                    state.adc_samples[ch] = raw;
                    state.saradc_pending = 32;
                }
            } else {
                state.saradc_pending = 0;
            }
        } else if (value & 0x2) {
            state.saradc_pending = 0;
        }
    }
    if (address == 0x00802c08 && chip->is_saradc_v3()) {
        state.saradc_cfg2 = value;
        state.saradc_channel = (int)(value & 0xF);
    }
    if (address == 0x00802c0c && chip->is_saradc_v3()) {
        state.saradc_state_reg = value & ~0x28010u;
        if ((value & 0x14008u) && !(value & 0x28010u)) {
            int ch = state.saradc_channel >= 0 ? state.saradc_channel : 0;
            state.saradc_channel = ch;
            state.saradc_conversions++;
            int raw = get_adc_input(ch);
            state.saradc_last_raw = raw;
            state.adc_samples[ch] = raw;
            bool custom_ch = false;
            {
                ScopedLock lk(adc_lock_);
                custom_ch = adc_inputs.count(ch) != 0;
            }
            if (ch < 7 || custom_ch) {
                if (state.saradc_pending == 0)
                    state.saradc_pending = 32;
            }
        } else if (!(value & 0x14008u)) {
            state.saradc_pending = 0;
        }
    }
    if (address == 0x00800074)
        state.efuse_ctrl = value & ~1u;

    /* SCTRL / deep sleep */
    if (address == 0x00800070)
        state.sctrl_reset_retention = value;
    else if (address == 0x0080011c) {
        if (value & 0x100)
            state.sctrl_rosc_int_status = false;
        state.sctrl_rosc_timer = value & ~0x100u;
    } else if (address == 0x00800140)
        state.sctrl_rosc_timer_h = value & 0xFFFF;
    else if (address == 0x00800120)
        state.sctrl_gpio_wakeup_en = value;
    else if (address == 0x00800124)
        state.sctrl_gpio_wakeup_type = value;
    else if (address == 0x00800128)
        state.sctrl_gpio_wakeup_int_status &= ~value;
    else if (address == 0x00800144)
        state.sctrl_gpio_wakeup_en1 = value;
    else if (address == 0x00800148)
        state.sctrl_gpio_wakeup_type1 = value;
    else if (address == 0x0080014c)
        state.sctrl_gpio_wakeup_int_status1 &= ~value;
    else if (address == 0x00808004)
        state.aon_gpio_wakeup_status &= ~value;
    else if (address == 0x00800104) {
        if ((value & 0xFFFF) == 0xADC1) {
            mem_.phys_write(address, 4, value);
            enter_deep_sleep(value);
            return;
        }
    }

    mem_.phys_write(address, size, value);
}

Bytes BekenEmulator::bl0942_reply_packet()
{
    u32 v_rms = (u32)py_round(meter_v_ * 15188.0);
    if (v_rms > 0xFFFFFF) v_rms = 0xFFFFFF;
    u32 i_rms = (u32)py_round(meter_a_ * 251210.0);
    if (i_rms > 0xFFFFFF) i_rms = 0xFFFFFF;
    u32 watt = (u32)py_round(meter_w_ * 598.0);
    if (watt > 0x7FFFFF) watt = 0x7FFFFF;
    u32 cf_cnt = (u32)(state.bl0942_polls * 10);
    if (cf_cnt < 1) cf_cnt = 1;
    if (cf_cnt > 0xFFFFFF) cf_cnt = 0xFFFFFF;
    u32 freq = 1000000 / 50;
    Bytes pkt = bytes_n(23, 0);
    pkt[0] = (char)0x55;
    pkt[1] = (char)(i_rms & 0xFF); pkt[2] = (char)((i_rms >> 8) & 0xFF); pkt[3] = (char)((i_rms >> 16) & 0xFF);
    pkt[4] = (char)(v_rms & 0xFF); pkt[5] = (char)((v_rms >> 8) & 0xFF); pkt[6] = (char)((v_rms >> 16) & 0xFF);
    pkt[7] = (char)(i_rms & 0xFF); pkt[8] = (char)((i_rms >> 8) & 0xFF); pkt[9] = (char)((i_rms >> 16) & 0xFF);
    pkt[10] = (char)(watt & 0xFF); pkt[11] = (char)((watt >> 8) & 0xFF); pkt[12] = (char)((watt >> 16) & 0xFF);
    pkt[13] = (char)(cf_cnt & 0xFF); pkt[14] = (char)((cf_cnt >> 8) & 0xFF); pkt[15] = (char)((cf_cnt >> 16) & 0xFF);
    pkt[16] = (char)(freq & 0xFF); pkt[17] = (char)((freq >> 8) & 0xFF);
    u32 s = 0x58;
    for (int i = 0; i < 22; i++)
        s += (u8)pkt[i];
    pkt[22] = (char)((s & 0xFF) ^ 0xFF);
    return pkt;
}

Bytes BekenEmulator::bl0942_reply_reg(u8 reg)
{
    u32 val = 0;
    if (reg == 0x03 || reg == 0x05) {
        val = (u32)py_round(meter_a_ * 251210.0);
        if (val > 0xFFFFFF) val = 0xFFFFFF;
    } else if (reg == 0x04) {
        val = (u32)py_round(meter_v_ * 15188.0);
        if (val > 0xFFFFFF) val = 0xFFFFFF;
    } else if (reg == 0x06) {
        val = (u32)py_round(meter_w_ * 598.0);
        if (val > 0x7FFFFF) val = 0x7FFFFF;
    } else if (reg == 0x07) {
        val = (u32)(state.bl0942_polls * 10);
        if (val < 1) val = 1;
        if (val > 0xFFFFFF) val = 0xFFFFFF;
    } else if (reg == 0x08) {
        val = 1000000 / 50;
    } else {
        std::map<u8, u32>::const_iterator it = state.bl0942_regs.find(reg);
        if (it != state.bl0942_regs.end())
            val = it->second & 0xFFFFFFu;
        else if (reg == 0x19)
            val = 0x000087;
    }
    u8 d0 = (u8)(val & 0xFF);
    u8 d1 = (u8)((val >> 8) & 0xFF);
    u8 d2 = (u8)((val >> 16) & 0xFF);
    u8 cs = (u8)(((0x58u + reg + d0 + d1 + d2) & 0xFFu) ^ 0xFFu);
    Bytes pkt = bytes_n(4, 0);
    pkt[0] = (char)d0;
    pkt[1] = (char)d1;
    pkt[2] = (char)d2;
    pkt[3] = (char)cs;
    return pkt;
}

Bytes BekenEmulator::bl0942_react_byte(u8 b_val)
{
    if (!state.bl0942_cmd_buf.empty() && (u8)state.bl0942_cmd_buf[0] == 0xA8) {
        state.bl0942_cmd_buf += (char)b_val;
        if (state.bl0942_cmd_buf.size() >= 6) {
            u8 reg = (u8)state.bl0942_cmd_buf[1];
            u8 d0 = (u8)state.bl0942_cmd_buf[2];
            u8 d1 = (u8)state.bl0942_cmd_buf[3];
            u8 d2 = (u8)state.bl0942_cmd_buf[4];
            u8 cs = (u8)state.bl0942_cmd_buf[5];
            state.bl0942_cmd_buf.clear();
            u8 want_cs = (u8)(((0xA8u + reg + d0 + d1 + d2) & 0xFFu) ^ 0xFFu);
            if (cs == want_cs) {
                state.bl0942_active = true;
                u32 val = (u32)d0 | ((u32)d1 << 8) | ((u32)d2 << 16);
                if (reg == 0x1D) {
                    state.bl0942_regs[0x1D] = (val == 0x55) ? 1 : 0;
                } else if (state.bl0942_regs[0x1D] == 1) {
                    if (reg == 0x1C && val == 0x5A5A5A) {
                        state.bl0942_regs.clear();
                        state.bl0942_regs[0x19] = 0x000087;
                    } else {
                        state.bl0942_regs[reg] = val;
                    }
                }
            }
        }
        return Bytes();
    }
    if (state.uart1_tx_prev == 0x58) {
        if (b_val == 0xAA) {
            state.bl0942_active = true;
            state.bl0942_polls++;
            uart1_rx_delay_ = 0;
            return bl0942_reply_packet();
        }
        if (state.bl0942_active && b_val >= 0x01 && b_val <= 0x20) {
            state.bl0942_polls++;
            uart1_rx_delay_ = 0;
            return bl0942_reply_reg(b_val);
        }
    }
    if (state.bl0942_cmd_buf.empty() && b_val == 0xA8) {
        state.bl0942_cmd_buf = Bytes(1, (char)0xA8);
    }
    return Bytes();
}

u64 BekenEmulator::timer_period(const SimState &s, int idx) const
{
    int bank = idx / 3;
    u32 ctl = bank ? s.timer3_5_ctl : s.timer0_2_ctl;
    u64 divider = ((ctl >> 3) & 0xF) + 1;
    u64 source_hz = bank ? 32000ULL : 26000000ULL;
    u64 p = (u64)s.hw_timer_counter[idx] * divider * TIMER_INSNS_PER_SEC / source_hz;
    return p < 1 ? 1 : p;
}

void BekenEmulator::timer_enable_changed(SimState &s, int bank, u32 old_enable, u32 new_ctl)
{
    if (!chip->is_timer_v2())
        return;
    u32 enable = new_ctl & 0x7;
    for (int n = 0; n < 3; n++) {
        int idx = 3 * bank + n;
        if (enable & (1u << n)) {
            if (!(old_enable & (1u << n)))
                s.hw_timer_due[idx] = s.insn_count + timer_period(s, idx);
        } else {
            s.hw_timer_due[idx] = 0;
        }
    }
}

void BekenEmulator::tick_hw_timers(SimState &s)
{
    u64 now = s.insn_count;
    for (int idx = 0; idx < 6; idx++) {
        u64 due = s.hw_timer_due[idx];
        if (!due || now < due)
            continue;
        u32 status = 1u << (7 + (idx % 3));
        if (idx < 3)
            s.timer0_2_ctl |= status;
        else
            s.timer3_5_ctl |= status;
        u64 period = timer_period(s, idx);
        s.hw_timer_due[idx] = (due + period > now) ? (due + period) : (now + period);
    }
}

u64 BekenEmulator::next_timer_due(const SimState &s) const
{
    u64 best = 0;
    for (int idx = 0; idx < 6; idx++) {
        u64 due = s.hw_timer_due[idx];
        if (due && (!best || due < best))
            best = due;
    }
    return best;
}
