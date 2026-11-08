#ifndef BK_ESP_UART_H
#define BK_ESP_UART_H

#include "common.h"

/* Shared ESP UART register/FIFO model. Time is measured in APB clock ticks;
 * transmitted bytes reach the host synchronously, receive timeouts do not. */
class EspUart {
public:
    EspUart();
    void reset(bool esp32,bool keep_output=false);
    bool read(u32 offset, u64 ticks, unsigned apb_mhz, u32 &value);
    bool write(u32 offset, u64 ticks, unsigned apb_mhz, u32 value);
    void receive(const Bytes &bytes, u64 ticks, unsigned apb_mhz);
    void transmit(u8 byte, u64 ticks, unsigned apb_mhz);
    void advance(u64 ticks, unsigned apb_mhz);
    bool interrupt() const;
    bool next_event_ticks(u64 ticks, unsigned apb_mhz, u64 &delta) const;
    const std::string &output() const { return output_; }
private:
    bool esp32_, timeout_armed_;
    u64 received_at_;
    u32 registers_[32], raw_;
    std::deque<u8> rx_;
    std::string output_;
    u64 timeout_ticks(unsigned apb_mhz) const;
    void update_fifo_flags();
    bool valid_offset(u32 offset) const;
};

#endif
