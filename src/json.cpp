#include "json.h"

#include <math.h>
#include <stdlib.h>

static const JsonValue NULL_VALUE;

bool JsonValue::has(const std::string &key) const
{
    if (type_ != OBJ)
        return false;
    for (size_t i = 0; i < obj_.size(); i++)
        if (obj_[i].first == key)
            return true;
    return false;
}

const JsonValue &JsonValue::get(const std::string &key) const
{
    if (type_ == OBJ)
        for (size_t i = 0; i < obj_.size(); i++)
            if (obj_[i].first == key)
                return obj_[i].second;
    return NULL_VALUE;
}

void JsonValue::set(const std::string &key, const JsonValue &val)
{
    type_ = OBJ;
    for (size_t i = 0; i < obj_.size(); i++)
        if (obj_[i].first == key) {
            obj_[i].second = val;
            return;
        }
    obj_.push_back(std::make_pair(key, val));
}

std::string JsonValue::str_value() const
{
    switch (type_) {
    case NUL:
        return "None";
    case BOOL:
        return b_ ? "True" : "False";
    case INT:
        /* a parsed int keeps its literal digits: Python ints are unbounded */
        return s_.empty() ? strf("%lld", i_) : s_;
    case REAL:
        return py_float_repr(r_);
    case STR:
        return s_;
    default:
        return dumps();
    }
}

/* -------------------------------------------------------------- serialize */
static void dump_string(std::string &out, const std::string &s)
{
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        u8 c = (u8)s[i];
        switch (c) {
        case '"':
            out += "\\\"";
            i++;
            break;
        case '\\':
            out += "\\\\";
            i++;
            break;
        case '\n':
            out += "\\n";
            i++;
            break;
        case '\r':
            out += "\\r";
            i++;
            break;
        case '\t':
            out += "\\t";
            i++;
            break;
        case '\b':
            out += "\\b";
            i++;
            break;
        case '\f':
            out += "\\f";
            i++;
            break;
        default:
            if (c < 0x20)
                out += strf("\\u%04x", c);
            else
                out += (char)c; /* ensure_ascii=False: keep UTF-8 bytes verbatim */
            i++;
        }
    }
    out += '"';
}

static void dump_value(std::string &out, const JsonValue &v)
{
    switch (v.type()) {
    case JsonValue::NUL:
        out += "null";
        break;
    case JsonValue::BOOL:
        out += v.as_bool() ? "true" : "false";
        break;
    case JsonValue::INT:
        out += v.str_value();
        break;
    case JsonValue::REAL: {
        /* json.dumps writes the non-finite floats as NaN / Infinity */
        double r = v.as_real();
        if (r != r)
            out += "NaN";
        else if (r == HUGE_VAL)
            out += "Infinity";
        else if (r == -HUGE_VAL)
            out += "-Infinity";
        else
            out += py_float_repr(r);
        break;
    }
    case JsonValue::STR:
        dump_string(out, v.as_str());
        break;
    case JsonValue::ARR: {
        out += '[';
        const std::vector<JsonValue> &a = v.arr();
        for (size_t i = 0; i < a.size(); i++) {
            if (i)
                out += ',';
            dump_value(out, a[i]);
        }
        out += ']';
        break;
    }
    case JsonValue::OBJ: {
        out += '{';
        const std::vector<std::pair<std::string, JsonValue> > &m = v.members();
        for (size_t i = 0; i < m.size(); i++) {
            if (i)
                out += ',';
            dump_string(out, m[i].first);
            out += ':';
            dump_value(out, m[i].second);
        }
        out += '}';
        break;
    }
    }
}

std::string JsonValue::dumps() const
{
    std::string out;
    dump_value(out, *this);
    return out;
}

/* ------------------------------------------------------------------ parse */
class JsonParser {
public:
    JsonParser(const std::string &t) : s_(t), p_(0) {}

    bool parse(JsonValue &out)
    {
        skip_ws();
        if (!value(out))
            return false;
        skip_ws();
        return p_ == s_.size();
    }

private:
    const std::string &s_;
    size_t p_;

    void skip_ws()
    {
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                p_++;
            else
                break;
        }
    }
    char peek() { return p_ < s_.size() ? s_[p_] : '\0'; }

    bool value(JsonValue &out)
    {
        skip_ws();
        if (p_ >= s_.size())
            return false;
        char c = s_[p_];
        if (c == '{')
            return object(out);
        if (c == '[')
            return array(out);
        if (c == '"')
            return string_val(out);
        if (c == 't' || c == 'f')
            return boolean(out);
        if (c == 'n')
            return null_val(out);
        /* json.loads also takes the JavaScript constants NaN/Infinity */
        if (c == 'N' || c == 'I' || (c == '-' && p_ + 1 < s_.size() && s_[p_ + 1] == 'I')) {
            double r;
            if (literal("NaN"))
                r = strtod("nan", 0);
            else if (literal("Infinity"))
                r = strtod("inf", 0);
            else if (literal("-Infinity"))
                r = -strtod("inf", 0);
            else
                return false;
            out.type_ = JsonValue::REAL;
            out.r_ = r;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return number(out);
        return false;
    }

    bool literal(const char *lit)
    {
        size_t n = 0;
        while (lit[n]) {
            if (p_ + n >= s_.size() || s_[p_ + n] != lit[n])
                return false;
            n++;
        }
        p_ += n;
        return true;
    }

    bool boolean(JsonValue &out)
    {
        if (literal("true")) {
            out.type_ = JsonValue::BOOL;
            out.b_ = true;
            return true;
        }
        if (literal("false")) {
            out.type_ = JsonValue::BOOL;
            out.b_ = false;
            return true;
        }
        return false;
    }
    bool null_val(JsonValue &out)
    {
        if (literal("null")) {
            out.type_ = JsonValue::NUL;
            return true;
        }
        return false;
    }

    bool number(JsonValue &out)
    {
        size_t start = p_;
        bool is_real = false;
        if (peek() == '-')
            p_++;
        if (peek() == '0')
            p_++;
        else if (peek() >= '1' && peek() <= '9')
            while (peek() >= '0' && peek() <= '9')
                p_++;
        else
            return false;
        if (peek() == '.') {
            is_real = true;
            p_++;
            if (!(peek() >= '0' && peek() <= '9'))
                return false;
            while (peek() >= '0' && peek() <= '9')
                p_++;
        }
        if (peek() == 'e' || peek() == 'E') {
            is_real = true;
            p_++;
            if (peek() == '+' || peek() == '-')
                p_++;
            if (!(peek() >= '0' && peek() <= '9'))
                return false;
            while (peek() >= '0' && peek() <= '9')
                p_++;
        }
        std::string num = s_.substr(start, p_ - start);
        if (is_real) {
            out.type_ = JsonValue::REAL;
            out.r_ = strtod(num.c_str(), 0);
        } else {
            out.type_ = JsonValue::INT;
            out.i_ = strtoll(num.c_str(), 0, 10);
            out.s_ = num == "-0" ? std::string("0") : num;
        }
        return true;
    }

    bool hex4(u32 &v)
    {
        if (p_ + 4 > s_.size())
            return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s_[p_++];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return false;
            v = (v << 4) | (u32)d;
        }
        return true;
    }

    void put_utf8(std::string &out, u32 cp)
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

    bool string_val(JsonValue &out)
    {
        std::string val;
        if (!parse_string(val))
            return false;
        out.type_ = JsonValue::STR;
        out.s_ = val;
        return true;
    }

    bool parse_string(std::string &val)
    {
        if (peek() != '"')
            return false;
        p_++;
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"')
                return true;
            if (c == '\\') {
                if (p_ >= s_.size())
                    return false;
                char e = s_[p_++];
                switch (e) {
                case '"':
                    val += '"';
                    break;
                case '\\':
                    val += '\\';
                    break;
                case '/':
                    val += '/';
                    break;
                case 'b':
                    val += '\b';
                    break;
                case 'f':
                    val += '\f';
                    break;
                case 'n':
                    val += '\n';
                    break;
                case 'r':
                    val += '\r';
                    break;
                case 't':
                    val += '\t';
                    break;
                case 'u': {
                    u32 cp;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p_ + 1 < s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                            p_ += 2;
                            u32 lo;
                            if (!hex4(lo))
                                return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else {
                                put_utf8(val, cp);
                                cp = lo;
                            }
                        }
                    }
                    put_utf8(val, cp);
                    break;
                }
                default:
                    return false;
                }
            } else if ((u8)c < 0x20) {
                return false; /* control char must be escaped */
            } else {
                val += c;
            }
        }
        return false;
    }

    bool array(JsonValue &out)
    {
        out.type_ = JsonValue::ARR;
        p_++; /* [ */
        skip_ws();
        if (peek() == ']') {
            p_++;
            return true;
        }
        for (;;) {
            JsonValue v;
            if (!value(v))
                return false;
            out.arr_.push_back(v);
            skip_ws();
            char c = peek();
            if (c == ',') {
                p_++;
                continue;
            }
            if (c == ']') {
                p_++;
                return true;
            }
            return false;
        }
    }

    bool object(JsonValue &out)
    {
        out.type_ = JsonValue::OBJ;
        p_++; /* { */
        skip_ws();
        if (peek() == '}') {
            p_++;
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key))
                return false;
            skip_ws();
            if (peek() != ':')
                return false;
            p_++;
            JsonValue v;
            if (!value(v))
                return false;
            out.set(key, v); /* dict: a repeated key keeps its slot, last value wins */
            skip_ws();
            char c = peek();
            if (c == ',') {
                p_++;
                continue;
            }
            if (c == '}') {
                p_++;
                return true;
            }
            return false;
        }
    }
};

bool json_parse(const std::string &text, JsonValue &out)
{
    JsonParser parser(text);
    return parser.parse(out);
}
