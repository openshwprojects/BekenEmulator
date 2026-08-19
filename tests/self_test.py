import subprocess
import threading
import time
import os
import sys

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

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
        "name": "OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        # Boot finishes around 28M instructions; the first Main_OnEverySecond
        # "Time N, idle ..." line lands around 38M (~90s wall on a typical machine).
        "timeout": 180,  # seconds
        "expected_strings": [
            "OpenBK7231T, version 1.18.300",
            "Main_Init_Delay",
            "Info:MQTT:MQTT_RegisterCallback called for",
            # Main_OnEverySecond runs off a FreeRTOS software timer - proves the
            # tick interrupt and the timer daemon task are alive.
            ", idle ",
            # Stable tail of the first "Time N" lines: no WiFi in the emulator,
            # so MQTT is disconnected and the ping watchdog never starts.
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
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
        "name": "OpenBK7231U_QIO_1.18.300 Boot to 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231U_QIO_1.18.300.bin"),
        # Plaintext image (beken_freertos_sdk layout) - no key.
        "args": ["--only-uart"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7231U, version 1.18.300",
            ", idle ",
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
        ]
    },
    {
        "name": "OpenBK7238_QIO_1.18.300 Boot to 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7238_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7238 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7238, version 1.18.300",
            ", idle ",
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
        ]
    },
    {
        # BK7231N now reaches the per-second Main_OnEverySecond loop. It used
        # to hang there: Main_OnEverySecond's first call reads the chip
        # temperature via the SARADC and blocks on the SARADC interrupt (ICU
        # bit 11), which the emulator now models. N (unlike T) unmasks bit 11,
        # so this is the case that guards the SARADC model.
        "name": "OpenBK7231N_QIO_1.18.300 Boot to 1s timer",
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
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
        ]
    },
    {
        # BK7231M is the BK7231N build stored UNENCRYPTED (verified: the two app
        # images are byte-identical for their first 828KB, and this one prints
        # the "OpenBK7231N" banner). Runs with no -key - the regression guard
        # for the plaintext path through crypto.py on a BK7231N-family image.
        # With the SARADC model it reaches the per-second loop like N.
        "name": "OpenBK7231M_QIO_1.18.300 Boot to 1s timer (no key)",
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
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
        ]
    },
    {
        "name": "OpenBK7252_QIO_1.18.300 Boot to 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 240,
        "expected_strings": [
            "OpenBK7252, version 1.18.300",
            ", idle ",
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
        ]
    },
    {
        "name": "OpenBK7252N_QIO_1.18.300 Boot to 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252N_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252N chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 240,
        "expected_strings": [
            "OpenBK7252N, version 1.18.300",
            ", idle ",
            "MQTT 0(0), bWifi 0, secondsWithNoPing -1"
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
            "TUYA Notice][simple_flash.c:486] init key:",
            "0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",
            "TUYA Notice][tuya_main.c:311] **********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********"
        ]
    }
]

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
        return False

    cmd = [sys.executable, MAIN_SCRIPT, binary] + args

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
                                bufsize=1, encoding="utf-8", errors="ignore")
    except Exception as e:
        print(f"FAIL: Failed to launch subprocess: {e}")
        return False

    lines = []
    remaining = set(expected)
    lock = threading.Lock()

    def reader():
        for line in proc.stdout:
            with lock:
                lines.append(line)
                for s_ in list(remaining):
                    if s_ in line:
                        remaining.discard(s_)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    start = time.time()
    hit_timeout = False
    while True:
        with lock:
            done = not remaining
        if done:
            break
        if proc.poll() is not None:
            # process exited on its own (e.g. a CLI test); let the reader drain.
            th.join(timeout=1.0)
            break
        if time.time() - start > timeout:
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
    for string in expected:
        if string in output:
            print(f"  [PASS] Found string: '{string}'")
        else:
            print(f"  [FAIL] Missing string: '{string}'")
            all_passed = False

    if not all_passed:
        print("--- CAPTURED OUTPUT ---")
        print(output)
        print("-----------------------")
        print(f"Test '{name}' FAILED.")
        return False

    print(f"Test '{name}' PASSED. ({elapsed:.0f}s)")
    return True

def main():
    failed = 0
    passed = 0
    
    for test in TEST_CASES:
        if run_test(test):
            passed += 1
        else:
            failed += 1
            
    print(f"=====================================")
    print(f"Test Run Completed: {passed} passed, {failed} failed.")
    
    if failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)

if __name__ == "__main__":
    main()
