/* A small JSON value, parser and serializer, replacing the json module used by
 * src/tuya_config.py. The parser is strict like CPython's json.loads (returns
 * failure on trailing data or malformed input); the serializer matches
 * json.dumps(separators=(",",":"), ensure_ascii=False).
 */
#ifndef BK_JSON_H
#define BK_JSON_H

#include "common.h"

class JsonValue {
public:
    enum Type { NUL, BOOL, INT, REAL, STR, ARR, OBJ };

    JsonValue() : type_(NUL), b_(false), i_(0), r_(0) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == NUL; }
    bool is_bool() const { return type_ == BOOL; }
    bool is_number() const { return type_ == INT || type_ == REAL; }
    bool is_int() const { return type_ == INT; }
    bool is_str() const { return type_ == STR; }
    bool is_arr() const { return type_ == ARR; }
    bool is_obj() const { return type_ == OBJ; }
    /* Python treats bool as a str/int for isinstance checks only where noted;
     * tuya_config uses isinstance(v, (str, int)) which includes bool. */
    bool is_str_or_int() const { return type_ == STR || type_ == INT || type_ == BOOL; }

    bool as_bool() const { return b_; }
    long long as_int() const { return type_ == REAL ? (long long)r_ : i_; }
    double as_real() const { return type_ == INT ? (double)i_ : (type_ == REAL ? r_ : 0); }
    const std::string &as_str() const { return s_; }
    const std::vector<JsonValue> &arr() const { return arr_; }
    std::vector<JsonValue> &arr() { return arr_; }

    /* dict.get(key): returns a null value if absent; use has() to distinguish. */
    bool has(const std::string &key) const;
    const JsonValue &get(const std::string &key) const;
    /* str(v) as Python str() would render a JSON scalar (for the "ids" map). */
    std::string str_value() const;

    /* Ordered object members (insertion order, like CPython dict). */
    const std::vector<std::pair<std::string, JsonValue> > &members() const { return obj_; }

    static JsonValue make_str(const std::string &s)
    {
        JsonValue v;
        v.type_ = STR;
        v.s_ = s;
        return v;
    }
    static JsonValue make_int(long long i)
    {
        JsonValue v;
        v.type_ = INT;
        v.i_ = i;
        return v;
    }
    static JsonValue make_obj()
    {
        JsonValue v;
        v.type_ = OBJ;
        return v;
    }
    void set(const std::string &key, const JsonValue &val); /* append or replace */

    std::string dumps() const; /* compact, ensure_ascii=False */

private:
    Type type_;
    bool b_;
    long long i_;
    double r_;
    std::string s_;
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue> > obj_;
    friend bool json_parse(const std::string &text, JsonValue &out);
    friend class JsonParser;
};

/* Parse a complete JSON document. Returns false on any malformed input or
 * trailing non-whitespace, exactly like json.loads raising ValueError. */
bool json_parse(const std::string &text, JsonValue &out);

#endif
