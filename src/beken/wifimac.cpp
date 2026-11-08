/* RivieraWaves nxmac Wi-Fi MAC hardware model (the 0xC0000000 block).
 * Faithful port of src/wifimac.py. See that file's module docstring for what is
 * and is not modelled. Register offsets are from reg_mac_core.h / reg_mac_pl.h;
 * descriptor layouts from hal_desc.h. */
#include "beken/wifimac.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- core registers (offsets from CORE) */
static const u32 VERSION_1 = 0x004;
static const u32 MAC_ERR_REC_CNTRL = 0x054;
static const u32 RX_CNTRL = 0x060;
static const u32 BCN_CNTRL_1 = 0x064;
static const u32 ACCEPT_UNICAST = 0x00000040;
static const u32 ACCEPT_OTHER_BSSID = 0x00000010;
static const u32 STATE_CNTRL = 0x038;
static const u32 ENCR_KEY0 = 0x0AC, ENCR_KEY1 = 0x0B0, ENCR_KEY2 = 0x0B4, ENCR_KEY3 = 0x0B8;
static const u32 ENCR_MAC_ADDR_LOW = 0x0BC, ENCR_MAC_ADDR_HIGH = 0x0C0;
static const u32 ENCR_CNTRL = 0x0C4;
static const u32 ENCR_RAM_CONFIG = 0x0D8;
static const u32 MONOTONIC_COUNTER_1 = 0x11C;
static const u32 MONOTONIC_COUNTER_2_LO = 0x120;
static const u32 MONOTONIC_COUNTER_2_HI = 0x124;
static const u32 ABS_TIMER = 0x128;
static const int ABS_TIMER_CNT = 10;
/* ---- platform registers: stored keyed by (addr - CORE), i.e. REG + PL - CORE */
static const u32 PLo = WIFI_PL - WIFI_CORE; /* PL base as an offset from CORE */
static const u32 DOZE_CNTRL_2 = 0x048;
static const u32 MAC_CNTRL_2 = 0x050;
static const u32 GEN_INT_STATUS = 0x06C, GEN_INT_ACK = 0x070, GEN_INT_ENABLE = 0x074;
static const u32 TX_RX_INT_STATUS = 0x078, TX_RX_INT_ACK = 0x07C, TX_RX_INT_ENABLE = 0x080;
static const u32 TIMERS_INT_EVENT_SET = 0x084, TIMERS_INT_EVENT_CLEAR = 0x088, TIMERS_INT_UN_MASK = 0x08C;
static const u32 TSF_LO = 0x0A4, TSF_HI = 0x0A8;
static const u32 TIME_ON_AIR_PARAM_1 = 0x160, TIME_ON_AIR_PARAM_2 = 0x164, TIME_ON_AIR_VALUE = 0x168;
static const u32 DMA_CNTRL_SET = 0x180, DMA_CNTRL_CLEAR = 0x184;
static const u32 DMA_STATUS_1 = 0x188;
static const u32 RX_HEADER_HEAD_PTR = 0x1B8, RX_PAYLOAD_HEAD_PTR = 0x1BC;

/* queue -> TX head-ptr register (PL offset). queues: 4=BCN,0..3=AC */
static u32 tx_head_ptr(int q)
{
    switch (q) {
    case 4: return 0x198;
    case 0: return 0x19C;
    case 1: return 0x1A0;
    case 2: return 0x1A4;
    case 3: return 0x1A8;
    }
    return 0;
}

/* ---- bits */
static const int HW_IDLE = 0, HW_DOZE = 2, HW_ACTIVE = 3;
static const u32 MASTER_INT_EN = 1u << 31;
static const u32 IMP_PRI_TBTT = 1u << 0;
static const u32 IDLE_INTERRUPT = 1u << 2;
static const u32 ABS_GEN_TIMERS = 1u << 3;
static u32 ac_tx_trigger(int q) { return 1u << (6 + q); } /* 0..4 -> bits 6..10 */
static const u32 RX_TRIGGER = 1u << 16;
static const u32 TIMER_RX_TRIGGER = 1u << 17;
static const u32 COUNTER_RX_TRIGGER = 1u << 19;
static const u32 TIMER_TX_TRIGGER = 1u << 14;
static const int FIQ_MAC_TX_RX_TIMER = 17, FIQ_MAC_TX_RX_MISC = 18;
static const int FIQ_MAC_RX_TRIGGER = 19, FIQ_MAC_TX_TRIGGER = 20, FIQ_MAC_PROT_TRIGGER = 21, FIQ_MAC_GENERAL = 22;
/* DMA_CNTRL_SET */
static const u32 RX_PAYLOAD_NEW_HEAD = 1u << 27, RX_HEADER_NEW_HEAD = 1u << 26;
static u32 tx_new_head(int q) { switch (q) { case 4: return 1u<<8; case 0: return 1u<<9; case 1: return 1u<<10; case 2: return 1u<<11; case 3: return 1u<<12; } return 0; }
static u32 tx_new_tail(int q) { switch (q) { case 4: return 1u<<0; case 0: return 1u<<1; case 1: return 1u<<2; case 2: return 1u<<3; case 3: return 1u<<4; } return 0; }
/* key storage control */
static const u32 NEW_READ = 1u << 31, NEW_WRITE = 1u << 30, NEW_SEARCH = 1u << 29, SEARCH_ERROR = 1u << 28;
/* time on air */
static const u32 COMPUTE_DURATION = 1u << 31, TIME_ON_AIR_VALID = 1u << 30;
/* descriptors */
static const u32 TX_HEADER_DESC_PATTERN = 0xCAFEBABE;
static const u32 TX_PAYLOAD_DESC_PATTERN = 0xCAFEFADE;
static const u32 RX_HEADER_DESC_PATTERN = 0xBAADF00D;
static const u32 RX_PAYLOAD_DESC_PATTERN = 0xC0DEDBAD;
static const u32 INTERRUPT_EN_TX = 1u << 8;
static const u32 DONT_ENCRYPT = 1u << 30;
static const u32 FRAME_SUCCESSFUL_TX_BIT = 1u << 23;
static const u32 DESC_DONE_TX_BIT = 1u << 31;
static const u32 TBD_DONE_HW = 1u << 31;
static const u32 RX_PD_LASTBUF = 0x0001, RX_PD_DONE = 0x0002;
static const u32 RX_HD_DONE = 0x4000, RX_HD_SUCCESS = 0x2000, RX_HD_GA_FRAME = 0x0400;
static const u32 RX_HD_KEYIDV = 0x02000000; static const int RX_HD_KEYID_LSB = 15;
static const u32 MAC_FCS_LEN = 4;
static const u64 TU_US = 1024;
static const u32 THD_SIZE = 68;
static const u32 TX_AIR_TIME_US = 150;

/* legacy / HT rates for time-on-air, Mbit/s (as doubles) */
static const double LEGACY_RATE_MBPS[] = {1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54};
static const double HT_RATE_MBPS[] = {6.5, 13, 19.5, 26, 39, 52, 58.5, 65};

/* unpack n little-endian u32 words from a byte buffer */
static void unpack_words(const Bytes &b, u32 *out, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = ((u8)b[i * 4]) | ((u8)b[i * 4 + 1] << 8) | ((u8)b[i * 4 + 2] << 16) | ((u8)b[i * 4 + 3] << 24);
}
static Bytes pack_words(const u32 *w, int n)
{
    Bytes out((size_t)n * 4, '\0');
    for (int i = 0; i < n; i++) {
        out[i * 4] = (char)(w[i] & 0xFF);
        out[i * 4 + 1] = (char)((w[i] >> 8) & 0xFF);
        out[i * 4 + 2] = (char)((w[i] >> 16) & 0xFF);
        out[i * 4 + 3] = (char)((w[i] >> 24) & 0xFF);
    }
    return out;
}

struct RbdEnt { u32 addr; u32 b[5]; u32 take; };

struct FctlInfo { int ftype, subtype; u32 fctl; int hdr; bool ok; };
static FctlInfo fctl_info(const Bytes &frame)
{
    FctlInfo r; r.ok = false;
    if (frame.size() < 10)
        return r;
    u32 fctl = (u8)frame[0] | ((u8)frame[1] << 8);
    int ftype = (fctl >> 2) & 3;
    int subtype = (fctl >> 4) & 0xF;
    int hdr = 24;
    if (ftype == 1) {
        hdr = (subtype == 0x4 || subtype == 0x5 || subtype == 0x6 || subtype == 0x8
               || subtype == 0x9 || subtype == 0xA || subtype == 0xB) ? 16 : 10;
    } else if (ftype == 2) {
        if ((fctl & 0x0300) == 0x0300)
            hdr += 6;
        if (subtype & 0x8)
            hdr += 2;
        if (fctl & 0x8000)
            hdr += 4;
    } else if (ftype == 0 && (fctl & 0x8000)) {
        hdr += 4;
    }
    r.ftype = ftype; r.subtype = subtype; r.fctl = fctl; r.hdr = hdr; r.ok = true;
    return r;
}

MacHw::MacHw(u64 insns_per_us)
    : have_mem(false), on_tx(0), on_tx_ctx(0), log(0), log_ctx(0), lines(0), debug(env_has("WIFIDBG")),
      insns_per_us_(insns_per_us < 1 ? 1 : insns_per_us), dbg_insns_(0), state_(HW_IDLE), next_state_(HW_IDLE),
      gen_raw_(0), txrx_raw_(0), timers_pending_(0), mono_offset_(0), tsf_offset_(0),
      rx_cur_rhd_(0), rx_cur_rbd_(0), rx_last_rhd_(0), rx_last_rbd_(0), rx_dropped_(0),
      monitor_(false), last_tbtt_idx_valid_(false), last_tbtt_idx_(0)
{
    mem.ctx = 0; mem.read = 0; mem.write = 0;
    for (int i = 0; i < ABS_TIMER_CNT; i++) { timer_armed_[i] = false; timer_value_[i] = 0; }
    int qs[5] = {4, 0, 1, 2, 3};
    for (int i = 0; i < 5; i++) { tx_cur_[qs[i]] = 0; tx_last_[qs[i]] = 0; }
}

void MacHw::logmsg(const std::string &m) { if (log) log(log_ctx, m); }

void MacHw::dbg(const std::string &msg)
{
    fprintf(stderr, "WIFIDBG %10llu %s\n", (unsigned long long)dbg_insns_, msg.c_str());
    fflush(stderr);
}

u32 MacHw::r32(u32 addr)
{
    if (!have_mem) return 0;
    Bytes b = mem.read(mem.ctx, addr, 4);
    if (b.size() < 4) return 0;
    return ((u8)b[0]) | ((u8)b[1] << 8) | ((u8)b[2] << 16) | ((u8)b[3] << 24);
}
void MacHw::w32(u32 addr, u32 value)
{
    if (!have_mem) return;
    Bytes d(4, '\0');
    d[0] = (char)(value & 0xFF); d[1] = (char)((value >> 8) & 0xFF);
    d[2] = (char)((value >> 16) & 0xFF); d[3] = (char)((value >> 24) & 0xFF);
    mem.write(mem.ctx, addr, d);
}

u32 MacHw::update_lines()
{
    u32 ln = 0;
    u32 gen_en = regs_.count(GEN_INT_ENABLE + PLo) ? regs_[GEN_INT_ENABLE + PLo] : 0;
    if ((gen_en & MASTER_INT_EN) && (gen_raw_ & gen_en & 0x7FFFFFFF))
        ln |= 1u << FIQ_MAC_GENERAL;
    u32 txrx_en = regs_.count(TX_RX_INT_ENABLE + PLo) ? regs_[TX_RX_INT_ENABLE + PLo] : 0;
    if (txrx_en & MASTER_INT_EN) {
        u32 act = txrx_raw_ & txrx_en & 0x7FFFFFFF;
        /* TXRX_LINES table */
        if (act & 0x0000003F) ln |= 1u << FIQ_MAC_PROT_TRIGGER;
        if (act & 0x00009FC0) ln |= 1u << FIQ_MAC_TX_TRIGGER;
        if (act & RX_TRIGGER) ln |= 1u << FIQ_MAC_RX_TRIGGER;
        if (act & (TIMER_TX_TRIGGER | TIMER_RX_TRIGGER)) ln |= 1u << FIQ_MAC_TX_RX_TIMER;
        if (act & (0x1FFC2000u & ~(TIMER_RX_TRIGGER | RX_TRIGGER | TIMER_TX_TRIGGER))) ln |= 1u << FIQ_MAC_TX_RX_MISC;
    }
    lines = ln;
    return ln;
}

u32 MacHw::read(u32 address, u64 insns, bool &has)
{
    has = true;
    u32 off = address - WIFI_CORE;
    if (off == VERSION_1)
        return 0x00001C03;
    if (off == STATE_CNTRL)
        return (u32)((next_state_ << 4) | state_);
    if (off == MONOTONIC_COUNTER_1 || off == MONOTONIC_COUNTER_2_LO)
        return (u32)mono(insns);
    if (off == MONOTONIC_COUNTER_2_HI)
        return (u32)(((now_us(insns) + mono_offset_) >> 32) & 0xFFFF);
    if (off == ENCR_CNTRL)
        return (regs_.count(off) ? regs_[off] : 0) & ~(NEW_READ | NEW_WRITE | NEW_SEARCH);
    if (off == MAC_ERR_REC_CNTRL || off == MAC_CNTRL_2 + PLo)
        return regs_.count(off) ? regs_[off] : 0;
    u32 p = address - WIFI_PL;
    if (p == GEN_INT_STATUS)
        return gen_raw_ & (regs_.count(GEN_INT_ENABLE + PLo) ? regs_[GEN_INT_ENABLE + PLo] : 0) & 0x7FFFFFFF;
    if (p == TX_RX_INT_STATUS)
        return txrx_raw_ & (regs_.count(TX_RX_INT_ENABLE + PLo) ? regs_[TX_RX_INT_ENABLE + PLo] : 0) & 0x7FFFFFFF;
    if (p == TIMERS_INT_EVENT_SET || p == TIMERS_INT_EVENT_CLEAR)
        return timers_pending_;
    if (p == TSF_LO)
        return (u32)(tsf(insns) & 0xFFFFFFFF);
    if (p == TSF_HI)
        return (u32)((tsf(insns) >> 32) & 0xFFFFFFFF);
    if (p == TIME_ON_AIR_PARAM_2)
        return regs_.count(off) ? regs_[off] : 0;
    if (p == TIME_ON_AIR_VALUE)
        return (regs_.count(off) ? regs_[off] : 0) & ~COMPUTE_DURATION;
    if (p == DMA_STATUS_1)
        return 0;
    if (p == DMA_CNTRL_SET || p == DMA_CNTRL_CLEAR)
        return 0;
    has = false;
    return 0;
}

void MacHw::write(u32 address, u32 value, u64 insns)
{
    value &= 0xFFFFFFFF;
    u32 off = address - WIFI_CORE;
    regs_[off] = value;
    u32 p = address - WIFI_PL;
    dbg_insns_ = insns;
    if (debug && (p == GEN_INT_ACK || p == TX_RX_INT_ACK || p == GEN_INT_ENABLE || p == TX_RX_INT_ENABLE
                  || p == TIMERS_INT_EVENT_SET || p == TIMERS_INT_EVENT_CLEAR || p == TIMERS_INT_UN_MASK
                  || p == DMA_CNTRL_SET || off == STATE_CNTRL || off == 0x04C /* MAC_CNTRL_1 */
                  || off == RX_CNTRL || off == BCN_CNTRL_1))
        dbg(strf("W %08x <= %08x", address, value));
    if (off == STATE_CNTRL) {
        int nxt = (value >> 4) & 0xF;
        next_state_ = nxt;
        int prev = state_;
        state_ = nxt;
        if (nxt == HW_IDLE && prev != HW_IDLE)
            gen_raw_ |= IDLE_INTERRUPT;
        if (nxt == HW_ACTIVE) {
            if (prev != HW_DOZE)
                last_tbtt_idx_valid_ = false;
            if (!rx_queue_.empty())
                rx_flush(insns);
        }
        update_lines();
        return;
    }
    if (off == RX_CNTRL) {
        bool monitor = (value & (ACCEPT_UNICAST | ACCEPT_OTHER_BSSID)) == (ACCEPT_UNICAST | ACCEPT_OTHER_BSSID);
        if (monitor != monitor_) {
            monitor_ = monitor;
            logmsg(strf("[WIFI] device RX filter 0x%08x: %s", value,
                        monitor ? "monitor mode, every frame on air accepted (sniffing, as in EZ pairing)"
                                : "back to normal reception"));
        }
        return;
    }
    if (off == MAC_ERR_REC_CNTRL) { regs_[off] = value & ~0xFCu; return; }
    if (off == ENCR_CNTRL) { key_ram(value); return; }
    if (off == MONOTONIC_COUNTER_2_HI && (value & 0x80000000)) {
        u32 lo = regs_.count(MONOTONIC_COUNTER_2_LO) ? regs_[MONOTONIC_COUNTER_2_LO] : 0;
        u64 want = (((u64)(value & 0xFFFF)) << 32) | lo;
        mono_offset_ = want - now_us(insns);
        return;
    }
    if (ABS_TIMER <= off && off < ABS_TIMER + 4 * ABS_TIMER_CNT) {
        int i = (off - ABS_TIMER) >> 2;
        timer_value_[i] = value;
        bool ahead = ((value - (u32)mono(insns)) & 0xFFFFFFFF) < 0x80000000;
        timer_armed_[i] = ahead && value != (u32)mono(insns);
        if (debug)
            dbg(strf("abs_timer[%d] = %u (now %u) %s", i, value, (u32)mono(insns),
                     timer_armed_[i] ? "armed" : "past, not armed"));
        return;
    }
    if (p == GEN_INT_ACK) { gen_raw_ &= ~value; update_lines(); return; }
    if (p == TX_RX_INT_ACK) { txrx_raw_ &= ~value; update_lines(); return; }
    if (p == GEN_INT_ENABLE || p == TX_RX_INT_ENABLE || p == TIMERS_INT_UN_MASK) { update_lines(); return; }
    if (p == TIMERS_INT_EVENT_SET) {
        timers_pending_ |= value & 0x3FF;
        if (timers_pending_ & (regs_.count(TIMERS_INT_UN_MASK + PLo) ? regs_[TIMERS_INT_UN_MASK + PLo] : 0))
            gen_raw_ |= ABS_GEN_TIMERS;
        update_lines();
        return;
    }
    if (p == TIMERS_INT_EVENT_CLEAR) { timers_pending_ &= ~value; return; }
    if (p == TSF_LO) { set_tsf((tsf(insns) & ~(u64)0xFFFFFFFF) | value, insns); return; }
    if (p == TSF_HI) { set_tsf(((u64)value << 32) | (tsf(insns) & 0xFFFFFFFF), insns); return; }
    if (p == MAC_CNTRL_2) { regs_[off] = value & ~1u; return; }
    if (p == DOZE_CNTRL_2) { if ((value & 1) && state_ == HW_DOZE) { state_ = next_state_ = HW_ACTIVE; } return; }
    if (p == TIME_ON_AIR_VALUE) {
        if (value & COMPUTE_DURATION)
            time_on_air(regs_.count(TIME_ON_AIR_PARAM_2 + PLo) ? regs_[TIME_ON_AIR_PARAM_2 + PLo] : 0);
        return;
    }
    if (p == DMA_CNTRL_SET) { dma_kick(value, insns); return; }
}

void MacHw::key_ram(u32 value)
{
    int idx = (value >> 16) & 0x3FF;
    if (value & NEW_WRITE) {
        u32 lo = regs_.count(ENCR_MAC_ADDR_LOW) ? regs_[ENCR_MAC_ADDR_LOW] : 0;
        u32 hi = regs_.count(ENCR_MAC_ADDR_HIGH) ? regs_[ENCR_MAC_ADDR_HIGH] : 0;
        Bytes addr(6, '\0');
        addr[0] = (char)(lo & 0xFF); addr[1] = (char)((lo >> 8) & 0xFF);
        addr[2] = (char)((lo >> 16) & 0xFF); addr[3] = (char)((lo >> 24) & 0xFF);
        addr[4] = (char)(hi & 0xFF); addr[5] = (char)((hi >> 8) & 0xFF);
        KeyEnt e;
        e.addr = addr;
        u32 kk[4];
        kk[0] = regs_.count(ENCR_KEY0) ? regs_[ENCR_KEY0] : 0;
        kk[1] = regs_.count(ENCR_KEY1) ? regs_[ENCR_KEY1] : 0;
        kk[2] = regs_.count(ENCR_KEY2) ? regs_[ENCR_KEY2] : 0;
        kk[3] = regs_.count(ENCR_KEY3) ? regs_[ENCR_KEY3] : 0;
        e.key = pack_words(kk, 4);
        e.ctype = (value >> 8) & 7; e.vlan = (value >> 4) & 0xF;
        e.spp = (value >> 2) & 3; e.usedef = (value >> 1) & 1; e.clen = value & 1;
        keys_[idx] = e;
    } else if (value & NEW_READ) {
        std::map<int, KeyEnt>::iterator it = keys_.find(idx);
        if (it != keys_.end()) {
            KeyEnt &ent = it->second;
            u32 lo = (u8)ent.addr[0] | ((u8)ent.addr[1] << 8) | ((u8)ent.addr[2] << 16) | ((u8)ent.addr[3] << 24);
            u32 hi = (u8)ent.addr[4] | ((u8)ent.addr[5] << 8);
            regs_[ENCR_MAC_ADDR_LOW] = lo;
            regs_[ENCR_MAC_ADDR_HIGH] = hi;
            u32 ek[4] = {ENCR_KEY0, ENCR_KEY1, ENCR_KEY2, ENCR_KEY3};
            for (int i = 0; i < 4; i++) {
                u32 kv = (u8)ent.key[4*i] | ((u8)ent.key[4*i+1] << 8) | ((u8)ent.key[4*i+2] << 16) | ((u8)ent.key[4*i+3] << 24);
                regs_[ek[i]] = kv;
                w32(WIFI_CORE + ek[i], kv);
            }
            w32(WIFI_CORE + ENCR_MAC_ADDR_LOW, regs_[ENCR_MAC_ADDR_LOW]);
            w32(WIFI_CORE + ENCR_MAC_ADDR_HIGH, regs_[ENCR_MAC_ADDR_HIGH]);
            value = (value & ~0x7FFu) | (ent.ctype << 8) | (ent.vlan << 4) | (ent.spp << 2) | (ent.usedef << 1) | ent.clen;
        }
    } else if (value & NEW_SEARCH) {
        u32 lo = regs_.count(ENCR_MAC_ADDR_LOW) ? regs_[ENCR_MAC_ADDR_LOW] : 0;
        u32 hi = regs_.count(ENCR_MAC_ADDR_HIGH) ? regs_[ENCR_MAC_ADDR_HIGH] : 0;
        Bytes addr(6, '\0');
        addr[0] = (char)(lo & 0xFF); addr[1] = (char)((lo >> 8) & 0xFF);
        addr[2] = (char)((lo >> 16) & 0xFF); addr[3] = (char)((lo >> 24) & 0xFF);
        addr[4] = (char)(hi & 0xFF); addr[5] = (char)((hi >> 8) & 0xFF);
        int found = -1;
        for (std::map<int, KeyEnt>::iterator it = keys_.begin(); it != keys_.end(); ++it)
            if (it->second.addr == addr) { found = it->first; break; }
        if (found < 0)
            value |= SEARCH_ERROR;
        else
            value = (value & ~(SEARCH_ERROR | (0x3FFu << 16))) | ((u32)found << 16);
    }
    regs_[ENCR_CNTRL] = value & ~(NEW_READ | NEW_WRITE | NEW_SEARCH);
}

void MacHw::time_on_air(u32 param2)
{
    u32 p1 = regs_.count(TIME_ON_AIR_PARAM_1 + PLo) ? regs_[TIME_ON_AIR_PARAM_1 + PLo] : 0;
    u32 length = p1 & 0xFFFFF;
    u32 mcs = param2 & 0x7F;
    u32 pre_type = (p1 >> 24) & 7;
    double rate;
    int overhead;
    if (pre_type >= 2) {
        rate = HT_RATE_MBPS[mcs < 7 ? mcs : 7];
        overhead = 40;
    } else {
        int n = (int)(sizeof(LEGACY_RATE_MBPS) / sizeof(double));
        rate = LEGACY_RATE_MBPS[(int)mcs < n ? (int)mcs : n - 1];
        overhead = rate < 6 ? 192 : 20;
    }
    u32 dur = (u32)((int)(overhead + (length * 8) / rate)) & 0xFFFF;
    regs_[TIME_ON_AIR_VALUE + PLo] = dur | TIME_ON_AIR_VALID;
    w32(WIFI_PL + TIME_ON_AIR_VALUE, dur | TIME_ON_AIR_VALID);
}

void MacHw::dma_kick(u32 value, u64 insns)
{
    if (value & RX_HEADER_NEW_HEAD) {
        rx_cur_rhd_ = (regs_.count(RX_HEADER_HEAD_PTR + PLo) ? regs_[RX_HEADER_HEAD_PTR + PLo] : 0) & ~3u;
        rx_last_rhd_ = 0;
    }
    if (value & RX_PAYLOAD_NEW_HEAD) {
        rx_cur_rbd_ = (regs_.count(RX_PAYLOAD_HEAD_PTR + PLo) ? regs_[RX_PAYLOAD_HEAD_PTR + PLo] : 0) & ~3u;
        rx_last_rbd_ = 0;
    }
    int qs[5] = {4, 0, 1, 2, 3};
    for (int i = 0; i < 5; i++) {
        int q = qs[i];
        if (value & tx_new_head(q)) {
            tx_cur_[q] = regs_.count(tx_head_ptr(q) + PLo) ? regs_[tx_head_ptr(q) + PLo] : 0;
            tx_run(q, insns);
        }
    }
    for (int i = 0; i < 5; i++) {
        int q = qs[i];
        if (value & tx_new_tail(q)) {
            if (!tx_cur_[q] && tx_last_[q])
                tx_cur_[q] = r32(tx_last_[q] + 4);
            tx_run(q, insns);
        }
    }
    if (!rx_queue_.empty())
        rx_flush(insns);
}

void MacHw::tx_run(int q, u64 insns)
{
    int guard = 0;
    while (tx_cur_[q] && guard < 64) {
        guard++;
        u32 thd = tx_cur_[q];
        bool ok = tx_one(q, thd, insns);
        tx_last_[q] = thd;
        if (!ok) { tx_cur_[q] = 0; break; }
        tx_cur_[q] = r32(thd + 4);
    }
}

bool MacHw::tx_one(int q, u32 thd_addr, u64 insns)
{
    Bytes raw = mem.read(mem.ctx, thd_addr, THD_SIZE);
    if (raw.size() < THD_SIZE) return false;
    u32 f[17];
    unpack_words(raw, f, 17);
    if (f[0] != TX_HEADER_DESC_PATTERN) {
        logmsg(strf("[WIFI] THD %08x bad pattern %08x", thd_addr, f[0]));
        return false;
    }
    u32 nextmpdu = f[2], first_pbd = f[3], dstart = f[4], dend = f[5], frmlen = f[6];
    u32 pol = f[9], mac2 = f[14];
    if (nextmpdu) {
        u32 node = nextmpdu;
        int guard = 0;
        while (node && guard < 64) {
            guard++;
            tx_one(q, node, insns);
            node = r32(node + 8);
        }
        tx_done(q, thd_addr, mac2, insns);
        return true;
    }
    Bytes data;
    if (dstart && dend >= dstart && dend - dstart < 4096)
        data += mem.read(mem.ctx, dstart, dend - dstart + 1);
    u32 pbd = first_pbd;
    int guard = 0;
    while (pbd && guard < 32) {
        guard++;
        Bytes tb = mem.read(mem.ctx, pbd, 20);
        if (tb.size() < 20) break;
        u32 t[5];
        unpack_words(tb, t, 5);
        if (t[0] != TX_PAYLOAD_DESC_PATTERN) break;
        if (t[3] >= t[2] && t[3] - t[2] < 4096)
            data += mem.read(mem.ctx, t[2], t[3] - t[2] + 1);
        w32(pbd + 16, t[4] | TBD_DONE_HW);
        pbd = t[1];
    }
    if (data.empty())
        return false;
    u32 on_air = frmlen - MAC_FCS_LEN;
    Bytes frame = plaintext(data, (int)on_air, mac2);
    if (debug)
        dbg(strf("tx q%d thd %08x len %d frmlen %u: %s", q, thd_addr, (int)frame.size(), frmlen,
                 hexstr(bhead(frame, 32)).c_str()));
    if (on_tx) {
        TxInfo info; info.queue = q; info.insns = insns; info.frmlen = frmlen; info.policy = pol; info.statinfo = f[15];
        on_tx(on_tx_ctx, frame, info);
    }
    tx_done(q, thd_addr, mac2, insns);
    return true;
}

Bytes MacHw::plaintext(const Bytes &data, int on_air, u32 mac2)
{
    FctlInfo info = fctl_info(data);
    if (!info.ok)
        return data;
    int hdr = info.hdr;
    if (!(info.fctl & 0x4000) || (mac2 & DONT_ENCRYPT))
        return on_air > 0 ? data.substr(0, on_air) : data;
    if ((int)data.size() < hdr + 8)
        return data;
    Bytes body = data.substr(hdr + 8);
    if ((int)data.size() >= on_air && on_air - hdr - 8 - 8 >= 0)
        body = body.substr(0, on_air - hdr - 8 - 8);
    Bytes hdr_bytes = data.substr(0, hdr);
    hdr_bytes[1] = (char)((u8)hdr_bytes[1] & ~0x40);
    return hdr_bytes + body;
}

void MacHw::tx_done(int q, u32 thd_addr, u32 mac2, u64 insns)
{
    Inflight it;
    it.done_at_us = now_us(insns) + TX_AIR_TIME_US;
    it.queue = q; it.thd_addr = thd_addr; it.want_irq = (mac2 & INTERRUPT_EN_TX) != 0;
    tx_inflight_.push_back(it);
}

void MacHw::tx_complete(u64 insns)
{
    u64 now = now_us(insns);
    dbg_insns_ = insns;
    std::vector<Inflight> keep;
    bool changed = false;
    for (size_t i = 0; i < tx_inflight_.size(); i++) {
        Inflight &item = tx_inflight_[i];
        if (item.done_at_us > now) { keep.push_back(item); continue; }
        u32 stat = r32(item.thd_addr + 60);
        w32(item.thd_addr + 60, stat | DESC_DONE_TX_BIT | FRAME_SUCCESSFUL_TX_BIT);
        w32(item.thd_addr + 64, TX_AIR_TIME_US);
        if (item.want_irq) { txrx_raw_ |= ac_tx_trigger(item.queue); changed = true; }
        if (debug)
            dbg(strf("tx done q%d thd %08x irq=%d", item.queue, item.thd_addr, item.want_irq ? 1 : 0));
    }
    tx_inflight_ = keep;
    if (changed)
        update_lines();
}

void MacHw::inject(const Bytes &frame, u64 insns, int rssi, u32 rate, int decr, int keyidx)
{
    if (state_ == HW_DOZE && frame.size() >= 10 && !((u8)frame[4] & 1)) {
        RxItem it; it.frame = frame; it.rssi = rssi; it.rate = rate; it.decr = decr; it.keyidx = keyidx;
        rx_queue_.push_back(it);
        return;
    }
    if (state_ != HW_ACTIVE)
        return;
    RxItem it; it.frame = frame; it.rssi = rssi; it.rate = rate; it.decr = decr; it.keyidx = keyidx;
    rx_queue_.push_back(it);
    rx_flush(insns);
}

int MacHw::sta_key_index(const Bytes &addr2)
{
    bool have_cfg = regs_.count(ENCR_RAM_CONFIG) != 0;
    u32 cfg = have_cfg ? regs_[ENCR_RAM_CONFIG] : 0;
    int lo = 24, hi = 63;
    if (have_cfg && cfg) { lo = cfg & 0xFF; hi = (cfg >> 8) & 0xFF; }
    if (hi < lo) { lo = 24; hi = 63; }
    for (std::map<int, KeyEnt>::iterator it = keys_.begin(); it != keys_.end(); ++it)
        if (it->first >= lo && it->first <= hi && it->second.addr == addr2)
            return it->first;
    return -1;
}

void MacHw::rx_flush(u64 insns)
{
    if (state_ != HW_ACTIVE)
        return;
    bool changed = false;
    while (!rx_queue_.empty()) {
        if (!rx_one(rx_queue_[0], insns))
            break;
        rx_queue_.erase(rx_queue_.begin());
        changed = true;
    }
    if (rx_queue_.size() > 64) {
        rx_dropped_ += (int)rx_queue_.size() - 64;
        rx_queue_.erase(rx_queue_.begin(), rx_queue_.end() - 64);
    }
    if (changed) {
        txrx_raw_ |= RX_TRIGGER | TIMER_RX_TRIGGER | COUNTER_RX_TRIGGER;
        update_lines();
    }
}

bool MacHw::rx_one(const RxItem &item, u64 insns)
{
    const Bytes &frame = item.frame;
    u32 rhd = rx_cur_rhd_;
    if (!rhd && rx_last_rhd_) {
        rhd = rx_cur_rhd_ = r32(rx_last_rhd_ + 4);
    }
    if (!rhd)
        return false;
    Bytes hraw = mem.read(mem.ctx, rhd, 68);
    if (hraw.size() < 68) return false;
    u32 h[17];
    unpack_words(hraw, h, 17);
    if (h[0] != RX_HEADER_DESC_PATTERN)
        return false;
    if (h[16] & RX_HD_DONE)
        return false;
    const Bytes &payload = frame;
    /* gather RBDs */
    std::vector<RbdEnt> rbds;
    u32 rbd = rx_cur_rbd_;
    if (!rbd && rx_last_rbd_)
        rbd = r32(rx_last_rbd_ + 4);
    long need = (long)payload.size();
    while (need > 0 && rbd) {
        Bytes braw = mem.read(mem.ctx, rbd, 20);
        if (braw.size() < 20) return false;
        u32 b[5];
        unpack_words(braw, b, 5);
        if (b[0] != RX_PAYLOAD_DESC_PATTERN || b[3] < b[2])
            return false;
        u32 cap = b[3] - b[2] + 1;
        RbdEnt e; e.addr = rbd; for (int k = 0; k < 5; k++) e.b[k] = b[k];
        e.take = (u32)((long)cap < need ? (long)cap : need);
        rbds.push_back(e);
        need -= cap;
        rbd = b[1];
    }
    if (need > 0)
        return false;
    u32 pos = 0;
    for (size_t i = 0; i < rbds.size(); i++) {
        RbdEnt &e = rbds[i];
        mem.write(mem.ctx, e.b[2], payload.substr(pos, e.take));
        pos += e.take;
        u32 stat = RX_PD_DONE | (i == rbds.size() - 1 ? RX_PD_LASTBUF : 0);
        w32(e.addr + 16, (e.b[4] & 0xFFFF0000u) | stat);
    }
    rx_last_rbd_ = rbds.back().addr;
    rx_cur_rbd_ = rbds.back().b[1];
    bool group = frame.size() >= 10 && ((u8)frame[4] & 1);
    u32 statinfo = RX_HD_DONE | RX_HD_SUCCESS | (group ? RX_HD_GA_FRAME : 0);
    if (item.decr >= 0)
        statinfo |= (u32)item.decr;
    int keyidx = item.keyidx;
    if (keyidx < 0 && frame.size() >= 16)
        keyidx = sta_key_index(frame.substr(10, 6));
    if (keyidx >= 0)
        statinfo |= RX_HD_KEYIDV | (((u32)keyidx & 0x3FF) << RX_HD_KEYID_LSB);
    u64 tsfv = tsf(insns);
    u32 frmlen = (u32)frame.size();
    u32 recvec1a = ((frmlen + MAC_FCS_LEN) & 0xFFF) | ((item.rate & 0xF) << 12);
    u32 recvec1b = 0;
    u32 recvec1c = ((u32)item.rssi & 0xFF) | (((u32)item.rssi & 0xFF) << 24);
    u32 words[17];
    words[0] = h[0]; words[1] = h[1]; words[2] = rbds[0].addr; words[3] = h[3];
    words[4] = h[4]; words[5] = h[5]; words[6] = h[6];
    words[7] = frmlen & 0xFFFF;
    words[8] = (u32)(tsfv & 0xFFFFFFFF);
    words[9] = (u32)((tsfv >> 32) & 0xFFFFFFFF);
    words[10] = recvec1a; words[11] = recvec1b; words[12] = recvec1c;
    words[13] = 0; words[14] = 0; words[15] = 0; words[16] = statinfo;
    mem.write(mem.ctx, rhd, pack_words(words, 17));
    if (debug) {
        dbg_insns_ = insns;
        dbg(strf("rx -> rhd %08x len %d: %s", rhd, (int)frame.size(), hexstr(bhead(frame, 32)).c_str()));
    }
    rx_last_rhd_ = rhd;
    rx_cur_rhd_ = h[1];
    return true;
}

u32 MacHw::tick(u64 insns)
{
    if (!tx_inflight_.empty())
        tx_complete(insns);
    if (!rx_queue_.empty())
        rx_flush(insns);
    u32 now = (u32)mono(insns);
    u32 fired = 0;
    u32 unmask = regs_.count(TIMERS_INT_UN_MASK + PLo) ? regs_[TIMERS_INT_UN_MASK + PLo] : 0;
    for (int i = 0; i < ABS_TIMER_CNT; i++) {
        if (timer_armed_[i] && ((now - timer_value_[i]) & 0xFFFFFFFF) < 0x80000000) {
            timer_armed_[i] = false;
            fired |= 1u << i;
        }
    }
    fired &= unmask;
    if (fired) {
        timers_pending_ |= fired;
        gen_raw_ |= ABS_GEN_TIMERS;
        if (debug) {
            dbg_insns_ = insns;
            dbg(strf("timers fired %03x (unmask %03x)", fired, unmask));
        }
    }
    u32 bcn_int = (regs_.count(BCN_CNTRL_1) ? regs_[BCN_CNTRL_1] : 0) & 0xFFFF;
    if (bcn_int && (state_ == HW_ACTIVE || state_ == HW_DOZE)) {
        u64 idx = tsf(insns) / (bcn_int * TU_US);
        if (!last_tbtt_idx_valid_) {
            last_tbtt_idx_ = idx;
            last_tbtt_idx_valid_ = true;
        } else if (idx != last_tbtt_idx_) {
            last_tbtt_idx_ = idx;
            if (state_ == HW_DOZE) {
                state_ = next_state_ = HW_ACTIVE;
                if (!rx_queue_.empty())
                    rx_flush(insns);
            }
            gen_raw_ |= IMP_PRI_TBTT;
        }
    }
    return update_lines();
}

/* ---- snapshot (de)serialization of the MAC engine state ---- */
static void mp_u32(Bytes &b, u32 v) { b += p32le(v); }
static void mp_u64(Bytes &b, u64 v) { b += p64le(v); }
static void mp_i(Bytes &b, int v) { b += p32le((u32)v); }
static void mp_by(Bytes &b, const Bytes &s) { b += p32le((u32)s.size()); b += s; }
static u32 mg_u32(const Bytes &b, size_t &p) { u32 v = g32le(b, p); p += 4; return v; }
static u64 mg_u64(const Bytes &b, size_t &p) { u64 v = g64le(b, p); p += 8; return v; }
static int mg_i(const Bytes &b, size_t &p) { return (int)mg_u32(b, p); }
static Bytes mg_by(const Bytes &b, size_t &p) { u32 n = mg_u32(b, p); Bytes s = b.substr(p, n); p += n; return s; }

void MacHw::serialize(Bytes &b) const
{
    mp_u32(b, (u32)regs_.size());
    for (std::map<u32, u32>::const_iterator it = regs_.begin(); it != regs_.end(); ++it) { mp_u32(b, it->first); mp_u32(b, it->second); }
    mp_i(b, state_); mp_i(b, next_state_);
    mp_u32(b, gen_raw_); mp_u32(b, txrx_raw_); mp_u32(b, timers_pending_);
    for (int i = 0; i < 10; i++) { b += (char)(timer_armed_[i] ? 1 : 0); mp_u32(b, timer_value_[i]); }
    mp_u64(b, mono_offset_); mp_u64(b, tsf_offset_);
    mp_u32(b, (u32)keys_.size());
    for (std::map<int, KeyEnt>::const_iterator it = keys_.begin(); it != keys_.end(); ++it) {
        mp_i(b, it->first); mp_by(b, it->second.addr); mp_by(b, it->second.key);
        mp_i(b, it->second.ctype); mp_i(b, it->second.vlan); mp_i(b, it->second.spp);
        mp_i(b, it->second.usedef); mp_i(b, it->second.clen);
    }
    mp_u32(b, (u32)tx_cur_.size());
    for (std::map<int, u32>::const_iterator it = tx_cur_.begin(); it != tx_cur_.end(); ++it) { mp_i(b, it->first); mp_u32(b, it->second); }
    mp_u32(b, (u32)tx_last_.size());
    for (std::map<int, u32>::const_iterator it = tx_last_.begin(); it != tx_last_.end(); ++it) { mp_i(b, it->first); mp_u32(b, it->second); }
    mp_u32(b, (u32)tx_inflight_.size());
    for (size_t i = 0; i < tx_inflight_.size(); i++) { mp_u64(b, tx_inflight_[i].done_at_us); mp_i(b, tx_inflight_[i].queue); mp_u32(b, tx_inflight_[i].thd_addr); b += (char)(tx_inflight_[i].want_irq ? 1 : 0); }
    mp_u32(b, rx_cur_rhd_); mp_u32(b, rx_cur_rbd_); mp_u32(b, rx_last_rhd_); mp_u32(b, rx_last_rbd_);
    mp_u32(b, (u32)rx_queue_.size());
    for (size_t i = 0; i < rx_queue_.size(); i++) { mp_by(b, rx_queue_[i].frame); mp_i(b, rx_queue_[i].rssi); mp_u32(b, rx_queue_[i].rate); mp_i(b, rx_queue_[i].decr); mp_i(b, rx_queue_[i].keyidx); }
    mp_i(b, rx_dropped_); b += (char)(monitor_ ? 1 : 0);
    b += (char)(last_tbtt_idx_valid_ ? 1 : 0); mp_u64(b, last_tbtt_idx_);
}

void MacHw::deserialize(const Bytes &b, size_t &p)
{
    regs_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { u32 k = mg_u32(b, p); u32 v = mg_u32(b, p); regs_[k] = v; } }
    state_ = mg_i(b, p); next_state_ = mg_i(b, p);
    gen_raw_ = mg_u32(b, p); txrx_raw_ = mg_u32(b, p); timers_pending_ = mg_u32(b, p);
    for (int i = 0; i < 10; i++) { timer_armed_[i] = b[p++] != 0; timer_value_[i] = mg_u32(b, p); }
    mono_offset_ = mg_u64(b, p); tsf_offset_ = mg_u64(b, p);
    keys_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { int k = mg_i(b, p); KeyEnt e; e.addr = mg_by(b, p); e.key = mg_by(b, p); e.ctype = mg_i(b, p); e.vlan = mg_i(b, p); e.spp = mg_i(b, p); e.usedef = mg_i(b, p); e.clen = mg_i(b, p); keys_[k] = e; } }
    tx_cur_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { int k = mg_i(b, p); tx_cur_[k] = mg_u32(b, p); } }
    tx_last_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { int k = mg_i(b, p); tx_last_[k] = mg_u32(b, p); } }
    tx_inflight_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { Inflight it; it.done_at_us = mg_u64(b, p); it.queue = mg_i(b, p); it.thd_addr = mg_u32(b, p); it.want_irq = b[p++] != 0; tx_inflight_.push_back(it); } }
    rx_cur_rhd_ = mg_u32(b, p); rx_cur_rbd_ = mg_u32(b, p); rx_last_rhd_ = mg_u32(b, p); rx_last_rbd_ = mg_u32(b, p);
    rx_queue_.clear();
    { u32 n = mg_u32(b, p); for (u32 i = 0; i < n; i++) { RxItem it; it.frame = mg_by(b, p); it.rssi = mg_i(b, p); it.rate = mg_u32(b, p); it.decr = mg_i(b, p); it.keyidx = mg_i(b, p); rx_queue_.push_back(it); } }
    rx_dropped_ = mg_i(b, p); monitor_ = b[p++] != 0;
    last_tbtt_idx_valid_ = b[p++] != 0; last_tbtt_idx_ = mg_u64(b, p);
}

/* ---------------------------------------------------------------------------
 * Standalone self-tests, no emulator: a fake guest memory and the register
 * writes an OpenBeken AP boot was traced making. Port of _run_selftests() in
 * src/wifimac.py, same checks, same messages, same order.
 * ------------------------------------------------------------------------- */
static int t_checks, t_failed;
static void tcheck(bool cond, const char *msg)
{
    if (!cond) {
        printf("  [FAIL] %s\n", msg);
        t_failed++;
        return;
    }
    t_checks++;
    printf("  [PASS] %s\n", msg);
}

static const u32 T_BASE = 0x400000;
static const u32 T_MEMSZ = 0x100000;
static Bytes t_mem;
static Bytes t_rd(void *, u32 a, u32 s)
{
    if (a < T_BASE || a - T_BASE + s > T_MEMSZ)
        return Bytes();
    return t_mem.substr(a - T_BASE, s);
}
static void t_wr(void *, u32 a, const Bytes &d)
{
    if (a < T_BASE || a - T_BASE + d.size() > T_MEMSZ)
        return;
    t_mem.replace(a - T_BASE, d.size(), d);
}
static Bytes rd(u32 a, u32 s) { return t_rd(0, a, s); }
static void wr(u32 a, const Bytes &d) { t_wr(0, a, d); }
static u32 rd32(u32 a) { return g32le(rd(a, 4), 0); }
static void wr32(u32 a, u32 v) { wr(a, p32le(v)); }
static Bytes pack17(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u32 a8,
                    u32 a9, u32 a10, u32 a11, u32 a12, u32 a13, u32 a14, u32 a15, u32 a16)
{
    u32 w[17] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16};
    return pack_words(w, 17);
}
static Bytes pack5(u32 a0, u32 a1, u32 a2, u32 a3, u32 a4)
{
    u32 w[5] = {a0, a1, a2, a3, a4};
    return pack_words(w, 5);
}

struct TSent { Bytes frame; TxInfo info; };
static std::vector<TSent> t_sent;
static void t_on_tx(void *, const Bytes &f, const TxInfo &info)
{
    TSent s; s.frame = f; s.info = info;
    t_sent.push_back(s);
}
static StrList t_said;
static void t_log(void *, const std::string &s) { t_said.push_back(s); }

int wifimac_selftests()
{
    t_checks = 0;
    t_failed = 0;
    t_mem.assign(T_MEMSZ, '\0');
    t_sent.clear();
    t_said.clear();
    const u32 CORE = WIFI_CORE, PL = WIFI_PL;
    /* RX_CNTRL filters (reg_mac_core.h / mm.h), as in src/wifimac.py */
    const u32 RX_FILTER_MONITOR = 0xFFFFFFFFu & ~(0x00000020u | 0x00000001u | 0x80000000u);
    const u32 MM_RX_FILTER_ACTIVE = 0x00000004 | 0x00000008 | 0x00000080 | 0x00000400 | 0x00008000
                                    | 0x00010000 | 0x00020000 | 0x01000000 | 0x04000000 | 0x10000000
                                    | 0x20000000;
    const u32 LSIG_VALID_MASK = 0xFF0F; /* codes 0..3, 8..0xF */
    const u32 ENCR_KEY[4] = {ENCR_KEY0, ENCR_KEY1, ENCR_KEY2, ENCR_KEY3};
    bool has;
    u32 v;

    MacHw m(5);
    m.mem.ctx = 0; m.mem.read = t_rd; m.mem.write = t_wr;
    m.have_mem = true;
    m.on_tx = t_on_tx;

    printf("== clocks ==\n");
    tcheck(m.read(CORE + MONOTONIC_COUNTER_2_LO, 5000, has) == 1000, "monotonic counter counts device microseconds");
    m.write(CORE + STATE_CNTRL, 0x30, 0);
    tcheck(m.read(CORE + STATE_CNTRL, 0, has) == 0x33, "nextState ACTIVE is taken at once (reads 0x33)");
    m.write(PL + GEN_INT_ENABLE, 0x8377f14d, 0);
    m.write(CORE + STATE_CNTRL, 0x00, 0);
    tcheck((m.gen_raw_ & IDLE_INTERRUPT) && (m.lines & (1u << FIQ_MAC_GENERAL)), "ACTIVE->IDLE raises the idle interrupt on the general line");
    m.write(PL + GEN_INT_ACK, IDLE_INTERRUPT, 0);
    tcheck(!m.lines, "acked: line drops");
    m.write(CORE + STATE_CNTRL, 0x00, 0);
    tcheck(!(m.gen_raw_ & IDLE_INTERRUPT), "IDLE->IDLE does not re-raise it");

    printf("== ABS timers ==\n");
    m.write(CORE + ABS_TIMER + 4 * 7, 100000, 0); /* MM timer at 100 ms */
    m.write(PL + TIMERS_INT_UN_MASK, 0x80, 0);
    m.tick(5 * 99999ull);
    tcheck(m.timers_pending_ == 0, "not before its time");
    m.tick(5 * 100000ull);
    tcheck(m.timers_pending_ == 0x80 && m.read(PL + TIMERS_INT_EVENT_SET, 0, has) == 0x80,
           "timer 7 fires at 100 ms, EVENT_SET reads the pending bit");
    tcheck((m.lines & (1u << FIQ_MAC_GENERAL)) && (m.gen_raw_ & ABS_GEN_TIMERS), "ABS_GEN_TIMERS on the general line");
    m.write(PL + TIMERS_INT_EVENT_CLEAR, 0x80, 0);
    m.write(PL + GEN_INT_ACK, ABS_GEN_TIMERS, 0);
    m.tick(5 * 200000ull);
    tcheck(m.timers_pending_ == 0 && !m.lines, "cleared and not re-fired for the same value");
    m.write(CORE + ABS_TIMER + 4 * 2, 50, 5 * 300000ull); /* a target already in the past */
    m.write(PL + TIMERS_INT_UN_MASK, 0x84, 0);
    m.tick(5 * 300001ull);
    tcheck(!(m.timers_pending_ & 0x4), "a target already behind the counter does not fire (match semantics)");
    m.write(PL + TIMERS_INT_EVENT_SET, 0x4, 0);
    tcheck((m.timers_pending_ & 0x4) && (m.gen_raw_ & ABS_GEN_TIMERS), "the firmware forces it through TIMERS_INT_EVENT_SET instead");
    m.write(PL + TIMERS_INT_EVENT_CLEAR, 0x4, 0); m.write(PL + GEN_INT_ACK, ABS_GEN_TIMERS, 0);

    printf("== key storage RAM ==\n");
    m.write(CORE + ENCR_MAC_ADDR_LOW, 0x44332211, 0); m.write(CORE + ENCR_MAC_ADDR_HIGH, 0x6655, 0);
    for (int i = 0; i < 4; i++)
        m.write(CORE + ENCR_KEY[i], 0x01010101u * (u32)(i + 1), 0);
    m.write(CORE + ENCR_CNTRL, NEW_WRITE | (30u << 16) | (3u << 8), 0);
    tcheck((m.read(CORE + ENCR_CNTRL, 0, has) & NEW_WRITE) == 0, "newWrite completes instantly");
    tcheck(m.keys_.count(30) && m.keys_[30].addr == H("112233445566") && m.keys_[30].ctype == 3, "entry 30 stored with CCMP");
    m.write(CORE + ENCR_MAC_ADDR_LOW, 0x44332211, 0); m.write(CORE + ENCR_MAC_ADDR_HIGH, 0x6655, 0);
    m.write(CORE + ENCR_CNTRL, NEW_SEARCH, 0);
    v = m.read(CORE + ENCR_CNTRL, 0, has);
    tcheck(!(v & SEARCH_ERROR) && ((v >> 16) & 0x3FF) == 30, "search by address finds index 30");
    m.write(CORE + ENCR_MAC_ADDR_LOW, 0, 0);
    m.write(CORE + ENCR_CNTRL, NEW_SEARCH, 0);
    tcheck((m.read(CORE + ENCR_CNTRL, 0, has) & SEARCH_ERROR) != 0, "unknown address -> searchError");

    printf("== time on air ==\n");
    m.write(PL + TIME_ON_AIR_PARAM_1, 100, 0);
    m.write(PL + TIME_ON_AIR_PARAM_2, 0, 0); /* nxmac_ppdu_mcs_index_setf(0) */
    tcheck(!(m.read(PL + TIME_ON_AIR_VALUE, 0, has) & TIME_ON_AIR_VALID), "no result before the compute command");
    m.write(PL + TIME_ON_AIR_VALUE, COMPUTE_DURATION, 0); /* nxmac_compute_duration_setf(1) */
    v = m.read(PL + TIME_ON_AIR_VALUE, 0, has);
    tcheck((v & TIME_ON_AIR_VALID) && (v & 0xFFFF) == 192 + 800, "100 bytes at 1 Mbps = 992 us, valid");

    printf("== TX: THD + TBD chain -> peer, done status, trigger ==\n");
    const u32 THD = 0x41cff0, TBD = 0x420c10, BUF = 0x420c2a, POL = 0x417ecc;
    Bytes frame = H("08020000ffffffffffffc8478c000001c8478c0000011000aaaa0300000008060001080006040001c8478c000001c0a80401000000000000c0a80401");
    u32 flen = (u32)frame.size();
    wr(BUF, frame);
    wr(TBD, pack5(TX_PAYLOAD_DESC_PATTERN, 0, BUF, BUF + flen - 1, 0));
    wr(THD, pack17(TX_HEADER_DESC_PATTERN, 0, 0, TBD, 0, 0, flen + 4, 0, 0x3f0002, POL, 0, 0, 0, 0, INTERRUPT_EN_TX, 0, 0));
    m.write(PL + TX_RX_INT_ENABLE, 0x800207c0, 0);
    m.write(PL + tx_head_ptr(2), THD, 0);
    m.write(PL + DMA_CNTRL_SET, tx_new_head(2), 1000);
    tcheck(t_sent.size() == 1 && t_sent[0].frame == frame && t_sent[0].info.queue == 2, "the MPDU reached the peer from queue AC2");
    tcheck(rd32(THD + 60) == 0, "statinfo untouched until the air time has passed");
    m.tick(1000 + 5 * TX_AIR_TIME_US);
    u32 st = rd32(THD + 60);
    tcheck((st & DESC_DONE_TX_BIT) && (st & FRAME_SUCCESSFUL_TX_BIT), "DESC_DONE|FRAME_SUCCESSFUL written after the air time");
    tcheck((rd32(TBD + 16) & TBD_DONE_HW) != 0, "TBD marked handled");
    tcheck((m.lines & (1u << FIQ_MAC_TX_TRIGGER)) && (m.read(PL + TX_RX_INT_STATUS, 0, has) & ac_tx_trigger(2)), "AC2 TX trigger on the tx-trigger line");
    m.write(PL + TX_RX_INT_ACK, ac_tx_trigger(2), 0);
    tcheck(!m.lines, "acked");
    /* chain a second THD behind it and kick the tail */
    const u32 THD2 = THD + 0x100;
    wr(THD2, pack17(TX_HEADER_DESC_PATTERN, 0, 0, TBD, 0, 0, flen + 4, 0, 0, POL, 0, 0, 0, 0, 0, 0, 0));
    wr32(THD + 4, THD2);
    m.write(PL + DMA_CNTRL_SET, tx_new_tail(2), 2000);
    tcheck(t_sent.size() == 2, "newTail resumes from the last descriptor's nextfrmexseq_ptr");
    m.tick(2000 + 5 * TX_AIR_TIME_US);
    tcheck(!(m.lines & (1u << FIQ_MAC_TX_TRIGGER)), "a THD without INTERRUPT_EN_TX completes silently");
    /* a protected data frame: IV and MIC space are stripped for the peer */
    Bytes prot = frame; prot[1] = (char)((u8)prot[1] | 0x40);
    Bytes body = prot.substr(24);
    Bytes buf2 = prot.substr(0, 24) + bytes_n(8) + body + bytes_n(8);
    u32 b2len = (u32)buf2.size();
    wr(BUF, buf2);
    wr(TBD, pack5(TX_PAYLOAD_DESC_PATTERN, 0, BUF, BUF + b2len - 1, 0));
    wr(THD, pack17(TX_HEADER_DESC_PATTERN, 0, 0, TBD, 0, 0, b2len + 4, 0, 0, POL, 0, 0, 0, 0, 0, 0, 0));
    m.write(PL + DMA_CNTRL_SET, tx_new_head(2), 3000);
    tcheck(!t_sent.empty() && t_sent.back().frame == frame, "protected frame handed over as the plaintext MPDU");

    printf("== RX: payload buffers, header descriptor, trigger ==\n");
    const u32 RHD0 = 0x41a4c0, RHD1 = 0x41a524, RBD0 = 0x41ad24, RBD1 = 0x41af48, PB0 = 0x41ad48, PB1 = 0x41af6c;
    wr(RHD0, pack17(RX_HEADER_DESC_PATTERN, RHD1, 0, 0x41f16c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    wr(RHD1, pack17(RX_HEADER_DESC_PATTERN, 0, 0, 0x41f17c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    wr(RBD0, pack5(RX_PAYLOAD_DESC_PATTERN, RBD1, PB0, PB0 + 511, 0));
    wr(RBD1, pack5(RX_PAYLOAD_DESC_PATTERN, 0, PB1, PB1 + 511, 0));
    m.write(PL + RX_HEADER_HEAD_PTR, RHD0 | 1, 0); m.write(PL + DMA_CNTRL_SET, RX_HEADER_NEW_HEAD, 0);
    m.write(PL + RX_PAYLOAD_HEAD_PTR, RBD0 | 1, 0); m.write(PL + DMA_CNTRL_SET, RX_PAYLOAD_NEW_HEAD, 0);
    Bytes probe = H("40000000ffffffffffff020000000001ffffffffffff1000") + H("0000010882848b960c121824");
    m.write(CORE + STATE_CNTRL, 0x00, 3900);
    m.inject(probe, 3900, -40);
    tcheck(rd32(RHD0 + 64) == 0 && m.rx_queue_.empty(),
           "a MAC in IDLE is not listening: the frame is lost, not queued");
    m.write(CORE + STATE_CNTRL, 0x30, 4000);
    m.write(PL + GEN_INT_ACK, 0xFFFFFFFF, 4000);
    m.inject(probe, 4000, -40);
    u32 h[17];
    unpack_words(rd(RHD0, 68), h, 17);
    tcheck((h[16] & RX_HD_DONE) && (h[16] & RX_HD_SUCCESS) && (h[16] & RX_HD_GA_FRAME), "RHD0 done|success|group-addressed");
    tcheck((h[7] & 0xFFFF) == probe.size() && h[2] == RBD0, "frmlen is the MPDU length (no FCS); first_pbd_ptr -> RBD0");
    tcheck(rd(PB0, (u32)probe.size()) == probe, "payload landed in the first buffer");
    tcheck((LSIG_VALID_MASK >> ((h[10] >> 12) & 0xF)) & 1,
           "RX vector legacy rate is a valid L-SIG code (an invalid one trips the firmware's MCS assert)");
    u32 b0[5];
    unpack_words(rd(RBD0, 20), b0, 5);
    tcheck((b0[4] & 0xFFFF) == (RX_PD_DONE | RX_PD_LASTBUF), "RBD0 done|lastbuf");
    tcheck((m.read(PL + TX_RX_INT_STATUS, 0, has) & TIMER_RX_TRIGGER) && (m.lines & (1u << FIQ_MAC_TX_RX_TIMER)),
           "timerRxTrigger raised (the one this firmware enables) on the tx-rx-timer line");
    tcheck(!(m.lines & (1u << FIQ_MAC_RX_TRIGGER)), "rxTrigger itself stays masked: not enabled by the firmware");
    tcheck(!(h[16] & RX_HD_KEYIDV), "a sender with no key RAM entry is reported without a key index");
    m.write(PL + TX_RX_INT_ACK, 0xFFFFFFFF, 0);
    /* The firmware registers an associated station in the key RAM (index 24+ = sta 0+). */
    m.write(CORE + ENCR_MAC_ADDR_LOW, 0x00000002, 0); m.write(CORE + ENCR_MAC_ADDR_HIGH, 0x0100, 0);
    m.write(CORE + ENCR_CNTRL, NEW_WRITE | (24u << 16), 0);
    Bytes seq;
    for (int i = 0; i < 256; i++)
        seq += (char)i;
    Bytes big = H("08010000c8478c000001020000000001ffffffffffff2000") + seq + bytes_n(20);
    m.inject(big, 5000);
    u32 h1[17];
    unpack_words(rd(RHD1, 68), h1, 17);
    tcheck((h1[16] & RX_HD_DONE) && h1[2] == RBD1, "second frame uses RHD1 and starts at RBD1");
    tcheck((h1[16] & RX_HD_KEYIDV) && ((h1[16] >> RX_HD_KEYID_LSB) & 0x3FF) == 24,
           "transmitter address found in the key RAM: keyIndexValid with index 24 (station 0)");
    m.inject(H("0802") + bytes_n(30), 6000);
    tcheck(m.rx_queue_.size() == 1, "no free RHD: the frame waits in the queue, nothing half-written");
    wr32(RHD1 + 4, RHD0); /* firmware re-chains RHD0 behind RHD1... */
    wr32(RHD0 + 64, 0);   /* ...after clearing its status */
    wr32(RHD0 + 4, 0);
    wr32(RBD1 + 4, RBD0);
    wr32(RBD0 + 16, 0); wr32(RBD0 + 4, 0);
    m.write(PL + DMA_CNTRL_SET, 1u << 24, 7000); /* RX_HEADER_NEW_TAIL */
    tcheck(m.rx_queue_.empty() && (rd32(RHD0 + 64) & RX_HD_DONE),
           "queued frame delivered once the firmware recycled a descriptor");

    printf("== TSF / TBTT ==\n");
    m.write(PL + TSF_LO, 1000000, 5 * 10); m.write(PL + TSF_HI, 0, 5 * 10);
    tcheck(m.read(PL + TSF_LO, 5 * 10, has) == 1000000, "TSF set by software");
    tcheck(m.read(PL + TSF_LO, 5 * 20, has) == 1000010, "and it runs");
    m.write(CORE + BCN_CNTRL_1, 0xff900064, 0);
    m.write(CORE + STATE_CNTRL, 0x30, 5 * 20);
    m.write(PL + GEN_INT_ACK, 0xFFFFFFFF, 0);
    m.tick(5 * 30);
    tcheck(!(m.gen_raw_ & IMP_PRI_TBTT), "no TBTT inside the current interval");
    m.tick(5 * (102400ull + 30));
    tcheck((m.gen_raw_ & IMP_PRI_TBTT) && (m.lines & (1u << FIQ_MAC_GENERAL)), "TBTT raised once the TSF crosses a 100 TU boundary");

    printf("== power save: doze, buffered unicast, wake at TBTT ==\n");
    m.write(PL + GEN_INT_ACK, 0xFFFFFFFF, 0);
    wr32(RHD0 + 64, 0); wr32(RHD1 + 64, 0); /* give the receiver fresh descriptors */
    wr32(RBD0 + 16, 0); wr32(RBD1 + 16, 0);
    wr32(RHD0 + 4, RHD1); wr32(RHD1 + 4, 0);
    wr32(RBD0 + 4, RBD1); wr32(RBD1 + 4, 0);
    m.write(PL + RX_HEADER_HEAD_PTR, RHD0 | 1, 0); m.write(PL + DMA_CNTRL_SET, RX_HEADER_NEW_HEAD, 0);
    m.write(PL + RX_PAYLOAD_HEAD_PTR, RBD0 | 1, 0); m.write(PL + DMA_CNTRL_SET, RX_PAYLOAD_NEW_HEAD, 0);
    u64 t0 = 5 * (2 * 102400ull + 100);
    m.tick(t0);
    m.write(CORE + STATE_CNTRL, (u32)HW_DOZE << 4, t0);
    Bytes uni = H("08020000c8478c000000020000000001020000000001e000") + H("aaaa030000000800") + bytes_n(20);
    m.inject(uni, t0 + 5);
    m.inject(H("80000000ffffffffffff020000000001020000000001") + bytes_n(40), t0 + 6);
    tcheck(m.rx_queue_.size() == 1 && rd32(RHD0 + 64) == 0,
           "dozing: the unicast frame is buffered (as the AP would), the broadcast one is lost");
    m.tick(5 * (3 * 102400ull + 10));
    tcheck(m.state_ == HW_ACTIVE && (m.gen_raw_ & IMP_PRI_TBTT), "the MAC wakes itself to ACTIVE at the next TBTT");
    tcheck(m.rx_queue_.empty() && (rd32(RHD0 + 64) & RX_HD_DONE),
           "and the buffered frame is delivered on waking");

    printf("== RX filter: monitor (sniffer) mode reported ==\n");
    MacHw m2(5);
    m2.log = t_log;
    m2.mem.ctx = 0; m2.mem.read = t_rd; m2.mem.write = t_wr;
    m2.have_mem = true;
    m2.write(CORE + RX_CNTRL, MM_RX_FILTER_ACTIVE, 0);
    tcheck(!m2.monitor_ && t_said.empty(), "the firmware's normal RX filter is not monitor mode and says nothing");
    m2.write(CORE + RX_CNTRL, RX_FILTER_MONITOR, 0);
    tcheck(m2.monitor_ && t_said.size() == 1 && contains(t_said[0], "monitor mode"),
           "the firmware's own monitor filter (MM_RX_FILTER_MONITOR) is reported once");
    m2.write(CORE + RX_CNTRL, RX_FILTER_MONITOR, 0);
    tcheck(t_said.size() == 1, "staying in monitor mode is not reported again");
    m2.write(CORE + RX_CNTRL, MM_RX_FILTER_ACTIVE, 0);
    tcheck(!m2.monitor_ && t_said.size() == 2 && contains(t_said[1], "normal"), "and leaving it is reported once");
    m2.read(CORE + RX_CNTRL, 0, has);
    tcheck(!has && m2.regs_[RX_CNTRL] == MM_RX_FILTER_ACTIVE,
           "the register itself still reads back from memory, as before");
    /* Accepting other BSSs' frames alone is ordinary scanning, and accepting
     * other stations' unicast alone is not sniffing either: only both at once
     * means the firmware wants every frame on the channel. */
    m2.write(CORE + RX_CNTRL, MM_RX_FILTER_ACTIVE | ACCEPT_OTHER_BSSID, 0);
    tcheck(!m2.monitor_ && t_said.size() == 2, "accepting other BSSIDs alone (scanning) is not monitor mode");
    m2.write(CORE + RX_CNTRL, MM_RX_FILTER_ACTIVE | ACCEPT_UNICAST, 0);
    tcheck(!m2.monitor_ && t_said.size() == 2, "accepting other stations' unicast alone is not monitor mode either");

    t_mem = Bytes();
    if (t_failed) {
        printf("\n%d Wi-Fi MAC self-test(s) FAILED (%d passed).\n", t_failed, t_checks);
        return -1;
    }
    printf("\nAll %d Wi-Fi MAC self-tests passed.\n", t_checks);
    return t_checks;
}
