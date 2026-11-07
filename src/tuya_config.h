/* Recover Tuya's on-flash configuration from a BK7231 dump. Full port of
 * src/tuya_config.py:
 *
 *   1. find the key block (AES-ECB under the fixed master key, MAGIC_KEY),
 *   2. derive the data key from KEY_PART_1/KEY_PART_2 + the inner key,
 *   3. decrypt the storage area and pull out the records: the network block,
 *      the datapoint schema, the pin/user_param_key map, the cloud identities.
 *
 * Python's dict results map onto the structs below:
 *   _collect()  -> Collected   {"network","dps","pins","ids","raw","tls_mode"}
 *   extract()   -> Decoded     {"offset","encrypted","raw","pairs","network",
 *                               "dps","ids","tls_mode","human"}; Python's
 *                               `None` return is Decoded::valid == false.
 */
#ifndef BK_TUYA_CONFIG_H
#define BK_TUYA_CONFIG_H

#include "common.h"
#include "json.h"
#include <map>

namespace tuya_config {

extern const u32 STORAGE_FROM; /* 0x1C0000 */
extern const u32 BLOCK;        /* 0x1000 */

/* REDACT_SECRETS (module global in Python): mask secrets in summaries and in
 * the raw network record. Default true. */
extern bool REDACT_SECRETS;

/* (key, value) list - the "pairs"/"pins" entries and the "human" summary rows. */
typedef std::vector<std::pair<std::string, std::string> > Pairs;

/* An insertion-ordered str->str dict (Python's "ids" dict). */
struct StrDict {
    std::vector<std::pair<std::string, std::string> > items;
    bool has(const std::string &k) const { return find(k) != 0; }
    size_t count(const std::string &k) const { return has(k) ? 1 : 0; }
    const std::string *find(const std::string &k) const;
    /* dict.get(k, def) */
    std::string get(const std::string &k, const std::string &def = "") const;
    void set(const std::string &k, const std::string &v);       /* d[k] = v */
    void setdefault(const std::string &k, const std::string &v); /* d.setdefault */
    void erase(const std::string &k);
    bool empty() const { return items.empty(); }
    size_t size() const { return items.size(); }
};

/* Python's `found` dict from _collect(). */
struct Collected {
    bool has_network;
    JsonValue network;            /* the first {"nc_tp"/"ssid"} record */
    std::vector<JsonValue> dps;   /* datapoint schema records, dedup'd by id */
    Pairs pins;                   /* user_param_key (loose) pairs */
    StrDict ids;                  /* uuid/auth_key/psk*_key/schemaId/devId/... */
    StrList raw;                  /* raw record texts for the report */
    bool has_tls_mode;
    long long tls_mode;           /* dy_tls_m */
    Collected() : has_network(false), has_tls_mode(false), tls_mode(0) {}
};

/* Python's extract() result. */
struct Decoded {
    bool valid;                   /* false == Python returned None */
    u32 offset;                   /* key block offset, or STORAGE_FROM */
    bool encrypted;
    std::string raw;              /* "\n\n".join of the first 6 non-empty raw texts */
    Pairs pairs;                  /* the pin map */
    bool has_network;
    JsonValue network;
    std::vector<JsonValue> dps;
    StrDict ids;
    bool has_tls_mode;
    long long tls_mode;
    Pairs human;                  /* summarise(): (label, value) rows */
    Decoded() : valid(false), offset(0), encrypted(false), has_network(false),
                has_tls_mode(false), tls_mode(0) {}
};

/* find_key_block: (offset, inner_key) or false for (None, None). */
bool find_key_block(const Bytes &data, u32 &off_out, Bytes &inner_out);
/* decrypt_storage: the whole storage area decrypted with the derived key. */
Bytes decrypt_storage(const Bytes &data, const Bytes &inner_key);
/* _json_objects: balanced {...} spans of printable text as (offset, blob). */
std::vector<std::pair<size_t, Bytes> > json_objects(const Bytes &buf, size_t limit = 4000);
/* _decode: json.loads(blob); false when it does not parse. */
bool decode(const Bytes &blob, JsonValue &out);
/* _stored_id_value: one bounded ASCII value for `name`, or false (None). */
bool stored_id_value(const Bytes &buf, const std::string &name, std::string &out);
/* _network_raw */
std::string network_raw(const JsonValue &obj, const std::string &original);
/* _split_loose_fields / _parse_loose */
StrList split_loose_fields(const std::string &text);
Pairs parse_loose(const std::string &text);
/* _collect */
Collected collect(const Bytes &buf);

/* describe_pin: the tool's bullet text, false (None) for a non-pin key. */
bool describe_pin(const std::string &key, const std::string &value, std::string &out);
/* _describe: (label, pretty value) */
std::pair<std::string, std::string> describe(const std::string &key, const std::string &value);
/* _redact (of a str value) */
std::string redact(const std::string &value);
/* Python str() of a decoded JSON value (repr() for containers). */
std::string py_str(const JsonValue &v);

/* summarise(found, key_off, inner_key, encrypted): has_key_off=false is
 * key_off=None; an empty inner_key is None. */
Pairs summarise(const Collected &found, bool has_key_off, u32 key_off,
                const Bytes &inner_key, bool encrypted);

/* extract(path) */
Decoded extract(const std::string &path);

/* Private TLS-PSK byte candidates (PSK 3.0 first, deduplicated). An invalid
 * Decoded (Python None) yields none. */
std::vector<Bytes> tls_psk_candidates(const Decoded &d);
std::vector<Bytes> tls_psk_candidates(const StrDict &ids);
/* Tuya's legacy identity-as-PSK trial gate (tls_mode 2, 37-char psk_key). */
bool uses_tuya_legacy_identity_psk(const Decoded &d);
bool uses_tuya_legacy_identity_psk(bool has_tls_mode, long long tls_mode, const StrDict &ids);

/* src/tuya_config.py _run_selftests(): returns the check count (13), or < 0. */
int tuya_config_selftests();
/* tests/tuya_config.py (the two paired-dump regressions); `root` is the repo
 * root holding firmwares/ (empty: the first of "..", ".", "../.." that has the
 * fixture). Returns the number of tests run (2), or < 0. */
int tuya_config_dump_tests(const std::string &root = "");

} /* namespace tuya_config */

#endif
