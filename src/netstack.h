/* User-mode IP network for the emulated device (ARP/DHCP/DNS/NTP/ICMP/TCP/UDP/
 * TLS-PSK/MQTT/HTTP). Port of src/netstack.py. The Wi-Fi peer bridges Ethernet
 * frames to this stack; the CLI configures it. */
#ifndef BK_NETSTACK_H
#define BK_NETSTACK_H

#include "common.h"
#include "platform.h"
#include <map>
#include <deque>

struct PyTestAccess; /* tests/beken/pytests.h: the ported Python unit-test files */

namespace netstack {

Bytes str2ip(const std::string &s);
std::string ip2str(const Bytes &b);
std::string mac2str(const Bytes &b);
u16 inet_checksum(const Bytes &data);

std::string describe_frame(const Bytes &frame);
bool ipv4_endpoints(const Bytes &frame, Bytes &src, Bytes &dst);
std::string hexdump(const Bytes &data, int width = 16, u32 base = 0);
std::string describe_payload(const Bytes &data);

class EthPcap {
public:
    explicit EthPcap(const std::string &path);
    ~EthPcap();
    void write(const Bytes &frame, u64 us);
    void close();
    int count;
private:
    FILE *f_;
    bool own_;
};

class HostIO;   /* real sockets on a helper thread (defined in netstack.cpp) */
class TcpConn;  /* one TCP connection (defined in netstack.cpp) */
class NetStack;

struct NetConfig {
    Bytes mac;
    bool gateway;
    Bytes ip;              /* empty = none */
    bool net_out;
    std::vector<std::pair<int, int> > forwards;
    StrList http_gets;
    std::vector<std::pair<std::string, Bytes> > mqtt_pubs;
    std::vector<Bytes> tls_psks;
    bool tuya_legacy_identity_psk;
    NetConfig() : gateway(true), net_out(false), tuya_legacy_identity_psk(false) {}
};

typedef void (*NetLogFn)(void *ctx, const std::string &msg);
typedef void (*NetOutFn)(void *ctx, const Bytes &frame);
typedef void (*NetFrameFn)(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us);

/* A DNS query awaiting an async resolver answer. */
struct DnsKey {
    Bytes src;
    int sport;
    u16 txid;
    std::string name;
    int qtype;
    Bytes dst;
    Bytes question;
};

struct ConnKey {
    Bytes lip; int lport; Bytes rip; int rport;
    bool operator<(const ConnKey &o) const {
        if (lip != o.lip) return lip < o.lip;
        if (lport != o.lport) return lport < o.lport;
        if (rip != o.rip) return rip < o.rip;
        return rport < o.rport;
    }
};

/* An online (--net-out) UDP flow: (device port, destination ip, destination port). */
struct UdpKey { int dev_port; Bytes dst_ip; int dst_port;
    bool operator<(const UdpKey &o) const {
        if (dev_port != o.dev_port) return dev_port < o.dev_port;
        if (dst_ip != o.dst_ip) return dst_ip < o.dst_ip;
        return dst_port < o.dst_port; } };

/* One --net-http-get reply: (path, status line, body). */
struct HttpReply { std::string path; std::string status; Bytes body; };

class NetStack {
public:
    NetStack(const NetConfig &cfg, NetLogFn log, void *log_ctx);
    ~NetStack();

    /* set by the peer: send an Ethernet frame onto the air */
    NetOutFn output;
    void *output_ctx;
    NetFrameFn on_frame;
    void *on_frame_ctx;

    bool gateway;
    Bytes ip;   /* empty = none */

    void input(const Bytes &frame);
    void link_up(const Bytes &device_mac);
    void tick(u64 now_us);
    void close();
    void device_reset(u64 now_us);
    /* host listeners the --net-forward ports bound (the GUI's "Open in browser") */
    size_t listener_count() const { return listeners_.size(); }

    /* called by TcpConn */
    void log(const std::string &msg);
    void send_ip(const Bytes &pkt, const Bytes &dst_ip);
    void report_http(TcpConn *conn, const Bytes &data);

    friend class TcpConn;
    friend struct NsSelfTest;   /* the self-tests (netstack_selftests.h) */
    friend struct ::PyTestAccess;

private:
    NetLogFn log_; void *log_ctx_;
    Bytes mac_;
    Bytes netmask_;
    bool net_out_;
    HostIO *io_;
    u64 now_us_;
    std::map<Bytes, Bytes> arp_;       /* ip -> mac */
    Bytes device_ip_;
    Bytes device_mac_;
    std::map<Bytes, Bytes> leases_;    /* mac -> ip */
    int pool_next_;
    std::map<ConnKey, TcpConn *> conns_;
    int next_port_;
    std::map<std::string, Bytes> dns_cache_;
    std::map<int, DnsKey> dns_pending_; /* id -> query */
    int dns_next_id_;
    std::map<std::string, Bytes> sinkhole_;
    std::map<Bytes, std::string> sinkhole_rev_;
    int sink_next_;
    struct UdpFlow { sock_t sock; u64 last_us; };
    std::map<UdpKey, UdpFlow> udp_flows_;
    std::vector<std::pair<int, int> > forwards_;
    std::vector<sock_t> listeners_;
    std::map<sock_t, int> listener_dport_;
    std::string dhcp_state_;
    u32 dhcp_xid_;
    u64 dhcp_next_us_;
    Bytes dhcp_server_;
    Bytes offered_;
    bool link_;
    bool link_up_valid_; u64 link_up_us_;
    u64 next_probe_us_;
    bool probe_sent_;
    int ping_seq_, ping_replies_;
    struct PendFwd { sock_t sock; int dport; u64 at; };
    std::vector<PendFwd> pending_forward_wait_;
    u16 ident_;
    StrList http_gets_;
    std::vector<Bytes> tls_psks_;
    bool tuya_legacy_identity_psk_;
    std::vector<std::pair<std::string, Bytes> > mqtt_pubs_;
    std::vector<std::pair<std::string, Bytes> > mqtt_publishes_;
    std::vector<std::pair<std::string, Bytes> > tls_identities_; /* (connection, identity) */
    std::vector<HttpReply> http_replies_;
    u64 http_next_us_;
    /* Connections that left conns_: kept (bounded) so a pointer a caller still
     * holds stays valid for a while, like a Python object outliving its dict
     * entry. Freed oldest-first beyond RETIRED_MAX, and on destruction. */
    std::deque<TcpConn *> retired_;
    void retire(TcpConn *conn);

    void trace(const std::string &dir, const Bytes &frame);
    void send_eth(const Bytes &dst_mac, u16 ethertype, const Bytes &payload);
    Bytes mac_for(const Bytes &ip);
    void send_arp_request(const Bytes &target_ip);
    void send_ping(const Bytes &target_ip);
    bool local(const Bytes &ip);
    void arp_in(const Bytes &p, const Bytes &src_mac);
    void ipv4_in(const Bytes &p, const Bytes &src_mac);
    void icmp_in(const Bytes &src, const Bytes &dst, const Bytes &p);
    void udp_in(const Bytes &src, const Bytes &dst, const Bytes &p);
    std::string name_of(const Bytes &ip);
    void ntp_in(const Bytes &src, int sport, const Bytes &dst, const Bytes &data);
    void dns_in(const Bytes &src, int sport, const Bytes &dst, const Bytes &data);
    void dns_reply(const DnsKey &key, const Bytes &ip, bool have_ip);
    std::map<int, Bytes> dhcp_opts(const Bytes &data);
    void dhcp_server(const Bytes &src, int sport, const Bytes &data);
    void dhcp_client_send(int mtype);
    void dhcp_client_rx(const Bytes &data);
    void arp_probe_tick();
    void dhcp_client_tick();
    void tcp_in(const Bytes &src, const Bytes &dst, const Bytes &p);
    void tcp_new(const Bytes &src, int sport, const Bytes &dst, int dport, u32 seq, u32 window);
    void http_client_tick();
    void forward_open(sock_t sock, int dport);
    void forget_connection(TcpConn *conn);
};

int netstack_selftests();

} /* namespace netstack */

#endif
