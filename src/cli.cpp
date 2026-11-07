/* Command-line front end: a port of src/main.py. That includes its argparse
 * parser - long-option prefixes, "-kVALUE"/"--opt=VALUE" forms, the error
 * texts with the usage line and exit status 2, and the --help layout (the
 * HelpFormatter + textwrap algorithm, at the terminal's width) - and main()'s
 * own checks, run in the same order with the same messages: a rejected value
 * prints its message and exits 1, an error Python leaves uncaught (a missing
 * dump, an unwritable capture) prints the exception's last traceback line to
 * stderr and exits 1. */
#include "emulator.h"
#include "periph.h"
#include "netstack.h"
#include "wifipeer.h"
#include "tuya_config.h"
#include "tuyamcu.h"
#include "blecore.h"
#include "wpa2.h"
#include "wifimac.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace pyc; /* main.py str/int/OSError semantics */

namespace {

/* ================================================================ parser */
enum ActId {
    A_HELP, A_DUMP, A_ONLY_UART, A_TRACE_PC, A_WITH_BOOT, A_UART1_HEX, A_UART1_RX, A_UART1_RX_DELAY,
    A_TUYAMCU, A_TUYAMCU_PID, A_TUYAMCU_RAW, A_TUYAMCU_DP, A_TUYAMCU_INJECT, A_XVR_SELFCLEAR,
    A_BLE_CORE, A_WIFI, A_WIFI_PEER, A_WIFI_SSID, A_WIFI_PASS, A_WIFI_CHANNEL, A_WIFI_MAC,
    A_NET_FORWARD, A_NET_OUT, A_NET_HTTP_GET, A_MQTT_PUBLISH, A_PCAP_IP, A_PCAP, A_SNAPSHOT_SAVE,
    A_SNAPSHOT_AT, A_SNAPSHOT_LOAD, A_GPIO_INPUT, A_ADC, A_DEEP_SLEEP_WAKE, A_KEY, A_CHIP, A_COUNT
};
enum Kind { K_HELP, K_TRUE, K_STORE, K_APPEND, K_POS };
enum Type { T_STR, T_INT, T_PC };

struct Action {
    int id;
    std::string s1, s2; /* option strings; both empty for the positional */
    Kind kind;
    Type type;
    std::string metavar;
    std::string help; /* after argparse's %-expansion */
};

const char *const WIFI_ROLES[] = {"auto", "sta", "ap", 0};

std::vector<Action> g_actions;
std::string g_prog;

void add(int id, const char *s1, const char *s2, Kind kind, Type type, const char *metavar, const std::string &help)
{
    Action a;
    a.id = id;
    a.s1 = s1 ? s1 : "";
    a.s2 = s2 ? s2 : "";
    a.kind = kind;
    a.type = type;
    a.metavar = metavar ? metavar : "";
    a.help = help;
    g_actions.push_back(a);
}

void build_parser()
{
    std::string keys, chips;
    for (int i = 0; KNOWN_KEY_NAMES[i]; i++)
        keys += (i ? ", " : "") + std::string(KNOWN_KEY_NAMES[i]);
    StrList cn = chip_family_names();
    std::sort(cn.begin(), cn.end());
    chips = join(cn, ", ");

    add(A_HELP, "-h", "--help", K_HELP, T_STR, 0, "show this help message and exit");
    add(A_DUMP, 0, 0, K_POS, T_STR, "dump_file", "Path to the raw BK7231 flash dump");
    add(A_ONLY_UART, "--only-uart", 0, K_TRUE, T_STR, 0, "Only print UART output (suppress MMIO/Trace/Info logs)");
    add(A_TRACE_PC, "--trace-pc", 0, K_APPEND, T_PC, "ADDRESS",
        "Report the first execution of this exact guest code address as an "
        "[EMU_PC] line. Repeatable; useful when firmware keeps running after "
        "its UART log goes quiet. Bit 0 may mark a Thumb address.");
    add(A_WITH_BOOT, "--with-boot", 0, K_TRUE, T_STR, 0,
        "Start execution from the chip's bootloader partition instead of its app partition");
    add(A_UART1_HEX, "--uart1-hex", 0, K_TRUE, T_STR, 0,
        "Show UART1 (TuyaMCU link) as tagged hex; UART2 stays text log");
    add(A_UART1_RX, "--uart1-rx", 0, K_STORE, T_STR, "TEXT",
        "Feed TEXT into UART1's receive FIFO (a CRLF is appended). With OBK's "
        "UART command console enabled (flag 31), e.g. --uart1-rx \"echo hello\" "
        "types that command in. Use \\n / \\r for explicit line endings.");
    add(A_UART1_RX_DELAY, "--uart1-rx-delay", 0, K_STORE, T_INT, "INSNS",
        strf("Hold the --uart1-rx bytes until this many instructions have run, so the "
             "firmware has registered its console RX callback first (default %llu, "
             "or 0 with --tuyamcu since those replies are already reactive).",
             (unsigned long long)CONSOLE_RX_HOLDOFF_INSNS));
    add(A_TUYAMCU, "--tuyamcu", 0, K_TRUE, T_STR, 0,
        "Attach a simulated TuyaMCU MCU to UART1: answer the firmware's heartbeat, "
        "product-info, working-mode and query-state frames so a TuyaMCU dump walks "
        "past its heartbeat loop instead of stalling with nothing on the wire.");
    add(A_TUYAMCU_PID, "--tuyamcu-pid", 0, K_STORE, T_STR, "PID",
        "Product id the simulated MCU reports for a product-info (0x01) query. "
        "Stock TuyaOS rejects a mismatch, so pass the device's real 16-char PID "
        "(often recoverable from its flash) to get it past the product query.");
    add(A_TUYAMCU_RAW, "--tuyamcu-raw", 0, K_TRUE, T_STR, 0,
        "Reply to the product-info query with the raw 16-byte PID + short version "
        "(TuyaOS 3.x wire form) instead of JSON. Use for TuyaOS 3.x dumps; leave "
        "off for OpenBeken and older SDKs (1.1.71) that expect the JSON record.");
    add(A_TUYAMCU_DP, "--tuyamcu-dp", 0, K_APPEND, T_STR, "ID:TYPE:VALUE",
        "Data point the simulated MCU reports for a query-state (0x08): "
        "ID:TYPE:VALUE, TYPE one of bool/value/enum/bitmap/string. Repeatable. "
        "E.g. --tuyamcu-dp 1:bool:1 --tuyamcu-dp 101:value:230. Lets a device "
        "advance past the working-mode query into data-point reporting.");
    add(A_TUYAMCU_INJECT, "--tuyamcu-inject", 0, K_APPEND, T_STR, "HEX",
        "Raw bytes the simulated MCU puts on the wire on its own initiative, "
        "once the module's startup handshake is done. Repeatable; all queued "
        "blobs go out together on the module's next frame, in order. Unlike "
        "the replies above these are NOT built by the codec, so they can be "
        "deliberately malformed - a wrong checksum, or leading garbage - to "
        "test how the firmware recovers. E.g. --tuyamcu-inject 55AA001C00001B "
        "asks OpenBeken for the time and it answers with a 0x1C frame.");
    add(A_XVR_SELFCLEAR, "--xvr-selfclear", 0, K_TRUE, T_STR, 0,
        "Model the XVR RF/BLE busy bits (0x9000F8 RF-cal, 0x900000 BLE llm_init) "
        "as self-clearing, so stock Tuya SDK 2.x dumps (ATORCH 2.1.17, PC321 "
        "2.0.2, ...) boot past their RF and BLE init spins to the TuyaMCU link. "
        "Opt-in: it breaks dumps that do real BLE init, so only enable per dump.");
    add(A_BLE_CORE, "--ble-core", 0, K_TRUE, T_STR, 0,
        "Force the RivieraWaves BLE 5.x core model for an N/M/BL2028N image "
        "being run under the ambiguous legacy -chip BK7231 identity. "
        "-chip BK7231N and BK7238 select their real BLE 5.1/5.2 hardware "
        "automatically. The model supplies clocks, interrupts, timers and "
        "AES; staged advertisements are printed as [BLE_ADV] lines. No BLE "
        "radio is modelled. Rejected on chips without a modelled 5.x core.");
    add(A_WIFI, "--wifi", 0, K_TRUE, T_STR, 0,
        "Model the RivieraWaves Wi-Fi MAC hardware at 0xC0000000 (TX/RX "
        "descriptor DMA, the 1 MHz MAC clock, ABS timers, TBTT and the "
        "six MAC FIQ lines) so the firmware's own Wi-Fi stack can "
        "actually move frames. Pair with --wifi-peer to put a virtual "
        "access point / station and an IP network on the other side.");
    add(A_WIFI_PEER, "--wifi-peer", 0, K_STORE, T_STR, "ROLE",
        "Attach a virtual 802.11 peer plus a user-mode IP network to the MAC "
        "model (implies --wifi). ROLE auto: take the opposite role of what "
        "the device does (it beacons -> we join it as a station; it probes "
        "or authenticates -> we are the access point it connects to). "
        "sta/ap force one. As an access point the peer is open unless "
        "--wifi-pass makes it WPA2-PSK; as a station it joins open "
        "networks only.");
    add(A_WIFI_SSID, "--wifi-ssid", 0, K_STORE, T_STR, "SSID",
        "SSID the peer beacons as an access point, or the one it joins as a "
        "station. In the AP role without this, the first SSID the device "
        "probes for is adopted.");
    add(A_WIFI_PASS, "--wifi-pass", 0, K_STORE, T_STR, "PASSPHRASE",
        "Make the peer's access point WPA2-PSK protected with this "
        "passphrase instead of open. A paired device that stored a "
        "protected network will not join an open one of the same name, so "
        "this is what lets it on. The device must hold the same passphrase: "
        "the four-way handshake is how each side proves that, and the link "
        "carries no traffic until it completes.");
    add(A_WIFI_CHANNEL, "--wifi-channel", 0, K_STORE, T_INT, "N",
        "Channel the peer's access point announces (default 1).");
    add(A_WIFI_MAC, "--wifi-mac", 0, K_STORE, T_STR, "MAC",
        "MAC address of the peer (default 02:00:00:00:00:01).");
    add(A_NET_FORWARD, "--net-forward", 0, K_APPEND, T_STR, "LPORT:DPORT",
        "Listen on 127.0.0.1:LPORT and connect each accepted TCP connection "
        "to the device's port DPORT over the virtual network, e.g. "
        "--net-forward 8080:80 then open http://127.0.0.1:8080/ for the "
        "firmware's own web panel. Repeatable.");
    add(A_NET_OUT, "--net-out", 0, K_TRUE, T_STR, 0,
        "Let the device's outgoing DNS/TCP/UDP reach real hosts through "
        "the host's sockets. For Tuya this can reconnect an identity "
        "already bound to the same app account; it does not pair or add "
        "a device. Default off: an offline network where DNS "
        "names resolve to private sinkhole addresses, TCP connections are "
        "accepted and what the firmware sends is logged ([NET] lines) and "
        "captured, NTP is answered with the host clock.");
    add(A_NET_HTTP_GET, "--net-http-get", 0, K_APPEND, T_STR, "PATH",
        "Once the device is reachable on the virtual network, send it "
        "\"GET PATH\" on port 80 and log the status line and body as a [NET] "
        "line. Repeatable; requests go one after another. E.g. "
        "--net-http-get \"/cm?cmnd=POWER%20ON\" drives OpenBeken's command API.");
    add(A_MQTT_PUBLISH, "--mqtt-publish", 0, K_APPEND, T_STR, "TOPIC=PAYLOAD",
        "Have the offline MQTT broker push this message to the device, once "
        "the device subscribes to a topic filter that matches it (+ and # "
        "wildcards honoured, as a real broker does). This is the way in: the "
        "firmware picked those filters itself, so e.g. --mqtt-publish "
        "\"cmnd/obkSim/POWER=ON\" drives OpenBeken over MQTT. Repeatable.");
    add(A_PCAP_IP, "--pcap-ip", 0, K_STORE, T_STR, "FILE",
        "Write every frame the IP bridge carried to FILE (link type "
        "Ethernet) for Wireshark: the device's own traffic, both "
        "directions, exactly as it crossed. This is the network; --pcap is "
        "the air.");
    add(A_PCAP, "--pcap", 0, K_STORE, T_STR, "FILE",
        "Write every 802.11 frame crossing the peer to FILE (link type "
        "IEEE 802.11, no FCS) for Wireshark.");
    add(A_SNAPSHOT_SAVE, "--snapshot-save", 0, K_STORE, T_STR, "FILE",
        "Save a snapshot of the whole device (memory, CPU, peripheral state) "
        "to FILE when --snapshot-at is reached, and keep running.");
    add(A_SNAPSHOT_AT, "--snapshot-at", 0, K_STORE, T_STR, "WHEN",
        "When to take the --snapshot-save snapshot: an instruction count, or a "
        "text marker that must appear in the UART output, e.g. "
        "\"sending broadcast_deauth\" for OpenBeken's AP being up.");
    add(A_SNAPSHOT_LOAD, "--snapshot-load", 0, K_STORE, T_STR, "FILE",
        "Resume from a snapshot instead of booting. The dump, -chip and --wifi "
        "must match the run that saved it; a peer / port forwards / pcap "
        "given now are attached to the restored device.");
    add(A_GPIO_INPUT, "--gpio-input", 0, K_APPEND, T_STR, "PIN:LEVEL",
        "Drive physical GPIO pin PIN to LEVEL (0 or 1) from power-on. "
        "Repeatable, e.g. --gpio-input 20:1 holds an active-low reset "
        "button high at its external pull-up idle state.");
    add(A_ADC, "--adc", 0, K_APPEND, T_STR, "CH:VALUE",
        "Set SARADC analog input channel CH (0..15, chN, or P<pin>) to "
        "VALUE (raw 10-bit 0..1023 or voltage with 'V'/'mV' suffix, e.g. "
        "--adc 3:512 or --adc P26:1.65V). Repeatable.");
    add(A_DEEP_SLEEP_WAKE, "--deep-sleep-wake", 0, K_APPEND, T_STR, "SPEC",
        "Automatically wake the chip when it enters deep sleep (0x800104=0xADC1). "
        "SPEC is rtc (RTC timer), gpio (first wake-enabled GPIO), gpio:PIN "
        "(specific GPIO pin), or auto. Repeatable for multi-cycle sleep/wake.");
    add(A_KEY, "-key", "--key", K_STORE, T_STR, "KEY",
        "Firmware decryption key: a known name (" + keys + "), 32 hex chars, or base64 of 16 bytes. "
        "Omit for plaintext images (no decryption).");
    add(A_CHIP, "-chip", "--chip", K_STORE, T_STR, "CHIP",
        "Chip/variant served from the SCTRL id registers: " + chips + ". "
        "Default: BK7231; select BK7231U for its GPIO32..39 bank.");
}

/* ------------------------------------------------- textwrap / formatter */
bool tw_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
bool tw_w(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool tw_lt(int c) { return tw_w(c) && !(c >= '0' && c <= '9'); }
bool tw_wp(int c) { return tw_w(c) || (c && strchr("!\"'&.,?", c)); }

/* textwrap.TextWrapper._split with break_on_hyphens: the wordsep_re chunks */
StrList tw_split(const std::string &t)
{
    StrList chunks;
    int n = (int)t.size();
#define AT(k) ((k) >= 0 && (k) < n ? (int)(u8)t[k] : 0)
    int i = 0;
    while (i < n) {
        int j = i;
        if (tw_space(AT(i))) { /* any whitespace */
            while (j < n && tw_space(AT(j)))
                j++;
        } else {
            bool done = false;
            if (AT(i) == '-' && tw_wp(AT(i - 1))) { /* em-dash between words */
                int k = i;
                while (AT(k) == '-')
                    k++;
                if (k - i >= 2 && tw_w(AT(k))) {
                    j = k;
                    done = true;
                }
            }
            for (int e = i + 1; !done; e++) { /* word, possibly hyphenated */
                bool lb = (tw_lt(AT(e - 2)) && tw_lt(AT(e - 1)))
                          || (tw_lt(AT(e - 3)) && AT(e - 2) == '-' && tw_lt(AT(e - 1)));
                bool la = tw_lt(AT(e + 1)) && ((AT(e + 2) == '-' && tw_lt(AT(e + 3))) || tw_lt(AT(e + 2)));
                if (AT(e) == '-' && lb && la) {
                    j = e + 1;
                    done = true;
                } else if (e >= n || tw_space(AT(e))) {
                    j = e;
                    done = true;
                } else if (tw_wp(AT(e - 1)) && AT(e) == '-' && AT(e + 1) == '-') {
                    int k = e;
                    while (AT(k) == '-')
                        k++;
                    if (tw_w(AT(k))) {
                        j = e;
                        done = true;
                    }
                }
            }
        }
        chunks.push_back(t.substr((size_t)i, (size_t)(j - i)));
        i = j;
    }
#undef AT
    return chunks;
}

bool all_space(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++)
        if (!tw_space((u8)s[i]))
            return false;
    return true;
}

/* textwrap.wrap(text, width) after argparse's whitespace squeeze */
StrList tw_wrap(const std::string &raw, int width, const std::string &indent = "")
{
    std::string text;
    for (size_t i = 0; i < raw.size(); i++) {
        if (tw_space((u8)raw[i])) {
            if (!text.empty() && text[text.size() - 1] != ' ')
                text += ' ';
        } else
            text += raw[i];
    }
    if (!text.empty() && text[text.size() - 1] == ' ')
        text.erase(text.size() - 1);
    StrList chunks = tw_split(text);
    std::reverse(chunks.begin(), chunks.end());
    StrList lines;
    while (!chunks.empty()) {
        StrList cur;
        int cur_len = 0;
        int w = width - (int)indent.size();
        if (all_space(chunks.back()) && !lines.empty())
            chunks.pop_back();
        while (!chunks.empty()) {
            int l = (int)py_len(chunks.back());
            if (cur_len + l <= w) {
                cur.push_back(chunks.back());
                chunks.pop_back();
                cur_len += l;
            } else
                break;
        }
        if (!chunks.empty() && (int)py_len(chunks.back()) > w) { /* _handle_long_word */
            int space_left = w < 1 ? 1 : w - cur_len;
            if (space_left > 0) {
                std::string chunk = chunks.back();
                int end = space_left;
                if ((int)chunk.size() > space_left) {
                    size_t hy = chunk.rfind('-', (size_t)space_left - 1);
                    if (hy != std::string::npos && hy > 0 && chunk.find_first_not_of('-') < hy)
                        end = (int)hy + 1;
                }
                cur.push_back(chunk.substr(0, (size_t)end));
                chunks.back() = chunk.substr((size_t)end);
            } else if (cur.empty()) {
                cur.push_back(chunks.back());
                chunks.pop_back();
            }
        }
        if (!cur.empty() && all_space(cur.back()))
            cur.pop_back();
        if (!cur.empty())
            lines.push_back(indent + join(cur, ""));
    }
    return lines;
}

/* shutil.get_terminal_size().columns - 2 */
int help_width()
{
    long long cols = 0;
    std::string c = env_get("COLUMNS"), l = env_get("LINES");
    long long lines = 0;
    if (!py_int(c, 10, cols))
        cols = 0;
    if (!py_int(l, 10, lines))
        lines = 0;
    if (cols <= 0 || lines <= 0) {
        long long term = 0;
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
            term = info.srWindow.Right - info.srWindow.Left + 1;
#else
        struct winsize ws;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0)
            term = ws.ws_col;
#endif
        if (cols <= 0)
            cols = term > 0 ? term : 80;
    }
    if (cols > 100000)
        cols = 100000;
    return (int)cols - 2;
}

std::string invocation(const Action &a)
{
    if (a.kind == K_POS)
        return a.metavar;
    std::string s = a.s1 + (a.s2.empty() ? "" : ", " + a.s2);
    if (a.kind == K_HELP || a.kind == K_TRUE)
        return s;
    return s + " " + a.metavar;
}

std::string format_usage(int width)
{
    const std::string prefix = "usage: ";
    StrList opt_parts, pos_parts;
    for (size_t i = 0; i < g_actions.size(); i++) {
        const Action &a = g_actions[i];
        if (a.kind == K_POS)
            pos_parts.push_back(a.metavar);
        else if (a.kind == K_HELP || a.kind == K_TRUE)
            opt_parts.push_back("[" + a.s1 + "]");
        else
            opt_parts.push_back("[" + a.s1 + " " + a.metavar + "]");
    }
    StrList all = opt_parts;
    all.insert(all.end(), pos_parts.begin(), pos_parts.end());
    std::string usage = g_prog + " " + join(all, " ");
    int text_width = width;
    if ((int)(prefix.size() + py_len(usage)) > text_width) {
        struct L {
            static StrList get_lines(const StrList &parts, const std::string &indent, const std::string *pfx,
                                     int text_width)
            {
                StrList lines, line;
                int indent_length = (int)indent.size();
                int line_len = pfx ? (int)pfx->size() - 1 : indent_length - 1;
                for (size_t i = 0; i < parts.size(); i++) {
                    int part_len = (int)py_len(parts[i]);
                    if (line_len + 1 + part_len > text_width && !line.empty()) {
                        lines.push_back(indent + join(line, " "));
                        line.clear();
                        line_len = indent_length - 1;
                    }
                    line.push_back(parts[i]);
                    line_len += part_len + 1;
                }
                if (!line.empty())
                    lines.push_back(indent + join(line, " "));
                if (pfx && !lines.empty())
                    lines[0] = lines[0].substr((size_t)indent_length);
                return lines;
            }
        };
        int prog_len = (int)py_len(g_prog);
        StrList lines;
        if (4 * ((int)prefix.size() + prog_len) <= 3 * text_width) {
            std::string indent((size_t)(prefix.size() + prog_len + 1), ' ');
            StrList first(1, g_prog);
            if (!opt_parts.empty()) {
                first.insert(first.end(), opt_parts.begin(), opt_parts.end());
                lines = L::get_lines(first, indent, &prefix, text_width);
                StrList more = L::get_lines(pos_parts, indent, 0, text_width);
                lines.insert(lines.end(), more.begin(), more.end());
            } else if (!pos_parts.empty()) {
                first.insert(first.end(), pos_parts.begin(), pos_parts.end());
                lines = L::get_lines(first, indent, &prefix, text_width);
            } else
                lines = first;
        } else {
            std::string indent(prefix.size(), ' ');
            lines = L::get_lines(all, indent, 0, text_width);
            if (lines.size() > 1) {
                lines = L::get_lines(opt_parts, indent, 0, text_width);
                StrList more = L::get_lines(pos_parts, indent, 0, text_width);
                lines.insert(lines.end(), more.begin(), more.end());
            }
            lines.insert(lines.begin(), g_prog);
        }
        usage = join(lines, "\n");
    }
    return prefix + usage + "\n\n";
}

std::string format_help()
{
    int width = help_width();
    int max_help_position = std::min(24, std::max(width - 20, 4));
    int max_len = 0;
    for (size_t i = 0; i < g_actions.size(); i++)
        max_len = std::max(max_len, (int)py_len(invocation(g_actions[i])) + 2);
    int help_position = std::min(max_len + 2, max_help_position);
    int hwidth = std::max(width - help_position, 11);
    int action_width = help_position - 2 - 2;

    std::string sec[2]; /* positional arguments, options */
    for (size_t i = 0; i < g_actions.size(); i++) {
        const Action &a = g_actions[i];
        std::string header = invocation(a), part;
        int indent_first;
        if ((int)py_len(header) <= action_width) {
            part = "  " + header + std::string((size_t)(action_width - (int)py_len(header)), ' ') + "  ";
            indent_first = 0;
        } else {
            part = "  " + header + "\n";
            indent_first = help_position;
        }
        StrList lines = tw_wrap(a.help, hwidth);
        for (size_t k = 0; k < lines.size(); k++)
            part += std::string((size_t)(k ? help_position : indent_first), ' ') + lines[k] + "\n";
        sec[a.kind == K_POS ? 0 : 1] += part;
    }
    std::string help = format_usage(width) + join(tw_wrap("BK7231T/N Emulator", std::max(width, 11)), "\n") + "\n\n"
                       + "\npositional arguments:\n" + sec[0] + "\n" + "\noptions:\n" + sec[1] + "\n";
    /* re.sub('\n\n\n+', '\n\n') then strip('\n') + '\n' */
    std::string out;
    for (size_t i = 0; i < help.size();) {
        if (help[i] == '\n') {
            size_t j = i;
            while (j < help.size() && help[j] == '\n')
                j++;
            out += std::string(j - i >= 3 ? 2 : j - i, '\n');
            i = j;
        } else
            out += help[i++];
    }
    size_t b = out.find_first_not_of('\n'), e = out.find_last_not_of('\n');
    return b == std::string::npos ? "\n" : out.substr(b, e - b + 1) + "\n";
}

/* parser.error(): usage + "prog: error: msg" on stderr, exit status 2 */
int arg_error(const std::string &msg)
{
    std::string u = format_usage(help_width());
    out_flush();
    err_write(u.substr(0, u.size() - 1)); /* format_usage() ends "\n" after strip */
    err_write(g_prog + ": error: " + msg + "\n");
    return 2;
}

std::string action_name(const Action &a)
{
    if (a.kind == K_POS)
        return a.metavar;
    return a.s1 + (a.s2.empty() ? "" : "/" + a.s2);
}

/* the parsed namespace */
struct Args {
    bool flag[A_COUNT];
    bool given[A_COUNT];
    std::string str[A_COUNT];
    StrList list[A_COUNT];
    long long num[A_COUNT];
    std::vector<u32> trace_pcs;
    Args()
    {
        for (int i = 0; i < A_COUNT; i++) {
            flag[i] = given[i] = false;
            num[i] = 0;
        }
    }
};

struct OptTuple {
    int act; /* index into g_actions, -1 = no such option */
    std::string option_string;
    bool has_sep; /* sep is not None */
    std::string sep;
    bool has_explicit;
    std::string explicit_arg;
};

int find_option(const std::string &s)
{
    for (size_t i = 0; i < g_actions.size(); i++)
        if (g_actions[i].s1 == s || (!g_actions[i].s2.empty() && g_actions[i].s2 == s))
            return (int)i;
    return -1;
}

/* the parser's option strings, in _option_string_actions order */
std::vector<std::pair<std::string, int> > option_strings()
{
    std::vector<std::pair<std::string, int> > out;
    for (size_t i = 0; i < g_actions.size(); i++) {
        if (!g_actions[i].s1.empty())
            out.push_back(std::make_pair(g_actions[i].s1, (int)i));
        if (!g_actions[i].s2.empty())
            out.push_back(std::make_pair(g_actions[i].s2, (int)i));
    }
    return out;
}

OptTuple tup(int act, const std::string &os, bool has_sep, const std::string &sep, bool has_ex, const std::string &ex)
{
    OptTuple t;
    t.act = act;
    t.option_string = os;
    t.has_sep = has_sep;
    t.sep = sep;
    t.has_explicit = has_ex;
    t.explicit_arg = ex;
    return t;
}

/* ArgumentParser._parse_optional: false = a positional ('A') */
bool parse_optional(const std::string &arg, std::vector<OptTuple> &out)
{
    out.clear();
    if (arg.empty() || arg[0] != '-')
        return false;
    int a = find_option(arg);
    if (a >= 0) {
        out.push_back(tup(a, arg, false, "", false, ""));
        return true;
    }
    if (arg.size() == 1)
        return false;
    size_t eq = arg.find('=');
    if (eq != std::string::npos) {
        int b = find_option(arg.substr(0, eq));
        if (b >= 0) {
            out.push_back(tup(b, arg.substr(0, eq), true, "=", true, arg.substr(eq + 1)));
            return true;
        }
    }
    /* _get_option_tuples */
    std::vector<std::pair<std::string, int> > opts = option_strings();
    std::string prefix = eq == std::string::npos ? arg : arg.substr(0, eq);
    bool has_sep = eq != std::string::npos;
    std::string ex = has_sep ? arg.substr(eq + 1) : "";
    if (arg[1] == '-') {
        for (size_t i = 0; i < opts.size(); i++)
            if (opts[i].first.compare(0, prefix.size(), prefix) == 0)
                out.push_back(tup(opts[i].second, opts[i].first, has_sep, "=", has_sep, ex));
    } else {
        std::string short_prefix = arg.substr(0, 2), short_ex = arg.substr(2);
        for (size_t i = 0; i < opts.size(); i++) {
            if (opts[i].first == short_prefix)
                out.push_back(tup(opts[i].second, opts[i].first, true, "", true, short_ex));
            else if (opts[i].first.compare(0, prefix.size(), prefix) == 0)
                out.push_back(tup(opts[i].second, opts[i].first, has_sep, "=", has_sep, ex));
        }
    }
    if (!out.empty())
        return true;
    /* _negative_number_matcher '-\.?\d' (no option looks like a number) */
    size_t k = 1;
    if (k < arg.size() && arg[k] == '.')
        k++;
    if (k < arg.size() && arg[k] >= '0' && arg[k] <= '9')
        return false;
    if (arg.find(' ') != std::string::npos)
        return false;
    out.push_back(tup(-1, arg, false, "", false, ""));
    return true;
}

/* argparse's take_action: -1 to go on, else the exit status */
int take_action(int ai, const std::string &value, Args &ns)
{
    const Action &a = g_actions[(size_t)ai];
    ns.given[a.id] = true;
    switch (a.kind) {
    case K_HELP:
        out_write(format_help());
        out_flush();
        return 0;
    case K_TRUE:
        ns.flag[a.id] = true;
        return -1;
    default:
        break;
    }
    if (a.type == T_INT) {
        long long n;
        bool huge;
        if (!py_int(value, 10, n, &huge))
            return arg_error("argument " + action_name(a) + ": invalid int value: " + py_repr(value));
        ns.num[a.id] = n;
    } else if (a.type == T_PC) { /* parse_pc */
        long long n;
        bool huge;
        if (!py_int(value, 0, n, &huge))
            return arg_error("argument " + action_name(a) + ": want an address such as 0x1234");
        if (huge || n < 0 || n > 0xFFFFFFFFLL)
            return arg_error("argument " + action_name(a) + ": address must fit in 32 bits");
        ns.trace_pcs.push_back((u32)n);
    }
    if (a.id == A_WIFI_PEER) {
        bool ok = false;
        for (int i = 0; WIFI_ROLES[i]; i++)
            ok = ok || value == WIFI_ROLES[i];
        if (!ok)
            return arg_error("argument " + action_name(a) + ": invalid choice: " + py_repr(value)
                             + " (choose from auto, sta, ap)");
    }
    if (a.kind == K_APPEND)
        ns.list[a.id].push_back(value);
    else
        ns.str[a.id] = value;
    return -1;
}

/* ArgumentParser.parse_args for this parser; -1 to go on, else exit status */
int parse_args(const StrList &args, Args &ns)
{
    size_t n = args.size();
    std::string pattern;
    std::vector<std::vector<OptTuple> > tuples(n);
    for (size_t i = 0; i < n; i++) {
        if (args[i] == "--") {
            pattern += '-';
            for (i++; i < n; i++)
                pattern += 'A';
            break;
        }
        pattern += parse_optional(args[i], tuples[i]) ? 'O' : 'A';
    }

    StrList extras;
    bool positional_done = false;
    int rc = -1;

    /* consume_positionals: the one dump_file, pattern '(-*A-*)' */
    struct P {
        static size_t consume(const StrList &args, const std::string &pattern, size_t start, bool &done, Args &ns,
                              int &rc)
        {
            if (done)
                return start;
            size_t k = start;
            while (k < pattern.size() && pattern[k] == '-')
                k++;
            if (k >= pattern.size() || pattern[k] != 'A')
                return start;
            k++;
            while (k < pattern.size() && pattern[k] == '-')
                k++;
            StrList vals(args.begin() + (long)start, args.begin() + (long)k);
            if (pattern.find('-', start) < k)
                vals.erase(std::find(vals.begin(), vals.end(), std::string("--")));
            done = true;
            for (size_t i = 0; i < g_actions.size(); i++)
                if (g_actions[i].kind == K_POS)
                    rc = take_action((int)i, vals[0], ns);
            return k;
        }
    };

    int max_opt = -1;
    for (size_t i = 0; i < pattern.size(); i++)
        if (pattern[i] == 'O')
            max_opt = (int)i;
    size_t start = 0;
    while ((int)start <= max_opt) {
        size_t next = start;
        while ((int)next <= max_opt && pattern[next] != 'O')
            next++;
        if (start != next) {
            size_t end = P::consume(args, pattern, start, positional_done, ns, rc);
            if (rc >= 0)
                return rc;
            if (end > start) {
                start = end;
                continue;
            }
            start = end;
        }
        if (pattern[start] != 'O') {
            for (size_t i = start; i < next; i++)
                extras.push_back(args[i]);
            start = next;
        }
        /* consume_optional */
        const std::vector<OptTuple> &ts = tuples[start];
        if (ts.size() > 1) {
            std::string names;
            for (size_t i = 0; i < ts.size(); i++)
                names += (i ? ", " : "") + ts[i].option_string;
            return arg_error("ambiguous option: " + args[start] + " could match " + names);
        }
        OptTuple t = ts[0];
        std::vector<std::pair<int, std::string> > todo; /* (action, value) */
        size_t stop = start + 1;
        for (;;) {
            if (t.act < 0) {
                extras.push_back(args[start]);
                break;
            }
            const Action &a = g_actions[(size_t)t.act];
            bool nargs0 = a.kind == K_HELP || a.kind == K_TRUE;
            if (t.has_explicit) {
                if (nargs0 && t.option_string[1] != '-' && !t.explicit_arg.empty()) {
                    if (!t.sep.empty() || t.explicit_arg[0] == '-')
                        return arg_error("argument " + action_name(a) + ": ignored explicit argument "
                                         + py_repr(t.explicit_arg));
                    todo.push_back(std::make_pair(t.act, std::string()));
                    std::string os = std::string("-") + t.explicit_arg[0];
                    int b = find_option(os);
                    if (b >= 0) {
                        std::string rest = t.explicit_arg.substr(1);
                        t = tup(b, os, false, "", false, "");
                        if (!rest.empty()) {
                            if (rest[0] == '=')
                                t = tup(b, os, true, "=", true, rest.substr(1));
                            else
                                t = tup(b, os, true, "", true, rest);
                        }
                        continue;
                    }
                    extras.push_back("-" + t.explicit_arg);
                    break;
                } else if (!nargs0) {
                    todo.push_back(std::make_pair(t.act, t.explicit_arg));
                    break;
                }
                return arg_error("argument " + action_name(a) + ": ignored explicit argument "
                                 + py_repr(t.explicit_arg));
            }
            if (nargs0) {
                todo.push_back(std::make_pair(t.act, std::string()));
            } else {
                if (start + 1 >= pattern.size() || pattern[start + 1] != 'A')
                    return arg_error("argument " + action_name(a) + ": expected one argument");
                todo.push_back(std::make_pair(t.act, args[start + 1]));
                stop = start + 2;
            }
            break;
        }
        for (size_t i = 0; i < todo.size(); i++) {
            rc = take_action(todo[i].first, todo[i].second, ns);
            if (rc >= 0)
                return rc;
        }
        start = stop;
    }
    size_t stop_index = P::consume(args, pattern, start, positional_done, ns, rc);
    if (rc >= 0)
        return rc;
    for (size_t i = stop_index; i < n; i++)
        extras.push_back(args[i]);
    if (!positional_done)
        return arg_error("the following arguments are required: dump_file");
    if (!extras.empty())
        return arg_error("unrecognized arguments: " + join(extras, " "));
    return -1;
}

/* ============================================================== main.py */
/* An exception main.py does not catch: its traceback ends with this line. */
int uncaught(const std::string &type, const std::string &msg)
{
    out_flush();
    err_write("Traceback (most recent call last):\n");
    err_write(type + ": " + msg + "\n");
    return 1;
}

/* A ValueError main() catches: print(e); sys.exit(1) */
int value_error(const std::string &msg)
{
    out_write(msg + "\n");
    out_flush();
    return 1;
}

/* args.uart1_rx.encode("latin-1").decode("unicode_escape"), then the CRLF
 * and .encode("latin-1", "replace"). Returns false with the exception. */
bool uart1_rx_bytes(const std::string &arg, Bytes &out, std::string &etype, std::string &emsg)
{
    std::vector<u32> cps = py_codepoints(arg);
    for (size_t i = 0; i < cps.size(); i++) {
        if (cps[i] <= 0xFF)
            continue;
        size_t j = i;
        while (j < cps.size() && cps[j] > 0xFF)
            j++;
        etype = "UnicodeEncodeError";
        if (j - i == 1)
            emsg = strf(cps[i] < 0x10000 ? "'latin-1' codec can't encode character '\\u%04x' in position %d: "
                                         : "'latin-1' codec can't encode character '\\U%08x' in position %d: ",
                        cps[i], (int)i);
        else
            emsg = strf("'latin-1' codec can't encode characters in position %d-%d: ", (int)i, (int)j - 1);
        emsg += "ordinal not in range(256)";
        return false;
    }
    std::vector<u32> text;
    std::string warned; /* the codec warns about the first invalid escape only */
    size_t n = cps.size();
    for (size_t i = 0; i < n;) {
        u32 c = cps[i];
        if (c != '\\') {
            text.push_back(c);
            i++;
            continue;
        }
        size_t start = i++;
        if (i >= n) {
            etype = "UnicodeDecodeError";
            emsg = strf("'unicodeescape' codec can't decode byte 0x5c in position %d: \\ at end of string", (int)start);
            return false;
        }
        u32 e = cps[i++];
        switch (e) {
        case '\n': continue;
        case '\\': text.push_back('\\'); continue;
        case '\'': text.push_back('\''); continue;
        case '"': text.push_back('"'); continue;
        case 'b': text.push_back(8); continue;
        case 'f': text.push_back(12); continue;
        case 't': text.push_back(9); continue;
        case 'n': text.push_back(10); continue;
        case 'r': text.push_back(13); continue;
        case 'v': text.push_back(11); continue;
        case 'a': text.push_back(7); continue;
        default: break;
        }
        if (e >= '0' && e <= '7') {
            u32 v = e - '0';
            for (int k = 0; k < 2 && i < n && cps[i] >= '0' && cps[i] <= '7'; k++)
                v = v * 8 + (cps[i++] - '0');
            if (v > 0377) {
                std::string w;
                for (size_t k = start + 1; k < i && k < start + 4; k++)
                    w += (char)cps[k];
                if (warned.empty()) warned = ("\"\\" + w + "\" is an invalid octal escape sequence. "
                              "Such sequences will not work in the future. ");
            }
            text.push_back(v);
            continue;
        }
        int digits = e == 'x' ? 2 : e == 'u' ? 4 : e == 'U' ? 8 : 0;
        if (digits) {
            u32 v = 0;
            int got = 0;
            while (got < digits && i < n) {
                u32 h = cps[i];
                int d = (h >= '0' && h <= '9') ? (int)h - '0' : (h >= 'a' && h <= 'f') ? (int)h - 'a' + 10
                        : (h >= 'A' && h <= 'F') ? (int)h - 'A' + 10 : -1;
                if (d < 0)
                    break;
                v = v * 16 + (u32)d;
                got++;
                i++;
            }
            etype = "UnicodeDecodeError";
            if (got < digits) {
                emsg = strf("'unicodeescape' codec can't decode bytes in position %d-%d: truncated \\%s escape",
                            (int)start, (int)i - 1, e == 'x' ? "xXX" : e == 'u' ? "uXXXX" : "UXXXXXXXX");
                return false;
            }
            if (v > 0x10FFFF) {
                emsg = strf("'unicodeescape' codec can't decode bytes in position %d-%d: illegal Unicode character",
                            (int)start, (int)i - 1);
                return false;
            }
            text.push_back(v);
            continue;
        }
        if (e == 'N') {
            /* \N{name}: the Unicode name database is not carried; a
             * well-formed name stands for a character outside Latin-1,
             * which the final encode turns into '?'. */
            size_t k = i;
            if (k < n && cps[k] == '{') {
                size_t close = k + 1;
                while (close < n && cps[close] != '}')
                    close++;
                if (close < n && close > k + 1) {
                    text.push_back(0xFFFD);
                    i = close + 1;
                    continue;
                }
                i = close; /* at the "}" of an empty name, or the end */
            }
            etype = "UnicodeDecodeError";
            emsg = strf("'unicodeescape' codec can't decode bytes in position %d-%d: malformed \\N character escape",
                        (int)start, (int)(i > start + 2 ? i - 1 : start + 1));
            return false;
        }
        /* an unknown escape stays as written (and warns) */
        if (warned.empty()) warned = ("\"\\" + py_utf8(e) + "\" is an invalid escape sequence. "
                      "Such sequences will not work in the future. ");
        text.push_back('\\');
        text.push_back(e);
    }
    if (!warned.empty()) /* shown when main.py runs as __main__ */
        err_write("DeprecationWarning: " + warned + "\n");
    if (text.empty() || (text.back() != '\n' && text.back() != '\r')) {
        text.push_back('\r');
        text.push_back('\n');
    }
    out.clear();
    for (size_t i = 0; i < text.size(); i++)
        out += (char)(u8)(text[i] <= 0xFF ? text[i] : '?');
    return true;
}

/* parse_dp_specs */
bool parse_dp_specs(const StrList &specs, std::vector<std::pair<int, tuyamcu::DpSpec> > &dps, std::string &err)
{
    std::map<long long, size_t> where;
    for (size_t i = 0; i < specs.size(); i++) {
        StrList parts = split(specs[i], ":", 2);
        if (parts.size() != 3) {
            err = "bad --tuyamcu-dp " + py_repr(specs[i]) + " (want ID:TYPE:VALUE)";
            return false;
        }
        const std::string &ts = parts[1];
        tuyamcu::DpSpec s;
        if (ts == "bool") s.type = tuyamcu::DP_BOOL;
        else if (ts == "value") s.type = tuyamcu::DP_VALUE;
        else if (ts == "enum") s.type = tuyamcu::DP_ENUM;
        else if (ts == "bitmap") s.type = tuyamcu::DP_BITMAP;
        else if (ts == "string") s.type = tuyamcu::DP_STRING;
        else {
            err = "bad DP type " + py_repr(ts) + " (one of bool/value/enum/bitmap/string)";
            return false;
        }
        s.ivalue = 0;
        if (s.type == tuyamcu::DP_STRING)
            s.svalue = parts[2];
        else if (!py_int(parts[2], 0, s.ivalue)) {
            err = py_int_error(parts[2], 0);
            return false;
        }
        long long id;
        if (!py_int(parts[0], 0, id)) {
            err = py_int_error(parts[0], 0);
            return false;
        }
        if (where.count(id)) /* a dict: a repeated id keeps its slot */
            dps[where[id]].second = s;
        else {
            where[id] = dps.size();
            dps.push_back(std::make_pair((int)id, s));
        }
    }
    return true;
}

/* parse_hex_blobs */
bool parse_hex_blobs(const StrList &specs, std::vector<Bytes> &out, std::string &err)
{
    for (size_t i = 0; i < specs.size(); i++) {
        std::string t = replace_all(replace_all(specs[i], " ", ""), "_", "");
        if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
            t = t.substr(2);
        bool ok = true;
        Bytes b = unhex(t, &ok);
        if (!ok) {
            err = "bad --tuyamcu-inject " + py_repr(specs[i]) + " (want hex bytes, e.g. 55AA001C00001B)";
            return false;
        }
        out.push_back(b);
    }
    return true;
}

/* parse_deep_sleep_wakes */
bool parse_deep_sleep_wakes(const StrList &specs, StrList &out, std::string &err)
{
    for (size_t i = 0; i < specs.size(); i++) {
        std::string text = lower(py_strip(specs[i])); /* str(spec).strip().lower() */
        if (text == "rtc" || text == "gpio" || text == "auto")
            out.push_back(text);
        else if (text.compare(0, 5, "gpio:") == 0) {
            long long pin;
            if (!py_int(text.substr(5), 0, pin)) {
                err = "bad --deep-sleep-wake " + py_repr(specs[i]) + " (want rtc, gpio, gpio:PIN, or auto)";
                return false;
            }
            out.push_back(strf("gpio:%lld", pin));
        } else {
            err = "bad --deep-sleep-wake " + py_repr(specs[i]) + " (want rtc, gpio, gpio:PIN, or auto)";
            return false;
        }
    }
    return true;
}

/* parse_gpio_inputs */
bool parse_gpio_inputs(const StrList &specs, std::map<int, int> &out, std::string &err)
{
    for (size_t i = 0; i < specs.size(); i++) {
        StrList parts = split(specs[i], ":", 1);
        long long pin, lvl;
        if (parts.size() != 2 || !py_int(parts[0], 0, pin) || !py_int(parts[1], 0, lvl)) {
            err = "bad --gpio-input " + py_repr(specs[i]) + " (want PIN:LEVEL, e.g. 20:1)";
            return false;
        }
        if (lvl != 0 && lvl != 1) {
            err = "bad --gpio-input " + py_repr(specs[i]) + " (level must be 0 or 1)";
            return false;
        }
        /* out of int range is out of the pin range all the same */
        out[(int)std::max(-0x7FFFFFFFLL, std::min(pin, 0x7FFFFFFFLL))] = (int)lvl;
    }
    return true;
}

/* parse_mac */
bool parse_mac(const std::string &text, Bytes &mac, std::string &err)
{
    StrList parts = split(replace_all(text, "-", ":"), ":");
    err = "bad --wifi-mac " + py_repr(text) + " (want aa:bb:cc:dd:ee:ff)";
    if (parts.size() != 6)
        return false;
    mac.clear();
    for (size_t i = 0; i < parts.size(); i++) {
        long long v;
        if (!py_int(parts[i], 16, v) || v < 0 || v > 255) /* int(p, 16) then bytes() */
            return false;
        mac += (char)(u8)v;
    }
    return true;
}

/* parse_forwards */
bool parse_forwards(const StrList &specs, std::vector<std::pair<int, int> > &out, std::string &err)
{
    for (size_t i = 0; i < specs.size(); i++) {
        StrList p = split(specs[i], ":");
        bool ok = p.size() == 2;
        long long v[2] = {0, 0};
        for (size_t k = 0; ok && k < p.size(); k++) {
            ok = !p[k].empty();
            for (size_t j = 0; ok && j < p[k].size(); j++)
                ok = p[k][j] >= '0' && p[k][j] <= '9';
            ok = ok && py_int(p[k], 10, v[k]) && v[k] > 0 && v[k] < 65536;
        }
        if (!ok) {
            err = "bad --net-forward " + py_repr(specs[i]) + " (want LPORT:DPORT, ports 1-65535, e.g. 8080:80)";
            return false;
        }
        out.push_back(std::make_pair((int)v[0], (int)v[1]));
    }
    return true;
}

/* parse_http_gets: the request line goes out as Latin-1 */
bool parse_http_gets(const StrList &paths, std::string &err)
{
    for (size_t i = 0; i < paths.size(); i++) {
        std::vector<u32> cps = py_codepoints(paths[i]);
        for (size_t k = 0; k < cps.size(); k++)
            if (cps[k] > 0xFF) {
                err = "bad --net-http-get " + py_repr(paths[i])
                      + " (percent-encode characters outside Latin-1, e.g. %20 for a space)";
                return false;
            }
    }
    return true;
}

/* parse_mqtt_pubs */
bool parse_mqtt_pubs(const StrList &specs, std::vector<std::pair<std::string, Bytes> > &out, std::string &err)
{
    for (size_t i = 0; i < specs.size(); i++) {
        std::string topic, payload;
        if (!partition(specs[i], "=", topic, payload) || topic.empty()) {
            err = "bad --mqtt-publish " + py_repr(specs[i]) + " (want TOPIC=PAYLOAD, e.g. cmnd/obk/POWER=ON)";
            return false;
        }
        out.push_back(std::make_pair(topic, Bytes(payload)));
    }
    return true;
}

/* Ctrl+C: Python's KeyboardInterrupt inside run() prints this and lets
 * run() finish normally (the "Emulation finished" / "Time taken" lines). */
BekenEmulator *volatile g_running = 0;
void on_sigint(int)
{
    BekenEmulator *emu = g_running;
    if (emu) {
        out_write("\nEmulation stopped by user.\n");
        out_flush();
        emu->stop();
    }
}

void cli_log(const std::string &s)
{
    out_write(s + "\n");
    out_flush();
}

int cli_package_smoke()
{
    Memory mem;
    mem.map(0x1000, 0x1000);
    Bytes code;
    for (int i = 0; i < 3; i++)
        code += Bytes("\x00\x00\xa0\xe1", 4);
    mem.write_bytes(0x1000, code);
    ArmCpu cpu(mem);
    cpu.run(0x1000, 0x1000 + (u32)code.size(), 3);
    u32 pc = cpu.reg_read(REG_PC);
    if (pc != 0x1000 + code.size() || !cpu.error().empty()) {
        printf("AssertionError: ARM core smoke test stopped at 0x%08x (%s)\n", pc, cpu.error().c_str());
        return 1;
    }
    static const char *const REQ_FW[] = {
        "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",
        "OpenBK7231T_QIO_1.18.300.bin",
        "OpenBK7231N_QIO_1.18.300.bin",
        "OpenBK7238_QIO_1.18.300.bin",
        "OpenBK7252_QIO_1.18.300.bin",
        "BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin",
        "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin",
        "BK7238_Zengge_ZJ21412_BP101_RGBCCT_Bulb.bin",
        "BK7231T_Tuya_A60_SmartBulb_RGBCWW_5ch_PWM_1000Hz.bin",
        "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin",
        "BK7231T_Tuya_BN-LINK_Plug_WB2S_1.0.3.bin",
        "BK7238_GBDQA_63V_16A.bin",
        "BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin",
        "BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin",
        "BK7231N_Tuya_Generic_Motion_Sensor_CBU_1.0.6.bin",
        "BK7231N_Tuya_Rain_Sensor_CBU_1.1.9.bin",
        "BK7238_Tuya_SRW-TH01_TempHum_T1-AHL_1.0.1.bin",
        "BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin",
        "BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin",
        0
    };
    std::string root;
    if (!ensure_bundled_firmwares(root)) {
        std::string d = path_dirname(self_exe_path());
        for (int i = 0; i < 5 && !d.empty(); i++) {
            if (is_dir(path_join(d, "firmwares"))) {
                root = d;
                break;
            }
            std::string up = path_dirname(d);
            if (up == d)
                break;
            d = up;
        }
        if (root.empty() && is_dir(path_join(current_dir(), "firmwares")))
            root = current_dir();
    }
    std::string fw_dir = path_join(root, "firmwares");
    StrList missing;
    int count = 0;
    for (int i = 0; REQ_FW[i]; i++) {
        count++;
        if (!file_exists(path_join(fw_dir, REQ_FW[i])))
            missing.push_back(REQ_FW[i]);
    }
    if (!missing.empty()) {
        printf("AssertionError: release firmware is missing: %s\n", join(missing, ", ").c_str());
        return 1;
    }
    printf("Packaged runtime smoke test passed: ARM core and %d firmware images.\n", count);
    return 0;
}

int cli_selftest()
{
    if (gpio_selftests() < 0 || periph::periph_selftests() < 0 || tuyamcu::tuyamcu_selftests() < 0
        || blecore_selftests() < 0 || wpa2::wpa2_selftests() < 0 || wifimac_selftests() < 0
        || wifipeer_selftests() < 0 || netstack::netstack_selftests() < 0
        || tuya_config::tuya_config_selftests() < 0)
        return 1;
    return cli_package_smoke();
}

} /* namespace */

int cli_main(int argc, char **argv)
{
    net_init();
    if (argc == 2 && std::string(argv[1]) == "--package-smoke")
        return cli_package_smoke();
    if (argc == 2 && std::string(argv[1]) == "--selftest")
        return cli_selftest();
    build_parser();
    {
        std::string a0 = argc > 0 ? argv[0] : "bekensim";
        size_t cut = a0.find_last_of("/\\");
        g_prog = cut == std::string::npos ? a0 : a0.substr(cut + 1);
#ifdef _WIN32
        if (g_prog.size() >= 2 && g_prog[1] == ':' && cut == std::string::npos)
            g_prog = g_prog.substr(2);
#endif
    }

    StrList argl;
    for (int i = 1; i < argc; i++)
        argl.push_back(argv[i]);
    Args ns;
    int rc = parse_args(argl, ns);
    if (rc >= 0)
        return rc;
    if (!ns.given[A_CHIP])
        ns.str[A_CHIP] = "BK7231";
    if (!ns.given[A_WIFI_MAC])
        ns.str[A_WIFI_MAC] = "02:00:00:00:00:01";
    if (!ns.given[A_WIFI_CHANNEL])
        ns.num[A_WIFI_CHANNEL] = 1;
    const std::string &dump = ns.str[A_DUMP];

    BuildOptions opt;
    if (ns.given[A_UART1_RX]) {
        std::string et, em;
        if (!uart1_rx_bytes(ns.str[A_UART1_RX], opt.uart1_rx, et, em)) /* + the note 3.12+ adds */
            return uncaught(et, et == "UnicodeDecodeError" ? em + "\ndecoding with 'unicode_escape' codec failed" : em);
    }

    /* Any network option means a peer, and a peer means the MAC model. */
    bool want_peer = ns.given[A_WIFI_PEER] || !ns.list[A_NET_FORWARD].empty() || !ns.str[A_PCAP].empty()
                     || !ns.str[A_WIFI_SSID].empty() || !ns.list[A_NET_HTTP_GET].empty()
                     || !ns.list[A_MQTT_PUBLISH].empty() || !ns.str[A_WIFI_PASS].empty() || ns.flag[A_NET_OUT]
                     || !ns.str[A_PCAP_IP].empty();

    /* build_emulator's keyword arguments, evaluated left to right */
    std::string err;
    KeyCoefs key;
    if (ns.given[A_KEY] && !parse_key(ns.str[A_KEY], key, &err))
        return value_error(err);
    if (!parse_dp_specs(ns.list[A_TUYAMCU_DP], opt.tuyamcu_dps, err)
        || !parse_hex_blobs(ns.list[A_TUYAMCU_INJECT], opt.tuyamcu_injects, err)
        || !parse_deep_sleep_wakes(ns.list[A_DEEP_SLEEP_WAKE], opt.deep_sleep_wakes, err)
        || !parse_gpio_inputs(ns.list[A_GPIO_INPUT], opt.gpio_inputs, err)
        || !periph::parse_adc_specs(ns.list[A_ADC], ns.str[A_CHIP], opt.adc_inputs, &err))
        return value_error(err);

    opt.chip = ns.str[A_CHIP];
    opt.with_boot = ns.flag[A_WITH_BOOT];
    opt.only_uart = ns.flag[A_ONLY_UART];
    opt.uart1_hex = ns.flag[A_UART1_HEX];
    if (ns.given[A_UART1_RX_DELAY]) {
        /* a negative hold-off is simply already over */
        opt.uart1_rx_delay = ns.num[A_UART1_RX_DELAY] < 0 ? 0 : (u64)ns.num[A_UART1_RX_DELAY];
        opt.uart1_rx_delay_set = true;
    }
    opt.tuyamcu = ns.flag[A_TUYAMCU];
    opt.tuyamcu_pid = ns.str[A_TUYAMCU_PID];
    opt.tuyamcu_raw = ns.flag[A_TUYAMCU_RAW];
    opt.xvr_selfclear = ns.flag[A_XVR_SELFCLEAR];
    opt.ble_core = ns.flag[A_BLE_CORE] ? 1 : -1;
    opt.wifi = ns.flag[A_WIFI] || want_peer;

    void (*logf)(const std::string &) = opt.only_uart ? 0 : cli_log;
    std::string etype;
    BekenEmulator *emu = build_emulator(dump, opt, ns.given[A_KEY] ? &key : 0, &err, logf, true, &etype);
    if (!emu)
        return etype == "ValueError" ? value_error(err) : uncaught(etype, err);

    struct Cleanup {
        BekenEmulator *e;
        ~Cleanup()
        {
            g_running = 0;
            if (e) {
                e->close_network();
                delete e;
            }
        }
    } cleanup = {emu};

    if (want_peer) {
        /* The offline TLS peer needs the image's PSK to authenticate its
         * local broker session; --net-out is an opaque relay, so skip it. */
        std::vector<Bytes> tls_psks;
        bool tuya_legacy = false;
        if (!ns.flag[A_NET_OUT]) {
            tuya_config::Decoded cfg = tuya_config::extract(dump);
            tls_psks = tuya_config::tls_psk_candidates(cfg);
            tuya_legacy = tuya_config::uses_tuya_legacy_identity_psk(cfg);
        }
        Bytes mac;
        std::vector<std::pair<int, int> > forwards;
        std::vector<std::pair<std::string, Bytes> > mqtt_pubs;
        if (!parse_mac(ns.str[A_WIFI_MAC], mac, err))
            return value_error(err);
        if (ns.given[A_WIFI_PASS]) { /* check_wifi_pass */
            size_t len = py_len(ns.str[A_WIFI_PASS]);
            if (len < 8 || len > 63)
                return value_error(strf("bad --wifi-pass (a WPA2 passphrase is 8 to 63 characters; this one "
                                        "is %d)", (int)len));
        }
        if (!parse_forwards(ns.list[A_NET_FORWARD], forwards, err) || !parse_http_gets(ns.list[A_NET_HTTP_GET], err)
            || !parse_mqtt_pubs(ns.list[A_MQTT_PUBLISH], mqtt_pubs, err))
            return value_error(err);
        long long ch = ns.num[A_WIFI_CHANNEL];
        emu->attach_wifi_peer(ns.given[A_WIFI_PEER] ? ns.str[A_WIFI_PEER] : "auto", ns.str[A_WIFI_SSID],
                              (int)std::max(-0x7FFFFFFFLL, std::min(ch, 0x7FFFFFFFLL)), mac, ns.str[A_WIFI_PASS],
                              forwards, ns.flag[A_NET_OUT], ns.str[A_PCAP], ns.list[A_NET_HTTP_GET], mqtt_pubs,
                              tls_psks, tuya_legacy);
        if (ns.given[A_PCAP]) {
            /* the peer opens --pcap itself (Python: open(pcap, "wb") in
             * WifiPeer, an OSError main() does not catch) */
            int e = 0;
            FILE *f = py_open(ns.str[A_PCAP], "ab", &e);
            if (f)
                fclose(f);
            else {
                std::string type, msg = py_oserror(e, ns.str[A_PCAP], &type);
                return uncaught(type, msg);
            }
        }
    }
    if (!ns.str[A_PCAP_IP].empty()) {
        int e = 0;
        FILE *f = py_open(ns.str[A_PCAP_IP], "wb", &e);
        if (!f) {
            std::string type, msg = py_oserror(e, ns.str[A_PCAP_IP], &type);
            return uncaught(type, msg);
        }
        fclose(f);
        emu->set_pcap_ip(ns.str[A_PCAP_IP]);
    }
    if (!ns.str[A_SNAPSHOT_SAVE].empty()) {
        const std::string &at = ns.str[A_SNAPSHOT_AT];
        if (at.empty())
            return value_error("--snapshot-save needs --snapshot-at (an instruction count or a UART marker)");
        bool is_num = is_digits(at);
        long long atn = 0;
        bool huge = false;
        if (is_num)
            py_int(at, 10, atn, &huge);
        emu->arm_snapshot(ns.str[A_SNAPSHOT_SAVE], is_num, huge ? ~0ull : (u64)atn, at);
    }

    if (emu->flash_empty()) {
        /* Python: emu.setup() -> mu.mem_map(SPI_FLASH_BASE, 0) raises */
        delete emu;
        return uncaught("unicorn.unicorn_py3.unicorn.UcError", "Invalid argument (UC_ERR_ARG)");
    }
    emu->setup();
    for (size_t i = 0; i < ns.trace_pcs.size(); i++)
        emu->trace_pc(ns.trace_pcs[i]);
    if (!ns.str[A_SNAPSHOT_LOAD].empty()) {
        const std::string &path = ns.str[A_SNAPSHOT_LOAD];
        int e = 0;
        FILE *f = py_open(path, "rb", &e);
        if (!f)
            return value_error(py_oserror(e, path)); /* except (ValueError, OSError) */
        fclose(f);
        std::string e2;
        if (!emu->load_snapshot(path, &e2))
            return value_error(e2);
    }
    g_running = emu;
    signal(SIGINT, on_sigint);
    emu->run();
    signal(SIGINT, SIG_DFL);
    g_running = 0;
    return 0;
}
