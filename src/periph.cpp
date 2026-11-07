#include "periph.h"
#include "chips.h"
#include "emulator.h"
#include "beken_crypto.h" /* pyc::py_int: Python int() parsing */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

using pyc::py_int;

namespace periph {

int chip_pin_count(const std::string &chip)
{
    const Chip *found = find_chip(chip);
    return found ? found->gpio_pins : PIN_COUNT;
}

int gpio_slot(int pin, int pin_count)
{
    if (pin >= 0 && pin < PIN_COUNT)
        return pin;
    if (pin >= PIN_COUNT && pin < pin_count)
        return BK7252_EXT_SLOT + (pin - PIN_COUNT);
    return -1;
}

static std::map<int, std::string> PIN_ALIAS_MAP()
{
    std::map<int, std::string> m;
    m[0] = "TXD2"; m[1] = "RXD2"; m[11] = "TXD1"; m[10] = "RXD1";
    m[6] = "PWM0"; m[7] = "PWM1"; m[8] = "PWM2"; m[9] = "PWM3";
    m[24] = "PWM4/ADC2"; m[26] = "PWM5/ADC1";
    m[21] = "ADC6"; m[22] = "ADC5"; m[28] = "ADC4";
    return m;
}
static std::map<int, std::string> PIN_ROLE_MAP()
{
    std::map<int, std::string> m;
    m[0] = "UART2 TX (log)"; m[1] = "UART2 RX (log)";
    m[11] = "UART1 TX (TuyaMCU / programming)"; m[10] = "UART1 RX (TuyaMCU / programming)";
    m[6] = "PWM channel 0"; m[7] = "PWM channel 1"; m[8] = "PWM channel 2"; m[9] = "PWM channel 3";
    m[24] = "PWM channel 4 / ADC2"; m[26] = "PWM channel 5 / ADC1";
    return m;
}

std::string pin_alias(int pin)
{
    static std::map<int, std::string> m = PIN_ALIAS_MAP();
    std::map<int, std::string>::iterator it = m.find(pin);
    return it == m.end() ? std::string() : it->second;
}
std::string pin_role(int pin)
{
    static std::map<int, std::string> m = PIN_ROLE_MAP();
    std::map<int, std::string>::iterator it = m.find(pin);
    return it == m.end() ? std::string() : it->second;
}

std::string decode_pin(u32 value)
{
    bool second = (value & (1u << 6)) != 0;
    bool driving = !(value & (1u << 3));
    bool in_en = (value & (1u << 2)) != 0;
    int level = (value & (1u << 1)) ? 1 : 0;
    StrList bits;
    if (second)
        bits.push_back("second function");
    else if (driving)
        bits.push_back(strf("OUTPUT = %d", level));
    if (in_en)
        bits.push_back("input");
    if (value & (1u << 5))
        bits.push_back(strf("pull-%s", (value & (1u << 4)) ? "up" : "down"));
    if (value & (1u << 7))
        bits.push_back("monitor");
    if (bits.empty())
        bits.push_back("configured, no direction enabled");
    return join(bits, ", ");
}

static std::map<u32, std::string> KNOWN_MODES()
{
    std::map<u32, std::string> m;
    m[0x00] = "GMODE_OUTPUT"; m[0x0C] = "GMODE_INPUT"; m[0x2C] = "GMODE_INPUT_PULLDOWN";
    m[0x3C] = "GMODE_INPUT_PULLUP"; m[0x48] = "GMODE_SECOND_FUNC"; m[0x78] = "GMODE_SECOND_FUNC_PULL_UP";
    return m;
}
std::string mode_name(u32 value)
{
    static std::map<u32, std::string> km = KNOWN_MODES();
    if (km.count(value))
        return km[value];
    u32 base = value & ~(1u << 1);
    if (km.count(base))
        return strf("%s + output %d", km[base].c_str(), (value & (1u << 1)) ? 1 : 0);
    return "";
}

void parse_lines(const StrList &lines, std::map<int, u32> &gpio, std::map<u32, u32> &pwm)
{
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        if (startswith(line, "[EMU_GPIO]")) {
            StrList p = split_ws(line);
            if (p.size() >= 3) {
                long long pin, val;
                if (py_int(p[1], 10, pin) && py_int(p[2], 16, val))
                    gpio[(int)pin] = (u32)val;
            }
        } else if (startswith(line, "[EMU_PWM]")) {
            StrList p = split_ws(line);
            if (p.size() >= 3) {
                long long off, val;
                if (py_int(p[1], 16, off) && py_int(p[2], 16, val))
                    pwm[(u32)off] = (u32)val;
            }
        }
    }
}

void parse_i2c_lines(const StrList &lines, std::map<std::pair<int, int>, Bytes> &frames)
{
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &line = lines[i];
        if (!startswith(line, "[EMU_I2C]"))
            continue;
        StrList p = split_ws(line);
        if (p.size() >= 4) {
            long long scl, sda;
            if (py_int(p[1], 10, scl) && py_int(p[2], 10, sda)) {
                bool ok;
                Bytes b = unhex(p[3], &ok);
                if (ok)
                    frames[std::make_pair((int)scl, (int)sda)] = b;
            }
        }
    }
}

/* ------------------------------------------------------------------ GPIO */
static std::map<int, u32> gpio_pin_values(const std::map<int, u32> &gpio, const std::string &chip)
{
    std::map<int, u32> pins;
    for (std::map<int, u32>::const_iterator it = gpio.begin(); it != gpio.end(); ++it)
        if (it->first >= 0 && it->first < PIN_COUNT)
            pins[it->first] = it->second;
    int pin_count = chip_pin_count(chip);
    if (pin_count > PIN_COUNT) {
        for (int pin = PIN_COUNT; pin < pin_count; pin++) {
            int slot = gpio_slot(pin, pin_count);
            std::map<int, u32>::const_iterator it = gpio.find(slot);
            if (it != gpio.end())
                pins[pin] = it->second;
        }
    }
    return pins;
}

static std::vector<GpioRow> gpio_table(const std::map<int, u32> &gpio, int pin_count)
{
    std::vector<GpioRow> rows;
    for (int pin = 0; pin < pin_count; pin++) {
        GpioRow row;
        row.pin = pin;
        row.alias = pin_alias(pin);
        std::map<int, u32>::const_iterator it = gpio.find(pin);
        if (it != gpio.end()) {
            std::string detail = decode_pin(it->second);
            std::string role = pin_role(pin);
            size_t sf = detail.find("second function");
            if (!role.empty() && sf != std::string::npos)
                detail = replace_all(detail, "second function", role);
            row.state = "set by firmware";
            row.detail = detail;
        } else {
            row.state = "factory";
            row.detail = "not configured";
        }
        rows.push_back(row);
    }
    return rows;
}

static const char *FUNC_NAME(int i)
{
    if (i == 32)
        return "REG_GPIO_FUNC_CFG";
    if (i == 46)
        return "REG_GPIO_FUNC_CFG_2";
    if (i == 47)
        return "REG_GPIO_FUNC_CFG_3";
    return 0;
}
static std::vector<std::pair<std::string, std::string> > func_rows(const std::map<int, u32> &gpio)
{
    std::vector<std::pair<std::string, std::string> > rows;
    for (std::map<int, u32>::const_iterator it = gpio.begin(); it != gpio.end(); ++it) {
        const char *n = FUNC_NAME(it->first);
        if (n)
            rows.push_back(std::make_pair(std::string(n), strf("0x%08X", it->second)));
    }
    return rows;
}

/* ------------------------------------------------------------------- PWM */
static u32 infer_base(const std::map<u32, u32> &pwm)
{
    for (std::map<u32, u32>::const_iterator it = pwm.begin(); it != pwm.end(); ++it)
        if (it->first >= 0x80 && it->first < 0x100)
            return 0x80;
    return 0x00;
}

std::vector<int> ctl_channels(u32 ctl)
{
    std::vector<int> out;
    for (int n = 0; n < 6; n++)
        if (ctl & (1u << (4 * n)))
            out.push_back(n);
    return out;
}

static int PWM_PIN_CH(int pin)
{
    switch (pin) {
    case 6: return 0;
    case 7: return 1;
    case 8: return 2;
    case 9: return 3;
    case 24: return 4;
    case 26: return 5;
    default: return -1;
    }
}

/* ---------------------------------------------------------------- SARADC */
std::map<int, int> adc_channel_pins(const std::string &chip)
{
    std::map<int, int> m;
    std::string c = upper(pyc::py_strip(chip));
    if (c == "BK7238") {
        m[1] = 20; m[2] = 22; m[3] = 23; m[4] = 26; m[5] = 24; m[6] = 28;
    } else if (c == "BK7252N") {
        m[1] = 26; m[2] = 24; m[3] = 20; m[4] = 28; m[5] = 1; m[6] = 10; m[7] = 13;
    } else {
        m[1] = 26; m[2] = 24; m[3] = 23; m[4] = 28; m[5] = 22; m[6] = 21; m[7] = 13;
    }
    return m;
}

static int adc_fallback_pin_to_channel(int pin)
{
    switch (pin) {
    case 4: case 26: return 1;
    case 5: case 24: return 2;
    case 23: case 20: return 3;
    case 2: case 28: return 4;
    case 3: case 22: case 1: return 5;
    case 12: case 21: case 10: return 6;
    case 13: return 7;
    default: return -1;
    }
}

std::string adc_channel_label(int channel, const std::string &chip)
{
    if (channel == 0)
        return "ch0(VBAT)";
    std::string c = upper(pyc::py_strip(chip));
    std::map<int, int> pins = adc_channel_pins(c);
    std::map<int, int>::const_iterator it = pins.find(channel);
    bool has_pin = (it != pins.end());
    if (channel == 7 && (c == "BK7231N" || c == "BK7231M" || c == "BL2028N" || c == "BK7238"))
        return has_pin ? "ch7(TEMP/P13)" : "ch7(TEMP)";
    if (has_pin)
        return strf("ch%d(P%d)", channel, it->second);
    if (channel == 8)
        return "ch8(TEMP/TSSI)";
    if (channel == 9)
        return "ch9(TEMP)";
    return strf("ch%d", channel);
}

bool parse_adc_channel(const std::string &target, const std::string &chip, int &out, std::string *err)
{
    std::string text = pyc::py_strip(target);
    if (text.empty()) {
        if (err) *err = "empty ADC channel specification";
        return false;
    }
    StrList parts = split_ws(text);
    std::string head = lower(pyc::py_strip(parts[0]));
    size_t paren = head.find('(');
    if (paren != std::string::npos)
        head = pyc::py_strip(head.substr(0, paren));
    if (head == "vbat") {
        out = 0;
        return true;
    }
    if (head == "temp") {
        std::string c = upper(pyc::py_strip(chip));
        if (c == "BK7252") out = 8;
        else if (c == "BK7252N") out = 9;
        else out = 7;
        return true;
    }
    if (startswith(head, "p") || startswith(head, "gpio")) {
        std::string pin_s = startswith(head, "gpio") ? head.substr(4) : head.substr(1);
        long long pin_ll;
        if (!py_int(pin_s, 0, pin_ll)) {
            if (err) *err = "bad ADC GPIO pin " + pyc::py_repr(target) + " (want e.g. P23, P26)";
            return false;
        }
        int pin = (int)pin_ll;
        std::map<int, int> ch_pins = adc_channel_pins(chip);
        int found_ch = -1;
        for (std::map<int, int>::const_iterator it = ch_pins.begin(); it != ch_pins.end(); ++it)
            if (it->second == pin)
                found_ch = it->first;
        if (found_ch < 0)
            found_ch = adc_fallback_pin_to_channel(pin);
        if (found_ch < 0) {
            if (err) *err = strf("GPIO P%d is not an ADC input pad", pin);
            return false;
        }
        out = found_ch;
        return true;
    }
    if (startswith(head, "adc"))
        head = head.substr(3);
    else if (startswith(head, "ch"))
        head = head.substr(2);
    long long ch_ll;
    if (!py_int(head, 0, ch_ll)) {
        if (err)
            *err = "bad ADC channel " + pyc::py_repr(target) + " (want 0..15, ADC1..ADC7, P23, vbat, or temp)";
        return false;
    }
    if (ch_ll < 0 || ch_ll > 15) {
        if (err) *err = strf("ADC channel %lld out of range 0..15", ch_ll);
        return false;
    }
    out = (int)ch_ll;
    return true;
}

static bool parse_float_strict(const std::string &s, double &out)
{
    std::string t = pyc::py_strip(s);
    if (t.empty())
        return false;
    char *end = 0;
    out = strtod(t.c_str(), &end);
    return end && *end == '\0';
}

bool parse_adc_spec(const std::string &spec, const std::string &chip, int &ch, int &val, std::string *err)
{
    std::string text = pyc::py_strip(spec);
    size_t sep = text.find(':');
    if (sep == std::string::npos)
        sep = text.find('=');
    if (sep == std::string::npos) {
        if (err)
            *err = "bad --adc " + pyc::py_repr(spec) + " (want CH:VALUE or P<pin>:VALUE, e.g. 3:512 or P23:1.65V)";
        return false;
    }
    std::string lhs = text.substr(0, sep);
    std::string rhs = text.substr(sep + 1);
    if (!parse_adc_channel(lhs, chip, ch, err))
        return false;
    std::string rhs_s = pyc::py_strip(rhs);
    std::string rhs_low = lower(rhs_s);
    long long v_ll = 0;
    bool ok = false;
    if (rhs_low.size() >= 2 && rhs_low.substr(rhs_low.size() - 2) == "mv") {
        double mv = 0;
        if (parse_float_strict(rhs_s.substr(0, rhs_s.size() - 2), mv)) {
            v_ll = (long long)py_round(mv * 1023.0 / 3300.0);
            ok = true;
        }
    } else if (rhs_low.size() >= 1 && rhs_low[rhs_low.size() - 1] == 'v' && !startswith(rhs_low, "0x")) {
        double volts = 0;
        if (parse_float_strict(rhs_s.substr(0, rhs_s.size() - 1), volts)) {
            v_ll = (long long)py_round(volts * 1023.0 / 3.3);
            ok = true;
        }
    } else {
        ok = py_int(rhs_s, 0, v_ll);
    }
    if (!ok) {
        if (err)
            *err = "bad --adc value in " + pyc::py_repr(spec) + " (want integer 0..65535 or voltage like 1.65V / 1650mV)";
        return false;
    }
    if (v_ll < 0 || v_ll > 0xFFFF) {
        if (err)
            *err = strf("bad --adc value %lld in %s (must be 0..65535)", v_ll, pyc::py_repr(spec).c_str());
        return false;
    }
    val = (int)v_ll;
    return true;
}

bool parse_adc_specs(const StrList &specs, const std::string &chip, std::map<int, int> &out, std::string *err)
{
    out.clear();
    for (size_t i = 0; i < specs.size(); i++) {
        int ch = 0, val = 0;
        if (!parse_adc_spec(specs[i], chip, ch, val, err))
            return false;
        out[ch] = val;
    }
    return true;
}

AdcInfo describe_adc(int saradc_channel, int saradc_conversions, int saradc_reads,
                     const std::map<int, int> *adc_samples, const std::map<int, int> *adc_inputs,
                     int last_raw, const std::string &chip)
{
    AdcInfo info;
    info.conversions = saradc_conversions;
    info.reads = saradc_reads;
    if (adc_inputs) {
        for (std::map<int, int>::const_iterator it = adc_inputs->begin(); it != adc_inputs->end(); ++it)
            info.samples[it->first] = it->second;
    }
    if (adc_samples) {
        for (std::map<int, int>::const_iterator it = adc_samples->begin(); it != adc_samples->end(); ++it)
            info.samples[it->first] = it->second;
    }
    info.active_channel = saradc_channel;
    info.last_raw = last_raw;
    info.active = (saradc_channel >= 0 || saradc_conversions > 0 || saradc_reads > 0);
    if (!info.active && info.samples.empty()) {
        info.summary = strf("ADC: idle (default %d / %.2fV)", DEFAULT_ADC_RAW, DEFAULT_ADC_RAW * 3.3 / 1023.0);
        return info;
    }
    StrList parts;
    if (saradc_channel >= 0) {
        int raw = last_raw >= 0 ? last_raw
                  : (info.samples.count(saradc_channel) ? info.samples[saradc_channel] : DEFAULT_ADC_RAW);
        double volts = raw * 3.3 / 1023.0;
        parts.push_back(strf("%s=%d (%.2fV)", adc_channel_label(saradc_channel, chip).c_str(), raw, volts));
    }
    StrList other_samples;
    for (std::map<int, int>::const_iterator it = info.samples.begin(); it != info.samples.end(); ++it) {
        if (it->first != saradc_channel) {
            other_samples.push_back(strf("%s=%d (%.2fV)", adc_channel_label(it->first, chip).c_str(),
                                         it->second, it->second * 3.3 / 1023.0));
        }
    }
    if (!other_samples.empty())
        parts.push_back(join(other_samples, ", "));
    if (saradc_conversions > 0 || saradc_reads > 0)
        parts.push_back(strf("%d conv, %d reads", saradc_conversions, saradc_reads));
    info.summary = "ADC: " + join(parts, " \xe2\x80\x94 ");
    return info;
}

std::vector<int> pwm_output_pins(const std::map<int, u32> &gpio)
{
    std::vector<int> out;
    for (std::map<int, u32>::const_iterator it = gpio.begin(); it != gpio.end(); ++it)
        if (PWM_PIN_CH(it->first) >= 0 && (it->second & (1u << 6)))
            out.push_back(it->first);
    std::sort(out.begin(), out.end());
    return out;
}

static u32 pwm_get(const std::map<u32, u32> &pwm, u32 off, bool &has)
{
    std::map<u32, u32>::const_iterator it = pwm.find(off);
    has = it != pwm.end();
    return has ? it->second : 0;
}

static std::vector<PwmChan> pwm_new_channels(const std::map<u32, u32> &pwm)
{
    std::vector<PwmChan> rows;
    const u32 OFF = 0x100;
    for (int ch = 0; ch < 6; ch++) {
        int g = ch / 2, s = ch % 2;
        u32 base = OFF + 0x40 * (u32)g;
        bool has;
        u32 ctrl = pwm_get(pwm, base, has);
        if (!has)
            continue;
        if (!((ctrl >> (8 * s + 3)) & 1))
            continue;
        if (((ctrl >> (8 * s)) & 0x7) != 1)
            continue;
        u32 toff = base + 0x04 + 0x10 * (u32)s;
        u32 t[4];
        for (int i = 0; i < 4; i++) {
            bool h;
            t[i] = pwm_get(pwm, toff + 4 * (u32)i, h);
        }
        u32 period = t[3];
        if (!period)
            continue;
        u32 pre = (ctrl >> 16) & 0xFF;
        double freq = PWM_CLOCK_HZ / ((pre < 1 ? 1 : pre) * (double)period);
        if (!(freq >= PWM_FREQ_MIN_HZ && freq <= PWM_FREQ_MAX_HZ))
            continue;
        int level = (ctrl >> (8 * s + 6)) & 1;
        u32 high = 0, prev = 0;
        std::vector<u32> togs;
        for (int i = 0; i < 3; i++)
            if (t[i] > 0 && t[i] < period)
                togs.push_back(t[i]);
        std::sort(togs.begin(), togs.end());
        for (size_t i = 0; i < togs.size(); i++) {
            high += (togs[i] - prev) * (u32)level;
            level ^= 1;
            prev = togs[i];
        }
        high += (period - prev) * (u32)level;
        PwmChan pc;
        pc.ch = ch;
        pc.period = period;
        pc.duty = high;
        pc.has_duty = true;
        pc.freq = freq;
        pc.pct = 100.0 * high / period;
        pc.has_pct = true;
        rows.push_back(pc);
    }
    return rows;
}

static std::vector<PwmChan> pwm_channels_old(const std::map<u32, u32> &pwm, u32 &ctl_out, u32 &base_out)
{
    std::vector<PwmChan> rows;
    u32 base = infer_base(pwm);
    base_out = base;
    bool h;
    ctl_out = pwm_get(pwm, base, h);
    if (!base) {
        base_out = 0;
        ctl_out = pwm_get(pwm, 0, h);
        return rows;
    }
    for (int ch = 0; ch < 6; ch++) {
        bool hp, hd;
        u32 period = pwm_get(pwm, base + 0x08 + 12 * (u32)ch, hp);
        u32 duty = pwm_get(pwm, base + 0x0C + 12 * (u32)ch, hd);
        if (!period)
            continue;
        double freq = PWM_CLOCK_HZ / (double)period;
        if (!(freq >= PWM_FREQ_MIN_HZ && freq <= PWM_FREQ_MAX_HZ))
            continue;
        PwmChan pc;
        pc.ch = ch;
        pc.period = period;
        pc.duty = duty;
        pc.has_duty = hd;
        pc.freq = freq;
        pc.has_pct = hd;
        pc.pct = hd ? (100.0 * duty / period) : -1;
        rows.push_back(pc);
    }
    return rows;
}

static std::vector<std::pair<std::string, std::string> > pwm_registers(const std::map<u32, u32> &pwm)
{
    std::vector<std::pair<std::string, std::string> > out;
    for (std::map<u32, u32>::const_iterator it = pwm.begin(); it != pwm.end(); ++it)
        out.push_back(std::make_pair(strf("+0x%02X", it->first), strf("0x%08X", it->second)));
    return out;
}

/* --------------------------------------------------------------- colours */
static double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

static const char *LED_OFF_HEX = "#181C22";

bool rgbcw_to_hex(double r, double g, double b, double c, double w, std::string &hex)
{
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);
    c = clamp01(c);
    w = clamp01(w);
    double level = r;
    if (g > level) level = g;
    if (b > level) level = b;
    if (c > level) level = c;
    if (w > level) level = w;
    if (level <= 0.001) {
        hex = LED_OFF_HEX;
        return false;
    }
    double lin_r = r + 0.90 * c + 1.00 * w;
    double lin_g = g + 0.96 * c + 0.78 * w;
    double lin_b = b + 1.00 * c + 0.42 * w;
    double peak = lin_r;
    if (lin_g > peak) peak = lin_g;
    if (lin_b > peak) peak = lin_b;
    if (1.0 > peak) peak = 1.0;
    lin_r /= peak;
    lin_g /= peak;
    lin_b /= peak;
    double bright = 0.25 + 0.75 * pow(level, 0.65);
    int out_r = (int)py_round(clamp01(lin_r * bright) * 255);
    int out_g = (int)py_round(clamp01(lin_g * bright) * 255);
    int out_b = (int)py_round(clamp01(lin_b * bright) * 255);
    hex = strf("#%02X%02X%02X", out_r, out_g, out_b);
    return true;
}

LedDecode decode_i2c_led(int scl, int sda, const Bytes &raw, const std::map<std::string, int> *cmap)
{
    LedDecode d;
    Bytes b = raw;
    if (b.size() < 2)
        return d;
    u8 b0 = (u8)b[0];
    std::string driver;
    int max_val = 1023;
    std::string cur_info;
    int outs[5] = {0, 0, 0, 0, 0};

    if ((b0 & 0xC0) == 0x80 && (b.size() == 17 || b0 == 0x80)) {
        driver = "BP5758D";
        max_val = 1023;
        if (b0 == 0x80 || b.size() < 17)
            cur_info = "sleep";
        else {
            int en = (u8)b[1] & 0x1F;
            StrList curs;
            for (int i = 2; i < 7; i++) {
                int cb = (u8)b[i];
                int ma = (cb & 0x40) ? ((cb & 0x3F) + 30) : (cb & 0x3F);
                curs.push_back(strf("%d", ma));
            }
            cur_info = join(curs, "/") + " mA";
            for (int i = 0; i < 5; i++)
                if (en & (1 << i)) {
                    int lo = (u8)b[7 + 2 * i] & 0x1F;
                    int hi = (u8)b[8 + 2 * i] & 0x1F;
                    outs[i] = lo | (hi << 5);
                }
        }
    } else if (((b0 & 0xF0) == 0xD0 || (b0 & 0xF0) == 0xE0) && (b0 & 0x07) == 0 && b.size() == 12) {
        driver = "SM2235";
        cur_info = strf("I=0x%02X", (u8)b[1]);
        for (int i = 0; i < 5; i++)
            outs[i] = (((u8)b[2 + 2 * i] & 0x03) << 8) | (u8)b[3 + 2 * i];
    } else if ((b0 & 0xF0) == 0xB0 && (b0 & 0x07) == 0 && b.size() == 12) {
        driver = "BP1658CJ";
        cur_info = strf("I=0x%02X", (u8)b[1]);
        for (int i = 0; i < 5; i++) {
            int lo = (u8)b[2 + 2 * i] & 0x1F;
            int hi = (u8)b[3 + 2 * i] & 0x1F;
            outs[i] = lo | (hi << 5);
        }
    } else if ((b0 & 0xF8) == 0xC0 && b.size() >= 3 && b.size() <= 8) {
        driver = "SM2135";
        max_val = 255;
        int start_reg = b0 & 0x07;
        std::map<int, int> regs;
        for (size_t i = 1; i < b.size(); i++)
            regs[start_reg + (int)(i - 1)] = (u8)b[i];
        if (regs.count(0))
            cur_info = strf("I=0x%02X", regs[0]);
        for (int i = 0; i < 5; i++)
            outs[i] = regs.count(2 + i) ? regs[2 + i] : 0;
    } else {
        return d;
    }

    std::map<std::string, int> default_map;
    default_map["r"] = 0;
    default_map["g"] = 1;
    default_map["b"] = 2;
    default_map["c"] = 3;
    default_map["w"] = 4;
    const std::map<std::string, int> &m = cmap ? *cmap : default_map;
    double ch[5] = {0, 0, 0, 0, 0};
    const char *names[5] = {"r", "g", "b", "c", "w"};
    double chfrac[5];
    for (int i = 0; i < 5; i++) {
        std::map<std::string, int>::const_iterator it = m.find(names[i]);
        double frac = 0.0;
        if (it != m.end() && it->second >= 0 && it->second < 5)
            frac = clamp01(outs[it->second] / (double)max_val);
        chfrac[i] = frac;
        (void)ch;
    }
    std::string hex;
    bool lit = rgbcw_to_hex(chfrac[0], chfrac[1], chfrac[2], chfrac[3], chfrac[4], hex);
    d.valid = true;
    d.driver = driver;
    d.scl = scl;
    d.sda = sda;
    for (int i = 0; i < 5; i++)
        d.raw_channels[i] = outs[i];
    d.max_val = max_val;
    d.cur_info = cur_info;
    d.r = chfrac[0];
    d.g = chfrac[1];
    d.b = chfrac[2];
    d.c = chfrac[3];
    d.w = chfrac[4];
    d.hex = hex;
    d.is_lit = lit;
    d.raw_hex = hexstr_sep(b, " ");
    return d;
}

std::vector<std::pair<int, std::pair<int, int> > > decode_ws2812_spi(const Bytes &buf)
{
    std::vector<std::pair<int, std::pair<int, int> > > pixels;
    if (buf.size() < 24)
        return pixels;
    size_t n = buf.size() / 24;
    for (size_t i = 0; i < n; i++) {
        u32 val = 0;
        for (int k = 0; k < 24; k++) {
            u8 byte = (u8)buf[i * 24 + k];
            int ones = 0;
            for (int bit = 0; bit < 8; bit++)
                if (byte & (1 << bit))
                    ones++;
            int b = ones >= 4 ? 1 : 0;
            val = (val << 1) | (u32)b;
        }
        int g = (val >> 16) & 0xFF;
        int r = (val >> 8) & 0xFF;
        int bl = val & 0xFF;
        pixels.push_back(std::make_pair(r, std::make_pair(g, bl)));
    }
    return pixels;
}

/* ------------------------------------------------- Tuya dmod names */
static std::string dmod_name(const std::string &v)
{
    if (v == "0") return "PWM";
    if (v == "1") return "SM16726B";
    if (v == "2") return "SM2135";
    if (v == "3") return "SM2135EH";
    if (v == "4") return "SM2135EJ";
    if (v == "5") return "BP1658CJ";
    if (v == "6") return "BP5758D";
    if (v == "7") return "SM2235";
    return "";
}

/* _tuya_pairs_dict: dict(pairs), so a repeated key keeps its LAST value */
static std::string pair_get(const Pairs *pairs, const std::string &key, bool &found)
{
    found = false;
    std::string v;
    if (!pairs)
        return v;
    for (size_t i = 0; i < pairs->size(); i++)
        if ((*pairs)[i].first == key) {
            found = true;
            v = (*pairs)[i].second;
        }
    return v;
}

/* int(pairs[key], 0); a value past the int range is clamped (it is out of
 * every pin / channel range all the same) */
static int pair_int(const Pairs *pairs, const std::string &key, bool &ok)
{
    bool f;
    std::string v = pair_get(pairs, key, f);
    ok = false;
    if (!f)
        return 0;
    long long n;
    if (py_int(v, 0, n)) {
        ok = true;
        return (int)(n > 0x7FFFFFFFLL ? 0x7FFFFFFFLL : n < -0x7FFFFFFFLL ? -0x7FFFFFFFLL : n);
    }
    return 0;
}

static std::map<std::string, int> tuya_i2c_channel_map(const Pairs *pairs, bool &have)
{
    std::map<std::string, int> cmap;
    const char *chs[5] = {"r", "g", "b", "c", "w"};
    for (int i = 0; i < 5; i++) {
        bool ok;
        int v = pair_int(pairs, std::string("iic") + chs[i], ok);
        if (ok)
            cmap[chs[i]] = v;
    }
    have = cmap.size() >= 3;
    return cmap;
}

static void describe_peripherals(const Pairs *pairs, DecodedState &out,
                                 const std::map<std::pair<int, int>, Bytes> *i2c_frames,
                                 const std::map<std::pair<int, int>, Bytes> *i2c_last_on,
                                 const Bytes *ws2812_spi, int bl0942_polls,
                                 const double *meter_values,
                                 int saradc_channel, int saradc_conversions, int saradc_reads,
                                 const std::map<int, int> *adc_samples, int saradc_last_raw,
                                 const std::map<int, int> *adc_inputs, const std::string &chip)
{
    bool have_cmap;
    std::map<std::string, int> cmap = tuya_i2c_channel_map(pairs, have_cmap);
    const std::map<std::string, int> *cmapp = have_cmap ? &cmap : 0;

    /* LED */
    LedDecode best_i2c;
    LedDecode best_on;
    if (i2c_frames) {
        for (std::map<std::pair<int, int>, Bytes>::const_iterator it = i2c_frames->begin();
             it != i2c_frames->end(); ++it) {
            LedDecode d = decode_i2c_led(it->first.first, it->first.second, it->second, cmapp);
            if (d.valid) {
                best_i2c = d;
                if (i2c_last_on) {
                    std::map<std::pair<int, int>, Bytes>::const_iterator it2 = i2c_last_on->find(it->first);
                    if (it2 != i2c_last_on->end())
                        best_on = decode_i2c_led(it->first.first, it->first.second, it2->second, cmapp);
                }
                break;
            }
        }
    }
    std::vector<std::pair<int, std::pair<int, int> > > ws_pixels;
    if (ws2812_spi)
        ws_pixels = decode_ws2812_spi(*ws2812_spi);

    std::map<int, double> pwm_by_ch;
    for (size_t i = 0; i < out.pwm_channels.size(); i++)
        if (out.pwm_channels[i].has_pct)
            pwm_by_ch[out.pwm_channels[i].ch] = out.pwm_channels[i].pct;

    std::map<std::string, int> pwm_pin_map;
    const char *chn[5] = {"r", "g", "b", "c", "w"};
    for (int i = 0; i < 5; i++) {
        bool ok;
        int p = pair_int(pairs, std::string(chn[i]) + "_pin", ok);
        if (ok && PWM_PIN_CH(p) >= 0)
            pwm_pin_map[chn[i]] = PWM_PIN_CH(p);
    }

    if (best_i2c.valid) {
        bool blinking_off = (!best_i2c.is_lit) && (best_on.valid && best_on.is_lit);
        LedDecode &shown = blinking_off ? best_on : best_i2c;
        bool f;
        std::string dmodv = pair_get(pairs, "dmod", f);
        std::string dmodn = f && !dmod_name(dmodv).empty() ? dmod_name(dmodv) : shown.driver;
        std::string drv_label;
        if ((dmodn == "SM2235" || dmodn == "SM2135EJ" || dmodn == "SM2135EH") && shown.driver == "SM2235")
            drv_label = dmodn;
        else
            drv_label = shown.driver;
        std::string cur_part = shown.cur_info.empty() ? "" : (", " + shown.cur_info);
        std::string pcts = strf("R:%.0f%% G:%.0f%% B:%.0f%% C:%.0f%% W:%.0f%%",
                                shown.r * 100, shown.g * 100, shown.b * 100, shown.c * 100, shown.w * 100);
        if (blinking_off)
            pcts += " (blink paused/off)";
        out.led.present = true;
        out.led.kind = drv_label;
        out.led.summary = strf("%s (SCL=P%d SDA=P%d%s)  %s  %s", drv_label.c_str(), shown.scl, shown.sda,
                               cur_part.c_str(), pcts.c_str(), shown.hex.c_str());
        out.led.hex = shown.hex;
        out.led.live_hex = best_i2c.hex;
        out.led.is_lit = shown.is_lit;
        out.led.pixels.push_back(shown.hex);
    } else if (!ws_pixels.empty()) {
        double r_avg = 0, g_avg = 0, b_avg = 0;
        for (size_t i = 0; i < ws_pixels.size(); i++) {
            r_avg += ws_pixels[i].first;
            g_avg += ws_pixels[i].second.first;
            b_avg += ws_pixels[i].second.second;
        }
        double n = 255.0 * ws_pixels.size();
        r_avg /= n;
        g_avg /= n;
        b_avg /= n;
        StrList hex_pixels;
        for (size_t i = 0; i < ws_pixels.size(); i++) {
            std::string hx;
            rgbcw_to_hex(ws_pixels[i].first / 255.0, ws_pixels[i].second.first / 255.0,
                         ws_pixels[i].second.second / 255.0, 0, 0, hx);
            hex_pixels.push_back(hx);
        }
        double c_val = pwm_pin_map.count("c") && pwm_by_ch.count(pwm_pin_map["c"]) ? pwm_by_ch[pwm_pin_map["c"]] / 100.0 : 0.0;
        double w_val = pwm_pin_map.count("w") && pwm_by_ch.count(pwm_pin_map["w"]) ? pwm_by_ch[pwm_pin_map["w"]] / 100.0 : 0.0;
        std::string hex_color;
        bool lit = rgbcw_to_hex(r_avg, g_avg, b_avg, c_val, w_val, hex_color);
        bool f;
        std::string mosi = pair_get(pairs, "mosi", f);
        if (!f)
            mosi = "16";
        std::string extra_pwm;
        if (pwm_pin_map.count("c") || pwm_pin_map.count("w"))
            extra_pwm = strf(" + PWM C:%.0f%% W:%.0f%%", c_val * 100, w_val * 100);
        out.led.present = true;
        out.led.kind = "WS2812 SPI";
        out.led.summary = strf("WS2812 SPI (MOSI=P%s, %d LEDs)%s  R:%.0f%% G:%.0f%% B:%.0f%%  %s",
                               mosi.c_str(), (int)ws_pixels.size(), extra_pwm.c_str(),
                               r_avg * 100, g_avg * 100, b_avg * 100, hex_color.c_str());
        out.led.hex = hex_color;
        out.led.live_hex = hex_color;
        out.led.is_lit = lit;
        out.led.pixels = hex_pixels;
    } else {
        bool fscl, fsda;
        pair_get(pairs, "iicscl", fscl);
        pair_get(pairs, "iicsda", fsda);
        bool fmosi, fled;
        pair_get(pairs, "mosi", fmosi);
        pair_get(pairs, "LedNum", fled);
        if (fscl && fsda) {
            bool f;
            std::string dmodv = pair_get(pairs, "dmod", f);
            std::string drv_label = f && !dmod_name(dmodv).empty() ? dmod_name(dmodv) : "I2C LED";
            std::string scl_s = pair_get(pairs, "iicscl", f);
            std::string sda_s = pair_get(pairs, "iicsda", f);
            /* the configured drive currents, as "key=value" notes */
            const char *cur_keys[] = {"2135ccur", "2135wcur", "2235ccur", "2235wcur", "5758ccur", "5758wcur",
                                      "rgbcur", "wcur", 0};
            StrList cur_notes;
            for (int k = 0; cur_keys[k]; k++) {
                bool fk;
                std::string v = pair_get(pairs, cur_keys[k], fk);
                if (fk)
                    cur_notes.push_back(std::string(cur_keys[k]) + "=" + v);
            }
            std::string cur_str = cur_notes.empty() ? "" : ", " + join(cur_notes, " ");
            out.led.present = true;
            out.led.kind = drv_label;
            out.led.summary = strf("%s (SCL=P%s SDA=P%s%s) \xe2\x80\x94 waiting for I2C frames",
                                   drv_label.c_str(), scl_s.c_str(), sda_s.c_str(), cur_str.c_str());
            out.led.hex = LED_OFF_HEX;
            out.led.live_hex = LED_OFF_HEX;
            out.led.is_lit = false;
            out.led.pixels.push_back(LED_OFF_HEX);
        } else if (fmosi || fled) {
            bool f;
            std::string mosi = pair_get(pairs, "mosi", f);
            if (!f) mosi = "16";
            std::string led_num = pair_get(pairs, "LedNum", f);
            if (!f) led_num = "?";
            double c_val = pwm_pin_map.count("c") && pwm_by_ch.count(pwm_pin_map["c"]) ? pwm_by_ch[pwm_pin_map["c"]] / 100.0 : 0.0;
            double w_val = pwm_pin_map.count("w") && pwm_by_ch.count(pwm_pin_map["w"]) ? pwm_by_ch[pwm_pin_map["w"]] / 100.0 : 0.0;
            std::string hex_color;
            bool lit = rgbcw_to_hex(0, 0, 0, c_val, w_val, hex_color);
            std::string cw_note = (c_val > 0 || w_val > 0)
                                  ? strf("  PWM C:%.0f%% W:%.0f%%  %s", c_val * 100, w_val * 100, hex_color.c_str())
                                  : " \xe2\x80\x94 waiting for SPI/PWM output";
            out.led.present = true;
            out.led.kind = "WS2812 SPI";
            out.led.summary = strf("WS2812 SPI (MOSI=P%s, %s LEDs)%s", mosi.c_str(), led_num.c_str(), cw_note.c_str());
            out.led.hex = hex_color;
            out.led.live_hex = hex_color;
            out.led.is_lit = lit;
            out.led.pixels.push_back(hex_color);
        } else if (!pwm_pin_map.empty() || !out.pwm_channels.empty()) {
            if (!pwm_pin_map.empty()) {
                double rv = pwm_pin_map.count("r") && pwm_by_ch.count(pwm_pin_map["r"]) ? pwm_by_ch[pwm_pin_map["r"]] / 100.0 : 0.0;
                double gv = pwm_pin_map.count("g") && pwm_by_ch.count(pwm_pin_map["g"]) ? pwm_by_ch[pwm_pin_map["g"]] / 100.0 : 0.0;
                double bv = pwm_pin_map.count("b") && pwm_by_ch.count(pwm_pin_map["b"]) ? pwm_by_ch[pwm_pin_map["b"]] / 100.0 : 0.0;
                double cv = pwm_pin_map.count("c") && pwm_by_ch.count(pwm_pin_map["c"]) ? pwm_by_ch[pwm_pin_map["c"]] / 100.0 : 0.0;
                double wv = pwm_pin_map.count("w") && pwm_by_ch.count(pwm_pin_map["w"]) ? pwm_by_ch[pwm_pin_map["w"]] / 100.0 : 0.0;
                StrList pin_desc;
                for (int i = 0; i < 5; i++) {
                    bool f;
                    std::string pv = pair_get(pairs, std::string(chn[i]) + "_pin", f);
                    if (f)
                        pin_desc.push_back(strf("%c=P%s", (char)(chn[i][0] - 32), pv.c_str()));
                }
                std::string hex_color;
                bool lit = rgbcw_to_hex(rv, gv, bv, cv, wv, hex_color);
                out.led.present = true;
                out.led.kind = "PWM Light";
                out.led.summary = strf("PWM Light (%s)  R:%.0f%% G:%.0f%% B:%.0f%% C:%.0f%% W:%.0f%%  %s",
                                       join(pin_desc, " ").c_str(), rv * 100, gv * 100, bv * 100, cv * 100, wv * 100,
                                       hex_color.c_str());
                out.led.hex = hex_color;
                out.led.live_hex = hex_color;
                out.led.is_lit = lit;
                out.led.pixels.push_back(hex_color);
            } else {
                double rv = 0, gv = 0, bv = 0, cv = 0, wv = 0;
                size_t nc = out.pwm_channels.size();
                if (nc == 1)
                    wv = out.pwm_channels[0].has_pct ? out.pwm_channels[0].pct / 100.0 : 0.0;
                else if (nc == 2) {
                    cv = out.pwm_channels[0].has_pct ? out.pwm_channels[0].pct / 100.0 : 0.0;
                    wv = out.pwm_channels[1].has_pct ? out.pwm_channels[1].pct / 100.0 : 0.0;
                } else {
                    rv = pwm_by_ch.count(0) ? pwm_by_ch[0] / 100.0 : 0.0;
                    gv = pwm_by_ch.count(1) ? pwm_by_ch[1] / 100.0 : 0.0;
                    bv = pwm_by_ch.count(2) ? pwm_by_ch[2] / 100.0 : 0.0;
                    cv = pwm_by_ch.count(3) ? pwm_by_ch[3] / 100.0 : 0.0;
                    wv = pwm_by_ch.count(4) ? pwm_by_ch[4] / 100.0 : 0.0;
                }
                std::string hex_color;
                bool lit = rgbcw_to_hex(rv, gv, bv, cv, wv, hex_color);
                StrList ch_str;
                for (size_t i = 0; i < out.pwm_channels.size(); i++)
                    ch_str.push_back(strf("ch%d:%.0f%%", out.pwm_channels[i].ch,
                                          out.pwm_channels[i].has_pct ? out.pwm_channels[i].pct : 0.0));
                out.led.present = true;
                out.led.kind = "PWM Output";
                out.led.summary = strf("PWM Output (%s)  %s", join(ch_str, " ").c_str(), hex_color.c_str());
                out.led.hex = hex_color;
                out.led.live_hex = hex_color;
                out.led.is_lit = lit;
                out.led.pixels.push_back(hex_color);
            }
        }
    }

    /* Meter */
    double v_val = 230.0, a_val = 1.5, w_val = 345.0;
    if (meter_values) {
        v_val = meter_values[0];
        a_val = meter_values[1];
        w_val = meter_values[2];
    }
    bool f_ele, f_vi;
    pair_get(pairs, "ele_pin", f_ele);
    pair_get(pairs, "vi_pin", f_vi);
    if (bl0942_polls > 0) {
        out.meter.present = true;
        out.meter.kind = "BL0942 (UART1)";
        out.meter.summary = strf("BL0942 UART1 (%d polls) \xe2\x80\x94 %.1fV  %.2fA  %.1fW",
                                 bl0942_polls, v_val, a_val, w_val);
    } else if (f_ele || f_vi) {
        bool okc, okc1, oks; /* _pin_int: None when missing or not an int */
        int cf = pair_int(pairs, "ele_pin", okc);
        int cf1 = pair_int(pairs, "vi_pin", okc1);
        int sel = pair_int(pairs, "sel_pin_pin", oks);
        bool okl;
        int sel_lv = pair_int(pairs, "sel_pin_lv", okl);
        bool fe, fv, fs;
        std::string ele_s = pair_get(pairs, "ele_pin", fe);
        std::string vi_s = pair_get(pairs, "vi_pin", fv);
        std::string sel_s = pair_get(pairs, "sel_pin_pin", fs);
        out.meter.present = true;
        out.meter.kind = "BL0937 (Pulse)";
        out.meter.summary = strf("BL0937 Pulse (CF=P%s CF1=P%s SEL=P%s) \xe2\x80\x94 %.1fV  %.2fA  %.1fW",
                                 fe ? ele_s.c_str() : "?", fv ? vi_s.c_str() : "?", fs ? sel_s.c_str() : "?",
                                 v_val, a_val, w_val);
        out.meter.cf_pin = okc ? cf : -1;
        out.meter.cf1_pin = okc1 ? cf1 : -1;
        out.meter.sel_pin = oks ? sel : -1;
        out.meter.sel_lv = okl ? sel_lv : 1;
    }
    out.adc = describe_adc(saradc_channel, saradc_conversions, saradc_reads,
                           adc_samples, adc_inputs, saradc_last_raw, chip);
}

/* describe_peripherals(tuya_config=..., meter_values=...) with no live state,
 * for the GUI's before-a-run LED/meter/ADC summary. */
DecodedState describe_peripherals(const Pairs *pairs, const double *meter_values,
                                  int saradc_channel, int saradc_conversions, int saradc_reads,
                                  const std::map<int, int> *adc_samples, int saradc_last_raw,
                                  const std::map<int, int> *adc_inputs, const std::string &chip)
{
    DecodedState out;
    describe_peripherals(pairs, out, 0, 0, 0, 0, meter_values,
                         saradc_channel, saradc_conversions, saradc_reads,
                         adc_samples, saradc_last_raw, adc_inputs, chip);
    return out;
}

static const char *PWM_NEW_CHIPS[] = {"BK7231N", "BK7231M", "BL2028N", "BK7238", 0};
static bool is_pwm_new_chip(const std::string &chip)
{
    for (int i = 0; PWM_NEW_CHIPS[i]; i++)
        if (chip == PWM_NEW_CHIPS[i])
            return true;
    return false;
}

DecodedState decode_state(const std::map<int, u32> &gpio, const std::map<u32, u32> &pwm,
                          const std::string &chip,
                          const std::map<std::pair<int, int>, Bytes> *i2c_frames,
                          const std::map<std::pair<int, int>, Bytes> *i2c_last_on,
                          const Bytes *ws2812_spi,
                          const JsonValue * /*unused*/,
                          const Pairs *pairs,
                          int bl0942_polls,
                          const double *meter_values,
                          int saradc_channel,
                          int saradc_conversions,
                          int saradc_reads,
                          const std::map<int, int> *adc_samples,
                          int saradc_last_raw,
                          const std::map<int, int> *adc_inputs)
{
    DecodedState out;
    bool any_i2c = i2c_frames && !i2c_frames->empty();
    bool any_ws = ws2812_spi && !ws2812_spi->empty();
    if (gpio.empty() && pwm.empty() && !any_i2c && !any_ws && bl0942_polls == 0 && saradc_conversions == 0)
        return out;
    out.present = true;

    bool new_layout = false;
    if (is_pwm_new_chip(chip)) {
        for (std::map<u32, u32>::const_iterator it = pwm.begin(); it != pwm.end(); ++it)
            if (it->first >= 0x100) {
                new_layout = true;
                break;
            }
    }
    if (new_layout) {
        out.pwm_channels = pwm_new_channels(pwm);
        out.pwm_ctl = 0;
        out.pwm_base = 0xFFFFFFFFu;
        out.pwm_layout = "new";
    } else {
        u32 ctl, base;
        out.pwm_channels = pwm_channels_old(pwm, ctl, base);
        out.pwm_ctl = ctl;
        out.pwm_base = base;
        out.pwm_layout = "old";
    }
    std::map<int, u32> logical = gpio_pin_values(gpio, chip);
    int pin_count = chip_pin_count(chip);
    out.gpio = gpio_table(logical, pin_count);
    out.raw = logical;
    out.pwm_pins = pwm_output_pins(gpio);
    out.func = func_rows(gpio);
    out.pwm_regs = pwm_registers(pwm);
    describe_peripherals(pairs, out, i2c_frames, i2c_last_on, ws2812_spi, bl0942_polls, meter_values,
                         saradc_channel, saradc_conversions, saradc_reads,
                         adc_samples, saradc_last_raw, adc_inputs, chip);
    return out;
}

std::string chip_from_name(const std::string &filename)
{
    std::string base = upper(path_basename(filename));
    const char *chips[] = {"BL2028N", "BK7252N", "BK7231T", "BK7231U", "BK7231N", "BK7231M",
                           "BK7231Q", "BK7238", "BK7252", "BK7236", "BK7258", "BK3231", 0};
    size_t best_pos = std::string::npos;
    std::string best;
    for (int i = 0; chips[i]; i++) {
        size_t p = base.find(chips[i]);
        if (p != std::string::npos && (best_pos == std::string::npos || p < best_pos)) {
            best_pos = p;
            best = chips[i];
        }
    }
    return best;
}

#define PER_CHECK(cond, msg)                                  \
    do {                                                      \
        checks++;                                             \
        if (!(cond)) {                                        \
            printf("  [FAIL] periph: %s\n", (msg));           \
            return -1;                                        \
        }                                                     \
    } while (0)

int periph_selftests()
{
    int checks = 0;

    /* 1. GPIO slot & pin decoding */
    PER_CHECK(gpio_slot(10, 40) == 10, "gpio_slot(10, 40) == 10");
    PER_CHECK(gpio_slot(32, 40) == 48, "gpio_slot(32, 40) == 48");
    PER_CHECK(gpio_slot(39, 40) == 55, "gpio_slot(39, 40) == 55");
    PER_CHECK(gpio_slot(40, 40) == -1, "gpio_slot(40, 40) == -1");
    PER_CHECK(gpio_slot(32, 32) == -1, "gpio_slot(32, 32) == -1");
    PER_CHECK(contains(decode_pin(0x00), "OUTPUT = 0"), "decode_pin(0x00)");
    PER_CHECK(contains(decode_pin(0x02), "OUTPUT = 1"), "decode_pin(0x02)");
    PER_CHECK(contains(decode_pin(0x3C), "pull-up"), "decode_pin(0x3C)");
    PER_CHECK(contains(decode_pin(0x2C), "pull-down"), "decode_pin(0x2C)");
    PER_CHECK(contains(decode_pin(0x48), "second function"), "decode_pin(0x48)");

    /* 2. PWM old & pwm_new decoders */
    {
        std::map<u32, u32> pwm;
        pwm[0x80] = 0x11;
        pwm[0x88] = 26000;
        pwm[0x8C] = 13000;
        u32 ctl = 0, base = 0;
        std::vector<PwmChan> old_ch = pwm_channels_old(pwm, ctl, base);
        PER_CHECK(base == 0x80 && ctl == 0x11, "old_base == 0x80 && old_ctl == 0x11");
        PER_CHECK(old_ch.size() == 1 && old_ch[0].ch == 0 && old_ch[0].period == 26000 && old_ch[0].duty == 13000,
                  "old_ch[0] == (0, 26000, 13000)");
        PER_CHECK(fabs(old_ch[0].freq - 1000.0) < 0.1 && fabs(old_ch[0].pct - 50.0) < 0.1,
                  "old_ch[0] freq=1000Hz pct=50%");

        std::map<u32, u32> npwm;
        npwm[0x100] = 0x09;
        npwm[0x104] = 0x171B;
        npwm[0x108] = 0x4E75;
        npwm[0x110] = 0x6590;
        std::vector<PwmChan> new_ch = pwm_new_channels(npwm);
        PER_CHECK(new_ch.size() == 1 && new_ch[0].ch == 0 && new_ch[0].period == 0x6590,
                  "new_ch[0] period == 0x6590");
        PER_CHECK(fabs(new_ch[0].pct - 54.5) < 0.2, "new_ch[0] pct ~ 54.5%");
    }

    /* 3. I2C LED decoders & WS2812 SPI */
    {
        LedDecode sm2135 = decode_i2c_led(24, 26, unhex("c01402ff00000000"));
        PER_CHECK(sm2135.valid && sm2135.driver == "SM2135" && sm2135.r == 1.0, "SM2135 decode");
        LedDecode sm2235 = decode_i2c_led(26, 24, unhex("d82400000000000003a20000"));
        PER_CHECK(sm2235.valid && sm2235.driver == "SM2235" && sm2235.c > 0.9, "SM2235 decode");
        LedDecode bp5758 = decode_i2c_led(24, 26, unhex("b01f0808081e1ebcbfbcbfbcbfa0a0a0a0"));
        PER_CHECK(bp5758.valid && bp5758.driver == "BP5758D" && bp5758.is_lit, "BP5758D decode");
        LedDecode bp1658 = decode_i2c_led(24, 26, unhex("b8231f1f0000000000000000"));
        PER_CHECK(bp1658.valid && bp1658.driver == "BP1658CJ" && bp1658.r == 1.0, "BP1658CJ decode");

        Bytes ws_spi;
        for (int i = 0; i < 3; i++)
            ws_spi += Bytes(8, '\xf8') + Bytes(16, '\xc0');
        std::vector<std::pair<int, std::pair<int, int> > > ws_px = decode_ws2812_spi(ws_spi);
        PER_CHECK(ws_px.size() == 3 && ws_px[0].first == 0 && ws_px[0].second.first == 255
                      && ws_px[0].second.second == 0,
                  "WS2812 SPI decode");
    }

    /* 4. SARADC parser & descriptor helpers */
    {
        int ch = -1, val = -1;
        PER_CHECK(parse_adc_channel("3", "", ch) && ch == 3, "parse_adc_channel('3')");
        PER_CHECK(parse_adc_channel("ch5", "", ch) && ch == 5, "parse_adc_channel('ch5')");
        PER_CHECK(parse_adc_channel("P26", "BK7231T", ch) && ch == 1, "parse_adc_channel('P26', BK7231T)");
        PER_CHECK(parse_adc_channel("P26", "BK7238", ch) && ch == 4, "parse_adc_channel('P26', BK7238)");
        PER_CHECK(parse_adc_spec("3:512", "", ch, val) && ch == 3 && val == 512, "parse_adc_spec('3:512')");
        PER_CHECK(parse_adc_spec("P23:1.65V", "BK7231T", ch, val) && ch == 3 && val == 512,
                  "parse_adc_spec('P23:1.65V')");
        PER_CHECK(parse_adc_spec("ch0:1650mV", "", ch, val) && ch == 0 && val == 512,
                  "parse_adc_spec('ch0:1650mV')");
        StrList specs;
        specs.push_back("0:420");
        specs.push_back("P26:777");
        std::map<int, int> parsed;
        PER_CHECK(parse_adc_specs(specs, "BK7231N", parsed) && parsed[0] == 420 && parsed[1] == 777,
                  "parse_adc_specs");
        std::map<int, int> samples;
        samples[3] = 512;
        AdcInfo adc_desc = describe_adc(3, 2, 2, &samples, 0, 512, "BK7231T");
        PER_CHECK(adc_desc.active && contains(adc_desc.summary, "ch3(P23)=512"), "describe_adc summary");
    }

    /* 5. BekenEmulator SARADC MMIO emulation (BK7231T/N and BK7238 register layouts) */
    {
        BuildOptions opts_t;
        opts_t.only_uart = true;
        opts_t.chip = "BK7231";
        opts_t.adc_inputs[1] = 700;
        opts_t.adc_inputs[3] = 512;
        ChipId cid_t;
        chip_family("BK7231", cid_t);
        BekenEmulator emu_t(Bytes(0x1000, (char)0xFF), Bytes(), Bytes(64, '\0'),
                            opts_t, cid_t, 0, find_chip("BK7231"));
        emu_t.setup();
        PER_CHECK(emu_t.get_adc_input(1) == 700 && emu_t.get_adc_input(3) == 512, "get_adc_input(1, 3)");
        PER_CHECK(emu_t.get_adc_input(2) == DEFAULT_ADC_RAW, "get_adc_input(2) default");
        emu_t.set_adc_input("P24", 888);
        PER_CHECK(emu_t.get_adc_input(2) == 888, "set_adc_input('P24', 888)");
        emu_t.mmio_write(0x00802C00, 4, 0x01 | 0x04 | (3u << 3) | 0x40);
        PER_CHECK(emu_t.state.saradc_channel == 3 && emu_t.state.saradc_last_raw == 512
                      && emu_t.state.saradc_pending == 32,
                  "BK7231T SARADC trigger on ch3");
        bool all_512 = true;
        for (int i = 0; i < 32; i++) {
            emu_t.mmio_read(0x00802C04, 4);
            if (emu_t.memory().phys_read(0x00802C04, 4) != 512u)
                all_512 = false;
        }
        PER_CHECK(all_512, "32 SARADC samples == 512");
        emu_t.mmio_read(0x00802C00, 4);
        u32 cfg_read = emu_t.memory().phys_read(0x00802C00, 4);
        PER_CHECK((cfg_read & (1u << 28)) != 0, "FIFO_EMPTY asserted after draining batch");

        BuildOptions opts_38;
        opts_38.only_uart = true;
        opts_38.chip = "BK7238";
        opts_38.adc_inputs[4] = 640;
        ChipId cid_38;
        chip_family("BK7238", cid_38);
        BekenEmulator emu_38(Bytes(0x1000, (char)0xFF), Bytes(), Bytes(64, '\0'),
                             opts_38, cid_38, 0, find_chip("BK7238"));
        emu_38.setup();
        emu_38.mmio_write(0x00802C08, 4, 4);
        emu_38.mmio_write(0x00802C0C, 4, 0x00004008);
        PER_CHECK(emu_38.state.saradc_channel == 4, "BK7238 SARADC channel == 4");
        emu_38.mmio_read(0x00802C10, 4);
        PER_CHECK(emu_38.memory().phys_read(0x00802C10, 4) == 640u, "BK7238 SARADC sample == 640");
    }

    return checks;
}

} /* namespace periph */
