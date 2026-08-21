r"""TuyaMCU serial-protocol codec and a simulated MCU peer.

TuyaMCU is the UART link between a Tuya Wi-Fi *module* (here, the BK7231 running
OpenBeken or stock Tuya firmware) and an external *MCU*. The module drives the
link: it sends heartbeats and queries; the MCU answers. Without an MCU on the
other end the firmware just repeats heartbeats forever, which is exactly what
our dumps do when emulated with nothing attached to UART1.

This module provides:
  * frame parse/build with the exact 0x55 0xAA framing and checksum, and
  * TuyaMCUSlave - a minimal MCU that answers a module's frames, enough to walk
    the firmware past its heartbeat loop into the product-info / working-mode /
    query-state handshake.

Frame layout (every field one byte unless noted):

    55 AA   ver   cmd   lenHi lenLo   payload...   checksum
    \___/   |     |     \_________/   \________/   |
    header  0x00  code   big-endian    len bytes   (sum of every preceding
                         length                     byte) & 0xFF

The checksum sums *all* bytes of the frame except the checksum itself, mod 256.
For the bare heartbeat 55 AA 00 00 00 00 that is 0x55+0xAA = 0xFF, which is the
exact byte the real firmware emits. Verified against a live emulated dump and
against OpenBK7231T_App/src/driver/drv_tuyaMCU.c.
"""

HEADER = b"\x55\xAA"

# Command codes, from OpenBK7231T_App drv_tuyaMCU.c. The module sends the query
# forms; the MCU answers with the same code.
CMD_HEARTBEAT      = 0x00   # keep-alive; module sends unprompted every ~3 s
CMD_QUERY_PRODUCT  = 0x01   # module asks the MCU for its product id / version
CMD_MCU_CONF       = 0x02   # module asks the MCU for its working mode
CMD_WIFI_STATE     = 0x03   # module reports its Wi-Fi state to the MCU
CMD_WIFI_RESET     = 0x04
CMD_WIFI_SELECT    = 0x05
CMD_SET_DP         = 0x06   # module -> MCU: set a data point
CMD_STATE          = 0x07   # MCU -> module: report data-point value(s)
CMD_QUERY_STATE    = 0x08   # module asks the MCU to report every data point
CMD_SET_TIME       = 0x1C
CMD_SET_RSSI       = 0x24
CMD_NETWORK_STATUS = 0x2B   # newer Wi-Fi-state report

# Data-point wire types (the tag inside a 0x07 report / 0x06 set).
DP_BOOL   = 0x01   # 1 byte, 0/1
DP_VALUE  = 0x02   # 4 bytes, big-endian signed
DP_STRING = 0x03   # raw bytes
DP_ENUM   = 0x04   # 1 byte
DP_BITMAP = 0x05   # 1/2/4 bytes


def checksum(data):
    """TuyaMCU checksum: the low byte of the sum of every byte given."""
    return sum(data) & 0xFF


def build_frame(cmd, payload=b"", version=0x00):
    """Assemble one framed TuyaMCU message (header .. checksum)."""
    payload = bytes(payload)
    body = bytes([0x55, 0xAA, version & 0xFF, cmd & 0xFF,
                  (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) + payload
    return body + bytes([checksum(body)])


def encode_dp(dp_id, dp_type, value):
    """Encode one data point as it appears inside a 0x07 report payload.

    Layout: dpId(1) type(1) len(2, big-endian) data. `value` is an int for
    BOOL/ENUM/BITMAP/VALUE (VALUE is 4-byte big-endian) or bytes/str for STRING.
    """
    if dp_type == DP_VALUE:
        data = int(value).to_bytes(4, "big", signed=True) if int(value) < 0 \
            else int(value).to_bytes(4, "big")
    elif dp_type in (DP_BOOL, DP_ENUM):
        data = bytes([int(value) & 0xFF])
    elif dp_type == DP_BITMAP:
        v = int(value)
        n = 1 if v <= 0xFF else (2 if v <= 0xFFFF else 4)
        data = v.to_bytes(n, "big")
    elif dp_type == DP_STRING:
        data = value.encode("utf-8") if isinstance(value, str) else bytes(value)
    else:
        raise ValueError("unknown dp type 0x%02X" % dp_type)
    return bytes([dp_id & 0xFF, dp_type & 0xFF,
                  (len(data) >> 8) & 0xFF, len(data) & 0xFF]) + data


class Frame:
    """One parsed TuyaMCU message. `valid` is the checksum verdict."""
    __slots__ = ("version", "cmd", "payload", "valid")

    def __init__(self, version, cmd, payload, valid=True):
        self.version = version
        self.cmd = cmd
        self.payload = bytes(payload)
        self.valid = valid

    def __repr__(self):
        return "Frame(cmd=0x%02X ver=0x%02X len=%d payload=%s valid=%s)" % (
            self.cmd, self.version, len(self.payload),
            self.payload.hex(" ") if self.payload else "-", self.valid)

    def __eq__(self, other):
        return (isinstance(other, Frame) and self.cmd == other.cmd
                and self.payload == other.payload and self.valid == other.valid)


class TuyaMCUParser:
    """Incremental byte-stream framer. feed(bytes) -> list[Frame].

    Tolerates the realities of a UART tap: bytes arriving a few at a time,
    garbage before a header, and a truncated trailing frame (kept until the
    rest arrives). A frame whose checksum is wrong is still returned, with
    valid=False, rather than silently dropped - so a test can assert on it.
    """

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += bytes(data)
        frames = []
        while True:
            i = self.buf.find(HEADER)
            if i < 0:
                # No header in the buffer. Keep a lone trailing 0x55 - it might
                # be the first half of a header still arriving; drop the rest.
                if self.buf and self.buf[-1] == 0x55:
                    del self.buf[:-1]
                else:
                    self.buf.clear()
                break
            if i > 0:
                del self.buf[:i]            # discard junk before the header
            if len(self.buf) < 6:
                break                        # need header..length first
            ln = (self.buf[4] << 8) | self.buf[5]
            total = 6 + ln + 1               # payload + checksum
            if len(self.buf) < total:
                break                        # wait for the rest of the frame
            raw = bytes(self.buf[:total])
            del self.buf[:total]
            ok = checksum(raw[:-1]) == raw[-1]
            frames.append(Frame(raw[2], raw[3], raw[6:6 + ln], valid=ok))
        return frames


class TuyaMCUSlave:
    """A minimal simulated MCU: answer a module's frames well enough that the
    firmware advances past its heartbeat loop.

    react(frame) -> list[bytes], the framed responses to feed back to the
    module. react_stream(data) parses raw bytes and reacts to each frame.
    """

    def __init__(self, product_key="bekenemulator000", version="1.0.0", dps=None,
                 raw_product=False):
        # product_key: the 16-char Tuya product id reported for a 0x01 query.
        # raw_product: product-info wire form. False -> JSON {"p":..,"v":..},
        #   which OpenBeken and older Tuya SDKs (e.g. 1.1.71) accept. True ->
        #   the raw 16-byte id + short version that TuyaOS 3.x wants (see
        #   _raw_product). The caller picks this per dump; it is NOT inferred,
        #   so a device is only ever sent the form it expects.
        # dps: {dp_id: (dp_type, value)} reported in response to a 0x08 query.
        self.product_key = product_key
        self.version = version
        self.raw_product = raw_product
        self.dps = dict(dps or {})
        self.parser = TuyaMCUParser()
        self.heartbeats = 0
        self.seen = []               # cmd bytes reacted to, for assertions

    def _raw_product(self):
        """Raw product-info payload for the TuyaOS 3.x wire form: the 16-byte
        product id followed by a short version, 16..24 bytes total.

        Verified against TuyaOS 3.11.12: its 0x01 handler rejects anything whose
        payload length is outside [16,24] ("prod len = N") and otherwise copies
        the first 16 bytes verbatim into gw_if.product_key. So the id must be
        exactly 16 bytes and match the module's own licensed product id.
        """
        pid = self.product_key.encode("ascii")[:16].ljust(16, b"\x00")
        return pid + self.version.encode("ascii")[:8]

    def react(self, frame):
        if not frame.valid:
            return []
        self.seen.append(frame.cmd)
        c = frame.cmd
        if c == CMD_HEARTBEAT:
            # First response 0x00 = "MCU just (re)started, please re-query";
            # subsequent 0x01 = "running normally". Matches real MCUs and makes
            # the module run its product/mode/state queries after the first ACK.
            state = 0x00 if self.heartbeats == 0 else 0x01
            self.heartbeats += 1
            return [build_frame(CMD_HEARTBEAT, bytes([state]))]
        if c == CMD_QUERY_PRODUCT:
            if self.raw_product:
                # TuyaOS 3.x wire form: raw 16-byte id + short version. A real
                # MCU paired with such a device knows this upfront, so the
                # caller sets raw_product for those dumps rather than probing.
                return [build_frame(CMD_QUERY_PRODUCT, self._raw_product())]
            # JSON product record - what OpenBeken and older Tuya SDKs (e.g.
            # 1.1.71, the TMWF02 fan switch) accept.
            js = '{"p":"%s","v":"%s"}' % (self.product_key, self.version)
            return [build_frame(CMD_QUERY_PRODUCT, js.encode("ascii"))]
        if c == CMD_MCU_CONF:
            # Empty working-mode response: the module drives its own Wi-Fi LED
            # and reset button (no MCU-side GPIOs handed over). working_mode
            # becomes valid, which is all the handshake needs here.
            return [build_frame(CMD_MCU_CONF, b"")]
        if c == CMD_QUERY_STATE:
            return [build_frame(CMD_STATE, encode_dp(dp, t, v))
                    for dp, (t, v) in sorted(self.dps.items())]
        # 0x03 / 0x2B Wi-Fi-state reports and anything else: nothing to answer.
        return []

    def react_stream(self, data):
        out = []
        for f in self.parser.feed(data):
            out.extend(self.react(f))
        return b"".join(out)


# ---------------------------------------------------------------------------
# Standalone self-tests. No firmware, no emulator: exercise the codec and the
# simulated MCU against the exact frames the firmware is known to send. Run
# with:  python src/tuyamcu.py
# ---------------------------------------------------------------------------
def _run_selftests():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, "FAIL: " + msg
        n += 1
        print("  [PASS]", msg)

    print("== TuyaMCU codec ==")
    # The real heartbeat the firmware emits, captured from an emulated dump.
    HB = bytes.fromhex("55aa000000 00ff".replace(" ", ""))
    check(build_frame(CMD_HEARTBEAT) == HB,
          "build heartbeat == captured 55 AA 00 00 00 00 FF")
    check(checksum(HB[:-1]) == HB[-1], "captured heartbeat checksum verifies")

    p = TuyaMCUParser()
    fr = p.feed(HB)
    check(len(fr) == 1 and fr[0].cmd == CMD_HEARTBEAT and fr[0].valid,
          "parse captured heartbeat -> one valid cmd 0x00 frame")

    # build/parse round-trip across several commands and payloads.
    for cmd, pl in [(CMD_HEARTBEAT, b""), (CMD_QUERY_PRODUCT, b""),
                    (CMD_MCU_CONF, b""), (CMD_STATE, b"\x01\x01\x00\x01\x01"),
                    (CMD_QUERY_STATE, b""), (CMD_SET_TIME, bytes(8))]:
        raw = build_frame(cmd, pl)
        got = TuyaMCUParser().feed(raw)
        check(len(got) == 1 and got[0].cmd == cmd and got[0].payload == pl
              and got[0].valid, "round-trip cmd 0x%02X, %d-byte payload" % (cmd, len(pl)))

    # a corrupted checksum must be reported invalid, not dropped.
    bad = bytearray(build_frame(CMD_QUERY_PRODUCT, b"x"))
    bad[-1] ^= 0xFF
    fr = TuyaMCUParser().feed(bytes(bad))
    check(len(fr) == 1 and not fr[0].valid, "bad checksum -> Frame.valid == False")

    # streaming: a frame split across two feeds is reassembled.
    p = TuyaMCUParser()
    whole = build_frame(CMD_QUERY_STATE, b"\xAA\xBB")
    check(p.feed(whole[:4]) == [] and len(p.feed(whole[4:])) == 1,
          "frame split across two feeds reassembles")

    # junk before the header is skipped; two frames in one buffer both parse.
    p = TuyaMCUParser()
    fr = p.feed(b"\x00\xffgarbage" + build_frame(CMD_HEARTBEAT)
                + build_frame(CMD_QUERY_PRODUCT))
    check([f.cmd for f in fr] == [CMD_HEARTBEAT, CMD_QUERY_PRODUCT],
          "junk skipped; two concatenated frames both parse")

    # data-point encoding.
    check(encode_dp(1, DP_BOOL, 1) == bytes.fromhex("0101000101"),
          "encode_dp bool -> dp1 type01 len0001 data01")
    check(encode_dp(2, DP_VALUE, 40) == bytes.fromhex("0202000400000028"),
          "encode_dp value 40 -> 4-byte big-endian 0x00000028")

    print("== simulated MCU reactions to the module's startup queries ==")
    mcu = TuyaMCUSlave(product_key="keyabcdefgh12345", version="1.0.0",
                       dps={1: (DP_BOOL, 1), 2: (DP_VALUE, 40)})

    def one(cmd, payload=b""):
        resp = mcu.react(Frame(0x00, cmd, payload))
        frames = TuyaMCUParser().feed(b"".join(resp))
        return resp, frames

    # heartbeat -> a valid 0x00 ACK; first payload 0x00 then 0x01.
    r, f = one(CMD_HEARTBEAT)
    check(len(f) == 1 and f[0].cmd == CMD_HEARTBEAT and f[0].valid
          and f[0].payload == b"\x00", "heartbeat -> valid ACK, first payload 0x00")
    r, f = one(CMD_HEARTBEAT)
    check(f[0].payload == b"\x01", "second heartbeat -> payload 0x01 (running)")

    # product query -> 0x01 with the JSON product record.
    r, f = one(CMD_QUERY_PRODUCT)
    check(len(f) == 1 and f[0].cmd == CMD_QUERY_PRODUCT and f[0].valid
          and b'"p":"keyabcdefgh12345"' in f[0].payload
          and b'"v":"1.0.0"' in f[0].payload,
          "query-product -> valid 0x01 JSON with product key and version")

    # a slave told upfront to use the raw form (TuyaOS 3.x) answers 0x01 with
    # the 16-byte id + short version, 16..24 bytes total, from the first query.
    rawmcu = TuyaMCUSlave(product_key="keyabcdefgh12345", raw_product=True)
    rf = TuyaMCUParser().feed(b"".join(rawmcu.react(Frame(0x00, CMD_QUERY_PRODUCT, b""))))
    check(len(rf) == 1 and rf[0].cmd == CMD_QUERY_PRODUCT and rf[0].valid
          and 16 <= len(rf[0].payload) <= 24
          and rf[0].payload[:16] == b"keyabcdefgh12345"
          and rf[0].payload[16:] == b"1.0.0",
          "raw_product slave -> raw 16-byte id + version (16..24 bytes)")

    # working-mode query -> 0x02 (empty payload here).
    r, f = one(CMD_MCU_CONF)
    check(len(f) == 1 and f[0].cmd == CMD_MCU_CONF and f[0].valid
          and f[0].payload == b"", "query-working-mode -> valid empty 0x02")

    # query-state -> one 0x07 report per configured DP, each a valid frame.
    r, f = one(CMD_QUERY_STATE)
    check(len(f) == 2 and all(x.cmd == CMD_STATE and x.valid for x in f),
          "query-state -> two valid 0x07 DP reports")
    check(f[0].payload == encode_dp(1, DP_BOOL, 1)
          and f[1].payload == encode_dp(2, DP_VALUE, 40),
          "0x07 reports carry the configured DP values, dp-id ordered")

    # a Wi-Fi-state report from the module needs no answer.
    check(mcu.react(Frame(0x00, CMD_WIFI_STATE, b"\x04")) == [],
          "module Wi-Fi-state report -> no response")
    # an invalid frame is ignored.
    check(mcu.react(Frame(0x00, CMD_HEARTBEAT, b"", valid=False)) == [],
          "invalid frame -> no response")

    # the full module startup sequence, driven through react_stream in order.
    mcu2 = TuyaMCUSlave()
    stream = (build_frame(CMD_HEARTBEAT) + build_frame(CMD_QUERY_PRODUCT)
              + build_frame(CMD_MCU_CONF) + build_frame(CMD_QUERY_STATE))
    out = mcu2.react_stream(stream)
    outf = TuyaMCUParser().feed(out)
    check([f.cmd for f in outf] == [CMD_HEARTBEAT, CMD_QUERY_PRODUCT, CMD_MCU_CONF]
          and all(f.valid for f in outf),
          "full startup sequence -> ACK, product, working-mode (no DPs configured)")
    check(mcu2.seen == [CMD_HEARTBEAT, CMD_QUERY_PRODUCT, CMD_MCU_CONF, CMD_QUERY_STATE],
          "slave recorded every module command it reacted to, in order")

    print("\nAll %d TuyaMCU self-tests passed." % n)
    return n


if __name__ == "__main__":
    _run_selftests()
