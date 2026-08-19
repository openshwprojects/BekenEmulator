import subprocess
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
            "Info:MAIN:quicktick"
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
        # BK7231N does NOT reach the per-second "Time N" line yet: after init a
        # task busy-spins above the FreeRTOS timer-daemon priority and starves
        # the software timers (suspected unmodelled FIQ wait). Everything up to
        # that point works, so assert the full init sequence instead - this
        # guards the efuse fix that carries N through RF calibration.
        "name": "OpenBK7231N_QIO_1.18.300 Boots through init",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231N_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 150,
        "expected_strings": [
            "OpenBK7231N, version 1.18.300",
            # Past RF calibration (this is where N used to hang).
            "calibration_main over",
            "app_init finished",
            # Full OpenBeken init completed.
            "Info:MAIN:Main_Init_After_Delay done"
        ]
    },
    {
        # BK7231M is the BK7231N build stored UNENCRYPTED (verified: the two app
        # images are byte-identical for their first 828KB, and this one prints
        # the "OpenBK7231N" banner). So it runs with no -key at all, which makes
        # this the regression guard for the plaintext path through crypto.py on
        # a BK7231N-family image. Like N, it does not reach the per-second
        # "Time N" line (timer-daemon starvation), so assert the init sequence.
        "name": "OpenBK7231M_QIO_1.18.300 Boots through init (no key)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231M_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 150,
        "expected_strings": [
            # M ships the N build, so the banner really does say 7231N.
            "OpenBK7231N, version 1.18.300",
            "calibration_main over",
            "app_init finished",
            "Info:MAIN:Main_Init_After_Delay done"
        ]
    },
    {
        "name": "OpenBK7252_QIO_1.18.300 Boot to 1s timer",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 180,
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
        "timeout": 180,
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
    
    try:
        # Run process and capture stdout/stderr merged
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
        output = result.stdout
    except subprocess.TimeoutExpired as e:
        # If it times out, that's often fine because it might just be hanging after booting
        output = e.stdout
        if isinstance(output, bytes):
            output = output.decode('utf-8', errors='ignore')
        elif output is None:
            output = ""
        print("Note: Process reached timeout. Checking output up to this point.")
    except Exception as e:
        print(f"FAIL: Failed to run subprocess: {e}")
        return False
        
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
        
    print(f"Test '{name}' PASSED.")
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
