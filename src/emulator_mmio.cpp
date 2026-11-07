/* MMIO read/write handlers and the I2C/camera/GDMA/EJPEG helpers. Port of
 * hook_mem_write_mmio / hook_mem_read_mmio and friends from src/emulator.py. */
#include "emulator.h"
#include "blecore.h"
#include "wifimac.h"
#include "periph.h"

#include <string.h>

/* register addresses */
static const u32 ICU_INT_STATUS = 0x0080204c;
static const u32 ICU_INT_RAW_STATUS = 0x00802048;
static const u32 ICU_INT_ENABLE = 0x00802040;
static const u32 ICU_GLOBAL_INT_EN = 0x00802044;
static const u32 ICU_PERI_CLK_PWD = 0x00802008;
static const u32 ICU_WDT_CLK_PWD = 1u << 8;
static const u32 WDT_CTRL = 0x00802900;
static const u32 UART1_FIFO_PORT = 0x0080210c;
static const u32 UART1_FIFO_STATUS = 0x00802108;
static const u32 UART2_FIFO_PORT = 0x0080220c;
static const u32 UART2_FIFO_STATUS = 0x00802208;
static const u32 TRNG_DATA = 0x00802484;
static const u32 GPIO_BASE = 0x00802800;
static const u32 GDMA_BASE = 0x00809000;
static const u32 GDMA_END = GDMA_BASE + 0x240;
static const u32 GDMA_EN = 1u << 0;
static const u32 GDMA_SRCADDR_INC = 1u << 8;
static const u32 GDMA_DSTADDR_INC = 1u << 9;
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


/* -------------------------------------------------------- bit-banged I2C LED */
void BekenEmulator::i2c_pin_edge(int pin, u32 old_cfg, u32 new_cfg)
{
    int old_lv = (!(old_cfg & 0x08) && !(old_cfg & 0x02)) ? 0 : 1;
    int new_lv = (!(new_cfg & 0x08) && !(new_cfg & 0x02)) ? 0 : 1;
    if (pin != state.i2c_last_pin) {
        if (state.i2c_last_pin >= 0) {
            std::pair<int, int> pr(pin < state.i2c_last_pin ? pin : state.i2c_last_pin,
                                   pin < state.i2c_last_pin ? state.i2c_last_pin : pin);
            int cnt = ++state.i2c_pair_counts[pr];
            if (cnt == 32) {
                state.i2c_buses[std::make_pair(pr.first, pr.second)] = SimState::I2cBus();
                state.i2c_buses[std::make_pair(pr.second, pr.first)] = SimState::I2cBus();
            }
        }
        state.i2c_last_pin = pin;
    }
    if (state.i2c_buses.empty() || old_lv == new_lv)
        return;
    for (std::map<std::pair<int, int>, SimState::I2cBus>::iterator it = state.i2c_buses.begin();
         it != state.i2c_buses.end(); ++it) {
        int scl = it->first.first, sda = it->first.second;
        SimState::I2cBus &bs = it->second;
        if (pin == sda) {
            std::map<int, u32>::const_iterator sit = state.gpio_cfg.find(gpio_slot(scl));
            u32 scl_cfg = (sit != state.gpio_cfg.end()) ? sit->second : 0x3C;
            int scl_lv = (!(scl_cfg & 0x08) && !(scl_cfg & 0x02)) ? 0 : 1;
            if (scl_lv == 1) {
                if (old_lv == 1 && new_lv == 0) {
                    bs.in_tx = true;
                    bs.cur_byte = 0;
                    bs.bits = 0;
                    bs.frame.clear();
                } else if (old_lv == 0 && new_lv == 1) {
                    if (bs.in_tx && bs.frame.size() >= 2) {
                        Bytes raw = bs.frame;
                        periph::LedDecode dec = periph::decode_i2c_led(scl, sda, raw);
                        if (dec.valid) {
                            state.i2c_frames[std::make_pair(scl, sda)] = raw;
                            if (dec.is_lit)
                                state.i2c_last_on[std::make_pair(scl, sda)] = raw;
                            if (emit_insns_)
                                state.i2c_dirty.insert(std::make_pair(scl, sda));
                        }
                    }
                    bs.in_tx = false;
                }
            }
        } else if (pin == scl && bs.in_tx) {
            if (old_lv == 0 && new_lv == 1) {
                std::map<int, u32>::const_iterator dit = state.gpio_cfg.find(gpio_slot(sda));
                u32 sda_cfg = (dit != state.gpio_cfg.end()) ? dit->second : 0x3C;
                int sda_lv = (!(sda_cfg & 0x08) && !(sda_cfg & 0x02)) ? 0 : 1;
                if (bs.bits < 8) {
                    bs.cur_byte = ((bs.cur_byte << 1) | sda_lv) & 0xFF;
                    bs.bits++;
                } else {
                    bs.frame += (char)(u8)bs.cur_byte;
                    bs.cur_byte = 0;
                    bs.bits = 0;
                }
            }
        }
    }
}
