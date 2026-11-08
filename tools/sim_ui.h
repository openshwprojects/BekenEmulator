/* Win32 front end for the Beken emulator: a full port of the Tkinter
 * tools/sim_ui.py. Shared declarations for tools/sim_ui*.cpp.
 *
 * Model: every Tk variable becomes a Var bound to (at most) one Win32
 * control, the control being the source of truth exactly like a Tk widget's
 * textvariable. Treeviews become report ListViews (Table), Text widgets
 * become RichEdit panes (Pane) whose tags are character colours.
 *
 * The emulator is reached through EmuIface so the self-test can substitute
 * fakes for it, as the Python self-test does with SimpleNamespace objects.
 */
#ifndef SIM_UI_H
#define SIM_UI_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <commctrl.h>

#include "beken/emulator.h"
#include "emulator_backend.h"
#include "beken/periph.h"
#include "beken/tuya_config.h"
#include "json.h"

namespace simui {

/* ----------------------------------------------------------- constants */
extern std::string ROOT_DIR;
extern std::string FIRMWARE_DIR;
extern std::string DEFAULT_IMAGE;
extern std::string CONFIG_PATH;

struct Profile {
    const char *label;
    const char *file;
    const char *chip;
    const char *key;
    bool wifi;
    bool paired;
    int net_out; /* -1: GUI default, else 0/1 */
    const char *uart1_rx;
};
extern const Profile INTERESTING_DUMPS[];
extern const int N_INTERESTING;

extern const int POLL_MS;
extern const int MAX_LINES;
extern const char *FRAME_PRIVACY_NOTE;
extern const u64 GUEST_SECOND_INSNS;
extern const char *QUICK_COMMANDS[];
extern const int N_QUICK;
extern const int QUICK_PER_ROW;
extern const char *QUICKTICK;
extern const char *NET_TITLE;
extern const size_t MAX_FRAMES;
extern const char *ALL_FRAME_TARGETS;
extern const char *WIFI_HOWTO;
extern std::string NET_IDLE_HINT;
extern const char *LED_OFF_HEX;
static const int BK7252_EXT_SLOT = 48;

/* Every Config-tab option, in OPTION_DEFAULTS order. */
struct OptDef {
    const char *name;
    bool is_bool;
    bool bdefault;
    const char *sdefault;
};
extern const OptDef OPTION_DEFAULTS[];
extern const int N_OPTIONS;
bool is_text_option(const std::string &name);
bool is_unsaved_option(const std::string &name);

/* ----------------------------------------------------------- helpers */
std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &w);
std::wstring latin1_widen(const Bytes &b);
std::string guess_chip(const std::string &path);
std::string norm_path(const std::string &p); /* os.path.normcase(os.path.abspath(p)) */
bool same_file(const std::string &a, const std::string &b);
std::string abs_path(const std::string &p);
bool path_isfile(const std::string &p);
bool path_isdir(const std::string &p);
std::string py_repr(const std::string &utf8); /* repr() of a str */
StrList splitlines(const std::string &s);
std::string json_quote_ascii(const std::string &utf8);  /* json.dumps(ensure_ascii=True) */
std::string json_quote_utf8(const std::string &utf8);   /* json.dumps(ensure_ascii=False) */
std::string json_pretty(const JsonValue &v, int indent, int level); /* json.dumps(v, indent=N, ensure_ascii=False) */
std::string filter_from_line(const std::string &line);
std::string net_tag(const std::string &line);
bool gpio_is_input(bool have, u32 raw);
std::map<int, int> tuya_button_levels(const std::vector<std::pair<std::string, std::string> > *pairs);

/* Python's main.py checks, raising ValueError -> false + message. */
bool check_tuyamcu_pid(const std::string &pid, std::string &err);
bool check_wifi_pass(const std::string &pass, std::string &err);
bool parse_forward(const std::string &spec, std::pair<int, int> &out, std::string &err);
bool parse_http_gets(const StrList &paths, std::string &err);
bool parse_mqtt_pubs(const StrList &specs, std::vector<std::pair<std::string, Bytes> > &out, std::string &err);
bool parse_hex_blob(const std::string &spec, Bytes &out, std::string &err);

/* ----------------------------------------------------------- Tuya config */
/* What tuya_config.extract() returns, as the GUI holds it (Python: a dict or
 * None). `ident` gives the Python `is` identity the self-test checks. */
struct TuyaCfg {
    bool present;
    int ident;
    std::vector<std::pair<std::string, std::string> > pairs;
    std::vector<std::pair<std::string, std::string> > human;
    bool encrypted;
    long offset;
    int dps;
    tuya_config::Decoded dec; /* the cloud PSK material, never drawn */
    TuyaCfg() : present(false), ident(0), encrypted(false), offset(0), dps(0) {}
};
int new_tuya_ident();
bool tuya_extract_real(const std::string &path, TuyaCfg &out, std::string &err);

/* ----------------------------------------------------------- run config */
struct RunCfg {
    std::string chip, key, rom_path;
    unsigned xtal_mhz, psram_mb;
    bool boot, xvr_selfclear, blecore, tuyamcu;
    std::string tuyamcu_pid; /* "" == None */
    bool tuyamcu_raw;
    bool wifi;
    /* only when wifi */
    std::string wifi_role;
    bool net_out;
    bool has_ssid;
    std::string wifi_ssid;
    bool has_pass;
    std::string wifi_pass;
    std::vector<std::pair<int, int> > forwards;
    bool has_pcap;
    std::string pcap;
    StrList http_gets;
    std::vector<std::pair<std::string, Bytes> > mqtt_pubs;
    std::vector<Bytes> tls_psks;
    bool tuya_legacy_identity_psk;
    /* extras _on_run adds */
    bool has_uart1_rx;
    Bytes uart1_rx;
    bool has_meter;
    double meter_v, meter_a, meter_w;
    int cf_pin, cf1_pin, sel_pin, sel_lv;
    std::map<int, int> adc_inputs;
    RunCfg()
        : xtal_mhz(0), psram_mb(0), boot(false), xvr_selfclear(false), blecore(false), tuyamcu(false), tuyamcu_raw(false),
          wifi(false), net_out(false), has_ssid(false), has_pass(false), has_pcap(false),
          tuya_legacy_identity_psk(false), has_uart1_rx(false), has_meter(false), meter_v(0),
          meter_a(0), meter_w(0), cf_pin(-1), cf1_pin(-1), sel_pin(-1), sel_lv(1) {}
};

/* attach_wifi_peer(**kw) as the worker calls it */
struct PeerArgs {
    std::string role;
    bool has_ssid;
    std::string ssid;
    bool has_pass;
    std::string passphrase;
    std::vector<std::pair<int, int> > forwards;
    bool net_out;
    bool has_pcap;
    std::string pcap;
    StrList http_gets;
    std::vector<std::pair<std::string, Bytes> > mqtt_pubs;
    std::vector<Bytes> tls_psks;
    bool tuya_legacy_identity_psk;
    PeerArgs() : has_ssid(false), has_pass(false), net_out(false), has_pcap(false), tuya_legacy_identity_psk(false) {}
};

/* ----------------------------------------------------------- emulator */
typedef BekenEmulator::UiView EmuView;

/* The emulator as the GUI uses it. Every method has the behaviour of an
 * attribute the Python duck-typing would not find (getattr defaults). */
class EmuIface {
public:
    virtual ~EmuIface() {}
    virtual bool attach_wifi_peer(const PeerArgs &a, std::string &err) { (void)a; (void)err; return true; }
    virtual void setup() {}
    virtual void run() {}
    virtual void stop() {}
    virtual void close_network() {}
    virtual bool is_paused() { return false; }
    virtual void pause(bool p) { (void)p; }
    virtual void feed_uart1(const Bytes &data) { (void)data; }
    /* level -1 = release (None). false + err = AttributeError/ValueError */
    virtual bool set_gpio_input(int pin, int level, std::string &err) { (void)pin; (void)level; err = "AttributeError: no set_gpio_input"; return false; }
    virtual bool has_wake() { return false; }
    virtual void wake_from_deep_sleep(const std::string &reason, int pin) { (void)reason; (void)pin; }
    virtual bool has_energy_meter() { return false; }
    virtual void set_energy_meter(double v, double a, double w, int cf, int cf1, int sel, int sel_lv) { (void)v; (void)a; (void)w; (void)cf; (void)cf1; (void)sel; (void)sel_lv; }
    virtual bool meter_values(double out[3]) { (void)out; return false; }
    /* getattr(emu, "state", None): false when there is none */
    virtual bool view(EmuView &out) { (void)out; return false; }
    /* True when the backend supplies a clock including idle/time skips. */
    virtual bool elapsed_us(u64 &out) { (void)out; return false; }
    virtual u64 uart1_rx_delay() { return 0; }
    virtual size_t uart1_rx_queued() { return 0; }
    virtual u64 reset_count() { return 0; }
    virtual bool forward_listening() { return false; }
    virtual bool run_error(std::string &out) { (void)out; return false; }
    virtual bool has_extended_gpio() { return false; }
    virtual bool is_bk7252() { return false; }
    virtual std::string chip_variant() { return ""; }
    virtual std::string chip_name() { return ""; }
    virtual int gpio_pins() { return has_extended_gpio() ? 40 : periph::PIN_COUNT; }
    virtual int gpio_slot(int pin) { return periph::gpio_slot(pin, gpio_pins()); }
    virtual bool has_adc_input() { return false; }
    virtual void set_adc_input(int ch, int val) { (void)ch; (void)val; }
};

/* The real BekenEmulator behind EmuIface. */
class RealEmu : public EmuIface {
public:
    explicit RealEmu(BekenEmulator *e, EmulatorBackend *owner = 0) : e_(e), owner_(owner) {}
    ~RealEmu();
    BekenEmulator *emu() { return e_; }
    bool attach_wifi_peer(const PeerArgs &a, std::string &err);
    void setup() { e_->setup(); }
    void run() { e_->run(); }
    void stop() { e_->stop(); }
    void close_network() { e_->close_network(); }
    bool is_paused() { return e_->is_paused(); }
    void pause(bool p) { e_->pause(p); }
    void feed_uart1(const Bytes &data) { e_->feed_uart1(data); }
    bool set_gpio_input(int pin, int level, std::string &err);
    bool has_wake() { return true; }
    void wake_from_deep_sleep(const std::string &reason, int pin) { e_->wake_from_deep_sleep(reason, pin); }
    bool has_energy_meter() { return true; }
    void set_energy_meter(double v, double a, double w, int cf, int cf1, int sel, int sel_lv) { e_->set_energy_meter(v, a, w, cf, cf1, sel, sel_lv); }
    bool meter_values(double out[3]) { out[0] = e_->meter_v(); out[1] = e_->meter_a(); out[2] = e_->meter_w(); return true; }
    bool view(EmuView &out);
    u64 uart1_rx_delay() { return e_->uart1_rx_delay(); }
    size_t uart1_rx_queued() { return e_->uart1_rx_queued(); }
    u64 reset_count() { return e_->reset_count; }
    bool forward_listening();
    bool run_error(std::string &out) { if (!e_->has_run_error) return false; out = e_->run_error; return true; }
    bool has_extended_gpio() { return e_->has_extended_gpio; }
    bool is_bk7252() { return e_->is_bk7252; }
    std::string chip_variant() { return e_->chip_variant; }
    std::string chip_name() { return e_->chip ? std::string(e_->chip->name) : e_->chip_variant; }
    int gpio_pins() { return e_->chip ? e_->chip->gpio_pins : (e_->has_extended_gpio ? 40 : periph::PIN_COUNT); }
    bool has_adc_input() { return true; }
    void set_adc_input(int ch, int val) { e_->set_adc_input(ch, val); }
private:
    BekenEmulator *e_;
    EmulatorBackend *owner_;
};

/* ----------------------------------------------------------- widgets */
class SimUI;

/* A Tk variable bound to one control (or none). */
struct Var {
    enum Kind { NONE, EDIT, CHECK, COMBO, LABEL, MULTI };
    Kind kind;
    HWND hwnd;
    std::string shadow;
    bool bshadow;
    bool traced;
    SimUI *ui;
    Var() : kind(NONE), hwnd(0), bshadow(false), traced(false), ui(0) {}
    std::string get() const;
    void set(const std::string &v);
    bool getb() const;
    void setb(bool v);
};

/* A Tk Text widget with tags: a RichEdit whose tags are colours. */
struct Pane {
    HWND hwnd;
    std::vector<std::pair<std::string, COLORREF> > tags;
    COLORREF fg, bg;
    long newlines;
    Pane() : hwnd(0), fg(0), bg(0), newlines(0) {}
};

/* A ttk Treeview: a report ListView with row ids and a tag per row. */
struct Table {
    HWND hwnd;
    int ncols;
    std::vector<std::string> iids;
    std::vector<std::string> tags;
    std::vector<std::pair<std::string, COLORREF> > tag_colors;
    int quiet; /* >0: selection changes are programmatic, not user events */
    Table() : hwnd(0), ncols(0), quiet(0) {}
    void clear();
    void insert(const std::string &iid, const StrList &values, const std::string &tag);
    int index_of(const std::string &iid) const;
    bool exists(const std::string &iid) const { return index_of(iid) >= 0; }
    StrList item(const std::string &iid) const;
    std::string selection() const; /* "" = none (single-select) */
    void selection_set(const std::string &iid);
    void selection_remove();
    size_t size() const { return iids.size(); }
    int top_index() const;
    void restore_top(int idx);
    void see(const std::string &iid);
};

/* Hooks the self-test replaces (Python monkeypatching). */
struct Hooks {
    void (*on_run)(SimUI *, void *);
    void *on_run_ctx;
    bool refresh_tuya_disabled;
    bool (*tuya_extract)(const std::string &path, TuyaCfg &out, std::string &err, void *ctx);
    void *tuya_ctx;
    void (*finish)(SimUI *, void *);
    void *finish_ctx;
    EmuIface *(*build)(SimUI *ui, const std::string &path, const BuildOptions &opt, const std::string &key,
                       std::string &err, void *ctx);
    void *build_ctx;
    bool (*browser_open)(const std::string &url, int new_, void *ctx);
    void *browser_ctx;
    std::string (*ask_save)(const char *ext, void *ctx);
    void *ask_save_ctx;
    void (*show_message)(const std::string &title, const std::string &msg, bool error, void *ctx);
    void *show_message_ctx;
    Hooks() { memset(this, 0, sizeof(*this)); }
};

struct Frame {
    std::string dir;
    Bytes data;
    u64 us;
};

struct FrameRow {
    long no;
    std::string dir;
    Bytes data;
    u64 us;
    std::string what;
};

/* ----------------------------------------------------------- the window */
class SimUI {
public:
    /* settings_path "" == None: neither read nor write one. hidden: the
     * self-test's withdrawn root. */
    SimUI(const std::string &initial_image, const std::string &settings_path, bool hidden);
    ~SimUI();

    HWND root;
    std::string settings_path;
    Hooks hooks;

    /* --- state (Python attribute names) --- */
    EmuIface *emu;
    bool emu_owned;
    Thread *thread;
    int thread_mode; /* 0 None, 1 real Thread, 2 a fake always-alive thread */
    double started_at;
    Mutex result_lock;
    bool has_finished;
    std::string finished;
    bool has_error;
    std::string error;
    std::string active_forward_url; /* "" == None */

    Mutex out_lock;
    Bytes out1, out2;
    StrList net_lines;
    long net_count;
    std::deque<Frame> frames;
    long frames_dropped;
    HWND detail_win;
    double detail_drawn_at;
    long detail_drawn_count;
    bool net_hint_shown;
    long tx_count[3];
    int hex_col;
    std::string u2_partial;
    bool periph_key_valid;
    std::string periph_key;
    std::map<int, int> gpio_pressed;
    int gpio_selected; /* -1 None */
    std::map<int, int> adc_overrides;
    TuyaCfg run_tuya_config;
    std::string run_image; /* "" None */
    bool tuya_key_valid;
    std::string tuya_key;
    TuyaCfg tuya_config;
    u64 last_insns;
    u64 last_reset_count;
    bool has_last_rate_at;
    double last_rate_at;
    double rate;
    StrList history;
    int history_pos; /* -1 None */
    bool pending_run;
    bool stop_requested;
    bool drop_enabled;
    std::string drop_note;
    bool has_pending_uart1_rx;
    Bytes pending_uart1_rx;
    bool ui_ready;
    int in_set; /* Var::set in progress: ignore the control's own notifications */
    std::string last_message; /* the last dialog shown (for the self-test) */

    /* --- variables --- */
    Var var_image, var_chip, var_key, var_summary, var_state;
    Var opt[17];               /* indexed like OPTION_DEFAULTS */
    Var *optv(const std::string &name);
    Var var_hex, var_hide_tick, var_net_ip_only, var_all_pins;
    Var var_pwm, var_adc, var_adc_ch, var_adc_val, var_meter, var_meter_v, var_meter_a, var_meter_w, var_led, var_camera, var_sleep;
    Var var_gpio_action, var_input, var_mode, var_ending;
    Var var_tuya_status, var_cfg_note;
    Var var_frame_filter, var_frame_target, var_frame_count, var_frame_privacy;
    bool frame_vars_made;
    StrList frame_target_values;

    /* --- widgets --- */
    HFONT font_ui, font_mono, font_bold;
    HWND grp_image, image_entry, btn_browse, lbl_chip, cmb_chip, lbl_key, cmb_key;
    std::vector<HWND> interesting_buttons;
    std::vector<int> interesting_index; /* profile index of each button */
    std::vector<HWND> interesting_group_labels;
    std::vector<std::vector<HWND> > interesting_rows;
    HWND btn_run, btn_pause, btn_stop, btn_clear, btn_save, lbl_summary, lbl_state;
    HWND tabs, run_tab, config_tab, tuya_tab;
    int tab_index;
    /* run tab */
    HWND grp_u1, chk_hex, btn_copy1, grp_u2, chk_hide_tick, btn_copy2;
    HWND grp_peri, chk_all_pins, lbl_pwm, lbl_adc, lbl_adc_ch, cmb_adc_ch, lbl_adc_val, ent_adc_val, btn_adc_apply;
    HWND lbl_meter, lbl_mv, lbl_ma, lbl_mw, ent_mv, ent_ma, ent_mw, btn_meter_apply;
    HWND led_swatch, lbl_led, cam_canvas, lbl_camera, lbl_sleep, btn_wake_gpio, btn_wake_rtc;
    HWND lbl_gpio_action, btn_gpio_press;
    HWND net_frame, chk_ip_only, btn_copy_net, btn_details;
    HWND grp_send, entry, cmb_mode, cmb_ending, btn_send, lbl_quick;
    std::vector<HWND> quick_buttons;
    Pane txt1, txt2, txt_net, txt_tuya_json, txt_bytes;
    Table tree, tuya_tree, frame_tree;
    std::string swatch_bg;
    StrList swatch_pixels;
    Bytes cam_rgb; /* the preview drawn on cam_canvas (64x48 RGB) */
    double split_cols[2]; /* run tab column splits (fractions) */
    double split_net;      /* run tab top/network split */
    int drag; /* 0 none, 1/2 column splitter, 3 net splitter */
    RECT gap_rc[3];       /* the three splitter gaps, run-tab page coordinates */
    RECT split_area;      /* the panes' area the splitters divide */
    bool hidden;          /* the self-test's withdrawn root */
    HWND pcap_browse;     /* the radio capture's "..." button */
    int cfg_scroll;       /* the Config page's vertical scroll offset */
    int layout_cfg_group(int g, int gx, int gy, int gw, bool do_place);
    /* config tab */
    HWND grp_dev, grp_netcfg, btn_reset, lbl_cfg_note, lbl_radio_capture_note, btn_open_forward;
    struct CfgRow {
        int group;        /* 0 Device, 1 Network */
        HWND w0;          /* column 0: a check or a label */
        int indent;       /* column 0 indent */
        bool w0_check;    /* w0 is a checkbox */
        bool spans;       /* w0 spans columns 0 and 1 */
        std::vector<std::pair<HWND, int> > c1; /* column 1 widgets, left to right, with widths */
        int c1_h;         /* column 1 height (0 = one control row) */
        HWND note;
        CfgRow() : group(0), w0(0), indent(0), w0_check(false), spans(false), c1_h(0), note(0) {}
    };
    std::vector<CfgRow> cfg_rows;
    std::map<std::string, HWND> opt_text; /* http_gets / mqtt_pubs boxes */
    std::vector<std::pair<std::string, std::vector<HWND> > > opt_widgets;
    std::set<HWND> note_labels;
    /* tuya tab */
    HWND lbl_tuya_status, btn_tuya_refresh, grp_tuya_sum, grp_tuya_json;
    /* details window */
    HWND lbl_filter, ent_filter, btn_apply, btn_refresh_d, lbl_target, frame_target, btn_save_pcap;
    HWND lbl_frame_count, lbl_frame_privacy, btn_copy_bytes;

    /* --- Python methods --- */
    void build(const std::string &initial_image);
    void build_config();
    void build_tuya_config();
    void tab_changed();
    void select_tab(int idx);
    bool refresh_tuya_config(bool force, const std::string &path_in = std::string(), bool have_path = false);
    void options_changed();
    void chip_changed();
    std::string summary_text();
    bool collect_cfg(RunCfg &cfg, std::string &err);
    void browse_pcap();
    void update_open_forward();
    bool forward_is_open();
    void open_forward();
    void set_text_option(const std::string &name, const std::string &value);
    std::map<std::string, std::string> option_values(); /* bools as "1"/"0" */
    bool optb(const std::string &name) { return optv(name)->getb(); }
    std::string opts(const std::string &name);
    void set_option_str(const std::string &name, const std::string &v);
    void set_option_bool(const std::string &name, bool v);
    void reset_options();
    bool load_settings(JsonValue &out);
    void apply_settings(const JsonValue &data, bool same_image);
    std::string settings_data();
    void save_settings();
    bool enable_drop();
    void on_drop(const StrList &paths);
    void load_image(const std::string &path, bool run, const std::string &chip = "", const std::string &key = "");
    void run_interesting(int idx);
    void details_for_click(bool have_line, const std::string &line);
    void open_details();
    static bool frame_target_host(const std::string &dir, const Bytes &data, std::string &host);
    static StrList target_hosts(const std::vector<Frame> &frames);
    void refresh_target_hosts(const std::vector<Frame> &frames);
    std::vector<FrameRow> visible_frames();
    void refresh_details();
    void show_frame_bytes();
    void save_pcap();
    bool detail_exists();
    void append(Pane &p, const std::wstring &text, const char *tag = 0);
    void append_utf8(Pane &p, const std::string &text, const char *tag = 0);
    std::string render_uart1(const Bytes &data);
    void gpio_press_level(int pin, u32 raw, int &level, std::string &why);
    bool gpio_config(int pin, u32 &raw);
    int selected_gpio();
    void gpio_selection_changed();
    void gpio_double_click(int index);
    std::map<int, int> sleep_wake_pins();
    void update_sleep_controls();
    void wake_deep_sleep(const std::string &reason);
    void update_gpio_control();
    void toggle_gpio_input();
    void release_gpio_inputs();
    void parsed_meter_values(double out[3]);
    void apply_energy_meter();
    void apply_adc_input();
    void update_adc_widget(const periph::AdcInfo *adc = 0);
    void draw_led_swatch(const periph::LedInfo *led);
    void update_camera_widget();
    void update_periph_widgets(const periph::LedInfo *led, const periph::MeterInfo *meter, const double mv[3]);
    void refresh_static_periph();
    void refresh_periph();
    std::string render_uart2(const Bytes &data);
    void copy(Pane &p, const char *label);
    void clear();
    void save();
    void browse();
    static void s_sink(void *ctx, int port, u8 byte);
    static void s_net_sink(void *ctx, const std::string &line);
    static void s_frame_sink(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us);
    void frame_sink(const std::string &dir, const Bytes &frame, u64 now_us);
    void net_sink(const std::string &line);
    void on_run();
    void on_run_real();
    void worker(const std::string &path, const RunCfg &cfg);
    void on_pause();
    void on_stop();
    void on_close();
    bool thread_alive();
    bool running();
    bool console_ready();
    void send_bytes(const Bytes &payload);
    void send_command(const std::string &text);
    void on_send();
    void history_back();
    void history_fwd();
    void poll();
    void update_state();
    std::string fmt_insns();
    void set_send_enabled(bool on);
    void finish();
    void finish_real();

    /* --- Win32 plumbing --- */
    void message(const std::string &msg, bool error);
    std::string ask_save(const char *ext, const wchar_t *filter);
    std::string ask_open();
    bool browser(const std::string &url);
    void layout();
    void layout_run();
    void layout_config();
    void layout_tuya();
    void layout_details();
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool &done);
    void on_command(HWND ctl, int code);
    LRESULT on_notify(NMHDR *nm, bool &done);
    bool pre_translate(MSG &m);
    HWND mk(const wchar_t *cls, const std::string &text, DWORD style, HWND parent, DWORD exstyle = 0);
    HWND mk_button(const std::string &text, HWND parent);
    HWND mk_check(const std::string &text, HWND parent);
    HWND mk_label(const std::string &text, HWND parent, DWORD extra = 0);
    HWND mk_edit(HWND parent, bool mono = false);
    HWND mk_combo(HWND parent, const StrList &values);
    void mk_pane(Pane &p, HWND parent, bool wrap);
    void mk_table(Table &t, HWND parent);
    void table_column(Table &t, const char *title, int width, bool right);
    int text_w(const std::string &s, HFONT f = 0);
    int text_h(const std::string &s, int width, HFONT f = 0);
    void bind(Var &v, Var::Kind kind, HWND h, bool traced);
    std::map<HWND, Var *> var_of;
    std::map<std::string, HWND> button_of_text;
    int dpi;
    int S(int v) const { return MulDiv(v, dpi, 96); }
    HBRUSH br_dark, br_face, br_window;
};

/* Pane helpers */
std::string pane_text(const Pane &p);          /* get("1.0", "end-1c") */
void pane_clear(Pane &p);
std::set<std::string> pane_tags_of(Pane &p, const std::string &needle);

bool enabled(HWND h);
std::string window_text(HWND h);
void set_window_text(HWND h, const std::string &s);
void set_state(HWND h, bool on);

/* the real build_emulator behind Hooks.build */
EmuIface *real_build(SimUI *ui, const std::string &path, const BuildOptions &opt, const std::string &key,
                     std::string &err, void *ctx);
void init_paths();
bool register_classes();
int run_selftests();
int run_package_smoke();
int run_live_check(const std::string &image, double seconds);
int run_live_sleep();

} /* namespace simui */

#endif
