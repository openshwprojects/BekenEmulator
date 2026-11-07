/* Shared basics for the C++ port: fixed-width types, a binary-safe byte
 * string (std::string, the closest thing classic C++ has to Python's bytes),
 * and the handful of Python string/number helpers the ported modules lean on.
 */
#ifndef BK_COMMON_H
#define BK_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <utility>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* Python bytes / bytearray. Binary-safe: std::string never stops at NUL. */
typedef std::string Bytes;
typedef std::vector<std::string> StrList;

#if defined(__GNUC__)
#define BK_LIKELY(x) __builtin_expect(!!(x), 1)
#define BK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BK_PRINTF(a, b) __attribute__((format(gnu_printf, a, b)))
#define BK_INLINE inline __attribute__((always_inline))
#else
#define BK_LIKELY(x) (x)
#define BK_UNLIKELY(x) (x)
#define BK_PRINTF(a, b)
#define BK_INLINE inline
#endif

/* printf into a std::string */
std::string strf(const char *fmt, ...) BK_PRINTF(1, 2);
std::string vstrf(const char *fmt, va_list ap);

/* little/big endian access */
static inline u16 rd16le(const void *p) { const u8 *b = (const u8 *)p; return (u16)(b[0] | (b[1] << 8)); }
static inline u32 rd32le(const void *p) { const u8 *b = (const u8 *)p; return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24); }
static inline u64 rd64le(const void *p) { return (u64)rd32le(p) | ((u64)rd32le((const u8 *)p + 4) << 32); }
static inline u16 rd16be(const void *p) { const u8 *b = (const u8 *)p; return (u16)((b[0] << 8) | b[1]); }
static inline u32 rd32be(const void *p) { const u8 *b = (const u8 *)p; return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3]; }
static inline u64 rd64be(const void *p) { return ((u64)rd32be(p) << 32) | (u64)rd32be((const u8 *)p + 4); }
static inline void wr16le(void *p, u16 v) { u8 *b = (u8 *)p; b[0] = (u8)v; b[1] = (u8)(v >> 8); }
static inline void wr32le(void *p, u32 v) { u8 *b = (u8 *)p; b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24); }
static inline void wr64le(void *p, u64 v) { wr32le(p, (u32)v); wr32le((u8 *)p + 4, (u32)(v >> 32)); }
static inline void wr16be(void *p, u16 v) { u8 *b = (u8 *)p; b[0] = (u8)(v >> 8); b[1] = (u8)v; }
static inline void wr32be(void *p, u32 v) { u8 *b = (u8 *)p; b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16); b[2] = (u8)(v >> 8); b[3] = (u8)v; }
static inline void wr64be(void *p, u64 v) { wr32be(p, (u32)(v >> 32)); wr32be((u8 *)p + 4, (u32)v); }

/* Bytes builders: struct.pack equivalents */
Bytes p8(u32 v);
Bytes p16le(u32 v);
Bytes p32le(u32 v);
Bytes p64le(u64 v);
Bytes p16be(u32 v);
Bytes p32be(u32 v);
Bytes p64be(u64 v);
/* struct.unpack_from equivalents on a Bytes; out-of-range bytes read as 0 */
u32 g8(const Bytes &b, size_t off);
u32 g16le(const Bytes &b, size_t off);
u32 g32le(const Bytes &b, size_t off);
u64 g64le(const Bytes &b, size_t off);
u32 g16be(const Bytes &b, size_t off);
u32 g32be(const Bytes &b, size_t off);
u64 g64be(const Bytes &b, size_t off);

static inline u8 bat(const Bytes &b, size_t i) { return (u8)b[i]; }
Bytes bytes_n(size_t n, u8 fill = 0);
Bytes bslice(const Bytes &b, long long start, long long end); /* Python b[start:end] */
Bytes bfrom(const Bytes &b, long long start);                 /* Python b[start:] */
Bytes bhead(const Bytes &b, long long end);                   /* Python b[:end] */
Bytes reversed_bytes(const Bytes &b);
Bytes bconcat(const Bytes &a, const Bytes &b);
Bytes join_bytes(const std::vector<Bytes> &parts); /* b"".join(parts) */

/* hex */
std::string hexstr(const Bytes &b);                        /* bytes.hex() */
std::string hexstr_sep(const Bytes &b, const char *sep);    /* bytes.hex(sep) */
Bytes unhex(const std::string &s, bool *ok = 0);             /* bytes.fromhex (spaces allowed) */
Bytes H(const char *hex);                                    /* unhex for literals in code */

/* Python-ish string helpers */
bool startswith(const std::string &s, const std::string &p);
bool endswith(const std::string &s, const std::string &p);
bool contains(const std::string &s, const std::string &p);
size_t count_sub(const std::string &s, const std::string &p); /* str.count */
std::string lower(const std::string &s);
std::string upper(const std::string &s);
std::string strip(const std::string &s, const char *chars = " \t\r\n\f\v");
std::string lstrip(const std::string &s, const char *chars = " \t\r\n\f\v");
std::string rstrip(const std::string &s, const char *chars = " \t\r\n\f\v");
StrList split(const std::string &s, const std::string &sep, int maxsplit = -1);
StrList split_ws(const std::string &s); /* str.split() */
std::string join(const StrList &parts, const std::string &sep);
std::string replace_all(const std::string &s, const std::string &from, const std::string &to);
/* str.partition: returns true when sep was found */
bool partition(const std::string &s, const std::string &sep, std::string &head, std::string &tail);
std::string ljust(const std::string &s, size_t width, char fill = ' ');
std::string rjust(const std::string &s, size_t width, char fill = ' ');
bool is_ascii(const std::string &s);
bool is_alnum_ascii(const std::string &s); /* bytes.isalnum(): non-empty, all [A-Za-z0-9] */
bool is_digits(const std::string &s);      /* str.isdigit() for ASCII */
/* printable ASCII (32..126) or '.', as the log dumps render bytes */
std::string printable(const Bytes &b, size_t limit = (size_t)-1);
/* Latin-1 bytes to UTF-8 text (Python's decode("latin-1")) */
std::string latin1_to_utf8(const Bytes &b);
/* Python bytes.decode("utf-8", "replace") / "ignore": invalid sequences -> U+FFFD / dropped */
std::string utf8_replace(const Bytes &b);
std::string utf8_ignore(const Bytes &b);
/* UTF-8 text to Latin-1 bytes; false if a character is outside Latin-1 */
bool utf8_to_latin1(const std::string &s, Bytes &out);
size_t utf8_length(const std::string &s); /* number of code points */

/* Python int(text, base); base 0 = auto prefix. Accepts surrounding
 * whitespace, a sign and single underscores between digits. */
bool parse_int(const std::string &text, int base, long long &out);
bool parse_double(const std::string &text, double &out);
/* Python round() of a double to an integer (banker's rounding) */
long long py_round(double x);
/* Python repr(float): shortest round-tripping form */
std::string py_float_repr(double x);
/* Python "%g" */
std::string py_g(double x);

/* files and paths */
bool read_file(const std::string &path, Bytes &out);
bool write_file(const std::string &path, const Bytes &data);
bool file_exists(const std::string &path);
bool is_dir(const std::string &path);
bool make_dirs(const std::string &path);
bool remove_file(const std::string &path);
std::string path_basename(const std::string &p);
std::string path_dirname(const std::string &p);
std::string path_join(const std::string &a, const std::string &b);
std::string path_abs(const std::string &p);
std::string path_stem(const std::string &p); /* basename without extension */
std::string temp_dir();
std::string make_temp_file(const std::string &prefix, const std::string &suffix);

/* environment */
std::string env_get(const char *name);
/* os.urandom(n): n bytes from the OS cryptographic RNG. */
Bytes os_urandom(size_t n);
bool env_has(const char *name);
void env_set(const char *name, const char *value);

/* stdout/stderr as raw byte streams (Python sys.__stdout__.buffer) */
void out_write(const Bytes &b);
void out_write(const char *s);
void out_flush();
void err_write(const std::string &s);

/* printf-style to stdout, newline included by the caller */
void outf(const char *fmt, ...) BK_PRINTF(1, 2);

#endif
