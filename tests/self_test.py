import subprocess
import os
import sys

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

# DRY Test definitions
TEST_CASES = [
    {
        "name": "OpenBK7231T_QIO_1.18.300 Boot to MQTT",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 120,  # seconds
        "expected_strings": [
            "OpenBK7231T, version 1.18.300",
            "Main_Init_Delay",
            "Info:MQTT:MQTT_RegisterCallback called for"
        ]
    },
    {
        "name": "MathDemo Boot and Float Verification",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300_mathDemo.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            "Info:MAIN:float test",
            "Info:MAIN:x + y = 5.000000"
        ]
    },
    {
        "name": "Woox Tuya Original Firmware Boot",
        "binary": os.path.join(ROOT_DIR, "references", "FlashDumps", "IoT", "BK7231T", "BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            "[01-01 18:12:15 TUYA Notice][simple_flash.c:486] init key:",
            "0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",
            "[01-01 18:12:15 TUYA Notice][tuya_main.c:311] **********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********"
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
