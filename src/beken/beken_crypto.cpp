#include "beken/beken_crypto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ===================================================== Python str helpers */
namespace pyc {

std::vector<u32> py_codepoints(const std::string &s)
{
    std::vector<u32> out;
    for (size_t i = 0; i < s.size();) {
        u8 b0 = (u8)s[i];
        int n = b0 < 0x80 ? 1 : (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0 && i + n <= s.size();
        u32 cp = n == 1 ? b0 : n == 2 ? (b0 & 0x1F) : n == 3 ? (b0 & 0x0F) : (b0 & 0x07);
        for (int k = 1; ok && k < n; k++) {
            u8 c = (u8)s[i + k];
            if ((c & 0xC0) != 0x80)
                ok = false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (ok && n > 1 && (cp < (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u) || cp > 0x10FFFF
                            || (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;
        if (!ok) { /* an undecodable byte, as surrogateescape keeps it */
            out.push_back(0xDC00 + b0);
            i++;
            continue;
        }
        out.push_back(cp);
        i += (size_t)n;
    }
    return out;
}

std::string py_utf8(u32 cp)
{
    std::string o;
    if (cp >= 0xDC80 && cp <= 0xDCFF) /* surrogateescape byte */
        o += (char)(u8)(cp - 0xDC00);
    else if (cp < 0x80)
        o += (char)cp;
    else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

size_t py_len(const std::string &s) { return py_codepoints(s).size(); }

bool py_isascii(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++)
        if ((u8)s[i] >= 0x80)
            return false;
    return true;
}

/* str.isspace() */
bool py_isspace(u32 c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680
           || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F
           || c == 0x3000;
}

/* str.strip() */
std::string py_strip(const std::string &s)
{
    std::vector<u32> cps = py_codepoints(s);
    size_t b = 0, e = cps.size();
    while (b < e && py_isspace(cps[b]))
        b++;
    while (e > b && py_isspace(cps[e - 1]))
        e--;
    std::string o;
    for (size_t i = b; i < e; i++)
        o += py_utf8(cps[i]);
    return o;
}

/* str.isprintable() for one code point (the cases a command line can hold) */
static bool py_printable(u32 c)
{
    if (c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0xAD)
        return false;
    if ((c >= 0x600 && c <= 0x605) || c == 0x61C || c == 0x6DD || c == 0x70F || c == 0x1680 || c == 0x180E)
        return false;
    if ((c >= 0x2000 && c <= 0x200F) || (c >= 0x2028 && c <= 0x202F) || (c >= 0x205F && c <= 0x2064)
        || (c >= 0x2066 && c <= 0x206F) || c == 0x3000)
        return false;
    if ((c >= 0xD800 && c <= 0xF8FF) || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || c >= 0xFFFE)
        return c >= 0x10000 && c < 0xE0000; /* astral letters print; tags/private use do not */
    return true;
}

/* repr(str) */
std::string py_repr(const std::string &s)
{
    std::vector<u32> cps = py_codepoints(s);
    bool has_sq = s.find('\'') != std::string::npos, has_dq = s.find('"') != std::string::npos;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (size_t i = 0; i < cps.size(); i++) {
        u32 c = cps[i];
        if (c == (u32)q || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (py_printable(c))
            out += py_utf8(c);
        else if (c < 0x100)
            out += strf("\\x%02x", c);
        else if (c < 0x10000)
            out += strf("\\u%04x", c);
        else
            out += strf("\\U%08x", c);
    }
    out += q;
    return out;
}

/* int(text, base) for base 0, 2, 8, 10 or 16: whitespace, a sign, the base
 * prefix, single underscores between digits; base 0 rejects "010". *huge is
 * set (and out clamped) when the value does not fit in 63 bits. */
bool py_int(const std::string &text, int base, long long &out, bool *huge)
{
    std::string t = py_strip(text);
    size_t i = 0;
    bool neg = false, big = false;
    if (huge)
        *huge = false;
    if (i < t.size() && (t[i] == '+' || t[i] == '-'))
        neg = t[i++] == '-';
    bool prefixed = false;
    if (i + 1 < t.size() && t[i] == '0') {
        char p = (char)(t[i + 1] | 0x20);
        int pb = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (pb && (base == 0 || base == pb)) {
            base = pb;
            i += 2;
            prefixed = true;
            if (i < t.size() && t[i] == '_')
                i++;
        }
    }
    if (base == 0) {
        base = 10;
        if (i < t.size() && t[i] == '0') /* "0", "00", "0_0" only */
            for (size_t j = i; j < t.size(); j++)
                if (t[j] != '0' && t[j] != '_')
                    return false;
    }
    (void)prefixed;
    if (i >= t.size())
        return false;
    unsigned long long v = 0;
    bool last_us = true;
    for (; i < t.size(); i++) {
        char c = t[i];
        if (c == '_') {
            if (last_us)
                return false;
            last_us = true;
            continue;
        }
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10
                : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
        if (d >= base)
            return false;
        if (v > (0x7FFFFFFFFFFFFFFFull - (unsigned)d) / (unsigned)base)
            big = true;
        else
            v = v * (unsigned)base + (unsigned)d;
        last_us = false;
    }
    if (last_us)
        return false;
    if (big) {
        v = 0x7FFFFFFFFFFFFFFFull;
        if (huge)
            *huge = true;
    }
    out = neg ? -(long long)v : (long long)v;
    return true;
}

FILE *py_open(const std::string &path, const char *mode, int *err)
{
    FILE *f = 0;
    errno = 0;
    if (!path.empty()) {
#ifdef _WIN32
        std::wstring w, m;
        int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), 0, 0);
        w.assign((size_t)n, L'\0');
        if (n)
            MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], n);
        for (const char *p = mode; *p; p++)
            m += (wchar_t)*p;
        f = _wfopen(w.c_str(), m.c_str());
#else
        f = fopen(path.c_str(), mode);
#endif
    }
    if (err)
        *err = f ? 0 : (path.empty() || errno == 0) ? ENOENT : errno;
    return f;
}

std::string py_oserror(int e, const std::string &path, std::string *exc_type)
{
    if (exc_type)
        *exc_type = e == ENOENT ? "FileNotFoundError" : (e == EACCES || e == EPERM) ? "PermissionError"
                    : e == EISDIR ? "IsADirectoryError" : e == ENOTDIR ? "NotADirectoryError" : "OSError";
    return strf("[Errno %d] %s: ", e, strerror(e)) + py_repr(path);
}

/* The ValueError text int(text, base) raises. */
std::string py_int_error(const std::string &text, int base)
{
    std::string r = py_repr(text);
    std::vector<u32> cps = py_codepoints(r);
    if (cps.size() > 200) { /* "%.200R" */
        r.clear();
        for (size_t i = 0; i < 200; i++)
            r += py_utf8(cps[i]);
    }
    return strf("invalid literal for int() with base %d: ", base) + r;
}

} /* namespace pyc */
using namespace pyc;

/* ================================================================= CRC16 */
static u16 crc16_table[256];
struct Crc16TableInit {
    Crc16TableInit()
    {
        for (int value = 0; value < 256; value++) {
            u32 reg = (u32)value << 8;
            for (int i = 0; i < 8; i++)
                reg = ((reg << 1) ^ ((reg & 0x8000) ? 0x8005 : 0)) & 0xFFFF;
            crc16_table[value] = (u16)reg;
        }
    }
};
static Crc16TableInit g_crc16_table_init;

static inline u16 crc16_raw(const u8 *p, size_t len, u16 initial_value)
{
    u16 reg = initial_value;
    for (size_t i = 0; i < len; i++)
        reg = (u16)(((reg << 8) & 0xFFFF) ^ crc16_table[((reg >> 8) ^ p[i]) & 0xFF]);
    return reg;
}

u16 crc16(const Bytes &data, u16 initial_value)
{
    return crc16_raw((const u8 *)data.data(), data.size(), initial_value);
}

bool block_crc_check(const Bytes &block, const Bytes &crc_bytes)
{
    u16 calculated = crc16(block, 0xFFFF);
    u16 unpacked = (u16)g16be(crc_bytes, 0);
    return calculated == unpacked;
}

int detect_stripe_offset(const Bytes &data)
{
    if (data.size() < 36)
        return -1;
    const u8 *p = (const u8 *)data.data();
    if (crc16_raw(p, 32, 0xFFFF) == (u16)((p[32] << 8) | p[33]))
        return 0;
    if (crc16_raw(p + 2, 32, 0xFFFF) == (u16)((p[34] << 8) | p[35]))
        return 2;
    return -1;
}

Bytes strip_crcs(const Bytes &data)
{
    int off = detect_stripe_offset(data);
    if (off < 0)
        return data;
    size_t offset = (size_t)off;
    Bytes out;
    out.reserve(((data.size() - offset) / 34 + 1) * 32);
    const char *base = data.data();
    for (size_t i = offset; i < data.size(); i += 34) {
        size_t chunk = data.size() - i < 32 ? data.size() - i : 32;
        out.append(base + i, chunk);
    }
    return out;
}

Bytes decode_crc_prefix(const Bytes &data, size_t offset, long max_blocks, size_t &consumed)
{
    Bytes out;
    if (max_blocks > 0)
        out.reserve((size_t)max_blocks * 32);
    else if (offset < data.size())
        out.reserve(((data.size() - offset) / 34 + 1) * 32);
    size_t cursor = offset;
    long blocks = 0;
    const u8 *p = (const u8 *)data.data();
    while (cursor + 34 <= data.size() && (max_blocks < 0 || blocks < max_blocks)) {
        u16 calc = crc16_raw(p + cursor, 32, 0xFFFF);
        u16 exp = (u16)((p[cursor + 32] << 8) | p[cursor + 33]);
        if (calc != exp)
            break;
        out.append((const char *)(p + cursor), 32);
        cursor += 34;
        blocks++;
    }
    consumed = cursor - offset;
    return out;
}

/* ============================================================ base64 / key */
/* binascii.a2b_base64(s, strict_mode=True), i.e. base64.b64decode(s,
 * validate=True): only alphabet characters, whole quads, and padding only as
 * the final "xx==" / "xxx=" of the string. Any violation is an error. */
static bool b64_decode(const std::string &s, Bytes &out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    for (int i = 0; i < 256; i++)
        rev[i] = -1;
    for (int i = 0; i < 64; i++)
        rev[(u8)tbl[i]] = i;
    out.clear();
    u32 acc = 0;
    int quad_pos = 0, pads = 0;
    bool padding_started = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '=') {
            if (i == 0)
                return false; /* leading padding */
            padding_started = true;
            if (quad_pos == 0)
                return false; /* excess padding */
            if (quad_pos >= 2 && quad_pos + ++pads >= 4)
                return i + 1 == s.size(); /* nothing may follow the pad */
            continue;
        }
        int v = rev[(u8)c];
        if (v < 0 || padding_started)
            return false; /* non-alphabet data / discontinuous padding */
        pads = 0;
        acc = (acc << 6) | (u32)v;
        if (quad_pos == 1)
            out += (char)(u8)(acc >> 4);
        else if (quad_pos == 2)
            out += (char)(u8)(acc >> 2);
        else if (quad_pos == 3)
            out += (char)(u8)acc;
        quad_pos = (quad_pos + 1) & 3;
    }
    return quad_pos == 0;
}

const char *KNOWN_KEY_NAMES[] = {"TUYA", 0};
static const char *TUYA_KEY_B64 = "UQ+wk6PL6txZk6F+x63rAw==";

static bool is_hex32(const std::string &s)
{
    if (s.size() != 32)
        return false;
    for (size_t i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool parse_key(const std::string &spec, KeyCoefs &out, std::string *err)
{
    /* crypto.py parse_key(spec) for a spec that is not None: "no decryption"
     * is the caller not passing a key at all, so even "" is rejected here. */
    out = KeyCoefs();
    std::string name = py_strip(spec);
    Bytes key_bytes;
    if (upper(name) == "TUYA") {
        b64_decode(TUYA_KEY_B64, key_bytes);
    } else if (is_hex32(name)) {
        key_bytes = unhex(name, 0);
    } else {
        if (!b64_decode(name, key_bytes) || key_bytes.size() != 16) {
            if (err)
                *err = "Invalid -key value: " + py_repr(spec) +
                       ". Use a known name (TUYA), 32 hex characters, or base64 of 16 bytes.";
            return false;
        }
    }
    out.c0 = g32be(key_bytes, 0);
    out.c1 = g32be(key_bytes, 4);
    out.c2 = g32be(key_bytes, 8);
    out.c3 = g32be(key_bytes, 12);
    out.has = true;
    return true;
}

/* ============================================================ the cipher */
static u32 u16m(u32 x) { return x & 0xFFFF; }
static u32 u8m(u32 x) { return x & 0xFF; }

static inline u32 gen_pn15(u32 index_mask, bool flag)
{
    if (flag)
        return 0;
    const u32 AND_CONST = 0x6371;
    u32 val_rshift_5 = u16m(index_mask >> 5);
    u32 val_rshift_5_nibble = val_rshift_5 & 0xF;
    u32 xor_lhs = u16m(u16m(index_mask >> 7) + u16m(index_mask * 0x200));
    u32 xor_rhs = u16m(val_rshift_5 * 0x1000) + u16m(val_rshift_5_nibble * 0x100)
                  + u8m(val_rshift_5 * 0x10) + val_rshift_5_nibble;
    xor_rhs &= AND_CONST;
    return u16m(xor_lhs ^ xor_rhs);
}

static inline u32 gen_pn16(u32 index_mask, bool flag)
{
    if (flag)
        return 0;
    const u32 AND_CONST = 0x13659;
    u32 part1 = (((index_mask >> 13) & 1) + (((index_mask >> 9) & 1) * 2)
                 + (((index_mask >> 5) & 1) * 4) + (((index_mask >> 1) & 1) * 8));
    u32 xor_lhs = ((index_mask & 0x3FF) << 7) + ((index_mask >> 10) & 0x7F);
    u32 xor_rhs = (((index_mask >> 4) & 1) * 0x10000) + (part1 * 0x1111u);
    xor_rhs &= AND_CONST;
    return u16m(xor_lhs ^ xor_rhs);
}

static inline u32 gen_pn32(u32 index_mask, bool flag)
{
    if (flag)
        return 0;
    const u32 AND_CONST = 0xE519A4F1;
    u32 xor_lhs = (index_mask >> 0xF) | (index_mask << 0x11);
    u32 xor_rhs_start = (index_mask >> 2) & 0xF;
    u32 xor_rhs = xor_rhs_start * 0x11111111u;
    xor_rhs &= AND_CONST;
    return xor_lhs ^ xor_rhs;
}

BekenCodeCipher::BekenCodeCipher(u32 c0, u32 c1, u32 c2, u32 c3)
    : coef0_(c0), coef1_(c1), coef2_(c2), coef3_(c3), overflow_(false)
{
    bool highbyte = ((coef3_ & 0xFF000000) == 0xFF000000) || ((coef3_ & 0xFF000000) == 0);
    c1b_ = highbyte || ((coef3_ & 1) != 0);
    c2b_ = highbyte || ((coef3_ & 2) != 0);
    c4b_ = highbyte || ((coef3_ & 4) != 0);
    c8b_ = highbyte || ((coef3_ & 8) != 0);
    u32 coef3_4_rsh = coef3_ >> 4;
    coef3_5_rsh_ = (coef3_ >> 5) & 3;
    coef3_8_rsh_ = (coef3_ >> 8) & 3;
    u32 coef3_11_rsh = (coef3_ >> 11) & 3;
    static const u32 PN32_SHIFTS[4][2] = {{0, 0}, {8, 24}, {16, 16}, {24, 8}};
    pn32_sh0_ = PN32_SHIFTS[coef3_11_rsh][0];
    pn32_sh1_ = PN32_SHIFTS[coef3_11_rsh][1];
    coef1_hi_ = coef1_ >> 16;
    pn16_base_ = u8m(coef1_) + (u8m(coef1_ >> 8) * 0x200) + u8m(coef3_4_rsh & 1) * 0x100;
    final_val_ = c8b_ ? 0 : coef2_;
}

u32 BekenCodeCipher::encrypt_word(u32 index, u32 word) const
{
    u32 idx16 = u16m(index >> 16);
    u32 idxseq = u16m(index >> 8);
    u32 pn15_word;
    if (coef3_5_rsh_ == 0)
        pn15_word = (u8m(idx16) + u16m((index >> 24) << 8)) ^ u16m(index);
    else if (coef3_5_rsh_ == 1) {
        pn15_word = u8m(idx16) + u16m((index >> 24) << 8);
        pn15_word ^= u8m(idxseq) + u16m(index << 8);
    } else if (coef3_5_rsh_ == 2)
        pn15_word = ((idx16 >> 8) + u16m((index >> 16) << 8)) ^ u16m(index);
    else {
        pn15_word = (idx16 >> 8) + u16m((index >> 16) << 8);
        pn15_word ^= u8m(idxseq) + u16m(index << 8);
    }
    u32 pn16_word = (index >> coef3_8_rsh_) & 0x1FFFF;
    u32 pn32_word = (index >> pn32_sh0_) | (index << pn32_sh1_);
    u32 pn15_index_mask = u16m(coef1_hi_ ^ pn15_word);
    u32 pn16_index_mask = pn16_base_ ^ pn16_word;
    u32 pn32_index_mask = pn32_word ^ coef0_;
    u32 pn15_val = gen_pn15(pn15_index_mask, c1b_);
    u32 pn16_val = gen_pn16(pn16_index_mask, c2b_);
    u32 pn32_val = gen_pn32(pn32_index_mask, c4b_);
    /* pn16 is 17 bits wide, so with pn15 = 0xFFFF the mask can carry into
     * bit 32; Python's int then fails to_bytes(4) with an OverflowError. */
    u64 word_encryption_mask = (u64)pn15_val * 0x10000 + pn16_val;
    if (word_encryption_mask >> 32)
        overflow_ = true;
    return (u32)word_encryption_mask ^ pn32_val ^ final_val_ ^ word;
}

Bytes BekenCodeCipher::decrypt(const Bytes &data, size_t stream_start_offset, bool *overflow) const
{
    Bytes out;
    out.reserve((data.size() + 3) & ~(size_t)3);
    overflow_ = false;
    const u8 *src = (const u8 *)data.data();
    for (size_t i = 0; i < data.size(); i += 32) {
        size_t block_len = data.size() - i < 32 ? data.size() - i : 32;
        size_t block_start = i + stream_start_offset;
        /* range(0, len(block), 4): a trailing 1..3-byte word is still
         * processed (little-endian, zero-extended) and emitted as 4 bytes. */
        for (size_t j = 0; j < block_len; j += 4) {
            u32 word = 0;
            if (j + 4 <= block_len) {
                word = rd32le(src + i + j);
            } else {
                for (size_t k = 0; k < 4 && j + k < block_len; k++)
                    word |= (u32)src[i + j + k] << (8 * k);
            }
            u32 enc = encrypt_word((u32)(block_start + j), word);
            if (overflow_) {
                if (overflow)
                    *overflow = true;
                return out;
            }
            char buf[4];
            wr32le((u8 *)buf, enc);
            out.append(buf, 4);
        }
    }
    if (overflow)
        *overflow = false;
    return out;
}

/* ============================================================ ARM vectors */
bool looks_like_arm_vectors(const Bytes &data, size_t offset)
{
    if (data.size() < offset + 32)
        return false;
    int good = 0;
    for (int i = 0; i < 8; i++) {
        u32 word = g32le(data, offset + (size_t)i * 4);
        if ((word >> 24) == 0xEA || (word >> 16) == 0xE59F)
            good++;
    }
    return good >= 6;
}

std::vector<u32> arm_vector_destinations(const Bytes &data, size_t offset, u32 load_address)
{
    std::vector<u32> dest;
    if (data.size() < offset + 32)
        return dest;
    for (int i = 0; i < 8; i++) {
        size_t insn_offset = offset + (size_t)i * 4;
        u32 insn_address = load_address + (u32)i * 4;
        u32 word = g32le(data, insn_offset);
        if ((word >> 24) == 0xEA) {
            u32 imm24 = word & 0x00FFFFFF;
            s32 s = (imm24 & 0x00800000) ? (s32)(imm24 - 0x01000000) : (s32)imm24;
            dest.push_back((insn_address + 8 + ((u32)(s << 2))) & 0xFFFFFFFF);
            continue;
        }
        u32 opcode = word & 0xFFFFF000;
        if (opcode != 0xE59FF000 && opcode != 0xE51FF000)
            continue;
        u32 displacement = word & 0xFFF;
        long literal_offset = (long)insn_offset + 8;
        literal_offset += (word & (1u << 23)) ? (long)displacement : -(long)displacement;
        if ((long)offset <= literal_offset && literal_offset + 4 <= (long)data.size())
            dest.push_back(g32le(data, (size_t)literal_offset));
    }
    return dest;
}

static bool looks_like_linked_app_vectors(const Bytes &data, size_t offset, u32 load_address)
{
    if (!looks_like_arm_vectors(data, offset))
        return false;
    std::vector<u32> dest = arm_vector_destinations(data, offset, load_address);
    int n = 0;
    for (size_t i = 0; i < dest.size(); i++)
        if (0x10000 <= dest[i] && dest[i] < 0x200000)
            n++;
    return n >= 6;
}

bool detect_bk7252_app_only(const Bytes &data, Bytes &app_out, size_t &encoded_size)
{
    size_t consumed;
    Bytes full_app = decode_crc_prefix(data, 0x11000, 130, consumed);
    if (looks_like_linked_app_vectors(full_app, 0, 0x10000)
        || looks_like_linked_app_vectors(full_app, 0x100, 0x10100))
        return false;
    size_t enc;
    Bytes app = decode_crc_prefix(data, 0, -1, enc);
    if (app.size() < 0x10000
        || !(looks_like_linked_app_vectors(app, 0, 0x10000)
             || looks_like_linked_app_vectors(app, 0x100, 0x10100)))
        return false;
    app_out = app;
    encoded_size = enc;
    return true;
}

/* ========================================================== RBL extraction */
Bytes extract_and_decrypt(const Bytes &data, const std::string &target_name,
                          const KeyCoefs &coefs, void (*log)(const std::string &), std::string *error,
                          u32 bootloader_addr, u32 app_addr)
{
    if (error)
        error->clear();
    /* Find RBL containers by the 4-byte magic 02 19 9a 01. */
    std::vector<size_t> containers;
    size_t idx = 0;
    const std::string magic("\x02\x19\x9a\x01", 4);
    while (idx + 4 < data.size()) { /* while idx < len(data) - 4 */
        size_t pos = data.find(magic, idx);
        if (pos == std::string::npos)
            break;
        containers.push_back(pos);
        idx = pos + 4;
    }

    for (size_t ci = 0; ci < containers.size(); ci++) {
        size_t off = containers[ci];
        if (off + 96 > data.size())
            continue;
        /* struct "<4sII16s24s24sIIIII" (no padding): algo is field 1 at +4,
         * name field 3 at +12..+28, size_raw field 8 at +84, size_package
         * field 9 at +88. */
        u32 algo = g32le(data, off + 4);
        Bytes name_bytes = bslice(data, (long long)off + 12, (long long)off + 28);
        u32 size_raw = g32le(data, off + 84);
        u32 size_package = g32le(data, off + 88);
        size_t nul = name_bytes.find('\0');
        std::string raw_name = nul == std::string::npos ? name_bytes : name_bytes.substr(0, nul);
        std::string name; /* .decode("ascii", "ignore").strip() */
        for (size_t k = 0; k < raw_name.size(); k++)
            if ((u8)raw_name[k] < 0x80)
                name += raw_name[k];
        name = py_strip(name);
        if (name != target_name)
            continue;
        u32 mapped_address = target_name == "bootloader" ? bootloader_addr : app_addr;
        Bytes payload = bslice(data, mapped_address, (long long)mapped_address + (long long)size_package);
        if (algo == 0) {
            long long padding = (long long)size_package - (long long)size_raw;
            if (padding > 255) { /* bytes([padding]) */
                if (error)
                    *error = "ValueError: bytes must be in range(0, 256)";
                return Bytes();
            }
            if (padding > 0)
                payload = bhead(payload, (long long)size_raw) + Bytes((size_t)padding, (char)(u8)padding);
        }
        if (!coefs.has)
            return payload;
        BekenCodeCipher cipher(coefs.c0, coefs.c1, coefs.c2, coefs.c3);
        bool overflow = false;
        Bytes out = cipher.decrypt(payload, mapped_address, &overflow);
        if (overflow) {
            if (error)
                *error = "OverflowError: int too big to convert";
            return Bytes();
        }
        return out;
    }

    /* No container: fall back to a fixed slice. */
    Bytes payload;
    u32 mapped_address;
    std::vector<size_t> vector_offsets;
    if (target_name == "bootloader") {
        payload = bslice(data, bootloader_addr, app_addr);
        mapped_address = bootloader_addr;
        vector_offsets.push_back(0);
    } else {
        payload = bslice(data, app_addr, 0x200000);
        mapped_address = app_addr;
        vector_offsets.push_back(0);
        vector_offsets.push_back(0x100);
        vector_offsets.push_back(0x1000);
    }
    /* These warnings use print() in crypto.py, i.e. they go to stdout even
     * under --only-uart (the self-test "encrypted image warns" case relies on
     * seeing them). Hence out_write, not the optional `log`. */
    Bytes result;
    (void)log;
    if (!coefs.has) {
        out_write(strf("Warning: '%s' RBL container not found. No key given, using raw slice without decryption.\n",
                       target_name.c_str()));
        result = payload;
    } else {
        out_write(strf("Warning: '%s' RBL container not found. Assuming raw payload and decrypting with the given key.\n",
                       target_name.c_str()));
        bool overflow = false;
        result = BekenCodeCipher(coefs.c0, coefs.c1, coefs.c2, coefs.c3).decrypt(payload, mapped_address, &overflow);
        if (overflow) {
            out_flush();
            if (error)
                *error = "OverflowError: int too big to convert";
            return Bytes();
        }
    }
    out_flush();
    bool ok = false;
    for (size_t i = 0; i < vector_offsets.size(); i++)
        if (looks_like_arm_vectors(result, vector_offsets[i])) {
            ok = true;
            break;
        }
    if (!ok) {
        const char *hint = coefs.has ? "wrong key for this image?" : "encrypted image? try: -key TUYA";
        out_write(strf("Warning: '%s' slice does not look like ARM code (%s).\n", target_name.c_str(), hint));
        out_flush();
    }
    return result;
}
