/* Standard primitives replacing the pycryptodome / hashlib / hmac calls in the
 * Python version: AES block cipher (ECB helpers and GCM mode), SHA-1, SHA-256,
 * MD5, HMAC and PBKDF2-HMAC-SHA1. Used by the Tuya flash-config reader, the
 * BLE core's AES engine, the TLS-PSK responder and the WPA2 handshake.
 */
#ifndef BK_CRYPTO_PRIMS_H
#define BK_CRYPTO_PRIMS_H

#include "common.h"

class Aes {
public:
    Aes();
    explicit Aes(const Bytes &key);       /* 16, 24 or 32 bytes */
    bool set_key(const Bytes &key);
    void encrypt_block(const u8 in[16], u8 out[16]) const;
    void decrypt_block(const u8 in[16], u8 out[16]) const;
    Bytes ecb_encrypt(const Bytes &data) const; /* length a multiple of 16 */
    Bytes ecb_decrypt(const Bytes &data) const;
private:
    u32 ek_[60];
    int rounds_;
};

Bytes aes_ecb_encrypt(const Bytes &key, const Bytes &data);
Bytes aes_ecb_decrypt(const Bytes &key, const Bytes &data);

/* AES-GCM, matching pycryptodome's MODE_GCM used in netstack's TLS layer.
 * encrypt returns ciphertext and fills tag[16]; decrypt verifies the tag and
 * returns false on a mismatch. nonce is the 12-byte IV the TLS record builds. */
bool aes_gcm_encrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad,
                     const Bytes &plaintext, Bytes &ciphertext, u8 tag[16]);
bool aes_gcm_decrypt(const Bytes &key, const Bytes &nonce, const Bytes &aad,
                     const Bytes &ciphertext, const u8 tag[16], Bytes &plaintext);

/* Hashes: one-shot helpers returning the raw digest. */
Bytes sha1(const Bytes &data);
Bytes sha256(const Bytes &data);
/* One SHA-256 compression block. State is eight native words; block is
 * exactly 64 bytes in message order. No padding or length is added. */
void sha256_compress(u32 state[8], const u8 block[64]);
Bytes md5(const Bytes &data);

struct Md5Context {
    u32 buf[4];
    u32 bits[2];
    u8 in[64];
};
void md5_init(Md5Context *ctx);
void md5_update(Md5Context *ctx, const u8 *data, size_t len);
void md5_final(u8 digest[16], Md5Context *ctx);

/* Incremental SHA-256, for the running TLS transcript hash. */
class Sha256 {
public:
    Sha256();
    void reset();
    void update(const Bytes &data);
    void update(const u8 *data, size_t len);
    Bytes digest() const; /* non-destructive: a copy finalizes */
private:
    u32 h_[8];
    u64 len_;
    u8 buf_[64];
    size_t buf_len_;
};

Bytes hmac_sha1(const Bytes &key, const Bytes &msg);
Bytes hmac_sha256(const Bytes &key, const Bytes &msg);
Bytes pbkdf2_hmac_sha1(const Bytes &password, const Bytes &salt, int iterations, int dklen);

/* Constant-time compare for MIC / Finished verification (hmac.compare_digest). */
bool ct_equal(const Bytes &a, const Bytes &b);

#endif
