/* The virtual 802.11 peer (station or AP) on the far side of the MAC model.
 * Port of src/wifipeer.py. */
#ifndef BK_WIFIPEER_H
#define BK_WIFIPEER_H

#include "common.h"
#include <vector>
#include <utility>

namespace netstack {
class NetStack;
}

/* Minimal pcap writer, DLT_IEEE802_11 (105): frames without FCS. */
class Pcap80211 {
public:
    explicit Pcap80211(const std::string &path);
    ~Pcap80211();
    void write(const Bytes &frame, u64 us);
    void close();
    bool is_open() const;
private:
    FILE *f_;
};

extern const char *WIFI_ROLES[]; /* "auto","sta","ap",0 */

/* What the peer needs from the IP stack behind it. Python duck-types this
 * (netstack.NetStack, or the self-tests' FakeNet); WifiPeer wraps a real
 * netstack::NetStack in an adapter, and the self-tests plug in a fake. */
class WifiNet {
public:
    typedef void (*OutputFn)(void *ctx, const Bytes &eth);
    virtual ~WifiNet() {}
    virtual void set_output(OutputFn fn, void *ctx) = 0;  /* net.output = peer.send_eth */
    virtual void input(const Bytes &eth) = 0;
    virtual void link_up(const Bytes &device_mac) = 0;
    virtual void tick(u64 now_us) = 0;
    virtual void device_reset(u64 now_us) = 0;
    virtual void close() = 0;
    virtual void set_gateway(bool gw) = 0;
    virtual bool has_ip() = 0;                 /* net.ip is not None */
    virtual void set_ip(const Bytes &ip) = 0;  /* empty = None */
};

class WifiPeer {
public:
    /* inject: hand a frame to the device's MAC. decr<0 = none. */
    typedef void (*InjectFn)(void *ctx, const Bytes &frame, int decr);
    typedef void (*LogFn)(void *ctx, const std::string &msg);
    typedef u64 (*NowFn)(void *ctx);

    /* net: a real IP stack, owned by the peer from here on (may be 0).
     * pcap_path: a capture the peer opens (and closes) itself; or
     * shared_pcap: a capture somebody else opened, which close() leaves open.
     * bssid: the AP role's BSSID (empty = our own mac). */
    WifiPeer(const Bytes &mac, netstack::NetStack *net, InjectFn inject, void *inject_ctx,
             LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
             const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
             const std::string &passphrase, const Bytes &bssid = Bytes(), Pcap80211 *shared_pcap = 0);
    /* Same, behind any WifiNet (not owned). */
    WifiPeer(const Bytes &mac, WifiNet *net, InjectFn inject, void *inject_ctx,
             LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
             const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
             const std::string &passphrase, const Bytes &bssid = Bytes(), Pcap80211 *shared_pcap = 0);
    ~WifiPeer();

    void on_device_tx(const Bytes &frame);
    void tick(u64 insns = 0);
    void close();
    void device_reset(bool have_now, u64 now_us);
    /* Ethernet frame from the IP stack onto the air (Python send_eth) */
    void send_eth(const Bytes &eth);

    /* Python self.stats {"tx","rx"} and self.events [(name, now_us)] */
    u64 stats_tx() const;
    u64 stats_rx() const;
    const std::vector<std::pair<std::string, u64> > &events() const;

    /* Test support (tests/beken/test_watchdog.cpp, the port of tests/watchdog.py,
     * which reads/primes peer.now_us(), peer.stations, peer.auth, peer._outq). */
    u64 now_us();
    size_t station_count() const;
    size_t auth_count() const;
    size_t outq_count() const;
    /* an associated station + authenticator + one queued frame for `sta` */
    void prime_session_for_test(const Bytes &sta, u64 queued_release_us);

    netstack::NetStack *net;   /* the real stack, when constructed with one */

    struct Impl; /* public so the netstack output trampoline can reach it */
private:
    Impl *d;
    void init(const Bytes &mac, WifiNet *wnet, bool own_wnet, InjectFn inject, void *inject_ctx,
              LogFn log, void *log_ctx, const std::string &role, const std::string &ssid, int channel,
              const std::string &pcap_path, NowFn now_us, void *now_ctx, u64 latency_us,
              const std::string &passphrase, const Bytes &bssid, Pcap80211 *shared_pcap);
    friend int wifipeer_selftests();
};

int wifipeer_selftests();

#endif
