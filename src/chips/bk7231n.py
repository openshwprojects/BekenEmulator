from .base import Chip

# BK7231N/M silicon's own identity (sys_ctrl.h: CHIP_ID_BK7231N,
# DEVICE_ID_BK7231N_A). Most N-family builds run fine on the generic BK7231
# identity, but newer SDK builds check it: their bk_check_chip_id accepts chip
# 0x7231C with device 0x2015xxxx / 0x2052xxxx / 0x20A2xxxx only, and hangs after
# "Unsupported chip or dev (7231a.18520001)" otherwise (VeSync plug 1.0.02,
# ChinaMobile HeJiaQin plug). Not the default because the real id also selects
# the full BK7231N RF calibration (bk7231N_cal.c), which measures test tones
# through the unmodelled RC block at 0x01050000 between thousands of busy-wait
# delays: still inside calibration_main after 70M instructions. Fine where the
# check demands it; slow otherwise.
CHIP = Chip(
    name="BK7231N",
    chip_id=0x0007231C,
    dev_id=0x20521023,
    gpio_pins=32,
    ble_core="5.1",
    ble_auto=True,
    xvr="v1",
    ejpeg="v1",
    pwm="v2",
)
