/* The virtual 802.11 peer (station or AP) on the far side of the MAC model.
 * Port of src/wifipeer.py. Bridges 802.11 management/data frames to the
 * user-mode IP stack (netstack), running open-system or WPA2-PSK association. */
#include "wifipeer.h"
#include "netstack.h"
#include "wpa2.h"
#include "beken_crypto.h"
#include <stdio.h>
#include <string.h>
#include <set>
#include <map>

/* frame control subtypes */
static const int MGMT_ASSOC_REQ = 0x0, MGMT_ASSOC_RESP = 0x1, MGMT_PROBE_REQ = 0x4, MGMT_PROBE_RESP = 0x5;
static const int MGMT_BEACON = 0x8, MGMT_DISASSOC = 0xA, MGMT_AUTH = 0xB, MGMT_DEAUTH = 0xC, MGMT_ACTION = 0xD;
static const u16 FC_TODS = 0x0100, FC_FROMDS = 0x0200;
static const int IE_SSID = 0, IE_RATES = 1, IE_DS = 3, IE_TIM = 5, IE_ERP = 42, IE_EXT_RATES = 50, IE_RSN = 48;
static const char *RATES_BG = "\x82\x84\x8b\x96\x0c\x12\x18\x24";
static const char *EXT_RATES_BG = "\x30\x48\x60\x6c";
static const char *LLC_SNAP = "\xaa\xaa\x03\x00\x00\x00";
static const int BEACON_INTERVAL_TU = 100;
static const u64 TU_US = 1024;

const char *WIFI_ROLES[] = {"auto", "sta", "ap", 0};

static const char *FACTORY_TEST_SSIDS[] = {"tuya_mdev_test1", "tuya_mdev_test2", 0};

static Bytes BCAST6() { return Bytes("\xff\xff\xff\xff\xff\xff", 6); }
static std::string mac2str(const Bytes &b) {
    std::string s; for (size_t i = 0; i < b.size(); i++) s += strf(i ? ":%02x" : "%02x", (u8)b[i]); return s;
}
static Bytes le16(u16 v) { Bytes b(2, '\0'); b[0] = (char)(v & 0xFF); b[1] = (char)(v >> 8); return b; }
static u16 rdle16(const Bytes &b, size_t o) { return (u16)((u8)b[o] | ((u8)b[o + 1] << 8)); }
static u16 rdbe16_etype(const Bytes &b) { return (u16)(((u8)b[0] << 8) | (u8)b[1]); }
static Bytes ie(int eid, const Bytes &payload) { Bytes b; b += (char)eid; b += (char)payload.size(); b += payload; return b; }

struct IeEnt { int eid; Bytes val; };
static std::vector<IeEnt> parse_ies(const Bytes &data) {
    std::vector<IeEnt> out; size_t p = 0;
    while (p + 2 <= data.size()) {
        int eid = (u8)data[p], ln = (u8)data[p + 1];
        if (p + 2 + ln > data.size()) break;
        IeEnt e; e.eid = eid; e.val = data.substr(p + 2, ln); out.push_back(e); p += 2 + ln;
    }
    return out;
}
static bool ie_get(const std::vector<IeEnt> &ies, int eid, Bytes &out) {
    for (size_t i = 0; i < ies.size(); i++) if (ies[i].eid == eid) { out = ies[i].val; return true; }
    return false;
}
static void fc_fields(const Bytes &frame, int &ftype, int &subtype, u16 &fc) {
    fc = (u16)((u8)frame[0] | ((u8)frame[1] << 8));
    ftype = (fc >> 2) & 3; subtype = (fc >> 4) & 0xF;
}
static int hdr_len(u16 fc) {
    int ftype = (fc >> 2) & 3, subtype = (fc >> 4) & 0xF;
    int n = 24;
    if (ftype == 2) { if ((fc & 0x0300) == 0x0300) n += 6; if (subtype & 0x8) n += 2; if (fc & 0x8000) n += 4; }
    else if (ftype == 1) n = (subtype == 0x4 || subtype == 0x5 || subtype == 0x6 || subtype == 0x8 || subtype == 0x9 || subtype == 0xA || subtype == 0xB) ? 16 : 10;
    else if (fc & 0x8000) n += 4;
    return n;
}

/* ------------------------------------------------------------- Pcap80211 */
Pcap80211::Pcap80211(const std::string &path) {
    f_ = pyc::py_open(path, "wb", 0); /* a UTF-8 path, as Python open() takes it */
    if (f_) { char h[24]; wr32le(h, 0xA1B2C3D4); wr16le(h + 4, 2); wr16le(h + 6, 4); wr32le(h + 8, 0); wr32le(h + 12, 0); wr32le(h + 16, 65535); wr32le(h + 20, 105); fwrite(h, 1, 24, f_); fflush(f_); }
}
Pcap80211::~Pcap80211() { close(); }
void Pcap80211::write(const Bytes &frame, u64 us) {
    if (!f_) return;
    char h[16]; wr32le(h, (u32)(us / 1000000)); wr32le(h + 4, (u32)(us % 1000000)); wr32le(h + 8, (u32)frame.size()); wr32le(h + 12, (u32)frame.size());
    fwrite(h, 1, 16, f_); fwrite(frame.data(), 1, frame.size(), f_); fflush(f_);
}
void Pcap80211::close() { if (f_) fclose(f_); f_ = 0; }
bool Pcap80211::is_open() const { return f_ != 0; }

/* ------------------------------------------------------------- WifiPeer::Impl */
struct Bss { bool valid; Bytes bssid; std::string ssid; int channel; int caps; bool privacy; Bytes rsn; bool has_rsn; int bint; Bss() : valid(false) {} };
struct Station { std::string state; int aid; bool encrypted; Station() : aid(0), encrypted(false) {} };
struct OutItem { u64 release_us; Bytes frame; int decr; };

/* A real netstack::NetStack behind the WifiNet interface; owns the stack. */
class NetStackNet : public WifiNet {
public:
    explicit NetStackNet(netstack::NetStack *n) : n_(n) {}
    ~NetStackNet() { delete n_; }
    void set_output(OutputFn fn, void *ctx) { n_->output = fn; n_->output_ctx = ctx; }
    void input(const Bytes &eth) { n_->input(eth); }
    void link_up(const Bytes &device_mac) { n_->link_up(device_mac); }
    void tick(u64 now_us) { n_->tick(now_us); }
    void device_reset(u64 now_us) { n_->device_reset(now_us); }
    void close() { n_->close(); }
    void set_gateway(bool gw) { n_->gateway = gw; }
    bool has_ip() { return !n_->ip.empty(); }
    void set_ip(const Bytes &ip) { n_->ip = ip; }
private:
    netstack::NetStack *n_;
};

struct WifiPeer::Impl {
    Bytes mac;
    WifiNet *net; bool own_net;
    u64 stats_tx, stats_rx;
    std::vector<std::pair<std::string, u64> > events;
    WifiPeer::InjectFn inject; void *inject_ctx;
    WifiPeer::LogFn logfn; void *log_ctx;
    WifiPeer::NowFn nowfn; void *now_ctx;
    std::string passphrase; bool have_pass;
    std::map<Bytes, wpa2::Authenticator *> auth;
    std::string role, active_role; bool have_active;
    std::string ssid; bool have_ssid;
    int channel;
    Pcap80211 *pcap; bool own_pcap;
    int seq;
    Bytes ap_bssid;
    Bss bss;
    std::string sta_state; u64 sta_next_us; int sta_tries; int aid;
    std::map<Bytes, Station> stations; int next_aid;
    bool last_beacon_valid; u64 last_beacon_us;
    bool tsf_start_valid; u64 tsf_start;
    std::set<std::string> skipped_factory;
    u64 latency_us;
    std::vector<OutItem> outq;

    void log(const std::string &m) { if (logfn) logfn(log_ctx, m); }
    u64 now() { return nowfn ? nowfn(now_ctx) : 0; }
    u16 seqf() { int s = seq; seq = (seq + 1) & 0xFFF; return (u16)(s << 4); }
    void send(const Bytes &frame, int decr = -1);
    void deliver(const Bytes &frame, int decr);
    void release(u64 now);
    Bytes mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body);
    Bytes data_frame(const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &payload, bool to_ds);
    void send_eth(const Bytes &eth);
    bool connected();
    void pick_role(const std::string &role_, const std::string &why);
    void on_mgmt(int subtype, u16 fc, const Bytes &frame);
    void on_data(int subtype, u16 fc, const Bytes &frame);
    void sta_learn_bss(const Bytes &sa, const Bytes &bssid, const Bytes &body, int subtype);
    void sta_mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body);
    void sta_auth(); void sta_assoc(); void sta_probe(); void sta_tick(u64 now);
    void eapol_to(const Bytes &sta, const Bytes &payload);
    void start_handshake(const Bytes &sta);
    bool on_eapol(const Bytes &sta, const Bytes &payload);
    int decr_for(const Bytes &dst);
    int ap_caps();
    Bytes ap_ies(bool probe);
    u64 ap_tsf(u64 now);
    void ap_beacon(u64 now);
    void ap_mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body);
    void ap_tick(u64 now);
};


void WifiPeer::Impl::send(const Bytes &frame, int decr) {
    stats_tx++;
    if (latency_us) { OutItem o; o.release_us = now() + latency_us; o.frame = frame; o.decr = decr; outq.push_back(o); return; }
    deliver(frame, decr);
}
void WifiPeer::Impl::deliver(const Bytes &frame, int decr) {
    if (pcap) pcap->write(frame, now());
    if (inject) inject(inject_ctx, frame, decr);
}
void WifiPeer::Impl::release(u64 nowv) {
    if (outq.empty() || outq[0].release_us > nowv) return;
    std::vector<OutItem> due, keep;
    for (size_t i = 0; i < outq.size(); i++) { if (outq[i].release_us <= nowv) due.push_back(outq[i]); else keep.push_back(outq[i]); }
    outq = keep;
    for (size_t i = 0; i < due.size(); i++) deliver(due[i].frame, due[i].decr);
}
Bytes WifiPeer::Impl::mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body) {
    u16 fc = (u16)(subtype << 4);
    return le16(fc) + le16(0) + da + sa + bssid + le16(seqf()) + body;
}
Bytes WifiPeer::Impl::data_frame(const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &payload, bool to_ds) {
    u16 fc; Bytes a1, a2, a3;
    if (to_ds) { fc = (2 << 2) | FC_TODS; a1 = bssid; a2 = sa; a3 = da; }
    else { fc = (2 << 2) | FC_FROMDS; a1 = da; a2 = bssid; a3 = sa; }
    return le16(fc) + le16(0) + a1 + a2 + a3 + le16(seqf()) + payload;
}
void WifiPeer::Impl::send_eth(const Bytes &eth) {
    Bytes dst = eth.substr(0, 6), src = eth.substr(6, 6), etype = eth.substr(12, 2);
    Bytes payload = Bytes(LLC_SNAP, 6) + etype + eth.substr(14);
    if (active_role == "sta") {
        if (!bss.valid || sta_state != "RUN") return;
        send(data_frame(dst, src, bss.bssid, payload, true));
    } else if (active_role == "ap") {
        if (stations.empty() && !((u8)dst[0] & 1)) return;
        send(data_frame(dst, src, ap_bssid, payload, false), decr_for(dst));
    }
}
bool WifiPeer::Impl::connected() {
    if (active_role == "sta") return sta_state == "RUN";
    if (active_role == "ap") { for (std::map<Bytes, Station>::iterator it = stations.begin(); it != stations.end(); ++it) if (it->second.state == "ASSOC") return true; return false; }
    return false;
}
void WifiPeer::Impl::pick_role(const std::string &role_, const std::string &why) {
    if (role == "auto" && have_active && active_role != role_ && !connected()) {
        log("[WIFI] device changed role (" + why + ")");
        have_active = false; active_role.clear(); bss = Bss(); sta_state = "SCAN"; stations.clear();
    }
    if (!have_active) {
        have_active = true; active_role = role_;
        log(strf("[WIFI] device acts as %s (%s): peer takes the %s role",
                 role_ == "sta" ? "an access point" : "a station", why.c_str(),
                 role_ == "sta" ? "station" : "access point"));
        if (net) {
            net->set_gateway(role_ == "ap");
            if (role_ == "ap" && !net->has_ip()) net->set_ip(Bytes("\xc0\xa8\x01\x01", 4));
            if (role_ == "sta") net->set_ip(Bytes());
        }
    }
}
void WifiPeer::Impl::on_mgmt(int subtype, u16 fc, const Bytes &frame) {
    (void)fc;
    Bytes da = frame.substr(4, 6), sa = frame.substr(10, 6), bssid = frame.substr(16, 6);
    Bytes body = frame.substr(24);
    if (subtype == MGMT_BEACON || subtype == MGMT_PROBE_RESP) {
        pick_role("sta", std::string("it sends ") + (subtype == MGMT_BEACON ? "beacons" : "probe responses"));
        if (active_role == "sta") sta_learn_bss(sa, bssid, body, subtype);
        return;
    }
    if (subtype == MGMT_PROBE_REQ) pick_role("ap", "it probes for networks");
    else if (subtype == MGMT_ASSOC_REQ) pick_role("ap", "it associates");
    else if (subtype == MGMT_AUTH && body.size() >= 4 && rdle16(body, 2) == 1) pick_role("ap", "it authenticates");
    if (active_role == "ap") ap_mgmt(subtype, da, sa, bssid, body);
    else if (active_role == "sta") sta_mgmt(subtype, da, sa, bssid, body);
}
void WifiPeer::Impl::sta_learn_bss(const Bytes &sa, const Bytes &bssid, const Bytes &body, int subtype) {
    (void)sa; (void)subtype;
    if (body.size() < 12) return;
    int bint = rdle16(body, 8), caps = rdle16(body, 10);
    std::vector<IeEnt> ies = parse_ies(body.substr(12));
    Bytes ssidb; if (!ie_get(ies, IE_SSID, ssidb)) return;
    std::string ssidv = utf8_replace(ssidb);   /* ssid.decode("utf-8", "replace") */
    Bytes ch, rsn; ie_get(ies, IE_DS, ch); bool has_rsn = ie_get(ies, IE_RSN, rsn);
    if (have_ssid && ssidv != ssid) return;
    if (!bss.valid) {
        /* `ch[0] if ch else self.channel`: an empty DS element falls back too */
        bss.valid = true; bss.bssid = bssid; bss.ssid = ssidv; bss.channel = !ch.empty() ? (u8)ch[0] : channel;
        bss.caps = caps; bss.privacy = (caps & 0x10) != 0; bss.rsn = rsn; bss.has_rsn = has_rsn; bss.bint = bint;
        log(strf("[WIFI] found BSS '%s' %s ch %d%s", ssidv.c_str(), mac2str(bssid).c_str(), bss.channel, bss.privacy ? " (privacy)" : " (open)"));
        if (bss.privacy) log("[WIFI] the device's AP requires a cipher; this peer only joins open networks");
        else { sta_state = "AUTH"; sta_next_us = now() + 50000; }
    }
}
void WifiPeer::Impl::sta_mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body) {
    (void)sa;
    if (!bss.valid || da != mac) return;
    if (subtype == MGMT_AUTH && body.size() >= 6) {
        int seqn = rdle16(body, 2), status = rdle16(body, 4);
        if (seqn == 2 && status == 0 && sta_state == "AUTH") {
            log("[WIFI] authenticated with " + mac2str(bssid));
            sta_state = "ASSOC"; sta_next_us = now(); sta_assoc();
        } else if (status) { log(strf("[WIFI] authentication refused, status %d", status)); sta_state = "AUTH"; sta_next_us = now() + 2000000; }
    } else if (subtype == MGMT_ASSOC_RESP && body.size() >= 6) {
        int status = rdle16(body, 2), aidv = rdle16(body, 4);
        if (status == 0 && sta_state == "ASSOC") {
            aid = aidv & 0x3FFF; sta_state = "RUN";
            log(strf("[WIFI] associated with '%s' (AID %d)", bss.ssid.c_str(), aid));
            events.push_back(std::make_pair(std::string("assoc"), now()));
            if (net) net->link_up(bss.bssid);
        } else if (status) { log(strf("[WIFI] association refused, status %d", status)); sta_state = "AUTH"; sta_next_us = now() + 2000000; }
    } else if (subtype == MGMT_DEAUTH || subtype == MGMT_DISASSOC) {
        int reason = body.size() >= 2 ? rdle16(body, 0) : 0;
        log(strf("[WIFI] %s from the device, reason %d", subtype == MGMT_DEAUTH ? "deauthenticated" : "disassociated", reason));
        sta_state = "AUTH"; sta_next_us = now() + 1000000;
    }
}
void WifiPeer::Impl::sta_auth() { Bytes body = le16(0) + le16(1) + le16(0); send(mgmt(MGMT_AUTH, bss.bssid, mac, bss.bssid, body)); }
void WifiPeer::Impl::sta_assoc() {
    u16 caps = 0x0421;
    Bytes body = le16(caps) + le16(10) + ie(IE_SSID, Bytes(bss.ssid)) + ie(IE_RATES, Bytes(RATES_BG, 8)) + ie(IE_EXT_RATES, Bytes(EXT_RATES_BG, 4));
    send(mgmt(MGMT_ASSOC_REQ, bss.bssid, mac, bss.bssid, body));
}
void WifiPeer::Impl::sta_probe() {
    Bytes body = ie(IE_SSID, have_ssid ? Bytes(ssid) : Bytes()) + ie(IE_RATES, Bytes(RATES_BG, 8)) + ie(IE_EXT_RATES, Bytes(EXT_RATES_BG, 4));
    send(mgmt(MGMT_PROBE_REQ, BCAST6(), mac, BCAST6(), body));
}
void WifiPeer::Impl::sta_tick(u64 nowv) {
    if (!bss.valid) { if (nowv >= sta_next_us) { sta_probe(); sta_next_us = nowv + 1000000; } return; }
    if (sta_state == "AUTH" && nowv >= sta_next_us) { sta_auth(); sta_tries++; sta_next_us = nowv + 1000000; }
    else if (sta_state == "ASSOC" && nowv >= sta_next_us + 1000000) { sta_assoc(); sta_next_us = nowv; }
}
void WifiPeer::Impl::eapol_to(const Bytes &sta, const Bytes &payload) {
    Bytes body = Bytes(LLC_SNAP, 6);
    body += (char)((wpa2::ETH_EAPOL >> 8) & 0xFF); body += (char)(wpa2::ETH_EAPOL & 0xFF);
    body += payload;
    send(data_frame(sta, mac, ap_bssid, body, false));
}
void WifiPeer::Impl::start_handshake(const Bytes &sta) {
    wpa2::Authenticator *a = new wpa2::Authenticator(passphrase, have_ssid ? ssid : std::string("BekenSim"), ap_bssid, sta, 0, 0, logfn, log_ctx);
    auth[sta] = a;
    log("[WIFI] station " + mac2str(sta) + " associated to a protected network; starting the four-way handshake");
    eapol_to(sta, a->start());
}
bool WifiPeer::Impl::on_eapol(const Bytes &sta, const Bytes &payload) {
    std::map<Bytes, wpa2::Authenticator *>::iterator it = auth.find(sta);
    if (it == auth.end()) return false;
    wpa2::Authenticator *a = it->second;
    bool has_reply = false;
    Bytes reply = a->on_message(payload, has_reply);
    if (has_reply) eapol_to(sta, reply);
    std::map<Bytes, Station>::iterator st = stations.find(sta);
    if (a->done && st != stations.end() && !st->second.encrypted) {
        st->second.encrypted = true;
        log("[WIFI] station " + mac2str(sta) + " completed the four-way handshake; the link is encrypted and open for traffic");
        events.push_back(std::make_pair(std::string("encrypted"), now()));
        if (net) net->link_up(sta);
    }
    return true;
}
int WifiPeer::Impl::decr_for(const Bytes &dst) {
    if (!have_pass) return -1;
    if ((u8)dst[0] & 1) { for (std::map<Bytes, Station>::iterator it = stations.begin(); it != stations.end(); ++it) if (it->second.encrypted) return (int)wpa2::DECR_CCMP_SUCCESS; return -1; }
    std::map<Bytes, Station>::iterator st = stations.find(dst);
    return (st != stations.end() && st->second.encrypted) ? (int)wpa2::DECR_CCMP_SUCCESS : -1;
}
int WifiPeer::Impl::ap_caps() { return 0x0421 | (have_pass ? wpa2::CAP_PRIVACY : 0); }
Bytes WifiPeer::Impl::ap_ies(bool probe) {
    Bytes ies = ie(IE_SSID, Bytes(have_ssid ? ssid : std::string("BekenSim"))) + ie(IE_RATES, Bytes(RATES_BG, 8)) + ie(IE_DS, Bytes(1, (char)channel));
    if (!probe) ies += ie(IE_TIM, Bytes("\x00\x01\x00\x00", 4));
    ies += ie(IE_ERP, Bytes("\x00", 1)) + ie(IE_EXT_RATES, Bytes(EXT_RATES_BG, 4));
    if (have_pass) ies += ie(IE_RSN, wpa2::RSN_IE_BODY());
    return ies;
}
u64 WifiPeer::Impl::ap_tsf(u64 nowv) { if (!tsf_start_valid) { tsf_start = nowv; tsf_start_valid = true; } return nowv - tsf_start; }
void WifiPeer::Impl::ap_beacon(u64 nowv) {
    Bytes body;
    u64 tsf = ap_tsf(nowv);
    for (int i = 0; i < 8; i++) body += (char)((tsf >> (8 * i)) & 0xFF);
    body += le16(BEACON_INTERVAL_TU) + le16((u16)ap_caps()) + ap_ies(false);
    send(mgmt(MGMT_BEACON, BCAST6(), ap_bssid, ap_bssid, body));
}
void WifiPeer::Impl::ap_mgmt(int subtype, const Bytes &da, const Bytes &sa, const Bytes &bssid, const Bytes &body) {
    (void)bssid;
    u64 nowv = now();
    if (subtype == MGMT_PROBE_REQ) {
        std::vector<IeEnt> ies = parse_ies(body);
        Bytes wantb;
        /* Only a NON-EMPTY SSID triggers directed matching (Python's `if want:`);
         * a wildcard/broadcast probe (absent or empty SSID) is always answered. */
        if (ie_get(ies, IE_SSID, wantb) && !wantb.empty()) {
            std::string want = utf8_replace(wantb);   /* want.decode("utf-8", "replace") */
            bool is_factory = false;
            for (int i = 0; FACTORY_TEST_SSIDS[i]; i++) if (want == FACTORY_TEST_SSIDS[i]) is_factory = true;
            if (!have_ssid && is_factory) {
                if (!skipped_factory.count(want)) { skipped_factory.insert(want);
                    log(strf("[WIFI] device probes for its factory test network '%s': not answering (pass --wifi-ssid %s to offer it)", want.c_str(), want.c_str())); }
                return;
            }
            if (!have_ssid) { ssid = want; have_ssid = true; log(strf("[WIFI] device probes for '%s': adopting it as our SSID", want.c_str())); }
            if (want != ssid) return;
        }
        Bytes resp;
        u64 tsf = ap_tsf(nowv);
        for (int i = 0; i < 8; i++) resp += (char)((tsf >> (8 * i)) & 0xFF);
        resp += le16(BEACON_INTERVAL_TU) + le16((u16)ap_caps()) + ap_ies(true);
        send(mgmt(MGMT_PROBE_RESP, sa, ap_bssid, ap_bssid, resp));
        return;
    }
    if (da != ap_bssid && da != mac) return;
    if (subtype == MGMT_AUTH && body.size() >= 6) {
        int alg = rdle16(body, 0);
        Bytes resp;
        if (alg != 0) { log(strf("[WIFI] device asks for authentication algorithm %d; only open system is offered", alg)); resp = le16((u16)alg) + le16(2) + le16(13); }
        else { resp = le16(0) + le16(2) + le16(0); Station s; s.state = "AUTH"; s.aid = 0; stations[sa] = s; log("[WIFI] station " + mac2str(sa) + " authenticated (open system)"); }
        send(mgmt(MGMT_AUTH, sa, ap_bssid, ap_bssid, resp));
    } else if (subtype == MGMT_ASSOC_REQ && body.size() >= 4) {
        std::map<Bytes, Station>::iterator sit = stations.find(sa);
        if (sit == stations.end()) { send(mgmt(MGMT_DEAUTH, sa, ap_bssid, ap_bssid, le16(7))); return; }
        std::vector<IeEnt> ies = parse_ies(body.substr(4));
        Bytes wantb, rsn; ie_get(ies, IE_SSID, wantb); bool has_rsn = ie_get(ies, IE_RSN, rsn);
        int status = 0;
        /* Python formats self.ssid with %s, so an unset SSID logs as 'None' */
        std::string ssid_s = have_ssid ? ssid : std::string("None");
        if (have_pass) { if (!has_rsn) { log(strf("[WIFI] station %s asks to join '%s' unprotected, but this network is protected", mac2str(sa).c_str(), ssid_s.c_str())); status = 40; } }
        else if (has_rsn) { log("[WIFI] station " + mac2str(sa) + " offers RSN (a cipher) in its association; this open BSS cannot take it"); status = 40; }
        sit->second.aid = next_aid; next_aid++;
        sit->second.state = status == 0 ? "ASSOC" : "AUTH";
        Bytes resp = le16((u16)ap_caps()) + le16((u16)status) + le16((u16)(0xC000 | sit->second.aid)) + ie(IE_RATES, Bytes(RATES_BG, 8)) + ie(IE_EXT_RATES, Bytes(EXT_RATES_BG, 4));
        send(mgmt(MGMT_ASSOC_RESP, sa, ap_bssid, ap_bssid, resp));
        if (status == 0) {
            log(strf("[WIFI] station %s associated to '%s' (AID %d)", mac2str(sa).c_str(), ssid_s.c_str(), sit->second.aid));
            events.push_back(std::make_pair(std::string("assoc"), nowv));
            if (have_pass) start_handshake(sa);
            else if (net) net->link_up(sa);
        }
    } else if (subtype == MGMT_DEAUTH || subtype == MGMT_DISASSOC) {
        std::map<Bytes, Station>::iterator sit = stations.find(sa);
        if (sit != stations.end()) { int reason = body.size() >= 2 ? rdle16(body, 0) : 0; log(strf("[WIFI] station %s left (%s, reason %d)", mac2str(sa).c_str(), subtype == MGMT_DEAUTH ? "deauth" : "disassoc", reason)); stations.erase(sit); }
    } else if (subtype == MGMT_ACTION && !body.empty()) {
        if ((u8)body[0] == 3 && body.size() > 1 && (u8)body[1] == 0) {
            int token = body.size() > 2 ? (u8)body[2] : 0;
            int params = body.size() >= 5 ? rdle16(body, 3) : 0;
            Bytes resp; resp += (char)3; resp += (char)1; resp += (char)token; resp += le16(37) + le16((u16)params) + le16(0);
            send(mgmt(MGMT_ACTION, sa, ap_bssid, ap_bssid, resp));
        }
    }
}
void WifiPeer::Impl::ap_tick(u64 nowv) {
    /* Python ints: a clock behind the last beacon is a negative gap, not a huge one */
    if (!last_beacon_valid || (nowv >= last_beacon_us && nowv - last_beacon_us >= (u64)BEACON_INTERVAL_TU * TU_US)) { last_beacon_valid = true; last_beacon_us = nowv; ap_beacon(nowv); }
}
void WifiPeer::Impl::on_data(int subtype, u16 fc, const Bytes &frame) {
    if (subtype & 0x4) return;
    int hl = hdr_len(fc);
    if ((int)frame.size() < hl + 8) return;
    bool to_ds = (fc & FC_TODS) != 0, from_ds = (fc & FC_FROMDS) != 0;
    Bytes a1 = frame.substr(4, 6), a2 = frame.substr(10, 6), a3 = frame.substr(16, 6);
    Bytes sa, da;
    if (active_role == "ap") {
        if (!to_ds) return;
        sa = a2; da = a3;
        std::map<Bytes, Station>::iterator sit = stations.find(a2);
        if (sit == stations.end() || sit->second.state != "ASSOC") return;
    } else if (active_role == "sta") {
        if (!from_ds) return;
        da = a1; sa = a3;
        if (da != mac && !((u8)da[0] & 1)) return;
    } else return;
    Bytes body = frame.substr(hl);
    if (body.substr(0, 6) != Bytes(LLC_SNAP, 6)) return;
    Bytes etype = body.substr(6, 2);
    if (active_role == "ap" && have_pass && rdbe16_etype(etype) == wpa2::ETH_EAPOL) { on_eapol(sa, body.substr(8)); return; }
    Bytes eth = da + sa + etype + body.substr(8);
    if (net) net->input(eth);
}

/* ------------------------------------------------------------- WifiPeer */
static void wp_wifinet_output(void *ctx, const Bytes &frame) { ((WifiPeer::Impl *)ctx)->send_eth(frame); }

WifiPeer::WifiPeer(const Bytes &mac, netstack::NetStack *net_, InjectFn inject, void *inject_ctx,
                   LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
                   const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
                   const std::string &passphrase, const Bytes &bssid, Pcap80211 *shared_pcap)
    : net(net_), d(0) {
    init(mac, net_ ? new NetStackNet(net_) : 0, true, inject, inject_ctx, log, log_ctx, role, ssid, channel,
         pcap_path, now_us, now_ctx, latency_us, passphrase, bssid, shared_pcap);
}
WifiPeer::WifiPeer(const Bytes &mac, WifiNet *wnet, InjectFn inject, void *inject_ctx,
                   LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
                   const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
                   const std::string &passphrase, const Bytes &bssid, Pcap80211 *shared_pcap)
    : net(0), d(0) {
    init(mac, wnet, false, inject, inject_ctx, log, log_ctx, role, ssid, channel,
         pcap_path, now_us, now_ctx, latency_us, passphrase, bssid, shared_pcap);
}
void WifiPeer::init(const Bytes &mac, WifiNet *wnet, bool own_wnet, InjectFn inject, void *inject_ctx,
                    LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
                    const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
                    const std::string &passphrase, const Bytes &bssid, Pcap80211 *shared_pcap) {
    d = new Impl();
    d->mac = mac; d->net = wnet; d->own_net = own_wnet;
    d->stats_tx = 0; d->stats_rx = 0;
    d->inject = inject; d->inject_ctx = inject_ctx;
    d->logfn = log; d->log_ctx = log_ctx;
    d->nowfn = now_us; d->now_ctx = now_ctx;
    d->have_pass = !passphrase.empty(); d->passphrase = passphrase;
    d->role = role.empty() ? "auto" : role;
    d->have_active = d->role != "auto"; d->active_role = d->have_active ? d->role : std::string();
    d->have_ssid = !ssid.empty(); d->ssid = ssid;
    d->channel = channel ? channel : 1;
    /* a path is opened here, so closed here; a Pcap80211 handed in is not ours */
    d->own_pcap = !pcap_path.empty();
    d->pcap = d->own_pcap ? new Pcap80211(pcap_path) : shared_pcap;
    d->seq = 0;
    d->ap_bssid = bssid.empty() ? mac : bssid;
    d->sta_state = "SCAN"; d->sta_next_us = 0; d->sta_tries = 0; d->aid = 0;
    d->next_aid = 1; d->last_beacon_valid = false; d->tsf_start_valid = false;
    d->latency_us = latency_us;
    if (wnet) wnet->set_output(wp_wifinet_output, d);
}
WifiPeer::~WifiPeer() {
    if (d) {
        for (std::map<Bytes, wpa2::Authenticator *>::iterator it = d->auth.begin(); it != d->auth.end(); ++it) delete it->second;
        if (d->own_pcap && d->pcap) delete d->pcap;
        if (d->own_net) delete d->net;   /* the peer owns the IP stack handed to it */
        delete d;
    }
}
void WifiPeer::on_device_tx(const Bytes &frame) {
    d->stats_rx++;
    if (d->pcap) d->pcap->write(frame, d->now());
    if (frame.size() < 24) return;
    int ftype, subtype; u16 fc; fc_fields(frame, ftype, subtype, fc);
    if (ftype == 0) d->on_mgmt(subtype, fc, frame);
    else if (ftype == 2) d->on_data(subtype, fc, frame);
}
void WifiPeer::send_eth(const Bytes &eth) { d->send_eth(eth); }
u64 WifiPeer::stats_tx() const { return d->stats_tx; }
u64 WifiPeer::stats_rx() const { return d->stats_rx; }
const std::vector<std::pair<std::string, u64> > &WifiPeer::events() const { return d->events; }
/* test support for alt/tests/test_watchdog.cpp (see wifipeer.h) */
u64 WifiPeer::now_us() { return d->now(); }
size_t WifiPeer::station_count() const { return d->stations.size(); }
size_t WifiPeer::auth_count() const { return d->auth.size(); }
size_t WifiPeer::outq_count() const { return d->outq.size(); }
void WifiPeer::prime_session_for_test(const Bytes &sta, u64 queued_release_us) {
    Station s; s.state = "ASSOC"; s.aid = 1;
    d->stations[sta] = s;
    delete d->auth[sta];
    d->auth[sta] = new wpa2::Authenticator("placeholder", d->ssid, d->ap_bssid, sta);
    OutItem o; o.release_us = queued_release_us; o.frame = Bytes("stale"); o.decr = -1;
    d->outq.push_back(o);
}
void WifiPeer::tick(u64 insns) {
    (void)insns;
    u64 now = d->now();
    d->release(now);
    if (d->active_role == "sta") d->sta_tick(now);
    else if (d->active_role == "ap") d->ap_tick(now);
    else if (d->role == "auto" && d->have_ssid) { if (now > 2000000) d->pick_role("ap", "an SSID was configured"); }
    if (d->net) d->net->tick(now);
}
void WifiPeer::close() {
    /* Release the host side: the network stack's sockets and a capture file
     * this peer opened itself. */
    if (d->net) d->net->close();
    if (d->own_pcap && d->pcap) d->pcap->close();
}
void WifiPeer::device_reset(bool have_now, u64 now_us) {
    /* Forget the rebooted radio endpoint while retaining this peer/AP. */
    u64 now = have_now ? now_us : d->now();
    d->outq.clear();
    for (std::map<Bytes, wpa2::Authenticator *>::iterator it = d->auth.begin(); it != d->auth.end(); ++it) delete it->second;
    d->auth.clear();
    d->bss = Bss(); d->sta_state = "SCAN"; d->sta_next_us = now; d->sta_tries = 0; d->aid = 0;
    d->stations.clear(); d->next_aid = 1; d->last_beacon_valid = false;
    if (d->role != "auto") { d->have_active = true; d->active_role = d->role; }
    if (d->net) d->net->device_reset(now);
}

/* ---------------------------------------------------------------------------
 * Self-tests: port of src/wifipeer.py _run_selftests().
 * ------------------------------------------------------------------------- */
namespace {

struct FakeNet : public WifiNet {
    bool gateway; bool ip_set; Bytes ip; std::vector<Bytes> frames;
    bool up_set; Bytes up; OutputFn output; void *output_ctx;
    bool reset_set; u64 reset_at; bool closed;
    FakeNet() : gateway(true), ip_set(false), up_set(false), output(0), output_ctx(0),
                reset_set(false), reset_at(0), closed(false) {}
    void set_output(OutputFn fn, void *ctx) { output = fn; output_ctx = ctx; }
    void input(const Bytes &eth) { frames.push_back(eth); }
    void link_up(const Bytes &device_mac) { up_set = true; up = device_mac; }
    void tick(u64) {}
    void device_reset(u64 now) { reset_set = true; reset_at = now; up_set = false; up.clear(); }
    void close() { closed = true; }
    void set_gateway(bool gw) { gateway = gw; }
    bool has_ip() { return ip_set; }
    void set_ip(const Bytes &v) { ip_set = !v.empty(); ip = v; }
};

void st_inject(void *ctx, const Bytes &f, int) { ((std::vector<Bytes> *)ctx)->push_back(f); }
void st_drop(void *, const Bytes &, int) {}
u64 st_now(void *ctx) { return *(u64 *)ctx; }

/* Python slicing: never throws, clamps to the data */
Bytes sl(const Bytes &b, size_t a, size_t e) {
    if (a >= b.size() || e <= a) return Bytes();
    return b.substr(a, (e > b.size() ? b.size() : e) - a);
}
int st_subtype(const Bytes &f) { return f.size() < 2 ? -1 : (((u8)f[0] | ((u8)f[1] << 8)) >> 4) & 0xF; }
int st_ftype(const Bytes &f) { return f.size() < 2 ? -1 : (((u8)f[0] | ((u8)f[1] << 8)) >> 2) & 3; }
bool st_last_is(const std::vector<Bytes> &v, int subtype) { return !v.empty() && st_subtype(v.back()) == subtype; }
bool st_any_is(const std::vector<Bytes> &v, size_t from, int subtype) {
    for (size_t i = from; i < v.size(); i++) if (st_subtype(v[i]) == subtype) return true;
    return false;
}
Bytes st_mac(const char *s) {
    Bytes b;
    for (int i = 0; i < 6; i++) { unsigned x = 0; sscanf(s + 3 * i, "%2x", &x); b += (char)x; }
    return b;
}
Bytes st_fixed(u64 tsf, u16 bint, u16 caps) {   /* struct.pack("<QHH", ...) */
    char h[12]; wr64le(h, tsf); wr16le(h + 8, bint); wr16le(h + 10, caps); return Bytes(h, 12);
}
Bytes st_h3(u16 a, u16 b, u16 c) { return le16(a) + le16(b) + le16(c); }

struct Rec { u64 t; u32 incl, orig; Bytes b; };

int g_wp_checks;
bool g_wp_failed;
bool wpcheck(bool cond, const char *msg) {
    if (!cond) { printf("  [FAIL] %s\n", msg); g_wp_failed = true; return false; }
    g_wp_checks++;
    printf("  [PASS] %s\n", msg);
    return true;
}

} /* namespace */

/* Python's assert stops the run at the first failure: so does this. */
#define WPCHECK(cond, msg) do { if (!wpcheck((cond), (msg))) return -1; } while (0)

int wifipeer_selftests() {
    g_wp_checks = 0; g_wp_failed = false;
    const Bytes BC = BCAST6(), SNAP(LLC_SNAP, 6), RATES(RATES_BG, 8);
    u64 clock = 0;
    std::vector<Bytes> sent;
    FakeNet net;
    WifiPeer p(st_mac("02:00:00:00:00:01"), &net, st_inject, &sent, 0, 0, "auto", "", 1, "", st_now, &clock, 0, "");
    WifiPeer::Impl *P = p.d;
    const Bytes dev = st_mac("c8:47:8c:00:00:01");

    printf("== STA role: device beacons an open AP ==\n");
    Bytes beacon = P->mgmt(MGMT_BEACON, BC, dev, dev, st_fixed(0, 100, 0x0421) + ie(IE_SSID, Bytes("OpenBK7231T_8C000000"))
                           + ie(IE_RATES, RATES) + ie(IE_DS, Bytes("\x01", 1)));
    p.on_device_tx(beacon);
    WPCHECK(P->have_active && P->active_role == "sta" && P->bss.valid && P->bss.ssid == "OpenBK7231T_8C000000",
            "beacon -> peer becomes a station, BSS learned");
    clock = 100000;
    p.tick();
    WPCHECK(st_last_is(sent, MGMT_AUTH), "authentication request sent");
    Bytes auth_ok = P->mgmt(MGMT_AUTH, P->mac, dev, dev, st_h3(0, 2, 0));
    p.on_device_tx(auth_ok);
    WPCHECK(st_last_is(sent, MGMT_ASSOC_REQ) && P->sta_state == "ASSOC", "auth response -> association request");
    Bytes assoc_ok = P->mgmt(MGMT_ASSOC_RESP, P->mac, dev, dev, st_h3(0x0421, 0, 0xC001));
    p.on_device_tx(assoc_ok);
    WPCHECK(P->sta_state == "RUN" && net.up_set && net.up == dev && P->aid == 1,
            "association response -> RUN, IP stack told the link is up");
    Bytes eth = BC + P->mac + Bytes("\x08\x06", 2) + Bytes(28, '\0');
    p.send_eth(eth);
    {
        Bytes f = sent.empty() ? Bytes() : sent.back();
        WPCHECK(st_ftype(f) == 2 && ((u8)f[1] & 1) && sl(f, 4, 10) == dev && sl(f, 16, 22) == BC && sl(f, 24, 30) == SNAP,
                "Ethernet -> ToDS data frame with LLC/SNAP");
    }
    Bytes down = P->data_frame(P->mac, dev, dev, SNAP + Bytes("\x08\x00", 2) + Bytes("IPDATA"), false);
    p.on_device_tx(down);
    WPCHECK(!net.frames.empty() && net.frames.back() == P->mac + dev + Bytes("\x08\x00", 2) + Bytes("IPDATA"),
            "FromDS data frame -> Ethernet into the IP stack");

    printf("== AP role: device probes and joins ==\n");
    std::vector<Bytes> sent2, sent3, sent4;
    FakeNet net2, net_t, net_t2, net_r;
    WifiPeer q(st_mac("02:00:00:00:00:02"), &net2, st_inject, &sent2, 0, 0, "auto", "", 1, "", st_now, &clock, 0, "");
    WifiPeer::Impl *Q = q.d;
    Bytes probe = Q->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("HomeNet")) + ie(IE_RATES, RATES));
    q.on_device_tx(probe);
    WPCHECK(Q->active_role == "ap" && Q->have_ssid && Q->ssid == "HomeNet",
            "probe request -> peer becomes an AP and adopts the SSID");
    WifiPeer t(st_mac("02:00:00:00:00:03"), &net_t, st_inject, &sent3, 0, 0, "auto", "", 1, "", st_now, &clock, 0, "");
    t.on_device_tx(t.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("tuya_mdev_test1")) + ie(IE_RATES, RATES)));
    WPCHECK(!t.d->have_ssid && sent3.empty(), "Tuya factory-test SSID is not adopted or answered by default");
    WifiPeer t2(st_mac("02:00:00:00:00:04"), &net_t2, st_inject, &sent3, 0, 0, "ap", "tuya_mdev_test1", 1, "", st_now, &clock, 0, "");
    t2.on_device_tx(t2.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("tuya_mdev_test1")) + ie(IE_RATES, RATES)));
    WPCHECK(st_last_is(sent3, MGMT_PROBE_RESP), "...but answered when given explicitly as --wifi-ssid");
    WifiPeer r(st_mac("02:00:00:00:00:05"), &net_r, st_inject, &sent4, 0, 0, "auto", "", 1, "", st_now, &clock, 0, "");
    r.on_device_tx(r.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("tuya_mdev_test1")) + ie(IE_RATES, RATES)));
    WPCHECK(r.d->have_active && r.d->active_role == "ap", "auto: a boot-time probe makes the peer an AP first");
    r.on_device_tx(r.d->mgmt(MGMT_BEACON, BC, dev, dev, st_fixed(0, 100, 0x0421)
                             + ie(IE_SSID, Bytes("SmartLife-E9FF")) + ie(IE_RATES, RATES) + ie(IE_DS, Bytes("\x06", 1))));
    WPCHECK(r.d->active_role == "sta" && r.d->bss.valid && r.d->bss.ssid == "SmartLife-E9FF",
            "auto: the device raising its own AP afterwards flips the peer to station and it finds the BSS");
    WPCHECK(st_last_is(sent2, MGMT_PROBE_RESP), "probe response sent");
    q.on_device_tx(Q->mgmt(MGMT_AUTH, Q->mac, dev, Q->mac, st_h3(0, 1, 0)));
    {
        Bytes f = sent2.back();
        WPCHECK(st_subtype(f) == MGMT_AUTH && sl(f, 24, 30) == st_h3(0, 2, 0), "open-system auth answered seq 2 status 0");
    }
    q.on_device_tx(Q->mgmt(MGMT_ASSOC_REQ, Q->mac, dev, Q->mac, le16(0x0421) + le16(10) + ie(IE_SSID, Bytes("HomeNet")) + ie(IE_RATES, RATES)));
    {
        Bytes f = sent2.back();
        Bytes st = sl(f, 26, 28);
        WPCHECK(st_subtype(f) == MGMT_ASSOC_RESP && st.size() == 2 && rdle16(st, 0) == 0 && net2.up_set && net2.up == dev,
                "association accepted, link up");
    }
    clock = 300000;
    q.tick();
    WPCHECK(st_any_is(sent2, 0, MGMT_BEACON), "AP beacons");
    Bytes up = Q->data_frame(BC, dev, Q->mac, SNAP + Bytes("\x08\x06", 2) + Bytes("ARP"), true);
    q.on_device_tx(up);
    WPCHECK(!net2.frames.empty() && sl(net2.frames.back(), 12, 14) == Bytes("\x08\x06", 2),
            "ToDS data from the associated station reaches the IP stack");
    net2.output = 0; net2.output_ctx = 0;
    q.send_eth(dev + Q->mac + Bytes("\x08\x00", 2) + Bytes("X"));
    {
        Bytes f = sent2.back();
        WPCHECK(((u8)f[1] & 2) && sl(f, 4, 10) == dev && sl(f, 10, 16) == Q->mac, "Ethernet -> FromDS data frame to the station");
    }

    printf("== device reset: external AP survives, old link does not ==\n");
    {
        OutItem o; o.release_us = clock + 10000; o.frame = Bytes("old"); o.decr = -1;
        Q->outq.push_back(o);
    }
    /* Python stores a placeholder object(); a real handshake state stands in for it */
    Q->auth[dev] = new wpa2::Authenticator("placeholder", "HomeNet", Q->ap_bssid, dev);
    q.device_reset(true, clock);
    WPCHECK(Q->active_role == "ap" && Q->stations.empty() && Q->auth.empty(),
            "reset keeps the auto-selected AP role but clears association and WPA state");
    WPCHECK(Q->outq.empty() && net2.reset_set && net2.reset_at == clock,
            "queued radio frames are dropped and the IP side is reset at the same time");
    size_t before = sent2.size();
    q.tick();
    WPCHECK(st_any_is(sent2, before, MGMT_BEACON), "the surviving AP beacons immediately after the device resets");
    FakeNet flip_net;
    std::vector<Bytes> flip_sent;
    WifiPeer flip(st_mac("02:00:00:00:00:07"), &flip_net, st_inject, &flip_sent, 0, 0, "auto", "", 1, "", st_now, &clock, 0, "");
    flip.on_device_tx(flip.d->mgmt(MGMT_BEACON, BC, dev, dev, st_fixed(0, 100, 0x0421)
                                   + ie(IE_SSID, Bytes("PairingAP")) + ie(IE_RATES, RATES) + ie(IE_DS, Bytes("\x06", 1))));
    flip.device_reset(true, clock);
    flip.on_device_tx(flip.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("HomeNet")) + ie(IE_RATES, RATES)));
    WPCHECK(flip.d->active_role == "ap", "auto redetects a device that changes from AP to station across reset");

    printf("== latency: frames reach the device after a delay ==\n");
    u64 clk = 0;
    std::vector<Bytes> got;
    FakeNet net_lp;
    WifiPeer lp(st_mac("02:00:00:00:00:06"), &net_lp, st_inject, &got, 0, 0, "ap", "Lat", 1, "", st_now, &clk, 3000, "");
    lp.on_device_tx(lp.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("Lat")) + ie(IE_RATES, RATES)));
    WPCHECK(got.empty(), "probe response held back at first");
    clk = 2999;
    lp.tick();
    WPCHECK(!st_any_is(got, 0, MGMT_PROBE_RESP), "...still held 1 us before the latency");
    clk = 3000;
    lp.tick();
    WPCHECK(st_any_is(got, 0, MGMT_PROBE_RESP), "...and delivered once it has passed");

    printf("== pcap: both directions recorded, readable back ==\n");
    std::string cap = make_temp_file("wifipeer", ".pcap");
    Bytes raw, probe2;
    std::vector<Bytes> got2;
    {
        u64 clk2 = 1500000;
        FakeNet net_cp, net_sp;
        WifiPeer cp(st_mac("02:00:00:00:00:07"), &net_cp, st_inject, &got2, 0, 0, "ap", "Cap", 1, cap, st_now, &clk2, 3000, "");
        probe2 = cp.d->mgmt(MGMT_PROBE_REQ, BC, dev, BC, ie(IE_SSID, Bytes("Cap")) + ie(IE_RATES, RATES));
        cp.on_device_tx(probe2);
        clk2 = 1503000;
        cp.tick();
        cp.close();
        bool ok = wpcheck(net_cp.closed && cp.d->pcap && !cp.d->pcap->is_open(),
                          "close() releases the network's host side and the capture file the peer opened");
        read_file(cap, raw);
        Pcap80211 shared(cap);
        bool ok2 = false;
        if (ok) {
            WifiPeer sp(st_mac("02:00:00:00:00:08"), &net_sp, st_drop, 0, 0, 0, "auto", "", 1, "", 0, 0, 0, "", Bytes(), &shared);
            sp.close();
            ok2 = wpcheck(shared.is_open(), "...but leaves a capture it was handed to whoever opened it");
        }
        shared.close();
        remove_file(cap);   /* finally: os.remove(cap) */
        if (!ok || !ok2) return -1;
    }
    {
        bool hdr_ok = raw.size() >= 24;
        u32 magic = 0, snap = 0, link = 0; u16 vmaj = 0, vmin = 0;
        if (hdr_ok) {
            const u8 *h = (const u8 *)raw.data();
            magic = (u32)h[0] | ((u32)h[1] << 8) | ((u32)h[2] << 16) | ((u32)h[3] << 24);
            vmaj = (u16)(h[4] | (h[5] << 8)); vmin = (u16)(h[6] | (h[7] << 8));
            snap = (u32)h[16] | ((u32)h[17] << 8) | ((u32)h[18] << 16) | ((u32)h[19] << 24);
            link = (u32)h[20] | ((u32)h[21] << 8) | ((u32)h[22] << 16) | ((u32)h[23] << 24);
        }
        WPCHECK(hdr_ok && magic == 0xA1B2C3D4 && vmaj == 2 && vmin == 4 && link == 105 && snap >= 2346,
                "global header: pcap 2.4, link type 105 (802.11 without FCS), snap length fits any MPDU");
    }

    std::vector<Rec> recs;
    size_t off = 24;
    while (off + 16 <= raw.size()) {
        const u8 *h = (const u8 *)raw.data() + off;
        u32 v[4];
        for (int i = 0; i < 4; i++) v[i] = (u32)h[4 * i] | ((u32)h[4 * i + 1] << 8) | ((u32)h[4 * i + 2] << 16) | ((u32)h[4 * i + 3] << 24);
        Rec rc; rc.t = (u64)v[0] * 1000000 + v[1]; rc.incl = v[2]; rc.orig = v[3];
        rc.b = sl(raw, off + 16, off + 16 + v[2]);
        recs.push_back(rc);
        off += 16 + v[2];
    }
    {
        bool all = true;
        for (size_t i = 0; i < recs.size(); i++) if (!(recs[i].incl == recs[i].orig && recs[i].orig == recs[i].b.size())) all = false;
        WPCHECK(!recs.empty() && off == raw.size() && all,
                "records parse back to back; captured length = frame length (nothing truncated)");
    }
    WPCHECK(recs[0].t == 1500000 && recs[0].b == probe2,
            "the device's frame is recorded byte for byte, stamped with the time it was sent");
    {
        const Rec *resp = 0;
        for (size_t i = 1; i < recs.size(); i++) if (st_subtype(recs[i].b) == MGMT_PROBE_RESP) { resp = &recs[i]; break; }
        bool in_got = false;
        if (resp) for (size_t i = 0; i < got2.size(); i++) if (got2[i] == resp->b) in_got = true;
        WPCHECK(resp && resp->t == 1503000 && in_got,
                "the peer's reply is recorded as delivered, stamped when the device received it (after the latency)");
    }

    printf("\nAll %d Wi-Fi peer self-tests passed.\n", g_wp_checks);
    return g_wp_failed ? -1 : g_wp_checks;
}
