from .base import Chip

# Accepts dev 0x2112xxxx or 0x2206xxxx (bk_check_chip_id).
CHIP = Chip(
    name="BK7238",
    chip_id=0x00007238,
    dev_id=0x21128000,
    gpio_pins=32,
    ble_core="5.2",
    ble_auto=True,
    xvr="v1",
    ejpeg="v1",
    pwm="v2",
    saradc="v3",
)
