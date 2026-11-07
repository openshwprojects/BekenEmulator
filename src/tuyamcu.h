/* TuyaMCU serial-protocol codec and a simulated MCU peer. Port of src/tuyamcu.py. */
#ifndef BK_TUYAMCU_H
#define BK_TUYAMCU_H

#include "common.h"

namespace tuyamcu {

static const u8 CMD_HEARTBEAT = 0x00;
static const u8 CMD_QUERY_PRODUCT = 0x01;
static const u8 CMD_MCU_CONF = 0x02;
static const u8 CMD_WIFI_STATE = 0x03;
static const u8 CMD_SET_DP = 0x06;
static const u8 CMD_STATE = 0x07;
static const u8 CMD_QUERY_STATE = 0x08;
static const u8 CMD_SET_TIME = 0x1C;

static const u8 DP_BOOL = 0x01;
static const u8 DP_VALUE = 0x02;
static const u8 DP_STRING = 0x03;
static const u8 DP_ENUM = 0x04;
static const u8 DP_BITMAP = 0x05;

u8 checksum(const Bytes &data);
Bytes build_frame(u8 cmd, const Bytes &payload = Bytes(), u8 version = 0x00);
/* encode_dp: value is an int for BOOL/ENUM/BITMAP/VALUE, string for STRING. */
Bytes encode_dp(int dp_id, u8 dp_type, long long ivalue, const std::string &svalue = "");

struct Frame {
    u8 version;
    u8 cmd;
    Bytes payload;
    bool valid;
    Frame() : version(0), cmd(0), valid(true) {}
    Frame(u8 v, u8 c, const Bytes &p, bool vl = true) : version(v), cmd(c), payload(p), valid(vl) {}
    bool operator==(const Frame &o) const { return cmd == o.cmd && payload == o.payload && valid == o.valid; }
};

class Parser {
public:
    std::vector<Frame> feed(const Bytes &data);
    const Bytes &pending() const { return buf_; }
    void set_pending(const Bytes &b) { buf_ = b; }
private:
    Bytes buf_;
};

struct DpSpec {
    u8 type;
    long long ivalue;
    std::string svalue;
};

class Slave {
public:
    Slave(const std::string &product_key = "bekenemulator000", const std::string &version = "1.0.0",
          bool raw_product = false);
    void set_dp(int id, u8 type, long long ivalue, const std::string &svalue = "");
    void add_inject(const Bytes &blob);

    std::vector<Bytes> react(const Frame &frame);
    Bytes react_stream(const Bytes &data);

    /* Snapshot: the whole simulated MCU, as Python pickles it (configuration,
     * DP table, parser buffer, handshake/heartbeat progress, unsent injects). */
    void serialize(Bytes &out) const;
    static Slave *deserialize(const Bytes &in, size_t &p);

    int heartbeats;
    std::vector<u8> seen;
    bool handshake_done;
    int injected;

private:
    std::string product_key_;
    std::string version_;
    bool raw_product_;
    std::map<int, DpSpec> dps_;
    Parser parser_;
    std::vector<Bytes> injects_;
    Bytes raw_product_payload() const;
    std::vector<Bytes> reply(const Frame &frame);
};

int tuyamcu_selftests();

} /* namespace tuyamcu */

#endif
