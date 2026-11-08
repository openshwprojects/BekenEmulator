/* Front-end configuration shared by chip backends. Hardware-specific options
 * are validated by the selected backend, never silently ignored. */
#ifndef EMULATOR_OPTIONS_H
#define EMULATOR_OPTIONS_H

#include "common.h"
#include "tuyamcu.h"

static const u64 CONSOLE_RX_HOLDOFF_INSNS = 2000000;

struct BuildOptions {
    std::string chip;
    std::string rom_path; /* optional silicon ROM ELF, separate from flash */
    unsigned xtal_mhz; /* zero selects the chip's default crystal */
    unsigned psram_mb; /* optional external ESP32 SPI RAM, absent by default */
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
        : chip("BK7231"), xtal_mhz(0), psram_mb(0), with_boot(false), only_uart(false), uart1_hex(false),
          uart1_rx_delay(CONSOLE_RX_HOLDOFF_INSNS), uart1_rx_delay_set(false),
          tuyamcu(false), tuyamcu_raw(false), xvr_selfclear(false), ble_core(-1), wifi(false) {}
};

#endif
