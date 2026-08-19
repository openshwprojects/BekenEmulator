"""Inject an OpenBeken config (with a startup command) into a BK7231 flash image.

OBK keeps its config in the BK_PARTITION_NET_PARAM partition at flash 0x1e1000,
as a 3584-byte mainConfig_t:

    offset 0..2   ident 'C','F','G'
    offset 3      crc      - Tiny_CRC8 over config[4:3584]
    offset 4      version  - int, MAIN_CFG_VERSION (5)
    offset 0x5E0  initCommandLine[1568] - the startup command

CRC note: OBK's Tiny_CRC8 (src/tiny_crc8.c) is written with plain `char`, and
this firmware is built with *signed* char, so `crc >>= 1` is an arithmetic
shift. Computing it as unsigned yields a different byte and the config is
rejected with "Config crc or ident mismatch", so the signed form is required.

Flash images carry a CRC stripe every 32 bytes (32 data + 2 big-endian CRC16).
Only the blocks actually touched are patched and re-stripped, so the rest of the
image stays byte-identical to the input. App-only images are shorter than the
config partition, so the image is first extended with erased (0xFF) blocks.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from src.crypto import block_crc_check, crc16

CONFIG_ADDR = 0x1E1000          # BK_PARTITION_NET_PARAM start
CONFIG_SIZE = 3584              # MAGIC_CONFIG_SIZE_V4 == sizeof(mainConfig_t)
CFG_VERSION = 5                 # MAIN_CFG_VERSION
CMDLINE_OFF = 0x5E0             # offsetof(mainConfig_t, initCommandLine)
CMDLINE_MAX = 1568              # sizeof(initCommandLine)
SECTOR_SIZE = 4096              # flash sector; images grow to a whole sector


def tiny_crc8_signed(data):
    """OBK's Tiny_CRC8 as compiled with signed char (arithmetic >>)."""
    def to_signed(x):
        x &= 0xFF
        return x - 256 if x >= 128 else x

    crc = 0
    for byte in data:
        extract = to_signed(byte)
        for _ in range(8):
            total = (crc ^ extract) & 0x01
            crc >>= 1
            if total:
                crc = to_signed(crc ^ 0x8C)
            extract >>= 1
        crc = to_signed(crc)
    return crc & 0xFF


def build_config(startup_command):
    """Build a 3584-byte mainConfig_t carrying the given startup command."""
    cmd = startup_command.encode("ascii")
    if len(cmd) >= CMDLINE_MAX:
        raise ValueError("startup command too long (max %d bytes)" % (CMDLINE_MAX - 1))

    cfg = bytearray(CONFIG_SIZE)
    cfg[0:3] = b"CFG"
    cfg[4:8] = struct.pack("<i", CFG_VERSION)
    cfg[CMDLINE_OFF:CMDLINE_OFF + len(cmd)] = cmd
    cfg[3] = tiny_crc8_signed(cfg[4:CONFIG_SIZE])
    return bytes(cfg)


def detect_stripe_offset(raw):
    """Find where the 34-byte CRC stripe grid starts (0 or 2), or None if flat."""
    if len(raw) < 36:
        return None
    if block_crc_check(raw[:32], raw[32:34]):
        return 0
    if block_crc_check(raw[2:34], raw[34:36]):
        return 2
    return None


def extend_striped(raw, offset, needed_stripped):
    """Grow a striped image until stripped space covers needed_stripped bytes.

    Added blocks are erased flash (0xFF) with a correct CRC16, and the result is
    rounded up to a whole sector of stripped space.
    """
    have_blocks = (len(raw) - offset) // 34
    if have_blocks * 32 >= needed_stripped:
        return raw          # a full dump already covers the config partition

    want = -(-needed_stripped // SECTOR_SIZE) * SECTOR_SIZE
    want_blocks = -(-want // 32)

    out = bytearray(raw[:offset + have_blocks * 34])

    # An app-only image can end mid-block; keep that data and round it out to a
    # whole block, or the last bytes of the app would be lost.
    leftover = raw[offset + have_blocks * 34:][:32]
    if leftover:
        block = leftover.ljust(32, b"\xff")
        out += block + struct.pack(">H", crc16(block, initial_value=0xFFFF))
        have_blocks += 1

    blank = b"\xff" * 32
    tail = struct.pack(">H", crc16(blank, initial_value=0xFFFF))
    out += (blank + tail) * (want_blocks - have_blocks)
    return bytes(out)


def patch_striped(raw, stripped_addr, payload, offset):
    """Write payload at a *stripped-space* address, fixing each block's CRC16."""
    raw = extend_striped(raw, offset, stripped_addr + len(payload))
    out = bytearray(raw)
    for i, value in enumerate(payload):
        block, within = divmod(stripped_addr + i, 32)
        raw_index = offset + block * 34 + within
        if raw_index >= len(out):
            raise ValueError("config extends past the end of the image")
        out[raw_index] = value

    first_block = stripped_addr // 32
    last_block = (stripped_addr + len(payload) - 1) // 32
    for block in range(first_block, last_block + 1):
        start = offset + block * 34
        crc = crc16(bytes(out[start:start + 32]), initial_value=0xFFFF)
        out[start + 32:start + 34] = struct.pack(">H", crc)
    return bytes(out)


def inject(raw, startup_command):
    """Return a copy of raw carrying a valid config with the given command."""
    cfg = build_config(startup_command)
    offset = detect_stripe_offset(raw)
    if offset is None:
        out = bytearray(raw)
        if len(out) < CONFIG_ADDR + CONFIG_SIZE:
            out += b"\xff" * (CONFIG_ADDR + CONFIG_SIZE - len(out))
        out[CONFIG_ADDR:CONFIG_ADDR + CONFIG_SIZE] = cfg
        return bytes(out), offset, cfg
    return patch_striped(raw, CONFIG_ADDR, cfg, offset), offset, cfg


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", help="Base BK7231 flash image")
    parser.add_argument("output", help="Image to write with the config injected")
    parser.add_argument("-c", "--command", required=True,
                        help="Startup command placed in initCommandLine, e.g. "
                             "\"uartInit 9600; uartSendHex 55AA00000000FF\"")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        raw = f.read()

    patched, offset, cfg = inject(raw, args.command)

    with open(args.output, "wb") as f:
        f.write(patched)

    print("Wrote %s (%d bytes, was %d)" % (args.output, len(patched), len(raw)))
    print("  stripe offset : %s" % ("none (flat image)" if offset is None else offset))
    print("  config crc    : 0x%02x" % cfg[3])
    print("  startup cmd   : %s" % args.command)


if __name__ == "__main__":
    main()
