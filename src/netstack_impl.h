/* Method bodies for netstack.cpp (HostIO, TcpConn, NetStack). Included once at
 * the end of netstack.cpp; not a standalone header. */
#ifndef BK_NETSTACK_IMPL_H
#define BK_NETSTACK_IMPL_H

namespace netstack {

/* =============================================================== HostIO */
static void hostio_thread(void *arg);

/* The shared mailbox between a HostIO and its worker threads. A worker may
 * still be inside a 10-second connect when the stack closes, so it never
 * touches the HostIO directly: it posts through this, under `m`, and only
 * while `io` is still set. The last of the HostIO and its workers frees it. */
struct HostSink {
    Mutex m;
    HostIO *io;
    int refs;
};
static void sink_release(HostSink *s) {
    bool last;
    { ScopedLock lk(s->m); last = --s->refs == 0; }
    if (last) delete s;
}

/* One blocking job (a DNS lookup or an outbound connect) on its own thread,
 * like Python's per-request daemon threads. The Thread object must outlive the
 * OS thread, so finished jobs are reaped later from a process-wide list. */
struct HostJob {
    Thread th;
    HostSink *sink;
    bool is_connect;
    std::string name;   /* resolve: the host name; connect: the dotted ip */
    int port;
    int dns_id;
    u64 conn_id;
    long generation;
};
static Mutex &host_jobs_lock() { static Mutex m; return m; }
static std::vector<HostJob *> &host_jobs() { static std::vector<HostJob *> v; return v; }
static void reap_host_jobs() {
    ScopedLock lk(host_jobs_lock());
    std::vector<HostJob *> &v = host_jobs();
    for (size_t i = 0; i < v.size();) {
        if (!v[i]->th.is_alive()) { delete v[i]; v.erase(v.begin() + i); }
        else i++;
    }
}
static void host_job_run(void *arg) {
    HostJob *job = (HostJob *)arg;
    sock_t s = BK_BAD_SOCK;
    bool ok = false; u8 ip[4] = {0, 0, 0, 0};
    if (job->is_connect) s = tcp_connect(job->name, job->port, 10000);
    else ok = resolve_host(job->name, ip);
    bool posted = false;
    {
        ScopedLock lk(job->sink->m);
        HostIO *io = job->sink->io;
        if (io) {
            ScopedLock lk2(io->lock);
            if (!atomic_get(&io->stop_) && io->generation == job->generation) {
                if (job->is_connect) { ConnectResult r; r.conn_id = job->conn_id; r.sock = s; io->connect_results.push_back(r); }
                else { DnsResult r; r.id = job->dns_id; r.ok = ok; r.ip = Bytes((const char *)ip, 4); io->dns_results.push_back(r); }
                posted = true;
            }
        }
    }
    if (!posted && s != BK_BAD_SOCK) sock_close(s);
    sink_release(job->sink);
}
static void start_host_job(HostIO *io, HostJob *job) {
    reap_host_jobs();
    if (!io->sink) { delete job; return; }   /* the stack has been closed */
    { ScopedLock lk(io->sink->m); io->sink->refs++; }
    job->sink = io->sink;
    { ScopedLock lk(io->lock); job->generation = io->generation; }
    ScopedLock lk(host_jobs_lock());
    host_jobs().push_back(job);
    if (!job->th.start(host_job_run, job)) { host_jobs().pop_back(); sink_release(job->sink); delete job; }
}

HostIO::HostIO() : generation(0), stop_(0) {
    sink = new HostSink();
    sink->io = this;
    sink->refs = 1;
    thread.start(hostio_thread, this);
}
HostIO::~HostIO() { shutdown(); }

sock_t HostIO::listen(int lport, int dport, std::string *err) {
    sock_t s = tcp_listen("127.0.0.1", lport, err);
    if (s == BK_BAD_SOCK) return s;
    { ScopedLock lk(lock); listeners[s] = dport; }
    return s;
}
void HostIO::resolve(const std::string &name, int id) {
    HostJob *job = new HostJob();
    job->is_connect = false; job->name = name; job->port = 0; job->dns_id = id; job->conn_id = 0;
    start_host_job(this, job);
}
void HostIO::connect(u64 conn_id, const std::string &ip, int port) {
    HostJob *job = new HostJob();
    job->is_connect = true; job->name = ip; job->port = port; job->dns_id = 0; job->conn_id = conn_id;
    start_host_job(this, job);
}
void HostIO::attach(sock_t s, TcpConn *conn) {
    /* An invalid handle is never serviced (Python's select() refuses a
     * fileno of -1 the same way), so a caller can hand over an inert socket. */
    if (s == BK_BAD_SOCK) return;
    sock_set_nonblocking(s);
    ScopedLock lk(lock);
    socks[s] = conn;
}
sock_t HostIO::udp_socket_for(const UdpKey &flow) {
    sock_t s = udp_socket();
    if (s == BK_BAD_SOCK) return s;
    ScopedLock lk(lock);
    udp_socks[s] = flow;
    return s;
}
void HostIO::close_sock(sock_t s) {
    { ScopedLock lk(lock); socks.erase(s); udp_socks.erase(s); }
    sock_close(s);
}
void HostIO::handoff(sock_t s, std::deque<Bytes> &buf) {
    ScopedLock lk(lock);
    socks.erase(s);
    drains[s] = buf;   /* HostIO flushes these on its thread, then closes */
    buf.clear();
}
void HostIO::reset_connections() {
    std::vector<sock_t> toclose;
    { ScopedLock lk(lock);
      generation++;
      for (std::map<sock_t, TcpConn *>::iterator it = socks.begin(); it != socks.end(); ++it) toclose.push_back(it->first);
      for (std::map<sock_t, UdpKey>::iterator it = udp_socks.begin(); it != udp_socks.end(); ++it) toclose.push_back(it->first);
      for (size_t i = 0; i < accepted.size(); i++) toclose.push_back(accepted[i].first);
      for (std::map<sock_t, std::deque<Bytes> >::iterator it = drains.begin(); it != drains.end(); ++it) toclose.push_back(it->first);
      for (size_t i = 0; i < connect_results.size(); i++) toclose.push_back(connect_results[i].sock);
      socks.clear(); udp_socks.clear(); accepted.clear(); drains.clear();
      dns_results.clear(); connect_results.clear(); udp_in.clear(); }
    for (size_t i = 0; i < toclose.size(); i++) sock_close(toclose[i]);
}
void HostIO::shutdown() {
    if (atomic_get(&stop_)) return;
    atomic_set(&stop_, 1);
    if (!thread.is_current()) thread.join(1000);
    /* detach from any worker still running; it closes what it was holding */
    { ScopedLock lk(sink->m); sink->io = 0; }
    sink_release(sink);
    sink = 0;
    std::vector<sock_t> toclose;
    { ScopedLock lk(lock);
      for (std::map<sock_t, int>::iterator it = listeners.begin(); it != listeners.end(); ++it) toclose.push_back(it->first);
      for (std::map<sock_t, TcpConn *>::iterator it = socks.begin(); it != socks.end(); ++it) toclose.push_back(it->first);
      for (std::map<sock_t, UdpKey>::iterator it = udp_socks.begin(); it != udp_socks.end(); ++it) toclose.push_back(it->first);
      for (size_t i = 0; i < accepted.size(); i++) toclose.push_back(accepted[i].first);
      for (std::map<sock_t, std::deque<Bytes> >::iterator it = drains.begin(); it != drains.end(); ++it) toclose.push_back(it->first);
      for (size_t i = 0; i < connect_results.size(); i++) toclose.push_back(connect_results[i].sock);
      listeners.clear(); socks.clear(); udp_socks.clear(); accepted.clear(); drains.clear();
      dns_results.clear(); connect_results.clear(); udp_in.clear(); }
    for (size_t i = 0; i < toclose.size(); i++) sock_close(toclose[i]);
}
void HostIO::run() {
    while (!atomic_get(&stop_)) {
        std::vector<sock_t> rs, ws;
        { ScopedLock lk(lock);
          for (std::map<sock_t, TcpConn *>::iterator it = socks.begin(); it != socks.end(); ++it) rs.push_back(it->first);
          for (std::map<sock_t, int>::iterator it = listeners.begin(); it != listeners.end(); ++it) rs.push_back(it->first);
          for (std::map<sock_t, UdpKey>::iterator it = udp_socks.begin(); it != udp_socks.end(); ++it) rs.push_back(it->first);
          for (std::map<sock_t, TcpConn *>::iterator it = socks.begin(); it != socks.end(); ++it) {
              TcpConn *c = it->second;
              if (!c->to_host.empty() || c->shutdown_pending || c->close_pending) ws.push_back(it->first);
          }
          for (std::map<sock_t, std::deque<Bytes> >::iterator it = drains.begin(); it != drains.end(); ++it) ws.push_back(it->first);
        }
        /* flush-and-close sockets handed off by a closing connection */
        {
            std::vector<sock_t> drain_socks;
            { ScopedLock lk(lock); for (std::map<sock_t, std::deque<Bytes> >::iterator it = drains.begin(); it != drains.end(); ++it) drain_socks.push_back(it->first); }
            for (size_t i = 0; i < drain_socks.size(); i++) {
                sock_t s = drain_socks[i];
                bool empty = false; Bytes chunk; bool have = false;
                { ScopedLock lk(lock);
                  std::map<sock_t, std::deque<Bytes> >::iterator it = drains.find(s);
                  if (it == drains.end()) continue;
                  if (it->second.empty()) empty = true; else { chunk = it->second.front(); have = true; } }
                if (empty) { sock_shutdown_wr(s); sock_close(s); ScopedLock lk(lock); drains.erase(s); continue; }
                if (have) {
                    int n = sock_send(s, chunk.data(), (int)chunk.size());
                    ScopedLock lk(lock);
                    std::map<sock_t, std::deque<Bytes> >::iterator it = drains.find(s);
                    if (it == drains.end()) continue;
                    if (n < 0) { it->second.clear(); }
                    else { it->second.pop_front(); if (n < (int)chunk.size()) it->second.push_front(chunk.substr(n)); }
                }
            }
        }
        if (rs.empty() && ws.empty()) { sleep_ms(50); continue; }
        std::vector<sock_t> rr, wr;
        if (!sock_select(rs, ws, rr, wr, 50)) { sleep_ms(50); continue; }
        for (size_t i = 0; i < rr.size(); i++) {
            sock_t s = rr[i];
            TcpConn *conn; int dport = -1; bool is_listener = false, is_udp = false; UdpKey flow;
            { ScopedLock lk(lock);
              std::map<sock_t, TcpConn *>::iterator ci = socks.find(s);
              conn = ci == socks.end() ? 0 : ci->second;
              std::map<sock_t, int>::iterator li = listeners.find(s);
              if (li != listeners.end()) { is_listener = true; dport = li->second; }
              std::map<sock_t, UdpKey>::iterator ui = udp_socks.find(s);
              if (ui != udp_socks.end()) { is_udp = true; flow = ui->second; } }
            if (is_listener) {
                sock_t c = tcp_accept(s);
                if (c != BK_BAD_SOCK) { ScopedLock lk(lock); accepted.push_back(std::make_pair(c, dport)); }
            } else if (is_udp) {
                char ubuf[65536];
                u8 from_ip[4]; int from_port = 0;
                int n = sock_recvfrom(s, ubuf, sizeof(ubuf), from_ip, &from_port);
                if (n >= 0) { ScopedLock lk(lock); udp_in.push_back(std::make_pair(flow, Bytes(ubuf, n))); }
            } else if (conn) {
                char buf[65536];
                int n = sock_recv(s, buf, sizeof(buf));
                if (n == -1) continue;               /* would block */
                ScopedLock lk(lock);
                if (socks.find(s) == socks.end()) continue;   /* closed meanwhile */
                if (n > 0) conn->from_host.push_back(Bytes(buf, n));
                else { conn->host_closed = true; socks.erase(s); }   /* EOF or error */
            }
        }
        for (size_t i = 0; i < wr.size(); i++) {
            sock_t s = wr[i];
            TcpConn *conn;
            { ScopedLock lk(lock); std::map<sock_t, TcpConn *>::iterator ci = socks.find(s); conn = ci == socks.end() ? 0 : ci->second; }
            if (!conn) continue;
            Bytes chunk; bool have = false;
            { ScopedLock lk(lock); if (!conn->to_host.empty()) { chunk = conn->to_host.front(); conn->to_host.pop_front(); have = true; } }
            if (!have) {
                if (conn->close_pending) { conn->close_pending = false; close_sock(s); }
                else if (conn->shutdown_pending) { conn->shutdown_pending = false; sock_shutdown_wr(s); }
                continue;
            }
            int n = sock_send(s, chunk.data(), (int)chunk.size());
            if (n == -1) { ScopedLock lk(lock); conn->to_host.push_front(chunk); }   /* would block: retry */
            else if (n < 0) { ScopedLock lk(lock); conn->host_closed = true; socks.erase(s); }
            else if (n < (int)chunk.size()) { ScopedLock lk(lock); conn->to_host.push_front(chunk.substr(n)); }
        }
    }
}
static void hostio_thread(void *arg) { ((HostIO *)arg)->run(); }

/* =============================================================== TcpConn */
TcpConn::TcpConn(NetStack *st, const Bytes &lip_, int lport_, const Bytes &rip_, int rport_, const std::string &kind_)
    : stack(st), id(0), lip(lip_), rip(rip_), lport(lport_), rport(rport_), kind(kind_), state("CLOSED"),
      rcv_nxt(0), peer_window(4096), unacked_seq(0), last_send_us(0), last_activity_us(0), opened_us(0),
      described(false), answered(false), tls(0), fin_sent(false), fin_rcvd(false), close_after_flush(false),
      sock(BK_BAD_SOCK), host_closed(false), shutdown_pending(false), close_pending(false), syn_tries(0) {
    static u64 next_id = 1;
    id = next_id++;
    snd_una = snd_nxt = 0x10000000 + py_tuple2_hash20((u64)lport_, (u64)rport_);
    log_prefix = strf("%s:%d <-> %s:%d", ip2str(rip_).c_str(), rport_, ip2str(lip_).c_str(), lport_);
}
TcpConn::~TcpConn() { delete tls; }

void TcpConn::send(u8 flags, const Bytes &payload, const Bytes &options) {
    Bytes pkt = tcp_segment(lip, rip, lport, rport, snd_nxt, rcv_nxt, flags, RCV_WINDOW, payload, options);
    stack->send_ip(pkt, rip);
}
void TcpConn::pump(u64 now_us) {
    if (state == "SYN_SENT") {
        if (elapsed_gt(now_us, last_send_us, RETRANSMIT_US)) {
            if (syn_tries >= SYN_MAX_TRIES) {
                stack->log(strf("[NET] TCP %s: no SYN-ACK after %d tries, giving up", log_prefix.c_str(), syn_tries));
                state = "CLOSED"; close_host(); return;
            }
            syn_tries++;
            snd_nxt = (snd_nxt - 1) & 0xFFFFFFFF;
            send(TCP_SYN, Bytes(), Bytes("\x02\x04", 2) + be16(MSS));
            snd_nxt = (snd_nxt + 1) & 0xFFFFFFFF;
            last_send_us = now_us;
        }
        return;
    }
    if (state != "ESTABLISHED" && state != "CLOSE_WAIT") return;
    if (!unacked.empty() && elapsed_gt(now_us, last_send_us, RETRANSMIT_US)) {
        u32 seq_save = snd_nxt;
        snd_nxt = unacked_seq;
        for (size_t i = 0; i < unacked.size(); i += MSS) {
            Bytes chunk = unacked.substr(i, MSS);
            send(TCP_ACK | TCP_PSH, chunk);
            snd_nxt += (u32)chunk.size();
        }
        snd_nxt = seq_save;
        last_send_us = now_us;
    }
    int room = (peer_window < 8 * MSS ? peer_window : 8 * MSS) - (int)unacked.size();
    while (!send_buf.empty() && room > 0) {
        int take = (int)send_buf.size() < (MSS < room ? MSS : room) ? (int)send_buf.size() : (MSS < room ? MSS : room);
        Bytes chunk = send_buf.substr(0, take);
        send_buf = send_buf.substr(take);
        if (unacked.empty()) unacked_seq = snd_nxt;
        send(TCP_ACK | TCP_PSH, chunk);
        snd_nxt += (u32)chunk.size();
        unacked += chunk;
        room -= (int)chunk.size();
        last_send_us = now_us;
    }
    if (close_after_flush && send_buf.empty() && unacked.empty() && !fin_sent) {
        send(TCP_FIN | TCP_ACK);
        snd_nxt += 1;
        fin_sent = true;
        state = fin_rcvd ? "LAST_ACK" : "FIN_WAIT_1";
    }
}
void TcpConn::on_segment(u32 seq, u32 ack, u8 flags, int window, const Bytes &payload, u64 now_us) {
    last_activity_us = now_us;
    peer_window = window ? window : 1;
    if (flags & TCP_RST) {
        stack->log(strf("[NET] TCP %s reset by device", log_prefix.c_str()));
        close_host(); state = "CLOSED"; return;
    }
    if (flags & TCP_ACK) {
        u32 acked = (ack - unacked_seq) & 0xFFFFFFFF;
        if (!unacked.empty() && acked > 0 && acked <= unacked.size()) { unacked = unacked.substr(acked); unacked_seq = ack; }
        if (state == "SYN_SENT" && (flags & TCP_SYN)) {
            rcv_nxt = (seq + 1) & 0xFFFFFFFF; snd_una = ack; state = "ESTABLISHED"; send(TCP_ACK);
            stack->log(strf("[NET] TCP %s established (%s)", log_prefix.c_str(), kind.c_str()));
            return;
        }
        if (state == "SYN_RECEIVED") state = "ESTABLISHED";
        if (state == "FIN_WAIT_1" && ack == snd_nxt) state = "FIN_WAIT_2";
        if (state == "LAST_ACK" && ack == snd_nxt) { state = "CLOSED"; close_host(); return; }
    }
    if (!payload.empty()) {
        if (seq == rcv_nxt) { rcv_nxt = (rcv_nxt + (u32)payload.size()) & 0xFFFFFFFF; deliver(payload, now_us); }
        send(TCP_ACK);
    }
    u32 fin_seq = (seq + (u32)payload.size()) & 0xFFFFFFFF;
    if ((flags & TCP_FIN) && fin_seq == rcv_nxt && !fin_rcvd) {
        fin_rcvd = true;
        rcv_nxt = (rcv_nxt + 1) & 0xFFFFFFFF;
        if (state == "FIN_WAIT_1" || state == "FIN_WAIT_2") { send(TCP_ACK); state = "CLOSED"; close_host(); }
        else {
            state = "CLOSE_WAIT"; send(TCP_ACK);
            if (kind == "forward" && sock != BK_BAD_SOCK) shutdown_pending = true;
            close_after_flush = true;
        }
    }
}
void TcpConn::deliver(const Bytes &data, u64 now_us) {
    if (kind == "client") { if (captured.size() < 65536) captured += data; return; }
    if (kind == "capture") {
        if ((int)captured.size() < CAPTURE_LIMIT) captured += data;
        if (!described && captured.size() >= 5) {
            described = true;
            stack->log(strf("[NET] TCP %s sends: %s", log_prefix.c_str(), describe_payload(captured).c_str()));
        }
        rx_buf += data;
        if (proto.empty() && rx_buf.size() >= 5) {
            if (!mqtt_connect_info(rx_buf).empty()) proto = "mqtt";
            else {
                size_t sp = rx_buf.find(" ");
                Bytes m = sp == std::string::npos ? rx_buf : rx_buf.substr(0, sp);
                if (m == "GET" || m == "POST" || m == "PUT" || m == "HEAD" || m == "DELETE" || m == "OPTIONS" || m == "PATCH") proto = "http";
                else if ((u8)rx_buf[0] == 0x16 && (u8)rx_buf[1] == 0x03 && ((u8)rx_buf[2] == 0x01 || (u8)rx_buf[2] == 0x02 || (u8)rx_buf[2] == 0x03)) proto = "tls";
                else proto = "raw";
            }
        }
        if (proto == "mqtt") mqtt_serve(false);
        else if (proto == "http") http_serve();
        else if (proto == "tls") tls_serve();
    } else {
        ScopedLock lk(stack->io_->lock);
        to_host.push_back(data);
    }
}
void TcpConn::http_serve() {
    if (answered) return;
    size_t sep = rx_buf.find("\r\n\r\n");
    if (sep == std::string::npos) return;
    Bytes head = rx_buf.substr(0, sep), body = rx_buf.substr(sep + 4);
    int length = 0;
    StrList lines = split(head, "\r\n");
    for (size_t i = 1; i < lines.size(); i++) {
        if (lower(lines[i]).substr(0, 15) == "content-length:") { long long n = 0; if (parse_int(strip(lines[i].substr(15)), 10, n)) length = (int)n; }
    }
    if ((int)body.size() < length) return;
    answered = true;
    std::string request = lines.empty() ? "" : lines[0];
    Bytes reply = Bytes("BekenEmulator offline network: ") + request + "\n";
    queue(Bytes("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ") + strf("%d", (int)reply.size())
          + "\r\nConnection: close\r\n\r\n" + reply);
    close_after_flush = true;
    stack->log(strf("[NET] HTTP %s answered 200 (offline)", latin1_to_utf8(request).c_str()));
}
void TcpConn::mqtt_send(const Bytes &payload, bool over_tls) {
    if (over_tls) { if (!tls || !tls->established()) return; queue(tls->encrypt_records(0x17, payload)); }
    else queue(payload);
}
void TcpConn::mqtt_serve(bool over_tls) {
    for (;;) {
        Bytes &buf = over_tls ? tls_plain_buf : rx_buf;
        if (buf.size() < 2) return;
        int n = 0, mult = 1; size_t p = 1;
        for (;;) {
            if (p >= buf.size() || p > 4) return;
            u8 b = (u8)buf[p];
            n += (b & 0x7F) * mult; mult *= 128; p++;
            if (!(b & 0x80)) break;
        }
        if (buf.size() < p + n) return;
        int ptype = (u8)buf[0] >> 4, flags = (u8)buf[0] & 0x0F;
        Bytes body = buf.substr(p, n);
        buf = buf.substr(p + n);
        if (ptype == 1) {
            mqtt_send(Bytes("\x20\x02\x00\x00", 4), over_tls);
            stack->log(strf("[NET] MQTT %s connected to the offline broker", log_prefix.c_str()));
        } else if (ptype == 3) {
            /* Python slices never raise: a short body yields short fields */
            int qos = (flags >> 1) & 3;
            Bytes tl = bhead(body, 2);
            long long tlen = 0; for (size_t i = 0; i < tl.size(); i++) tlen = (tlen << 8) | (u8)tl[i];
            std::string topic = utf8_replace(bslice(body, 2, 2 + tlen));
            long long q = 2 + tlen;
            Bytes pid = qos ? bslice(body, q, q + 2) : Bytes();
            Bytes payload = bfrom(body, q + (qos ? 2 : 0));
            std::string text = utf8_replace(payload);
            if (utf8_length(text) > 120) text = utf8_prefix(text, 120) + " ...";
            stack->log(strf("[NET] MQTT PUBLISH %s = %s", topic.c_str(), text.c_str()));
            stack->mqtt_publishes_.push_back(std::make_pair(topic, payload));
            if (qos == 1) mqtt_send(Bytes("\x40\x02", 2) + pid, over_tls);
            else if (qos == 2) mqtt_send(Bytes("\x50\x02", 2) + pid, over_tls);
        } else if (ptype == 6) {
            mqtt_send(Bytes("\x70\x02", 2) + bhead(body, 2), over_tls);
        } else if (ptype == 8) {
            Bytes pid = bhead(body, 2); size_t q = 2; Bytes granted; StrList topics;
            while (q + 2 <= body.size()) {
                size_t tlen = rdbe16(body, q);
                topics.push_back(utf8_replace(bslice(body, (long long)q + 2, (long long)(q + 2 + tlen))));
                int g = (q + 2 + tlen < body.size()) ? (u8)body[q + 2 + tlen] : 0;
                granted += (char)(g < 1 ? g : 1);
                q += 3 + tlen;
            }
            mqtt_send(Bytes(1, (char)0x90) + (char)(2 + (int)granted.size()) + pid + granted, over_tls);
            std::string joined; for (size_t i = 0; i < topics.size(); i++) { if (i) joined += ", "; joined += topics[i]; }
            stack->log(strf("[NET] MQTT SUBSCRIBE %s", joined.c_str()));
            for (size_t i = 0; i < topics.size(); i++) subscribed.push_back(topics[i]);
            mqtt_deliver(over_tls);
        } else if (ptype == 10) {
            mqtt_send(Bytes("\xb0\x02", 2) + bhead(body, 2), over_tls);
        } else if (ptype == 12) {
            mqtt_send(Bytes("\xd0\x00", 2), over_tls);
        } else if (ptype == 14) {
            close_after_flush = true; return;
        }
    }
}
void TcpConn::mqtt_deliver(bool over_tls) {
    std::vector<std::pair<std::string, Bytes> > &pubs = stack->mqtt_pubs_;
    for (size_t i = 0; i < pubs.size();) {
        std::string topic = pubs[i].first; Bytes payload = pubs[i].second;
        bool match = false;
        for (size_t j = 0; j < subscribed.size(); j++) if (mqtt_topic_matches(subscribed[j], topic)) { match = true; break; }
        if (!match) { i++; continue; }
        pubs.erase(pubs.begin() + i);
        mqtt_send(mqtt_publish_packet(Bytes(topic), payload), over_tls);
        stack->log(strf("[NET] MQTT -> device PUBLISH %s = %s", topic.c_str(), utf8_replace(payload).c_str()));
    }
}
void TcpConn::tls_note_identity(const Bytes &ident) {
    /* credential-adjacent: kept for inspection, only its shape is logged */
    stack->log(strf("[NET] TLS %s pre-shared key identity: %d bytes", log_prefix.c_str(), (int)ident.size()));
    stack->tls_identities_.push_back(std::make_pair(log_prefix, ident));
}
void TcpConn::tls_fail(const std::string &reason) {
    proto = "raw";
    stack->log(strf("[NET] TLS %s %s", log_prefix.c_str(), reason.c_str()));
}
Bytes TcpConn::tls_server_hello(u16 suite, const std::map<u16, Bytes> &client_exts) {
    std::string seed = strf("%s|%d", log_prefix.c_str(), suite);
    std::string rseed(seed.rbegin(), seed.rend());
    Bytes rnd = (sha256(Bytes(seed)) + sha256(Bytes(rseed))).substr(0, 32);
    Bytes ext;
    u16 echo[3] = {0x0001, 0x0016, 0x0017};
    for (int i = 0; i < 3; i++) {
        u16 et = echo[i];
        std::map<u16, Bytes>::const_iterator it = client_exts.find(et);
        if (it == client_exts.end()) continue;
        Bytes val = (et == 0x0001) ? it->second : Bytes();
        if (et == 0x0001 && (val.size() != 1 || !((u8)val[0] >= 1 && (u8)val[0] <= 4))) continue;
        if ((et == 0x0016 || et == 0x0017) && !it->second.empty()) continue;
        ext += be16(et) + be16((u16)val.size()) + val;
    }
    Bytes body = be16(0x0303) + rnd + Bytes(1, '\0') + be16(suite) + Bytes(1, '\0');
    if (!ext.empty()) body += be16((u16)ext.size()) + ext;
    Bytes hello = Bytes(1, (char)0x02) + be32((u32)body.size()).substr(1) + body;
    Bytes done = Bytes("\x0e\x00\x00\x00", 4);
    Bytes payload = hello + done;
    return Bytes("\x16\x03\x03", 3) + be16((u16)payload.size()) + payload;
}
void TcpConn::tls_serve() {
    for (;;) {
        Bytes &buf = rx_buf;
        if (buf.size() < 5) return;
        u8 b0 = (u8)buf[0];
        if (b0 != 0x16 && b0 != 0x14 && b0 != 0x15 && b0 != 0x17) { proto = "raw"; return; }
        int n = rdbe16(buf, 3);
        bool encrypted_epoch = tls && (tls->state == "client_finished" || tls->state == "established");
        int limit = encrypted_epoch ? TLS_MAX_CIPHERTEXT : TLS_MAX_PLAINTEXT;
        if (n > limit) { tls_fail("record exceeds the TLS length limit"); return; }
        if ((int)buf.size() < 5 + n) return;
        Bytes rec = buf.substr(0, 5 + n); rx_buf = buf.substr(5 + n);
        u8 content_type = (u8)rec[0]; Bytes version = rec.substr(1, 2); Bytes fragment = rec.substr(5);

        if (tls && tls->state == "client_finished") {
            if (content_type != 0x16) { tls_fail("expected the encrypted client Finished"); return; }
            Bytes server_flight;
            if (!tls->finish(version, fragment, server_flight)) { tls_fail("client Finished authentication failed"); return; }
            queue(server_flight);
            stack->log(strf("[NET] TLS %s handshake authenticated; serving encrypted MQTT offline", log_prefix.c_str()));
            continue;
        }
        if (tls && tls->established()) {
            Bytes plain;
            if (!tls->decrypt_record(content_type, version, fragment, plain)) { tls_fail("encrypted record authentication failed"); return; }
            if (content_type == 0x17) {
                tls_plain_buf += plain;
                if (tls_app_proto.empty()) {
                    if (!mqtt_connect_info(tls_plain_buf).empty()) tls_app_proto = "mqtt";
                    else if (tls_plain_buf.size() >= 12) { tls_app_proto = "raw"; stack->log(strf("[NET] TLS %s decrypted non-MQTT application data", log_prefix.c_str())); }
                }
                if (tls_app_proto == "mqtt") mqtt_serve(true);
            } else if (content_type == 0x15) {
                stack->log(strf("[NET] TLS %s received an authenticated alert", log_prefix.c_str()));
                close_after_flush = true;
            }
            continue;
        }
        if (content_type == 0x16) {
            tls_handshake_buf += fragment;
            while (tls_handshake_buf.size() >= 4) {
                u32 msg_len = ((u32)(u8)tls_handshake_buf[1] << 16) | ((u8)tls_handshake_buf[2] << 8) | (u8)tls_handshake_buf[3];
                if (msg_len > 0x10000) { tls_fail("oversized clear handshake message"); return; }
                if (tls_handshake_buf.size() < 4 + msg_len) break;
                Bytes msg = tls_handshake_buf.substr(0, 4 + msg_len);
                tls_handshake_buf = tls_handshake_buf.substr(4 + msg_len);
                if ((u8)msg[0] == 0x01 && !answered) {
                    Bytes hello_rec = Bytes("\x16", 1) + version + be16((u16)msg.size()) + msg;
                    ClientHello info = tls_client_hello(hello_rec);
                    if (!info.ok) { tls_fail("malformed ClientHello"); return; }
                    answered = true;
                    std::string named;
                    for (size_t i = 0; i < info.suites.size(); i++) { if (i) named += ", "; named += tls_suite_name(info.suites[i]); }
                    stack->log(strf("[NET] TLS %s client hello: version %s, %d cipher suites (%s)%s",
                                    log_prefix.c_str(), info.version.c_str(), (int)info.suites.size(), named.c_str(),
                                    info.sni.empty() ? ", no server name" : (", server name " + info.sni).c_str()));
                    if (info.psk.empty()) { stack->log(strf("[NET] TLS %s offers no pre-shared-key suite, so nothing further can be read", log_prefix.c_str())); return; }
                    std::map<u16, Bytes>::const_iterator emsIt = info.extensions.find(TLS_EXT_EXTENDED_MASTER_SECRET);
                    bool ems = emsIt != info.extensions.end() && emsIt->second.empty();
                    std::map<u16, Bytes>::const_iterator mflIt = info.extensions.find(0x0001);
                    int mfl = 0;
                    if (mflIt != info.extensions.end() && mflIt->second.size() == 1) {
                        int c = (u8)mflIt->second[0];
                        mfl = c == 1 ? 512 : c == 2 ? 1024 : c == 3 ? 2048 : c == 4 ? 4096 : 0;
                    }
                    bool has_gcm = false; for (size_t i = 0; i < info.psk.size(); i++) if (info.psk[i] == TLS_PSK_AES_128_GCM_SHA256) has_gcm = true;
                    bool can_complete = !stack->tls_psks_.empty() && info.version == "1.2" && has_gcm && ems;
                    u16 suite = can_complete ? TLS_PSK_AES_128_GCM_SHA256 : info.psk[0];
                    Bytes reply = tls_server_hello(suite, info.extensions);
                    if (can_complete) {
                        tls = new TLS12PSKGCMServer(stack->tls_psks_, info.random, reply.substr(11, 32),
                                                    info.handshake, reply.substr(5), mfl ? mfl : TLS_MAX_PLAINTEXT,
                                                    stack->tuya_legacy_identity_psk_);
                        if (!tls->ok) { delete tls; tls = 0; tls_fail("TLS credential setup failed"); return; }
                    }
                    queue(reply);
                    stack->log(strf("[NET] TLS %s answered with %s, waiting for the key identity", log_prefix.c_str(), tls_suite_name(suite).c_str()));
                } else if ((u8)msg[0] == 0x10) {
                    Bytes ident;
                    if (tls) { if (!tls->add_client_key_exchange(msg, ident)) { tls_fail("malformed ClientKeyExchange"); return; } }
                    else {
                        Bytes synthetic = Bytes("\x16", 1) + version + be16((u16)msg.size()) + msg;
                        if (!tls_psk_identity(synthetic, ident)) { tls_fail("malformed ClientKeyExchange"); return; }
                    }
                    tls_note_identity(ident);
                } else if (tls) { tls_fail("unexpected clear handshake message"); return; }
            }
            continue;
        }
        if (content_type == 0x14) {
            if (tls) {
                if (!tls->change_cipher_spec(fragment)) { tls_fail("unexpected ChangeCipherSpec"); return; }
                stack->log(strf("[NET] TLS %s switched to encryption (ChangeCipherSpec); authenticating the client Finished", log_prefix.c_str()));
                continue;
            }
            proto = "raw";
            stack->log(strf("[NET] TLS %s switched to encryption (ChangeCipherSpec); everything after this needs the device's own secret to read", log_prefix.c_str()));
            return;
        }
        if (content_type == 0x15) { proto = "raw"; stack->log(strf("[NET] TLS %s alert, the device gave up on the handshake", log_prefix.c_str())); return; }
        if (content_type == 0x17) { proto = "raw"; stack->log(strf("[NET] TLS %s is encrypted from here on (%d bytes), which needs the device's own secret to read", log_prefix.c_str(), (int)fragment.size())); return; }
    }
}
void TcpConn::close_host() {
    if (sock != BK_BAD_SOCK) {
        bool unsent;
        { ScopedLock lk(stack->io_->lock); unsent = !to_host.empty(); }
        if (unsent) {
            /* Hand the socket + remaining bytes to HostIO so it can flush and
             * close them on its own thread; this conn may be deleted right
             * after, so HostIO must not keep a pointer to it. */
            stack->io_->handoff(sock, to_host);
        } else {
            stack->io_->close_sock(sock);
        }
        sock = BK_BAD_SOCK;
    }
}
void TcpConn::service(u64 now_us) {
    if (kind == "relay" || kind == "forward") {
        std::vector<Bytes> chunks; bool hc;
        { ScopedLock lk(stack->io_->lock);
          while (!from_host.empty()) { chunks.push_back(from_host.front()); from_host.pop_front(); }
          hc = host_closed; }
        for (size_t i = 0; i < chunks.size(); i++) queue(chunks[i]);
        if (hc && !close_after_flush) close_after_flush = true;
    } else if (kind == "capture") {
        bool mqtt_session = (proto == "mqtt") || (proto == "tls" && tls && tls->established() && tls_app_proto == "mqtt");
        if ((state == "ESTABLISHED" || state == "CLOSE_WAIT") && !close_after_flush && !mqtt_session
            && elapsed_gt(now_us, last_activity_us, CAPTURE_IDLE_US)) {
            if (!captured.empty() && !described) { described = true; stack->log(strf("[NET] TCP %s sends: %s", log_prefix.c_str(), describe_payload(captured).c_str())); }
            close_after_flush = true;
        }
    } else if (kind == "client") {
        bool idle = elapsed_gt(now_us, last_activity_us, CAPTURE_IDLE_US);
        if (!described && (fin_rcvd || (!captured.empty() && idle))) {
            described = true; stack->report_http(this, captured); close_after_flush = true;
        } else if (!described && idle && elapsed_gt(now_us, opened_us, 30000000)) {
            described = true; stack->log(strf("[NET] HTTP GET %s: no reply from the device", request_path.c_str())); close_after_flush = true;
        }
    }
    pump(now_us);
}

#include "netstack_stack.h"

} /* namespace netstack */

#endif
