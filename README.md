# BekenEmulator

Guide: https://www.elektroda.com/news/news4185451.html

A standalone C++ (`gnu++98`) Beken SoC (BK7231T, BK7231N, BK7231M, BK7231U, BK7238, BK7252, BK7252N) hardware and CPU emulator with its own ARM (A32) and Thumb/Thumb-2 interpreter core, peripheral models, RivieraWaves BLE and Wi-Fi MAC emulation, and user-mode IP/cloud stack. Zero external library dependencies (only the C++ standard library and Win32 / POSIX APIs).

## Architecture

| Layer | Files | Description |
|---|---|---|
| Foundation | `common`, `platform`, `json`, `crypto_prims` | Byte/string utilities, threads/sockets/subprocesses, JSON parser/formatter, AES/GCM/SHA/HMAC/PBKDF2 |
| Image / Cipher | `beken_crypto`, `chips` | CRC16 stripes, Beken flash stream cipher, RBL partition layout, SoC definitions |
| ARM CPU Core | `arm_cpu`, `arm_exec`, `arm_thumb`, `arm_thumb2` | Full ARM (A32) + Thumb/Thumb-2 interpreter, banked registers, exceptions |
| Peripherals | `gpio`, `periph`, `tuyamcu`, `blecore`, `wpa2` | GPIO/PWM/SARADC decode, TuyaMCU UART1 codec+MCU, RivieraWaves BLE 5.x core, WPA2 |
| SoC & MMIO | `emulator*` | ICU/timers (BK7231 & BK7252), UART1/UART2, flash & SPI controllers, SARADC, PWM, WDT, deep sleep, camera/EJPEG, GDMA |
| Wi-Fi & Network | `wifimac`, `wifipeer`, `netstack`, `tuya_config` | RivieraWaves nxmac MAC, virtual 802.11 AP/STA (open + WPA2-PSK), user-mode IP stack (ARP, DHCP, DNS, NTP, ICMP, TCP, HTTP, MQTT, TLS-PSK-GCM, `--net-forward`, PCAP capture), Tuya flash config decoder |
| Front Ends | `cli` (`bekensim`), `tools/sim_ui` (Win32 GUI), `tools/make_obk_config` | CLI runner, interactive Win32 GUI, OpenBeken config image tool |
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
