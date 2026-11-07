/* C++ port of tests/self_test.py's runner (everything after TEST_CASES /
 * DESCRIPTIONS, which tools/gen_cases.py turns into testcase_data.cpp).
 *
 * Spawns the emulator per case, streams its output (decoded like Python's
 * text-mode pipe: UTF-8 with invalid bytes dropped, universal newlines),
 * stops once every expected string has its count and the peripheral state is
 * captured, then checks expected/unexpected strings, pcap captures, GPIO pin
 * and PWM assertions; snapshot cases do the save/resume round trip. Results
 * feed the same HTML report as tests/report.py (report.cpp).
 *
 *   self_test [-k PATTERN]... [-j N | --jobs N] [-v] [--python] [--emu PATH]
 *             [--cap-timeout S] [--report PATH] [--results-json PATH] [--table]
 *
 * Like Python: -k filters by name (case-insensitive, repeatable); the default
 * job count is env JOBS, else min(6, cpus) (min(4, cpus) under CI); env LINGER
 * / CI / REPEATS work the same; report/index.html is written only on an
 * unfiltered run. C++-only: --emu (the emulator binary, default bekensim.exe),
 * --python (drive src/main.py instead), --cap-timeout (cap every case's
 * timeout), --report PATH (always write the report there, filtered or not),
 * --results-json PATH (the raw results, for cross-checking the report against
 * report.py), -v (Python's full per-case output), --table (accepted, no-op).
 */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "testcase.h"
#include "report.h"
#include "common.h"
#include "platform.h"
#include "periph.h"
#include "tuya_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

/* ------------------------------------------------------------ globals */
static double g_linger = 0;   /* LINGER: seconds to keep capturing after a pass */
static int g_repeats = 2;     /* REPEATS */
static Mutex g_lock;          /* temp files, port claims, process spawning */
static std::set<int> g_claimed_ports;

static const char *SEP = "=====================================";

struct Runner {
    StrList emu_argv_prefix; /* {"<abs>/bekensim.exe"} or {"python", "<abs>/src/main.py"} */
    std::string root;
    bool use_python;
    bool verbose;
};

/* What one case produced: the report record plus what the console shows. */
struct CaseOut {
    report::Result res;
    std::string log;           /* Python's printed lines for this case */
    StrList fail_lines;        /* the "  [FAIL] ..." lines, shown even when not verbose */
    int markers_hit, markers_total;
    bool early;                /* binary missing / launch failed: no periph/uart1 keys */
    StrList periph_used;       /* the [EMU_GPIO]/[EMU_PWM] lines the decode used */
    CaseOut() : markers_hit(0), markers_total(0), early(false) {}
};

static void say(CaseOut &o, const std::string &line) { o.log += line + "\n"; }

/* ------------------------------------------------------ case metadata */
static std::string dump_url(const std::string &binary)
{
    std::string repo = env_has("GITHUB_REPOSITORY") ? env_get("GITHUB_REPOSITORY")
                                                    : std::string("openshwprojects/BekenEmulator");
    std::string ref = env_get("GITHUB_SHA");
    if (ref.empty())
        ref = "main";
    return "https://github.com/" + repo + "/raw/" + ref + "/firmwares/" + path_basename(binary);
}

static std::string chip_of(const TestCase &tc)
{
    std::string name = periph::chip_from_name(tc.binary);
    if (!name.empty())
        return name;
    for (int i = 0; i < tc.n_args; i++) {
        std::string a = tc.args[i];
        if ((a == "-chip" || a == "--chip") && i + 1 < tc.n_args)
            return upper(tc.args[i + 1]);
    }
    return "BK7231";
}

/* _parse_expected, with REPEATS applied the way Python evaluates it */
static std::vector<std::pair<std::string, int> > parse_expected(const TestCase &tc)
{
    std::vector<std::pair<std::string, int> > out;
    for (int k = 0; k < tc.n_expected; k++)
        out.push_back(std::make_pair(std::string(tc.expected[k].s),
                                     tc.expected[k].repeats ? g_repeats : tc.expected[k].count));
    return out;
}

static bool has_tag(const std::vector<report::Tag> &tags, const std::string &n)
{
    for (size_t i = 0; i < tags.size(); i++)
        if (tags[i].name == n)
            return true;
    return false;
}

/* tags_for(): the report tags inferred from the case itself */
static std::vector<report::Tag> tags_for(const TestCase &tc)
{
    std::string name = tc.name;
    std::string base = path_basename(tc.binary);
    StrList exps;
    for (int k = 0; k < tc.n_expected; k++)
        exps.push_back(tc.expected[k].s);
    std::string exp = join(exps, " ");
    std::string hay = name + " " + base + " " + exp;
    std::vector<report::Tag> tags;

    if (startswith(name, "CLI:"))
        tags.push_back(report::Tag("CLI", "source"));
    else if (contains(base, "OpenBK") || contains(base, "mathDemo"))
        tags.push_back(report::Tag("OpenBeken", "source"));
    else
        tags.push_back(report::Tag("Tuya", "source"));

    tags.push_back(report::Tag(chip_of(tc), "chip"));

    if (contains(exp, "mf_init succ") || contains(exp, "have actived"))
        tags.push_back(report::Tag("paired", "state"));
    else if (contains(exp, "mf_test"))
        tags.push_back(report::Tag("pre-pair", "state"));

    bool is_obk = has_tag(tags, "OpenBeken");
    StrList feat;
    if (contains(base, "TuyaMCU") || contains(name, "TuyaMCU") || contains(exp, "55 aa 00 00"))
        feat.push_back("TuyaMCU");
    if (contains(exp, "UART1/MCU"))
        feat.push_back("UART1");
    if (contains(hay, "uartSendHex"))
        feat.push_back("uartSendHex");
    if (contains(exp, "CFG_InitAndLoad") || contains(base, "obkStartupCommand"))
        feat.push_back("OBK config");
    if (contains(exp, "key_addr") || contains(exp, "get key") || contains(exp, "get kvs") || contains(exp, "kv info"))
        feat.push_back("flash/KV");
    if (contains(exp, "idle ") || contains(name, "1s timer"))
        feat.push_back("timers");
    if (contains(exp, "MQTT_RegisterCallback"))
        feat.push_back("MQTT");
    if (contains(exp, "rwble") || contains(exp, "STACK INIT") || contains(exp, "advertising"))
        feat.push_back("BLE");
    if (is_obk && (contains(base, "7231N") || contains(base, "7231M")))
        feat.push_back("SARADC");
    if (tc.n_pins > 0)
        feat.push_back("GPIO");
    if (contains(exp, "BERRY:") || contains(base, "berry"))
        feat.push_back("Berry");
    if (contains(exp, "Float basic"))
        feat.push_back("float math");
    if (contains(exp, "Invalid -key") || contains(exp, "ARM code"))
        feat.push_back("crypto");
    if (contains(name, "SPI mirror"))
        feat.push_back("SPI mirror");
    if (contains(exp, "[WIFI] "))
        feat.push_back("Wi-Fi");
    if (contains(exp, "[NET] "))
        feat.push_back("IP network");
    if (contains(exp, "[HTTP_PROBE]"))
        feat.push_back("localhost forward");
    if (contains(exp, "[NET] MQTT"))
        feat.push_back("MQTT");
    if (contains(exp, "[NET] NTP"))
        feat.push_back("NTP");
    if (contains(exp, "Tuya LAN"))
        feat.push_back("Tuya LAN");
    if (contains(exp, "TLS ClientHello"))
        feat.push_back("cloud capture");
    if (tc.n_pcap > 0)
        feat.push_back("pcap");
    if (std::string(tc.kind) == "snapshot_roundtrip")
        feat.push_back("snapshot");
    if (contains(name, "XVR"))
        feat.push_back("XVR");
    for (size_t i = 0; i < feat.size(); i++)
        tags.push_back(report::Tag(feat[i], "feature"));
    for (int i = 0; i < tc.n_tags; i++)
        if (!has_tag(tags, tc.tags[i]))
            tags.push_back(report::Tag(tc.tags[i], "feature"));
    return tags;
}

static void add_derived_tags(report::Result &r)
{
    StrList derived;
    if (r.tuya_config.valid)
        derived.push_back("has Tuya config");
    if (r.has_periph) {
        bool any = false;
        for (size_t i = 0; i < r.periph.gpio.size(); i++)
            if (r.periph.gpio[i].state != "factory")
                any = true;
        if (any)
            derived.push_back("has GPIO data");
        if (!r.periph.pwm_pins.empty())
            derived.push_back("has PWM");
    }
    if (r.timed_out)
        derived.push_back("timed out");
    std::vector<report::Tag> have = r.tags;
    for (size_t i = 0; i < derived.size(); i++)
        if (!has_tag(have, derived[i]))
            r.tags.push_back(report::Tag(derived[i], "data"));
}

/* _record_check: print + record one non-string assertion; returns ok */
static bool record_check(CaseOut &o, std::vector<report::Check> &checks, const std::string &label, bool ok,
                         const std::string &detail)
{
    std::string line = std::string("  [") + (ok ? "PASS" : "FAIL") + "] " + label
                       + (ok ? " (" + detail + ")" : ", got " + detail);
    say(o, line);
    if (!ok)
        o.fail_lines.push_back(line);
    checks.push_back(report::Check(label, ok, ok ? 1 : 0, 1));
    return ok;
}

static tuya_config::Decoded cached_tuya_config(const std::string &binary)
{
    static Mutex tc_lock;
    static std::map<std::pair<std::string, bool>, tuya_config::Decoded> tc_cache;
    std::pair<std::string, bool> key(binary, tuya_config::REDACT_SECRETS);
    {
        ScopedLock lk(tc_lock);
        std::map<std::pair<std::string, bool>, tuya_config::Decoded>::const_iterator it = tc_cache.find(key);
        if (it != tc_cache.end())
            return it->second;
    }
    tuya_config::Decoded ext = tuya_config::extract(binary);
    {
        ScopedLock lk(tc_lock);
        tc_cache[key] = ext;
    }
    return ext;
}

/* _result(): the structured record the HTML report consumes */
static report::Result make_result(const Runner &R, const TestCase &tc, bool passed, double elapsed,
                                  long long insns, bool timed_out, const std::string &output,
                                  const std::vector<report::Check> *checks)
{
    report::Result r;
    std::string binary = path_join(R.root, tc.binary);
    r.name = tc.name;
    r.description = tc.description ? tc.description : "";
    r.binary = binary;
    r.binary_name = path_basename(binary);
    r.chip = chip_of(tc);
    r.tuya_config = cached_tuya_config(binary);
    r.tags = tags_for(tc);
    r.dump_url = dump_url(binary);
    for (int i = 0; i < tc.n_args; i++)
        r.args.push_back(tc.args[i]);
    r.passed = passed;
    r.timed_out = timed_out;
    r.elapsed = elapsed;
    r.insns = insns;
    if (checks) {
        r.checks = *checks;
    } else {
        std::vector<std::pair<std::string, int> > reqs = parse_expected(tc);
        for (size_t i = 0; i < reqs.size(); i++)
            r.checks.push_back(report::Check(reqs[i].first, false, 0, reqs[i].second));
    }
    r.output = output;
    return r;
}

/* ------------------------------------------------------- OS helpers */
static int cpu_count()
{
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(0xFFFF /* ALL_PROCESSOR_GROUPS */);
    if (!n) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
    }
    return n ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* _free_port(): a 127.0.0.1 port nothing listens on, never handed out twice */
static int claim_free_port()
{
    ScopedLock lk(g_lock);
    int port = 0;
    for (int i = 0; i < 32; i++) {
        port = free_tcp_port();
        if (!g_claimed_ports.count(port)) {
            g_claimed_ports.insert(port);
            return port;
        }
    }
    return port;
}

static std::string temp_file(const std::string &prefix, const std::string &suffix)
{
    ScopedLock lk(g_lock);
    return make_temp_file(prefix, suffix);
}

/* Spawn under the global lock: on Windows the pipe's write end is inheritable
 * until start() closes it, and a concurrent CreateProcess in another worker
 * would inherit it - that child would then hold this case's pipe open past
 * its own process's exit. */
static bool spawn(ChildProcess &proc, const StrList &argv)
{
    std::vector<std::pair<std::string, std::string> > env;
    env.push_back(std::make_pair(std::string("EMU_REPORT"), std::string("1")));
    ScopedLock lk(g_lock);
    return proc.start(argv, env);
}

/* The child's stdout as Python's text-mode pipe yields it: universal newlines
 * ("\r\n" and a lone "\r" become "\n") and UTF-8 with invalid bytes dropped
 * (encoding="utf-8", errors="ignore"); a final unterminated line at EOF. */
struct LineReader {
    std::string raw;
    void feed(const std::string &chunk, StrList &out, bool eof)
    {
        raw += chunk;
        size_t start = 0, i = 0;
        for (; i < raw.size(); i++) {
            char c = raw[i];
            if (c == '\n') {
                out.push_back(utf8_ignore(raw.substr(start, i - start) + "\n"));
                start = i + 1;
            } else if (c == '\r') {
                if (i + 1 < raw.size()) {
                    out.push_back(utf8_ignore(raw.substr(start, i - start) + "\n"));
                    if (raw[i + 1] == '\n')
                        i++;
                    start = i + 1;
                } else if (eof) {
                    out.push_back(utf8_ignore(raw.substr(start, i - start) + "\n"));
                    start = i + 1;
                } else {
                    break;
                }
            }
        }
        raw.erase(0, start);
        if (eof && !raw.empty()) {
            out.push_back(utf8_ignore(raw));
            raw.clear();
        }
    }
};

/* --------------------------------------------------------- pcap checks */
static bool pcap_frames(const std::string &path, int want_link, std::vector<Bytes> &frames, std::string &err)
{
    Bytes raw;
    if (!read_file(path, raw)) {
        err = "[Errno 2] No such file or directory: '" + path + "'";
        return false;
    }
    if (raw.size() < 24) {
        err = strf("%d bytes, no pcap header", (int)raw.size());
        return false;
    }
    u32 magic = rd32le(raw.data());
    u16 vmaj = rd16le(raw.data() + 4), vmin = rd16le(raw.data() + 6);
    u32 link = rd32le(raw.data() + 20);
    if (magic != 0xA1B2C3D4 || vmaj != 2 || vmin != 4 || (int)link != want_link) {
        err = strf("header magic 0x%08X v%d.%d link type %u", magic, vmaj, vmin, link);
        return false;
    }
    size_t off = 24;
    while (off + 16 <= raw.size()) {
        u32 incl = rd32le(raw.data() + off + 8);
        if (off + 16 + incl > raw.size())
            break;
        frames.push_back(raw.substr(off + 16, incl));
        off += 16 + incl;
    }
    return true;
}

static int frames_containing(const std::vector<Bytes> &frames, const std::string &needle)
{
    int n = 0;
    for (size_t f = 0; f < frames.size(); f++)
        if (frames[f].find(needle) != std::string::npos)
            n++;
    return n;
}

/* _check_pcap / _check_ip_pcap */
static bool check_pcap(CaseOut &o, std::vector<report::Check> &checks, const std::string &path,
                       const char *const *expect, int n, bool is_ip)
{
    std::vector<Bytes> frames;
    std::string err;
    bool parsed = pcap_frames(path, is_ip ? 1 : 105, frames, err);
    bool ok = parsed && !frames.empty();
    std::string detail = parsed ? strf("%d frames", (int)frames.size()) : err;
    bool all_ok = record_check(o, checks, is_ip ? "pcap-ip: valid Ethernet capture" : "pcap: valid 802.11 capture",
                               ok, detail);
    for (int i = 0; i < n; i++) {
        std::string item = expect[i];
        int ftype = -1, fsub = -2; /* fsub -1 = any subtype */
        if (!is_ip) {
            if (item == "beacon") { ftype = 0; fsub = 8; }
            else if (item == "probe request") { ftype = 0; fsub = 4; }
            else if (item == "probe response") { ftype = 0; fsub = 5; }
            else if (item == "authentication") { ftype = 0; fsub = 11; }
            else if (item == "association request") { ftype = 0; fsub = 0; }
            else if (item == "association response") { ftype = 0; fsub = 1; }
            else if (item == "data") { ftype = 2; fsub = -1; }
        }
        int cnt = 0;
        std::string label;
        if (ftype >= 0) {
            for (size_t f = 0; f < frames.size(); f++) {
                if (frames[f].size() < 2)
                    continue;
                u8 fc = (u8)frames[f][0];
                if (((fc >> 2) & 3) == ftype && (fsub < 0 || (fc >> 4) == fsub))
                    cnt++;
            }
            label = "pcap: " + item + " frame";
        } else {
            cnt = frames_containing(frames, item);
            label = std::string(is_ip ? "pcap-ip" : "pcap") + ": a frame carries '" + item + "'";
        }
        bool r = record_check(o, checks, label, cnt > 0, cnt ? strf("%d frames", cnt) : std::string("none"));
        all_ok = r && all_ok;
    }
    return all_ok;
}

/* ------------------------------------------------------- run_test state */
/* Shared between the reading loop and the http_probe thread (Python's lock). */
struct RunShared {
    Mutex lock;
    std::string output;                         /* "".join(lines) */
    std::vector<std::pair<std::string, int> > reqs;
    std::vector<int> counts;
    std::vector<bool> done;
    int remaining;
    StrList periph_lines;
    bool has_periph_exp;
    bool have_assert;
    StrList assert_lines;
    StrList uart1;
    long long insns;
    RunShared() : remaining(0), has_periph_exp(false), have_assert(false), insns(-1) {}

    /* caller holds lock */
    void complete_periph_boundary()
    {
        if (has_periph_exp && !have_assert && remaining == 0) {
            assert_lines = periph_lines;
            have_assert = true;
        }
    }
    void ingest(const std::string &line)
    {
        ScopedLock lk(lock);
        output += line;
        for (size_t k = 0; k < reqs.size(); k++) {
            if (done[k])
                continue;
            size_t c = count_sub(line, reqs[k].first);
            if (c) {
                counts[k] += (int)c;
                if (counts[k] >= reqs[k].second) {
                    done[k] = true;
                    remaining--;
                }
            }
        }
    }
};

struct ProbeCtx {
    RunShared *sh;
    int port;
    const TestCase *tc;
};

static std::string py_rstrip_ws(const std::string &s)
{
    return rstrip(s, " \t\r\n\f\v\x1c\x1d\x1e\x1f");
}

/* _http_probe: one request the way a browser sends it; reports via ingest */
static void http_probe(RunShared *sh, int port, const HttpProbe &pr)
{
    std::string method = pr.method ? pr.method : "GET";
    std::string path = pr.path;
    std::string payload = pr.body ? pr.body : "";
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + strf("%d", port)
                      + "\r\nUser-Agent: Mozilla/5.0 (BekenEmulator self-test)\r\n"
                        "Accept: text/html,application/xhtml+xml,application/json\r\nConnection: close\r\n";
    if (pr.body)
        req += strf("Content-Type: text/plain\r\nContent-Length: %d\r\n", (int)payload.size());
    req += "\r\n" + payload;
    std::string data;
    sock_t s = port > 0 ? tcp_connect_blocking("127.0.0.1", port, 180000) : BK_BAD_SOCK;
    if (s == BK_BAD_SOCK) {
        sh->ingest(strf("[HTTP_PROBE] %s %s failed: %s (%d bytes received)\n", method.c_str(), path.c_str(),
                        "[WinError 10061] No connection could be made because the target machine actively refused it",
                        0));
        return;
    }
    bool failed = !sock_send_all(s, Bytes(req));
    if (!failed) {
        char b[65536];
        for (;;) {
            int n = sock_recv(s, b, sizeof(b));
            if (n > 0) {
                data.append(b, n);
                continue;
            }
            if (n < 0)
                failed = true;
            break;
        }
    }
    sock_close(s);
    if (failed) {
        sh->ingest(strf("[HTTP_PROBE] %s %s failed: %s (%d bytes received)\n", method.c_str(), path.c_str(),
                        "timed out", (int)data.size()));
        if (data.empty())
            return;
    }
    std::string head, reply;
    if (!partition(data, "\r\n\r\n", head, reply)) {
        head = data;
        reply.clear();
    }
    std::string status = "(no reply)";
    if (!head.empty()) {
        size_t nl = head.find('\n');
        status = latin1_to_utf8(strip(nl == std::string::npos ? head : head.substr(0, nl)));
    }
    sh->ingest(strf("[HTTP_PROBE] %s %s -> %s (%d bytes)\n", method.c_str(), path.c_str(), status.c_str(),
                    (int)data.size()));
    std::string text = utf8_replace(reply);
    for (int i = 0; i < pr.n_expect; i++)
        if (contains(text, pr.expect[i]))
            sh->ingest(strf("[HTTP_PROBE] page contains: %s\n", pr.expect[i]));
    if (pr.tail && endswith(py_rstrip_ws(text), pr.tail))
        sh->ingest(strf("[HTTP_PROBE] %s %s ends with: %s\n", method.c_str(), path.c_str(), pr.tail));
}

static void probe_thread(void *arg)
{
    ProbeCtx *c = (ProbeCtx *)arg;
    for (int i = 0; i < c->tc->n_probes; i++)
        http_probe(c->sh, c->port, c->tc->probes[i]);
}

static CaseOut run_snapshot_roundtrip(const Runner &R, const TestCase &tc);

/* ------------------------------------------------------------ run_test */
static CaseOut run_test(const Runner &R, const TestCase &tc)
{
    if (std::string(tc.kind) == "snapshot_roundtrip")
        return run_snapshot_roundtrip(R, tc);
    CaseOut o;
    o.markers_total = tc.n_expected;
    std::string binary = path_join(R.root, tc.binary);
    bool want_port = false;
    for (int i = 0; i < tc.n_args; i++)
        if (contains(tc.args[i], "{free_port}"))
            want_port = true;
    int port = want_port ? claim_free_port() : 0;
    StrList args;
    for (int i = 0; i < tc.n_args; i++)
        args.push_back(replace_all(tc.args[i], "{free_port}", strf("%d", port)));
    int timeout = tc.timeout;

    say(o, SEP);
    say(o, std::string("Running Test: ") + tc.name);
    say(o, "Binary: " + binary);
    if (!file_exists(binary)) {
        say(o, "FAIL: Binary not found: " + binary);
        o.fail_lines.push_back("  FAIL: Binary not found: " + binary);
        o.early = true;
        o.res = make_result(R, tc, false, 0.0, -1, false, "Binary not found: " + binary, 0);
        return o;
    }

    std::string pcap_path, ip_pcap_path;
    bool any_pcap = false, any_ip = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (contains(args[i], "{pcap}"))
            any_pcap = true;
    }
    if (any_pcap) {
        pcap_path = temp_file("bekensim_", ".pcap");
        for (size_t i = 0; i < args.size(); i++)
            args[i] = replace_all(args[i], "{pcap}", pcap_path);
    }
    for (size_t i = 0; i < args.size(); i++)
        if (contains(args[i], "{pcap_ip}"))
            any_ip = true;
    if (any_ip) {
        ip_pcap_path = temp_file("bekensim_ip_", ".pcap");
        for (size_t i = 0; i < args.size(); i++)
            args[i] = replace_all(args[i], "{pcap_ip}", ip_pcap_path);
    }

    StrList argv = R.emu_argv_prefix;
    argv.push_back(binary);
    argv.insert(argv.end(), args.begin(), args.end());

    ChildProcess proc;
    if (!spawn(proc, argv)) {
        std::string e = "could not start " + argv[0];
        say(o, "FAIL: Failed to launch subprocess: " + e);
        o.fail_lines.push_back("  FAIL: Failed to launch subprocess: " + e);
        o.early = true;
        o.res = make_result(R, tc, false, 0.0, -1, false, "Failed to launch subprocess: " + e, 0);
        if (!pcap_path.empty())
            remove_file(pcap_path);
        if (!ip_pcap_path.empty())
            remove_file(ip_pcap_path);
        return o;
    }

    RunShared *sh = new RunShared();
    sh->reqs = parse_expected(tc);
    sh->counts.assign(sh->reqs.size(), 0);
    sh->done.assign(sh->reqs.size(), false);
    sh->remaining = (int)sh->reqs.size();
    /* Python keeps `remaining` as a set of strings: a string listed twice is
     * one entry, satisfied by its own count */
    {
        std::set<std::string> uniq;
        for (size_t k = 0; k < sh->reqs.size(); k++) {
            if (uniq.count(sh->reqs[k].first)) {
                sh->done[k] = true;
                sh->remaining--;
            }
            uniq.insert(sh->reqs[k].first);
        }
        /* and `required` keeps the LAST count given for a string */
        for (size_t k = 0; k < sh->reqs.size(); k++)
            for (size_t j = k + 1; j < sh->reqs.size(); j++)
                if (sh->reqs[j].first == sh->reqs[k].first)
                    sh->reqs[k].second = sh->reqs[j].second;
    }
    sh->has_periph_exp = tc.n_pins > 0 || tc.n_pwm > 0;
    std::string test_chip = chip_of(tc);

    bool probe_started = false;
    Thread *probe_th = 0;
    ProbeCtx *pctx = 0;
    LineReader rd;

    /* reader(): classify and record one line */
    struct Local {
        static void line(RunShared *sh, const std::string &ln, const TestCase &tc, bool &probe_started,
                         Thread *&probe_th, ProbeCtx *&pctx, int port)
        {
            if (startswith(ln, "[EMU_GPIO]") || startswith(ln, "[EMU_PWM]")) {
                ScopedLock lk(sh->lock);
                sh->periph_lines.push_back(strip(ln));
                return;
            }
            if (startswith(ln, "[EMU_PERIPH]")) {
                ScopedLock lk(sh->lock);
                sh->complete_periph_boundary();
                return;
            }
            if (startswith(ln, "[EMU_INSNS]")) {
                StrList p = split_ws(ln);
                long long n;
                if (p.size() >= 2 && is_digits(p[1]) && parse_int(p[1], 10, n))
                    sh->insns = n;
                ScopedLock lk(sh->lock);
                sh->complete_periph_boundary();
                return;
            }
            if (startswith(ln, "[EMU_UART1]")) {
                sh->uart1.push_back(strip(ln));
                return;
            }
            sh->ingest(ln);
            if (tc.n_probes > 0 && !probe_started && contains(ln, tc.probes[0].after)) {
                probe_started = true;
                pctx = new ProbeCtx();
                pctx->sh = sh;
                pctx->port = port;
                pctx->tc = &tc;
                probe_th = new Thread();
                probe_th->start(probe_thread, pctx);
            }
        }
    };

    double start = mono_time();
    bool hit_timeout = false, eof = false;
    double done_at = -1;
    for (;;) {
        std::string chunk;
        if (!eof) {
            bool alive = proc.read_some(chunk, 10);
            StrList lines;
            rd.feed(chunk, lines, !alive);
            for (size_t i = 0; i < lines.size(); i++)
                Local::line(sh, lines[i], tc, probe_started, probe_th, pctx, port);
            if (!alive)
                eof = true;
        } else {
            sleep_ms(10);
        }
        bool done;
        {
            ScopedLock lk(sh->lock);
            done = sh->remaining == 0 && (!sh->has_periph_exp || sh->have_assert);
        }
        bool exited = !proc.running();
        if (done) {
            if (exited)
                break;
            if (g_linger <= 0)
                break;
            if (done_at < 0) {
                done_at = mono_time();
                say(o, strf("  ... all completion conditions met, lingering %.0fs for a fuller log", g_linger));
            } else if (mono_time() - done_at >= g_linger) {
                break;
            }
        } else if (exited) {
            break; /* e.g. a CLI case; the drain below collects the rest */
        } else if (mono_time() - start > timeout) {
            hit_timeout = true;
            break;
        }
    }
    if (proc.running()) {
        proc.kill();
        proc.wait(5000);
    }
    /* th.join(): the reader keeps draining what the child already wrote */
    double drain_end = mono_time() + 3.0;
    while (!eof && mono_time() < drain_end) {
        std::string chunk;
        bool alive = proc.read_some(chunk, 5);
        StrList lines;
        rd.feed(chunk, lines, !alive);
        for (size_t i = 0; i < lines.size(); i++)
            Local::line(sh, lines[i], tc, probe_started, probe_th, pctx, port);
        if (!alive)
            eof = true;
    }

    std::string output;
    StrList periph_lines, assert_lines, uart1;
    bool have_assert;
    long long insns;
    {
        ScopedLock lk(sh->lock);
        output = sh->output;
        periph_lines = sh->periph_lines;
        assert_lines = sh->assert_lines;
        have_assert = sh->have_assert;
        uart1 = sh->uart1;
        insns = sh->insns;
    }
    double elapsed = mono_time() - start;
    if (hit_timeout)
        say(o, strf("Note: reached %ds timeout. Checking output up to this point.", timeout));

    bool all_passed = true;
    std::vector<report::Check> checks;
    std::vector<std::pair<std::string, int> > reqs = parse_expected(tc);
    for (size_t k = 0; k < reqs.size(); k++) {
        const std::string &s = reqs[k].first;
        int need = reqs[k].second;
        int actual = (int)count_sub(output, s);
        bool found = actual >= need;
        checks.push_back(report::Check(s, found, actual, need));
        if (found) {
            o.markers_hit++;
            if (need > 1)
                say(o, strf("  [PASS] Found '%s' x%d (need %d)", s.c_str(), actual, need));
            else
                say(o, "  [PASS] Found string: '" + s + "'");
        } else {
            std::string line = need > 1 ? strf("  [FAIL] '%s' found %dx, need %d", s.c_str(), actual, need)
                                        : "  [FAIL] Missing string: '" + s + "'";
            say(o, line);
            o.fail_lines.push_back(line);
            all_passed = false;
        }
    }
    for (int k = 0; k < tc.n_unexpected; k++) {
        int actual = (int)count_sub(output, tc.unexpected[k]);
        if (!record_check(o, checks, std::string("absent: '") + tc.unexpected[k] + "'", actual == 0,
                          actual == 0 ? std::string("never printed") : strf("printed %dx", actual)))
            all_passed = false;
    }
    if (!pcap_path.empty()) {
        if (!check_pcap(o, checks, pcap_path, tc.pcap_expect, tc.n_pcap, false))
            all_passed = false;
        remove_file(pcap_path);
    }
    if (!ip_pcap_path.empty()) {
        if (!check_pcap(o, checks, ip_pcap_path, tc.ip_pcap_expect, tc.n_ippcap, true))
            all_passed = false;
        remove_file(ip_pcap_path);
    }

    /* the first complete peripheral batch after the final text marker */
    bool snapshot_captured = !sh->has_periph_exp || have_assert;
    o.periph_used = have_assert ? assert_lines : periph_lines;
    periph::DecodedState pd;
    {
        std::map<int, u32> gpio;
        std::map<u32, u32> pwm;
        periph::parse_lines(o.periph_used, gpio, pwm);
        pd = periph::decode_state(gpio, pwm, test_chip, 0, 0, 0, 0, 0, 0, 0);
    }
    for (int i = 0; i < tc.n_pins; i++) {
        int pin = tc.pins[i].pin, want = tc.pins[i].want;
        std::map<int, u32>::const_iterator it = pd.raw.find(pin);
        bool got_any = pd.present && it != pd.raw.end();
        bool ok = snapshot_captured && pd.present && (want < 0 ? !got_any : (got_any && (long long)it->second == want));
        std::string label = want < 0 ? strf("GPIO P%d untouched", pin) : strf("GPIO P%d config = 0x%02X", pin, want);
        std::string detail;
        if (ok)
            detail = want < 0 ? std::string("no write captured") : periph::decode_pin((u32)want);
        else
            detail = got_any ? strf("0x%02X", (unsigned)it->second) : std::string("no write captured");
        if (!record_check(o, checks, label, ok, detail))
            all_passed = false;
    }
    for (int i = 0; i < tc.n_pwm; i++) {
        int ch = tc.pwm[i].ch, want_period = tc.pwm[i].period, want_duty = tc.pwm[i].duty;
        const periph::PwmChan *got = 0;
        for (size_t j = 0; j < pd.pwm_channels.size(); j++)
            if (pd.pwm_channels[j].ch == ch)
                got = &pd.pwm_channels[j]; /* dict: the last one wins */
        bool ok = snapshot_captured && got && (long long)got->period == want_period
                  && (want_duty < 0 || (got->has_duty && (long long)got->duty == want_duty));
        std::string label = want_duty < 0
            ? strf("PWM%d period = 0x%04X (duty varies, not asserted)", ch, want_period)
            : strf("PWM%d period = 0x%04X, duty = 0x%04X", ch, want_period, want_duty);
        std::string detail;
        if (ok)
            detail = got->has_pct ? strf("%.1f Hz, %.1f%%", got->freq, got->pct) : strf("%.1f Hz, None%%", got->freq);
        else if (got)
            detail = strf("period 0x%04X duty ", (unsigned)got->period)
                     + (got->has_duty ? strf("%u", (unsigned)got->duty) : std::string("None"));
        else
            detail = "channel not decoded";
        if (!record_check(o, checks, label, ok, detail))
            all_passed = false;
    }

    o.res = make_result(R, tc, all_passed, elapsed, insns, hit_timeout, output, &checks);
    o.res.has_periph = pd.present;
    o.res.periph = pd;
    o.res.uart1 = uart1;
    add_derived_tags(o.res);

    if (!all_passed) {
        say(o, "--- CAPTURED OUTPUT ---");
        say(o, output);
        say(o, "-----------------------");
        say(o, std::string("Test '") + tc.name + "' FAILED.");
    } else {
        say(o, strf("Test '%s' PASSED. (%.0fs)", tc.name, elapsed));
    }

    /* the probe thread is a daemon in Python; here it must not outlive what it
     * points at, so wait for it (the device is gone, so its socket fails fast) */
    bool joined = true;
    if (probe_th) {
        joined = probe_th->join(15000);
        if (joined) {
            delete probe_th;
            delete pctx;
        }
    }
    if (joined)
        delete sh; /* else leak: the stuck probe still references it */
    return o;
}

/* ------------------------------------------------ snapshot round trip */
/* _stream_until: every line (the [EMU_*] ones included) until `after` has
 * appeared and every `until` marker has its count on the lines after it. */
static void stream_until(const StrList &argv, const std::string &after,
                         const std::vector<std::pair<std::string, int> > &until, double timeout,
                         StrList &lines_out, bool &timed_out)
{
    timed_out = false;
    ChildProcess proc;
    if (!spawn(proc, argv))
        return;
    double start = mono_time();
    bool seen_after = false, eof = false;
    std::vector<int> counts(until.size(), 0);
    LineReader rd;
    for (;;) {
        if (!eof) {
            std::string chunk;
            bool alive = proc.read_some(chunk, 50);
            StrList lines;
            rd.feed(chunk, lines, !alive);
            for (size_t i = 0; i < lines.size(); i++) {
                lines_out.push_back(lines[i]);
                if (seen_after) {
                    for (size_t k = 0; k < until.size(); k++)
                        counts[k] += (int)count_sub(lines[i], until[k].first);
                } else if (contains(lines[i], after)) {
                    seen_after = true;
                }
            }
            if (!alive)
                eof = true;
        } else {
            sleep_ms(50);
        }
        bool done = seen_after;
        for (size_t k = 0; k < until.size(); k++)
            if (counts[k] < until[k].second)
                done = false;
        if (done || !proc.running())
            break;
        if (mono_time() - start > timeout) {
            timed_out = true;
            break;
        }
    }
    if (proc.running()) {
        proc.kill();
        proc.wait(5000);
    }
    double drain_end = mono_time() + 2.0;
    while (!eof && mono_time() < drain_end) {
        std::string chunk;
        bool alive = proc.read_some(chunk, 50);
        rd.feed(chunk, lines_out, !alive);
        if (!alive)
            eof = true;
    }
}

static StrList after_marker(const StrList &lines, const std::string &marker)
{
    for (size_t i = 0; i < lines.size(); i++)
        if (contains(lines[i], marker))
            return StrList(lines.begin() + (long)i + 1, lines.end());
    return StrList();
}

static std::vector<long long> insn_marks(const StrList &lines)
{
    std::vector<long long> out;
    for (size_t i = 0; i < lines.size(); i++) {
        StrList parts = split_ws(lines[i]);
        long long n;
        if (startswith(lines[i], "[EMU_INSNS]") && parts.size() > 1 && is_digits(parts[1])
            && parse_int(parts[1], 10, n))
            out.push_back(n);
    }
    return out;
}

/* Python repr() of a str */
static std::string py_repr(const std::string &s)
{
    bool has_sq = s.find('\'') != std::string::npos, has_dq = s.find('"') != std::string::npos;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    std::string o(1, q);
    for (size_t i = 0; i < s.size(); i++) {
        u8 c = (u8)s[i];
        if (c == '\\')
            o += "\\\\";
        else if (c == (u8)q)
            o += std::string("\\") + (char)q;
        else if (c == '\n')
            o += "\\n";
        else if (c == '\r')
            o += "\\r";
        else if (c == '\t')
            o += "\\t";
        else if (c < 0x20 || c == 0x7F)
            o += strf("\\x%02x", c);
        else if (c == 0xC2 && i + 1 < s.size() && ((u8)s[i + 1] < 0xA1 || (u8)s[i + 1] == 0xAD)) {
            /* C1 controls, NBSP and soft hyphen are not printable */
            o += strf("\\x%02x", (u8)s[i + 1]);
            i++;
        } else
            o += (char)c;
    }
    o += q;
    return o;
}

static CaseOut run_snapshot_roundtrip(const Runner &R, const TestCase &tc)
{
    CaseOut o;
    o.markers_total = tc.n_expected;
    std::string binary = path_join(R.root, tc.binary);
    int timeout = tc.timeout;
    std::vector<std::pair<std::string, int> > until = parse_expected(tc);
    say(o, SEP);
    say(o, std::string("Running Test: ") + tc.name);
    say(o, "Binary: " + binary);
    if (!file_exists(binary)) {
        say(o, "FAIL: Binary not found: " + binary);
        o.fail_lines.push_back("  FAIL: Binary not found: " + binary);
        o.early = true;
        o.res = make_result(R, tc, false, 0.0, -1, false, "Binary not found: " + binary, 0);
        return o;
    }
    std::string snap = temp_file("bekensim_selftest_", ".snap");
    remove_file(snap);
    StrList base = R.emu_argv_prefix;
    base.push_back(binary);
    for (int i = 0; i < tc.n_args; i++)
        base.push_back(tc.args[i]);
    double start = mono_time();
    StrList a_argv = base;
    a_argv.push_back("--snapshot-save");
    a_argv.push_back(snap);
    a_argv.push_back("--snapshot-at");
    a_argv.push_back(strf("%d", tc.snapshot_at));
    StrList a_lines, b_lines;
    bool a_to = false, b_to = false;
    stream_until(a_argv, "[SNAPSHOT] saved", until, timeout, a_lines, a_to);
    if (file_exists(snap)) {
        StrList b_argv = base;
        b_argv.push_back("--snapshot-load");
        b_argv.push_back(snap);
        stream_until(b_argv, "[SNAPSHOT] restored", until, timeout, b_lines, b_to);
    }
    double elapsed = mono_time() - start;
    remove_file(snap);

    StrList a_all = after_marker(a_lines, "[SNAPSHOT] saved"), b_all = after_marker(b_lines, "[SNAPSHOT] restored");
    StrList a_after, b_after;
    for (size_t i = 0; i < a_all.size(); i++)
        if (!startswith(a_all[i], "[EMU_"))
            a_after.push_back(a_all[i]);
    for (size_t i = 0; i < b_all.size(); i++)
        if (!startswith(b_all[i], "[EMU_"))
            b_after.push_back(b_all[i]);
    std::vector<long long> a_ins = insn_marks(a_all), b_ins = insn_marks(b_all);
    long n_ins = (long)std::min(a_ins.size(), b_ins.size()) - 1;
    if (n_ins < 0)
        n_ins = 0;
    long long drift = -1;
    for (long i = 0; i < n_ins; i++) {
        long long d = a_ins[i] - b_ins[i];
        if (d < 0)
            d = -d;
        if (d > drift)
            drift = d;
    }
    /* The firmware output must be byte-identical (first_diff); the counter is a
     * secondary bound on the device-clock ESTIMATE. The C++ chunk model carries
     * ~2x the per-resume jitter of Python's per-block estimate, so against the
     * C++ emulator the bound is at least 2500 (Python's default is 1000). */
    int tolerance = tc.insn_tolerance;
    if (!R.use_python && tolerance < 2500)
        tolerance = 2500;
    long k = (long)std::min(a_after.size(), b_after.size()) - 1;
    if (k < 0)
        k = 0;
    long first_diff = -1;
    for (long i = 0; i < k; i++)
        if (a_after[i] != b_after[i]) {
            first_diff = i;
            break;
        }
    int min_lines = tc.min_compared_lines;

    std::vector<report::Check> checks;
    bool ok = true;
    bool saved = false, restored = false;
    for (size_t i = 0; i < a_lines.size(); i++)
        if (contains(a_lines[i], "[SNAPSHOT] saved"))
            saved = true;
    for (size_t i = 0; i < b_lines.size(); i++)
        if (contains(b_lines[i], "[SNAPSHOT] restored"))
            restored = true;
    ok &= record_check(o, checks, "run A saved a snapshot", saved, saved ? "saved" : "no [SNAPSHOT] saved line");
    ok &= record_check(o, checks, "run B restored it", restored,
                       restored ? "restored" : "no [SNAPSHOT] restored line");
    std::string joined_b = join(b_after, "");
    for (size_t u = 0; u < until.size(); u++) {
        int c = (int)count_sub(joined_b, until[u].first);
        int n = until[u].second;
        if (c >= n)
            o.markers_hit++;
        ok &= record_check(o, checks, "resumed run reaches '" + until[u].first + "'" + (n > 1 ? strf(" x%d", n) : ""),
                           c >= n, strf("%dx", c));
    }
    ok &= record_check(o, checks, strf("at least %d lines compared after the snapshot point", min_lines),
                       k >= min_lines, strf("%ld lines", k));
    ok &= record_check(o, checks, "resumed firmware output identical to the original run", first_diff < 0,
                       first_diff < 0 ? strf("%ld lines match", k)
                                      : strf("line %ld differs: ", first_diff + 1) + py_repr(rstrip(a_after[first_diff]))
                                            + " vs " + py_repr(rstrip(b_after[first_diff])));
    ok &= record_check(o, checks, strf("instruction counter within %d of the original run", tolerance),
                       drift >= 0 && drift <= tolerance,
                       drift >= 0 ? strf("%ld marks compared, max drift %lld", n_ins, drift)
                                  : std::string("no [EMU_INSNS] marks to compare"));
    std::string output = strf("=== run A (saves at %d instructions) ===\n", tc.snapshot_at) + join(a_lines, "")
                         + "\n=== run B (resumes from the snapshot) ===\n" + join(b_lines, "");
    o.res = make_result(R, tc, ok, elapsed, -1, a_to || b_to, output, &checks);
    add_derived_tags(o.res);
    if (!ok) {
        say(o, "--- CAPTURED OUTPUT ---");
        say(o, output);
        say(o, "-----------------------");
        say(o, std::string("Test '") + tc.name + "' FAILED.");
    } else {
        say(o, strf("Test '%s' PASSED. (%.0fs)", tc.name, elapsed));
    }
    return o;
}

/* ------------------------------------------------------------ output */
static void print_case(const Runner &R, const CaseOut &o)
{
    if (R.verbose)
        fwrite(o.log.data(), 1, o.log.size(), stdout);
    const report::Result &r = o.res;
    if (r.passed) {
        printf("PASS  %-70s %5.1fs  (%d/%d markers)\n", r.name.c_str(), r.elapsed, o.markers_hit, o.markers_total);
    } else {
        printf("FAIL  %-70s %5.1fs  (%d/%d markers)%s\n", r.name.c_str(), r.elapsed, o.markers_hit,
               o.markers_total, r.timed_out ? " [timeout]" : "");
        if (!R.verbose)
            for (size_t i = 0; i < o.fail_lines.size(); i++)
                printf("%s\n", o.fail_lines[i].c_str());
    }
    fflush(stdout);
}

/* -------------------------------------------------------- parallel pool */
struct Pool {
    const Runner *R;
    std::vector<const TestCase *> cases; /* `selected`, original order */
    std::vector<int> order;              /* submission order (indices into cases) */
    size_t next;
    Mutex lock;
    std::vector<CaseOut> out;
    std::vector<Event *> done;
};

static void worker(void *arg)
{
    Pool *p = (Pool *)arg;
    for (;;) {
        int idx;
        {
            ScopedLock lk(p->lock);
            if (p->next >= p->order.size())
                return;
            idx = p->order[p->next++];
        }
        CaseOut r = run_test(*p->R, *p->cases[idx]);
        {
            ScopedLock lk(p->lock);
            p->out[idx] = r;
        }
        p->done[idx]->set();
    }
}

struct SubmitOrder {
    const std::vector<const TestCase *> *cases;
    /* Python: key=(-int(selected[i].get("kind") == "snapshot_roundtrip"), -timeout, index). */
    bool operator()(int a, int b) const
    {
        const bool snap_a = std::string((*cases)[a]->kind) == "snapshot_roundtrip";
        const bool snap_b = std::string((*cases)[b]->kind) == "snapshot_roundtrip";
        if (snap_a != snap_b)
            return snap_a;
        int ta = (*cases)[a]->timeout, tb = (*cases)[b]->timeout;
        if (ta != tb)
            return ta > tb;
        return a < b;
    }
};

/* ------------------------------------------------------------ JSON dump */
static std::string jstr(const std::string &s)
{
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        u8 c = (u8)s[i];
        if (c == '"')
            o += "\\\"";
        else if (c == '\\')
            o += "\\\\";
        else if (c == '\n')
            o += "\\n";
        else if (c < 0x20)
            o += strf("\\u%04x", c);
        else
            o += (char)c;
    }
    return o + "\"";
}
static std::string jlist(const StrList &l)
{
    std::string o = "[";
    for (size_t i = 0; i < l.size(); i++)
        o += (i ? "," : "") + jstr(l[i]);
    return o + "]";
}

/* The raw results (for cross-checking report.cpp against tests/report.py) */
static bool write_results_json(const std::string &path, const std::vector<int> &case_index,
                               const std::vector<CaseOut> &outs, const report::Meta &meta)
{
    std::string j = "{\"meta\":{";
    j += "\"total_time\":" + strf("%.17g", meta.total_time);
    j += ",\"chips\":" + jlist(meta.chips);
    j += ",\"generated_at\":" + jstr(meta.generated_at);
    j += ",\"commit\":" + (meta.commit.empty() ? std::string("null") : jstr(meta.commit));
    j += ",\"repo\":" + (meta.repo.empty() ? std::string("null") : jstr(meta.repo));
    j += ",\"run_url\":" + (meta.run_url.empty() ? std::string("null") : jstr(meta.run_url));
    j += "},\"results\":[";
    for (size_t i = 0; i < outs.size(); i++) {
        const CaseOut &o = outs[i];
        const report::Result &r = o.res;
        if (i)
            j += ",\n";
        j += "{\"index\":" + strf("%d", case_index[i]);
        j += ",\"name\":" + jstr(r.name);
        j += ",\"early\":" + std::string(o.early ? "true" : "false");
        j += ",\"passed\":" + std::string(r.passed ? "true" : "false");
        j += ",\"timed_out\":" + std::string(r.timed_out ? "true" : "false");
        j += ",\"elapsed\":" + strf("%.17g", r.elapsed);
        j += ",\"insns\":" + (r.insns >= 0 ? strf("%lld", r.insns) : std::string("null"));
        j += ",\"checks\":[";
        for (size_t c = 0; c < r.checks.size(); c++)
            j += std::string(c ? "," : "") + "{\"string\":" + jstr(r.checks[c].string) + ",\"found\":"
                 + (r.checks[c].found ? "true" : "false") + strf(",\"count\":%d,\"required\":%d}",
                                                                 r.checks[c].count, r.checks[c].required);
        j += "],\"output\":" + jstr(r.output);
        j += ",\"periph_lines\":" + jlist(o.periph_used);
        j += ",\"uart1\":" + jlist(r.uart1);
        j += "}";
    }
    j += "]}\n";
    return write_file(path, j);
}

/* ----------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    net_init();
    StrList patterns;
    bool use_python = false, verbose = false;
#ifdef _WIN32
    std::string emu_path = "bekensim.exe";
#else
    std::string emu_path = "bekensim";
#endif
    std::string report_path, results_json;
    int cap_timeout = 0; /* 0 = use each case's own timeout */
    std::vector<int> jobs_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-k" && i + 1 < argc)
            patterns.push_back(lower(argv[++i]));
        else if ((a == "-j" || a == "--jobs") && i + 1 < argc)
            jobs_arg.push_back(atoi(argv[++i]));
        else if (a == "--python")
            use_python = true;
        else if (a == "--emu" && i + 1 < argc)
            emu_path = argv[++i];
        else if (a == "--table")
            ; /* accepted for compatibility */
        else if (a == "--cap-timeout" && i + 1 < argc)
            cap_timeout = atoi(argv[++i]);
        else if (a == "--report" && i + 1 < argc)
            report_path = argv[++i];
        else if (a == "--results-json" && i + 1 < argc)
            results_json = argv[++i];
        else if (a == "-v" || a == "--verbose")
            verbose = true;
    }
    int ncpu = cpu_count();
    int default_jobs = env_has("CI") ? std::min(4, ncpu) : std::min(6, ncpu);
    int jobs = !jobs_arg.empty() ? jobs_arg.back()
                                 : (env_has("JOBS") ? atoi(env_get("JOBS").c_str()) : default_jobs);
    if (jobs < 1)
        jobs = 1;
    if (env_has("REPEATS"))
        g_repeats = atoi(env_get("REPEATS").c_str());
    {
        double lg = 0;
        if (env_has("LINGER") && parse_double(env_get("LINGER"), lg))
            g_linger = lg;
        else
            g_linger = env_has("CI") ? 5.0 : 0.0;
    }

    Runner R;
    R.root = path_abs(".");
    R.use_python = use_python;
    R.verbose = verbose;
    if (use_python) {
        R.emu_argv_prefix.push_back("python");
        R.emu_argv_prefix.push_back(path_abs("src/main.py"));
    } else {
        R.emu_argv_prefix.push_back(path_abs(emu_path));
    }

    /* selected cases (with --cap-timeout applied to the copies we run) */
    std::vector<TestCase> capped;
    std::vector<int> case_index;
    for (int i = 0; i < g_case_count; i++) {
        const TestCase &tc = g_cases[i];
        std::string lname = lower(tc.name);
        bool sel = patterns.empty();
        for (size_t p = 0; p < patterns.size(); p++)
            if (contains(lname, patterns[p]))
                sel = true;
        if (!sel)
            continue;
        TestCase c = tc;
        if (cap_timeout > 0 && c.timeout > cap_timeout)
            c.timeout = cap_timeout;
        capped.push_back(c);
        case_index.push_back(i);
    }
    if (!patterns.empty() && capped.empty()) {
        printf("No test name contains any of: %s\n", join(patterns, ", ").c_str());
        return 2;
    }

    double suite_start = mono_time();
    std::vector<CaseOut> results(capped.size());
    if (jobs > 1 && capped.size() > 1) {
        Pool pool;
        pool.R = &R;
        for (size_t i = 0; i < capped.size(); i++)
            pool.cases.push_back(&capped[i]);
        /* the submission order sorts on the case's own timeout, not the cap */
        std::vector<const TestCase *> orig;
        for (size_t i = 0; i < capped.size(); i++)
            orig.push_back(&g_cases[case_index[i]]);
        for (size_t i = 0; i < capped.size(); i++)
            pool.order.push_back((int)i);
        SubmitOrder so;
        so.cases = &orig;
        std::stable_sort(pool.order.begin(), pool.order.end(), so);
        pool.next = 0;
        pool.out.resize(capped.size());
        for (size_t i = 0; i < capped.size(); i++)
            pool.done.push_back(new Event(false));
        int nthreads = std::min(jobs, (int)capped.size());
        std::vector<Thread *> threads;
        for (int t = 0; t < nthreads; t++) {
            Thread *th = new Thread();
            th->start(worker, &pool);
            threads.push_back(th);
        }
        /* print in the original order as soon as each case and all before it are in */
        for (size_t i = 0; i < capped.size(); i++) {
            pool.done[i]->wait(-1);
            {
                ScopedLock lk(pool.lock);
                results[i] = pool.out[i];
            }
            print_case(R, results[i]);
        }
        for (size_t t = 0; t < threads.size(); t++) {
            threads[t]->join(-1);
            delete threads[t];
        }
        for (size_t i = 0; i < pool.done.size(); i++)
            delete pool.done[i];
    } else {
        for (size_t i = 0; i < capped.size(); i++) {
            results[i] = run_test(R, capped[i]);
            print_case(R, results[i]);
        }
    }
    double wall_time = mono_time() - suite_start;
    int passed = 0;
    for (size_t i = 0; i < results.size(); i++)
        if (results[i].res.passed)
            passed++;
    int failed = (int)results.size() - passed;

    printf("%s\n", SEP);
    printf("Test Run Completed: %d passed, %d failed. (%.1fs)\n", passed, failed, wall_time);
    fflush(stdout);

    /* _write_report: report/index.html, only for an unfiltered run (or --report) */
    std::vector<report::Result> rr;
    for (size_t i = 0; i < results.size(); i++)
        rr.push_back(results[i].res);
    report::Meta meta;
    meta.total_time = wall_time;
    {
        std::set<std::string> cs;
        for (size_t i = 0; i < rr.size(); i++)
            if (!rr[i].chip.empty())
                cs.insert(rr[i].chip);
        meta.chips.assign(cs.begin(), cs.end());
    }
    meta.generated_at = utc_timestamp();
    meta.commit = env_get("GITHUB_SHA");
    meta.repo = env_has("GITHUB_REPOSITORY") ? env_get("GITHUB_REPOSITORY")
                                             : std::string("openshwprojects/BekenEmulator");
    if (env_has("GITHUB_RUN_ID"))
        meta.run_url = (env_has("GITHUB_SERVER_URL") ? env_get("GITHUB_SERVER_URL") : std::string("https://github.com"))
                       + "/" + env_get("GITHUB_REPOSITORY") + "/actions/runs/" + env_get("GITHUB_RUN_ID");
    StrList targets;
    if (patterns.empty() && report_path.empty())
        targets.push_back(path_join(path_join(R.root, "report"), "index.html"));
    if (!report_path.empty())
        targets.push_back(report_path);
    for (size_t t = 0; t < targets.size(); t++) {
        std::string err;
        std::string dir = path_dirname(targets[t]);
        if (!dir.empty() && !is_dir(dir))
            make_dirs(dir);
        if (report::generate(rr, meta, targets[t], &err))
            printf("HTML report written: %s\n", targets[t].c_str());
        else
            printf("WARN: could not write HTML report: %s\n", err.c_str());
    }
    if (!results_json.empty() && !write_results_json(results_json, case_index, results, meta))
        printf("WARN: could not write %s\n", results_json.c_str());
    fflush(stdout);
    return failed ? 1 : 0;
}
