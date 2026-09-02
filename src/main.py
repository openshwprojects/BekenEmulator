import argparse
import sys
import os

# Add root directory to sys.path so we can import src.*
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.crypto import extract_and_decrypt, strip_crcs, parse_key, KNOWN_KEYS
from src.emulator import BekenEmulator, CHIP_FAMILIES, CONSOLE_RX_HOLDOFF_INSNS
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

def parse_hex_blobs(specs):
    """Parse --tuyamcu-inject hex strings into raw byte strings.

    Spaces and a leading 0x are allowed so a frame can be pasted in the same
    shape the logs print it ("55 aa 00 1c 00 00 1b").
    """
    out = []
    for spec in specs:
        text = spec.replace(" ", "").replace("_", "")
        if text[:2].lower() == "0x":
            text = text[2:]
        try:
            out.append(bytes.fromhex(text))
        except ValueError:
            raise ValueError("bad --tuyamcu-inject %r (want hex bytes, e.g. 55AA001C00001B)" % spec)
    return out


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
                             "firmware has registered its console RX callback first (default %d, "
                             "or 0 with --tuyamcu since those replies are already reactive)."
                             % CONSOLE_RX_HOLDOFF_INSNS)
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
    parser.add_argument("--tuyamcu-inject", dest="tuyamcu_injects", action="append", default=[],
                        metavar="HEX",
                        help="Raw bytes the simulated MCU puts on the wire on its own initiative, "
                             "once the module's startup handshake is done. Repeatable; all queued "
                             "blobs go out together on the module's next frame, in order. Unlike "
                             "the replies above these are NOT built by the codec, so they can be "
                             "deliberately malformed - a wrong checksum, or leading garbage - to "
                             "test how the firmware recovers. E.g. --tuyamcu-inject 55AA001C00001B "
                             "asks OpenBeken for the time and it answers with a 0x1C frame.")
    parser.add_argument("--xvr-selfclear", dest="xvr_selfclear", action="store_true",
                        help="Model the XVR RF/BLE busy bits (0x9000F8 RF-cal, 0x900000 BLE llm_init) "
                             "as self-clearing, so stock Tuya SDK 2.x dumps (ATORCH 2.1.17, PC321 "
                             "2.0.2, ...) boot past their RF and BLE init spins to the TuyaMCU link. "
                             "Opt-in: it breaks dumps that do real BLE init, so only enable per dump.")
    parser.add_argument("--ble-core", dest="ble_core", action="store_true",
                        help="Model the RivieraWaves BLE core registers at 0x900000 (slot clock, "
                             "deep-sleep wake-up, interrupt status/ack, timer targets) and deliver "
                             "the BLE/BTDM FIQs, so a stock BLE+Wi-Fi image's controller keeps "
                             "running and answers the host's HCI commands instead of the host "
                             "timing out. No radio is modelled: nothing goes on air. Opt-in.")
    parser.add_argument("-key", "--key", dest="key", default=None, metavar="KEY",
                        help="Firmware decryption key: a known name (%s), 32 hex chars, or base64 of 16 bytes. "
                             "Omit for plaintext images (no decryption)." % ", ".join(sorted(KNOWN_KEYS)))
    parser.add_argument("-chip", "--chip", dest="chip", default="BK7231", metavar="CHIP",
                        help="Chip identity served from the SCTRL id registers: %s. Default: BK7231 (T/U family)."
                             % ", ".join(sorted(CHIP_FAMILIES)))
    return parser.parse_args()

def build_emulator(dump_path, key=None, chip="BK7231", with_boot=False, only_uart=False,
                   uart1_hex=False, uart1_rx=b"", uart1_rx_delay=None, tuyamcu=False,
                   tuyamcu_pid=None, tuyamcu_raw=False, tuyamcu_dps=None,
                   tuyamcu_injects=None, xvr_selfclear=False, uart_sink=None,
                   save_decrypted=True, log=None, ble_core=False):
    """Load a dump and return a configured, not-yet-started BekenEmulator.

    Everything between "here is an image and some options" and "start running"
    lives here, so the GUI drives the emulator through exactly the same path as
    the command line instead of a second copy that quietly drifts from it.
    Rejects bad options with ValueError carrying the message the CLI prints.

    `key`, `tuyamcu_dps` and `tuyamcu_injects` are the parsed forms; the CLI
    string parsers above turn command-line text into them first.
    """
    if log is None:
        log = (lambda *a: None) if only_uart else print

    chip_name = (chip or "").strip().upper()
    if chip_name not in CHIP_FAMILIES:
        raise ValueError("Unknown -chip value: %r. Known chips: %s."
                         % (chip, ", ".join(sorted(CHIP_FAMILIES))))

    with open(dump_path, "rb") as f:
        raw_data = f.read()

    flash_data = strip_crcs(raw_data)
    bootloader = extract_and_decrypt(flash_data, "bootloader", key)
    app = extract_and_decrypt(flash_data, "app", key)

    if app and save_decrypted:
        with open("app_decrypted.bin", "wb") as f:
            f.write(app)

    if bootloader is None:
        log("Warning: Could not find 'bootloader' RBL container. Booting from 0x10000.")
    else:
        log(f"Loaded 'bootloader' payload, size: {len(bootloader)} bytes")
    if app is None:
        log("Warning: Could not find 'app' RBL container.")
    else:
        log(f"Loaded 'app' payload, size: {len(app)} bytes")

    # A typed console command must wait for OBK's console callback (~2M insns);
    # TuyaMCU replies are generated only after the firmware transmits a frame, so
    # the firmware is already listening and no hold-off is needed.
    if uart1_rx_delay is None:
        uart1_rx_delay = 0 if tuyamcu else CONSOLE_RX_HOLDOFF_INSNS

    return BekenEmulator(raw_flash=flash_data, bootloader=bootloader, app=app,
                         with_boot=with_boot, only_uart=only_uart,
                         chip_identity=CHIP_FAMILIES[chip_name], uart1_hex=uart1_hex,
                         physical_flash=raw_data, uart1_rx=uart1_rx,
                         uart1_rx_delay=uart1_rx_delay, tuyamcu_enabled=tuyamcu,
                         tuyamcu_pid=tuyamcu_pid, tuyamcu_raw=tuyamcu_raw,
                         xvr_selfclear=xvr_selfclear, tuyamcu_dps=tuyamcu_dps,
                         tuyamcu_injects=tuyamcu_injects, uart_sink=uart_sink,
                         ble_core=ble_core)


def main():
    args = parse_args()

    uart1_rx = b""
    if args.uart1_rx is not None:
        # Let \n and \r in the argument mean real line endings; default to CRLF.
        text = args.uart1_rx.encode("latin-1").decode("unicode_escape")
        if not text.endswith(("\n", "\r")):
            text += "\r\n"
        uart1_rx = text.encode("latin-1", "replace")

    try:
        # Everything that turns a command-line string into emulator input, so a
        # bad argument of ANY kind gets the same one-line message rather than a
        # traceback from somewhere deep in setup.
        emu = build_emulator(
            args.dump_file, key=parse_key(args.key), chip=args.chip,
            with_boot=args.with_boot, only_uart=args.only_uart,
            uart1_hex=args.uart1_hex, uart1_rx=uart1_rx,
            uart1_rx_delay=args.uart1_rx_delay, tuyamcu=args.tuyamcu,
            tuyamcu_pid=args.tuyamcu_pid, tuyamcu_raw=args.tuyamcu_raw,
            tuyamcu_dps=parse_dp_specs(args.tuyamcu_dps),
            tuyamcu_injects=parse_hex_blobs(args.tuyamcu_injects),
            xvr_selfclear=args.xvr_selfclear, ble_core=args.ble_core)
    except ValueError as e:
        print(e)
        sys.exit(1)

    emu.setup()
    emu.run()

if __name__ == "__main__":
    main()
