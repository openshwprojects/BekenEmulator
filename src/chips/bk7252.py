from .base import MIB, Chip

# BK7252 = BK7221U silicon (the A9-style WiFi cameras). It is the odd one out of
# the Beken family: external SDRAM at 0x00900000, where its RT-Thread firmware
# puts the whole system heap (rt_system_heap_init(RT_HW_SDRAM_BEGIN=0x900000,
# +256K)). No BLE 5.x core is modelled. UART tools read from physical flash
# 0x11000 (CPU 0x10000, the app) and save that window at file offset zero.
CHIP = Chip(
    name="BK7252",
    chip_id=0x0007221A,
    dev_id=0x18221020,
    gpio_pins=40,
    flash_size=4 * MIB,
    app_only_phys=0x11000,
    sdram=(0x00900000, 0x40000),
    ejpeg="v1",
    audio="v1",
    saradc="v2",
    timer="v2",
    rtthread_console=True,
    usb_plug_wake=True,
)
