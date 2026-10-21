from .base import Chip

# Accepts dev 0x23A1xxxx or 0x2431xxxx (bk_check_chip_id). Not a BK7252 variant:
# its GDMA, I2C and EJPEG are the second generation (EJPEG at 0x00A03000, with
# nothing at 0x0080A000), and 0x900000 is the xvr register block.
CHIP = Chip(
    name="BK7252N",
    chip_id=0x0007252A,
    dev_id=0x23A18000,
    gpio_pins=32,
    xvr="v1",
    gdma="v2",
    i2c="v2",
    ejpeg="v2",
    audio="v1",
    saradc="v3",
    bktimer_v2=True,
    rtthread_console=True,
)
