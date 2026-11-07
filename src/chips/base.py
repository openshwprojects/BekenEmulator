from dataclasses import dataclass

MIB = 1024 * 1024

# The flash controller stores each 32 data bytes with 2 CRC bytes: a physical
# flash address maps to the CPU/XIP address below it by these ratios.
CRC_BLOCK_PHYS = 34
CRC_BLOCK_DATA = 32


# Firmware addresses the whole flash chip, not just what a dump happened to
# carry: an app-only build is ~1 MB, yet keeps settings in the top sectors (the
# A9 camera writes its MAC to the last 4K, 0x1FF000), and program/erase past the
# end of the dump were dropped. This is the floor when a chip names no size.
DEFAULT_FLASH_SIZE = 2 * MIB


@dataclass(frozen=True)
class Chip:
    """What distinguishes one SoC from another; data only, no behaviour.

    Peripheral fields hold a variant name by generation ("v1", "v2"), or None
    when the chip lacks the peripheral. The emulator branches on these names.
    """
    name: str                  # the --chip key, upper case
    chip_id: int               # SCTRL chip id register (0x00800000)
    dev_id: int                # SCTRL device id register (0x00800004)
    gpio_pins: int             # 32, or 40 with the GPIO32..39 bank
    # BLE 5.x core, as the blecore.LAYOUTS key ("5.1" or "5.2"; CFG_BLE_VERSION
    # in the chip's sys_config_*.h), or None. ble_auto models it without
    # --ble-core.
    ble_core: str = None
    ble_auto: bool = False
    # Emulated flash size in bytes; None pads the dump to the smallest power
    # of two that holds it, at least 2 MiB.
    flash_size: int = None
    bootloader_addr: int = 0x00000000
    app_addr: int = 0x00010000
    # Physical flash address of an app-only dump (no bootloader), or None.
    app_only_phys: int = None
    ram_base: int = 0x00400000
    ram_size: int = 1 * MIB
    # (base, size) of external SDRAM at 0x900000, or None. Where there is none
    # that window is the xvr register block.
    sdram: tuple = None
    xvr: str = None
    gdma: str = "v1"
    i2c: str = "v1"
    ejpeg: str = None
    pwm: str = "v1"
    audio: str = None
    # v1: BK7231 die. v2: BK7252 (SOC_BK7221U), same CHNL_EN model but no state
    # register at 0x802c0c. v3: BK7238/BK7252N layout, served via 0x802c0c.
    saradc: str = "v1"
    # Hardware timers at 0x802A00 / 0x802A40. v1: the fixed periodic tick every
    # slow tick, whatever the firmware programs. v2: six timers counted in device
    # time from their counter, clock divider and enable bits.
    timer: str = "v1"
    # One-off quirks, plain flags until a second chip needs a variant name.
    bktimer_v2: bool = False       # timer 3-5 interrupt status layout (timer v1)
    rtthread_console: bool = False  # hold console input until RT-Thread's msh />
    usb_plug_wake: bool = False    # SCTRL_USB_PLUG_WAKEUP reports 5V present

    @property
    def app_only_cpu(self):
        """CPU/XIP address of the app-only dump (CRC bytes removed)."""
        return self.app_only_phys // CRC_BLOCK_PHYS * CRC_BLOCK_DATA
