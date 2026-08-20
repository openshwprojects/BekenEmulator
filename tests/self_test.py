import subprocess
import threading
import time
import os
import re
import sys
from datetime import datetime, timezone

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

# How many times a periodic line must repeat for the "1s timers" cases.
# Two is the sweet spot: a second tick already proves the timer REPEATS rather
# than merely starting, and each extra tick costs real wall time (emulation runs
# far slower than the clock - about 4 ticks per 90s here). Same everywhere so CI
# and local runs assert the same thing; override with REPEATS=n to dig deeper.
REPEATS = int(os.environ.get("REPEATS") or 2)

# Seconds to keep the emulator running AFTER a test has met all its markers.
# The verdict is already decided at that point, so this changes no pass/fail -
# it just keeps capturing output, giving the report more of the boot than the
# bare minimum, and letting periodic checks show how many ticks actually
# happened (e.g. 7/2) instead of stopping dead on the second one.
# Tune these two; env LINGER=n overrides both.
LINGER_SECONDS_CI = 5.0      # GitHub Actions: spend a little more for richer logs
LINGER_SECONDS_LOCAL = 0.0   # local runs stay as quick as before

LINGER = float(os.environ.get("LINGER") or
               (LINGER_SECONDS_CI if os.environ.get("CI") else LINGER_SECONDS_LOCAL))

# DRY Test definitions.
#
# Ordering matters: the boot tests below run the emulator until their timeout
# expires (the emulator never exits on its own), so each one costs its full
# timeout. The CLI tests come first because their process exits in about a
# second - a broken command line then fails the run in seconds instead of
# after twenty minutes of emulation.
TEST_CASES = [
    {
        # Guards parse_key(): an unusable key must be rejected up front with a
        # message naming the accepted forms, not fail deep inside emulation.
        "name": "CLI: invalid -key is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "NOT_A_REAL_KEY"],
        "timeout": 60,
        "expected_strings": [
            "Invalid -key value",
            # The error lists the known key names and the accepted formats.
            "TUYA",
            "32 hex characters"
        ]
    },
    {
        # Guards the -chip lookup: an unknown chip must be rejected and the
        # known identities listed.
        "name": "CLI: invalid -chip is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-chip", "BK9999"],
        "timeout": 60,
        "expected_strings": [
            "Unknown -chip value",
            "BK7238",
            "BK7252N"
        ]
    },
    {
        # Guards the plaintext-vs-encrypted heuristic in crypto.py: running an
        # encrypted image with no key must warn that the slice is not ARM code
        # and suggest the key, instead of silently emulating garbage.
        "name": "CLI: encrypted image without a key warns",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 60,
        "expected_strings": [
            "does not look like ARM code",
            "try: -key TUYA"
        ]
    },
    {
        "name": "OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        # Boot finishes around 28M instructions and each Main_OnEverySecond tick
        # costs a few million more, so waiting for REPEATS ticks needs a longer
        # budget. Streaming stops as soon as the last one is seen.
        "timeout": 360,  # seconds
        "expected_strings": [
            "OpenBK7231T, version 1.18.300",
            "Main_Init_Delay",
            "Info:MQTT:MQTT_RegisterCallback called for",
            # Main_OnEverySecond runs off a FreeRTOS software timer - proves the
            # tick interrupt and the timer daemon task are alive.
            ", idle ",
            # Require the stable per-second line REPEATS times: the "Time N"
            # numbers can skip, but each Main_OnEverySecond prints this line
            # exactly once, so several of them prove the timer keeps firing, not
            # just starts. No WiFi in the emulator, so MQTT stays down and the
            # ping watchdog never starts. (string, count) asks for count matches.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "MathDemo Boot and Float Verification",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300_mathDemo.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 120,
        "expected_strings": [
            "Info:MAIN:Advanced math test started",
            "Info:MAIN:Integer: (123 * 456) / 10 = 5608",
            "Info:MAIN:Float basic: (3.141590 * 1.860000) / (3.141590 - 1.0) = 2.728513",
            "Info:MAIN:Casting: float_to_int = 272, int_to_float = 1684.084106",
            # The mathDemo build logs every QuickTick - proves FreeRTOS software
            # timers fire (the same mechanism that drives Main_OnEverySecond).
            "Info:MAIN:quicktick",
            # No valid OBK config in flash -> default config path (the crafted
            # config test below is the complement: it must NOT print this).
            "CFG_InitAndLoad: Config crc or ident mismatch"
        ]
    },
    {
        # Complement of the case above: the same mathDemo image, but with a
        # hand-crafted mainConfig_t written into the BK_PARTITION_NET_PARAM
        # partition (flash 0x1e1000). This proves the emulated flash controller
        # serves a full 32-byte page per operate-write - OBK pulls the whole
        # 3584-byte config through REG_FLASH_DATA_FLASH_SW (8 reads per page),
        # so a controller that repeats word 0 corrupts the config and it is
        # silently rejected as a crc mismatch.
        #
        # The stored crc byte is Tiny_CRC8 over config[4:sizeof] computed with
        # SIGNED char semantics (arithmetic >>), which is what the firmware's
        # own build does - the unsigned reading of the same code yields a
        # different byte and the config is refused.
        "name": "MathDemo Startup Command: echo",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_echo.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            # Config accepted - the mismatch branch was NOT taken.
            "CFG_InitAndLoad: Correct config has been loaded",
            # initCommandLine (offset 0x5E0) is "echo Test12343242343243";
            # CMD_Echo logs its argument under LOG_FEATURE_CMD.
            "Info:CMD:Test12343242343243"
        ]
    },
    {
        # Same config-injection trick, but the startup command drives OBK's own
        # uartSendHex to put bytes on UART1. This exercises the whole path end
        # to end: config load -> command registration (UART_AddCommands runs in
        # CMD_Init_Delayed, before the startup command is executed) -> HAL uart
        # write -> the emulator's UART1 hex capture. uartSendHex targets
        # UART_PORT_INDEX_0, which maps to BK_UART_1, so the bytes land on the
        # MCU link rather than the UART2 log.
        # This is a plain UART demo - the payload is arbitrary marker bytes, not
        # a TuyaMCU frame; TuyaMCU gets its own image and case.
        "name": "MathDemo Startup Command: uartSendHex on UART1",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartSendHex.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # "backlog uartInit 115200; uartSendHex BADF00D12345"
            "[UART1/MCU] ba df 00 d1 23 45"
        ]
    },
    {
        # "startDriver TuyaMCU" - the driver opens UART1 itself (TuyaMCU_Init
        # calls UART_InitUART(9600)), so no uartInit is needed. It then talks
        # unprompted: TuyaMCU_RunStateMachine_V3 starts with heartbeat_timer==0,
        # so the very first per-second tick emits a HEARTBEAT (cmd 0x00) without
        # the MCU ever having said anything. Frame is built by
        # TuyaMCU_SendCommandWithData: 55 AA <ver 00> <cmd> <lenHi> <lenLo>
        # <checksum>, checksum = 0xFF + cmd + lenHi + lenLo = 0xFF for a
        # zero-length heartbeat.
        "name": "MathDemo Startup Command: startDriver TuyaMCU sends heartbeat",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 240,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Started TuyaMCU.",
            "[UART1/MCU] 55 aa 00 00 00 00 ff"
        ]
    },
    {
        "name": "OpenBK7231U_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231U_QIO_1.18.300.bin"),
        # Plaintext image (beken_freertos_sdk layout) - no key.
        "args": ["--only-uart"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7231U, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7238_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7238_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7238 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7238, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # BK7231N now reaches the per-second Main_OnEverySecond loop. It used
        # to hang there: Main_OnEverySecond's first call reads the chip
        # temperature via the SARADC and blocks on the SARADC interrupt (ICU
        # bit 11), which the emulator now models. N (unlike T) unmasks bit 11,
        # so this is the case that guards the SARADC model.
        "name": "OpenBK7231N_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231N_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7231N, version 1.18.300",
            "calibration_main over",
            "app_init finished",
            # Full OpenBeken init completed.
            "Info:MAIN:Main_Init_After_Delay done",
            # The temperature read (SARADC) inside Main_OnEverySecond completes.
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # BK7231M is the BK7231N build stored UNENCRYPTED (verified: the two app
        # images are byte-identical for their first 828KB, and this one prints
        # the "OpenBK7231N" banner). Runs with no -key - the regression guard
        # for the plaintext path through crypto.py on a BK7231N-family image.
        # With the SARADC model it reaches the per-second loop like N.
        "name": "OpenBK7231M_QIO_1.18.300 Boot to 1s timers (no key)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231M_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 180,
        "expected_strings": [
            # M ships the N build, so the banner really does say 7231N.
            "OpenBK7231N, version 1.18.300",
            "calibration_main over",
            "app_init finished",
            # Full OpenBeken init completed.
            "Info:MAIN:Main_Init_After_Delay done",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7252_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 480,  # heaviest boots; 240s left too little headroom on a slow CI runner
        "expected_strings": [
            "OpenBK7252, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7252N_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252N_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252N chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 480,  # heaviest boots; 240s left too little headroom on a slow CI runner
        "expected_strings": [
            "OpenBK7252N, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # 4MB dump; guards two emulator fixes at once:
        #  - SPI-flash-mirror size cap in setup() (a >2MB image used to map past
        #    RAM_BASE and throw UC_ERR_MAP before any code ran -> 0 output).
        #  - XVR (RF transceiver) 0x900100 transaction register: RF init spins
        #    on its bit-31 "busy" flag; without the model it hangs after
        #    xvr_reg_init, before "enter normal mode".
        "name": "BK7238 Sonoff 4MB Dump Boots (SPI mirror + XVR)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "Sonoff_S61s_EUPlug_WBBK_01P_V1.3.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        # "enter normal mode" lands around the 3-minute mark on a typical
        # machine; keep headroom so the last marker is not timing-flaky.
        "timeout": 240,
        # Markers form a boot ladder so a failure pinpoints the stage:
        "expected_strings": [
            # Proves the 4MB image maps and code executes (SPI mirror cap).
            "bk_misc_init_start_type",
            "[Flash]init over",
            # Early SDK banner of this build.
            "SDK Rev: 3.0.70",
            # Wifi init reached; the -chip BK7238 identity is served.
            "chip id=7238 device id=21128000",
            # Past the XVR transaction wait loop: RF cal completes, the SDK
            # brings up all threads and goes operational.
            "calibration_main over",
            "enter normal mode"
        ]
    },
    {
        # BK7231Q original Tuya firmware (MOES 1-gang relay). Plaintext image,
        # no key. Runs the full Tuya IoT SDK v2.3.1 - unlike the OpenBeken
        # builds this is stock vendor firmware, and it boots far enough to
        # enumerate the device's own GPIO/datapoint map, then init TCP/IP.
        "name": "BK7231Q Tuya MOES Relay Boot",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            "TUYA IOT SDK V:2.3.1",
            # The build identifies itself as a 1-switch BK7231 OEM app.
            "oem_bk7321_bk_1_switch:1.1.3",
            # Device brings up its pin map (relay on pin 18, button on pin 6).
            "IO - relay[0]:",
            "Initializing TCP/IP stack"
        ]
    },
    {
        # BL2028N is BK7231N silicon rebadged: this dump self-reports
        # "chip id=7231a device id=18520001" - the exact BK7231 default identity
        # - so it runs with no -key and the default -chip. It is the ONLY test
        # that reaches BLE/HCI bring-up, so it also guards the XVR fix on the
        # BLE path. (Ordinary Tuya BL2028N dumps are plaintext; the Uascent
        # Matter builds are custom-keyed and are intentionally not used here.)
        "name": "BL2028N (=BK7231N) Boots to BLE init",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BL2028N_Dreo_DR-HTF004S_Fan_PAI-053.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            # Proves BL2028N presents as BK7231N.
            "chip id=7231a device id=18520001",
            "calibration_main over",
            # Reaches BLE host-stack init - deeper than any other case, and only
            # possible because the XVR transaction register is modelled.
            "rwble_hl_init ok",
            "Initializing TCP/IP"
        ]
    },
    {
        # Original Tuya TuyaMCU firmware (BLE+WiFi fan switch). This dump's
        # TCP/IP init drives a hardware crypto/accelerator at 0x810000 (set a
        # busy bit, spin until it clears); without that block modelled the boot
        # hangs at "Initializing TCP/IP stack". These markers are all PAST that
        # stall - BLE stack bring-up and BLE-netcfg advertising - so this case
        # guards the 0x810000 / 0x81001c accelerator model. -key TUYA, and it is
        # the deepest-booting original-Tuya dump in the suite.
        "name": "Tuya TMWF02 TuyaMCU Boots past crypto accel",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231T_Tuya_TMWF02_Fan_Switch_TuyaMCU_1.1.71.bin"),
        # --uart1-hex keeps any TuyaMCU 55AA bytes off the UART2 log stream the
        # markers match, and exercises the dual-UART feature.
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "Initializing TCP/IP stack",
            # Past the 0x810000 crypto-accel stall: BLE host stack comes up.
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            # BLE network-config advertising starts.
            "appm start advertising"
        ]
    },
    {
        "name": "Woox Tuya Original Firmware Boot",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 120,
        # Timestamps are stripped from the expected strings: the RTOS tick now
        # advances Tuya's clock, so lines print at 18:12:15/16/... depending on
        # emulation timing.
        "expected_strings": [
            # Tuya keeps its "protected" key block at flash 0x1ee000, which is
            # above the end of the CRC-stripped (logical) image - a 2MB physical
            # dump yields only ~1.88MB of logical space. The emulator used to
            # answer 0xFF there, so the firmware saw blank flash and took the
            # "init key:" path (simple_flash.c:486), inventing a key. Serving the
            # dump's real bytes makes it retrieve the stored one instead, which
            # is what the hardware does; assert that stronger behaviour.
            "simple_flash.c:432] key_addr: 0x1ee000",
            "simple_flash.c:500] get key:",
            "0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",
            "TUYA Notice][tuya_main.c:311] **********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********"
        ]
    },
    {
        # The BK7231N temp/humidity sensor exercises the same protected-key path
        # and was the case that exposed it: with 0xFF served it failed the magic
        # check ("flash is encrypted or empty" - the magic and crc32 it reported
        # are literally AES-128-ECB-decrypt(0xFF x16) under the firmware's
        # hardcoded key "qwertyuiopasdfgh"), then dropped to mf_test with
        # "db init fails". Serving the dump's real bytes lets it decrypt its key.
        "name": "BK7231N Tuya TempHum Sensor Reads Protected Key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_TempHum_LCD_Sensor_TuyaMCU_2025-26-9.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "key_addr: 0x1ee000",
            "get key:",
            "0x6b 0x24 0x0 0x73 0xf4 0x11 0x45 0xed 0x40 0xf1 0x9 0x9f 0x16 0xd3 0xb5 0xc0"
        ]
    },
    {
        # Stock Tuya smart plug (Tuya IoT SDK 2.3.3). A third, independent
        # device covering the protected-key path at 0x1ee000 - it was picked at
        # random from the dump corpus after that fix landed, so it checks the
        # fix generalises rather than being tuned to the two devices that
        # motivated it. Boots further than the sensor: reaches OEM config load
        # and never drops into mf_test.
        "name": "BK7231N Tuya Plug (SDK 2.3.3) Boots and Reads Protected Key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Plug_TSL-PS-F4U-01-W_1.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "IOT DEFS < WIFI_GW:1 DEBUG:1 KV_FILE:0 SHUTDOWN_MODE:0 LITTLE_END:1 TLS_MODE:2",
            "ENABLE_LAN_ENCRYPTION:1",
            "oem_bk7231n_plug:1.1.17",
            "firmware compiled at Jun 13 2023 20:36:20",
            # Protected key store read correctly (would be a blank-flash failure
            # chain ending in mf_test if the raw bytes above the logical end
            # were not served).
            "key_addr: 0x1ee000",
            "get key:",
            "0xe7 0x10 0xac 0x15 0x88 0x2b 0x38 0x50 0xb2 0x9a 0xd 0xfa 0x35 0xbe 0x99 0x37"
        ]
    },
    {
        # BK7231N "zmai90" energy meter built around an RN8209C metering chip on
        # UART. Stock Tuya firmware (SDK 2.3.1). Reads its protected key fine
        # (0x1ee000, exercising the same above-logical-end fallback), then drops
        # into the mf_test manufacturing-test thread and does not reach normal
        # operation - so it never polls the RN8209C and emits nothing on UART1.
        # This is a *pre-pair* (unactivated) dump: with no activation record the
        # SDK treats the device as unprovisioned and parks in mf_test. Only a
        # post-pair dump would boot on and drive the RN8209C over UART1. The
        # check is that it boots this far and reads the key.
        "name": "BK7231N Tuya zmai90 RN8209C Energy Meter Boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_zmai90_RN8209C_EnergyMeter.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",
            "firmware compiled at Apr 26 2022 15:12:39",
            "init protected data length 460",
            "key_addr: 0x1ee000",
            "get key:",
            "0x80 0xd9 0xca 0xc2 0x32 0xb4 0x9c 0x30 0x6e 0x8b 0xc2 0x3d 0xf4 0x4c 0x39 0x7c"
        ]
    },
    {
        # The first *stock Tuya* dump found that actually drives its MCU. An
        # ETWF4301 thermostat (AXZN / TM1640 panel): the MCU owns the display and
        # sensors, the Beken is only the radio, so the two talk over UART1.
        #
        # It is a PAIRED dump - it logs "have actived over 15 min, not enter
        # mf_init", so unlike the pre-pair dumps (TempHum sensor, zmai90) it does
        # not park in the manufacturing-test thread. It reaches normal operation
        # and then sends TuyaMCU heartbeats unprompted, with no MCU attached -
        # byte-identical to the frame OpenBeken's own driver emits.
        #
        # This case guards three things at once:
        #  - the PHYSICAL flash-addressing model: both KV copies must read valid
        #    with matching counts (the mirror at 0x1cf000 read as blank flash
        #    under the old stripped/logical model, which is what exposed the bug);
        #  - that a paired stock-Tuya image boots through to normal operation;
        #  - the UART1 capture path on stock vendor firmware, not just OpenBeken.
        "name": "BK7231N Tuya Ettroit ETWF4301 (paired) sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Ettroit_ETWF4301_Thermostat_TuyaMCU_3.1.28.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        # The first frame lands only after the SDK reaches normal operation
        # (~3-4 min here); streaming stops as soon as the required count is seen.
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:3.1.28",
            # Paired: skips manufacturing test.
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid, counts matching.
            "current kv info, addr: 1ed000, cnt: 97, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 97, is valid: 0",
            # TuyaMCU heartbeat (55 AA ver=00 cmd=00 len=0000 chk=FF), repeated -
            # proves the MCU link keeps running, not just starts.
            ("[UART1/MCU] 55 aa 00 00 00 00 ff", REPEATS)
        ]
    },
    {
        # Second paired stock-Tuya device that drives its MCU, and deliberately a
        # DIFFERENT SDK generation from the Ettroit case: TuyaOS 3.11.12 (built
        # 2025) rather than the older "TUYA IOT SDK V:2.3.3" line. It also takes a
        # different path - it never prints "have actived", yet still skips
        # manufacturing test - so the two cases together cover both variants.
        #
        # Picked by a static signal that separates paired from factory dumps
        # perfectly across every device checked: a paired unit has written BOTH
        # KV copies (mirror 0x1cf000 as well as current 0x1ed000), because the
        # store only rotates after real service use. Factory dumps leave the
        # mirror erased. (The earlier "user data %" heuristic was noise - it
        # scored this device the same as unpaired ones.)
        "name": "BK7231N Tuya BEOK Thermostat (TuyaOS 3.11.12) sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid with matching counts.
            "current kv info, addr: 1ed000, cnt: 8, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 8, is valid: 0",
            # Protected key decrypted from 0x1ee000.
            "0x53 0xc1 0xb1 0x85 0xe3 0xbf 0xb6 0x3 0xe8 0x43 0xad 0xfb 0x83 0x82 0x69 0xdf",
            # The MCU link is opened at the TuyaMCU baud before any frame goes out.
            "read baud rate 9600",
            ("[UART1/MCU] 55 aa 00 00 00 00 ff", REPEATS)
        ]
    },
    {
        # Third paired stock-Tuya device driving its MCU, chosen for a different
        # device CLASS rather than a different SDK: a dual-clamp power meter,
        # where the MCU owns the current clamps and all metering and the Beken is
        # purely the radio. (It ships the same TuyaOS 3.11.12 build as the BEOK
        # case, so this adds hardware-class coverage, not SDK coverage.)
        #
        # Its KV pair carries cnt 16 - a different rotation count from the other
        # two (97 and 8) - which is useful: it shows the physical-addressing fix
        # is not tied to one particular store state.
        "name": "BK7231N Tuya PJ1103C Dual-Clamp Power Meter sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_PJ1103C_DualClampPowerMeter_TuyaMCU_TuyaOS_3.11.12.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid, counts matching.
            "current kv info, addr: 1ed000, cnt: 16, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",
            # This device's own protected key, decrypted from 0x1ee000.
            "0xfe 0x2f 0xe2 0x48 0x9 0x3f 0x8d 0x4a 0xad 0x1a 0xc1 0xf3 0x79 0x2f 0x56 0xf6",
            "read baud rate 9600",
            ("[UART1/MCU] 55 aa 00 00 00 00 ff", REPEATS)
        ]
    },
    {
        # A SECOND UART protocol on the same path, so the UART1 capture is not
        # only ever proven with TuyaMCU framing. Startup command
        # "startDriver BL0942" runs OpenBeken's BL0942 energy-meter driver, which
        # opens UART1 at 4800 baud (not 9600) and speaks a completely different
        # wire format - per drv_bl0942.c, write is 0xA8|addr and read is
        # 0x58|addr, each frame checksum-terminated:
        #   a8 1d 55 00 00 e5   write reg 0x1D (mode), checksum e5
        #   a8 19 8f 00 00 af   write reg 0x19,        checksum af
        #   58 aa               read reg 0xAA = the full measurement packet,
        #                       then repeated as the driver polls every second.
        # Nothing here resembles TuyaMCU's 55 AA framing, which is the point.
        "name": "MathDemo Startup Command: startDriver BL0942 drives the meter UART",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 240,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Started BL0942.",
            # Init: the two register writes the driver sends before polling.
            "[UART1/MCU] a8 1d 55 00 00 e5 a8 19 8f 00 00 af",
            # Then the repeating full-packet read - proves the poll loop runs.
            ("[UART1/MCU] 58 aa", REPEATS)
        ]
    }
,
    {
        # Presence/radar sensor (TuyaOS 3.8.18) - a third SDK line and another
        # hardware class alongside the thermostats and the clamp meter.
        #
        # This is the case that guards FLASH WRITES. The device persists a 4K
        # sector during start-up, and the emulator used to ignore page-program
        # and sector-erase opcodes entirely - the opcode field was decoded from
        # the wrong bits of REG_FLASH_OPERATE_SW - so the firmware read back
        # stale bytes, failed its own verify with "Err write addr:0x001f1000"
        # and called bk_reboot, looping forever. With writes implemented it runs
        # on to normal operation and drives its MCU. Any firmware that saves
        # state at boot depends on this.
        "name": "BK7231N Tuya NAS-PS10 Presence Sensor sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_NAS-PS10_PresenceSensor_TuyaMCU_TuyaOS_3.8.18.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.8.18 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            # Reached normal operation - it rebooted here before flash writes
            # worked, so this line is the flash-write regression guard.
            "mf_init succ",
            "current kv info, addr: 1ed000, cnt: 16, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",
            "0x9a 0xe5 0x6a 0x9b 0x84 0x26 0x71 0x1a 0x9f 0x6a 0xdb 0x77 0x34 0xf9 0xae 0xf7",
            "read baud rate 9600",
            ("[UART1/MCU] 55 aa 00 00 00 00 ff", REPEATS)
        ]
    }
,
    {
        # A PWM-driven light rather than an MCU device: the Beken itself drives
        # the LED channels through hardware PWM (its stored config declares
        # cool on P6, warm on P8 at 1000 Hz), so there is no TuyaMCU traffic on
        # UART1 by design - every other stock-Tuya case here talks to an MCU.
        #
        # Also a fourth Tuya SDK generation (TuyaOS 3.3.44) next to IOT SDK
        # 2.3.3, TuyaOS 3.8.18 and 3.11.12. Paired dump: reads its protected key
        # and reaches normal operation.
        "name": "BK7231N Tuya Arlec RGB Strip (PWM light, TuyaOS 3.3.44) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Arlec_Razer_RGBStrip_PWM_TuyaOS_3.3.44.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TuyaOS V:3.3.44 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_slide_strip_ty:1.3.3",
            "mf_init succ",
            "key_addr: 0x1ee000",
            "get key:",
            "0x34 0x8b 0xdd 0xe7 0x7b 0x9c 0x63 0xe0 0x44 0x1e 0xa7 0x1f 0x21 0x6b 0x5 0xc"
        ]
    }
]

# Report-facing prose, one per test name. Kept separate from TEST_CASES so the
# (working, exactly-asserted) case definitions stay untouched; the inline
# comments there document specific assertion strings for maintainers, while
# these summarise each test for the HTML report's expandable description.
DESCRIPTIONS = {
    "CLI: invalid -key is rejected":
        "A bad -key value must be rejected up front by parse_key(), with a message naming the "
        "accepted forms (a known key name, 32 hex chars, or base64 of 16 bytes), rather than "
        "failing deep inside emulation.",
    "CLI: invalid -chip is rejected":
        "An unknown -chip name must be rejected and the known chip identities listed, so a typo "
        "fails fast instead of emulating with the wrong SCTRL id registers.",
    "CLI: encrypted image without a key warns":
        "Running an encrypted image with no key must warn that the decrypted slice does not look "
        "like ARM code and suggest -key TUYA, instead of silently emulating garbage.",
    "OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timers":
        "Full OpenBeken boot on the flagship BK7231T image, then ten Main_OnEverySecond ticks - "
        "proving the timer keeps firing, not just starts. Also proves the tick interrupt, the "
        "FreeRTOS timer daemon and MQTT registration are alive; with no WiFi in the emulator, "
        "MQTT stays disconnected (bWifi 0).",
    "MathDemo Boot and Float Verification":
        "A special OpenBeken build that runs integer and floating-point math at boot and logs the "
        "results. Verifies the CPU's software-float paths and that FreeRTOS software timers fire "
        "(quicktick); with no config in flash it takes the default-config path.",
    "MathDemo Startup Command: echo":
        "The mathDemo image with a hand-crafted OpenBeken config injected at flash 0x1e1000, whose "
        "startup command is 'echo Test...'. Proves the emulated flash controller serves a full "
        "32-byte page per read and that the config's signed-char Tiny_CRC8 is accepted, then the "
        "echo command runs.",
    "MathDemo Startup Command: uartSendHex on UART1":
        "Same config-injection trick, but the startup command drives OpenBeken's uartSendHex to "
        "place arbitrary bytes on UART1 (the MCU link). Exercises config load -> command "
        "registration -> HAL UART write -> the emulator's UART1 hex capture.",
    "MathDemo Startup Command: startDriver TuyaMCU sends heartbeat":
        "Startup command 'startDriver TuyaMCU'. The driver opens UART1 itself and talks first, "
        "unprompted: the first per-second tick emits a TuyaMCU heartbeat frame "
        "(55 AA 00 00 00 00 FF) with no MCU attached.",
    "OpenBK7231U_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on the BK7231U variant from a plaintext (no-key) image, booting through to the "
        "per-second timer - confirms the shared BK7231 model and the no-decrypt path.",
    "OpenBK7238_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on the newer BK7238 silicon (-chip BK7238). Boots through the SPI flash mirror "
        "and the crypto/XVR peripherals to the per-second timer, checking BK7238 chip-identity gating.",
    "OpenBK7231N_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7231N. This variant blocks in the per-second temperature read waiting on "
        "the SARADC interrupt (ICU bit 11), so reaching the 1s timer proves the emulated SARADC "
        "FIFO + interrupt model works.",
    "OpenBK7231M_QIO_1.18.300 Boot to 1s timers (no key)":
        "OpenBeken on BK7231M from a plaintext image (no key). Confirms the shared model and "
        "no-key path reach the per-second timer.",
    "OpenBK7252_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7252 (BK7221U silicon, -chip BK7252). One of the heavier boots; reaches "
        "the per-second timer.",
    "OpenBK7252N_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7252N (-chip BK7252N). The heaviest boot in the suite; reaches the "
        "per-second timer.",
    "BK7238 Sonoff 4MB Dump Boots (SPI mirror + XVR)":
        "A real 4MB Sonoff flash dump on BK7238. Exercises the SPI flash mirror and the XVR "
        "peripheral through the vendor SDK boot to calibration / normal-mode markers.",
    "BK7231Q Tuya MOES Relay Boot":
        "A Tuya BK7231Q relay dump booting the Tuya IoT SDK through TCP/IP stack initialisation.",
    "BL2028N (=BK7231N) Boots to BLE init":
        "A BL2028N (a BK7231N-class part) fan dump booting to BLE stack init (rwble_hl_init), "
        "covering the BLE-capable boot path.",
    "Tuya TMWF02 TuyaMCU Boots past crypto accel":
        "A stock Tuya fan-switch dump. Boots past the 0x810000 crypto-accelerator stall into the "
        "BLE host stack and network-config advertising.",
    "Woox Tuya Original Firmware Boot":
        "Original Tuya (non-OpenBeken) light firmware. Verifies the fix that serves the un-striped "
        "protected-key region at 0x1ee000: instead of inventing a key from blank flash, the "
        "firmware now retrieves the stored key.",
    "BK7231N Tuya TempHum Sensor Reads Protected Key":
        "The stock Tuya temp/humidity sensor that first exposed the protected-key bug. With blank "
        "flash it failed the AES magic check and dropped to mf_test; serving the dump's real bytes "
        "lets it decrypt its key (get key).",
    "BK7231N Tuya Plug (SDK 2.3.3) Boots and Reads Protected Key":
        "A stock Tuya smart plug (SDK 2.3.3), picked at random after the protected-key fix to check "
        "it generalises. Boots past SDK init and OEM config, reading its protected key; never drops "
        "into mf_test.",
    "BK7231N Tuya BEOK Thermostat (TuyaOS 3.11.12) sends TuyaMCU heartbeats":
        "A paired BEOK TOL47WIFI-WP-WF thermostat running TuyaOS 3.11.12 - a newer SDK line than "
        "the Ettroit case, and one that never prints \"have actived\" yet still skips manufacturing "
        "test. It opens the MCU link at 9600 baud and streams TuyaMCU heartbeats. Selected by the "
        "mirror-KV signal: paired units have written both KV copies, factory dumps leave the mirror "
        "erased.",
    "BK7231N Tuya PJ1103C Dual-Clamp Power Meter sends TuyaMCU heartbeats":
        "A paired PJ1103C dual-clamp power meter (TuyaOS 3.11.12). The MCU owns the current clamps "
        "and all metering while the Beken is only the radio, so this covers a different hardware "
        "class from the two thermostat cases. Its KV pair carries a third distinct rotation count "
        "(16), showing the physical flash-addressing model is not tied to one store state.",
    "MathDemo Startup Command: startDriver BL0942 drives the meter UART":
        "Runs OpenBeken's BL0942 energy-meter driver from an injected startup command. The driver "
        "opens UART1 at 4800 baud and speaks the BL0942 register protocol - two init writes "
        "(0xA8|addr) followed by a repeating full-packet read (0x58 0xAA) - which looks nothing like "
        "TuyaMCU framing. Proves the UART1 capture path is protocol-agnostic, not tuned to 55 AA.",
    "BK7231N Tuya NAS-PS10 Presence Sensor sends TuyaMCU heartbeats":
        "A paired presence/radar sensor on TuyaOS 3.8.18 - a third SDK line and hardware class. "
        "It is the flash-write guard: the device persists a 4K sector during start-up, and while "
        "page-program and sector-erase opcodes were ignored it failed its own read-back verify and "
        "rebooted in a loop. With writes implemented it reaches normal operation and drives its MCU.",
    "BK7231N Tuya Arlec RGB Strip (PWM light, TuyaOS 3.3.44) boots":
        "A paired RGB LED strip driven by hardware PWM straight from the Beken - cool on P6, warm "
        "on P8 at 1000 Hz - rather than by an external MCU, so unlike the other stock-Tuya cases it "
        "produces no UART1 traffic by design. Adds a fourth Tuya SDK generation (TuyaOS 3.3.44) and "
        "reaches normal operation after reading its protected key.",
    "BK7231N Tuya Ettroit ETWF4301 (paired) sends TuyaMCU heartbeats":
        "A stock Tuya ETWF4301 thermostat (SDK 3.1.28) and the first non-OpenBeken image found "
        "that actually drives its MCU. Because this dump was taken after pairing, the SDK skips "
        "manufacturing test, reaches normal operation, and sends TuyaMCU heartbeats unprompted - "
        "the same 55 AA 00 00 00 00 FF frame OpenBeken emits. Also guards the physical "
        "flash-addressing model: both KV copies (current 0x1ed000, mirror 0x1cf000) must read "
        "valid with matching counts.",
    "BK7231N Tuya zmai90 RN8209C Energy Meter Boots":
        "A stock Tuya energy meter (zmai90, RN8209C metering chip). A pre-pair (unactivated) dump: "
        "it reads its protected key but then parks in the mf_test thread, so it never polls the "
        "meter over UART1. Proves the boot + key read; the empty UART1 is the dump's provisioning "
        "state.",
}


def dump_url(binary):
    """A downloadable GitHub raw URL for a firmware dump (works in CI and locally)."""
    name = os.path.basename(binary)
    repo = os.environ.get("GITHUB_REPOSITORY", "openshwprojects/BekenEmulator")
    ref = os.environ.get("GITHUB_SHA") or "main"
    return "https://github.com/%s/raw/%s/firmwares/%s" % (repo, ref, name)


# Which silicon a test covers, read off the dump's filename. Longer names come
# first so BK7252N is not matched as BK7252, and BK7231N not as BK7231. This is
# the *part* the dump came from, which is finer-grained than the emulated
# identity: --chip collapses the whole T/U/N/M family onto BK7231.
_CHIP_RE = re.compile(r"(BL2028N|BK7252N|BK7231[TUNMQ]|BK7238|BK7252|BK7236|BK7258|BK3231)", re.I)


def chip_of(test_config):
    """Best-effort chip name for a test: filename first, then the -chip arg."""
    m = _CHIP_RE.search(os.path.basename(test_config["binary"]))
    if m:
        return m.group(1).upper()
    args = test_config.get("args", [])
    for i, a in enumerate(args):
        if a in ("-chip", "--chip") and i + 1 < len(args):
            return args[i + 1].upper()
    return "BK7231"


# Tags shown under each test title in the HTML report. Derived from the case
# itself rather than hand-maintained, so a new test is tagged the moment it is
# added. Four groups, rendered in this order:
#   source  - who wrote the firmware: OpenBeken / Tuya, or CLI for the arg tests
#   chip    - the silicon the dump came from (BK7231N, BK7238, ...)
#   state   - pairing state, for stock Tuya dumps that expose it
#   feature - what the case actually exercises (TuyaMCU, UART1, Flash, ...)
# A case may set "tags": [...] to add anything this cannot infer.
TAG_GROUPS = {}   # tag -> group, filled in below


def _tag(group, name):
    TAG_GROUPS[name] = group
    return name


def tags_for(test_config):
    """Infer the report tags for one test case."""
    name = test_config["name"]
    base = os.path.basename(test_config["binary"])
    args = " ".join(test_config.get("args", []))
    exp = " ".join(s if isinstance(s, str) else s[0]
                   for s in test_config.get("expected_strings", []))
    hay = " ".join((name, base, exp))
    tags = []

    # --- source -----------------------------------------------------------
    if name.startswith("CLI:"):
        tags.append(_tag("source", "CLI"))
    elif "OpenBK" in base or "mathDemo" in base:
        tags.append(_tag("source", "OpenBeken"))
    else:
        tags.append(_tag("source", "Tuya"))

    # --- chip -------------------------------------------------------------
    tags.append(_tag("chip", chip_of(test_config)))

    # --- pairing state (stock Tuya dumps only) ----------------------------
    if "mf_init succ" in exp or "have actived" in exp:
        tags.append(_tag("state", "paired"))
    elif "mf_test" in exp:
        tags.append(_tag("state", "pre-pair"))

    # --- features ---------------------------------------------------------
    # Each rule keys off what the case ASSERTS, not merely what it passes on the
    # command line, so a tag means "this test covers it" rather than "this test
    # happens to touch it".
    is_obk = "OpenBeken" in tags
    feat = []
    if "TuyaMCU" in base or "TuyaMCU" in name or "55 aa 00 00" in exp:
        feat.append("TuyaMCU")
    # UART1 only when the test actually asserts traffic on it.
    if "UART1/MCU" in exp:
        feat.append("UART1")
    if "uartSendHex" in hay:
        feat.append("uartSendHex")
    if "CFG_InitAndLoad" in exp or "obkStartupCommand" in base:
        feat.append("OBK config")
    if any(k in exp for k in ("key_addr", "get key", "get kvs", "kv info")):
        feat.append("flash/KV")
    if "idle " in exp or "1s timer" in name:
        feat.append("timers")
    # Only the case that genuinely exercises MQTT registration.
    if "MQTT_RegisterCallback" in exp:
        feat.append("MQTT")
    if any(k in exp for k in ("rwble", "STACK INIT", "advertising")):
        feat.append("BLE")
    # SARADC is an OpenBeken concern: N/M block in Main_OnEverySecond's
    # temperature read until the ADC interrupt is modelled. Stock Tuya dumps on
    # the same silicon never reach that path.
    if is_obk and ("7231N" in base or "7231M" in base):
        feat.append("SARADC")
    if "Float basic" in exp:
        feat.append("float math")
    # Crypto only for the cases whose subject IS key handling.
    if "Invalid -key" in exp or "ARM code" in exp:
        feat.append("crypto")
    if "SPI mirror" in name:
        feat.append("SPI mirror")
    if "XVR" in name:
        feat.append("XVR")
    for f in feat:
        tags.append(_tag("feature", f))

    for extra in test_config.get("tags", []):
        if extra not in tags:
            tags.append(_tag("feature", extra))
    return tags


def _parse_expected(expected):
    """Normalise expected_strings entries to (string, min_count) pairs.

    An entry is either a plain string (needs to appear at least once) or a
    (string, count) tuple requiring at least `count` occurrences - used to prove
    a periodic line keeps printing, not just appears once.
    """
    out = []
    for e in expected:
        if isinstance(e, (tuple, list)) and len(e) == 2:
            out.append((e[0], int(e[1])))
        else:
            out.append((e, 1))
    return out


def _periph(lines):
    """Decode captured [EMU_GPIO]/[EMU_PWM] lines; never breaks a run."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import periph
        gpio, pwm = periph.parse_lines(lines)
        if not gpio and not pwm:
            return None
        channels, ctl = periph.pwm_channels(pwm)
        return {"gpio": periph.gpio_table(gpio),
                "func": periph.func_rows(gpio),
                "pwm_ctl": ctl,
                "pwm_channels": channels,
                "pwm_regs": periph.pwm_registers(pwm)}
    except Exception:
        return None


def _tuya_config(binary):
    """Best-effort Tuya config extraction; never breaks a test run."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import tuya_config
        return tuya_config.extract(binary)
    except Exception:
        return None


def _result(test_config, passed, elapsed=0.0, insns=None, timed_out=False, output="", checks=None):
    """Assemble the structured record the HTML report consumes."""
    binary = test_config["binary"]
    if checks is None:
        checks = [{"string": s, "found": False, "count": 0, "required": n}
                  for s, n in _parse_expected(test_config["expected_strings"])]
    return {
        "name": test_config["name"],
        "description": DESCRIPTIONS.get(test_config["name"], ""),
        "binary": binary,
        "binary_name": os.path.basename(binary),
        "chip": chip_of(test_config),
        # Tuya user_param_key record recovered from the dump, if it keeps
        # one in plaintext - shown in the report's "Tuya config" tab.
        "tuya_config": _tuya_config(binary),
        "tags": [{"name": t, "group": TAG_GROUPS.get(t, "feature")}
                 for t in tags_for(test_config)],
        "dump_url": dump_url(binary),
        "args": test_config["args"],
        "passed": passed,
        "timed_out": timed_out,
        "elapsed": elapsed,
        "insns": insns,
        "checks": checks,
        "output": output,
    }


def run_test(test_config):
    name = test_config["name"]
    binary = test_config["binary"]
    args = test_config["args"]
    timeout = test_config.get("timeout", 30)
    expected = test_config["expected_strings"]

    print(f"=====================================")
    print(f"Running Test: {name}")
    print(f"Binary: {binary}")

    if not os.path.exists(binary):
        print(f"FAIL: Binary not found: {binary}")
        return _result(test_config, passed=False, output="Binary not found: %s" % binary)

    cmd = [sys.executable, MAIN_SCRIPT, binary] + args
    # EMU_REPORT makes the emulator periodically print its approximate
    # instruction count on [EMU_INSNS] lines, which the reader below strips out
    # of the displayed log and records for the report.
    child_env = dict(os.environ)
    child_env["EMU_REPORT"] = "1"

    # Stream the child's output and stop as soon as every expected string has
    # been seen. The emulator never exits on its own, so the old
    # run-until-timeout approach cost each boot case its full timeout - and made
    # the suite fragile, because a case that reached its markers late (heavy
    # chip, or a busy machine) would be killed by the deadline before finishing.
    # Streaming makes a passing case take only as long as its last marker needs,
    # and a real failure still fails at the timeout.
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                bufsize=1, encoding="utf-8", errors="ignore", env=child_env)
    except Exception as e:
        print(f"FAIL: Failed to launch subprocess: {e}")
        return _result(test_config, passed=False, output="Failed to launch subprocess: %s" % e)

    reqs = _parse_expected(expected)
    required = {s: n for s, n in reqs}
    counts = {s: 0 for s, n in reqs}
    lines = []
    remaining = set(required)          # strings not yet at their required count
    insns_holder = [None]
    periph_lines = []
    lock = threading.Lock()

    def reader():
        for line in proc.stdout:
            if line.startswith("[EMU_GPIO]") or line.startswith("[EMU_PWM]"):
                periph_lines.append(line.strip())
                continue
            if line.startswith("[EMU_INSNS]"):
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    insns_holder[0] = int(parts[1])
                continue
            with lock:
                lines.append(line)
                for s_ in list(remaining):
                    c = line.count(s_)
                    if c:
                        counts[s_] += c
                        if counts[s_] >= required[s_]:
                            remaining.discard(s_)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    start = time.time()
    hit_timeout = False
    done_at = None          # when every marker was first satisfied
    while True:
        with lock:
            done = not remaining
        if done:
            if proc.poll() is not None:
                break       # nothing left to capture - process already gone
            if LINGER <= 0:
                break
            # Keep running a little longer to capture more of the boot. The
            # verdict is already settled; this only enriches the log.
            if done_at is None:
                done_at = time.time()
                print(f"  ... all markers met, lingering {LINGER:.0f}s for a fuller log")
            elif time.time() - done_at >= LINGER:
                break
        elif proc.poll() is not None:
            # process exited on its own (e.g. a CLI test); let the reader drain.
            th.join(timeout=1.0)
            break
        elif time.time() - start > timeout:
            hit_timeout = True
            break
        time.sleep(0.25)

    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    th.join(timeout=2.0)

    with lock:
        output = "".join(lines)
    elapsed = time.time() - start
    if hit_timeout:
        print(f"Note: reached {timeout}s timeout. Checking output up to this point.")

    all_passed = True
    checks = []
    for string, need in reqs:
        actual = output.count(string)      # recount from the full captured log
        found = actual >= need
        checks.append({"string": string, "found": found, "count": actual, "required": need})
        if found:
            if need > 1:
                print(f"  [PASS] Found '{string}' x{actual} (need {need})")
            else:
                print(f"  [PASS] Found string: '{string}'")
        else:
            if need > 1:
                print(f"  [FAIL] '{string}' found {actual}x, need {need}")
            else:
                print(f"  [FAIL] Missing string: '{string}'")
            all_passed = False

    result = _result(test_config, passed=all_passed, elapsed=elapsed,
                     insns=insns_holder[0], timed_out=hit_timeout,
                     output=output, checks=checks)
    result["periph"] = _periph(periph_lines)

    if not all_passed:
        print("--- CAPTURED OUTPUT ---")
        print(output)
        print("-----------------------")
        print(f"Test '{name}' FAILED.")
        return result

    print(f"Test '{name}' PASSED. ({elapsed:.0f}s)")
    return result

def _write_report(results):
    """Emit the HTML report to report/index.html. Never fails the run itself."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import report as report_mod
        out_dir = os.path.join(ROOT_DIR, "report")
        os.makedirs(out_dir, exist_ok=True)
        run_id = os.environ.get("GITHUB_RUN_ID")
        run_url = None
        if run_id:
            run_url = "%s/%s/actions/runs/%s" % (
                os.environ.get("GITHUB_SERVER_URL", "https://github.com"),
                os.environ.get("GITHUB_REPOSITORY", ""), run_id)
        meta = {
            "total_time": sum(r["elapsed"] for r in results),
            "chips": sorted({r["chip"] for r in results if r.get("chip")}),
            "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
            "commit": os.environ.get("GITHUB_SHA"),
            "repo": os.environ.get("GITHUB_REPOSITORY", "openshwprojects/BekenEmulator"),
            "run_url": run_url,
        }
        path = report_mod.generate(results, meta, os.path.join(out_dir, "index.html"))
        print(f"HTML report written: {path}")
    except Exception as e:
        print(f"WARN: could not write HTML report: {e}")

def main():
    results = [run_test(test) for test in TEST_CASES]
    passed = sum(1 for r in results if r["passed"])
    failed = len(results) - passed

    print(f"=====================================")
    print(f"Test Run Completed: {passed} passed, {failed} failed.")

    _write_report(results)

    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
