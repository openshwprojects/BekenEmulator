#include "blecore.h"
#include "crypto_prims.h"

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

/* register offsets (5.2 blecore) */
static const u32 RWBLECNTL = 0x000, VERSION = 0x004;
static const u32 INTCNTL0 = 0x00C, INTSTAT0 = 0x010, INTACK0 = 0x014;
static const u32 INTCNTL1 = 0x018, INTSTAT1 = 0x01C, INTACK1 = 0x020;
static const u32 DEEPSLCNTL = 0x030, DEEPSLWKUP = 0x034, DEEPSLSTAT = 0x038;
static const u32 CLKNCNTCORR = 0x044;
static const u32 AESCNTL = 0x0B0;
static const u32 AESKEY0 = 0x0B4, AESKEY1 = 0x0B8, AESKEY2 = 0x0BC, AESKEY3 = 0x0C0;
static const u32 AESPTR = 0x0C4;
static const u32 AES_START = 1u << 0;
static const u32 FINETIMTGT = 0x0E4;
static const u32 BKRWEXMEM = 0x19C;
static const u32 EM_SCAN_BYTES = 0x2000;
static const u32 SIZE_52 = 0x200;
static const u32 VERSION_RESET = 0x0B001100;

static const u32 MASTER_SOFT_RST = 1u << 31;
static const u32 MASTER_TGSOFT_RST = 1u << 30;
static const u32 REG_SOFT_RST = 1u << 29;
static const u32 RADIOCNTL_SOFT_RST = 1u << 28;
static const u32 SWINT_REQ = 1u << 27;
static const u32 SOFT_RESETS = MASTER_SOFT_RST | MASTER_TGSOFT_RST | REG_SOFT_RST | RADIOCNTL_SOFT_RST;
static const u32 SELF_CLEARING = SOFT_RESETS | SWINT_REQ;

static const u32 CLKNINT = 1u << 0, SLPINT = 1u << 1, CRYPTINT = 1u << 2, SWINT = 1u << 3, FINETGTINT = 1u << 4;
static const u32 TIMESTAMPTGTINT[3] = {1u << 5, 1u << 6, 1u << 7};

static const u32 DEEP_SLEEP_ON = 1u << 2;
static const u32 DEEP_SLEEP_CORR_EN = 1u << 3;
static const u32 DEEP_SLEEP_STAT = 1u << 15;

static const u32 SAMP = 1u << 31;
static const u32 SLOT_MASK = 0x0FFFFFFF;
static const u32 FINE_MASK = 0x3FF;
static const u32 SLOT_US = 625;
static const u32 LP_CLOCK_HZ = 32768;

static const u32 CLKNTGT_52[3] = {0x0E8, 0x0F0, 0x0F8};
static const u32 HMICROSECTGT_52[3] = {0x0EC, 0x0F4, 0x0FC};
static const u32 AESKEY[4] = {AESKEY0, AESKEY1, AESKEY2, AESKEY3};

BleCore::BleCore(u32 base, u64 insns_per_second, const std::string &layout, u64 max_sleep_us)
    : window(0), layout(layout), base_(base), raw_evt_(0), raw_core_(0), slot_latch_(0), slot_offset_(0),
      last_clkn_slot_valid_(false), last_clkn_slot_(0), sleeping_(false), wake_at_(0), slept_from_(0),
      have_mem_(false)
{
    int targets;
    if (layout == "5.1") {
        slotclk_ = 0x0F8;
        finetimecnt_ = 0x0FC;
        targets = 2;
        version_ = 0x0A000700;
        em_fixed_valid_ = true;
        em_fixed_ = 0x00910000;
        window = 0x1000;
        ble_version_valid_ = true;
        ble_version_off_ = 0x804;
        ble_version_val_ = 0x0A000F00;
        ble_control_valid_ = true;
        ble_control_ = 0x800;
    } else { /* 5.2 */
        slotclk_ = 0x100;
        finetimecnt_ = 0x104;
        targets = 3;
        version_ = VERSION_RESET;
        em_fixed_valid_ = false;
        em_fixed_ = 0;
        window = SIZE_52;
        ble_version_valid_ = false;
        ble_version_off_ = 0;
        ble_version_val_ = 0;
        ble_control_valid_ = false;
        ble_control_ = 0;
    }
    for (int i = 0; i < targets; i++) {
        clkntgt_.push_back(CLKNTGT_52[i]);
        hmicrosectgt_.push_back(HMICROSECTGT_52[i]);
    }
    max_sleep_insns_ = insns_per_second * max_sleep_us / 1000000;
    if (max_sleep_insns_ < 1)
        max_sleep_insns_ = 1;
    insns_per_slot_ = insns_per_second * SLOT_US / 1000000;
    if (insns_per_slot_ < 1)
        insns_per_slot_ = 1;
    insns_per_lp_cycle_ = insns_per_second / LP_CLOCK_HZ;
    if (insns_per_lp_cycle_ < 1)
        insns_per_lp_cycle_ = 1;
}

u32 BleCore::slot(u64 insns) const { return (u32)((insns / insns_per_slot_ + slot_offset_) & SLOT_MASK); }
u32 BleCore::fine(u64 insns) const { return (u32)((insns % insns_per_slot_) * SLOT_US / insns_per_slot_); }

static u32 regget(const std::map<u32, u32> &m, u32 k)
{
    std::map<u32, u32>::const_iterator it = m.find(k);
    return it == m.end() ? 0 : it->second;
}

u32 BleCore::read(u32 address, u64 insns, bool &has)
{
    has = true;
    u32 off = address - base_;
    if (off == VERSION)
        return version_;
    if (ble_version_valid_ && off == ble_version_off_)
        return ble_version_val_;
    if (off == INTSTAT0)
        return raw_evt_ & regget(regs_, INTCNTL0);
    if (off == INTSTAT1)
        return raw_core_ & regget(regs_, INTCNTL1);
    if (off == DEEPSLCNTL) {
        u32 v = regget(regs_, DEEPSLCNTL);
        return sleeping_ ? (v | DEEP_SLEEP_STAT) : (v & ~DEEP_SLEEP_STAT);
    }
    if (off == slotclk_)
        return slot_latch_ & SLOT_MASK;
    if (off == finetimecnt_)
        return fine(insns) & FINE_MASK;
    if (off == RWBLECNTL || off == DEEPSLSTAT || (ble_control_valid_ && off == ble_control_))
        return regget(regs_, off);
    if (off == AESCNTL)
        return regget(regs_, AESCNTL) & ~AES_START;
    has = false;
    return 0;
}

void BleCore::write(u32 address, u32 value, u64 insns)
{
    u32 off = address - base_;
    value &= 0xFFFFFFFF;
    if (off == RWBLECNTL) {
        if (value & SWINT_REQ)
            raw_core_ |= SWINT;
        regs_[off] = value & ~SELF_CLEARING;
        return;
    }
    if (ble_control_valid_ && off == ble_control_) {
        regs_[off] = value & ~SELF_CLEARING;
        return;
    }
    if (off == INTACK0) {
        raw_evt_ &= ~value;
        return;
    }
    if (off == INTACK1) {
        raw_core_ &= ~value;
        return;
    }
    if (off == DEEPSLCNTL) {
        regs_[off] = value;
        if ((value & DEEP_SLEEP_CORR_EN) && regs_.count(CLKNCNTCORR)) {
            u32 want = regs_[CLKNCNTCORR] & SLOT_MASK;
            slot_offset_ = (u32)((want - insns / insns_per_slot_) & SLOT_MASK);
        }
        if (sleeping_ && !(value & DEEP_SLEEP_ON))
            wake_at_ = insns;
        if ((value & DEEP_SLEEP_ON) && !sleeping_) {
            u32 cycles = regget(regs_, DEEPSLWKUP);
            sleeping_ = true;
            slept_from_ = insns;
            u64 dur = (u64)(cycles < 1 ? 1 : cycles) * insns_per_lp_cycle_;
            if (dur > max_sleep_insns_)
                dur = max_sleep_insns_;
            wake_at_ = insns + dur;
            Event e;
            e.name = "sleep";
            e.insns = insns;
            e.extra = cycles;
            events.push_back(e);
        }
        return;
    }
    if (off == slotclk_) {
        if (value & SAMP)
            slot_latch_ = slot(insns);
        return;
    }
    if (off == AESCNTL) {
        regs_[off] = value & ~AES_START;
        if (value & AES_START)
            aes_run();
        return;
    }
    bool is_tgt = (off == FINETIMTGT);
    for (size_t i = 0; i < clkntgt_.size(); i++)
        if (off == clkntgt_[i])
            is_tgt = true;
    if (is_tgt)
        fired_tgt_.erase(off);
    regs_[off] = value;
}

u32 BleCore::em_base()
{
    if (em_fixed_valid_)
        return em_fixed_;
    return regget(regs_, BKRWEXMEM);
}

void BleCore::aes_run()
{
    if (!have_mem_)
        return;
    u32 base = em_base();
    if (!base)
        return;
    Bytes key_le;
    for (int i = 0; i < 4; i++)
        key_le += p32le(regget(regs_, AESKEY[i]));
    u32 ptr = base + (regget(regs_, AESPTR) & 0xFFFF);
    Bytes plain_le = mem_.read(mem_.ctx, ptr, 16);
    if (plain_le.size() != 16)
        return;
    Bytes key_msb = reversed_bytes(key_le);
    Bytes plain_msb = reversed_bytes(plain_le);
    Aes aes(key_msb);
    u8 out_msb[16];
    aes.encrypt_block((const u8 *)plain_msb.data(), out_msb);
    Bytes out_le = reversed_bytes(Bytes((const char *)out_msb, 16));
    mem_.write(mem_.ctx, ptr + 16, out_le);
    raw_core_ |= CRYPTINT;
    Event e;
    e.name = "aes";
    e.insns = ptr;
    e.extra = 0;
    events.push_back(e);
}

std::vector<AdField> BleCore::parse_ad(const Bytes &blob)
{
    std::vector<AdField> fields;
    size_t p = 0;
    while (p < blob.size() && (u8)blob[p]) {
        int length = (u8)blob[p];
        if (p + 1 + length > blob.size())
            return std::vector<AdField>();
        AdField f;
        f.type = (u8)blob[p + 1];
        f.value = blob.substr(p + 2, length - 1);
        fields.push_back(f);
        p += 1 + length;
    }
    return fields.size() >= 2 ? fields : std::vector<AdField>();
}

std::vector<AdvFound> BleCore::find_adv()
{
    std::vector<AdvFound> out;
    if (!have_mem_)
        return out;
    u32 base = em_base();
    if (!base)
        return out;
    Bytes blob;
    for (u32 off = 0; off < EM_SCAN_BYTES; off += 0x1000) {
        u32 want = EM_SCAN_BYTES - off < 0x1000 ? EM_SCAN_BYTES - off : 0x1000;
        Bytes chunk = mem_.read(mem_.ctx, base + off, want);
        if (chunk.size() != want)
            break;
        blob += chunk;
    }
    if (blob.empty())
        return out;
    const char *prefixes[3] = {"\x02\x01\x04", "\x02\x01\x05", "\x02\x01\x06"};
    std::set<Bytes> seen;
    for (int pi = 0; pi < 3; pi++) {
        std::string prefix(prefixes[pi], 3);
        size_t start = 0;
        for (;;) {
            size_t i = blob.find(prefix, start);
            if (i == std::string::npos)
                break;
            start = i + 1;
            std::vector<AdField> fields = parse_ad(blob.substr(i, 62));
            if (fields.empty())
                continue;
            u32 used = 0;
            for (size_t j = 0; j < fields.size(); j++)
                used += 2 + (u32)fields[j].value.size();
            Bytes raw = blob.substr(i, used);
            if (seen.count(raw))
                continue;
            seen.insert(raw);
            AdvFound a;
            a.offset = (u32)i;
            a.raw = raw;
            a.fields = fields;
            out.push_back(a);
        }
    }
    return out;
}

StrList BleCore::describe_ad(const std::vector<AdField> &fields)
{
    StrList lines;
    for (size_t i = 0; i < fields.size(); i++) {
        u8 t = fields[i].type;
        const Bytes &v = fields[i].value;
        std::string name;
        switch (t) {
        case 0x01: name = "Flags"; break;
        case 0x02: name = "16-bit UUIDs (incomplete)"; break;
        case 0x03: name = "16-bit UUIDs"; break;
        case 0x08: name = "Short Name"; break;
        case 0x09: name = "Complete Name"; break;
        case 0x0A: name = "TX Power"; break;
        case 0x16: name = "Service Data"; break;
        case 0xFF: name = "Manufacturer Data"; break;
        default: name = strf("type 0x%02X", t); break;
        }
        std::string note;
        if ((t == 0x02 || t == 0x03 || t == 0x16) && v.size() >= 2)
            note = strf("  uuid=0x%04X", (u32)rd16le(v.data()));
        else if (t == 0xFF && v.size() >= 2)
            note = strf("  company=0x%04X", (u32)rd16le(v.data()));
        std::string text;
        for (size_t j = 0; j < v.size(); j++) {
            u8 c = (u8)v[j];
            text += (c >= 32 && c < 127) ? (char)c : '.';
        }
        std::string shown = hexstr_sep(bhead(v, 12), " ") + (v.size() > 12 ? " .." : "");
        lines.push_back(strf("%-26s %-39s %s%s", name.c_str(), shown.c_str(), text.c_str(), note.c_str()));
    }
    return lines;
}

bool BleCore::reached(u32 now, u32 target) { return ((now - target) & SLOT_MASK) < (SLOT_MASK >> 1); }

void BleCore::tick(u64 insns, bool &core_irq, bool &ble_irq)
{
    if (sleeping_ && insns >= wake_at_) {
        sleeping_ = false;
        u64 slept = (insns - slept_from_) / insns_per_lp_cycle_;
        regs_[DEEPSLSTAT] = (u32)(slept & 0xFFFFFFFF);
        regs_[DEEPSLCNTL] = regget(regs_, DEEPSLCNTL) & ~DEEP_SLEEP_ON;
        raw_core_ |= SLPINT;
        Event e;
        e.name = "wake";
        e.insns = insns;
        e.extra = slept;
        events.push_back(e);
    }
    u32 mask0 = regget(regs_, INTCNTL1);
    u32 now = slot(insns);
    if ((mask0 & CLKNINT) && (!last_clkn_slot_valid_ || last_clkn_slot_ != now)) {
        last_clkn_slot_valid_ = true;
        last_clkn_slot_ = now;
        raw_core_ |= CLKNINT;
    }
    if ((mask0 & FINETGTINT) && regs_.count(FINETIMTGT)) {
        u32 tgt = regs_[FINETIMTGT] & SLOT_MASK;
        if ((!fired_tgt_.count(FINETIMTGT) || fired_tgt_[FINETIMTGT] != tgt) && reached(now, tgt)) {
            fired_tgt_[FINETIMTGT] = tgt;
            raw_core_ |= FINETGTINT;
        }
    }
    for (size_t i = 0; i < clkntgt_.size(); i++) {
        u32 bit = TIMESTAMPTGTINT[i];
        u32 tgt_off = clkntgt_[i];
        if (!(mask0 & bit) || !regs_.count(tgt_off))
            continue;
        u32 tgt = regs_[tgt_off] & SLOT_MASK;
        if ((fired_tgt_.count(tgt_off) && fired_tgt_[tgt_off] == tgt) || !reached(now, tgt))
            continue;
        if (now == tgt && fine(insns) < (regget(regs_, hmicrosectgt_[i]) & FINE_MASK))
            continue;
        fired_tgt_[tgt_off] = tgt;
        raw_core_ |= bit;
    }
    core_irq = (raw_core_ & mask0) != 0;
    ble_irq = (raw_evt_ & regget(regs_, INTCNTL0)) != 0;
}

/* ---- snapshot (de)serialization: every field, as the Python pickle has it ---- */
static void bs_u32(Bytes &b, u32 v) { b += p32le(v); }
static void bs_u64(Bytes &b, u64 v) { b += p64le(v); }
static void bs_b(Bytes &b, bool v) { b += (char)(v ? 1 : 0); }
static void bs_map(Bytes &b, const std::map<u32, u32> &m)
{
    bs_u32(b, (u32)m.size());
    for (std::map<u32, u32>::const_iterator it = m.begin(); it != m.end(); ++it) {
        bs_u32(b, it->first);
        bs_u32(b, it->second);
    }
}
static void bs_vec(Bytes &b, const std::vector<u32> &v)
{
    bs_u32(b, (u32)v.size());
    for (size_t i = 0; i < v.size(); i++)
        bs_u32(b, v[i]);
}
static u32 bg_u32(const Bytes &b, size_t &p) { u32 v = g32le(b, p); p += 4; return v; }
static u64 bg_u64(const Bytes &b, size_t &p) { u64 v = g64le(b, p); p += 8; return v; }
static bool bg_b(const Bytes &b, size_t &p) { return b[p++] != 0; }
static void bg_map(const Bytes &b, size_t &p, std::map<u32, u32> &m)
{
    m.clear();
    u32 n = bg_u32(b, p);
    for (u32 i = 0; i < n; i++) {
        u32 k = bg_u32(b, p);
        m[k] = bg_u32(b, p);
    }
}
static void bg_vec(const Bytes &b, size_t &p, std::vector<u32> &v)
{
    v.clear();
    u32 n = bg_u32(b, p);
    for (u32 i = 0; i < n; i++)
        v.push_back(bg_u32(b, p));
}

void BleCore::serialize(Bytes &b) const
{
    bs_u32(b, (u32)layout.size());
    b += layout;
    bs_u32(b, base_);
    bs_u32(b, window);
    bs_u32(b, slotclk_); bs_u32(b, finetimecnt_); bs_u32(b, version_);
    bs_b(b, em_fixed_valid_); bs_u32(b, em_fixed_);
    bs_b(b, ble_version_valid_); bs_u32(b, ble_version_off_); bs_u32(b, ble_version_val_);
    bs_b(b, ble_control_valid_); bs_u32(b, ble_control_);
    bs_vec(b, clkntgt_); bs_vec(b, hmicrosectgt_);
    bs_u64(b, max_sleep_insns_); bs_u64(b, insns_per_slot_); bs_u64(b, insns_per_lp_cycle_);
    bs_map(b, regs_);
    bs_u32(b, raw_evt_); bs_u32(b, raw_core_);
    bs_u32(b, slot_latch_); bs_u32(b, slot_offset_);
    bs_b(b, last_clkn_slot_valid_); bs_u32(b, last_clkn_slot_);
    bs_b(b, sleeping_); bs_u64(b, wake_at_); bs_u64(b, slept_from_);
    bs_map(b, fired_tgt_);
}

BleCore *BleCore::deserialize(const Bytes &b, size_t &p)
{
    u32 n = bg_u32(b, p);
    std::string lay = b.substr(p, n);
    p += n;
    u32 base = bg_u32(b, p);
    BleCore *c = new BleCore(base, 1, lay);
    c->window = bg_u32(b, p);
    c->slotclk_ = bg_u32(b, p); c->finetimecnt_ = bg_u32(b, p); c->version_ = bg_u32(b, p);
    c->em_fixed_valid_ = bg_b(b, p); c->em_fixed_ = bg_u32(b, p);
    c->ble_version_valid_ = bg_b(b, p); c->ble_version_off_ = bg_u32(b, p); c->ble_version_val_ = bg_u32(b, p);
    c->ble_control_valid_ = bg_b(b, p); c->ble_control_ = bg_u32(b, p);
    bg_vec(b, p, c->clkntgt_); bg_vec(b, p, c->hmicrosectgt_);
    c->max_sleep_insns_ = bg_u64(b, p); c->insns_per_slot_ = bg_u64(b, p); c->insns_per_lp_cycle_ = bg_u64(b, p);
    bg_map(b, p, c->regs_);
    c->raw_evt_ = bg_u32(b, p); c->raw_core_ = bg_u32(b, p);
    c->slot_latch_ = bg_u32(b, p); c->slot_offset_ = bg_u32(b, p);
    c->last_clkn_slot_valid_ = bg_b(b, p); c->last_clkn_slot_ = bg_u32(b, p);
    c->sleeping_ = bg_b(b, p); c->wake_at_ = bg_u64(b, p); c->slept_from_ = bg_u64(b, p);
    bg_map(b, p, c->fired_tgt_);
    return c;
}

/* ------------------------------------------------------------- self-tests */
/* Standalone, no emulator: drive the model with the exact register writes the
 * BK7238 RGB image was traced making. Mirrors src/blecore.py _run_selftests. */
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
static bool core_line(BleCore &c, u64 insns)
{
    bool ci, bi;
    c.tick(insns, ci, bi);
    return ci;
}
static bool ble_line(BleCore &c, u64 insns)
{
    bool ci, bi;
    c.tick(insns, ci, bi);
    return bi;
}

/* exchange-memory readers for the advertising-payload scanner tests */
struct EmCtx {
    Bytes *m;
    u32 base;
};
static Bytes em_rd_empty(void *, u32, u32) { return Bytes(); }
/* reads only from the register-provided base */
static Bytes em_rd_strict(void *ctx, u32 addr, u32 n)
{
    EmCtx &e = *(EmCtx *)ctx;
    if (!(e.base <= addr && addr < e.base + EM_SCAN_BYTES)) {
        fprintf(stderr, "FAIL: must read from the register-provided base\n");
        exit(1);
    }
    return e.m->substr(addr - e.base, n);
}
/* what Unicorn does past the touched page: the read fails (short result) */
static Bytes em_rd_one_page(void *ctx, u32 addr, u32 n)
{
    EmCtx &e = *(EmCtx *)ctx;
    if (addr >= e.base + 0x1000)
        return Bytes();
    return e.m->substr(addr - e.base, n);
}
static Bytes em_rd_plain(void *ctx, u32 addr, u32 n)
{
    EmCtx &e = *(EmCtx *)ctx;
    return e.m->substr(addr - e.base, n);
}
static void em_wr_plain(void *ctx, u32 addr, const Bytes &d)
{
    EmCtx &e = *(EmCtx *)ctx;
    for (size_t i = 0; i < d.size(); i++)
        (*e.m)[addr - e.base + i] = d[i];
}
static MemAccess em_access(EmCtx *ctx, Bytes (*rd)(void *, u32, u32))
{
    MemAccess m;
    m.ctx = ctx;
    m.read = rd;
    m.write = em_wr_plain;
    return m;
}

int blecore_selftests()
{
    g_checks = 0;
    const u32 SLOTCLK = 0x100, FINETIMECNT = 0x104;
    u32 b = 0x900000;
    BleCore c(b, 5000000); /* 5M insns per device second */
    bool has;

    printf("== reset, software interrupt, ack ==\n");
    c.write(b + RWBLECNTL, MASTER_SOFT_RST | 0x100607, 0);
    ck(c.read(b + RWBLECNTL, 0, has) == 0x100607, "MASTER_SOFT_RST self-clears, config bits kept");
    c.write(b + INTCNTL1, 0x1001e, 0); /* the mask the firmware actually programs */
    c.write(b + RWBLECNTL, SWINT_REQ, 0);
    ck(core_line(c, 0) && (c.read(b + INTSTAT1, 0, has) & SWINT), "SWINT_REQ -> INTSTAT1.SWINT, core line up");
    c.write(b + INTACK1, SWINT, 0);
    ck(!core_line(c, 0), "INTACK1 clears it, line drops");
    ck(c.read(b + VERSION, 0, has) == VERSION_RESET, "VERSION reads the 5.2 reset value");

    printf("== slot clock ==\n");
    c.write(b + SLOTCLK, SAMP, 3125 * 10);
    ck(c.read(b + SLOTCLK, 3125 * 10, has) == 10, "SAMP latches slot 10 at 31250 insns (625 us slots)");
    ck(c.read(b + SLOTCLK, 3125 * 99, has) == 10, "SLOTCLK holds the latched value until the next SAMP");
    ck(c.read(b + FINETIMECNT, 3125 * 10 + 1562, has) == 312, "FINETIMECNT mid-slot reads ~312 us");

    printf("== deep sleep and wake-up ==\n");
    c.write(b + DEEPSLWKUP, 0x4e1ff, 0); /* 9.7 s, as programmed by the firmware */
    c.write(b + DEEPSLCNTL, DEEP_SLEEP_ON, 1000000);
    ck((c.read(b + DEEPSLCNTL, 1000000, has) & DEEP_SLEEP_STAT) != 0, "DEEP_SLEEP_STAT set while asleep");
    ck(c.wake_at() == 1000000 + c.max_sleep_insns(), "sleep capped to 10 ms device time despite the 9.7 s timer");
    ck(!core_line(c, 1049000), "still asleep just before the cap");
    ck(core_line(c, 1050000) && c.raw_core_has(SLPINT), "SLPINT fires at the cap, core line up");
    ck(!(c.read(b + DEEPSLCNTL, 1050000, has) & (DEEP_SLEEP_ON | DEEP_SLEEP_STAT)), "ON and STAT cleared on wake");
    ck(c.read(b + DEEPSLSTAT, 0, has) == 50000 / c.insns_per_lp_cycle(), "DEEPSLSTAT reports the LP cycles actually slept");
    c.write(b + INTACK1, 0xFFFFFFFF, 1050000);
    c.write(b + DEEPSLCNTL, DEEP_SLEEP_ON, 2000000);
    c.write(b + DEEPSLCNTL, 0, 2010000);
    ck(core_line(c, 2010000) && c.raw_core_has(SLPINT), "clearing DEEP_SLEEP_ON while asleep wakes immediately");
    c.write(b + INTACK1, 0xFFFFFFFF, 2010000);

    printf("== timer targets ==\n");
    u64 now = 3000000;
    c.write(b + FINETIMTGT, c.slot(now) + 4, now);
    ck(!core_line(c, now), "FINETGT not yet");
    ck(core_line(c, now + 4 * 3125) && c.raw_core_has(FINETGTINT), "FINETGT fires when the slot reaches the target");
    c.write(b + INTACK1, FINETGTINT, now);
    ck(!core_line(c, now + 5 * 3125), "FINETGT does not refire for the same target");
    c.write(b + INTCNTL1, 0x1001e | TIMESTAMPTGTINT[0], now);
    c.write(b + CLKNTGT_52[0], c.slot(now) + 6, now);
    c.write(b + HMICROSECTGT_52[0], 600, now);
    ck(!core_line(c, now + 6 * 3125 + 100), "TIMESTAMPTGT1 waits for the fine part of the target");
    ck(core_line(c, now + 6 * 3125 + 3100) && c.raw_core_has(TIMESTAMPTGTINT[0]),
       "TIMESTAMPTGT1 fires once slot and fine part are reached");

    printf("== wake-up completion: clock correction + slot-clock interrupt ==\n");
    c.write(b + INTACK1, 0xFFFFFFFF, now);
    c.write(b + CLKNCNTCORR, 0x80000000u | 5000, now);
    c.write(b + DEEPSLCNTL, DEEP_SLEEP_CORR_EN, now);
    ck(c.slot(now) == 5000, "DEEP_SLEEP_CORR_EN re-bases the slot clock onto CLKNCNTCORR");
    c.write(b + SLOTCLK, SAMP, now + 3125 * 7);
    ck(c.read(b + SLOTCLK, 0, has) == 5007, "and the corrected clock keeps advancing from there");
    c.write(b + INTCNTL1, 0x808e | CLKNINT, now);
    ck(core_line(c, now) && c.raw_core_has(CLKNINT), "unmasking CLKNINT raises it on the current slot");
    c.write(b + INTACK1, CLKNINT, now);
    ck(!core_line(c, now + 100), "no second CLKNINT inside the same slot");
    ck(core_line(c, now + 3125) && c.raw_core_has(CLKNINT), "next slot raises it again while unmasked");
    c.write(b + INTACK1, CLKNINT, now);
    c.write(b + INTCNTL1, 0x808e, now);
    ck(!core_line(c, now + 2 * 3125), "masked again: silent");

    printf("== advertising payload, located through the EM base register ==\n");
    Bytes ADV = H("020106" "030250fd" "1716" "50fd430400106b6579773937716a736379666d73796e");
    BleCore c3(b, 5000000);
    c3.set_mem(em_access(0, em_rd_empty));
    ck(c3.em_base() == 0 && c3.find_adv().empty(), "no EM base yet -> nothing reported, no guessing");
    const u32 EM = 0x00424480;
    c3.write(b + BKRWEXMEM, EM, 0);
    ck(c3.em_base() == EM, "EM base is read back from BKRWEXMEM");
    Bytes mem = bytes_n(EM_SCAN_BYTES, 0);
    mem.replace(0xBB8, ADV.size(), ADV);
    EmCtx ectx = {&mem, EM};
    c3.set_mem(em_access(&ectx, em_rd_strict));
    std::vector<AdvFound> got = c3.find_adv();
    ck(got.size() == 1 && got[0].offset == 0xBB8 && got[0].raw == ADV,
       "payload found at its EM offset, read from the register base");
    c3.set_mem(em_access(&ectx, em_rd_one_page));
    std::vector<AdvFound> got1 = c3.find_adv();
    ck(got1.size() == 1 && got1[0].offset == 0xBB8, "an untouched (unmapped) second EM page does not hide the first");
    const std::vector<AdField> &fl = got[0].fields;
    ck(fl.size() == 3 && fl[0].type == 0x01 && fl[1].type == 0x02 && fl[2].type == 0x16,
       "AD chain decodes to Flags + UUID + Service Data");
    Bytes svc; /* dict(fields)[0x16]: the last structure of that type wins */
    for (size_t i = 0; i < fl.size(); i++)
        if (fl[i].type == 0x16)
            svc = fl[i].value;
    ck(svc.substr(0, 2) == H("50fd") && svc.find("keyw97qjscyfmsyn") != std::string::npos,
       "service data carries UUID 0xFD50 and the firmware key as ASCII");
    /* a build with EM somewhere else is found just the same */
    BleCore c4(b, 5000000);
    c4.write(b + BKRWEXMEM, 0x00431000, 0);
    Bytes m2 = bytes_n(EM_SCAN_BYTES, 0);
    m2.replace(0x40, ADV.size(), ADV);
    EmCtx ectx2 = {&m2, 0x00431000};
    c4.set_mem(em_access(&ectx2, em_rd_plain));
    std::vector<AdvFound> got2 = c4.find_adv();
    ck(got2.size() == 1 && got2[0].offset == 0x40, "a different EM base and offset is found with no code change");
    ck(BleCore::parse_ad(H("0201064009") + "zzz").empty(), "a length that overruns the buffer is rejected, not reported");
    ck(BleCore::parse_ad(H("020106")).empty(), "a lone Flags structure is not enough to call it an advertisement");

    printf("== masking ==\n");
    BleCore c2(b, 5000000);
    c2.write(b + RWBLECNTL, SWINT_REQ, 0);
    ck(!core_line(c2, 0) && c2.read(b + INTSTAT1, 0, has) == 0,
       "a masked-out interrupt neither shows in INTSTAT1 nor raises the line");
    ck(ble_line(c2, 0) == false, "the BLE EVENT line (set 0) never rises: no radio events are modelled");

    printf("== 5.1 layout (BK7231N/M) ==\n");
    BleCore c5(b, 5000000, "5.1");
    ck(c5.read(b + VERSION, 0, has) == 0x0A000700, "ipcore VERSION reads the 5.1 reset value");
    ck(c5.read(b + 0x804, 0, has) == 0x0A000F00, "BLE-block VERSION at +0x804 reads 0x0A000F00 (lld.c:404 asserts it)");
    ck(c5.window == 0x1000 && BleCore(b, 5000000).window == SIZE_52,
       "5.1 claims the whole page (BLE block at +0x800); 5.2 keeps its window");
    c5.write(b + 0x800, MASTER_SOFT_RST | MASTER_TGSOFT_RST | REG_SOFT_RST | RADIOCNTL_SOFT_RST | 0x00100607, 0);
    ck(c5.read(b + 0x800, 0, has) == 0x00100607, "5.1 BLE-block RWBLECNTL at +0x800 self-clears all reset commands");
    bool h2;
    BleCore(b, 5000000).read(b + 0x800, 0, h2);
    ck(!h2, "5.2 does not claim a second RWBLECNTL at +0x800");
    c5.write(b + 0x0F8, SAMP, 3 * 3125);
    ck(c5.read(b + 0x0F8, 3 * 3125, has) == 3, "SLOTCLK lives at +0xF8: SAMP latches the slot there");
    c5.read(b + 0x100, 0, has);
    ck(!has, "+0x100 is not SLOTCLK on 5.1 (plain memory)");
    ck(c5.read(b + 0x0FC, 3 * 3125 + 1562, has) == c5.fine(3 * 3125 + 1562) && has, "FINETIMECNT lives at +0xFC");
    c5.write(b + INTCNTL1, TIMESTAMPTGTINT[2], 0);
    c5.write(b + 0x0F8, 5, 0); /* a SLOTCLK write, not a CLKNTGT3 target */
    ck(!core_line(c5, 10 * 3125), "the 5.2 third timestamp target does not exist on 5.1");
    ck(c5.em_base() == 0x00910000, "5.1 exchange memory is at its fixed 0x910000");

    printf("== AES engine (BLE byte order, spec ah() vector) ==\n");
    /* Bluetooth Core Vol 3 Part H D.7: ah(IRK, prand) = e(IRK, 0..0||prand) mod 2^24. */
    Bytes irk_msb = H("ec0234a357c8ad05341010a60a397d9b");
    Bytes plain_msb = bytes_n(13) + H("708194");
    Bytes em = bytes_n(0x100, 0);
    u32 ptr = 0x4C;
    em.replace(ptr, 16, reversed_bytes(plain_msb)); /* the firmware stages it LSB first */
    BleCore ca(b, 5000000, "5.1");
    EmCtx actx = {&em, 0x910000};
    ca.set_mem(em_access(&actx, em_rd_plain));
    Bytes key_le = reversed_bytes(irk_msb);
    for (int i = 0; i < 4; i++)
        ca.write(b + AESKEY[i], rd32le(key_le.data() + 4 * i), 0);
    ca.write(b + AESPTR, ptr, 0);
    ca.write(b + INTCNTL1, CRYPTINT, 0);
    ca.write(b + AESCNTL, AES_START, 0);
    ck(em.substr(ptr + 16, 3) == H("aafb0d"), "result lands 16 bytes after the input, LSB first: ah = 0x0dfbaa");
    ck((ca.read(b + AESCNTL, 0, has) & AES_START) == 0, "AES_START reads back clear once done");
    ck(core_line(ca, 0) && (ca.read(b + INTSTAT1, 0, has) & CRYPTINT), "CRYPTINT raised on the core line when unmasked");
    ca.write(b + INTACK1, CRYPTINT, 0);
    ck(!core_line(ca, 0), "acked: line drops");

    printf("\nAll %d BLE core self-tests passed.\n", g_checks);
    return g_checks;
}
