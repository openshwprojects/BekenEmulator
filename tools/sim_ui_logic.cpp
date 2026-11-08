/* sim_ui: the SimulatorUI methods of tools/sim_ui.py - options and settings,
 * the captured-traffic window's data, GPIO/sleep/meter/LED/camera panes, the
 * run/stop lifecycle, the worker thread and the poll loop. Window creation and
 * layout live in sim_ui.cpp. */
#include "sim_ui.h"
#include "netstack.h"
#include "wifipeer.h"
#include "beken/beken_crypto.h"

#include <richedit.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace simui {

Var *SimUI::optv(const std::string &name)
{
    for (int i = 0; i < N_OPTIONS; i++)
        if (name == OPTION_DEFAULTS[i].name)
            return &opt[i];
    return 0;
}

std::string SimUI::opts(const std::string &name) { return optv(name)->get(); }
void SimUI::set_option_bool(const std::string &name, bool v) { optv(name)->setb(v); }

/* ------------------------------------------------------- Tuya config tab */
void SimUI::tab_changed()
{
    if (tab_index == 2)
        refresh_tuya_config(false);
}

/* _tuya_json_value: restore obvious JSON scalars; loose strings stay strings */
static JsonValue tuya_json_value(const std::string &value)
{
    JsonValue v;
    if (json_parse(value, v))
        return v;
    return JsonValue::make_str(value);
}

bool SimUI::refresh_tuya_config(bool force, const std::string &path_in, bool have_path)
{
    if (hooks.refresh_tuya_disabled)
        return false;
    std::string path = strip(have_path && !path_in.empty() ? path_in : var_image.get());
    bool key_ok = false;
    std::string key;
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!path.empty() && GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fa)) {
            unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            unsigned long long mt = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32)
                                    | fa.ftLastWriteTime.dwLowDateTime;
            key = strf("%s|%llu|%llu", norm_path(path).c_str(), size, mt);
            key_ok = true;
        }
    }
    if (!force && key_ok && tuya_key_valid && key == tuya_key)
        return tuya_config.present;
    tuya_key_valid = key_ok;
    tuya_key = key;
    tuya_config = TuyaCfg();

    tuya_tree.clear();
    pane_clear(txt_tuya_json);
    if (!key_ok) {
        var_tuya_status.set("Pick an existing flash dump to decode.");
        if (!running())
            refresh_static_periph();
        return false;
    }

    TuyaCfg decoded;
    std::string err;
    bool ok = hooks.tuya_extract ? hooks.tuya_extract(path, decoded, err, hooks.tuya_ctx)
                                 : tuya_extract_real(path, decoded, err);
    if (!ok) {
        var_tuya_status.set("Tuya config decode failed: " + err);
        if (!running())
            refresh_static_periph();
        return false;
    }
    tuya_config = decoded;
    if (!decoded.present) {
        var_tuya_status.set("No recoverable Tuya configuration found in this image.");
        if (!running())
            refresh_static_periph();
        return false;
    }

    for (size_t i = 0; i < decoded.human.size(); i++) {
        StrList vals;
        vals.push_back(decoded.human[i].first);
        vals.push_back(decoded.human[i].second);
        tuya_tree.insert(strf("I%03X", (unsigned)i + 1), vals, "");
    }
    /* {name: _tuya_json_value(value)}: a repeated name keeps its first place */
    JsonValue safe = JsonValue::make_obj();
    for (size_t i = 0; i < decoded.pairs.size(); i++)
        safe.set(decoded.pairs[i].first, tuya_json_value(decoded.pairs[i].second));
    std::string body = json_pretty(safe, 2, 0);
    append_utf8(txt_tuya_json, body + "\n");
    SendMessageW(txt_tuya_json.hwnd, WM_VSCROLL, SB_TOP, 0);

    var_tuya_status.set(strf("%s storage at 0x%06lX \xe2\x80\x94 %d config fields, %d datapoints",
                             decoded.encrypted ? "encrypted" : "plaintext", (unsigned long)decoded.offset,
                             (int)safe.members().size(), decoded.dps));
    if (!running())
        refresh_static_periph();
    return true;
}

/* ------------------------------------------------------------ Config tab */
void SimUI::options_changed()
{
    if (!ui_ready)
        return;
    bool wifi = optv("wifi")->getb();
    for (size_t i = 0; i < opt_widgets.size(); i++) {
        bool on = optv(opt_widgets[i].first)->getb();
        if (opt_widgets[i].first == "pcap")
            on = on && wifi; /* a capture needs the radio peer at all */
        for (size_t k = 0; k < opt_widgets[i].second.size(); k++) {
            HWND w = opt_widgets[i].second[k];
            set_state(w, on);
            InvalidateRect(w, 0, TRUE);
        }
    }
    var_summary.set(summary_text());
    var_cfg_note.set("Choices are remembered between launches. " + drop_note);
}

std::map<std::string, std::string> SimUI::option_values()
{
    std::map<std::string, std::string> o;
    for (int i = 0; i < N_OPTIONS; i++)
        o[OPTION_DEFAULTS[i].name] = OPTION_DEFAULTS[i].is_bool ? (opt[i].getb() ? "1" : "0") : opt[i].get();
    return o;
}

static int count_lines(const std::string &text)
{
    StrList l = splitlines(text);
    int n = 0;
    for (size_t i = 0; i < l.size(); i++)
        if (!strip(l[i]).empty())
            n++;
    return n;
}

std::string SimUI::summary_text()
{
    std::map<std::string, std::string> o = option_values();
    StrList parts;
    if (o["boot"] == "1")
        parts.push_back("boot ROM");
    if (o["xvr_selfclear"] == "1")
        parts.push_back("RF-cal self-clear");
    if (o["blecore"] == "1")
        parts.push_back("BLE core");
    if (o["tuyamcu"] == "1") {
        StrList extra;
        if (!strip(o["tuyamcu_pid"]).empty())
            extra.push_back(strip(o["tuyamcu_pid"]));
        if (o["tuyamcu_raw"] == "1")
            extra.push_back("raw");
        parts.push_back("TuyaMCU" + (extra.empty() ? std::string() : " (" + join(extra, ", ") + ")"));
    }
    if (o["wifi"] == "1") {
        StrList extra;
        extra.push_back(o["wifi_role"]);
        extra.push_back(o["net_out"] == "1" ? "real internet" : "offline");
        if (!strip(o["wifi_ssid"]).empty())
            extra.push_back("'" + o["wifi_ssid"] + "'");
        extra.push_back(!o["wifi_pass"].empty() ? "WPA2" : "open");
        if (!strip(o["fwd_local"]).empty() && !strip(o["fwd_device"]).empty())
            extra.push_back(":" + strip(o["fwd_local"]) + " to :" + strip(o["fwd_device"]));
        if (o["pcap"] == "1")
            extra.push_back("capturing");
        int n = count_lines(o["http_gets"]);
        if (n)
            extra.push_back(strf("%d HTTP GET", n));
        n = count_lines(o["mqtt_pubs"]);
        if (n)
            extra.push_back(strf("%d MQTT", n));
        parts.push_back("Wi-Fi peer (" + join(extra, ", ") + ")");
    }
    return "options: " + (parts.empty() ? std::string("none (Config tab)") : join(parts, ", "));
}

static StrList nonblank_lines(const std::string &text)
{
    StrList out, l = splitlines(text);
    for (size_t i = 0; i < l.size(); i++)
        if (!strip(l[i]).empty())
            out.push_back(strip(l[i]));
    return out;
}

bool SimUI::collect_cfg(RunCfg &cfg, std::string &err)
{
    std::map<std::string, std::string> o = option_values();
    cfg = RunCfg();
    bool tuyamcu = o["tuyamcu"] == "1";
    cfg.chip = var_chip.get();
    cfg.key = var_key.get();
    cfg.rom_path = strip(o["rom_path"]);
    if (!strip(o["psram_mb"]).empty()) {
        long long mb=0;
        if(!parse_int(strip(o["psram_mb"]),10,mb) || (mb!=0 && mb!=2 && mb!=4 && mb!=8)
            || (mb && cfg.chip!="ESP32")) {
            err="External PSRAM requires ESP32 and a size of 0, 2, 4 or 8 MiB.";
            return false;
        }
        cfg.psram_mb=(unsigned)mb;
    }
    if (!strip(o["xtal_mhz"]).empty()) {
        long long mhz = 0;
        if (!parse_int(strip(o["xtal_mhz"]), 10, mhz)
            || (mhz != 26 && mhz != 40 && !(cfg.chip == "ESP8266" && mhz == 24))
            || (cfg.chip != "ESP8266" && cfg.chip != "ESP32")) {
            err = "Crystal frequency: ESP8266 supports 24, 26 or 40 MHz; ESP32 supports 26 or 40 MHz.";
            return false;
        }
        cfg.xtal_mhz = (unsigned)mhz;
    }
    cfg.boot = o["boot"] == "1";
    cfg.xvr_selfclear = o["xvr_selfclear"] == "1";
    cfg.blecore = o["blecore"] == "1";
    cfg.tuyamcu = tuyamcu;
    if (tuyamcu) {
        std::string pid = strip(o["tuyamcu_pid"]), e;
        if (!check_tuyamcu_pid(pid, e)) {
            err = "Product ID: " + e;
            return false;
        }
        cfg.tuyamcu_pid = pid;
        cfg.tuyamcu_raw = o["tuyamcu_raw"] == "1";
    }
    cfg.wifi = o["wifi"] == "1";
    if (!cfg.wifi)
        return true;
    std::string lp = strip(o["fwd_local"]), dp = strip(o["fwd_device"]);
    if (!lp.empty() || !dp.empty()) {
        std::pair<int, int> f;
        std::string e;
        if (!parse_forward(lp + ":" + dp, f, e)) {
            err = "Port forward: " + e;
            return false;
        }
        cfg.forwards.push_back(f);
    }
    if (o["pcap"] == "1") {
        std::string pcap = strip(o["pcap_path"]);
        if (pcap.empty()) {
            err = "Radio capture is ticked but no file is set.";
            return false;
        }
        if (!path_isdir(path_dirname(abs_path(pcap)))) {
            err = "Radio capture: the folder for " + pcap + " does not exist.";
            return false;
        }
        cfg.has_pcap = true;
        cfg.pcap = pcap;
    }
    cfg.wifi_role = o["wifi_role"];
    cfg.net_out = o["net_out"] == "1";
    /* Neither is stripped: an SSID is matched exactly and salts the WPA2
     * key, and spaces are part of a passphrase. */
    cfg.has_ssid = !strip(o["wifi_ssid"]).empty();
    if (cfg.has_ssid)
        cfg.wifi_ssid = o["wifi_ssid"];
    if (!o["wifi_pass"].empty()) {
        std::string e;
        if (!check_wifi_pass(o["wifi_pass"], e)) {
            err = "Passphrase: " + e;
            return false;
        }
        cfg.has_pass = true;
        cfg.wifi_pass = o["wifi_pass"];
    }
    StrList gets = nonblank_lines(o["http_gets"]);
    {
        std::string e;
        if (!parse_http_gets(gets, e)) {
            err = "HTTP GET: " + e;
            return false;
        }
    }
    cfg.http_gets = gets;
    {
        std::string e;
        if (!parse_mqtt_pubs(nonblank_lines(o["mqtt_pubs"]), cfg.mqtt_pubs, e)) {
            err = "MQTT messages: " + e;
            return false;
        }
    }
    return true;
}

void SimUI::browse_pcap()
{
    std::string path = ask_save(".pcap", L"pcap\0*.pcap\0All\0*.*\0");
    if (!path.empty())
        optv("pcap_path")->set(path);
}

void SimUI::update_open_forward() { set_state(btn_open_forward, forward_is_open()); }

bool SimUI::forward_is_open()
{
    if (active_forward_url.empty() || !running() || stop_requested)
        return false;
    return emu->forward_listening();
}

void SimUI::open_forward()
{
    if (!forward_is_open())
        return;
    if (!browser(active_forward_url))
        message("Could not find a browser to open " + active_forward_url, true);
}

void SimUI::set_text_option(const std::string &name, const std::string &value)
{
    /* A disabled box ignores nothing in Win32, but the edit is not one the
     * person made: load it without leaving an undo step behind. */
    optv(name)->set(value);
}

void SimUI::set_option_str(const std::string &name, const std::string &v)
{
    if (is_text_option(name))
        set_text_option(name, v);
    else
        optv(name)->set(v);
}

void SimUI::reset_options()
{
    for (int i = 0; i < N_OPTIONS; i++) {
        if (OPTION_DEFAULTS[i].is_bool)
            opt[i].setb(OPTION_DEFAULTS[i].bdefault);
        else
            set_option_str(OPTION_DEFAULTS[i].name, OPTION_DEFAULTS[i].sdefault);
    }
}

/* ------------------------------------------------------- saved settings */
bool SimUI::load_settings(JsonValue &out)
{
    out = JsonValue::make_obj();
    if (settings_path.empty())
        return false;
    Bytes raw;
    HANDLE h = CreateFileW(widen(settings_path).c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[65536];
    DWORD n;
    while (ReadFile(h, buf, sizeof(buf), &n, 0) && n)
        raw.append(buf, n);
    CloseHandle(h);
    if (startswith(raw, "\xef\xbb\xbf")) /* json.load of a utf-8 file rejects a BOM; be kind */
        raw = raw.substr(3);
    JsonValue v;
    if (!json_parse(raw, v) || !v.is_obj())
        return false;
    out = v;
    return true;
}

static bool chip_known(const std::string &c)
{
    StrList names = backend_chip_names();
    return std::find(names.begin(), names.end(), c) != names.end();
}

static bool key_known(const std::string &k)
{
    for (int i = 0; KNOWN_KEY_NAMES[i]; i++)
        if (k == KNOWN_KEY_NAMES[i])
            return true;
    return false;
}

void SimUI::apply_settings(const JsonValue &data, bool same_image)
{
    if (!data.is_obj())
        return;
    for (int i = 0; i < N_OPTIONS; i++) {
        const OptDef &d = OPTION_DEFAULTS[i];
        if (is_unsaved_option(d.name) || !data.has(d.name))
            continue;
        const JsonValue &v = data.get(d.name);
        if (d.is_bool) {
            if (!v.is_bool())
                continue;
            opt[i].setb(v.as_bool());
        } else {
            if (!v.is_str())
                continue;
            if (std::string(d.name) == "wifi_role") {
                bool ok = false;
                for (int k = 0; WIFI_ROLES[k]; k++)
                    if (v.as_str() == WIFI_ROLES[k])
                        ok = true;
                if (!ok)
                    continue;
            }
            set_option_str(d.name, v.as_str());
        }
    }
    if (same_image) {
        if (data.has("chip") && data.get("chip").is_str() && chip_known(data.get("chip").as_str()))
            var_chip.set(data.get("chip").as_str());
        if (data.has("key") && data.get("key").is_str()) {
            std::string k = data.get("key").as_str();
            if (key_known(k) || k == "(none)")
                var_key.set(k);
        }
    }
}

std::string SimUI::settings_data()
{
    std::string image = strip(var_image.get());
    std::vector<std::pair<std::string, std::string> > items; /* name -> JSON text */
    items.push_back(std::make_pair("image", json_quote_ascii(image.empty() ? std::string() : abs_path(image))));
    items.push_back(std::make_pair("chip", json_quote_ascii(var_chip.get())));
    items.push_back(std::make_pair("key", json_quote_ascii(var_key.get())));
    for (int i = 0; i < N_OPTIONS; i++) {
        const OptDef &d = OPTION_DEFAULTS[i];
        if (is_unsaved_option(d.name))
            continue;
        items.push_back(std::make_pair(d.name, d.is_bool ? std::string(opt[i].getb() ? "true" : "false")
                                                         : json_quote_ascii(opt[i].get())));
    }
    /* json.dump(data, f, indent=1) */
    std::string out = "{\n";
    for (size_t i = 0; i < items.size(); i++)
        out += " " + json_quote_ascii(items[i].first) + ": " + items[i].second + (i + 1 < items.size() ? ",\n" : "\n");
    return out + "}";
}

void SimUI::save_settings()
{
    if (settings_path.empty())
        return;
    /* Written aside and swapped in, so a failed write leaves the previous
     * settings rather than a truncated file that loads as defaults. */
    std::string tmp = settings_path + ".tmp";
    std::wstring wtmp = widen(tmp), wdst = widen(settings_path);
    std::string body = settings_data();
    /* Python text mode on Windows writes \n as \r\n */
    body = replace_all(body, "\n", "\r\n");
    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    bool ok = h != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD n = 0;
        ok = WriteFile(h, body.data(), (DWORD)body.size(), &n, 0) && n == body.size();
        CloseHandle(h);
    }
    if (ok)
        ok = MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok)
        DeleteFileW(wtmp.c_str()); /* a convenience; never worth an error dialog */
}

/* ---------------------------------------------------------- drag and drop */
bool SimUI::enable_drop()
{
    DragAcceptFiles(root, TRUE);
    drop_note = "Drop a .bin file on the window to load and run it.";
    return true;
}

void SimUI::on_drop(const StrList &all)
{
    StrList paths;
    for (size_t i = 0; i < all.size(); i++)
        if (path_isfile(all[i]))
            paths.push_back(all[i]);
    if (paths.empty()) {
        var_state.set("that drop held no file to load");
        return;
    }
    load_image(paths[0], true);
}

void SimUI::chip_changed()
{
    if (var_chip.get() == "ESP32" || var_chip.get() == "ESP8266") {
        var_key.set("(none)");
        set_option_bool("wifi", false);
        set_option_bool("blecore", false);
        set_option_bool("xvr_selfclear", false);
    }
}

void SimUI::load_image(const std::string &path, bool run, const std::string &chip, const std::string &key)
{
    var_image.set(path);
    var_chip.set(!chip.empty() ? chip : guess_chip(path));
    chip_changed();
    if (!key.empty())
        var_key.set(key);
    refresh_tuya_config(false, path, true);
    select_tab(0);
    if (!run)
        return;
    if (thread_alive()) {
        pending_run = true;
        on_stop();
        var_state.set("stopping the current run to start " + path_basename(path) + " ...");
    } else
        on_run();
}

void SimUI::run_interesting(int idx)
{
    const Profile &p = INTERESTING_DUMPS[idx];
    std::string path = path_join(FIRMWARE_DIR, p.file);
    if (!path_isfile(path)) {
        message("The bundled quick-run image is missing:\n" + path, true);
        return;
    }
    has_pending_uart1_rx = p.uart1_rx != 0;
    pending_uart1_rx = p.uart1_rx ? Bytes(p.uart1_rx) : Bytes();
    if (p.wifi) {
        set_option_bool("wifi", true);
        /* A quick run must be reproducible even after the Config tab was used
         * for another network: empty fields adopt the dump's stored network. */
        optv("wifi_role")->set("auto");
        optv("wifi_ssid")->set("");
        optv("wifi_pass")->set("");
        set_option_bool("net_out", p.net_out < 0 ? true : p.net_out != 0);
    }
    load_image(path, true, p.chip, p.key);
}

/* ------------------------------------------------- captured traffic window */
void SimUI::details_for_click(bool have_line, const std::string &line)
{
    std::string want = have_line ? filter_from_line(line) : std::string();
    open_details();
    if (!want.empty() && frame_vars_made) {
        var_frame_filter.set(want);
        refresh_details();
    }
}

bool SimUI::detail_exists() { return detail_win && IsWindow(detail_win); }

bool SimUI::frame_target_host(const std::string &dir, const Bytes &data, std::string &host)
{
    Bytes src, dst;
    if (!netstack::ipv4_endpoints(data, src, dst))
        return false;
    Bytes h;
    if (dir == "from device")
        h = dst;
    else if (dir == "to device")
        h = src;
    else
        return false;
    if (h == Bytes(4, '\0') || h == Bytes(4, '\xff') || ((u8)h[0] >= 224 && (u8)h[0] <= 239))
        return false;
    host = netstack::ip2str(h);
    return true;
}

static bool host_less(const std::string &a, const std::string &b)
{
    StrList pa = split(a, "."), pb = split(b, ".");
    for (size_t i = 0; i < pa.size() && i < pb.size(); i++) {
        long long x = 0, y = 0;
        parse_int(pa[i], 10, x);
        parse_int(pb[i], 10, y);
        if (x != y)
            return x < y;
    }
    return pa.size() < pb.size();
}

StrList SimUI::target_hosts(const std::vector<Frame> &frames)
{
    std::set<std::string> hosts;
    for (size_t i = 0; i < frames.size(); i++) {
        std::string h;
        if (frame_target_host(frames[i].dir, frames[i].data, h))
            hosts.insert(h);
    }
    StrList out(hosts.begin(), hosts.end());
    std::sort(out.begin(), out.end(), host_less);
    return out;
}

void SimUI::refresh_target_hosts(const std::vector<Frame> &frames)
{
    if (!detail_exists())
        return;
    StrList choices;
    choices.push_back(ALL_FRAME_TARGETS);
    StrList h = target_hosts(frames);
    choices.insert(choices.end(), h.begin(), h.end());
    std::string current = var_frame_target.get();
    if (choices != frame_target_values) {
        frame_target_values = choices;
        SendMessageW(frame_target, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < choices.size(); i++)
            SendMessageW(frame_target, CB_ADDSTRING, 0, (LPARAM)widen(choices[i]).c_str());
    }
    if (std::find(choices.begin(), choices.end(), current) == choices.end())
        var_frame_target.set(ALL_FRAME_TARGETS);
    else
        var_frame_target.set(current);
}

std::vector<FrameRow> SimUI::visible_frames()
{
    std::vector<Frame> fr;
    long base;
    {
        ScopedLock lk(out_lock);
        fr.assign(frames.begin(), frames.end());
        base = frames_dropped;
    }
    refresh_target_hosts(fr);
    std::string needle = frame_vars_made ? lower(strip(var_frame_filter.get())) : std::string();
    std::string target = frame_vars_made ? var_frame_target.get() : std::string(ALL_FRAME_TARGETS);
    std::vector<FrameRow> out;
    for (size_t i = 0; i < fr.size(); i++) {
        std::string what = netstack::describe_frame(fr[i].data);
        if (target != ALL_FRAME_TARGETS) {
            std::string h;
            if (!frame_target_host(fr[i].dir, fr[i].data, h) || h != target)
                continue;
        }
        if (!needle.empty() && lower(what).find(needle) == std::string::npos
            && lower(fr[i].dir).find(needle) == std::string::npos)
            continue;
        FrameRow r;
        r.no = base + (long)i + 1;
        r.dir = fr[i].dir;
        r.data = fr[i].data;
        r.us = fr[i].us;
        r.what = what;
        out.push_back(r);
    }
    return out;
}

void SimUI::refresh_details()
{
    if (!detail_exists())
        return;
    std::vector<FrameRow> rows = visible_frames();
    std::string was_selected = frame_tree.selection();
    int was_at = frame_tree.top_index();
    SendMessageW(frame_tree.hwnd, WM_SETREDRAW, FALSE, 0);
    frame_tree.clear();
    size_t first = rows.size() > 2000 ? rows.size() - 2000 : 0;
    size_t shown = rows.size() - first;
    for (size_t i = first; i < rows.size(); i++) {
        StrList v;
        v.push_back(strf("%ld", rows[i].no));
        v.push_back(strf("%.3f s", rows[i].us / 1e6));
        v.push_back(rows[i].dir);
        v.push_back(strf("%d", (int)rows[i].data.size()));
        v.push_back(rows[i].what);
        frame_tree.insert(strf("%ld", rows[i].no), v, rows[i].dir == "from device" ? "in" : "out");
    }
    long nframes;
    {
        ScopedLock lk(out_lock);
        nframes = (long)frames.size();
    }
    long total = nframes + frames_dropped;
    std::string note = strf("%ld frames captured", total);
    if (shown != rows.size())
        note += strf(", showing the last %d of %d matching", (int)shown, (int)rows.size());
    else if ((long)rows.size() != nframes)
        note += strf(", %d match the filter", (int)rows.size());
    if (frames_dropped)
        note += strf(" (the earliest %ld were dropped)", frames_dropped);
    if (!total)
        note = std::string("Nothing captured yet. ") + WIFI_HOWTO
               + "; every frame the bridge carries then appears here with its bytes.";
    var_frame_count.set(note);
    if (!shown) {
        SendMessageW(frame_tree.hwnd, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(frame_tree.hwnd, 0, TRUE);
        return;
    }
    if (!was_selected.empty() && frame_tree.exists(was_selected)) {
        frame_tree.selection_set(was_selected);
        frame_tree.restore_top(was_at);
        show_frame_bytes(); /* <<TreeviewSelect>> */
    } else if (!was_selected.empty()) {
        /* filtered out or dropped: leave the view where it was */
        frame_tree.restore_top(was_at);
    } else {
        std::string last = strf("%ld", rows[rows.size() - 1].no);
        frame_tree.selection_set(last);
        frame_tree.see(last);
        show_frame_bytes();
    }
    SendMessageW(frame_tree.hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(frame_tree.hwnd, 0, TRUE);
}

void SimUI::show_frame_bytes()
{
    std::string sel = frame_tree.selection();
    if (sel.empty())
        return;
    long want = atol(sel.c_str());
    std::vector<FrameRow> rows = visible_frames();
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].no != want)
            continue;
        std::string head = strf("frame %ld   %s   %.6f s   %d bytes\n", rows[i].no, rows[i].dir.c_str(),
                                rows[i].us / 1e6, (int)rows[i].data.size())
                           + rows[i].what + "\n\n";
        pane_clear(txt_bytes);
        append_utf8(txt_bytes, head + netstack::hexdump(rows[i].data) + "\n");
        SendMessageW(txt_bytes.hwnd, WM_VSCROLL, SB_TOP, 0);
        return;
    }
}

void SimUI::save_pcap()
{
    std::vector<FrameRow> rows = visible_frames();
    if (rows.empty()) {
        message("Nothing captured yet.", false);
        return;
    }
    std::string path = ask_save(".pcap", L"pcap\0*.pcap\0All\0*.*\0");
    if (path.empty())
        return;
    netstack::EthPcap cap(path);
    for (size_t i = 0; i < rows.size(); i++)
        cap.write(rows[i].data, rows[i].us);
    cap.close();
    var_state.set(strf("saved %d frames to %s", (int)rows.size(), path.c_str()));
}

/* ------------------------------------------------------------ text panes */
void SimUI::append_utf8(Pane &p, const std::string &text, const char *tag) { append(p, widen(text), tag); }

void SimUI::append(Pane &p, const std::wstring &text_in, const char *tag)
{
    if (text_in.empty() || !p.hwnd)
        return;
    /* RichEdit ends a paragraph with \r; a CR from the device stays inside
     * its line in Tk, so it is dropped here and \n is the line break. */
    std::wstring text;
    text.reserve(text_in.size());
    long nl = 0;
    for (size_t i = 0; i < text_in.size(); i++) {
        wchar_t c = text_in[i];
        if (c == L'\r')
            continue;
        if (c == L'\n') {
            text += L'\r';
            nl++;
        } else if (c == 0)
            text += L'.';
        else
            text += c;
    }
    if (text.empty())
        return;
    HWND h = p.hwnd;
    /* Only follow the tail if the view is already at the tail. */
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    bool at_end = true;
    if (GetScrollInfo(h, SB_VERT, &si) && si.nMax > 0 && (int)si.nPage > 0)
        at_end = si.nPos + (int)si.nPage >= si.nMax - 1;
    POINT scroll_pos;
    SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&scroll_pos);
    CHARRANGE save;
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&save);
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SendMessageW(h, EM_SETREADONLY, FALSE, 0);
    GETTEXTLENGTHEX gl;
    gl.flags = GTL_NUMCHARS | GTL_PRECISE;
    gl.codepage = 1200;
    LONG end = (LONG)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    CHARRANGE cr;
    cr.cpMin = cr.cpMax = end;
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
    COLORREF col = p.fg;
    if (tag)
        for (size_t k = 0; k < p.tags.size(); k++)
            if (p.tags[k].first == tag)
                col = p.tags[k].second;
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = col;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    p.newlines += nl;
    /* Lines are counted as Tk does (index of end-1c): newlines + 1. */
    long lines = p.newlines + 1;
    if (lines > MAX_LINES) {
        long k = lines - MAX_LINES; /* drop this many leading lines */
        long total = (long)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
        long want = 8192;
        for (;;) {
            long n = want < total ? want : total;
            TEXTRANGEW tr;
            std::wstring buf((size_t)n + 1, L'\0');
            tr.chrg.cpMin = 0;
            tr.chrg.cpMax = n;
            tr.lpstrText = &buf[0];
            LRESULT got = SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
            long seen = 0, cut = -1;
            for (long i = 0; i < (long)got; i++)
                if (buf[(size_t)i] == L'\r' && ++seen == k) {
                    cut = i + 1;
                    break;
                }
            if (cut >= 0 || n >= total) {
                if (cut < 0)
                    cut = total;
                CHARRANGE del;
                del.cpMin = 0;
                del.cpMax = cut;
                SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&del);
                SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
                p.newlines -= cut < total ? k : seen;
                save.cpMin = save.cpMin > cut ? save.cpMin - cut : 0;
                save.cpMax = save.cpMax > cut ? save.cpMax - cut : 0;
                break;
            }
            want *= 4;
        }
    }
    SendMessageW(h, EM_SETREADONLY, TRUE, 0);
    if (save.cpMin != save.cpMax)
        SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&save);
    if (!at_end)
        SendMessageW(h, EM_SETSCROLLPOS, 0, (LPARAM)&scroll_pos);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, 0, TRUE);
    if (at_end)
        SendMessageW(h, WM_VSCROLL, SB_BOTTOM, 0);
}

std::string SimUI::render_uart1(const Bytes &data)
{
    if (!var_hex.getb())
        return latin1_to_utf8(data);
    std::string out;
    for (size_t i = 0; i < data.size(); i++) {
        out += strf("%02x ", (u8)data[i]);
        if (++hex_col >= 16) {
            out += "\n";
            hex_col = 0;
        }
    }
    return out;
}

/* ------------------------------------------------------------ GPIO pane */
void SimUI::gpio_press_level(int pin, u32 raw, int &level, std::string &why)
{
    const TuyaCfg &decoded = running() ? run_tuya_config : tuya_config;
    std::map<int, int> levels = tuya_button_levels(decoded.present ? &decoded.pairs : 0);
    std::map<int, int>::iterator it = levels.find(pin);
    if (it != levels.end()) {
        level = it->second;
        why = std::string("Tuya config says active ") + (level ? "HIGH" : "LOW");
        return;
    }
    if (raw & (1 << 5)) {
        int idle = (raw & (1 << 4)) ? 1 : 0;
        level = 1 - idle;
        why = std::string(idle ? "pull-up" : "pull-down") + " input";
        return;
    }
    level = 0;
    why = "no pull; defaulting the simulated press to LOW";
}

bool SimUI::gpio_config(int pin, u32 &raw)
{
    if (!emu)
        return false;
    int slot = emu->gpio_slot(pin);
    EmuView v;
    if (!emu->view(v))
        return false;
    std::map<int, u32>::iterator it = v.gpio_cfg.find(slot);
    if (it == v.gpio_cfg.end())
        return false;
    raw = it->second;
    return true;
}

int SimUI::selected_gpio()
{
    std::string iid = tree.selection();
    if (!startswith(iid, "gpio:"))
        return -1;
    long long v;
    if (!parse_int(iid.substr(5), 10, v))
        return -1;
    return (int)v;
}

void SimUI::gpio_selection_changed()
{
    gpio_selected = selected_gpio();
    update_gpio_control();
    update_sleep_controls();
}

void SimUI::gpio_double_click(int index)
{
    if (index < 0 || index >= (int)tree.size())
        return;
    tree.selection_set(tree.iids[(size_t)index]);
    gpio_selected = selected_gpio();
    update_gpio_control();
    update_sleep_controls();
    if (enabled(btn_gpio_press))
        toggle_gpio_input();
}

std::map<int, int> SimUI::sleep_wake_pins()
{
    EmuView v;
    if (!emu || !emu->view(v) || !v.in_deep_sleep)
        return std::map<int, int>();
    return v.deep_sleep_wake_pins;
}

void SimUI::update_sleep_controls()
{
    EmuView st;
    if (!emu || !emu->view(st)) {
        var_sleep.set("Sleep: AWAKE");
        set_window_text(btn_wake_rtc, "Wake RTC");
        set_state(btn_wake_rtc, false);
        set_window_text(btn_wake_gpio, "Wake GPIO");
        set_state(btn_wake_gpio, false);
        return;
    }
    bool can_act = running() && !stop_requested;
    if (st.in_deep_sleep) {
        StrList parts;
        if (st.deep_sleep_rtc_sec > 0)
            parts.push_back(strf("RTC=%.0fs", st.deep_sleep_rtc_sec));
        if (!st.deep_sleep_wake_pins.empty()) {
            StrList pl;
            for (std::map<int, int>::iterator it = st.deep_sleep_wake_pins.begin(); it != st.deep_sleep_wake_pins.end(); ++it)
                pl.push_back(strf("P%d:%s", it->first, it->second ? "HIGH" : "LOW"));
            parts.push_back("GPIO=[" + join(pl, ", ") + "]");
        }
        std::string detail = parts.empty() ? std::string() : " (" + join(parts, ", ") + ")";
        var_sleep.set(strf("DEEP SLEEP #%d", st.deep_sleep_count > 1 ? st.deep_sleep_count : 1) + detail);
        set_window_text(btn_wake_rtc, "Wake RTC");
        set_state(btn_wake_rtc, can_act);
        int sel = gpio_selected >= 0 ? gpio_selected : selected_gpio();
        std::string lbl;
        if (sel >= 0 && st.deep_sleep_wake_pins.count(sel))
            lbl = strf("Wake P%d", sel);
        else if (!st.deep_sleep_wake_pins.empty())
            lbl = strf("Wake P%d", st.deep_sleep_wake_pins.begin()->first);
        else
            lbl = "Wake GPIO";
        set_window_text(btn_wake_gpio, lbl);
        set_state(btn_wake_gpio, can_act);
    } else {
        if (st.deep_sleep_count > 0 && !st.last_wake_reason.empty()) {
            if (st.last_wake_reason == "gpio" && st.last_wake_pin >= 0)
                var_sleep.set(strf("Sleep: AWAKE (woke #%d via GPIO P%d)", st.deep_sleep_count, st.last_wake_pin));
            else
                var_sleep.set(strf("Sleep: AWAKE (woke #%d via %s)", st.deep_sleep_count,
                                   upper(st.last_wake_reason).c_str()));
        } else
            var_sleep.set("Sleep: AWAKE");
        set_window_text(btn_wake_rtc, "Wake RTC");
        set_state(btn_wake_rtc, false);
        set_window_text(btn_wake_gpio, "Wake GPIO");
        set_state(btn_wake_gpio, false);
    }
}

void SimUI::wake_deep_sleep(const std::string &reason)
{
    EmuView st;
    if (!emu || !emu->view(st) || !running() || stop_requested || !st.in_deep_sleep) {
        update_sleep_controls();
        return;
    }
    int pin = -1;
    if (reason == "gpio") {
        int sel = gpio_selected >= 0 ? gpio_selected : selected_gpio();
        if (sel >= 0 && st.deep_sleep_wake_pins.count(sel))
            pin = sel;
        else if (!st.deep_sleep_wake_pins.empty())
            pin = st.deep_sleep_wake_pins.begin()->first;
        else
            pin = sel;
    }
    if (emu->has_wake())
        emu->wake_from_deep_sleep(reason, pin);
    periph_key_valid = false;
    refresh_periph();
    update_state();
}

void SimUI::update_gpio_control()
{
    int pin = gpio_selected;
    if (pin < 0) {
        pin = selected_gpio();
        gpio_selected = pin;
    }
    bool live = running() && !stop_requested;
    if (pin < 0) {
        set_window_text(btn_gpio_press, "Press");
        set_state(btn_gpio_press, false);
        var_gpio_action.set("Select a configured GPIO input to simulate its button.");
        return;
    }
    std::map<int, int>::iterator it = gpio_pressed.find(pin);
    if (it != gpio_pressed.end()) {
        set_window_text(btn_gpio_press, strf("Release P%d", pin));
        set_state(btn_gpio_press, live);
        var_gpio_action.set(strf("P%d is PRESSED %s; release it after the firmware debounces.", pin,
                                 it->second ? "HIGH" : "LOW"));
        return;
    }
    std::map<int, int> wake_pins = sleep_wake_pins();
    if (wake_pins.count(pin)) {
        set_window_text(btn_gpio_press, strf("Press P%d", pin));
        set_state(btn_gpio_press, live);
        var_gpio_action.set(strf("P%d: deep-sleep wake pin (active %s); Press wakes the chip.", pin,
                                 wake_pins[pin] ? "HIGH" : "LOW"));
        return;
    }
    u32 raw = 0;
    bool have = gpio_config(pin, raw);
    if (!gpio_is_input(have, raw)) {
        set_window_text(btn_gpio_press, strf("Press P%d", pin));
        set_state(btn_gpio_press, false);
        var_gpio_action.set(strf("P%d is not a plain GPIO input.", pin));
        return;
    }
    int level;
    std::string why;
    gpio_press_level(pin, raw, level, why);
    set_window_text(btn_gpio_press, strf("Press P%d", pin));
    set_state(btn_gpio_press, live);
    var_gpio_action.set(strf("P%d: %s; Press drives %s, then click Release.", pin, why.c_str(), level ? "HIGH" : "LOW"));
}

void SimUI::toggle_gpio_input()
{
    int pin = gpio_selected >= 0 ? gpio_selected : selected_gpio();
    if (pin < 0 || !running() || stop_requested) {
        update_gpio_control();
        return;
    }
    std::string err;
    if (gpio_pressed.count(pin)) {
        if (!emu->set_gpio_input(pin, -1, err)) {
            var_state.set("GPIO release failed: " + err);
            return;
        }
        gpio_pressed.erase(pin);
    } else {
        std::map<int, int> wake_pins = sleep_wake_pins();
        u32 raw = 0;
        bool have = gpio_config(pin, raw);
        int level;
        if (wake_pins.count(pin))
            level = wake_pins[pin];
        else if (gpio_is_input(have, raw)) {
            std::string why;
            gpio_press_level(pin, raw, level, why);
        } else {
            update_gpio_control();
            return;
        }
        if (!emu->set_gpio_input(pin, level, err)) {
            var_state.set("GPIO press failed: " + err);
            return;
        }
        gpio_pressed[pin] = level;
    }
    periph_key_valid = false;
    refresh_periph();
    update_gpio_control();
}

void SimUI::release_gpio_inputs()
{
    for (std::map<int, int>::iterator it = gpio_pressed.begin(); it != gpio_pressed.end(); ++it)
        if (emu) {
            std::string err;
            emu->set_gpio_input(it->first, -1, err);
        }
    gpio_pressed.clear();
    periph_key_valid = false;
    update_gpio_control();
}

/* ------------------------------------------------------- meter/LED/camera */
void SimUI::parsed_meter_values(double out[3])
{
    Var *vars[3] = {&var_meter_v, &var_meter_a, &var_meter_w};
    double defs[3] = {230.0, 1.5, 345.0};
    for (int i = 0; i < 3; i++) {
        double d;
        out[i] = parse_double(strip(vars[i]->get()), d) ? (d > 0 ? d : 0.0) : defs[i];
    }
}

void SimUI::apply_energy_meter()
{
    double v = 0.0, a = 0.0, w = 0.0;
    if (!parse_double(strip(var_meter_v.get()), v)) {
        var_state.set("Invalid energy meter reading: could not convert string to float: "
                      + py_repr(strip(var_meter_v.get())));
        return;
    }
    if (!parse_double(strip(var_meter_a.get()), a)) {
        var_state.set("Invalid energy meter reading: could not convert string to float: "
                      + py_repr(strip(var_meter_a.get())));
        return;
    }
    if (!parse_double(strip(var_meter_w.get()), w)) {
        var_state.set("Invalid energy meter reading: could not convert string to float: "
                      + py_repr(strip(var_meter_w.get())));
        return;
    }
    if (v < 0 || a < 0 || w < 0) {
        var_state.set("Invalid energy meter reading: values must be non-negative");
        return;
    }
    const TuyaCfg &decoded = running() ? run_tuya_config : tuya_config;
    double mv[3] = {v, a, w};
    periph::DecodedState info = periph::describe_peripherals(decoded.present ? &decoded.pairs : 0, mv);
    if (emu && emu->has_energy_meter()) {
        const periph::MeterInfo &m = info.meter;
        emu->set_energy_meter(v, a, w, m.present ? m.cf_pin : -1, m.present ? m.cf1_pin : -1,
                              m.present ? m.sel_pin : -1, m.present ? m.sel_lv : 1);
    }
    periph_key_valid = false;
    if (emu)
        refresh_periph();
    else
        refresh_static_periph();
    var_state.set(strf("Energy meter set to %.1f V, %.2f A, %.1f W", v, a, w));
}

void SimUI::apply_adc_input()
{
    std::string ch_s = strip(var_adc_ch.get());
    std::string ch_raw = "0";
    if (!ch_s.empty()) {
        size_t sp = ch_s.find_first_of(" \t");
        ch_raw = sp == std::string::npos ? ch_s : ch_s.substr(0, sp);
    }
    std::string val_raw = strip(var_adc_val.get());
    std::string chip = (emu && !emu->chip_name().empty()) ? emu->chip_name() : var_chip.get();
    int ch = 0, raw = 0;
    std::string err;
    if (!periph::parse_adc_spec(ch_raw + ":" + val_raw, chip, ch, raw, &err)) {
        var_state.set("Invalid ADC input: " + err);
        return;
    }
    adc_overrides[ch] = raw;
    if (emu && emu->has_adc_input())
        emu->set_adc_input(ch, raw);
    periph_key_valid = false;
    if (emu)
        refresh_periph();
    else
        refresh_static_periph();
    double volts = raw * 3.3 / 1023.0;
    var_state.set(strf("ADC %s set to %d (0x%03X, %.2fV)", periph::adc_channel_label(ch, chip).c_str(), raw, raw, volts));
}

void SimUI::update_adc_widget(const periph::AdcInfo *adc)
{
    if (adc && !adc->summary.empty()) {
        var_adc.set(adc->summary);
        return;
    }
    std::string chip = (emu && !emu->chip_name().empty()) ? emu->chip_name() : var_chip.get();
    periph::AdcInfo info = periph::describe_adc(-1, 0, 0, 0, &adc_overrides, -1, chip);
    var_adc.set(info.summary);
}

void SimUI::draw_led_swatch(const periph::LedInfo *led)
{
    swatch_pixels.clear();
    if (!led || !led->present) {
        swatch_bg = LED_OFF_HEX;
    } else {
        std::string hex = !led->live_hex.empty() ? led->live_hex : (!led->hex.empty() ? led->hex : std::string(LED_OFF_HEX));
        swatch_bg = hex;
        swatch_pixels = led->pixels.empty() ? StrList(1, hex) : led->pixels;
    }
    InvalidateRect(led_swatch, 0, TRUE);
}

void SimUI::update_camera_widget()
{
    EmuView st;
    if (!emu || !emu->view(st)) {
        var_camera.set("Camera: none detected");
        cam_rgb.clear();
        InvalidateRect(cam_canvas, 0, TRUE);
        return;
    }
    if (!st.camera_sensor.empty() || st.camera_frame_count > 0 || st.camera_i2c_writes > 0) {
        std::string name = !st.camera_sensor.empty() ? st.camera_sensor : std::string("GC0328C");
        if (st.camera_frame_count > 0)
            var_camera.set(strf("Camera: %s %dx%d \xe2\x80\x94 %d frames (%d B JPEG, %d I2C reg)", name.c_str(),
                                st.camera_width, st.camera_height, st.camera_frame_count,
                                (int)st.camera_last_jpeg.size(), st.camera_i2c_writes));
        else
            var_camera.set(strf("Camera: %s %dx%d (%d I2C reg configured)", name.c_str(), st.camera_width,
                                st.camera_height, st.camera_i2c_writes));
    } else
        var_camera.set("Camera: none detected");
    cam_rgb.clear();
    if (st.camera_frame_rgb.size() == 64 * 48 * 3)
        cam_rgb = st.camera_frame_rgb;
    InvalidateRect(cam_canvas, 0, TRUE);
}

void SimUI::update_periph_widgets(const periph::LedInfo *led, const periph::MeterInfo *meter, const double mv[3])
{
    if (led && led->present)
        var_led.set(led->summary);
    else
        var_led.set("LED driver: none detected");
    draw_led_swatch(led);
    if (meter && meter->present)
        var_meter.set(meter->summary);
    else
        var_meter.set(strf("Meter: BL0942 UART1 ready (%.0fV %.2fA %.0fW)", mv[0], mv[1], mv[2]));
}

void SimUI::refresh_static_periph()
{
    double mv[3];
    parsed_meter_values(mv);
    periph::DecodedState info = periph::describe_peripherals(tuya_config.present ? &tuya_config.pairs : 0, mv,
                                                             -1, 0, 0, 0, -1, &adc_overrides, var_chip.get());
    update_periph_widgets(&info.led, &info.meter, mv);
    update_adc_widget(&info.adc);
    update_camera_widget();
    update_sleep_controls();
}

static std::string bytes_key(const Bytes &b) { return strf("%d:", (int)b.size()) + b; }

void SimUI::refresh_periph()
{
    if (!emu) {
        refresh_static_periph();
        return;
    }
    EmuView st;
    emu->view(st);
    double parsed[3], mv[3];
    parsed_meter_values(parsed);
    if (!emu->meter_values(mv)) {
        mv[0] = parsed[0];
        mv[1] = parsed[1];
        mv[2] = parsed[2];
    }
    /* the redraw key: everything the table and widgets are drawn from */
    std::string key;
    for (std::map<int, u32>::iterator it = st.gpio_cfg.begin(); it != st.gpio_cfg.end(); ++it)
        key += strf("g%d=%x,", it->first, it->second);
    for (std::map<u32, u32>::iterator it = st.pwm_regs.begin(); it != st.pwm_regs.end(); ++it)
        key += strf("p%x=%x,", it->first, it->second);
    for (std::map<std::pair<int, int>, Bytes>::iterator it = st.i2c_frames.begin(); it != st.i2c_frames.end(); ++it)
        key += strf("i%d/%d=", it->first.first, it->first.second) + bytes_key(it->second);
    key += "|ws" + bytes_key(st.ws2812_spi);
    key += strf("|bl%d|m%.17g/%.17g/%.17g|a%d|", st.bl0942_polls, mv[0], mv[1], mv[2], var_all_pins.getb() ? 1 : 0);
    for (std::map<int, int>::iterator it = gpio_pressed.begin(); it != gpio_pressed.end(); ++it)
        key += strf("x%d=%d,", it->first, it->second);
    key += strf("|s%d/%d|", st.in_deep_sleep ? 1 : 0, st.deep_sleep_count);
    for (std::map<int, int>::iterator it = st.deep_sleep_wake_pins.begin(); it != st.deep_sleep_wake_pins.end(); ++it)
        key += strf("w%d=%d,", it->first, it->second);
    key += "|c" + st.camera_sensor + strf("/%d/%d", st.camera_i2c_writes, st.camera_frame_count);
    key += strf("|adc%d/%d/%d/%d|", st.saradc_channel, st.saradc_last_raw, st.saradc_conversions, st.saradc_reads);
    for (std::map<int, int>::iterator it = st.adc_samples.begin(); it != st.adc_samples.end(); ++it)
        key += strf("as%d=%d,", it->first, it->second);
    const std::map<int, int> &adc_in = st.adc_inputs.empty() ? adc_overrides : st.adc_inputs;
    for (std::map<int, int>::const_iterator it = adc_in.begin(); it != adc_in.end(); ++it)
        key += strf("ai%d=%d,", it->first, it->second);
    if (periph_key_valid && key == periph_key) {
        update_gpio_control();
        update_sleep_controls();
        return;
    }
    periph_key_valid = true;
    periph_key = key;

    std::string image = !run_image.empty() ? run_image : var_image.get();
    const TuyaCfg &tuya = running() ? run_tuya_config : tuya_config;
    const periph::Pairs *pairs = tuya.present ? &tuya.pairs : 0;
    std::string chip;
    if (emu->gpio_pins() > periph::PIN_COUNT) {
        chip = emu->chip_name();
        if (chip.empty())
            chip = emu->chip_variant();
        if (chip.empty())
            chip = emu->is_bk7252() ? "BK7252" : "BK7231U";
    } else {
        chip = periph::chip_from_name(image);
        if (chip.empty())
            chip = emu->chip_name();
    }
    periph::DecodedState data;
    if (chip == "ESP32" || chip == "ESP8266") {
        data.present = !st.gpio_cfg.empty();
        data.raw = st.gpio_cfg;
        for (std::map<int, u32>::const_iterator i = st.gpio_cfg.begin(); i != st.gpio_cfg.end(); ++i) {
            periph::GpioRow row;
            row.pin = i->first; row.state = "live";
            row.detail = (i->second & 8) ? strf("INPUT = %u", i->second & 1)
                : strf("OUTPUT = %u", (i->second >> 1) & 1);
            data.gpio.push_back(row);
        }
    } else data = periph::decode_state(st.gpio_cfg, st.pwm_regs, chip, &st.i2c_frames, &st.i2c_last_on,
                                                     &st.ws2812_spi, 0, pairs, st.bl0942_polls, mv,
                                                     st.saradc_channel, st.saradc_conversions, st.saradc_reads,
                                                     &st.adc_samples, st.saradc_last_raw, &adc_in);
    int selected = gpio_selected;
    SendMessageW(tree.hwnd, WM_SETREDRAW, FALSE, 0);
    tree.clear();
    bool in_sleep = st.in_deep_sleep;
    std::map<int, int> &wake_pins = st.deep_sleep_wake_pins;
    if (!data.present && !(in_sleep && !wake_pins.empty())) {
        SendMessageW(tree.hwnd, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(tree.hwnd, 0, TRUE);
        var_pwm.set("firmware has not touched GPIO or PWM yet");
        periph::DecodedState info = periph::describe_peripherals(pairs, mv, st.saradc_channel, st.saradc_conversions,
                                                                 st.saradc_reads, &st.adc_samples, st.saradc_last_raw,
                                                                 &adc_in, chip);
        update_periph_widgets(&info.led, &info.meter, mv);
        update_adc_widget(&info.adc);
        update_camera_widget();
        update_gpio_control();
        update_sleep_controls();
        return;
    }
    bool show_all = var_all_pins.getb();
    std::vector<periph::GpioRow> rows = data.gpio;
    std::set<int> seen;
    for (size_t i = 0; i < rows.size(); i++)
        seen.insert(rows[i].pin);
    if (in_sleep && !wake_pins.empty()) {
        for (std::map<int, int>::iterator it = wake_pins.begin(); it != wake_pins.end(); ++it)
            if (!seen.count(it->first)) {
                periph::GpioRow r;
                r.pin = it->first;
                r.state = "factory";
                r.detail = "INPUT (deep-sleep wake)";
                rows.push_back(r);
            }
        /* sorted(key=pin): stable */
        std::vector<periph::GpioRow> sorted;
        std::vector<int> pins(seen.begin(), seen.end());
        for (std::map<int, int>::iterator it = wake_pins.begin(); it != wake_pins.end(); ++it)
            pins.push_back(it->first);
        std::sort(pins.begin(), pins.end());
        pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
        for (size_t p = 0; p < pins.size(); p++)
            for (size_t i = 0; i < rows.size(); i++)
                if (rows[i].pin == pins[p])
                    sorted.push_back(rows[i]);
        rows = sorted;
    }
    for (size_t i = 0; i < rows.size(); i++) {
        int pin = rows[i].pin;
        std::string detail = rows[i].detail;
        bool is_wake = in_sleep && wake_pins.count(pin);
        if (rows[i].state == "factory" && !show_all && !is_wake)
            continue;
        if (is_wake)
            detail += std::string(" [WAKE ") + (wake_pins[pin] ? "HIGH" : "LOW") + "]";
        std::string tag;
        if (rows[i].state == "factory" && !is_wake)
            tag = "factory";
        else if (detail.find("OUTPUT") != std::string::npos)
            tag = "out";
        else
            tag = "func";
        std::map<int, u32>::iterator raw = data.raw.find(pin);
        if (gpio_pressed.count(pin)) {
            detail += std::string(" \xe2\x80\x94 PRESSED ") + (gpio_pressed[pin] ? "HIGH" : "LOW");
            tag = "pressed";
        }
        StrList vals;
        vals.push_back(strf("%d", pin));
        vals.push_back(rows[i].alias);
        vals.push_back(detail);
        vals.push_back(raw == data.raw.end() ? std::string() : strf("0x%02X", raw->second));
        tree.insert(strf("gpio:%d", pin), vals, tag);
    }
    for (size_t i = 0; i < data.func.size(); i++) {
        StrList vals;
        vals.push_back("");
        vals.push_back(data.func[i].first);
        vals.push_back("function select");
        vals.push_back(data.func[i].second);
        tree.insert(strf("I%03X", (unsigned)(tree.size() + 1)), vals, "func");
    }
    if (selected >= 0 && tree.exists(strf("gpio:%d", selected)))
        tree.selection_set(strf("gpio:%d", selected));
    SendMessageW(tree.hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree.hwnd, 0, TRUE);

    std::string pins_s = "-";
    if (!data.pwm_pins.empty()) {
        StrList pl;
        for (size_t i = 0; i < data.pwm_pins.size(); i++)
            pl.push_back(strf("%d", data.pwm_pins[i]));
        pins_s = "[" + join(pl, ", ") + "]";
    }
    if (!data.pwm_channels.empty()) {
        StrList cl;
        for (size_t i = 0; i < data.pwm_channels.size(); i++) {
            const periph::PwmChan &c = data.pwm_channels[i];
            cl.push_back(strf("ch%d %.0f Hz %.0f%%", c.ch, c.freq, c.has_pct ? c.pct : 0.0));
        }
        var_pwm.set("PWM (" + data.pwm_layout + " layout), pins " + pins_s + ":  " + join(cl, "   "));
    } else
        var_pwm.set("PWM pins " + pins_s + ", no channels decoded");
    update_periph_widgets(&data.led, &data.meter, mv);
    update_adc_widget(&data.adc);
    update_camera_widget();
    update_gpio_control();
    update_sleep_controls();
}

std::string SimUI::render_uart2(const Bytes &data)
{
    std::string text = data; /* latin-1, one char per byte */
    if (!var_hide_tick.getb()) {
        if (!u2_partial.empty()) {
            text = u2_partial + text;
            u2_partial.clear();
        }
        return text;
    }
    StrList lines = split(u2_partial + text, "\n");
    u2_partial = lines.back();
    lines.pop_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); i++)
        if (lines[i].find(QUICKTICK) == std::string::npos)
            out += lines[i] + "\n";
    return out;
}

void SimUI::copy(Pane &p, const char *label)
{
    std::string text = pane_text(p);
    std::wstring w = widen(text);
    size_t nchars = w.size();
    std::wstring crlf;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'\n')
            crlf += L'\r';
        crlf += w[i];
    }
    if (OpenClipboard(root)) {
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t));
        if (g) {
            wchar_t *dst = (wchar_t *)GlobalLock(g);
            memcpy(dst, crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    }
    var_state.set(strf("copied %d characters from %s", (int)nchars, label));
}

void SimUI::clear()
{
    u2_partial.clear();
    pane_clear(txt1);
    pane_clear(txt2);
    pane_clear(txt_net);
    hex_col = 0;
    {
        ScopedLock lk(out_lock);
        net_lines.clear();
        frames.clear();
    }
    frames_dropped = 0;
    net_count = 0;
    refresh_details();
    set_window_text(net_frame, NET_TITLE);
    append_utf8(txt_net, NET_IDLE_HINT + "\n", "note");
    net_hint_shown = true;
}

void SimUI::save()
{
    std::string path = ask_save(".log", L"Log\0*.log\0All\0*.*\0");
    if (path.empty())
        return;
    /* get("1.0", "end") carries one trailing newline; text mode writes CRLF */
    std::string body = "=== UART1 ===\n" + pane_text(txt1) + "\n" + "\n=== UART2 ===\n" + pane_text(txt2) + "\n"
                       + "\n=== network (virtual Wi-Fi and IP bridge) ===\n" + pane_text(txt_net) + "\n";
    body = replace_all(body, "\n", "\r\n");
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        message("Could not write " + path, true);
        return;
    }
    DWORD n;
    WriteFile(h, body.data(), (DWORD)body.size(), &n, 0);
    CloseHandle(h);
    var_state.set("saved to " + path);
}

void SimUI::browse()
{
    std::string path = ask_open();
    if (!path.empty())
        load_image(path, false);
}

/* --------------------------------------------------------------- run/stop */
void SimUI::s_sink(void *ctx, int port, u8 byte)
{
    SimUI *ui = (SimUI *)ctx;
    ScopedLock lk(ui->out_lock);
    if (port == 1)
        ui->out1 += (char)byte;
    else
        ui->out2 += (char)byte;
}

void SimUI::s_net_sink(void *ctx, const std::string &line) { ((SimUI *)ctx)->net_sink(line); }
void SimUI::net_sink(const std::string &line)
{
    ScopedLock lk(out_lock);
    net_lines.push_back(line);
}

void SimUI::s_frame_sink(void *ctx, const std::string &dir, const Bytes &frame, u64 now_us)
{
    ((SimUI *)ctx)->frame_sink(dir, frame, now_us);
}

void SimUI::frame_sink(const std::string &dir, const Bytes &frame, u64 now_us)
{
    ScopedLock lk(out_lock);
    Frame f;
    f.dir = dir;
    f.data = frame;
    f.us = now_us;
    frames.push_back(f);
    while (frames.size() > MAX_FRAMES) {
        frames.pop_front();
        frames_dropped++;
    }
}

bool SimUI::thread_alive()
{
    if (thread_mode == 2)
        return true;
    if (thread_mode == 1 && thread)
        return thread->is_alive();
    return false;
}

bool SimUI::running() { return emu != 0 && thread_alive(); }

bool SimUI::console_ready()
{
    if (!running())
        return false;
    EmuView v;
    emu->view(v);
    return v.insn_count >= emu->uart1_rx_delay();
}

void SimUI::on_run()
{
    if (hooks.on_run) {
        hooks.on_run(this, hooks.on_run_ctx);
        return;
    }
    on_run_real();
}

struct WorkerArgs {
    SimUI *ui;
    std::string path;
    RunCfg cfg;
};

static void worker_main(void *arg)
{
    WorkerArgs *a = (WorkerArgs *)arg;
    a->ui->worker(a->path, a->cfg);
    delete a;
}

void SimUI::on_run_real()
{
    if (thread_alive())
        return;
    std::string path = strip(var_image.get());
    if (path.empty() || !path_isfile(path)) {
        message("Pick a flash dump first.", true);
        return;
    }
    refresh_tuya_config(false, path, true);
    RunCfg cfg;
    std::string err;
    if (!collect_cfg(cfg, err)) {
        select_tab(1);
        message(err, true);
        return;
    }
    run_tuya_config = tuya_config;
    run_image = path;
    bool esp = cfg.chip == "ESP32" || cfg.chip == "ESP8266";
    set_window_text(grp_u1, esp ? "UART0 - command console / firmware log" : "UART1 - MCU link / command console");
    set_window_text(grp_u2, esp ? "UART1 / UART2 - additional serial output" : "UART2 - firmware debug log");
    set_window_text(grp_send, esp ? "send to UART0" : "send to UART1");
    if (has_pending_uart1_rx) {
        cfg.has_uart1_rx = true;
        cfg.uart1_rx = pending_uart1_rx;
    }
    has_pending_uart1_rx = false;
    pending_uart1_rx.clear();
    double mv[3];
    parsed_meter_values(mv);
    periph::DecodedState mi = periph::describe_peripherals(run_tuya_config.present ? &run_tuya_config.pairs : 0, mv);
    cfg.has_meter = true;
    cfg.meter_v = mv[0];
    cfg.meter_a = mv[1];
    cfg.meter_w = mv[2];
    cfg.cf_pin = mi.meter.present ? mi.meter.cf_pin : -1;
    cfg.cf1_pin = mi.meter.present ? mi.meter.cf1_pin : -1;
    cfg.sel_pin = mi.meter.present ? mi.meter.cf_pin >= 0 || mi.meter.cf1_pin >= 0 || mi.meter.sel_pin >= 0 ? mi.meter.sel_pin : -1 : -1;
    cfg.sel_lv = mi.meter.present ? mi.meter.sel_lv : 1;
    cfg.adc_inputs = adc_overrides;
    /* Private cloud credentials only for the offline TLS peer; a real-cloud
     * run is a transparent relay and gets no copy. */
    if (cfg.net_out) {
        cfg.tls_psks.clear();
        cfg.tuya_legacy_identity_psk = false;
    } else if (run_tuya_config.present) {
        cfg.tls_psks = tuya_config::tls_psk_candidates(run_tuya_config.dec);
        cfg.tuya_legacy_identity_psk = tuya_config::uses_tuya_legacy_identity_psk(run_tuya_config.dec);
    }
    active_forward_url = cfg.forwards.empty() ? std::string() : strf("http://127.0.0.1:%d/", cfg.forwards[0].first);
    update_open_forward();
    save_settings();

    release_gpio_inputs();
    clear();
    periph_key_valid = false;
    tx_count[1] = tx_count[2] = 0;
    last_insns = 0;
    last_reset_count = 0;
    rate = 0.0;
    has_last_rate_at = false;
    {
        ScopedLock lk(result_lock);
        has_finished = has_error = false;
        finished.clear();
        error.clear();
    }
    if (emu && emu_owned)
        delete emu;
    emu = 0;
    emu_owned = false;
    stop_requested = false;
    pending_run = false;
    {
        ScopedLock lk(out_lock);
        out1.clear();
        out2.clear();
    }
    started_at = mono_time();

    set_state(btn_run, false);
    set_state(btn_stop, true);
    set_state(btn_pause, true);
    set_window_text(btn_pause, "Pause");
    var_state.set("loading " + path_basename(path) + " ...");
    if (thread) {
        thread->join(-1);
        delete thread;
    }
    thread = new Thread();
    thread_mode = 1;
    WorkerArgs *a = new WorkerArgs;
    a->ui = this;
    a->path = path;
    a->cfg = cfg;
    if (!thread->start(worker_main, a)) {
        delete a;
        ScopedLock lk(result_lock);
        has_error = true;
        error = "RuntimeError: cannot start the emulation thread";
    }
}

void SimUI::worker(const std::string &path, const RunCfg &cfg)
{
    EmuIface *e = 0;
    std::string err;
    BuildOptions opt;
    opt.chip = cfg.chip;
    opt.rom_path = cfg.rom_path;
    opt.xtal_mhz = cfg.xtal_mhz;
    opt.psram_mb = cfg.psram_mb;
    opt.with_boot = cfg.boot;
    opt.only_uart = true;
    opt.uart1_hex = false;
    opt.tuyamcu = cfg.tuyamcu;
    opt.tuyamcu_pid = cfg.tuyamcu_pid;
    opt.tuyamcu_raw = cfg.tuyamcu_raw;
    opt.xvr_selfclear = cfg.xvr_selfclear;
    opt.ble_core = cfg.blecore ? 1 : -1;
    opt.wifi = cfg.wifi;
    opt.adc_inputs = cfg.adc_inputs;
    if (cfg.has_uart1_rx && !cfg.uart1_rx.empty())
        opt.uart1_rx = cfg.uart1_rx;
    e = hooks.build ? hooks.build(this, path, opt, cfg.key, err, hooks.build_ctx)
                    : real_build(this, path, opt, cfg.key, err, 0);
    if (!e) {
        ScopedLock lk(result_lock);
        has_error = true;
        error = err;
        return;
    }
    bool ok = true;
    if (cfg.has_meter && e->has_energy_meter())
        e->set_energy_meter(cfg.meter_v, cfg.meter_a, cfg.meter_w, cfg.cf_pin, cfg.cf1_pin, cfg.sel_pin, cfg.sel_lv);
    if (cfg.wifi) {
        PeerArgs p;
        p.role = cfg.wifi_role;
        p.has_ssid = cfg.has_ssid;
        p.ssid = cfg.wifi_ssid;
        p.has_pass = cfg.has_pass;
        p.passphrase = cfg.wifi_pass;
        p.forwards = cfg.forwards;
        p.net_out = cfg.net_out;
        p.has_pcap = cfg.has_pcap;
        p.pcap = cfg.pcap;
        p.http_gets = cfg.http_gets;
        p.mqtt_pubs = cfg.mqtt_pubs;
        if (!cfg.net_out) {
            p.tls_psks = cfg.tls_psks;
            p.tuya_legacy_identity_psk = cfg.tuya_legacy_identity_psk;
        }
        ok = e->attach_wifi_peer(p, err);
    }
    if (ok) {
        e->setup();
        emu_owned = true;
        emu = e;
        /* A Stop pressed while loading found no emulator to stop. */
        if (stop_requested)
            e->stop();
        e->run();
        std::string run_err;
        ScopedLock lk(result_lock);
        if (e->run_error(run_err)) {
            has_error = true;
            error = run_err;
        } else {
            has_finished = true;
            finished = "emulation finished";
        }
    } else {
        ScopedLock lk(result_lock);
        has_error = true;
        error = err;
    }
    /* The next run needs the forward port back, and a capture file should be
     * complete once the run is over. */
    e->close_network();
    if (emu != e)
        delete e;
}

void SimUI::on_pause()
{
    if (!emu)
        return;
    if (emu->is_paused()) {
        emu->pause(false);
        set_window_text(btn_pause, "Pause");
    } else {
        emu->pause(true);
        set_window_text(btn_pause, "Resume");
    }
}

void SimUI::on_stop()
{
    stop_requested = true;
    release_gpio_inputs();
    if (emu)
        emu->stop();
    set_state(btn_stop, false);
    set_state(btn_pause, false);
    set_window_text(btn_pause, "Pause");
    set_state(btn_open_forward, false);
}

void SimUI::on_close()
{
    save_settings();
    release_gpio_inputs();
    if (emu)
        emu->stop();
}

/* ------------------------------------------------------------------ send */
void SimUI::send_bytes(const Bytes &payload)
{
    if (!running())
        return;
    emu->feed_uart1(payload);
    std::string shown;
    if (var_hex.getb())
        shown = hexstr_sep(payload, " ");
    else
        shown = rstrip(replace_all(latin1_to_utf8(payload), "\r", ""), "\n");
    append_utf8(txt1, "> " + shown + "\n", "sent");
}

static Bytes line_ending(const std::string &name)
{
    if (name == "CR+LF")
        return "\r\n";
    if (name == "LF")
        return "\n";
    if (name == "CR")
        return "\r";
    return "";
}

static Bytes latin1_or_replace(const std::string &utf8)
{
    Bytes out;
    std::wstring w = widen(utf8);
    for (size_t i = 0; i < w.size(); i++)
        out += (w[i] < 256) ? (char)w[i] : '?';
    return out;
}

void SimUI::send_command(const std::string &text)
{
    send_bytes(latin1_or_replace(text) + line_ending(var_ending.get()));
    history.push_back(text);
    history_pos = -1;
}

void SimUI::on_send()
{
    if (!running())
        return;
    std::string text = var_input.get();
    if (text.empty() && var_mode.get() == "HEX")
        return;
    Bytes payload;
    if (var_mode.get() == "HEX") {
        std::string err;
        if (!parse_hex_blob(text, payload, err)) {
            var_state.set(err);
            return;
        }
    } else
        payload = latin1_or_replace(text);
    send_bytes(payload + line_ending(var_ending.get()));
    if (!text.empty())
        history.push_back(text);
    history_pos = -1;
    var_input.set("");
}

void SimUI::history_back()
{
    if (history.empty())
        return;
    history_pos = history_pos < 0 ? (int)history.size() - 1 : (history_pos > 0 ? history_pos - 1 : 0);
    var_input.set(history[(size_t)history_pos]);
    SendMessageW(entry, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
}

void SimUI::history_fwd()
{
    if (history_pos < 0)
        return;
    history_pos++;
    if (history_pos >= (int)history.size()) {
        history_pos = -1;
        var_input.set("");
    } else
        var_input.set(history[(size_t)history_pos]);
    SendMessageW(entry, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
}

/* ------------------------------------------------------------------ poll */
void SimUI::poll()
{
    Bytes c1, c2;
    StrList lines;
    {
        ScopedLock lk(out_lock);
        c1.swap(out1);
        c2.swap(out2);
        lines.swap(net_lines);
    }
    if (!c1.empty()) {
        tx_count[1] += (long)c1.size();
        std::string r = render_uart1(c1);
        if (var_hex.getb())
            append_utf8(txt1, r);
        else
            append(txt1, latin1_widen(c1));
    }
    if (!c2.empty()) {
        tx_count[2] += (long)c2.size();
        append(txt2, latin1_widen(render_uart2(c2)));
    }
    if (!lines.empty()) {
        if (net_hint_shown) {
            pane_clear(txt_net);
            net_hint_shown = false;
        }
        bool ip_only = var_net_ip_only.getb();
        for (size_t i = 0; i < lines.size(); i++) {
            net_count++;
            std::string tag = net_tag(lines[i]);
            if (ip_only && tag == "wifi")
                continue;
            append_utf8(txt_net, lines[i] + "\n", tag.c_str());
        }
        set_window_text(net_frame, strf("%s - %ld lines", NET_TITLE, net_count));
    }
    /* Keep an open details window current, once a second at most. */
    if (detail_exists() && mono_time() - detail_drawn_at > 1.0) {
        long total;
        {
            ScopedLock lk(out_lock);
            total = (long)frames.size() + frames_dropped;
        }
        if (total != detail_drawn_count) {
            detail_drawn_count = total;
            refresh_details();
        }
        detail_drawn_at = mono_time();
    }
    refresh_periph();
    update_state();
}

void SimUI::update_state()
{
    bool alive = thread_alive();
    bool has_err, has_fin;
    std::string err, fin;
    {
        ScopedLock lk(result_lock);
        has_err = has_error;
        err = error;
        has_fin = has_finished;
        fin = finished;
    }
    if (has_err && !alive) {
        var_state.set("ERROR  " + err);
        append_utf8(txt2, "\n[!] " + err + "\n", "note");
        {
            ScopedLock lk(result_lock);
            has_error = false;
            error.clear();
        }
        finish();
        return;
    }
    if (has_fin && !alive) {
        var_state.set("stopped after " + fmt_insns() + " insns");
        append_utf8(txt2, "\n--- " + fin + " ---\n", "note");
        {
            ScopedLock lk(result_lock);
            has_finished = false;
            finished.clear();
        }
        finish();
        return;
    }
    if (!emu) {
        update_open_forward();
        update_gpio_control();
        update_sleep_controls();
        return;
    }
    update_open_forward();
    EmuView st;
    emu->view(st);
    u64 insns = st.insn_count;
    u64 resets = emu->reset_count();
    double now = mono_time();
    if (resets != last_reset_count) {
        last_reset_count = resets;
        last_insns = insns;
        last_rate_at = now;
        has_last_rate_at = true;
        rate = 0.0;
    }
    if (has_last_rate_at) {
        double dt = now - last_rate_at;
        if (dt >= 0.5) {
            rate = 0.7 * rate + 0.3 * (((double)insns - (double)last_insns) / dt);
            last_insns = insns;
            last_rate_at = now;
        }
    } else {
        last_insns = insns;
        last_rate_at = now;
        has_last_rate_at = true;
    }
    bool ready = console_ready();
    set_send_enabled(ready);
    std::string what;
    if (!alive)
        what = "stopped";
    else if (emu->is_paused())
        what = "PAUSED";
    else if (st.in_deep_sleep)
        what = "DEEP SLEEP";
    else
        what = "running";
    u64 guest_us = 0;
    const bool has_guest_clock = emu->elapsed_us(guest_us);
    const double guest_seconds = has_guest_clock ? guest_us / 1e6 : (double)insns / GUEST_SECOND_INSNS;
    var_state.set(strf("%s   %s insns (%.0fk/s)   %s%.1f s guest   WDT resets %d   %.0f s wall   "
                       "PC 0x%08x   TX u1 %ld / u2 %ld   RX queued %d   console %s",
                       what.c_str(), fmt_insns().c_str(), rate / 1000.0, has_guest_clock ? "" : "~", guest_seconds,
                       (int)resets, now - started_at, st.last_pc, tx_count[1], tx_count[2],
                       (int)emu->uart1_rx_queued(), ready ? "ready" : "not listening yet"));
    update_gpio_control();
    update_sleep_controls();
}

std::string SimUI::fmt_insns()
{
    u64 n = 0;
    if (emu) {
        EmuView v;
        emu->view(v);
        n = v.insn_count;
    }
    return n >= 1000000 ? strf("%.1fM", n / 1e6) : strf("%llu", (unsigned long long)n);
}

void SimUI::set_send_enabled(bool on)
{
    set_state(btn_send, on);
    for (size_t i = 0; i < quick_buttons.size(); i++)
        set_state(quick_buttons[i], on);
}

void SimUI::finish()
{
    if (hooks.finish) {
        hooks.finish(this, hooks.finish_ctx);
        return;
    }
    finish_real();
}

void SimUI::finish_real()
{
    release_gpio_inputs();
    update_sleep_controls();
    set_state(btn_run, true);
    set_state(btn_stop, false);
    set_state(btn_pause, false);
    set_window_text(btn_pause, "Pause");
    set_state(btn_open_forward, false);
    active_forward_url.clear();
    set_send_enabled(false);
    if (pending_run) { /* an image was dropped while this run ended */
        pending_run = false;
        on_run();
    }
}

} /* namespace simui */
