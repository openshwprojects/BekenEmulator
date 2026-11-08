/* sim_ui: constants, string/JSON helpers, the Var/Pane/Table widget wrappers
 * and the real-emulator adapter. Port of the module level of tools/sim_ui.py. */
#include "sim_ui.h"
#include "netstack.h"
#include "wifipeer.h"
#include "beken/beken_crypto.h"

#include <richedit.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

namespace simui {

std::string ROOT_DIR, FIRMWARE_DIR, DEFAULT_IMAGE, CONFIG_PATH;

/* Useful end-to-end runs (INTERESTING_DUMPS). net_out -1 = the GUI default. */
const Profile INTERESTING_DUMPS[] = {
    {"OBK T", "OpenBK7231T_QIO_1.18.300.bin", "BK7231", "TUYA", true, false, -1, 0},
    {"OBK N", "OpenBK7231N_QIO_1.18.300.bin", "BK7231N", "TUYA", true, false, -1, 0},
    {"OBK 7238", "OpenBK7238_QIO_1.18.300.bin", "BK7238", "(none)", true, false, -1, 0},
    {"OBK 7252", "OpenBK7252_QIO_1.18.300.bin", "BK7252", "(none)", true, false, -1, 0},
    {"WB3S relay", "BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin", "BK7231", "TUYA", true, false, -1, 0},
    {"MOES relay", "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin", "BK7231", "(none)", true, false, -1, 0},
    {"BP5758D bulb", "BK7238_Zengge_ZJ21412_BP101_RGBCCT_Bulb.bin", "BK7238", "(none)", true, false, -1, 0},
    {"PWM bulb", "BK7231T_Tuya_A60_SmartBulb_RGBCWW_5ch_PWM_1000Hz.bin", "BK7231", "TUYA", true, false, -1, 0},
    {"BL0942 meter", "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin", "BK7231", "TUYA", true, false, -1, 0},
    {"BL0937 plug", "BK7231T_Tuya_BN-LINK_Plug_WB2S_1.0.3.bin", "BK7231", "TUYA", true, false, -1, 0},
    {"OBK sleep", "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin", "BK7231", "TUYA", true,
     false, -1, "DeepSleep 10\r\n"},
    {"PIR sleep", "BK7231N_Tuya_Generic_Motion_Sensor_CBU_1.0.6.bin", "BK7231N", "TUYA", true, false, -1, 0},
    {"Rain sleep", "BK7231N_Tuya_Rain_Sensor_CBU_1.1.9.bin", "BK7231N", "TUYA", true, false, -1, 0},
    {"TH01 sleep", "BK7238_Tuya_SRW-TH01_TempHum_T1-AHL_1.0.1.bin", "BK7238", "(none)", true, false, -1, 0},
    {"HA15 cam", "BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin", "BK7252", "(none)", true, false, -1, 0},
    {"Pinmei cam", "BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin", "BK7252N", "(none)", true, false, -1,
     "test_W291M\n"},
    {"GBDQA breaker", "BK7238_GBDQA_63V_16A.bin", "BK7238", "(none)", true, true, 1, 0},
    {"IR blaster", "BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin", "BK7238", "(none)", true, true, 1, 0},
    {"LSC bulb", "BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin", "BK7231N", "TUYA", true, true, 1, 0},
};
const int N_INTERESTING = sizeof(INTERESTING_DUMPS) / sizeof(INTERESTING_DUMPS[0]);

const int POLL_MS = 60;
const int MAX_LINES = 4000;
const char *FRAME_PRIVACY_NOTE = "Raw frames and saved PCAPs can contain identities or credentials "
                                 "that firmware sends in clear.";
const u64 GUEST_SECOND_INSNS = SLOW_TICK_INSNS * (1000 / SLOW_TICK_MS);
const char *QUICK_COMMANDS[] = {
    "setChannel 1 1",
    "setChannel 1 5*2",
    "setChannel 1 $CH1+2",
    "addChannel 1 7",
    "setChannel 1 0",
    "toggleChannel 1",
    "getChannel 1",
    "setPinRole 1 AlwaysHigh",
    "setPinRole 1 AlwaysLow",
    "DeepSleep 10",
    "backlog setPinRole 10 Btn; PinDeepSleep",
};
const int N_QUICK = sizeof(QUICK_COMMANDS) / sizeof(QUICK_COMMANDS[0]);
const int QUICK_PER_ROW = 4;
const char *QUICKTICK = "Info:MAIN:quicktick";
const char *NET_TITLE = "network - virtual Wi-Fi and IP bridge";
const size_t MAX_FRAMES = 20000;
const char *ALL_FRAME_TARGETS = "All targets";
const char *WIFI_HOWTO = "Enable \"Wi-Fi peer\" on the Config tab before Run";
std::string NET_IDLE_HINT = strf("Nothing on air yet. %s to attach the virtual radio and the IP bridge; this "
                                 "pane then shows every frame and connection, and what the device sent "
                                 "inside them.",
                                 "Enable \"Wi-Fi peer\" on the Config tab before Run");
const char *LED_OFF_HEX = "#181C22";

const OptDef OPTION_DEFAULTS[] = {
    {"boot", true, false, ""},
    {"xvr_selfclear", true, false, ""},
    {"blecore", true, false, ""},
    {"tuyamcu", true, false, ""},
    {"tuyamcu_pid", false, false, ""},
    {"tuyamcu_raw", true, false, ""},
    {"wifi", true, true, ""},
    {"net_out", true, true, ""},
    {"wifi_role", false, false, "auto"},
    {"wifi_ssid", false, false, ""},
    {"wifi_pass", false, false, ""},
    {"fwd_local", false, false, "8080"},
    {"fwd_device", false, false, "80"},
    {"pcap", true, false, ""},
    {"pcap_path", false, false, ""},
    {"http_gets", false, false, ""},
    {"mqtt_pubs", false, false, ""},
    {"rom_path", false, false, ""},
    {"xtal_mhz", false, false, ""},
    {"psram_mb", false, false, ""},
};
const int N_OPTIONS = sizeof(OPTION_DEFAULTS) / sizeof(OPTION_DEFAULTS[0]);

bool is_text_option(const std::string &name) { return name == "http_gets" || name == "mqtt_pubs"; }
bool is_unsaved_option(const std::string &name) { return name == "wifi_pass" || name == "net_out"; }

/* ------------------------------------------------------------ strings */
std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    if (n <= 0) /* not UTF-8: take it as Latin-1 */
        return latin1_widen(s);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}

std::wstring latin1_widen(const Bytes &b)
{
    std::wstring w(b.size(), L'\0');
    for (size_t i = 0; i < b.size(); i++)
        w[i] = (wchar_t)(u8)b[i];
    return w;
}

std::string guess_chip(const std::string &path)
{
    std::string name = upper(path);
    if (contains(name, "ESP8266")) return "ESP8266";
    if (contains(name, "ESP32")) return "ESP32";
    std::string c = periph::chip_from_name(path);
    if (c == "BK7231U" || c == "BK7238" || c == "BK7252N" || c == "BK7252")
        return c;
    return "BK7231";
}

std::string abs_path(const std::string &p)
{
    if (p.empty())
        return abs_path(".");
    std::wstring w = widen(p);
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(w.c_str(), 4096, buf, 0);
    if (n == 0 || n >= 4096)
        return p;
    std::wstring r(buf, n);
    /* abspath drops a trailing separator (except a root like C:\) */
    while (r.size() > 3 && (r[r.size() - 1] == L'\\' || r[r.size() - 1] == L'/'))
        r.erase(r.size() - 1);
    return narrow(r);
}

std::string norm_path(const std::string &p)
{
    std::wstring w = widen(abs_path(p));
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] == L'/')
            w[i] = L'\\';
    if (!w.empty())
        CharLowerBuffW(&w[0], (DWORD)w.size());
    return narrow(w);
}

bool same_file(const std::string &a, const std::string &b) { return norm_path(a) == norm_path(b); }

bool path_isfile(const std::string &p)
{
    if (p.empty())
        return false;
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool path_isdir(const std::string &p)
{
    if (p.empty())
        return false;
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Decode one UTF-8 code point; returns bytes used (1 for an invalid byte). */
static size_t utf8_next(const std::string &s, size_t i, u32 &cp)
{
    u8 c = (u8)s[i];
    cp = c;
    if (c < 0x80)
        return 1;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0 || i + n >= s.size())
        return 1;
    u32 v = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        u8 d = (u8)s[i + k];
        if ((d & 0xC0) != 0x80)
            return 1;
        v = (v << 6) | (d & 0x3F);
    }
    cp = v;
    return (size_t)n + 1;
}

std::string py_repr(const std::string &s)
{
    bool has_sq = s.find('\'') != std::string::npos, has_dq = s.find('"') != std::string::npos;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string out(1, q);
    for (size_t i = 0; i < s.size();) {
        u32 cp;
        size_t n = utf8_next(s, i, cp);
        if (cp == (u32)q || cp == '\\') {
            out += '\\';
            out += (char)cp;
        } else if (cp == '\n')
            out += "\\n";
        else if (cp == '\r')
            out += "\\r";
        else if (cp == '\t')
            out += "\\t";
        else if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0))
            out += strf("\\x%02x", cp);
        else if (cp >= 0x80 && n == 1)
            out += strf("\\x%02x", cp);
        else
            out += s.substr(i, n);
        i += n;
    }
    out += q;
    return out;
}

StrList splitlines(const std::string &s)
{
    StrList out;
    std::string cur;
    size_t i = 0;
    bool open = false;
    while (i < s.size()) {
        u8 c = (u8)s[i];
        size_t brk = 0;
        if (c == '\r')
            brk = (i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1;
        else if (c == '\n' || c == 0x0B || c == 0x0C || c == 0x1C || c == 0x1D || c == 0x1E)
            brk = 1;
        else if (c == 0xC2 && i + 1 < s.size() && (u8)s[i + 1] == 0x85)
            brk = 2;
        else if (c == 0xE2 && i + 2 < s.size() && (u8)s[i + 1] == 0x80
                 && ((u8)s[i + 2] == 0xA8 || (u8)s[i + 2] == 0xA9))
            brk = 3;
        if (brk) {
            out.push_back(cur);
            cur.clear();
            open = false;
            i += brk;
        } else {
            cur += (char)c;
            open = true;
            i++;
        }
    }
    if (open)
        out.push_back(cur);
    return out;
}

static std::string json_quote(const std::string &s, bool ascii)
{
    std::string out = "\"";
    for (size_t i = 0; i < s.size();) {
        u32 cp;
        size_t n = utf8_next(s, i, cp);
        switch (cp) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (cp < 0x20)
                out += strf("\\u%04x", cp);
            else if (cp >= 0x80 && ascii) {
                if (cp >= 0x10000) {
                    u32 v = cp - 0x10000;
                    out += strf("\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else
                    out += strf("\\u%04x", cp);
            } else
                out += s.substr(i, n);
        }
        i += n;
    }
    return out + "\"";
}

std::string json_quote_ascii(const std::string &s) { return json_quote(s, true); }
std::string json_quote_utf8(const std::string &s) { return json_quote(s, false); }

std::string json_pretty(const JsonValue &v, int indent, int level)
{
    std::string pad((size_t)(indent * (level + 1)), ' '), pad0((size_t)(indent * level), ' ');
    switch (v.type()) {
    case JsonValue::NUL: return "null";
    case JsonValue::BOOL: return v.as_bool() ? "true" : "false";
    case JsonValue::INT: return strf("%lld", v.as_int());
    case JsonValue::REAL: {
        double d = v.as_real();
        if (d != d)
            return "NaN";
        if (d > 1.7976931348623157e308)
            return "Infinity";
        if (d < -1.7976931348623157e308)
            return "-Infinity";
        return py_float_repr(d);
    }
    case JsonValue::STR: return json_quote_utf8(v.as_str());
    case JsonValue::ARR: {
        if (v.arr().empty())
            return "[]";
        std::string out = "[\n";
        for (size_t i = 0; i < v.arr().size(); i++) {
            out += pad + json_pretty(v.arr()[i], indent, level + 1);
            out += (i + 1 < v.arr().size()) ? ",\n" : "\n";
        }
        return out + pad0 + "]";
    }
    case JsonValue::OBJ: {
        const std::vector<std::pair<std::string, JsonValue> > &m = v.members();
        if (m.empty())
            return "{}";
        std::string out = "{\n";
        for (size_t i = 0; i < m.size(); i++) {
            out += pad + json_quote_utf8(m[i].first) + ": " + json_pretty(m[i].second, indent, level + 1);
            out += (i + 1 < m.size()) ? ",\n" : "\n";
        }
        return out + pad0 + "}";
    }
    }
    return "null";
}

/* \d+\.\d+\.\d+\.\d+ at i; end = one past the match */
static bool match_ipv4(const std::string &s, size_t i, size_t &end)
{
    size_t p = i;
    for (int part = 0; part < 4; part++) {
        size_t st = p;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9')
            p++;
        if (p == st)
            return false;
        if (part < 3) {
            if (p >= s.size() || s[p] != '.')
                return false;
            p++;
        }
    }
    end = p;
    return true;
}

std::string filter_from_line(const std::string &line)
{
    /* re.findall(r"\d+\.\d+\.\d+\.\d+:(\d+)") */
    StrList ports;
    for (size_t i = 0; i < line.size();) {
        size_t e;
        if (match_ipv4(line, i, e) && e < line.size() && line[e] == ':' && e + 1 < line.size()
            && line[e + 1] >= '0' && line[e + 1] <= '9') {
            size_t p = e + 1;
            while (p < line.size() && line[p] >= '0' && line[p] <= '9')
                p++;
            ports.push_back(line.substr(e + 1, p - e - 1));
            i = p;
        } else
            i++;
    }
    for (size_t k = 0; k < ports.size(); k++) {
        long long v;
        if (parse_int(ports[k], 10, v) && v >= 1024)
            return ":" + ports[k];
    }
    if (!ports.empty())
        return ":" + ports[0];
    for (size_t i = 0; i < line.size(); i++) {
        size_t e;
        if (match_ipv4(line, i, e))
            return line.substr(i, e - i);
    }
    return "";
}

std::string net_tag(const std::string &line)
{
    if (startswith(line, "[WIFI]"))
        return "wifi";
    if (startswith(line, "[SLEEP]"))
        return "payload";
    static const char *marks[] = {" sends: ", "MQTT PUBLISH", "MQTT -> device", "Tuya LAN broadcast",
                                  "HTTP GET", "answered 200", 0};
    for (int i = 0; marks[i]; i++)
        if (line.find(marks[i]) != std::string::npos)
            return "payload";
    return "net";
}

bool gpio_is_input(bool have, u32 raw) { return have && (raw & 0x0C) == 0x0C && !(raw & 0x40); }

static bool all_digits(const std::string &s, size_t from, size_t to, bool allow_empty)
{
    if (to <= from)
        return allow_empty;
    for (size_t i = from; i < to; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

/* ^(?:bt(?:\d+)?_pin|bt|k\d+pin_pin|key\d*_pin|reset_pin|total_bt_pin)$ */
static bool button_name(const std::string &n)
{
    if (n == "bt" || n == "reset_pin" || n == "total_bt_pin")
        return true;
    if (startswith(n, "bt") && endswith(n, "_pin") && n.size() >= 6)
        return all_digits(n, 2, n.size() - 4, true);
    if (startswith(n, "k") && endswith(n, "pin_pin") && n.size() > 8)
        return all_digits(n, 1, n.size() - 7, false);
    if (startswith(n, "key") && endswith(n, "_pin") && n.size() >= 7)
        return all_digits(n, 3, n.size() - 4, true);
    return false;
}

std::map<int, int> tuya_button_levels(const std::vector<std::pair<std::string, std::string> > *pairs_in)
{
    std::map<int, int> out;
    if (!pairs_in)
        return out;
    /* dict(): the last value of a repeated key wins, at the first one's place */
    std::vector<std::pair<std::string, std::string> > pairs;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < pairs_in->size(); i++) {
        const std::pair<std::string, std::string> &kv = (*pairs_in)[i];
        std::map<std::string, size_t>::iterator it = at.find(kv.first);
        if (it != at.end())
            pairs[it->second].second = kv.second;
        else {
            at[kv.first] = pairs.size();
            pairs.push_back(kv);
        }
    }
    for (size_t i = 0; i < pairs.size(); i++) {
        const std::string &name = pairs[i].first;
        if (!button_name(name))
            continue;
        long long pin;
        if (!parse_int(pairs[i].second, 0, pin))
            continue;
        std::string stem = endswith(name, "_pin") ? name.substr(0, name.size() - 4) : name;
        std::map<std::string, size_t>::iterator it = at.find(stem + "_lv");
        if (it == at.end())
            continue;
        const std::string &lv = pairs[it->second].second;
        if (lv == "0" || lv == "1")
            out[(int)pin] = lv == "1" ? 1 : 0;
    }
    return out;
}

/* ------------------------------------------------------- main.py checks */
bool check_tuyamcu_pid(const std::string &pid, std::string &err)
{
    if (!pid.empty() && !is_ascii(pid)) {
        err = "bad --tuyamcu-pid " + py_repr(pid) + " (a Tuya product id is plain ASCII)";
        return false;
    }
    return true;
}

bool check_wifi_pass(const std::string &pass, std::string &err)
{
    size_t n = utf8_length(pass);
    if (!(n >= 8 && n <= 63)) {
        err = strf("bad --wifi-pass (a WPA2 passphrase is 8 to 63 characters; this one is %d)", (int)n);
        return false;
    }
    return true;
}

bool parse_forward(const std::string &spec, std::pair<int, int> &out, std::string &err)
{
    StrList parts = split(spec, ":");
    bool ok = parts.size() == 2;
    long long v[2] = {0, 0};
    for (size_t i = 0; ok && i < 2; i++) {
        ok = !parts[i].empty() && is_digits(parts[i]) && parse_int(parts[i], 10, v[i]) && v[i] > 0 && v[i] < 65536;
    }
    if (!ok) {
        err = "bad --net-forward " + py_repr(spec) + " (want LPORT:DPORT, ports 1-65535, e.g. 8080:80)";
        return false;
    }
    out = std::make_pair((int)v[0], (int)v[1]);
    return true;
}

bool parse_http_gets(const StrList &paths, std::string &err)
{
    for (size_t i = 0; i < paths.size(); i++) {
        Bytes l1;
        if (!utf8_to_latin1(paths[i], l1)) {
            err = "bad --net-http-get " + py_repr(paths[i])
                  + " (percent-encode characters outside Latin-1, e.g. %20 for a space)";
            return false;
        }
    }
    return true;
}

bool parse_mqtt_pubs(const StrList &specs, std::vector<std::pair<std::string, Bytes> > &out, std::string &err)
{
    out.clear();
    for (size_t i = 0; i < specs.size(); i++) {
        std::string topic, payload;
        bool sep = partition(specs[i], "=", topic, payload);
        if (!sep || topic.empty()) {
            err = "bad --mqtt-publish " + py_repr(specs[i]) + " (want TOPIC=PAYLOAD, e.g. cmnd/obk/POWER=ON)";
            return false;
        }
        out.push_back(std::make_pair(topic, Bytes(payload)));
    }
    return true;
}

bool parse_hex_blob(const std::string &spec, Bytes &out, std::string &err)
{
    std::string text = replace_all(replace_all(spec, " ", ""), "_", "");
    if (lower(text.substr(0, 2)) == "0x")
        text = text.substr(2);
    bool ok = true;
    /* bytes.fromhex: pairs of hex digits (whitespace already removed) */
    if (text.size() % 2)
        ok = false;
    for (size_t i = 0; ok && i < text.size(); i++)
        ok = isxdigit((u8)text[i]) != 0;
    if (ok)
        out = unhex(text, &ok);
    if (!ok) {
        err = "bad --tuyamcu-inject " + py_repr(spec) + " (want hex bytes, e.g. 55AA001C00001B)";
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- Tuya cfg */
static int g_tuya_ident = 0;
int new_tuya_ident() { return ++g_tuya_ident; }

bool tuya_extract_real(const std::string &path, TuyaCfg &out, std::string &err)
{
    (void)err;
    out = TuyaCfg();
    tuya_config::Decoded d = tuya_config::extract(path);
    if (!d.valid)
        return true; /* None */
    out.present = true;
    out.ident = new_tuya_ident();
    out.pairs = d.pairs;
    out.human = d.human;
    out.encrypted = d.encrypted;
    out.offset = (long)d.offset;
    out.dps = (int)d.dps.size();
    out.dec = d;
    return true;
}

/* ------------------------------------------------------------- Var */
std::string Var::get() const
{
    switch (kind) {
    case EDIT: {
        int n = GetWindowTextLengthW(hwnd);
        std::wstring w((size_t)n + 1, L'\0');
        GetWindowTextW(hwnd, &w[0], n + 1);
        w.resize((size_t)n);
        return narrow(w);
    }
    case MULTI: {
        int n = GetWindowTextLengthW(hwnd);
        std::wstring w((size_t)n + 1, L'\0');
        GetWindowTextW(hwnd, &w[0], n + 1);
        w.resize((size_t)n);
        return replace_all(narrow(w), "\r\n", "\n");
    }
    case CHECK:
        return getb() ? "1" : "0";
    default:
        return shadow;
    }
}

void Var::set(const std::string &v)
{
    if (ui)
        ui->in_set++;
    switch (kind) {
    case EDIT:
        SetWindowTextW(hwnd, widen(v).c_str());
        break;
    case MULTI:
        SetWindowTextW(hwnd, widen(replace_all(replace_all(v, "\r\n", "\n"), "\n", "\r\n")).c_str());
        SendMessageW(hwnd, EM_EMPTYUNDOBUFFER, 0, 0);
        break;
    case LABEL:
        if (hwnd && (v != shadow || window_text(hwnd) != v))
            SetWindowTextW(hwnd, widen(v).c_str());
        shadow = v;
        break;
    case COMBO: {
        shadow = v;
        LRESULT idx = SendMessageW(hwnd, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)widen(v).c_str());
        SendMessageW(hwnd, CB_SETCURSEL, idx == CB_ERR ? (WPARAM)-1 : (WPARAM)idx, 0);
        break;
    }
    case CHECK:
        setb(v == "1");
        break;
    default:
        shadow = v;
    }
    if (ui)
        ui->in_set--;
    if (traced && ui && kind != CHECK)
        ui->options_changed();
}

bool Var::getb() const
{
    if (kind == CHECK && hwnd)
        return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return bshadow;
}

void Var::setb(bool v)
{
    bshadow = v;
    if (kind == CHECK && hwnd)
        SendMessageW(hwnd, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    if (traced && ui)
        ui->options_changed();
}

bool enabled(HWND h) { return h && IsWindowEnabled(h); }

std::string window_text(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(h, &w[0], n + 1);
    w.resize((size_t)n);
    return narrow(w);
}

/* Only when it changes: re-setting the same text or state every poll would
 * repaint (and flicker) the control 16 times a second. */
void set_window_text(HWND h, const std::string &s)
{
    if (h && window_text(h) != s)
        SetWindowTextW(h, widen(s).c_str());
}
void set_state(HWND h, bool on)
{
    if (h && (IsWindowEnabled(h) != 0) != on)
        EnableWindow(h, on ? TRUE : FALSE);
}

/* ------------------------------------------------------------- Pane */
static long pane_len(const Pane &p)
{
    GETTEXTLENGTHEX gl;
    gl.flags = GTL_NUMCHARS | GTL_PRECISE;
    gl.codepage = 1200;
    return (long)SendMessageW(p.hwnd, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}

static std::wstring pane_range(const Pane &p, long a, long b)
{
    if (b <= a)
        return std::wstring();
    std::wstring w((size_t)(b - a) + 1, L'\0');
    TEXTRANGEW tr;
    tr.chrg.cpMin = a;
    tr.chrg.cpMax = b;
    tr.lpstrText = &w[0];
    LRESULT n = SendMessageW(p.hwnd, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    w.resize((size_t)n);
    return w;
}

static std::wstring pane_wtext(const Pane &p)
{
    return pane_range(p, 0, pane_len(p));
}

std::string pane_text(const Pane &p)
{
    std::wstring w = pane_wtext(p);
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] == L'\r')
            w[i] = L'\n';
    return narrow(w);
}

void pane_clear(Pane &p)
{
    SendMessageW(p.hwnd, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(p.hwnd, L"");
    SendMessageW(p.hwnd, EM_SETREADONLY, TRUE, 0);
    p.newlines = 0;
}

std::set<std::string> pane_tags_of(Pane &p, const std::string &needle)
{
    std::set<std::string> out;
    std::wstring all = pane_wtext(p), wn = widen(needle);
    for (size_t i = 0; i < wn.size(); i++)
        if (wn[i] == L'\n')
            wn[i] = L'\r';
    size_t at = all.find(wn);
    if (at == std::wstring::npos)
        return out;
    CHARRANGE save, cr;
    SendMessageW(p.hwnd, EM_EXGETSEL, 0, (LPARAM)&save);
    cr.cpMin = (LONG)at;
    cr.cpMax = (LONG)at + 1;
    SendMessageW(p.hwnd, EM_EXSETSEL, 0, (LPARAM)&cr);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    SendMessageW(p.hwnd, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(p.hwnd, EM_EXSETSEL, 0, (LPARAM)&save);
    for (size_t k = 0; k < p.tags.size(); k++)
        if (p.tags[k].second == cf.crTextColor) {
            out.insert(p.tags[k].first);
            break;
        }
    return out;
}

/* ------------------------------------------------------------- Table */
void Table::clear()
{
    quiet++;
    SendMessageW(hwnd, LVM_DELETEALLITEMS, 0, 0);
    quiet--;
    iids.clear();
    tags.clear();
}

void Table::insert(const std::string &iid, const StrList &values, const std::string &tag)
{
    int idx = (int)iids.size();
    iids.push_back(iid);
    tags.push_back(tag);
    quiet++;
    for (int c = 0; c < ncols; c++) {
        std::wstring w = widen(c < (int)values.size() ? values[c] : std::string());
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = idx;
        it.iSubItem = c;
        it.pszText = (LPWSTR)w.c_str();
        if (c == 0)
            SendMessageW(hwnd, LVM_INSERTITEMW, 0, (LPARAM)&it);
        else
            SendMessageW(hwnd, LVM_SETITEMTEXTW, (WPARAM)idx, (LPARAM)&it);
    }
    quiet--;
}

int Table::index_of(const std::string &iid) const
{
    for (size_t i = 0; i < iids.size(); i++)
        if (iids[i] == iid)
            return (int)i;
    return -1;
}

StrList Table::item(const std::string &iid) const
{
    StrList out;
    int idx = index_of(iid);
    if (idx < 0)
        return out;
    for (int c = 0; c < ncols; c++) {
        wchar_t buf[4096];
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.iSubItem = c;
        it.pszText = buf;
        it.cchTextMax = 4096;
        LRESULT n = SendMessageW(hwnd, LVM_GETITEMTEXTW, (WPARAM)idx, (LPARAM)&it);
        out.push_back(narrow(std::wstring(buf, (size_t)n)));
    }
    return out;
}

std::string Table::selection() const
{
    LRESULT i = SendMessageW(hwnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (i < 0 || i >= (LRESULT)iids.size())
        return "";
    return iids[(size_t)i];
}

void Table::selection_remove()
{
    quiet++;
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = 0;
    SendMessageW(hwnd, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
    quiet--;
}

void Table::selection_set(const std::string &iid)
{
    int idx = index_of(iid);
    selection_remove();
    if (idx < 0)
        return;
    quiet++;
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(hwnd, LVM_SETITEMSTATE, (WPARAM)idx, (LPARAM)&it);
    quiet--;
}

int Table::top_index() const { return (int)SendMessageW(hwnd, LVM_GETTOPINDEX, 0, 0); }

void Table::restore_top(int idx)
{
    int now = top_index();
    if (now == idx || iids.empty())
        return;
    RECT rc;
    rc.left = LVIR_BOUNDS;
    if (!SendMessageW(hwnd, LVM_GETITEMRECT, 0, (LPARAM)&rc))
        return;
    int h = rc.bottom - rc.top;
    SendMessageW(hwnd, LVM_SCROLL, 0, (LPARAM)((idx - now) * h));
}

void Table::see(const std::string &iid)
{
    int idx = index_of(iid);
    if (idx >= 0)
        SendMessageW(hwnd, LVM_ENSUREVISIBLE, (WPARAM)idx, FALSE);
}

/* ------------------------------------------------------------- RealEmu */
RealEmu::~RealEmu() { if (owner_) delete owner_; else delete e_; }

bool RealEmu::attach_wifi_peer(const PeerArgs &a, std::string &err)
{
    WifiPeer *p = e_->attach_wifi_peer(a.role, a.has_ssid ? a.ssid : std::string(), 1, H("020000000001"),
                                       a.has_pass ? a.passphrase : std::string(), a.forwards, a.net_out,
                                       a.has_pcap ? a.pcap : std::string(), a.http_gets, a.mqtt_pubs,
                                       a.tls_psks, a.tuya_legacy_identity_psk);
    if (!p) {
        err = "RuntimeError: the image has no Wi-Fi MAC to attach a peer to";
        return false;
    }
    return true;
}

bool RealEmu::set_gpio_input(int pin, int level, std::string &err)
{
    if (level != -1 && level != 0 && level != 1) {
        err = strf("ValueError: bad GPIO level %d", level);
        return false;
    }
    e_->set_gpio_input(pin, level);
    return true;
}

bool RealEmu::view(EmuView &out)
{
    e_->ui_view(out);
    return true;
}

bool RealEmu::forward_listening()
{
    EmuView v;
    e_->ui_view(v);
    return v.forward_listening;
}

/* The architecture-neutral adapter never exposes the live CPU to the UI.
 * Status and GPIO are snapshots, and input is queued to the worker thread. */
class BackendEmu : public EmuIface {
    EmulatorBackend *backend_;
    SimUI *ui_;
public:
    BackendEmu(EmulatorBackend *backend, SimUI *ui) : backend_(backend), ui_(ui) {
        backend_->set_uart_sink(&BackendEmu::uart_sink, this);
    }
    ~BackendEmu() { delete backend_; }
    void run() { backend_->run(true); }
    void stop() { backend_->stop(); }
    void pause(bool value) { backend_->pause(value); }
    bool is_paused() { return backend_->is_paused(); }
    void feed_uart1(const Bytes &bytes) { backend_->feed_uart(backend_->info().console_uart, bytes); }
    bool set_gpio_input(int pin, int level, std::string &err) {
        if (backend_->set_gpio_input(pin, level)) return true;
        err = "Invalid GPIO pin or input level.";
        return false;
    }
    bool view(EmuView &out) {
        BackendStatus status = backend_->status();
        out = EmuView(); out.valid = true;
        out.last_pc = status.pc; out.insn_count = status.instructions;
        for (std::map<int, BackendStatus::Gpio>::const_iterator i = status.gpio.begin(); i != status.gpio.end(); ++i)
            /* Translate into the existing UI's normalized input/output bits;
             * these values are presentation data, never chip registers. */
            out.gpio_cfg[i->first] = (i->second.enabled ? 4u : 12u)
                | (i->second.input ? 1u : 0u) | (i->second.output ? 2u : 0u);
        return true;
    }
    bool run_error(std::string &out) {
        BackendStatus status = backend_->status();
        out = status.diagnostic; return status.faulted;
    }
    bool elapsed_us(u64 &out) {
        const BackendStatus status = backend_->status();
        out = status.elapsed_us;
        return status.elapsed_us_known;
    }
    std::string chip_name() { return backend_->info().chip; }
    int gpio_pins() { return (int)backend_->info().gpio_pins; }
    int gpio_slot(int pin) { return pin >= 0 && pin < gpio_pins() ? pin : -1; }
    static void uart_sink(void *ctx, int port, u8 value) {
        BackendEmu *self = (BackendEmu *)ctx;
        SimUI::s_sink(self->ui_, port == (int)self->backend_->info().console_uart ? 1 : 2, value);
    }
};

EmuIface *real_build(SimUI *ui, const std::string &path, const BuildOptions &opt, const std::string &key,
                     std::string &err, void *ctx)
{
    (void)ctx;
    KeyCoefs k;
    bool have_key = false;
    if (key != "(none)") {
        std::string e;
        if (!parse_key(key, k, &e)) {
            err = "ValueError: " + e;
            return 0;
        }
        have_key = k.has;
    }
    std::string e, etype;
    EmulatorBackend *backend = build_backend(path, opt, have_key ? &k : 0, &e, 0, false, &etype);
    if (!backend) {
        /* sim_ui.py shows "%s: %s" % (type(e).__name__, e) */
        err = e.empty() ? std::string("could not load the image") : etype + ": " + e;
        return 0;
    }
    BekenEmulator *emu = backend->beken();
    if (!emu) {
        return new BackendEmu(backend, ui);
    }
    emu->uart_sink = &SimUI::s_sink;
    emu->uart_sink_ctx = ui;
    emu->net_sink = &SimUI::s_net_sink;
    emu->net_sink_ctx = ui;
    emu->net_frame_sink = &SimUI::s_frame_sink;
    emu->net_frame_sink_ctx = ui;
    return new RealEmu(emu, backend);
}

/* ------------------------------------------------------------- paths */
void init_paths()
{
    /* The repository root holds firmwares/; a packaged build keeps it next to
     * the executable. Look upward from the exe, then the working directory. */
    std::string exe = path_dirname(self_exe_path());
    std::string root;
    std::string d = exe;
    for (int i = 0; i < 5 && !d.empty(); i++) {
        if (path_isdir(path_join(d, "firmwares"))) {
            root = d;
            break;
        }
        std::string up = path_dirname(d);
        if (up == d)
            break;
        d = up;
    }
    if (root.empty() && path_isdir(path_join(current_dir(), "firmwares")))
        root = current_dir();
    if (root.empty()) {
        std::string bundled;
        if (ensure_bundled_firmwares(bundled))
            root = bundled;
    }
    if (root.empty())
        root = exe;
    ROOT_DIR = abs_path(root);
    FIRMWARE_DIR = path_join(ROOT_DIR, "firmwares");
    DEFAULT_IMAGE = path_join(FIRMWARE_DIR, "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin");
    /* os.path.expanduser("~") on Windows: USERPROFILE */
    std::string home;
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, 4096);
    if (n > 0 && n < 4096)
        home = narrow(std::wstring(buf, n));
    else {
        std::string drive, hp;
        n = GetEnvironmentVariableW(L"HOMEDRIVE", buf, 4096);
        if (n > 0 && n < 4096)
            drive = narrow(std::wstring(buf, n));
        n = GetEnvironmentVariableW(L"HOMEPATH", buf, 4096);
        if (n > 0 && n < 4096)
            hp = narrow(std::wstring(buf, n));
        home = drive + hp;
    }
    CONFIG_PATH = home.empty() ? std::string(".bekensim_ui.json") : path_join(home, ".bekensim_ui.json");
}

} /* namespace simui */
