/* Optional silicon ROM images are supplied separately from flash dumps.
 * Loading symbols or a writable ELF header is not a ROM implementation. */
#ifndef EMULATOR_ROM_IMAGE_H
#define EMULATOR_ROM_IMAGE_H
#include "common.h"

struct RomSegment {
    u32 address, physical_address;
    bool executable;
    bool initialize_ram;
    Bytes data;
    RomSegment() : address(0), physical_address(0), executable(false), initialize_ram(false) {}
};

/* Read-only PT_LOAD segments from an ELF32 little-endian executable.
 * Writable segments are deliberately excluded: Espressif ROM ELFs contain
 * RAM/BSS symbol sections and even ELF headers mapped at RAM addresses.
 * They are debugging metadata, not a power-on RAM snapshot. Explicit .data*
 * PROGBITS sections supply the ROM's initialized RAM variables separately. */
bool parse_elf_rom(const Bytes &bytes, u16 machine, std::vector<RomSegment> &segments,
                   std::string *error = 0);
#endif
