#ifndef BK_ESP_SPI_H
#define BK_ESP_SPI_H
#include "esp/esp_image.h"

/* Shared SPI0/SPI1 NOR protocol. The chip supplies the register layout; the
 * flash bytes and controller state remain owned by its machine. */
namespace esp_spi {
enum Result { UNHANDLED, OK, ERROR };
/* Off-chip protocol state survives a CPU/digital/RTC reset of the SoC. */
bool external_state_register(u32 address);
Result read(esp::Chip chip, std::map<u32,u32> &registers, const Bytes &flash,
            u32 address, u32 &value, std::string *error = 0);
Result write(esp::Chip chip, std::map<u32,u32> &registers, Bytes &flash,
             u32 address, u32 value, std::string *error = 0, Bytes *psram = 0);
}
#endif
