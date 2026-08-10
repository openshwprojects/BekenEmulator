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
