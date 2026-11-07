/* Test-case definitions shared by the C++ self-test harness. The data array is
 * generated from tests/self_test.py (TEST_CASES + DESCRIPTIONS) by
 * tools/gen_cases.py, 1:1. POD-only so it aggregate-initialises in classic C++.
 *
 * Python's None is encoded as -1 for ints and as a null pointer for strings. */
#ifndef BK_TESTCASE_H
#define BK_TESTCASE_H

struct ExpectStr {
    const char *s;
    int count;
    int repeats; /* 1 = count is tests/self_test.py's REPEATS (env REPEATS, default 2) */
};
struct PinWant {
    int pin;
    int want; /* -1 = None (assert untouched) */
};
struct PwmWant {
    int ch;
    int period;
    int duty; /* -1 = None (period only) */
};
/* one "http_probe" request (a case may list several, run in order) */
struct HttpProbe {
    const char *after;  /* only the first probe's "after" is used, like Python */
    const char *path;
    const char *method; /* "GET" default */
    const char *body;   /* 0 = None */
    const char *tail;   /* "expect_tail", 0 = None */
    const char *const *expect;
    int n_expect;
};

struct TestCase {
    const char *name;
    const char *binary; /* relative to repo root */
    const char *const *args;
    int n_args;
    int timeout;
    const char *kind;  /* "" or "snapshot_roundtrip" */
    int snapshot_at;
    int min_compared_lines; /* default 10 */
    const ExpectStr *expected;
    int n_expected;
    const char *const *unexpected;
    int n_unexpected;
    const PinWant *pins; /* sorted by pin */
    int n_pins;
    const PwmWant *pwm;  /* sorted by channel */
    int n_pwm;
    const char *const *pcap_expect;
    int n_pcap;
    const char *const *ip_pcap_expect;
    int n_ippcap;
    const HttpProbe *probes;
    int n_probes;
    const char *const *tags; /* the case's own "tags": [...] */
    int n_tags;
    int insn_tolerance;      /* snapshot cases; default 1000 */
    const char *description; /* DESCRIPTIONS[name], "" when none */
};

extern TestCase *g_cases;
extern int g_case_count;

#endif
