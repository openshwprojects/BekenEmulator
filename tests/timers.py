"""Hardware timer banks (0x802A00 / 0x802A40): counters, divider, enable, W1C status.

BK7252 counts them in device time; every other chip keeps the fixed tick.
Device time is state.insn_count: TIMER_INSNS_PER_SEC instructions are one second.
"""
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from src.chips import CHIPS
from src.emulator import BekenEmulator, SLOW_TICK_INSNS

ICU_BIT_TIMER = 1 << 8
BANK0_COUNTER, BANK0_CTL = 0x00802A00, 0x00802A0C
BANK1_COUNTER, BANK1_CTL = 0x00802A40, 0x00802A4C
STATUS0 = 1 << 7                       # irq_status bit of the bank's first timer
SECOND = BekenEmulator.TIMER_INSNS_PER_SEC


def _emu(name="BK7252"):
    emu = BekenEmulator(b"\xff" * 0x20000, None, b"", only_uart=True, chip=CHIPS[name])
    emu.setup()
    emu.state.icu_int_enable |= ICU_BIT_TIMER
    return emu


def _write(emu, address, value):
    emu.hook_mem_write_mmio(emu.mu, 0, address, 4, value, None)


def _ctl(emu, bank):
    return emu.state.timer0_2_ctl if bank == 0 else emu.state.timer3_5_ctl


def _step(emu, insns):
    emu.state.insn_count += insns
    emu._pacing(emu.state)


def test_period_follows_the_counter():
    """26 MHz with counter 26000 is the FreeRTOS 1 kHz tick: 1/1000 of a second."""
    emu = _emu()
    period = SECOND // 1000
    _write(emu, BANK0_COUNTER, 26000)
    _write(emu, BANK0_CTL, 0x1)
    _step(emu, period - 1)
    assert not _ctl(emu, 0) & STATUS0
    _step(emu, 1)
    assert _ctl(emu, 0) & STATUS0
    assert emu.state.pending_irqs & ICU_BIT_TIMER


def test_one_second_gives_a_thousand_ticks():
    emu = _emu()
    _write(emu, BANK0_COUNTER, 26000)
    _write(emu, BANK0_CTL, 0x1)
    ticks = 0
    for _ in range(SECOND // 1000):
        _step(emu, 1000)
        if _ctl(emu, 0) & STATUS0:
            ticks += 1
            _write(emu, BANK0_CTL, 0x1 | STATUS0)      # W1C, timer stays enabled
    assert ticks == 1000, ticks


def test_divider_and_32k_bank():
    """Bank 1 runs on 32 kHz; the divider is clk_divider + 1."""
    emu = _emu()
    _write(emu, BANK1_COUNTER, 32)
    _write(emu, BANK1_CTL, 0x1 | (1 << 3))             # timer 3, divider 1
    period = 32 * 2 * SECOND // 32000
    _step(emu, period - 1)
    assert not _ctl(emu, 1) & STATUS0
    _step(emu, 1)
    assert _ctl(emu, 1) & STATUS0


def test_timers_are_independent():
    emu = _emu()
    _write(emu, BANK0_COUNTER + 8, 26000)              # timer 2 of bank 0
    _write(emu, BANK0_CTL, 0x4)
    _step(emu, SECOND // 1000)
    assert _ctl(emu, 0) & (1 << 9)
    assert not _ctl(emu, 0) & (STATUS0 | (1 << 8))


def test_disabled_timer_never_fires():
    emu = _emu()
    _write(emu, BANK0_COUNTER, 26000)
    _step(emu, SECOND)
    assert not _ctl(emu, 0) & (0x7 << 7)
    _write(emu, BANK0_CTL, 0x1)
    _step(emu, SECOND // 2000)
    _write(emu, BANK0_CTL, 0x0)
    _step(emu, SECOND)
    assert not _ctl(emu, 0) & (0x7 << 7)


def test_missed_periods_coalesce_into_one_tick():
    emu = _emu()
    period = SECOND // 1000
    _write(emu, BANK0_COUNTER, 26000)
    _write(emu, BANK0_CTL, 0x1)
    _step(emu, 10 * period)
    assert _ctl(emu, 0) & STATUS0
    _write(emu, BANK0_CTL, 0x1 | STATUS0)
    assert not _ctl(emu, 0) & STATUS0
    _step(emu, period - 1)
    assert not _ctl(emu, 0) & STATUS0
    _step(emu, 1)
    assert _ctl(emu, 0) & STATUS0


def test_acknowledged_status_drops_the_interrupt():
    emu = _emu()
    _write(emu, BANK0_COUNTER, 26000)
    _write(emu, BANK0_CTL, 0x1)
    _step(emu, SECOND // 1000)
    assert emu.state.pending_irqs & ICU_BIT_TIMER
    _write(emu, BANK0_CTL, 0x1 | STATUS0)
    _step(emu, 1)
    assert not emu.state.pending_irqs & ICU_BIT_TIMER


def test_next_timer_due_bounds_idle_skipping():
    emu = _emu()
    assert emu._next_timer_due(emu.state) is None
    _write(emu, BANK0_COUNTER, 26000)
    _write(emu, BANK0_CTL, 0x1)
    assert emu._next_timer_due(emu.state) == emu.state.insn_count + SECOND // 1000
    _write(emu, BANK0_CTL, 0x0)
    assert emu._next_timer_due(emu.state) is None


def test_other_chips_keep_the_fixed_tick():
    for name in ("BK7231", "BK7231N", "BK7238"):
        emu = _emu(name)
        _step(emu, SLOW_TICK_INSNS)
        assert emu.state.timer3_5_ctl & STATUS0, name      # no counter written, still fires
    emu = _emu("BK7252")
    _step(emu, 10 * SLOW_TICK_INSNS)
    assert not emu.state.timer3_5_ctl & (0x7 << 7)         # BK7252 needs an enabled timer


if __name__ == "__main__":
    test_period_follows_the_counter()
    test_one_second_gives_a_thousand_ticks()
    test_divider_and_32k_bank()
    test_timers_are_independent()
    test_disabled_timer_never_fires()
    test_missed_periods_coalesce_into_one_tick()
    test_acknowledged_status_drops_the_interrupt()
    test_next_timer_due_bounds_idle_skipping()
    test_other_chips_keep_the_fixed_tick()
    print("All timer tests passed.")
