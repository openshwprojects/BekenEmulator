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
FINETIMTGT = 0x0E4
CLKNTGT = (0x0E8, 0x0F0, 0x0F8)          # CLKNTGT1..3
HMICROSECTGT = (0x0EC, 0x0F4, 0x0FC)     # HMICROSECTGT1..3
SLOTCLK, FINETIMECNT = 0x100, 0x104
SIZE = 0x200                              # window the model claims

VERSION_RESET = 0x0B001100               # BLE_VERSION_RESET, 5.2 blecore

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

SLOT_US = 625
LP_CLOCK_HZ = 32768                      # the sleep clock DEEPSLWKUP counts in


class BleCore:
    """One BLE core. Drive it with write()/read()/tick(); poll pending_*."""

    def __init__(self, base, insns_per_second, max_sleep_us=10_000):
        self.base = base
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
            return VERSION_RESET
        if off == INTSTAT0:
            return self.raw_evt & self.regs.get(INTCNTL0, 0)
        if off == INTSTAT1:
            return self.raw_core & self.regs.get(INTCNTL1, 0)
        if off == DEEPSLCNTL:
            v = self.regs.get(DEEPSLCNTL, 0)
            return v | DEEP_SLEEP_STAT if self.sleeping else v & ~DEEP_SLEEP_STAT
        if off == SLOTCLK:
            # SAMP and CLKN_UPD are commands; they always read back clear.
            return self.slot_latch & SLOT_MASK
        if off == FINETIMECNT:
            return self.fine(insns) & FINE_MASK
        if off in (RWBLECNTL, DEEPSLSTAT):
            return self.regs.get(off, 0)
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
        if off == SLOTCLK:
            if value & SAMP:
                self.slot_latch = self.slot(insns)
            return
        if off in (FINETIMTGT,) + CLKNTGT:
            # A new target may fire again even if the previous value did.
            self.fired_tgt.pop(off, None)
        self.regs[off] = value

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
        for i in range(3):
            bit = TIMESTAMPTGTINT[i]
            if not (mask0 & bit) or CLKNTGT[i] not in self.regs:
                continue
            tgt = self.regs[CLKNTGT[i]] & SLOT_MASK
            if self.fired_tgt.get(CLKNTGT[i]) == tgt or not self._reached(now, tgt):
                continue
            if now == tgt and self.fine(insns) < (self.regs.get(HMICROSECTGT[i], 0) & FINE_MASK):
                continue
            self.fired_tgt[CLKNTGT[i]] = tgt
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

    print("== masking ==")
    c2 = BleCore(b, 5_000_000)
    c2.write(b + RWBLECNTL, SWINT_REQ, 0)
    check(not c2.tick(0)[0] and c2.read(b + INTSTAT1, 0) == 0, "a masked-out interrupt neither shows in INTSTAT1 nor raises the line")
    check(c2.tick(0)[1] is False, "the BLE EVENT line (set 0) never rises: no radio events are modelled")

    print("\nAll %d BLE core self-tests passed." % n)
    return n


if __name__ == "__main__":
    _run_selftests()
