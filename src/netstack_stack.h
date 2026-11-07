/* NetStack method bodies, included inside namespace netstack by netstack_impl.h. */
#ifndef BK_NETSTACK_STACK_H
#define BK_NETSTACK_STACK_H

NetStack::NetStack(const NetConfig &cfg, NetLogFn logfn, void *log_ctx)
    : output(0), output_ctx(0), on_frame(0), on_frame_ctx(0),
      gateway(cfg.gateway), log_(logfn), log_ctx_(log_ctx), mac_(cfg.mac),
      netmask_(Bytes("\xff\xff\xff\x00", 4)), net_out_(cfg.net_out), now_us_(0),
      pool_next_(100), next_port_(40000), dns_next_id_(1), sink_next_(1),
      forwards_(cfg.forwards), dhcp_state_("INIT"), dhcp_xid_(0x5A17C0DE), dhcp_next_us_(0),
      link_(false), link_up_valid_(false), link_up_us_(0), next_probe_us_(0), probe_sent_(false),
      ping_seq_(0), ping_replies_(0), ident_(1), http_gets_(cfg.http_gets),
      tls_psks_(cfg.tls_psks), tuya_legacy_identity_psk_(cfg.tuya_legacy_identity_psk),
      mqtt_pubs_(cfg.mqtt_pubs), http_next_us_(0) {
    ip = !cfg.ip.empty() ? cfg.ip : (gateway ? str2ip("192.168.1.1") : Bytes());
    io_ = new HostIO();
    for (size_t i = 0; i < forwards_.size(); i++) {
        std::string err;
        sock_t s = io_->listen(forwards_[i].first, forwards_[i].second, &err);
        if (s != BK_BAD_SOCK) { listeners_.push_back(s); listener_dport_[s] = forwards_[i].second;
            log(strf("[NET] forwarding 127.0.0.1:%d -> device:%d", forwards_[i].first, forwards_[i].second)); }
        else log(strf("[NET] cannot listen on 127.0.0.1:%d: %s", forwards_[i].first, err.c_str()));
    }
    if (net_out_) log("[NET] REAL INTERNET enabled: device DNS/TCP/UDP can leave this machine");
}
NetStack::~NetStack() {
    close();
    delete io_;
    while (!retired_.empty()) { delete retired_.front(); retired_.pop_front(); }
}

void NetStack::log(const std::string &msg) { if (log_) log_(log_ctx_, msg); }

static const size_t RETIRED_MAX = 256;
void NetStack::retire(TcpConn *conn) {
    retired_.push_back(conn);
    while (retired_.size() > RETIRED_MAX) { delete retired_.front(); retired_.pop_front(); }
}

/* Release host resources and forget all TLS credential material. */
void NetStack::close() {
    if (io_) io_->shutdown();
    tls_psks_.clear(); tuya_legacy_identity_psk_ = false;
    tls_identities_.clear();
    /* these hold the clear ClientKeyExchange (with its identity) and decrypted
     * application records while a run is live */
    for (std::map<ConnKey, TcpConn *>::iterator it = conns_.begin(); it != conns_.end(); ++it) {
        forget_connection(it->second);
        retire(it->second);
    }
    conns_.clear();
    output = 0; on_frame = 0;
}

void NetStack::forget_connection(TcpConn *conn) {
    if (conn->tls) { conn->tls->clear_secrets(); delete conn->tls; conn->tls = 0; }
    conn->captured.clear(); conn->rx_buf.clear(); conn->tls_handshake_buf.clear(); conn->tls_plain_buf.clear();
}

void NetStack::device_reset(u64 now_us) {
    now_us_ = now_us;
    io_->reset_connections();
    for (std::map<ConnKey, TcpConn *>::iterator it = conns_.begin(); it != conns_.end(); ++it) { forget_connection(it->second); retire(it->second); }
    conns_.clear();
    udp_flows_.clear();
    dns_pending_.clear();
    for (size_t i = 0; i < pending_forward_wait_.size(); i++) sock_close(pending_forward_wait_[i].sock);
    pending_forward_wait_.clear();
    link_ = false; link_up_valid_ = false; device_mac_.clear(); device_ip_.clear();
    probe_sent_ = false; next_probe_us_ = now_us; http_next_us_ = now_us;
    dhcp_state_ = "INIT"; dhcp_next_us_ = now_us; dhcp_server_.clear(); offered_.clear();
    if (!gateway) ip.clear();
}

void NetStack::trace(const std::string &dir, const Bytes &frame) {
    if (!on_frame) return;
    try { on_frame(on_frame_ctx, dir, frame, now_us_); }
    catch (...) {}   /* a front end must never break the run */
}
void NetStack::send_eth(const Bytes &dst_mac, u16 ethertype, const Bytes &payload) {
    if (output) { Bytes frame = eth_frame(dst_mac, mac_, ethertype, payload); trace("to device", frame); output(output_ctx, frame); }
}
Bytes NetStack::mac_for(const Bytes &ipaddr) {
    std::map<Bytes, Bytes>::iterator it = arp_.find(ipaddr);
    if (it != arp_.end()) return it->second;
    if (!device_mac_.empty() && (ipaddr == device_ip_ || !gateway)) return device_mac_;
    return BCAST_MAC();
}
void NetStack::send_ip(const Bytes &pkt, const Bytes &dst_ip) { send_eth(mac_for(dst_ip), ETH_IP, pkt); }
void NetStack::send_arp_request(const Bytes &target_ip) {
    if (ip.empty()) return;
    Bytes req = be16(1) + be16(ETH_IP) + Bytes(1, (char)6) + (char)4 + be16(1) + mac_ + ip + Bytes(6, '\0') + target_ip;
    send_eth(BCAST_MAC(), ETH_ARP, req);
}
void NetStack::send_ping(const Bytes &target_ip) {
    if (ip.empty()) return;
    ping_seq_++;
    Bytes icmp = Bytes(1, (char)8) + (char)0 + be16(0) + be16(0xBEEF) + be16((u16)ping_seq_) + "BekenEmulator ping";
    icmp = icmp.substr(0, 2) + be16(inet_checksum(icmp)) + icmp.substr(4);
    send_ip(ipv4_packet(ip, target_ip, IP_ICMP, icmp, ident_), target_ip);
    ident_ = (ident_ + 1) & 0xFFFF;
}
void NetStack::link_up(const Bytes &device_mac) {
    if (!device_mac.empty()) device_mac_ = device_mac;
    link_ = true; link_up_valid_ = true; link_up_us_ = now_us_;
    if (!gateway && ip.empty()) { dhcp_state_ = "INIT"; dhcp_next_us_ = now_us_; }
}
void NetStack::input(const Bytes &frame) {
    if (frame.size() < 14) return;
    Bytes dst = frame.substr(0, 6), src = frame.substr(6, 6);
    u16 etype = rdbe16(frame, 12);
    if (src != mac_) trace("from device", frame);
    if (src == mac_) return;
    if (device_mac_.empty() || (gateway && src != device_mac_)) device_mac_ = src;
    if (etype == ETH_ARP) arp_in(frame.substr(14), src);
    else if (etype == ETH_IP) ipv4_in(frame.substr(14), src);
}
void NetStack::arp_in(const Bytes &p, const Bytes &src_mac) {
    if (p.size() < 28) return;
    int op = rdbe16(p, 6);
    Bytes sha = p.substr(8, 6), spa = p.substr(14, 4), tpa = p.substr(24, 4);
    if (spa != Bytes("\x00\x00\x00\x00", 4)) { arp_[spa] = sha; if (sha == device_mac_) device_ip_ = spa; }
    if (op == 2 && sha == device_mac_) log(strf("[NET] ARP: device %s is at %s", ip2str(spa).c_str(), mac2str(sha).c_str()));
    if (op == 1 && !ip.empty() && tpa == ip) {
        Bytes reply = be16(1) + be16(ETH_IP) + (char)6 + (char)4 + be16(2) + mac_ + ip + sha + spa;
        send_eth(sha, ETH_ARP, reply);
    } else if (op == 1 && gateway && tpa != spa && !local(tpa)) {
        Bytes reply = be16(1) + be16(ETH_IP) + (char)6 + (char)4 + be16(2) + mac_ + tpa + sha + spa;
        send_eth(sha, ETH_ARP, reply);
    }
}
bool NetStack::local(const Bytes &ipaddr) {
    if (ip.empty() || ipaddr.size() < 4) return false;
    for (int i = 0; i < 4; i++) if (((u8)ipaddr[i] & (u8)netmask_[i]) != ((u8)ip[i] & (u8)netmask_[i])) return false;
    return true;
}
void NetStack::ipv4_in(const Bytes &p, const Bytes &src_mac) {
    if (p.size() < 20 || ((u8)p[0] >> 4) != 4) return;
    int ihl = ((u8)p[0] & 0xF) * 4;
    int total = rdbe16(p, 2);
    int proto = (u8)p[9];
    Bytes src = p.substr(12, 4), dst = p.substr(16, 4);
    Bytes payload = (total >= ihl && total <= (int)p.size()) ? p.substr(ihl, total - ihl) : p.substr(ihl);
    if (src != Bytes("\x00\x00\x00\x00", 4) && src_mac == device_mac_) { device_ip_ = src; arp_[src] = src_mac; }
    if (proto == IP_UDP) udp_in(src, dst, payload);
    else if (proto == IP_TCP) tcp_in(src, dst, payload);
    else if (proto == IP_ICMP) icmp_in(src, dst, payload);
}
void NetStack::icmp_in(const Bytes &src, const Bytes &dst, const Bytes &p) {
    if (p.size() >= 8 && (u8)p[0] == 0 && p.substr(4, 2) == Bytes("\xbe\xef", 2)) {
        ping_replies_++;
        log(strf("[NET] ping reply from %s (seq %d)", ip2str(src).c_str(), rdbe16(p, 6)));
        return;
    }
    if (p.size() < 8 || (u8)p[0] != 8) return;
    if (dst != ip && !gateway) return;
    Bytes reply = Bytes("\x00\x00\x00\x00", 4) + p.substr(4);
    reply = reply.substr(0, 2) + be16(inet_checksum(reply)) + reply.substr(4);
    send_ip(ipv4_packet(dst, src, IP_ICMP, reply), src);
    log(strf("[NET] ping %s -> %s answered", ip2str(src).c_str(), ip2str(dst).c_str()));
}
std::string NetStack::name_of(const Bytes &ipaddr) {
    std::map<Bytes, std::string>::iterator it = sinkhole_rev_.find(ipaddr);
    return it != sinkhole_rev_.end() ? strf("%s(%s)", ip2str(ipaddr).c_str(), it->second.c_str()) : ip2str(ipaddr);
}
void NetStack::udp_in(const Bytes &src, const Bytes &dst, const Bytes &p) {
    if (p.size() < 8) return;
    int sport = rdbe16(p, 0), dport = rdbe16(p, 2), ulen = rdbe16(p, 4);
    Bytes data = (ulen >= 8 && ulen <= (int)p.size()) ? p.substr(8, ulen - 8) : p.substr(8);
    if (dport == 67 && gateway) { dhcp_server(src, sport, data); return; }
    if (dport == 68 && !gateway) { dhcp_client_rx(data); return; }
    if (dport == 53) { dns_in(src, sport, dst, data); return; }
    if (dport == 123) { ntp_in(src, sport, dst, data); return; }
    if ((u8)dst[0] >= 224 || dst == Bytes("\xff\xff\xff\xff", 4) || (u8)dst[3] == 255) {
        if (dport == 6666 || dport == 6667) {
            std::string info = tuya_lan_decode(data);
            if (!info.empty()) { log(strf("[NET] Tuya LAN broadcast from %s:%d: %s", ip2str(src).c_str(), sport, info.c_str())); return; }
        }
        std::string text; for (size_t i = 0; i < data.size() && i < 60; i++) { u8 c = (u8)data[i]; text += (c >= 32 && c < 127) ? (char)c : '.'; }
        log(strf("[NET] UDP broadcast %s:%d -> %s:%d %d bytes: %s", ip2str(src).c_str(), sport, ip2str(dst).c_str(), dport, (int)data.size(), text.c_str()));
        return;
    }
    if (gateway && net_out_) {
        UdpKey key; key.dev_port = sport; key.dst_ip = dst; key.dst_port = dport;
        std::map<UdpKey, UdpFlow>::iterator ent = udp_flows_.find(key);
        if (ent == udp_flows_.end()) {
            sock_t s = io_->udp_socket_for(key);
            if (s == BK_BAD_SOCK) return;
            UdpFlow f; f.sock = s; f.last_us = now_us_;
            ent = udp_flows_.insert(std::make_pair(key, f)).first;
            log(strf("[NET] UDP %s:%d -> %s:%d relayed", ip2str(src).c_str(), sport, ip2str(dst).c_str(), dport));
        }
        ent->second.last_us = now_us_;
        sock_sendto(ent->second.sock, data.data(), (int)data.size(), (const u8 *)dst.data(), dport);
        return;
    }
    log(strf("[NET] UDP %s:%d -> %s:%d %d bytes (offline, dropped)", ip2str(src).c_str(), sport, name_of(dst).c_str(), dport, (int)data.size()));
}
void NetStack::ntp_in(const Bytes &src, int sport, const Bytes &dst, const Bytes &data) {
    if (data.size() < 48) return;
    const u64 NTP_DELTA = 2208988800ULL;
    double t = wall_time() + (double)NTP_DELTA;
    u32 secs = (u32)t;
    u32 frac = (u32)((t - secs) * 4294967296.0);
    Bytes reply(48, '\0');
    reply[0] = (char)((0 << 6) | (4 << 3) | 4);
    reply[1] = (char)2;
    reply[2] = (char)((u8)data[2] ? (u8)data[2] : 6);
    reply[3] = (char)0xEC;
    reply.replace(12, 4, "LOCL", 4);
    Bytes ts = be32(secs) + be32(frac);
    reply.replace(16, 8, ts);
    reply.replace(24, 8, data.substr(40, 8));
    reply.replace(32, 8, ts);
    reply.replace(40, 8, ts);
    Bytes reply_src = dst != Bytes("\xff\xff\xff\xff", 4) ? dst : ip;
    send_ip(udp_packet(reply_src, src, 123, sport, reply), src);
    log(strf("[NET] NTP %s -> %s answered with the host clock", ip2str(src).c_str(), name_of(dst).c_str()));
}
void NetStack::dns_in(const Bytes &src, int sport, const Bytes &dst, const Bytes &data) {
    if (data.size() < 12) return;
    u16 txid = rdbe16(data, 0);
    int qd = rdbe16(data, 4);
    if (qd < 1) return;
    size_t off;
    std::string name = dns_name(data, 12, off);
    if (off + 4 > data.size()) return;
    int qtype = rdbe16(data, off), qclass = rdbe16(data, off + 2); (void)qclass;
    Bytes question = data.substr(12, off + 4 - 12);
    DnsKey key; key.src = src; key.sport = sport; key.txid = txid; key.name = name; key.qtype = qtype; key.dst = dst; key.question = question;
    if (qtype != 1) { dns_reply(key, Bytes(), false); return; }   /* AAAA (no IPv6 here) and others */
    if (!net_out_) {
        std::map<std::string, Bytes>::iterator it = sinkhole_.find(name);
        Bytes ipb;
        if (it == sinkhole_.end()) {
            ipb = Bytes(1, (char)10) + (char)200 + (char)((sink_next_ >> 8) & 0xFF) + (char)(sink_next_ & 0xFF);
            sink_next_++;
            sinkhole_[name] = ipb; sinkhole_rev_[ipb] = name;
        } else ipb = it->second;
        log(strf("[NET] DNS %s -> %s (offline sinkhole)", name.c_str(), ip2str(ipb).c_str()));
        dns_reply(key, ipb, true);
        return;
    }
    std::map<std::string, Bytes>::iterator c = dns_cache_.find(name);
    if (c != dns_cache_.end()) { dns_reply(key, c->second, true); return; }
    /* Python keys pending queries by (src, sport, txid, name, qtype, dst): an
     * identical repeat replaces the earlier entry, so it is answered once. */
    for (std::map<int, DnsKey>::iterator p = dns_pending_.begin(); p != dns_pending_.end(); ++p) {
        const DnsKey &k = p->second;
        if (k.src == key.src && k.sport == key.sport && k.txid == key.txid && k.name == key.name
            && k.qtype == key.qtype && k.dst == key.dst) { dns_pending_.erase(p); break; }
    }
    int id = dns_next_id_++;
    dns_pending_[id] = key;
    io_->resolve(name, id);
}
void NetStack::dns_reply(const DnsKey &key, const Bytes &ipb, bool have_ip) {
    Bytes answer; int an = 0; u16 flags;
    if (have_ip) { flags = 0x8180; answer = Bytes("\xc0\x0c", 2) + be16(1) + be16(1) + be32(60) + be16(4) + ipb; an = 1; }
    else { flags = key.qtype == 1 ? 0x8183 : 0x8180; }
    Bytes pkt = be16(key.txid) + be16(flags) + be16(1) + be16((u16)an) + be16(0) + be16(0) + key.question + answer;
    Bytes reply_src = key.dst != Bytes("\xff\xff\xff\xff", 4) ? key.dst : ip;
    send_ip(udp_packet(reply_src, key.src, 53, key.sport, pkt), key.src);
}
std::map<int, Bytes> NetStack::dhcp_opts(const Bytes &data) {
    std::map<int, Bytes> opts;
    if (data.size() < 240 || data.substr(236, 4) != Bytes("\x63\x82\x53\x63", 4)) return opts;
    size_t p = 240;
    while (p < data.size()) {
        int t = (u8)data[p];
        if (t == 255) break;
        if (t == 0) { p++; continue; }
        if (p + 1 >= data.size()) break;
        int l = (u8)data[p + 1];
        opts[t] = data.substr(p + 2, l);
        p += 2 + l;
    }
    return opts;
}
void NetStack::dhcp_server(const Bytes &src, int sport, const Bytes &data) {
    (void)src; (void)sport;
    std::map<int, Bytes> opts = dhcp_opts(data);
    if (data.size() < 240 || !opts.count(53)) return;
    int mtype = (u8)opts[53][0];
    Bytes xid = data.substr(4, 4);
    Bytes chaddr = data.substr(28, 6);
    Bytes lease_ip;
    std::map<Bytes, Bytes>::iterator it = leases_.find(chaddr);
    if (it == leases_.end()) { lease_ip = ip.substr(0, 3) + (char)pool_next_; pool_next_++; leases_[chaddr] = lease_ip; }
    else lease_ip = it->second;
    int reply_type;
    if (mtype == 1) reply_type = 2;
    else if (mtype == 3) reply_type = 5;
    else return;
    Bytes resp(240, '\0');
    resp.replace(0, 4, "\x02\x01\x06\x00", 4);
    resp.replace(4, 4, xid);
    resp.replace(10, 2, data.substr(10, 2));
    resp.replace(16, 4, lease_ip);
    resp.replace(20, 4, ip);
    resp.replace(28, 6, chaddr);
    resp.replace(236, 4, "\x63\x82\x53\x63", 4);
    Bytes o = Bytes("\x35\x01", 2) + (char)reply_type;
    o += Bytes("\x36\x04", 2) + ip;
    o += Bytes("\x33\x04", 2) + be32(86400);
    o += Bytes("\x01\x04", 2) + netmask_;
    o += Bytes("\x03\x04", 2) + ip;
    o += Bytes("\x06\x04", 2) + ip;
    o += (char)0xff;
    Bytes pkt = udp_packet(ip, Bytes("\xff\xff\xff\xff", 4), 67, 68, resp + o);
    send_eth(BCAST_MAC(), ETH_IP, pkt);
    if (reply_type == 5) {
        device_ip_ = lease_ip; device_mac_ = chaddr; arp_[lease_ip] = chaddr;
        log(strf("[NET] DHCP: device %s got %s (gateway/DNS %s)", mac2str(chaddr).c_str(), ip2str(lease_ip).c_str(), ip2str(ip).c_str()));
    } else log(strf("[NET] DHCP: offering %s to %s", ip2str(lease_ip).c_str(), mac2str(chaddr).c_str()));
}
void NetStack::dhcp_client_send(int mtype) {
    Bytes msg(240, '\0');
    msg.replace(0, 4, "\x01\x01\x06\x00", 4);
    msg.replace(4, 4, be32(dhcp_xid_));
    msg.replace(10, 2, "\x80\x00", 2);
    msg.replace(28, 6, mac_);
    msg.replace(236, 4, "\x63\x82\x53\x63", 4);
    Bytes o = Bytes("\x35\x01", 2) + (char)mtype;
    o += Bytes("\x3d\x07\x01", 3) + mac_;
    if (mtype == 3 && !offered_.empty()) {
        o += Bytes("\x32\x04", 2) + offered_;
        if (!dhcp_server_.empty()) o += Bytes("\x36\x04", 2) + dhcp_server_;
    }
    o += Bytes("\x37\x03\x01\x03\x06\xff", 6);
    Bytes pkt = udp_packet(Bytes("\x00\x00\x00\x00", 4), Bytes("\xff\xff\xff\xff", 4), 68, 67, msg + o);
    send_eth(BCAST_MAC(), ETH_IP, pkt);
}
void NetStack::dhcp_client_rx(const Bytes &data) {
    std::map<int, Bytes> opts = dhcp_opts(data);
    if (data.size() < 240 || !opts.count(53) || data.substr(4, 4) != be32(dhcp_xid_)) return;
    int mtype = (u8)opts[53][0];
    if (mtype == 2 && dhcp_state_ == "DISCOVER") {
        offered_ = data.substr(16, 4);
        dhcp_server_ = opts.count(54) ? opts[54] : data.substr(20, 4);
        dhcp_state_ = "REQUEST";
        dhcp_client_send(3);
        dhcp_next_us_ = now_us_ + 3000000;
    } else if (mtype == 5 && dhcp_state_ == "REQUEST") {
        ip = data.substr(16, 4);
        if (opts.count(1)) netmask_ = opts[1];
        Bytes router = opts.count(3) ? opts[3] : Bytes();
        dhcp_state_ = "BOUND";
        log(strf("[NET] DHCP: we got %s/%s from the device (router %s)", ip2str(ip).c_str(), ip2str(netmask_).c_str(), router.empty() ? "-" : ip2str(router).c_str()));
        if (!router.empty()) device_ip_ = router;
    } else if (mtype == 6) { dhcp_state_ = "INIT"; dhcp_next_us_ = now_us_ + 2000000; }
}
void NetStack::arp_probe_tick() {
    if (!device_ip_.empty() || !link_up_valid_) return;
    if (elapsed_lt(now_us_, link_up_us_, 4000000) || now_us_ < next_probe_us_) return;
    next_probe_us_ = now_us_ + 2000000;
    const char *guesses[] = {"192.168.4.1", "192.168.175.1", "192.168.0.1", "192.168.1.1", "10.0.0.1"};
    for (int i = 0; i < 5; i++) {
        Bytes cand = str2ip(guesses[i]);
        Bytes req = be16(1) + be16(ETH_IP) + (char)6 + (char)4 + be16(1) + mac_ + Bytes(4, '\0') + Bytes(6, '\0') + cand;
        send_eth(BCAST_MAC(), ETH_ARP, req);
    }
}
void NetStack::dhcp_client_tick() {
    if (gateway || !link_) return;
    arp_probe_tick();
    if (dhcp_state_ == "BOUND" || dhcp_state_ == "STATIC") return;
    if (ip.empty() && !device_ip_.empty() && link_up_valid_ && elapsed_gt(now_us_, link_up_us_, STATIC_FALLBACK_US)) {
        ip = device_ip_.substr(0, 3) + (char)(((u8)device_ip_[3] % 250) + 2);
        dhcp_state_ = "STATIC";
        log(strf("[NET] no DHCP lease from the device after %d s: using static %s/%s", (int)(STATIC_FALLBACK_US / 1000000), ip2str(ip).c_str(), ip2str(netmask_).c_str()));
        return;
    }
    if (now_us_ < dhcp_next_us_) return;
    if (dhcp_state_ == "INIT" || dhcp_state_ == "DISCOVER") { dhcp_state_ = "DISCOVER"; dhcp_client_send(1); dhcp_next_us_ = now_us_ + 3000000; }
    else if (dhcp_state_ == "REQUEST") { dhcp_client_send(3); dhcp_next_us_ = now_us_ + 3000000; }
}
void NetStack::tcp_in(const Bytes &src, const Bytes &dst, const Bytes &p) {
    if (p.size() < 20) return;
    int sport = rdbe16(p, 0), dport = rdbe16(p, 2);
    u32 seq = rdbe32(p, 4), ack = rdbe32(p, 8);
    int doff = ((u8)p[12] >> 4) * 4;
    u8 flags = (u8)p[13] & 0x3F;
    int window = rdbe16(p, 14);
    Bytes payload = p.substr(doff);
    ConnKey key; key.lip = dst; key.lport = dport; key.rip = src; key.rport = sport;
    std::map<ConnKey, TcpConn *>::iterator it = conns_.find(key);
    if (it == conns_.end()) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) tcp_new(src, sport, dst, dport, seq, window);
        else if (!(flags & TCP_RST)) {
            Bytes pkt = tcp_segment(dst, src, dport, sport, ack, (seq + (u32)payload.size()) & 0xFFFFFFFF, TCP_RST | TCP_ACK, 0);
            send_ip(pkt, src);
        }
        return;
    }
    TcpConn *conn = it->second;
    conn->on_segment(seq, ack, flags, window, payload, now_us_);
    if (conn->state == "CLOSED") { conns_.erase(key); retire(conn); }
}
void NetStack::tcp_new(const Bytes &src, int sport, const Bytes &dst, int dport, u32 seq, u32 window) {
    std::string target = name_of(dst);
    std::string kind = (dst == ip || (gateway && !net_out_) || !gateway) ? "capture" : "relay";
    TcpConn *conn = new TcpConn(this, dst, dport, src, sport, kind);
    conn->rcv_nxt = (seq + 1) & 0xFFFFFFFF;
    conn->peer_window = window;
    conn->last_activity_us = now_us_;
    ConnKey key; key.lip = dst; key.lport = dport; key.rip = src; key.rport = sport;
    conns_[key] = conn;
    if (kind == "capture") {
        log(strf("[NET] TCP connect %s:%d -> %s:%d (accepted for capture)", ip2str(src).c_str(), sport, target.c_str(), dport));
        conn->state = "SYN_RECEIVED";
        conn->send(TCP_SYN | TCP_ACK, Bytes(), Bytes("\x02\x04", 2) + be16(MSS));
        conn->snd_nxt += 1;
    } else {
        log(strf("[NET] TCP connect %s:%d -> %s:%d (relaying)", ip2str(src).c_str(), sport, target.c_str(), dport));
        conn->state = "CONNECTING";
        io_->connect(conn->id, ip2str(dst), dport);
    }
}
void NetStack::http_client_tick() {
    if (http_gets_.empty() || ip.empty() || device_ip_.empty()) return;
    if (now_us_ < http_next_us_) return;
    for (std::map<ConnKey, TcpConn *>::iterator it = conns_.begin(); it != conns_.end(); ++it)
        if (it->second->kind == "client" && !it->second->described) return;
    std::string path = http_gets_.front(); http_gets_.erase(http_gets_.begin());
    http_next_us_ = now_us_ + 500000;
    int lport = next_port_; next_port_ = 40000 + ((next_port_ - 40000 + 1) % 20000);
    TcpConn *conn = new TcpConn(this, ip, lport, device_ip_, 80, "client");
    conn->request_path = path; conn->opened_us = now_us_; conn->last_activity_us = now_us_;
    /* Python: ("GET %s ..." % path).encode("latin-1") - the CLI hands the path
     * over as UTF-8 text (already checked to be Latin-1 encodable) */
    Bytes wire_path;
    if (!utf8_to_latin1(path, wire_path))
        wire_path = Bytes(path);
    conn->queue(strf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: BekenEmulator\r\nConnection: close\r\n\r\n", wire_path.c_str(), ip2str(device_ip_).c_str()));
    ConnKey key; key.lip = ip; key.lport = lport; key.rip = device_ip_; key.rport = 80;
    conns_[key] = conn;
    conn->state = "SYN_SENT"; conn->last_send_us = now_us_;
    conn->send(TCP_SYN, Bytes(), Bytes("\x02\x04", 2) + be16(MSS));
    conn->snd_nxt += 1;
    log(strf("[NET] HTTP GET http://%s%s", ip2str(device_ip_).c_str(), path.c_str()));
}
void NetStack::report_http(TcpConn *conn, const Bytes &data) {
    size_t sep = data.find("\r\n\r\n");
    Bytes head, body;
    if (sep != std::string::npos) { head = data.substr(0, sep); body = data.substr(sep + 4); }
    else { size_t s2 = data.find("\n\n"); if (s2 != std::string::npos) { head = data.substr(0, s2); body = data.substr(s2 + 2); } else { head = data; } }
    std::string status;
    if (!head.empty()) { size_t nl = head.find("\n"); status = strip(nl == std::string::npos ? head : head.substr(0, nl)); }
    else status = "(empty reply)";
    std::string text = body;
    /* collapse whitespace */
    std::string one; bool sp = false;
    for (size_t i = 0; i < text.size(); i++) { char c = text[i]; if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { sp = true; } else { if (sp && !one.empty()) one += ' '; sp = false; one += c; } }
    if (one.size() > 300) one = one.substr(0, 300) + " ...";
    log(strf("[NET] HTTP GET %s -> %s, %d byte body: %s", conn->request_path.c_str(), status.c_str(), (int)body.size(), one.c_str()));
    HttpReply r; r.path = conn->request_path; r.status = status; r.body = body;
    http_replies_.push_back(r);
}
void NetStack::forward_open(sock_t sock, int dport) {
    if (device_ip_.empty() || ip.empty()) { PendFwd pf; pf.sock = sock; pf.dport = dport; pf.at = now_us_; pending_forward_wait_.push_back(pf); return; }
    int lport = next_port_; next_port_ = 40000 + ((next_port_ - 40000 + 1) % 20000);
    TcpConn *conn = new TcpConn(this, ip, lport, device_ip_, dport, "forward");
    conn->sock = sock;
    io_->attach(sock, conn);
    ConnKey key; key.lip = ip; key.lport = lport; key.rip = device_ip_; key.rport = dport;
    conns_[key] = conn;
    conn->state = "SYN_SENT"; conn->last_send_us = now_us_; conn->last_activity_us = now_us_;
    conn->send(TCP_SYN, Bytes(), Bytes("\x02\x04", 2) + be16(MSS));
    conn->snd_nxt += 1;
    log(strf("[NET] TCP forward: %s:%d -> device %s:%d", ip2str(ip).c_str(), lport, ip2str(device_ip_).c_str(), dport));
}
void NetStack::tick(u64 now_us) {
    now_us_ = now_us;
    dhcp_client_tick();
    if (!gateway && link_ && !ip.empty() && !device_ip_.empty() && !probe_sent_) {
        probe_sent_ = true; send_arp_request(device_ip_); send_ping(device_ip_);
    }
    http_client_tick();
    std::vector<std::pair<sock_t, int> > accepted;
    { ScopedLock lk(io_->lock); while (!io_->accepted.empty()) { accepted.push_back(io_->accepted.front()); io_->accepted.pop_front(); } }
    for (size_t i = 0; i < accepted.size(); i++) forward_open(accepted[i].first, accepted[i].second);
    if (!pending_forward_wait_.empty() && !device_ip_.empty() && !ip.empty()) {
        std::vector<PendFwd> w; w.swap(pending_forward_wait_);
        for (size_t i = 0; i < w.size(); i++) forward_open(w[i].sock, w[i].dport);
    }
    std::vector<DnsResult> dns;
    std::vector<ConnectResult> connects;
    std::vector<std::pair<UdpKey, Bytes> > udp_in_q;
    { ScopedLock lk(io_->lock);
      dns.assign(io_->dns_results.begin(), io_->dns_results.end()); io_->dns_results.clear();
      connects.assign(io_->connect_results.begin(), io_->connect_results.end()); io_->connect_results.clear();
      udp_in_q.assign(io_->udp_in.begin(), io_->udp_in.end()); io_->udp_in.clear(); }
    for (size_t i = 0; i < dns.size(); i++) {
        std::map<int, DnsKey>::iterator p = dns_pending_.find(dns[i].id);
        if (p == dns_pending_.end()) continue;
        DnsKey key = p->second;
        dns_pending_.erase(p);
        if (dns[i].ok) dns_cache_[key.name] = dns[i].ip;
        log(strf("[NET] DNS %s -> %s", key.name.c_str(), dns[i].ok ? ip2str(dns[i].ip).c_str() : "NXDOMAIN"));
        dns_reply(key, dns[i].ip, dns[i].ok);
    }
    for (size_t i = 0; i < connects.size(); i++) {
        TcpConn *conn = 0;
        for (std::map<ConnKey, TcpConn *>::iterator it = conns_.begin(); it != conns_.end(); ++it)
            if (it->second->id == connects[i].conn_id) { conn = it->second; break; }
        bool ok = connects[i].sock != BK_BAD_SOCK;
        if (!conn || conn->state != "CONNECTING") { sock_close(connects[i].sock); continue; }
        if (ok) {
            conn->sock = connects[i].sock;
            io_->attach(conn->sock, conn);
            conn->state = "SYN_RECEIVED";
            conn->send(TCP_SYN | TCP_ACK, Bytes(), Bytes("\x02\x04", 2) + be16(MSS));
            conn->snd_nxt += 1;
        } else {
            log(strf("[NET] TCP %s: remote refused/unreachable", conn->log_prefix.c_str()));
            Bytes pkt = tcp_segment(conn->lip, conn->rip, conn->lport, conn->rport, 0, conn->rcv_nxt, TCP_RST | TCP_ACK, 0);
            send_ip(pkt, conn->rip);
            conn->state = "CLOSED";
        }
    }
    for (size_t i = 0; i < udp_in_q.size(); i++) {
        const UdpKey &flow = udp_in_q[i].first;
        if (!device_ip_.empty())
            send_ip(udp_packet(flow.dst_ip, device_ip_, flow.dst_port, flow.dev_port, udp_in_q[i].second), device_ip_);
    }
    std::vector<ConnKey> dead;
    for (std::map<ConnKey, TcpConn *>::iterator it = conns_.begin(); it != conns_.end(); ++it) {
        it->second->service(now_us);
        if (it->second->state == "CLOSED") { it->second->close_host(); dead.push_back(it->first); }
    }
    for (size_t i = 0; i < dead.size(); i++) { std::map<ConnKey, TcpConn *>::iterator it = conns_.find(dead[i]); if (it != conns_.end()) { TcpConn *c = it->second; conns_.erase(it); retire(c); } }
}

#include "netstack_selftests.h"

#endif
