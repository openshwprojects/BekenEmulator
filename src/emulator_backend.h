/* Architecture-neutral front-end boundary. Image decoding, CPU execution and
 * MMIO remain separate modules inside each backend. No CPU headers are needed
 * by a runner that only loads firmware, receives UART and inspects status. */
#ifndef EMULATOR_BACKEND_H
#define EMULATOR_BACKEND_H

#include "emulator_options.h"

struct KeyCoefs;
class BekenEmulator;

struct BackendInfo {
    std::string chip, family, architecture;
    unsigned gpio_pins, console_uart, uart_rx_mask;
    BackendInfo() : gpio_pins(0), console_uart(0), uart_rx_mask(0) {}
    BackendInfo(const std::string &c, const char *f, const char *a, unsigned pins,
                unsigned console, unsigned rx_mask)
        : chip(c), family(f), architecture(a), gpio_pins(pins), console_uart(console), uart_rx_mask(rx_mask) {}
};

struct BackendStatus {
    struct Gpio {
        bool input, output, enabled;
        Gpio() : input(false), output(false), enabled(false) {}
    };
    u32 pc;
    u64 instructions;
    u64 elapsed_us;
    bool elapsed_us_known;
    bool faulted;
    std::string diagnostic;
    std::map<int, Gpio> gpio;
    BackendStatus() : pc(0), instructions(0), elapsed_us(0), elapsed_us_known(false), faulted(false) {}
};

class EmulatorBackend {
public:
    virtual ~EmulatorBackend() {}
    virtual const BackendInfo &info() const = 0;
    virtual bool setup(std::string *err = 0) = 0;
    /* Interactive runners stay available for host input when all CPUs wait. */
    virtual void run(bool wait_for_input = false) = 0;
    virtual void stop() = 0;
    virtual void pause(bool paused) = 0;
    virtual bool is_paused() const = 0;
    virtual BackendStatus status() const = 0;
    virtual void trace_pc(u32 address) = 0;
    virtual void set_uart_sink(void (*sink)(void *, int, u8), void *ctx) = 0;
    /* UART numbers follow the chip's hardware names (ESP UART0, Beken UART1).
     * False means that port/pin cannot accept the requested input. */
    virtual bool feed_uart(unsigned port, const Bytes &bytes) = 0;
    virtual bool set_gpio_input(int pin, int level) = 0;
    virtual void attach_wifi_peer(const std::string & /*role*/, const std::string & /*ssid*/, int /*channel*/,
                                  const Bytes & /*peer_mac*/, const std::string & /*passphrase*/,
                                  const std::vector<std::pair<int, int> > & /*forwards*/, bool /*net_out*/,
                                  const std::string & /*pcap*/, const StrList & /*http_gets*/,
                                  const std::vector<std::pair<std::string, Bytes> > & /*mqtt_pubs*/,
                                  const std::vector<Bytes> & /*tls_psks*/, bool /*tuya_legacy_identity_psk*/) {}
    virtual void set_pcap_ip(const std::string & /*path*/) {}
    /* Compatibility access for existing Beken-only network/UI controls. New
     * front ends use the common interface; an ESP backend never instantiates
     * ARM or the Beken memory/peripheral state. */
    virtual BekenEmulator *beken() { return 0; }
};

StrList backend_chip_names();
bool backend_info(const std::string &chip, BackendInfo &info);
EmulatorBackend *build_backend(const std::string &path, const BuildOptions &options,
    const KeyCoefs *key, std::string *err = 0,
    void (*log)(const std::string &) = 0, bool save_decrypted = false,
    std::string *exception_type = 0);

#endif
