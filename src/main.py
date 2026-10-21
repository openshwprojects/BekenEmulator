import argparse
import sys
import os

# Add root directory to sys.path so we can import src.*
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.crypto import (extract_and_decrypt, strip_crcs, parse_key, KNOWN_KEYS,
                        detect_bk7252_app_only)
from src.chips import chip_names, get_chip
from src.chips.base import DEFAULT_FLASH_SIZE
from src.emulator import BekenEmulator, CONSOLE_RX_HOLDOFF_INSNS
from src import tuyamcu, tuya_config
from src.periph import parse_adc_specs
from src.wifipeer import WIFI_ROLES

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


def parse_forwards(specs):
    """Parse --net-forward 'LPORT:DPORT' items into [(lport, dport)]."""
    out = []
    for spec in specs:
        parts = spec.split(":")
        if len(parts) != 2 or not all(p.isdigit() and 0 < int(p) < 65536 for p in parts):
            raise ValueError("bad --net-forward %r (want LPORT:DPORT, ports 1-65535, e.g. 8080:80)"
                             % spec)
        out.append((int(parts[0]), int(parts[1])))
    return out


def parse_mqtt_pubs(specs):
    """Parse --mqtt-publish 'TOPIC=PAYLOAD' items into [(topic, payload bytes)]."""
    out = []
    for spec in specs:
        topic, sep, payload = spec.partition("=")
        if not sep or not topic:
            raise ValueError("bad --mqtt-publish %r (want TOPIC=PAYLOAD, e.g. cmnd/obk/POWER=ON)" % spec)
        out.append((topic, payload.encode("utf-8")))
    return out


def parse_gpio_inputs(specs):
    """Parse --gpio-input 'PIN:LEVEL' items into {pin: level}."""
    out = {}
    for spec in specs or ():
        parts = spec.split(":", 1)
        if len(parts) != 2:
            raise ValueError("bad --gpio-input %r (want PIN:LEVEL, e.g. 20:1)" % spec)
        try:
            pin = int(parts[0], 0)
            lvl = int(parts[1], 0)
        except ValueError:
            raise ValueError("bad --gpio-input %r (want PIN:LEVEL, e.g. 20:1)" % spec)
        if lvl not in (0, 1):
            raise ValueError("bad --gpio-input %r (level must be 0 or 1)" % spec)
        out[pin] = lvl
    return out


def parse_deep_sleep_wakes(specs):
    """Parse --deep-sleep-wake items ('rtc', 'gpio', 'gpio:PIN', 'auto')."""
    out = []
    for spec in specs or ():
        text = str(spec).strip().lower()
        if text in ("rtc", "gpio", "auto"):
            out.append(text)
        elif text.startswith("gpio:"):
            try:
                pin = int(text.split(":", 1)[1], 0)
            except ValueError:
                raise ValueError("bad --deep-sleep-wake %r (want rtc, gpio, gpio:PIN, or auto)" % spec)
            out.append("gpio:%d" % pin)
        else:
            raise ValueError("bad --deep-sleep-wake %r (want rtc, gpio, gpio:PIN, or auto)" % spec)
    return out


# The checks below are shared by the command line and the GUI, so the two
# accept exactly the same values. Each raises ValueError naming the flag.
def parse_http_gets(paths):
    """Check --net-http-get paths: the request line is sent as Latin-1, so a
    path holding anything else has to be percent-encoded first."""
    for path in paths:
        try:
            path.encode("latin-1")
        except UnicodeEncodeError:
            raise ValueError("bad --net-http-get %r (percent-encode characters outside "
                             "Latin-1, e.g. %%20 for a space)" % path)
    return list(paths)


def check_wifi_pass(passphrase):
    """Check --wifi-pass: WPA2-PSK takes an 8 to 63 character passphrase."""
    if passphrase is not None and not 8 <= len(passphrase) <= 63:
        raise ValueError("bad --wifi-pass (a WPA2 passphrase is 8 to 63 characters; this one "
                         "is %d)" % len(passphrase))
    return passphrase


def fit_flash(physical, chip):
    """Pad a physical dump with erased bytes out to the size of the flash chip.

    The size is the chip's; a chip without one takes the smallest power of two
    that holds the dump, at least DEFAULT_FLASH_SIZE.
    """
    flash_size = chip.flash_size
    if flash_size is None:
        flash_size = DEFAULT_FLASH_SIZE
        while flash_size < len(physical):
            flash_size *= 2
    if len(physical) > flash_size:
        raise ValueError("The dump (%d bytes) does not fit the %d-byte flash of %s."
                         % (len(physical), flash_size, chip.name))
    return bytes(physical) + b"\xff" * (flash_size - len(physical))


def check_tuyamcu_pid(pid):
    """Check --tuyamcu-pid: the MCU sends it as ASCII on the wire."""
    if pid and not pid.isascii():
        raise ValueError("bad --tuyamcu-pid %r (a Tuya product id is plain ASCII)" % pid)
    return pid


def parse_mac(text):
    parts = text.replace("-", ":").split(":")
    if len(parts) != 6:
        raise ValueError("bad --wifi-mac %r (want aa:bb:cc:dd:ee:ff)" % text)
    try:
        return bytes(int(p, 16) for p in parts)
    except ValueError:
        raise ValueError("bad --wifi-mac %r (want aa:bb:cc:dd:ee:ff)" % text)


def parse_pc(text):
    """Parse one ARM/Thumb code address for --trace-pc."""
    try:
        address = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("want an address such as 0x1234")
    if not 0 <= address <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("address must fit in 32 bits")
    return address


def parse_args():
    parser = argparse.ArgumentParser(description="BK7231T/N Emulator")
    parser.add_argument("dump_file", help="Path to the raw BK7231 flash dump")
    parser.add_argument("--only-uart", action="store_true", help="Only print UART output (suppress MMIO/Trace/Info logs)")
    parser.add_argument("--trace-pc", dest="trace_pcs", action="append", default=[],
                        type=parse_pc, metavar="ADDRESS",
                        help="Report the first execution of this exact guest code address as an "
                             "[EMU_PC] line. Repeatable; useful when firmware keeps running after "
                             "its UART log goes quiet. Bit 0 may mark a Thumb address.")
    parser.add_argument("--with-boot", action="store_true", help="Start execution from the chip's bootloader partition instead of its app partition")
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
    parser.add_argument("--ble-core", dest="ble_core", action="store_true", default=None,
                        help="Force the RivieraWaves BLE 5.x core model for an N/M/BL2028N image "
                             "being run under the ambiguous legacy -chip BK7231 identity. "
                             "-chip BK7231N and BK7238 select their real BLE 5.1/5.2 hardware "
                             "automatically. The model supplies clocks, interrupts, timers and "
                             "AES; staged advertisements are printed as [BLE_ADV] lines. No BLE "
                             "radio is modelled. Rejected on chips without a modelled 5.x core.")
    parser.add_argument("--wifi", dest="wifi", action="store_true",
                        help="Model the RivieraWaves Wi-Fi MAC hardware at 0xC0000000 (TX/RX "
                             "descriptor DMA, the 1 MHz MAC clock, ABS timers, TBTT and the "
                             "six MAC FIQ lines) so the firmware's own Wi-Fi stack can "
                             "actually move frames. Pair with --wifi-peer to put a virtual "
                             "access point / station and an IP network on the other side.")
    parser.add_argument("--wifi-peer", dest="wifi_peer", default=None, choices=list(WIFI_ROLES),
                        metavar="ROLE",
                        help="Attach a virtual 802.11 peer plus a user-mode IP network to the MAC "
                             "model (implies --wifi). ROLE auto: take the opposite role of what "
                             "the device does (it beacons -> we join it as a station; it probes "
                             "or authenticates -> we are the access point it connects to). "
                             "sta/ap force one. As an access point the peer is open unless "
                             "--wifi-pass makes it WPA2-PSK; as a station it joins open "
                             "networks only.")
    parser.add_argument("--wifi-ssid", dest="wifi_ssid", default=None, metavar="SSID",
                        help="SSID the peer beacons as an access point, or the one it joins as a "
                             "station. In the AP role without this, the first SSID the device "
                             "probes for is adopted.")
    parser.add_argument("--wifi-pass", dest="wifi_pass", default=None, metavar="PASSPHRASE",
                        help="Make the peer's access point WPA2-PSK protected with this "
                             "passphrase instead of open. A paired device that stored a "
                             "protected network will not join an open one of the same name, so "
                             "this is what lets it on. The device must hold the same passphrase: "
                             "the four-way handshake is how each side proves that, and the link "
                             "carries no traffic until it completes.")
    parser.add_argument("--wifi-channel", dest="wifi_channel", type=int, default=1, metavar="N",
                        help="Channel the peer's access point announces (default 1).")
    parser.add_argument("--wifi-mac", dest="wifi_mac", default="02:00:00:00:00:01", metavar="MAC",
                        help="MAC address of the peer (default 02:00:00:00:00:01).")
    parser.add_argument("--net-forward", dest="net_forwards", action="append", default=[],
                        metavar="LPORT:DPORT",
                        help="Listen on 127.0.0.1:LPORT and connect each accepted TCP connection "
                             "to the device's port DPORT over the virtual network, e.g. "
                             "--net-forward 8080:80 then open http://127.0.0.1:8080/ for the "
                             "firmware's own web panel. Repeatable.")
    parser.add_argument("--net-out", dest="net_out", action="store_true",
                        help="Let the device's outgoing DNS/TCP/UDP reach real hosts through "
                             "the host's sockets. For Tuya this can reconnect an identity "
                             "already bound to the same app account; it does not pair or add "
                             "a device. Default off: an offline network where DNS "
                             "names resolve to private sinkhole addresses, TCP connections are "
                             "accepted and what the firmware sends is logged ([NET] lines) and "
                             "captured, NTP is answered with the host clock.")
    parser.add_argument("--net-http-get", dest="net_http_gets", action="append", default=[],
                        metavar="PATH",
                        help="Once the device is reachable on the virtual network, send it "
                             "\"GET PATH\" on port 80 and log the status line and body as a [NET] "
                             "line. Repeatable; requests go one after another. E.g. "
                             "--net-http-get \"/cm?cmnd=POWER%%20ON\" drives OpenBeken's command API.")
    parser.add_argument("--mqtt-publish", dest="mqtt_pubs", action="append", default=[],
                        metavar="TOPIC=PAYLOAD",
                        help="Have the offline MQTT broker push this message to the device, once "
                             "the device subscribes to a topic filter that matches it (+ and # "
                             "wildcards honoured, as a real broker does). This is the way in: the "
                             "firmware picked those filters itself, so e.g. --mqtt-publish "
                             "\"cmnd/obkSim/POWER=ON\" drives OpenBeken over MQTT. Repeatable.")
    parser.add_argument("--pcap-ip", dest="pcap_ip", default=None, metavar="FILE",
                        help="Write every frame the IP bridge carried to FILE (link type "
                             "Ethernet) for Wireshark: the device's own traffic, both "
                             "directions, exactly as it crossed. This is the network; --pcap is "
                             "the air.")
    parser.add_argument("--pcap", dest="pcap", default=None, metavar="FILE",
                        help="Write every 802.11 frame crossing the peer to FILE (link type "
                             "IEEE 802.11, no FCS) for Wireshark.")
    parser.add_argument("--snapshot-save", dest="snapshot_save", default=None, metavar="FILE",
                        help="Save a snapshot of the whole device (memory, CPU, peripheral state) "
                             "to FILE when --snapshot-at is reached, and keep running.")
    parser.add_argument("--snapshot-at", dest="snapshot_at", default=None, metavar="WHEN",
                        help="When to take the --snapshot-save snapshot: an instruction count, or a "
                             "text marker that must appear in the UART output, e.g. "
                             "\"sending broadcast_deauth\" for OpenBeken's AP being up.")
    parser.add_argument("--snapshot-load", dest="snapshot_load", default=None, metavar="FILE",
                        help="Resume from a snapshot instead of booting. The dump, -chip and --wifi "
                             "must match the run that saved it; a peer / port forwards / pcap "
                             "given now are attached to the restored device.")
    parser.add_argument("--gpio-input", dest="gpio_inputs", action="append", default=[],
                        metavar="PIN:LEVEL",
                        help="Drive physical GPIO pin PIN to LEVEL (0 or 1) from power-on. "
                             "Repeatable, e.g. --gpio-input 20:1 holds an active-low reset "
                             "button high at its external pull-up idle state.")
    parser.add_argument("--adc", dest="adc_inputs", action="append", default=[],
                        metavar="CH:VALUE",
                        help="Set SARADC analog input channel CH (0..15, chN, or P<pin>) to "
                             "VALUE (raw 10-bit 0..1023 or voltage with 'V'/'mV' suffix, e.g. "
                             "--adc 3:512 or --adc P26:1.65V). Repeatable.")
    parser.add_argument("--deep-sleep-wake", dest="deep_sleep_wakes", action="append", default=[],
                        metavar="SPEC",
                        help="Automatically wake the chip when it enters deep sleep (0x800104=0xADC1). "
                             "SPEC is rtc (RTC timer), gpio (first wake-enabled GPIO), gpio:PIN "
                             "(specific GPIO pin), or auto. Repeatable for multi-cycle sleep/wake.")
    parser.add_argument("-key", "--key", dest="key", default=None, metavar="KEY",
                        help="Firmware decryption key: a known name (%s), 32 hex chars, or base64 of 16 bytes. "
                             "Omit for plaintext images (no decryption)." % ", ".join(sorted(KNOWN_KEYS)))
    parser.add_argument("-chip", "--chip", dest="chip", default="BK7231", metavar="CHIP",
                        help="Chip/variant served from the SCTRL id registers: %s. "
                             "Default: BK7231; select BK7231U for its GPIO32..39 bank."
                             % ", ".join(chip_names()))
    return parser.parse_args()

def build_emulator(dump_path, key=None, chip="BK7231", with_boot=False, only_uart=False,
                   uart1_hex=False, uart1_rx=b"", uart1_rx_delay=None, tuyamcu=False,
                   tuyamcu_pid=None, tuyamcu_raw=False, tuyamcu_dps=None,
                   tuyamcu_injects=None, xvr_selfclear=False, uart_sink=None,
                   save_decrypted=True, log=None, ble_core=None, wifi=False,
                   deep_sleep_wakes=None, gpio_inputs=None, adc_inputs=None):
    """Load a dump and return a configured, not-yet-started BekenEmulator.

    Everything between "here is an image and some options" and "start running"
    lives here, so the GUI drives the emulator through exactly the same path as
    the command line instead of a second copy that quietly drifts from it.
    Rejects bad options with ValueError carrying the message the CLI prints.

    `key`, `tuyamcu_dps` and `tuyamcu_injects` are the parsed forms; the CLI
    string parsers above turn command-line text into them first. `ble_core`
    is tri-state: None selects hardware from the unambiguous chip identity,
    True forces the legacy generic-BK7231 mapping, and False explicitly turns
    it off for diagnostic callers.
    """
    if log is None:
        log = (lambda *a: None) if only_uart else print

    chip = get_chip(chip)

    with open(dump_path, "rb") as f:
        raw_data = f.read()

    # Some BK7252 UART tools read from physical flash 0x11000 and save that
    # window at file offset zero.  The flash controller's 32-data + 2-CRC
    # mapping makes this CPU/XIP address 0x10000: it is an app-only image, not
    # a full dump whose first bytes belong at address zero.
    app_window = detect_bk7252_app_only(raw_data) if chip.app_only_phys is not None else None
    if app_window is not None:
        if key is not None:
            raise ValueError("Detected a plaintext BK7252 app-only dump; do not pass -key.")
        if with_boot:
            raise ValueError("This BK7252 dump starts at physical flash 0x11000 and has no "
                             "bootloader; remove --with-boot.")

        app, encoded_size = app_window
        bootloader = None

        # Keep the application at its linked address.  The ARM968E-S vector
        # base is the CPU address of the dump in this direct-app configuration,
        # so the omitted low bootloader region remains erased rather than
        # containing a fake copy.
        base = chip.app_only_cpu
        if base + len(app) > BekenEmulator.FLASH_SIZE:
            raise ValueError("Decoded BK7252 application does not fit the 2 MiB XIP window.")
        logical_flash = bytearray(b"\xff" * BekenEmulator.FLASH_SIZE)
        logical_flash[base:base + len(app)] = app
        flash_data = bytes(logical_flash)

        # Software flash-controller operations use physical addresses.  File
        # offset zero came from app_only_phys, so represent the omitted low
        # region as erased rather than shifting config/filesystem reads down.
        physical_data = (b"\xff" * chip.app_only_phys) + raw_data
        log("Detected BK7252 app-only dump: file offset 0 is physical 0x%X / "
            "CPU 0x%X." % (chip.app_only_phys, base))
        log("Decoded CRC-framed app prefix: %d bytes from %d encoded bytes."
            % (len(app), encoded_size))
    else:
        flash_data = strip_crcs(raw_data)
        bootloader = extract_and_decrypt(flash_data, "bootloader", key,
                                         chip.bootloader_addr, chip.app_addr)
        app = extract_and_decrypt(flash_data, "app", key,
                                  chip.bootloader_addr, chip.app_addr)
        physical_data = raw_data
    physical_data = fit_flash(physical_data, chip)

    if app and save_decrypted and not os.environ.get("EMU_REPORT"):
        with open("app_decrypted.bin", "wb") as f:
            f.write(app)

    if app_window is not None:
        log("Bootloader region is absent; starting the app directly at 0x%X." % chip.app_addr)
    elif bootloader is None:
        log("Warning: Could not find 'bootloader' RBL container. Booting from 0x%X." % chip.app_addr)
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
    check_tuyamcu_pid(tuyamcu_pid)

    return BekenEmulator(raw_flash=flash_data, bootloader=bootloader, app=app,
                         with_boot=with_boot, only_uart=only_uart,
                         chip=chip, uart1_hex=uart1_hex,
                         physical_flash=physical_data, uart1_rx=uart1_rx,
                         uart1_rx_delay=uart1_rx_delay, tuyamcu_enabled=tuyamcu,
                         tuyamcu_pid=tuyamcu_pid, tuyamcu_raw=tuyamcu_raw,
                         xvr_selfclear=xvr_selfclear, tuyamcu_dps=tuyamcu_dps,
                         tuyamcu_injects=tuyamcu_injects, uart_sink=uart_sink,
                         ble_core=ble_core, wifi=wifi,
                         exception_vector_base=chip.app_only_cpu if app_window is not None else 0,
                         deep_sleep_wakes=deep_sleep_wakes,
                         gpio_inputs=gpio_inputs,
                         adc_inputs=adc_inputs)


def main():
    args = parse_args()

    uart1_rx = b""
    if args.uart1_rx is not None:
        # Let \n and \r in the argument mean real line endings; default to CRLF.
        text = args.uart1_rx.encode("latin-1").decode("unicode_escape")
        if not text.endswith(("\n", "\r")):
            text += "\r\n"
        uart1_rx = text.encode("latin-1", "replace")

    # Any network option means a peer, and a peer means the MAC model. Listed
    # once: --net-out and --pcap-ip given alone used to be silently ignored.
    want_peer = bool(args.wifi_peer or args.net_forwards or args.pcap or args.wifi_ssid
                     or args.net_http_gets or args.mqtt_pubs or args.wifi_pass
                     or args.net_out or args.pcap_ip)

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
            xvr_selfclear=args.xvr_selfclear, ble_core=args.ble_core,
            wifi=args.wifi or want_peer,
            deep_sleep_wakes=parse_deep_sleep_wakes(args.deep_sleep_wakes),
            gpio_inputs=parse_gpio_inputs(args.gpio_inputs),
            adc_inputs=parse_adc_specs(args.adc_inputs, chip=args.chip))
        if want_peer:
            # The offline TLS peer needs the image's PSK to authenticate its
            # local broker session.  --net-out is an opaque end-to-end relay,
            # so do not recover or retain a redundant host-side copy there.
            decoded_config = None if args.net_out else tuya_config.extract(args.dump_file)
            tls_psks, tuya_legacy_identity_psk = tuya_config.offline_tls_config(
                decoded_config, net_out=args.net_out)
            emu.attach_wifi_peer(role=args.wifi_peer or "auto", ssid=args.wifi_ssid,
                                 channel=args.wifi_channel, peer_mac=parse_mac(args.wifi_mac),
                                 passphrase=check_wifi_pass(args.wifi_pass),
                                 forwards=parse_forwards(args.net_forwards),
                                 net_out=args.net_out, pcap=args.pcap,
                                 http_gets=parse_http_gets(args.net_http_gets),
                                 mqtt_pubs=parse_mqtt_pubs(args.mqtt_pubs),
                                 tls_psks=tls_psks,
                                 tuya_legacy_identity_psk=tuya_legacy_identity_psk)
        if args.pcap_ip:
            from src import netstack as _netstack
            ip_cap = _netstack.EthPcap(args.pcap_ip)
            emu.net_frame_sink = lambda d, f, us: ip_cap.write(f, us)
            emu.net_log("[NET] writing the bridge's frames to %s" % args.pcap_ip)
        if args.snapshot_save:
            if not args.snapshot_at:
                raise ValueError("--snapshot-save needs --snapshot-at (an instruction count or a UART marker)")
            at = int(args.snapshot_at) if args.snapshot_at.isdigit() else args.snapshot_at
            emu.arm_snapshot(args.snapshot_save, at)
    except ValueError as e:
        print(e)
        sys.exit(1)

    emu.setup()
    for address in args.trace_pcs:
        emu.trace_pc(address)
    if args.snapshot_load:
        try:
            emu.load_snapshot(args.snapshot_load)
        except (ValueError, OSError) as e:
            print(e)
            sys.exit(1)
    emu.run()

if __name__ == "__main__":
    main()
