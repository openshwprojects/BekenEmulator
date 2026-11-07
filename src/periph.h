/* Decode the GPIO/PWM/LED/meter registers the emulator observed, for the report
 * and the pin/PWM test assertions. Port of src/periph.py. */
#ifndef BK_PERIPH_H
#define BK_PERIPH_H

#include "common.h"
#include "json.h"

namespace periph {

static const int PIN_COUNT = 32;
static const int BK7252_PIN_COUNT = 40;
static const int BK7252_EXT_SLOT = 48;
static const int DEFAULT_ADC_RAW = 330;
static const double PWM_CLOCK_HZ = 26000000.0;
static const double PWM_FREQ_MIN_HZ = 20.0;
static const double PWM_FREQ_MAX_HZ = 100000.0;

int chip_pin_count(const std::string &chip);
int gpio_slot(int pin, int pin_count = PIN_COUNT); /* -1 = None */

std::string pin_alias(int pin);
std::string pin_role(int pin);
std::string decode_pin(u32 value);
std::string mode_name(u32 value);

/* SARADC channel <-> GPIO pad mapping & decoding */
std::map<int, int> adc_channel_pins(const std::string &chip = "");
std::string adc_channel_label(int channel, const std::string &chip = "");
bool parse_adc_channel(const std::string &target, const std::string &chip, int &out, std::string *err = 0);
bool parse_adc_spec(const std::string &spec, const std::string &chip, int &ch, int &val, std::string *err = 0);
bool parse_adc_specs(const StrList &specs, const std::string &chip, std::map<int, int> &out, std::string *err = 0);

struct AdcInfo {
    bool active;
    int active_channel; /* -1 = None */
    int conversions;
    int reads;
    std::map<int, int> samples;
    int last_raw;       /* -1 = None */
    std::string summary;
    AdcInfo() : active(false), active_channel(-1), conversions(0), reads(0), last_raw(-1) {}
};

AdcInfo describe_adc(int saradc_channel = -1, int saradc_conversions = 0, int saradc_reads = 0,
                     const std::map<int, int> *adc_samples = 0, const std::map<int, int> *adc_inputs = 0,
                     int last_raw = -1, const std::string &chip = "");

/* parse [EMU_GPIO]/[EMU_PWM]/[EMU_I2C] capture lines */
void parse_lines(const StrList &lines, std::map<int, u32> &gpio, std::map<u32, u32> &pwm);
void parse_i2c_lines(const StrList &lines, std::map<std::pair<int, int>, Bytes> &frames);

struct GpioRow {
    int pin;
    std::string alias;
    std::string state;
    std::string detail;
};

struct PwmChan {
    int ch;
    u32 period;
    u32 duty;    /* 0xFFFFFFFF = "None" */
    double freq;
    double pct;  /* <0 = None */
    bool has_duty;
    bool has_pct;
};

struct LedInfo {
    bool present;
    std::string kind;
    std::string summary;
    std::string hex;
    std::string live_hex;
    bool is_lit;
    StrList pixels;
    LedInfo() : present(false), is_lit(false) {}
};

struct MeterInfo {
    bool present;
    std::string kind;
    std::string summary;
    int cf_pin, cf1_pin, sel_pin, sel_lv;
    MeterInfo() : present(false), cf_pin(-1), cf1_pin(-1), sel_pin(-1), sel_lv(1) {}
};

struct DecodedState {
    bool present;
    std::vector<GpioRow> gpio;
    std::string pwm_layout;
    std::map<int, u32> raw;         /* logical pin -> config */
    std::vector<int> pwm_pins;
    std::vector<std::pair<std::string, std::string> > func;
    u32 pwm_ctl;
    u32 pwm_base;                   /* 0xFFFFFFFF = None */
    std::vector<PwmChan> pwm_channels;
    std::vector<std::pair<std::string, std::string> > pwm_regs;
    LedInfo led;
    MeterInfo meter;
    AdcInfo adc;
    DecodedState() : present(false), pwm_ctl(0), pwm_base(0xFFFFFFFFu) {}
};

std::vector<int> ctl_channels(u32 ctl);
std::vector<int> pwm_output_pins(const std::map<int, u32> &gpio);

/* Tuya config pairs are passed as a (key,value) list (from tuya_config). */
typedef std::vector<std::pair<std::string, std::string> > Pairs;

bool rgbcw_to_hex(double r, double g, double b, double c, double w, std::string &hex);

struct LedDecode {
    bool valid;
    std::string driver;
    int scl, sda;
    int raw_channels[5];
    int max_val;
    std::string cur_info;
    double r, g, b, c, w;
    std::string hex;
    bool is_lit;
    std::string raw_hex;
    LedDecode() : valid(false) {}
};
LedDecode decode_i2c_led(int scl, int sda, const Bytes &raw, const std::map<std::string, int> *cmap = 0);
std::vector<std::pair<int, std::pair<int, int> > > decode_ws2812_spi(const Bytes &buf); /* (r,g),b packed */

DecodedState decode_state(const std::map<int, u32> &gpio, const std::map<u32, u32> &pwm,
                          const std::string &chip,
                          const std::map<std::pair<int, int>, Bytes> *i2c_frames,
                          const std::map<std::pair<int, int>, Bytes> *i2c_last_on,
                          const Bytes *ws2812_spi,
                          const JsonValue *tuya_config_pairs, /* unused shortcut */
                          const Pairs *pairs,
                          int bl0942_polls,
                          const double *meter_values /* [v,a,w] or 0 */,
                          int saradc_channel = -1,
                          int saradc_conversions = 0,
                          int saradc_reads = 0,
                          const std::map<int, int> *adc_samples = 0,
                          int saradc_last_raw = -1,
                          const std::map<int, int> *adc_inputs = 0);

std::string chip_from_name(const std::string &filename);

/* describe_peripherals(tuya_config, meter_values) with no live hardware: the
 * Tuya-configured LED driver / energy meter (.led / .meter / .adc of the result). */
DecodedState describe_peripherals(const Pairs *pairs, const double *meter_values,
                                  int saradc_channel = -1,
                                  int saradc_conversions = 0,
                                  int saradc_reads = 0,
                                  const std::map<int, int> *adc_samples = 0,
                                  int saradc_last_raw = -1,
                                  const std::map<int, int> *adc_inputs = 0,
                                  const std::string &chip = "");
static const char *const LED_OFF_HEX_STR = "#181C22";

int periph_selftests();

} /* namespace periph */

#endif
