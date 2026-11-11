#include "esp/esp_machine.h"
#include "esp8266/esp8266_wdev.h"
#include "crypto_prims.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
struct MemoryRange { u32 base, size; bool executable; };
/* ESP8266EX technical reference; ESP-IDF components/soc/esp32/include/soc/soc.h. */
const MemoryRange esp8266_ram[] = {
    {0x3ffe8000u, 0x18000u, false}, {0x40100000u, 0x10000u, true},
    {0x60001000u, 0x400u, false}
};
const MemoryRange esp32_ram[] = {
    {0x3ffae000u, 0x52000u, false}, {0x40070000u, 0x50000u, true},
    {0x400c0000u, 0x2000u, true}, {0x50000000u, 0x2000u, true}
};
/* ESP32 SRAM1 is dual ported. Its DRAM and IRAM buses address words in
 * opposite order, while retaining byte order within a word. Keep the IRAM
 * view as the physical storage (ESP-IDF SOC_DIRAM_INVERTED). */
u32 esp32_dram_alias(u32 address)
{
    return address>=0x3ffe0000u && address<0x40000000u
        ? 0x400a0000u+(0x3ffffffcu-(address&~3u))+(address&3u) : address;
}

bool boot_remap_range(u32 address,unsigned size)
{
    return (address>=0x40000000u && address<0x40008000u) ||
        (address<0x400b8000u && (u64)address+size>0x400b0000u);
}
int esp32_flash_entry(u32 address)
{
    if (address >= 0x3f400000 && address < 0x3f800000)
        return (address & 0x3fffff) >> 16;
    if (address >= 0x400d0000 && address < 0x40400000)
        return 64 + ((address & 0x3fffff) >> 16);
    if (address >= 0x40400000 && address < 0x40800000)
        return 128 + ((address & 0x3fffff) >> 16);
    if (address >= 0x40800000 && address < 0x40c00000)
        return 192 + ((address & 0x3fffff) >> 16);
    return -1;
}
u32 esp_crc32_le_buf(u32 init, const u8 *buf, size_t len)
{
    u32 crc = ~init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}
u8 obk_crc8_unsigned(const u8 *data, size_t len)
{
    u8 crc = 0;
    for (size_t i = 0; i < len; ++i) {
        u8 extract = data[i];
        for (unsigned k = 0; k < 8; ++k) {
            u8 sum = (crc ^ extract) & 0x01u;
            crc >>= 1;
            if (sum) crc ^= 0x8cu;
            extract >>= 1;
        }
    }
    return crc;
}
bool has_obk_cfg_at(const Bytes &flash, size_t off)
{
    return off + 0x0e00 <= flash.size() &&
           flash[off] == 'C' && flash[off + 1] == 'F' && flash[off + 2] == 'G' &&
           rd32le((const u8 *)&flash[off + 4]) == 5u;
}
void init_esp_nvs_partition(Bytes &flash)
{
    if (flash.size() < 0x9000) return;
    u32 nvs_offset = 0, nvs_size = 0;
    for (size_t off = 0x8000; off + 32 <= 0x8c00 && off + 32 <= flash.size(); off += 32) {
        const u8 *ent = (const u8 *)&flash[off];
        u16 magic = rd16le(ent);
        if (magic == 0xebeb || magic == 0xffff) break;
        if (magic != 0x50aa) break;
        if (ent[2] == 1 && ent[3] == 2) {
            nvs_offset = rd32le(ent + 4);
            nvs_size = rd32le(ent + 8);
            break;
        }
    }
    if (!nvs_offset || nvs_size < 0x1000 || (u64)nvs_offset + nvs_size > flash.size())
        return;
    size_t cfg_off = (size_t)-1;
    const size_t candidates[] = { (size_t)nvs_offset, 0x1e1000u, 0x1d1000u, 0x1fc000u };
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (has_obk_cfg_at(flash, candidates[i])) {
            cfg_off = candidates[i];
            break;
        }
    }
    if (cfg_off != (size_t)-1) {
        Bytes cfg = flash.substr(cfg_off, 0x0e00);
        cfg[3] = (char)obk_crc8_unsigned((const u8 *)cfg.data() + 4, 0x0e00 - 4);
        memset(&flash[nvs_offset], 0xff, nvs_size);
        u8 *page = (u8 *)&flash[nvs_offset];
        static const u8 page_hdr[32] = {
            0xfe,0xff,0xff,0xff, 0x00,0x00,0x00,0x00, 0xfe,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0x84,0x2d,0xba,0xb9
        };
        static const u8 entry_ns[32] = {
            0x00,0x01,0x01,0xff, 0xf1,0x27,0xca,0xdf, 'c','o','n','f','i','g',0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0xff,0xff,0xff, 0xff,0xff,0xff,0xff
        };
        static const u8 entry_bootc[32] = {
            0x01,0x04,0x01,0xff, 0xa7,0x2f,0xae,0x22, 'b','o','o','t','c',0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0xff,0xff,0xff,0xff
        };
        static const u8 entry_blob_idx[32] = {
            0x01,0x48,0x01,0xff, 0xbb,0x3e,0x4f,0x7e, 'c','f','g',0x00,0x00,0x00,0x00,0x00,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x0e,0x00,0x00, 0x01,0x00,0xff,0xff
        };
        memcpy(page + 0x0000, page_hdr, 32);
        memset(page + 0x0020, 0xaa, 29);
        memcpy(page + 0x0040, entry_ns, 32);
        memcpy(page + 0x0060, entry_bootc, 32);
        u8 *blob_hdr = page + 0x0080;
        memset(blob_hdr, 0x00, 32);
        blob_hdr[0] = 0x01; /* ns = 1 */
        blob_hdr[1] = 0x42; /* NVS_TYPE_BLOB_DATA */
        blob_hdr[2] = 0x71; /* span = 113 entries (1 + 3584/32) */
        blob_hdr[3] = 0x00; /* chunk_index = 0 */
        blob_hdr[8] = 'c'; blob_hdr[9] = 'f'; blob_hdr[10] = 'g';
        blob_hdr[24] = 0x00; blob_hdr[25] = 0x0e; /* size = 0x0e00 */
        blob_hdr[26] = 0xff; blob_hdr[27] = 0xff;
        u32 data_crc = esp_crc32_le_buf(0xffffffffu, (const u8 *)cfg.data(), 0x0e00);
        wr32le(blob_hdr + 28, data_crc);
        u8 crc_input[28];
        memcpy(crc_input, blob_hdr, 4);
        memcpy(crc_input + 4, blob_hdr + 8, 24);
        u32 hdr_crc = esp_crc32_le_buf(0xffffffffu, crc_input, 28);
        wr32le(blob_hdr + 4, hdr_crc);
        memcpy(page + 0x00a0, cfg.data(), 0x0e00);
        memcpy(page + 0x0ea0, entry_blob_idx, 32);
    } else {
        bool all_zero = true;
        for (u32 i = 0; i < nvs_size; ++i) {
            if (flash[nvs_offset + i] != '\0') { all_zero = false; break; }
        }
        if (all_zero)
            memset(&flash[nvs_offset], 0xff, nvs_size);
    }
}
enum RomOp {
    ROM_COPY, ROM_SET, ROM_ZERO, ROM_COMPARE, ROM_LENGTH, ROM_STRCMP,
    ROM_STRNCMP, ROM_STRCPY, ROM_STRNCPY, ROM_PUTC, ROM_PRINTF,
    ROM_FLASH_READ, ROM_FLASH_WRITE, ROM_FLASH_ERASE, ROM_DELAY,
    ROM_CPU_FREQ, ROM_SET_FREQ, ROM_CACHE_ON, ROM_CACHE_OFF,
    ROM_UART_DIV, ROM_GET_CCOUNT, ROM_SRAND, ROM_RAND, ROM_RESET_REASON,
    ROM_UDIV, ROM_UMOD, ROM_SDIV, ROM_INSTALL_PUTC, ROM_UART_PRINTF,
    ROM_FLASH_READ_DATA,
    ROM_FLOAT_SI_SF, ROM_FLOAT_UNS_SF, ROM_FIX_UNS_SF_SI,
    ROM_ADD_SF, ROM_SUB_SF, ROM_MUL_SF, ROM_STRSTR, ROM_UMULSIDI3,
    ROM_FLOAT_SI_DF, ROM_FLOAT_UNS_DF, ROM_FIX_DF_SI, ROM_FIX_UNS_DF_SI,
    ROM_EXTEND_SF_DF, ROM_TRUNC_DF_SF, ROM_ADD_DF, ROM_SUB_DF, ROM_MUL_DF, ROM_DIV_DF,
    ROM_MUL_DI, ROM_DIV_DI, ROM_UDIV_DI, ROM_UMOD_DI,
    ROM_SMOD, ROM_CLZSI2, ROM_ASHLDI3, ROM_ASHRDI3, ROM_LSHRDI3,
    ROM_MD5_INIT, ROM_MD5_UPDATE, ROM_MD5_FINAL,
    ROM_SHA_ENABLE, ROM_SHA_DISABLE,
    ROM_CRC32_LE, ROM_CRC32_BE,
    ROM_LLDESC_BUILD_CHAIN, ROM_LLDESC_SET_OWNER, ROM_LLDESC_NUM2LINK,
    ROM_SPI_IDLE, ROM_SPI_READ_STATUS, ROM_SPI_WRITE_STATUS,
    ROM_SPI_WRITE_ENABLE, ROM_SPI_NOOP, ROM_TRUE, ROM_BSWAP32, ROM_BSWAP64,
    ROM_CACHE_MMU_SET, ROM_CACHE_SRAM_MMU_SET, ROM_MMU_INIT, ROM_PHY_GET_ROMFUNCS, ROM_FLASH_ERASE_DATA,
    ROM_FLASH_WRITE_DATA, ROM_IO_VPRINTF, ROM_SET_INTLEVEL, ROM_RESTORE_INTLEVEL,
    ROM_SET_APPCPU_BOOT_ADDR, ROM_INTR_MATRIX_SET, ROM_QSORT,
    ROM_DIV_T, ROM_ITOA, ROM_SINIT, ROM_SWBUF_R, ROM_FFLUSH_R,
    ROM_MEMCHR, ROM_STRCHR, ROM_STRRCHR, ROM_STRCAT, ROM_STRNCAT, ROM_STRCSPN, ROM_STRLCPY,
    ROM_STRNLEN, ROM_STRSPN, ROM_STRCASESTR, ROM_STRDUP, ROM_SCCL,
    ROM_ATOI, ROM_STRTOL_R, ROM_STRTOUL_R, ROM_MOD_DI, ROM_CTZDI2,
    ROM_POPCOUNTSI2, ROM_ROUNDUP2, ROM_CRC8, ROM_UART_RX_ONE_CHAR,
    ROM_SOFTWARE_RESET, ROM_FLOAT_DI_SF, ROM_FLOAT_UND_SF, ROM_FLOAT_DI_DF,
    ROM_FIX_DF_DI, ROM_CMP_DF_LT, ROM_CMP_DF_GT, ROM_UNORD_DF,
    ROM_SETJMP, ROM_LONGJMP, ROM_ASCII_WCTOMB, ROM_SYSCALL_STUB,
    ROM_SFP, ROM_SFVWRITE_R, ROM_SWRITE, ROM_SREFILL_R, ROM_LOCALECONV_R,
    ROM_STRCASECMP, ROM_STRNCASECMP, ROM_STRLCAT, ROM_STRTOL, ROM_STRTOUL, ROM_NEGSF2,
    ROM_PHY_PLL_CAP, ROM_PHY_FREQ2CHAN
};
struct RomEntry { u32 address; RomOp operation; };
/* Addresses from Espressif's eagle.rom.addr.v6.ld and esp32.rom*.ld.
 * https://github.com/espressif/ESP8266_NONOS_SDK/blob/master/ld/eagle.rom.addr.v6.ld
 * https://github.com/espressif/esp-idf/tree/v4.4/components/esp_rom/esp32/ld
 */
const RomEntry esp8266_rom[] = {
    {0x400018b4,ROM_COPY},{0x400018c4,ROM_COPY},{0x4000df48,ROM_COPY},
    {0x4000e04c,ROM_COPY},{0x400006c4,ROM_COPY},
    {0x400018a4,ROM_SET},{0x4000e190,ROM_SET},
    {0x40002ae8,ROM_ZERO},{0x4000de84,ROM_ZERO},
    {0x400018d4,ROM_COMPARE},{0x4000dea8,ROM_COMPARE},
    {0x40002ac8,ROM_LENGTH},{0x4000bf4c,ROM_LENGTH},
    {0x40002aa8,ROM_STRCMP},{0x4000bdc8,ROM_STRCMP},
    {0x40002ab8,ROM_STRNCMP},{0x4000bfa8,ROM_STRNCMP},
    {0x40002a88,ROM_STRCPY},{0x4000bec8,ROM_STRCPY},
    {0x40002a98,ROM_STRNCPY},{0x4000c0a0,ROM_STRNCPY},
    {0x40003b30,ROM_PUTC},{0x40002be8,ROM_PUTC},
    {0x40001da0,ROM_PUTC}, /* ets_write_char */
    {0x400024cc,ROM_PRINTF},{0x40002544,ROM_PRINTF},
    {0x4000242c,ROM_INSTALL_PUTC},{0x40002438,ROM_UART_PRINTF},
    {0x40004b1c,ROM_FLASH_READ},{0x40004a4c,ROM_FLASH_WRITE},
    {0x400042ac,ROM_FLASH_READ_DATA},
    {0x40004a00,ROM_FLASH_ERASE},{0x40002ecc,ROM_DELAY},
    {0x40002f0c,ROM_CPU_FREQ},{0x40002f04,ROM_SET_FREQ},
    {0x40004678,ROM_CACHE_ON},{0x400047f0,ROM_CACHE_OFF},
    {0x4000dd38,ROM_GET_CCOUNT},{0x400005f0,ROM_SRAND},
    {0x40000600,ROM_RAND},{0x4000e21c,ROM_UDIV},
    {0x4000e268,ROM_UMOD},{0x4000dc88,ROM_SDIV},
    {0x4000e2ac,ROM_FLOAT_SI_SF},{0x4000e2a4,ROM_FLOAT_UNS_SF},
    {0x4000c4c4,ROM_FIX_UNS_SF_SI},{0x4000c180,ROM_ADD_SF},
    {0x4000c268,ROM_SUB_SF},{0x4000c3dc,ROM_MUL_SF},
    {0x4000e1e0,ROM_STRSTR},{0x40002ad8,ROM_STRSTR},{0x4000dcf0,ROM_UMULSIDI3},
    {0x4000e2f0,ROM_FLOAT_SI_DF},{0x4000e2e8,ROM_FLOAT_UNS_DF},
    {0x4000ccb8,ROM_FIX_DF_SI},{0x4000cd00,ROM_FIX_UNS_DF_SI},
    {0x4000cdfc,ROM_EXTEND_SF_DF},{0x4000cd5c,ROM_TRUNC_DF_SF},
    {0x4000c538,ROM_ADD_DF},{0x4000c688,ROM_SUB_DF},
    {0x4000c8f0,ROM_MUL_DF},{0x4000cb94,ROM_DIV_DF},
    {0x40000650,ROM_MUL_DI},{0x4000ce60,ROM_DIV_DI},
    {0x4000d310,ROM_UDIV_DI},{0x4000d770,ROM_UMOD_DI},
    {0x40004f40,ROM_LLDESC_BUILD_CHAIN},
    {0x4000507c,ROM_LLDESC_SET_OWNER},
    {0x40005050,ROM_LLDESC_NUM2LINK},
    {0x4000448c,ROM_SPI_IDLE},
    {0x4000443c,ROM_SPI_WRITE_ENABLE},
    {0x400043c8,ROM_SPI_READ_STATUS},
    {0x40004400,ROM_SPI_WRITE_STATUS},
    {0x400044c0,ROM_SPI_NOOP},
    {0x400048a8,ROM_SPI_NOOP},
    {0x40004878,ROM_SPI_NOOP},
    {0x40004644,ROM_SPI_NOOP},
    {0x40004c2c,ROM_SPI_NOOP},
    {0x400048ec,ROM_SPI_NOOP},
    {0x40003f58,ROM_SPI_NOOP},
    {0x40000454,ROM_SPI_NOOP}, /* _xtos_set_exception_handler */
    {0x40000f74,ROM_SPI_NOOP}, /* ets_intr_lock */
    {0x40000f80,ROM_SPI_NOOP}, /* ets_intr_unlock */
    {0x40000f88,ROM_SPI_NOOP}, /* ets_isr_attach */
    {0x40000f98,ROM_SPI_NOOP}, /* ets_isr_mask */
    {0x40000fa8,ROM_SPI_NOOP}, /* ets_isr_unmask */
    {0x40000dd0,ROM_TRUE},     /* ets_task */
    {0x40000e24,ROM_TRUE},     /* ets_post */
    {0x40000e04,ROM_SPI_NOOP}, /* ets_run */
    {0x40000dc0,ROM_SPI_NOOP}, /* ets_set_idle_cb */
    {0x40000fbc,ROM_SPI_NOOP}, /* ets_set_user_start */
    {0x40002cc4,ROM_SPI_NOOP}, /* ets_timer_arm */
    {0x40002d40,ROM_SPI_NOOP}, /* ets_timer_disarm */
    {0x40002d80,ROM_SPI_NOOP}, /* ets_timer_done */
    {0x40002e68,ROM_SPI_NOOP}, /* ets_timer_init */
    {0x40002c48,ROM_SPI_NOOP}, /* ets_timer_setfn */
    {0x400030f0,ROM_SPI_NOOP}, /* ets_wdt_disable */
    {0x40002fa0,ROM_SPI_NOOP}, /* ets_wdt_enable */
    {0x40003170,ROM_SPI_NOOP}, /* ets_wdt_init */
    {0x40003158,ROM_SPI_NOOP}, /* ets_wdt_restore */
    {0x40002a40,ROM_SPI_NOOP}, /* ets_rtc_int_register */
    {0x400025e0,ROM_RESET_REASON}, /* rtc_get_reset_reason */
    {0x40006b08,ROM_PHY_GET_ROMFUNCS}, /* phy_get_romfuncs */
    {0x400040c0,ROM_FLASH_ERASE_DATA},
    {0x40004174,ROM_FLASH_WRITE_DATA},
    {0x40001f00,ROM_IO_VPRINTF}
};
const RomEntry esp32_rom[] = {
    {0x4000c2c8,ROM_COPY},{0x4000c3c0,ROM_COPY},{0x4000c44c,ROM_SET},
    {0x4000c1f4,ROM_ZERO},{0x4000c260,ROM_COMPARE},
    {0x400014c0,ROM_LENGTH},{0x40001274,ROM_STRCMP},
    {0x4000c5f4,ROM_STRNCMP},{0x400013ac,ROM_STRCPY},
    {0x400015d4,ROM_STRNCPY},{0x40009200,ROM_PUTC},
    {0x4000922c,ROM_PUTC},{0x40007cf8,ROM_PUTC},
    {0x40007d54,ROM_PRINTF},{0x40008534,ROM_DELAY},
    {0x4000855c,ROM_CPU_FREQ},{0x40008550,ROM_SET_FREQ},
    {0x40009a84,ROM_CACHE_ON},{0x40009ab8,ROM_CACHE_OFF},
    {0x400090cc,ROM_UART_DIV},{0x400081d4,ROM_RESET_REASON},
    {0x40001004,ROM_SRAND},{0x40001058,ROM_RAND},
    {0x400041a4,ROM_SPI_NOOP}, /* rom_i2c_writeReg */
    {0x40004148,ROM_SPI_NOOP}, /* rom_i2c_readReg */
    {0x40009a14,ROM_SPI_NOOP}, /* Cache_Flush_rom */
    {0x400097f4,ROM_CACHE_SRAM_MMU_SET}, /* cache_sram_mmu_set_rom */
    {0x400095a4,ROM_MMU_INIT}, /* mmu_init */
    {0x400095e0,ROM_CACHE_MMU_SET}, /* cache_flash_mmu_set_rom */
    {0x40009950,ROM_SPI_NOOP}, /* Cache_Read_Init_rom */
    {0x40008658,ROM_SPI_NOOP}, /* ets_efuse_get_spiconfig */
    {0x40008600,ROM_SPI_NOOP}, /* ets_efuse_read_op */
    {0x40008628,ROM_SPI_NOOP}, /* ets_efuse_program_op */
    {0x40008710,ROM_SPI_NOOP}, /* ets_efuse_get_8M_clock */
    {0x40008588,ROM_CPU_FREQ}, /* ets_get_detected_xtal_freq */
    {0x40009c20,ROM_SPI_NOOP}, /* gpio_init */
    {0x40009edc,ROM_SPI_NOOP}, /* rom_gpio_matrix_in */
    {0x40009f0c,ROM_SPI_NOOP}, /* rom_gpio_matrix_out */
    {0x40009fdc,ROM_SPI_NOOP}, /* rom_gpio_pad_select_gpio */
    {0x40009b24,ROM_SPI_NOOP}, /* rom_gpio_output_set */
    {0x40009b5c,ROM_SPI_NOOP}, /* rom_gpio_output_set_high */
    {0x40009b88,ROM_SPI_NOOP}, /* rom_gpio_input_get */
    {0x40009b9c,ROM_SPI_NOOP}, /* rom_gpio_input_get_high */
    {0x40009dd4,ROM_SPI_NOOP}, /* gpio_intr_ack */
    {0x40009e1c,ROM_SPI_NOOP}, /* gpio_intr_ack_high */
    {0x40009e6c,ROM_SPI_NOOP}, /* gpio_intr_handler_register */
    {0x40009cec,ROM_SPI_NOOP}, /* gpio_intr_pending */
    {0x40009cf8,ROM_SPI_NOOP}, /* gpio_intr_pending_high */
    {0x40009d04,ROM_SPI_NOOP}, /* gpio_pin_intr_state_set */
    {0x40009eb0,ROM_SPI_NOOP}, /* gpio_pin_wakeup_disable */
    {0x40009e7c,ROM_SPI_NOOP}, /* gpio_pin_wakeup_enable */
    {0x40009cbc,ROM_SPI_NOOP}, /* gpio_register_get */
    {0x40009bbc,ROM_SPI_NOOP}, /* gpio_register_set */
    {0x40008fd0,ROM_SPI_NOOP}, /* uartAttach */
    {0x40009028,ROM_SPI_NOOP}, /* uart_tx_switch */
    {0x40009034,ROM_SPI_NOOP}, /* uart_baudrate_detect */
    {0x40009120,ROM_SPI_NOOP}, /* Uart_Init */
    {0x40009258,ROM_SPI_NOOP}, /* uart_tx_flush */
    {0x40007d18,ROM_INSTALL_PUTC}, /* ets_install_putc1 */
    {0x40007d28,ROM_UART_PRINTF},  /* ets_install_uart_printf */
    {0x40007d38,ROM_INSTALL_PUTC}, /* ets_install_putc2 */
    {0x4000cff8,ROM_UDIV_DI},   /* __udivdi3 */
    {0x4000d280,ROM_UMOD_DI},   /* __umoddi3 */
    {0x4000ca84,ROM_DIV_DI},    /* __divdi3 */
    {0x4000c9fc,ROM_MUL_DI},    /* __muldi3 */
    {0x4000c7b8,ROM_SDIV},      /* __divsi3 */
    {0x4000c7c0,ROM_SMOD},      /* __modsi3 */
    {0x4000c7c8,ROM_UDIV},      /* __udivsi3 */
    {0x4000c7d0,ROM_UMOD},      /* __umodsi3 */
    {0x4000c7d8,ROM_UMULSIDI3}, /* __umulsidi3 */
    {0x4000c7e8,ROM_CLZSI2},    /* __clzsi2 */
    {0x4000c818,ROM_ASHLDI3},   /* __ashldi3 */
    {0x4000c830,ROM_ASHRDI3},   /* __ashrdi3 */
    {0x4000c84c,ROM_LSHRDI3},   /* __lshrdi3 */
    {0x4000c870,ROM_FLOAT_SI_SF},
    {0x4000c864,ROM_FLOAT_UNS_SF},
    {0x4000c944,ROM_FLOAT_SI_DF},
    {0x4000c938,ROM_FLOAT_UNS_DF},
    {0x40002a78,ROM_FIX_DF_SI},
    {0x40002b30,ROM_FIX_UNS_DF_SI},
    {0x40002c34,ROM_EXTEND_SF_DF},
    {0x40002b90,ROM_TRUNC_DF_SF},
    {0x400020e8,ROM_ADD_SF},
    {0x400021d0,ROM_SUB_SF},
    {0x400632c8,ROM_MUL_SF},
    {0x40002590,ROM_ADD_DF},
    {0x400026e4,ROM_SUB_DF},
    {0x4006358c,ROM_MUL_DF},
    {0x40002954,ROM_DIV_DF},
    {0x40064ae0,ROM_BSWAP32},   /* __bswapsi2 */
    {0x40064b08,ROM_BSWAP64},   /* __bswapdi2 */
    {0x400621b0,ROM_SPI_READ_STATUS}, /* esp_rom_spiflash_read_user_cmd */
    {0x40062320,ROM_SPI_WRITE_ENABLE}, /* SPI_write_enable */
    {0x40062a6c,ROM_SPI_NOOP}, /* esp_rom_spiflash_attach */
    {0x40062bc8,ROM_SPI_NOOP}, /* esp_rom_spiflash_config_clk */
    {0x40063238,ROM_SPI_NOOP}, /* esp_rom_spiflash_config_param */
    {0x40061ddc,ROM_SPI_NOOP}, /* esp_rom_spiflash_select_qio_pins */
    {0x4005c07c,ROM_SHA_ENABLE},  /* ets_sha_enable */
    {0x4005c0a8,ROM_SHA_DISABLE}, /* ets_sha_disable */
    {0x4005da7c,ROM_MD5_INIT},   /* MD5Init */
    {0x4005da9c,ROM_MD5_UPDATE}, /* MD5Update */
    {0x4005db1c,ROM_MD5_FINAL},  /* MD5Final */
    {0x4005cfec,ROM_CRC32_LE},   /* crc32_le */
    {0x4005d024,ROM_CRC32_BE},   /* crc32_be */
    {0x4000bfdc,ROM_SET_INTLEVEL},     /* _xtos_set_intlevel */
    {0x40000928,ROM_RESTORE_INTLEVEL}, /* _xtos_restore_intlevel */
    {0x4000689c,ROM_SET_APPCPU_BOOT_ADDR}, /* ets_set_appcpu_boot_addr */
    {0x4000681c,ROM_INTR_MATRIX_SET},      /* intr_matrix_set */
    {0x40056424,ROM_QSORT},                /* qsort */
    {0x40001e38,ROM_SINIT},                /* __sinit */
    {0x40056348,ROM_DIV_T},                /* div */
    {0x40056378,ROM_DIV_T},                /* ldiv */
    {0x400566b4,ROM_ITOA},                 /* itoa */
    {0x40058bec,ROM_SWBUF_R},              /* __swbuf_r */
    {0x40058f3c,ROM_SPI_NOOP},             /* __submore */
    {0x40059320,ROM_FFLUSH_R},             /* _fflush_r */
    {0x40001fec,ROM_SPI_NOOP},             /* _fclose_r */
    {0x40001148,ROM_SPI_NOOP},             /* __seofread */
    {0x40001778,ROM_SPI_NOOP},             /* close */
    {0x4000c244,ROM_MEMCHR},               /* memchr */
    {0x4000c53c,ROM_STRCHR},               /* strchr */
    {0x40001708,ROM_STRRCHR},              /* strrchr */
    {0x4000c518,ROM_STRCAT},               /* strcat */
    {0x4000c558,ROM_STRCSPN},              /* strcspn */
    {0x4000c584,ROM_STRLCPY},              /* strlcpy */
    {0x4000c5c4,ROM_STRNCAT},              /* strncat */
    {0x4000c628,ROM_STRNLEN},              /* strnlen */
    {0x4000c648,ROM_STRSPN},               /* strspn */
    {0x4000c674,ROM_STRSTR},               /* strstr */
    {0x40001210,ROM_STRCASESTR},           /* strcasestr */
    {0x4000143c,ROM_STRDUP},               /* strdup */
    {0x4000c498,ROM_SCCL},                 /* __sccl */
    {0x400566c4,ROM_ATOI},                 /* atoi */
    {0x400566ec,ROM_ATOI},                 /* atol */
    {0x40056714,ROM_STRTOL_R},             /* _strtol_r */
    {0x40056834,ROM_STRTOUL_R},            /* _strtoul_r */
    {0x4000cd4c,ROM_MOD_DI},               /* __moddi3 */
    {0x4000ca64,ROM_CTZDI2},               /* __ctzdi2 */
    {0x40002ed0,ROM_POPCOUNTSI2},          /* __popcountsi2 */
    {0x4000ab7c,ROM_ROUNDUP2},             /* roundup2 */
    {0x4005d144,ROM_CRC8},                 /* esp_crc8 */
    {0x400092d0,ROM_UART_RX_ONE_CHAR},     /* uart_rx_one_char */
    {0x40008208,ROM_SPI_NOOP},             /* set_rtc_memory_crc */
    {0x4000824c,ROM_SOFTWARE_RESET},       /* software_reset */
    {0x40008264,ROM_SOFTWARE_RESET},       /* software_reset_cpu */
    {0x40056268,ROM_SETJMP},               /* setjmp */
    {0x400562cc,ROM_LONGJMP},              /* longjmp */
    {0x400562e0,ROM_LONGJMP},              /* longjmp + 0x14 (after entry/windowstart) */
    {0x40058ef0,ROM_ASCII_WCTOMB},         /* __ascii_wctomb */
    {0x4000bbb4,ROM_SYSCALL_STUB},         /* _malloc_r */
    {0x4000bbcc,ROM_SYSCALL_STUB},         /* _free_r */
    {0x4000bbe0,ROM_SYSCALL_STUB},         /* _realloc_r */
    {0x4000bbf8,ROM_SYSCALL_STUB},         /* _calloc_r */
    {0x4000bdc4,ROM_SYSCALL_STUB},         /* _lock_init */
    {0x4000bdd8,ROM_SYSCALL_STUB},         /* _lock_init_recursive */
    {0x4000bdec,ROM_SYSCALL_STUB},         /* _lock_close */
    {0x4000be00,ROM_SYSCALL_STUB},         /* _lock_close_recursive */
    {0x4000be14,ROM_SYSCALL_STUB},         /* _lock_acquire */
    {0x4000be28,ROM_SYSCALL_STUB},         /* _lock_acquire_recursive */
    {0x4000be3c,ROM_SYSCALL_STUB},         /* _lock_try_acquire */
    {0x4000be50,ROM_SYSCALL_STUB},         /* _lock_try_acquire_recursive */
    {0x4000be64,ROM_SYSCALL_STUB},         /* _lock_release */
    {0x4000be78,ROM_SYSCALL_STUB},         /* _lock_release_recursive */
    {0x4000be8c,ROM_SYSCALL_STUB},         /* __getreent */
    {0x4000bea0,ROM_SYSCALL_STUB},         /* malloc */
    {0x4000beb8,ROM_SYSCALL_STUB},         /* free */
    {0x4000becc,ROM_SYSCALL_STUB},         /* realloc */
    {0x4000bee4,ROM_SYSCALL_STUB},         /* calloc */
    {0x4000bc10,ROM_SYSCALL_STUB},         /* _system_r */
    {0x4000bc28,ROM_SYSCALL_STUB},         /* _rename_r */
    {0x4000bc40,ROM_SYSCALL_STUB},         /* _times_r */
    {0x4000bc58,ROM_SYSCALL_STUB},         /* _gettimeofday_r */
    {0x4000bc70,ROM_SYSCALL_STUB},         /* _raise_r */
    {0x4000bc84,ROM_SYSCALL_STUB},         /* _unlink_r */
    {0x4000bc9c,ROM_SYSCALL_STUB},         /* _link_r */
    {0x4000bcb4,ROM_SYSCALL_STUB},         /* _stat_r */
    {0x4000bccc,ROM_SYSCALL_STUB},         /* _fstat_r */
    {0x4000bce4,ROM_SYSCALL_STUB},         /* _sbrk_r */
    {0x4000bcfc,ROM_SYSCALL_STUB},         /* _getpid_r */
    {0x4000bd10,ROM_SYSCALL_STUB},         /* _kill_r */
    {0x4000bd28,ROM_SYSCALL_STUB},         /* _exit_r */
    {0x4000bd3c,ROM_SYSCALL_STUB},         /* _close_r */
    {0x4000bd54,ROM_SYSCALL_STUB},         /* _open_r */
    {0x4000bd70,ROM_SYSCALL_STUB},         /* _write_r */
    {0x4000bd8c,ROM_SYSCALL_STUB},         /* _lseek_r */
    {0x4000bda8,ROM_SYSCALL_STUB},         /* _read_r */
    {0x40001e90,ROM_SFP},                  /* __sfp */
    {0x40001e08,ROM_SPI_NOOP},             /* __sfp_lock_acquire */
    {0x40001e14,ROM_SPI_NOOP},             /* __sfp_lock_release */
    {0x40001d48,ROM_SPI_NOOP},             /* _cleanup_r */
    {0x40001118,ROM_SPI_NOOP},             /* __sread */
    {0x40001150,ROM_SWRITE},               /* __swrite */
    {0x40001184,ROM_SPI_NOOP},             /* __sseek */
    {0x400011b8,ROM_SPI_NOOP},             /* __sclose */
    {0x400020ac,ROM_SPI_NOOP},             /* fclose */
    {0x4005893c,ROM_SFVWRITE_R},           /* __sfvwrite_r */
    {0x40058cc8,ROM_SPI_NOOP},             /* __swsetup_r */
    {0x40059108,ROM_SPI_NOOP},             /* __smakebuf_r */
    {0x40059394,ROM_SPI_NOOP},             /* fflush */
    {0x400593d4,ROM_SREFILL_R},            /* __srefill_r */
    {0x40059560,ROM_LOCALECONV_R},         /* _localeconv_r */
    {0x400011cc,ROM_STRCASECMP},           /* strcasecmp */
    {0x40001550,ROM_STRNCASECMP},          /* strncasecmp */
    {0x40001470,ROM_STRLCAT},              /* strlcat */
    {0x4005681c,ROM_STRTOL},               /* strtol */
    {0x4005692c,ROM_STRTOUL},              /* strtoul */
    {0x400020c0,ROM_NEGSF2},               /* __negsf2 */
    {0x40014470,ROM_SPI_NOOP},             /* r_dbg_swdiag_init */
    {0x40004100,ROM_PHY_GET_ROMFUNCS},     /* phy_get_romfuncs */
    {0x400044c0,ROM_SPI_NOOP},             /* default phy stub */
    {0x400044c4,ROM_PHY_PLL_CAP},          /* rom phy pll lock status */
    {0x400044c8,ROM_PHY_FREQ2CHAN},        /* rom phy_freq2chan */
    {0x4000c8b0,ROM_FLOAT_UND_SF},         /* __floatundisf */
    {0x4000c8c0,ROM_FLOAT_DI_SF},          /* __floatdisf */
    {0x4000c988,ROM_FLOAT_DI_DF},          /* __floatdidf */
    {0x40002ac4,ROM_FIX_DF_DI},            /* __fixdfdi */
    {0x400636a8,ROM_CMP_DF_LT},            /* __eqdf2 / __nedf2 */
    {0x400636dc,ROM_CMP_DF_GT},            /* __gtdf2 */
    {0x40063704,ROM_CMP_DF_LT},            /* __ledf2 */
    {0x40063768,ROM_CMP_DF_GT},            /* __gedf2 */
    {0x40063790,ROM_CMP_DF_LT},            /* __ltdf2 */
    {0x400637f4,ROM_UNORD_DF}              /* __unorddf2 */
};
}

EspMachine::EspMachine(esp::Chip chip)
    : chip_(chip), cpu_(*this, chip == esp::CHIP_ESP32), cpu1_(*this, true), interrupts_(cpu_), frc_timers_(cpu_), wifi_dma_(*this),
      active_core_(0), next_core_(0), app_reset_(true), reset_domain_(RESET_NONE), reset_mask_(0), reset_reason_(0), reset_count_(0), rtc_epoch_us_(0), retired_cpu_instructions_(0), gpio_levels_(0), putc1_(0), rom_calls_(0),
      delay_us_(0), clock_origin_instructions_(0), clock_skipped_cycles_(0), apb_origin_ticks_(0), apb_origin_delay_us_(0), rtc_wake_us_(0), rtc_calibration_us_(0), esp8266_timer_check_ticks_(0), cpu_mhz_(80), xtal_mhz_(chip==esp::CHIP_ESP8266?26:40), flash_window_(0), random_(1),
      cache_enabled_(true), stopped_(XTENSA_OK)
{
    initialize_memory();
    cpu1_.set_processor_id(0xabab);
    for(unsigned i=0;i<3;++i) uarts_[i].reset(chip_==esp::CHIP_ESP32);
}

bool EspMachine::set_xtal_frequency(unsigned mhz)
{
    if(mhz!=26 && mhz!=40 && !(chip_==esp::CHIP_ESP8266 && mhz==24))return false;
    xtal_mhz_=mhz;
    return true;
}

bool EspMachine::set_psram_size(unsigned bytes)
{
    if(bytes && (chip_!=esp::CHIP_ESP32 || (bytes!=0x200000 && bytes!=0x400000 && bytes!=0x800000)))
        return false;
    psram_.assign(bytes,'\0');
    return true;
}

void EspMachine::initialize_memory()
{
    regions_.clear();
    const MemoryRange *ranges = chip_ == esp::CHIP_ESP32 ? esp32_ram : esp8266_ram;
    size_t n = chip_ == esp::CHIP_ESP32 ? sizeof(esp32_ram)/sizeof(esp32_ram[0])
                                      : sizeof(esp8266_ram)/sizeof(esp8266_ram[0]);
    for (size_t i=0; i<n; ++i)
        regions_.push_back(Region(ranges[i].base, ranges[i].size, true, ranges[i].executable,
            chip_==esp::CHIP_ESP32 && ranges[i].base==0x400c0000?1:3)); /* RTC FAST: PRO only */
    if (chip_ == esp::CHIP_ESP32) {
        /* Default Xtensa window overflow and underflow exception vectors in mask ROM. */
        Region rom_vectors(0x40000000, 0x400, false, true, 3);
        rom_vectors.bytes.assign(0x400, '\0');
        static const u8 ovf4[] = { 0x00,0xc5,0x49, 0x10,0xd5,0x49, 0x20,0xe5,0x49, 0x30,0xf5,0x49, 0x00,0x34,0x00 };
        static const u8 unf4[] = { 0x00,0xc5,0x09, 0x10,0xd5,0x09, 0x20,0xe5,0x09, 0x30,0xf5,0x09, 0x00,0x35,0x00 };
        static const u8 ovf8[] = { 0x00,0xc9,0x49, 0x00,0xd1,0x09, 0x10,0xd9,0x49, 0x20,0xe9,0x49, 0x30,0xf9,0x49,
                                   0x40,0x80,0x49, 0x50,0x90,0x49, 0x60,0xa0,0x49, 0x70,0xb0,0x49, 0x00,0x34,0x00 };
        static const u8 unf8[] = { 0x00,0xc9,0x09, 0x10,0xd9,0x09, 0x20,0xe9,0x09, 0x30,0xf9,0x09, 0x70,0xd1,0x09,
                                   0x40,0x87,0x09, 0x50,0x97,0x09, 0x60,0xa7,0x09, 0x70,0xb7,0x09, 0x00,0x35,0x00 };
        static const u8 ovf12[] = { 0x00,0xcd,0x49, 0x00,0xd1,0x09, 0x10,0xdd,0x49, 0x20,0xed,0x49, 0x30,0xfd,0x49,
                                    0x40,0x80,0x49, 0x50,0x90,0x49, 0x60,0xa0,0x49, 0x70,0xb0,0x49,
                                    0x80,0xc0,0x49, 0x90,0xd0,0x49, 0xa0,0xe0,0x49, 0xb0,0xf0,0x49, 0x00,0x34,0x00 };
        static const u8 unf12[] = { 0x00,0xcd,0x09, 0x10,0xdd,0x09, 0x20,0xed,0x09, 0x30,0xfd,0x09, 0xb0,0xd1,0x09,
                                    0x40,0x8b,0x09, 0x50,0x9b,0x09, 0x60,0xab,0x09, 0x70,0xbb,0x09,
                                    0x80,0xcb,0x09, 0x90,0xdb,0x09, 0xa0,0xeb,0x09, 0xb0,0xfb,0x09, 0x00,0x35,0x00 };
        memcpy(&rom_vectors.bytes[0x000], ovf4, sizeof(ovf4));
        memcpy(&rom_vectors.bytes[0x040], unf4, sizeof(unf4));
        memcpy(&rom_vectors.bytes[0x080], ovf8, sizeof(ovf8));
        memcpy(&rom_vectors.bytes[0x0c0], unf8, sizeof(unf8));
        memcpy(&rom_vectors.bytes[0x100], ovf12, sizeof(ovf12));
        memcpy(&rom_vectors.bytes[0x140], unf12, sizeof(unf12));
        static const u8 rom_strdup[] = {
            0xc0,0x14,0x00,0x40, 0x20,0xe0,0xfa,0x3f, 0xc8,0xc2,0x00,0x40,
            0x36,0x41,0x00, 0xad,0x02, 0x81,0xfb,0xff, 0xe0,0x08,0x00,
            0x32,0xca,0x01, 0x91,0xfa,0xff, 0x98,0x09, 0x88,0x19,
            0x0c,0x0a, 0xbd,0x03, 0xe0,0x08,0x00, 0xac,0x88,
            0xbd,0x02, 0xcd,0x03, 0x81,0xf6,0xff, 0xe0,0x08,0x00,
            0x2d,0x0a, 0x1d,0xf0
        };
        memcpy(&rom_vectors.bytes[0x200], rom_strdup, sizeof(rom_strdup));
        regions_.push_back(rom_vectors);
    }
}

EspMachine::Region *EspMachine::region(u32 address, unsigned size)
{
    if (chip_ == esp::CHIP_ESP32 && address >= 0x3ff80000 && address < 0x3ff82000)
        address = 0x400c0000 + (address - 0x3ff80000);
    for (size_t i=0; i<regions_.size(); ++i) {
        Region &r = regions_[i];
        if ((r.core_mask & (1u << active_core_)) && address >= r.base &&
            (u64)address + size <= (u64)r.base + r.bytes.size())
            return &r;
    }
    return 0;
}

bool EspMachine::load_segment(const esp::Segment &s, std::string *error)
{
    if (!s.address || s.data.empty()) return true;
    if (s.data.size() > 0x1000000u || (u64)s.address+s.data.size() > 0x100000000ull) {
        if (error) *error = "ESP segment exceeds address space";
        return false;
    }
    if (s.mapped) {
        /* LX106 has a single banked flash window, not independent segment
         * mappings. Read it from flash_ so writes and bank selection stay
         * coherent; the parser's file offset determines its virtual address. */
        if(chip_==esp::CHIP_ESP8266) return true;
        Region mapped(s.address, s.data.size(), false,
                      chip_ == esp::CHIP_ESP8266 || s.address >= 0x400d0000);
        mapped.bytes = s.data;
        mapped.flash_mapped = true;
        regions_.push_back(mapped);
        return true;
    }
    Region *r = region(s.address, (unsigned)s.data.size());
    if (!r || !r->writable) {
        if (error) *error = strf("ESP segment at 0x%08x is outside chip RAM", s.address);
        return false;
    }
    u32 addr = s.address;
    if (chip_ == esp::CHIP_ESP32 && addr >= 0x3ff80000 && addr < 0x3ff82000)
        addr = 0x400c0000 + (addr - 0x3ff80000);
    if(chip_==esp::CHIP_ESP32 && addr<0x40000000u && (u64)addr+s.data.size()>0x3ffe0000u) {
        for(size_t i=0;i<s.data.size();++i) write(addr+(u32)i,1,(u8)s.data[i]);
    } else r->bytes.replace(addr-r->base, s.data.size(), s.data);
    return true;
}

bool EspMachine::load(const Bytes &flash, const esp::FlashImage &layout,
                      bool bootloader, std::string *error)
{
    if (layout.chip != chip_ || (chip_ != esp::CHIP_ESP8266 && chip_ != esp::CHIP_ESP32)) {
        if (error) *error = "ESP image/chip mismatch";
        return false;
    }
    int index = bootloader ? layout.boot_index : layout.app_index;
    if (index < 0 || (size_t)index >= layout.images.size()) {
        if (error) *error = bootloader ? "ESP bootloader image is absent" : "ESP application image is absent";
        return false;
    }
    initialize_memory();
    registers_.clear();
    wifi_dma_.reset();
    gpio_external_.clear();
    gpio_levels_=0;
    for (unsigned i=0;i<3;++i) uarts_[i].reset(chip_==esp::CHIP_ESP32);
    diagnostic_.clear(); flash_ = flash;
    init_esp_nvs_partition(flash_);
    psram_.assign(psram_.size(),'\0');
    active_core_=next_core_=0; app_reset_=true; retired_cpu_instructions_=0;
    reset_domain_=RESET_NONE;reset_mask_=reset_reason_=reset_count_=0;rtc_epoch_us_=0;
    putc1_=0;output_stack_.clear();
    rom_calls_=delay_us_=clock_origin_instructions_=clock_skipped_cycles_=0; cpu_mhz_=80; flash_window_=0; random_=1;
    apb_origin_ticks_=apb_origin_delay_us_=0;
    rtc_wake_us_=rtc_calibration_us_=esp8266_timer_check_ticks_=0;
    cache_enabled_=true; stopped_=XTENSA_OK;
    const esp::Image &image=layout.images[index];
    for (size_t i=0; i<image.segments.size(); ++i)
        if (!load_segment(image.segments[i],error)) return false;
    if(chip_==esp::CHIP_ESP32 && !bootloader) {
        /* Direct app entry starts after the bootloader mapped the image.
         * Populate the real tables too: the SDK's mmap allocator reads them
         * to reserve existing pages. A private Region alone is insufficient. */
        for(size_t i=0;i<image.segments.size();++i) {
            const esp::Segment &s=image.segments[i];
            if(!s.mapped || s.data.empty())continue;
            const bool drom=s.address>=0x3f400000 && s.address<0x3f800000;
            const bool irom=s.address>=0x400d0000 && s.address<0x40c00000;
            const u64 end=u64(s.address)+s.data.size();
            if((!drom && !irom) || end>(drom?0x3f800000ull:0x40c00000ull) ||
                (s.address&0xffff)!=(s.file_offset&0xffff) ||
                s.file_offset>flash_.size() || s.data.size()>flash_.size()-s.file_offset ||
                s.file_offset+s.data.size()>0x1000000u ||
                flash_.compare(s.file_offset,s.data.size(),s.data)!=0) {
                if(error)*error="ESP32 mapped segment has invalid physical flash placement";
                return false;
            }
            const u32 first=s.address&~0xffffu;
            for(u64 address=first;address<end;address+=0x10000) {
                const unsigned entry=drom?unsigned((address-0x3f400000)>>16):
                    64+unsigned((address-0x40000000)>>16);
                const u32 physical=u32((s.file_offset>>16)+((address-first)>>16));
                for(unsigned core=0;core<2;++core) {
                    const u32 cell=0x3ff10000+core*0x2000+entry*4;
                    std::map<u32,u32>::const_iterator prior=registers_.find(cell);
                    if(prior!=registers_.end() && prior->second!=physical) {
                        if(error)*error="ESP32 mapped segments require conflicting MMU pages";
                        return false;
                    }
                    registers_[cell]=physical;
                }
            }
            registers_[0x3ff00058]=0x28;
        }
    }
    cpu_.reset(image.entry_point);
    cpu1_.reset(0x40000400);
    interrupts_.attach_cpu(1,0);
    interrupts_.reset();
    frc_timers_.reset();
    cpu_.set_reg(1, chip_ == esp::CHIP_ESP32
        ? ((!bootloader && index > 0) ? 0x3ffe3d00u : 0x3ffe3f20u) : 0x3ffffff0u);
    if (chip_ == esp::CHIP_ESP32) {
        /* Flash entry is a windowed C call from the ROM/bootloader. Keep a
         * caller window live so ENTRY and the real overflow vectors create
         * the normal stack back-links. Starting ENTRY with CALLINC=0 loses
         * that caller and its first spill reads an uninitialized sp-12. */
        cpu_.set_special(XT_SR_PS, 0x00060020u); /* WOE | CALLINC=2 | UM */
        cpu_.set_reg(8, 0x80000740u); /* ROM _start continuation, call8 encoding */
        write(cpu_.reg(1) - 12, 4, cpu_.reg(1) + 64);
        write(0x3ffe01e0, 4, cpu_mhz_);
        write(0x3ffe01e4, 4, cpu_mhz_);
    } else {
        /* LX106 ROM _start at 0x40000431 sets PS=0x20 before main and
         * flash entry. Preserve UM so the first IRQ uses the user vector. */
        cpu_.set_special(XT_SR_PS,0x20);
        if (!bootloader && image.file_offset != 0)
            cpu_.set_reg(2, u32(image.file_offset));
    }
    initialize_flash_descriptor();
    reset_clocked_devices();
    return true;
}

void EspMachine::initialize_flash_descriptor()
{
    /* ROM probes the NOR capacity before transferring control. Restore this
     * handover state after loading ROM-library RAM initializers as well. */
    u32 descriptor=chip_==esp::CHIP_ESP32 ? 0x3ffae270u : 0x3fffc718u;
    if(chip_==esp::CHIP_ESP8266)write(0x3fffc714,4,descriptor);
    unsigned capacity=0;for(size_t bytes=flash_.size();bytes>1;bytes>>=1) ++capacity;
    write(descriptor,4,(capacity<<16)|0x40efu); /* Winbond identity with actual NOR capacity */
    write(descriptor+4,4,(u32)flash_.size());
    write(descriptor+8,4,0x10000); write(descriptor+12,4,0x1000);
    write(descriptor+16,4,0x100); write(descriptor+20,4,0xffff);
}

bool EspMachine::fault(const char *operation, u32 address)
{
    diagnostic_=strf("%s at 0x%08x (PC 0x%08x)",operation,address,active_cpu().pc());
    return false;
}

bool EspMachine::uart_address(u32 address, unsigned &index, u32 &offset) const
{
    if(chip_==esp::CHIP_ESP32 && address>=0x60000000u && address<0x60040000u)
        address=address-0x60000000u+0x3ff40000u;
    static const u32 bases8266[]={0x60000000u,0x60000f00u};
    static const u32 bases32[]={0x3ff40000u,0x3ff50000u,0x3ff6e000u};
    const u32 *bases=chip_==esp::CHIP_ESP32 ? bases32 : bases8266;
    unsigned count=chip_==esp::CHIP_ESP32 ? 3 : 2;
    for (unsigned i=0;i<count;++i) if (address>=bases[i] && address<bases[i]+0x80) {
        index=i;offset=address-bases[i];return true;
    }
    return false;
}

bool EspMachine::mmio_read(u32 address,u32 &value)
{
    if(chip_==esp::CHIP_ESP32 && address>=0x60000000u && address<0x60040000u)
        address=address-0x60000000u+0x3ff40000u;
    if(chip_==esp::CHIP_ESP32 && interrupts_.handles(address))
        return interrupts_.read(address,esp32_apb_ticks(),value);
    if(chip_==esp::CHIP_ESP8266 && (address==0x6000036c || address==0x60000370))
        return esp8266_mmio_read(address,value);
    if(chip_==esp::CHIP_ESP32 && address>=0x3ff49000u && address<=0x3ff49090u && !(address&3)) {
        value=registers_[address];return true;
    }
    unsigned index; u32 offset;
    if (uart_address(address,index,offset)) {
        const bool ok=uarts_[index].read(offset,esp32_apb_ticks(),apb_frequency(),value);
        update_uart_interrupts();
        return ok;
    }
    u32 gpio=chip_==esp::CHIP_ESP32?0x3ff44000u:0x60000300u;
    u32 span=chip_==esp::CHIP_ESP32?0x700u:0xb0u;
    if(address>=gpio && address<gpio+span)return gpio_mmio_read(address,value);
    return chip_==esp::CHIP_ESP32 ? esp32_mmio_read(address,value) : esp8266_mmio_read(address,value);
}

bool EspMachine::mmio_write(u32 address,u32 value)
{
    if(chip_==esp::CHIP_ESP32 && address>=0x60000000u && address<0x60040000u)
        address=address-0x60000000u+0x3ff40000u;
    if(chip_==esp::CHIP_ESP32 && interrupts_.handles(address))
        return interrupts_.write(address,esp32_apb_ticks(),value);
    if(chip_==esp::CHIP_ESP8266 && (address==0x6000036c || address==0x60000370))
        return esp8266_mmio_write(address,value);
    if(chip_==esp::CHIP_ESP32 && address>=0x3ff49000u && address<=0x3ff49090u && !(address&3)) {
        update_gpio_interrupts();registers_[address]=value&0x7fffu;update_gpio_interrupts();return true;
    }
    unsigned index; u32 offset;
    if (uart_address(address,index,offset)) {
        const bool ok=uarts_[index].write(offset,esp32_apb_ticks(),apb_frequency(),value);
        update_uart_interrupts();
        return ok;
    }
    u32 gpio=chip_==esp::CHIP_ESP32?0x3ff44000u:0x60000300u;
    u32 span=chip_==esp::CHIP_ESP32?0x700u:0xb0u;
    if(address>=gpio && address<gpio+span)return gpio_mmio_write(address,value);
    return chip_==esp::CHIP_ESP32 ? esp32_mmio_write(address,value) : esp8266_mmio_write(address,value);
}

bool EspMachine::read(u32 address,unsigned size,u32 &value,bool instruction)
{
    if(chip_==esp::CHIP_ESP8266 && (size==1 || size==2 || size==3 || size==4)) {
        if(!instruction && address>=0x3ffe8000u && (u64)address+size<=0x40000000ull) {
            value=0;
            const u8 *p=(const u8 *)regions_[0].bytes.data()+(address-0x3ffe8000u);
            for(unsigned i=0;i<size;++i) value|=(u32)p[i]<<(i*8);
            return true;
        }
        if(address>=0x40100000u && (u64)address+size<=0x40110000ull) {
            value=0;
            const u8 *p=(const u8 *)regions_[1].bytes.data()+(address-0x40100000u);
            for(unsigned i=0;i<size;++i) value|=(u32)p[i]<<(i*8);
            return true;
        }
        if(address>=0x40200000u && (u64)address+size<=0x40300000ull) {
            value=0;
            size_t offset=(size_t)(address-0x40200000u)+flash_window_;
            if(!cache_enabled_) return fault("flash cache disabled",address);
            if((u64)offset+size>flash_.size()) return fault("flash read beyond image",address);
            const u8 *p=(const u8 *)flash_.data()+offset;
            for(unsigned i=0;i<size;++i) value|=(u32)p[i]<<(i*8);
            return true;
        }
    }
    if(!instruction && chip_==esp::CHIP_ESP8266 && esp8266_wdev::key_memory(address,size))
        return esp8266_wdev::read_key(registers_,address,size,value);
    value=0;
    if (!(size==1 || size==2 || size==3 || size==4)) return fault("invalid read size",address);
    if(chip_==esp::CHIP_ESP32) {
        bool handled=false;
        bool ram_ok=esp32_psram_read(address,size,value,instruction,handled);
        if(handled)return ram_ok;
        bool ok=esp32_mmu_read(address,size,value,instruction,handled);
        if(handled)return ok;
    }
    u32 physical=address;
    const bool boot_remap=chip_==esp::CHIP_ESP32 && boot_remap_range(address,size) &&
        (register_value(0x3ff00000+active_core_*4)&1);
    if(boot_remap && address<0x400b8000 && (u64)address+size>0x400b0000)
        return fault("SRAM instruction alias hidden by boot remap",address);
    if(boot_remap && address>=0x40000000 && address<0x40008000) {
        if((u64)address+size>0x40008000) {
            /* A density instruction can straddle the remap boundary. */
            for(unsigned i=0;i<size;++i) {
                u32 byte;if(!read(address+i,1,byte,instruction))return false;
                value|=byte<<(8*i);
            }
            return true;
        }
        physical=0x400b0000+(address-0x40000000);
    }
    if (chip_==esp::CHIP_ESP32 && address>=0x3ff80000 && address<0x3ff82000)
        physical=0x400c0000+(address-0x3ff80000);
    Region *r=region(physical,size);
    if (r && (!instruction || r->executable)) {
        if(chip_==esp::CHIP_ESP8266 && r->flash_mapped && !cache_enabled_) return fault("flash cache disabled",address);
        if(chip_==esp::CHIP_ESP32 && physical<0x40000000u && (u64)physical+size>0x3ffe0000u) {
            Region *alias=region(0x400a0000u,0x20000u);
            for(unsigned i=0;i<size;++i) {
                u32 a=esp32_dram_alias(physical+i);
                Region *source=a==physical+i?r:alias;
                value|=(u32)(u8)source->bytes[a-source->base]<<(i*8);
            }
            return true;
        }
        for (unsigned i=0;i<size;++i) value|=(u32)(u8)r->bytes[physical-r->base+i]<<(i*8);
        return true;
    }
    if (!instruction && chip_==esp::CHIP_ESP32 && address>=0x3ff96354u && (u64)address+size<=0x3ff96455ull) {
        for (unsigned i=0;i<size;++i) {
            u32 idx = address + i - 0x3ff96354u;
            u8 flag = 0;
            if (idx > 0) {
                u32 ch = idx - 1;
                if (ch <= 8 || (ch >= 14 && ch <= 31) || ch == 127) flag = 0x20;
                else if (ch >= 9 && ch <= 13) flag = 0x28;
                else if (ch == 32) flag = 0x88;
                else if ((ch >= 33 && ch <= 47) || (ch >= 58 && ch <= 64) ||
                         (ch >= 91 && ch <= 96) || (ch >= 123 && ch <= 126)) flag = 0x10;
                else if (ch >= 48 && ch <= 57) flag = 0x04;
                else if (ch >= 65 && ch <= 70) flag = 0x41;
                else if (ch >= 71 && ch <= 90) flag = 0x01;
                else if (ch >= 97 && ch <= 102) flag = 0x42;
                else if (ch >= 103 && ch <= 122) flag = 0x02;
            }
            value |= (u32)flag << (i * 8);
        }
        return true;
    }
    if (chip_==esp::CHIP_ESP8266 && address>=0x40200000 && (u64)address+size<=0x40300000ull) {
        size_t offset=(size_t)(address-0x40200000)+flash_window_;
        if (!cache_enabled_) return fault("flash cache disabled",address);
        if ((u64)offset+size>flash_.size()) return fault("flash read beyond image",address);
        for (unsigned i=0;i<size;++i) value|=(u32)(u8)flash_[offset+i]<<(i*8);
        return true;
    }
    unsigned uart_index;u32 uart_offset;
    const bool uart_byte=size==1 &&
        uart_address(address,uart_index,uart_offset) && uart_offset==0;
    if (!instruction && ((size==4 && !(address&3)) || uart_byte)) {
        diagnostic_.clear();
        if(mmio_read(address,value))return true;
        if(!diagnostic_.empty()) {
            diagnostic_+=strf(" at 0x%08x (PC 0x%08x)",address,active_cpu().pc());return false;
        }
    }
    return fault(instruction?"unmapped instruction fetch":"unsupported memory read",address);
}

bool EspMachine::write(u32 address,unsigned size,u32 value)
{
    if(chip_==esp::CHIP_ESP8266 && (size==1 || size==2 || size==4)) {
        if(address>=0x3ffe8000u && (u64)address+size<=0x40000000ull) {
            char *p=&regions_[0].bytes[address-0x3ffe8000u];
            for(unsigned i=0;i<size;++i) p[i]=(char)(value>>(8*i));
            return true;
        }
        if(address>=0x40100000u && (u64)address+size<=0x40110000ull) {
            char *p=&regions_[1].bytes[address-0x40100000u];
            for(unsigned i=0;i<size;++i) p[i]=(char)(value>>(8*i));
            return true;
        }
    }
    if(chip_==esp::CHIP_ESP8266 && esp8266_wdev::key_memory(address,size))
        return esp8266_wdev::write_key(registers_,address,size,value);
    if (!(size==1 || size==2 || size==4)) return fault("invalid write size",address);
    if(chip_==esp::CHIP_ESP32) {
        bool handled=false;
        const bool ok=esp32_psram_write(address,size,value,handled);
        if(handled)return ok;
    }
    u32 physical=address;
    const bool boot_remap=chip_==esp::CHIP_ESP32 && boot_remap_range(address,size) &&
        (register_value(0x3ff00000+active_core_*4)&1);
    if(boot_remap && address<0x400b8000 && (u64)address+size>0x400b0000)
        return fault("SRAM instruction alias hidden by boot remap",address);
    if(boot_remap && address>=0x40000000 && address<0x40008000) {
        if((u64)address+size>0x40008000)return fault("write crosses remapped SRAM boundary",address);
        physical=0x400b0000+(address-0x40000000);
    }
    if (chip_==esp::CHIP_ESP32 && address>=0x3ff80000 && address<0x3ff82000)
        physical=0x400c0000+(address-0x3ff80000);
    Region *r=region(physical,size);
    if (r && r->writable) {
        if(chip_==esp::CHIP_ESP32 && physical<0x40000000u && (u64)physical+size>0x3ffe0000u) {
            Region *alias=region(0x400a0000u,0x20000u);
            for(unsigned i=0;i<size;++i) {
                u32 a=esp32_dram_alias(physical+i);
                Region *target=a==physical+i?r:alias;
                target->bytes[a-target->base]=(char)(value>>(8*i));
            }
            return true;
        }
        for (unsigned i=0;i<size;++i) r->bytes[physical-r->base+i]=(char)(value>>(8*i));
        return true;
    }
    /* ESP32 firmware also accesses the UART FIFO with L8UI/S8I. One
     * transaction consumes or emits one byte; adjacent config registers
     * retain their word-access requirement. */
    unsigned uart_index;u32 uart_offset;
    const bool uart_byte=size==1 &&
        uart_address(address,uart_index,uart_offset) && uart_offset==0;
    if ((size==4 && !(address&3)) || uart_byte) {
        diagnostic_.clear();
        if(mmio_write(address,value))return true;
        if(!diagnostic_.empty()) {
            diagnostic_+=strf(" at 0x%08x (write=0x%08x, PC 0x%08x)",address,value,active_cpu().pc());return false;
        }
    }
    return fault("unsupported memory write",address);
}

const std::string &EspMachine::uart(unsigned index) const
{
    static const std::string empty;
    return index<(chip_==esp::CHIP_ESP32?3u:2u) ? uarts_[index].output() : empty;
}

void EspMachine::receive_uart(unsigned index,const Bytes &data)
{
    if(index>=(chip_==esp::CHIP_ESP32?3u:2u))return;
    uarts_[index].receive(data,esp32_apb_ticks(),apb_frequency());
    update_uart_interrupts();
}

void EspMachine::uart_transmit(unsigned index,u8 byte)
{
    if(index>=(chip_==esp::CHIP_ESP32?3u:2u))return;
    uarts_[index].transmit(byte,esp32_apb_ticks(),apb_frequency());
    update_uart_interrupts();
}

void EspMachine::update_uart_interrupts()
{
    const u64 ticks=esp32_apb_ticks();
    const unsigned count=chip_==esp::CHIP_ESP32?3:2;
    bool shared=false;
    for(unsigned i=0;i<count;++i) {
        uarts_[i].advance(ticks,apb_frequency());
        const bool active=uarts_[i].interrupt();
        if(chip_==esp::CHIP_ESP32)interrupts_.set_source(34+i,active);
        else shared=shared||active;
    }
    if(chip_==esp::CHIP_ESP8266)active_cpu().set_irq(5,shared);
}

bool EspMachine::cycles_to_uart_interrupt(u64 &cycles) const
{
    const u64 ticks=esp32_apb_ticks();
    const unsigned count=chip_==esp::CHIP_ESP32?3:2;
    bool found=false;u64 minimum=0;
    for(unsigned i=0;i<count;++i) {
        if(chip_==esp::CHIP_ESP32) {
            if(!interrupts_.source_can_wake(34+i))continue;
        } else if(!(active_cpu().special(XT_SR_INTENABLE)&32) || (active_cpu().special(XT_SR_PS)&31))continue;
        u64 delta;
        if(uarts_[i].next_event_ticks(ticks,apb_frequency(),delta) && (!found || delta<minimum)) {
            minimum=delta;found=true;
        }
    }
    return found && cycles_for_apb_ticks(minimum,cycles);
}

u32 EspMachine::register_value(u32 address) const
{
    std::map<u32,u32>::const_iterator found=registers_.find(address);
    return found==registers_.end()?0:found->second;
}

u64 EspMachine::elapsed_us() const {
    return delay_us_+(cpu_instructions()-clock_origin_instructions_+clock_skipped_cycles_)/cpu_mhz_;
}

void EspMachine::set_cpu_frequency(u32 mhz)
{
    if(!mhz || mhz==cpu_mhz_)return;
    const u64 apb_ticks=esp32_apb_ticks();
    update_esp32_interrupts();
    update_uart_interrupts();
    delay_us_=elapsed_us();
    clock_origin_instructions_=cpu_instructions();
    clock_skipped_cycles_=0;
    cpu_mhz_=mhz;
    if(chip_==esp::CHIP_ESP32) {
        write(0x3ffe01e0, 4, mhz);
        write(0x3ffe01e4, 4, mhz);
    }
    apb_origin_ticks_=apb_ticks;
    apb_origin_delay_us_=delay_us_;
}

bool EspMachine::rom_copy(u32 dst,u32 src,u32 length)
{
    if (length>0x1000000u || (u64)src+length>0x100000000ull || (u64)dst+length>0x100000000ull)
        return fault("ROM copy range invalid",dst);
    Bytes data(length,'\0');
    u32 value;
    for (u32 i=0;i<length;) {
        u32 addr=src+i;
        if (!(addr&3) && i+4<=length && read(addr,4,value,false)) {
            data[i]=(char)(value&0xff);
            data[i+1]=(char)((value>>8)&0xff);
            data[i+2]=(char)((value>>16)&0xff);
            data[i+3]=(char)((value>>24)&0xff);
            i+=4;
        } else if (read(addr,1,value,false)) {
            data[i++]=(char)value;
        } else if (chip_==esp::CHIP_ESP32 && addr>=0x3ff40000u && addr<0x3ff80000u &&
                   read(addr&~3u,4,value,false)) {
            data[i++]=(char)((value>>((addr&3)*8))&0xff);
        } else return false;
    }
    for (u32 i=0;i<length;) {
        u32 addr=dst+i;
        if (!(addr&3) && i+4<=length) {
            u32 word=(u32)(u8)data[i] | ((u32)(u8)data[i+1]<<8) |
                     ((u32)(u8)data[i+2]<<16) | ((u32)(u8)data[i+3]<<24);
            if (!write(addr,4,word)) return false;
            i+=4;
        } else if (write(addr,1,(u8)data[i])) {
            ++i;
        } else if (chip_==esp::CHIP_ESP32 && addr>=0x3ff40000u && addr<0x3ff80000u &&
                   read(addr&~3u,4,value,false)) {
            unsigned shift=(addr&3)*8;
            value=(value&~(0xffu<<shift))|((u32)(u8)data[i]<<shift);
            if (!write(addr&~3u,4,value)) return false;
            ++i;
        } else return false;
    }
    return true;
}

bool EspMachine::rom_string(u32 address,std::string &out,size_t limit)
{
    out.clear();
    for (size_t i=0;i<limit;++i) {
        u32 value;
        if ((u64)address+i>0xffffffffull || !read(address+(u32)i,1,value,false)) return false;
        if (!value) return true;
        out+=(char)value;
    }
    return fault("unterminated ROM string",address);
}

bool EspMachine::rom_argument(unsigned index,u32 &value)
{
    /* Both ABIs pass six integer arguments in registers; further arguments
     * are in the caller's stack frame, before the callee executes ENTRY. */
    if(index<6) { value=active_cpu().rom_argument(index);return true; }
    u64 address=(u64)active_cpu().reg(1)+(index-6)*4;
    if(address>0xffffffffull) return fault("ROM argument stack overflow",active_cpu().reg(1));
    return read((u32)address,4,value,false);
}

bool EspMachine::rom_printf(std::string &out)
{
    std::string format;
    if (!rom_string(active_cpu().rom_argument(0),format)) return false;
    out.clear(); unsigned arg=1;
    for (size_t i=0;i<format.size();++i) {
        if (format[i]!='%') { out+=format[i];continue; }
        ++i; if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        if (format[i]=='%') { out+='%';continue; }
        bool left=false,zero=false;
        if (format[i]=='-') { left=true;++i; }
        if (format[i]=='0') { zero=true;++i; }
        unsigned width=0;
        while (i<format.size() && format[i]>='0' && format[i]<='9') {
            width=width*10+(format[i++]-'0');if (width>4096) return fault("ROM printf width too large",active_cpu().pc());
        }
        int precision=-1;
        if (i<format.size() && format[i]=='.') {
            ++i;precision=0;
            while (i<format.size() && format[i]>='0' && format[i]<='9') {
                precision=precision*10+(format[i++]-'0');if (precision>65536) return fault("ROM printf precision too large",active_cpu().pc());
            }
        }
        if (i<format.size() && format[i]=='l') ++i;
        if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        u32 value;
        if(!rom_argument(arg++,value)) return false;
        std::string piece;
        char temp[48];
        switch (format[i]) {
        case 's': if (!value) piece="(null)";else if (!rom_string(value,piece)) return false;
                  if (precision>=0 && piece.size()>(size_t)precision) piece.resize(precision);
                  break;
        case 'c': piece+=(char)value;break;
        case 'd': case 'i': sprintf(temp,"%d",(s32)value);piece=temp;break;
        case 'u': sprintf(temp,"%u",value);piece=temp;break;
        case 'x': sprintf(temp,"%x",value);piece=temp;break;
        case 'X': sprintf(temp,"%X",value);piece=temp;break;
        case 'p': sprintf(temp,"0x%08x",value);piece=temp;break;
        default:return fault("unsupported ROM printf conversion",active_cpu().pc());
        }
        if (piece.size()<width) {
            std::string pad(width-piece.size(),zero?'0':' ');
            if (left) piece+=pad;
            else if (zero && !piece.empty() && piece[0]=='-') piece="-"+pad+piece.substr(1);
            else piece=pad+piece;
        }
        out+=piece;
    }
    return true;
}

bool EspMachine::rom_vprintf(u32 format_addr, u32 va_stack, u32 va_reg, u32 va_ndx, std::string &out)
{
    std::string format;
    if (!rom_string(format_addr, format)) return false;
    out.clear();
    for (size_t i=0;i<format.size();++i) {
        if (format[i]!='%') { out+=format[i];continue; }
        ++i; if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        if (format[i]=='%') { out+='%';continue; }
        bool left=false,zero=false;
        if (format[i]=='-') { left=true;++i; }
        if (format[i]=='0') { zero=true;++i; }
        unsigned width=0;
        while (i<format.size() && format[i]>='0' && format[i]<='9') {
            width=width*10+(format[i++]-'0');if (width>4096) return fault("ROM printf width too large",active_cpu().pc());
        }
        int precision=-1;
        if (i<format.size() && format[i]=='.') {
            ++i;precision=0;
            while (i<format.size() && format[i]>='0' && format[i]<='9') {
                precision=precision*10+(format[i++]-'0');if (precision>65536) return fault("ROM printf precision too large",active_cpu().pc());
            }
        }
        if (i<format.size() && format[i]=='l') ++i;
        if (i==format.size()) return fault("incomplete ROM printf format",active_cpu().pc());
        u32 value = 0;
        if (va_reg && va_ndx < 24) {
            if (!read(va_reg + va_ndx, 4, value, false)) return fault("ROM vprintf reg read failure", va_reg + va_ndx);
            va_ndx += 4;
        } else {
            u32 addr = va_stack + 32 + (va_ndx >= 24 ? va_ndx - 24 : 0);
            if (!read(addr, 4, value, false)) return fault("ROM vprintf stack read failure", addr);
            va_ndx += 4;
        }
        std::string piece;
        char temp[48];
        switch (format[i]) {
        case 's': if (!value) piece="(null)";else if (!rom_string(value,piece)) return false;
                  if (precision>=0 && piece.size()>(size_t)precision) piece.resize(precision);
                  break;
        case 'c': piece+=(char)value;break;
        case 'd': case 'i': sprintf(temp,"%d",(s32)value);piece=temp;break;
        case 'u': sprintf(temp,"%u",value);piece=temp;break;
        case 'x': sprintf(temp,"%x",value);piece=temp;break;
        case 'X': sprintf(temp,"%X",value);piece=temp;break;
        case 'p': sprintf(temp,"0x%08x",value);piece=temp;break;
        default:return fault("unsupported ROM printf conversion",active_cpu().pc());
        }
        if (piece.size()<width) {
            std::string pad(width-piece.size(),zero?'0':' ');
            if (left) piece+=pad;
            else if (zero && !piece.empty() && piece[0]=='-') piece="-"+pad+piece.substr(1);
            else piece=pad+piece;
        }
        out+=piece;
    }
    return true;
}

bool EspMachine::rom_service(u32 address,XtensaResult &result)
{
    /* HLE callback return bridge, used only while an ets_printf output
     * callback is active. Firmware callbacks execute on the real CPU/bus;
     * output is never inferred from which callback was installed. */
    const u32 output_return_bridge=0x4000fff0u;
    if(chip_==esp::CHIP_ESP8266 && address==output_return_bridge && !output_stack_.empty()) {
        RomOutput &output=output_stack_.back();
        if(output.position<output.text.size()) {
            active_cpu().set_reg(2,(u8)output.text[output.position++]);
            active_cpu().set_reg(0,output_return_bridge);active_cpu().set_pc(output.callback);
        } else {
            u32 target=output.return_pc,length=(u32)output.text.size();
            output_stack_.pop_back();
            active_cpu().set_reg(0,target);active_cpu().set_reg(2,length);active_cpu().set_pc(target);
        }
        ++rom_calls_;result=XTENSA_OK;return true;
    }
    const RomEntry *table=chip_==esp::CHIP_ESP32?esp32_rom:esp8266_rom;
    size_t count=chip_==esp::CHIP_ESP32?sizeof(esp32_rom)/sizeof(esp32_rom[0]):sizeof(esp8266_rom)/sizeof(esp8266_rom[0]);
    const RomEntry *entry=0;
    for (size_t i=0;i<count;++i) if (table[i].address==address) { entry=&table[i];break; }
    if (!entry) return false;
    u32 a=active_cpu().rom_argument(0),b=active_cpu().rom_argument(1),c=active_cpu().rom_argument(2),ret=0;
    bool ok=true,redirected=false;u32 x,y;std::string text;
    switch (entry->operation) {
    case ROM_COPY: ok=rom_copy(a,b,c);ret=a;break;
    case ROM_ZERO: c=b;b=0; /* fall through */
    case ROM_SET: {
        if (c>0x1000000u || (u64)a+c>0x100000000ull) { ok=fault("ROM fill range invalid",a);break; }
        u8 byte = (u8)b;
        u32 word = (u32)byte | ((u32)byte << 8) | ((u32)byte << 16) | ((u32)byte << 24);
        for (u32 i=0;i<c && ok;) {
            u32 addr=a+i;
            if (!(addr&3) && i+4<=c) {
                ok=write(addr,4,word);
                i+=4;
            } else if (write(addr,1,byte)) {
                ++i;
            } else if (chip_==esp::CHIP_ESP32 && addr>=0x3ff40000u && addr<0x3ff80000u) {
                u32 v=0;
                if (!(ok=read(addr&~3u,4,v,false))) break;
                unsigned shift=(addr&3)*8;
                v=(v&~(0xffu<<shift))|((u32)byte<<shift);
                ok=write(addr&~3u,4,v);
                ++i;
            } else {
                ok=false;
            }
        }
        ret=a;break;
    }
    case ROM_COMPARE: case ROM_STRCMP: case ROM_STRNCMP: {
        u32 limit=entry->operation==ROM_STRCMP?65536:c;
        if (limit>0x1000000u) { ok=fault("ROM compare range invalid",a);break; }
        for (u32 i=0;i<limit;++i) {
            if (!read(a+i,1,x,false) || !read(b+i,1,y,false)) { ok=false;break; }
            if (x!=y) { ret=(u32)((s32)x-(s32)y);break; }
            if (!x && entry->operation!=ROM_COMPARE) break;
        }
        break;
    }
    case ROM_LENGTH: ok=rom_string(a,text);ret=(u32)text.size();break;
    case ROM_STRCPY: case ROM_STRNCPY: {
        bool bounded=entry->operation==ROM_STRNCPY;u32 limit=bounded?c:65536;
        if (limit>0x1000000u) { ok=fault("ROM string copy range invalid",a);break; }
        bool ended=false;
        for (u32 i=0;i<limit && ok;++i) {
            x=0;if (!ended) ok=read(b+i,1,x,false);
            if (ok) ok=write(a+i,1,x);
            if (!x) { ended=true;if (!bounded) break; }
        }
        ret=a;break;
    }
    case ROM_PUTC: uart_transmit(0,(u8)a);break;
    case ROM_PRINTF:
        ok=rom_printf(text);
        if(ok) {
            ret=(u32)text.size();
            if(chip_==esp::CHIP_ESP8266 && putc1_ && !text.empty()) {
                RomOutput output;output.return_pc=active_cpu().reg(0);output.callback=putc1_;output.text=text;
                output_stack_.push_back(output);active_cpu().set_pc(output_return_bridge);redirected=true;
            } else for(size_t i=0;i<text.size();++i)uart_transmit(0,(u8)text[i]);
        }
        break;
    case ROM_INSTALL_PUTC:putc1_=a;break;
    case ROM_UART_PRINTF:putc1_=0;break;
    case ROM_IO_VPRINTF: {
        u32 va_stack=c,va_reg=active_cpu().rom_argument(3),va_ndx=active_cpu().rom_argument(4);
        ok=rom_vprintf(b,va_stack,va_reg,va_ndx,text);
        if(ok) {
            ret=(u32)text.size();
            if(chip_==esp::CHIP_ESP8266 && a && !text.empty()) {
                RomOutput output;output.return_pc=active_cpu().reg(0);output.callback=a;output.text=text;
                output_stack_.push_back(output);active_cpu().set_pc(output_return_bridge);redirected=true;
            } else for(size_t i=0;i<text.size();++i)uart_transmit(0,(u8)text[i]);
        }
        break;
    }
    case ROM_FLASH_READ_DATA:
        /* Lower-level ROM entry takes the caller's SpiFlashChip descriptor
         * before the normal (offset, destination, length) arguments. */
        if(!read(a+4,4,x,false)) { ok=false;break; }
        a=b;b=c;c=active_cpu().rom_argument(3);
        if((u64)a+c>x) { ret=1;break; }
        /* fall through */
    case ROM_FLASH_READ:
        if ((u64)a+c>flash_.size()) { ret=1;break; }
        for (u32 i=0;i<c && ok;++i) ok=write(b+i,1,(u8)flash_[a+i]);
        break;
    case ROM_FLASH_WRITE:
        if ((u64)a+c>flash_.size()) { ret=1;break; }
        for (u32 i=0;i<c && ok;++i) { ok=read(b+i,1,x,false);if(ok) flash_[a+i]=(char)((u8)flash_[a+i]&x); }break;
    case ROM_FLASH_ERASE:
        if ((u64)a*4096+4096>flash_.size()) ret=1;
        else flash_.replace((size_t)a*4096,4096,4096,(char)0xff);
        break;
    case ROM_DELAY: delay_us_+=a;advance_clocked_cores((u64)a*cpu_mhz_);break;
    case ROM_CPU_FREQ: ret=cpu_mhz_;break;
    case ROM_SET_FREQ: set_cpu_frequency(a);break;
    case ROM_CACHE_ON:
        if(chip_==esp::CHIP_ESP8266) {
            cache_enabled_=true;flash_window_=(b&3)*0x100000u+(a&1)*0x80000u;
        } else if(a>1)ok=fault("invalid ESP32 cache core",a);
        else {
            u32 control=0x3ff00040+a*0x18;
            ok=mmio_read(control,x) && mmio_write(control,x|8);
        }
        break;
    case ROM_CACHE_OFF:
        if(chip_==esp::CHIP_ESP8266)cache_enabled_=false;
        else if(a>1)ok=fault("invalid ESP32 cache core",a);
        else {
            u32 control=0x3ff00040+a*0x18;
            ok=mmio_read(control,x) && mmio_write(control,x&~8u);
        }
        break;
    case ROM_UART_DIV: ok=mmio_write((a==1?0x3ff50000u:0x3ff40000u)+0x14,b);break;
    case ROM_GET_CCOUNT:ret=active_cpu().special(234);break;
    case ROM_SRAND:random_=a;break;
    case ROM_RAND:random_=random_*1103515245u+12345u;ret=(random_>>1)&0x7fffffffu;break;
    case ROM_RESET_REASON:
        if(chip_==esp::CHIP_ESP8266) ret=0;
        else {
            x=registers_.count(0x3ff48034)?registers_[0x3ff48034]:0x41;
            ret=a<2?(x>>(a*6))&63:0;
        }
        break;
    case ROM_BSWAP32:
        ret = ((a >> 24) & 0xff) | ((a >> 8) & 0xff00) | ((a << 8) & 0xff0000) | ((a << 24) & 0xff000000);
        break;
    case ROM_BSWAP64: {
        u64 val = (u64)a | ((u64)b << 32);
        u64 swapped = ((val >> 56) & 0x00000000000000ffull) |
                      ((val >> 40) & 0x000000000000ff00ull) |
                      ((val >> 24) & 0x0000000000ff0000ull) |
                      ((val >>  8) & 0x00000000ff000000ull) |
                      ((val <<  8) & 0x000000ff00000000ull) |
                      ((val << 24) & 0x0000ff0000000000ull) |
                      ((val << 40) & 0x00ff000000000000ull) |
                      ((val << 56) & 0xff00000000000000ull);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(swapped >> 32));
        ret = (u32)swapped;
        break;
    }
    case ROM_UDIV:if(!b)ok=fault("ROM divide by zero",address);else ret=a/b;break;
    case ROM_UMOD:if(!b)ok=fault("ROM divide by zero",address);else ret=a%b;break;
    case ROM_SDIV:if(!b)ok=fault("ROM divide by zero",address);else ret=(a==0x80000000u && b==0xffffffffu)?a:(u32)((s32)a/(s32)b);break;
    case ROM_SMOD:if(!b)ok=fault("ROM divide by zero",address);else ret=(a==0x80000000u && b==0xffffffffu)?0:(u32)((s32)a%(s32)b);break;
    case ROM_CLZSI2: {
        if (!a) ret = 32;
        else {
            u32 cnt = 0;
            while ((a & 0x80000000u) == 0) { ++cnt; a <<= 1; }
            ret = cnt;
        }
        break;
    }
    case ROM_ASHLDI3: {
        u64 val = (u64)a | ((u64)b << 32);
        u64 res = (c >= 64) ? 0 : (val << c);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(res >> 32));
        ret = (u32)res;
        break;
    }
    case ROM_ASHRDI3: {
        s64 val = (s64)((u64)a | ((u64)b << 32));
        s64 res = (c >= 64) ? (val < 0 ? -1 : 0) : (val >> c);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)((u64)res >> 32));
        ret = (u32)res;
        break;
    }
    case ROM_LSHRDI3: {
        u64 val = (u64)a | ((u64)b << 32);
        u64 res = (c >= 64) ? 0 : (val >> c);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(res >> 32));
        ret = (u32)res;
        break;
    }
    case ROM_FLOAT_SI_SF: {
        float f=(float)(s32)a;
        memcpy(&ret,&f,4);
        break;
    }
    case ROM_FLOAT_UNS_SF: {
        float f=(float)a;
        memcpy(&ret,&f,4);
        break;
    }
    case ROM_FIX_UNS_SF_SI: {
        float f; memcpy(&f,&a,4);
        ret=(u32)f;
        break;
    }
    case ROM_ADD_SF: {
        float fa,fb,f;
        memcpy(&fa,&a,4); memcpy(&fb,&b,4);
        f=fa+fb;
        memcpy(&ret,&f,4);
        break;
    }
    case ROM_SUB_SF: {
        float fa,fb,f;
        memcpy(&fa,&a,4); memcpy(&fb,&b,4);
        f=fa-fb;
        memcpy(&ret,&f,4);
        break;
    }
    case ROM_MUL_SF: {
        float fa,fb,f;
        memcpy(&fa,&a,4); memcpy(&fb,&b,4);
        f=fa*fb;
        memcpy(&ret,&f,4);
        break;
    }
    case ROM_STRSTR: {
        std::string haystack,needle;
        if(!rom_string(a,haystack) || !rom_string(b,needle)) { ok=false; break; }
        size_t pos=haystack.find(needle);
        ret=(pos!=std::string::npos)?(a+(u32)pos):0;
        break;
    }
    case ROM_UMULSIDI3: {
        u64 prod=(u64)a * (u64)b;
        active_cpu().set_reg(active_cpu().pending_call_increment() * 4 + 3, (u32)(prod >> 32));
        ret=(u32)prod;
        break;
    }
    case ROM_FLOAT_SI_DF: {
        double d=(double)(s32)a; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_FLOAT_UNS_DF: {
        double d=(double)a; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_FIX_DF_SI: {
        u64 bits=(u64)a | ((u64)b<<32); double d; memcpy(&d,&bits,8); ret=(u32)(s32)d; break;
    }
    case ROM_FIX_UNS_DF_SI: {
        u64 bits=(u64)a | ((u64)b<<32); double d; memcpy(&d,&bits,8); ret=(u32)d; break;
    }
    case ROM_EXTEND_SF_DF: {
        float f; memcpy(&f,&a,4); double d=(double)f; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_TRUNC_DF_SF: {
        u64 bits=(u64)a | ((u64)b<<32); double d; memcpy(&d,&bits,8); float f=(float)d; memcpy(&ret,&f,4); break;
    }
    case ROM_ADD_DF: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 b1=(u64)a | ((u64)b<<32), b2=(u64)c_arg | ((u64)d_arg<<32);
        double d1, d2; memcpy(&d1,&b1,8); memcpy(&d2,&b2,8);
        double d=d1+d2; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_SUB_DF: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 b1=(u64)a | ((u64)b<<32), b2=(u64)c_arg | ((u64)d_arg<<32);
        double d1, d2; memcpy(&d1,&b1,8); memcpy(&d2,&b2,8);
        double d=d1-d2; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_MUL_DF: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 b1=(u64)a | ((u64)b<<32), b2=(u64)c_arg | ((u64)d_arg<<32);
        double d1, d2; memcpy(&d1,&b1,8); memcpy(&d2,&b2,8);
        double d=d1*d2; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_DIV_DF: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 b1=(u64)a | ((u64)b<<32), b2=(u64)c_arg | ((u64)d_arg<<32);
        double d1, d2; memcpy(&d1,&b1,8); memcpy(&d2,&b2,8);
        double d=d2!=0.0?(d1/d2):0.0; u64 bits; memcpy(&bits,&d,8);
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_MUL_DI: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        s64 op1=(s64)((u64)a | ((u64)b<<32)), op2=(s64)((u64)c_arg | ((u64)d_arg<<32));
        s64 res=op1*op2; u64 bits=(u64)res;
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_DIV_DI: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        s64 op1=(s64)((u64)a | ((u64)b<<32)), op2=(s64)((u64)c_arg | ((u64)d_arg<<32));
        if(!op2) { ok=fault("ROM divide by zero",address); break; }
        s64 res=op1/op2; u64 bits=(u64)res;
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(bits>>32));
        ret=(u32)bits; break;
    }
    case ROM_UDIV_DI: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 op1=(u64)a | ((u64)b<<32), op2=(u64)c_arg | ((u64)d_arg<<32);
        if(!op2) { ok=fault("ROM divide by zero",address); break; }
        u64 res=op1/op2;
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(res>>32));
        ret=(u32)res; break;
    }
    case ROM_UMOD_DI: {
        u32 c_arg=active_cpu().rom_argument(2), d_arg=active_cpu().rom_argument(3);
        u64 op1=(u64)a | ((u64)b<<32), op2=(u64)c_arg | ((u64)d_arg<<32);
        if(!op2) { ok=fault("ROM divide by zero",address); break; }
        u64 res=op1%op2;
        active_cpu().set_reg(active_cpu().pending_call_increment()*4+3, (u32)(res>>32));
        ret=(u32)res; break;
    }
    case ROM_LLDESC_BUILD_CHAIN: {
        u32 buflen=active_cpu().rom_argument(3);
        u32 blksz=active_cpu().rom_argument(4);
        u32 owner=active_cpu().rom_argument(5);
        u32 head_ptr=0, tail_ptr=0;
        rom_argument(6, head_ptr);
        rom_argument(7, tail_ptr);
        if(!blksz) blksz=1;
        u32 cur_desc=a, cur_buf=c, rem=buflen, last_desc=a;
        if(buflen==0 && b>=12) {
            u32 config=(blksz&0xfff) | ((owner?0u:1u)<<30) | ((owner&1)<<31);
            write(a+0,4,config); write(a+4,4,c); write(a+8,4,0);
            last_desc=a;
        } else {
            while(rem>0 && cur_desc+12<=a+b) {
                u32 chunk=rem>blksz?blksz:rem;
                rem-=chunk;
                u32 eof=(!owner && rem==0)?1:0;
                u32 next_desc=(rem>0)?(cur_desc+12):0;
                u32 config=(chunk&0xfff) | ((chunk&0xfff)<<12) | (eof<<30) | ((owner&1)<<31);
                write(cur_desc+0,4,config);
                write(cur_desc+4,4,cur_buf);
                write(cur_desc+8,4,next_desc);
                last_desc=cur_desc;
                cur_desc+=12;
                cur_buf+=chunk;
            }
        }
        if(head_ptr) write(head_ptr,4,a);
        if(tail_ptr) write(tail_ptr,4,last_desc);
        ret=0; break;
    }
    case ROM_LLDESC_SET_OWNER: {
        u32 desc=a;
        while(desc) {
            u32 config=0, next=0;
            if(!read(desc,4,config,false)) break;
            config=(config & ~0x80000000u) | ((b&1)<<31);
            write(desc,4,config);
            if(!read(desc+8,4,next,false)) break;
            desc=next;
        }
        ret=0; break;
    }
    case ROM_LLDESC_NUM2LINK: {
        u32 desc=a;
        for(u32 i=0;i<b && desc;++i) {
            u32 next=0;
            if(!read(desc+8,4,next,false)) { desc=0; break; }
            desc=next;
        }
        ret=desc; break;
    }
    case ROM_SPI_IDLE:
    case ROM_SPI_NOOP:
        ret=0; break;
    case ROM_SPI_WRITE_ENABLE:
        registers_[0xff800000] |= 2u;
        ret=0; break;
    case ROM_SPI_WRITE_STATUS:
        registers_[0xff800000] &= ~2u;
        ret=0; break;
    case ROM_TRUE:
        ret=1; break;
    case ROM_SET_INTLEVEL:
    case ROM_RESTORE_INTLEVEL: {
        u32 old_ps = active_cpu().special(XT_SR_PS);
        active_cpu().set_special(XT_SR_PS, (old_ps & ~15u) | (a & 15u));
        ret = old_ps;
        break;
    }
    case ROM_SET_APPCPU_BOOT_ADDR:
        registers_[0x3ff00038] = a;
        ret = 0; break;
    case ROM_INTR_MATRIX_SET: {
        u32 reg = (a ? 0x3ff00218u : 0x3ff00104u) + b * 4;
        write(reg, 4, c);
        ret = 0; break;
    }
    case ROM_QSORT: {
        u32 base = a, nmemb = b, elem_size = c;
        if (elem_size >= 4 && nmemb <= 256) {
            std::vector<std::vector<u8> > items;
            for (u32 i = 0; i < nmemb; ++i) {
                std::vector<u8> item(elem_size);
                for (u32 j = 0; j < elem_size; ++j) {
                    u32 byte = 0; read(base + i * elem_size + j, 1, byte, false);
                    item[j] = (u8)byte;
                }
                items.push_back(item);
            }
            // Sort by first 32-bit word (little-endian u32)
            for (size_t i = 0; i < items.size(); ++i) {
                for (size_t j = i + 1; j < items.size(); ++j) {
                    u32 key_i = items[i][0] | (items[i][1] << 8) | (items[i][2] << 16) | (items[i][3] << 24);
                    u32 key_j = items[j][0] | (items[j][1] << 8) | (items[j][2] << 16) | (items[j][3] << 24);
                    if (key_i > key_j) {
                        std::swap(items[i], items[j]);
                    }
                }
            }
            // Write back sorted items
            for (u32 i = 0; i < nmemb; ++i) {
                for (u32 j = 0; j < elem_size; ++j) {
                    write(base + i * elem_size + j, 1, items[i][j]);
                }
            }
        }
        ret = 0; break;
    }
    case ROM_DIV_T: {
        s32 numer = (s32)a, denom = (s32)b;
        s32 quot = denom ? (numer / denom) : 0;
        s32 rem = denom ? (numer % denom) : 0;
        active_cpu().set_reg(active_cpu().pending_call_increment() * 4 + 3, (u32)rem);
        ret = (u32)quot;
        break;
    }
    case ROM_MMU_INIT:
        registers_[0x3ff00040 + (a & 1) * 0x18] |= 8;
        ret=0; break;
    case ROM_CACHE_MMU_SET: {
        u32 cpu_no = a;
        u32 vaddr = c;
        u32 paddr = active_cpu().rom_argument(3);
        u32 psize = active_cpu().rom_argument(4);
        u32 num = active_cpu().rom_argument(5);
        if (!psize) psize = 64;
        u32 step_bytes = psize * 1024;
        for (u32 i = 0; i < num; ++i) {
            int entry = esp32_flash_entry(vaddr + i * step_bytes);
            if (entry >= 0) {
                u32 cell = 0x3ff10000 + (cpu_no & 1) * 0x2000 + (u32)entry * 4;
                registers_[cell] = (paddr / 0x10000) + i;
            }
        }
        ret=0; break;
    }
    case ROM_CACHE_SRAM_MMU_SET: {
        u32 cpu_no = a;
        u32 vaddr = c;
        u32 paddr = active_cpu().rom_argument(3);
        u32 psize = active_cpu().rom_argument(4);
        u32 num = active_cpu().rom_argument(5);
        if (!psize) psize = 32;
        unsigned mode = psize == 16 ? 1 : psize == 8 ? 2 : psize == 4 ? 3 : psize == 2 ? 4 : 0;
        u32 ctrl1 = (cpu_no & 1) ? 0x3ff0005cu : 0x3ff00044u;
        u32 cfg = registers_.count(ctrl1) ? registers_[ctrl1] : 0x8ffu;
        registers_[ctrl1] = (cfg & ~0x1e0u) | (mode << 6);
        u32 step_bytes = psize * 1024;
        if (vaddr >= 0x3f800000u && vaddr < 0x3fc00000u && step_bytes) {
            u32 first = (vaddr - 0x3f800000u) / step_bytes;
            u32 pfirst = paddr / step_bytes;
            for (u32 i = 0; i < num && first + i < 128; ++i) {
                u32 cell = 0x3ff11200u + (cpu_no & 1) * 0x2000u + (first + i) * 4u;
                registers_[cell] = (pfirst + i) & 0xffu;
            }
        }
        ret=0; break;
    }
    case ROM_PHY_GET_ROMFUNCS:
        for (unsigned i = 0; i < 128; ++i) {
            u32 fn = 0x400044c0u;
            if (i == 0x29 || i == 0x37) fn = 0x400044c4u;
            else if (i == 0x1b) fn = 0x400044c8u;
            write(0x3ffae400u + i * 4, 4, fn);
        }
        ret=0x3ffae400u; break;
    case ROM_PHY_PLL_CAP:
        ret=0x80u; break;
    case ROM_PHY_FREQ2CHAN:
        if (a == 2484) ret = 14;
        else if (a >= 2412 && a <= 2472) ret = (a - 2407) / 5;
        else if (a >= 1 && a <= 14) ret = a;
        else ret = 1;
        break;
    case ROM_SPI_READ_STATUS: {
        u32 st = registers_[0xff800000];
        if (b >= 0x3f000000 && b < 0x40000000) {
            write(b, 4, st);
        } else if (a >= 0x3f000000 && a < 0x40000000) {
            if (b == 0x35) st = registers_[0xff800004] ? registers_[0xff800004] : 0x02;
            write(a, 4, st);
        }
        ret=0; break;
    }
    case ROM_FLASH_ERASE_DATA: {
        u32 addr=(b < (flash_.size()/4096) ? b*4096 : b);
        if((u64)addr+4096>flash_.size()) ret=1;
        else {
            flash_.replace((size_t)(addr&~4095u),4096,4096,(char)0xff);
            ret=0;
        }
        break;
    }
    case ROM_FLASH_WRITE_DATA: {
        u32 dest=b, src=c, len=active_cpu().rom_argument(3);
        if((u64)dest+len>flash_.size()) { ret=1; break; }
        for(u32 i=0;i<len && ok;++i) {
            if(!read(src+i,1,x,false)) { ok=false; break; }
            flash_[dest+i]=(char)((u8)flash_[dest+i]&x);
        }
        ret=ok?0:1; break;
    }
    case ROM_SHA_ENABLE:
        registers_[0x3ff0001c] |= 2;
        registers_[0x3ff00020] &= ~10;
        ret = 0; break;
    case ROM_SHA_DISABLE:
        registers_[0x3ff0001c] &= ~2;
        registers_[0x3ff00020] |= 10;
        ret = 0; break;
    case ROM_CRC32_LE: {
        u32 crc = ~a;
        for (u32 i = 0; i < c; ++i) {
            u32 v = 0;
            if (!read(b + i, 1, v, false)) { ok = false; break; }
            crc ^= (u8)v;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
        ret = ~crc; break;
    }
    case ROM_CRC32_BE: {
        u32 crc = ~a;
        for (u32 i = 0; i < c; ++i) {
            u32 v = 0;
            if (!read(b + i, 1, v, false)) { ok = false; break; }
            crc ^= ((u32)(u8)v << 24);
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
        }
        ret = ~crc; break;
    }
    case ROM_MD5_INIT: {
        Md5Context ctx;
        md5_init(&ctx);
        for (unsigned i = 0; i < 4; ++i) write(a + i * 4, 4, ctx.buf[i]);
        for (unsigned i = 0; i < 2; ++i) write(a + 16 + i * 4, 4, ctx.bits[i]);
        for (unsigned i = 0; i < 64; ++i) write(a + 24 + i, 1, 0);
        ret = 0; break;
    }
    case ROM_MD5_UPDATE: {
        Md5Context ctx;
        for (unsigned i = 0; i < 4; ++i) read(a + i * 4, 4, ctx.buf[i], false);
        for (unsigned i = 0; i < 2; ++i) read(a + 16 + i * 4, 4, ctx.bits[i], false);
        for (unsigned i = 0; i < 64; ++i) { u32 v = 0; read(a + 24 + i, 1, v, false); ctx.in[i] = (u8)v; }
        std::vector<u8> buf(c);
        for (u32 i = 0; i < c; ++i) { u32 v = 0; read(b + i, 1, v, false); buf[i] = (u8)v; }
        if (c > 0) md5_update(&ctx, &buf[0], c);
        for (unsigned i = 0; i < 4; ++i) write(a + i * 4, 4, ctx.buf[i]);
        for (unsigned i = 0; i < 2; ++i) write(a + 16 + i * 4, 4, ctx.bits[i]);
        for (unsigned i = 0; i < 64; ++i) write(a + 24 + i, 1, ctx.in[i]);
        ret = 0; break;
    }
    case ROM_MD5_FINAL: {
        Md5Context ctx;
        for (unsigned i = 0; i < 4; ++i) read(b + i * 4, 4, ctx.buf[i], false);
        for (unsigned i = 0; i < 2; ++i) read(b + 16 + i * 4, 4, ctx.bits[i], false);
        for (unsigned i = 0; i < 64; ++i) { u32 v = 0; read(b + 24 + i, 1, v, false); ctx.in[i] = (u8)v; }
        u8 dig[16];
        md5_final(dig, &ctx);
        for (unsigned i = 0; i < 16; ++i) write(a + i, 1, dig[i]);
        for (unsigned i = 0; i < 88; ++i) write(b + i, 1, 0);
        ret = 0; break;
    }
    case ROM_ITOA: {
        s32 val = (s32)a;
        u32 str = b;
        s32 base = (s32)c;
        if (base < 2 || base > 36) {
            ok = write(str, 1, 0);
            ret = str;
            break;
        }
        bool neg = (val < 0 && base == 10);
        u32 uval = neg ? (u32)(-(s64)val) : (u32)val;
        char buf[36];
        unsigned len = 0;
        do {
            unsigned d = uval % (u32)base;
            buf[len++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
            uval /= (u32)base;
        } while (uval > 0);
        if (neg) buf[len++] = '-';
        for (unsigned i = 0; i < len && ok; ++i)
            ok = write(str + i, 1, (u8)buf[len - 1 - i]);
        if (ok) ok = write(str + len, 1, 0);
        ret = str;
        break;
    }
    case ROM_SINIT: {
        u32 stdin_fp = 0, stdout_fp = 0, stderr_fp = 0, lock_handle = 0;
        read(a + 4, 4, stdin_fp, false);
        read(a + 8, 4, stdout_fp, false);
        read(a + 12, 4, stderr_fp, false);
        if (!stdin_fp) { stdin_fp = a + 0x60; write(a + 4, 4, stdin_fp); }
        if (!stdout_fp) { stdout_fp = a + 0xc8; write(a + 8, 4, stdout_fp); }
        if (!stderr_fp) { stderr_fp = a + 0x130; write(a + 12, 4, stderr_fp); }
        read(0x3ffae0ac, 4, lock_handle, false);
        const u32 fps[3] = { stdin_fp, stdout_fp, stderr_fp };
        const u16 flags[3] = { 0x0004, 0x0009, 0x000a };
        for (unsigned i = 0; i < 3; ++i) {
            u32 fp = fps[i];
            write(fp + 0, 4, 0);
            write(fp + 4, 4, 0);
            write(fp + 8, 4, 0);
            write(fp + 12, 2, flags[i]);
            write(fp + 14, 2, i);
            write(fp + 16, 4, 0);
            write(fp + 20, 4, 0);
            write(fp + 24, 4, 0);
            write(fp + 28, 4, fp);
            write(fp + 88, 4, lock_handle);
        }
        write(a + 24, 4, 1);
        ret = 0; break;
    }
    case ROM_SWBUF_R: {
        u32 ch = b & 0xff, fp = c;
        u32 base = 0, p = 0, size = 0, flags = 0, file_no = 0;
        read(fp + 0, 4, p, false);
        read(fp + 12, 2, flags, false);
        read(fp + 14, 2, file_no, false);
        read(fp + 16, 4, base, false);
        read(fp + 20, 4, size, false);
        if (base && p > base && p - base <= 65536) {
            for (u32 i = base; i < p; ++i) {
                u32 v = 0;
                read(i, 1, v, false);
                if (file_no == 1 || file_no == 2) uart_transmit(0, (u8)v);
            }
            write(fp + 0, 4, base);
        }
        if ((flags & 2) || ch == '\n' || !base || !size) {
            if (file_no == 1 || file_no == 2) uart_transmit(0, (u8)ch);
            write(fp + 8, 4, (flags & 3) ? 0 : size);
        } else {
            write(base, 1, ch);
            write(fp + 0, 4, base + 1);
            write(fp + 8, 4, (flags & 1) ? 0xffffffffu : (size - 1));
        }
        ret = ch; break;
    }
    case ROM_FFLUSH_R: {
        u32 fp = b;
        if (!fp && a) read(a + 8, 4, fp, false);
        if (fp) {
            u32 base = 0, p = 0, size = 0, flags = 0, file_no = 0;
            read(fp + 0, 4, p, false);
            read(fp + 12, 2, flags, false);
            read(fp + 14, 2, file_no, false);
            read(fp + 16, 4, base, false);
            read(fp + 20, 4, size, false);
            if (base && p > base && p - base <= 65536) {
                for (u32 i = base; i < p; ++i) {
                    u32 v = 0;
                    read(i, 1, v, false);
                    if (file_no == 1 || file_no == 2) uart_transmit(0, (u8)v);
                }
                write(fp + 0, 4, base);
                write(fp + 8, 4, (flags & 3) ? 0 : size);
            }
        }
        ret = 0; break;
    }
    case ROM_MEMCHR: {
        ret = 0;
        u32 target = b & 0xff;
        for (u32 i = 0; i < c; ++i) {
            u32 v = 0;
            if (!read(a + i, 1, v, false)) { ok = false; break; }
            if ((v & 0xff) == target) { ret = a + i; break; }
        }
        break;
    }
    case ROM_STRCHR: {
        ret = 0;
        u32 target = b & 0xff;
        for (u32 i = 0; i < 65536; ++i) {
            u32 v = 0;
            if (!read(a + i, 1, v, false)) { ok = false; break; }
            if ((v & 0xff) == target) { ret = a + i; break; }
            if ((v & 0xff) == 0) break;
        }
        break;
    }
    case ROM_STRRCHR: {
        ret = 0;
        u32 target = b & 0xff;
        for (u32 i = 0; i < 65536; ++i) {
            u32 v = 0;
            if (!read(a + i, 1, v, false)) { ok = false; break; }
            if ((v & 0xff) == target) ret = a + i;
            if ((v & 0xff) == 0) break;
        }
        break;
    }
    case ROM_STRCAT: {
        std::string s1, s2;
        if (!(ok = rom_string(a, s1) && rom_string(b, s2))) break;
        u32 start = a + (u32)s1.size();
        for (size_t i = 0; i <= s2.size() && ok; ++i)
            ok = write(start + (u32)i, 1, i == s2.size() ? 0 : (u8)s2[i]);
        ret = a; break;
    }
    case ROM_STRNCAT: {
        std::string s1, s2;
        if (!(ok = rom_string(a, s1) && rom_string(b, s2))) break;
        u32 start = a + (u32)s1.size();
        size_t n = s2.size() < (size_t)c ? s2.size() : (size_t)c;
        for (size_t i = 0; i < n && ok; ++i)
            ok = write(start + (u32)i, 1, (u8)s2[i]);
        if (ok) ok = write(start + (u32)n, 1, 0);
        ret = a; break;
    }
    case ROM_STRCSPN: {
        std::string s1, s2;
        if (!(ok = rom_string(a, s1) && rom_string(b, s2))) break;
        size_t n = 0;
        while (n < s1.size() && s2.find(s1[n]) == std::string::npos) ++n;
        ret = (u32)n; break;
    }
    case ROM_STRLCPY: {
        std::string src;
        if (!(ok = rom_string(b, src))) break;
        if (c > 0) {
            size_t n = src.size() < (size_t)(c - 1) ? src.size() : (size_t)(c - 1);
            for (size_t i = 0; i < n && ok; ++i) ok = write(a + (u32)i, 1, (u8)src[i]);
            if (ok) ok = write(a + (u32)n, 1, 0);
        }
        ret = (u32)src.size(); break;
    }
    case ROM_STRNLEN: {
        ret = 0;
        for (u32 i = 0; i < b; ++i) {
            u32 v = 0;
            if (!read(a + i, 1, v, false)) { ok = false; break; }
            if ((v & 0xff) == 0) break;
            ++ret;
        }
        break;
    }
    case ROM_STRSPN: {
        std::string s1, s2;
        if (!(ok = rom_string(a, s1) && rom_string(b, s2))) break;
        size_t n = 0;
        while (n < s1.size() && s2.find(s1[n]) != std::string::npos) ++n;
        ret = (u32)n; break;
    }
    case ROM_STRCASESTR: {
        std::string s1, s2;
        if (!(ok = rom_string(a, s1) && rom_string(b, s2))) break;
        for (size_t i = 0; i < s1.size(); ++i)
            if (s1[i] >= 'A' && s1[i] <= 'Z') s1[i] = (char)(s1[i] - 'A' + 'a');
        for (size_t i = 0; i < s2.size(); ++i)
            if (s2[i] >= 'A' && s2[i] <= 'Z') s2[i] = (char)(s2[i] - 'A' + 'a');
        size_t pos = s1.find(s2);
        ret = (pos == std::string::npos) ? 0 : (a + (u32)pos);
        break;
    }
    case ROM_STRDUP: {
        u32 table = 0;
        read(0x3ffae020, 4, table, false);
        if (table) {
            active_cpu().set_pc(0x4000020c);
            redirected = true;
        } else {
            ret = 0;
        }
        break;
    }
    case ROM_SCCL: {
        u32 fmt = b, ch = 0;
        read(fmt++, 1, ch, false);
        u8 def = 0;
        if (ch == '^') { def = 1; read(fmt++, 1, ch, false); }
        for (unsigned i = 0; i < 256; ++i) write(a + i, 1, def);
        if (ch == 0) { ret = fmt - 1; break; }
        u8 v = (u8)(1 - def);
        for (;;) {
            write(a + (ch & 0xff), 1, v);
            u32 n = 0;
            read(fmt++, 1, n, false);
            if (n == 0) { ret = fmt - 1; break; }
            if (n == ']') { ret = fmt; break; }
            if (n == '-') {
                u32 next = 0;
                read(fmt, 1, next, false);
                if (next != ']' && next >= ch) {
                    ++fmt;
                    while (ch < next) { ++ch; write(a + (ch & 0xff), 1, v); }
                    read(fmt++, 1, n, false);
                    if (n == 0) { ret = fmt - 1; break; }
                    if (n == ']') { ret = fmt; break; }
                } else {
                    n = '-';
                }
            }
            ch = n;
        }
        break;
    }
    case ROM_ATOI: {
        std::string s;
        if (!(ok = rom_string(a, s))) break;
        ret = (u32)strtol(s.c_str(), 0, 10);
        break;
    }
    case ROM_STRTOL_R: {
        std::string s;
        if (!(ok = rom_string(b, s))) break;
        char *endp = 0;
        s32 val = (s32)strtol(s.c_str(), &endp, (int)active_cpu().rom_argument(3));
        if (c) write(c, 4, b + (u32)(endp - s.c_str()));
        ret = (u32)val; break;
    }
    case ROM_STRTOUL_R: {
        std::string s;
        if (!(ok = rom_string(b, s))) break;
        char *endp = 0;
        u32 val = (u32)strtoul(s.c_str(), &endp, (int)active_cpu().rom_argument(3));
        if (c) write(c, 4, b + (u32)(endp - s.c_str()));
        ret = val; break;
    }
    case ROM_MOD_DI: {
        u32 c_arg = active_cpu().rom_argument(2), d_arg = active_cpu().rom_argument(3);
        s64 op1 = (s64)((u64)a | ((u64)b << 32)), op2 = (s64)((u64)c_arg | ((u64)d_arg << 32));
        if (!op2) { ok = fault("ROM divide by zero", address); break; }
        s64 res = op1 % op2; u64 bits = (u64)res;
        active_cpu().set_reg(active_cpu().pending_call_increment() * 4 + 3, (u32)(bits >> 32));
        ret = (u32)bits; break;
    }
    case ROM_CTZDI2: {
        u64 val = (u64)a | ((u64)b << 32);
        if (!val) ret = 64;
        else {
            u32 cnt = 0;
            while ((val & 1) == 0) { ++cnt; val >>= 1; }
            ret = cnt;
        }
        break;
    }
    case ROM_POPCOUNTSI2: {
        u32 cnt = 0, v = a;
        while (v) { cnt += (v & 1); v >>= 1; }
        ret = cnt; break;
    }
    case ROM_ROUNDUP2:
        ret = b ? ((a + b - 1) & ~(b - 1)) : a;
        break;
    case ROM_CRC8: {
        u8 crc = 0;
        for (u32 i = 0; i < b; ++i) {
            u32 v = 0;
            if (!read(a + i, 1, v, false)) { ok = false; break; }
            crc ^= (u8)v;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (u8)((crc >> 1) ^ ((crc & 1) ? 0x8c : 0));
        }
        ret = crc; break;
    }
    case ROM_UART_RX_ONE_CHAR: {
        u32 status = 0;
        uarts_[0].read(0x1c, esp32_apb_ticks(), apb_frequency(), status);
        if ((status & 0xff) > 0) {
            u32 ch = 0;
            uarts_[0].read(0x00, esp32_apb_ticks(), apb_frequency(), ch);
            update_uart_interrupts();
            write(a, 1, ch & 0xff);
            ret = 0;
        } else {
            ret = 1;
        }
        break;
    }
    case ROM_SOFTWARE_RESET:
        request_reset(RESET_DIGITAL, 3, 3);
        ret = 0; break;
    case ROM_FLOAT_UND_SF: {
        u64 val = (u64)a | ((u64)b << 32);
        float f = (float)val;
        memcpy(&ret, &f, 4); break;
    }
    case ROM_FLOAT_DI_SF: {
        s64 val = (s64)((u64)a | ((u64)b << 32));
        float f = (float)val;
        memcpy(&ret, &f, 4); break;
    }
    case ROM_FLOAT_DI_DF: {
        s64 val = (s64)((u64)a | ((u64)b << 32));
        double d = (double)val;
        u64 bits; memcpy(&bits, &d, 8);
        active_cpu().set_reg(active_cpu().pending_call_increment() * 4 + 3, (u32)(bits >> 32));
        ret = (u32)bits; break;
    }
    case ROM_FIX_DF_DI: {
        u64 bits = (u64)a | ((u64)b << 32);
        double d; memcpy(&d, &bits, 8);
        s64 res = (s64)d; u64 rbits = (u64)res;
        active_cpu().set_reg(active_cpu().pending_call_increment() * 4 + 3, (u32)(rbits >> 32));
        ret = (u32)rbits; break;
    }
    case ROM_CMP_DF_LT: {
        u32 c_arg = active_cpu().rom_argument(2), d_arg = active_cpu().rom_argument(3);
        u64 b1 = (u64)a | ((u64)b << 32), b2 = (u64)c_arg | ((u64)d_arg << 32);
        double d1, d2; memcpy(&d1, &b1, 8); memcpy(&d2, &b2, 8);
        ret = (d1 != d1 || d2 != d2) ? 1u : (d1 < d2 ? 0xffffffffu : (d1 > d2 ? 1u : 0u));
        break;
    }
    case ROM_CMP_DF_GT: {
        u32 c_arg = active_cpu().rom_argument(2), d_arg = active_cpu().rom_argument(3);
        u64 b1 = (u64)a | ((u64)b << 32), b2 = (u64)c_arg | ((u64)d_arg << 32);
        double d1, d2; memcpy(&d1, &b1, 8); memcpy(&d2, &b2, 8);
        ret = (d1 != d1 || d2 != d2) ? 0xffffffffu : (d1 < d2 ? 0xffffffffu : (d1 > d2 ? 1u : 0u));
        break;
    }
    case ROM_UNORD_DF: {
        u32 c_arg = active_cpu().rom_argument(2), d_arg = active_cpu().rom_argument(3);
        u64 b1 = (u64)a | ((u64)b << 32), b2 = (u64)c_arg | ((u64)d_arg << 32);
        double d1, d2; memcpy(&d1, &b1, 8); memcpy(&d2, &b2, 8);
        ret = (d1 != d1 || d2 != d2) ? 1u : 0u;
        break;
    }
    case ROM_SETJMP: {
        unsigned wb = active_cpu().special(XT_SR_WINDOWBASE) & 15;
        u32 ws = active_cpu().special(XT_SR_WINDOWSTART) & 0xffffu;
        unsigned setjmp_ci = active_cpu().pending_call_increment();
        u32 ret_a0 = active_cpu().reg(setjmp_ci * 4);
        /* Spill all older active windows in the ring to their stack save areas. */
        unsigned w = wb;
        u32 sp = active_cpu().reg(1);
        unsigned ci = active_cpu().reg(0) >> 30;
        for (unsigned step = 0; step < 16 && ci >= 1 && ci <= 3; ++step) {
            unsigned prev_w = (w - ci) & 15;
            if (prev_w == wb || !(ws & (1u << prev_w))) break;
            active_cpu().set_special(XT_SR_WINDOWBASE, prev_w);
            u32 prev_a0 = active_cpu().reg(0), prev_sp = active_cpu().reg(1);
            for (unsigned r = 0; r < 4; ++r)
                write(sp - 16 + r * 4, 4, active_cpu().reg(r));
            if (ci >= 2) {
                u32 ovf = prev_sp - ci * 16;
                for (unsigned r = 4; r < ci * 4; ++r)
                    write(ovf + (r - 4) * 4, 4, active_cpu().reg(r));
            }
            w = prev_w;
            sp = prev_sp;
            ci = prev_a0 >> 30;
        }
        active_cpu().set_special(XT_SR_WINDOWBASE, wb);
        u32 caller_sp = active_cpu().reg(1);
        for (unsigned r = 0; r < 4 && ok; ++r)
            ok = write(a + r * 4, 4, active_cpu().reg(r));
        if (setjmp_ci >= 2) {
            for (unsigned r = 4; r < setjmp_ci * 4 && ok; ++r)
                ok = write(a + r * 4, 4, active_cpu().reg(r));
        }
        for (unsigned r = 0; r < 4 && ok; ++r) {
            u32 v = 0;
            read(caller_sp - 16 + r * 4, 4, v, false);
            ok = write(a + 48 + r * 4, 4, v);
        }
        if (ok) ok = write(a + 64, 4, ret_a0);
        ret = 0;
        break;
    }
    case ROM_LONGJMP: {
        if (address == 0x400562cc) {
            unsigned ci = active_cpu().pending_call_increment();
            if (!ci) ci = (active_cpu().special(XT_SR_PS) >> 16) & 3;
            u32 caller_sp = active_cpu().reg(1);
            active_cpu().set_reg(ci * 4 + 1, caller_sp - 16);
            unsigned new_wb = (active_cpu().special(XT_SR_WINDOWBASE) + ci) & 15;
            active_cpu().set_special(XT_SR_WINDOWBASE, new_wb);
            active_cpu().set_special(XT_SR_WINDOWSTART, 1u << new_wb);
        }
        u32 sp = active_cpu().reg(1);
        u32 env = active_cpu().reg(2);
        u32 val = active_cpu().reg(3);
        u32 ret_a0 = 0;
        if (!(ok = read(env + 64, 4, ret_a0, false))) break;
        active_cpu().set_reg(0, ret_a0);
        for (unsigned r = 0; r < 4 && ok; ++r) {
            u32 v = 0;
            ok = read(env + r * 4, 4, v, false) && write(sp - 16 + r * 4, 4, v);
        }
        unsigned setjmp_ci = ret_a0 >> 30;
        if (setjmp_ci >= 2 && ok) {
            u32 a8 = 0;
            ok = read(env + 52, 4, a8, false);
            u32 dst = a8 - setjmp_ci * 16;
            for (unsigned r = 0; r < (setjmp_ci - 1) * 4 && ok; ++r) {
                u32 v = 0;
                ok = read(env + 16 + r * 4, 4, v, false) && write(dst + r * 4, 4, v);
            }
        }
        u32 target_sp = 0;
        if (ok) ok = read(env + 4, 4, target_sp, false);
        for (unsigned r = 0; r < 4 && ok; ++r) {
            u32 v = 0;
            ok = read(env + 48 + r * 4, 4, v, false) && write(target_sp - 16 + r * 4, 4, v);
        }
        active_cpu().set_reg(2, val ? val : 1u);
        active_cpu().set_pc(0x40000236u); /* retw.n in rom_vectors */
        redirected = true;
        break;
    }
    case ROM_ASCII_WCTOMB: {
        if (b == 0) {
            ret = 0;
        } else {
            ok = write(b, 1, c & 0xff);
            ret = 1;
        }
        break;
    }
    case ROM_SYSCALL_STUB: {
        u32 table = 0, fn = 0;
        read(active_core_ == 0 ? 0x3ffae020u : 0x3ffae01cu, 4, table, false);
        if (!table) { ret = 0; break; }
        unsigned slot = 0;
        bool prepend_reent = false;
        if (address == 0x4000be8c) slot = 0;
        else if (address == 0x4000bbb4) slot = 1;
        else if (address == 0x4000bbcc) slot = 2;
        else if (address == 0x4000bbe0) slot = 3;
        else if (address == 0x4000bbf8) slot = 4;
        else if (address == 0x4000bea0) { slot = 1; prepend_reent = true; }
        else if (address == 0x4000beb8) { slot = 2; prepend_reent = true; }
        else if (address == 0x4000becc) { slot = 3; prepend_reent = true; }
        else if (address == 0x4000bee4) { slot = 4; prepend_reent = true; }
        else if (address >= 0x4000bdc4 && address <= 0x4000be78)
            slot = 25 + (address - 0x4000bdc4) / 0x14;
        else if (address == 0x4000bc10) slot = 6;
        else if (address == 0x4000bc28) slot = 7;
        else if (address == 0x4000bc40) slot = 8;
        else if (address == 0x4000bc58) slot = 9;
        else if (address == 0x4000bc70) slot = 10;
        else if (address == 0x4000bc84) slot = 11;
        else if (address == 0x4000bc9c) slot = 12;
        else if (address == 0x4000bcb4) slot = 13;
        else if (address == 0x4000bccc) slot = 14;
        else if (address == 0x4000bce4) slot = 15;
        else if (address == 0x4000bcfc) slot = 16;
        else if (address == 0x4000bd10) slot = 17;
        else if (address == 0x4000bd28) slot = 18;
        else if (address == 0x4000bd3c) slot = 19;
        else if (address == 0x4000bd54) slot = 20;
        else if (address == 0x4000bd70) slot = 21;
        else if (address == 0x4000bd8c) slot = 22;
        else if (address == 0x4000bda8) slot = 23;
        read(table + slot * 4, 4, fn, false);
        if (!fn) { ret = 0; break; }
        if (prepend_reent) {
            unsigned base_reg = active_cpu().pending_call_increment() * 4;
            active_cpu().set_reg(base_reg + 4, b);
            active_cpu().set_reg(base_reg + 3, a);
            active_cpu().set_reg(base_reg + 2, 0x3ffb2850u);
        }
        active_cpu().set_pc(fn);
        redirected = true;
        break;
    }
    case ROM_SFP: {
        u32 lock_handle = 0;
        read(0x3ffae0ac, 4, lock_handle, false);
        u32 idx = 0;
        read(0x3ffae1fc, 4, idx, false);
        u32 fp = 0x3ffae290u + (idx % 3u) * 0x68u;
        write(0x3ffae1fc, 4, idx + 1);
        for (u32 off = 0; off < 0x68; off += 4) write(fp + off, 4, 0);
        write(fp + 12, 2, 1);
        write(fp + 28, 4, fp);
        write(fp + 88, 4, lock_handle);
        ret = fp;
        break;
    }
    case ROM_SFVWRITE_R: {
        u32 fp = b, uio = c;
        u32 iov = 0, iovcnt = 0;
        read(uio + 0, 4, iov, false);
        read(uio + 4, 4, iovcnt, false);
        u32 file_no = 1;
        if (fp) read(fp + 14, 2, file_no, false);
        for (u32 i = 0; i < iovcnt && i < 64; ++i) {
            u32 base = 0, len = 0;
            read(iov + i * 8, 4, base, false);
            read(iov + i * 8 + 4, 4, len, false);
            for (u32 j = 0; j < len && j < 65536; ++j) {
                u32 ch = 0;
                read(base + j, 1, ch, false);
                if (file_no == 1 || file_no == 2 || file_no == 0xffff)
                    uart_transmit(0, (u8)ch);
            }
        }
        write(uio + 8, 4, 0);
        ret = 0;
        break;
    }
    case ROM_SWRITE: {
        u32 buf = c, n = active_cpu().rom_argument(3);
        for (u32 i = 0; i < n && i < 65536; ++i) {
            u32 ch = 0;
            read(buf + i, 1, ch, false);
            uart_transmit(0, (u8)ch);
        }
        ret = n;
        break;
    }
    case ROM_SREFILL_R:
        ret = 0xffffffffu;
        break;
    case ROM_LOCALECONV_R: {
        u32 lconv = 0x3ffae180u, dot = 0x3ffae1f8u;
        write(dot, 2, '.');
        for (u32 i = 0; i < 14; ++i) write(lconv + i * 4, 4, i == 0 ? dot : (dot + 1));
        ret = lconv;
        break;
    }
    case ROM_STRCASECMP:
    case ROM_STRNCASECMP: {
        u32 limit = entry->operation == ROM_STRCASECMP ? 65536u : c;
        ret = 0;
        for (u32 i = 0; i < limit; ++i) {
            u32 c1 = 0, c2 = 0;
            if (!read(a + i, 1, c1, false) || !read(b + i, 1, c2, false)) { ok = false; break; }
            if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
            if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
            if (c1 != c2) { ret = (u32)((s32)c1 - (s32)c2); break; }
            if (!c1) break;
        }
        break;
    }
    case ROM_STRLCAT: {
        std::string dst_s, src_s;
        if (!(ok = rom_string(a, dst_s) && rom_string(b, src_s))) break;
        size_t dlen = dst_s.size();
        if (dlen >= (size_t)c) {
            ret = (u32)((size_t)c + src_s.size());
            break;
        }
        size_t copy_n = src_s.size() < (size_t)c - dlen - 1 ? src_s.size() : (size_t)c - dlen - 1;
        for (size_t i = 0; i < copy_n && ok; ++i)
            ok = write(a + (u32)(dlen + i), 1, (u8)src_s[i]);
        if (ok) ok = write(a + (u32)(dlen + copy_n), 1, 0);
        ret = (u32)(dlen + src_s.size());
        break;
    }
    case ROM_STRTOL:
    case ROM_STRTOUL: {
        std::string s;
        if (!(ok = rom_string(a, s))) break;
        char *endp = 0;
        if (entry->operation == ROM_STRTOL)
            ret = (u32)strtol(s.c_str(), &endp, (int)b);
        else
            ret = (u32)strtoul(s.c_str(), &endp, (int)b);
        if (c) ok = write(c, 4, a + (u32)(endp - s.c_str()));
        break;
    }
    case ROM_NEGSF2:
        ret = a ^ 0x80000000u;
        break;
    }
    ++rom_calls_;
    if(ok) { if(!redirected)active_cpu().rom_return(ret);result=XTENSA_OK; }
    else result=XTENSA_MEMORY_FAULT;
    return true;
}

XtensaResult EspMachine::run(u64 max_steps)
{
    for(u64 i=0;i<max_steps;++i) { XtensaResult r=step();if(r!=XTENSA_OK)return r; }
    return XTENSA_OK;
}
