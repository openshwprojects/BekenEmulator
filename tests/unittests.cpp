/* Module-level unit tests: runs every self-test the ported modules expose.
 * Each *_selftests() returns the number of checks it ran and aborts (returns
 * < 0, or prints a [FAIL] line) on a mismatch. Mirrors the per-module checks
 * that the Python tree runs under tests/.
 *
 *   unittests        run them all; non-zero exit on any failure
 */
#include "common.h"
#include "beken/gpio.h"
#include "beken/periph.h"
#include "tuyamcu.h"
#include "beken/blecore.h"
#include "wpa2.h"
#include "beken/wifimac.h"
#include "wifipeer.h"
#include "netstack.h"
#include "beken/tuya_config.h"
#include "beken/pytests.h"

#include <stdio.h>
#include <string.h>

int backend_tests();
int esp_image_tests();
int esp_image_dump_tests();
int xtensa_tests();
int run_esp_machine_tests();
int rom_image_tests();
int esp32_mmio_tests();
int esp_timing_tests();
int esp_clocked_device_tests();
int esp8266_radio_tests();
int esp_wifi_acceptance_tests();
int esp_spi_tests();
int esp_interrupt_tests();
int esp_exception_tests();
int esp_uart_tests();
int esp_eri_tests();
int esp32_rng_tests();
int esp_multicore_tests();
int esp_reset_tests();
int esp_psram_tests();
int esp_gpio_tests();
int esp_memory_tests();
int esp_acceptance_tests();
int esp8266_timer_tests();
int esp32_frc_tests();
int esp8266_watchdog_tests();
int esp_aes_tests();
int esp_cache_fault_tests();
int esp8266_wdev_tests();
int esp8266_mac_timers_tests();
int esp_rtc_io_tests();
int esp8266_wifi_dma_tests();
int esp_wifi_tests();
int esp32_analog_tests();
int esp32_phy_tests();
int esp_rf_math_tests();

/* tuya_config_dump_tests takes the repo root; "" = locate firmwares/ itself. */
static int tuya_config_dump_tests_default() { return tuya_config::tuya_config_dump_tests(); }

struct Suite {
    const char *name;
    int (*fn)();
};

int main(int argc, char **argv)
{
    Suite suites[] = {
        {"backends", backend_tests},
        {"esp_image", esp_image_tests},
        {"esp_image_dump", esp_image_dump_tests},
        {"xtensa", xtensa_tests},
        {"esp_machine", run_esp_machine_tests},
        {"esp32_mmio", esp32_mmio_tests},
        {"esp_timing", esp_timing_tests},
        {"esp_clocked_devices", esp_clocked_device_tests},
        {"esp8266_radio", esp8266_radio_tests},
        {"esp_wifi_acceptance", esp_wifi_acceptance_tests},
        {"esp_spi", esp_spi_tests},
        {"esp_interrupts", esp_interrupt_tests},
        {"esp_exceptions", esp_exception_tests},
        {"esp_uart", esp_uart_tests},
        {"esp_eri", esp_eri_tests},
        {"esp32_rng", esp32_rng_tests},
        {"esp_multicore", esp_multicore_tests},
        {"esp_resets", esp_reset_tests},
        {"esp_psram", esp_psram_tests},
        {"esp_gpio", esp_gpio_tests},
        {"esp_memory", esp_memory_tests},
        {"esp_acceptance", esp_acceptance_tests},
        {"esp8266_timers", esp8266_timer_tests},
        {"esp32_frc", esp32_frc_tests},
        {"esp8266_watchdog", esp8266_watchdog_tests},
        {"esp_aes", esp_aes_tests},
        {"esp_cache_faults", esp_cache_fault_tests},
        {"esp8266_wdev", esp8266_wdev_tests},
        {"esp8266_mac_timers", esp8266_mac_timers_tests},
        {"esp_rtc_io", esp_rtc_io_tests},
        {"esp8266_wifi_dma", esp8266_wifi_dma_tests},
        {"esp_wifi", esp_wifi_tests},
        {"esp32_analog", esp32_analog_tests},
        {"esp32_phy", esp32_phy_tests},
        {"esp_rf_math", esp_rf_math_tests},
        {"rom_image", rom_image_tests},
        {"gpio", gpio_selftests},
        {"periph", periph::periph_selftests},
        {"tuyamcu", tuyamcu::tuyamcu_selftests},
        {"blecore", blecore_selftests},
        {"wpa2", wpa2::wpa2_selftests},
        {"wifimac", wifimac_selftests},
        {"wifipeer", wifipeer_selftests},
        {"netstack", netstack::netstack_selftests},
        {"tuya_config", tuya_config::tuya_config_selftests},
        {"tuya_config_dump", tuya_config_dump_tests_default},
        /* unit tests in tests/ */
        {"chips", chips_tests},
        {"timers", timers_tests},
        {"gpio_emulator", gpio_emulator_tests},
        {"image_loader", image_loader_tests},
        {"watchdog", watchdog_tests},
        {"gpio_button", gpio_button_tests},
    };
    int n = (int)(sizeof(suites) / sizeof(suites[0]));
    int total = 0, failed = 0, ran = 0;
    for (int i = 0; i < n; i++) {
        if (argc > 1) {
            bool match = false;
            for (int a = 1; a < argc; a++) {
                if (strcmp(argv[a], suites[i].name) == 0) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
        }
        ran++;
        int checks = suites[i].fn();
        if (checks < 0) {
            printf("[FAIL] %s self-tests\n", suites[i].name);
            failed++;
        } else {
            printf("[ok]   %-18s %d checks\n", suites[i].name, checks);
            total += checks;
        }
    }
    if (ran == 0) {
        printf("[FAIL] no matching suite found\n");
        return 1;
    }
    printf("\n%d checks across %d module suites, %d suite(s) failed.\n",
           total, ran, failed);
    return failed ? 1 : 0;
}
