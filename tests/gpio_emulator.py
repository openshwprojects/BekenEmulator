"""Fast integration tests for GPIO injection and emulator register routing."""

import pickle
import struct
import sys
import threading
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from src.chips import CHIPS
from src.emulator import BekenEmulator, SimulatorState
from src import periph


class FakeMemory:
    """The tiny mem_read/mem_write surface used by the MMIO hooks."""

    def __init__(self):
        self.words = {}

    def mem_write(self, address, data):
        data = bytes(data)
        for offset, octet in enumerate(data):
            self.words[address + offset] = octet

    def mem_read(self, address, size):
        return bytes(self.words.get(address + offset, 0) for offset in range(size))

    def word(self, address):
        return struct.unpack("<I", self.mem_read(address, 4))[0]


def make_emulator(extended=False):
    """Build only the host-side pieces needed by GPIO; no Unicorn instance."""
    emu = BekenEmulator.__new__(BekenEmulator)
    emu.chip = CHIPS["BK7231U" if extended else "BK7231"]
    emu.gpio_pin_count = 40 if extended else 32
    emu.state = SimulatorState()
    if extended:
        from src.gpio import GpioModel
        emu.state.gpio_hw = GpioModel(40)
    emu._gpio_lock = threading.Lock()
    emu._gpio_events = []
    emu._emit_insns = False
    emu.wifimac = None
    emu.blecore = None
    emu.xvr_selfclear = False
    emu.trng_state = None
    emu.chip_id_value = 0
    emu.device_id_value = 0
    return emu


def write(emu, memory, address, value):
    emu.hook_mem_write_mmio(memory, 0, address, 4, value, None)


def read(emu, memory, address):
    emu.hook_mem_read_mmio(memory, 0, address, 4, 0, None)
    return memory.word(address)


def test_pin_routing_and_queued_drive():
    emu = make_emulator()
    pull_up = 0x3C
    emu.state.gpio_cfg[9] = pull_up

    assert emu._gpio_pin_at(emu.GPIO_BASE + 9 * 4) == 9
    assert emu._gpio_pin_at(emu.GPIO_BASE + 31 * 4) == 31
    assert emu._gpio_pin_at(emu.GPIO_FUNC_CFG) is None
    assert emu._gpio_pin_at(emu.GPIO_INTEN) is None
    assert emu._gpio_pin_at(emu.GPIO_EXT_PIN_END) is None

    emu.set_gpio_input(9, 0)
    assert emu.state.gpio_hw.inputs == {}, "host calls must only enqueue"
    emu._apply_gpio_events(emu.state)
    assert emu.state.gpio_hw.inputs == {9: 0}
    assert not (emu.state.gpio_hw.read_config(9, pull_up) & 1)

    emu.set_gpio_input(9, None)
    emu._apply_gpio_events(emu.state)
    assert emu.state.gpio_hw.inputs == {}
    assert emu.state.gpio_hw.read_config(9, pull_up) & 1

    try:
        emu.set_gpio_input(32, 0)
    except ValueError:
        pass
    else:
        raise AssertionError("a controller-register slot must not be injectable as GPIO32")


def test_old_pwm_timer_status_uses_configured_channel():
    # One nibble per channel: enable, interrupt-enable, then mode=timer (1).
    # The Q SDK's 0x7000 therefore names channel 3, and must never synthesize
    # the channel-0 status bit that has no handler in that firmware.
    status = BekenEmulator._old_pwm_timer_status_mask(0x7000)
    assert status == 1 << 3
    assert not (status & 1)
    assert BekenEmulator._old_pwm_timer_status_mask(0x0007) == 1
    assert BekenEmulator._old_pwm_timer_status_mask(0x0001) == 0  # PWM output


def test_interrupt_register_hooks_and_icu_line():
    emu = make_emulator()
    memory = FakeMemory()
    pin = 6
    bit = 1 << pin
    pull_up = 0x3C
    emu.state.gpio_cfg[pin] = pull_up

    # Mode 3 is falling edge.  INTLV and INTEN are controller registers, not
    # extra GPIO config words, even though they share the same MMIO window.
    write(emu, memory, emu.GPIO_INTLV0, 3 << (2 * pin))
    write(emu, memory, emu.GPIO_INTEN, bit)
    assert set(emu.state.gpio_cfg) == {pin}
    assert read(emu, memory, emu.GPIO_INTLV0) == 3 << (2 * pin)
    assert read(emu, memory, emu.GPIO_INTEN) == bit

    emu.set_gpio_input(pin, 0)
    emu._apply_gpio_events(emu.state)
    emu._sync_gpio_irq(emu.state)
    assert read(emu, memory, emu.GPIO_INTSTA) == bit
    assert emu.state.pending_irqs & (1 << 7), "GPIO must drive ICU IRQ bit 7"

    # INTSTA is W1C.  A held edge does not re-latch until release + press.
    write(emu, memory, emu.GPIO_INTSTA, bit)
    assert read(emu, memory, emu.GPIO_INTSTA) == 0
    assert not (emu.state.pending_irqs & (1 << 7))
    emu.set_gpio_input(pin, None)
    emu.set_gpio_input(pin, 0)
    emu._apply_gpio_events(emu.state)
    emu._sync_gpio_irq(emu.state)
    assert read(emu, memory, emu.GPIO_INTSTA) == bit
    write(emu, memory, emu.GPIO_INTSTA, bit)

    # Level interrupt semantics are visible immediately through MMIO.  The
    # guest need not wait for the emulator's next pacing chunk after enabling
    # a source or clearing a still-active level.
    emu = make_emulator()
    memory = FakeMemory()
    high_pin = 7
    high_bit = 1 << high_pin
    emu.state.gpio_cfg[high_pin] = pull_up
    write(emu, memory, emu.GPIO_INTLV0, 1 << (2 * high_pin))  # high level
    write(emu, memory, emu.GPIO_INTEN, high_bit)
    assert read(emu, memory, emu.GPIO_INTSTA) == high_bit
    assert emu.state.pending_irqs & (1 << 7)
    write(emu, memory, emu.GPIO_INTSTA, high_bit)
    assert read(emu, memory, emu.GPIO_INTSTA) == high_bit, "held level must reassert now"


def test_legacy_extended_snapshot_migration():
    emu = make_emulator(extended=True)
    memory = FakeMemory()
    emu.mu = memory
    del emu.state.gpio_hw

    pin = 39
    slot = emu.GPIO_EXT_SLOT_FIRST + pin - emu.GPIO_EXT_PIN_FIRST
    memory.mem_write(emu.GPIO_BASE + slot * 4, struct.pack("<I", 0x3C))
    memory.mem_write(emu.GPIO_INTEN2, struct.pack("<I", 1 << 7))
    memory.mem_write(emu.GPIO_INTLV3, struct.pack("<I", 1 << (2 * 7)))
    emu._restore_legacy_gpio_state()

    assert emu.state.gpio_cfg[slot] == 0x3C
    assert emu.state.gpio_hw.pin_count == 40
    assert emu.state.gpio_hw.int_enable == 1 << pin
    assert emu.state.gpio_hw.int_modes[pin] == 1
    emu._sync_gpio_irq(emu.state)
    assert emu.state.gpio_hw.int_status == 1 << pin
    assert emu.state.pending_irqs & (1 << 7)


def test_extended_peripheral_decode_uses_logical_pins():
    raw = {
        32: 0x11111111,       # FUNC_CFG, not logical P32
        46: 0x22222222,
        47: 0x33333333,
        48: 0x3C,             # logical P32
        55: 0x2C,             # logical P39
    }
    for variant in ("BK7231U", "BK7252"):
        decoded = periph.decode_state(raw, {}, variant)
        assert decoded["raw"] == {32: 0x3C, 39: 0x2C}
        assert len(decoded["gpio"]) == 40
        assert dict(decoded["func"]) == {
            "REG_GPIO_FUNC_CFG": "0x11111111",
            "REG_GPIO_FUNC_CFG_2": "0x22222222",
            "REG_GPIO_FUNC_CFG_3": "0x33333333",
        }


def test_extended_bank_and_snapshot_payload():
    emu = make_emulator(extended=True)
    memory = FakeMemory()
    assert emu._gpio_pin_at(emu.GPIO_BASE + 48 * 4) == 32
    assert emu._gpio_pin_at(emu.GPIO_BASE + 55 * 4) == 39
    assert emu._gpio_pin_at(emu.GPIO_INTEN2) is None

    pin = 39
    bit = 1 << pin
    emu.state.gpio_cfg[55] = 0x2C  # pull-down at physical slot 55
    write(emu, memory, emu.GPIO_INTLV3, 2 << (2 * 7))  # rising edge
    write(emu, memory, emu.GPIO_INTEN2, 1 << 7)
    emu.set_gpio_input(pin, 1)
    emu._apply_gpio_events(emu.state)
    emu._sync_gpio_irq(emu.state)
    assert emu.state.gpio_hw.int_status == bit
    assert read(emu, memory, emu.GPIO_INTSTA2) == 1 << 7

    # The live model has no locks/callbacks and remains snapshot-safe.
    restored = pickle.loads(pickle.dumps(emu.state, protocol=4))
    assert restored.gpio_hw.pin_count == 40
    assert restored.gpio_hw.inputs == {pin: 1}
    assert restored.gpio_hw.int_status == bit


def test_close_network_detaches_peer_callback():
    class FakePeer:
        def __init__(self):
            self.closed = False

        def on_device_tx(self, _frame, _info=None):
            pass

        def close(self):
            self.closed = True

    peer = FakePeer()
    emu = BekenEmulator.__new__(BekenEmulator)
    emu.wifi_peer = peer
    emu.wifimac = SimpleNamespace(on_tx=peer.on_device_tx)
    emu.close_network()
    assert emu.wifi_peer is None
    assert emu.wifimac.on_tx is None, "closed peer callback still retains the network stack"
    assert peer.closed


def main():
    tests = [
        test_pin_routing_and_queued_drive,
        test_old_pwm_timer_status_uses_configured_channel,
        test_interrupt_register_hooks_and_icu_line,
        test_extended_bank_and_snapshot_payload,
        test_legacy_extended_snapshot_migration,
        test_extended_peripheral_decode_uses_logical_pins,
        test_close_network_detaches_peer_callback,
    ]
    for test in tests:
        test()
        print("  [PASS]", test.__name__)
    print("\nAll %d GPIO/emulator integration tests passed." % len(tests))


if __name__ == "__main__":
    main()
