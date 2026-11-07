/* sim_ui --selftest: a port of _run_selftests() in tools/sim_ui.py. Drives the
 * real (withdrawn) window the way the emulation thread and a person would,
 * with fake emulators where Python used SimpleNamespace objects, and checks
 * the Win32 state where Python inspected Tk widgets. Same messages, same
 * count. Also --package-smoke (_run_package_smoke). */
#include "sim_ui.h"
#include "netstack.h"
#include "beken_crypto.h"

#include <richedit.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace simui {

static int g_n = 0;

#define CHECK(cond, msg)                                         \
    do {                                                         \
        std::string m_ = (msg);                                  \
        if (!(cond)) {                                           \
            printf("FAIL: %s\n", m_.c_str());                    \
            fflush(stdout);                                      \
            return -1;                                           \
        }                                                        \
        g_n++;                                                   \
        printf("  [PASS] %s\n", m_.c_str());                     \
    } while (0)

static bool has(const std::string &s, const std::string &sub) { return s.find(sub) != std::string::npos; }

/* ------------------------------------------------------------ frame kit */
static Bytes ipv4(const Bytes &src, const Bytes &dst, int proto, const Bytes &payload)
{
    static u16 ident = 1;
    Bytes h = Bytes("\x45\x00", 2) + p16be((u32)(20 + payload.size())) + p16be(ident++) + Bytes("\x40\x00\x40", 3);
    h += (char)proto;
    h += Bytes("\x00\x00", 2) + src + dst;
    u16 c = netstack::inet_checksum(h);
    h[10] = (char)(c >> 8);
    h[11] = (char)c;
    return h + payload;
}

static u16 l4sum(const Bytes &src, const Bytes &dst, int proto, const Bytes &seg)
{
    Bytes pseudo = src + dst;
    pseudo += (char)0;
    pseudo += (char)proto;
    pseudo += p16be((u32)seg.size());
    return netstack::inet_checksum(pseudo + seg);
}

static const u8 TCP_SYN = 0x02, TCP_PSH = 0x08, TCP_ACK = 0x10;
static const u16 ETH_IP = 0x0800;

static Bytes tcp_segment(const Bytes &src, const Bytes &dst, int sport, int dport, u32 seq, u32 ack, u8 flags,
                         int window, const Bytes &payload = Bytes())
{
    Bytes seg = p16be(sport) + p16be(dport) + p32be(seq) + p32be(ack);
    seg += (char)(5 << 4);
    seg += (char)flags;
    seg += p16be(window) + p16be(0) + p16be(0) + payload;
    u16 c = l4sum(src, dst, 6, seg);
    seg[16] = (char)(c >> 8);
    seg[17] = (char)c;
    return ipv4(src, dst, 6, seg);
}

static Bytes udp_packet(const Bytes &src, const Bytes &dst, int sport, int dport, const Bytes &payload)
{
    Bytes seg = p16be(sport) + p16be(dport) + p16be((u32)(8 + payload.size())) + p16be(0) + payload;
    u16 c = l4sum(src, dst, 17, seg);
    if (!c)
        c = 0xFFFF;
    seg[6] = (char)(c >> 8);
    seg[7] = (char)c;
    return ipv4(src, dst, 17, seg);
}

static Bytes eth_frame(const Bytes &dst, const Bytes &src, u16 type, const Bytes &payload)
{
    return dst + src + p16be(type) + payload;
}

/* ------------------------------------------------------------ files */
static std::string temp_dir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    std::string d = narrow(std::wstring(buf, n));
    while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/'))
        d.erase(d.size() - 1);
    return d;
}

static std::string temp_file(const char *suffix)
{
    static int k = 0;
    std::string p = path_join(temp_dir(), strf("bks_ui_%lu_%d_%lu%s", (unsigned long)GetCurrentProcessId(), ++k,
                                               (unsigned long)GetTickCount(), suffix));
    write_file(p, Bytes());
    return p;
}

static std::string make_temp_dir()
{
    static int k = 0;
    std::string p = path_join(temp_dir(), strf("bks_ui_dir_%lu_%d_%lu", (unsigned long)GetCurrentProcessId(), ++k,
                                               (unsigned long)GetTickCount()));
    CreateDirectoryW(widen(p).c_str(), 0);
    return p;
}

static void remove_tree(const std::string &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(path_join(dir, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..")
                continue;
            std::string p = path_join(dir, narrow(n));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(p);
            else
                DeleteFileW(widen(p).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(widen(dir).c_str());
}

static std::string read_text(const std::string &p)
{
    Bytes b;
    read_file(p, b);
    return b;
}

/* ------------------------------------------------------------ hooks */
static std::string g_save_path;
static std::string hook_ask_save(const char *ext, void *ctx)
{
    (void)ext;
    (void)ctx;
    return g_save_path;
}

static void hook_message(const std::string &title, const std::string &msg, bool error, void *ctx)
{
    (void)title;
    (void)error;
    (void)ctx;
    printf("  (dialog) %s\n", msg.c_str());
}

struct Started {
    std::vector<StrList> runs;
};
static void hook_record_run(SimUI *ui, void *ctx)
{
    Started *s = (Started *)ctx;
    StrList r;
    r.push_back(ui->var_image.get());
    r.push_back(ui->var_chip.get());
    r.push_back(ui->var_key.get());
    r.push_back(ui->optv("wifi")->getb() ? "True" : "False");
    r.push_back(ui->optv("net_out")->getb() ? "True" : "False");
    r.push_back(ui->optv("wifi_role")->get());
    r.push_back(ui->optv("wifi_ssid")->get());
    r.push_back(ui->optv("wifi_pass")->get());
    s->runs.push_back(r);
}

/* the worker's fake emulator (FakeEmu / fake_build) */
struct Calls {
    bool has_build;
    BuildOptions build;
    bool has_peer;
    PeerArgs peer;
    StrList order;
    int closed;
    bool has_run_error;
    std::string run_error;
    bool attach_fails;
    Calls() : has_build(false), has_peer(false), closed(0), has_run_error(false), attach_fails(false) {}
};

class FakeEmu : public EmuIface {
public:
    explicit FakeEmu(Calls *c) : c_(c), has_err_(c->has_run_error), err_(c->run_error) {}
    bool attach_wifi_peer(const PeerArgs &a, std::string &err)
    {
        c_->peer = a;
        c_->has_peer = true;
        if (c_->attach_fails) {
            err = "OSError: cannot open the capture";
            return false;
        }
        return true;
    }
    void setup() { c_->order = StrList(1, "setup"); }
    void stop() { c_->order.push_back("stop"); }
    void run() { c_->order.push_back("run"); }
    void close_network() { c_->closed++; }
    bool run_error(std::string &out)
    {
        if (!has_err_)
            return false;
        out = err_;
        return true;
    }
private:
    Calls *c_;
    bool has_err_;
    std::string err_;
};

static EmuIface *fake_build(SimUI *ui, const std::string &path, const BuildOptions &opt, const std::string &key,
                            std::string &err, void *ctx)
{
    (void)ui;
    (void)path;
    (void)key;
    (void)err;
    Calls *c = (Calls *)ctx;
    c->build = opt;
    c->has_build = true;
    return new FakeEmu(c);
}

class FeedEmu : public EmuIface {
public:
    std::vector<Bytes> fed;
    void feed_uart1(const Bytes &d) { fed.push_back(d); }
};

/* GPIOEmu, and the SimpleNamespace emulators of the later sections */
class ViewEmu : public EmuIface {
public:
    EmuView v;
    std::vector<std::pair<int, int> > inputs;
    bool stopped, ext, bk7252, has_meter_attr, meter_set, wake_ok, listening;
    std::string variant;
    double meter[3], set_vals[3];
    u64 rx_delay;
    std::vector<std::pair<std::string, int> > wakes;
    std::vector<std::pair<int, int> > adc_pushed;
    ViewEmu()
        : stopped(false), ext(false), bk7252(false), has_meter_attr(false), meter_set(false), wake_ok(false),
          listening(false), rx_delay(0)
    {
        meter[0] = 230.0;
        meter[1] = 1.5;
        meter[2] = 345.0;
        set_vals[0] = set_vals[1] = set_vals[2] = 0;
        v.valid = true;
    }
    bool set_gpio_input(int pin, int level, std::string &err)
    {
        (void)err;
        inputs.push_back(std::make_pair(pin, level));
        return true;
    }
    void stop() { stopped = true; }
    bool view(EmuView &out)
    {
        out = v;
        return true;
    }
    bool has_extended_gpio() { return ext; }
    bool is_bk7252() { return bk7252; }
    std::string chip_variant() { return variant; }
    std::string chip_name() { return !variant.empty() ? variant : (bk7252 ? "BK7252" : (ext ? "BK7231U" : "BK7231")); }
    int gpio_pins() { return ext ? 40 : 32; }
    bool has_energy_meter() { return has_meter_attr; }
    void set_energy_meter(double a, double b, double c, int, int, int, int)
    {
        meter_set = true;
        set_vals[0] = a;
        set_vals[1] = b;
        set_vals[2] = c;
    }
    bool meter_values(double out[3])
    {
        if (!has_meter_attr)
            return false;
        out[0] = meter[0];
        out[1] = meter[1];
        out[2] = meter[2];
        return true;
    }
    bool has_adc_input() { return true; }
    void set_adc_input(int ch, int val)
    {
        adc_pushed.push_back(std::make_pair(ch, val));
        v.adc_inputs[ch] = val;
    }
    bool has_wake() { return wake_ok; }
    void wake_from_deep_sleep(const std::string &reason, int pin)
    {
        wakes.push_back(std::make_pair(reason, pin));
        v.in_deep_sleep = false;
        v.last_wake_reason = reason;
        v.last_wake_pin = pin;
    }
    u64 uart1_rx_delay() { return rx_delay; }
    bool forward_listening() { return listening; }
};

static std::vector<std::pair<std::string, int> > g_opened;
static bool hook_browser(const std::string &url, int new_, void *ctx)
{
    (void)ctx;
    g_opened.push_back(std::make_pair(url, new_));
    return true;
}

struct ExtractCalls {
    StrList calls;
    std::string dump;
};
static bool fake_extract(const std::string &path, TuyaCfg &out, std::string &err, void *ctx)
{
    (void)err;
    ExtractCalls *x = (ExtractCalls *)ctx;
    x->calls.push_back(path);
    out = TuyaCfg();
    if (!same_file(path, x->dump))
        return true;
    out.present = true;
    out.ident = new_tuya_ident();
    out.offset = 0x1EE000;
    out.encrypted = true;
    out.pairs.push_back(std::make_pair(std::string("rl1_pin"), std::string("6")));
    out.pairs.push_back(std::make_pair(std::string("module"), std::string("CB2S")));
    out.dps = 1;
    out.dec.valid = true;
    out.dec.ids.set("auth_key", "secret");
    out.dec.raw = "{\"passwd\":\"must-not-reach-the-widget\"}";
    out.dec.has_network = true;
    json_parse("{\"ssid\": \"must-not-reach-the-widget\"}", out.dec.network);
    out.human.push_back(std::make_pair(std::string("Storage"), std::string("encrypted (decrypted with device key)")));
    out.human.push_back(std::make_pair(std::string("Device key"), std::string("12****************************34")));
    out.human.push_back(std::make_pair(std::string("Paired WiFi SSID"), std::string("Ho****et")));
    out.human.push_back(std::make_pair(std::string("-"), std::string("Relay (channel 1) on P6")));
    return true;
}

static TuyaCfg g_private;
static bool private_extract(const std::string &path, TuyaCfg &out, std::string &err, void *ctx)
{
    (void)path;
    (void)err;
    (void)ctx;
    out = g_private;
    return true;
}

struct Finishes {
    int n;
};
static void hook_finish(SimUI *ui, void *ctx)
{
    (void)ui;
    ((Finishes *)ctx)->n++;
}

static StrList combo_values(HWND h)
{
    StrList out;
    LRESULT n = SendMessageW(h, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < n; i++) {
        LRESULT len = SendMessageW(h, CB_GETLBTEXTLEN, (WPARAM)i, 0);
        std::wstring w((size_t)len + 1, L'\0');
        SendMessageW(h, CB_GETLBTEXT, (WPARAM)i, (LPARAM)&w[0]);
        w.resize((size_t)len);
        out.push_back(narrow(w));
    }
    return out;
}

static std::vector<HWND> widgets_of(SimUI &ui, const std::string &name)
{
    for (size_t i = 0; i < ui.opt_widgets.size(); i++)
        if (ui.opt_widgets[i].first == name)
            return ui.opt_widgets[i].second;
    return std::vector<HWND>();
}

/* The background a text box is painted with: what its parent answers to
 * the WM_CTLCOLOR* message the control sends while drawing itself. */
static COLORREF box_background(HWND box)
{
    HDC dc = GetDC(box);
    UINT msg = IsWindowEnabled(box) ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC;
    SetBkColor(dc, RGB(1, 2, 3));
    SendMessageW(GetParent(box), msg, (WPARAM)dc, (LPARAM)box);
    COLORREF c = GetBkColor(dc);
    ReleaseDC(box, dc);
    return c;
}

static void drop_emu(SimUI &ui)
{
    if (ui.emu && ui.emu_owned)
        delete ui.emu;
    ui.emu = 0;
    ui.emu_owned = false;
}


/* the profile chooser's own spelling of a bundled image */
static std::string fw(const char *file) { return path_join(FIRMWARE_DIR, file); }

static int selftests_body()
{
    SimUI *uip = new SimUI("", "", true);
    SimUI &ui = *uip;
    ui.hooks.show_message = hook_message;

    std::string hint0 = NET_IDLE_HINT.substr(0, NET_IDLE_HINT.find('.'));
#define PANE() pane_text(ui.txt_net)
    printf("== network pane: before a run ==\n");
    CHECK(has(PANE(), hint0), "the pane says how to switch the bridge on");
    CHECK(window_text(ui.net_frame) == NET_TITLE, "and its heading carries no count yet");

    printf("== network pane: lines from the emulation thread ==\n");
    const char *lines[] = {
        "[WIFI] device acts as a station (it probes for networks): peer takes the access point role",
        "[WIFI] station 4c:a9:19:14:71:50 associated to 'HojnikB WiFi' (AID 1)",
        "[NET] DHCP: device 4c:a9:19:14:71:50 got 192.168.1.100 (gateway/DNS 192.168.1.1)",
        "[NET] TCP 192.168.1.100:40127 <-> 18.185.31.196:8886 sends: TLS ClientHello (no server name)",
        "[NET] Tuya LAN broadcast from 192.168.1.100:63035: {\"gwId\":\"bf0e\",\"productKey\":\"gqmm\"}",
        "[NET] MQTT -> device PUBLISH cmnd/obkSim/POWER = ON",
    };
    const int NL = 6;
    for (int i = 0; i < NL; i++)
        ui.net_sink(lines[i]); /* exactly what Emulator.net_log calls */
    CHECK(!has(PANE(), "HojnikB"), "a line is held until the UI drains it, not drawn from that thread");
    ui.poll();
    std::string shown = PANE();
    CHECK(!has(shown, hint0), "the 'nothing yet' note goes once there IS something, rather than sitting above it");
    bool all = true;
    for (int i = 0; i < NL; i++)
        all = all && has(shown, lines[i]);
    CHECK(all, "after one poll every line is in the pane, verbatim");
    CHECK(shown.find(lines[0]) < shown.find(lines[NL - 1]), "and in the order they happened");
    CHECK(window_text(ui.net_frame) == strf("%s - 6 lines", NET_TITLE),
          "the heading counts them, so a glance says whether anything was captured");

    printf("== network pane: what each line is coloured as ==\n");
    CHECK(pane_tags_of(ui.txt_net, "device acts as a station").count("wifi"), "Wi-Fi link lines get the link ink");
    CHECK(pane_tags_of(ui.txt_net, "DHCP: device").count("net"), "plain IP lines get the IP ink");
    CHECK(pane_tags_of(ui.txt_net, "TLS ClientHello").count("payload"), "a line carrying what was SENT is inked apart");
    CHECK(pane_tags_of(ui.txt_net, "Tuya LAN broadcast").count("payload"), "...as is a decoded announcement");
    CHECK(pane_tags_of(ui.txt_net, "MQTT -> device").count("payload"), "...and a message pushed to the device");

    printf("== network pane: the IP-only filter ==\n");
    ui.var_net_ip_only.setb(true);
    for (int i = 0; i < NL; i++)
        ui.net_sink(lines[i]);
    ui.poll();
    std::string tail = PANE().substr(shown.size());
    CHECK(!has(tail, "[WIFI]") && has(tail, "DHCP: device"),
          "with the filter on, link lines are hidden and IP lines still arrive");
    CHECK(window_text(ui.net_frame) == strf("%s - 12 lines", NET_TITLE),
          "the count still reflects everything captured, not only what is shown");
    ui.var_net_ip_only.setb(false);

    printf("== network pane: clearing and saving ==\n");
    ui.clear();
    CHECK(has(PANE(), hint0) && !has(PANE(), "HojnikB"), "Clear empties the pane and puts the hint back");
    CHECK(window_text(ui.net_frame) == NET_TITLE, "and resets the count");
    ui.net_sink("[NET] back again");
    ui.poll();
    CHECK(has(PANE(), "[NET] back again"), "the pane keeps working after a clear");
    {
        std::string path = temp_file(".log");
        g_save_path = path;
        ui.hooks.ask_save = hook_ask_save;
        ui.save();
        std::string body = read_text(path);
        ui.hooks.ask_save = 0;
        DeleteFileW(widen(path).c_str());
        CHECK(has(body, "=== network (virtual Wi-Fi and IP bridge) ===") && has(body, "[NET] back again"),
              "Save log writes the network pane out under its own heading");
    }

    printf("== captured traffic window: the frames themselves ==\n");
    Bytes dev = H("c8478c000000"), ours = H("020000000001");
    Bytes dev_ip = netstack::str2ip("192.168.1.100"), our_ip = netstack::str2ip("192.168.1.1");
    Bytes seg = tcp_segment(dev_ip, netstack::str2ip("10.200.0.2"), 40127, 1883, 1, 1, TCP_ACK | TCP_PSH, 8192,
                            Bytes("\x10\x14\x00\x04MQTT\x04\x02\x00\x3c\x00\x06obkSim", 22));
    Bytes f_in = eth_frame(ours, dev, ETH_IP, seg);
    Bytes f_out = eth_frame(dev, ours, ETH_IP, udp_packet(our_ip, dev_ip, 53, 5353, Bytes("\xbe\xef", 2) + Bytes(20, '\0')));
    ui.frame_sink("from device", f_in, 1500000);
    ui.frame_sink("to device", f_out, 1600000);
    ui.open_details();
    CHECK(ui.detail_exists(), "the details window opens from the pane");
    CHECK(ui.var_frame_privacy.get() == FRAME_PRIVACY_NOTE && window_text(ui.lbl_frame_privacy) == FRAME_PRIVACY_NOTE,
          "the raw-frame viewer warns that exact captures can contain clear credentials");
    CHECK(ui.frame_tree.size() == 2, "both captured frames are listed");
    StrList vals = ui.frame_tree.item(ui.frame_tree.iids[0]);
    CHECK(vals[0] == "1" && vals[2] == "from device" && vals[3] == strf("%d", (int)f_in.size()),
          "a row gives the frame's number, direction and exact length");
    CHECK(has(vals[4], "TCP 192.168.1.100:40127 to 10.200.0.2:1883"), "and says what the frame is");
    CHECK(has(ui.frame_tree.item(ui.frame_tree.iids[1])[4], "(DNS)"), "the other direction is listed too");

    ui.frame_tree.selection_set(ui.frame_tree.iids[0]);
    ui.show_frame_bytes();
    std::string dump = pane_text(ui.txt_bytes);
    CHECK(has(dump, "obkSim") && has(dump, "MQTT"), "selecting a frame shows its bytes, the payload readable in the dump");
    std::string first = strf("%08x  ", 0) + hexstr_sep(f_in.substr(0, 16), " ");
    CHECK(has(dump, first), "the hex is the frame itself, from its first byte");
    CHECK(has(dump, strf("%d bytes", (int)f_in.size())) && has(dump, "from device"),
          "with a heading saying which frame this is");

    ui.var_frame_filter.set("DNS");
    ui.refresh_details();
    CHECK(ui.frame_tree.size() == 1, "the filter narrows the list");
    CHECK(has(ui.var_frame_count.get(), "1 match") || has(ui.var_frame_count.get(), "matching"),
          "and the count line says how many matched");
    ui.var_frame_filter.set("");
    ui.refresh_details();
    CHECK(ui.frame_tree.size() == 2, "clearing the filter brings them back");

    {
        std::string pcap_path = temp_file(".pcap");
        g_save_path = pcap_path;
        ui.hooks.ask_save = hook_ask_save;
        ui.save_pcap();
        ui.hooks.ask_save = 0;
        Bytes blob = read_text(pcap_path);
        DeleteFileW(widen(pcap_path).c_str());
        CHECK(blob.size() >= 24 && g32le(blob, 0) == 0xA1B2C3D4 && g16le(blob, 4) == 2 && g16le(blob, 6) == 4
                  && g32le(blob, 20) == 1,
              "Save .pcap writes an Ethernet capture Wireshark can open");
        size_t off = 24;
        std::vector<std::pair<u64, Bytes> > saved;
        while (off + 16 <= blob.size()) {
            u32 sec = g32le(blob, off), usec = g32le(blob, off + 4), incl = g32le(blob, off + 8);
            saved.push_back(std::make_pair((u64)sec * 1000000 + usec, blob.substr(off + 16, incl)));
            off += 16 + incl;
        }
        CHECK(off == blob.size() && saved.size() == 2 && saved[0].second == f_in && saved[1].second == f_out,
              "...holding the captured frames byte for byte, in order");
        CHECK(saved[0].first == 1500000 && saved[1].first == 1600000, "each stamped with the device time it crossed");
    }

    printf("== double-clicking a line opens the bytes behind THAT line ==\n");
    CHECK(filter_from_line("[NET] TCP 192.168.1.100:40127 <-> 10.200.0.2:1883 sends: MQTT CONNECT") == ":40127",
          "a connection line narrows to its own port, which is unique to it");
    CHECK(filter_from_line("[NET] DHCP: device got 192.168.1.100") == "192.168.1.100",
          "a line with no port falls back to the address it names");
    CHECK(filter_from_line("[WIFI] device acts as a station") == "",
          "and a line naming neither shows everything rather than hiding frames");
    ui.var_frame_filter.set("");
    ui.refresh_details();
    ui.details_for_click(false, "");
    CHECK(ui.frame_tree.size() == 2, "opening the window without a specific line still shows every frame");

    printf("== the packet list refreshing during a run ==\n");
    ui.frame_tree.selection_set("1");
    ui.show_frame_bytes();
    std::string before = pane_text(ui.txt_bytes);
    ui.frame_sink("from device", f_in, 1700000); /* more traffic arrives */
    ui.refresh_details();
    CHECK(ui.frame_tree.selection() == "1", "a refresh keeps the frame the reader had selected");
    CHECK(pane_text(ui.txt_bytes) == before, "...and leaves its bytes on screen rather than jumping to the newest");
    CHECK(ui.frame_tree.size() == 3, "while still listing what has since arrived");

    printf("== captured traffic target-host selector ==\n");
    Bytes remote_ip = netstack::str2ip("10.200.0.2");
    Bytes f_reply = eth_frame(dev, ours, ETH_IP, tcp_segment(remote_ip, dev_ip, 1883, 40127, 1, 2, TCP_ACK, 8192));
    Bytes near_ip = netstack::str2ip("10.200.0.20");
    Bytes f_near = eth_frame(ours, dev, ETH_IP, tcp_segment(dev_ip, near_ip, 40128, 1883, 1, 0, TCP_SYN, 8192));
    ui.frame_sink("to device", f_reply, 1800000);
    ui.frame_sink("from device", f_near, 1900000);
    ui.refresh_details();
    {
        StrList choices = combo_values(ui.frame_target);
        StrList want;
        want.push_back(ALL_FRAME_TARGETS);
        want.push_back("10.200.0.2");
        want.push_back("10.200.0.20");
        want.push_back("192.168.1.1");
        CHECK(choices == want, "the target list contains each exact remote IPv4 host once, in numeric order");
    }
    ui.var_frame_target.set("10.200.0.2");
    ui.var_frame_filter.set("");
    ui.refresh_details();
    CHECK(ui.frame_tree.size() == 3, "selecting a target keeps both traffic directions and repeated frames");
    ui.var_frame_filter.set("PSH");
    ui.refresh_details();
    CHECK(ui.frame_tree.size() == 2, "the exact target selection combines with the existing text filter");
    {
        std::vector<Frame> special;
        Frame f;
        f.dir = "from device";
        f.us = 0;
        f.data = eth_frame(ours, dev, ETH_IP, udp_packet(dev_ip, Bytes(4, '\0'), 1, 1, Bytes()));
        special.push_back(f);
        f.data = eth_frame(ours, dev, ETH_IP, udp_packet(dev_ip, Bytes(4, '\xff'), 1, 1, Bytes()));
        special.push_back(f);
        f.data = eth_frame(ours, dev, ETH_IP, udp_packet(dev_ip, netstack::str2ip("239.1.2.3"), 1, 1, Bytes()));
        special.push_back(f);
        CHECK(SimUI::target_hosts(special).empty(),
              "unspecified, broadcast and multicast endpoints are not offered as target hosts");
    }
    ui.var_frame_target.set(ALL_FRAME_TARGETS);
    ui.var_frame_filter.set("");
    ui.refresh_details();

    ui.clear();
    CHECK(ui.frame_tree.size() == 0 && has(ui.var_frame_count.get(), "Nothing captured yet")
              && combo_values(ui.frame_target) == StrList(1, ALL_FRAME_TARGETS),
          "Clear empties the captured frames and target hosts as well as the pane");
    DestroyWindow(ui.detail_win);

    /* --- Config tab --- */
    ui.reset_options();
    RunCfg cfg;
    std::string err;
    bool ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.wifi && !cfg.tuyamcu && !cfg.boot && !cfg.xvr_selfclear && !cfg.blecore && cfg.tuyamcu_pid.empty()
              && !cfg.tuyamcu_raw && cfg.net_out && cfg.forwards.size() == 1 && cfg.forwards[0] == std::make_pair(8080, 80),
          "the Config defaults attach Wi-Fi with real outbound access");
    CHECK(guess_chip("OpenBK7231U_QIO.bin") == "BK7231U" && guess_chip("OpenBK7231T_QIO.bin") == "BK7231",
          "chip guessing distinguishes U's extended GPIO bank from generic BK7231");
    CHECK(enabled(ui.opt_text["http_gets"]) && !enabled(widgets_of(ui, "pcap")[0])
              && !enabled(widgets_of(ui, "tuyamcu")[0]),
          "default Wi-Fi fields are active while capture and TuyaMCU fields stay gated");
    CHECK(has(window_text(ui.lbl_radio_capture_note), FRAME_PRIVACY_NOTE),
          "the radio-PCAP option carries the same clear-credential warning");
    CHECK(!enabled(ui.btn_open_forward), "Open in browser waits until a configured forward is actually running");
    CHECK(ui.var_summary.get() == "options: Wi-Fi peer (auto, real internet, open, :8080 to :80)"
              && window_text(ui.lbl_summary) == ui.var_summary.get(),
          "the summary line shows the default Wi-Fi peer");

    printf("== interesting-dump quick runs ==\n");
    {
        StrList labels, want;
        for (size_t i = 0; i < ui.interesting_buttons.size(); i++)
            labels.push_back(window_text(ui.interesting_buttons[i]));
        for (int i = 0; i < N_INTERESTING; i++)
            want.push_back(INTERESTING_DUMPS[i].label);
        const char *need[] = {"OBK T", "OBK N", "OBK 7238", "MOES relay", "OBK sleep", "PIR sleep", "Rain sleep",
                              "TH01 sleep", "HA15 cam", "Pinmei cam", "GBDQA breaker", "IR blaster", "LSC bulb", 0};
        bool sub = true;
        for (int i = 0; need[i]; i++)
            sub = sub && std::find(labels.begin(), labels.end(), need[i]) != labels.end();
        CHECK(labels == want && sub,
              "the launcher exposes the requested OpenBeken, relay, sleep, camera and paired-cloud profiles");
        StrList gl;
        for (size_t i = 0; i < ui.interesting_group_labels.size(); i++)
            gl.push_back(window_text(ui.interesting_group_labels[i]));
        StrList gw;
        gw.push_back("Quick run:");
        gw.push_back("Real Tuya reconnect (owned IDs only; sends to cloud):");
        CHECK(gl == gw, "proven paired callers have their own clearly labelled launcher row");
        StrList chips = chip_family_names();
        bool good = true;
        for (int i = 0; i < N_INTERESTING; i++) {
            const Profile &p = INTERESTING_DUMPS[i];
            bool key_ok = std::string(p.key) == "(none)";
            for (int k = 0; KNOWN_KEY_NAMES[k]; k++)
                key_ok = key_ok || std::string(p.key) == KNOWN_KEY_NAMES[k];
            good = good && path_isfile(fw(p.file)) && std::find(chips.begin(), chips.end(), p.chip) != chips.end()
                   && key_ok;
        }
        CHECK(good, "every quick-run profile names a bundled image and a valid chip/key combination");
        std::string manifest = path_join(path_join(ROOT_DIR, "tools"), "release_firmwares.txt");
        if (path_isfile(manifest)) {
            std::set<std::string> files, expected;
            StrList ml = splitlines(read_text(manifest));
            for (size_t i = 0; i < ml.size(); i++)
                if (!strip(ml[i]).empty() && !startswith(lstrip(ml[i]), "#"))
                    files.insert(strip(ml[i]));
            expected.insert(path_basename(DEFAULT_IMAGE));
            for (int i = 0; i < N_INTERESTING; i++)
                expected.insert(INTERESTING_DUMPS[i].file);
            CHECK(files == expected, "the Windows release firmware manifest exactly matches the launcher");
        }
    }
    {
        std::string old_image = ui.var_image.get(), old_chip = ui.var_chip.get(), old_key = ui.var_key.get();
        bool o_wifi = ui.optb("wifi"), o_net = ui.optb("net_out");
        std::string o_role = ui.opts("wifi_role"), o_ssid = ui.opts("wifi_ssid"), o_pass = ui.opts("wifi_pass");
        Started started;
        ui.hooks.on_run = hook_record_run;
        ui.hooks.on_run_ctx = &started;
        ui.hooks.refresh_tuya_disabled = true;
        std::vector<int> paired;
        for (int i = 0; i < N_INTERESTING; i++)
            if (INTERESTING_DUMPS[i].paired)
                paired.push_back(i);
        for (size_t k = 0; k < paired.size(); k++) {
            ui.set_option_bool("wifi", false);
            ui.set_option_bool("net_out", false);
            ui.optv("wifi_role")->set("sta");
            ui.optv("wifi_ssid")->set("wrong-network");
            ui.optv("wifi_pass")->set("wrongpass");
            ui.run_interesting(paired[k]);
        }
        bool good = paired.size() >= 3 && started.runs.size() >= paired.size();
        for (size_t k = 0; good && k < paired.size(); k++) {
            const Profile &p = INTERESTING_DUMPS[paired[k]];
            const StrList &r = started.runs[started.runs.size() - paired.size() + k];
            good = r[0] == fw(p.file) && r[1] == p.chip && r[2] == p.key && r[3] == "True" && r[4] == "True"
                   && r[5] == "auto" && r[6] == "" && r[7] == "";
        }
        CHECK(good, "every paired-cloud shortcut selects its exact profile and a known-good real-cloud relay, then runs");
        int moes = -1;
        for (int i = 0; i < N_INTERESTING; i++)
            if (std::string(INTERESTING_DUMPS[i].label) == "MOES relay")
                moes = i;
        ui.run_interesting(moes);
        const StrList &last = started.runs.back();
        CHECK(last[0] == fw(INTERESTING_DUMPS[moes].file) && last[1] == "BK7231" && last[2] == "(none)" && last[4] == "True",
              "ordinary shortcuts inherit the real-internet GUI default too");
        ui.hooks.on_run = 0;
        ui.hooks.on_run_ctx = 0;
        ui.hooks.refresh_tuya_disabled = false;
        ui.var_image.set(old_image);
        ui.var_chip.set(old_chip);
        ui.var_key.set(old_key);
        ui.set_option_bool("wifi", o_wifi);
        ui.set_option_bool("net_out", o_net);
        ui.optv("wifi_role")->set(o_role);
        ui.optv("wifi_ssid")->set(o_ssid);
        ui.optv("wifi_pass")->set(o_pass);
    }

    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.wifi_role == "auto" && cfg.forwards.size() == 1 && cfg.forwards[0] == std::make_pair(8080, 80)
              && cfg.net_out && !cfg.has_ssid && !cfg.has_pass && !cfg.has_pcap && cfg.http_gets.empty()
              && cfg.mqtt_pubs.empty(),
          "the default Wi-Fi peer uses auto role and forwards 8080 to 80");
    CHECK(enabled(ui.opt_text["http_gets"]) && !enabled(widgets_of(ui, "pcap")[0]),
          "default Wi-Fi opens its fields, but the capture file waits for its own tick");

    ui.set_text_option("mqtt_pubs", "cmnd/obk/POWER=ON\n\n  stat/obk/x=a=b  \n");
    ui.set_text_option("http_gets", "/cm?cmnd=POWER%20ON\n \n/index\n");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.mqtt_pubs.size() == 2 && cfg.mqtt_pubs[0] == std::make_pair(std::string("cmnd/obk/POWER"), Bytes("ON"))
              && cfg.mqtt_pubs[1] == std::make_pair(std::string("stat/obk/x"), Bytes("a=b")),
          "MQTT lines parse as TOPIC=PAYLOAD; blank lines are skipped, '=' stays in a payload");
    CHECK(cfg.http_gets.size() == 2 && cfg.http_gets[0] == "/cm?cmnd=POWER%20ON" && cfg.http_gets[1] == "/index",
          "HTTP GET paths are one per line, blank lines skipped");

#define REJECTS(fragment, label)                                     \
    do {                                                             \
        RunCfg c_;                                                   \
        std::string e_;                                              \
        bool ok_ = ui.collect_cfg(c_, e_);                           \
        CHECK(!ok_ && has(e_, fragment), label);                     \
    } while (0)

    ui.set_text_option("mqtt_pubs", "no-equals-sign");
    REJECTS("TOPIC=PAYLOAD", "an MQTT line without '=' is refused before the run starts");
    ui.set_text_option("mqtt_pubs", "");
    ui.set_text_option("http_gets", "/caf\xc3\xa9\n/\xc5\xbc\xc3\xb3\xc5\x82w");
    REJECTS("percent-encode", "an HTTP path a request line cannot carry is refused, not left to end the run");
    ui.set_text_option("http_gets", "/cm?cmnd=POWER%20ON\n \n/index\n");
    ui.optv("fwd_local")->set("80x");
    REJECTS("port", "a port forward that is not a number is refused");
    {
        const char *pairs[3][2] = {{"0", "80"}, {"70000", "80"}, {"8080", ""}};
        for (int i = 0; i < 3; i++) {
            ui.optv("fwd_local")->set(pairs[i][0]);
            ui.optv("fwd_device")->set(pairs[i][1]);
            REJECTS("ports 1-65535", "a port forward of " + py_repr(pairs[i][0]) + " to " + py_repr(pairs[i][1])
                                         + " is refused");
        }
    }
    ui.optv("fwd_local")->set("");
    ui.optv("fwd_device")->set("");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.forwards.empty(), "clearing both forward ports turns the forward off");
    ui.optv("wifi_pass")->set("short");
    REJECTS("8 to 63", "a WPA2 passphrase under 8 characters is refused");
    ui.optv("wifi_pass")->set(std::string(64, 'x'));
    REJECTS("8 to 63", "a WPA2 passphrase over 63 characters is refused");
    ui.optv("wifi_pass")->set(" pass word ");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.has_pass && cfg.wifi_pass == " pass word ", "a passphrase keeps its spaces: they are part of the key");
    ui.optv("wifi_ssid")->set(" Home Net ");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.has_ssid && cfg.wifi_ssid == " Home Net ",
          "a network name keeps its spaces: it is matched exactly and salts the WPA2 key");
    ui.optv("wifi_ssid")->set("   ");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && !cfg.has_ssid, "a network name of only spaces means 'adopt the one the device asks for'");
    ui.set_option_bool("pcap", true);
    REJECTS("no file", "radio capture without a file is refused");
    CHECK(enabled(widgets_of(ui, "pcap")[0]), "ticking radio capture opens its file field");
    ui.optv("pcap_path")->set(path_join(path_join(temp_dir(), "no_such_dir_bks"), "x.pcap"));
    REJECTS("does not exist", "a capture file in a folder that does not exist is refused");
    ui.optv("pcap_path")->set("");
    ui.set_option_bool("wifi", false);
    CHECK(!enabled(widgets_of(ui, "pcap")[0]), "the capture file field closes again with the Wi-Fi peer");
    HWND box = ui.opt_text["http_gets"];
    CHECK(box_background(box) != GetSysColor(COLOR_WINDOW), "a disabled text box is drawn greyed, not as an editable white box");
    ui.set_option_bool("wifi", true);
    CHECK(box_background(box) == GetSysColor(COLOR_WINDOW), "...and drawn normally again once enabled");
    ui.set_option_bool("tuyamcu", true);
    ui.optv("tuyamcu_pid")->set("\xc5\xbc\xc3\xb3\xc5\x82w");
    REJECTS("ASCII", "a product ID that is not ASCII is refused, not left to end the run");
    ui.optv("tuyamcu_pid")->set("");
    ui.set_option_bool("tuyamcu", false);

    ui.set_option_bool("tuyamcu", true);
    ui.optv("tuyamcu_pid")->set("  abcdefghijklmnop ");
    ui.set_option_bool("tuyamcu_raw", true);
    ui.set_option_bool("pcap", false);
    ui.optv("wifi_ssid")->set("HomeNet");
    std::string summary = ui.var_summary.get();
    CHECK(has(summary, "TuyaMCU (abcdefghijklmnop, raw)") && has(summary, "'HomeNet'") && has(summary, "Wi-Fi peer (auto"),
          "the summary line follows the choices");
    CHECK(has(summary, "WPA2") && has(summary, "2 HTTP GET"),
          "it names WPA2 when a passphrase is set, and counts the HTTP requests");
    ui.set_option_bool("net_out", true);
    CHECK(has(ui.var_summary.get(), "real internet"), "the summary makes an externally connected run unmistakable");
    ui.set_option_bool("net_out", false);
    ui.optv("wifi_pass")->set("");
    CHECK(has(ui.var_summary.get(), "open") && !has(ui.var_summary.get(), "WPA2"),
          "...and says 'open' without one, which is what a relaunch leaves");
    {
        /* typed, as a person would: the edit control's own EN_CHANGE */
        ui.set_text_option("mqtt_pubs", "");
        HWND mb = ui.opt_text["mqtt_pubs"];
        SendMessageW(mb, EM_SETSEL, 0, -1);
        SendMessageW(mb, EM_REPLACESEL, TRUE, (LPARAM)L"a/b=1");
    }
    CHECK(has(ui.var_summary.get(), "1 MQTT"), "typing in a text box updates the summary too, though it has no variable");
    ui.set_text_option("mqtt_pubs", "");
    ui.optv("wifi_pass")->set(" pass word ");
    ok = ui.collect_cfg(cfg, err);
    CHECK(ok && cfg.tuyamcu_pid == "abcdefghijklmnop" && cfg.tuyamcu_raw,
          "the TuyaMCU product ID is trimmed and passed on with the raw flag");

    /* The worker hands every option to the same calls the command line makes. */
    {
        Calls calls;
        std::string img = temp_file(".bin");
        ui.hooks.build = fake_build;
        ui.hooks.build_ctx = &calls;
        ui.set_option_bool("xvr_selfclear", true);
        ui.optv("wifi_role")->set("ap");
        ui.optv("fwd_local")->set("9090");
        ui.optv("fwd_device")->set("80");
        ui.optv("wifi_pass")->set("password1");
        ui.set_option_bool("pcap", true);
        ui.optv("pcap_path")->set("cap.pcap");
        ui.set_option_bool("net_out", false);
        ui.set_text_option("mqtt_pubs", "cmnd/obk/POWER=OFF");
        ok = ui.collect_cfg(cfg, err);
        if (!ok)
            printf("  collect_cfg: %s\n", err.c_str());
        cfg.tls_psks = std::vector<Bytes>(1, Bytes("synthetic-gui-cloud-key"));
        cfg.tuya_legacy_identity_psk = true;
        ui.stop_requested = false;
        ui.worker("image.bin", cfg);
        const BuildOptions &b = calls.build;
        CHECK(calls.has_build && b.xvr_selfclear && b.tuyamcu_pid == "abcdefghijklmnop" && b.tuyamcu_raw && b.wifi
                  && b.only_uart && b.ble_core == -1,
              "the worker passes device options and leaves BLE hardware selection automatic");
        const PeerArgs &p = calls.peer;
        StrList gets;
        gets.push_back("/cm?cmnd=POWER%20ON");
        gets.push_back("/index");
        CHECK(calls.has_peer && p.role == "ap" && p.has_ssid && p.ssid == "HomeNet" && p.has_pass
                  && p.passphrase == "password1" && p.forwards.size() == 1 && p.forwards[0] == std::make_pair(9090, 80)
                  && !p.net_out && p.has_pcap && p.pcap == "cap.pcap" && p.http_gets == gets && p.mqtt_pubs.size() == 1
                  && p.mqtt_pubs[0] == std::make_pair(std::string("cmnd/obk/POWER"), Bytes("OFF"))
                  && p.tls_psks == std::vector<Bytes>(1, Bytes("synthetic-gui-cloud-key")) && p.tuya_legacy_identity_psk,
              "the worker passes offline network options and private TLS mode");
        StrList setup_run;
        setup_run.push_back("setup");
        setup_run.push_back("run");
        bool fin;
        {
            ScopedLock lk(ui.result_lock);
            fin = ui.has_finished && ui.finished == "emulation finished";
        }
        CHECK(calls.order == setup_run && fin, "a run nobody stopped goes straight from setup to run");
        CHECK(calls.closed == 1, "the run's network is released when it ends, freeing the port");

        drop_emu(ui);
        ui.on_stop();
        CHECK(ui.stop_requested, "Stop with nothing loaded yet is remembered");
        ui.worker("image.bin", cfg);
        StrList setup_stop_run;
        setup_stop_run.push_back("setup");
        setup_stop_run.push_back("stop");
        setup_stop_run.push_back("run");
        CHECK(calls.order == setup_stop_run, "a Stop pressed while the image was loading reaches the emulator before it runs");
        drop_emu(ui);

        ui.var_image.set(img);
        ui.pending_run = true;
        g_private = TuyaCfg();
        g_private.present = true;
        g_private.ident = new_tuya_ident();
        g_private.dec.valid = true;
        g_private.dec.has_tls_mode = true;
        g_private.dec.tls_mode = 2;
        g_private.dec.ids.set("psk_key", std::string(37, 'A'));
        ui.hooks.tuya_extract = private_extract;
        ui.tuya_key_valid = false;
        ui.on_run();
        ui.hooks.tuya_extract = 0;
        if (ui.thread)
            ui.thread->join(5000);
        CHECK(calls.order == setup_run && !ui.stop_requested, "Run clears a Stop left over from the previous run");
        CHECK(!ui.pending_run, "...and uses up a dropped image that was waiting");
        CHECK(calls.peer.tls_psks == std::vector<Bytes>(1, Bytes(37, 'A')) && calls.peer.tuya_legacy_identity_psk
                  && ui.run_tuya_config.present && ui.run_tuya_config.ident == g_private.ident,
              "Run snapshots the selected image's private PSK mode for its offline TLS peer");
        CHECK(ui.active_forward_url == "http://127.0.0.1:9090/",
              "Run binds Open in browser to that run's validated local port");

        ui.set_option_bool("net_out", true);
        ui.on_run();
        if (ui.thread)
            ui.thread->join(5000);
        CHECK(calls.peer.net_out && calls.peer.tls_psks.empty() && !calls.peer.tuya_legacy_identity_psk,
              "a real-cloud Run passes no PSK copy to its transparent network relay");
        ui.set_option_bool("net_out", false);

        {
            ScopedLock lk(ui.result_lock);
            ui.has_finished = ui.has_error = false;
            ui.finished.clear();
            ui.error.clear();
        }
        calls.has_run_error = true;
        calls.run_error = "UC_ERR_FETCH_UNMAPPED at pc 0x00000000";
        drop_emu(ui);
        ui.worker("image.bin", cfg);
        CHECK(ui.has_error && ui.error == calls.run_error && !ui.has_finished,
              "a run that ended on an error is reported as one, not as a normal finish");
        calls.has_run_error = false;
        calls.run_error.clear();
        ui.has_error = false;
        ui.error.clear();
        calls.attach_fails = true;
        int closed = calls.closed;
        drop_emu(ui);
        ui.worker("image.bin", cfg);
        CHECK(ui.has_error && has(ui.error, "OSError") && calls.closed == closed + 1,
              "a peer that fails to start still has its network released");
        calls.attach_fails = false;

        ui.hooks.build = 0;
        ui.hooks.build_ctx = 0;
        drop_emu(ui);
        if (ui.thread) {
            ui.thread->join(5000);
            delete ui.thread;
            ui.thread = 0;
        }
        ui.thread_mode = 0;
        ui.has_finished = ui.has_error = false;
        ui.finished.clear();
        ui.error.clear();
        ui.stop_requested = ui.pending_run = false;
        ui.active_forward_url.clear();
        set_state(ui.btn_open_forward, false);
        DeleteFileW(widen(img).c_str());
    }

    /* Enter in the send box runs on_send whatever the button says. */
    {
        FeedEmu feed;
        ui.emu = &feed;
        ui.emu_owned = false;
        size_t hist = ui.history.size();
        ui.var_input.set("help");
        ui.on_send();
        CHECK(feed.fed.empty() && ui.history.size() == hist && ui.var_input.get() == "help",
              "after a run has ended, Enter sends nothing and echoes nothing");
        ui.emu = 0;
        ui.var_input.set("");
    }

    printf("== GPIO inputs: pressing a real emulated pad from the table ==\n");
    {
        ViewEmu g;
        g.v.gpio_cfg[6] = 0x3C;  /* pull-up input */
        g.v.gpio_cfg[7] = 0x2C;  /* pull-down input */
        g.v.gpio_cfg[18] = 0x02; /* output */
        g.v.gpio_cfg[24] = 0x78; /* second function */
        ui.emu = &g;
        ui.emu_owned = false;
        ui.thread_mode = 2;
        ui.stop_requested = false;
        ui.tuya_config = TuyaCfg();
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(ui.tree.exists("gpio:6") && ui.tree.exists("gpio:7") && ui.tree.exists("gpio:18") && ui.tree.exists("gpio:24"),
              "configured GPIO rows have stable pin identities");

        ui.tree.selection_set("gpio:6");
        ui.gpio_selection_changed();
        CHECK(enabled(ui.btn_gpio_press) && window_text(ui.btn_gpio_press) == "Press P6"
                  && has(ui.var_gpio_action.get(), "drives LOW"),
              "a pull-up input offers a LOW press");
        ui.toggle_gpio_input();
        CHECK(g.inputs.back() == std::make_pair(6, 0) && window_text(ui.btn_gpio_press) == "Release P6"
                  && has(ui.tree.item("gpio:6")[2], "PRESSED LOW"),
              "Press drives the external pad low and marks it as held");

        g.v.gpio_cfg[18] = 0x00;
        ui.refresh_periph();
        CHECK(ui.tree.selection() == "gpio:6" && window_text(ui.btn_gpio_press) == "Release P6",
              "a relay change preserves the held input and its selection");
        ui.toggle_gpio_input();
        CHECK(g.inputs.back() == std::make_pair(6, -1) && !ui.gpio_pressed.count(6),
              "Release disconnects the external drive so the hardware pull-up wins again");

        ui.tree.selection_set("gpio:7");
        ui.gpio_selection_changed();
        ui.toggle_gpio_input();
        CHECK(g.inputs.back() == std::make_pair(7, 1), "a pull-down input gets a HIGH press");
        ui.toggle_gpio_input();

        std::vector<std::pair<int, int> > before_inputs = g.inputs;
        ui.tree.selection_set("gpio:18");
        ui.gpio_selection_changed();
        ui.toggle_gpio_input();
        CHECK(!enabled(ui.btn_gpio_press) && g.inputs == before_inputs,
              "an output row cannot be driven through the input control");
        ui.tree.selection_set("gpio:24");
        ui.gpio_selection_changed();
        CHECK(!enabled(ui.btn_gpio_press), "a peripheral-owned pad is not mislabelled as a push button");

        std::vector<std::pair<std::string, std::string> > lp;
        lp.push_back(std::make_pair(std::string("bt1_pin"), std::string("6")));
        lp.push_back(std::make_pair(std::string("bt1_lv"), std::string("1")));
        lp.push_back(std::make_pair(std::string("reset_pin"), std::string("9")));
        lp.push_back(std::make_pair(std::string("reset_lv"), std::string("0")));
        lp.push_back(std::make_pair(std::string("rl1_pin"), std::string("18")));
        lp.push_back(std::make_pair(std::string("rl1_lv"), std::string("1")));
        std::map<int, int> levels = tuya_button_levels(&lp), want;
        want[6] = 1;
        want[9] = 0;
        CHECK(levels == want, "Tuya button active levels are used without treating relay metadata as a button");

        ui.run_tuya_config = TuyaCfg();
        ui.run_tuya_config.present = true;
        ui.run_tuya_config.pairs.push_back(std::make_pair(std::string("bt1_pin"), std::string("6")));
        ui.run_tuya_config.pairs.push_back(std::make_pair(std::string("bt1_lv"), std::string("0")));
        ui.tuya_config = TuyaCfg();
        ui.tuya_config.present = true;
        ui.tuya_config.pairs.push_back(std::make_pair(std::string("bt1_pin"), std::string("6")));
        ui.tuya_config.pairs.push_back(std::make_pair(std::string("bt1_lv"), std::string("1")));
        int level;
        std::string why;
        ui.gpio_press_level(6, 0x3C, level, why);
        CHECK(level == 0, "a newly browsed image cannot change the running image's button polarity");
        ui.run_tuya_config = TuyaCfg();
        ui.tuya_config = TuyaCfg();

        g.bk7252 = true;
        g.ext = true;
        g.variant = "BK7252";
        g.v.gpio_cfg[32] = 0xA5A5;
        g.v.gpio_cfg[48] = 0x3C;
        ui.run_image = "BK7252_upper_bank.bin";
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(ui.tree.exists("gpio:32") && ui.tree.item("gpio:32")[3] == "0x3C",
              "BK7252 slot 48 is shown as logical input P32, not FUNC_CFG");
        ui.tree.selection_set("gpio:32");
        ui.gpio_selection_changed();
        ui.toggle_gpio_input();
        CHECK(g.inputs.back() == std::make_pair(32, 0), "the BK7252 upper-bank Press action drives logical P32");
        ui.toggle_gpio_input();
        g.bk7252 = false;
        g.ext = false;
        g.variant = "BK7231";
        g.v.gpio_cfg.erase(48);
        ui.run_image.clear();
        ui.periph_key_valid = false;
        ui.refresh_periph();

        ui.tree.selection_set("gpio:6");
        ui.gpio_selection_changed();
        ui.toggle_gpio_input();
        ui.on_stop();
        CHECK(g.inputs.back() == std::make_pair(6, -1) && ui.gpio_pressed.empty() && g.stopped,
              "Stop releases every held GPIO before stopping the emulator");
        ui.emu = 0;
        ui.thread_mode = 0;
        ui.stop_requested = false;
        ui.gpio_selected = -1;
        ui.run_tuya_config = TuyaCfg();
        ui.run_image.clear();
        ui.tree.selection_remove();
        ui.update_gpio_control();
    }

    {
        ViewEmu c;
        c.v.insn_count = 10;
        c.rx_delay = 0;
        c.listening = true;
        ui.thread_mode = 2;
        ui.emu = &c;
        CHECK(ui.console_ready(), "with no RX hold-off (TuyaMCU) the console is ready at once");
        c.rx_delay = 2000000;
        CHECK(!ui.console_ready(), "...and with OpenBeken's hold-off it waits for it");

        g_opened.clear();
        ui.hooks.browser_open = hook_browser;
        ui.active_forward_url = "http://127.0.0.1:9090/";
        ui.stop_requested = false;
        c.listening = false;
        ui.update_open_forward();
        ui.open_forward();
        CHECK(!enabled(ui.btn_open_forward) && g_opened.empty(),
              "Open in browser stays disabled when the requested port did not bind");
        c.listening = true;
        ui.update_open_forward();
        CHECK(enabled(ui.btn_open_forward), "Open in browser becomes available once the forward is running");
        ui.open_forward();
        CHECK(g_opened.size() == 1 && g_opened[0] == std::make_pair(std::string("http://127.0.0.1:9090/"), 2),
              "Open in browser sends the active loopback URL to the default browser");
        ui.stop_requested = true;
        ui.update_open_forward();
        ui.open_forward();
        CHECK(!enabled(ui.btn_open_forward) && g_opened.size() == 1,
              "Stop disables the browser action and a stale click opens nothing");
        ui.hooks.browser_open = 0;
        ui.active_forward_url.clear();
        ui.stop_requested = false;
        ui.emu = 0;
        ui.thread_mode = 0;
    }

    /* Saved settings round-trip through a file, minus the passphrase. */
    {
        std::string tmpdir = make_temp_dir();
        std::string spath = path_join(tmpdir, "ui.json");
        std::string dump = path_join(tmpdir, "dump.bin"), other = path_join(tmpdir, "other.bin");
        write_file(dump, Bytes(16, '\0'));
        write_file(other, Bytes(16, '\0'));

        ExtractCalls xc;
        xc.dump = dump;
        ui.hooks.tuya_extract = fake_extract;
        ui.hooks.tuya_ctx = &xc;
        ui.tuya_key_valid = false;
        ui.var_image.set(dump);
        ui.refresh_tuya_config(true);
        std::string rows_s;
        bool row_ok = false;
        for (size_t i = 0; i < ui.tuya_tree.size(); i++) {
            StrList r = ui.tuya_tree.item(ui.tuya_tree.iids[i]);
            rows_s += "(" + r[0] + ", " + r[1] + ")";
            if (r[0] == "Paired WiFi SSID" && r[1] == "Ho****et")
                row_ok = true;
        }
        std::string cfg_json = pane_text(ui.txt_tuya_json);
        CHECK(row_ok && has(rows_s, "12****************************34"), "the Tuya tab shows the decoder's masked summary");
        CHECK(has(cfg_json, "\"rl1_pin\": 6") && has(cfg_json, "\"module\": \"CB2S\""),
              "the Tuya tab formats user_param_key as decoded JSON");
        CHECK(!has(cfg_json + rows_s, "must-not-reach-the-widget") && !has(cfg_json + rows_s, "secret"),
              "raw network credentials never reach the Tuya tab");
        size_t before_calls = xc.calls.size();
        ui.refresh_tuya_config(false);
        CHECK(xc.calls.size() == before_calls, "the Tuya decoder is cached while the selected file is unchanged");
        ui.load_image(other, false);
        CHECK(!ui.tuya_config.present && ui.tuya_tree.size() == 0 && has(ui.var_tuya_status.get(), "No recoverable"),
              "loading another image clears stale Tuya config");
        ui.hooks.tuya_extract = 0;
        ui.hooks.tuya_ctx = 0;

        ui.settings_path = spath;
        ui.var_image.set(dump);
        ui.var_chip.set("BK7238");
        ui.var_key.set("(none)");
        ui.save_settings();
        JsonValue saved;
        bool parsed = json_parse(read_text(spath), saved);
        CHECK(parsed && !saved.has("wifi_pass") && !saved.has("net_out") && saved.get("wifi_ssid").as_str() == "HomeNet"
                  && saved.get("mqtt_pubs").as_str() == "cmnd/obk/POWER=OFF",
              "settings are saved without the Wi-Fi passphrase or external-access grant");
        std::string simg = saved.get("image").as_str();
        CHECK(!path_isfile(spath + ".tmp") && simg.size() > 2 && simg[1] == ':' && (simg[2] == '\\' || simg[2] == '/'),
              "the file is swapped in whole, and the image is saved as an absolute path");

        SimUI *ui2 = new SimUI("", spath, true);
        CHECK(ui2->optb("wifi") && ui2->opts("wifi_ssid") == "HomeNet" && ui2->optb("net_out")
                  && ui2->opts("fwd_local") == "9090" && ui2->opts("wifi_pass") == "" && ui2->opts("wifi_role") == "ap"
                  && ui2->opts("mqtt_pubs") == "cmnd/obk/POWER=OFF",
              "a new window restores the saved choices, role and text boxes included");
        CHECK(same_file(ui2->var_image.get(), dump) && ui2->var_chip.get() == "BK7238" && ui2->var_key.get() == "(none)",
              "...and reopens the saved image with its saved chip and key");
        SendMessageW(ui2->opt_text["mqtt_pubs"], EM_UNDO, 0, 0); /* Ctrl+Z */
        CHECK(ui2->opts("mqtt_pubs") == "cmnd/obk/POWER=OFF", "Ctrl+Z cannot take a restored text box back to empty");
        delete ui2;

        std::string same_dump = path_join(path_join(path_dirname(dump), "."), path_basename(dump));
        same_dump = upper(replace_all(same_dump, "\\", "/"));
        ui2 = new SimUI(same_dump, spath, true);
        CHECK(ui2->var_chip.get() == "BK7238" && ui2->var_key.get() == "(none)",
              "the same image spelled differently still gets its chip and key back");
        delete ui2;
        ui2 = new SimUI(other, spath, true);
        CHECK(ui2->var_image.get() == other && ui2->var_chip.get() == guess_chip(other) && ui2->var_key.get() == "TUYA",
              "the saved chip and key stay with the saved image, not one named at launch");
        delete ui2;

        write_file(spath, "{\"wifi\": \"yes\", \"wifi_role\": \"mesh\", \"fwd_local\": 8080, \"boot\": true");
        SimUI *ui3 = new SimUI("", spath, true);
        CHECK(ui3->optb("wifi") && !ui3->optb("boot"), "a corrupt settings file means defaults, not a crash");
        write_file(spath, "{\"wifi\": \"yes\", \"wifi_role\": \"mesh\", \"fwd_local\": 8080, \"boot\": true}");
        JsonValue loaded;
        ui3->load_settings(loaded);
        ui3->apply_settings(loaded, true);
        CHECK(ui3->optb("wifi") && ui3->opts("wifi_role") == "auto" && ui3->opts("fwd_local") == "8080" && ui3->optb("boot"),
              "saved values of the wrong type or out of range are ignored one by one");
        JsonValue sta;
        json_parse("{\"wifi_role\": \"sta\", \"wifi_pass\": \"password1\", \"net_out\": false}", sta);
        ui3->apply_settings(sta, true);
        CHECK(ui3->opts("wifi_role") == "sta" && ui3->opts("wifi_pass") == "" && ui3->optb("net_out"),
              "a valid saved role is restored; secrets and stale network policy never are");
        write_file(spath, "[]");
        JsonValue arr;
        ui3->load_settings(arr);
        CHECK(arr.is_obj() && arr.members().empty(), "a settings file that is valid JSON but not an object means defaults");
        delete ui3;
        write_file(spath, "{\"image\": " + json_quote_ascii(dump) + ", \"chip\": [\"BK7231\"], \"key\": {\"x\": 1}}");
        ui3 = new SimUI("", spath, true);
        CHECK(ui3->var_chip.get() == guess_chip(dump) && ui3->var_key.get() == "TUYA",
              "a chip or key of the wrong type is ignored instead of stopping the window");
        delete ui3;
        ui.settings_path.clear();
        ui.save_settings(); /* no settings file: must neither write nor raise */

        ui.reset_options();
        CHECK(ui.opts("mqtt_pubs") == "" && ui.optb("wifi") && ui.optb("net_out") && ui.opts("wifi_pass") == "",
              "Reset to defaults restores real-internet Wi-Fi and clears optional values");

        /* Dropping a file: during a run it stops that run and waits for it to
         * end, then starts; while idle it starts at once. */
        Started started;
        ui.hooks.on_run = hook_record_run;
        ui.hooks.on_run_ctx = &started;
        std::string dropped = temp_file(".bin");
        ui.select_tab(1);
        ui.thread_mode = 2;
        ui.on_drop(StrList(1, dropped));
        CHECK(ui.var_image.get() == dropped && ui.pending_run && ui.stop_requested && started.runs.empty(),
              "a file dropped during a run stops that run and waits for it to end");
        CHECK(SendMessageW(ui.tabs, TCM_GETCURSEL, 0, 0) == 0 && (GetWindowLongW(ui.run_tab, GWL_STYLE) & WS_VISIBLE)
                  && !(GetWindowLongW(ui.config_tab, GWL_STYLE) & WS_VISIBLE),
              "...showing the Run tab");
        ui.pending_run = ui.stop_requested = false;
        ui.on_drop(StrList(1, path_join(tmpdir, "gone.bin")));
        CHECK(!ui.pending_run && !ui.stop_requested && started.runs.empty(), "a drop holding no file is ignored");
        ui.thread_mode = 0;
        ui.on_drop(StrList(1, dropped));
        CHECK(started.runs.size() == 1 && started.runs[0][0] == dropped, "a file dropped while idle runs at once");

        started.runs.clear();
        ui.pending_run = true;
        ui.finish();
        CHECK(started.runs.size() == 1 && !ui.pending_run,
              "a waiting dropped image starts as soon as the run before it has ended");
        ui.finish();
        CHECK(started.runs.size() == 1, "and only once");
        ui.hooks.on_run = 0;
        ui.hooks.on_run_ctx = 0;
        ui.thread_mode = 0;
        ui.pending_run = ui.stop_requested = false;
        DeleteFileW(widen(dropped).c_str());
        remove_tree(tmpdir);
    }

    /* An error is handled only once the worker has gone. */
    {
        Finishes fins;
        fins.n = 0;
        ui.hooks.finish = hook_finish;
        ui.hooks.finish_ctx = &fins;
        ui.thread_mode = 2;
        ui.has_error = true;
        ui.error = "boom";
        ui.update_state();
        CHECK(fins.n == 0 && ui.has_error && ui.error == "boom",
              "a worker error waits for its thread to exit before the run is finished");
        ui.thread_mode = 0;
        ui.update_state();
        CHECK(fins.n == 1 && !ui.has_error, "...and is reported once it has");
        ui.hooks.finish = 0;
        ui.hooks.finish_ctx = 0;
        ui.thread_mode = 0;
        ui.has_error = false;
        ui.error.clear();
    }

    /* LED driver colour swatch and Energy Meter (BL0942 UART / BL0937 Pulse) */
    {
        TuyaCfg lsc;
        lsc.present = true;
        const char *kv[][2] = {{"dmod", "7"}, {"cmod", "rgbcw"}, {"iicscl", "26"}, {"iicsda", "24"}, {"iicr", "0"},
                               {"iicg", "1"}, {"iicb", "2"},     {"iicw", "3"},     {"iicc", "4"},   {"2235ccur", "2"},
                               {"2235wcur", "4"}};
        for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++)
            lsc.pairs.push_back(std::make_pair(std::string(kv[i][0]), std::string(kv[i][1])));
        Bytes sm2235_raw = H("d82400000000000003a20000");
        Bytes bp5758_raw = H("b01f0808081e1ebcbfbcbfbcbfa0a0a0a0");
        Bytes ws_spi;
        for (int i = 0; i < 4; i++)
            ws_spi += Bytes(8, '\xf8') + Bytes(16, '\xc0'); /* 4 green WS2812 pixels */

        ui.tuya_config = lsc;
        ui.run_tuya_config = lsc;
        ViewEmu led;
        led.v.gpio_cfg[24] = 0x02;
        led.v.gpio_cfg[26] = 0x02;
        led.v.i2c_frames[std::make_pair(26, 24)] = sm2235_raw;
        led.v.i2c_last_on[std::make_pair(26, 24)] = sm2235_raw;
        led.v.bl0942_polls = 3;
        led.has_meter_attr = true;
        ui.emu = &led;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(has(ui.var_led.get(), "SM2235") && has(ui.var_led.get(), "W:91%") && ui.swatch_bg != LED_OFF_HEX,
              "SM2235 I2C frame decodes Warm White 91% and lights the color swatch");
        CHECK(has(ui.var_meter.get(), "BL0942 UART1 (3 polls)"),
              "BL0942 UART1 poll count and V/A/W readings show in the meter summary");

        ui.var_meter_v.set("240.5");
        ui.var_meter_a.set("2.25");
        ui.var_meter_w.set("541.0");
        ui.apply_energy_meter();
        CHECK(led.meter_set && led.set_vals[0] == 240.5 && led.set_vals[1] == 2.25 && led.set_vals[2] == 541.0,
              "Set V/A/W pushes updated voltage, current and power into the emulator");

        ui.tuya_config = TuyaCfg();
        ui.run_tuya_config = TuyaCfg();
        led.v.i2c_frames.clear();
        led.v.i2c_last_on.clear();
        led.v.i2c_frames[std::make_pair(24, 26)] = bp5758_raw;
        led.v.i2c_last_on[std::make_pair(24, 26)] = bp5758_raw;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(has(ui.var_led.get(), "BP5758D") && has(ui.var_led.get(), "8/8/8/30/30 mA")
                  && has(ui.var_led.get(), "R:100% G:100% B:100%"),
              "BP5758D I2C frame decodes per-channel mA currents and 100% RGB white");

        led.v.i2c_frames.clear();
        led.v.i2c_last_on.clear();
        led.v.ws2812_spi = ws_spi;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(has(ui.var_led.get(), "WS2812 SPI") && has(ui.var_led.get(), "4 LEDs") && has(ui.var_led.get(), "G:100%"),
              "WS2812 SPI DMA buffer decodes pixel count and RGB color onto the swatch");

        /* Deep sleep status & wake controls */
        ViewEmu sl;
        sl.v.gpio_cfg[16] = 0x0C;
        sl.v.gpio_cfg[20] = 0x0C;
        sl.v.in_deep_sleep = true;
        sl.v.deep_sleep_count = 1;
        sl.v.deep_sleep_rtc_sec = 28800.0;
        sl.v.deep_sleep_wake_pins[16] = 1;
        sl.v.deep_sleep_wake_pins[20] = 0;
        sl.v.insn_count = 500000;
        sl.v.last_pc = 0x10000;
        sl.wake_ok = true;
        ui.thread_mode = 2;
        ui.started_at = mono_time();
        ui.emu = &sl;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        ui.update_state();
        CHECK(ui.var_sleep.get() == "DEEP SLEEP #1 (RTC=28800s, GPIO=[P16:HIGH, P20:LOW])"
                  && startswith(ui.var_state.get(), "DEEP SLEEP") && enabled(ui.btn_wake_rtc) && enabled(ui.btn_wake_gpio)
                  && window_text(ui.btn_wake_gpio) == "Wake P16",
              "Deep sleep state displays RTC timer and GPIO wake pins and enables Wake buttons");
        ui.tree.selection_set("gpio:20");
        ui.gpio_selection_changed();
        CHECK(window_text(ui.btn_wake_gpio) == "Wake P20" && has(ui.var_gpio_action.get(), "deep-sleep wake pin (active LOW)"),
              "selecting a wake-enabled GPIO updates Wake button and Press prompt");
        ui.wake_deep_sleep("gpio");
        CHECK(!sl.wakes.empty() && sl.wakes.back() == std::make_pair(std::string("gpio"), 20)
                  && has(ui.var_sleep.get(), "woke #1 via GPIO P20") && !enabled(ui.btn_wake_rtc),
              "Wake GPIO triggers emulator wakeup on the selected wake pin and updates status");

        /* Camera sensor preview & EJPEG frame status */
        Bytes rgb;
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 64; x++) {
                rgb += (char)((x * 3) & 0xFF);
                rgb += (char)((y * 5) & 0xFF);
                rgb += (char)(((x + y) * 2) & 0xFF);
            }
        sl.v.camera_sensor = "GC0328C";
        sl.v.camera_width = 640;
        sl.v.camera_height = 480;
        sl.v.camera_i2c_writes = 374;
        sl.v.camera_frame_count = 12;
        sl.v.camera_last_jpeg = Bytes("\xff\xd8", 2) + Bytes(1176, '\0') + Bytes("\xff\xd9", 2);
        sl.v.camera_frame_rgb = rgb;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(ui.var_camera.get() == "Camera: GC0328C 640x480 \xe2\x80\x94 12 frames (1180 B JPEG, 374 I2C reg)"
                  && ui.cam_rgb.size() == 64 * 48 * 3,
              "Camera widget displays GC0328C sensor resolution, JPEG frame count, and live preview");

        /* SARADC analog input controls & live status: */
        sl.variant = "BK7231T";
        ui.var_adc_ch.set("3 (P23)");
        ui.var_adc_val.set("512");
        ui.apply_adc_input();
        CHECK(sl.adc_pushed.size() == 1 && sl.adc_pushed[0] == std::make_pair(3, 512) && ui.adc_overrides[3] == 512
                  && has(ui.var_state.get(), "ADC ch3(P23) set to 512"),
              "Set ADC parses channel and sample value and pushes to the running emulator");
        sl.v.saradc_channel = 3;
        sl.v.saradc_last_raw = 512;
        sl.v.saradc_conversions = 4;
        sl.v.saradc_reads = 4;
        sl.v.adc_samples[3] = 512;
        ui.periph_key_valid = false;
        ui.refresh_periph();
        CHECK(has(ui.var_adc.get(), "ch3(P23)=512") && has(ui.var_adc.get(), "4 conv"),
              "ADC widget shows live SARADC channel, raw sample, voltage and conversion count");
        ui.adc_overrides.clear();
        ui.emu = 0;
        ui.thread_mode = 0;
        ui.tuya_config = TuyaCfg();
        ui.run_tuya_config = TuyaCfg();
    }

    delete uip;
    printf("\nAll %d simulator UI self-tests passed.\n", g_n);
    return g_n;
}

int run_selftests()
{
    setvbuf(stdout, 0, _IONBF, 0);
    register_classes();
    net_init();
    g_n = 0;
    int r = selftests_body();
    fflush(stdout);
    return r;
}

/* --live-check (not part of the Python suite): a real emulator run behind the
 * withdrawn window - UART, the network pane, frames, the console, GPIO view
 * and Stop - through the very poll loop the timer drives. */
int run_live_check(const std::string &image, double seconds)
{
    register_classes();
    net_init();
    SimUI *uip = new SimUI("", "", true);
    SimUI &ui = *uip;
    ui.hooks.show_message = hook_message;
    ui.var_image.set(image.empty() ? DEFAULT_IMAGE : image);
    ui.var_chip.set(guess_chip(ui.var_image.get()));
    ui.var_key.set("TUYA");
    ui.reset_options();
    ui.set_option_bool("net_out", false);
    int port = free_tcp_port();
    ui.optv("fwd_local")->set(strf("%d", port));
    ui.on_run();
    double t0 = mono_time();
    bool sent = false, fwd = false;
    while (mono_time() - t0 < seconds) {
        ui.poll();
        if (!sent && ui.console_ready()) {
            ui.send_command("getChannel 1");
            sent = true;
        }
        if (ui.forward_is_open())
            fwd = true;
        if (!ui.thread_alive())
            break;
        sleep_ms(POLL_MS);
    }
    std::string u2 = pane_text(ui.txt2), u1 = pane_text(ui.txt1), net = pane_text(ui.txt_net);
    size_t nframes;
    {
        ScopedLock lk(ui.out_lock);
        nframes = ui.frames.size();
    }
    printf("UART2 %d chars, UART1 %d chars, network %d chars, %d frames, gpio rows %d\n", (int)u2.size(),
           (int)u1.size(), (int)net.size(), (int)nframes, (int)ui.tree.size());
    printf("state: %s\n", ui.var_state.get().c_str());
    printf("summary: %s\n", ui.var_summary.get().c_str());
    printf("pwm: %s\nled: %s\nmeter: %s\nsleep: %s\n", ui.var_pwm.get().c_str(), ui.var_led.get().c_str(),
           ui.var_meter.get().c_str(), ui.var_sleep.get().c_str());
    printf("console sent: %s, forward open: %s, net heading: %s\n", sent ? "yes" : "no", fwd ? "yes" : "no",
           window_text(ui.net_frame).c_str());
    printf("UART1 tail: %s\n", u1.substr(u1.size() > 200 ? u1.size() - 200 : 0).c_str());
    {
        StrList ls = split(u2, "\n");
        for (size_t i = 0; i < ls.size(); i++)
            if (has(ls[i], "hannel") || has(ls[i], "CMD"))
                printf("UART2: %s\n", ls[i].c_str());
    }
    ui.on_stop();
    double t1 = mono_time();
    while (ui.thread_alive() && mono_time() - t1 < 10)
        sleep_ms(20);
    ui.poll();
    printf("after stop: %s (run button %s)\n", ui.var_state.get().c_str(), enabled(ui.btn_run) ? "enabled" : "disabled");
    bool ok = !u2.empty() && sent && !ui.thread_alive() && enabled(ui.btn_run);
    delete uip;
    printf("live check %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

/* --live-sleep: the "OBK sleep" quick run through a real deep sleep and a
 * GUI Wake RTC (the emulator's wake_from_deep_sleep hook). */
int run_live_sleep()
{
    register_classes();
    net_init();
    SimUI *uip = new SimUI("", "", true);
    SimUI &ui = *uip;
    ui.hooks.show_message = hook_message;
    ui.reset_options();
    int idx = -1;
    for (int i = 0; i < N_INTERESTING; i++)
        if (std::string(INTERESTING_DUMPS[i].label) == "OBK sleep")
            idx = i;
    /* run_interesting(idx), but offline: no relay to the real internet */
    const Profile &p = INTERESTING_DUMPS[idx];
    ui.has_pending_uart1_rx = true;
    ui.pending_uart1_rx = p.uart1_rx;
    ui.set_option_bool("net_out", false);
    ui.load_image(fw(p.file), true, p.chip, p.key);
    bool slept = false, woke = false;
    double t0 = mono_time();
    while (mono_time() - t0 < 40 && ui.thread_alive()) {
        ui.poll();
        if (!slept && startswith(ui.var_sleep.get(), "DEEP SLEEP")) {
            slept = true;
            printf("sleep: %s | rtc %s, gpio %s\n", ui.var_sleep.get().c_str(), enabled(ui.btn_wake_rtc) ? "on" : "off",
                   enabled(ui.btn_wake_gpio) ? "on" : "off");
            ui.wake_deep_sleep("rtc");
        }
        if (slept && has(ui.var_sleep.get(), "woke")) {
            woke = true;
            printf("after wake: %s\n", ui.var_sleep.get().c_str());
            break;
        }
        sleep_ms(POLL_MS);
    }
    printf("state: %s\n", ui.var_state.get().c_str());
    ui.on_stop();
    double t1 = mono_time();
    while (ui.thread_alive() && mono_time() - t1 < 10)
        sleep_ms(20);
    ui.poll();
    delete uip;
    printf("live sleep %s\n", slept && woke ? "passed" : "FAILED");
    return slept && woke ? 0 : 1;
}

/* _run_package_smoke: the own ARM core runs (the frozen build's Unicorn
 * hook check), and every release firmware image is present. */
int run_package_smoke()
{
    Memory mem;
    mem.map(0x1000, 0x1000);
    Bytes code;
    for (int i = 0; i < 3; i++)
        code += Bytes("\x00\x00\xa0\xe1", 4); /* ARM: mov r0, r0 */
    mem.write_bytes(0x1000, code);
    ArmCpu cpu(mem);
    cpu.run(0x1000, 0x1000 + (u32)code.size(), 3);
    u32 pc = cpu.reg_read(REG_PC);
    if (pc != 0x1000 + code.size() || !cpu.error().empty()) {
        printf("AssertionError: ARM core smoke test stopped at 0x%08x (%s)\n", pc, cpu.error().c_str());
        return 1;
    }
    std::string fw_dir = FIRMWARE_DIR;
    std::string bundled;
    if (ensure_bundled_firmwares(bundled))
        fw_dir = path_join(bundled, "firmwares");
    StrList required(1, path_join(fw_dir, path_basename(DEFAULT_IMAGE)));
    for (int i = 0; i < N_INTERESTING; i++)
        required.push_back(path_join(fw_dir, INTERESTING_DUMPS[i].file));
    StrList missing;
    std::set<std::string> uniq;
    for (size_t i = 0; i < required.size(); i++) {
        uniq.insert(required[i]);
        if (!path_isfile(required[i]))
            missing.push_back(path_basename(required[i]));
    }
    if (!missing.empty()) {
        printf("AssertionError: release firmware is missing: %s\n", join(missing, ", ").c_str());
        return 1;
    }
    printf("Packaged runtime smoke test passed: ARM core and %d firmware images.\n", (int)uniq.size());
    return 0;
}

} /* namespace simui */
