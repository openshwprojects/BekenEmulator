#include "crypto_prims.h"

#include <string.h>

/* ==================================================================== AES */
static const u8 SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static u8 INV_SBOX[256];
static const u8 RCON[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static u8 gmul(u8 a, u8 b)
{
    u8 p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            p ^= a;
        u8 hi = a & 0x80;
        a = (u8)(a << 1);
        if (hi)
            a ^= 0x1b;
        b = (u8)(b >> 1);
    }
    return p;
}

/* InvMixColumns products, precomputed: the bit-loop gmul made decrypting a
 * 2 MB Tuya storage area take over a second. */
static u8 MUL9[256], MUL11[256], MUL13[256], MUL14[256];

static void init_inv_sbox()
{
    static bool done = false;
    if (done)
        return;
    for (int i = 0; i < 256; i++) {
        INV_SBOX[SBOX[i]] = (u8)i;
        MUL9[i] = gmul((u8)i, 0x09);
        MUL11[i] = gmul((u8)i, 0x0b);
        MUL13[i] = gmul((u8)i, 0x0d);
        MUL14[i] = gmul((u8)i, 0x0e);
    }
    done = true;
}

Aes::Aes() : rounds_(0) { init_inv_sbox(); }
Aes::Aes(const Bytes &key) : rounds_(0) { init_inv_sbox(); set_key(key); }

bool Aes::set_key(const Bytes &key)
{
    int nk;
    if (key.size() == 16)
        nk = 4, rounds_ = 10;
    else if (key.size() == 24)
        nk = 6, rounds_ = 12;
    else if (key.size() == 32)
        nk = 8, rounds_ = 14;
    else {
        rounds_ = 0;
        return false;
    }
    int total = 4 * (rounds_ + 1);
    for (int i = 0; i < nk; i++)
        ek_[i] = rd32be(key.data() + i * 4);
    for (int i = nk; i < total; i++) {
        u32 t = ek_[i - 1];
        if (i % nk == 0) {
            t = (t << 8) | (t >> 24); /* RotWord */
            t = ((u32)SBOX[(t >> 24) & 0xFF] << 24) | ((u32)SBOX[(t >> 16) & 0xFF] << 16)
                | ((u32)SBOX[(t >> 8) & 0xFF] << 8) | (u32)SBOX[t & 0xFF];
            t ^= (u32)RCON[i / nk] << 24;
        } else if (nk > 6 && i % nk == 4) {
            t = ((u32)SBOX[(t >> 24) & 0xFF] << 24) | ((u32)SBOX[(t >> 16) & 0xFF] << 16)
                | ((u32)SBOX[(t >> 8) & 0xFF] << 8) | (u32)SBOX[t & 0xFF];
        }
        ek_[i] = ek_[i - nk] ^ t;
    }
    return true;
}

void Aes::encrypt_block(const u8 in[16], u8 out[16]) const
{
    u8 s[16];
    memcpy(s, in, 16);
    for (int c = 0; c < 4; c++) {
        u32 k = ek_[c];
        s[c * 4] ^= (u8)(k >> 24);
        s[c * 4 + 1] ^= (u8)(k >> 16);
        s[c * 4 + 2] ^= (u8)(k >> 8);
        s[c * 4 + 3] ^= (u8)k;
    }
    for (int r = 1; r <= rounds_; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++)
            t[i] = SBOX[s[i]];
        /* ShiftRows (column-major state: byte index = col*4 + row) */
        u8 a[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                a[col * 4 + row] = t[((col + row) % 4) * 4 + row];
        if (r != rounds_) {
            for (int col = 0; col < 4; col++) {
                u8 *p = &a[col * 4];
                u8 b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3];
                s[col * 4] = (u8)(xtime(b0) ^ (xtime(b1) ^ b1) ^ b2 ^ b3);
                s[col * 4 + 1] = (u8)(b0 ^ xtime(b1) ^ (xtime(b2) ^ b2) ^ b3);
                s[col * 4 + 2] = (u8)(b0 ^ b1 ^ xtime(b2) ^ (xtime(b3) ^ b3));
                s[col * 4 + 3] = (u8)((xtime(b0) ^ b0) ^ b1 ^ b2 ^ xtime(b3));
            }
        } else {
            memcpy(s, a, 16);
        }
        for (int c = 0; c < 4; c++) {
            u32 k = ek_[r * 4 + c];
            s[c * 4] ^= (u8)(k >> 24);
            s[c * 4 + 1] ^= (u8)(k >> 16);
            s[c * 4 + 2] ^= (u8)(k >> 8);
            s[c * 4 + 3] ^= (u8)k;
        }
    }
    memcpy(out, s, 16);
}

void Aes::decrypt_block(const u8 in[16], u8 out[16]) const
{
    u8 s[16];
    memcpy(s, in, 16);
    for (int c = 0; c < 4; c++) {
        u32 k = ek_[rounds_ * 4 + c];
        s[c * 4] ^= (u8)(k >> 24);
        s[c * 4 + 1] ^= (u8)(k >> 16);
        s[c * 4 + 2] ^= (u8)(k >> 8);
        s[c * 4 + 3] ^= (u8)k;
    }
    for (int r = rounds_ - 1; r >= 0; r--) {
        /* InvShiftRows then InvSubBytes */
        u8 a[16];
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                a[col * 4 + row] = s[((col - row + 4) % 4) * 4 + row];
        u8 t[16];
        for (int i = 0; i < 16; i++)
            t[i] = INV_SBOX[a[i]];
        for (int c = 0; c < 4; c++) {
            u32 k = ek_[r * 4 + c];
            t[c * 4] ^= (u8)(k >> 24);
            t[c * 4 + 1] ^= (u8)(k >> 16);
            t[c * 4 + 2] ^= (u8)(k >> 8);
            t[c * 4 + 3] ^= (u8)k;
        }
        if (r != 0) {
            for (int col = 0; col < 4; col++) {
                u8 *p = &t[col * 4];
                u8 b0 = p[0], b1 = p[1], b2 = p[2], b3 = p[3];
                s[col * 4] = (u8)(MUL14[b0] ^ MUL11[b1] ^ MUL13[b2] ^ MUL9[b3]);
                s[col * 4 + 1] = (u8)(MUL9[b0] ^ MUL14[b1] ^ MUL11[b2] ^ MUL13[b3]);
                s[col * 4 + 2] = (u8)(MUL13[b0] ^ MUL9[b1] ^ MUL14[b2] ^ MUL11[b3]);
                s[col * 4 + 3] = (u8)(MUL11[b0] ^ MUL13[b1] ^ MUL9[b2] ^ MUL14[b3]);
            }
        } else {
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
}

Bytes Aes::ecb_encrypt(const Bytes &data) const
{
    Bytes out(data.size(), '\0');
    for (size_t i = 0; i + 16 <= data.size(); i += 16)
        encrypt_block((const u8 *)data.data() + i, (u8 *)&out[i]);
    return out;
}
Bytes Aes::ecb_decrypt(const Bytes &data) const
{
    Bytes out(data.size(), '\0');
    for (size_t i = 0; i + 16 <= data.size(); i += 16)
        decrypt_block((const u8 *)data.data() + i, (u8 *)&out[i]);
    return out;
}

Bytes aes_ecb_encrypt(const Bytes &key, const Bytes &data) { return Aes(key).ecb_encrypt(data); }
Bytes aes_ecb_decrypt(const Bytes &key, const Bytes &data) { return Aes(key).ecb_decrypt(data); }

/* ================================================================ AES-GCM */
static void gf_mul(const u8 x[16], const u8 y[16], u8 out[16])
{
    u8 z[16];
    memset(z, 0, 16);
    u8 v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7)))
            for (int j = 0; j < 16; j++)
                z[j] ^= v[j];
        u8 lsb = v[15] & 1;
        for (int j = 15; j > 0; j--)
            v[j] = (u8)((v[j] >> 1) | ((v[j - 1] & 1) << 7));
        v[0] >>= 1;
        if (lsb)
            v[0] ^= 0xe1;
    }
    memcpy(out, z, 16);
}

static void ghash(const u8 H[16], const Bytes &aad, const Bytes &cipher, u8 out[16])
{
    u8 y[16];
    memset(y, 0, 16);
    Bytes blocks = aad;
    while (blocks.size() % 16)
        blocks += '\0';
    size_t aad_pad = blocks.size();
    blocks += cipher;
    while (blocks.size() % 16)
        blocks += '\0';
    for (size_t i = 0; i < blocks.size(); i += 16) {
        for (int j = 0; j < 16; j++)
            y[j] ^= (u8)blocks[i + j];
        u8 t[16];
        gf_mul(y, H, t);
        memcpy(y, t, 16);
    }
    (void)aad_pad;
    u8 lens[16];
    wr64be(lens, (u64)aad.size() * 8);
    wr64be(lens + 8, (u64)cipher.size() * 8);
    for (int j = 0; j < 16; j++)
        y[j] ^= lens[j];
    u8 t[16];
    gf_mul(y, H, t);
    memcpy(out, t, 16);
}

static void gctr(const Aes &aes, const u8 icb[16], const Bytes &in, Bytes &out)
{
    out.assign(in.size(), '\0');
    u8 cb[16];
    memcpy(cb, icb, 16);
    for (size_t i = 0; i < in.size(); i += 16) {
        u8 ks[16];
        aes.encrypt_block(cb, ks);
        size_t n = in.size() - i < 16 ? in.size() - i : 16;
        for (size_t j = 0; j < n; j++)
            out[i + j] = (char)((u8)in[i + j] ^ ks[j]);
        /* increment the 32-bit counter in the low word */
        for (int k = 15; k >= 12; k--)
            if (++cb[k])
                break;
    }
}

static void gcm_setup(const Bytes &key, const Bytes &nonce, Aes &aes, u8 H[16], u8 J0[16])
{
    aes.set_key(key);
    u8 zero[16];
    memset(zero, 0, 16);
    aes.encrypt_block(zero, H);
    if (nonce.size() == 12) {
        memcpy(J0, nonce.data(), 12);
        J0[12] = J0[13] = J0[14] = 0;
        J0[15] = 1;
    } else {
        ghash(H, Bytes(), nonce, J0);
    }
}

bool aes_gcm_encrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad,
                     const Bytes &plaintext, Bytes &ciphertext, u8 tag[16])
{
    Aes aes;
    u8 H[16], J0[16];
    gcm_setup(key, nonce, aes, H, J0);
    u8 cb[16];
    memcpy(cb, J0, 16);
    for (int k = 15; k >= 12; k--)
        if (++cb[k])
            break;
    gctr(aes, cb, plaintext, ciphertext);
    u8 S[16];
    ghash(H, aad, ciphertext, S);
    u8 ej0[16];
    aes.encrypt_block(J0, ej0);
    for (int j = 0; j < 16; j++)
        tag[j] = (u8)(S[j] ^ ej0[j]);
    return true;
}

bool aes_gcm_decrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad,
                     const Bytes &ciphertext, const u8 tag[16], Bytes &plaintext)
{
    Aes aes;
    u8 H[16], J0[16];
    gcm_setup(key, nonce, aes, H, J0);
    u8 S[16];
    ghash(H, aad, ciphertext, S);
    u8 ej0[16];
    aes.encrypt_block(J0, ej0);
    u8 want[16];
    for (int j = 0; j < 16; j++)
        want[j] = (u8)(S[j] ^ ej0[j]);
    u8 diff = 0;
    for (int j = 0; j < 16; j++)
        diff |= (u8)(want[j] ^ tag[j]);
    if (diff)
        return false;
    u8 cb[16];
    memcpy(cb, J0, 16);
    for (int k = 15; k >= 12; k--)
        if (++cb[k])
            break;
    gctr(aes, cb, ciphertext, plaintext);
    return true;
}

/* =================================================================== SHA-1 */
static u32 rol32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

Bytes sha1(const Bytes &data)
{
    u32 h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    Bytes msg = data;
    u64 ml = (u64)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56)
        msg += '\0';
    msg += p64be(ml);
    for (size_t off = 0; off < msg.size(); off += 64) {
        u32 w[80];
        for (int i = 0; i < 16; i++)
            w[i] = rd32be(msg.data() + off + i * 4);
        for (int i = 16; i < 80; i++)
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            u32 t = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = t;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    Bytes out;
    out += p32be(h0);
    out += p32be(h1);
    out += p32be(h2);
    out += p32be(h3);
    out += p32be(h4);
    return out;
}

/* ================================================================= SHA-256 */
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

void sha256_compress(u32 h[8], const u8 p[64])
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = rd32be(p + i * 4);
    for (int i = 16; i < 64; i++) {
        u32 s0 = rol32(w[i - 15], 25) ^ rol32(w[i - 15], 14) ^ (w[i - 15] >> 3);
        u32 s1 = rol32(w[i - 2], 15) ^ rol32(w[i - 2], 13) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 S1 = rol32(e, 26) ^ rol32(e, 21) ^ rol32(e, 7);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + S1 + ch + K256[i] + w[i];
        u32 S0 = rol32(a, 30) ^ rol32(a, 19) ^ rol32(a, 10);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

Sha256::Sha256() { reset(); }
void Sha256::reset()
{
    h_[0] = 0x6a09e667;
    h_[1] = 0xbb67ae85;
    h_[2] = 0x3c6ef372;
    h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f;
    h_[5] = 0x9b05688c;
    h_[6] = 0x1f83d9ab;
    h_[7] = 0x5be0cd19;
    len_ = 0;
    buf_len_ = 0;
}
void Sha256::update(const u8 *data, size_t len)
{
    len_ += len;
    while (len) {
        size_t take = 64 - buf_len_ < len ? 64 - buf_len_ : len;
        memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ == 64) {
            sha256_compress(h_, buf_);
            buf_len_ = 0;
        }
    }
}
void Sha256::update(const Bytes &data) { update((const u8 *)data.data(), data.size()); }
Bytes Sha256::digest() const
{
    u32 h[8];
    memcpy(h, h_, sizeof(h));
    u8 buf[64];
    memcpy(buf, buf_, buf_len_);
    size_t bl = buf_len_;
    u64 total = len_ * 8;
    buf[bl++] = 0x80;
    if (bl > 56) {
        while (bl < 64)
            buf[bl++] = 0;
        sha256_compress(h, buf);
        bl = 0;
    }
    while (bl < 56)
        buf[bl++] = 0;
    wr64be(buf + 56, total);
    sha256_compress(h, buf);
    Bytes out;
    for (int i = 0; i < 8; i++)
        out += p32be(h[i]);
    return out;
}

Bytes sha256(const Bytes &data)
{
    Sha256 s;
    s.update(data);
    return s.digest();
}

/* ==================================================================== MD5 */
static void md5_transform(u32 buf[4], const u32 in[16])
{
    static const u32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    u32 A = buf[0], B = buf[1], C = buf[2], D = buf[3];
    for (int i = 0; i < 64; i++) {
        u32 F;
        int g;
        if (i < 16) {
            F = (B & C) | (~B & D);
            g = i;
        } else if (i < 32) {
            F = (D & B) | (~D & C);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            F = B ^ C ^ D;
            g = (3 * i + 5) % 16;
        } else {
            F = C ^ (B | ~D);
            g = (7 * i) % 16;
        }
        F = F + A + K[i] + in[g];
        A = D;
        D = C;
        C = B;
        B = B + rol32(F, S[i]);
    }
    buf[0] += A;
    buf[1] += B;
    buf[2] += C;
    buf[3] += D;
}

void md5_init(Md5Context *ctx)
{
    ctx->buf[0] = 0x67452301;
    ctx->buf[1] = 0xefcdab89;
    ctx->buf[2] = 0x98badcfe;
    ctx->buf[3] = 0x10325476;
    ctx->bits[0] = 0;
    ctx->bits[1] = 0;
    memset(ctx->in, 0, 64);
}

void md5_update(Md5Context *ctx, const u8 *data, size_t len)
{
    u32 t = ctx->bits[0];
    if ((ctx->bits[0] = t + ((u32)len << 3)) < t)
        ctx->bits[1]++;
    ctx->bits[1] += (u32)(len >> 29);

    u32 off = (t >> 3) & 0x3f;
    if (off) {
        u8 *p = ctx->in + off;
        size_t part = 64 - off;
        if (len < part) {
            memcpy(p, data, len);
            return;
        }
        memcpy(p, data, part);
        u32 in32[16];
        for (int i = 0; i < 16; ++i) in32[i] = rd32le(ctx->in + i * 4);
        md5_transform(ctx->buf, in32);
        data += part;
        len -= part;
    }
    while (len >= 64) {
        u32 in32[16];
        for (int i = 0; i < 16; ++i) in32[i] = rd32le(data + i * 4);
        md5_transform(ctx->buf, in32);
        data += 64;
        len -= 64;
    }
    if (len) memcpy(ctx->in, data, len);
}

void md5_final(u8 digest[16], Md5Context *ctx)
{
    unsigned count = (ctx->bits[0] >> 3) & 0x3f;
    u8 *p = ctx->in + count;
    *p++ = 0x80;
    count = 64 - 1 - count;
    if (count < 8) {
        memset(p, 0, count);
        u32 in32[16];
        for (int i = 0; i < 16; ++i) in32[i] = rd32le(ctx->in + i * 4);
        md5_transform(ctx->buf, in32);
        memset(ctx->in, 0, 56);
    } else {
        memset(p, 0, count - 8);
    }
    wr32le(ctx->in + 56, ctx->bits[0]);
    wr32le(ctx->in + 60, ctx->bits[1]);
    u32 in32[16];
    for (int i = 0; i < 16; ++i) in32[i] = rd32le(ctx->in + i * 4);
    md5_transform(ctx->buf, in32);
    for (int i = 0; i < 4; ++i) wr32le(digest + i * 4, ctx->buf[i]);
    memset(ctx, 0, sizeof(*ctx));
}

Bytes md5(const Bytes &data)
{
    Md5Context ctx;
    md5_init(&ctx);
    md5_update(&ctx, (const u8 *)data.data(), data.size());
    u8 dig[16];
    md5_final(dig, &ctx);
    return Bytes((const char *)dig, 16);
}

/* ================================================================== HMAC */
static Bytes hmac_generic(const Bytes &key, const Bytes &msg, int block, Bytes (*hash)(const Bytes &))
{
    Bytes k = key;
    if ((int)k.size() > block)
        k = hash(k);
    k.resize((size_t)block, '\0');
    Bytes ipad(block, '\0'), opad(block, '\0');
    for (int i = 0; i < block; i++) {
        ipad[i] = (char)((u8)k[i] ^ 0x36);
        opad[i] = (char)((u8)k[i] ^ 0x5c);
    }
    return hash(opad + hash(ipad + msg));
}
Bytes hmac_sha1(const Bytes &key, const Bytes &msg) { return hmac_generic(key, msg, 64, sha1); }
Bytes hmac_sha256(const Bytes &key, const Bytes &msg) { return hmac_generic(key, msg, 64, sha256); }

Bytes pbkdf2_hmac_sha1(const Bytes &password, const Bytes &salt, int iterations, int dklen)
{
    Bytes out;
    u32 block = 1;
    while ((int)out.size() < dklen) {
        Bytes u = hmac_sha1(password, salt + p32be(block));
        Bytes t = u;
        for (int i = 1; i < iterations; i++) {
            u = hmac_sha1(password, u);
            for (size_t j = 0; j < t.size(); j++)
                t[j] = (char)((u8)t[j] ^ (u8)u[j]);
        }
        out += t;
        block++;
    }
    out.resize((size_t)dklen);
    return out;
}

bool ct_equal(const Bytes &a, const Bytes &b)
{
    if (a.size() != b.size())
        return false;
    u8 d = 0;
    for (size_t i = 0; i < a.size(); i++)
        d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}
