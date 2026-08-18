import argparse
import sys
import os

# Add root directory to sys.path so we can import src.*
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.crypto import extract_and_decrypt, strip_crcs, parse_key, KNOWN_KEYS
from src.emulator import BekenEmulator

def parse_args():
    parser = argparse.ArgumentParser(description="BK7231T/N Emulator")
    parser.add_argument("dump_file", help="Path to the raw BK7231 flash dump")
    parser.add_argument("--only-uart", action="store_true", help="Only print UART output (suppress MMIO/Trace/Info logs)")
    parser.add_argument("--with-boot", action="store_true", help="Start execution from bootloader (0x00000000) instead of app (0x10000)")
    parser.add_argument("-key", "--key", dest="key", default=None, metavar="KEY",
                        help="Firmware decryption key: a known name (%s), 32 hex chars, or base64 of 16 bytes. "
                             "Omit for plaintext images (no decryption)." % ", ".join(sorted(KNOWN_KEYS)))
    return parser.parse_args()

def main():
    args = parse_args()

    if args.only_uart:
        # Redirect stdout internally if needed, but the emulator handles self._print()
        pass

    with open(args.dump_file, "rb") as f:
        raw_data = f.read()

    try:
        coefs = parse_key(args.key)
    except ValueError as e:
        print(e)
        sys.exit(1)

    flash_data = strip_crcs(raw_data)

    bootloader = extract_and_decrypt(flash_data, "bootloader", coefs)
    app = extract_and_decrypt(flash_data, "app", coefs)
    
    if app:
        with open("app_decrypted.bin", "wb") as f:
            f.write(app)

    if not args.only_uart:
        if bootloader is None:
            print("Warning: Could not find 'bootloader' RBL container. Booting from 0x10000.")
        else:
            print(f"Loaded 'bootloader' payload, size: {len(bootloader)} bytes")
            
        if app is None:
            print("Warning: Could not find 'app' RBL container.")
        else:
            print(f"Loaded 'app' payload, size: {len(app)} bytes")

    emu = BekenEmulator(raw_flash=flash_data, bootloader=bootloader, app=app, with_boot=args.with_boot, only_uart=args.only_uart)
    emu.setup()
    emu.run()

if __name__ == "__main__":
    main()
