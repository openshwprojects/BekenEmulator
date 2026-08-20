"""Recover Tuya's on-flash configuration from a BK7231 dump.

Implements the same scheme BK7231GUIFlashTool and bk7231tools use, rather than
scraping plaintext (which only works on a handful of older images):

  1. Find the key block - a 4K block whose first AES-ECB decryption under the
     fixed master key starts with MAGIC_KEY. Its header is
     `<I magic, I crc, 16s inner_key>`, and that inner key is exactly what the
     firmware prints as "get key:" during boot.
  2. Build the data key from two fixed strings plus that inner key, byte-wise:
        data_key[i] = (KEY_PART_1[i & 3] + KEY_PART_2[i] + inner_key[i]) % 256
  3. AES-ECB decrypt the storage area with it. Out come the stored records:
     the network block, the datapoint schema, the pin/`user_param_key` map.

Older firmware keeps `user_param_key` in the clear, so a plaintext scan is kept
as a fallback for those.
"""
import json
import re
import struct

try:
    from Crypto.Cipher import AES
except ImportError:                     # extraction is optional; tests never depend on it
    AES = None

KEY_MASTER = b"qwertyuiopasdfgh"
KEY_PART_1 = b"8710_2M"
KEY_PART_2 = b"HHRRQbyemofrtytf"
MAGIC_KEY = 0x13579753

STORAGE_FROM = 0x1C0000       # storage/user area sits above the application
BLOCK = 0x1000


def _data_aes(inner_key):
    key = bytearray(16)
    for i in range(16):
        key[i] = (KEY_PART_1[i & 0b11] + KEY_PART_2[i] + inner_key[i]) % 256
    return AES.new(bytes(key), AES.MODE_ECB)


def find_key_block(data):
    """Locate the key block and return (offset, inner_key), or (None, None)."""
    if AES is None:
        return None, None
    master = AES.new(KEY_MASTER, AES.MODE_ECB)
    for off in range(STORAGE_FROM, len(data) - 32, BLOCK):
        head = master.decrypt(data[off:off + 32])
        magic, _crc = struct.unpack("<II", head[:8])
        if magic == MAGIC_KEY:
            return off, head[8:24]
    return None, None


def decrypt_storage(data, inner_key):
    """Decrypt the whole storage area with the derived data key."""
    seg = data[STORAGE_FROM:]
    seg = seg[:len(seg) // 16 * 16]
    return _data_aes(inner_key).decrypt(seg)


# --- record recovery ---------------------------------------------------------

def _json_objects(buf, limit=4000):
    """Yield balanced {...} spans of printable text.

    A plain regex cannot do this: the datapoint records nest
    ({"mode":"rw","property":{...},"id":103}), so a non-greedy match stops at
    the inner closing brace and yields something json.loads rejects.
    """
    n = len(buf)
    i = buf.find(b"{")
    while 0 <= i < n:
        depth = 0
        for j in range(i, min(i + limit, n)):
            c = buf[j]
            if not (32 <= c <= 126 or c in (9, 10, 13)):
                break                       # ran into binary - not a record
            if c == 0x7B:
                depth += 1
            elif c == 0x7D:
                depth -= 1
                if depth == 0:
                    yield i, buf[i:j + 1]
                    i = buf.find(b"{", j + 1)
                    break
        else:
            i = buf.find(b"{", i + 1)
            continue
        if depth != 0:
            i = buf.find(b"{", i + 1)


_JSON = re.compile(rb"\{[ -~\r\n\t]{15,4000}?\}")   # kept for the loose fallback
# Tuya's pin map uses unquoted keys: {rl1_pin:6,module:CB2S,crc:74,}
_LOOSE = re.compile(rb"\{[a-zA-Z0-9_]+:[ -~]{20,2000}\}")


def _decode(blob):
    try:
        return json.loads(blob.decode("utf-8", "replace"))
    except Exception:
        return None


def _parse_loose(text):
    out = []
    for part in text.strip("{}").split(","):
        k, _, v = part.strip().partition(":")
        if k.strip():
            out.append((k.strip().strip('"'), v.strip().strip('"')))
    return out


def _collect(buf):
    """Pull the interesting records out of a (decrypted) buffer."""
    found = {"network": None, "dps": [], "pins": [], "ids": {}, "raw": []}
    seen_dp = set()
    for _off, blob in _json_objects(buf):
        obj = _decode(blob)
        if not isinstance(obj, dict):
            continue
        text = blob.decode("ascii", "replace")
        if "nc_tp" in obj or "ssid" in obj:
            if found["network"] is None:
                found["network"] = obj
                found["raw"].append(text)
        elif "id" in obj and "property" in obj:
            if obj["id"] in seen_dp:        # the schema is stored more than once
                continue
            seen_dp.add(obj["id"])
            found["dps"].append(obj)
            if len(found["raw"]) < 6:
                found["raw"].append(text)
        elif "schemaId" in obj or "productKey" in obj or "devId" in obj:
            found["ids"].update({k: str(v) for k, v in obj.items()
                                 if isinstance(v, (str, int)) and len(str(v)) < 48})
    for m in _LOOSE.finditer(buf):
        text = m.group().decode("ascii", "replace")
        if any(s in text for s in ("_pin", "module", "Jsonver", "jv:")):
            found["pins"] = _parse_loose(text)
            found["raw"].insert(0, text)
            break
    for name in (b"uuid", b"auth_key", b"ap_ssid", b"pskKey"):
        m = re.search(name + rb"[\"':= ]{1,4}([A-Za-z0-9_\-]{8,40})", buf)
        if m:
            found["ids"][name.decode()] = m.group(1).decode("ascii", "replace")
    return found


# --- human-readable summary --------------------------------------------------

# Ported from BK7231GUIFlashTool's GetKeysHumanReadableInternal
# (BK7231Flasher/Utils/TuyaConfig.cs) so the wording matches the tool exactly.
# Each rule is (pattern, template); {n} is the channel number taken from the
# key, {v} is the stored value. Order matters - first match wins.
_PIN_RULES = [
    (r"^led(\d+)_pin$",        "LED (channel {n}) on P{v}"),
    (r"^netled\d+_pin$",       "WiFi LED on P{v}"),
    (r"^netled_pin$",          "WiFi LED on P{v}"),
    (r"^wfst$",                "WiFi LED on P{v}"),
    (r"^wfst_pin$",            "WiFi LED on P{v}"),
    (r"bz_pin_pin",            "Buzzer Pin (TODO) on P{v}"),
    (r"^buzzer_io$",           "Buzzer Pin (TODO) on P{v}"),
    (r"status_led_pin",        "Status LED on P{v}"),
    (r"remote_io",             "RF Remote on P{v}"),
    (r"samp_sw_pin",           "Battery Relay on P{v}"),
    (r"samp_pin",              "Battery ADC on P{v}"),
    (r"i2c_scl_pin",           "I2C SCL on P{v}"),
    (r"i2c_sda_pin",           "I2C SDA on P{v}"),
    (r"alt_pin_pin",           "ALT pin on P{v}"),
    (r"one_wire_pin",          "OneWire IO pin on P{v}"),
    (r"backlit_io_pin",        "Backlit IO pin on P{v}"),
    (r"^max_V$",               "Battery Max Voltage: {v}"),
    (r"^min_V$",               "Battery Min Voltage: {v}"),
    (r"^rl$",                  "Relay (channel 0) on P{v}"),
    (r"^rl(\d+)_pin$",         "Relay (channel {n}) on P{v}"),
    (r"^rl_on(\d+)_pin$",      "Bridge Relay On (channel {n}) on P{v}"),
    (r"^rl_off(\d+)_pin$",     "Bridge Relay Off (channel {n}) on P{v}"),
    (r"^bt_pin$",              "Button (channel 0) on P{v}"),
    (r"^bt$",                  "Button (channel 0) on P{v}"),
    (r"^k(\d+)pin_pin$",       "Button (channel {n}) on P{v}"),
    (r"^bt(\d+)_pin$",         "Button (channel {n}) on P{v}"),
    (r"^door(\d+)_magt_pin$",  "Door Sensor (channel {n}) on P{v}"),
    (r"^onoff(\d+)$",          "TglChannelToggle (channel {n}) on P{v}"),
    (r"gate_sensor_pin_pin",   "Door/Gate sensor on P{v}"),
    (r"basic_pin_pin",         "PIR sensor on P{v}"),
    (r"^ele_pin$",             "BL0937 ELE (CF) on P{v}"),
    (r"^epin$",                "BL0937 ELE (CF) on P{v}"),
    (r"^vi_pin$",              "BL0937 VI (CF1) on P{v}"),
    (r"^ivpin$",               "BL0937 VI (CF1) on P{v}"),
    (r"sel_pin_pin",           "BL0937 SEL on P{v}"),
    (r"^ivcpin$",              "BL0937 SEL on P{v}"),
    (r"^r_pin$",               "LED Red (Channel 1) on P{v}"),
    (r"^g_pin$",               "LED Green (Channel 2) on P{v}"),
    (r"^b_pin$",               "LED Blue (Channel 3) on P{v}"),
    (r"^c_pin$",               "LED Cool (Channel 4) on P{v}"),
    (r"^w_pin$",               "LED Warm (Channel 5) on P{v}"),
    (r"^mic$",                 "Microphone (TODO) on P{v}"),
    (r"^micpin$",              "Microphone (TODO) on P{v}"),
    (r"^ctrl_pin$",            "Control Pin (TODO) on P{v}"),
    (r"^buzzer_pwm$",          "Buzzer Frequency (TODO) is {v}"),
    (r"^irpin$",               "IR Receiver is on P{v}"),
    (r"^infrr$",               "IR Receiver is on P{v}"),
    (r"^infre$",               "IR Transmitter is on P{v}"),
    (r"^reset_pin$",           "Button is on P{v}"),
    (r"^pwmhz$",               "PWM Frequency {v}"),
    (r"^pirsense_pin$",        "PIR Sensitivity {v}"),
    (r"^pirlduty$",            "PIR Low Duty {v}"),
    (r"^pirfreq$",             "PIR Frequency {v}"),
    (r"^pirmduty$",            "PIR High Duty {v}"),
    (r"^pirin_pin$",           "PIR Input {v}"),
    (r"^mosi$",                "SPI MOSI {v}"),
    (r"^miso$",                "SPI MISO {v}"),
    (r"^SCL$",                 "SPI SCL {v}"),
    (r"^CS$",                  "SPI CS {v}"),
    (r"^total_bt_pin$",        "Pair/Toggle All Button on P{v}"),
]
_PIN_RULES = [(re.compile(p), t) for p, t in _PIN_RULES]


def describe_pin(key, value):
    """Return the tool's bullet text for a key, or None if it is not a pin key."""
    for rx, template in _PIN_RULES:
        m = rx.search(key)
        if m:
            n = m.group(1) if m.groups() else ""
            return template.format(n=n, v=value)
    return None


_NAMES = {
    "module": "Tuya module", "jv": "Config version", "Jsonver": "Config version",
    "crc": "Config checksum", "category": "Product category",
    "ch_num": "Channels (gangs)", "net_trig": "Net-config trigger",
    "reset_t": "Reset hold time (s)", "rstnum": "Power cycles to reset",
    "wfcfg": "Pairing mode", "wfct": "Pairing timeout",
    "cmod": "Colour mode", "cwtype": "Cold/warm driver", "dmod": "Dimmer mode",
    "pwmhz": "PWM frequency (Hz)", "brightmin": "Min brightness (%)",
    "brightmax": "Max brightness (%)", "colormin": "Min colour (%)",
    "colormax": "Max colour (%)", "cwmaxp": "Max C+W power (%)",
    "deftemp": "Default colour temp", "defbright": "Default brightness (%)",
    "defcolor": "Default colour", "pmemory": "Restore state after power loss",
    "onoffmode": "On/off behaviour", "remdmode": "Remote/dimming mode",
    "prodagain": "Allow re-running prod test", "title20": "title20 flag",
    "nety_led": "Net LED state (paired)", "netn_led": "Net LED state (unpaired)",
    "netled_reuse": "Net LED pin reused", "total_stat": "Total-status mode",
    "rstcor": "Reset indicator colour", "rstbr": "Reset indicator brightness (%)",
    "rsttemp": "Reset indicator temp", "cagt": "Colour adjust time",
    "wt": "White transition", "rgbt": "RGB transition", "ffc_select": "FFC variant",
}
_ROLE = {"rl": "Relay", "ch": "Channel", "netled": "Network LED", "bt": "Button",
         "total_bt": "Button (combined)", "c": "Cold white", "w": "Warm white",
         "r": "Red", "g": "Green", "b": "Blue", "net": "Network"}
_VALUES = {("cmod", "cw"): "cold/warm white", ("cmod", "rgb"): "RGB",
           ("cmod", "rgbcw"): "RGB + cold/warm", ("cmod", "c"): "single white",
           ("defcolor", "c"): "cold white", ("defcolor", "w"): "warm white",
           ("rstcor", "c"): "cold white", ("wfcfg", "spcl"): "special (hold button)",
           ("wfcfg", "old"): "legacy pairing"}


def _describe(key, value):
    label = _NAMES.get(key)
    if label is None:
        m = re.match(r"^([a-z_]*?)(\d*)(_pin|_lv|_dpid|_type)$", key)
        if m:
            base, idx, suffix = m.groups()
            role = _ROLE.get(base.rstrip("_"), base.rstrip("_") or "Pin")
            what = {"_pin": "GPIO", "_lv": "active level",
                    "_dpid": "datapoint id", "_type": "type"}[suffix]
            label = " ".join(("%s %s %s" % (role, idx, what)).split())
        else:
            label = key
    pretty = _VALUES.get((key, value))
    if pretty is None and key.endswith("_lv"):
        pretty = {"0": "active low", "1": "active high"}.get(value)
    if pretty is None and key.endswith("_pin"):
        pretty = "P%s" % value
    return label, (pretty or value)


_DP_TYPE = {"value": "numeric", "bool": "on/off", "enum": "enum",
            "string": "text", "bitmap": "bitmap", "raw": "raw"}


def _redact(value):
    """Mask secrets before they reach a published report.

    Decrypted Tuya storage carries the owner's WiFi SSID and PSK plus the
    device's auth key. These dumps are public, but re-publishing someone's home
    network name and credentials on a web page is not something to do by
    default. Shape is kept so the field is still recognisable.
    """
    v = str(value)
    if len(v) <= 4:
        return "*" * len(v)
    return "%s%s%s" % (v[:2], "*" * (len(v) - 4), v[-2:])


REDACT_SECRETS = True       # set False to show raw values locally


def summarise(found, key_off, inner_key, encrypted):
    """Build the [(label, value)] list shown beside the raw records."""
    out = []
    out.append(("Storage", "encrypted (decrypted with device key)" if encrypted
                else "plaintext"))
    if key_off is not None:
        out.append(("Key block", "0x%06X" % key_off))
    if inner_key:
        out.append(("Device key", inner_key.hex()))
    for name, label in (("uuid", "Device UUID"), ("auth_key", "Auth key"),
                        ("ap_ssid", "AP SSID"), ("pskKey", "PSK key")):
        if found["ids"].get(name):
            val = found["ids"][name]
            if REDACT_SECRETS and name in ("auth_key", "pskKey", "ap_ssid"):
                val = _redact(val)
            out.append((label, val))
    net = found.get("network")
    if net:
        if net.get("ssid"):
            ssid = str(net["ssid"])
            out.append(("Paired WiFi SSID",
                        _redact(ssid) if REDACT_SECRETS else ssid))
        out.append(("Network config type", str(net.get("nc_tp"))))
        out.append(("Network status", str(net.get("stat"))))
    if found["dps"]:
        out.append(("Datapoints defined", str(len(found["dps"]))))
        for dp in found["dps"][:8]:
            prop = dp.get("property", {})
            kind = _DP_TYPE.get(prop.get("type"), prop.get("type", "?"))
            detail = kind
            if "min" in prop and "max" in prop:
                detail += " %s..%s" % (prop["min"], prop["max"])
            if "range" in prop:
                detail += " " + "/".join(str(x) for x in prop["range"][:4])
            out.append(("  DP %s (%s)" % (dp.get("id"), dp.get("mode", "?")), detail))
    # Pin/wiring lines first, worded exactly as BK7231GUIFlashTool prints them.
    bullets = []
    others = []
    for k, v in found["pins"]:
        text = describe_pin(k, v)
        if text:
            bullets.append(("-", text))
        else:
            others.append(_describe(k, v))
    if bullets:
        out.append(("Pin configuration", "%d entries" % len(bullets)))
        out.extend(bullets)
    out.extend(others)
    return out


def extract(path):
    """Return a dict for the report, or None when nothing can be recovered."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None

    key_off, inner = find_key_block(data)
    found, encrypted = None, False
    if inner:
        found = _collect(decrypt_storage(data, inner))
        encrypted = True
    plain = _collect(data[STORAGE_FROM:])
    if found is None:
        found = plain
    else:
        # Older images keep the pin map in the clear even when the rest is not.
        if not found["pins"] and plain["pins"]:
            found["pins"] = plain["pins"]
            found["raw"].insert(0, plain["raw"][0] if plain["raw"] else "")
        for k, v in plain["ids"].items():
            found["ids"].setdefault(k, v)

    if not (found["pins"] or found["dps"] or found["network"] or found["ids"]):
        return None
    return {
        "offset": key_off if key_off is not None else STORAGE_FROM,
        "encrypted": encrypted,
        "raw": "\n\n".join(x for x in found["raw"][:6] if x),
        "pairs": found["pins"],
        "human": summarise(found, key_off, inner, encrypted),
    }
