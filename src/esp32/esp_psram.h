#ifndef BK_ESP_PSRAM_H
#define BK_ESP_PSRAM_H
#include "common.h"

namespace esp_psram {
bool external_state_register(u32 address);
bool qpi_enabled(const std::map<u32,u32> &registers);
bool command(std::map<u32,u32> &registers,Bytes &memory,u8 opcode,u32 address,
             bool quad_command,const Bytes &transmit,unsigned receive_size,
             Bytes &received,std::string *error=0);
}
#endif
