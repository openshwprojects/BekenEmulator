#include "tuyamcu.h"

#include <stdio.h>
#include <stdlib.h>

namespace tuyamcu {

u8 checksum(const Bytes &data)
{
    u32 s = 0;
    for (size_t i = 0; i < data.size(); i++)
        s += (u8)data[i];
    return (u8)(s & 0xFF);
}

Bytes build_frame(u8 cmd, const Bytes &payload, u8 version)
{
    Bytes body;
    body += (char)0x55;
    body += (char)0xAA;
    body += (char)version;
    body += (char)cmd;
    body += (char)((payload.size() >> 8) & 0xFF);
    body += (char)(payload.size() & 0xFF);
    body += payload;
    body += (char)checksum(body);
    return body;
}

Bytes encode_dp(int dp_id, u8 dp_type, long long value, const std::string &svalue)
{
    Bytes data;
    if (dp_type == DP_VALUE) {
        /* 4-byte big-endian, signed */
        u32 v = (u32)(s32)value;
        data = p32be(v);
    } else if (dp_type == DP_BOOL || dp_type == DP_ENUM) {
        data += (char)(u8)(value & 0xFF);
    } else if (dp_type == DP_BITMAP) {
        long long v = value;
        int n = v <= 0xFF ? 1 : (v <= 0xFFFF ? 2 : 4);
        for (int i = n - 1; i >= 0; i--)
            data += (char)(u8)((v >> (8 * i)) & 0xFF);
    } else if (dp_type == DP_STRING) {
        data = svalue;
    }
    Bytes out;
    out += (char)(u8)(dp_id & 0xFF);
    out += (char)(u8)(dp_type & 0xFF);
    out += (char)((data.size() >> 8) & 0xFF);
    out += (char)(data.size() & 0xFF);
    out += data;
    return out;
}

std::vector<Frame> Parser::feed(const Bytes &data)
{
    buf_ += data;
    std::vector<Frame> frames;
    const std::string header("\x55\xAA", 2);
    for (;;) {
        size_t i = buf_.find(header);
        if (i == std::string::npos) {
            if (!buf_.empty() && (u8)buf_[buf_.size() - 1] == 0x55)
                buf_ = buf_.substr(buf_.size() - 1);
            else
                buf_.clear();
            break;
        }
        if (i > 0)
            buf_ = buf_.substr(i);
        if (buf_.size() < 6)
            break;
        u32 ln = ((u8)buf_[4] << 8) | (u8)buf_[5];
        size_t total = 6 + ln + 1;
        if (buf_.size() < total)
            break;
        Bytes raw = buf_.substr(0, total);
        buf_ = buf_.substr(total);
        bool ok = checksum(raw.substr(0, raw.size() - 1)) == (u8)raw[raw.size() - 1];
        frames.push_back(Frame((u8)raw[2], (u8)raw[3], raw.substr(6, ln), ok));
    }
    return frames;
}

Slave::Slave(const std::string &product_key, const std::string &version, bool raw_product)
    : heartbeats(0), handshake_done(false), injected(0), product_key_(product_key),
      version_(version), raw_product_(raw_product)
{
}

void Slave::set_dp(int id, u8 type, long long ivalue, const std::string &svalue)
{
    DpSpec s;
    s.type = type;
    s.ivalue = ivalue;
    s.svalue = svalue;
    dps_[id] = s;
}

void Slave::add_inject(const Bytes &blob) { injects_.push_back(blob); }

Bytes Slave::raw_product_payload() const
{
    Bytes pid = bytes_n(16, 0);
    for (size_t i = 0; i < product_key_.size() && i < 16; i++)
        pid[i] = product_key_[i];
    Bytes ver = version_.substr(0, 8);
    return pid + ver;
}

std::vector<Bytes> Slave::reply(const Frame &frame)
{
    std::vector<Bytes> out;
    if (!frame.valid)
        return out;
    seen.push_back(frame.cmd);
    u8 c = frame.cmd;
    if (c == CMD_HEARTBEAT) {
        u8 state = heartbeats == 0 ? 0x00 : 0x01;
        heartbeats++;
        out.push_back(build_frame(CMD_HEARTBEAT, Bytes(1, (char)state)));
        return out;
    }
    if (c == CMD_QUERY_PRODUCT) {
        if (raw_product_) {
            out.push_back(build_frame(CMD_QUERY_PRODUCT, raw_product_payload()));
        } else {
            std::string js = strf("{\"p\":\"%s\",\"v\":\"%s\"}", product_key_.c_str(), version_.c_str());
            out.push_back(build_frame(CMD_QUERY_PRODUCT, js));
        }
        return out;
    }
    if (c == CMD_MCU_CONF) {
        out.push_back(build_frame(CMD_MCU_CONF, Bytes()));
        return out;
    }
    if (c == CMD_QUERY_STATE) {
        handshake_done = true;
        for (std::map<int, DpSpec>::iterator it = dps_.begin(); it != dps_.end(); ++it)
            out.push_back(build_frame(CMD_STATE, encode_dp(it->first, it->second.type, it->second.ivalue, it->second.svalue)));
        return out;
    }
    return out;
}

std::vector<Bytes> Slave::react(const Frame &frame)
{
    bool was_done = handshake_done;
    std::vector<Bytes> out = reply(frame);
    if (was_done && !injects_.empty()) {
        for (size_t i = 0; i < injects_.size(); i++)
            out.push_back(injects_[i]);
        injected += (int)injects_.size();
        injects_.clear();
    }
    return out;
}

Bytes Slave::react_stream(const Bytes &data)
{
    Bytes out;
    std::vector<Frame> frames = parser_.feed(data);
    for (size_t i = 0; i < frames.size(); i++) {
        std::vector<Bytes> r = react(frames[i]);
        for (size_t j = 0; j < r.size(); j++)
            out += r[j];
    }
    return out;
}

static void ts_by(Bytes &b, const Bytes &s) { b += p32le((u32)s.size()); b += s; }
static u32 tg_u32(const Bytes &b, size_t &p) { u32 v = g32le(b, p); p += 4; return v; }
static Bytes tg_by(const Bytes &b, size_t &p) { u32 n = tg_u32(b, p); Bytes s = b.substr(p, n); p += n; return s; }

void Slave::serialize(Bytes &b) const
{
    ts_by(b, product_key_);
    ts_by(b, version_);
    b += (char)(raw_product_ ? 1 : 0);
    b += p32le((u32)dps_.size());
    for (std::map<int, DpSpec>::const_iterator it = dps_.begin(); it != dps_.end(); ++it) {
        b += p32le((u32)it->first);
        b += (char)it->second.type;
        b += p64le((u64)it->second.ivalue);
        ts_by(b, it->second.svalue);
    }
    ts_by(b, parser_.pending());
    b += p32le((u32)heartbeats);
    b += p32le((u32)seen.size());
    for (size_t i = 0; i < seen.size(); i++)
        b += (char)seen[i];
    b += p32le((u32)injects_.size());
    for (size_t i = 0; i < injects_.size(); i++)
        ts_by(b, injects_[i]);
    b += (char)(handshake_done ? 1 : 0);
    b += p32le((u32)injected);
}

Slave *Slave::deserialize(const Bytes &b, size_t &p)
{
    std::string key = tg_by(b, p);
    std::string ver = tg_by(b, p);
    bool raw = b[p++] != 0;
    Slave *s = new Slave(key, ver, raw);
    u32 n = tg_u32(b, p);
    for (u32 i = 0; i < n; i++) {
        int id = (int)tg_u32(b, p);
        DpSpec d;
        d.type = (u8)b[p++];
        d.ivalue = (long long)g64le(b, p);
        p += 8;
        d.svalue = tg_by(b, p);
        s->dps_[id] = d;
    }
    s->parser_.set_pending(tg_by(b, p));
    s->heartbeats = (int)tg_u32(b, p);
    n = tg_u32(b, p);
    for (u32 i = 0; i < n; i++)
        s->seen.push_back((u8)b[p++]);
    n = tg_u32(b, p);
    for (u32 i = 0; i < n; i++)
        s->injects_.push_back(tg_by(b, p));
    s->handshake_done = b[p++] != 0;
    s->injected = (int)tg_u32(b, p);
    return s;
}

/* ------------------------------------------------------------- self-tests */
static int g_checks;
static void ck(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
    g_checks++;
    printf("  [PASS] %s\n", msg);
}

int tuyamcu_selftests()
{
    g_checks = 0;
    printf("== TuyaMCU codec ==\n");
    /* The real heartbeat the firmware emits, captured from an emulated dump. */
    Bytes HB = H("55aa00000000ff");
    ck(build_frame(CMD_HEARTBEAT) == HB, "build heartbeat == captured 55 AA 00 00 00 00 FF");
    ck(checksum(bhead(HB, (long long)HB.size() - 1)) == (u8)HB[HB.size() - 1], "captured heartbeat checksum verifies");

    {
        Parser p;
        std::vector<Frame> fr = p.feed(HB);
        ck(fr.size() == 1 && fr[0].cmd == CMD_HEARTBEAT && fr[0].valid, "parse captured heartbeat -> one valid cmd 0x00 frame");
    }
    {
        struct { u8 cmd; Bytes pl; } cases[] = {
            {CMD_HEARTBEAT, Bytes()}, {CMD_QUERY_PRODUCT, Bytes()}, {CMD_MCU_CONF, Bytes()},
            {CMD_STATE, H("0101000101")}, {CMD_QUERY_STATE, Bytes()}, {CMD_SET_TIME, bytes_n(8)}};
        for (int i = 0; i < 6; i++) {
            Bytes raw = build_frame(cases[i].cmd, cases[i].pl);
            Parser pp;
            std::vector<Frame> got = pp.feed(raw);
            ck(got.size() == 1 && got[0].cmd == cases[i].cmd && got[0].payload == cases[i].pl && got[0].valid,
               strf("round-trip cmd 0x%02X, %d-byte payload", cases[i].cmd, (int)cases[i].pl.size()).c_str());
        }
    }
    {
        Bytes bad = build_frame(CMD_QUERY_PRODUCT, "x");
        bad[bad.size() - 1] = (char)((u8)bad[bad.size() - 1] ^ 0xFF);
        Parser pp;
        std::vector<Frame> fr = pp.feed(bad);
        ck(fr.size() == 1 && !fr[0].valid, "bad checksum -> Frame.valid == False");
    }
    {
        Parser p;
        Bytes whole = build_frame(CMD_QUERY_STATE, H("AABB"));
        bool a = p.feed(bhead(whole, 4)).empty();
        bool b = p.feed(bfrom(whole, 4)).size() == 1;
        ck(a && b, "frame split across two feeds reassembles");
    }
    {
        Parser p;
        Bytes in = Bytes("\x00\xffgarbage", 9) + build_frame(CMD_HEARTBEAT) + build_frame(CMD_QUERY_PRODUCT);
        std::vector<Frame> fr = p.feed(in);
        ck(fr.size() == 2 && fr[0].cmd == CMD_HEARTBEAT && fr[1].cmd == CMD_QUERY_PRODUCT,
           "junk skipped; two concatenated frames both parse");
    }
    ck(encode_dp(1, DP_BOOL, 1) == H("0101000101"), "encode_dp bool -> dp1 type01 len0001 data01");
    ck(encode_dp(2, DP_VALUE, 40) == H("0202000400000028"), "encode_dp value 40 -> 4-byte big-endian 0x00000028");

    printf("== simulated MCU reactions to the module's startup queries ==\n");
    {
        Slave mcu("keyabcdefgh12345", "1.0.0");
        mcu.set_dp(1, DP_BOOL, 1);
        mcu.set_dp(2, DP_VALUE, 40);
        /* one(cmd): react to a module frame, then parse the joined responses. */
#define ONE(cmd, pl) Parser().feed(join_bytes(mcu.react(Frame(0, (cmd), (pl)))))
        {
            /* heartbeat -> a valid 0x00 ACK; first payload 0x00 then 0x01. */
            std::vector<Frame> f = ONE(CMD_HEARTBEAT, Bytes());
            ck(f.size() == 1 && f[0].cmd == CMD_HEARTBEAT && f[0].valid && f[0].payload == Bytes(1, '\0'),
               "heartbeat -> valid ACK, first payload 0x00");
        }
        {
            std::vector<Frame> f = ONE(CMD_HEARTBEAT, Bytes());
            ck(!f.empty() && f[0].payload == Bytes(1, (char)0x01), "second heartbeat -> payload 0x01 (running)");
        }
        {
            /* product query -> 0x01 with the JSON product record. */
            std::vector<Frame> f = ONE(CMD_QUERY_PRODUCT, Bytes());
            ck(f.size() == 1 && f[0].cmd == CMD_QUERY_PRODUCT && f[0].valid
                   && contains(f[0].payload, "\"p\":\"keyabcdefgh12345\"")
                   && contains(f[0].payload, "\"v\":\"1.0.0\""),
               "query-product -> valid 0x01 JSON with product key and version");
        }
        {
            /* a slave told upfront to use the raw form (TuyaOS 3.x) answers 0x01
             * with the 16-byte id + short version, 16..24 bytes total. */
            Slave rawmcu("keyabcdefgh12345", "1.0.0", true);
            std::vector<Frame> rf = Parser().feed(join_bytes(rawmcu.react(Frame(0, CMD_QUERY_PRODUCT, Bytes()))));
            ck(rf.size() == 1 && rf[0].cmd == CMD_QUERY_PRODUCT && rf[0].valid
                   && rf[0].payload.size() >= 16 && rf[0].payload.size() <= 24
                   && bhead(rf[0].payload, 16) == Bytes("keyabcdefgh12345") && bfrom(rf[0].payload, 16) == Bytes("1.0.0"),
               "raw_product slave -> raw 16-byte id + version (16..24 bytes)");
        }
        {
            /* working-mode query -> 0x02 (empty payload here). */
            std::vector<Frame> f = ONE(CMD_MCU_CONF, Bytes());
            ck(f.size() == 1 && f[0].cmd == CMD_MCU_CONF && f[0].valid && f[0].payload.empty(),
               "query-working-mode -> valid empty 0x02");
        }
        {
            /* query-state -> one 0x07 report per configured DP, each a valid frame. */
            std::vector<Frame> f = ONE(CMD_QUERY_STATE, Bytes());
            bool all_ok = true;
            for (size_t i = 0; i < f.size(); i++)
                all_ok = all_ok && f[i].cmd == CMD_STATE && f[i].valid;
            ck(f.size() == 2 && all_ok, "query-state -> two valid 0x07 DP reports");
            ck(f[0].payload == encode_dp(1, DP_BOOL, 1) && f[1].payload == encode_dp(2, DP_VALUE, 40),
               "0x07 reports carry the configured DP values, dp-id ordered");
        }
#undef ONE
        /* a Wi-Fi-state report from the module needs no answer. */
        ck(mcu.react(Frame(0, CMD_WIFI_STATE, Bytes(1, (char)0x04))).empty(), "module Wi-Fi-state report -> no response");
        /* an invalid frame is ignored. */
        ck(mcu.react(Frame(0, CMD_HEARTBEAT, Bytes(), false)).empty(), "invalid frame -> no response");
    }
    {
        /* the full module startup sequence, driven through react_stream in order. */
        Slave mcu2;
        Bytes stream = build_frame(CMD_HEARTBEAT) + build_frame(CMD_QUERY_PRODUCT)
                       + build_frame(CMD_MCU_CONF) + build_frame(CMD_QUERY_STATE);
        Bytes out = mcu2.react_stream(stream);
        std::vector<Frame> outf = Parser().feed(out);
        bool all_valid = true;
        for (size_t i = 0; i < outf.size(); i++)
            all_valid = all_valid && outf[i].valid;
        ck(outf.size() == 3 && outf[0].cmd == CMD_HEARTBEAT && outf[1].cmd == CMD_QUERY_PRODUCT
               && outf[2].cmd == CMD_MCU_CONF && all_valid,
           "full startup sequence -> ACK, product, working-mode (no DPs configured)");
        ck(mcu2.seen.size() == 4 && mcu2.seen[0] == CMD_HEARTBEAT && mcu2.seen[1] == CMD_QUERY_PRODUCT
               && mcu2.seen[2] == CMD_MCU_CONF && mcu2.seen[3] == CMD_QUERY_STATE,
           "slave recorded every module command it reacted to, in order");
    }

    printf("== MCU-initiated (injected) traffic ==\n");
    {
        Bytes ACK_BOOT = build_frame(CMD_HEARTBEAT, Bytes(1, '\0'));
        Bytes ACK_RUN = build_frame(CMD_HEARTBEAT, Bytes(1, (char)0x01));
        Bytes ASK_TIME = build_frame(CMD_SET_TIME);
        Bytes JUNK = H("DEADBEEF");
        Slave mcu3;
        mcu3.add_inject(ASK_TIME);
        mcu3.add_inject(JUNK);
        ck(mcu3.react_stream(HB) == ACK_BOOT && mcu3.injected == 0, "no inject before the handshake (heartbeat)");
        mcu3.react_stream(build_frame(CMD_QUERY_PRODUCT) + build_frame(CMD_MCU_CONF));
        ck(mcu3.injected == 0 && !mcu3.handshake_done, "still no inject after product / working-mode");
        ck(mcu3.react_stream(build_frame(CMD_QUERY_STATE)) == Bytes() && mcu3.handshake_done && mcu3.injected == 0,
           "0x08 completes the handshake but does not itself carry the injects");
        ck(mcu3.react_stream(HB) == ACK_RUN + ASK_TIME + JUNK && mcu3.injected == 2,
           "next module frame carries ACK + every inject, in the given order");
        ck(mcu3.react_stream(HB) == ACK_RUN && mcu3.injected == 2, "injects are sent once, not repeated on later frames");

        Bytes bad_hb = HB;
        bad_hb[bad_hb.size() - 1] = (char)((u8)bad_hb[bad_hb.size() - 1] ^ 0xFF);
        Slave mcu4;
        mcu4.add_inject(bad_hb);
        mcu4.react_stream(build_frame(CMD_QUERY_STATE));
        ck(endswith(mcu4.react_stream(HB), bad_hb), "a deliberately corrupt inject is passed through byte for byte");

        Slave mcu5;
        std::vector<Bytes> r5 = mcu5.react(Frame(0, CMD_HEARTBEAT, Bytes()));
        ck(r5.size() == 1 && r5[0] == ACK_BOOT, "a slave with no injects behaves exactly as before");
    }

    printf("\nAll %d TuyaMCU self-tests passed.\n", g_checks);
    return g_checks;
}

} /* namespace tuyamcu */
