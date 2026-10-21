"""Chip files: registry, values and what the emulator reads from them."""
import os
import pickle
import sys
import tempfile

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from src.chips import CHIPS, DEFAULT_CHIP, chip_names, get_chip
from src import periph
from src.emulator import BekenEmulator

MiB = 1024 * 1024

# The (chip_id, device_id) pairs of the old emulator.CHIP_FAMILIES table.
OLD_IDS = {
    "BK7231": (0x0007231A, 0x18520001),
    "BK7231U": (0x0007231A, 0x18520001),
    "BK7231N": (0x0007231C, 0x20521023),
    "BK7238": (0x00007238, 0x21128000),
    "BK7252": (0x0007221A, 0x18221020),
    "BK7252N": (0x0007252A, 0x23A18000),
}


def _assert_raises(text, fn):
    try:
        fn()
    except ValueError as exc:
        assert text in str(exc), str(exc)
        return
    raise AssertionError("expected ValueError containing %r" % text)


def test_ids_match_old_table():
    assert set(CHIPS) == set(OLD_IDS)
    for name, ids in OLD_IDS.items():
        assert (CHIPS[name].chip_id, CHIPS[name].dev_id) == ids, name


def test_registry():
    assert chip_names() == ["BK7231", "BK7231N", "BK7231U", "BK7238", "BK7252", "BK7252N"]
    assert get_chip(" bk7252 ") is CHIPS["BK7252"]
    _assert_raises("Known chips: BK7231, BK7231N", lambda: get_chip("nope"))


def test_bk7252_values():
    chip = CHIPS["BK7252"]
    assert chip.flash_size == 4 * MiB
    assert chip.app_only_phys == 0x11000
    assert chip.app_only_cpu == 0x10000
    assert chip.sdram == (0x900000, 0x40000)
    assert chip.xvr is None
    assert chip.gpio_pins == 40


def test_bk7252n_is_not_bk7252():
    chip = CHIPS["BK7252N"]
    assert chip.gdma == chip.i2c == chip.ejpeg == "v2"
    assert chip.audio == "v1"
    assert chip.sdram is None and chip.xvr == "v1"


def test_name_matches_key():
    for key, chip in CHIPS.items():
        assert chip.name == key


def _emu(name=None, **kwargs):
    chip = CHIPS[name] if name else None
    return BekenEmulator(b"\xff" * 0x20000, None, b"", only_uart=True, chip=chip, **kwargs)


def test_emulator_identity():
    for name, ids in OLD_IDS.items():
        emu = _emu(name)
        assert emu.chip is CHIPS[name]
        assert (emu.chip_id_value, emu.device_id_value) == ids, name


def test_default_chip():
    assert _emu().chip is DEFAULT_CHIP


def test_ble_selection():
    auto = {"BK7231N": "5.1", "BK7238": "5.2"}
    for name in OLD_IDS:
        emu = _emu(name)
        if name in auto:
            assert emu.blecore is not None and emu._ble_layout == auto[name], name
        else:
            assert emu.blecore is None, name
    assert _emu("BK7231", ble_core=True).blecore is not None
    for name in ("BK7231U", "BK7252", "BK7252N"):
        _assert_raises("this chip has no such core", lambda: _emu(name, ble_core=True))


def test_gpio_pins():
    for name in OLD_IDS:
        emu = _emu(name)
        want = 40 if name in ("BK7231U", "BK7252") else 32
        assert emu.gpio_pin_count == want, name
        assert emu.state.gpio_hw.pin_count == want, name


def test_periph_pin_layout():
    for name, want in (("BK7252", 40), ("BK7231U", 40), ("BK7231N", 32),
                       ("BK7231M", 32), ("", 32)):
        table = periph.decode_state({0: 1}, {}, chip=name)["gpio"]
        assert len(table) == want, name


def test_pwm_and_saradc_variants():
    assert {n for n, c in CHIPS.items() if c.pwm == "v2"} == {
        "BK7231", "BK7231U", "BK7231N", "BK7238"}
    assert {n: c.saradc for n, c in CHIPS.items()} == {
        "BK7231": "v1", "BK7231U": "v1", "BK7231N": "v1",
        "BK7252": "v2", "BK7238": "v3", "BK7252N": "v3"}


def _region(emu, address):
    """(begin, size) of the mapped region holding address, or None."""
    for begin, end, _perms in emu.mu.mem_regions():
        if begin <= address <= end:
            return begin, end - begin + 1
    return None


def test_memory_map():
    for name in OLD_IDS:
        emu = _emu(name)
        emu.setup()
        chip = CHIPS[name]
        assert _region(emu, 0x00400000) == (0x00400000, 1 * MiB), name
        want = (0x900000, 0x40000) if chip.sdram else (0x900000, 0x1000)
        assert _region(emu, 0x900000) == want, name
        want_a = (0xA00000, 0x10000) if chip.ejpeg == "v2" else None
        assert _region(emu, 0xA00000) == want_a, name


def test_memory_map_after_reset():
    for name in ("BK7252", "BK7252N", "BK7231N"):
        emu = _emu(name)
        emu.setup()
        before = sorted(emu.mu.mem_regions())
        emu.mu.mem_write(0x900000, b"\x55" * 8)
        emu._watchdog_reset()
        assert sorted(emu.mu.mem_regions()) == before, name
        assert bytes(emu.mu.mem_read(0x900000, 8)) == b"\x00" * 8, name


def _write(emu, address, value):
    emu.hook_mem_write_mmio(emu.mu, 0, address, 4, value, None)


def test_gdma_channel_layout():
    for name in OLD_IDS:
        emu = _emu(name)
        if name == "BK7252N":     # gdma v2: 0x809040 + ch * 0x40
            assert emu._gdma_channel(0x00809040) == 0, name
            assert emu._gdma_channel(0x00809080) == 1, name
            assert emu._gdma_channel(0x00809000) is None, name
        else:                     # gdma v1: 0x809000 + ch * 0x20
            assert emu._gdma_channel(0x00809000) == 0, name
            assert emu._gdma_channel(0x00809040) == 2, name
            assert emu._gdma_channel(0x00809010) is None, name


def test_ejpeg_blocks():
    for name in OLD_IDS:
        emu = _emu(name)
        emu.setup()
        _write(emu, 0x0080A000, 0x5)
        assert emu.state.ejpeg_reg0 == (0 if name == "BK7252N" else 0x5), name
        emu = _emu(name)
        emu.setup()
        if name == "BK7252N":
            _write(emu, 0x00A03008, 0x5)
            assert emu.state.ejpeg_reg0 == 0x5


def test_audio_block():
    for name in OLD_IDS:
        emu = _emu(name)
        emu.setup()
        _write(emu, 0x00802B08, 0x3C)
        has_audio = name in ("BK7252", "BK7252N")
        assert emu.state.audio_fifo_conf == (0x3C if has_audio else 0), name


def test_i2c_v2():
    for name in OLD_IDS:
        emu = _emu(name)
        assert emu.state.hw_i2c[1].get("v2", False) == (name == "BK7252N"), name
        emu.setup()
        emu._watchdog_reset()
        assert emu.state.hw_i2c[1].get("v2", False) == (name == "BK7252N"), name


def test_usb_plug_wake():
    for name in OLD_IDS:
        emu = _emu(name)
        emu.setup()
        word = emu.hook_mem_read_mmio(emu.mu, 0, 0x00800084, 4, 0, None)
        val = int.from_bytes(bytes(emu.mu.mem_read(0x00800084, 4)), "little")
        assert bool(val & (1 << 21)) == (name == "BK7252"), name


def test_no_chip_flags():
    emu = _emu("BK7252")
    for flag in ("is_bk7252", "is_bk7252n", "is_bk7231", "has_extended_gpio",
                 "has_pwm_new", "chip_variant"):
        assert not hasattr(emu, flag), flag


def test_snapshot_rejects_old():
    emu = _emu("BK7252")
    path = os.path.join(tempfile.mkdtemp(), "old.snap")
    with open(path, "wb") as f:
        pickle.dump({"magic": emu.SNAPSHOT_MAGIC, "chip": (0x0007221A, 0x18221020)}, f)
    _assert_raises("different -chip", lambda: emu.load_snapshot(path))


if __name__ == "__main__":
    test_ids_match_old_table()
    test_registry()
    test_bk7252_values()
    test_bk7252n_is_not_bk7252()
    test_name_matches_key()
    test_emulator_identity()
    test_default_chip()
    test_ble_selection()
    test_gpio_pins()
    test_periph_pin_layout()
    test_pwm_and_saradc_variants()
    test_memory_map()
    test_memory_map_after_reset()
    test_gdma_channel_layout()
    test_ejpeg_blocks()
    test_audio_block()
    test_i2c_v2()
    test_usb_plug_wake()
    test_no_chip_flags()
    test_snapshot_rejects_old()
    print("All chip tests passed.")
