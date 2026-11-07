/* One struct per emulated SoC; port of src/chips/.
 *
 * --chip selects one of these. Each Chip describes what distinguishes one SoC
 * from another (data only).
 */
#ifndef BK_CHIPS_H
#define BK_CHIPS_H

#include "common.h"

static const u32 MIB = 1024 * 1024;
static const int CRC_BLOCK_PHYS = 34;
static const int CRC_BLOCK_DATA = 32;
static const u32 DEFAULT_FLASH_SIZE = 2 * MIB;

struct Chip {
    const char *name;           /* the --chip key, upper case */
    u32 chip_id;                /* SCTRL chip id register (0x00800000) */
    u32 dev_id;                 /* SCTRL device id register (0x00800004) */
    int gpio_pins;              /* 32, or 40 with the GPIO32..39 bank */
    const char *ble_core;       /* "5.1", "5.2", or "" (None) */
    bool ble_auto;              /* true when BLE 5.x core is modelled without --ble-core */
    u32 flash_size;             /* 0 = None (smallest power of two >= 2 MiB) */
    u32 bootloader_addr;        /* 0x00000000 */
    u32 app_addr;               /* 0x00010000 */
    int app_only_phys;          /* -1 = None, or physical address of app-only dump */
    u32 ram_base;               /* 0x00400000 */
    u32 ram_size;               /* 1 * MIB */
    u32 sdram_base;             /* 0 when None, else 0x00900000 */
    u32 sdram_size;             /* 0 when None, else 0x40000 */
    const char *xvr;            /* "" (None) or "v1" */
    const char *gdma;           /* "v1" or "v2" */
    const char *i2c;            /* "v1" or "v2" */
    const char *ejpeg;          /* "" (None), "v1", or "v2" */
    const char *pwm;            /* "v1" or "v2" */
    const char *audio;          /* "" (None) or "v1" */
    const char *saradc;         /* "v1", "v2", or "v3" */
    const char *timer;          /* "v1" or "v2" */
    bool bktimer_v2;            /* timer 3-5 interrupt status layout (timer v1) */
    bool rtthread_console;      /* hold console input until RT-Thread's msh /> */
    bool usb_plug_wake;         /* SCTRL_USB_PLUG_WAKEUP reports 5V present */

    int app_only_cpu() const
    {
        return app_only_phys >= 0 ? (app_only_phys / CRC_BLOCK_PHYS * CRC_BLOCK_DATA) : -1;
    }
    bool has_sdram() const { return sdram_size > 0; }
    bool has_xvr() const { return xvr && xvr[0] != '\0'; }
    bool is_gdma_v2() const { return gdma && gdma[0] == 'v' && gdma[1] == '2'; }
    bool is_i2c_v2() const { return i2c && i2c[0] == 'v' && i2c[1] == '2'; }
    bool is_ejpeg_v1() const { return ejpeg && ejpeg[0] == 'v' && ejpeg[1] == '1'; }
    bool is_ejpeg_v2() const { return ejpeg && ejpeg[0] == 'v' && ejpeg[1] == '2'; }
    bool is_pwm_v2() const { return pwm && pwm[0] == 'v' && pwm[1] == '2'; }
    bool has_audio() const { return audio && audio[0] != '\0'; }
    bool is_saradc_v1() const { return saradc && saradc[0] == 'v' && saradc[1] == '1'; }
    bool is_saradc_v2() const { return saradc && saradc[0] == 'v' && saradc[1] == '2'; }
    bool is_saradc_v3() const { return saradc && saradc[0] == 'v' && saradc[1] == '3'; }
    bool is_timer_v1() const { return !is_timer_v2(); }
    bool is_timer_v2() const { return timer && timer[0] == 'v' && timer[1] == '2'; }
};

const Chip &default_chip();
const Chip *find_chip(const std::string &name);
const Chip *get_chip(const std::string &name, std::string *err = 0);
StrList chip_names();
const std::map<std::string, const Chip *> &chips_map();
bool fit_flash(const Bytes &physical, const Chip &chip, Bytes &out, std::string *err = 0);

#endif
