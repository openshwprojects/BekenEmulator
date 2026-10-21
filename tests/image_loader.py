"""Fast regression tests for firmware-image layout detection and mapping."""

import os
import struct
import sys
import threading
from pathlib import Path
from unittest.mock import mock_open, patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from unicorn.arm_const import UC_ARM_REG_CPSR, UC_ARM_REG_PC

from src.crypto import (crc16, decode_crc_prefix, detect_bk7252_app_only,
                        _arm_vector_destinations)
from src.main import build_emulator


def _slow_crc16(data, initial=0):
    """Bit-for-bit reference for the table-driven production implementation."""
    reg = initial
    for octet in data:
        for bit in range(8):
            top = reg & 0x8000
            if octet & (0x80 >> bit):
                top ^= 0x8000
            reg <<= 1
            if top:
                reg ^= 0x8005
            reg &= 0xFFFF
    return reg


def _crc_encode(data):
    assert len(data) % 32 == 0
    encoded = bytearray()
    for offset in range(0, len(data), 32):
        block = data[offset:offset + 32]
        encoded.extend(block)
        encoded.extend(crc16(block, 0xFFFF).to_bytes(2, "big"))
    return bytes(encoded)


def _vectors(app_linked=True):
    """A BK-style ARM vector table plus its seven LDR-PC literals."""
    out = bytearray(0x40)
    struct.pack_into("<I", out, 0, 0xEA00000E)  # B base+0x40
    for i in range(1, 8):
        struct.pack_into("<I", out, i * 4, 0xE59FF014)
    base = 0x10100 if app_linked else 0x500
    for i in range(7):
        struct.pack_into("<I", out, 0x20 + i * 4, base + i * 0x20)
    struct.pack_into("<I", out, 0x3C, 0xDEADBEEF)
    return bytes(out)


def _synthetic_app_window():
    app = bytearray(b"\xff" * 0x10000)
    app[:0x40] = _vectors(app_linked=True)
    # Absolute data/code pointers must survive mapping without relocation.
    struct.pack_into("<I", app, 0x200, 0x00080BDD)
    raw = _crc_encode(app)
    # A raw-storage tail starts immediately after the CRC-framed application.
    raw += b"raw-config-tail" + b"\xff" * 64
    return bytes(app), raw


def _assert_raises(fragment, function):
    try:
        function()
    except ValueError as exc:
        assert fragment in str(exc), str(exc)
    else:
        raise AssertionError("expected ValueError containing %r" % fragment)


def _build_from_bytes(raw, **kwargs):
    """Exercise build_emulator without creating a platform-specific temp file."""
    with patch("builtins.open", mock_open(read_data=raw)):
        return build_emulator("synthetic.bin", **kwargs)


def test_crc_prefix():
    samples = [b"", b"a", bytes(range(32)), bytes(range(255)), b"Beken" * 211]
    for sample in samples:
        assert crc16(sample, 0xFFFF) == _slow_crc16(sample, 0xFFFF)

    first = bytes(range(32))
    second = bytes(reversed(range(32)))
    later = b"L" * 32
    encoded = _crc_encode(first + second)
    encoded += b"X" * 32 + b"\x00\x00"  # deliberately invalid third block
    encoded += _crc_encode(later)          # must not resume after the gap
    decoded, consumed = decode_crc_prefix(encoded)
    assert decoded == first + second
    assert consumed == 68
    limited, limited_consumed = decode_crc_prefix(encoded, max_blocks=1)
    assert limited == first and limited_consumed == 34


def test_app_window_detection_and_mapping():
    app, raw = _synthetic_app_window()
    detected = detect_bk7252_app_only(raw)
    assert detected == (app, len(_crc_encode(app)))
    assert _arm_vector_destinations(app, load_address=0x10000)[:1] == (0x10040,)

    logs = []
    emu = _build_from_bytes(raw, chip="BK7252", only_uart=True,
                            save_decrypted=False, log=logs.append)

    assert emu.bootloader is None
    assert emu.app == app
    assert emu.exception_vector_base == 0x10000
    assert len(emu.raw_flash) == emu.FLASH_SIZE
    assert emu.raw_flash[:0x10000] == b"\xff" * 0x10000
    assert emu.raw_flash[0x10000:0x20000] == app
    assert emu.raw_flash[0x20000:] == b"\xff" * (emu.FLASH_SIZE - 0x20000)
    assert emu.physical_flash[:0x11000] == b"\xff" * 0x11000
    assert emu.physical_flash[0x11000:0x11000 + len(raw)] == raw
    assert struct.unpack_from("<I", emu.app, 0x200)[0] == 0x00080BDD
    assert any("physical 0x11000" in line for line in logs)

    _assert_raises("has no bootloader", lambda: _build_from_bytes(
        raw, chip="BK7252", with_boot=True, only_uart=True,
        save_decrypted=False))
    _assert_raises("do not pass -key", lambda: _build_from_bytes(
        raw, key=(0, 0, 0, 0), chip="BK7252", only_uart=True,
        save_decrypted=False))

    # The layout alone must not silently change another requested chip.
    other = _build_from_bytes(raw, chip="BK7252N", only_uart=True,
                              save_decrypted=False)
    assert other.exception_vector_base == 0
    assert bytes(other.physical_flash[:len(raw)]) == raw

    # Exercise all three manually injected exception types at the relocated
    # table; this is the behaviour the real app needs after its first tick.
    emu.setup()
    emu.state.icu_global_int_en = 3
    emu.mu.reg_write(UC_ARM_REG_CPSR, 0x13)
    emu.mu.reg_write(UC_ARM_REG_PC, 0x10100)
    emu.trigger_irq()
    assert emu.mu.reg_read(UC_ARM_REG_PC) == 0x10018
    emu.mu.reg_write(UC_ARM_REG_CPSR, 0x13)
    emu.mu.reg_write(UC_ARM_REG_PC, 0x10100)
    emu.trigger_fiq()
    assert emu.mu.reg_read(UC_ARM_REG_PC) == 0x1001C
    emu.mu.reg_write(UC_ARM_REG_CPSR, 0x13)
    emu.mu.reg_write(UC_ARM_REG_PC, 0x10100)
    emu.hook_intr(emu.mu, 2, None)
    assert emu.mu.reg_read(UC_ARM_REG_PC) == 0x10008


def test_full_image_precedence_and_false_positive_guard():
    full_path = ROOT / "firmwares" / "OpenBK7252_QIO_1.18.300.bin"
    full = full_path.read_bytes()
    assert detect_bk7252_app_only(full) is None
    emu = build_emulator(full_path, chip="BK7252", only_uart=True,
                         save_decrypted=False)
    assert emu.exception_vector_base == 0
    assert bytes(emu.physical_flash[:len(full)]) == full
    assert emu.physical_flash[len(full):] == b"\xff" * (len(emu.physical_flash) - len(full))

    low_app = bytearray(b"\xff" * 0x10000)
    low_app[:0x40] = _vectors(app_linked=False)
    assert detect_bk7252_app_only(_crc_encode(low_app)) is None


def test_flash_size():
    """Physical flash is the whole chip, not just the bytes the file carried."""
    MiB = 1024 * 1024
    app, raw = _synthetic_app_window()

    # An app-only build is ~1 MB, but firmware keeps data at the top of the
    # chip (the A9 camera stores its MAC in the last 4K sector, 0x1FF000).
    emu = _build_from_bytes(raw, chip="BK7252", only_uart=True,
                            save_decrypted=False)
    assert len(emu.physical_flash) == 4 * MiB
    assert emu.physical_flash[0x11000:0x11000 + len(raw)] == raw
    assert emu.physical_flash[0x11000 + len(raw):] == b"\xff" * (4 * MiB - 0x11000 - len(raw))
    emu._flash_erase(0x1FF000, 0x1000)
    emu._flash_program(0x1FF000, [0x8C47C8A5, 0xC6B4508C])
    assert emu.physical_flash[0x1FF000:0x1FF008] == bytes.fromhex("a5c8478c8c50b4c6")

    # Dumps of both module sizes load (the bundled 2 MiB app-only read, which
    # runs past 2 MiB once placed at 0x11000, and the 4 MiB full dumps); a dump
    # that fills the BK7252's flash fits and one byte more is rejected rather
    # than truncated.
    for size in (2 * MiB, 2 * MiB + 0x11000, 4 * MiB):
        assert len(_build_from_bytes(b"\xa5" * size, chip="BK7252", only_uart=True,
                                     save_decrypted=False).physical_flash) == 4 * MiB
    _assert_raises("does not fit", lambda: _build_from_bytes(
        b"\xa5" * (4 * MiB + 1), chip="BK7252", only_uart=True, save_decrypted=False))

    # A chip without a flash size takes the smallest power of two that holds
    # the dump, at least 2 MiB.
    for size, want in ((1 * MiB, 2 * MiB), (2 * MiB, 2 * MiB), (3 * MiB, 4 * MiB),
                       (4 * MiB, 4 * MiB)):
        emu = _build_from_bytes(b"\xff" * size, chip="BK7231N", only_uart=True,
                                save_decrypted=False)
        assert len(emu.physical_flash) == want, size


def test_ble_core_selection():
    """Real BLE 5.x silicon is automatic; the legacy override stays narrow."""
    raw = b"\xff" * 68

    bk7231n = _build_from_bytes(raw, chip="BK7231N", only_uart=True,
                                save_decrypted=False)
    bk7238 = _build_from_bytes(raw, chip="BK7238", only_uart=True,
                               save_decrypted=False)
    assert bk7231n.blecore is not None and bk7231n.blecore.layout == "5.1"
    assert bk7238.blecore is not None and bk7238.blecore.layout == "5.2"

    # Generic BK7231 may mean T/U (BLE 4.2) or N/M (BLE 5.1), so it cannot be
    # enabled from chip identity alone.  The retained flag is an explicit
    # assertion that this particular image is N/M-family firmware.
    ambiguous = _build_from_bytes(raw, chip="BK7231", only_uart=True,
                                  save_decrypted=False)
    forced = _build_from_bytes(raw, chip="BK7231", only_uart=True,
                               save_decrypted=False, ble_core=True)
    assert ambiguous.blecore is None
    assert forced.blecore is not None and forced.blecore.layout == "5.1"

    # BK7231U serves the generic family's same silicon ID, but an explicit
    # variant selection exposes its second GPIO bank and keeps its BLE 4.2
    # hardware out of the unrelated BLE 5.1 model.
    bk7231u = _build_from_bytes(raw, chip="BK7231U", only_uart=True,
                               save_decrypted=False)
    assert bk7231u.gpio_pin_count == 40
    assert bk7231u.blecore is None
    assert ambiguous.gpio_pin_count == 32
    _assert_raises("this chip has no such core", lambda: _build_from_bytes(
        raw, chip="BK7231U", only_uart=True, save_decrypted=False,
        ble_core=True))

    # False remains a deliberate API-level diagnostic opt-out, while chips
    # without this BLE generation stay off in automatic mode and reject a
    # nonsensical force exactly as the CLI has always done.
    disabled = _build_from_bytes(raw, chip="BK7231N", only_uart=True,
                                 save_decrypted=False, ble_core=False)
    unsupported = _build_from_bytes(raw, chip="BK7252", only_uart=True,
                                    save_decrypted=False)
    assert disabled.blecore is None
    assert unsupported.blecore is None
    _assert_raises("this chip has no such core", lambda: _build_from_bytes(
        raw, chip="BK7252", only_uart=True, save_decrypted=False,
        ble_core=True))


def optional_real_bk7252u_smoke():
    """Run the public A9 dump when BK7252U_DUMP names it; CI stays synthetic."""
    path = os.environ.get("BK7252U_DUMP")
    if not path:
        print("SKIP real BK7252U smoke (set BK7252U_DUMP to enable)")
        return

    output = {1: bytearray(), 2: bytearray()}
    holder = {}

    def uart_sink(port, byte):
        if port not in output:
            return
        output[port].append(byte)
        if any(stream.endswith(b"msh />") for stream in output.values()) and "emu" in holder:
            holder["emu"].stop()

    emu = build_emulator(path, chip="BK7252", only_uart=True,
                         save_decrypted=False, uart_sink=uart_sink)
    holder["emu"] = emu
    emu.setup()
    worker = threading.Thread(target=emu.run, daemon=True)
    worker.start()
    worker.join(120)
    if worker.is_alive():
        emu.stop()
        worker.join(5)
        raise AssertionError("BK7252U firmware did not reach its shell in 120 seconds")

    text = b"\n".join(output.values()).decode("latin-1", "replace")
    for marker in (
        "- RT -     Thread Operating System",
        "3.1.0 build May 14 2022",
        "OSK Rev: R-3.0.22",
        "SDK Rev: 3.0.33",
        "lwIP-2.0.2 initialized!",
        "beken wlan hw init",
        "drv_pm_init",
        "msh />",
    ):
        assert marker in text, "missing UART marker: %s" % marker
    assert emu.run_error is None, emu.run_error


if __name__ == "__main__":
    test_crc_prefix()
    test_app_window_detection_and_mapping()
    test_full_image_precedence_and_false_positive_guard()
    test_flash_size()
    test_ble_core_selection()
    optional_real_bk7252u_smoke()
    print("All image-loader self-tests passed.")
