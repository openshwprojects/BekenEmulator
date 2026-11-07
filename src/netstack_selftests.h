/* Self-tests: a fake device (just the frames) against the stack. A port of
 * src/netstack.py _run_selftests(): the same checks, in the same order, with
 * the same messages and count. Included inside namespace netstack at the end
 * of the netstack.cpp translation unit, so it reaches the internal classes
 * (TcpConn, TLS12PSKGCMServer, the static helpers) the Python tests reach. */
#ifndef BK_NETSTACK_SELFTESTS_H
#define BK_NETSTACK_SELFTESTS_H

struct NsFail {};          /* a failed check: unwind to netstack_selftests() */
struct NsFrontEndBug {};   /* thrown by a deliberately broken on_frame callback */
static int ns_checks;

static void ncheck(bool cond, const std::string &msg) {
    if (!cond) {
        printf("  [FAIL] %s\n", msg.c_str());
        fflush(stdout);
        throw NsFail();
    }
    ns_checks++;
    printf("  [PASS] %s\n", msg.c_str());
}
/* a byte-string literal with its exact length (embedded NULs included) */
#define NSB(lit) Bytes(lit, sizeof(lit) - 1)
static void nsection(const char *title) { printf("%s\n", title); }

/* ---- capture plumbing: log lines, frames out, frames traced */
static void ns_log(void *ctx, const std::string &m) { ((StrList *)ctx)->push_back(m); }
static void ns_out(void *ctx, const Bytes &f) { ((std::vector<Bytes> *)ctx)->push_back(f); }
struct NsSeen { std::string dir; Bytes frame; u64 us; };
static void ns_on_frame(void *ctx, const std::string &dir, const Bytes &frame, u64 us) {
    NsSeen s; s.dir = dir; s.frame = frame; s.us = us;
    ((std::vector<NsSeen> *)ctx)->push_back(s);
}
static void ns_on_frame_throws(void *, const std::string &, const Bytes &, u64) { throw NsFrontEndBug(); }
static void ns_on_frame_noop(void *, const std::string &, const Bytes &, u64) {}

static bool any_log(const StrList &logs, const std::string &a, const std::string &b = std::string()) {
    for (size_t i = 0; i < logs.size(); i++)
        if (contains(logs[i], a) && (b.empty() || contains(logs[i], b))) return true;
    return false;
}
static bool any_log_from(const StrList &logs, size_t from, const std::string &a) {
    for (size_t i = from; i < logs.size(); i++) if (contains(logs[i], a)) return true;
    return false;
}

static NetConfig ns_cfg(const char *mac_hex, bool gw) {
    NetConfig c; c.mac = H(mac_hex); c.gateway = gw; return c;
}
static void ns_wire(NetStack &s, StrList &logs, std::vector<Bytes> &out) {
    (void)logs;
    s.output = ns_out; s.output_ctx = &out;
}

static Bytes S(const Bytes &b, long long a, long long e) { return bslice(b, a, e); }
static Bytes F(const Bytes &b, long long a) { return bfrom(b, a); }
static u8 at(const Bytes &b, size_t i) { return i < b.size() ? (u8)b[i] : 0; }
static u32 rd16(const Bytes &b, size_t o) { return o + 2 <= b.size() ? rdbe16(b, o) : 0; }
static u32 rd32(const Bytes &b, size_t o) { return o + 4 <= b.size() ? rdbe32(b, o) : 0; }
static Bytes zeros(size_t n) { return Bytes(n, '\0'); }
static Bytes byte_range(int a, int b) { Bytes r; for (int i = a; i < b; i++) r += (char)i; return r; }
static Bytes be24(u32 v) { return be32(v).substr(1); }
static Bytes arp_hdr(int op) { return be16(1) + be16(ETH_IP) + Bytes(1, (char)6) + Bytes(1, (char)4) + be16((u16)op); }

static const char *DEV_MAC_HEX = "c8478c000001";

/* The tests proper. A struct so one friend declaration in NetStack gives
 * them the internal access the Python tests have. */
struct NsPcapRec { u64 t; u32 incl, orig; Bytes data; };

struct NsSelfTest {
static TcpConn *conn_at(NetStack &s, const Bytes &lip, int lport, const Bytes &rip, int rport) {
    ConnKey k; k.lip = lip; k.lport = lport; k.rip = rip; k.rport = rport;
    std::map<ConnKey, TcpConn *>::iterator it = s.conns_.find(k);
    if (it == s.conns_.end()) {
        printf("  [FAIL] no connection %s:%d <-> %s:%d\n", ip2str(lip).c_str(), lport, ip2str(rip).c_str(), rport);
        throw NsFail();
    }
    return it->second;
}
static bool has_conn(NetStack &s, const Bytes &lip, int lport, const Bytes &rip, int rport) {
    ConnKey k; k.lip = lip; k.lport = lport; k.rip = rip; k.rport = rport;
    return s.conns_.count(k) != 0;
}
static TcpConn *first_kind(NetStack &s, const char *kind) {
    for (std::map<ConnKey, TcpConn *>::iterator it = s.conns_.begin(); it != s.conns_.end(); ++it)
        if (it->second->kind == kind) return it->second;
    printf("  [FAIL] no %s connection\n", kind);
    throw NsFail();
}

/* TCP segments we sent: (seq, ack, flags, payload, sport, dport) */
struct NsSeg { u32 seq, ack; int flags; Bytes p; int sp, dp; };
static std::vector<NsSeg> segs(const std::vector<Bytes> &frames, int dport = -1, size_t from = 0) {
    std::vector<NsSeg> res;
    for (size_t i = from; i < frames.size(); i++) {
        const Bytes &f = frames[i];
        if (S(f, 12, 14) == NSB("\x08\x00") && at(f, 14 + 9) == IP_TCP) {
            Bytes t = F(f, 14 + 20);
            NsSeg s; s.sp = rd16(t, 0); s.dp = rd16(t, 2); s.seq = rd32(t, 4); s.ack = rd32(t, 8);
            s.flags = at(t, 13); s.p = F(t, (at(t, 12) >> 4) * 4);
            if (dport < 0 || s.dp == dport) res.push_back(s);
        }
    }
    return res;
}
static Bytes joined_payload(const std::vector<NsSeg> &v, int sport, bool nonempty = false) {
    Bytes out;
    for (size_t i = 0; i < v.size(); i++) if (v[i].sp == sport && (!nonempty || !v[i].p.empty())) out += v[i].p;
    return out;
}

/* UDP datagrams we sent: (src ip, sport, dport, payload) */
struct NsUdp { Bytes src; int sp, dp; Bytes p; };
static std::vector<NsUdp> udp_out(const std::vector<Bytes> &frames, int sport = -1, int dport = -1, size_t from = 0) {
    std::vector<NsUdp> res;
    for (size_t i = from; i < frames.size(); i++) {
        const Bytes &f = frames[i];
        if (S(f, 12, 14) == NSB("\x08\x00") && at(f, 14 + 9) == IP_UDP) {
            Bytes u = F(f, 14 + (at(f, 14) & 0x0F) * 4);
            NsUdp d; d.sp = rd16(u, 0); d.dp = rd16(u, 2); d.src = S(f, 14 + 12, 14 + 16); d.p = F(u, 8);
            if ((sport < 0 || d.sp == sport) && (dport < 0 || d.dp == dport)) res.push_back(d);
        }
    }
    return res;
}

/* Device SYN to dst:dport, then its ACK of our SYN-ACK; returns the conn and
 * the device's next sequence number. */
static TcpConn *open_capture(NetStack &st, const Bytes &dst, int dport, int sport, u32 &seq_out, u32 isn = 5000) {
    Bytes dev_mac = H(DEV_MAC_HEX);
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(st.device_ip_, dst, sport, dport, isn, 0, TCP_SYN, 8192)));
    TcpConn *c = conn_at(st, dst, dport, st.device_ip_, sport);
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(st.device_ip_, dst, sport, dport, isn + 1, c->snd_nxt, TCP_ACK, 8192)));
    seq_out = isn + 1;
    return c;
}
static u32 dev_send(NetStack &st, TcpConn *c, const Bytes &dst, int dport, int sport, u32 seq, const Bytes &payload,
                    u8 flags = TCP_ACK | TCP_PSH, int window = 8192) {
    Bytes dev_mac = H(DEV_MAC_HEX);
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(st.device_ip_, dst, sport, dport, seq, c->snd_nxt, flags, window, payload)));
    return seq + (u32)payload.size();
}
static void dns_ask(NetStack &st, const std::string &name, int qtype = 1, u16 txid = 0x4242, int sport = 5353) {
    Bytes labels;
    StrList parts = split(name, ".");
    for (size_t i = 0; i < parts.size(); i++) labels += Bytes(1, (char)parts[i].size()) + parts[i];
    labels += '\0';
    Bytes q = be16(txid) + be16(0x0100) + be16(1) + be16(0) + be16(0) + be16(0) + labels + be16((u16)qtype) + be16(1);
    st.input(eth_frame(st.mac_, H(DEV_MAC_HEX), ETH_IP, udp_packet(st.device_ip_, st.ip, sport, 53, q)));
}
static Bytes hello_with_extensions(const Bytes &hello_base, const Bytes &ext_blob) {
    Bytes body = hello_base + be16((u16)ext_blob.size()) + ext_blob;
    Bytes msg = Bytes("\x01", 1) + be24((u32)body.size()) + body;
    return Bytes("\x16\x03\x03", 3) + be16((u16)msg.size()) + msg;
}
static std::vector<Bytes> tls_records(Bytes stream) {
    std::vector<Bytes> records;
    while (!stream.empty()) {
        if (stream.size() < 5) return std::vector<Bytes>();
        size_t length = rdbe16(stream, 3);
        if (stream.size() < 5 + length) return std::vector<Bytes>();
        records.push_back(stream.substr(0, 5 + length));
        stream = stream.substr(5 + length);
    }
    return records;
}
static Bytes tuya_packet(const Bytes &payload, u32 cmd = 0x13, bool retcode = true) {
    Bytes body = (retcode ? zeros(4) : Bytes()) + payload;
    return Bytes(TUYA_PREFIX, 4) + be32(0) + be32(cmd) + be32((u32)body.size() + 8) + body
           + Bytes("\x24\x2d\x2c\x95", 4) + Bytes(TUYA_SUFFIX, 4);
}
static Bytes pkcs7(const Bytes &raw) { size_t pad = 16 - raw.size() % 16; return raw + Bytes(pad, (char)pad); }
static Bytes ping_with_checksum(const Bytes &icmp) {
    return icmp.substr(0, 2) + be16(inet_checksum(icmp)) + icmp.substr(4);
}

static int netstack_selftests_body() {
    const Bytes dev_mac = H(DEV_MAC_HEX);
    const Bytes BCAST = BCAST_MAC();
    const Bytes Z4 = zeros(4), FF4 = NSB("\xff\xff\xff\xff");
    const Bytes TLS12 = NSB("\x03\x03");

    StrList logs; std::vector<Bytes> out;
    NetStack st(ns_cfg("020000000001", true), ns_log, &logs);
    ns_wire(st, logs, out);

    nsection("== ARP ==");
    Bytes req = arp_hdr(1) + dev_mac + str2ip("192.168.1.100") + zeros(6) + st.ip;
    st.input(eth_frame(BCAST, dev_mac, ETH_ARP, req));
    ncheck(!out.empty() && S(out.back(), 12, 14) == NSB("\x08\x06") && S(out.back(), 14 + 8, 14 + 14) == st.mac_,
           "ARP request for our IP answered with our MAC");

    nsection("== DHCP server ==");
    Bytes disc = zeros(240);
    disc.replace(0, 4, "\x01\x01\x06\x00", 4); disc.replace(4, 4, "\x12\x34\x56\x78", 4);
    disc.replace(28, 6, dev_mac); disc.replace(236, 4, "\x63\x82\x53\x63", 4);
    st.input(eth_frame(BCAST, dev_mac, ETH_IP, udp_packet(Z4, FF4, 68, 67, disc + NSB("\x35\x01\x01\xff"))));
    Bytes offer = F(out.back(), 14);
    Bytes yiaddr = S(offer, 20 + 8 + 16, 20 + 8 + 20);
    ncheck(yiaddr == str2ip("192.168.1.100"), "DISCOVER -> OFFER of the first pool address");
    st.input(eth_frame(BCAST, dev_mac, ETH_IP, udp_packet(Z4, FF4, 68, 67, disc + NSB("\x35\x01\x03\x32\x04") + yiaddr + NSB("\xff"))));
    ncheck(st.device_ip_ == yiaddr && any_log(logs, "got 192.168.1.100"), "REQUEST -> ACK, device address learned");

    nsection("== DNS sinkhole + TCP capture ==");
    Bytes q = be16(0xBEEF) + be16(0x0100) + be16(1) + be16(0) + be16(0) + be16(0)
              + NSB("\x03" "api" "\x04" "tuya" "\x03" "com" "\x00") + be16(1) + be16(1);
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, udp_packet(yiaddr, st.ip, 5000, 53, q)));
    Bytes resp = F(out.back(), 14 + 20 + 8);
    ncheck(S(resp, 0, 2) == NSB("\xbe\xef") && S(resp, -4, -2) == NSB("\x0a\xc8"), "A query answered from the 10.200/16 sinkhole");
    Bytes sink = F(resp, -4);
    ncheck(st.sinkhole_rev_.count(sink) && st.sinkhole_rev_[sink] == "api.tuya.com", "name remembered for that address");
    Bytes syn = tcp_segment(yiaddr, sink, 6000, 443, 1000, 0, TCP_SYN, 8192);
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, syn));
    Bytes sa = F(out.back(), 14 + 20);
    ncheck((at(sa, 13) & TCP_SYN) && (at(sa, 13) & TCP_ACK) && rd32(sa, 8) == 1001, "SYN to a sinkhole address gets SYN-ACK");
    TcpConn *conn = conn_at(st, sink, 443, yiaddr, 6000);
    u32 isn = conn->snd_nxt;
    {
        Bytes ext = NSB("\x00\x00") + be16(2 + 3 + 12) + be16(3 + 12) + NSB("\x00") + be16(12) + Bytes("a1.tuyaeu.com").substr(0, 12);
        Bytes body = NSB("\x03\x03") + zeros(32) + NSB("\x00") + NSB("\x00\x02\x13\x01") + NSB("\x01\x00") + be16((u16)ext.size()) + ext;
        Bytes hs = NSB("\x01") + be24((u32)body.size()) + body;
        Bytes rec = NSB("\x16\x03\x01") + be16((u16)hs.size()) + hs;
        st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(yiaddr, sink, 6000, 443, 1001, isn, TCP_ACK | TCP_PSH, 8192, rec)));
    }
    ncheck(any_log(logs, "server name a1.tuyaeu.co"), "captured ClientHello decoded to its server name");
    st.tick(10000000);
    ncheck(conn->fin_sent, "idle capture is closed with FIN");
    {
        Bytes mq = NSB("\x00\x04MQTT\x04\xc2\x00\x3c\x00\x0bobk8C000000\x00\x04user\x00\x04pass");
        ncheck(describe_payload(NSB("\x10") + Bytes(1, (char)mq.size()) + mq)
               == "MQTT CONNECT (v3.1.1, client id 'obk8C000000', keepalive 60s, with username/password)",
               "MQTT CONNECT decoded: protocol level, client id, keepalive, credential flags");
        ncheck(startswith(describe_payload(NSB("\x10\x90\x01") + mq + zeros(144 - mq.size())), "MQTT CONNECT (v3.1.1"),
               "...also behind a two-byte remaining-length field");
    }

    nsection("== NTP ==");
    {
        Bytes ntp = NSB("\x23") + zeros(47);
        st.input(eth_frame(st.mac_, dev_mac, ETH_IP, udp_packet(yiaddr, str2ip("10.200.0.9"), 123, 123, ntp)));
        Bytes r = F(out.back(), 14 + 20 + 8);
        ncheck(r.size() == 48 && (at(r, 0) & 7) == 4 && rd32(r, 40) > 3900000000u,
               "NTP answered with a server-mode packet carrying the host clock");
    }

    nsection("== ping ==");
    {
        Bytes icmp = ping_with_checksum(NSB("\x08\x00\x00\x00\x00\x01\x00\x01" "abc"));
        st.input(eth_frame(st.mac_, dev_mac, ETH_IP, ipv4_packet(yiaddr, st.ip, IP_ICMP, icmp)));
        ncheck(at(out.back(), 14 + 20) == 0 && F(out.back(), 14 + 20 + 8) == "abc", "echo request answered");
    }

    nsection("== forward: our side opens TCP into the device ==");
    /* Python hands _forward_open a FakeSock its host thread can never service;
     * the C++ equivalent is the invalid handle, which HostIO never selects on. */
    st.forward_open(BK_BAD_SOCK, 80);
    syn = F(out.back(), 14 + 20);
    ncheck(at(syn, 13) == TCP_SYN && rd16(syn, 2) == 80, "SYN sent to device port 80");
    TcpConn *fconn = first_kind(st, "forward");
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(st.device_ip_, st.ip, 80, fconn->lport, 5000, fconn->snd_nxt, TCP_SYN | TCP_ACK, 2048)));
    ncheck(fconn->state == "ESTABLISHED" && at(out.back(), 14 + 20 + 13) == TCP_ACK, "SYN-ACK -> ACK, established");
    { ScopedLock lk(st.io_->lock); fconn->from_host.push_back(Bytes("GET / HTTP/1.0\r\n\r\n")); }
    st.tick(11000000);
    {
        Bytes seg = F(out.back(), 14 + 20);
        ncheck(F(seg, 20) == "GET / HTTP/1.0\r\n\r\n", "host bytes pushed to the device");
    }
    st.input(eth_frame(st.mac_, dev_mac, ETH_IP, tcp_segment(st.device_ip_, st.ip, 80, fconn->lport, 5001, fconn->snd_nxt, TCP_ACK | TCP_PSH, 2048, Bytes("HTTP/1.0 200 OK\r\n"))));
    ncheck(!fconn->to_host.empty() && fconn->to_host[0] == "HTTP/1.0 200 OK\r\n", "device bytes queued for the host socket");
    ncheck(fconn->unacked.empty(), "our data acknowledged");

    nsection("== station side: DHCP fallback, ARP/ping probe ==");
    {
        StrList logs2; std::vector<Bytes> out2;
        NetStack cl(ns_cfg("020000000001", false), ns_log, &logs2);
        ns_wire(cl, logs2, out2);
        Bytes garp = arp_hdr(1) + dev_mac + str2ip("192.168.4.1") + zeros(6) + str2ip("192.168.4.1");
        cl.input(eth_frame(BCAST, dev_mac, ETH_ARP, garp));
        ncheck(cl.device_ip_ == str2ip("192.168.4.1") && cl.device_mac_ == dev_mac, "device address learned from its gratuitous ARP");
        cl.tick(1000000); cl.link_up(dev_mac); cl.tick(1000001);
        ncheck(!out2.empty() && S(F(out2.back(), 14 + 20 + 8 + 240), 0, 3) == NSB("\x35\x01\x01"), "DHCP DISCOVER sent once the link is up");
        cl.tick(1000000 + STATIC_FALLBACK_US + 1);
        ncheck(cl.ip == str2ip("192.168.4.3") && cl.dhcp_state_ == "STATIC", "static fallback next to the device's address");
        bool arp_seen = false, ping_seen = false;
        for (size_t i = out2.size() >= 2 ? out2.size() - 2 : 0; i < out2.size(); i++) {
            Bytes et = S(out2[i], 12, 14);
            if (et == NSB("\x08\x06")) arp_seen = true;
            if (et == NSB("\x08\x00") && at(out2[i], 14 + 20) == 8) ping_seen = true;
        }
        ncheck(arp_seen && ping_seen, "ARP request and ping sent to the device");
        Bytes echo = ping_with_checksum(NSB("\x00\x00\x00\x00\xbe\xef\x00\x01") + "BekenEmulator ping");
        cl.input(eth_frame(cl.mac_, dev_mac, ETH_IP, ipv4_packet(str2ip("192.168.4.1"), cl.ip, IP_ICMP, echo)));
        ncheck(cl.ping_replies_ == 1 && any_log(logs2, "ping reply from 192.168.4.1"), "ping reply recognised");
    }

    nsection("== --net-http-get: our own request to the device ==");
    {
        StrList logs3; std::vector<Bytes> out3;
        NetConfig c = ns_cfg("020000000001", false);
        c.http_gets.push_back("/cm?cmnd=echo%20hi");
        NetStack hc(c, ns_log, &logs3);
        ns_wire(hc, logs3, out3);
        hc.ip = str2ip("192.168.4.100"); hc.device_ip_ = str2ip("192.168.4.1"); hc.device_mac_ = dev_mac;
        hc.link_ = true; hc.probe_sent_ = true; hc.dhcp_state_ = "BOUND";
        hc.tick(1000000);
        Bytes s2 = F(out3.back(), 14 + 20);
        ncheck(at(s2, 13) == TCP_SYN && rd16(s2, 2) == 80, "queued GET opens a connection to the device's port 80");
        TcpConn *cc = first_kind(hc, "client");
        hc.input(eth_frame(hc.mac_, dev_mac, ETH_IP, tcp_segment(hc.device_ip_, hc.ip, 80, cc->lport, 7000, cc->snd_nxt, TCP_SYN | TCP_ACK, 4096)));
        hc.tick(1100000);
        ncheck(startswith(F(out3.back(), 14 + 40), "GET /cm?cmnd=echo%20hi HTTP/1.0\r\n"), "request sent once established");
        Bytes reply = "HTTP/1.1 200 OK\r\nContent-type: application/json\r\n\r\nhi";
        hc.input(eth_frame(hc.mac_, dev_mac, ETH_IP, tcp_segment(hc.device_ip_, hc.ip, 80, cc->lport, 7001, cc->snd_nxt,
                                                                TCP_ACK | TCP_PSH | TCP_FIN, 4096, reply)));
        hc.tick(1200000);
        ncheck(hc.http_replies_.size() == 1 && hc.http_replies_[0].path == "/cm?cmnd=echo%20hi"
               && hc.http_replies_[0].status == "HTTP/1.1 200 OK" && hc.http_replies_[0].body == "hi",
               "reply whose last segment also carries FIN is complete (FIN at seq + data length)");
        ncheck(any_log(logs3, "-> HTTP/1.1 200 OK, 2 byte body: hi"), "status and body logged");
    }

    nsection("== offline HTTP responder: the device's HTTP client gets an answer ==");
    StrList logs7; std::vector<Bytes> out7;
    NetStack hs(ns_cfg("020000000001", true), ns_log, &logs7);
    ns_wire(hs, logs7, out7);
    hs.device_ip_ = str2ip("192.168.1.100"); hs.device_mac_ = dev_mac;
    hs.arp_[hs.device_ip_] = dev_mac;
    Bytes sink2 = str2ip("10.200.0.7");
    u32 sq;
    TcpConn *hc2 = open_capture(hs, sink2, 80, 6100, sq);
    sq = dev_send(hs, hc2, sink2, 80, 6100, sq, Bytes("GET /hello?dev=obk HTTP/1.1\r\nHost: api.bekensim.test\r\n\r\n"));
    hs.tick(1000000);
    {
        Bytes rep = joined_payload(segs(out7), 80);
        ncheck(startswith(rep, "HTTP/1.1 200 OK\r\n") && contains(rep, "GET /hello?dev=obk"),
               "GET to a sinkhole address answered 200, the reply names the request");
    }
    ncheck(any_log(logs7, "HTTP GET /hello?dev=obk HTTP/1.1 answered 200 (offline)"), "answer logged");
    hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6100, 80, sq, hc2->snd_nxt, TCP_ACK, 8192)));
    hs.tick(1100000);
    ncheck(segs(out7).back().flags & TCP_FIN, "connection closed after the reply is acknowledged (Connection: close)");
    {
        TcpConn *pc_ = open_capture(hs, sink2, 80, 6101, sq, 7000);
        size_t n_before = segs(out7).size();
        sq = dev_send(hs, pc_, sink2, 80, 6101, sq, Bytes("POST /api HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345"));
        hs.tick(1200000);
        bool answered = false;
        std::vector<NsSeg> v = segs(out7);
        for (size_t i = n_before; i < v.size(); i++) if (startswith(v[i].p, "HTTP/1.1")) answered = true;
        ncheck(!answered, "POST with half its body: no answer yet");
        sq = dev_send(hs, pc_, sink2, 80, 6101, sq, Bytes("67890"));
        hs.tick(1300000);
        answered = false;
        v = segs(out7);
        for (size_t i = n_before; i < v.size(); i++) if (startswith(v[i].p, "HTTP/1.1 200 OK")) answered = true;
        ncheck(answered, "...answered once Content-Length bytes have arrived");
    }

    nsection("== offline MQTT broker: CONNACK, SUBACK, PUBACK, PINGRESP, PUBLISH logged ==");
    Bytes connect = NSB("\x00\x04MQTT\x04\x02\x00\x3c\x00\x06obkSim");
    TcpConn *mc = open_capture(hs, sink2, 1883, 6200, sq, 9000);
    sq = dev_send(hs, mc, sink2, 1883, 6200, sq, NSB("\x10") + Bytes(1, (char)connect.size()) + connect);
    hs.tick(2000000);
    ncheck(segs(out7, 6200).back().p == NSB("\x20\x02\x00\x00"), "CONNECT -> CONNACK (accepted)");
    {
        Bytes sub = NSB("\x00\x01") + NSB("\x00\x0cobkSim/+/set") + NSB("\x01");
        sq = dev_send(hs, mc, sink2, 1883, 6200, sq, NSB("\x82") + Bytes(1, (char)sub.size()) + sub);
        hs.tick(2100000);
        ncheck(segs(out7, 6200).back().p == NSB("\x90\x03\x00\x01\x01"), "SUBSCRIBE -> SUBACK with the packet id and granted QoS");
        Bytes pub0 = NSB("\x00\x10obkSim/connected") + "online";
        Bytes pub1 = NSB("\x00\x0cobkSim/1/get") + NSB("\x00\x05") + "1";
        sq = dev_send(hs, mc, sink2, 1883, 6200, sq, NSB("\x30") + Bytes(1, (char)pub0.size()) + pub0
                                                     + NSB("\x32") + Bytes(1, (char)pub1.size()) + pub1);
        hs.tick(2200000);
        bool a = false, b = false;
        for (size_t i = 0; i < hs.mqtt_publishes_.size(); i++) {
            if (hs.mqtt_publishes_[i].first == "obkSim/connected" && hs.mqtt_publishes_[i].second == "online") a = true;
            if (hs.mqtt_publishes_[i].first == "obkSim/1/get" && hs.mqtt_publishes_[i].second == "1") b = true;
        }
        ncheck(a && b, "two PUBLISHes in one segment both decoded (topic, payload)");
        ncheck(any_log(logs7, "MQTT PUBLISH obkSim/connected = online"), "publish logged as topic = payload");
        ncheck(segs(out7, 6200).back().p == NSB("\x40\x02\x00\x05"), "QoS 1 PUBLISH -> PUBACK with its packet id");
        sq = dev_send(hs, mc, sink2, 1883, 6200, sq, NSB("\xc0\x00"));
        hs.tick(2300000);
        ncheck(segs(out7, 6200).back().p == NSB("\xd0\x00"), "PINGREQ -> PINGRESP");
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6200, 1883, sq, mc->snd_nxt, TCP_ACK, 8192)));
        hs.tick(30000000);
        ncheck(!mc->fin_sent && mc->state == "ESTABLISHED", "an idle MQTT session is kept open (keep-alive, not end of flow)");
    }

    nsection("== offline MQTT broker: QoS 2, UNSUBSCRIBE, split packets, DISCONNECT ==");
    {
        TcpConn *m2 = open_capture(hs, sink2, 1883, 6400, sq, 13000);
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, NSB("\x10") + Bytes(1, (char)connect.size()) + connect);
        hs.tick(31000000);
        Bytes pub2 = NSB("\x00\x08obkSim/q") + NSB("\x00\x07") + "two";
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, NSB("\x34") + Bytes(1, (char)pub2.size()) + pub2);
        hs.tick(31100000);
        ncheck(segs(out7, 6400).back().p == NSB("\x50\x02\x00\x07"), "QoS 2 PUBLISH -> PUBREC with its packet id");
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, NSB("\x62\x02\x00\x07"));
        hs.tick(31200000);
        ncheck(segs(out7, 6400).back().p == NSB("\x70\x02\x00\x07"), "PUBREL -> PUBCOMP, the QoS 2 exchange completes");
        Bytes uns = NSB("\x00\x09") + NSB("\x00\x0cobkSim/+/set");
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, NSB("\xa2") + Bytes(1, (char)uns.size()) + uns);
        hs.tick(31300000);
        ncheck(segs(out7, 6400).back().p == NSB("\xb0\x02\x00\x09"), "UNSUBSCRIBE -> UNSUBACK with its packet id");
        Bytes big = NSB("\x00\x0cobkSim/state") + Bytes(300, 'x');
        Bytes pkt = NSB("\x30") + Bytes(1, (char)((big.size() & 0x7F) | 0x80)) + Bytes(1, (char)(big.size() >> 7)) + big;
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, S(pkt, 0, 2));           /* ends inside the length field */
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, S(pkt, 2, 100));
        hs.tick(31400000);
        bool seen = false;
        for (size_t i = 0; i < hs.mqtt_publishes_.size(); i++) if (hs.mqtt_publishes_[i].first == "obkSim/state") seen = true;
        ncheck(!seen, "a PUBLISH split across segments (even inside its length field) waits for the rest");
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, F(pkt, 100));
        hs.tick(31500000);
        seen = false;
        for (size_t i = 0; i < hs.mqtt_publishes_.size(); i++)
            if (hs.mqtt_publishes_[i].first == "obkSim/state" && hs.mqtt_publishes_[i].second == Bytes(300, 'x')) seen = true;
        ncheck(seen, "...and is decoded whole once it arrives (two-byte remaining length)");
        sq = dev_send(hs, m2, sink2, 1883, 6400, sq, NSB("\xe0\x00"));
        hs.tick(31600000);
        ncheck(segs(out7, 6400).back().flags & TCP_FIN, "DISCONNECT -> the broker closes the connection");
    }

    nsection("== offline MQTT broker: pushing a message TO the device ==");
    ncheck(mqtt_varint(5) == NSB("\x05") && mqtt_varint(300) == NSB("\xac\x02"),
           "remaining length encodes as one byte, and as two past 127");
    {
        Bytes pkt = mqtt_publish_packet(Bytes("cmnd/obkSim/POWER"), Bytes("ON"));
        ncheck(pkt == NSB("\x30\x15\x00\x11" "cmnd/obkSim/POWERON"), "a QoS 0 PUBLISH is built topic-length, topic, payload");
    }
    ncheck(mqtt_topic_matches("cmnd/obkSim/+", "cmnd/obkSim/POWER")
           && mqtt_topic_matches("obkSim/#", "obkSim/1/set")
           && mqtt_topic_matches("a/b", "a/b"),
           "topic filters match: + for one level, # for the rest, and plain equality");
    ncheck(!mqtt_topic_matches("cmnd/obkSim/+", "cmnd/obkSim/1/POWER")
           && !mqtt_topic_matches("cmnd/obkSim/+", "cmnd/other/POWER")
           && !mqtt_topic_matches("a/b/c", "a/b"),
           "+ does not span a level, a different level does not match, and a longer filter does not match");
    {
        StrList logsP; std::vector<Bytes> outP;
        NetConfig c = ns_cfg("020000000001", true);
        c.mqtt_pubs.push_back(std::make_pair(std::string("cmnd/obkSim/POWER"), Bytes("ON")));
        c.mqtt_pubs.push_back(std::make_pair(std::string("nobody/listens"), Bytes("x")));
        NetStack ps(c, ns_log, &logsP);
        ns_wire(ps, logsP, outP);
        ps.device_ip_ = str2ip("192.168.1.100"); ps.device_mac_ = dev_mac;
        ps.arp_[ps.device_ip_] = dev_mac;
        Bytes sinkP = str2ip("10.200.0.3");
        TcpConn *pc = open_capture(ps, sinkP, 1883, 6500, sq, 21000);
        sq = dev_send(ps, pc, sinkP, 1883, 6500, sq, NSB("\x10") + Bytes(1, (char)connect.size()) + connect);
        ps.tick(1000000);
        bool pushed_early = false;
        std::vector<NsSeg> v = segs(outP, 6500);
        for (size_t i = 0; i < v.size(); i++) if (contains(v[i].p, "cmnd/obkSim/POWER")) pushed_early = true;
        ncheck(!pushed_early, "nothing is pushed to a device that has only connected");
        Bytes sub2 = NSB("\x00\x01") + NSB("\x00\x0d" "cmnd/obkSim/+") + NSB("\x00");
        sq = dev_send(ps, pc, sinkP, 1883, 6500, sq, NSB("\x82") + Bytes(1, (char)sub2.size()) + sub2);
        ps.tick(1100000);
        Bytes pushed = joined_payload(segs(outP, 6500), 1883);
        ncheck(contains(pushed, mqtt_publish_packet(Bytes("cmnd/obkSim/POWER"), Bytes("ON"))),
               "...and it is pushed once the device subscribes to a filter that matches it");
        ncheck(any_log(logsP, "[NET] MQTT -> device PUBLISH cmnd/obkSim/POWER = ON"), "the push is logged");
        ncheck(ps.mqtt_pubs_.size() == 1 && ps.mqtt_pubs_[0].first == "nobody/listens" && ps.mqtt_pubs_[0].second == "x",
               "a message nobody subscribed to is still waiting, not sent to the wrong topic");
        size_t n_before = segs(outP, 6500).size();
        sq = dev_send(ps, pc, sinkP, 1883, 6500, sq, NSB("\x82") + Bytes(1, (char)sub2.size()) + sub2);
        ps.tick(1200000);
        bool again = false;
        v = segs(outP, 6500);
        for (size_t i = n_before; i < v.size(); i++) if (contains(v[i].p, "cmnd/obkSim/POWER")) again = true;
        ncheck(!again, "subscribing again does not send the same message twice");
    }

    nsection("== offline UDP: broadcasts logged, other datagrams dropped ==");
    {
        size_t n_out = out7.size();
        hs.input(eth_frame(BCAST, dev_mac, ETH_IP, udp_packet(hs.device_ip_, FF4, 6666, 6667,
                                                             NSB("\x00\x00\x55\xaa{\"gwId\":\"sim\"}"))));
        ncheck(out7.size() == n_out && any_log(logs7, "UDP broadcast 192.168.1.100:6666 -> 255.255.255.255:6667", "gwId"),
               "a LAN broadcast (Tuya's discovery port 6667) is logged with its text, nothing answered");
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, udp_packet(hs.device_ip_, sink2, 7000, 9999, Bytes("data"))));
        ncheck(out7.size() == n_out && any_log(logs7, "UDP 192.168.1.100:7000 -> 10.200.0.7:9999 4 bytes (offline, dropped)"),
               "a datagram to any other port is logged and dropped while offline");
    }

    nsection("== frame trace: every byte that crossed the bridge ==");
    std::vector<NsSeen> seen;
    StrList logsF; std::vector<Bytes> outF;
    NetStack fs(ns_cfg("020000000001", true), ns_log, &logsF);
    ns_wire(fs, logsF, outF);
    fs.on_frame = ns_on_frame; fs.on_frame_ctx = &seen;
    fs.device_ip_ = str2ip("192.168.1.100"); fs.device_mac_ = dev_mac;
    fs.arp_[fs.device_ip_] = dev_mac;
    fs.now_us_ = 1234;
    Bytes ping = ping_with_checksum(NSB("\x08\x00\x00\x00\x00\x01\x00\x07" "hello"));
    Bytes inframe = eth_frame(fs.mac_, dev_mac, ETH_IP, ipv4_packet(fs.device_ip_, fs.ip, IP_ICMP, ping));
    fs.input(inframe);
    ncheck(seen.size() == 2, "one frame in, one frame out, both on the record");
    ncheck(seen[0].dir == "from device" && seen[0].frame == inframe && seen[0].us == 1234,
           "the device's frame is recorded whole, byte for byte, with the time it arrived");
    ncheck(seen[1].dir == "to device" && seen[1].frame == outF.back(), "and so is the reply, exactly as it went out");
    {
        Bytes odd = eth_frame(fs.mac_, dev_mac, 0x88CC, NSB("\x01\x02\x03"));
        fs.input(odd);
        ncheck(seen.back().dir == "from device" && seen.back().frame == odd && seen.back().us == 1234,
               "a frame the stack ignores is recorded too");
        size_t n_before = seen.size();
        fs.input(eth_frame(fs.mac_, fs.mac_, ETH_IP, zeros(20)));
        ncheck(seen.size() == n_before, "our own frame echoed back by the device's AP is not double-counted");
        fs.on_frame = ns_on_frame_throws;
        fs.input(inframe);
        ncheck(true, "a front end that raises cannot break the run");
    }

    nsection("== frame summaries for a packet list ==");
    {
        Bytes ip = fs.device_ip_;
        ncheck(describe_frame(inframe) == "ICMP echo request 192.168.1.100 to 192.168.1.1", "an echo request is named with both addresses");
        Bytes es, ed;
        ncheck(ipv4_endpoints(inframe, es, ed) && es == str2ip("192.168.1.100") && ed == str2ip("192.168.1.1"),
               "raw IPv4 endpoints are extracted exactly for packet-list filtering");
        Bytes malformed_ip = ipv4_packet(ip, fs.ip, IP_ICMP, ping);
        malformed_ip.replace(2, 2, be16((u16)(malformed_ip.size() + 1)));
        ncheck(!ipv4_endpoints(eth_frame(fs.mac_, dev_mac, ETH_ARP, malformed_ip), es, ed)
               && !ipv4_endpoints(eth_frame(fs.mac_, dev_mac, ETH_IP, malformed_ip), es, ed)
               && !ipv4_endpoints(Bytes("short"), es, ed),
               "endpoint extraction rejects non-IPv4, truncated and inconsistent frames safely");
        Bytes syn3 = tcp_segment(ip, str2ip("10.200.0.1"), 40127, 1883, 1, 0, TCP_SYN, 8192);
        ncheck(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, syn3)) == "TCP 192.168.1.100:40127 to 10.200.0.1:1883 [SYN]",
               "a SYN names its ports and flag");
        Bytes data = tcp_segment(ip, str2ip("10.200.0.1"), 40127, 1883, 2, 1, TCP_ACK | TCP_PSH, 8192, Bytes("hello there"));
        ncheck(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, data)) == "TCP 192.168.1.100:40127 to 10.200.0.1:1883 [ACK,PSH], 11 bytes",
               "a data segment says how many bytes it carried");
        Bytes dns = udp_packet(ip, str2ip("192.168.1.1"), 5353, 53, Bytes(20, 'q'));
        ncheck(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, dns)) == "UDP 192.168.1.100:5353 to 192.168.1.1:53 (DNS), 20 bytes",
               "a datagram is named by the service on its port");
        Bytes tuya = udp_packet(ip, FF4, 63035, 6667, Bytes(40, 'x'));
        ncheck(contains(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, tuya)), "(Tuya LAN)"), "...including Tuya's own discovery port");
        Bytes reply = udp_packet(str2ip("192.168.1.1"), ip, 53, 5353, Bytes(22, 'a'));
        ncheck(contains(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, reply)), "(DNS)"),
               "a reply is named for the service it came FROM, not whatever its destination port means");
        Bytes arp = arp_hdr(1) + dev_mac + ip + zeros(6) + str2ip("192.168.1.1");
        ncheck(describe_frame(eth_frame(BCAST, dev_mac, ETH_ARP, arp)) == "ARP who has 192.168.1.1, tell 192.168.1.100",
               "an ARP request reads as a question");
        ncheck(describe_frame(zeros(8)) == "runt frame, 8 bytes"
               && contains(describe_frame(eth_frame(BCAST, dev_mac, ETH_IP, NSB("\x45"))), "truncated"),
               "a short or malformed frame is described as such, not hidden or raised over");
    }

    nsection("== writing the captured frames as an Ethernet pcap ==");
    {
        const char *tmpdir = getenv("TEMP");
        if (!tmpdir) tmpdir = getenv("TMP");
        if (!tmpdir) tmpdir = getenv("TMPDIR");
        std::string cap_path = path_join(tmpdir ? tmpdir : ".",
                                         strf("bekensim_netstack_selftest_%u_%u.pcap", (unsigned)time(0), (unsigned)(size_t)&seen));
        Bytes blob;
        {
            EthPcap cap(cap_path);
            cap.write(inframe, 1500000);
            cap.write(outF[0], 2250500);
            cap.close();
            read_file(cap_path, blob);
            remove_file(cap_path);
        }
        ncheck(blob.size() >= 24 && rd32le(blob.data()) == 0xA1B2C3D4 && rd16le(blob.data() + 4) == 2
               && rd16le(blob.data() + 6) == 4 && rd32le(blob.data() + 20) == 1,
               "the header says pcap 2.4, link type 1, which is what Wireshark reads as Ethernet");
        size_t off = 24;
        typedef NsPcapRec Rec;
        std::vector<Rec> recs;
        while (off + 16 <= blob.size()) {
            Rec r;
            u32 sec = rd32le(blob.data() + off), usec = rd32le(blob.data() + off + 4);
            r.incl = rd32le(blob.data() + off + 8); r.orig = rd32le(blob.data() + off + 12);
            r.t = (u64)sec * 1000000 + usec;
            r.data = S(blob, (long long)off + 16, (long long)(off + 16 + r.incl));
            recs.push_back(r);
            off += 16 + r.incl;
        }
        ncheck(off == blob.size() && recs.size() == 2 && recs[0].data == inframe && recs[1].data == outF[0],
               "both frames are written whole, in order, with nothing added between them");
        ncheck(recs[0].t == 1500000 && recs[1].t == 2250500, "each keeps the device time it crossed, seconds and microseconds apart");
        bool full = true;
        for (size_t i = 0; i < recs.size(); i++) if (!(recs[i].incl == recs[i].orig && recs[i].orig == recs[i].data.size())) full = false;
        ncheck(full, "and is recorded at its full length");
    }

    nsection("== hex dump ==");
    {
        std::string dump = hexdump(byte_range(0, 8) + NSB("Hi!\x7f"));
        ncheck(startswith(dump, "00000000  00 01 02 03 04 05 06 07 48 69 21 7f"), "offset then the bytes");
        ncheck(endswith(dump, "  ........Hi!."), "then the printable ones, with the rest as dots");
        ncheck(hexdump(Bytes()) == "", "an empty buffer dumps to nothing");
        std::string two = hexdump(zeros(20));
        StrList lines = split(two, "\n");
        ncheck(count_sub(two, "\n") == 1 && lines.size() == 2 && startswith(lines[1], "00000010  00 00 00 00  "),
               "a second row starts at the right offset and is padded so the text column lines up");
    }

    nsection("== TLS 1.2 PSK/GCM/EMS crypto engine ==");
    /* Published TLS 1.2 SHA-256 PRF vector (100-byte vector, first 32 bytes). */
    ncheck(hexstr(tls12_prf(H("9bbe436ba940f017b17652849a71db35"), Bytes("test label"), H("a0ba9f936cda311827a6f796ffd5198c"), 32))
           == "e3f229ba727be17b8d122620557cd453c2aab21d07c3d495329b52d4e61edb5a",
           "TLS 1.2 P_SHA256 matches its published deterministic vector");
    ncheck(tls_psk_premaster(Bytes("abc")) == H("00030000000003") + "abc",
           "plain PSK premaster encodes an equal-length all-zero other_secret");
    /* A fixed synthetic transcript pins EMS, Finished, GCM nonce/AAD/tag and
     * both traffic directions without carrying any real device credential. */
    const Bytes fx_ch = H("0100002f0303000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                          "00000200a80100000400170000");
    const Bytes fx_server = H("0200002c0303202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                              "0000a8000004001700000e000000");
    const Bytes fx_cke = H("10000010000e666978747572652d636c69656e74");
    const Bytes fx_client_finished = H("16030300280000000000000000dd86ff0778b91eebcf1068a28cbe65d34ce39485bdb25ac6c5f9"
                                       "41df023e7520");
    const Bytes fx_server_flight = H("14030300010116030300280000000000000000c2ab5020ab616191d1e89dbb55870f79df90619e99"
                                     "d92d9dd97cc43a81ac7557");
    const Bytes fx_app_record = H("170303002c0000000000000001dc9548dca48a3d054700292fe30be85d011960ff23ca3d85f04835"
                                  "6eaa524b568806b8f4");
    const Bytes fx_app = H("101200044d5154540402003c0006636c69656e74");
    {
        std::vector<Bytes> cands;
        cands.push_back(Bytes("incorrect synthetic candidate"));
        cands.push_back(Bytes("synthetic tls psk fixture"));
        TLS12PSKGCMServer fx(cands, byte_range(0, 32), byte_range(32, 64), fx_ch, fx_server, TLS_MAX_PLAINTEXT, false);
        ncheck(fx.ok && hexstr(sha256(fx.transcript + fx_cke)) == "9b53d23c0caa1f9f1ccf93274bfe8ec7f22a6193c5ad83e81ef93ad0fe147836",
               "ClientHello, ServerHello/Done and ClientKeyExchange form the EMS transcript");
        Bytes ident;
        ncheck(fx.add_client_key_exchange(fx_cke, ident) && ident == "fixture-client",
               "ClientKeyExchange contributes its identity and complete handshake framing");
        fx.change_cipher_spec(NSB("\x01"));
        Bytes flight;
        ncheck(fx.finish(TLS12, F(fx_client_finished, 5), flight) && flight == fx_server_flight,
               "authenticated client Finished selects the right candidate and yields CCS + server Finished");
        ncheck(fx.established() && fx.client_seq == 1 && fx.server_seq == 1,
               "record epochs start at sequence zero for Finished, then advance independently");
        Bytes plain;
        ncheck(fx.decrypt_record(0x17, TLS12, F(fx_app_record, 5), plain) && plain == fx_app,
               "AES-GCM application record decrypts with explicit nonce, sequence AAD and tag");
        Bytes rec;
        ncheck(fx.encrypt_record(0x17, NSB("\x20\x02\x00\x00"), rec)
               && hexstr(rec) == "170303001c0000000000000001cafd534ff055bbc5fd23d7c9e74fbf9103d90558",
               "server application record uses its own sequence, nonce, AAD and authentication tag");
        bool replay_rejected = !fx.decrypt_record(0x17, TLS12, F(fx_app_record, 5), plain);
        ncheck(replay_rejected, "replaying a valid GCM record at the next sequence number is rejected");
    }
    {
        /* TuyaOS 3.3's legacy bootstrap mode passes one embedded 32-byte token
         * as both the public identity and the PSK; a synthetic token pins it. */
        Bytes legacy_identity = "legacyIdentityActsAsPskValue0001";
        Bytes legacy_body = be16((u16)legacy_identity.size()) + legacy_identity;
        Bytes legacy_cke = NSB("\x10") + be24((u32)legacy_body.size()) + legacy_body;
        std::vector<Bytes> wrong; wrong.push_back(Bytes("wrong stored legacy candidate"));
        TLS12PSKGCMServer legacy(wrong, byte_range(0, 32), byte_range(32, 64), fx_ch, fx_server, TLS_MAX_PLAINTEXT, true);
        Bytes ident;
        ncheck(legacy.add_client_key_exchange(legacy_cke, ident) && ident == legacy_identity
               && !legacy.psks.empty() && legacy.psks.back() == legacy_identity,
               "enabled Tuya legacy mode trials an exact 32-byte alphanumeric identity as PSK");
        legacy.change_cipher_spec(NSB("\x01"));
        Bytes legacy_hash = sha256(legacy.transcript);
        Bytes legacy_master = tls12_prf(tls_psk_premaster(legacy_identity), Bytes("extended master secret"), legacy_hash, 48);
        Bytes legacy_block = tls12_prf(legacy_master, Bytes("key expansion"), byte_range(32, 64) + byte_range(0, 32), 40);
        Bytes legacy_finished_plain = NSB("\x14\x00\x00\x0c") + tls12_prf(legacy_master, Bytes("client finished"), legacy_hash, 12);
        Bytes legacy_finished = TLS12PSKGCMServer::encrypt(S(legacy_block, 0, 16), S(legacy_block, 32, 36), 0, 0x16, TLS12,
                                                           legacy_finished_plain);
        Bytes flight;
        legacy.finish(TLS12, F(legacy_finished, 5), flight);
        ncheck(legacy.established() && legacy.psks.empty(),
               "the identity key is admitted only after its encrypted Finished authenticates");

        TLS12PSKGCMServer legacy_off(wrong, byte_range(0, 32), byte_range(32, 64), fx_ch, fx_server, TLS_MAX_PLAINTEXT, false);
        legacy_off.add_client_key_exchange(legacy_cke, ident);
        legacy_off.change_cipher_spec(NSB("\x01"));
        bool legacy_disabled_rejected = !legacy_off.finish(TLS12, F(legacy_finished, 5), flight);
        Bytes malformed_identity = "not-a-32-byte-alphanumeric-token";
        Bytes malformed_body = be16((u16)malformed_identity.size()) + malformed_identity;
        Bytes malformed_cke = NSB("\x10") + be24((u32)malformed_body.size()) + malformed_body;
        TLS12PSKGCMServer legacy_shaped(wrong, byte_range(0, 32), byte_range(32, 64), fx_ch, fx_server, TLS_MAX_PLAINTEXT, true);
        legacy_shaped.add_client_key_exchange(malformed_cke, ident);
        ncheck(legacy_disabled_rejected && legacy_shaped.psks == wrong,
               "disabled mode and a non-matching identity shape never become PSK candidates");
    }

    nsection("== TLS-PSK: reading who is calling the cloud ==");
    /* The exact ClientHello a BK7231N 3-phase meter dump sent to its Tuya cloud:
     * TLS 1.2, four suites, three of them pre-shared-key, no server name. */
    const Bytes real_hello = H("16030300460100004203032aec34e9f5ba50499bcdd89e1070246f6db6ee0ef786727acdbfa08a4a07fd0a"
                               "000008008d00ae008c00ff010000110001000102001600000017000000230000");
    {
        ClientHello info = tls_client_hello(real_hello);
        ncheck(info.ok && info.version == "1.2", "the hello is read, and names TLS 1.2");
        ncheck(info.suites.size() == 4 && info.suites[0] == 0x008D && info.suites[1] == 0x00AE
               && info.suites[2] == 0x008C && info.suites[3] == 0x00FF,
               "all four offered cipher suites are listed, in the order offered");
        ncheck(info.psk.size() == 3 && info.psk[0] == 0x008D && info.psk[1] == 0x00AE && info.psk[2] == 0x008C
               && tls_suite_name(info.psk[0]) == "TLS_PSK_WITH_AES_256_CBC_SHA",
               "the pre-shared-key suites are picked out by name");
        ncheck(info.sni.empty(), "and it carries no server name, as a PSK client need not");
        ncheck(!tls_client_hello(Bytes("GET / HTTP/1.1\r\n\r\n")).ok && !tls_client_hello(S(real_hello, 0, 20)).ok,
               "something that is not a complete hello is not read as one");
    }
    const Bytes hello_base = S(fx_ch, 4, -6);   /* body before extensions_length + the one EMS extension */
    {
        Bytes duplicate_ems = hello_with_extensions(hello_base, NSB("\x00\x17\x00\x00\x00\x17\x00\x00"));
        Bytes trailing_ext_byte = hello_with_extensions(hello_base, NSB("\x00\x17\x00\x00\xff"));
        ncheck(!tls_client_hello(duplicate_ems).ok && !tls_client_hello(trailing_ext_byte).ok,
               "duplicate extensions and trailing bytes in the extension block reject a ClientHello");
    }
    Bytes cloud = str2ip("10.200.0.5");
    {
        StrList logsT2; std::vector<Bytes> outT2;
        NetStack ts3(ns_cfg("020000000001", true), ns_log, &logsT2);
        ns_wire(ts3, logsT2, outT2);
        ts3.device_ip_ = str2ip("192.168.1.100"); ts3.device_mac_ = dev_mac;
        ts3.arp_[ts3.device_ip_] = dev_mac;
        TcpConn *tc = open_capture(ts3, cloud, 8886, 6700, sq, 31000);
        sq = dev_send(ts3, tc, cloud, 8886, 6700, sq, real_hello);
        ts3.tick(1000000);
        ncheck(any_log(logsT2, "client hello: version 1.2, 4 cipher suites", "TLS_PSK_WITH_AES_256_CBC_SHA"),
               "a device's hello to the cloud is reported with what it offered");
        Bytes reply = joined_payload(segs(outT2, 6700), 8886);
        ncheck(S(reply, 0, 3) == NSB("\x16\x03\x03") && at(reply, 5) == 0x02, "the stack answers with a ServerHello of its own");
        u32 chosen = rd16(reply, 5 + 4 + 2 + 32 + 1);
        ncheck(chosen == 0x008D && F(reply, -4) == NSB("\x0e\x00\x00\x00"),
               "...picking one of the PSK suites the device offered, then ServerHelloDone");
        ncheck(any_log(logsT2, "answered with TLS_PSK_WITH_AES_256_CBC_SHA"), "which is logged");
        /* Now the device names the key it means to use. That identity is in clear. */
        Bytes ident = "bf0e0f689d000d56656gmp";
        Bytes cke_body = be16((u16)ident.size()) + ident;
        Bytes cke = NSB("\x10") + be24((u32)cke_body.size()) + cke_body;
        sq = dev_send(ts3, tc, cloud, 8886, 6700, sq, NSB("\x16\x03\x03") + be16((u16)cke.size()) + cke);
        ts3.tick(1100000);
        ncheck(any_log(logsT2, strf("pre-shared key identity: %d bytes", (int)ident.size())) && !any_log(logsT2, ident),
               "the identity is retained without exposing its bytes in the log");
        ncheck(!ts3.tls_identities_.empty() && ts3.tls_identities_.back().second == ident,
               "and kept, so a front end can show which device called");
        size_t n_logs = logsT2.size();
        sq = dev_send(ts3, tc, cloud, 8886, 6700, sq, NSB("\x17\x03\x03\x00\x20") + zeros(32));
        ts3.tick(1200000);
        ncheck(any_log_from(logsT2, n_logs, "encrypted from here on"),
               "once it switches to encryption, the stack says so rather than pretending to read it");

        /* After CCS the device's Finished is itself a type 0x16 record; if its
         * ciphertext happened to look like a ClientKeyExchange it must still
         * NOT be read as a second identity. */
        u32 sqc;
        TcpConn *ce = open_capture(ts3, cloud, 8886, 6900, sqc, 41000);
        sqc = dev_send(ts3, ce, cloud, 8886, 6900, sqc, real_hello);
        ts3.tick(2000000);
        size_t before = ts3.tls_identities_.size();
        Bytes cke_rec = NSB("\x16\x03\x03") + be16((u16)cke.size()) + cke;
        Bytes ccs_rec = NSB("\x14\x03\x03\x00\x01\x01");
        dev_send(ts3, ce, cloud, 8886, 6900, sqc, cke_rec + ccs_rec + cke_rec);
        ts3.tick(2100000);
        ncheck(ts3.tls_identities_.size() == before + 1,
               "the identity is read once from the real key exchange; a matching record after "
               "the ChangeCipherSpec is not read as a second one");
        ncheck(any_log(logsT2, "switched to encryption (ChangeCipherSpec)"),
               "the switch to encryption is reported instead of parsing past it");
    }
    {
        /* A client offering only certificate suites gets no ServerHello. */
        StrList logsT3; std::vector<Bytes> outT3;
        NetStack ts4(ns_cfg("020000000001", true), ns_log, &logsT3);
        ns_wire(ts4, logsT3, outT3);
        ts4.device_ip_ = str2ip("192.168.1.100"); ts4.device_mac_ = dev_mac;
        ts4.arp_[ts4.device_ip_] = dev_mac;
        Bytes rsa_hello = NSB("\x16\x03\x03") + be16(4 + 2 + 32 + 1 + 2 + 4 + 2)
                          + NSB("\x01") + be24(2 + 32 + 1 + 2 + 4 + 2)
                          + NSB("\x03\x03") + zeros(32) + NSB("\x00") + be16(4)
                          + NSB("\x00\x2f\x00\x35") + NSB("\x01\x00") + NSB("\x00\x00");
        u32 sq2;
        TcpConn *rc2 = open_capture(ts4, cloud, 443, 6701, sq2, 33000);
        sq2 = dev_send(ts4, rc2, cloud, 443, 6701, sq2, rsa_hello);
        ts4.tick(1000000);
        ncheck(any_log(logsT3, "offers no pre-shared-key suite"), "a certificate-based hello is reported and left alone");
        ncheck(joined_payload(segs(outT3, 6701), 443).empty(),
               "...with no ServerHello sent back, because there is nothing honest to send");
    }

    nsection("== TLS keyed-mode admission and record limits ==");
    {
        StrList logsG; std::vector<Bytes> outG;
        NetConfig c = ns_cfg("020000000001", true);
        c.tls_psks.push_back(Bytes("synthetic gate key"));
        NetStack gated(c, ns_log, &logsG);
        ns_wire(gated, logsG, outG);
        gated.device_ip_ = str2ip("192.168.1.100"); gated.device_mac_ = dev_mac;
        gated.arp_[gated.device_ip_] = dev_mac;
        /* EMS is an empty extension by definition; a non-empty one must never
         * activate credential-backed completion. */
        Bytes nonempty_ems = hello_with_extensions(hello_base, NSB("\x00\x17\x00\x01\x00"));
        u32 gs1, gs2, gs3;
        TcpConn *gc1 = open_capture(gated, cloud, 8883, 6702, gs1, 33500);
        dev_send(gated, gc1, cloud, 8883, 6702, gs1, nonempty_ems);
        gated.tick(1000000);
        /* Likewise suite 0x00A8 does not turn a TLS 1.1 ClientHello into 1.2. */
        Bytes tls11_hello = hello_with_extensions(hello_base, NSB("\x00\x17\x00\x00"));
        tls11_hello.replace(9, 2, "\x03\x02", 2);
        TcpConn *gc2 = open_capture(gated, cloud, 8883, 6703, gs2, 34000);
        dev_send(gated, gc2, cloud, 8883, 6703, gs2, tls11_hello);
        gated.tick(1100000);
        ncheck(gc1->tls == 0 && gc2->tls == 0, "keyed completion requires TLS 1.2 and an exactly empty EMS extension");
        TcpConn *gc3 = open_capture(gated, cloud, 8883, 6704, gs3, 34500);
        Bytes oversize_header = NSB("\x16\x03\x03") + be16((u16)(TLS_MAX_PLAINTEXT + 1));
        dev_send(gated, gc3, cloud, 8883, 6704, gs3, oversize_header);
        ncheck(gc3->proto == "raw" && any_log(logsG, "record exceeds the TLS length limit"),
               "an oversized clear TLS record is rejected from its header without buffering its body");
    }

    nsection("== keyed TLS-PSK record flow reaches the offline MQTT broker ==");
    {
        Bytes tls_secret = "synthetic integration psk, never a device key";
        Bytes large_push = Bytes(2300, 'x');
        StrList logsK; std::vector<Bytes> outK;
        NetConfig c = ns_cfg("020000000001", true);
        c.tls_psks.push_back(Bytes("wrong synthetic candidate"));
        c.tls_psks.push_back(tls_secret);
        c.mqtt_pubs.push_back(std::make_pair(std::string("cmnd/large"), large_push));
        NetStack keyed(c, ns_log, &logsK);
        ns_wire(keyed, logsK, outK);
        keyed.device_ip_ = str2ip("192.168.1.100"); keyed.device_mac_ = dev_mac;
        keyed.arp_[keyed.device_ip_] = dev_mac;
        u32 kseq;
        TcpConn *kc = open_capture(keyed, cloud, 8883, 6710, kseq, 35000);

        /* Split the ClientHello after its TLS header: recognition and record
         * buffering must not depend on how TCP happened to segment it. */
        Bytes keyed_hello_record = hello_with_extensions(hello_base,
            NSB("\x00\x01\x00\x01\x02")      /* max_fragment_length = 1024 */
            + NSB("\x00\x17\x00\x00"));      /* extended_master_secret */
        Bytes keyed_ch = F(keyed_hello_record, 5);
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, S(keyed_hello_record, 0, 7));
        ncheck(kc->proto == "tls" && !kc->answered,
               "a split TLS record is recognised from its header and waits for the complete ClientHello");
        size_t hello_out_from = outK.size();
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, F(keyed_hello_record, 7));
        keyed.tick(1000000);
        Bytes hello_reply = joined_payload(segs(outK, -1, hello_out_from), 8883, true);
        ncheck(startswith(hello_reply, NSB("\x16\x03\x03")) && at(hello_reply, 5) == 0x02
               && contains(hello_reply, NSB("\x00\x01\x00\x01\x02"))
               && F(hello_reply, -4) == NSB("\x0e\x00\x00\x00"),
               "keyed ServerHello selects AES-128-GCM, EMS and the offered 1024-byte fragment limit");

        Bytes server_random = S(hello_reply, 11, 43);
        Bytes transcript = keyed_ch + F(hello_reply, 5) + fx_cke;
        Bytes master = tls12_prf(tls_psk_premaster(tls_secret), Bytes("extended master secret"), sha256(transcript), 48);
        Bytes key_block = tls12_prf(master, Bytes("key expansion"), server_random + byte_range(0, 32), 40);
        Bytes client_key = S(key_block, 0, 16), server_key = S(key_block, 16, 32);
        Bytes client_iv = S(key_block, 32, 36), server_iv = S(key_block, 36, 40);
        Bytes client_finished_plain = NSB("\x14\x00\x00\x0c") + tls12_prf(master, Bytes("client finished"), sha256(transcript), 12);
        Bytes client_finished_record = TLS12PSKGCMServer::encrypt(client_key, client_iv, 0, 0x16, TLS12, client_finished_plain);
        Bytes cke_record = NSB("\x16\x03\x03") + be16((u16)fx_cke.size()) + fx_cke;
        Bytes client_handshake_flight = cke_record + NSB("\x14\x03\x03\x00\x01\x01") + client_finished_record;
        size_t finished_out_from = outK.size();
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, client_handshake_flight);
        keyed.tick(1100000);
        Bytes server_finished_flight = joined_payload(segs(outK, -1, finished_out_from), 8883, true);
        ncheck(startswith(server_finished_flight, NSB("\x14\x03\x03\x00\x01\x01\x16\x03\x03")),
               "verified client Finished produces server ChangeCipherSpec and encrypted Finished");
        ncheck(kc->tls != 0 && kc->tls->max_plaintext == 1024 && kc->tls->psks.empty(),
               "the session installs the negotiated fragment limit and discards its PSK candidates");
        Bytes server_finished_record = F(server_finished_flight, 6);
        Bytes server_finished_plain;
        bool dec_ok = TLS12PSKGCMServer::decrypt(server_key, server_iv, 0, 0x16, TLS12, F(server_finished_record, 5), server_finished_plain);
        Bytes expected_server_finished = NSB("\x14\x00\x00\x0c")
            + tls12_prf(master, Bytes("server finished"), sha256(transcript + client_finished_plain), 12);
        ncheck(dec_ok && ct_equal(server_finished_plain, expected_server_finished),
               "server Finished authenticates the transcript including the client's Finished");

        Bytes kconnect = fx_app;
        Bytes sub_body = NSB("\x12\x34") + be16(6) + NSB("cmnd/#\x00");
        Bytes subscribe = NSB("\x82") + mqtt_varint((int)sub_body.size()) + sub_body;
        Bytes pub_body = be16(9) + "dev/state" + "online";
        Bytes publish = NSB("\x30") + mqtt_varint((int)pub_body.size()) + pub_body;
        Bytes mqtt_flight = kconnect + subscribe + publish;
        Bytes client_app_record = TLS12PSKGCMServer::encrypt(client_key, client_iv, 1, 0x17, TLS12, mqtt_flight);
        size_t app_out_from = outK.size();
        /* Split the protected record too: neither GCM nor MQTT sees a partial unit. */
        size_t cut = client_app_record.size() / 2;
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, S(client_app_record, 0, (long long)cut));
        ncheck(keyed.mqtt_publishes_.empty(), "a partial encrypted application record is not decrypted early");
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, F(client_app_record, (long long)cut));
        keyed.tick(1200000);
        Bytes mqtt_reply_stream = joined_payload(segs(outK, -1, app_out_from), 8883, true);
        std::vector<Bytes> mqtt_reply_records = tls_records(mqtt_reply_stream);
        std::vector<Bytes> mqtt_replies;
        bool all_dec = true;
        for (size_t i = 0; i < mqtt_reply_records.size(); i++) {
            const Bytes &r = mqtt_reply_records[i];
            Bytes p;
            if (!TLS12PSKGCMServer::decrypt(server_key, server_iv, (u64)(i + 1), (u8)r[0], S(r, 1, 3), F(r, 5), p)) all_dec = false;
            mqtt_replies.push_back(p);
        }
        ncheck(all_dec && mqtt_replies.size() >= 2 && mqtt_replies[0] == NSB("\x20\x02\x00\x00")
               && mqtt_replies[1] == NSB("\x90\x03\x12\x34\x00"),
               "decrypted CONNECT and SUBSCRIBE reach the existing broker and return encrypted CONNACK/SUBACK");
        Bytes expected_push = mqtt_publish_packet(Bytes("cmnd/large"), large_push);
        Bytes push_joined; bool small = true;
        for (size_t i = 2; i < mqtt_replies.size(); i++) { push_joined += mqtt_replies[i]; if (mqtt_replies[i].size() > 1024) small = false; }
        ncheck(mqtt_replies.size() == 5 && small && push_joined == expected_push,
               "a large broker PUBLISH is split into sequenced TLS records at max_fragment_length");
        ncheck(keyed.mqtt_publishes_.size() == 1 && keyed.mqtt_publishes_[0].first == "dev/state"
               && keyed.mqtt_publishes_[0].second == "online" && any_log(logsK, "MQTT PUBLISH dev/state = online"),
               "a decrypted application PUBLISH reaches the existing MQTT capture path");

        Bytes ping_record = TLS12PSKGCMServer::encrypt(client_key, client_iv, 2, 0x17, TLS12, NSB("\xc0\x00"));
        size_t ping_out_from = outK.size();
        kseq = dev_send(keyed, kc, cloud, 8883, 6710, kseq, ping_record);
        keyed.tick(1300000);
        std::vector<Bytes> ping_records = tls_records(joined_payload(segs(outK, -1, ping_out_from), 8883, true));
        u64 next_server_seq = 1 + mqtt_reply_records.size();
        Bytes pong;
        ncheck(ping_records.size() == 1
               && TLS12PSKGCMServer::decrypt(server_key, server_iv, next_server_seq, 0x17, TLS12, F(ping_records[0], 5), pong)
               && pong == NSB("\xd0\x00"),
               "later encrypted PINGREQ/PINGRESP records advance each direction's sequence correctly");
        keyed.tick(5000000);
        ncheck(kc->state == "ESTABLISHED" && !kc->close_after_flush,
               "an authenticated MQTT-over-TLS connection stays open between keep-alives");
        std::string all = join(logsK, "\n");
        ncheck(!contains(all, tls_secret) && !contains(all, "wrong synthetic candidate") && !contains(all, "fixture-client"),
               "TLS diagnostics log neither candidate key nor PSK identity bytes");
        keyed.on_frame = ns_on_frame_noop;
        ncheck(!keyed.tls_psks_.empty() && !keyed.tls_identities_.empty() && kc->tls != 0 && !kc->tls->client_key.empty(),
               "the live stack retains only the credentials and traffic keys needed for reconnects");
        keyed.close();
        ncheck(keyed.tls_psks_.empty() && keyed.tls_identities_.empty() && !keyed.tuya_legacy_identity_psk_
               && keyed.conns_.empty() && keyed.output == 0 && keyed.on_frame == 0
               && kc->tls == 0 && kc->captured.empty() && kc->rx_buf.empty()
               && kc->tls_handshake_buf.empty() && kc->tls_plain_buf.empty(),
               "close drops PSKs, identities, TLS sessions, buffers and callback references");
    }

    nsection("== Tuya LAN discovery broadcast decoded ==");
    {
        /* A real announcement a BK7231N meter dump broadcast, re-encrypted here
         * from its decoded JSON. */
        std::string real = "{\"ip\":\"192.168.1.100\",\"gwId\":\"bf0e0f689d000d56656gmp\",\"active\":2,\"ablilty\":0,"
                           "\"encrypt\":true,\"productKey\":\"gqmmtjclqb7reg5p\",\"version\":\"3.3\"}";
        Bytes bkey = md5(Bytes("yGAdlopoPVldABfn"));
        ncheck(hexstr(bkey) == "6c1ec8e2bb9bb59ab50b0daf649b410a", "the shared broadcast key is the md5 every Tuya device uses");
        Bytes enc = aes_ecb_encrypt(bkey, pkcs7(real));
        ncheck(tuya_lan_decode(tuya_packet(enc)) == real,
               "an encrypted 3.3 announcement decodes to its JSON (gateway id, product key)");
        ncheck(tuya_lan_decode(S(tuya_packet(enc), 0, -1)).empty(), "a truncated one is not decoded");
        ncheck(tuya_lan_decode(tuya_packet(S(enc, 0, -16))).empty(), "nor is one whose ciphertext was cut short");
        Bytes bad = aes_ecb_encrypt(md5(Bytes("wrongkey")), pkcs7(real));
        ncheck(tuya_lan_decode(tuya_packet(bad)).empty(), "nor one encrypted with a different key");
        std::string plain = "{\"ip\":\"192.168.1.50\",\"gwId\":\"abc\",\"version\":\"3.1\"}";
        ncheck(tuya_lan_decode(tuya_packet(plain, 0x13, false)) == plain,
               "a 3.1 announcement is read straight out, it is not encrypted");
        ncheck(tuya_lan_decode(Bytes("hello")).empty() && tuya_lan_decode(Bytes(TUYA_PREFIX, 4) + zeros(30)).empty(),
               "ordinary datagrams are not mistaken for Tuya ones");
        Bytes good = tuya_packet(plain, 0x13, false);
        ncheck(tuya_lan_decode(S(good, 0, -4) + zeros(4)).empty(),
               "a datagram of the right length but without the Tuya end marker is rejected");
        ncheck(tuya_lan_decode(S(good, 0, 12) + be32((u32)good.size()) + F(good, 16)).empty(),
               "so is one whose declared length disagrees with how many bytes arrived");
        ncheck(tuya_lan_decode(S(good, 0, 12) + be32(18) + F(good, 16)).empty(),
               "...including one that declares less than arrived, which would otherwise read as half a message");
        Bytes sealed = Bytes(TUYA_PREFIX_35, 4) + NSB("\x00\x00") + be32(0) + be32(0x11) + be32(259)
                       + zeros(259) + Bytes(TUYA_SUFFIX_35, 4);
        std::string desc = tuya_lan_decode(sealed);
        ncheck(!desc.empty() && contains(desc, "3.4/3.5") && contains(desc, "command 0x11") && contains(desc, "sealed"),
               "a 3.4/3.5 announcement is named by its framing, and its contents are not guessed at");
        ncheck(tuya_lan_decode(S(sealed, 0, -1)).empty() && tuya_lan_decode(S(sealed, 0, -5) + Bytes(TUYA_SUFFIX_35, 4)).empty(),
               "...and a frame whose declared length does not match what arrived is rejected");
        StrList logsT; std::vector<Bytes> outT;
        NetStack ts2(ns_cfg("020000000001", true), ns_log, &logsT);
        ns_wire(ts2, logsT, outT);
        ts2.device_ip_ = str2ip("192.168.1.100"); ts2.device_mac_ = dev_mac;
        ts2.input(eth_frame(BCAST, dev_mac, ETH_IP, udp_packet(ts2.device_ip_, FF4, 63035, 6667, tuya_packet(plain, 0x13, false))));
        ncheck(any_log(logsT, "[NET] Tuya LAN broadcast from 192.168.1.100:63035", "\"gwId\":\"abc\"") && outT.empty(),
               "a device's announcement is reported with its identity, and nothing is sent back");
    }

    nsection("== offline DNS: stable sinkhole names, AAAA without records ==");
    {
        dns_ask(hs, "api.bekensim.test", 1, 0x4141);
        Bytes r1 = udp_out(out7, 53).back().p;
        dns_ask(hs, "api.bekensim.test", 1, 0x4343);
        Bytes r2 = udp_out(out7, 53).back().p;
        dns_ask(hs, "mqtt.bekensim.test", 1, 0x4444);
        Bytes r3 = udp_out(out7, 53).back().p;
        ncheck(F(r1, -4) == F(r2, -4) && S(r1, 0, 2) == NSB("\x41\x41") && S(r2, 0, 2) == NSB("\x43\x43"),
               "a name keeps its sinkhole address across lookups; each reply carries its query's id");
        ncheck(F(r3, -4) != F(r1, -4) && S(r3, -4, -2) == NSB("\x0a\xc8"), "another name gets its own 10.200/16 address");
        dns_ask(hs, "api.bekensim.test", 28, 0x4545);
        Bytes r4 = udp_out(out7, 53).back().p;
        ncheck(S(r4, 0, 2) == NSB("\x45\x45") && rd16(r4, 2) == 0x8180 && rd16(r4, 6) == 0,
               "AAAA answered with no records (no IPv6 here), not NXDOMAIN, so the A lookup still counts");
    }

    nsection("== TCP: the device closes (FIN) or resets (RST) a captured connection ==");
    {
        TcpConn *fc = open_capture(hs, sink2, 8883, 6300, sq, 11000);
        sq = dev_send(hs, fc, sink2, 8883, 6300, sq, NSB("\x16\x03\x01\x00\x05hello"));
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6300, 8883, sq, fc->snd_nxt, TCP_FIN | TCP_ACK, 8192)));
        hs.tick(32000000);
        NsSeg last = segs(out7, 6300).back();
        ncheck(last.ack == sq + 1 && (last.flags & TCP_FIN), "device FIN acknowledged and answered with our own FIN");
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6300, 8883, sq + 1, fc->snd_nxt, TCP_ACK, 8192)));
        hs.tick(32100000);
        ncheck(!has_conn(hs, sink2, 8883, hs.device_ip_, 6300), "...and the connection is gone once the device acknowledges that FIN");
        TcpConn *rc_ = open_capture(hs, sink2, 8883, 6301, sq, 12000);
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6301, 8883, sq, rc_->snd_nxt, TCP_RST | TCP_ACK, 0)));
        size_t n_before = segs(out7, 6301).size();
        hs.tick(32200000);
        ncheck(rc_->state == "CLOSED" && !has_conn(hs, sink2, 8883, hs.device_ip_, 6301)
               && segs(out7, 6301).size() == n_before && any_log(logs7, "reset by device"),
               "device RST drops the connection at once and nothing more is sent on it");
        hs.input(eth_frame(hs.mac_, dev_mac, ETH_IP, tcp_segment(hs.device_ip_, sink2, 6301, 8883, sq, 777, TCP_ACK | TCP_PSH, 8192, Bytes("late"))));
        NsSeg late = segs(out7, 6301).back();
        ncheck(late.flags == (TCP_RST | TCP_ACK) && late.seq == 777 && late.ack == sq + 4,
               "a segment for a connection we no longer know is answered with RST, so the firmware does not hang");
    }

    nsection("== TCP: window, segmentation, retransmission, duplicates ==");
    {
        StrList logs8; std::vector<Bytes> out8;
        NetStack ts(ns_cfg("020000000001", false), ns_log, &logs8);
        ns_wire(ts, logs8, out8);
        ts.ip = str2ip("192.168.4.100"); ts.device_ip_ = str2ip("192.168.4.1"); ts.device_mac_ = dev_mac;
        TcpConn *tc = new TcpConn(&ts, ts.ip, 41000, ts.device_ip_, 80, "forward");
        ConnKey k; k.lip = ts.ip; k.lport = 41000; k.rip = ts.device_ip_; k.rport = 80;
        ts.conns_[k] = tc;
        tc->state = "ESTABLISHED"; tc->rcv_nxt = 100; tc->peer_window = 3000;
        tc->unacked_seq = tc->snd_nxt;
        Bytes blob;
        for (int i = 0; i < 10000; i++) blob += (char)(i & 0xFF);
        tc->queue(blob);
        tc->pump(0);
        std::vector<NsSeg> all = segs(out8), first;
        for (size_t i = 0; i < all.size(); i++) if (!all[i].p.empty()) first.push_back(all[i]);
        size_t total = 0; bool mss_ok = true;
        for (size_t i = 0; i < first.size(); i++) { total += first[i].p.size(); if ((int)first[i].p.size() > MSS) mss_ok = false; }
        ncheck(total <= 3000 && mss_ok,
               "10000 bytes queued: no more than the device's 3000-byte window in flight, segments <= MSS");
        size_t resend_from = segs(out8).size();
        tc->pump(RETRANSMIT_US + 1);
        std::vector<NsSeg> again_all = segs(out8, -1), again;
        for (size_t i = resend_from; i < again_all.size(); i++) if (!again_all[i].p.empty()) again.push_back(again_all[i]);
        ncheck(!again.empty() && !first.empty() && again[0].seq == first[0].seq && again[0].p == first[0].p,
               "no ACK within the retransmit time: the unacknowledged data is sent again from the same seq");
        Bytes got; u64 t = RETRANSMIT_US + 2;
        while (got.size() < blob.size() && t < 50000000) {
            std::vector<NsSeg> sent = segs(out8);
            std::map<u32, Bytes> stream;   /* the device acknowledges everything it has seen so far, in order */
            for (size_t i = 0; i < sent.size(); i++) if (!sent[i].p.empty()) stream[sent[i].seq] = sent[i].p;
            u32 seq0 = first[0].seq;
            got.clear();
            for (;;) {
                std::map<u32, Bytes>::iterator it = stream.find(seq0 + (u32)got.size());
                if (it == stream.end()) break;
                got += it->second;
            }
            tc->on_segment(100, (seq0 + (u32)got.size()) & 0xFFFFFFFF, TCP_ACK, 3000, Bytes(), t);
            tc->pump(t);
            t += 1000;
        }
        ncheck(got == blob, "all 10000 bytes arrive in order once the device keeps acknowledging");
        size_t n_cap = tc->to_host.size();
        tc->on_segment(100, tc->snd_nxt, TCP_ACK | TCP_PSH, 3000, Bytes("abc"), t);
        tc->on_segment(100, tc->snd_nxt, TCP_ACK | TCP_PSH, 3000, Bytes("abc"), t);
        ncheck(tc->to_host.size() == n_cap + 1 && tc->rcv_nxt == 103,
               "a retransmitted (duplicate) segment from the device is delivered once");
        tc->on_segment(200, tc->snd_nxt, TCP_ACK | TCP_PSH, 3000, Bytes("later"), t);
        ncheck(tc->to_host.size() == n_cap + 1 && tc->rcv_nxt == 103,
               "an out-of-order segment is not delivered ahead of the gap");
        ncheck(segs(out8).back().ack == 103, "...and is answered with an ACK for what was actually received");
    }

    nsection("== SYN retransmission ==");
    {
        StrList logs9; std::vector<Bytes> out9;
        NetConfig c = ns_cfg("020000000001", false);
        c.http_gets.push_back("/x");
        NetStack ss(c, ns_log, &logs9);
        ns_wire(ss, logs9, out9);
        ss.ip = str2ip("192.168.4.100"); ss.device_ip_ = str2ip("192.168.4.1"); ss.device_mac_ = dev_mac;
        ss.link_ = true; ss.probe_sent_ = true; ss.dhcp_state_ = "BOUND";
        ss.tick(1000000);
        std::vector<NsSeg> syns;
        { std::vector<NsSeg> v = segs(out9); for (size_t i = 0; i < v.size(); i++) if (v[i].flags == TCP_SYN) syns.push_back(v[i]); }
        ncheck(syns.size() == 1, "first SYN sent");
        ss.tick(1000000 + RETRANSMIT_US + 1);
        std::vector<NsSeg> syns2;
        { std::vector<NsSeg> v = segs(out9); for (size_t i = 0; i < v.size(); i++) if (v[i].flags == TCP_SYN) syns2.push_back(v[i]); }
        ncheck(syns2.size() == 2 && syns2[1].seq == syns2[0].seq, "no SYN-ACK: the SYN is resent with the same sequence number");
        u64 t = 1000000 + RETRANSMIT_US + 1;
        for (int i = 0; i < SYN_MAX_TRIES + 2; i++) { t += RETRANSMIT_US + 1; ss.tick(t); }
        bool pending = false;
        for (std::map<ConnKey, TcpConn *>::iterator it = ss.conns_.begin(); it != ss.conns_.end(); ++it)
            if (it->second->kind == "client" && it->second->state == "SYN_SENT") pending = true;
        ncheck(any_log(logs9, "no SYN-ACK after") && !pending,
               strf("after %d resends the attempt is abandoned instead of hanging", SYN_MAX_TRIES));
    }

    nsection("== --net-forward: a real listener on 127.0.0.1 into the device ==");
    {
        int fport = free_tcp_port();
        StrList logs5; std::vector<Bytes> out5;
        NetConfig c = ns_cfg("020000000001", false);
        c.forwards.push_back(std::make_pair(fport, 80));
        NetStack fw(c, ns_log, &logs5);
        ns_wire(fw, logs5, out5);
        fw.ip = str2ip("192.168.4.100"); fw.device_ip_ = str2ip("192.168.4.1"); fw.device_mac_ = dev_mac;
        fw.link_ = true; fw.probe_sent_ = true; fw.dhcp_state_ = "BOUND";
        ncheck(any_log(logs5, strf("forwarding 127.0.0.1:%d -> device:80", fport)), strf("listening on 127.0.0.1:%d", fport));
        {
            StrList logs6;
            NetConfig c2 = ns_cfg("020000000002", false);
            c2.forwards.push_back(std::make_pair(fport, 80));
            NetStack second(c2, ns_log, &logs6);
            ncheck(any_log(logs6, strf("cannot listen on 127.0.0.1:%d", fport)),
                   "a second instance on the same port is refused, not silently sharing it");
        }
        sock_t browser = tcp_connect_blocking("127.0.0.1", fport, 5000);
        struct TcpOut { static bool syn80(const std::vector<Bytes> &frames) {
            std::vector<NsSeg> v = segs(frames);
            for (size_t i = 0; i < v.size(); i++) if (v[i].flags == TCP_SYN && v[i].dp == 80) return true;
            return false; }
            static bool has_payload(const std::vector<Bytes> &frames, const Bytes &p) {
            std::vector<NsSeg> v = segs(frames);
            for (size_t i = 0; i < v.size(); i++) if (v[i].p == p) return true;
            return false; } };
        double deadline = mono_time() + 5;
        while (mono_time() < deadline && !TcpOut::syn80(out5)) { fw.tick(20000000); sleep_ms(20); }
        ncheck(browser != BK_BAD_SOCK && TcpOut::syn80(out5), "browser connect accepted -> SYN to the device's port 80");
        TcpConn *fc = first_kind(fw, "forward");
        fw.input(eth_frame(fw.mac_, dev_mac, ETH_IP, tcp_segment(fw.device_ip_, fw.ip, 80, fc->lport, 9000, fc->snd_nxt, TCP_SYN | TCP_ACK, 4096)));
        Bytes request = "GET /index HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        sock_send_all(browser, request);
        deadline = mono_time() + 5;
        while (mono_time() < deadline && !TcpOut::has_payload(out5, request)) { fw.tick(21000000); sleep_ms(20); }
        ncheck(TcpOut::has_payload(out5, request), "the browser's request reaches the device");
        Bytes page = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n<html>OBK panel</html>";
        /* The device's last segment carries the FIN too: the page must still arrive whole. */
        fw.input(eth_frame(fw.mac_, dev_mac, ETH_IP, tcp_segment(fw.device_ip_, fw.ip, 80, fc->lport, 9001, fc->snd_nxt,
                                                                TCP_ACK | TCP_PSH | TCP_FIN, 4096, page)));
        Bytes got;
        char buf[65536];
        for (;;) {
            fw.tick(22000000);
            int n = sock_recv(browser, buf, sizeof(buf));   /* blocking, 5 s timeout */
            if (n <= 0) break;                               /* closed, or timed out */
            got += Bytes(buf, n);
        }
        ncheck(got == page, "the whole page reaches the browser even though data and FIN came in one segment");
        ncheck(got == page && sock_recv(browser, buf, 1) == 0, "then the browser sees the connection closed");
        sock_close(browser);
        /* A process that runs more than once (the GUI) must get its port back. */
        fw.close();
        ncheck(!fw.io_->thread.is_alive(), "close() stops the stack's host thread");
        StrList logs7b;
        NetConfig c3 = ns_cfg("020000000003", false);
        c3.forwards.push_back(std::make_pair(fport, 80));
        NetStack again(c3, ns_log, &logs7b);
        ncheck(any_log(logs7b, strf("forwarding 127.0.0.1:%d -> device:80", fport)),
               "after close() the same port can be forwarded again by the next run");
        again.close();
    }

    nsection("== --net-out: device TCP relayed through a host socket (to 127.0.0.1 only) ==");
    {
        std::string err;
        sock_t srv = tcp_listen("127.0.0.1", 0, &err);
        int port = tcp_bound_port(srv);
        StrList logs4; std::vector<Bytes> out4;
        NetConfig c = ns_cfg("020000000001", true);
        c.net_out = true;
        NetStack gw(c, ns_log, &logs4);
        ns_wire(gw, logs4, out4);
        gw.device_ip_ = str2ip("192.168.1.100"); gw.device_mac_ = dev_mac;
        gw.arp_[gw.device_ip_] = dev_mac;
        Bytes lo = str2ip("127.0.0.1");
        gw.input(eth_frame(gw.mac_, dev_mac, ETH_IP, tcp_segment(gw.device_ip_, lo, 5555, port, 100, 0, TCP_SYN, 8192)));
        sock_t peer = BK_BAD_SOCK;
        double deadline = mono_time() + 5;
        while (mono_time() < deadline && (peer = tcp_accept(srv)) == BK_BAD_SOCK) sleep_ms(10);
        deadline = mono_time() + 5;
        while (mono_time() < deadline && !(!out4.empty() && (at(out4.back(), 14 + 20 + 13) & TCP_SYN))) { gw.tick(1); sleep_ms(20); }
        Bytes sa4 = out4.empty() ? Bytes() : F(out4.back(), 14 + 20);
        ncheck(peer != BK_BAD_SOCK && at(sa4, 13) == (TCP_SYN | TCP_ACK), "device SYN -> real host connect -> SYN-ACK back to the device");
        TcpConn *rc = conn_at(gw, lo, port, gw.device_ip_, 5555);
        gw.input(eth_frame(gw.mac_, dev_mac, ETH_IP, tcp_segment(gw.device_ip_, lo, 5555, port, 101, rc->snd_nxt,
                                                                TCP_ACK | TCP_PSH, 8192, Bytes("hello host"))));
        char buf[256];
        int n = -1;
        deadline = mono_time() + 5;
        while (mono_time() < deadline) { n = sock_recv(peer, buf, 100); if (n != -1) break; sleep_ms(10); }
        ncheck(n > 0 && Bytes(buf, n) == "hello host", "device bytes arrive at the real host socket");
        sock_send_all(peer, Bytes("hello device"));
        deadline = mono_time() + 5;
        Bytes got;
        while (mono_time() < deadline && got.empty()) {
            gw.tick(2000000);
            for (size_t i = 0; i < out4.size(); i++) {
                const Bytes &f = out4[i];
                if (S(f, 12, 14) == NSB("\x08\x00") && at(f, 14 + 9) == IP_TCP && F(f, 14 + 40) == "hello device") got = F(f, 14 + 40);
            }
            sleep_ms(20);
        }
        ncheck(got == "hello device", "host reply relayed back to the device");
        sock_close(peer); sock_close(srv);

        nsection("== --net-out: DNS through the host resolver, UDP relayed both ways ==");
        u64 t_us = 3000000;
        size_t n4 = out4.size();
        dns_ask(gw, "localhost", 1, 0x5151);
        ncheck(udp_out(out4, 53, -1, n4).empty(), "an online lookup is not answered from the sinkhole");
        deadline = mono_time() + 10;
        while (mono_time() < deadline && udp_out(out4, 53, -1, n4).empty()) { t_us += 10000; gw.tick(t_us); sleep_ms(20); }
        std::vector<NsUdp> rr = udp_out(out4, 53, -1, n4);
        ncheck(!rr.empty() && S(rr[0].p, 0, 2) == NSB("\x51\x51") && F(rr[0].p, -4) == lo,
               "the name is resolved by the host (localhost -> 127.0.0.1) and the answer reaches the device");
        ncheck(any_log(logs4, "[NET] DNS localhost -> 127.0.0.1"), "the lookup is logged");
        n4 = out4.size();
        dns_ask(gw, "localhost", 1, 0x5252);
        rr = udp_out(out4, 53, -1, n4);
        ncheck(!rr.empty() && S(rr[0].p, 0, 2) == NSB("\x52\x52") && F(rr[0].p, -4) == lo,
               "a repeat lookup is answered at once from the cache");
        sock_t us = udp_bind("127.0.0.1", 0);
        int uport = tcp_bound_port(us);
        gw.input(eth_frame(gw.mac_, dev_mac, ETH_IP, udp_packet(gw.device_ip_, lo, 5600, uport, Bytes("ping udp"))));
        u8 addr_ip[4] = {0, 0, 0, 0}, addr2_ip[4] = {0, 0, 0, 0}; int addr_port = 0, addr2_port = 0;
        n = -1;
        deadline = mono_time() + 5;
        while (mono_time() < deadline) { n = sock_recvfrom(us, buf, 100, addr_ip, &addr_port); if (n >= 0) break; sleep_ms(10); }
        ncheck(n >= 0 && Bytes(buf, n) == "ping udp", "a device datagram leaves through a host UDP socket");
        gw.input(eth_frame(gw.mac_, dev_mac, ETH_IP, udp_packet(gw.device_ip_, lo, 5600, uport, Bytes("again"))));
        n = -1;
        deadline = mono_time() + 5;
        while (mono_time() < deadline) { n = sock_recvfrom(us, buf, 100, addr2_ip, &addr2_port); if (n >= 0) break; sleep_ms(10); }
        int relayed = 0;
        for (size_t i = 0; i < logs4.size(); i++) if (contains(logs4[i], "relayed") && contains(logs4[i], ":5600 ->")) relayed++;
        ncheck(n >= 0 && Bytes(buf, n) == "again" && memcmp(addr_ip, addr2_ip, 4) == 0 && addr_port == addr2_port && relayed == 1,
               "the flow keeps its host socket: same source port, logged once");
        n4 = out4.size();
        sock_sendto(us, "pong udp", 8, addr_ip, addr_port);
        deadline = mono_time() + 5;
        while (mono_time() < deadline && udp_out(out4, -1, 5600, n4).empty()) { t_us += 10000; gw.tick(t_us); sleep_ms(20); }
        rr = udp_out(out4, -1, 5600, n4);
        ncheck(!rr.empty() && rr[0].src == lo && rr[0].sp == uport && rr[0].p == "pong udp",
               "the host's reply comes back to the device from the address it wrote to");
        sock_close(us);
    }

    printf("\nAll %d netstack self-tests passed.\n", ns_checks);
    return ns_checks;
}
};   /* struct NsSelfTest */

int netstack_selftests() {
    ns_checks = 0;
    try {
        return NsSelfTest::netstack_selftests_body();
    } catch (NsFail &) {
        return -1;
    } catch (std::exception &e) {
        printf("  [FAIL] exception: %s\n", e.what());
        return -1;
    } catch (...) {
        printf("  [FAIL] unexpected exception\n");
        return -1;
    }
}

#undef NSB

#endif
