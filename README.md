# BekenEmulator

Guide: https://www.elektroda.com/news/news4185451.html

A standalone C++ (`gnu++98`) Beken SoC (BK7231T, BK7231N, BK7231M, BK7231U, BK7238, BK7252, BK7252N) hardware and CPU emulator with its own ARM (A32) and Thumb/Thumb-2 interpreter core, peripheral models, RivieraWaves BLE and Wi-Fi MAC emulation, and user-mode IP/cloud stack. Zero external library dependencies (only the C++ standard library and Win32 / POSIX APIs).

## Architecture

Chip hardware and CPU interpreters are organized separately:

- `src/beken/`: Beken SoCs, peripherals, flash cipher and backend.
- `src/esp8266/` and `src/esp32/`: chip-specific registers and peripherals.
- `src/esp/`: shared Espressif machine, backend, image loader and device helpers.
- `src/arch/arm/` and `src/arch/xtensa/`: CPU instruction interpreters.
- `src/`: shared frontend interfaces, utilities, protocols and network services.

Tests follow the same grouping under `tests/beken/`, `tests/esp8266/`,
`tests/esp32/`, `tests/esp/` and `tests/arch/xtensa/`. Shared test runners and
backend tests remain in `tests/`. Module paths below omit `.cpp` / `.h` suffixes.

| Layer | Files | Description |
|---|---|---|
| Foundation | `src/common`, `src/platform`, `src/json`, `src/crypto_prims` | Byte/string utilities, threads/sockets/subprocesses, JSON parser/formatter, AES/GCM/SHA/HMAC/PBKDF2 |
| Image / Cipher | `src/beken/beken_crypto`, `src/beken/chips` | CRC16 stripes, Beken flash stream cipher, RBL partition layout, SoC definitions |
| Backend boundary | `src/emulator_backend`, `src/emulator_options`, `src/beken/beken_backend`, `src/esp/esp_backend` | Shared construction, execution, UART, GPIO inputs and diagnostics; chip-family option validation |
| Espressif images | `src/esp/esp_image`, `src/rom_image`, `src/esp/esp_rom` | ESP8266 v1/v2 and classic ESP32 image/partition parsing, checksums, optional silicon ROM ELF |
| Xtensa CPU | `src/arch/xtensa/xtensa_cpu` | Shared LX106/LX6 interpreter with density instructions, windowed calls, exceptions and explicit unsupported-instruction faults |
| Espressif hardware | `src/esp/esp_machine`, `src/esp8266/esp8266_mmio`, `src/esp32/esp32_mmio`, `src/esp/esp_spi`, `src/esp/esp_uart`, `src/esp32/esp32_interrupts` | Shared memory/ROM, UART and NOR flash, separate chip registers, interrupt matrix and hardware timers |
| Legacy ESP timers | `src/esp/esp_frc_timers`, `src/esp8266/esp8266_timers` | Shared FRC counter math with separate chip address maps, interrupt routing and acknowledgment rules |
| ESP8266 watchdog | `src/esp8266/esp8266_watchdog` | Clocked interrupt/reset stages, feed operations and reset delivery during CPU waits |
| ESP8266 MAC | `src/esp8266/esp8266_wdev`, `src/esp8266/esp8266_mac_timers` | Interrupt latches, key RAM, receive configuration and TSF alarm delivery during CPU waits; packet transport is still in development |
| ESP8266 legacy Wi-Fi DMA | `src/esp8266/esp8266_wifi_dma`, `src/esp/esp_wifi` | Guest descriptor chains, timed CCK/OFDM transmission and reception, CRC/FCS, address matching, completion interrupts and explicit peer ACK outcomes |
| ESP8266 radio state | `src/esp8266/esp8266_radio` | Channel decoding, RF/baseband readiness and tuning/gating changes observed from modeled registers |
| ESP32 analog / receiver controls | `src/esp32/esp32_analog`, `src/esp32/esp32_phy` | BBPLL transactions, indexed PHY trim RAM, and separate AGC/CCA configuration and reset state |
| Radio arithmetic | `src/esp/esp_rf_math` | Shared PLL divider decoding and configurable oscillator tuning curves, independent of chip registers |
| ESP32 RTC pads | `src/esp32/esp32_rtc_io` | Pad mux, shared pulls, RTC GPIO and latched hold state across digital reset |
| ESP32 AES | `src/esp32/esp_aes`, `src/crypto_prims` | AES-128/192/256 encrypt/decrypt, independent key/input/output byte orders, clock gating and reset |
| ESP32 cache faults | `src/esp32/esp32_cache` | Per-core cache fault status, MMU diagnostics and interrupt routing |
| ESP device state | `src/esp/esp_state` | Reserved private state ranges shared by map-backed device models |
| ESP32 multiprocessing | `src/esp/esp_multicore`, `src/esp32/esp_eri` | Independent LX6 contexts and ROM views, shared SRAM/clock, per-core interrupts and boot remapping, APP reset/release/stall and debug registers |
| ESP reset lifecycle | `src/esp/esp_reset` | Deferred CPU, digital-system and RTC reset domains; retained RAM, external devices and UART transcript |
| External ESP32 RAM | `src/esp32/esp_psram`, `src/esp32/esp32_psram_mmu` | Configurable SPI PSRAM device, QPI commands and per-core memory mapping |
| ARM CPU Core | `src/arch/arm/arm_cpu`, `src/arch/arm/arm_exec`, `src/arch/arm/arm_thumb`, `src/arch/arm/arm_thumb2` | Full ARM (A32) + Thumb/Thumb-2 interpreter, banked registers, exceptions |
| Peripherals | `src/beken/gpio`, `src/beken/periph`, `src/tuyamcu`, `src/beken/blecore`, `src/wpa2` | GPIO/PWM/SARADC decode, TuyaMCU UART1 codec+MCU, RivieraWaves BLE 5.x core, WPA2 |
| SoC & MMIO | `src/beken/emulator*` | ICU/timers (BK7231 & BK7252), UART1/UART2, flash & SPI controllers, SARADC, PWM, WDT, deep sleep, camera/EJPEG, GDMA |
| Wi-Fi & Network | `src/beken/wifimac`, `src/wifipeer`, `src/netstack`, `src/beken/tuya_config` | RivieraWaves nxmac MAC, virtual 802.11 AP/STA (open + WPA2-PSK), user-mode IP stack (ARP, DHCP, DNS, NTP, ICMP, TCP, HTTP, MQTT, TLS-PSK-GCM, `--net-forward`, PCAP capture), Tuya flash config decoder |
| Front Ends | `src/cli` (`bekensim`), `tools/sim_ui` (Win32 GUI), `tools/make_obk_config` | CLI runner, interactive Win32 GUI, OpenBeken config image tool |
| Test Suite | `tests/unittests`, `tests/self_test`, `tests/testcase_data` | Module unit tests and full firmware regression suite |

## Build

Requires a C++ compiler (`g++` on Linux or MSYS2 MinGW-w64 `g++` on Windows):

```sh
make            # bekensim(.exe), the CLI emulator
make tests      # unittests(.exe) and self_test(.exe)
make tools      # make_obk_config(.exe) and sim_ui.exe (Windows)
make all        # build all targets
make check      # build and run module unit tests
```

## Run

```sh
./bekensim firmwares/OpenBK7231T_QIO_1.18.300_mathDemo.bin --only-uart -key TUYA
```

## Tests

```sh
./unittests                     # run all module unit test suites
./sim_ui.exe --selftest         # run the Win32 simulator UI self-tests (Windows)
./self_test -j 6                # run all firmware test cases and generate report/index.html
./self_test -k "MathDemo"       # run a filtered subset of firmware test cases
```

ESP32-specific CI suites are temporarily commented out in
`.github/workflows/test.yml` during bring-up; `./unittests` still runs them locally.

## Espressif backends (in development)

The CLI and desktop chip selector share the same backend registry. In the CLI:

```sh
./bekensim firmware.bin --chip ESP8266 --with-boot
./bekensim firmware.bin --chip ESP32 --with-boot
./bekensim firmware.bin --chip ESP32 --with-boot --rom /path/to/esp32_rev0_rom.elf
./bekensim Sonoff-NSPanel.bin --chip ESP32 --with-boot --psram-mb 8 --rom /path/to/esp32_rev300_rom.elf
```

`--with-boot` executes the dump's bootloader. Without it, the loader places the
application segments and starts its entry point. `--rom` optionally maps real
mask-ROM instructions from an Espressif ROM ELF or an ESP8266 64 KiB raw ROM
binary. The flash dump does not contain
that silicon ROM. `EMU_MAX_INSNS` limits a diagnostic run, and `--trace-pc` reports
execution of specified addresses. ESP8266 uses Xtensa LX106; `ESP32` means the
original Xtensa LX6 chip. S2/S3 and RISC-V family images are rejected by chip ID.
`--xtal-mhz` selects the board crystal: 24/26/40 MHz for ESP8266 (default 26),
or 26/40 MHz for ESP32 (default 40). This drives clock calibration and timing.
`--psram-mb` adds 2, 4 or 8 MiB of external SPI PSRAM to an ESP32 board; it is
absent by default. The NSPanel test fixture requires external PSRAM. Its SPI ID,
configuration and memory accesses are handled by the peripheral model.

For recognized classic ESP32 ROM layouts, the loader derives the APP CPU's
private ROM pointers from the supplied PRO ELF. Both CPUs then execute their
own ROM view with separate startup stacks. ROM ELF images are available from
[Espressif's ROM repository](https://github.com/espressif/esp-rom-elfs).
SRAM aliases, boot remapping and RTC memory ownership follow the
[ESP32 memory map](https://documentation.espressif.com/esp32_technical_reference_manual_en.pdf).

The desktop UI also supports ESP UART output/input, GPIO inputs, pause, stop,
and instruction/PC status. Its Config tab accepts the optional silicon ROM image
and board crystal frequency and PSRAM capacity.
UART0 appears in the primary console, with UART1/UART2 output in the second pane.

The stock firmware acceptance tests are still being brought up. A validated
image or a bootloader banner does **not** establish that its application boots.
Unsupported hardware and instructions produce explicit failures. ESP8266 Wi-Fi
registers and packet DMA have unit and guest receive tests, but connecting the
ESP radio to the virtual network remains unfinished. ESP snapshots and several
peripheral controls are not implemented yet.

```sh
./unittests backends esp_image esp_image_dump xtensa esp_machine esp32_mmio esp_timing esp_spi esp_interrupts esp_exceptions esp_uart esp_eri esp32_rng esp_multicore esp_memory esp_resets esp_psram esp_gpio rom_image
ESP_FLASH_DUMPS=/path/to/FlashDumps make check-esp-dumps
./esp_firmware /path/to/FlashDumps --app --filter "Tuya BW-SHP6" --trace
```

On PowerShell, set `$env:ESP_FLASH_DUMPS = 'W:/GIT/FlashDumps'` before running the
test executable. The fixture manifest in `tests/esp/esp_fixtures.h` pins two ESP8266
Tuya dumps and an ESP32 Sonoff NSPanel dump by SHA-256. The dedicated
`esp_firmware` tests require actual UART boot/application milestones followed by
at least 100,000 guest instructions and one simulated second without another
reset. Query-based fixtures must answer a fresh command after their first
recognizable banner or reply. The Tuya plugs must also initialize their button
and relay GPIOs and change the relay after a simulated 200 ms button press.
The button is released to the guest-configured pull-up; startup output changes
do not satisfy this interaction check. Execution must continue for one simulated
second after release. A reset discards previously observed milestones
and outstanding queries. Missing or changed fixtures fail. Set `ESP32_ROM` or
`ESP8266_ROM` to supply a silicon ROM image to these tests. Firmware bytes remain external.
The runner defaults to 500 million steps and 600 seconds per sample; `--steps`
and `--timeout` adjust those limits. `--trace` records both ESP32 program counters.

The radio calibration peripherals use deterministic functional models. ESP8266
idle I/Q samples vary by at most three ADC units, and PLL calibration takes
20 simulated microseconds with a tuning result derived from the programmed
divider and board crystal. These values let the guest perform its calibration
algorithm reproducibly; they are not measurements of physical RF performance
or a source of cryptographic entropy.
