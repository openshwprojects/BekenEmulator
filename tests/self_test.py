import subprocess
import os
import sys

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

# DRY Test definitions
TEST_CASES = [
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
        # 4MB dump: guards the SPI-flash-mirror size cap in emulator.setup().
        # Before the cap, a >2MB image mapped past RAM_BASE and threw
        # UC_ERR_MAP before any code ran (0 boot output). A shallow boot marker
        # is enough - this only has to prove the 4MB image maps and executes.
        "name": "BK7238 Sonoff 4MB Dump Boots (SPI mirror cap)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "Sonoff_S61s_EUPlug_WBBK_01P_V1.3.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 90,
        "expected_strings": [
            # First SDK line - proves setup() didn't crash and code executed.
            "bk_misc_init_start_type",
            # Early boot milestone - flash driver ran.
            "[Flash]init over",
            # Proves it reached wifi init and the -chip identity is served.
            "chip id=7238 device id=21128000"
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
