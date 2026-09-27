"""RivieraWaves BLE 5.x core register model (the block Beken maps at 0x900000).

Why this exists: on a stock BLE+Wi-Fi image the NimBLE *host* hands HCI commands
to the RivieraWaves *controller* that runs on the same CPU, and the controller
only ever gets to run - `rwip_schedule()` - after one of its two FIQs fires
(`ble_main.c`: the BLE and BTDM service routines are the only callers of
`ble_send_msg(BLE_MSG_POLL)`). Those FIQs come from this register block. With
the block served as plain memory the controller arms deep sleep, samples a slot
clock that never moves, and waits for a wake-up interrupt that never comes; the
host times out (`ble_hs_hci_wait_for_ack rc = 19`) and loops on assert/reset.

What is modelled, all derived from beken378/driver/ble/ble_5_2/.../ble_reg_blecore.h
and from register traces of a real BK7238 image (scratch: ipcore900_probe.py):

  RWBLECNTL  +0x000  soft-reset bits self-clear; SWINT_REQ raises the SW interrupt
  VERSION    +0x004  reads as the IP's reset value
  INTCNTL0/INTSTAT0/INTACK0  +0x00c/+0x010/+0x014  BLE EVENT interrupts  (FIQ_BLE, rwble_isr)
  INTCNTL1/INTSTAT1/INTACK1  +0x018/+0x01c/+0x020  CORE interrupts       (FIQ_BTDM, rwip_isr)

  Set 1 is the core side, set 0 the event side - the reverse of the older
  4.2/5.1 ipcore headers. Confirmed on the BK7238 RGB image two ways: the
  BTDM FIQ handler reads +0x1c, and the mask it programs into INTCNTL1
  (0x808e = SLP|CRYPT|SW|TIMESTAMPTGT3|FIFO) is exactly the core set, while
  INTCNTL0 (0x1001e) carries the end/skip-event and RX bits.
  DEEPSLCNTL/DEEPSLWKUP/DEEPSLSTAT  +0x030/+0x034/+0x038  deep sleep + wake-up
  FINETIMTGT +0x0e4, CLKNTGTn/HMICROSECTGTn +0x0e8..+0x0fc  timer targets
  SLOTCLK    +0x100  625 us slot counter, latched on SAMP;  FINETIMECNT +0x104

Everything else in the block is left as plain memory: the firmware reads back
what it wrote, which is what the radio-config registers do on silicon too.

Two generations of the block are modelled, picked per chip (see LAYOUTS):

  5.2  BK7238. The map above.
  5.1  BK7231N/M (and BL2028N, which is N silicon). Same registers and the
       same interrupt bits in both sets, but the IP has one timestamp target
       fewer, so everything from +0xF8 moves up a word: SLOTCLK is at +0xF8
       (where 5.2 has CLKNTGT3) and FINETIMECNT at +0xFC. Its exchange memory
       sits at a fixed 0x910000 (EM_BASE_ADDR in ble_5_x_rw/.../em_map.h), so
       there is no base register to read. From ble_5_1/.../ble_reg_ipcore.h,
       identical in the OpenBK7231N SDK. A stock image with this layout polls
       SLOTCLK's SAMP bit right after "rwble_hl_init ok" and, served as plain
       memory, never gets past it.

BK7231T/U and BK7252N carry the older BLE 4.2 IP, which is not modelled.

Nothing here models the radio. No packets are ever sent or received, so the
BLE-side event interrupts (start/end of event, RX) are never raised. That is
enough for the controller to keep time, wake up, run its scheduler and answer
HCI commands - not enough to actually advertise on air.
"""

# Layout of the 5.2 blecore block, as offsets from its base.
RWBLECNTL, VERSION = 0x000, 0x004
INTCNTL0, INTSTAT0, INTACK0 = 0x00C, 0x010, 0x014
INTCNTL1, INTSTAT1, INTACK1 = 0x018, 0x01C, 0x020
DEEPSLCNTL, DEEPSLWKUP, DEEPSLSTAT = 0x030, 0x034, 0x038
FINECNTCORR, CLKNCNTCORR = 0x040, 0x044
# AES-128 engine: key in four words (AESKEY31_0 first), AESPTR = exchange-memory
# offset of the 16-byte input; the result is written 16 bytes after it
# (em_map.h: EM_ENC_OUT_OFFSET = EM_ENC_IN_OFFSET + 16). Same offsets on 5.1/5.2.
AESCNTL = 0x0B0
AESKEY = (0x0B4, 0x0B8, 0x0BC, 0x0C0)
AESPTR = 0x0C4
AES_START = 1 << 0
FINETIMTGT = 0x0E4
CLKNTGT = (0x0E8, 0x0F0, 0x0F8)          # CLKNTGT1..3
HMICROSECTGT = (0x0EC, 0x0F4, 0x0FC)     # HMICROSECTGT1..3
SLOTCLK, FINETIMECNT = 0x100, 0x104
# Beken addition, not in the RivieraWaves map: the absolute base of the
# exchange memory. The RW stack reaches EM through a runtime pointer
# (`extern uint8_t *ex_mem`), so its address moves with every build - but the
# hardware needs the absolute address, and the firmware writes it here. Reading
# it back is therefore a build-independent way to find EM, where a hardcoded
# address or a scan of all RAM would not be.
BKRWEXMEM = 0x19C
EM_SCAN_BYTES = 0x2000            # EM is small; this covers the descriptor area
SIZE = 0x200                              # window the model claims

VERSION_RESET = 0x0B001100               # BLE_VERSION_RESET, 5.2 blecore

# Per-generation differences. Everything not listed here is common to both.
#   window       bytes of the block the model is offered (reads it returns
#                None for stay plain memory)
#   ble_version  (offset, value) of a second version register the link layer
#                checks, or None
LAYOUTS = {
    "5.2": {"slotclk": SLOTCLK, "finetimecnt": FINETIMECNT, "targets": 3,
            "version": VERSION_RESET, "em_fixed": None,
            "window": SIZE, "ble_version": None},
    "5.1": {"slotclk": 0x0F8, "finetimecnt": 0x0FC, "targets": 2,
            "version": 0x0A000700,          # IP_VERSION_RESET, 5.1 ipcore
            "em_fixed": 0x00910000,         # REG_EM_ET_BASE_ADDR
            # On 5.1 the BLE-specific registers are a separate block at
            # +0x800 (ble_reg_blecore.h: BLE_RWBLECNTL_ADDR 0x00900800), and
            # lld_init asserts its version before anything else - lld.c:404,
            # "param0 = 0, param1 = 167776000" with it unmodelled. The link
            # layer then runs uninitialised and the host never gets its
            # random numbers (gapm "wait GAPM_GEN_RAND_NB").
            "window": 0x1000, "ble_version": (0x804, 0x0A000F00)},
}

# RWBLECNTL bits
MASTER_SOFT_RST = 1 << 31
REG_SOFT_RST = 1 << 29
RADIOCNTL_SOFT_RST = 1 << 28
SWINT_REQ = 1 << 27
SELF_CLEARING = MASTER_SOFT_RST | REG_SOFT_RST | RADIOCNTL_SOFT_RST | SWINT_REQ

# Core-side interrupt bits: INTSTAT1 / INTCNTL1 / INTACK1 (+0x18/+0x1c/+0x20)
CLKNINT, SLPINT, CRYPTINT, SWINT, FINETGTINT = 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4
TIMESTAMPTGTINT = (1 << 5, 1 << 6, 1 << 7)

# DEEPSLCNTL bits
DEEP_SLEEP_ON = 1 << 2
DEEP_SLEEP_CORR_EN = 1 << 3     # apply CLKNCNTCORR/FINECNTCORR to the clock
DEEP_SLEEP_STAT = 1 << 15

# SLOTCLK bits
SAMP = 1 << 31
CLKN_UPD = 1 << 30
SLOT_MASK = 0x0FFFFFFF
FINE_MASK = 0x3FF

# A payload the controller is advertising always starts with the Flags AD
# structure, and LE General/Limited Discoverable is what a pairable Tuya device
# sets. Used only to locate the payload INSIDE the register-identified EM
# window, never to search memory at large.
AD_FLAGS_PREFIXES = tuple(bytes.fromhex(h) for h in ("020104", "020105", "020106"))
AD_TYPE_NAMES = {
    0x01: "Flags", 0x02: "16-bit UUIDs (incomplete)", 0x03: "16-bit UUIDs",
    0x08: "Short Name", 0x09: "Complete Name", 0x0A: "TX Power",
    0x16: "Service Data", 0xFF: "Manufacturer Data",
}

SLOT_US = 625
LP_CLOCK_HZ = 32768                      # the sleep clock DEEPSLWKUP counts in


class BleCore:
    """One BLE core. Drive it with write()/read()/tick(); poll pending_*."""

    def __init__(self, base, insns_per_second, max_sleep_us=10_000, layout="5.2"):
        self.base = base
        self.layout = layout
        lay = LAYOUTS[layout]
        self.slotclk = lay["slotclk"]
        self.finetimecnt = lay["finetimecnt"]
        self.version = lay["version"]
        self.em_fixed = lay["em_fixed"]
        self.window = lay["window"]
        self.ble_version = lay["ble_version"]
        # Timestamp targets this IP has; on 5.1 the third pair's offsets are
        # SLOTCLK/FINETIMECNT, so they must not be treated as targets there.
        self.clkntgt = CLKNTGT[:lay["targets"]]
        self.hmicrosectgt = HMICROSECTGT[:lay["targets"]]
        # Deep sleep is cut short at this much device time. On silicon a
        # sleeping controller is woken early by external events (an HCI
        # command from the host, an AON timer); the RW stack expects that and
        # re-derives time from DEEPSLSTAT, so an early wake is legitimate. A
        # 9.7 s wake-up timer against a 2 s HCI timeout is otherwise a dead
        # end - measured on the BK7238 RGB image.
        self.max_sleep_insns = max(1, insns_per_second * max_sleep_us // 1_000_000)
        # Both clocks are derived from the emulator's instruction count, the
        # same fiction every other device-time source here is paced from.
        self.insns_per_slot = max(1, insns_per_second * SLOT_US // 1_000_000)
        self.insns_per_lp_cycle = max(1, insns_per_second // LP_CLOCK_HZ)
        self.regs = {}                    # offset -> shadow value
        self.raw_evt = 0                  # unmasked INTSTAT0: BLE event side (never raised: no radio)
        self.raw_core = 0                 # unmasked INTSTAT1: core side (sleep, SW, timers)
        self.slot_latch = 0               # SLOTCLK value captured by the last SAMP
        self.slot_offset = 0              # correction applied by DEEP_SLEEP_CORR_EN
        self.last_clkn_slot = None        # last slot a CLKNINT was raised for
        self.sleeping = False
        self.wake_at = None               # insn count at which SLPINT fires
        self.slept_from = None
        self.fired_tgt = {}               # target offset -> value already fired for
        self.events = []                  # ("sleep", insns) ... for probes/tests
        # (read(addr, n) -> bytes, write(addr, data)) over guest memory, set by
        # the emulator; the AES engine needs it to reach exchange memory.
        self.mem = None

    # ---------------------------------------------------------------- clocks
    def slot(self, insns):
        return (insns // self.insns_per_slot + self.slot_offset) & SLOT_MASK

    def fine(self, insns):
        return (insns % self.insns_per_slot) * SLOT_US // self.insns_per_slot

    # -------------------------------------------------------------- register
    def read(self, address, insns):
        """Value to serve for a read inside the block, or None for plain memory."""
        off = address - self.base
        if off == VERSION:
            return self.version
        if self.ble_version is not None and off == self.ble_version[0]:
            return self.ble_version[1]
        if off == INTSTAT0:
            return self.raw_evt & self.regs.get(INTCNTL0, 0)
        if off == INTSTAT1:
            return self.raw_core & self.regs.get(INTCNTL1, 0)
        if off == DEEPSLCNTL:
            v = self.regs.get(DEEPSLCNTL, 0)
            return v | DEEP_SLEEP_STAT if self.sleeping else v & ~DEEP_SLEEP_STAT
        if off == self.slotclk:
            # SAMP and CLKN_UPD are commands; they always read back clear.
            return self.slot_latch & SLOT_MASK
        if off == self.finetimecnt:
            return self.fine(insns) & FINE_MASK
        if off in (RWBLECNTL, DEEPSLSTAT):
            return self.regs.get(off, 0)
        if off == AESCNTL:
            # AES_START self-clears: the block is encrypted by the time the
            # firmware can look (see _aes_run).
            return self.regs.get(AESCNTL, 0) & ~AES_START
        return None

    def write(self, address, value, insns):
        """Apply a write inside the block. The caller still lets it land in memory."""
        off = address - self.base
        value &= 0xFFFFFFFF
        if off == RWBLECNTL:
            if value & SWINT_REQ:
                self.raw_core |= SWINT
            # Reset requests complete instantly; the firmware polls for them
            # to clear before it goes on.
            self.regs[off] = value & ~SELF_CLEARING
            return
        if off == INTACK0:
            self.raw_evt &= ~value
            return
        if off == INTACK1:
            self.raw_core &= ~value
            return
        if off == DEEPSLCNTL:
            self.regs[off] = value
            if value & DEEP_SLEEP_CORR_EN and CLKNCNTCORR in self.regs:
                # After a sleep the firmware works out where the clock must
                # now be (from DEEPSLSTAT) and hands it to hardware; from here
                # on that value is the truth, so re-base ours onto it.
                want = self.regs[CLKNCNTCORR] & SLOT_MASK
                self.slot_offset = (want - insns // self.insns_per_slot) & SLOT_MASK
            if self.sleeping and not (value & DEEP_SLEEP_ON):
                self.wake_at = insns          # soft wake-up: fires on the next tick
            if value & DEEP_SLEEP_ON and not self.sleeping:
                cycles = self.regs.get(DEEPSLWKUP, 0) & 0xFFFFFFFF
                self.sleeping = True
                self.slept_from = insns
                self.wake_at = insns + min(max(1, cycles) * self.insns_per_lp_cycle,
                                           self.max_sleep_insns)
                self.events.append(("sleep", insns, cycles))
            return
        if off == self.slotclk:
            if value & SAMP:
                self.slot_latch = self.slot(insns)
            return
        if off == AESCNTL:
            self.regs[off] = value & ~AES_START
            if value & AES_START:
                self._aes_run()
            return
        if off in (FINETIMTGT,) + self.clkntgt:
            # A new target may fire again even if the previous value did.
            self.fired_tgt.pop(off, None)
        self.regs[off] = value

    # ------------------------------------------------------------ AES engine
    def _aes_run(self):
        """Encrypt the 16 bytes at EM+AESPTR into EM+AESPTR+16, raise CRYPTINT.

        The controller uses this for everything from HCI LE_Rand/LE_Encrypt to
        resolvable-address generation; the host's GAPM_GEN_RAND_NB waits on it
        during start-up, so with no engine BLE init never completes ("wait
        GAPM_GEN_RAND_NB" -> "create cmd db fail").

        Byte order is BLE's: key, input and output are all LSB first, the key
        loaded from key[0..3] into AESKEY31_0 as a little-endian word. AES
        itself is MSB first, so each side is reversed around a plain AES-128
        ECB - checked against the spec's ah() vector in the self-tests.
        """
        if self.mem is None:
            return
        base = self.em_base()
        if not base:
            return
        from Crypto.Cipher import AES      # pycryptodome, see requirements.txt
        key_le = b"".join((self.regs.get(k, 0) & 0xFFFFFFFF).to_bytes(4, "little")
                          for k in AESKEY)
        ptr = base + (self.regs.get(AESPTR, 0) & 0xFFFF)
        read_mem, write_mem = self.mem
        try:
            plain_le = bytes(read_mem(ptr, 16))
            out = AES.new(key_le[::-1], AES.MODE_ECB).encrypt(plain_le[::-1])
            write_mem(ptr + 16, out[::-1])
        except Exception:
            return
        self.raw_core |= CRYPTINT
        self.events.append(("aes", ptr))

    # --------------------------------------------------- advertising payload
    def em_base(self):
        """Absolute exchange-memory base: fixed on 5.1, handed over by the firmware on 5.2."""
        if self.em_fixed is not None:
            return self.em_fixed
        return self.regs.get(BKRWEXMEM, 0)

    @staticmethod
    def parse_ad(blob):
        """Split a BLE AD payload into [(type, value)], or [] if malformed.

        Length-type-value, walked to the first zero length. A chain that does
        not parse cleanly is rejected, which is what keeps a chance byte match
        from being reported as an advertisement.
        """
        fields, p = [], 0
        while p < len(blob) and blob[p]:
            length = blob[p]
            if p + 1 + length > len(blob):
                return []
            fields.append((blob[p + 1], bytes(blob[p + 2:p + 1 + length])))
            p += 1 + length
        return fields if len(fields) >= 2 else []

    def find_adv(self, read_mem):
        """Advertising payloads staged in EM: [(em_offset, raw, fields)].

        `read_mem(addr, size) -> bytes`. The EM base comes from the register
        above, so nothing here depends on where a particular build put its
        buffers.
        """
        base = self.em_base()
        if not base:
            return []
        # Page by page, stopping at the first page the firmware never touched:
        # EM is ordinary RAM mapped on first use, and a smaller controller
        # (the 5.1 core uses under 4K of it) leaves the rest unmapped, which
        # would fail a single read of the whole window.
        blob = b""
        for off in range(0, EM_SCAN_BYTES, 0x1000):
            try:
                blob += bytes(read_mem(base + off, min(0x1000, EM_SCAN_BYTES - off)))
            except Exception:
                break
        if not blob:
            return []
        out, seen = [], set()
        for prefix in AD_FLAGS_PREFIXES:
            start = 0
            while True:
                i = blob.find(prefix, start)
                if i < 0:
                    break
                start = i + 1
                fields = self.parse_ad(blob[i:i + 62])
                if not fields:
                    continue
                used = sum(2 + len(v) for _, v in fields)
                raw = bytes(blob[i:i + used])
                if raw in seen:
                    continue
                seen.add(raw)
                out.append((i, raw, fields))
        return out

    @staticmethod
    def describe_ad(fields):
        """One readable line per AD structure."""
        lines = []
        for t, v in fields:
            name = AD_TYPE_NAMES.get(t, "type 0x%02X" % t)
            note = ""
            if t in (0x02, 0x03, 0x16) and len(v) >= 2:
                note = "  uuid=0x%04X" % int.from_bytes(v[:2], "little")
            elif t == 0xFF and len(v) >= 2:
                # Bluetooth SIG company identifier, little endian.
                note = "  company=0x%04X" % int.from_bytes(v[:2], "little")
            text = "".join(chr(c) if 32 <= c < 127 else "." for c in v)
            # Whole bytes only; the full payload is on the line above anyway.
            shown = v[:12].hex(" ") + (" .." if len(v) > 12 else "")
            lines.append("%-26s %-39s %s%s" % (name, shown, text, note))
        return lines

    # ------------------------------------------------------------------ time
    def tick(self, insns):
        """Advance the model; returns (core_irq, ble_irq) level flags."""
        if self.sleeping and insns >= self.wake_at:
            self.sleeping = False
            slept = (insns - self.slept_from) // self.insns_per_lp_cycle
            self.regs[DEEPSLSTAT] = slept & 0xFFFFFFFF
            self.regs[DEEPSLCNTL] = self.regs.get(DEEPSLCNTL, 0) & ~DEEP_SLEEP_ON
            self.raw_core |= SLPINT
            self.events.append(("wake", insns, slept))

        mask0 = self.regs.get(INTCNTL1, 0)          # core-side mask lives in set 1
        now = self.slot(insns)
        # Slot-clock interrupt: the firmware unmasks it right after a wake-up
        # and finishes waking (rwip_wakeup_end) on the first tick, then masks
        # it again. Raised once per new slot while unmasked.
        if mask0 & CLKNINT and self.last_clkn_slot != now:
            self.last_clkn_slot = now
            self.raw_core |= CLKNINT
        # Fine-target: a 28-bit slot target on its own.
        if mask0 & FINETGTINT and FINETIMTGT in self.regs:
            tgt = self.regs[FINETIMTGT] & SLOT_MASK
            if self.fired_tgt.get(FINETIMTGT) != tgt and self._reached(now, tgt):
                self.fired_tgt[FINETIMTGT] = tgt
                self.raw_core |= FINETGTINT
        # Timestamp targets 1..3: slot target plus a fine (half-microsecond) part.
        for i, tgt_off in enumerate(self.clkntgt):
            bit = TIMESTAMPTGTINT[i]
            if not (mask0 & bit) or tgt_off not in self.regs:
                continue
            tgt = self.regs[tgt_off] & SLOT_MASK
            if self.fired_tgt.get(tgt_off) == tgt or not self._reached(now, tgt):
                continue
            if now == tgt and self.fine(insns) < (self.regs.get(self.hmicrosectgt[i], 0) & FINE_MASK):
                continue
            self.fired_tgt[tgt_off] = tgt
            self.raw_core |= bit

        # (core line -> FIQ_BTDM, event line -> FIQ_BLE)
        return (bool(self.raw_core & mask0), bool(self.raw_evt & self.regs.get(INTCNTL0, 0)))

    @staticmethod
    def _reached(now, target):
        """True once the 28-bit slot counter has passed target (wrap-aware)."""
        return ((now - target) & SLOT_MASK) < (SLOT_MASK >> 1)


# ---------------------------------------------------------------------------
# Standalone self-tests, no emulator: drive the model with the exact register
# writes the BK7238 RGB image was traced making. Run:  python src/blecore.py
# ---------------------------------------------------------------------------
def _run_selftests():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, "FAIL: " + msg
        n += 1
        print("  [PASS]", msg)

    b = 0x900000
    c = BleCore(b, 5_000_000)             # 5M insns per device second

    print("== reset, software interrupt, ack ==")
    c.write(b + RWBLECNTL, MASTER_SOFT_RST | 0x100607, 0)
    check(c.read(b + RWBLECNTL, 0) == 0x100607, "MASTER_SOFT_RST self-clears, config bits kept")
    c.write(b + INTCNTL1, 0x1001e, 0)      # the mask the firmware actually programs
    c.write(b + RWBLECNTL, SWINT_REQ, 0)
    check(c.tick(0)[0] and c.read(b + INTSTAT1, 0) & SWINT, "SWINT_REQ -> INTSTAT1.SWINT, core line up")
    c.write(b + INTACK1, SWINT, 0)
    check(not c.tick(0)[0], "INTACK1 clears it, line drops")
    check(c.read(b + VERSION, 0) == VERSION_RESET, "VERSION reads the 5.2 reset value")

    print("== slot clock ==")
    c.write(b + SLOTCLK, SAMP, 3125 * 10)
    check(c.read(b + SLOTCLK, 3125 * 10) == 10, "SAMP latches slot 10 at 31250 insns (625 us slots)")
    check(c.read(b + SLOTCLK, 3125 * 99) == 10, "SLOTCLK holds the latched value until the next SAMP")
    check(c.read(b + FINETIMECNT, 3125 * 10 + 1562) == 312, "FINETIMECNT mid-slot reads ~312 us")

    print("== deep sleep and wake-up ==")
    c.write(b + DEEPSLWKUP, 0x4e1ff, 0)   # 9.7 s, as programmed by the firmware
    c.write(b + DEEPSLCNTL, DEEP_SLEEP_ON, 1_000_000)
    check(c.read(b + DEEPSLCNTL, 1_000_000) & DEEP_SLEEP_STAT, "DEEP_SLEEP_STAT set while asleep")
    check(c.wake_at == 1_000_000 + c.max_sleep_insns, "sleep capped to 10 ms device time despite the 9.7 s timer")
    check(not c.tick(1_049_000)[0], "still asleep just before the cap")
    check(c.tick(1_050_000)[0] and c.raw_core & SLPINT, "SLPINT fires at the cap, core line up")
    check(not (c.read(b + DEEPSLCNTL, 1_050_000) & (DEEP_SLEEP_ON | DEEP_SLEEP_STAT)), "ON and STAT cleared on wake")
    check(c.read(b + DEEPSLSTAT, 0) == 50_000 // c.insns_per_lp_cycle, "DEEPSLSTAT reports the LP cycles actually slept")
    c.write(b + INTACK1, 0xFFFFFFFF, 1_050_000)
    c.write(b + DEEPSLCNTL, DEEP_SLEEP_ON, 2_000_000)
    c.write(b + DEEPSLCNTL, 0, 2_010_000)
    check(c.tick(2_010_000)[0] and c.raw_core & SLPINT, "clearing DEEP_SLEEP_ON while asleep wakes immediately")
    c.write(b + INTACK1, 0xFFFFFFFF, 2_010_000)

    print("== timer targets ==")
    now = 3_000_000
    c.write(b + FINETIMTGT, c.slot(now) + 4, now)
    check(not c.tick(now)[0], "FINETGT not yet")
    check(c.tick(now + 4 * 3125)[0] and c.raw_core & FINETGTINT, "FINETGT fires when the slot reaches the target")
    c.write(b + INTACK1, FINETGTINT, now)
    check(not c.tick(now + 5 * 3125)[0], "FINETGT does not refire for the same target")
    c.write(b + INTCNTL1, 0x1001e | TIMESTAMPTGTINT[0], now)
    c.write(b + CLKNTGT[0], c.slot(now) + 6, now); c.write(b + HMICROSECTGT[0], 600, now)
    check(not c.tick(now + 6 * 3125 + 100)[0], "TIMESTAMPTGT1 waits for the fine part of the target")
    check(c.tick(now + 6 * 3125 + 3100)[0] and c.raw_core & TIMESTAMPTGTINT[0], "TIMESTAMPTGT1 fires once slot and fine part are reached")

    print("== wake-up completion: clock correction + slot-clock interrupt ==")
    c.write(b + INTACK1, 0xFFFFFFFF, now)
    c.write(b + CLKNCNTCORR, 0x80000000 | 5000, now); c.write(b + DEEPSLCNTL, DEEP_SLEEP_CORR_EN, now)
    check(c.slot(now) == 5000, "DEEP_SLEEP_CORR_EN re-bases the slot clock onto CLKNCNTCORR")
    c.write(b + SLOTCLK, SAMP, now + 3125 * 7)
    check(c.read(b + SLOTCLK, 0) == 5007, "and the corrected clock keeps advancing from there")
    c.write(b + INTCNTL1, 0x808e | CLKNINT, now)
    check(c.tick(now)[0] and c.raw_core & CLKNINT, "unmasking CLKNINT raises it on the current slot")
    c.write(b + INTACK1, CLKNINT, now)
    check(not c.tick(now + 100)[0], "no second CLKNINT inside the same slot")
    check(c.tick(now + 3125)[0] and c.raw_core & CLKNINT, "next slot raises it again while unmasked")
    c.write(b + INTACK1, CLKNINT, now); c.write(b + INTCNTL1, 0x808e, now)
    check(not c.tick(now + 2 * 3125)[0], "masked again: silent")

    print("== advertising payload, located through the EM base register ==")
    ADV = bytes.fromhex("020106" "030250fd" "1716" "50fd430400106b6579773937716a736379666d73796e")
    c3 = BleCore(b, 5_000_000)
    check(c3.em_base() == 0 and c3.find_adv(lambda a, n: b"") == [],
          "no EM base yet -> nothing reported, no guessing")
    EM = 0x00424480
    c3.write(b + BKRWEXMEM, EM, 0)
    check(c3.em_base() == EM, "EM base is read back from BKRWEXMEM")
    mem = bytearray(EM_SCAN_BYTES)
    mem[0xBB8:0xBB8 + len(ADV)] = ADV
    def rd(addr, n, _base=EM, _m=mem):
        assert _base <= addr < _base + EM_SCAN_BYTES, "must read from the register-provided base"
        return bytes(_m[addr - _base:addr - _base + n])
    got = c3.find_adv(rd)
    check(len(got) == 1 and got[0][0] == 0xBB8 and got[0][1] == ADV,
          "payload found at its EM offset, read from the register base")
    def rd_one_page(addr, n, _base=EM, _m=mem):
        if addr >= _base + 0x1000:
            raise RuntimeError("unmapped")      # what Unicorn does past the touched page
        return bytes(_m[addr - _base:addr - _base + n])
    got1 = c3.find_adv(rd_one_page)
    check(len(got1) == 1 and got1[0][0] == 0xBB8,
          "an untouched (unmapped) second EM page does not hide the first")
    types = [t for t, _ in got[0][2]]
    check(types == [0x01, 0x02, 0x16], "AD chain decodes to Flags + UUID + Service Data")
    svc = dict(got[0][2])[0x16]
    check(svc[:2] == bytes.fromhex("50fd") and b"keyw97qjscyfmsyn" in svc,
          "service data carries UUID 0xFD50 and the firmware key as ASCII")
    # a build with EM somewhere else is found just the same
    c4 = BleCore(b, 5_000_000); c4.write(b + BKRWEXMEM, 0x00431000, 0)
    m2 = bytearray(EM_SCAN_BYTES); m2[0x40:0x40 + len(ADV)] = ADV
    got2 = c4.find_adv(lambda a, n: bytes(m2[a - 0x00431000:a - 0x00431000 + n]))
    check(len(got2) == 1 and got2[0][0] == 0x40,
          "a different EM base and offset is found with no code change")
    check(BleCore.parse_ad(bytes.fromhex("0201064009") + b"zzz") == [],
          "a length that overruns the buffer is rejected, not reported")
    check(BleCore.parse_ad(bytes.fromhex("020106")) == [],
          "a lone Flags structure is not enough to call it an advertisement")

    print("== masking ==")
    c2 = BleCore(b, 5_000_000)
    c2.write(b + RWBLECNTL, SWINT_REQ, 0)
    check(not c2.tick(0)[0] and c2.read(b + INTSTAT1, 0) == 0, "a masked-out interrupt neither shows in INTSTAT1 nor raises the line")
    check(c2.tick(0)[1] is False, "the BLE EVENT line (set 0) never rises: no radio events are modelled")

    print("== 5.1 layout (BK7231N/M) ==")
    c5 = BleCore(b, 5_000_000, layout="5.1")
    check(c5.read(b + VERSION, 0) == 0x0A000700, "ipcore VERSION reads the 5.1 reset value")
    check(c5.read(b + 0x804, 0) == 0x0A000F00,
          "BLE-block VERSION at +0x804 reads 0x0A000F00 (lld.c:404 asserts it)")
    check(c5.window == 0x1000 and BleCore(b, 5_000_000).window == SIZE,
          "5.1 claims the whole page (BLE block at +0x800); 5.2 keeps its window")
    c5.write(b + 0x0F8, SAMP, 3 * 3125)
    check(c5.read(b + 0x0F8, 3 * 3125) == 3, "SLOTCLK lives at +0xF8: SAMP latches the slot there")
    check(c5.read(b + 0x100, 0) is None, "+0x100 is not SLOTCLK on 5.1 (plain memory)")
    check(c5.read(b + 0x0FC, 3 * 3125 + 1562) == c5.fine(3 * 3125 + 1562),
          "FINETIMECNT lives at +0xFC")
    c5.write(b + INTCNTL1, TIMESTAMPTGTINT[2], 0)
    c5.write(b + 0x0F8, 5, 0)                 # a SLOTCLK write, not a CLKNTGT3 target
    check(not c5.tick(10 * 3125)[0], "the 5.2 third timestamp target does not exist on 5.1")
    check(c5.em_base() == 0x00910000, "5.1 exchange memory is at its fixed 0x910000")

    print("== AES engine (BLE byte order, spec ah() vector) ==")
    # Bluetooth Core Vol 3 Part H D.7: ah(IRK, prand) = e(IRK, 0..0||prand) mod 2^24.
    irk_msb = bytes.fromhex("ec0234a357c8ad05341010a60a397d9b")
    plain_msb = bytes(13) + bytes.fromhex("708194")
    em = bytearray(0x100)
    ptr = 0x4C
    em[ptr:ptr + 16] = plain_msb[::-1]        # the firmware stages it LSB first
    ca = BleCore(b, 5_000_000, layout="5.1")
    ca.mem = (lambda a, n: bytes(em[a - 0x910000:a - 0x910000 + n]),
              lambda a, d: em.__setitem__(slice(a - 0x910000, a - 0x910000 + len(d)), d))
    key_le = irk_msb[::-1]
    for i, off in enumerate(AESKEY):
        ca.write(b + off, int.from_bytes(key_le[4 * i:4 * i + 4], "little"), 0)
    ca.write(b + AESPTR, ptr, 0)
    ca.write(b + INTCNTL1, CRYPTINT, 0)
    ca.write(b + AESCNTL, AES_START, 0)
    check(bytes(em[ptr + 16:ptr + 19]) == bytes.fromhex("aafb0d"),
          "result lands 16 bytes after the input, LSB first: ah = 0x0dfbaa")
    check(ca.read(b + AESCNTL, 0) & AES_START == 0, "AES_START reads back clear once done")
    check(ca.tick(0)[0] and ca.read(b + INTSTAT1, 0) & CRYPTINT,
          "CRYPTINT raised on the core line when unmasked")
    ca.write(b + INTACK1, CRYPTINT, 0)
    check(not ca.tick(0)[0], "acked: line drops")

    print("\nAll %d BLE core self-tests passed." % n)
    return n


if __name__ == "__main__":
    _run_selftests()
