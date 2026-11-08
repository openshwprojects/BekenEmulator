/* Real firmware acceptance tests. Unlike image-loader tests, these require
 * guest-executed UART milestones and continued execution after initialization.
 * Missing fixtures and unimplemented hardware are failures, never passes. */
#include "esp/esp_machine.h"
#include "esp/esp_fixtures.h"
#include "esp/esp_acceptance.h"
#include "crypto_prims.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

namespace {
bool run_fixture(const std::string &root, const EspFixture &fixture,
                 u64 budget, double timeout, bool bootloader, bool trace)
{
    Bytes flash;
    std::string error;
    if (!read_file(path_join(root, fixture.relative_path), flash)) {
        printf("[FAIL] %s: fixture missing under %s\n", fixture.label, root.c_str());
        return false;
    }
    if (hexstr(sha256(flash)) != fixture.sha256) {
        printf("[FAIL] %s: fixture SHA-256 mismatch\n", fixture.label);
        return false;
    }
    esp::FlashImage layout;
    if (!esp::parse_flash(flash, fixture.chip, layout, &error)) {
        printf("[FAIL] %s: %s\n", fixture.label, error.c_str());
        return false;
    }
    EspMachine machine(fixture.chip);
    if(!machine.set_psram_size(fixture.psram_mb*0x100000)) {
        printf("[FAIL] %s: unsupported board PSRAM capacity\n",fixture.label);return false;
    }
    if (!machine.load(flash, layout, bootloader, &error)) {
        printf("[FAIL] %s: %s\n", fixture.label, error.c_str());
        return false;
    }
    std::string rom_path = env_get(fixture.chip == esp::CHIP_ESP32 ? "ESP32_ROM" : "ESP8266_ROM");
    if (!rom_path.empty()) {
        Bytes rom;
        if (!read_file(rom_path, rom) || !machine.load_rom(rom, &error)) {
            printf("[FAIL] %s: supplied ROM could not be loaded: %s\n", fixture.label, error.c_str());
            return false;
        }
    }
    u32 history[2][64] = {{0}};
    u64 steps = 0;
    EspAcceptance acceptance(fixture.boot_marker,fixture.app_marker,bootloader,fixture.uart_command[0]!=0);
    EspButtonAcceptance button;
    XtensaResult result = XTENSA_OK;
    double started = mono_time();
    for (; steps < budget; ++steps) {
        if(trace && steps && steps%10000000==0) {
            printf("[progress] %s: instructions=%llu us=%llu core=%u PC=%08x boot=%s app=%s uart=%u\n",
                fixture.label,(unsigned long long)machine.instructions(),
                (unsigned long long)machine.elapsed_us(),machine.active_core(),
                machine.cpu(machine.active_core()).pc(),acceptance.boot_seen()?"yes":"no",
                acceptance.app_seen()?"yes":"no",(unsigned)machine.uart().size());
            fflush(stdout);
        }
        if(trace)for(unsigned core=0;core<machine.core_count();++core)
            history[core][steps % 64] = machine.cpu(core).pc();
        result = machine.step();
        if (result != XTENSA_OK) break;
        acceptance.observe(machine.uart(),machine.instructions(),machine.elapsed_us(),machine.reset_count());
        /* Query-capable firmware must actually schedule its UART task before
         * it can produce the required reply. Retry after each million steps
         * while firmware initializes its UART task. */
        if (fixture.uart_command[0] && steps > 0 && steps % 1000000 == 0) {
            machine.receive_uart(0, fixture.uart_command);
            acceptance.query_sent(machine.uart().size());
        }
        if(fixture.button_pin>=0) {
            bool input=false,output=false,enabled=false;
            bool relay_input=false,relay_output=false,relay_enabled=false;
            if(!machine.gpio_state(fixture.button_pin,input,output,enabled) ||
               !machine.gpio_state(fixture.relay_pin,relay_input,relay_output,relay_enabled)) {
                error="board fixture refers to an unavailable GPIO";break;
            }
            EspButtonAcceptance::Action action=button.observe(acceptance.app_seen(),input&&!enabled,
                relay_enabled,relay_output,machine.elapsed_us(),machine.reset_count());
            if(action!=EspButtonAcceptance::NONE &&
               !machine.set_gpio_input(fixture.button_pin,action==EspButtonAcceptance::PRESS?0:-1)) {
                error="could not drive the board button";break;
            }
            if(trace && action!=EspButtonAcceptance::NONE) {
                printf("[button] %s: %s GPIO%d at %llu us, relay GPIO%d=%u\n",fixture.label,
                    action==EspButtonAcceptance::PRESS?"press":"release",fixture.button_pin,
                    (unsigned long long)machine.elapsed_us(),fixture.relay_pin,relay_output?1:0);
                fflush(stdout);
            }
            if(button.failed()) {error="short button press produced no relay response";break;}
        }
        if (acceptance.complete(machine.instructions(),machine.elapsed_us()) &&
            (fixture.button_pin<0 || button.complete())) break;
        if (!(steps&1023) && mono_time() - started > timeout) {
            error = "wall-clock deadline exceeded";
            break;
        }
    }
    bool passed = error.empty() && result == XTENSA_OK &&
        acceptance.complete(machine.instructions(),machine.elapsed_us()) &&
        (fixture.button_pin<0 || button.complete());
    printf("[%s] %s (%s): boot=%s app=%s button=%s instructions=%llu ROM=%llu resets=%u core=%u PC=%08x us=%llu uart=%u\n",
        passed ? "PASS" : "FAIL", fixture.label, bootloader ? "bootloader" : "application",
        acceptance.boot_seen() ? "yes" : "no", acceptance.app_seen() ? "yes" : "no",
        fixture.button_pin<0 ? "n/a" : (button.complete()?"yes":"no"),
        (unsigned long long)machine.instructions(), (unsigned long long)machine.rom_calls(),
        machine.reset_count(),machine.active_core(),machine.cpu(machine.active_core()).pc(),
        (unsigned long long)machine.elapsed_us(),(unsigned)machine.uart().size());
    if (!passed) {
        if (!machine.diagnostic().empty()) printf("  %s\n", machine.diagnostic().c_str());
        else printf("  %s (result=%d, opcode=%06x)\n",
            error.empty() ? "Required runtime milestone was not reached" : error.c_str(),
            (int)result, machine.cpu(machine.active_core()).last_instruction());
        if (trace) {
            u64 first = steps < 63 ? 0 : steps - 63;
            for (u64 i = first; i <= steps; ++i) {
                printf("  PC[%llu] core0=%08x", (unsigned long long)i, history[0][i % 64]);
                if(machine.core_count()>1)printf(" core1=%08x",history[1][i % 64]);
                printf("\n");
            }
        }
    }
    fflush(stdout);
    return passed;
}
}

int main(int argc, char **argv)
{
    std::string root = env_get("ESP_FLASH_DUMPS"), filter;
    /* Tuya board drivers initialize after the SDK banner; allow time for
     * real button/relay interaction. ESP32 also runs at a higher clock. */
    u64 steps = 500000000;
    double timeout=600;
    bool bootloader = true, trace = false;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument == "--app") bootloader = false;
        else if (argument == "--trace") trace = true;
        else if (argument == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (argument == "--timeout" && i + 1 < argc) {
            long long n;
            if(!parse_int(argv[++i],10,n) || n<=0) {
                fprintf(stderr,"--timeout requires a positive number of seconds\n");return 2;
            }
            timeout=(double)n;
        }
        else if (argument == "--steps" && i + 1 < argc) {
            long long n;
            if (!parse_int(argv[++i], 0, n) || n <= 0) {
                fprintf(stderr, "--steps requires a positive instruction budget\n"); return 2;
            }
            steps = (u64)n;
        } else if (!argument.empty() && argument[0] != '-' && root.empty()) root = argument;
        else {
            fprintf(stderr, "Usage: esp_firmware [FlashDumps-root] [--app] [--filter text] [--steps N] [--timeout seconds] [--trace]\n");
            return 2;
        }
    }
    if (root.empty()) root = "../FlashDumps";
    unsigned ran = 0, failed = 0;
    for (unsigned i = 0; i < ESP_FIXTURE_COUNT; ++i) {
        if (!filter.empty() && !contains(ESP_FIXTURES[i].label, filter)) continue;
        ++ran;
        if (!run_fixture(root, ESP_FIXTURES[i], steps, timeout, bootloader, trace)) ++failed;
    }
    printf("%u real-firmware tests, %u failed.\n", ran, failed);
    return !ran || failed ? 1 : 0;
}
