/* RivieraWaves nxmac Wi-Fi MAC hardware model (the 0xC0000000 block). Port of
 * src/wifimac.py. */
#ifndef BK_WIFIMAC_H
#define BK_WIFIMAC_H

#include "common.h"
#include "blecore.h" /* reuse MemAccess */

static const u32 WIFI_CORE = 0xC0000000;
static const u32 WIFI_PL = 0xC0008000;
static const u32 WIFI_SIZE = 0x9000;
static const u32 WIFI_LINE_MASK = 0x007F0000;

struct TxInfo {
    int queue;
    u64 insns;
    u32 frmlen;
    u32 policy;
    u32 statinfo;
};

class MacHw {
public:
    explicit MacHw(u64 insns_per_us);

    MemAccess mem;
    bool have_mem;
    /* on_tx callback: a peer registers to receive transmitted frames. */
    void (*on_tx)(void *ctx, const Bytes &frame, const TxInfo &info);
    void *on_tx_ctx;
    void (*log)(void *ctx, const std::string &msg);
    void *log_ctx;

    u32 lines; /* ICU level mask, bits 16..22 */
    /* WIFIDBG: register/TX/RX/timer trace on stderr ("WIFIDBG %10d ...") */
    bool debug;

    u64 now_us(u64 insns) const { return insns / insns_per_us_; }

    /* register access: read returns served value (has=true) else plain memory */
    u32 read(u32 address, u64 insns, bool &has);
    void write(u32 address, u32 value, u64 insns);
    u32 tick(u64 insns); /* returns the ICU level mask */
    void inject(const Bytes &frame, u64 insns, int rssi = -45, u32 rate = 0, int decr = -1, int keyidx = -1);
    bool busy() const { return !tx_inflight_.empty() || !rx_queue_.empty(); }
    void serialize(Bytes &out) const;   /* snapshot the MAC register/engine state */
    void deserialize(const Bytes &in, size_t &p);

private:
    friend int wifimac_selftests(); /* inspects the engine state, as the Python tests do */
    u64 insns_per_us_;
    u64 dbg_insns_;
    void dbg(const std::string &msg);
    std::map<u32, u32> regs_;
    int state_, next_state_;
    u32 gen_raw_, txrx_raw_, timers_pending_;
    bool timer_armed_[10];
    u32 timer_value_[10];
    u64 mono_offset_, tsf_offset_;
    struct KeyEnt {
        Bytes addr;
        Bytes key;
        int ctype, vlan, spp, usedef, clen;
    };
    std::map<int, KeyEnt> keys_;
    std::map<int, u32> tx_cur_, tx_last_;
    struct Inflight {
        u64 done_at_us;
        int queue;
        u32 thd_addr;
        bool want_irq;
    };
    std::vector<Inflight> tx_inflight_;
    u32 rx_cur_rhd_, rx_cur_rbd_, rx_last_rhd_, rx_last_rbd_;
    struct RxItem {
        Bytes frame;
        int rssi;
        u32 rate;
        int decr;
        int keyidx;
    };
    std::vector<RxItem> rx_queue_;
    int rx_dropped_;
    bool monitor_;
    bool last_tbtt_idx_valid_;
    u64 last_tbtt_idx_;

    u64 mono(u64 insns) const { return (now_us(insns) + mono_offset_) & 0xFFFFFFFF; }
    u64 tsf(u64 insns) const { return (now_us(insns) + tsf_offset_); }
    void set_tsf(u64 value, u64 insns) { tsf_offset_ = (value - now_us(insns)); }
    void logmsg(const std::string &m);
    u32 r32(u32 addr);
    void w32(u32 addr, u32 value);
    u32 update_lines();
    void key_ram(u32 value);
    void time_on_air(u32 param2);
    void dma_kick(u32 value, u64 insns);
    void tx_run(int q, u64 insns);
    bool tx_one(int q, u32 thd_addr, u64 insns);
    Bytes plaintext(const Bytes &data, int on_air, u32 mac2);
    void tx_done(int q, u32 thd_addr, u32 mac2, u64 insns);
    void tx_complete(u64 insns);
    void rx_flush(u64 insns);
    bool rx_one(const RxItem &item, u64 insns);
    int sta_key_index(const Bytes &addr2);
};

int wifimac_selftests();

#endif
