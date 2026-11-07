/* User-mode IP network for the emulated device. Port of src/netstack.py.
 * ARP, DHCP (server + client), DNS (sinkhole/offline), NTP, ICMP echo, a TCP
 * engine with offline HTTP/MQTT/TLS-PSK responders, (for --net-forward) a
 * host-side listener bridged to the device, and (for --net-out) the online
 * relay: host DNS lookups, TCP connects and UDP flows. Self-tests are in
 * netstack_selftests.h. */
#include "netstack.h"
#include "crypto_prims.h"
#include "beken_crypto.h"
#include <string.h>
#include <stdio.h>
#include <deque>
#include <exception>
#include <stdlib.h>
#include <time.h>

namespace netstack {

/* --------------------------------------------------------------- constants */
static const u16 ETH_IP = 0x0800, ETH_ARP = 0x0806;
static const int IP_ICMP = 1, IP_TCP = 6, IP_UDP = 17;
static const u8 TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_PSH = 0x08, TCP_ACK = 0x10;
static const int MSS = 1400;
static const int RCV_WINDOW = 16384;
static const u64 RETRANSMIT_US = 1000000;
static const int SYN_MAX_TRIES = 6;
static const u64 CAPTURE_IDLE_US = 3000000;
static const int CAPTURE_LIMIT = 4096;
static const u64 STATIC_FALLBACK_US = 12000000;
static const int TLS_MAX_PLAINTEXT = 1 << 14;
static const int TLS_MAX_CIPHERTEXT = TLS_MAX_PLAINTEXT + 2048;
static const u16 TLS_PSK_AES_128_GCM_SHA256 = 0x00A8;
static const u16 TLS_EXT_EXTENDED_MASTER_SECRET = 0x0017;

static const char *BCAST_MAC_STR = "\xff\xff\xff\xff\xff\xff";
static Bytes BCAST_MAC() { return Bytes(BCAST_MAC_STR, 6); }

/* --------------------------------------------------------------- byte utils */
static Bytes be16(u16 v) { Bytes b(2, '\0'); b[0] = (char)(v >> 8); b[1] = (char)(v & 0xFF); return b; }
static Bytes be32(u32 v) { Bytes b(4, '\0'); b[0] = (char)(v >> 24); b[1] = (char)(v >> 16); b[2] = (char)(v >> 8); b[3] = (char)v; return b; }
static Bytes be64(u64 v) { Bytes b(8, '\0'); for (int i = 0; i < 8; i++) b[i] = (char)((v >> (56 - 8 * i)) & 0xFF); return b; }
static u16 rdbe16(const Bytes &b, size_t o) { return (u16)(((u8)b[o] << 8) | (u8)b[o + 1]); }
static u32 rdbe32(const Bytes &b, size_t o) { return ((u32)(u8)b[o] << 24) | ((u32)(u8)b[o + 1] << 16) | ((u32)(u8)b[o + 2] << 8) | (u8)b[o + 3]; }
/* Python's `now - then > limit` on device-time integers: a clock that went
 * backwards gives a negative difference, never a huge unsigned one. */
static bool elapsed_gt(u64 now, u64 then, u64 limit) { return (long long)(now - then) > (long long)limit; }
static bool elapsed_lt(u64 now, u64 then, u64 limit) { return (long long)(now - then) < (long long)limit; }
static bool utf8_valid(const Bytes &b) { return utf8_replace(b) == b; }
/* The first n code points of a UTF-8 string (Python text[:n]). */
static std::string utf8_prefix(const std::string &s, size_t n) {
    size_t i = 0, cps = 0;
    while (i < s.size() && cps < n) {
        u8 c = (u8)s[i];
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        cps++;
    }
    return s.substr(0, i);
}
/* Python's "replace" decoding of one byte at a time as ASCII. */
static std::string ascii_replace(const Bytes &b) {
    std::string out;
    for (size_t i = 0; i < b.size(); i++) { if ((u8)b[i] < 0x80) out += b[i]; else out += "\xEF\xBF\xBD"; }
    return out;
}
/* hash((a, b)) & 0xFFFFF for two small non-negative ints: CPython's xxHash-based
 * tuple hash (3.8+), so a connection's initial sequence number matches Python's. */
static u32 py_tuple2_hash20(u64 a, u64 b) {
    const u64 P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P5 = 2870177450012600261ULL;
    u64 acc = P5;
    u64 lanes[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        acc += lanes[i] * P2;
        acc = (acc << 31) | (acc >> 33);
        acc *= P1;
    }
    acc += 2ULL ^ (P5 ^ 3527539ULL);
    if (acc == (u64)-1) acc = 1546275796ULL;
    return (u32)(acc & 0xFFFFF);
}

Bytes str2ip(const std::string &s) {
    StrList p = split(s, ".");
    Bytes out;
    for (size_t i = 0; i < p.size() && i < 4; i++) { long long n = 0; parse_int(p[i], 10, n); out += (char)(u8)n; }
    while (out.size() < 4) out += '\0';
    return out;
}
std::string ip2str(const Bytes &b) {
    if (b.size() < 4) return "0.0.0.0";
    return strf("%d.%d.%d.%d", (u8)b[0], (u8)b[1], (u8)b[2], (u8)b[3]);
}
std::string mac2str(const Bytes &b) {
    std::string s;
    for (size_t i = 0; i < b.size(); i++) s += strf(i ? ":%02x" : "%02x", (u8)b[i]);
    return s;
}
u16 inet_checksum(const Bytes &data) {
    Bytes d = data; if (d.size() % 2) d += '\0';
    u32 s = 0; for (size_t i = 0; i < d.size(); i += 2) s += ((u8)d[i] << 8) | (u8)d[i + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)(~s & 0xFFFF);
}

static Bytes ipv4_packet(const Bytes &src, const Bytes &dst, int proto, const Bytes &payload, u16 ident = 0, int ttl = 64) {
    Bytes hdr;
    hdr += (char)0x45; hdr += (char)0; hdr += be16((u16)(20 + payload.size()));
    hdr += be16(ident); hdr += be16(0); hdr += (char)ttl; hdr += (char)proto; hdr += be16(0);
    hdr += src; hdr += dst;
    u16 c = inet_checksum(hdr);
    hdr = hdr.substr(0, 10) + be16(c) + hdr.substr(12);
    return hdr + payload;
}
static u16 l4_checksum(const Bytes &src, const Bytes &dst, int proto, const Bytes &seg) {
    Bytes pseudo = src + dst;
    pseudo += (char)0; pseudo += (char)proto; pseudo += be16((u16)seg.size());
    return inet_checksum(pseudo + seg);
}
static Bytes udp_packet(const Bytes &src, const Bytes &dst, int sport, int dport, const Bytes &payload) {
    Bytes seg = be16((u16)sport) + be16((u16)dport) + be16((u16)(8 + payload.size())) + be16(0) + payload;
    u16 c = l4_checksum(src, dst, IP_UDP, seg); if (!c) c = 0xFFFF;
    seg = seg.substr(0, 6) + be16(c) + seg.substr(8);
    return ipv4_packet(src, dst, IP_UDP, seg);
}
static Bytes tcp_segment(const Bytes &src, const Bytes &dst, int sport, int dport, u32 seq, u32 ack,
                         u8 flags, int window, const Bytes &payload = Bytes(), const Bytes &options = Bytes()) {
    int doff = (20 + (int)options.size()) / 4;
    Bytes seg = be16((u16)sport) + be16((u16)dport) + be32(seq) + be32(ack);
    seg += (char)(doff << 4); seg += (char)flags; seg += be16((u16)window); seg += be16(0); seg += be16(0);
    seg += options; seg += payload;
    u16 c = l4_checksum(src, dst, IP_TCP, seg);
    seg = seg.substr(0, 16) + be16(c) + seg.substr(18);
    return ipv4_packet(src, dst, IP_TCP, seg);
}
static Bytes eth_frame(const Bytes &dst, const Bytes &src, u16 ethertype, const Bytes &payload) {
    return dst + src + be16(ethertype) + payload;
}

/* --------------------------------------------------------------- decoders */
static std::string dns_name(const Bytes &data, size_t off, size_t &next_off) {
    StrList labels;
    bool jumped = false;
    long nxt = -1;
    int guard = 0;
    while (guard < 64) {
        guard++;
        if (off >= data.size()) break;
        u8 n = (u8)data[off];
        if (n == 0) { off += 1; break; }
        if ((n & 0xC0) == 0xC0) {
            if (off + 1 >= data.size()) break;
            size_t ptr = ((n & 0x3F) << 8) | (u8)data[off + 1];
            if (!jumped) nxt = (long)(off + 2);
            jumped = true;
            off = ptr;
            continue;
        }
        labels.push_back(ascii_replace(data.substr(off + 1, n)));  /* a short label is kept, as sliced */
        off += 1 + n;
    }
    next_off = (nxt >= 0) ? (size_t)nxt : off;
    std::string s;
    for (size_t i = 0; i < labels.size(); i++) { if (i) s += "."; s += labels[i]; }
    return s;
}

static std::string tls_sni(const Bytes &payload) {
    if (payload.size() < 6 || (u8)payload[0] != 0x16 || (u8)payload[5] != 0x01)
        return "";
    size_t p = 5 + 4 + 2 + 32;
    if (p >= payload.size()) return "";
    int sid = (u8)payload[p]; p += 1 + sid;
    if (p + 2 > payload.size()) return "";
    int cs = rdbe16(payload, p); p += 2 + cs;
    if (p >= payload.size()) return "";
    int cm = (u8)payload[p]; p += 1 + cm;
    if (p + 2 > payload.size()) return "";
    int ext_len = rdbe16(payload, p); p += 2;
    size_t end = p + ext_len;
    while (p + 4 <= end && p + 4 <= payload.size()) {
        int et = rdbe16(payload, p), el = rdbe16(payload, p + 2); p += 4;
        if (et == 0) {
            size_t q = p + 2;
            while (q + 3 <= p + el && q + 3 <= payload.size()) {
                int nt = (u8)payload[q], nl = rdbe16(payload, q + 1);
                if (nt == 0)
                    return ascii_replace(payload.substr(q + 3, nl));  /* clamped, like a slice */
                q += 3 + nl;
            }
        }
        p += el;
    }
    return "";
}

static std::string mqtt_connect_info(const Bytes &data) {
    if (data.size() < 12 || (u8)data[0] != 0x10) return "";
    size_t p = 1;
    while (p < 5 && ((u8)data[p] & 0x80)) p++;
    p += 1;
    if (p + 2 > data.size()) return "";
    int name_len = rdbe16(data, p);
    if (p + 2 + (size_t)name_len > data.size()) return "";
    Bytes name = data.substr(p + 2, name_len);
    if (name != "MQTT" && name != "MQIsdp") return "";
    size_t q = p + 2 + name_len;
    if (q + 6 > data.size()) return "MQTT CONNECT";
    int level = (u8)data[q], flags = (u8)data[q + 1];
    int keepalive = rdbe16(data, q + 2);
    int cid_len = rdbe16(data, q + 4);
    std::string cid = utf8_replace(q + 6 <= data.size() ? data.substr(q + 6, cid_len) : Bytes());
    std::string extra;
    if (flags & 0x80) extra += extra.empty() ? "username" : "/username";
    if (flags & 0x40) extra += extra.empty() ? "password" : "/password";
    if (flags & 0x04) extra += extra.empty() ? "will" : "/will";
    std::string lvl = level == 3 ? "3.1" : level == 4 ? "3.1.1" : level == 5 ? "5" : strf("%d", level);
    return strf("MQTT CONNECT (v%s, client id '%s', keepalive %ds%s)", lvl.c_str(), cid.c_str(), keepalive,
                extra.empty() ? "" : (", with " + extra).c_str());
}

/* Tuya LAN discovery */
static const char *TUYA_PREFIX = "\x00\x00\x55\xaa";
static const char *TUYA_SUFFIX = "\x00\x00\xaa\x55";
static const char *TUYA_PREFIX_35 = "\x00\x00\x66\x99";
static const char *TUYA_SUFFIX_35 = "\x00\x00\x99\x66";
static bool starts_with(const Bytes &b, const char *p, size_t n) { return b.size() >= n && memcmp(b.data(), p, n) == 0; }
static bool ends_with(const Bytes &b, const char *p, size_t n) { return b.size() >= n && memcmp(b.data() + b.size() - n, p, n) == 0; }

static std::string tuya_lan_decode(const Bytes &data) {
    if (starts_with(data, TUYA_PREFIX_35, 4) && ends_with(data, TUYA_SUFFIX_35, 4) && data.size() >= 50) {
        u32 cmd = rdbe32(data, 10), length = rdbe32(data, 14);
        if (18 + length + 4 != data.size()) return "";
        return strf("Tuya LAN protocol 3.4/3.5 announcement, command 0x%02x, %u bytes, encrypted "
                    "with this device's own key (not the shared one), so its contents stay sealed", cmd, length);
    }
    if (data.size() < 24 || !starts_with(data, TUYA_PREFIX, 4) || !ends_with(data, TUYA_SUFFIX, 4))
        return "";
    u32 length = rdbe32(data, 12);
    if (length < 8 || 16 + length != data.size()) return "";
    Bytes body = data.substr(16, length - 8);
    if (body.size() > 4 && body.substr(0, 4) == Bytes("\x00\x00\x00\x00", 4))
        body = body.substr(4);
    if (!body.empty() && body[0] == '{')
        return utf8_valid(body) ? body : std::string(); /* already JSON text */
    if (body.size() < 16 || body.size() % 16) return "";
    static const char *K = "yGAdlopoPVldABfn";
    Bytes key = md5(Bytes(K, 16)); /* 16-char shared broadcast key */
    Aes aes; aes.set_key(key);
    Bytes out;
    for (size_t i = 0; i < body.size(); i += 16) {
        u8 blk[16]; aes.decrypt_block((const u8 *)body.data() + i, blk);
        out += Bytes((char *)blk, 16);
    }
    int pad = out.empty() ? 0 : (u8)out[out.size() - 1];
    if (pad < 1 || pad > 16) return "";
    for (int i = 0; i < pad; i++) if ((u8)out[out.size() - 1 - i] != (u8)pad) return "";
    out = out.substr(0, out.size() - pad);
    if (out.empty() || out[0] != '{' || !utf8_valid(out)) return "";
    return out;
}

std::string hexdump(const Bytes &data, int width, u32 base) {
    std::string out;
    for (size_t off = 0; off < data.size(); off += width) {
        Bytes chunk = data.substr(off, width);
        std::string hexed, text;
        for (size_t i = 0; i < chunk.size(); i++) { hexed += strf(i ? " %02x" : "%02x", (u8)chunk[i]); }
        for (size_t i = 0; i < chunk.size(); i++) { u8 c = (u8)chunk[i]; text += (c >= 32 && c < 127) ? (char)c : '.'; }
        out += strf("%08x  %-*s  %s", base + (u32)off, width * 3 - 1, hexed.c_str(), text.c_str());
        if (off + width < data.size()) out += "\n";
    }
    return out;
}

bool ipv4_endpoints(const Bytes &frame, Bytes &src, Bytes &dst) {
    if (frame.size() < 14 + 20 || frame.substr(12, 2) != be16(ETH_IP)) return false;
    Bytes packet = frame.substr(14);
    if (((u8)packet[0] >> 4) != 4) return false;
    int ihl = ((u8)packet[0] & 0x0F) * 4;
    if (ihl < 20 || (int)packet.size() < ihl) return false;
    int total = rdbe16(packet, 2);
    if (total < ihl || total > (int)packet.size()) return false;
    src = packet.substr(12, 4); dst = packet.substr(16, 4);
    return true;
}

std::string describe_frame(const Bytes &frame) {
    if (frame.size() < 14) return strf("runt frame, %d bytes", (int)frame.size());
    u16 etype = rdbe16(frame, 12);
    Bytes p = frame.substr(14);
    if (etype == ETH_ARP) {
        if (p.size() < 28) return "ARP (truncated)";
        int op = rdbe16(p, 6);
        Bytes sender = p.substr(14, 4), target = p.substr(24, 4);
        if (op == 1) return strf("ARP who has %s, tell %s", ip2str(target).c_str(), ip2str(sender).c_str());
        if (op == 2) return strf("ARP %s is at %s", ip2str(sender).c_str(), mac2str(p.substr(8, 6)).c_str());
        return strf("ARP opcode %d", op);
    }
    if (etype != ETH_IP) return strf("ethertype 0x%04x, %d bytes", etype, (int)p.size());
    if (p.size() < 20) return "IPv4 (truncated)";
    int ihl = ((u8)p[0] & 0x0F) * 4;
    int proto = (u8)p[9];
    Bytes src = p.substr(12, 4), dst = p.substr(16, 4);
    Bytes body = p.substr(ihl);
    if (proto == IP_ICMP && body.size() >= 4) {
        std::string kind = (u8)body[0] == 0 ? "echo reply" : (u8)body[0] == 8 ? "echo request" : strf("type %d", (u8)body[0]);
        return strf("ICMP %s %s to %s", kind.c_str(), ip2str(src).c_str(), ip2str(dst).c_str());
    }
    if (proto == IP_UDP && body.size() >= 8) {
        int sp = rdbe16(body, 0), dp = rdbe16(body, 2);
        /* A reply has the service on its SOURCE port, so both ends are
         * candidates; a well-known (< 1024) port wins. */
        struct Svc { int port; const char *name; };
        static const Svc services[] = {{53, "DNS"}, {67, "DHCP"}, {68, "DHCP"}, {123, "NTP"}, {1900, "SSDP"},
                                       {5353, "mDNS"}, {6666, "Tuya LAN"}, {6667, "Tuya LAN"}};
        const char *named = 0, *low = 0;
        int ends[2] = {dp, sp};
        for (int e = 0; e < 2; e++)
            for (size_t k = 0; k < sizeof(services) / sizeof(services[0]); k++)
                if (services[k].port == ends[e]) {
                    if (!named) named = services[k].name;
                    if (!low && ends[e] < 1024) low = services[k].name;
                }
        const char *what = low ? low : named;
        return strf("UDP %s:%d to %s:%d%s, %d bytes", ip2str(src).c_str(), sp, ip2str(dst).c_str(), dp,
                    what ? strf(" (%s)", what).c_str() : "", (int)body.size() - 8);
    }
    if (proto == IP_TCP && body.size() >= 20) {
        int sp = rdbe16(body, 0), dp = rdbe16(body, 2);
        int flags = (u8)body[13];
        std::string names;
        const char *nm[5] = {"SYN", "ACK", "PSH", "FIN", "RST"}; int bits[5] = {TCP_SYN, TCP_ACK, TCP_PSH, TCP_FIN, TCP_RST};
        for (int i = 0; i < 5; i++) if (flags & bits[i]) { if (!names.empty()) names += ","; names += nm[i]; }
        int payload = (int)body.size() - ((u8)body[12] >> 4) * 4;
        return strf("TCP %s:%d to %s:%d [%s]%s", ip2str(src).c_str(), sp, ip2str(dst).c_str(), dp,
                    names.empty() ? "none" : names.c_str(), payload > 0 ? strf(", %d bytes", payload).c_str() : "");
    }
    return strf("IPv4 protocol %d, %s to %s", proto, ip2str(src).c_str(), ip2str(dst).c_str());
}

static Bytes mqtt_varint(int n) {
    Bytes out;
    for (;;) { int b = n % 128; n /= 128; out += (char)(b | (n ? 0x80 : 0)); if (!n) return out; }
}
static Bytes mqtt_publish_packet(const Bytes &topic, const Bytes &payload) {
    Bytes body = be16((u16)topic.size()) + topic + payload;
    return Bytes("\x30", 1) + mqtt_varint((int)body.size()) + body;
}
static bool mqtt_topic_matches(const std::string &filt, const std::string &topic) {
    StrList f = split(filt, "/"), t = split(topic, "/");
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] == "#") return true;
        if (i >= t.size()) return false;
        if (f[i] != "+" && f[i] != t[i]) return false;
    }
    return f.size() == t.size();
}

std::string describe_payload(const Bytes &data) {
    std::string sni = tls_sni(data);
    if (!sni.empty()) return "TLS ClientHello, server name " + sni;
    if (!data.empty() && (u8)data[0] == 0x16) return "TLS ClientHello (no server name)";
    size_t nl = data.find("\r\n");
    Bytes head = nl == std::string::npos ? data : data.substr(0, nl);
    Bytes m4 = head.substr(0, 4);
    if (m4 == "GET " || m4 == "POST" || m4 == "PUT " || m4 == "HEAD" || head.substr(0, 7) == "OPTIONS") {
        std::string line = latin1_to_utf8(head);
        std::string host;
        StrList parts = split(data, "\r\n");
        for (size_t i = 0; i < parts.size(); i++) {
            std::string low = lower(parts[i]);
            if (low.substr(0, 5) == "host:") { host = " host " + latin1_to_utf8(strip(parts[i].substr(5))); break; }
        }
        return "HTTP " + line + host;
    }
    std::string mqtt = mqtt_connect_info(data);
    if (!mqtt.empty()) return mqtt;
    std::string text;
    for (size_t i = 0; i < data.size() && i < 48; i++) { u8 c = (u8)data[i]; text += (c >= 32 && c < 127) ? (char)c : '.'; }
    return strf("%d bytes: %s", (int)data.size(), text.c_str());
}

/* ------------------------------------------------------------ TLS 1.2 PRF */
static Bytes tls12_prf(const Bytes &secret, const Bytes &label, const Bytes &seed, int length) {
    Bytes out;
    Bytes prf_seed = label + seed;
    Bytes a = hmac_sha256(secret, prf_seed);
    while ((int)out.size() < length) {
        out += hmac_sha256(secret, a + prf_seed);
        a = hmac_sha256(secret, a);
    }
    return out.substr(0, length);
}
static Bytes tls_psk_premaster(const Bytes &psk) {
    Bytes size = be16((u16)psk.size());
    return size + Bytes(psk.size(), '\0') + size + psk;
}

/* --------------------------------------------------- TLS-PSK GCM server */
class TLS12PSKGCMServer {
public:
    std::vector<Bytes> psks;
    bool tuya_legacy;
    Bytes client_random, server_random;
    Bytes transcript;
    std::string state;
    u64 client_seq, server_seq;
    Bytes client_key, server_key, client_iv, server_iv;
    int max_plaintext;
    bool ok;

    TLS12PSKGCMServer(const std::vector<Bytes> &psks_, const Bytes &crand, const Bytes &srand,
                      const Bytes &client_hello, const Bytes &server_handshakes, int maxpt, bool tuya)
        : tuya_legacy(tuya), client_seq(0), server_seq(0), max_plaintext(maxpt), ok(false) {
        for (size_t i = 0; i < psks_.size(); i++)
            if (!psks_[i].empty() && psks_[i].size() <= 0xFFFF) psks.push_back(psks_[i]);
        if (psks.empty()) return;
        if (crand.size() != 32 || srand.size() != 32) return;
        if (client_hello.empty() || (u8)client_hello[0] != 0x01) return;
        if (server_handshakes.empty() || (u8)server_handshakes[0] != 0x02) return;
        if (maxpt < 1 || maxpt > TLS_MAX_PLAINTEXT) return;
        client_random = crand; server_random = srand;
        transcript = client_hello + server_handshakes;
        state = "client_key_exchange";
        ok = true;
    }

    static bool decrypt(const Bytes &key, const Bytes &iv, u64 seq, u8 ct, const Bytes &version, const Bytes &fragment, Bytes &out) {
        if (version.size() != 2 || fragment.size() < 8 + 16 || (int)fragment.size() > TLS_MAX_CIPHERTEXT) return false;
        Bytes explicit_ = fragment.substr(0, 8);
        Bytes sealed = fragment.substr(8);
        Bytes ciphertext = sealed.substr(0, sealed.size() - 16);
        Bytes tag = sealed.substr(sealed.size() - 16);
        Bytes aad = be64(seq) + Bytes(1, (char)ct) + version + be16((u16)ciphertext.size());
        u8 t16[16]; memcpy(t16, tag.data(), 16);
        return aes_gcm_decrypt(key, iv + explicit_, aad, ciphertext, t16, out);
    }
    static Bytes encrypt(const Bytes &key, const Bytes &iv, u64 seq, u8 ct, const Bytes &version, const Bytes &plaintext) {
        Bytes seqb = be64(seq);
        Bytes aad = seqb + Bytes(1, (char)ct) + version + be16((u16)plaintext.size());
        Bytes ciphertext; u8 tag[16];
        aes_gcm_encrypt(key, iv + seqb, aad, plaintext, ciphertext, tag);
        Bytes fragment = seqb + ciphertext + Bytes((char *)tag, 16);
        return Bytes(1, (char)ct) + version + be16((u16)fragment.size()) + fragment;
    }

    bool add_client_key_exchange(const Bytes &message, Bytes &identity) {
        if (state != "client_key_exchange" || message.size() < 6 || (u8)message[0] != 0x10) return false;
        u32 declared = ((u32)(u8)message[1] << 16) | ((u8)message[2] << 8) | (u8)message[3];
        if (declared != message.size() - 4) return false;
        int ident_len = rdbe16(message, 4);
        if ((size_t)ident_len != message.size() - 6) return false;
        transcript += message;
        state = "change_cipher_spec";
        identity = message.substr(6);
        bool alnum = !identity.empty();
        for (size_t i = 0; i < identity.size(); i++) { char c = identity[i]; if (!((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z'))) { alnum = false; break; } }
        bool present = false; for (size_t i = 0; i < psks.size(); i++) if (psks[i] == identity) present = true;
        if (tuya_legacy && identity.size() == 32 && alnum && !present)
            psks.push_back(identity);
        return true;
    }
    bool change_cipher_spec(const Bytes &payload) {
        if (state != "change_cipher_spec" || payload != Bytes("\x01", 1)) return false;
        state = "client_finished";
        return true;
    }
    void candidate_schedule(const Bytes &psk, Bytes &master, Bytes &ckey, Bytes &skey, Bytes &civ, Bytes &siv) {
        Bytes premaster = tls_psk_premaster(psk);
        Bytes session_hash = sha256(transcript);
        master = tls12_prf(premaster, Bytes("extended master secret", 22), session_hash, 48);
        Bytes block = tls12_prf(master, Bytes("key expansion", 13), server_random + client_random, 40);
        ckey = block.substr(0, 16); skey = block.substr(16, 16); civ = block.substr(32, 4); siv = block.substr(36, 4);
    }
    bool finish(const Bytes &version, const Bytes &fragment, Bytes &server_flight) {
        if (state != "client_finished") return false;
        Bytes transcript_hash = sha256(transcript);
        bool found = false;
        Bytes master, ckey, skey, civ, siv, plain;
        for (size_t i = 0; i < psks.size(); i++) {
            Bytes m, ck, sk, ci, si, p;
            candidate_schedule(psks[i], m, ck, sk, ci, si);
            if (!decrypt(ck, ci, 0, 0x16, version, fragment, p)) continue;
            Bytes expected = Bytes("\x14\x00\x00\x0c", 4) + tls12_prf(m, Bytes("client finished", 15), transcript_hash, 12);
            if (ct_equal(p, expected)) { master = m; ckey = ck; skey = sk; civ = ci; siv = si; plain = p; found = true; break; }
        }
        psks.clear(); tuya_legacy = false;
        if (!found) return false;
        client_key = ckey; server_key = skey; client_iv = civ; server_iv = siv;
        client_seq = 1;
        transcript += plain;
        Bytes server_verify = tls12_prf(master, Bytes("server finished", 15), sha256(transcript), 12);
        Bytes server_finished = Bytes("\x14\x00\x00\x0c", 4) + server_verify;
        Bytes encrypted = encrypt(server_key, server_iv, 0, 0x16, Bytes("\x03\x03", 2), server_finished);
        server_seq = 1;
        state = "established";
        server_flight = Bytes("\x14\x03\x03\x00\x01\x01", 6) + encrypted;
        return true;
    }
    bool established() const { return state == "established"; }
    bool decrypt_record(u8 ct, const Bytes &version, const Bytes &fragment, Bytes &out) {
        if (!established()) return false;
        if (!decrypt(client_key, client_iv, client_seq, ct, version, fragment, out)) return false;
        if ((int)out.size() > max_plaintext) return false;
        client_seq++;
        return true;
    }
    /* One application record; false (Python: TLSProtocolError) before Finished
     * or past the negotiated fragment limit. */
    bool encrypt_record(u8 ct, const Bytes &plaintext, Bytes &record, const Bytes &version = Bytes("\x03\x03", 2)) {
        if (!established() || (int)plaintext.size() > max_plaintext || version.size() != 2) return false;
        record = encrypt(server_key, server_iv, server_seq, ct, version, plaintext);
        server_seq++;
        return true;
    }
    Bytes encrypt_records(u8 ct, const Bytes &plaintext) {
        Bytes out, rec;
        if (plaintext.empty()) { if (encrypt_record(ct, plaintext, rec)) out = rec; return out; }
        for (size_t off = 0; off < plaintext.size(); off += max_plaintext) {
            if (!encrypt_record(ct, plaintext.substr(off, max_plaintext), rec)) break;
            out += rec;
        }
        return out;
    }
    void clear_secrets() { psks.clear(); tuya_legacy = false; client_key.clear(); server_key.clear(); client_iv.clear(); server_iv.clear(); transcript.clear(); state = "closed"; }
};

/* a parsed ClientHello */
struct ClientHello {
    bool ok;
    std::string version;
    Bytes random;
    Bytes handshake;
    std::vector<u16> suites;
    std::vector<u16> psk;
    std::map<u16, Bytes> extensions;
    std::string sni;
    ClientHello() : ok(false) {}
};
static ClientHello tls_client_hello(const Bytes &data) {
    ClientHello h;
    if (data.size() < 45 || (u8)data[0] != 0x16 || (u8)data[5] != 0x01) return h;
    int record_len = rdbe16(data, 3);
    if ((int)data.size() < 5 + record_len || record_len < 4 || record_len > TLS_MAX_PLAINTEXT) return h;
    u32 handshake_len = ((u32)(u8)data[6] << 16) | ((u8)data[7] << 8) | (u8)data[8];
    if (handshake_len + 4 > (u32)record_len) return h;
    size_t handshake_end = 9 + handshake_len;
    u16 ver = rdbe16(data, 9);
    h.random = data.substr(11, 32);
    size_t p = 11 + 32;
    if (p >= data.size()) return h;
    p += 1 + (u8)data[p];
    if (p + 2 > handshake_end) return h;
    int cs_len = rdbe16(data, p);
    if (cs_len % 2) return h;
    p += 2;
    if (p + cs_len + 1 > handshake_end) return h;
    for (int i = 0; i < cs_len; i += 2) h.suites.push_back(rdbe16(data, p + i));
    p += cs_len;
    p += 1 + (u8)data[p];
    if (p > handshake_end) return h;
    if (p + 2 <= handshake_end) {
        int ext_len = rdbe16(data, p); p += 2;
        if (p + ext_len != handshake_end) return h;
        size_t end = p + ext_len;
        while (p + 4 <= end) {
            u16 et = rdbe16(data, p), el = rdbe16(data, p + 2);
            if (p + 4 + el > end) return h;
            if (h.extensions.count(et)) return h;
            h.extensions[et] = data.substr(p + 4, el);
            p += 4 + el;
        }
        if (p != end) return h;
    } else if (p != handshake_end) return h;
    h.version = ver == 0x0301 ? "1.0" : ver == 0x0302 ? "1.1" : ver == 0x0303 ? "1.2" : ver == 0x0304 ? "1.3" : strf("0x%04x", ver);
    h.handshake = data.substr(5, handshake_end - 5);
    static const u16 psks[] = {0x008A,0x008B,0x008C,0x008D,0x00A8,0x00AE,0x00AF,0xC0A4,0xC0A8};
    for (size_t i = 0; i < h.suites.size(); i++)
        for (size_t j = 0; j < sizeof(psks)/sizeof(psks[0]); j++)
            if (h.suites[i] == psks[j]) h.psk.push_back(h.suites[i]);
    h.sni = tls_sni(data);
    h.ok = true;
    return h;
}
/* The clear identity from a TLS-PSK ClientKeyExchange record; false if it is not one. */
static bool tls_psk_identity(const Bytes &data, Bytes &ident) {
    if (data.size() < 11 || (u8)data[0] != 0x16 || (u8)data[5] != 0x10) return false;
    size_t n = rdbe16(data, 9);
    if (11 + n > data.size()) return false;
    ident = data.substr(11, n);
    return true;
}
static std::string tls_suite_name(u16 s) {
    switch (s) {
    case 0x008A: return "TLS_PSK_WITH_RC4_128_SHA";
    case 0x008B: return "TLS_PSK_WITH_3DES_EDE_CBC_SHA";
    case 0x008C: return "TLS_PSK_WITH_AES_128_CBC_SHA";
    case 0x008D: return "TLS_PSK_WITH_AES_256_CBC_SHA";
    case 0x00A8: return "TLS_PSK_WITH_AES_128_GCM_SHA256";
    case 0x00AE: return "TLS_PSK_WITH_AES_128_CBC_SHA256";
    case 0x00AF: return "TLS_PSK_WITH_AES_256_CBC_SHA384";
    case 0xC0A4: return "TLS_PSK_WITH_AES_128_CCM";
    case 0xC0A8: return "TLS_PSK_WITH_AES_128_CCM_8";
    }
    return strf("0x%04x", s);
}

/* --------------------------------------------------------------- EthPcap */
EthPcap::EthPcap(const std::string &path) : count(0), own_(true) {
    f_ = pyc::py_open(path, "wb", 0); /* a UTF-8 path, as Python open() takes it */
    if (f_) { char h[24]; wr32le(h, 0xA1B2C3D4); wr16le(h + 4, 2); wr16le(h + 6, 4); wr32le(h + 8, 0); wr32le(h + 12, 0); wr32le(h + 16, 65535); wr32le(h + 20, 1); fwrite(h, 1, 24, f_); fflush(f_); }
}
EthPcap::~EthPcap() { close(); }
void EthPcap::write(const Bytes &frame, u64 us) {
    if (!f_) return;
    char h[16]; wr32le(h, (u32)(us / 1000000)); wr32le(h + 4, (u32)(us % 1000000)); wr32le(h + 8, (u32)frame.size()); wr32le(h + 12, (u32)frame.size());
    fwrite(h, 1, 16, f_); fwrite(frame.data(), 1, frame.size(), f_); fflush(f_); count++;
}
void EthPcap::close() { if (f_ && own_) fclose(f_); f_ = 0; }

/* =============================================================== HostIO */
/* A helper thread servicing real sockets: --net-forward listeners, the forward
 * and relay connections, and the online (--net-out) UDP flows. Blocking work
 * (DNS lookups, outbound connects) runs on short-lived worker threads that
 * post results back here; everything else stays in device time. */
struct HostSink;   /* outlives the HostIO for any worker still running */
struct DnsResult { int id; bool ok; Bytes ip; };
struct ConnectResult { u64 conn_id; sock_t sock; };   /* BK_BAD_SOCK = failed */
class HostIO {
public:
    Mutex lock;
    std::map<sock_t, TcpConn *> socks;      /* forward/relay connection sockets */
    std::map<sock_t, int> listeners;        /* listen sock -> device dport */
    std::deque<std::pair<sock_t, int> > accepted;
    std::map<sock_t, std::deque<Bytes> > drains; /* sockets being flushed then closed */
    std::deque<DnsResult> dns_results;
    std::deque<ConnectResult> connect_results;
    std::map<sock_t, UdpKey> udp_socks;     /* relay UDP socket -> its flow */
    std::deque<std::pair<UdpKey, Bytes> > udp_in;
    /* Workers capture this; a device reboot bumps it so a late result from
     * the old TCP/IP instance cannot answer a query in the new one. */
    long generation;
    volatile long stop_;
    Thread thread;
    HostSink *sink;

    HostIO();
    ~HostIO();
    sock_t listen(int lport, int dport, std::string *err);
    void resolve(const std::string &name, int id);
    void connect(u64 conn_id, const std::string &ip, int port);
    void attach(sock_t s, TcpConn *conn);
    sock_t udp_socket_for(const UdpKey &flow);
    void close_sock(sock_t s);
    void handoff(sock_t s, std::deque<Bytes> &buf); /* flush remaining bytes then close */
    void reset_connections();
    void shutdown();
    void run();
};

/* =============================================================== TcpConn */
class TcpConn {
public:
    NetStack *stack;
    u64 id;             /* unique per process: how worker results find this conn */
    Bytes lip, rip; int lport, rport;
    std::string kind;   /* capture | relay | forward | client */
    std::string state;
    u32 snd_una, snd_nxt, rcv_nxt;
    int peer_window;
    Bytes unacked; u32 unacked_seq;
    Bytes send_buf;
    u64 last_send_us, last_activity_us, opened_us;
    Bytes captured; bool described;
    Bytes rx_buf;
    std::string proto;      /* "" | http | mqtt | tls | raw */
    bool answered;
    StrList subscribed;
    TLS12PSKGCMServer *tls;
    Bytes tls_handshake_buf, tls_plain_buf;
    std::string tls_app_proto;
    bool fin_sent, fin_rcvd, close_after_flush;
    sock_t sock;
    std::deque<Bytes> to_host, from_host;
    bool host_closed, shutdown_pending, close_pending;
    int syn_tries;
    std::string log_prefix;
    std::string request_path;

    TcpConn(NetStack *st, const Bytes &lip_, int lport_, const Bytes &rip_, int rport_, const std::string &kind_);
    ~TcpConn();
    void send(u8 flags, const Bytes &payload = Bytes(), const Bytes &options = Bytes());
    void queue(const Bytes &data) { send_buf += data; }
    void pump(u64 now_us);
    void on_segment(u32 seq, u32 ack, u8 flags, int window, const Bytes &payload, u64 now_us);
    void deliver(const Bytes &data, u64 now_us);
    void http_serve();
    void mqtt_send(const Bytes &payload, bool over_tls);
    void mqtt_serve(bool over_tls);
    void tls_serve();
    void tls_note_identity(const Bytes &ident);
    void tls_fail(const std::string &reason);
    Bytes tls_server_hello(u16 suite, const std::map<u16, Bytes> &client_exts);
    void mqtt_deliver(bool over_tls);
    void close_host();
    void service(u64 now_us);
};

} /* namespace netstack (reopened after method bodies include NetStack) */

#include "netstack_impl.h"
