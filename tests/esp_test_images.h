#ifndef ESP_TEST_IMAGES_H
#define ESP_TEST_IMAGES_H
#include "common.h"

namespace esp_test {
/* Minimal single-segment images shared by machine and front-end tests. */
inline Bytes image(bool esp32,const Bytes &code,unsigned entry_offset)
{
    const u32 base = esp32 ? 0x40080000u : 0x40100000u;
    Bytes image(esp32 ? 24 : 8, '\0');
    image[0] = (char)0xe9; image[1] = 1;
    wr32le(&image[4], base + entry_offset);
    char word[4];
    wr32le(word, base); image.append(word, 4);
    wr32le(word, (u32)code.size()); image.append(word, 4);
    image += code;
    u8 checksum = 0xef;
    for (size_t i = 0; i < code.size(); ++i) checksum ^= (u8)code[i];
    image.append(15 - image.size() % 16, '\0');
    image += (char)checksum;
    return image;
}

/* A literal load and genuine Xtensa instructions drive UART MMIO, then
 * WAITI or loop forever. Both front ends use this identical firmware. */
inline Bytes uart_image(bool esp32, bool loop = false)
{
    Bytes code(4, '\0');
    wr32le(&code[0], esp32 ? 0x3ff40000u : 0x60000000u);
    code.append("\x31\xff\xff\x22\xa0\x41\x22\x63\x00\x00\x70\x00", 12);
    if (loop) code.replace(code.size() - 3, 3, "\x06\xff\xff", 3);
    return image(esp32,code,4);
}
}
#endif
