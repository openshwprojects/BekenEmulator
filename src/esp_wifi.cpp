#include "esp_machine.h"
#include "esp8266_interrupts.h"
#include "esp8266_wdev.h"

bool EspMachine::wifi_radio_state(WifiRadioState &state)
{
    state=WifiRadioState();
    if(chip_!=esp::CHIP_ESP8266)return false;
    state=esp8266_radio::snapshot(registers_,xtal_mhz_,elapsed_us());
    return true;
}

bool EspMachine::set_wifi_air(Esp8266WifiDma::Air *air)
{
    if(chip_!=esp::CHIP_ESP8266)return false;
    wifi_dma_.set_air(air);return true;
}

Esp8266WifiDma::ReceiveResult EspMachine::receive_wifi(const Bytes &frame,
    const Esp8266WifiDma::RxInfo &info,std::string *error)
{
    if(chip_!=esp::CHIP_ESP8266) {
        if(error)*error="ESP32 Wi-Fi receive DMA is not implemented yet";
        return Esp8266WifiDma::RX_INVALID;
    }
    const Esp8266WifiDma::ReceiveResult result=wifi_dma_.receive(registers_,frame,info,esp32_apb_ticks(),error);
    esp8266_interrupts::update_mac(registers_,cpu_);return result;
}
