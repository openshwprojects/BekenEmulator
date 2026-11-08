/* Recover Tuya's on-flash configuration from a BK7231 dump. Full port of
 * src/tuya_config.py (same scheme as BK7231GUIFlashTool / bk7231tools):
 *
 *   1. Find the key block - a 4K block whose first AES-ECB decryption under
 *      the fixed master key starts with MAGIC_KEY. Its header is
 *      `<I magic, I crc, 16s inner_key>`.
 *   2. data_key[i] = (KEY_PART_1[i & 3] + KEY_PART_2[i] + inner_key[i]) % 256
 *   3. AES-ECB decrypt the storage area with it and pull out the records.
 *
 * Older firmware keeps `user_param_key` in the clear, so a plaintext scan is
 * kept as a fallback for those. Every regular expression of the Python module
 * is hand-matched here with the same (backtracking) semantics.
 */
#include "beken/tuya_config.h"
#include "crypto_prims.h"
#include <stdio.h>
#include <string.h>

namespace tuya_config {

static const char *KEY_MASTER = "qwertyuiopasdfgh";
static const char *KEY_PART_1 = "8710_2M";
static const char *KEY_PART_2 = "HHRRQbyemofrtytf";
static const u32 MAGIC_KEY = 0x13579753;
extern const u32 STORAGE_FROM = 0x1C0000; /* storage/user area sits above the application */
extern const u32 BLOCK = 0x1000;

bool REDACT_SECRETS = true; /* set false to show raw values locally */

static const size_t MIN_STORED_ID_LEN = 8;
static const size_t MAX_STORED_ID_LEN = 512;

static const char *STORED_ID_FIELDS[] = {"uuid", "auth_key", "ap_ssid", "pskKey", "psk_key", "psk30_key", 0};
static const char *NETWORK_SECRET_FIELDS[] = {"ssid", "passwd", "password", "ap_ssid", "ap_passwd",
                                              "pskKey", "psk_key", "psk30_key", "auth_key", "token",
                                              "reg_key", 0};

/* Python str.strip() whitespace (ASCII part) and bytes regex \s */
static const char *PY_WS = " \t\n\r\v\f\x1c\x1d\x1e\x1f";
static bool re_space(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static bool is_word(u8 c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(u8 c) { return c >= '0' && c <= '9'; }
static bool is_print(u8 c) { return c >= 32 && c <= 126; }

/* ------------------------------------------------------------ StrDict */
const std::string *StrDict::find(const std::string &k) const
{
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].first == k)
            return &items[i].second;
    return 0;
}
std::string StrDict::get(const std::string &k, const std::string &def) const
{
    const std::string *v = find(k);
    return v ? *v : def;
}
void StrDict::set(const std::string &k, const std::string &v)
{
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].first == k) {
            items[i].second = v;
            return;
        }
    items.push_back(std::make_pair(k, v));
}
void StrDict::setdefault(const std::string &k, const std::string &v)
{
    if (!has(k))
        items.push_back(std::make_pair(k, v));
}
void StrDict::erase(const std::string &k)
{
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].first == k) {
            items.erase(items.begin() + i);
            return;
        }
}

/* ------------------------------------------------------- UTF-8 / str() */
/* Split UTF-8 text into code-point slices (lenient: a stray byte is one). */
static std::vector<std::string> code_points(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        u8 c = (u8)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size())
            n = 1;
        for (size_t k = 1; k < n; k++)
            if (((u8)s[i + k] & 0xC0) != 0x80) {
                n = 1;
                break;
            }
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

static u32 cp_value(const std::string &c)
{
    u8 b0 = (u8)c[0];
    if (c.size() == 1)
        return b0;
    if (c.size() == 2)
        return ((b0 & 0x1F) << 6) | ((u8)c[1] & 0x3F);
    if (c.size() == 3)
        return ((b0 & 0x0F) << 12) | (((u8)c[1] & 0x3F) << 6) | ((u8)c[2] & 0x3F);
    return ((b0 & 0x07) << 18) | (((u8)c[1] & 0x3F) << 12) | (((u8)c[2] & 0x3F) << 6) | ((u8)c[3] & 0x3F);
}

static size_t py_len(const std::string &s) { return code_points(s).size(); }

/* Python repr(str) */
static std::string py_repr_str(const std::string &s)
{
    std::vector<std::string> cps = code_points(s);
    bool has_sq = s.find('\'') != std::string::npos, has_dq = s.find('"') != std::string::npos;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (size_t i = 0; i < cps.size(); i++) {
        u32 cp = cp_value(cps[i]);
        if (cps[i].size() == 1 && cp >= 0x80) /* a stray byte: surrogateescape-like */
            out += strf("\\x%02x", cp);
        else if (cp == (u32)q || cp == '\\')
            out += '\\', out += (char)cp;
        else if (cp == '\t')
            out += "\\t";
        else if (cp == '\n')
            out += "\\n";
        else if (cp == '\r')
            out += "\\r";
        else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0xA0) || cp == 0xAD)
            out += strf("\\x%02x", cp);
        else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0x2028 || cp == 0x2029)
            out += strf("\\u%04x", cp);
        else
            out += cps[i];
    }
    out += q;
    return out;
}

static std::string py_repr(const JsonValue &v)
{
    switch (v.type()) {
    case JsonValue::STR:
        return py_repr_str(v.as_str());
    case JsonValue::ARR: {
        std::string out = "[";
        for (size_t i = 0; i < v.arr().size(); i++) {
            if (i)
                out += ", ";
            out += py_repr(v.arr()[i]);
        }
        return out + "]";
    }
    case JsonValue::OBJ: {
        std::string out = "{";
        const std::vector<std::pair<std::string, JsonValue> > &m = v.members();
        for (size_t i = 0; i < m.size(); i++) {
            if (i)
                out += ", ";
            out += py_repr_str(m[i].first) + ": " + py_repr(m[i].second);
        }
        return out + "}";
    }
    default:
        return v.str_value(); /* None / True / False / int / float repr */
    }
}

std::string py_str(const JsonValue &v)
{
    return v.is_str() ? v.as_str() : py_repr(v);
}

/* ------------------------------------------------------------ crypto */
static Aes data_aes(const Bytes &inner_key)
{
    Bytes key(16, '\0');
    for (int i = 0; i < 16; i++)
        key[i] = (char)(((u8)KEY_PART_1[i & 3] + (u8)KEY_PART_2[i] + (u8)inner_key[i]) % 256);
    return Aes(key);
}

bool find_key_block(const Bytes &data, u32 &off_out, Bytes &inner_out)
{
    Aes master(Bytes(KEY_MASTER, 16));
    for (size_t off = STORAGE_FROM; off + 32 < data.size(); off += BLOCK) {
        u8 head[32];
        master.decrypt_block((const u8 *)data.data() + off, head);
        master.decrypt_block((const u8 *)data.data() + off + 16, head + 16);
        if (rd32le(head) == MAGIC_KEY) {
            off_out = (u32)off;
            inner_out = Bytes((const char *)head + 8, 16);
            return true;
        }
    }
    return false;
}

Bytes decrypt_storage(const Bytes &data, const Bytes &inner_key)
{
    Bytes seg = data.size() > STORAGE_FROM ? data.substr(STORAGE_FROM) : Bytes();
    seg.resize(seg.size() / 16 * 16);
    return data_aes(inner_key).ecb_decrypt(seg);
}

/* ------------------------------------------------------ record recovery */
std::vector<std::pair<size_t, Bytes> > json_objects(const Bytes &buf, size_t limit)
{
    std::vector<std::pair<size_t, Bytes> > out;
    const size_t n = buf.size();
    size_t i = buf.find('{');
    while (i != std::string::npos && i < n) {
        int depth = 0;
        bool broke = false;
        size_t end = i + limit < n ? i + limit : n;
        for (size_t j = i; j < end; j++) {
            u8 c = (u8)buf[j];
            if (!(is_print(c) || c == 9 || c == 10 || c == 13)) {
                broke = true; /* ran into binary - not a record */
                break;
            }
            if (c == 0x7B)
                depth++;
            else if (c == 0x7D) {
                depth--;
                if (depth == 0) {
                    out.push_back(std::make_pair(i, buf.substr(i, j + 1 - i)));
                    i = buf.find('{', j + 1);
                    broke = true;
                    break;
                }
            }
        }
        if (!broke) {
            i = buf.find('{', i + 1);
            continue;
        }
        if (depth != 0)
            i = buf.find('{', i + 1);
    }
    return out;
}

bool decode(const Bytes &blob, JsonValue &out)
{
    return json_parse(utf8_replace(blob), out);
}

/* rb"([A-Za-z0-9_+/\-=]{8,512})" */
static bool is_token(u8 c) { return is_word(c) || c == '+' || c == '/' || c == '-' || c == '='; }
/* rb"(?=$|[\"'\s,;}\]\x00])" */
static bool is_token_end(const Bytes &b, size_t p)
{
    if (p >= b.size())
        return true;
    u8 c = (u8)b[p];
    return c == '"' || c == '\'' || re_space(c) || c == ',' || c == ';' || c == '}' || c == ']' || c == 0;
}
static bool token_at(const Bytes &b, size_t p, std::string &out)
{
    size_t q = p;
    while (q < b.size() && is_token((u8)b[q]))
        q++;
    /* the token class and the lookahead class are disjoint, so backtracking
     * into a shorter token can never satisfy the lookahead: only the maximal
     * run can match. */
    size_t len = q - p;
    if (len < MIN_STORED_ID_LEN || len > MAX_STORED_ID_LEN || !is_token_end(b, q))
        return false;
    out = b.substr(p, len);
    return true;
}

/* (?<![A-Za-z0-9_])KEY(?:["']?\s*[:=]\s*["']?|\s+)TOKEN(?=...) - re.search */
bool stored_id_value(const Bytes &buf, const std::string &name, std::string &out)
{
    if (name.empty())
        return false;
    for (size_t pos = buf.find(name); pos != std::string::npos; pos = buf.find(name, pos + 1)) {
        if (pos > 0 && is_word((u8)buf[pos - 1]))
            continue;
        const size_t p = pos + name.size();
        /* alternative 1: ["']?\s*[:=]\s*["']? */
        size_t q = p;
        if (q < buf.size() && (buf[q] == '"' || buf[q] == '\''))
            q++;
        while (q < buf.size() && re_space((u8)buf[q]))
            q++;
        if (q < buf.size() && (buf[q] == ':' || buf[q] == '=')) {
            q++;
            while (q < buf.size() && re_space((u8)buf[q]))
                q++;
            if (q < buf.size() && (buf[q] == '"' || buf[q] == '\''))
                q++;
            if (token_at(buf, q, out))
                return true;
        }
        /* alternative 2: \s+ */
        q = p;
        while (q < buf.size() && re_space((u8)buf[q]))
            q++;
        if (q > p && token_at(buf, q, out))
            return true;
    }
    return false;
}

static std::string redact_any(const JsonValue &v) { return redact(py_str(v)); }

std::string network_raw(const JsonValue &obj, const std::string &original)
{
    if (!REDACT_SECRETS)
        return original;
    JsonValue safe = obj;
    for (int i = 0; NETWORK_SECRET_FIELDS[i]; i++) {
        const std::string name = NETWORK_SECRET_FIELDS[i];
        if (!safe.has(name))
            continue;
        const JsonValue &v = safe.get(name);
        /* safe.get(name) not in (None, "") */
        if (v.is_null() || (v.is_str() && v.as_str().empty()))
            continue;
        safe.set(name, JsonValue::make_str(redact_any(v)));
    }
    return safe.dumps();
}

StrList split_loose_fields(const std::string &text)
{
    StrList out;
    size_t start = 0;
    int depth = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (quote) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '[' || c == '(' || c == '{')
            depth++;
        else if (c == ']' || c == ')' || c == '}')
            depth = depth > 0 ? depth - 1 : 0;
        else if (c == ',' && depth == 0) {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(text.substr(start));
    return out;
}

static bool key_shape(const std::string &k)
{
    if (k.empty())
        return false;
    for (size_t i = 0; i < k.size(); i++)
        if (!is_word((u8)k[i]))
            return false;
    return true;
}

Pairs parse_loose(const std::string &text)
{
    Pairs out;
    std::string body = strip(text, PY_WS);
    if (startswith(body, "{"))
        body = body.substr(1);
    if (endswith(body, "}"))
        body = body.substr(0, body.size() - 1);
    StrList parts = split_loose_fields(body);
    for (size_t i = 0; i < parts.size(); i++) {
        std::string k, v;
        bool sep = partition(strip(parts[i], PY_WS), ":", k, v);
        std::string key = strip(strip(k, PY_WS), "\"");
        /* A damaged record can run into the JSON record after it. Never turn
         * braces, quotes, or array fragments from that neighbour into fields. */
        if (sep && key_shape(key))
            out.push_back(std::make_pair(key, strip(strip(v, PY_WS), "\"'")));
    }
    return out;
}

/* Python hash/eq identity of a datapoint id for the seen_dp set (1 == 1.0 == True). */
static std::string dp_key(const JsonValue &id)
{
    switch (id.type()) {
    case JsonValue::BOOL:
        return id.as_bool() ? "n:1" : "n:0";
    case JsonValue::INT:
        return "n:" + id.str_value();
    case JsonValue::REAL: {
        double r = id.as_real();
        if (r == (double)(long long)r && r > -9e18 && r < 9e18)
            return strf("n:%lld", (long long)r);
        return "r:" + id.str_value();
    }
    case JsonValue::STR:
        return "s:" + id.as_str();
    case JsonValue::NUL:
        return "none";
    default:
        return "x:" + id.dumps();
    }
}

/* int(obj["dy_tls_m"]) catching TypeError/ValueError */
static bool py_int(const JsonValue &v, long long &out)
{
    switch (v.type()) {
    case JsonValue::INT:
        out = v.as_int();
        return true;
    case JsonValue::BOOL:
        out = v.as_bool() ? 1 : 0;
        return true;
    case JsonValue::REAL: {
        double r = v.as_real();
        if (r != r || r > 9.2e18 || r < -9.2e18)
            return false;
        out = (long long)r; /* truncates toward zero, like int(float) */
        return true;
    }
    case JsonValue::STR:
        return parse_int(v.as_str(), 10, out);
    default:
        return false;
    }
}

/* _LOOSE = rb"\{[a-zA-Z0-9_]+:[ -~]{20,2000}?\}" - match at `i` (a '{'). */
static bool loose_match_at(const Bytes &buf, size_t i, size_t &end)
{
    size_t p = i + 1;
    while (p < buf.size() && is_word((u8)buf[p]))
        p++;
    if (p == i + 1 || p >= buf.size() || buf[p] != ':')
        return false;
    p++;
    for (size_t k = 0;; k++) {
        if (p + k >= buf.size())
            return false;
        u8 c = (u8)buf[p + k];
        if (k >= 20 && c == '}') {
            end = p + k + 1;
            return true;
        }
        if (k == 2000 || !is_print(c))
            return false;
    }
}

Collected collect(const Bytes &buf)
{
    Collected found;
    std::set<std::string> seen_dp;
    std::vector<std::pair<size_t, Bytes> > objs = json_objects(buf);
    for (size_t oi = 0; oi < objs.size(); oi++) {
        const Bytes &blob = objs[oi].second;
        JsonValue obj;
        if (!decode(blob, obj) || !obj.is_obj())
            continue;
        /* Credential-only KV records do not carry schemaId/productKey/devId,
         * so recover their known fields from every valid JSON object. */
        for (int i = 0; STORED_ID_FIELDS[i]; i++) {
            const JsonValue &value = obj.get(STORED_ID_FIELDS[i]);
            if (value.is_str_or_int()) {
                std::string s = py_str(value);
                size_t len = py_len(s);
                if (len >= MIN_STORED_ID_LEN && len <= MAX_STORED_ID_LEN)
                    found.ids.setdefault(STORED_ID_FIELDS[i], s);
            }
        }
        /* dy_tls_m is not a credential: mode 2 identifies the older Tuya
         * PSK-only/bootstrap path. */
        if (!found.has_tls_mode && obj.has("dy_tls_m")) {
            long long m;
            if (py_int(obj.get("dy_tls_m"), m)) {
                found.has_tls_mode = true;
                found.tls_mode = m;
            }
        }
        std::string text = blob; /* printable ASCII: decode("ascii") is identity */
        if (obj.has("nc_tp") || obj.has("ssid")) {
            if (!found.has_network) {
                found.has_network = true;
                found.network = obj;
                found.raw.push_back(network_raw(obj, text));
            }
        } else if (obj.has("id") && obj.has("property")) {
            std::string k = dp_key(obj.get("id"));
            if (seen_dp.count(k)) /* the schema is stored more than once */
                continue;
            seen_dp.insert(k);
            found.dps.push_back(obj);
            if (found.raw.size() < 6)
                found.raw.push_back(text);
        } else if (obj.has("schemaId") || obj.has("productKey") || obj.has("devId")) {
            const std::vector<std::pair<std::string, JsonValue> > &m = obj.members();
            for (size_t i = 0; i < m.size(); i++)
                if (m[i].second.is_str_or_int()) {
                    std::string s = py_str(m[i].second);
                    if (py_len(s) < 48)
                        found.ids.set(m[i].first, s);
                }
        }
    }
    for (size_t i = buf.find('{'); i != std::string::npos;) {
        size_t end;
        if (!loose_match_at(buf, i, end)) {
            i = buf.find('{', i + 1);
            continue;
        }
        Bytes blob = buf.substr(i, end - i);
        /* A torn final copy of user_param_key can be followed by a valid JSON
         * network record; keep the recoverable prefix, drop the neighbour. */
        size_t json_start = blob.find("{\"", 1);
        if (json_start != std::string::npos)
            blob = rstrip(blob.substr(0, json_start), " \t\n\r\v\f") + "}";
        std::string text = blob;
        if (contains(text, "_pin") || contains(text, "module") || contains(text, "Jsonver") ||
            contains(text, "jv:")) {
            Pairs pairs = parse_loose(text);
            if (!pairs.empty()) {
                found.pins = pairs;
                found.raw.insert(found.raw.begin(), text);
                break;
            }
        }
        i = buf.find('{', end);
    }
    for (int i = 0; STORED_ID_FIELDS[i]; i++) {
        std::string value;
        if (stored_id_value(buf, STORED_ID_FIELDS[i], value))
            found.ids.setdefault(STORED_ID_FIELDS[i], value); /* prefer the json.loads result */
    }
    return found;
}

/* ------------------------------------------------- human-readable summary */
/* Ported from BK7231GUIFlashTool's GetKeysHumanReadableInternal. Kinds:
 *   EXACT  ^a$            SUB  a (re.search)
 *   NUM    ^a(\d+)b$      NUMX ^a\d+b$ (no group: {n} is "") */
enum { EXACT, SUB, NUM, NUMX };
struct PinRule {
    int kind;
    const char *a, *b, *tmpl;
};
static const PinRule PIN_RULES[] = {
    {NUM, "led", "_pin", "LED (channel {n}) on P{v}"},
    {NUMX, "netled", "_pin", "WiFi LED on P{v}"},
    {EXACT, "netled_pin", "", "WiFi LED on P{v}"},
    {EXACT, "wfst", "", "WiFi LED on P{v}"},
    {EXACT, "wfst_pin", "", "WiFi LED on P{v}"},
    {SUB, "bz_pin_pin", "", "Buzzer Pin (TODO) on P{v}"},
    {EXACT, "buzzer_io", "", "Buzzer Pin (TODO) on P{v}"},
    {SUB, "status_led_pin", "", "Status LED on P{v}"},
    {SUB, "remote_io", "", "RF Remote on P{v}"},
    {SUB, "samp_sw_pin", "", "Battery Relay on P{v}"},
    {SUB, "samp_pin", "", "Battery ADC on P{v}"},
    {SUB, "i2c_scl_pin", "", "I2C SCL on P{v}"},
    {SUB, "i2c_sda_pin", "", "I2C SDA on P{v}"},
    {SUB, "alt_pin_pin", "", "ALT pin on P{v}"},
    {SUB, "one_wire_pin", "", "OneWire IO pin on P{v}"},
    {SUB, "backlit_io_pin", "", "Backlit IO pin on P{v}"},
    {EXACT, "max_V", "", "Battery Max Voltage: {v}"},
    {EXACT, "min_V", "", "Battery Min Voltage: {v}"},
    {EXACT, "rl", "", "Relay (channel 0) on P{v}"},
    {NUM, "rl", "_pin", "Relay (channel {n}) on P{v}"},
    {NUM, "rl_on", "_pin", "Bridge Relay On (channel {n}) on P{v}"},
    {NUM, "rl_off", "_pin", "Bridge Relay Off (channel {n}) on P{v}"},
    {EXACT, "bt_pin", "", "Button (channel 0) on P{v}"},
    {EXACT, "bt", "", "Button (channel 0) on P{v}"},
    {NUM, "k", "pin_pin", "Button (channel {n}) on P{v}"},
    {NUM, "bt", "_pin", "Button (channel {n}) on P{v}"},
    {NUM, "door", "_magt_pin", "Door Sensor (channel {n}) on P{v}"},
    {NUM, "onoff", "", "TglChannelToggle (channel {n}) on P{v}"},
    {SUB, "gate_sensor_pin_pin", "", "Door/Gate sensor on P{v}"},
    {SUB, "basic_pin_pin", "", "PIR sensor on P{v}"},
    {EXACT, "ele_pin", "", "BL0937 ELE (CF) on P{v}"},
    {EXACT, "epin", "", "BL0937 ELE (CF) on P{v}"},
    {EXACT, "vi_pin", "", "BL0937 VI (CF1) on P{v}"},
    {EXACT, "ivpin", "", "BL0937 VI (CF1) on P{v}"},
    {SUB, "sel_pin_pin", "", "BL0937 SEL on P{v}"},
    {EXACT, "ivcpin", "", "BL0937 SEL on P{v}"},
    {EXACT, "r_pin", "", "LED Red (Channel 1) on P{v}"},
    {EXACT, "g_pin", "", "LED Green (Channel 2) on P{v}"},
    {EXACT, "b_pin", "", "LED Blue (Channel 3) on P{v}"},
    {EXACT, "c_pin", "", "LED Cool (Channel 4) on P{v}"},
    {EXACT, "w_pin", "", "LED Warm (Channel 5) on P{v}"},
    {EXACT, "mic", "", "Microphone (TODO) on P{v}"},
    {EXACT, "micpin", "", "Microphone (TODO) on P{v}"},
    {EXACT, "ctrl_pin", "", "Control Pin (TODO) on P{v}"},
    {EXACT, "buzzer_pwm", "", "Buzzer Frequency (TODO) is {v}"},
    {EXACT, "irpin", "", "IR Receiver is on P{v}"},
    {EXACT, "infrr", "", "IR Receiver is on P{v}"},
    {EXACT, "infre", "", "IR Transmitter is on P{v}"},
    {EXACT, "reset_pin", "", "Button is on P{v}"},
    {EXACT, "pwmhz", "", "PWM Frequency {v}"},
    {EXACT, "pirsense_pin", "", "PIR Sensitivity {v}"},
    {EXACT, "pirlduty", "", "PIR Low Duty {v}"},
    {EXACT, "pirfreq", "", "PIR Frequency {v}"},
    {EXACT, "pirmduty", "", "PIR High Duty {v}"},
    {EXACT, "pirin_pin", "", "PIR Input {v}"},
    {EXACT, "mosi", "", "SPI MOSI {v}"},
    {EXACT, "miso", "", "SPI MISO {v}"},
    {EXACT, "SCL", "", "SPI SCL {v}"},
    {EXACT, "CS", "", "SPI CS {v}"},
    {EXACT, "total_bt_pin", "", "Pair/Toggle All Button on P{v}"},
};

static bool rule_match(const PinRule &r, const std::string &key, std::string &n)
{
    n = "";
    if (r.kind == EXACT)
        return key == r.a;
    if (r.kind == SUB)
        return contains(key, r.a);
    if (!startswith(key, r.a))
        return false;
    size_t p = strlen(r.a), q = p;
    while (q < key.size() && is_digit((u8)key[q]))
        q++;
    if (q == p || key.substr(q) != r.b) /* every suffix starts with a non-digit */
        return false;
    if (r.kind == NUM)
        n = key.substr(p, q - p);
    return true;
}

bool describe_pin(const std::string &key, const std::string &value, std::string &out)
{
    for (size_t i = 0; i < sizeof(PIN_RULES) / sizeof(PIN_RULES[0]); i++) {
        std::string n;
        if (rule_match(PIN_RULES[i], key, n)) {
            out = replace_all(replace_all(PIN_RULES[i].tmpl, "{n}", n), "{v}", value);
            return true;
        }
    }
    return false;
}

static const char *NAMES[][2] = {
    {"module", "Tuya module"}, {"jv", "Config version"}, {"Jsonver", "Config version"},
    {"crc", "Config checksum"}, {"category", "Product category"},
    {"ch_num", "Channels (gangs)"}, {"net_trig", "Net-config trigger"},
    {"reset_t", "Reset hold time (s)"}, {"rstnum", "Power cycles to reset"},
    {"wfcfg", "Pairing mode"}, {"wfct", "Pairing timeout"},
    {"cmod", "Colour mode"}, {"cwtype", "Cold/warm driver"}, {"dmod", "Dimmer mode"},
    {"pwmhz", "PWM frequency (Hz)"}, {"brightmin", "Min brightness (%)"},
    {"brightmax", "Max brightness (%)"}, {"colormin", "Min colour (%)"},
    {"colormax", "Max colour (%)"}, {"cwmaxp", "Max C+W power (%)"},
    {"deftemp", "Default colour temp"}, {"defbright", "Default brightness (%)"},
    {"defcolor", "Default colour"}, {"pmemory", "Restore state after power loss"},
    {"onoffmode", "On/off behaviour"}, {"remdmode", "Remote/dimming mode"},
    {"prodagain", "Allow re-running prod test"}, {"title20", "title20 flag"},
    {"nety_led", "Net LED state (paired)"}, {"netn_led", "Net LED state (unpaired)"},
    {"netled_reuse", "Net LED pin reused"}, {"total_stat", "Total-status mode"},
    {"rstcor", "Reset indicator colour"}, {"rstbr", "Reset indicator brightness (%)"},
    {"rsttemp", "Reset indicator temp"}, {"cagt", "Colour adjust time"},
    {"wt", "White transition"}, {"rgbt", "RGB transition"}, {"ffc_select", "FFC variant"},
    {0, 0}};
static const char *ROLE[][2] = {
    {"rl", "Relay"}, {"ch", "Channel"}, {"netled", "Network LED"}, {"bt", "Button"},
    {"total_bt", "Button (combined)"}, {"c", "Cold white"}, {"w", "Warm white"},
    {"r", "Red"}, {"g", "Green"}, {"b", "Blue"}, {"net", "Network"}, {0, 0}};
static const char *VALUES[][3] = {
    {"cmod", "cw", "cold/warm white"}, {"cmod", "rgb", "RGB"},
    {"cmod", "rgbcw", "RGB + cold/warm"}, {"cmod", "c", "single white"},
    {"defcolor", "c", "cold white"}, {"defcolor", "w", "warm white"},
    {"rstcor", "c", "cold white"}, {"wfcfg", "spcl", "special (hold button)"},
    {"wfcfg", "old", "legacy pairing"}, {0, 0, 0}};

static const char *lookup2(const char *(*tbl)[2], const std::string &k)
{
    for (int i = 0; tbl[i][0]; i++)
        if (k == tbl[i][0])
            return tbl[i][1];
    return 0;
}

/* re.match(r"^([a-z_]*?)(\d*)(_pin|_lv|_dpid|_type)$", key) */
static bool describe_match(const std::string &key, std::string &base, std::string &idx, std::string &suffix)
{
    static const char *SUFFIXES[] = {"_pin", "_lv", "_dpid", "_type", 0};
    for (size_t b = 0; b <= key.size(); b++) {
        if (b > 0 && !((key[b - 1] >= 'a' && key[b - 1] <= 'z') || key[b - 1] == '_'))
            break;
        size_t dmax = 0;
        while (b + dmax < key.size() && is_digit((u8)key[b + dmax]))
            dmax++;
        for (size_t d = dmax + 1; d-- > 0;) {
            std::string rest = key.substr(b + d);
            for (int s = 0; SUFFIXES[s]; s++)
                if (rest == SUFFIXES[s]) {
                    base = key.substr(0, b);
                    idx = key.substr(b, d);
                    suffix = SUFFIXES[s];
                    return true;
                }
        }
    }
    return false;
}

std::pair<std::string, std::string> describe(const std::string &key, const std::string &value)
{
    std::string label;
    const char *name = lookup2(NAMES, key);
    if (name)
        label = name;
    else {
        std::string base, idx, suffix;
        if (describe_match(key, base, idx, suffix)) {
            std::string b = rstrip(base, "_");
            const char *role_s = lookup2(ROLE, b);
            std::string role = role_s ? std::string(role_s) : (b.empty() ? std::string("Pin") : b);
            const char *what = suffix == "_pin" ? "GPIO" : suffix == "_lv" ? "active level"
                             : suffix == "_dpid" ? "datapoint id" : "type";
            label = join(split_ws(role + " " + idx + " " + what), " ");
        } else
            label = key;
    }
    std::string pretty;
    bool have = false;
    for (int i = 0; VALUES[i][0]; i++)
        if (key == VALUES[i][0] && value == VALUES[i][1]) {
            pretty = VALUES[i][2];
            have = true;
            break;
        }
    if (!have && endswith(key, "_lv")) {
        if (value == "0")
            pretty = "active low", have = true;
        else if (value == "1")
            pretty = "active high", have = true;
    }
    if (!have && endswith(key, "_pin")) {
        pretty = "P" + value;
        have = true;
    }
    return std::make_pair(label, (have && !pretty.empty()) ? pretty : value);
}

static const char *DP_TYPE[][2] = {
    {"value", "numeric"}, {"bool", "on/off"}, {"enum", "enum"},
    {"string", "text"}, {"bitmap", "bitmap"}, {"raw", "raw"}, {0, 0}};

std::string redact(const std::string &value)
{
    std::vector<std::string> cps = code_points(value);
    if (cps.size() <= 4)
        return std::string(cps.size(), '*');
    return cps[0] + cps[1] + std::string(cps.size() - 4, '*') + cps[cps.size() - 2] + cps[cps.size() - 1];
}

Pairs summarise(const Collected &found, bool has_key_off, u32 key_off, const Bytes &inner_key, bool encrypted)
{
    Pairs out;
    out.push_back(std::make_pair(std::string("Storage"),
                                 std::string(encrypted ? "encrypted (decrypted with device key)" : "plaintext")));
    if (has_key_off)
        out.push_back(std::make_pair(std::string("Key block"), strf("0x%06X", key_off)));
    if (!inner_key.empty()) {
        std::string value = hexstr(inner_key);
        out.push_back(std::make_pair(std::string("Device key"), REDACT_SECRETS ? redact(value) : value));
    }
    static const char *ID_LABELS[][2] = {
        {"uuid", "Device UUID"}, {"auth_key", "Auth key"}, {"ap_ssid", "AP SSID"},
        {"pskKey", "PSK key"}, {"psk_key", "PSK key"}, {"psk30_key", "PSK 3.0 key"}, {0, 0}};
    for (int i = 0; ID_LABELS[i][0]; i++) {
        const std::string name = ID_LABELS[i][0];
        const std::string *val = found.ids.find(name);
        if (val && !val->empty()) {
            std::string v = *val;
            if (REDACT_SECRETS && name != "uuid")
                v = redact(v);
            out.push_back(std::make_pair(std::string(ID_LABELS[i][1]), v));
        }
    }
    if (found.has_network && !found.network.members().empty()) {
        const JsonValue &net = found.network;
        const JsonValue &ssid = net.get("ssid");
        bool truthy = false;
        switch (ssid.type()) {
        case JsonValue::NUL: truthy = false; break;
        case JsonValue::BOOL: truthy = ssid.as_bool(); break;
        case JsonValue::INT: truthy = ssid.str_value() != "0"; break;
        case JsonValue::REAL: truthy = ssid.as_real() != 0; break;
        case JsonValue::STR: truthy = !ssid.as_str().empty(); break;
        case JsonValue::ARR: truthy = !ssid.arr().empty(); break;
        case JsonValue::OBJ: truthy = !ssid.members().empty(); break;
        }
        if (truthy) {
            std::string s = py_str(ssid);
            out.push_back(std::make_pair(std::string("Paired WiFi SSID"), REDACT_SECRETS ? redact(s) : s));
        }
        out.push_back(std::make_pair(std::string("Network config type"), py_str(net.get("nc_tp"))));
        out.push_back(std::make_pair(std::string("Network status"), py_str(net.get("stat"))));
    }
    if (!found.dps.empty()) {
        out.push_back(std::make_pair(std::string("Datapoints defined"), strf("%d", (int)found.dps.size())));
        for (size_t i = 0; i < found.dps.size() && i < 8; i++) {
            const JsonValue &dp = found.dps[i];
            JsonValue prop = dp.has("property") ? dp.get("property") : JsonValue::make_obj();
            if (!prop.is_obj()) /* Python would raise AttributeError here */
                prop = JsonValue::make_obj();
            std::string kind;
            const JsonValue &t = prop.get("type");
            const char *mapped = t.is_str() ? lookup2(DP_TYPE, t.as_str()) : 0;
            if (mapped)
                kind = mapped;
            else
                kind = prop.has("type") ? py_str(t) : "?";
            std::string detail = kind;
            if (prop.has("min") && prop.has("max"))
                detail += " " + py_str(prop.get("min")) + ".." + py_str(prop.get("max"));
            if (prop.has("range")) {
                const JsonValue &r = prop.get("range");
                StrList parts;
                if (r.is_arr())
                    for (size_t k = 0; k < r.arr().size() && k < 4; k++)
                        parts.push_back(py_str(r.arr()[k]));
                else if (r.is_str()) {
                    std::vector<std::string> cps = code_points(r.as_str());
                    for (size_t k = 0; k < cps.size() && k < 4; k++)
                        parts.push_back(cps[k]);
                }
                detail += " " + join(parts, "/");
            }
            std::string mode = dp.has("mode") ? py_str(dp.get("mode")) : "?";
            out.push_back(std::make_pair("  DP " + py_str(dp.get("id")) + " (" + mode + ")", detail));
        }
    }
    /* Pin/wiring lines first, worded exactly as BK7231GUIFlashTool prints them. */
    Pairs bullets, others;
    for (size_t i = 0; i < found.pins.size(); i++) {
        std::string text;
        if (describe_pin(found.pins[i].first, found.pins[i].second, text))
            bullets.push_back(std::make_pair(std::string("-"), text));
        else
            others.push_back(describe(found.pins[i].first, found.pins[i].second));
    }
    if (!bullets.empty()) {
        out.push_back(std::make_pair(std::string("Pin configuration"), strf("%d entries", (int)bullets.size())));
        out.insert(out.end(), bullets.begin(), bullets.end());
    }
    out.insert(out.end(), others.begin(), others.end());
    return out;
}

Decoded extract(const std::string &path)
{
    Decoded d;
    Bytes data;
    if (!read_file(path, data))
        return d;
    u32 key_off = 0;
    Bytes inner;
    bool has_key = find_key_block(data, key_off, inner);
    Collected found;
    bool encrypted = false;
    if (has_key) {
        found = collect(decrypt_storage(data, inner));
        encrypted = true;
    }
    Collected plain = collect(data.size() > STORAGE_FROM ? data.substr(STORAGE_FROM) : Bytes());
    if (!has_key)
        found = plain;
    else {
        /* Older images keep the pin map in the clear even when the rest is not. */
        if (found.pins.empty() && !plain.pins.empty()) {
            found.pins = plain.pins;
            found.raw.insert(found.raw.begin(), plain.raw.empty() ? std::string() : plain.raw[0]);
        }
        for (size_t i = 0; i < plain.ids.items.size(); i++)
            found.ids.setdefault(plain.ids.items[i].first, plain.ids.items[i].second);
        if (!found.has_tls_mode) {
            found.has_tls_mode = plain.has_tls_mode;
            found.tls_mode = plain.tls_mode;
        }
    }
    if (found.pins.empty() && found.dps.empty() && !found.has_network && found.ids.empty())
        return d;
    d.valid = true;
    d.offset = has_key ? key_off : STORAGE_FROM;
    d.encrypted = encrypted;
    StrList raw;
    for (size_t i = 0; i < found.raw.size() && i < 6; i++)
        if (!found.raw[i].empty())
            raw.push_back(found.raw[i]);
    d.raw = join(raw, "\n\n");
    d.pairs = found.pins;
    d.has_network = found.has_network;
    d.network = found.network;
    d.dps = found.dps;
    d.ids = found.ids;
    d.has_tls_mode = found.has_tls_mode;
    d.tls_mode = found.tls_mode;
    d.human = summarise(found, has_key, key_off, inner, encrypted);
    return d;
}

std::vector<Bytes> tls_psk_candidates(const StrDict &ids)
{
    std::vector<Bytes> out;
    const char *order[] = {"psk30_key", "psk_key", "pskKey", 0};
    for (int i = 0; order[i]; i++) {
        const std::string *v = ids.find(order[i]);
        if (!v || !is_ascii(*v))
            continue;
        if (v->size() < MIN_STORED_ID_LEN || v->size() > MAX_STORED_ID_LEN)
            continue;
        bool dup = false;
        for (size_t j = 0; j < out.size(); j++)
            if (out[j] == *v)
                dup = true;
        if (!dup)
            out.push_back(*v);
    }
    return out;
}

std::vector<Bytes> tls_psk_candidates(const Decoded &d)
{
    return tls_psk_candidates(d.ids); /* an invalid Decoded (None) has no ids */
}

bool uses_tuya_legacy_identity_psk(bool has_tls_mode, long long tls_mode, const StrDict &ids)
{
    if (!has_tls_mode || tls_mode != 2)
        return false;
    if (!ids.get("psk30_key").empty())
        return false;
    const std::string *v = ids.find("psk_key");
    if (!v)
        v = ids.find("pskKey");
    if (!v || !is_ascii(*v))
        return false;
    return v->size() == 37 && is_alnum_ascii(*v);
}

bool uses_tuya_legacy_identity_psk(const Decoded &d)
{
    return uses_tuya_legacy_identity_psk(d.has_tls_mode, d.tls_mode, d.ids);
}

/* ---------------------------------------------------------- self-tests */
#define TCHECK(cond, msg)                                 \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL: %s\n", msg);                    \
            return -1;                                    \
        }                                                 \
        n++;                                              \
        printf("  [PASS] %s\n", msg);                     \
    } while (0)

static std::string pairs_get(const Pairs &p, const std::string &k) /* dict(pairs)[k] */
{
    std::string v;
    for (size_t i = 0; i < p.size(); i++)
        if (p[i].first == k)
            v = p[i].second;
    return v;
}
static bool pairs_has(const Pairs &p, const std::string &k)
{
    for (size_t i = 0; i < p.size(); i++)
        if (p[i].first == k)
            return true;
    return false;
}

int tuya_config_selftests()
{
    int n = 0;

    printf("== loose Tuya user_param_key parser ==\n");
    Pairs pairs = parse_loose("{module:CBU,r_pin:26,irfunset:[[6,02],[7,1e],[3,01]],label:'a,b',crc:13,}");
    TCHECK(pairs_get(pairs, "irfunset") == "[[6,02],[7,1e],[3,01]]",
           "commas inside nested arrays stay in one value");
    TCHECK(pairs_has(pairs, "label") && pairs_get(pairs, "label") == "a,b" && pairs.size() == 5,
           "commas inside quoted values do not create bogus keys");
    bool shapes = true;
    for (size_t i = 0; i < pairs.size(); i++)
        shapes = shapes && key_shape(pairs[i].first);
    TCHECK(shapes, "every recovered key has the storage format's key shape");

    printf("== adjacent/truncated records ==\n");
    Collected found = collect(Bytes("xxxx{rstnum:3,c_pin:8,module:WB2L,def{\"nc_tp\":3,"
                                    "\"ssid\":\"U3VsaWFuYQ==\",\"passwd\":\"secret\" }yyyy"));
    Pairs want;
    want.push_back(std::make_pair(std::string("rstnum"), std::string("3")));
    want.push_back(std::make_pair(std::string("c_pin"), std::string("8")));
    want.push_back(std::make_pair(std::string("module"), std::string("WB2L")));
    TCHECK(found.pins == want, "a torn config does not absorb fields from the following network JSON");
    bool no_creds = true;
    for (size_t i = 0; i < found.pins.size(); i++)
        if (contains(found.pins[i].first, "ssid") || contains(found.pins[i].first, "passwd"))
            no_creds = false;
    TCHECK(no_creds, "network credentials never appear as pin/config keys");
    Collected network = collect(Bytes("{\"nc_tp\":3,\"ssid\":\"Home Net\",\"passwd\":\"password1\"}"));
    TCHECK(!network.raw.empty() && !contains(network.raw[0], "Home Net") &&
               !contains(network.raw[0], "password1"),
           "raw report records mask stored WiFi credentials");

    printf("== stored credential recovery ==\n");
    /* Synthetic: standard-Base64 punctuation and the former 40-char truncation. */
    std::string psk30 = std::string(21, 'A') + "+" + std::string(20, 'B') + "/=";
    Collected from_json = collect("{\"psk30_key\":\"" + psk30 + "\"}");
    const std::string *json_value = from_json.ids.find("psk30_key");
    TCHECK(json_value != 0, "PSK 3.0 is recovered from credential-only JSON");
    TCHECK(json_value != 0 && json_value->size() == psk30.size(),
           "standard Base64 PSK 3.0 keeps its full length");
    TCHECK(json_value != 0 && *json_value == psk30,
           "standard Base64 punctuation and padding survive JSON recovery");

    Collected from_loose = collect(Bytes("\0psk30_key=", 11) + psk30 + Bytes("\0", 1));
    TCHECK(from_loose.ids.get("psk30_key") == psk30 && from_loose.ids.has("psk30_key"),
           "the bounded loose-storage fallback preserves the same full value");
    Bytes too_long(MAX_STORED_ID_LEN + 1, 'A');
    Collected overrun = collect("psk30_key=" + too_long + Bytes("\0", 1));
    TCHECK(!overrun.ids.has("psk30_key"), "the loose-storage fallback does not accept a truncated prefix");
    Collected embedded = collect("backup_psk30_key=" + psk30 + Bytes("\0", 1));
    TCHECK(!embedded.ids.has("psk30_key"), "the loose-storage fallback requires an exact field name");

    Pairs summary = summarise(from_json, false, 0, Bytes(), false);
    StrList masked;
    for (size_t i = 0; i < summary.size(); i++)
        if (summary[i].first == "PSK 3.0 key")
            masked.push_back(summary[i].second);
    bool mask_shape_ok = masked.size() == 1 && masked[0].size() == psk30.size() &&
                         count_sub(masked[0], "*") == psk30.size() - 4;
    TCHECK(mask_shape_ok, "the human/GUI summary keeps the recovered PSK masked");

    StrDict ids;
    ids.set("psk30_key", "synthetic-psk30-value==");
    ids.set("psk_key", "synthetic-legacy-value");
    ids.set("pskKey", "synthetic-legacy-value");
    ids.set("auth_key", "not-a-tls-candidate");
    std::vector<Bytes> candidates = tls_psk_candidates(ids);
    TCHECK(candidates.size() == 2 && candidates[0] == "synthetic-psk30-value==" &&
               candidates[1] == "synthetic-legacy-value",
           "TLS candidates prefer PSK 3.0, include legacy spelling and remove duplicates");
    StrDict nonascii;
    nonascii.set("psk30_key", "nonascii-\xc5\xbc"); /* "nonascii-ż" */
    TCHECK(tls_psk_candidates(nonascii).empty() && tls_psk_candidates(Decoded()).empty(),
           "invalid or absent private PSKs are ignored without inventing a key");

    StrDict legacy;
    legacy.set("psk_key", std::string(37, 'A'));
    TCHECK(uses_tuya_legacy_identity_psk(true, 2, legacy),
           "Tuya TLS mode 2 with its exact legacy PSK shape enables identity-key trial");
    StrDict with30 = legacy;
    with30.set("psk30_key", std::string(44, 'B'));
    StrDict short36;
    short36.set("psk_key", std::string(36, 'A'));
    TCHECK(!uses_tuya_legacy_identity_psk(true, 2, with30) && !uses_tuya_legacy_identity_psk(true, 1, legacy) &&
               !uses_tuya_legacy_identity_psk(true, 2, short36),
           "PSK 3.0, another TLS mode, or another key shape keeps legacy trial disabled");

    printf("\nAll %d Tuya config self-tests passed.\n", n);
    return n;
}

/* tests/tuya_config.py: the paired-dump regressions. As in Python, every
 * comparison is reduced to a boolean so a failure cannot print key data. */
#define DASSERT(cond, msg)                                \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("AssertionError: %s\n", msg);          \
            return -1;                                    \
        }                                                 \
    } while (0)

static std::string fixture(const std::string &root, const char *name)
{
    if (!root.empty())
        return root + "/firmwares/" + name;
    const char *cands[] = {"..", ".", "../..", 0};
    for (int i = 0; cands[i]; i++) {
        std::string p = std::string(cands[i]) + "/firmwares/" + name;
        if (file_exists(p))
            return p;
    }
    return std::string("../firmwares/") + name;
}

static StrList masked_values(const Decoded &d, const char *label)
{
    StrList out;
    for (size_t i = 0; i < d.human.size(); i++)
        if (d.human[i].first == label)
            out.push_back(d.human[i].second);
    return out;
}

static int test_paired_ir_psk30_recovery(const std::string &root)
{
    /* The paired BK7238 image retains its complete PSK without displaying it. */
    std::string path = fixture(root, "BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin");
    DASSERT(file_exists(path), "paired BK7238 IR fixture is missing");
    Decoded result = extract(path);
    DASSERT(result.valid, "paired BK7238 IR config was not recovered");
    const std::string *recovered = result.ids.find("psk30_key");
    DASSERT(recovered != 0, "paired BK7238 IR PSK 3.0 key is missing");
    DASSERT(py_len(*recovered) == 44, "paired BK7238 IR PSK 3.0 key is not full length");

    /* Independently walk the decoded JSON records. */
    Bytes image;
    read_file(path, image);
    u32 key_off;
    Bytes inner_key;
    DASSERT(find_key_block(image, key_off, inner_key), "paired BK7238 IR storage key block is missing");
    Bytes storage = decrypt_storage(image, inner_key);
    StrList copies;
    std::vector<std::pair<size_t, Bytes> > objs = json_objects(storage);
    for (size_t i = 0; i < objs.size(); i++) {
        JsonValue obj;
        if (decode(objs[i].second, obj) && obj.is_obj() && obj.get("psk30_key").is_str())
            copies.push_back(obj.get("psk30_key").as_str());
    }
    DASSERT(copies.size() > 1, "paired BK7238 IR PSK records are missing");
    bool same_len = true, equal = true;
    for (size_t i = 0; i < copies.size(); i++) {
        same_len = same_len && py_len(copies[i]) == py_len(*recovered);
        equal = equal && copies[i] == *recovered;
    }
    DASSERT(same_len, "paired BK7238 IR PSK record length differs");
    DASSERT(equal, "paired BK7238 IR PSK records disagree");

    StrList displayed = masked_values(result, "PSK 3.0 key");
    bool summary_is_masked = displayed.size() == 1 && py_len(displayed[0]) == py_len(*recovered) &&
                             count_sub(displayed[0], "*") == py_len(*recovered) - 4;
    DASSERT(summary_is_masked, "paired BK7238 IR PSK summary is not masked");
    DASSERT(!contains(result.raw, *recovered), "paired BK7238 IR raw report exposes the PSK");
    return 0;
}

static int test_paired_lsc_legacy_tls_mode(const std::string &root)
{
    /* The LSC fixture alone opts into its authenticated identity-key mode. */
    std::string path = fixture(root, "BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin");
    DASSERT(file_exists(path), "paired LSC fixture is missing");
    Decoded result = extract(path);
    DASSERT(result.valid, "paired LSC config was not recovered");
    const std::string *legacy = result.ids.find("psk_key");
    bool legacy_shape_ok = legacy != 0 && legacy->size() == 37 && is_ascii(*legacy) && is_alnum_ascii(*legacy);
    DASSERT(legacy_shape_ok, "paired LSC legacy PSK has the wrong shape");
    DASSERT(result.has_tls_mode && result.tls_mode == 2, "paired LSC dynamic TLS mode was not recovered");
    DASSERT(uses_tuya_legacy_identity_psk(result),
            "paired LSC did not enable its narrowly gated TLS compatibility mode");
    std::vector<Bytes> cands = tls_psk_candidates(result);
    DASSERT(cands.size() == 1 && cands[0].size() == 37, "paired LSC exposed an unexpected TLS candidate set");

    StrList displayed = masked_values(result, "PSK key");
    bool summary_is_masked = displayed.size() == 1 && displayed[0].size() == legacy->size() &&
                             count_sub(displayed[0], "*") == legacy->size() - 4;
    DASSERT(summary_is_masked, "paired LSC PSK summary is not masked");
    DASSERT(!contains(result.raw, *legacy), "paired LSC raw report exposes the PSK");
    return 0;
}

int tuya_config_dump_tests(const std::string &root)
{
    if (test_paired_ir_psk30_recovery(root) < 0)
        return -1;
    if (test_paired_lsc_legacy_tls_mode(root) < 0)
        return -1;
    printf("Paired Tuya-config regressions passed.\n");
    return 2;
}

} /* namespace tuya_config */
