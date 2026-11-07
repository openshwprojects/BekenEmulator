/* Flash-image extraction and the Beken firmware cipher. Port of src/crypto.py:
 * CRC16 stripe handling, the BekenCodeCipher stream cipher, RBL container
 * location + decryption, named decryption keys, and the BK7252 app-only
 * dump heuristic.
 */
#ifndef BK_BEKEN_CRYPTO_H
#define BK_BEKEN_CRYPTO_H

#include "common.h"

#include <stdio.h>

u16 crc16(const Bytes &data, u16 initial_value = 0x0000);
bool block_crc_check(const Bytes &block, const Bytes &crc_bytes);
int detect_stripe_offset(const Bytes &data);
Bytes strip_crcs(const Bytes &data);
/* decode_crc_prefix: contiguous valid 32+2 run at offset. Fills consumed with
 * encoded bytes used. max_blocks < 0 means unbounded. */
Bytes decode_crc_prefix(const Bytes &data, size_t offset, long max_blocks, size_t &consumed);

/* The four cipher coefficients from a key, or has=false for "no decryption". */
struct KeyCoefs {
    u32 c0, c1, c2, c3;
    bool has;
    KeyCoefs() : c0(0), c1(0), c2(0), c3(0), has(false) {}
};

/* parse_key: a known name (TUYA), 32 hex chars, or base64 of 16 bytes.
 * Returns has=false for an empty spec (no decryption). Throws BkError text in
 * *err and returns has=false,ok=false on an invalid spec. */
bool parse_key(const std::string &spec, KeyCoefs &out, std::string *err);

class BekenCodeCipher {
public:
    BekenCodeCipher(u32 c0, u32 c1, u32 c2, u32 c3);
    /* *overflow is set where Python raises OverflowError (a keystream word
     * wider than 32 bits; the output then stops there). */
    Bytes decrypt(const Bytes &data, size_t stream_start_offset = 0, bool *overflow = 0) const;
private:
    u32 coef0_, coef1_, coef2_, coef3_;
    bool c1b_, c2b_, c4b_, c8b_;
    u32 coef3_5_rsh_, coef3_8_rsh_, pn32_sh0_, pn32_sh1_;
    u32 coef1_hi_, pn16_base_, final_val_;
    mutable bool overflow_;
    u32 encrypt_word(u32 index, u32 word) const;
};

/* extract_and_decrypt: pull the "bootloader" / "app" slice, decrypting with
 * coefs when coefs.has. Its warnings go to stdout like crypto.py's print().
 * *error receives the text of an exception crypto.py would raise instead
 * ("ValueError: ..." / "OverflowError: ..."), with an empty result. */
Bytes extract_and_decrypt(const Bytes &data, const std::string &target_name,
                          const KeyCoefs &coefs, void (*log)(const std::string &) = 0,
                          std::string *error = 0,
                          u32 bootloader_addr = 0x00000000, u32 app_addr = 0x00010000);

bool looks_like_arm_vectors(const Bytes &data, size_t offset = 0);
/* _arm_vector_destinations: the direct B / LDR-PC targets of an ARM vector
 * table at `offset` loaded at `load_address` (empty if the data is too short). */
std::vector<u32> arm_vector_destinations(const Bytes &data, size_t offset = 0, u32 load_address = 0);

/* detect_bk7252_app_only: returns true and fills app+encoded_size for a
 * BK7252 app-window dump (file offset 0 == physical 0x11000). */
bool detect_bk7252_app_only(const Bytes &data, Bytes &app, size_t &encoded_size);

extern const char *KNOWN_KEY_NAMES[]; /* NULL-terminated, for help text */

/* Python str semantics for the UTF-8 strings the command line hands over
 * (main.py / crypto.py validate their arguments with these). Undecodable
 * bytes travel as surrogateescape code points 0xDC80..0xDCFF. In their own
 * namespace: the GUI and the self-test runner carry py_repr()s of their own. */
namespace pyc {
std::vector<u32> py_codepoints(const std::string &s);
std::string py_utf8(u32 cp);
size_t py_len(const std::string &s);           /* len(str) */
bool py_isascii(const std::string &s);         /* str.isascii() */
bool py_isspace(u32 cp);                       /* str.isspace() of one char */
std::string py_strip(const std::string &s);    /* str.strip() */
std::string py_repr(const std::string &s);     /* repr(str) */
/* int(text, base), base 0/2/8/10/16; *huge flags a value beyond 63 bits */
bool py_int(const std::string &text, int base, long long &out, bool *huge = 0);
std::string py_int_error(const std::string &text, int base); /* its ValueError text */
/* open(path, mode) on a UTF-8 path; *err gets the errno of a failure */
FILE *py_open(const std::string &path, const char *mode, int *err);
/* str(OSError) "[Errno N] text: 'path'"; *exc_type gets the class name */
std::string py_oserror(int err, const std::string &path, std::string *exc_type = 0);
} /* namespace pyc */

#endif
