#include "emulator_backend.h"
#include "esp/esp_machine.h"
#include "netstack.h"
#include "wifipeer.h"
#include "tuyamcu.h"
#include "platform.h"
#include <set>

namespace {
class EspBackend : public EmulatorBackend {
    struct AirBridge : public Esp8266WifiDma::Air {
        EspBackend *owner;
        AirBridge() : owner(0) {}
        bool transmit(const Esp8266WifiDma::Frame &frame) {
            if (owner && owner->wifi_peer_) owner->wifi_peer_->on_device_tx(frame.bytes);
            return true;
        }
    };
    EspMachine machine_;
    BackendInfo info_;
    BuildOptions options_;
    volatile long stopping_, paused_;
    mutable Mutex control_;
    BackendStatus published_;
    Bytes pending_uart_[3];
    std::vector<std::pair<int, int> > pending_gpio_;
    XtensaResult result_;
    std::set<u32> trace_;
    size_t sent_[3];
    void (*sink_)(void *, int, u8);
    void *sink_context_;
    AirBridge air_;
    WifiPeer *wifi_peer_;
    netstack::EthPcap *ip_pcap_;
    tuyamcu::Slave *tuyamcu_;
    bool uart_at_line_start_;

    void net_log(const std::string &msg) {
        Bytes data = Bytes(msg) + "\n";
        if (sink_) {
            Bytes all = Bytes("\n") + data;
            for (size_t i = 0; i < all.size(); ++i)
                sink_(sink_context_, 0, (u8)all[i]);
            return;
        }
        if (!uart_at_line_start_) {
            data = Bytes("\n") + data;
            uart_at_line_start_ = true;
        }
        out_write(data);
        out_flush();
    }
    static void wifi_inject(void *ctx, const Bytes &frame, int) {
        EspBackend *b = (EspBackend *)ctx;
        Esp8266WifiDma::RxInfo info;
        info.rate = 11;
        info.channel = 1;
        info.rssi_dbm = -40;
        b->machine_.receive_wifi(frame, info, 0);
    }
    static void wifi_net_log(void *ctx, const std::string &msg) {
        ((EspBackend *)ctx)->net_log(msg);
    }
    static u64 wifi_now_us(void *ctx) {
        return ((EspBackend *)ctx)->machine_.elapsed_us();
    }
    static void wifi_on_frame(void *ctx, const std::string &, const Bytes &frame, u64 now_us) {
        EspBackend *b = (EspBackend *)ctx;
        if (b->ip_pcap_) b->ip_pcap_->write(frame, now_us);
    }

    void publish_status() {
        BackendStatus out;
        out.pc = machine_.cpu(machine_.active_core()).pc();
        out.instructions = machine_.instructions();
        out.elapsed_us = machine_.elapsed_us();
        out.elapsed_us_known = true;
        out.faulted = result_ != XTENSA_OK && result_ != XTENSA_WAIT;
        out.diagnostic = machine_.diagnostic();
        if (out.faulted && out.diagnostic.empty())
            out.diagnostic = strf("%s execution stopped (result %d) at PC 0x%08x, instruction 0x%06x",
                info_.chip.c_str(), (int)result_, out.pc, machine_.cpu(machine_.active_core()).last_instruction());
        for (unsigned pin = 0; pin < info_.gpio_pins; ++pin) {
            BackendStatus::Gpio state;
            if (machine_.gpio_state(pin, state.input, state.output, state.enabled))
                out.gpio[(int)pin] = state;
        }
        ScopedLock lock(control_);
        published_ = out;
    }
    void apply_inputs() {
        ScopedLock lock(control_);
        for (unsigned port = 0; port < 3; ++port) {
            if (!pending_uart_[port].empty()) {
                machine_.receive_uart(port, pending_uart_[port]);
                pending_uart_[port].clear();
            }
        }
        for (std::vector<std::pair<int, int> >::const_iterator i = pending_gpio_.begin(); i != pending_gpio_.end(); ++i)
            machine_.set_gpio_input(i->first, i->second);
        pending_gpio_.clear();
    }

    void flush_uart() {
        for (unsigned port = 0; port < 3; ++port) {
            const std::string &bytes = machine_.uart(port);
            if (bytes.size() <= sent_[port]) continue;
            Bytes chunk = bytes.substr(sent_[port]);
            if (port == 0 && tuyamcu_) {
                Bytes reply = tuyamcu_->react_stream(chunk);
                if (!reply.empty()) machine_.receive_uart(0, reply);
            }
            if (sink_) {
                for (; sent_[port] < bytes.size(); ++sent_[port])
                    sink_(sink_context_, (int)port, (u8)bytes[sent_[port]]);
            } else {
                out_write(chunk);
                uart_at_line_start_ = (u8)chunk[chunk.size() - 1] == 0x0A;
                if (uart_at_line_start_) out_flush();
                sent_[port] = bytes.size();
            }
        }
    }
public:
    explicit EspBackend(const BuildOptions &options)
        : machine_(options.chip == "ESP32" ? esp::CHIP_ESP32 : esp::CHIP_ESP8266),
          options_(options), stopping_(0), paused_(0), result_(XTENSA_OK), sink_(0), sink_context_(0),
          wifi_peer_(0), ip_pcap_(0), tuyamcu_(0), uart_at_line_start_(true) {
        backend_info(options.chip, info_);
        sent_[0] = sent_[1] = sent_[2] = 0;
        air_.owner = this;
    }
    ~EspBackend() {
        if (wifi_peer_) {
            wifi_peer_->close();
            delete wifi_peer_;
        }
        delete ip_pcap_;
        delete tuyamcu_;
    }
    bool load(const Bytes &bytes, const esp::FlashImage &layout, std::string *err) {
        if (options_.psram_mb > 8 || !machine_.set_psram_size(options_.psram_mb*0x100000)) {
            if (err) *err = "External PSRAM requires ESP32 and a size of 0, 2, 4 or 8 MiB.";
            return false;
        }
        if (options_.xtal_mhz && !machine_.set_xtal_frequency(options_.xtal_mhz)) {
            if (err) *err = "Invalid crystal frequency for " + info_.chip + ".";
            return false;
        }
        if (!machine_.load(bytes, layout, options_.with_boot, err)) return false;
        if (options_.wifi) machine_.set_wifi_air(&air_);
        if (options_.tuyamcu || !options_.tuyamcu_dps.empty() || !options_.tuyamcu_injects.empty()) {
            std::string pid = options_.tuyamcu_pid.empty() ? "bekenemulator000" : options_.tuyamcu_pid;
            tuyamcu_ = new tuyamcu::Slave(pid, "1.0.0", options_.tuyamcu_raw);
            for (size_t i = 0; i < options_.tuyamcu_dps.size(); ++i)
                tuyamcu_->set_dp(options_.tuyamcu_dps[i].first, options_.tuyamcu_dps[i].second.type,
                                 options_.tuyamcu_dps[i].second.ivalue, options_.tuyamcu_dps[i].second.svalue);
            for (size_t i = 0; i < options_.tuyamcu_injects.size(); ++i)
                tuyamcu_->add_inject(options_.tuyamcu_injects[i]);
        }
        if (!options_.rom_path.empty()) {
            Bytes rom;
            if (!read_file(options_.rom_path, rom)) {
                if (err) *err = "Cannot read the supplied silicon ROM image.";
                return false;
            }
            if (!machine_.load_rom(rom, err)) return false;
        }
        for (std::map<int, int>::const_iterator i = options_.gpio_inputs.begin(); i != options_.gpio_inputs.end(); ++i) {
            if (!machine_.set_gpio_input(i->first, i->second)) {
                if (err) *err = strf("Invalid %s GPIO input pin %d or level %d.",
                    info_.chip.c_str(), i->first, i->second);
                return false;
            }
        }
        publish_status();
        return true;
    }
    const BackendInfo &info() const { return info_; }
    bool setup(std::string *) { return true; }
    void run(bool wait_for_input) {
        long long configured = 0;
        u64 limit = 0;
        if (parse_int(env_get("EMU_MAX_INSNS"), 0, configured) && configured > 0)
            limit = (u64)configured;
        bool fed = options_.uart1_rx.empty();
        u64 delay = options_.uart1_rx_delay_set ? options_.uart1_rx_delay : CONSOLE_RX_HOLDOFF_INSNS;
        if (!options_.only_uart)
            out_write(strf("Starting %s (%s) at 0x%08x...\n", info_.chip.c_str(),
                info_.architecture.c_str(), machine_.cpu().pc()));
        while (!atomic_get(&stopping_) && (!limit || machine_.instructions() < limit)) {
            const bool paused = atomic_get(&paused_) != 0;
            if ((machine_.instructions() & 1023) == 0 || result_ == XTENSA_WAIT || paused) {
                apply_inputs();
                publish_status();
            }
            if (paused) {
                sleep_ms(2);
                continue;
            }
            if (!fed && machine_.instructions() >= delay) {
                machine_.receive_uart(0, options_.uart1_rx);
                fed = true;
            }
            u32 pc = machine_.cpu(machine_.active_core()).pc();
            if (trace_.erase(pc))
                out_write(strf("[EMU_PC] 0x%08x\n", pc));
            result_ = machine_.step();
            if ((machine_.instructions() & 1023) == 0 || result_ != XTENSA_OK) {
                flush_uart();
                if (wifi_peer_) wifi_peer_->tick(machine_.instructions());
            }
            if (result_ == XTENSA_WAIT && wait_for_input) sleep_ms(1);
            else if (result_ != XTENSA_OK) break;
        }
        if (wifi_peer_) wifi_peer_->tick(machine_.instructions());
        flush_uart();
        publish_status();
        BackendStatus current = status();
        if (current.faulted)
            out_write("\n[!] " + current.diagnostic + "\n");
        if (env_has("EMU_REPORT"))
            out_write(strf("[EMU_INSNS] %llu\n", (unsigned long long)current.instructions));
        if (!options_.only_uart)
            out_write(strf("Emulation finished. PC: 0x%08x, instructions: %llu\n", current.pc,
                (unsigned long long)current.instructions));
        out_flush();
    }
    void stop() { atomic_set(&stopping_, 1); }
    void pause(bool paused) { atomic_set(&paused_, paused ? 1 : 0); }
    bool is_paused() const { return atomic_get(const_cast<volatile long *>(&paused_)) != 0; }
    BackendStatus status() const {
        ScopedLock lock(control_);
        return published_;
    }
    void trace_pc(u32 address) { trace_.insert(address); }
    void set_uart_sink(void (*sink)(void *, int, u8), void *ctx) {
        sink_ = sink;
        sink_context_ = ctx;
    }
    bool feed_uart(unsigned port, const Bytes &bytes) {
        if (port >= 3 || !(info_.uart_rx_mask & (1u << port))) return false;
        ScopedLock lock(control_);
        pending_uart_[port] += bytes;
        return true;
    }
    bool set_gpio_input(int pin, int level) {
        if (pin < 0 || (unsigned)pin >= info_.gpio_pins
            || level < -1 || level > 1) return false;
        ScopedLock lock(control_);
        if(!published_.gpio.count(pin))return false;
        pending_gpio_.push_back(std::make_pair(pin, level));
        return true;
    }
    void attach_wifi_peer(const std::string &role, const std::string &ssid, int channel,
                          const Bytes &peer_mac, const std::string &passphrase,
                          const std::vector<std::pair<int, int> > &forwards, bool net_out,
                          const std::string &pcap, const StrList &http_gets,
                          const std::vector<std::pair<std::string, Bytes> > &mqtt_pubs,
                          const std::vector<Bytes> &tls_psks, bool tuya_legacy_identity_psk) {
        machine_.set_wifi_air(&air_);
        Bytes mac = peer_mac.empty() ? Bytes("\x02\x00\x00\x00\x00\x01", 6) : peer_mac;
        netstack::NetConfig cfg;
        cfg.mac = mac;
        cfg.gateway = (role == "ap");
        cfg.ip = (role == "ap") ? netstack::str2ip("192.168.1.1") : Bytes();
        cfg.net_out = net_out;
        cfg.forwards = forwards;
        cfg.http_gets = http_gets;
        cfg.mqtt_pubs = mqtt_pubs;
        cfg.tls_psks = tls_psks;
        cfg.tuya_legacy_identity_psk = tuya_legacy_identity_psk;
        netstack::NetStack *net = new netstack::NetStack(cfg, &EspBackend::wifi_net_log, this);
        net->on_frame = &EspBackend::wifi_on_frame;
        net->on_frame_ctx = this;
        wifi_peer_ = new WifiPeer(mac, net, &EspBackend::wifi_inject, this,
                                  &EspBackend::wifi_net_log, this, role.empty() ? "auto" : role,
                                  ssid, channel, pcap, &EspBackend::wifi_now_us, this,
                                  0, passphrase);
    }
    void set_pcap_ip(const std::string &path) {
        delete ip_pcap_;
        ip_pcap_ = new netstack::EthPcap(path);
        net_log(strf("[NET] writing the bridge's frames to %s", path.c_str()));
    }
};

bool validate_options(const BuildOptions &options, const KeyCoefs *key, std::string *err)
{
    const char *unsupported = 0;
    if (key) unsupported = "-key is a Beken flash cipher option; ESP images use their own image format.";
    else if (options.ble_core > 0) unsupported = "--ble-core selects Beken RivieraWaves hardware, unavailable on ESP.";
    else if (options.xvr_selfclear) unsupported = "--xvr-selfclear selects Beken RF hardware, unavailable on ESP.";
    else if (!options.adc_inputs.empty()) unsupported = "ESP ADC input is not implemented yet.";
    else if (!options.deep_sleep_wakes.empty()) unsupported = "ESP deep-sleep wake scheduling is not implemented yet.";
    else if (options.uart1_hex) unsupported = "ESP UART hex formatting is not implemented yet.";
    if (unsupported && err) *err = unsupported;
    return unsupported == 0;
}
}

EmulatorBackend *build_esp_backend(const std::string &path, const BuildOptions &options,
    const KeyCoefs *key, std::string *err, void (*log)(const std::string &),
    bool, std::string *exception_type)
{
    if (!validate_options(options, key, err)) return 0;
    Bytes bytes;
    if (!read_file(path, bytes)) {
        if (exception_type) *exception_type = "OSError";
        if (err) *err = options.wifi
            ? "Cannot read Wi-Fi firmware image '" + path + "'."
            : "Cannot read firmware image '" + path + "'.";
        return 0;
    }
    esp::FlashImage layout;
    esp::Chip chip = options.chip == "ESP32" ? esp::CHIP_ESP32 : esp::CHIP_ESP8266;
    if (!esp::parse_flash(bytes, chip, layout, err)) return 0;
    EspBackend *backend = new EspBackend(options);
    if (!backend->load(bytes, layout, err)) { delete backend; return 0; }
    if (log) log(strf("Loaded %s flash: %u image(s), %u partition(s).", options.chip.c_str(),
        (unsigned)layout.images.size(), (unsigned)layout.partitions.size()));
    return backend;
}
