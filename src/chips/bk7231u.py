from .base import Chip

# Same served ID as the generic BK7231, but its own chip exposes the U variant's
# GPIO32..39 bank without mapping those addresses as pins for BK7231T firmware.
# T/U carry the older BLE 4.2 IP, which is not modelled.
CHIP = Chip(
    name="BK7231U",
    chip_id=0x0007231A,
    dev_id=0x18520001,
    gpio_pins=40,
    xvr="v1",
    ejpeg="v1",
    pwm="v2",
)
