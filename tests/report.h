/* C++ port of tests/report.py: render the self-test results as one
 * self-contained HTML file (inline CSS/JS), byte-identical to the Python
 * report for the same results. The record below mirrors the dict that
 * tests/self_test.py's _result() builds; Python's None / missing keys are
 * the has_* flags and the -1 sentinels. */
#ifndef BK_REPORT_H
#define BK_REPORT_H

#include "common.h"
#include "periph.h"
#include "tuya_config.h"

namespace report {

struct Check {
    std::string string;
    bool found;
    int count;
    int required;
    Check() : found(false), count(0), required(1) {}
    Check(const std::string &s, bool f, int c, int r) : string(s), found(f), count(c), required(r) {}
};

struct Tag {
    std::string name;
    std::string group;
    Tag() {}
    Tag(const std::string &n, const std::string &g) : name(n), group(g) {}
};

struct Result {
    std::string name;
    std::string description;
    std::string binary;
    std::string binary_name;
    std::string chip;
    tuya_config::Decoded tuya_config; /* .valid == false is None */
    std::vector<Tag> tags;
    std::string dump_url;
    StrList args;
    bool passed;
    bool timed_out;
    double elapsed;
    long long insns;                  /* -1 = None */
    std::vector<Check> checks;
    std::string output;
    bool has_periph;                  /* result["periph"] exists and is not None */
    periph::DecodedState periph;
    StrList uart1;                    /* result["uart1"] (empty list == missing) */
    Result() : passed(false), timed_out(false), elapsed(0), insns(-1), has_periph(false) {}
};

struct Meta {
    double total_time;
    StrList chips;
    std::string generated_at;
    std::string commit;  /* "" = None */
    std::string repo;    /* "" = None */
    std::string run_url; /* "" = None */
    Meta() : total_time(0) {}
};

/* html.escape(s, quote=True) */
std::string html_escape(const std::string &s);

/* The whole page as a string. */
std::string render(const std::vector<Result> &results, const Meta &meta);

/* Render to out_path (text mode, like Python's open(..., "w")). Returns false
 * and sets *err on an I/O error. */
bool generate(const std::vector<Result> &results, const Meta &meta, const std::string &out_path,
              std::string *err = 0);

} /* namespace report */

#endif
