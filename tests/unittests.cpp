/* Module-level unit tests: runs every self-test the ported modules expose.
 * Each *_selftests() returns the number of checks it ran and aborts (returns
 * < 0, or prints a [FAIL] line) on a mismatch. Mirrors the per-module checks
 * that the Python tree runs under tests/.
 *
 *   unittests        run them all; non-zero exit on any failure
 */
#include "common.h"
#include "gpio.h"
#include "periph.h"
#include "tuyamcu.h"
#include "blecore.h"
#include "wpa2.h"
#include "wifimac.h"
#include "wifipeer.h"
#include "netstack.h"
#include "tuya_config.h"
#include "pytests.h"

#include <stdio.h>
#include <string.h>

/* tuya_config_dump_tests takes the repo root; "" = locate firmwares/ itself. */
static int tuya_config_dump_tests_default() { return tuya_config::tuya_config_dump_tests(); }

struct Suite {
    const char *name;
    int (*fn)();
};

int main(int argc, char **argv)
{
    Suite suites[] = {
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
