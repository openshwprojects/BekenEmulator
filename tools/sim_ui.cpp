/* Win32 front end for the Beken emulator, a full native port of the Tkinter
 * tools/sim_ui.py: the image picker with drag and drop, quick-run profiles,
 * Run/Pause/Stop, the Config and Tuya config tabs, UART1/UART2 consoles, the
 * network pane with its captured-traffic window, and the GPIO/PWM column with
 * GPIO input presses, deep-sleep wake, energy meter, LED swatch and camera.
 *
 *   sim_ui [image]          launch the GUI
 *   sim_ui --selftest       the GUI self-tests, run headlessly
 *   sim_ui --package-smoke  the release smoke test
 *
 * Window creation, layout and message routing live here; the methods that
 * mirror SimulatorUI live in sim_ui_logic.cpp, helpers in sim_ui_util.cpp
 * and the self-test in sim_ui_selftest.cpp.
 */
#include "sim_ui.h"
#include "wifipeer.h"
#include "beken_crypto.h"

#include <richedit.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace simui {

static HINSTANCE g_inst;
static int g_next_id = 100;
static const wchar_t *MAIN_CLASS = L"BekenSimUI";
static const wchar_t *PAGE_CLASS = L"BekenSimUIPage";
static const wchar_t *DETAIL_CLASS = L"BekenSimUIDetails";
static const wchar_t *SWATCH_CLASS = L"BekenSimUISwatch";

static COLORREF hexcolor(const std::string &hex)
{
    unsigned r = 0, g = 0, b = 0;
    if (hex.size() == 7 && hex[0] == '#')
        sscanf(hex.c_str() + 1, "%2x%2x%2x", &r, &g, &b);
    return RGB(r, g, b);
}

static const COLORREF CON_BG = RGB(0x10, 0x12, 0x16);
static const COLORREF CON_FG = RGB(0xd8, 0xdd, 0xe3);
static const COLORREF NOTE_INK = RGB(0x5b, 0x64, 0x72);
static const COLORREF PRIVACY_INK = RGB(0x8a, 0x4b, 0x08);

static LRESULT CALLBACK ui_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    SimUI *ui = (SimUI *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (ui) {
        bool done = false;
        LRESULT r = ui->handle(hwnd, msg, wp, lp, done);
        if (done)
            return r;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_classes()
{
    static bool done = false;
    if (done)
        return true;
    g_inst = GetModuleHandleW(0);
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll");
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(0, IDI_APPLICATION);
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassW(&wc);
    wc.lpszClassName = DETAIL_CLASS;
    RegisterClassW(&wc);
    wc.hIcon = 0;
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wc);
    wc.hbrBackground = 0;
    wc.lpszClassName = SWATCH_CLASS;
    RegisterClassW(&wc);
    done = true;
    return true;
}

/* ------------------------------------------------------------ factories */
HWND SimUI::mk(const wchar_t *cls, const std::string &text, DWORD style, HWND parent, DWORD exstyle)
{
    HWND h = CreateWindowExW(exstyle, cls, widen(text).c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)(g_next_id++), g_inst, this);
    SendMessageW(h, WM_SETFONT, (WPARAM)font_ui, FALSE);
    return h;
}

HWND SimUI::mk_button(const std::string &text, HWND parent) { return mk(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, parent); }
HWND SimUI::mk_check(const std::string &text, HWND parent) { return mk(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, parent); }
HWND SimUI::mk_label(const std::string &text, HWND parent, DWORD extra) { return mk(L"STATIC", text, SS_LEFT | SS_NOPREFIX | extra, parent); }

HWND SimUI::mk_edit(HWND parent, bool mono)
{
    HWND h = mk(L"EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, parent, WS_EX_CLIENTEDGE);
    if (mono)
        SendMessageW(h, WM_SETFONT, (WPARAM)font_mono, FALSE);
    return h;
}

HWND SimUI::mk_combo(HWND parent, const StrList &values)
{
    HWND h = mk(WC_COMBOBOXW, "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, parent);
    for (size_t i = 0; i < values.size(); i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)widen(values[i]).c_str());
    return h;
}

void SimUI::mk_pane(Pane &p, HWND parent, bool wrap)
{
    DWORD style = WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP;
    if (!wrap)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    p.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)(g_next_id++), g_inst, 0);
    if (!p.hwnd) /* very old systems: RichEdit 2.0 */
        p.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit20W", L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                 parent, (HMENU)(INT_PTR)(g_next_id++), g_inst, 0);
    p.fg = CON_FG;
    p.bg = CON_BG;
    SendMessageW(p.hwnd, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SendMessageW(p.hwnd, EM_SETUNDOLIMIT, 0, 0);
    SendMessageW(p.hwnd, EM_SETBKGNDCOLOR, 0, (LPARAM)CON_BG);
    SendMessageW(p.hwnd, EM_SETTARGETDEVICE, 0, wrap ? 0 : 1);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET | CFM_BOLD | CFM_ITALIC;
    cf.crTextColor = CON_FG;
    cf.yHeight = 180;
    cf.bCharSet = DEFAULT_CHARSET;
    wcscpy(cf.szFaceName, L"Consolas");
    SendMessageW(p.hwnd, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(p.hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    p.tags.push_back(std::make_pair(std::string("sent"), RGB(0x7f, 0xd6, 0xa0)));
    p.tags.push_back(std::make_pair(std::string("note"), RGB(0xe0, 0xa2, 0x4a)));
}

void SimUI::mk_table(Table &t, HWND parent)
{
    t.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 0, 0,
                             10, 10, parent, (HMENU)(INT_PTR)(g_next_id++), g_inst, 0);
    SendMessageW(t.hwnd, WM_SETFONT, (WPARAM)font_ui, FALSE);
    SendMessageW(t.hwnd, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
}

void SimUI::table_column(Table &t, const char *title, int width, bool right)
{
    std::wstring w = widen(title);
    LVCOLUMNW c;
    memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    c.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
    c.cx = S(width);
    c.pszText = (LPWSTR)w.c_str();
    SendMessageW(t.hwnd, LVM_INSERTCOLUMNW, (WPARAM)t.ncols, (LPARAM)&c);
    t.ncols++;
}

int SimUI::text_w(const std::string &s, HFONT f)
{
    HDC dc = GetDC(root);
    HGDIOBJ old = SelectObject(dc, f ? f : font_ui);
    std::wstring w = widen(s);
    SIZE sz;
    sz.cx = 0;
    GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(root, dc);
    return sz.cx;
}

int SimUI::text_h(const std::string &s, int width, HFONT f)
{
    HDC dc = GetDC(root);
    HGDIOBJ old = SelectObject(dc, f ? f : font_ui);
    std::wstring w = widen(s);
    RECT rc = {0, 0, width > 10 ? width : 10, 0};
    DrawTextW(dc, w.c_str(), (int)w.size(), &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(root, dc);
    return rc.bottom - rc.top;
}

void SimUI::bind(Var &v, Var::Kind kind, HWND h, bool traced)
{
    v.kind = kind;
    v.hwnd = h;
    v.ui = this;
    v.traced = traced;
    if (h)
        var_of[h] = &v;
}

/* ------------------------------------------------------------ construct */
SimUI::SimUI(const std::string &initial_image, const std::string &settings_path_in, bool hidden_in)
    : root(0), settings_path(settings_path_in), emu(0), emu_owned(false), thread(0), thread_mode(0), started_at(0),
      has_finished(false), has_error(false), net_count(0), frames_dropped(0), detail_win(0), detail_drawn_at(0),
      detail_drawn_count(-1), net_hint_shown(false), hex_col(0), periph_key_valid(false), gpio_selected(-1),
      tuya_key_valid(false), last_insns(0), last_reset_count(0), has_last_rate_at(false), last_rate_at(0), rate(0),
      history_pos(-1), pending_run(false), stop_requested(false), drop_enabled(false), has_pending_uart1_rx(false),
      ui_ready(false), in_set(0), frame_vars_made(false), tab_index(0), drag(0), hidden(hidden_in), pcap_browse(0), cfg_scroll(0)
{
    tx_count[0] = tx_count[1] = tx_count[2] = 0;
    split_cols[0] = 1.0 / 3;
    split_cols[1] = 2.0 / 3;
    split_net = 0.75;
    memset(gap_rc, 0, sizeof(gap_rc));
    memset(&split_area, 0, sizeof(split_area));
    register_classes();
    br_dark = CreateSolidBrush(CON_BG);
    br_face = GetSysColorBrush(COLOR_BTNFACE);
    br_window = GetSysColorBrush(COLOR_WINDOW);

    /* Last session's choices. An image named on the command line (or dropped
     * on the launcher) wins over the remembered one. */
    JsonValue settings;
    load_settings(settings);
    std::string saved;
    if (settings.has("image") && settings.get("image").is_str() && !settings.get("image").as_str().empty()
        && path_isfile(settings.get("image").as_str()))
        saved = settings.get("image").as_str();
    std::string start = !initial_image.empty() ? initial_image : (!saved.empty() ? saved : DEFAULT_IMAGE);

    root = CreateWindowExW(WS_EX_ACCEPTFILES, MAIN_CLASS, L"BekenEmulator", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1360, 900, 0, 0, g_inst, this);
    HDC dc = GetDC(root);
    dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(root, dc);
    if (dpi <= 0)
        dpi = 96;
    if (!hidden) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = S(1360), h = S(900);
        if (w > wa.right - wa.left)
            w = wa.right - wa.left;
        if (h > wa.bottom - wa.top)
            h = wa.bottom - wa.top;
        SetWindowPos(root, 0, wa.left + 20, wa.top + 10, w, h, SWP_NOZORDER);
    }
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    font_ui = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    font_bold = CreateFontIndirectW(&lf);
    font_mono = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, L"Consolas");

    build(start);
    apply_settings(settings, !saved.empty() && same_file(start, saved));
    refresh_tuya_config(false);
    drop_enabled = enable_drop();
    options_changed();
    layout();
    if (!hidden) {
        SetTimer(root, 1, POLL_MS, 0);
        ShowWindow(root, SW_SHOW);
        UpdateWindow(root);
    }
}

SimUI::~SimUI()
{
    if (emu)
        emu->stop();
    bool worker_gone = true;
    if (thread) {
        thread->join(5000);
        worker_gone = !thread->is_alive();
        if (worker_gone)
            delete thread;
    }
    if (emu && emu_owned && worker_gone) /* never pull the emulator from under a live worker */
        delete emu;
    emu = 0;
    if (detail_win && IsWindow(detail_win))
        DestroyWindow(detail_win);
    if (root && IsWindow(root)) {
        SetWindowLongPtrW(root, GWLP_USERDATA, 0);
        DestroyWindow(root);
    }
    DeleteObject(font_ui);
    DeleteObject(font_bold);
    DeleteObject(font_mono);
    DeleteObject(br_dark);
}

void SimUI::build(const std::string &initial_image)
{
    /* --- image row --- */
    grp_image = mk(L"BUTTON", "image", BS_GROUPBOX, root);
    image_entry = mk_edit(root);
    bind(var_image, Var::EDIT, image_entry, false);
    var_image.set(path_isfile(initial_image) || path_isdir(initial_image) ? initial_image : std::string());
    btn_browse = mk_button("Browse...", root);
    lbl_chip = mk_label("chip", root);
    StrList chips = chip_family_names();
    std::sort(chips.begin(), chips.end());
    cmb_chip = mk_combo(root, chips);
    bind(var_chip, Var::COMBO, cmb_chip, false);
    var_chip.set(guess_chip(initial_image));
    lbl_key = mk_label("key", root);
    StrList keys(1, "(none)");
    StrList kn;
    for (int i = 0; KNOWN_KEY_NAMES[i]; i++)
        kn.push_back(KNOWN_KEY_NAMES[i]);
    std::sort(kn.begin(), kn.end());
    keys.insert(keys.end(), kn.begin(), kn.end());
    cmb_key = mk_combo(root, keys);
    bind(var_key, Var::COMBO, cmb_key, false);
    var_key.set("TUYA");

    /* Every Config-tab option: bound to its control in build_config. */
    for (int i = 0; i < N_OPTIONS; i++) {
        opt[i].ui = this;
        opt[i].traced = true;
        if (OPTION_DEFAULTS[i].is_bool)
            opt[i].bshadow = OPTION_DEFAULTS[i].bdefault;
        else
            opt[i].shadow = OPTION_DEFAULTS[i].sdefault;
    }

    /* --- quick-run launchers --- */
    for (int paired = 0; paired < 2; paired++) {
        HWND lbl = mk_label(paired ? "Real Tuya reconnect (owned IDs only; sends to cloud):" : "Quick run:", root);
        interesting_group_labels.push_back(lbl);
        std::vector<HWND> row;
        for (int i = 0; i < N_INTERESTING; i++) {
            if (INTERESTING_DUMPS[i].paired != (paired != 0))
                continue;
            HWND b = mk_button(INTERESTING_DUMPS[i].label, root);
            interesting_buttons.push_back(b);
            interesting_index.push_back(i);
            row.push_back(b);
        }
        interesting_rows.push_back(row);
    }

    /* --- run controls --- */
    btn_run = mk_button("Run", root);
    btn_pause = mk_button("Pause", root);
    set_state(btn_pause, false);
    btn_stop = mk_button("Stop", root);
    set_state(btn_stop, false);
    btn_clear = mk_button("Clear", root);
    btn_save = mk_button("Save log...", root);
    lbl_summary = mk_label("", root, SS_ENDELLIPSIS);
    bind(var_summary, Var::LABEL, lbl_summary, false);
    lbl_state = mk_label("", root, SS_SUNKEN | SS_ENDELLIPSIS | SS_CENTERIMAGE);
    bind(var_state, Var::LABEL, lbl_state, false);
    var_state.set("idle - pick an image and press Run");

    /* --- tabs --- */
    /* The pages are the tab control's children, so its themed body never
     * paints over them. */
    tabs = mk(WC_TABCONTROLW, "", WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP, root, WS_EX_CONTROLPARENT);
    const char *names[3] = {"Run", "Config", "Tuya config"};
    for (int i = 0; i < 3; i++) {
        std::wstring w = widen(names[i]);
        TCITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)w.c_str();
        SendMessageW(tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&ti);
    }
    run_tab = CreateWindowExW(WS_EX_CONTROLPARENT, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10,
                              10, tabs, 0, g_inst, this);
    config_tab = CreateWindowExW(WS_EX_CONTROLPARENT, PAGE_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, tabs,
                                 0, g_inst, this);
    tuya_tab = CreateWindowExW(WS_EX_CONTROLPARENT, PAGE_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, tabs, 0,
                               g_inst, this);
    SetWindowPos(run_tab, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    /* --- Run tab: UART1 --- */
    HWND p = run_tab;
    grp_u1 = mk(L"BUTTON", "UART1 - MCU link / command console", BS_GROUPBOX, p);
    mk_pane(txt1, p, true);
    chk_hex = mk_check("show as hex", p);
    bind(var_hex, Var::CHECK, chk_hex, false);
    btn_copy1 = mk_button("Copy", p);

    /* --- GPIO / PWM --- */
    grp_peri = mk(L"BUTTON", "GPIO / PWM", BS_GROUPBOX, p);
    mk_table(tree, p);
    table_column(tree, "Pin", 38, true);
    table_column(tree, "Name", 74, false);
    table_column(tree, "State", 250, false);
    table_column(tree, "Reg", 58, true);
    tree.tag_colors.push_back(std::make_pair(std::string("factory"), RGB(0x5b, 0x64, 0x72)));
    tree.tag_colors.push_back(std::make_pair(std::string("out"), RGB(0x0b, 0x6b, 0x3a)));
    tree.tag_colors.push_back(std::make_pair(std::string("func"), RGB(0x14, 0x44, 0x8f)));
    tree.tag_colors.push_back(std::make_pair(std::string("pressed"), RGB(0x9b, 0x2c, 0x2c)));
    chk_all_pins = mk_check("show untouched pins", p);
    bind(var_all_pins, Var::CHECK, chk_all_pins, false);
    lbl_pwm = mk_label("", p);
    bind(var_pwm, Var::LABEL, lbl_pwm, false);
    var_pwm.set("no PWM channels decoded yet");
    lbl_adc = mk_label("", p);
    bind(var_adc, Var::LABEL, lbl_adc, false);
    var_adc.set(strf("ADC: idle (default %d / %.2fV)", periph::DEFAULT_ADC_RAW, periph::DEFAULT_ADC_RAW * 3.3 / 1023.0));
    lbl_adc_ch = mk_label("Ch:", p);
    StrList adc_chs;
    const char *const adc_ch_items[] = {"0 (VBAT)", "1 (P4)", "2 (P5)", "3 (P23)", "4 (P2)", "5 (P3)",
                                        "6 (P12)", "7 (P13)", "P26", "P24", "P20", 0};
    for (int i = 0; adc_ch_items[i]; i++)
        adc_chs.push_back(adc_ch_items[i]);
    cmb_adc_ch = mk_combo(p, adc_chs);
    bind(var_adc_ch, Var::COMBO, cmb_adc_ch, false);
    var_adc_ch.set("3");
    lbl_adc_val = mk_label("Val:", p);
    ent_adc_val = mk_edit(p);
    bind(var_adc_val, Var::EDIT, ent_adc_val, false);
    var_adc_val.set(strf("%d", periph::DEFAULT_ADC_RAW));
    btn_adc_apply = mk_button("Set ADC", p);
    lbl_meter = mk_label("", p);
    bind(var_meter, Var::LABEL, lbl_meter, false);
    var_meter.set("Meter: none detected (BL0942 UART1 ready)");
    lbl_mv = mk_label("V:", p);
    ent_mv = mk_edit(p);
    bind(var_meter_v, Var::EDIT, ent_mv, false);
    var_meter_v.set("230");
    lbl_ma = mk_label("A:", p);
    ent_ma = mk_edit(p);
    bind(var_meter_a, Var::EDIT, ent_ma, false);
    var_meter_a.set("1.5");
    lbl_mw = mk_label("W:", p);
    ent_mw = mk_edit(p);
    bind(var_meter_w, Var::EDIT, ent_mw, false);
    var_meter_w.set("345");
    btn_meter_apply = mk_button("Set V/A/W", p);
    led_swatch = CreateWindowExW(0, SWATCH_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, p, 0, g_inst, this);
    swatch_bg = LED_OFF_HEX;
    lbl_led = mk_label("", p);
    bind(var_led, Var::LABEL, lbl_led, false);
    var_led.set("LED driver: none detected");
    cam_canvas = CreateWindowExW(0, SWATCH_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, p, 0, g_inst, this);
    lbl_camera = mk_label("", p);
    bind(var_camera, Var::LABEL, lbl_camera, false);
    var_camera.set("Camera: none detected");
    lbl_sleep = mk_label("", p);
    bind(var_sleep, Var::LABEL, lbl_sleep, false);
    var_sleep.set("Sleep: AWAKE");
    btn_wake_gpio = mk_button("Wake GPIO", p);
    set_state(btn_wake_gpio, false);
    btn_wake_rtc = mk_button("Wake RTC", p);
    set_state(btn_wake_rtc, false);
    lbl_gpio_action = mk_label("", p, SS_ENDELLIPSIS);
    bind(var_gpio_action, Var::LABEL, lbl_gpio_action, false);
    var_gpio_action.set("Select a configured GPIO input to simulate its button.");
    btn_gpio_press = mk_button("Press", p);
    set_state(btn_gpio_press, false);

    /* --- UART2 --- */
    grp_u2 = mk(L"BUTTON", "UART2 - firmware debug log", BS_GROUPBOX, p);
    mk_pane(txt2, p, true);
    chk_hide_tick = mk_check(std::string("suppress ") + QUICKTICK, p);
    bind(var_hide_tick, Var::CHECK, chk_hide_tick, false);
    btn_copy2 = mk_button("Copy", p);

    /* --- network pane --- */
    net_frame = mk(L"BUTTON", NET_TITLE, BS_GROUPBOX, p);
    mk_pane(txt_net, p, true);
    txt_net.tags.push_back(std::make_pair(std::string("wifi"), RGB(0x8a, 0xb4, 0xf8)));
    txt_net.tags.push_back(std::make_pair(std::string("net"), RGB(0x7f, 0xd6, 0xa0)));
    txt_net.tags.push_back(std::make_pair(std::string("payload"), RGB(0xe8, 0xc0, 0x6a)));
    /* "sent" shares its ink with "net"; the network pane never uses it */
    txt_net.tags.erase(txt_net.tags.begin());
    SendMessageW(txt_net.hwnd, EM_SETEVENTMASK, 0, ENM_MOUSEEVENTS);
    chk_ip_only = mk_check("IP traffic only (hide Wi-Fi link)", p);
    bind(var_net_ip_only, Var::CHECK, chk_ip_only, false);
    btn_copy_net = mk_button("Copy", p);
    btn_details = mk_button("Details (bytes)...", p);
    append_utf8(txt_net, NET_IDLE_HINT + "\n", "note");
    net_hint_shown = true;

    /* --- send bar --- */
    grp_send = mk(L"BUTTON", "send to UART1", BS_GROUPBOX, p);
    entry = mk_edit(p, true);
    bind(var_input, Var::EDIT, entry, false);
    StrList modes;
    modes.push_back("ASCII");
    modes.push_back("HEX");
    cmb_mode = mk_combo(p, modes);
    bind(var_mode, Var::COMBO, cmb_mode, false);
    var_mode.set("ASCII");
    StrList endings;
    endings.push_back("CR+LF");
    endings.push_back("LF");
    endings.push_back("CR");
    endings.push_back("(none)");
    cmb_ending = mk_combo(p, endings);
    bind(var_ending, Var::COMBO, cmb_ending, false);
    var_ending.set("CR+LF");
    btn_send = mk_button("Send", p);
    set_state(btn_send, false);
    lbl_quick = mk_label("quick:", p);
    for (int i = 0; i < N_QUICK; i++) {
        HWND b = mk_button(QUICK_COMMANDS[i], p);
        set_state(b, false);
        quick_buttons.push_back(b);
    }

    ui_ready = false;
    build_config();
    build_tuya_config();
    ui_ready = true;
    options_changed();
}

/* ------------------------------------------------------------ Config tab */
void SimUI::build_config()
{
    HWND p = config_tab;
    grp_dev = mk(L"BUTTON", "Device", BS_GROUPBOX, p);
    grp_netcfg = mk(L"BUTTON", "Network", BS_GROUPBOX, p);

    struct L {
        static HWND note(SimUI *ui, HWND p, const std::string &text)
        {
            HWND h = ui->mk_label(text, p);
            ui->note_labels.insert(h);
            return h;
        }
    };
    /* check(box, row, name, text, help) */
    HWND w;
    CfgRow r;
#define CHECK_ROW(G, NAME, TEXT, HELP)                     \
    do {                                                   \
        r = CfgRow();                                      \
        r.group = G;                                       \
        w = mk_check(TEXT, p);                             \
        bind(*optv(NAME), Var::CHECK, w, true);            \
        SendMessageW(w, BM_SETCHECK, optv(NAME)->bshadow ? BST_CHECKED : BST_UNCHECKED, 0); \
        r.w0 = w;                                          \
        r.w0_check = true;                                 \
        r.spans = true;                                    \
        r.note = L::note(this, p, HELP);                   \
        cfg_rows.push_back(r);                             \
    } while (0)

    CHECK_ROW(0, "boot", "boot ROM", "Start in the bootloader at 0x0 instead of jumping into the app.");
    CHECK_ROW(0, "xvr_selfclear", "RF calibration self-clear",
              "Clear the radio calibration bit at 0x9000f8. Many stock dumps stall early without it.");
    CHECK_ROW(0, "blecore", "force BLE core",
              "Legacy override for N/M dumps using generic BK7231. BK7231N and BK7238 select their Bluetooth "
              "hardware automatically.");
    CHECK_ROW(0, "tuyamcu", "TuyaMCU peer", "Answer the TuyaMCU serial protocol on UART1, as the device's MCU would.");
    /* entry: product ID */
    r = CfgRow();
    r.group = 0;
    r.w0 = mk_label("product ID", p);
    r.indent = 18;
    HWND pid = mk_edit(p);
    bind(*optv("tuyamcu_pid"), Var::EDIT, pid, true);
    r.c1.push_back(std::make_pair(pid, 160));
    r.note = L::note(this, p, "The 16-character id the MCU reports; stock firmware checks it against its own. Empty "
                              "uses a built-in one.");
    cfg_rows.push_back(r);
    r = CfgRow();
    r.group = 0;
    HWND raw = mk_check("raw product info", p);
    bind(*optv("tuyamcu_raw"), Var::CHECK, raw, true);
    r.w0 = raw;
    r.w0_check = true;
    r.spans = true;
    r.indent = 18;
    r.note = L::note(this, p, "Send product info in the raw 16-byte form newer TuyaOS needs, not JSON.");
    cfg_rows.push_back(r);
    std::vector<HWND> tw;
    tw.push_back(pid);
    tw.push_back(raw);
    opt_widgets.push_back(std::make_pair(std::string("tuyamcu"), tw));

    CHECK_ROW(1, "wifi", "Wi-Fi peer",
              "Attach a virtual radio peer and IP network. Outbound traffic is relayed through this computer by "
              "default; clear the option below for an offline sinkhole.");
    CHECK_ROW(1, "net_out", "real internet / Tuya cloud",
              "Relay the firmware's outbound DNS, TCP and UDP through this computer. This does not pair or add a "
              "device. It only reconnects an already-bound identity; if its credentials remain valid and the app "
              "uses the same account and region, that existing device may come online. Use only identities you "
              "own; keep the physical device offline to avoid a duplicate connection. Frame history stays complete, "
              "but real-cloud TLS/MQTT payloads remain encrypted.");
#undef CHECK_ROW
    HWND net_out = w;
    /* role */
    r = CfgRow();
    r.group = 1;
    r.w0 = mk_label("role", p);
    r.indent = 18;
    StrList roles;
    for (int i = 0; WIFI_ROLES[i]; i++)
        roles.push_back(WIFI_ROLES[i]);
    HWND role = mk_combo(p, roles);
    bind(*optv("wifi_role"), Var::COMBO, role, true);
    optv("wifi_role")->set(OPTION_DEFAULTS[8].sdefault);
    r.c1.push_back(std::make_pair(role, 80));
    r.note = L::note(this, p, "auto takes the opposite role to whatever the device turns out to be.");
    cfg_rows.push_back(r);
    /* network name */
    r = CfgRow();
    r.group = 1;
    r.w0 = mk_label("network name", p);
    r.indent = 18;
    HWND ssid = mk_edit(p);
    bind(*optv("wifi_ssid"), Var::EDIT, ssid, true);
    r.c1.push_back(std::make_pair(ssid, 160));
    r.note = L::note(this, p, "Empty adopts whichever network the device asks for.");
    cfg_rows.push_back(r);
    /* passphrase */
    r = CfgRow();
    r.group = 1;
    r.w0 = mk_label("passphrase", p);
    r.indent = 18;
    HWND pw = mk(L"EDIT", "", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, p, WS_EX_CLIENTEDGE);
    SendMessageW(pw, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
    bind(*optv("wifi_pass"), Var::EDIT, pw, true);
    r.c1.push_back(std::make_pair(pw, 160));
    r.note = L::note(this, p, "Makes the access point WPA2 protected. A paired device only joins with the passphrase "
                              "it stored. Not saved between launches.");
    cfg_rows.push_back(r);
    /* forward */
    r = CfgRow();
    r.group = 1;
    r.w0 = mk_label("forward", p);
    r.indent = 18;
    HWND l1 = mk_label("127.0.0.1:", p);
    HWND f_local = mk_edit(p);
    bind(*optv("fwd_local"), Var::EDIT, f_local, true);
    optv("fwd_local")->set(OPTION_DEFAULTS[11].sdefault);
    HWND l2 = mk_label(" to device:", p);
    HWND f_dev = mk_edit(p);
    bind(*optv("fwd_device"), Var::EDIT, f_dev, true);
    optv("fwd_device")->set(OPTION_DEFAULTS[12].sdefault);
    btn_open_forward = mk_button("Open in browser", p);
    set_state(btn_open_forward, false);
    r.c1.push_back(std::make_pair(l1, text_w("127.0.0.1:") + 2));
    r.c1.push_back(std::make_pair(f_local, 52));
    r.c1.push_back(std::make_pair(l2, text_w(" to device:") + 2));
    r.c1.push_back(std::make_pair(f_dev, 44));
    r.c1.push_back(std::make_pair(btn_open_forward, text_w("Open in browser") + 24));
    r.note = L::note(this, p, "A local port forwarded to a device port, e.g. its web panel. Clear both to turn it "
                              "off. Open works while that forward is running.");
    cfg_rows.push_back(r);
    /* radio capture */
    r = CfgRow();
    r.group = 1;
    HWND pcap = mk_check("radio capture", p);
    bind(*optv("pcap"), Var::CHECK, pcap, true);
    r.w0 = pcap;
    r.w0_check = true;
    r.indent = 18;
    HWND p_entry = mk_edit(p);
    bind(*optv("pcap_path"), Var::EDIT, p_entry, true);
    pcap_browse = mk_button("...", p);
    r.c1.push_back(std::make_pair(p_entry, 120));
    r.c1.push_back(std::make_pair(pcap_browse, 28));
    lbl_radio_capture_note = L::note(this, p,
                                     std::string("Save every 802.11 frame to a pcap file for Wireshark. The IP-level "
                                                 "capture is in the network pane's Details window. ")
                                         + FRAME_PRIVACY_NOTE);
    r.note = lbl_radio_capture_note;
    cfg_rows.push_back(r);
    /* the two multi-line boxes */
    const char *tnames[2] = {"http_gets", "mqtt_pubs"};
    const char *tlabels[2] = {"HTTP GET", "MQTT messages"};
    const char *thelp[2] = {"One path per line, requested from the device's port 80 once it is reachable. E.g. "
                            "/cm?cmnd=POWER%20ON",
                            "One TOPIC=PAYLOAD per line, pushed to the device once it subscribes to a matching topic."};
    HWND boxes[2];
    for (int i = 0; i < 2; i++) {
        r = CfgRow();
        r.group = 1;
        r.w0 = mk_label(tlabels[i], p);
        r.indent = 18;
        HWND box = mk(L"EDIT", "", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, p,
                      WS_EX_CLIENTEDGE);
        SendMessageW(box, WM_SETFONT, (WPARAM)font_mono, FALSE);
        bind(*optv(tnames[i]), Var::MULTI, box, true);
        opt_text[tnames[i]] = box;
        boxes[i] = box;
        r.c1.push_back(std::make_pair(box, 230));
        r.c1_h = 58;
        r.note = L::note(this, p, thelp[i]);
        cfg_rows.push_back(r);
    }
    std::vector<HWND> ww;
    ww.push_back(net_out);
    ww.push_back(role);
    ww.push_back(ssid);
    ww.push_back(pw);
    ww.push_back(f_local);
    ww.push_back(f_dev);
    ww.push_back(pcap);
    ww.push_back(p_entry);
    ww.push_back(pcap_browse);
    ww.push_back(boxes[0]);
    ww.push_back(boxes[1]);
    opt_widgets.push_back(std::make_pair(std::string("wifi"), ww));
    std::vector<HWND> pw2;
    pw2.push_back(p_entry);
    pw2.push_back(pcap_browse);
    opt_widgets.push_back(std::make_pair(std::string("pcap"), pw2));

    btn_reset = mk_button("Reset to defaults", p);
    lbl_cfg_note = mk_label("", p);
    note_labels.insert(lbl_cfg_note);
    bind(var_cfg_note, Var::LABEL, lbl_cfg_note, false);
}

void SimUI::build_tuya_config()
{
    HWND p = tuya_tab;
    lbl_tuya_status = mk_label("", p, SS_ENDELLIPSIS);
    bind(var_tuya_status, Var::LABEL, lbl_tuya_status, false);
    var_tuya_status.set("No image scanned yet.");
    btn_tuya_refresh = mk_button("Refresh", p);
    grp_tuya_sum = mk(L"BUTTON", "Decoded summary (credentials masked)", BS_GROUPBOX, p);
    mk_table(tuya_tree, p);
    table_column(tuya_tree, "Field", 180, false);
    table_column(tuya_tree, "Value", 390, false);
    grp_tuya_json = mk(L"BUTTON", "Decoded user_param_key JSON (no credentials)", BS_GROUPBOX, p);
    mk_pane(txt_tuya_json, p, true);
}

void SimUI::select_tab(int idx)
{
    tab_index = idx;
    SendMessageW(tabs, TCM_SETCURSEL, (WPARAM)idx, 0);
    ShowWindow(run_tab, idx == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(config_tab, idx == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(tuya_tab, idx == 2 ? SW_SHOW : SW_HIDE);
    HWND pg = idx == 0 ? run_tab : (idx == 1 ? config_tab : tuya_tab);
    SetWindowPos(pg, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

/* ------------------------------------------------------- details window */
void SimUI::open_details()
{
    if (detail_exists()) {
        ShowWindow(detail_win, hidden ? SW_HIDE : SW_RESTORE);
        if (!hidden)
            SetForegroundWindow(detail_win);
        refresh_details();
        return;
    }
    detail_win = CreateWindowExW(0, DETAIL_CLASS, L"captured traffic - every frame the IP bridge carried",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(980), S(640),
                                 0, 0, g_inst, this);
    HWND p = detail_win;
    lbl_filter = mk_label("filter:", p);
    ent_filter = mk_edit(p);
    bind(var_frame_filter, Var::EDIT, ent_filter, false);
    var_frame_filter.set("");
    btn_apply = mk_button("Apply", p);
    btn_refresh_d = mk_button("Refresh", p);
    lbl_target = mk_label("target host:", p);
    StrList t(1, ALL_FRAME_TARGETS);
    frame_target = mk_combo(p, t);
    frame_target_values = t;
    bind(var_frame_target, Var::COMBO, frame_target, false);
    var_frame_target.set(ALL_FRAME_TARGETS);
    btn_save_pcap = mk_button("Save .pcap...", p);
    lbl_frame_count = mk_label("", p, SS_ENDELLIPSIS);
    bind(var_frame_count, Var::LABEL, lbl_frame_count, false);
    lbl_frame_privacy = mk_label("", p, SS_ENDELLIPSIS);
    bind(var_frame_privacy, Var::LABEL, lbl_frame_privacy, false);
    var_frame_privacy.set(FRAME_PRIVACY_NOTE);
    frame_tree = Table();
    mk_table(frame_tree, p);
    table_column(frame_tree, "#", 52, true);
    table_column(frame_tree, "Time", 86, true);
    table_column(frame_tree, "Direction", 92, false);
    table_column(frame_tree, "Bytes", 56, true);
    table_column(frame_tree, "Frame", 520, false);
    frame_tree.tag_colors.push_back(std::make_pair(std::string("in"), RGB(0x0b, 0x6b, 0x3a)));
    frame_tree.tag_colors.push_back(std::make_pair(std::string("out"), RGB(0x14, 0x44, 0x8f)));
    txt_bytes = Pane();
    mk_pane(txt_bytes, p, false);
    btn_copy_bytes = mk_button("Copy these bytes", p);
    frame_vars_made = true;
    layout_details();
    if (!hidden) {
        ShowWindow(detail_win, SW_SHOW);
        UpdateWindow(detail_win);
    }
    refresh_details();
}

/* ------------------------------------------------------------ dialogs */
void SimUI::message(const std::string &msg, bool error)
{
    last_message = msg;
    if (hooks.show_message) {
        hooks.show_message("BekenEmulator", msg, error, hooks.show_message_ctx);
        return;
    }
    MessageBoxW(root, widen(msg).c_str(), L"BekenEmulator", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

std::string SimUI::ask_save(const char *ext, const wchar_t *filter)
{
    if (hooks.ask_save)
        return hooks.ask_save(ext, hooks.ask_save_ctx);
    wchar_t buf[MAX_PATH * 4] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = detail_exists() && GetForegroundWindow() == detail_win ? detail_win : root;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    std::wstring de = widen(ext[0] == '.' ? ext + 1 : ext);
    ofn.lpstrDefExt = de.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return "";
    return narrow(buf);
}

std::string SimUI::ask_open()
{
    wchar_t buf[MAX_PATH * 4] = {0};
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = root;
    ofn.lpstrFilter = L"Flash dumps (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    std::wstring dir = widen(path_isdir(FIRMWARE_DIR) ? FIRMWARE_DIR : ROOT_DIR);
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return "";
    return narrow(buf);
}

bool SimUI::browser(const std::string &url)
{
    if (hooks.browser_open)
        return hooks.browser_open(url, 2, hooks.browser_ctx);
    HINSTANCE r = ShellExecuteW(root, L"open", widen(url).c_str(), 0, 0, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

/* ------------------------------------------------------------ layout */
static void place(HWND h, int x, int y, int w, int hh)
{
    if (h)
        MoveWindow(h, x, y, w > 0 ? w : 0, hh > 0 ? hh : 0, TRUE);
}

/* ttk's stretch=True column: it takes whatever the others leave */
static void stretch_column(Table &t, int col)
{
    if (!t.hwnd)
        return;
    RECT rc;
    GetClientRect(t.hwnd, &rc);
    int used = 0;
    for (int c = 0; c < t.ncols; c++)
        if (c != col)
            used += (int)SendMessageW(t.hwnd, LVM_GETCOLUMNWIDTH, (WPARAM)c, 0);
    int w = rc.right - used - GetSystemMetrics(SM_CXVSCROLL);
    if (w < 60)
        w = 60;
    SendMessageW(t.hwnd, LVM_SETCOLUMNWIDTH, (WPARAM)col, w);
}

void SimUI::layout()
{
    if (!root)
        return;
    RECT rc;
    GetClientRect(root, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(6), rh = S(24), bw = S(84);
    int y = m;
    /* image group */
    int gh = S(52);
    place(grp_image, m, y, W - 2 * m, gh);
    int iy = y + S(20), x = m + S(8), right = W - m - S(8);
    int ck = S(96);
    place(cmb_key, right - ck, iy, ck, S(300));
    right -= ck + S(4);
    int lw = text_w("key") + S(4);
    place(lbl_key, right - lw, iy + S(4), lw, S(18));
    right -= lw + S(10);
    place(cmb_chip, right - ck, iy, ck, S(300));
    right -= ck + S(4);
    lw = text_w("chip") + S(4);
    place(lbl_chip, right - lw, iy + S(4), lw, S(18));
    right -= lw + S(10);
    place(btn_browse, right - bw, iy - 1, bw, rh + 1);
    right -= bw + S(4);
    place(image_entry, x, iy, right - x, S(23));
    y += gh + S(3);
    /* quick-run rows: 10 buttons per row after the heading */
    for (size_t g = 0; g < interesting_rows.size(); g++) {
        int lx = text_w(window_text(interesting_group_labels[g])) + S(8);
        place(interesting_group_labels[g], m, y + S(4), lx, S(18));
        int bx = m + lx, rows = 0;
        for (size_t i = 0; i < interesting_rows[g].size(); i++) {
            if (i && i % 10 == 0) {
                rows++;
                bx = m + lx;
            }
            int w = text_w(window_text(interesting_rows[g][i])) + S(20);
            place(interesting_rows[g][i], bx, y + rows * (rh + S(2)), w, rh);
            bx += w + S(4);
        }
        y += (rows + 1) * (rh + S(2)) + S(1);
    }
    /* run row */
    x = m;
    place(btn_run, x, y, bw, rh);
    x += bw + S(4);
    place(btn_pause, x, y, bw, rh);
    x += bw + S(4);
    place(btn_stop, x, y, bw, rh);
    x += bw + S(12);
    place(btn_clear, x, y, bw, rh);
    x += bw + S(4);
    place(btn_save, x, y, bw, rh);
    x += bw + S(12);
    place(lbl_summary, x, y + S(4), W - m - x, S(18));
    y += rh + S(4);
    place(lbl_state, m, y, W - 2 * m, S(24));
    y += S(28);
    /* the notebook */
    place(tabs, m, y, W - 2 * m, H - y - m);
    RECT tr = {0, 0, W - 2 * m, H - m - y};
    SendMessageW(tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    HWND pages[3] = {run_tab, config_tab, tuya_tab};
    for (int i = 0; i < 3; i++)
        SetWindowPos(pages[i], i == tab_index ? HWND_TOP : 0, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top,
                     i == tab_index ? 0 : SWP_NOZORDER);
    layout_run();
    layout_config();
    layout_tuya();
}

void SimUI::layout_run()
{
    RECT rc;
    GetClientRect(run_tab, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(4), rh = S(24), pad = S(8), title = S(18);
    /* send bar: packed at the bottom so a short window squeezes the logs */
    int qrows = (N_QUICK + QUICK_PER_ROW - 1) / QUICK_PER_ROW;
    int send_h = title + rh + S(6) + qrows * (rh + S(2)) + S(8);
    int sy = H - send_h - S(4);
    place(grp_send, m, sy, W - 2 * m, send_h);
    int ix = m + pad, iw = W - 2 * m - 2 * pad, iy = sy + title;
    int bw = S(70);
    place(btn_send, ix + iw - bw, iy - 1, bw, rh + 1);
    place(cmb_ending, ix + iw - bw - S(6) - S(84), iy, S(84), S(200));
    place(cmb_mode, ix + iw - bw - S(10) - S(84) - S(74), iy, S(74), S(200));
    place(entry, ix, iy, iw - bw - S(16) - S(84) - S(74), S(23));
    iy += rh + S(6);
    int qlw = text_w("quick:") + S(8);
    place(lbl_quick, ix, iy + S(4), qlw, S(18));
    std::vector<int> colw(QUICK_PER_ROW, 0);
    for (int i = 0; i < N_QUICK; i++) {
        int w = text_w(QUICK_COMMANDS[i]) + S(20);
        if (w > colw[i % QUICK_PER_ROW])
            colw[i % QUICK_PER_ROW] = w;
    }
    for (int i = 0; i < N_QUICK; i++) {
        int col = i % QUICK_PER_ROW, row = i / QUICK_PER_ROW;
        int bx = ix + qlw;
        for (int c = 0; c < col; c++)
            bx += colw[c] + S(4);
        place(quick_buttons[i], bx, iy + row * (rh + S(2)), text_w(QUICK_COMMANDS[i]) + S(20), rh);
    }

    /* the split: three columns over the network pane */
    int ah = sy - S(4) - m;
    split_area.left = m;
    split_area.top = m;
    split_area.right = W - m;
    split_area.bottom = m + ah;
    int gap = S(6);
    int top_h = (int)((ah - gap) * split_net);
    if (top_h < S(120))
        top_h = S(120);
    if (top_h > ah - gap - S(80))
        top_h = ah - gap - S(80);
    int aw = W - 2 * m;
    int x1 = m + (int)((aw - 2 * gap) * split_cols[0]);
    int x2 = m + gap + (int)((aw - 2 * gap) * split_cols[1]);
    int c1w = x1 - m, c2w = x2 - (x1 + gap), c3w = W - m - (x2 + gap);
    SetRect(&gap_rc[0], x1, m, x1 + gap, m + top_h);
    SetRect(&gap_rc[1], x2, m, x2 + gap, m + top_h);
    SetRect(&gap_rc[2], m, m + top_h, W - m, m + top_h + gap);

    /* one log column: group, pane, a control row under it */
    struct Col {
        static void log(SimUI *ui, HWND grp, Pane &p, HWND chk, HWND copy, int x, int y, int w, int h)
        {
            int pad = ui->S(8), title = ui->S(18), rh = ui->S(24);
            place(grp, x, y, w, h);
            int iw = w - 2 * pad;
            int by = y + h - pad - rh + ui->S(2);
            place(p.hwnd, x + pad, y + title, iw, by - S2(ui) - (y + title));
            place(chk, x + pad, by + ui->S(2), iw - ui->S(70), ui->S(20));
            place(copy, x + pad + iw - ui->S(60), by, ui->S(60), rh);
        }
        static int S2(SimUI *ui) { return ui->S(4); }
    };
    Col::log(this, grp_u1, txt1, chk_hex, btn_copy1, m, m, c1w, top_h);
    Col::log(this, grp_u2, txt2, chk_hide_tick, btn_copy2, x1 + gap, m, c2w, top_h);

    /* GPIO / PWM column, bottom-up as Tk packs it */
    int gx = x2 + gap, gy = m;
    place(grp_peri, gx, gy, c3w, top_h);
    int px = gx + pad, pw = c3w - 2 * pad;
    int by = gy + top_h - pad;
    by -= S(20);
    place(chk_all_pins, px, by, pw, S(20));
    int hp = text_h(var_pwm.get(), pw) + S(2);
    if (hp > S(48))
        hp = S(48);
    by -= hp + S(2);
    place(lbl_pwm, px, by, pw, hp);
    /* ADC: label, then inputs */
    by -= rh + S(2);
    {
        int ax = px;
        int clw = text_w("Ch:") + S(4), cw = S(78), vlw = text_w("Val:") + S(4), vw = S(64), abw = S(74);
        place(lbl_adc_ch, ax, by + S(4), clw, S(18));
        ax += clw;
        place(cmb_adc_ch, ax, by + 1, cw, S(200));
        ax += cw + S(6);
        place(lbl_adc_val, ax, by + S(4), vlw, S(18));
        ax += vlw;
        place(ent_adc_val, ax, by + 1, vw, S(22));
        place(btn_adc_apply, px + pw - abw, by, abw, rh);
    }
    int ha = text_h(var_adc.get(), pw) + S(2);
    if (ha > S(34))
        ha = S(34);
    by -= ha + S(2);
    place(lbl_adc, px, by, pw, ha);
    /* meter: label, then inputs */
    by -= rh + S(2);
    int mx = px;
    int lw = text_w("W:") + S(4), ew = S(52);
    place(lbl_mv, mx, by + S(4), lw, S(18));
    mx += lw;
    place(ent_mv, mx, by + 1, ew, S(22));
    mx += ew + S(6);
    place(lbl_ma, mx, by + S(4), lw, S(18));
    mx += lw;
    place(ent_ma, mx, by + 1, ew, S(22));
    mx += ew + S(6);
    place(lbl_mw, mx, by + S(4), lw, S(18));
    mx += lw;
    place(ent_mw, mx, by + 1, ew, S(22));
    int mbw = S(80);
    place(btn_meter_apply, px + pw - mbw, by, mbw, rh);
    int hm = text_h(var_meter.get(), pw) + S(2);
    if (hm > S(34))
        hm = S(34);
    by -= hm + S(2);
    place(lbl_meter, px, by, pw, hm);
    /* LED swatch row */
    by -= S(32) + S(4);
    place(led_swatch, px, by + S(5), S(38), S(22));
    place(lbl_led, px + S(44), by, pw - S(44), S(32));
    /* camera row */
    by -= S(50) + S(4);
    place(cam_canvas, px, by, S(66), S(50));
    place(lbl_camera, px + S(72), by, pw - S(72), S(50));
    /* sleep row */
    by -= rh + S(4);
    int wbw = S(84);
    place(btn_wake_gpio, px + pw - wbw, by, wbw, rh);
    place(btn_wake_rtc, px + pw - 2 * wbw - S(4), by, wbw, rh);
    place(lbl_sleep, px, by + S(1), pw - 2 * wbw - S(10), rh);
    /* gpio control row */
    by -= rh + S(4);
    int pbw = S(100);
    place(btn_gpio_press, px + pw - pbw, by, pbw, rh);
    place(lbl_gpio_action, px, by + S(4), pw - pbw - S(6), S(18));
    place(tree.hwnd, px, gy + title, pw, by - S(4) - (gy + title));
    stretch_column(tree, 2);

    /* network pane */
    int ny = m + top_h + gap, nh = ah - top_h - gap;
    place(net_frame, m, ny, aw, nh);
    int nb = ny + nh - pad - rh + S(2);
    place(txt_net.hwnd, m + pad, ny + title, aw - 2 * pad, nb - S(4) - (ny + title));
    place(chk_ip_only, m + pad, nb + S(2), S(260), S(20));
    place(btn_copy_net, m + aw - pad - S(60), nb, S(60), rh);
    place(btn_details, m + aw - pad - S(60) - S(6) - S(130), nb, S(130), rh);
    InvalidateRect(run_tab, 0, TRUE);
}

/* One Config group's rows: returns its height; places them when asked. */
int SimUI::layout_cfg_group(int g, int gx, int gy, int gw, bool do_place)
{
    int pad = S(8), title = S(20), rh = S(24);
    int c0 = 0, c1 = 0;
    for (size_t i = 0; i < cfg_rows.size(); i++) {
        const CfgRow &r = cfg_rows[i];
        if (r.group != g)
            continue;
        int w0 = S(r.indent) + text_w(window_text(r.w0)) + (r.w0_check ? S(22) : S(6));
        int w1 = 0;
        for (size_t k = 0; k < r.c1.size(); k++)
            w1 += S(r.c1[k].second) + (k ? S(2) : 0);
        if (!r.spans && w0 > c0)
            c0 = w0;
        if (w1 > c1)
            c1 = w1;
    }
    for (size_t i = 0; i < cfg_rows.size(); i++) {
        const CfgRow &r = cfg_rows[i];
        if (r.group != g || !r.spans)
            continue;
        int w0 = S(r.indent) + text_w(window_text(r.w0)) + S(22);
        if (w0 > c0 + c1)
            c1 = w0 - c0;
    }
    int nx = gx + pad + c0 + c1 + S(10);
    int nw = gx + gw - pad - nx;
    if (nw < S(120))
        nw = S(120);
    int y = gy + title;
    for (size_t i = 0; i < cfg_rows.size(); i++) {
        const CfgRow &r = cfg_rows[i];
        if (r.group != g)
            continue;
        int ch = r.c1_h ? S(r.c1_h) : rh;
        int nh = text_h(window_text(r.note), nw) + S(4);
        int row_h = ch > nh ? ch : nh;
        if (row_h < rh)
            row_h = rh;
        if (do_place) {
            int x0 = gx + pad + S(r.indent);
            if (r.spans)
                place(r.w0, x0, y + S(2), c0 + c1 - S(r.indent), S(20));
            else
                place(r.w0, x0, y + S(4), c0 - S(r.indent), S(20));
            int x1 = gx + pad + c0;
            for (size_t k = 0; k < r.c1.size(); k++) {
                HWND h = r.c1[k].first;
                int w = S(r.c1[k].second);
                wchar_t cls[32];
                GetClassNameW(h, cls, 32);
                if (!lstrcmpiW(cls, L"ComboBox"))
                    place(h, x1, y + 1, w, S(200));
                else if (!lstrcmpiW(cls, L"Static"))
                    place(h, x1, y + S(4), w, S(18));
                else if (!lstrcmpiW(cls, L"Button"))
                    place(h, x1, y, w, rh);
                else
                    place(h, x1, y + (r.c1_h ? 0 : 1), w, r.c1_h ? ch : S(23));
                x1 += w + S(2);
            }
            place(r.note, nx, y + S(3), nw, nh);
        }
        y += row_h + S(4);
    }
    return y - gy + pad;
}

void SimUI::layout_config()
{
    RECT rc;
    GetClientRect(config_tab, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(8), gap = S(6), rh = S(24);
    /* Side by side, as the Tk grid has them, when both groups keep a
     * readable note column; otherwise one above the other. */
    int half = (W - 2 * m - gap) / 2;
    bool side = true;
    for (int g = 0; g < 2; g++) {
        int c = 0;
        for (size_t i = 0; i < cfg_rows.size(); i++)
            if (cfg_rows[i].group == g) {
                int w = 0;
                for (size_t k = 0; k < cfg_rows[i].c1.size(); k++)
                    w += S(cfg_rows[i].c1[k].second) + S(2);
                if (w > c)
                    c = w;
            }
        if (S(16) + S(130) + c + S(10) + S(260) > half)
            side = false;
    }
    int gx[2], gy[2], gw[2], gh[2];
    if (side) {
        gw[0] = gw[1] = half;
        gx[0] = m;
        gx[1] = m + half + gap;
        gh[0] = layout_cfg_group(0, gx[0], 0, gw[0], false);
        gh[1] = layout_cfg_group(1, gx[1], 0, gw[1], false);
        gh[0] = gh[1] = gh[0] > gh[1] ? gh[0] : gh[1];
        gy[0] = gy[1] = m;
    } else {
        gw[0] = gw[1] = W - 2 * m;
        gx[0] = gx[1] = m;
        gh[0] = layout_cfg_group(0, m, 0, gw[0], false);
        gh[1] = layout_cfg_group(1, m, 0, gw[1], false);
        gy[0] = m;
        gy[1] = m + gh[0] + gap;
    }
    int total = gy[1] + gh[1] + S(10) + rh + m;
    /* scroll when the page is shorter than the options */
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    if (cfg_scroll > total - H)
        cfg_scroll = total - H > 0 ? total - H : 0;
    si.nMin = 0;
    si.nMax = total > H ? total - 1 : 0;
    si.nPage = total > H ? (UINT)H : 0;
    si.nPos = cfg_scroll;
    SetScrollInfo(config_tab, SB_VERT, &si, TRUE);
    ShowScrollBar(config_tab, SB_VERT, total > H);
    int off = -cfg_scroll;
    for (int g = 0; g < 2; g++) {
        layout_cfg_group(g, gx[g], gy[g] + off, gw[g], true);
        place(g ? grp_netcfg : grp_dev, gx[g], gy[g] + off, gw[g], gh[g]);
    }
    int fy = gy[1] + gh[1] + S(10) + off;
    int bw = text_w("Reset to defaults") + S(24);
    place(btn_reset, m, fy, bw, rh);
    place(lbl_cfg_note, m + bw + S(12), fy + S(4), W - 2 * m - bw - S(12), S(18));
    InvalidateRect(config_tab, 0, TRUE);
}

void SimUI::layout_tuya()
{
    RECT rc;
    GetClientRect(tuya_tab, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(8), rh = S(24), pad = S(8), title = S(20), gap = S(6);
    place(btn_tuya_refresh, W - m - S(84), m, S(84), rh);
    place(lbl_tuya_status, m, m + S(4), W - 3 * m - S(84), S(18));
    int y = m + rh + S(6), h = H - y - m;
    int gw = (W - 2 * m - gap) / 2;
    place(grp_tuya_sum, m, y, gw, h);
    place(tuya_tree.hwnd, m + pad, y + title, gw - 2 * pad, h - title - pad);
    stretch_column(tuya_tree, 1);
    place(grp_tuya_json, m + gw + gap, y, gw, h);
    place(txt_tuya_json.hwnd, m + gw + gap + pad, y + title, gw - 2 * pad, h - title - pad);
    InvalidateRect(tuya_tab, 0, TRUE);
}

void SimUI::layout_details()
{
    if (!detail_exists())
        return;
    RECT rc;
    GetClientRect(detail_win, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(6), rh = S(24);
    int y = m, x = m;
    int lw = text_w("filter:") + S(4);
    place(lbl_filter, x, y + S(4), lw, S(18));
    x += lw;
    int right = W - m;
    int sw = S(104);
    place(btn_save_pcap, right - sw, y, sw, rh);
    right -= sw + S(8);
    place(frame_target, right - S(130), y + 1, S(130), S(240));
    right -= S(130) + S(4);
    int tw = text_w("target host:") + S(4);
    place(lbl_target, right - tw, y + S(4), tw, S(18));
    right -= tw + S(8);
    place(btn_refresh_d, right - S(70), y, S(70), rh);
    right -= S(70) + S(4);
    place(btn_apply, right - S(62), y, S(62), rh);
    right -= S(62) + S(6);
    place(ent_filter, x, y + 1, right - x, S(22));
    y += rh + S(6);
    place(lbl_frame_count, m + S(2), y, W - 2 * m, S(18));
    y += S(18);
    place(lbl_frame_privacy, m + S(2), y, W - 2 * m, S(18));
    y += S(22);
    int bottom = H - m - rh - S(6);
    int lh = (bottom - y) * 2 / 3;
    place(frame_tree.hwnd, m, y, W - 2 * m, lh);
    stretch_column(frame_tree, 4);
    place(txt_bytes.hwnd, m, y + lh + S(4), W - 2 * m, bottom - (y + lh + S(4)));
    int cw = S(130);
    place(btn_copy_bytes, W - m - cw, H - m - rh, cw, rh);
}

/* ------------------------------------------------------------ messages */
static void paint_swatch(SimUI *ui, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH border = CreateSolidBrush(NOTE_INK);
    if (h == ui->led_swatch) {
        HBRUSH bg = CreateSolidBrush(hexcolor(ui->swatch_bg));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT in = {1, 1, rc.right - 1, rc.bottom - 1};
        int n = (int)ui->swatch_pixels.size(), w = in.right - in.left;
        for (int i = 0; i < n; i++) {
            int x0 = in.left + (int)(i * w / (double)n + 0.5);
            int x1 = in.left + (int)((i + 1) * w / (double)n + 0.5);
            if (x1 <= x0)
                x1 = x0 + 1;
            RECT r = {x0, in.top, x1, in.bottom};
            HBRUSH b = CreateSolidBrush(hexcolor(ui->swatch_pixels[(size_t)i]));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
    } else {
        HBRUSH bg = CreateSolidBrush(CON_BG);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (ui->cam_rgb.size() == 64 * 48 * 3) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = 64;
            bi.bmiHeader.biHeight = -48;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;
            std::string bgr(ui->cam_rgb);
            for (size_t i = 0; i + 2 < bgr.size(); i += 3)
                std::swap(bgr[i], bgr[i + 2]);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 1, 1, rc.right - 2, rc.bottom - 2, 0, 0, 64, 48, bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        }
    }
    FrameRect(dc, &rc, border);
    DeleteObject(border);
    EndPaint(h, &ps);
}

static std::string pane_line_at(Pane &p, int x, int y)
{
    POINTL pt;
    pt.x = x;
    pt.y = y;
    long ci = (long)SendMessageW(p.hwnd, EM_CHARFROMPOS, 0, (LPARAM)&pt);
    std::string all = pane_text(p);
    std::wstring w = widen(all);
    if (ci < 0 || ci > (long)w.size())
        return "";
    size_t a = w.rfind(L'\n', ci > 0 ? (size_t)ci - 1 : 0);
    if (ci == 0 || a == std::wstring::npos)
        a = 0;
    else
        a++;
    size_t b = w.find(L'\n', (size_t)ci);
    if (b == std::wstring::npos)
        b = w.size();
    return narrow(w.substr(a, b - a));
}

LRESULT SimUI::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool &done)
{
    done = false;
    switch (msg) {
    case WM_COMMAND:
        if (lp)
            on_command((HWND)lp, HIWORD(wp));
        done = true;
        return 0;
    case WM_NOTIFY:
        return on_notify((NMHDR *)lp, done);
    case WM_TIMER:
        if (hwnd == root) {
            poll();
            done = true;
        }
        return 0;
    case WM_SIZE:
        if (hwnd == root)
            layout();
        else if (hwnd == detail_win)
            layout_details();
        return 0;
    case WM_GETMINMAXINFO:
        if (hwnd == root) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = S(1280) < GetSystemMetrics(SM_CXMAXTRACK) ? S(1100) : 800;
            mm->ptMinTrackSize.y = S(620);
            done = true;
        } else if (hwnd == detail_win) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = S(820);
            mm->ptMinTrackSize.y = S(480);
            done = true;
        }
        return 0;
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, 0, 0);
        StrList paths;
        for (UINT i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW(hd, i, buf, MAX_PATH * 4))
                paths.push_back(narrow(buf));
        }
        DragFinish(hd);
        on_drop(paths);
        done = true;
        return 0;
    }
    case WM_PAINT:
        if (hwnd == led_swatch || hwnd == cam_canvas) {
            paint_swatch(this, hwnd);
            done = true;
        }
        return 0;
    case WM_ERASEBKGND:
        if (hwnd == led_swatch || hwnd == cam_canvas) {
            done = true;
            return 1;
        }
        if (hwnd == run_tab || hwnd == config_tab || hwnd == tuya_tab || hwnd == root || hwnd == detail_win) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, br_face);
            done = true;
            return 1;
        }
        return 0;
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        done = true;
        return (LRESULT)br_face;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        if (note_labels.count(c) || c == lbl_frame_privacy) {
            SetTextColor(dc, c == lbl_frame_privacy ? PRIVACY_INK : NOTE_INK);
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            done = true;
            return (LRESULT)br_face;
        }
        for (std::map<std::string, HWND>::iterator it = opt_text.begin(); it != opt_text.end(); ++it)
            if (it->second == c) {
                /* a disabled box is drawn greyed, an enabled one as an editable white box */
                bool on = enabled(c);
                SetTextColor(dc, GetSysColor(on ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
                SetBkColor(dc, GetSysColor(on ? COLOR_WINDOW : COLOR_BTNFACE));
                done = true;
                return (LRESULT)(on ? br_window : br_face);
            }
        if (msg == WM_CTLCOLORSTATIC) {
            /* labels, checks and group boxes on the page colour */
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(dc, GetSysColor(enabled(c) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
            done = true;
            return (LRESULT)br_face;
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (hwnd == run_tab && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(run_tab, &pt);
            for (int i = 0; i < 3; i++)
                if (PtInRect(&gap_rc[i], pt)) {
                    SetCursor(LoadCursor(0, i < 2 ? IDC_SIZEWE : IDC_SIZENS));
                    done = true;
                    return TRUE;
                }
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (hwnd == run_tab) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            for (int i = 0; i < 3; i++)
                if (PtInRect(&gap_rc[i], pt)) {
                    drag = i + 1;
                    SetCapture(run_tab);
                    done = true;
                }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (hwnd == run_tab && drag) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            double aw = split_area.right - split_area.left, ah = split_area.bottom - split_area.top;
            if (drag == 1 || drag == 2) {
                double f = (pt.x - split_area.left) / (aw > 1 ? aw : 1);
                if (drag == 1) {
                    f = f < 0.1 ? 0.1 : f;
                    f = f > split_cols[1] - 0.1 ? split_cols[1] - 0.1 : f;
                    split_cols[0] = f;
                } else {
                    f = f < split_cols[0] + 0.1 ? split_cols[0] + 0.1 : f;
                    f = f > 0.9 ? 0.9 : f;
                    split_cols[1] = f;
                }
            } else {
                double f = (pt.y - split_area.top) / (ah > 1 ? ah : 1);
                split_net = f < 0.2 ? 0.2 : (f > 0.92 ? 0.92 : f);
            }
            layout_run();
            done = true;
        }
        return 0;
    case WM_VSCROLL:
    case WM_MOUSEWHEEL:
        if (hwnd == config_tab) {
            SCROLLINFO si;
            memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(config_tab, SB_VERT, &si);
            int pos = cfg_scroll;
            if (msg == WM_MOUSEWHEEL)
                pos -= (short)HIWORD(wp) * S(40) / WHEEL_DELTA;
            else
                switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= S(20); break;
                case SB_LINEDOWN: pos += S(20); break;
                case SB_PAGEUP: pos -= (int)si.nPage; break;
                case SB_PAGEDOWN: pos += (int)si.nPage; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = si.nMax; break;
                }
            int maxpos = si.nMax - (int)si.nPage + 1;
            if (pos > maxpos)
                pos = maxpos;
            if (pos < 0)
                pos = 0;
            if (pos != cfg_scroll) {
                cfg_scroll = pos;
                layout_config();
            }
            done = true;
        }
        return 0;
    case WM_LBUTTONUP:
        if (hwnd == run_tab && drag) {
            drag = 0;
            ReleaseCapture();
            done = true;
        }
        return 0;
    case WM_CLOSE:
        if (hwnd == root) {
            on_close();
            KillTimer(root, 1);
            DestroyWindow(root);
            done = true;
        } else if (hwnd == detail_win) {
            DestroyWindow(detail_win);
            done = true;
        }
        return 0;
    case WM_DESTROY:
        if (hwnd == detail_win) {
            /* Tk variables outlive their widgets: keep the values */
            Var *vs[4] = {&var_frame_filter, &var_frame_target, &var_frame_count, &var_frame_privacy};
            for (int i = 0; i < 4; i++) {
                std::string v = vs[i]->get();
                var_of.erase(vs[i]->hwnd);
                vs[i]->kind = Var::NONE;
                vs[i]->hwnd = 0;
                vs[i]->shadow = v;
            }
            detail_win = 0;
            frame_tree.hwnd = 0;
            txt_bytes.hwnd = 0;
        } else if (hwnd == root) {
            if (!hidden)
                PostQuitMessage(0);
        }
        return 0;
    }
    return 0;
}

void SimUI::on_command(HWND c, int code)
{
    /* the user changed a bound control: keep the variable and its traces in step */
    std::map<HWND, Var *>::iterator vi = var_of.find(c);
    if (vi != var_of.end()) {
        Var *v = vi->second;
        if (v->kind == Var::COMBO && code == CBN_SELCHANGE) {
            LRESULT i = SendMessageW(c, CB_GETCURSEL, 0, 0);
            if (i != CB_ERR) {
                LRESULT n = SendMessageW(c, CB_GETLBTEXTLEN, (WPARAM)i, 0);
                std::wstring w((size_t)n + 1, L'\0');
                SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)&w[0]);
                w.resize((size_t)n);
                v->shadow = narrow(w);
            }
            if (v->traced)
                options_changed();
            if (v == &var_frame_target)
                refresh_details();
            return;
        }
        if ((v->kind == Var::EDIT || v->kind == Var::MULTI) && code == EN_CHANGE) {
            if (!in_set && v->traced)
                options_changed();
            return;
        }
        if (v->kind == Var::CHECK && code == BN_CLICKED) {
            v->bshadow = v->getb();
            if (v->traced)
                options_changed();
            if (v == &var_all_pins)
                refresh_periph();
            return;
        }
    }
    if (c == image_entry && code == EN_KILLFOCUS) {
        refresh_tuya_config(false);
        return;
    }
    if (code != BN_CLICKED)
        return;
    for (size_t i = 0; i < interesting_buttons.size(); i++)
        if (c == interesting_buttons[i]) {
            run_interesting(interesting_index[i]);
            return;
        }
    for (size_t i = 0; i < quick_buttons.size(); i++)
        if (c == quick_buttons[i]) {
            send_command(QUICK_COMMANDS[i]);
            return;
        }
    if (c == btn_browse)
        browse();
    else if (c == btn_run)
        on_run();
    else if (c == btn_pause)
        on_pause();
    else if (c == btn_stop)
        on_stop();
    else if (c == btn_clear)
        clear();
    else if (c == btn_save)
        save();
    else if (c == btn_copy1)
        copy(txt1, "UART1");
    else if (c == btn_copy2)
        copy(txt2, "UART2");
    else if (c == btn_copy_net)
        copy(txt_net, "network");
    else if (c == btn_details)
        open_details();
    else if (c == btn_meter_apply)
        apply_energy_meter();
    else if (c == btn_adc_apply)
        apply_adc_input();
    else if (c == btn_wake_rtc)
        wake_deep_sleep("rtc");
    else if (c == btn_wake_gpio)
        wake_deep_sleep("gpio");
    else if (c == btn_gpio_press)
        toggle_gpio_input();
    else if (c == btn_send)
        on_send();
    else if (c == btn_open_forward)
        open_forward();
    else if (c == pcap_browse)
        browse_pcap();
    else if (c == btn_reset)
        reset_options();
    else if (c == btn_tuya_refresh)
        refresh_tuya_config(true);
    else if (c == btn_apply || c == btn_refresh_d)
        refresh_details();
    else if (c == btn_save_pcap)
        save_pcap();
    else if (c == btn_copy_bytes)
        copy(txt_bytes, "frame bytes");
}

static LRESULT table_customdraw(Table &t, NMLVCUSTOMDRAW *cd)
{
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        size_t i = (size_t)cd->nmcd.dwItemSpec;
        if (i < t.tags.size())
            for (size_t k = 0; k < t.tag_colors.size(); k++)
                if (t.tag_colors[k].first == t.tags[i])
                    cd->clrText = t.tag_colors[k].second;
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

LRESULT SimUI::on_notify(NMHDR *nm, bool &done)
{
    done = false;
    if (nm->hwndFrom == tabs && nm->code == TCN_SELCHANGE) {
        select_tab((int)SendMessageW(tabs, TCM_GETCURSEL, 0, 0));
        tab_changed();
        done = true;
        return 0;
    }
    Table *tables[3] = {&tree, &frame_tree, &tuya_tree};
    for (int i = 0; i < 3; i++) {
        Table &t = *tables[i];
        if (!t.hwnd || nm->hwndFrom != t.hwnd)
            continue;
        if (nm->code == NM_CUSTOMDRAW) {
            done = true;
            return table_customdraw(t, (NMLVCUSTOMDRAW *)nm);
        }
        if (nm->code == LVN_ITEMCHANGED && !t.quiet) {
            NMLISTVIEW *lv = (NMLISTVIEW *)nm;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)) {
                if (&t == &tree)
                    gpio_selection_changed();
                else if (&t == &frame_tree && (lv->uNewState & LVIS_SELECTED))
                    show_frame_bytes();
            }
        }
        if (nm->code == NM_DBLCLK && &t == &tree) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)nm;
            gpio_double_click(ia->iItem);
            done = true;
        }
        return 0;
    }
    if (nm->hwndFrom == txt_net.hwnd && nm->code == EN_MSGFILTER) {
        MSGFILTER *mf = (MSGFILTER *)nm;
        if (mf->msg == WM_LBUTTONDBLCLK) {
            std::string line = pane_line_at(txt_net, (short)LOWORD(mf->lParam), (short)HIWORD(mf->lParam));
            details_for_click(true, line);
            done = true;
            return 1;
        }
    }
    return 0;
}

bool SimUI::pre_translate(MSG &m)
{
    if (m.message == WM_KEYDOWN) {
        if (m.hwnd == image_entry && m.wParam == VK_RETURN) {
            refresh_tuya_config(true);
            return true;
        }
        if (m.hwnd == entry) {
            if (m.wParam == VK_RETURN) {
                on_send();
                return true;
            }
            if (m.wParam == VK_UP) {
                history_back();
                return true;
            }
            if (m.wParam == VK_DOWN) {
                history_fwd();
                return true;
            }
        }
        if ((m.hwnd == ent_mv || m.hwnd == ent_ma || m.hwnd == ent_mw) && m.wParam == VK_RETURN) {
            apply_energy_meter();
            return true;
        }
        if (m.hwnd == ent_adc_val && m.wParam == VK_RETURN) {
            apply_adc_input();
            return true;
        }
        if (detail_exists() && m.hwnd == ent_filter && m.wParam == VK_RETURN) {
            refresh_details();
            return true;
        }
    }
    if (m.message == WM_CHAR && m.wParam == L'\r'
        && (m.hwnd == image_entry || m.hwnd == entry || m.hwnd == ent_mv || m.hwnd == ent_ma || m.hwnd == ent_mw
            || m.hwnd == ent_adc_val || (detail_exists() && m.hwnd == ent_filter)))
        return true; /* no beep */
    if (detail_exists() && IsDialogMessageW(detail_win, &m))
        return true;
    if (IsDialogMessageW(root, &m))
        return true;
    return false;
}

/* ------------------------------------------------------------ visual styles */
static void enable_visual_styles()
{
    /* comctl32 v6 without a resource manifest: shell32 carries one (#124). */
    wchar_t dir[MAX_PATH];
    if (!GetSystemDirectoryW(dir, MAX_PATH))
        return;
    std::wstring shell = std::wstring(dir) + L"\\shell32.dll";
    ACTCTXW ac;
    memset(&ac, 0, sizeof(ac));
    ac.cbSize = sizeof(ac);
    ac.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_SET_PROCESS_DEFAULT | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
    ac.lpSource = shell.c_str();
    ac.lpResourceName = MAKEINTRESOURCEW(124);
    ac.lpAssemblyDirectory = dir;
    HANDLE h = CreateActCtxW(&ac);
    if (h != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie;
        ActivateActCtx(h, &cookie);
    }
}

static int run_gui(const std::string &initial)
{
    typedef BOOL(WINAPI * DpiFn)(void);
    DpiFn dpi_aware = (DpiFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (dpi_aware)
        dpi_aware();
    enable_visual_styles();
    /* A double-clicked console exe owns a console of its own: drop it. */
    DWORD procs[4];
    if (GetConsoleProcessList(procs, 4) == 1)
        FreeConsole();
    net_init();
    SimUI *ui = new SimUI(initial, CONFIG_PATH, false);
    MSG m;
    while (GetMessageW(&m, 0, 0, 0) > 0) {
        if (ui->pre_translate(m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    /* the daemon worker: stop it, give it a moment, then leave */
    if (ui->emu)
        ui->emu->stop();
    if (ui->thread && !ui->thread->join(3000))
        ExitProcess(0);
    delete ui;
    return 0;
}

} /* namespace simui */

int main(int argc, char **argv)
{
    simui::init_paths();
    std::string initial;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--package-smoke")
            return simui::run_package_smoke();
        if (a == "--selftest")
            return simui::run_selftests() < 0 ? 1 : 0;
        if (a == "--live-sleep")
            return simui::run_live_sleep();
        if (a == "--live-check")
            return simui::run_live_check(i + 1 < argc ? argv[i + 1] : "", i + 2 < argc ? atof(argv[i + 2]) : 15.0);
    }
    /* sys.argv[1], as Python reads it; spell it in UTF-16 for non-ASCII paths */
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc > 1)
        initial = simui::narrow(wargv[1]);
    else if (argc > 1)
        initial = argv[1];
    if (wargv)
        LocalFree(wargv);
    return simui::run_gui(initial);
}
