"""One file per emulated SoC; each exposes CHIP, a Chip describing it.

--chip selects one of these. Adding a chip is a new file and a line below.
"""
from .base import Chip
from . import bk7231, bk7231n, bk7231u, bk7238, bk7252, bk7252n

CHIPS = {chip.name: chip for chip in (
    bk7231.CHIP, bk7231u.CHIP, bk7231n.CHIP,
    bk7238.CHIP, bk7252.CHIP, bk7252n.CHIP,
)}

DEFAULT_CHIP = CHIPS["BK7231"]


def chip_names():
    return sorted(CHIPS)


def get_chip(name):
    """The chip for a --chip value (case-insensitive)."""
    chip = CHIPS.get((name or "").strip().upper())
    if chip is None:
        raise ValueError("Unknown -chip value: %r. Known chips: %s."
                         % (name, ", ".join(chip_names())))
    return chip
