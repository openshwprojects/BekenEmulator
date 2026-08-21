import argparse
import sys
import os

# Add root directory to sys.path so we can import src.*
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.crypto import extract_and_decrypt, strip_crcs, parse_key, KNOWN_KEYS
from src.emulator import BekenEmulator, CHIP_FAMILIES
from src import tuyamcu

# Data-point type names accepted by --tuyamcu-dp, mapped to the wire type codes.
DP_TYPE_NAMES = {
    "bool": tuyamcu.DP_BOOL, "value": tuyamcu.DP_VALUE, "enum": tuyamcu.DP_ENUM,
    "bitmap": tuyamcu.DP_BITMAP, "string": tuyamcu.DP_STRING,
}


def parse_dp_specs(specs):
    """Parse --tuyamcu-dp 'ID:TYPE:VALUE' items into {dp_id: (type_code, value)}."""
    dps = {}
    for spec in specs:
        parts = spec.split(":", 2)
        if len(parts) != 3:
            raise ValueError("bad --tuyamcu-dp %r (want ID:TYPE:VALUE)" % spec)
        id_s, type_s, val_s = parts
        if type_s not in DP_TYPE_NAMES:
            raise ValueError("bad DP type %r (one of %s)" % (type_s, "/".join(DP_TYPE_NAMES)))
        value = val_s if type_s == "string" else int(val_s, 0)
        dps[int(id_s, 0)] = (DP_TYPE_NAMES[type_s], value)
    return dps

def parse_args():
    parser = argparse.ArgumentParser(description="BK7231T/N Emulator")
    parser.add_argument("dump_file", help="Path to the raw BK7231 flash dump")
    parser.add_argument("--only-uart", action="store_true", help="Only print UART output (suppress MMIO/Trace/Info logs)")
    parser.add_argument("--with-boot", action="store_true", help="Start execution from bootloader (0x00000000) instead of app (0x10000)")
    parser.add_argument("--uart1-hex", action="store_true", help="Show UART1 (TuyaMCU link) as tagged hex; UART2 stays text log")
    parser.add_argument("--uart1-rx", dest="uart1_rx", default=None, metavar="TEXT",
                        help="Feed TEXT into UART1's receive FIFO (a CRLF is appended). With OBK's "
                             "UART command console enabled (flag 31), e.g. --uart1-rx \"echo hello\" "
                             "types that command in. Use \\n / \\r for explicit line endings.")
    parser.add_argument("--uart1-rx-delay", dest="uart1_rx_delay", type=int, default=None,
                        metavar="INSNS",
                        help="Hold the --uart1-rx bytes until this many instructions have run, so the "
                             "firmware has registered its console RX callback first (default 2000000, "
                             "or 0 with --tuyamcu since those replies are already reactive).")
    parser.add_argument("--tuyamcu", dest="tuyamcu", action="store_true",
                        help="Attach a simulated TuyaMCU MCU to UART1: answer the firmware's heartbeat, "
                             "product-info, working-mode and query-state frames so a TuyaMCU dump walks "
                             "past its heartbeat loop instead of stalling with nothing on the wire.")
    parser.add_argument("--tuyamcu-pid", dest="tuyamcu_pid", default=None, metavar="PID",
                        help="Product id the simulated MCU reports for a product-info (0x01) query. "
                             "Stock TuyaOS rejects a mismatch, so pass the device's real 16-char PID "
                             "(often recoverable from its flash) to get it past the product query.")
    parser.add_argument("--tuyamcu-raw", dest="tuyamcu_raw", action="store_true",
                        help="Reply to the product-info query with the raw 16-byte PID + short version "
                             "(TuyaOS 3.x wire form) instead of JSON. Use for TuyaOS 3.x dumps; leave "
                             "off for OpenBeken and older SDKs (1.1.71) that expect the JSON record.")
    parser.add_argument("--tuyamcu-dp", dest="tuyamcu_dps", action="append", default=[],
                        metavar="ID:TYPE:VALUE",
                        help="Data point the simulated MCU reports for a query-state (0x08): "
                             "ID:TYPE:VALUE, TYPE one of bool/value/enum/bitmap/string. Repeatable. "
                             "E.g. --tuyamcu-dp 1:bool:1 --tuyamcu-dp 101:value:230. Lets a device "
                             "advance past the working-mode query into data-point reporting.")
    parser.add_argument("--xvr-selfclear", dest="xvr_selfclear", action="store_true",
                        help="Model the XVR RF/BLE busy bits (0x9000F8 RF-cal, 0x900000 BLE llm_init) "
                             "as self-clearing, so stock Tuya SDK 2.x dumps (ATORCH 2.1.17, PC321 "
                             "2.0.2, ...) boot past their RF and BLE init spins to the TuyaMCU link. "
                             "Opt-in: it breaks dumps that do real BLE init, so only enable per dump.")
    parser.add_argument("-key", "--key", dest="key", default=None, metavar="KEY",
                        help="Firmware decryption key: a known name (%s), 32 hex chars, or base64 of 16 bytes. "
                             "Omit for plaintext images (no decryption)." % ", ".join(sorted(KNOWN_KEYS)))
    parser.add_argument("-chip", "--chip", dest="chip", default="BK7231", metavar="CHIP",
                        help="Chip identity served from the SCTRL id registers: %s. Default: BK7231 (T/U family)."
                             % ", ".join(sorted(CHIP_FAMILIES)))
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

    chip_name = args.chip.strip().upper()
    if chip_name not in CHIP_FAMILIES:
        print("Unknown -chip value: %r. Known chips: %s." % (args.chip, ", ".join(sorted(CHIP_FAMILIES))))
        sys.exit(1)
    chip_identity = CHIP_FAMILIES[chip_name]

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

    uart1_rx = b""
    if args.uart1_rx is not None:
        # Let \n and \r in the argument mean real line endings; default to CRLF.
        text = args.uart1_rx.encode("latin-1").decode("unicode_escape")
        if not text.endswith(("\n", "\r")):
            text += "\r\n"
        uart1_rx = text.encode("latin-1", "replace")

    # A typed console command must wait for OBK's console callback (~2M insns);
    # TuyaMCU replies are generated only after the firmware transmits a frame, so
    # the firmware is already listening and no hold-off is needed.
    if args.uart1_rx_delay is not None:
        rx_delay = args.uart1_rx_delay
    else:
        rx_delay = 0 if args.tuyamcu else 2_000_000

    emu = BekenEmulator(raw_flash=flash_data, bootloader=bootloader, app=app, with_boot=args.with_boot, only_uart=args.only_uart, chip_identity=chip_identity, uart1_hex=args.uart1_hex, physical_flash=raw_data, uart1_rx=uart1_rx, uart1_rx_delay=rx_delay, tuyamcu_enabled=args.tuyamcu, tuyamcu_pid=args.tuyamcu_pid, tuyamcu_raw=args.tuyamcu_raw, xvr_selfclear=args.xvr_selfclear, tuyamcu_dps=parse_dp_specs(args.tuyamcu_dps))
    emu.setup()
    emu.run()

if __name__ == "__main__":
    main()
