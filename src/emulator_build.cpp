/* build_emulator (dump -> configured emulator) and the network attach/close
 * glue. Port of build_emulator from main.py. */
#include "emulator.h"
#include "netstack.h"
#include "wifipeer.h"
#include "wifimac.h"

#include <algorithm>
#include <errno.h>
#include <string.h>

using namespace pyc; /* main.py str/int/OSError semantics */

BekenEmulator *build_emulator(const std::string &dump_path, const BuildOptions &opt_in,
                              const KeyCoefs *key, std::string *err, void (*log)(const std::string &),
                              bool save_decrypted, std::string *exc_type)
{
    BuildOptions opt = opt_in;
    if (exc_type)
        *exc_type = "ValueError";
    const Chip *chip_def = get_chip(opt.chip, err);
    if (!chip_def)
        return 0;
    ChipId chip;
    chip.chip = chip_def->chip_id;
    chip.device = chip_def->dev_id;

    Bytes raw_data;
    {
        /* open(dump_path, "rb"): an OSError that main.py does not catch */
        int e = 0;
        FILE *f = py_open(dump_path, "rb", &e);
        if (f)
            fclose(f);
        if (!f || !read_file(dump_path, raw_data)) {
            std::string msg = py_oserror(f ? EIO : e, dump_path, exc_type);
            if (err)
                *err = msg;
            return 0;
        }
    }

    Bytes flash_data, bootloader, app, physical_data;
    bool have_physical = true;
    u32 vector_base = 0;

    /* BK7252 app-only detection */
    Bytes app_window;
    size_t encoded_size = 0;
    bool app_only = false;
    if (chip_def->app_only_phys >= 0)
        app_only = detect_bk7252_app_only(raw_data, app_window, encoded_size);

    if (app_only) {
        if (key && key->has) {
            if (err)
                *err = "Detected a plaintext BK7252 app-only dump; do not pass -key.";
            return 0;
        }
        if (opt.with_boot) {
            if (err)
                *err = "This BK7252 dump starts at physical flash 0x11000 and has no bootloader; remove --with-boot.";
            return 0;
        }
        app = app_window;
        bootloader.clear();
        u32 base = (u32)chip_def->app_only_cpu();
        if (base + app.size() > BekenEmulator::FLASH_SIZE) {
            if (err)
                *err = "Decoded BK7252 application does not fit the 2 MiB XIP window.";
            return 0;
        }
        Bytes logical(BekenEmulator::FLASH_SIZE, (char)0xFF);
        for (size_t i = 0; i < app.size(); i++)
            logical[base + i] = app[i];
        flash_data = logical;
        physical_data = Bytes((size_t)chip_def->app_only_phys, (char)0xFF) + raw_data;
        vector_base = base;
        if (log) {
            log(strf("Detected BK7252 app-only dump: file offset 0 is physical 0x%X / CPU 0x%X.",
                     (unsigned)chip_def->app_only_phys, (unsigned)base));
            log(strf("Decoded CRC-framed app prefix: %d bytes from %d encoded bytes.", (int)app.size(), (int)encoded_size));
        }
    } else {
        flash_data = strip_crcs(raw_data);
        KeyCoefs k;
        if (key)
            k = *key;
        const char *targets[2] = {"bootloader", "app"};
        for (int t = 0; t < 2; t++) {
            std::string xerr; /* crypto.py raising instead of returning */
            Bytes payload = extract_and_decrypt(flash_data, targets[t], k, log, &xerr,
                                                chip_def->bootloader_addr, chip_def->app_addr);
            if (!xerr.empty()) {
                size_t colon = xerr.find(": ");
                if (exc_type)
                    *exc_type = xerr.substr(0, colon);
                if (err)
                    *err = xerr.substr(colon + 2);
                return 0;
            }
            (t ? app : bootloader) = payload;
        }
        physical_data = raw_data;
    }
    if (!fit_flash(physical_data, *chip_def, physical_data, err))
        return 0;

    if (!app.empty() && save_decrypted && !env_has("EMU_REPORT"))
        write_file("app_decrypted.bin", app);

    /* extract_and_decrypt always returns bytes (its fallback is a raw slice,
     * possibly empty), so Python's "is None" branches are only reachable for
     * the app-only bootloader. */
    if (app_only) {
        if (log)
            log(strf("Bootloader region is absent; starting the app directly at 0x%X.", (unsigned)chip_def->app_addr));
    } else if (log)
        log(strf("Loaded 'bootloader' payload, size: %d bytes", (int)bootloader.size()));
    if (log)
        log(strf("Loaded 'app' payload, size: %d bytes", (int)app.size()));

    if (!opt.uart1_rx_delay_set) {
        opt.uart1_rx_delay = opt.tuyamcu ? 0 : CONSOLE_RX_HOLDOFF_INSNS;
        opt.uart1_rx_delay_set = true;
    }
    /* check_tuyamcu_pid: the MCU sends the product id as ASCII on the wire */
    if (!opt.tuyamcu_pid.empty() && !py_isascii(opt.tuyamcu_pid)) {
        if (err)
            *err = "bad --tuyamcu-pid " + py_repr(opt.tuyamcu_pid) + " (a Tuya product id is plain ASCII)";
        return 0;
    }

    BekenEmulator *emu = new BekenEmulator(flash_data, bootloader, app, opt, chip,
                                           have_physical ? &physical_data : 0, chip_def);
    if (!emu->init_error.empty()) {
        /* the constructor's ValueError (e.g. a --gpio-input pin out of range) */
        if (err)
            *err = emu->init_error;
        delete emu;
        return 0;
    }
    if (vector_base != 0)
        emu->set_exception_vector_base(vector_base);
    (void)have_physical;
    return emu;
}

/* -------------------------------------------------------------- network */
void BekenEmulator::close_network()
{
    if (wifi_peer_) {
        if (wifimac_)
            wifimac_->on_tx = 0;
        wifi_peer_->close();
        delete wifi_peer_;
        wifi_peer_ = 0;
    }
}

void BekenEmulator::set_pcap_ip(const std::string &path)
{
    delete ip_pcap_;
    ip_pcap_ = new netstack::EthPcap(path);
    net_log(strf("[NET] writing the bridge's frames to %s", path.c_str()));
}

/* ----- callback trampolines (ctx = the BekenEmulator) ----- */
void BekenEmulator::wifi_on_tx(void *ctx, const Bytes &frame, const TxInfo &info) {
    (void)info;
    BekenEmulator *e = (BekenEmulator *)ctx;
    if (e->wifi_peer_) e->wifi_peer_->on_device_tx(frame);
}
void BekenEmulator::wifi_inject(void *ctx, const Bytes &frame, int decr) {
    BekenEmulator *e = (BekenEmulator *)ctx;
    if (e->wifimac_) e->wifimac_->inject(frame, e->state.insn_count, -45, 0, decr, -1);
}
void BekenEmulator::wifi_net_log(void *ctx, const std::string &msg) {
    ((BekenEmulator *)ctx)->net_log(msg);
}
u64 BekenEmulator::wifi_now_us(void *ctx) {
    return ((BekenEmulator *)ctx)->network_now_us();
}
void BekenEmulator::wifi_on_frame(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us) {
    BekenEmulator *e = (BekenEmulator *)ctx;
    if (e->ip_pcap_) e->ip_pcap_->write(frame, now_us);
    if (e->net_frame_sink) e->net_frame_sink(e->net_frame_sink_ctx, dir, frame, now_us);
}

WifiPeer *BekenEmulator::attach_wifi_peer(const std::string &role, const std::string &ssid, int channel,
                                          const Bytes &peer_mac, const std::string &passphrase,
                                          const std::vector<std::pair<int, int> > &forwards, bool net_out,
                                          const std::string &pcap, const StrList &http_gets,
                                          const std::vector<std::pair<std::string, Bytes> > &mqtt_pubs,
                                          const std::vector<Bytes> &tls_psks, bool tuya_legacy_identity_psk)
{
    if (!wifimac_)
        return 0;
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
    netstack::NetStack *net = new netstack::NetStack(cfg, &BekenEmulator::wifi_net_log, this);
    net->on_frame = &BekenEmulator::wifi_on_frame;
    net->on_frame_ctx = this;
    WifiPeer *peer = new WifiPeer(mac, net, &BekenEmulator::wifi_inject, this,
                                  &BekenEmulator::wifi_net_log, this, role.empty() ? "auto" : role,
                                  ssid, channel, pcap, &BekenEmulator::wifi_now_us, this,
                                  NET_LATENCY_US, passphrase);
    wifimac_->on_tx = &BekenEmulator::wifi_on_tx;
    wifimac_->on_tx_ctx = this;
    wifi_peer_ = peer;
    return peer;
}
