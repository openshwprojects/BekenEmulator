#include "esp_machine.h"
#include <stdio.h>

int esp_timing_tests()
{
    int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { printf("[FAIL] ESP timing line %d: %s\n", __LINE__, #x); return -1; } } while (0)
    const esp::Chip chips[] = {esp::CHIP_ESP8266, esp::CHIP_ESP32};
    for (unsigned i = 0; i < 2; ++i) {
        esp::FlashImage layout;
        layout.chip = chips[i]; layout.app_index = layout.boot_index = 0;
        esp::Image image; image.chip = chips[i];
        image.entry_point = i ? 0x40080000 : 0x40100000;
        esp::Segment segment; segment.address = image.entry_point;
        segment.data.assign(0x400, '\0');
        segment.data.replace(0, 3, "\x00\x70\x00", 3); /* WAITI 0 */
        segment.data.replace(i ? 0x300 : 0x30, 3, "\x22\xa0\x17", 3); /* MOVI a2,23 at IRQ vector */
        image.segments.push_back(segment); layout.images.push_back(image);
        EspMachine machine(chips[i]);
        CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
        machine.cpu().set_special(XT_SR_VECBASE, image.entry_point);
        machine.cpu().set_special(XT_SR_PS, i ? 0x40000 : 0);
        machine.cpu().set_special(XT_SR_INTENABLE, 1u << 6);
        machine.cpu().set_special(240, 800); /* 10 us at 80 MHz */
        CHECK(machine.step() == XTENSA_OK);
        CHECK(machine.cpu().pc() == image.entry_point + (i ? 0x300 : 0x30));
        CHECK(machine.cpu().special(XT_SR_CCOUNT) == 800);
        CHECK(machine.elapsed_us() == 10);
        CHECK(machine.instructions() == 1); /* Idle time isn't guest execution. */
        CHECK(machine.step() == XTENSA_OK && machine.cpu().reg(2) == 23);
        CHECK(machine.elapsed_us() == 10);
        if (i == 1) {
            /* A masked level-1 CCOMPARE must not hide a later timer-group
             * alarm routed to an unmasked level-2 interrupt. */
            layout.images[0].segments[0].data.replace(0, 3, "\x00\x71\x00", 3); /* WAITI 1 */
            CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
            machine.cpu().set_special(XT_SR_VECBASE, image.entry_point);
            machine.cpu().set_special(XT_SR_PS, 0x40000);
            machine.cpu().set_special(XT_SR_INTENABLE, (1u << 6) | (1u << 19));
            machine.cpu().set_special(240, 100);
            CHECK(machine.write(0x3ff00104 + 14 * 4, 4, 19));
            CHECK(machine.write(0x3ff5f098, 4, 1));
            CHECK(machine.write(0x3ff5f010, 4, 400));
            CHECK(machine.write(0x3ff5f000, 4, 0xc0004c00)); /* enabled, up, /2, level, alarm */
            CHECK(machine.step() == XTENSA_OK);
            CHECK(machine.cpu().pc() == image.entry_point + 0x180);
            CHECK(machine.cpu().special(XT_SR_CCOUNT) == 800);
            CHECK(machine.instructions() == 1 && machine.elapsed_us() == 10);
            /* Long hardware deadlines cross CCOUNT's 32-bit wrap without
             * executing billions of instructions or losing sticky compares. */
            layout.images[0].segments[0].data.replace(0, 3, "\x00\x70\x00", 3);
            CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
            machine.cpu().set_special(XT_SR_VECBASE, image.entry_point);
            machine.cpu().set_special(XT_SR_PS, 0x40000);
            machine.cpu().set_special(XT_SR_INTENABLE, 1);
            CHECK(machine.write(0x3ff00104 + 14 * 4, 4, 0));
            CHECK(machine.write(0x3ff5f098, 4, 1));
            CHECK(machine.write(0x3ff5f010, 4, 16) && machine.write(0x3ff5f014, 4, 1));
            CHECK(machine.write(0x3ff5f000, 4, 0xc0004c00));
            CHECK(machine.step() == XTENSA_OK);
            CHECK(machine.cpu().special(XT_SR_CCOUNT) == 32);
            CHECK(machine.elapsed_us() == 0x200000020ull / 80 && machine.instructions() == 1);
        }
        CHECK(machine.load(Bytes(0x100000, (char)0xff), layout, false));
        CHECK(machine.step() == XTENSA_WAIT);
        CHECK(machine.step() == XTENSA_WAIT && machine.instructions() == 1);
        machine.cpu().set_special(XT_SR_VECBASE, image.entry_point);
        machine.cpu().set_special(XT_SR_PS, i ? 0x40000 : 0);
        machine.cpu().set_special(XT_SR_INTENABLE, 1u << 6);
        machine.cpu().set_irq(6);
        CHECK(machine.step() == XTENSA_OK && machine.cpu().reg(2) == 23);
    }
#undef CHECK
    return checks;
}
