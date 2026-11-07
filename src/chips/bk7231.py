from .base import Chip

# The generic BK7231T/U identity (default). BK7231T-era firmwares do not check
# the id registers at all, so this default is safe for them. The same identity
# is shared by many N/M/BL2028N images (BLE 5.1), so the id alone cannot safely
# turn the BLE model on: it is available through the legacy --ble-core only.
CHIP = Chip(
    name="BK7231",
    chip_id=0x0007231A,
    dev_id=0x18520001,
    gpio_pins=32,
    ble_core="5.1",
    xvr="v1",
    ejpeg="v1",
    pwm="v2",
)
