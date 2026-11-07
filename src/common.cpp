#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

std::string vstrf(const char *fmt, va_list ap)
{
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return std::string();
    if ((size_t)n < sizeof(buf))
        return std::string(buf, (size_t)n);
    std::string out((size_t)n + 1, '\0');
    vsnprintf(&out[0], out.size(), fmt, ap);
    out.resize((size_t)n);
    return out;
}

std::string strf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrf(fmt, ap);
    va_end(ap);
    return s;
}

Bytes p8(u32 v) { return Bytes(1, (char)(u8)v); }
Bytes p16le(u32 v) { char b[2]; wr16le(b, (u16)v); return Bytes(b, 2); }
Bytes p32le(u32 v) { char b[4]; wr32le(b, v); return Bytes(b, 4); }
Bytes p64le(u64 v) { char b[8]; wr64le(b, v); return Bytes(b, 8); }
Bytes p16be(u32 v) { char b[2]; wr16be(b, (u16)v); return Bytes(b, 2); }
Bytes p32be(u32 v) { char b[4]; wr32be(b, v); return Bytes(b, 4); }
Bytes p64be(u64 v) { char b[8]; wr64be(b, v); return Bytes(b, 8); }

static void grab(const Bytes &b, size_t off, u8 *dst, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = (off + i < b.size()) ? (u8)b[off + i] : 0;
}
u32 g8(const Bytes &b, size_t off) { return off < b.size() ? (u8)b[off] : 0; }
u32 g16le(const Bytes &b, size_t off) { u8 t[2]; grab(b, off, t, 2); return rd16le(t); }
u32 g32le(const Bytes &b, size_t off) { u8 t[4]; grab(b, off, t, 4); return rd32le(t); }
u64 g64le(const Bytes &b, size_t off) { u8 t[8]; grab(b, off, t, 8); return rd64le(t); }
u32 g16be(const Bytes &b, size_t off) { u8 t[2]; grab(b, off, t, 2); return rd16be(t); }
u32 g32be(const Bytes &b, size_t off) { u8 t[4]; grab(b, off, t, 4); return rd32be(t); }
u64 g64be(const Bytes &b, size_t off) { u8 t[8]; grab(b, off, t, 8); return rd64be(t); }

Bytes bytes_n(size_t n, u8 fill) { return Bytes(n, (char)fill); }

static long long clamp_index(long long i, long long n)
{
    if (i < 0) {
        i += n;
        if (i < 0)
            i = 0;
    }
    if (i > n)
        i = n;
    return i;
}

Bytes bslice(const Bytes &b, long long start, long long end)
{
    long long n = (long long)b.size();
    start = clamp_index(start, n);
    end = clamp_index(end, n);
    if (end <= start)
        return Bytes();
    return b.substr((size_t)start, (size_t)(end - start));
}
Bytes bfrom(const Bytes &b, long long start) { return bslice(b, start, (long long)b.size()); }
Bytes bhead(const Bytes &b, long long end) { return bslice(b, 0, end); }
Bytes reversed_bytes(const Bytes &b) { return Bytes(b.rbegin(), b.rend()); }
Bytes bconcat(const Bytes &a, const Bytes &b) { return a + b; }
Bytes join_bytes(const std::vector<Bytes> &parts)
{
    Bytes out;
    for (size_t i = 0; i < parts.size(); i++)
        out += parts[i];
    return out;
}

static const char HEXD[] = "0123456789abcdef";

std::string hexstr(const Bytes &b)
{
    std::string out;
    out.reserve(b.size() * 2);
    for (size_t i = 0; i < b.size(); i++) {
        u8 c = (u8)b[i];
        out += HEXD[c >> 4];
        out += HEXD[c & 15];
    }
    return out;
}

std::string hexstr_sep(const Bytes &b, const char *sep)
{
    std::string out;
    for (size_t i = 0; i < b.size(); i++) {
        if (i)
            out += sep;
        u8 c = (u8)b[i];
        out += HEXD[c >> 4];
        out += HEXD[c & 15];
    }
    return out;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

Bytes unhex(const std::string &s, bool *ok)
{
    Bytes out;
    int hi = -1;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            if (hi >= 0) {
                if (ok)
                    *ok = false;
                return Bytes();
            }
            continue;
        }
        int v = hexval(c);
        if (v < 0) {
            if (ok)
                *ok = false;
            return Bytes();
        }
        if (hi < 0)
            hi = v;
        else {
            out += (char)(u8)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) {
        if (ok)
            *ok = false;
        return Bytes();
    }
    if (ok)
        *ok = true;
    return out;
}

Bytes H(const char *hex) { return unhex(hex, 0); }

bool startswith(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endswith(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string &s, const std::string &p) { return s.find(p) != std::string::npos; }

size_t count_sub(const std::string &s, const std::string &p)
{
    if (p.empty())
        return s.size() + 1;
    size_t n = 0, pos = 0;
    while ((pos = s.find(p, pos)) != std::string::npos) {
        n++;
        pos += p.size();
    }
    return n;
}

std::string lower(const std::string &s)
{
    std::string o(s);
    for (size_t i = 0; i < o.size(); i++)
        if (o[i] >= 'A' && o[i] <= 'Z')
            o[i] = (char)(o[i] + 32);
    return o;
}
std::string upper(const std::string &s)
{
    std::string o(s);
    for (size_t i = 0; i < o.size(); i++)
        if (o[i] >= 'a' && o[i] <= 'z')
            o[i] = (char)(o[i] - 32);
    return o;
}

std::string lstrip(const std::string &s, const char *chars)
{
    size_t b = s.find_first_not_of(chars);
    return b == std::string::npos ? std::string() : s.substr(b);
}
std::string rstrip(const std::string &s, const char *chars)
{
    size_t e = s.find_last_not_of(chars);
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}
std::string strip(const std::string &s, const char *chars) { return lstrip(rstrip(s, chars), chars); }

StrList split(const std::string &s, const std::string &sep, int maxsplit)
{
    StrList out;
    size_t start = 0;
    int n = 0;
    while (maxsplit < 0 || n < maxsplit) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos)
            break;
        out.push_back(s.substr(start, pos - start));
        start = pos + sep.size();
        n++;
    }
    out.push_back(s.substr(start));
    return out;
}

StrList split_ws(const std::string &s)
{
    StrList out;
    size_t i = 0;
    const char *ws = " \t\r\n\f\v";
    while (i < s.size()) {
        while (i < s.size() && strchr(ws, s[i]) && s[i])
            i++;
        if (i >= s.size())
            break;
        size_t j = i;
        while (j < s.size() && !(strchr(ws, s[j]) && s[j]))
            j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string join(const StrList &parts, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

std::string replace_all(const std::string &s, const std::string &from, const std::string &to)
{
    if (from.empty())
        return s;
    std::string out;
    size_t start = 0, pos;
    while ((pos = s.find(from, start)) != std::string::npos) {
        out.append(s, start, pos - start);
        out += to;
        start = pos + from.size();
    }
    out.append(s, start, std::string::npos);
    return out;
}

bool partition(const std::string &s, const std::string &sep, std::string &head, std::string &tail)
{
    size_t pos = s.find(sep);
    if (pos == std::string::npos) {
        head = s;
        tail.clear();
        return false;
    }
    head = s.substr(0, pos);
    tail = s.substr(pos + sep.size());
    return true;
}

std::string ljust(const std::string &s, size_t width, char fill)
{
    return s.size() >= width ? s : s + std::string(width - s.size(), fill);
}
std::string rjust(const std::string &s, size_t width, char fill)
{
    return s.size() >= width ? s : std::string(width - s.size(), fill) + s;
}

bool is_ascii(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++)
        if ((u8)s[i] >= 0x80)
            return false;
    return true;
}

bool is_alnum_ascii(const std::string &s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    }
    return true;
}

bool is_digits(const std::string &s)
{
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

std::string printable(const Bytes &b, size_t limit)
{
    std::string out;
    size_t n = b.size() < limit ? b.size() : limit;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)b[i];
        out += (c >= 32 && c < 127) ? (char)c : '.';
    }
    return out;
}

static void put_utf8(std::string &out, u32 cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string latin1_to_utf8(const Bytes &b)
{
    std::string out;
    out.reserve(b.size());
    for (size_t i = 0; i < b.size(); i++)
        put_utf8(out, (u8)b[i]);
    return out;
}

/* One UTF-8 decoding step with Python's "maximal subpart" error recovery.
 * Returns the code point (or -1 for an invalid subsequence) and how many
 * bytes it consumed. */
static long utf8_step(const Bytes &b, size_t i, size_t &used)
{
    u8 c = (u8)b[i];
    if (c < 0x80) {
        used = 1;
        return c;
    }
    int need;
    u8 lo = 0x80, hi = 0xBF;
    u32 cp;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0F;
        if (c == 0xE0)
            lo = 0xA0;
        if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07;
        if (c == 0xF0)
            lo = 0x90;
        if (c == 0xF4)
            hi = 0x8F;
    } else {
        used = 1;
        return -1;
    }
    size_t j = i + 1;
    for (int k = 0; k < need; k++, j++) {
        if (j >= b.size()) {
            used = j - i;
            return -1;
        }
        u8 d = (u8)b[j];
        u8 l = k == 0 ? lo : 0x80, h = k == 0 ? hi : 0xBF;
        if (d < l || d > h) {
            used = j - i;
            return -1;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    used = j - i;
    return (long)cp;
}

std::string utf8_replace(const Bytes &b)
{
    std::string out;
    out.reserve(b.size());
    size_t i = 0;
    while (i < b.size()) {
        size_t used;
        long cp = utf8_step(b, i, used);
        if (cp < 0)
            out += "\xEF\xBF\xBD";
        else
            out.append(b, i, used);
        i += used;
    }
    return out;
}

std::string utf8_ignore(const Bytes &b)
{
    std::string out;
    size_t i = 0;
    while (i < b.size()) {
        size_t used;
        long cp = utf8_step(b, i, used);
        if (cp >= 0)
            out.append(b, i, used);
        i += used;
    }
    return out;
}

bool utf8_to_latin1(const std::string &s, Bytes &out)
{
    out.clear();
    size_t i = 0;
    while (i < s.size()) {
        size_t used;
        long cp = utf8_step(s, i, used);
        if (cp < 0 || cp > 0xFF)
            return false;
        out += (char)(u8)cp;
        i += used;
    }
    return true;
}

size_t utf8_length(const std::string &s)
{
    size_t n = 0, i = 0;
    while (i < s.size()) {
        size_t used;
        utf8_step(s, i, used);
        i += used;
        n++;
    }
    return n;
}

bool parse_int(const std::string &text, int base, long long &out)
{
    std::string t = strip(text);
    if (t.empty())
        return false;
    size_t i = 0;
    bool neg = false;
    if (t[i] == '+' || t[i] == '-') {
        neg = t[i] == '-';
        i++;
    }
    if (base == 0) {
        if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
            base = 16;
            i += 2;
            if (i < t.size() && t[i] == '_')
                i++;
        } else if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'o' || t[i + 1] == 'O')) {
            base = 8;
            i += 2;
            if (i < t.size() && t[i] == '_')
                i++;
        } else if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'b' || t[i + 1] == 'B')) {
            base = 2;
            i += 2;
            if (i < t.size() && t[i] == '_')
                i++;
        } else {
            base = 10;
            /* Python forbids leading zeros in base-0 decimal ("010") */
            size_t j = i;
            bool nonzero = false, leading_zero = false;
            if (j < t.size() && t[j] == '0')
                leading_zero = true;
            for (; j < t.size(); j++)
                if (t[j] != '0' && t[j] != '_')
                    nonzero = true;
            if (leading_zero && nonzero)
                return false;
        }
    } else if (base == 16) {
        if (i + 1 < t.size() && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X'))
            i += 2;
    }
    if (i >= t.size())
        return false;
    unsigned long long v = 0;
    bool any = false, last_us = true;
    for (; i < t.size(); i++) {
        char c = t[i];
        if (c == '_') {
            if (last_us)
                return false;
            last_us = true;
            continue;
        }
        int d = hexval(c);
        if (d < 0 || d >= base)
            return false;
        v = v * (unsigned)base + (unsigned)d;
        any = true;
        last_us = false;
    }
    if (!any || last_us)
        return false;
    out = neg ? -(long long)v : (long long)v;
    return true;
}

bool parse_double(const std::string &text, double &out)
{
    std::string t = strip(text);
    if (t.empty())
        return false;
    char *end = 0;
    errno = 0;
    double v = strtod(t.c_str(), &end);
    if (!end || *end)
        return false;
    out = v;
    return true;
}

long long py_round(double x) { return (long long)rint(x); }

std::string py_float_repr(double x)
{
    if (x != x)
        return "nan";
    if (x == HUGE_VAL)
        return "inf";
    if (x == -HUGE_VAL)
        return "-inf";
    if (x == 0)
        return signbit(x) ? "-0.0" : "0.0";
    char buf[64];
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, x);
        if (strtod(buf, 0) == x)
            break;
    }
    /* buf = [-]d.ddde[+-]XX ; rebuild in Python's repr layout */
    std::string s(buf);
    bool neg = s[0] == '-';
    if (neg)
        s = s.substr(1);
    size_t epos = s.find('e');
    std::string mant = s.substr(0, epos);
    int exp10 = atoi(s.c_str() + epos + 1);
    std::string digits;
    for (size_t i = 0; i < mant.size(); i++)
        if (mant[i] != '.')
            digits += mant[i];
    while (digits.size() > 1 && digits[digits.size() - 1] == '0')
        digits.erase(digits.size() - 1);
    int decpt = exp10 + 1;
    std::string out;
    if (decpt <= -4 || decpt > 16) {
        out = digits.substr(0, 1);
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        int e = decpt - 1;
        out += strf("e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (decpt <= 0) {
        out = "0." + std::string((size_t)-decpt, '0') + digits;
    } else if ((size_t)decpt >= digits.size()) {
        out = digits + std::string((size_t)decpt - digits.size(), '0') + ".0";
    } else {
        out = digits.substr(0, (size_t)decpt) + "." + digits.substr((size_t)decpt);
    }
    return neg ? "-" + out : out;
}

std::string py_g(double x)
{
    if (x != x)
        return "nan";
    if (x == HUGE_VAL)
        return "inf";
    if (x == -HUGE_VAL)
        return "-inf";
    return strf("%g", x);
}

#ifdef _WIN32
static std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}
static FILE *open_file(const std::string &path, const char *mode)
{
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
}
#else
static FILE *open_file(const std::string &path, const char *mode) { return fopen(path.c_str(), mode); }
#endif

bool read_file(const std::string &path, Bytes &out)
{
    FILE *f = open_file(path, "rb");
    if (!f)
        return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool write_file(const std::string &path, const Bytes &data)
{
    FILE *f = open_file(path, "wb");
    if (!f)
        return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

bool file_exists(const std::string &path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool is_dir(const std::string &path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool make_dirs(const std::string &path)
{
    if (path.empty() || is_dir(path))
        return true;
    std::string parent = path_dirname(path);
    if (!parent.empty() && parent != path && !is_dir(parent))
        make_dirs(parent);
#ifdef _WIN32
    return CreateDirectoryW(widen(path).c_str(), 0) || is_dir(path);
#else
    return mkdir(path.c_str(), 0777) == 0 || is_dir(path);
#endif
}

bool remove_file(const std::string &path)
{
#ifdef _WIN32
    return DeleteFileW(widen(path).c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

static bool is_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

std::string path_basename(const std::string &p)
{
    size_t i = p.size();
    while (i > 0 && !is_sep(p[i - 1]))
        i--;
    return p.substr(i);
}

std::string path_dirname(const std::string &p)
{
    size_t i = p.size();
    while (i > 0 && !is_sep(p[i - 1]))
        i--;
    if (i == 0)
        return std::string();
    size_t j = i - 1;
    while (j > 0 && is_sep(p[j - 1]))
        j--;
    if (j == 0)
        return p.substr(0, 1);
#ifdef _WIN32
    if (j == 2 && p[1] == ':')
        return p.substr(0, 3);
#endif
    return p.substr(0, j);
}

std::string path_join(const std::string &a, const std::string &b)
{
    if (a.empty())
        return b;
    if (!b.empty() && (is_sep(b[0]) || (b.size() > 1 && b[1] == ':')))
        return b;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    if (is_sep(a[a.size() - 1]))
        return a + b;
    return a + sep + b;
}

std::string path_abs(const std::string &p)
{
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(widen(p).c_str(), 4096, buf, 0);
    if (n == 0 || n >= 4096)
        return p;
    return narrow(std::wstring(buf, n));
#else
    char buf[4096];
    if (realpath(p.c_str(), buf))
        return buf;
    return p;
#endif
}

std::string path_stem(const std::string &p)
{
    std::string b = path_basename(p);
    size_t dot = b.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return b;
    return b.substr(0, dot);
}

static bool dir_writable(const std::string &dir)
{
    if (dir.empty() || !is_dir(dir))
        return false;
    std::string probe = path_join(dir, strf("bk_tmp_probe_%x", (unsigned)clock()));
    FILE *f = open_file(probe, "wb");
    if (!f)
        return false;
    fclose(f);
    remove_file(probe);
    return true;
}

/* Python's tempfile.gettempdir(): the first WRITABLE of TMPDIR, TEMP, TMP,
 * the platform's usual places, then the working directory. (A launcher that
 * strips TMP, e.g. msys2's coreutils, makes GetTempPath fall back to an
 * unwritable C:\Windows; Python would still find a usable directory.) */
std::string temp_dir()
{
    static std::string cached;
    if (!cached.empty())
        return cached;
    StrList cands;
    const char *envs[] = {"TMPDIR", "TEMP", "TMP"};
    for (int i = 0; i < 3; i++)
        cands.push_back(env_get(envs[i]));
#ifdef _WIN32
    if (!env_get("LOCALAPPDATA").empty())
        cands.push_back(path_join(env_get("LOCALAPPDATA"), "Temp"));
    if (!env_get("SYSTEMROOT").empty())
        cands.push_back(path_join(env_get("SYSTEMROOT"), "Temp"));
    cands.push_back("c:\\temp");
    cands.push_back("c:\\tmp");
    cands.push_back("\\temp");
    cands.push_back("\\tmp");
#else
    cands.push_back("/tmp");
    cands.push_back("/var/tmp");
    cands.push_back("/usr/tmp");
#endif
    cands.push_back(".");
    for (size_t i = 0; i < cands.size(); i++) {
        std::string s = cands[i];
        while (s.size() > 1 && is_sep(s[s.size() - 1]))
            s.erase(s.size() - 1);
        if (dir_writable(s))
            return cached = s;
    }
    return ".";
}

std::string make_temp_file(const std::string &prefix, const std::string &suffix)
{
    static unsigned counter = 0;
    std::string dir = temp_dir();
    for (int tries = 0; tries < 1000; tries++) {
#ifdef _WIN32
        unsigned pid = (unsigned)GetCurrentProcessId();
        unsigned tick = (unsigned)GetTickCount();
#else
        unsigned pid = (unsigned)getpid();
        unsigned tick = (unsigned)time(0);
#endif
        counter++;
        std::string path = path_join(dir, strf("%s%x%x%x%s", prefix.c_str(), pid, tick, counter, suffix.c_str()));
        if (file_exists(path))
            continue;
        FILE *f = open_file(path, "wb");
        if (f) {
            fclose(f);
            return path;
        }
    }
    return std::string();
}

Bytes os_urandom(size_t n)
{
    Bytes out(n, '\0');
    if (!n)
        return out;
    bool ok = false;
#ifdef _WIN32
    /* RtlGenRandom (advapi32!SystemFunction036), resolved at run time so no
     * extra import library is needed. */
    typedef BOOLEAN(WINAPI * GenRandom)(PVOID, ULONG);
    static GenRandom fn = 0;
    if (!fn) {
        HMODULE h = LoadLibraryA("advapi32.dll");
        if (h)
            fn = (GenRandom)(void *)GetProcAddress(h, "SystemFunction036");
    }
    ok = fn && fn(&out[0], (ULONG)n);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        ok = fread(&out[0], 1, n, f) == n;
        fclose(f);
    }
#endif
    if (!ok) {
        fprintf(stderr, "os_urandom: no OS random source\n");
        abort();
    }
    return out;
}

std::string env_get(const char *name)
{
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf, 32768);
    if (n == 0 || n >= 32768)
        return std::string();
    return narrow(std::wstring(buf, n));
#else
    const char *v = getenv(name);
    return v ? v : "";
#endif
}

bool env_has(const char *name) { return !env_get(name).empty(); }

void env_set(const char *name, const char *value)
{
#ifdef _WIN32
    SetEnvironmentVariableW(widen(name).c_str(), value ? widen(value).c_str() : 0);
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static bool g_stdout_binary = false;
static void ensure_binary()
{
    if (g_stdout_binary)
        return;
    g_stdout_binary = true;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

void out_write(const Bytes &b)
{
    ensure_binary();
    if (!b.empty())
        fwrite(b.data(), 1, b.size(), stdout);
}
void out_write(const char *s)
{
    ensure_binary();
    fputs(s, stdout);
}
void out_flush() { fflush(stdout); }
void err_write(const std::string &s)
{
    fwrite(s.data(), 1, s.size(), stderr);
    fflush(stderr);
}

void outf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrf(fmt, ap);
    va_end(ap);
    out_write(s);
}
