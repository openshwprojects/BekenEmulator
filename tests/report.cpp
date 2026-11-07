/* C++ port of tests/report.py. See report.h. The long literals (page, CSS, JS,
 * card template, notes) come from report_templates.h, generated from
 * report.py itself by tools/gen_cases.py, so they stay byte-identical. */
#include "report.h"
#include "report_templates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace report {

std::string html_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#x27;"; break;
        default: o += c;
        }
    }
    return o;
}

/* Python "tpl % (args)" where every argument is already a formatted string:
 * each %-conversion takes the next one, "%%" is a literal percent. */
static std::string pct(const char *tpl, const StrList &args)
{
    std::string o;
    size_t ai = 0;
    for (const char *p = tpl; *p; p++) {
        if (*p != '%') {
            o += *p;
            continue;
        }
        p++;
        if (*p == '%') {
            o += '%';
            continue;
        }
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
            p++;
        if (!*p)
            break;
        if (ai < args.size())
            o += args[ai++];
    }
    return o;
}

static StrList A(const std::string &a) { StrList v; v.push_back(a); return v; }
static StrList A(const std::string &a, const std::string &b) { StrList v = A(a); v.push_back(b); return v; }
static StrList A(const std::string &a, const std::string &b, const std::string &c)
{ StrList v = A(a, b); v.push_back(c); return v; }
static StrList A(const std::string &a, const std::string &b, const std::string &c, const std::string &d)
{ StrList v = A(a, b, c); v.push_back(d); return v; }
static StrList A(const std::string &a, const std::string &b, const std::string &c, const std::string &d,
                 const std::string &e)
{ StrList v = A(a, b, c, d); v.push_back(e); return v; }

/* str.format over a {literal, field, ..., literal} segment table */
static std::string fill(const char *const *segs, int n, const std::map<std::string, std::string> &vals)
{
    std::string o;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) {
            o += segs[i];
        } else {
            std::map<std::string, std::string>::const_iterator it = vals.find(segs[i]);
            if (it != vals.end())
                o += it->second;
        }
    }
    return o;
}

/* "{:,}".format(n) */
static std::string fmt_int(long long n)
{
    bool neg = n < 0;
    unsigned long long u = neg ? (unsigned long long)(-(n + 1)) + 1 : (unsigned long long)n;
    std::string d = strf("%llu", u);
    std::string o;
    int c = 0;
    for (size_t i = d.size(); i-- > 0;) {
        o += d[i];
        if (++c % 3 == 0 && i > 0)
            o += ',';
    }
    std::reverse(o.begin(), o.end());
    return neg ? "-" + o : o;
}

static std::string fmt_secs(double s) { return s >= 1 ? strf("%.0fs", s) : strf("%.1fs", s); }

/* Python divmod for floats (both operands positive here, but keep the sign rules) */
static double py_mod(double a, double b)
{
    double m = fmod(a, b);
    if (m != 0 && ((m < 0) != (b < 0)))
        m += b;
    return m;
}
static double py_floordiv(double a, double b)
{
    double m = fmod(a, b);
    double div = (a - m) / b;
    if (m != 0 && ((b < 0) != (m < 0)))
        div -= 1.0;
    double fl;
    if (div != 0) {
        fl = floor(div);
        if (div - fl > 0.5)
            fl += 1.0;
    } else {
        fl = div;
    }
    return fl;
}

static std::string fmt_compact(double secs)
{
    if (secs >= 60)
        return strf("%dm %02ds", (int)py_floordiv(secs, 60), (int)py_round(py_mod(secs, 60)));
    return strf("%.1fs", secs);
}

static std::string fmt_total(double secs)
{
    if (secs >= 60) {
        int m = (int)py_floordiv(secs, 60);
        return strf("%d minute%s %.2f seconds", m, m == 1 ? "" : "s", secs - m * 60);
    }
    return strf("%.2f seconds", secs);
}

static size_t cp_len(const std::string &s) { return utf8_length(s); }

struct LenGreater {
    bool operator()(const std::string &a, const std::string &b) const { return cp_len(a) > cp_len(b); }
};

static std::string highlight(const std::string &log_text, const StrList &found_strings)
{
    std::string escaped = html_escape(log_text);
    /* {s for s in found if s}, longest first (stable on first appearance) */
    StrList uniq;
    std::set<std::string> seen;
    for (size_t i = 0; i < found_strings.size(); i++) {
        const std::string &s = found_strings[i];
        if (s.empty() || seen.count(s))
            continue;
        seen.insert(s);
        uniq.push_back(s);
    }
    std::stable_sort(uniq.begin(), uniq.end(), LenGreater());
    for (size_t i = 0; i < uniq.size(); i++) {
        std::string needle = html_escape(uniq[i]);
        escaped = replace_all(escaped, needle, "<mark class=\"hit\">" + needle + "</mark>");
    }
    return escaped;
}

static std::string tag_class(const Tag &t)
{
    const std::string &name = t.name, &group = t.group;
    if (group == "source") {
        if (name == "OpenBeken")
            return "tag src-obk";
        if (name == "Tuya")
            return "tag src-tuya";
        return "tag src-cli";
    }
    if (group == "chip" || group == "state" || group == "data")
        return "tag " + group;
    return "tag feat";
}

static std::string tags_html(const Result &r)
{
    std::string o;
    for (size_t i = 0; i < r.tags.size(); i++)
        o += "<span class=\"" + tag_class(r.tags[i]) + "\">" + html_escape(r.tags[i].name) + "</span>";
    return o;
}

static std::string tuya_html(const Result &r)
{
    const tuya_config::Decoded &cfg = r.tuya_config;
    if (!cfg.valid)
        return TUYA_EMPTY;
    std::string rows, human;
    for (size_t i = 0; i < cfg.pairs.size(); i++)
        rows += "<tr><td><code>" + html_escape(cfg.pairs[i].first) + "</code></td><td><code>"
                + html_escape(cfg.pairs[i].second) + "</code></td></tr>";
    for (size_t i = 0; i < cfg.human.size(); i++)
        human += "<tr><td>" + html_escape(cfg.human[i].first) + "</td><td><b>"
                 + html_escape(cfg.human[i].second) + "</b></td></tr>";
    return pct(TUYA_BLOCK, A(strf("%06X", (unsigned)cfg.offset), strf("%d", (int)cfg.pairs.size()),
                             html_escape(cfg.raw), rows, human));
}

static std::string gpio_html(const Result &r)
{
    if (!r.has_periph)
        return GPIO_EMPTY;
    const periph::DecodedState &per = r.periph;
    std::string rows;
    int touched = 0;
    for (size_t i = 0; i < per.gpio.size(); i++) {
        const periph::GpioRow &g = per.gpio[i];
        std::string cls = g.state == "set by firmware" ? "set" : "factory";
        rows += strf("<tr class=\"%s\"><td><code>P%d</code></td><td><code>", cls.c_str(), g.pin)
                + html_escape(g.alias) + "</code></td><td>" + html_escape(g.detail) + "</td></tr>";
        if (g.state != "factory")
            touched++;
    }
    std::string extra;
    if (!per.func.empty()) {
        std::string fr;
        for (size_t i = 0; i < per.func.size(); i++)
            fr += "<tr><td><code>" + html_escape(per.func[i].first) + "</code></td><td><code>"
                  + html_escape(per.func[i].second) + "</code></td></tr>";
        extra = "<div class=\"cfg-h\">Function select</div><table class=\"cfg-t\">" + fr + "</table>";
    }
    std::string pwm;
    if (!per.pwm_channels.empty() && !per.pwm_pins.empty()) {
        std::set<int> pads(per.pwm_pins.begin(), per.pwm_pins.end());
        std::vector<int> ctl_on = periph::ctl_channels(per.pwm_ctl);
        std::set<int> decoded;
        for (size_t i = 0; i < per.pwm_channels.size(); i++)
            decoded.insert(per.pwm_channels[i].ch);
        std::string ctl_note;
        if (!ctl_on.empty()) {
            StrList missing, on;
            for (size_t i = 0; i < ctl_on.size(); i++) {
                on.push_back(strf("PWM%d", ctl_on[i]));
                if (!decoded.count(ctl_on[i]))
                    missing.push_back(strf("PWM%d", ctl_on[i]));
            }
            std::string agree = !missing.empty()
                ? "but " + join(missing, ", ") + " could NOT be decoded below"
                : std::string("consistent with the channels decoded below");
            ctl_note = strf("<p class=\"cfg-note\">PWM_CTL = 0x%X enables ", (unsigned)per.pwm_ctl)
                       + join(on, ", ") + ", " + agree + ".</p>";
        }
        static const int canon[6] = {6, 7, 8, 9, 24, 26};
        std::string chans;
        for (size_t i = 0; i < per.pwm_channels.size(); i++) {
            const periph::PwmChan &c = per.pwm_channels[i];
            bool has_canon = c.ch >= 0 && c.ch < 6;
            std::string pad = (has_canon && pads.count(canon[c.ch]))
                ? strf("<code>P%d</code>", canon[c.ch])
                : std::string("<span class=\"cfg-unk\">not identified</span>");
            std::string freq = c.freq != 0 ? strf("%.1f Hz", c.freq) : std::string("-");
            std::string pc = c.has_pct ? strf("%.1f%%", c.pct) : std::string("-");
            chans += strf("<tr><td><code>PWM%d</code></td><td>", c.ch) + pad + "</td><td>" + freq
                     + "</td><td>" + pc + strf("</td><td><code>0x%X</code></td></tr>", (unsigned)c.period);
        }
        StrList padnames;
        for (std::set<int>::const_iterator it = pads.begin(); it != pads.end(); ++it)
            padnames.push_back(strf("P%d", *it));
        std::string padlist = join(padnames, ", ");
        if (padlist.empty())
            padlist = "none";
        pwm += std::string(PWM_CH_HEAD) + ctl_note + PWM_CH_TABLE + chans + PWM_CH_PADS + padlist + PWM_CH_TAIL;
    }
    if (!per.pwm_regs.empty()) {
        bool base = per.pwm_base != 0xFFFFFFFFu && per.pwm_base != 0;
        const char *note;
        if (per.pwm_layout == "new")
            note = PWM_NOTE_NEW;
        else if (!per.pwm_pins.empty() && per.pwm_channels.empty())
            note = PWM_NOTE_BELOW;
        else if (per.pwm_pins.empty())
            note = PWM_NOTE_TICK;
        else if (base)
            note = PWM_NOTE_SHIFTED;
        else
            note = PWM_NOTE_BASE;
        std::string rr;
        for (size_t i = 0; i < per.pwm_regs.size(); i++)
            rr += "<tr><td><code>" + html_escape(per.pwm_regs[i].first) + "</code></td><td><code>"
                  + html_escape(per.pwm_regs[i].second) + "</code></td></tr>";
        pwm += std::string(PWM_REGS_HEAD) + rr + PWM_REGS_MID + html_escape(note) + "</p>";
    }
    return pct(GPIO_FINAL, A(strf("%d", touched), strf("%d", (int)per.gpio.size()), rows, extra, pwm));
}

struct U1Ev { long long insn; char d; long long b; };
struct U1Frame { char d; long long start; std::vector<long long> bs; long long last; };

static std::string uart1_html(const Result &r)
{
    typedef U1Ev Ev;
    typedef U1Frame Frame;
    std::vector<Ev> events;
    for (size_t i = 0; i < r.uart1.size(); i++) {
        StrList parts = split_ws(r.uart1[i]);
        if (parts.size() != 4 || (parts[2] != "T" && parts[2] != "R"))
            continue;
        long long insn, b;
        if (!parse_int(parts[1], 10, insn) || !parse_int(parts[3], 16, b))
            continue;
        Ev e;
        e.insn = insn;
        e.d = parts[2][0];
        e.b = b;
        events.push_back(e);
    }
    if (events.empty())
        return UART1_EMPTY;
    const long long GAP_INSNS = 50000;
    const double INSNS_PER_MS = 5000.0;
    std::vector<Frame> frames;
    for (size_t i = 0; i < events.size(); i++) {
        const Ev &e = events[i];
        if (!frames.empty() && frames.back().d == e.d && e.insn - frames.back().last <= GAP_INSNS) {
            frames.back().bs.push_back(e.b);
            frames.back().last = e.insn;
        } else {
            Frame f;
            f.d = e.d;
            f.start = e.insn;
            f.bs.push_back(e.b);
            f.last = e.insn;
            frames.push_back(f);
        }
    }
    StrList rows;
    for (size_t i = 0; i < frames.size(); i++) {
        const Frame &f = frames[i];
        StrList hx;
        std::string asc;
        for (size_t k = 0; k < f.bs.size(); k++) {
            hx.push_back(strf("%02llX", f.bs[k]));
            long long b = f.bs[k];
            if (b >= 32 && b < 127)
                asc += (char)b;
            else
                asc += '.';
        }
        bool tx = f.d == 'T';
        rows.push_back(strf("<span class=\"%s\">%10.2f ms  %s:</span> ", tx ? "u1tx" : "u1rx",
                            f.start / INSNS_PER_MS, tx ? "Sent" : "Recv")
                       + html_escape(join(hx, " ")) + "   <span class=\"u1asc\">" + html_escape(asc) + "</span>");
    }
    return std::string(UART1_NOTE) + "<pre class=\"log uart1-log\">" + join(rows, "\n") + "</pre>";
}

static std::string test_card(const Result &r)
{
    bool ok = r.passed;
    std::string checks_html;
    StrList found;
    for (size_t i = 0; i < r.checks.size(); i++) {
        const Check &c = r.checks[i];
        std::string tally;
        if (c.required > 1)
            tally = strf("<span class=\"count\">%d/%d</span>", c.count, c.required);
        checks_html += std::string("<li class=\"") + (c.found ? "found" : "missing") + "\"><span class=\"chk\">"
                       + (c.found ? "\xe2\x9c\x93" : "\xe2\x9c\x97") + "</span><code>" + html_escape(c.string)
                       + "</code>" + tally + "</li>";
        if (c.found)
            found.push_back(c.string);
    }
    std::string insns = r.insns >= 0 ? "\xe2\x89\x88 " + fmt_int(r.insns) + " instrs" : std::string();
    std::string timed_out = r.timed_out ? " <span class=\"warn\">(timed out)</span>" : "";
    std::string dump;
    if (!r.dump_url.empty())
        dump = "<a class=\"dump\" href=\"" + html_escape(r.dump_url) + "\" download>\xe2\xa4\x93 "
               + html_escape(r.binary_name) + "</a>";
    StrList tagdata;
    for (size_t i = 0; i < r.tags.size(); i++)
        tagdata.push_back(r.tags[i].group + ":" + r.tags[i].name);

    std::map<std::string, std::string> v;
    v["status_cls"] = ok ? "pass" : "fail";
    v["status_txt"] = ok ? "PASS" : "FAIL";
    v["name"] = html_escape(r.name);
    v["secs"] = fmt_secs(r.elapsed);
    v["timed_out"] = timed_out;
    v["insns"] = insns;
    v["desc"] = html_escape(r.description.empty() ? std::string("(no description)") : r.description);
    v["dump"] = dump;
    v["args"] = html_escape(join(r.args, " "));
    v["checks"] = checks_html;
    v["tags"] = tags_html(r);
    v["tagdata"] = html_escape(join(tagdata, "|"));
    v["tuya"] = tuya_html(r);
    v["tuya_dis"] = r.tuya_config.valid ? "" : " disabled";
    v["gpio"] = gpio_html(r);
    v["gpio_dis"] = r.has_periph ? "" : " disabled";
    v["uart1"] = uart1_html(r);
    v["uart1_dis"] = !r.uart1.empty() ? "" : " disabled";
    v["log"] = highlight(r.output, found);
    return fill(TPL_CARD, TPL_CARD_N, v);
}

struct ByCountThenName {
    bool operator()(const std::pair<std::string, int> &a, const std::pair<std::string, int> &b) const
    {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    }
};

static std::string filters_html(const std::vector<Result> &results)
{
    static const char *order[] = {"source", "chip", "state", "feature", "data"};
    std::map<std::string, std::map<std::string, int> > groups;
    for (size_t i = 0; i < results.size(); i++)
        for (size_t k = 0; k < results[i].tags.size(); k++)
            groups[results[i].tags[k].group][results[i].tags[k].name]++;
    if (groups.empty())
        return "";
    std::string blocks;
    for (int gi = 0; gi < 5; gi++) {
        std::string g = order[gi];
        if (!groups.count(g))
            continue;
        std::vector<std::pair<std::string, int> > items(groups[g].begin(), groups[g].end());
        std::stable_sort(items.begin(), items.end(), ByCountThenName());
        std::string chips;
        for (size_t i = 0; i < items.size(); i++) {
            std::string cls = replace_all(tag_class(Tag(items[i].first, g)), "tag ", "");
            chips += "<button class=\"fchip " + cls + "\" type=\"button\" data-group=\"" + html_escape(g)
                     + "\" data-tag=\"" + html_escape(items[i].first) + "\">" + html_escape(items[i].first)
                     + strf("<span class=\"fcount\">%d</span></button>", items[i].second);
        }
        blocks += "<div class=\"fgroup\"><span class=\"flabel\">" + html_escape(g) + "</span>" + chips + "</div>";
    }
    return pct(FILTERS_BLOCK, A(blocks));
}

/* days since 1970-01-01 of a proleptic Gregorian date */
static long long days_from_civil(long long y, long long m, long long d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* re.match(r"(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d) UTC", s) -> epoch ms, or "" */
static std::string generated_ms(const std::string &s)
{
    const char *shape = "dddd-dd-dd dd:dd UTC";
    size_t n = strlen(shape);
    if (s.size() < n)
        return "";
    for (size_t i = 0; i < n; i++) {
        if (shape[i] == 'd') {
            if (s[i] < '0' || s[i] > '9')
                return "";
        } else if (s[i] != shape[i]) {
            return "";
        }
    }
    int y = atoi(s.substr(0, 4).c_str()), mo = atoi(s.substr(5, 2).c_str()), d = atoi(s.substr(8, 2).c_str());
    int h = atoi(s.substr(11, 2).c_str()), mi = atoi(s.substr(14, 2).c_str());
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59)
        return ""; /* Python's datetime() would raise here */
    long long secs = days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL;
    return strf("%lld", secs * 1000LL);
}

std::string render(const std::vector<Result> &results, const Meta &meta)
{
    int passed = 0;
    for (size_t i = 0; i < results.size(); i++)
        if (results[i].passed)
            passed++;
    int failed = (int)results.size() - passed;

    std::string commit_html;
    if (!meta.commit.empty()) {
        std::string short_ = meta.commit.substr(0, 8);
        if (!meta.repo.empty())
            commit_html = "commit <a href=\"https://github.com/" + html_escape(meta.repo) + "/commit/"
                          + html_escape(meta.commit) + "\"><code>" + short_ + "</code></a>";
        else
            commit_html = "commit <code>" + short_ + "</code>";
    }
    std::string run_html;
    if (!meta.run_url.empty())
        run_html = " \xc2\xb7 <a href=\"" + html_escape(meta.run_url) + "\">CI run</a>";

    StrList chips = meta.chips;
    if (chips.empty()) {
        std::set<std::string> cs;
        for (size_t i = 0; i < results.size(); i++)
            if (!results[i].chip.empty())
                cs.insert(results[i].chip);
        chips.assign(cs.begin(), cs.end());
    }

    std::string cards;
    for (size_t i = 0; i < results.size(); i++)
        cards += test_card(results[i]);

    std::map<std::string, std::string> v;
    v["overall_cls"] = failed == 0 ? "pass" : "fail";
    v["passed"] = strf("%d", passed);
    v["failed"] = strf("%d", failed);
    v["total"] = strf("%d", (int)results.size());
    v["runtime"] = fmt_compact(meta.total_time);
    v["chips"] = strf("%d", (int)chips.size());
    v["chips_list"] = html_escape(join(chips, ", "));
    v["total_time"] = fmt_total(meta.total_time);
    v["generated"] = html_escape(meta.generated_at);
    v["generated_ms"] = generated_ms(meta.generated_at);
    v["commit_html"] = commit_html;
    v["run_html"] = run_html;
    v["filters"] = filters_html(results);
    v["cards"] = cards;
    v["script"] = SCRIPT;
    return fill(TPL_PAGE, TPL_PAGE_N, v);
}

bool generate(const std::vector<Result> &results, const Meta &meta, const std::string &out_path, std::string *err)
{
    std::string page = render(results, meta);
    /* text mode, like Python's open(path, "w"): "\n" -> "\r\n" on Windows */
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, out_path.c_str(), -1, 0, 0);
    std::vector<wchar_t> w(wn > 0 ? wn : 1);
    MultiByteToWideChar(CP_UTF8, 0, out_path.c_str(), -1, &w[0], wn);
    FILE *f = _wfopen(&w[0], L"w");
#else
    FILE *f = fopen(out_path.c_str(), "w");
#endif
    if (!f) {
        if (err)
            *err = "cannot open " + out_path + " for writing";
        return false;
    }
    bool ok = fwrite(page.data(), 1, page.size(), f) == page.size();
    if (fclose(f) != 0)
        ok = false;
    if (!ok && err)
        *err = "write failed: " + out_path;
    return ok;
}

} /* namespace report */
