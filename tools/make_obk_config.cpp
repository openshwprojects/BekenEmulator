/* Inject an OpenBeken config (startup command, flags, SSID, MQTT) into a
 * BK7231 flash image. Port of tools/make_obk_config.py.
 *
 * The command line is parsed the way Python's argparse parses the original
 * (same options, positionals anywhere, --opt=value, -cVALUE, unique-prefix
 * abbreviations of long options, "--" ends options, an option value may not
 * look like another option, errors exit 2), and the image is built with the
 * same validation and the same Python slice-assignment semantics, so every
 * accepted command line writes a byte-identical image. */
#include "common.h"
#include "beken/beken_crypto.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

static const long long CONFIG_ADDR = 0x1E1000;   /* BK7231T/U family */
static const long long CONFIG_ADDR_N = 0x1D1000; /* BK7231N/M family */
static const u32 CONFIG_SIZE = 3584;             /* sizeof(mainConfig_t) */
static const int CFG_VERSION = 5;
static const u32 CMDLINE_OFF = 0x5E0;
static const u32 FLAGS_OFF = 0x08;
static const u32 SSID_OFF = 0x14;
static const u32 PASS_OFF = 0x54;
static const u32 CRED_MAX = 64;
static const u32 MQTT_HOST_OFF = 0x94;
static const u32 MQTT_HOST_MAX = 256;
static const u32 MQTT_PORT_OFF = 0x294;
static const u32 MQTT_CLIENT_OFF = 0x194;
static const u32 MQTT_CLIENT_MAX = 64;
static const u32 CMDLINE_MAX = 1568;
static const long long SECTOR_SIZE = 4096;

static const char *PROG = "make_obk_config";

/* OBK's Tiny_CRC8 as compiled with signed char (arithmetic >>). */
static u8 tiny_crc8_signed(const Bytes &data)
{
    int crc = 0;
    for (size_t i = 0; i < data.size(); i++) {
        int extract = (u8)data[i];
        if (extract >= 128)
            extract -= 256;
        for (int k = 0; k < 8; k++) {
            int total = (crc ^ extract) & 0x01;
            crc >>= 1; /* arithmetic shift on a signed value */
            if (total) {
                crc = (crc ^ 0x8C) & 0xFF;
                if (crc >= 128)
                    crc -= 256;
            }
            extract >>= 1;
        }
        crc &= 0xFF;
        if (crc >= 128)
            crc -= 256;
    }
    return (u8)(crc & 0xFF);
}

/* build_config: the Python function, raising (returning false + *err) in the
 * same order and with the same messages. */
static bool build_config(const std::string &startup_command, const std::vector<long long> &flags,
                         const std::string &ssid, const std::string &password,
                         const std::string &mqtt_host, long long mqtt_port, const std::string &mqtt_client,
                         Bytes &cfg, std::string *err)
{
    for (size_t i = 0; i < startup_command.size(); i++)
        if ((u8)startup_command[i] >= 0x80) {
            *err = strf("UnicodeEncodeError: 'ascii' codec can't encode the startup command "
                        "(non-ASCII byte at position %d)", (int)i);
            return false;
        }
    if (startup_command.size() >= CMDLINE_MAX) {
        *err = strf("ValueError: startup command too long (max %u bytes)", CMDLINE_MAX - 1);
        return false;
    }
    if (ssid.size() >= CRED_MAX || password.size() >= CRED_MAX) {
        *err = strf("ValueError: SSID/password too long (max %u bytes)", CRED_MAX - 1);
        return false;
    }
    if (mqtt_client.size() >= MQTT_CLIENT_MAX) {
        *err = strf("ValueError: MQTT client id too long (max %u bytes)", MQTT_CLIENT_MAX - 1);
        return false;
    }
    if (mqtt_host.size() >= MQTT_HOST_MAX) {
        *err = strf("ValueError: MQTT host too long (max %u bytes)", MQTT_HOST_MAX - 1);
        return false;
    }
    u32 generic0 = 0, generic1 = 0;
    for (size_t i = 0; i < flags.size(); i++) {
        long long f = flags[i];
        if (f < 0 || f > 63) {
            *err = strf("ValueError: flag out of range 0..63: %lld", f);
            return false;
        }
        if (f < 32)
            generic0 |= 1u << f;
        else
            generic1 |= 1u << (f - 32);
    }
    cfg.assign(CONFIG_SIZE, '\0');
    cfg[0] = 'C';
    cfg[1] = 'F';
    cfg[2] = 'G';
    wr32le(&cfg[4], (u32)CFG_VERSION);
    wr32le(&cfg[FLAGS_OFF], generic0);
    wr32le(&cfg[FLAGS_OFF + 4], generic1);
    cfg.replace(SSID_OFF, ssid.size(), ssid);
    cfg.replace(PASS_OFF, password.size(), password);
    if (!mqtt_host.empty()) {
        cfg.replace(MQTT_HOST_OFF, mqtt_host.size(), mqtt_host);
        /* struct.pack("<i", mqtt_port) */
        if (mqtt_port < -2147483647LL - 1 || mqtt_port > 2147483647LL) {
            *err = "struct.error: 'i' format requires -2147483648 <= number <= 2147483647";
            return false;
        }
        wr32le(&cfg[MQTT_PORT_OFF], (u32)(long)mqtt_port);
    }
    if (!mqtt_client.empty())
        cfg.replace(MQTT_CLIENT_OFF, mqtt_client.size(), mqtt_client);
    cfg.replace(CMDLINE_OFF, startup_command.size(), startup_command);
    cfg[3] = (char)tiny_crc8_signed(bslice(cfg, 4, CONFIG_SIZE));
    return true;
}

/* inject: pad to a whole sector past the config, then out[addr:end] = cfg
 * with Python's slice-assignment semantics (negative indices count from the
 * end, indices clamp, an empty slice inserts). */
static Bytes inject(const Bytes &raw, long long addr, const Bytes &cfg)
{
    Bytes out = raw;
    long long end = addr + (long long)CONFIG_SIZE;
    if ((long long)out.size() < end) {
        long long want = ((end + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
        out += Bytes((size_t)(want - (long long)out.size()), (char)0xFF);
    }
    long long n = (long long)out.size();
    long long s = addr, e = end;
    if (s < 0) {
        s += n;
        if (s < 0)
            s = 0;
    } else if (s > n)
        s = n;
    if (e < 0) {
        e += n;
        if (e < 0)
            e = 0;
    } else if (e > n)
        e = n;
    if (e < s)
        e = s;
    out.replace((size_t)s, (size_t)(e - s), cfg);
    return out;
}

/* ------------------------------------------------------------ argparse port */
struct OptDef {
    const char *name;  /* option string */
    int id;
    bool takes_value;
};
enum { OPT_HELP, OPT_COMMAND, OPT_SSID, OPT_MQTT_HOST, OPT_MQTT_CLIENT, OPT_MQTT_PORT, OPT_PASS, OPT_ADDR, OPT_FLAG };
static const OptDef OPTS[] = {
    {"-h", OPT_HELP, false},
    {"--help", OPT_HELP, false},
    {"-c", OPT_COMMAND, true},
    {"--command", OPT_COMMAND, true},
    {"--ssid", OPT_SSID, true},
    {"--mqtt-host", OPT_MQTT_HOST, true},
    {"--mqtt-client", OPT_MQTT_CLIENT, true},
    {"--mqtt-port", OPT_MQTT_PORT, true},
    {"--pass", OPT_PASS, true},
    {"--addr", OPT_ADDR, true},
    {"--flag", OPT_FLAG, true},
};
static const int NOPTS = (int)(sizeof(OPTS) / sizeof(OPTS[0]));

static const char *opt_display(int id)
{
    switch (id) {
    case OPT_HELP: return "-h/--help";
    case OPT_COMMAND: return "-c/--command";
    case OPT_SSID: return "--ssid";
    case OPT_MQTT_HOST: return "--mqtt-host";
    case OPT_MQTT_CLIENT: return "--mqtt-client";
    case OPT_MQTT_PORT: return "--mqtt-port";
    case OPT_PASS: return "--pass";
    case OPT_ADDR: return "--addr";
    default: return "--flag";
    }
}

static const char *USAGE =
    "usage: make_obk_config [-h] [-c COMMAND] [--ssid SSID] [--mqtt-host MQTT_HOST]\n"
    "                       [--mqtt-client MQTT_CLIENT] [--mqtt-port MQTT_PORT]\n"
    "                       [--pass PASSWORD] [--addr ADDR] [--flag N]\n"
    "                       input output\n";

static void print_help()
{
    printf("%s", USAGE);
    printf("\n"
           "Inject an OpenBeken config (with a startup command) into a BK7231 flash image.\n"
           "\n"
           "OBK keeps its config in the BK_PARTITION_NET_PARAM partition at flash 0x1e1000,\n"
           "as a 3584-byte mainConfig_t:\n"
           "\n"
           "    offset 0..2   ident 'C','F','G'\n"
           "    offset 3      crc      - Tiny_CRC8 over config[4:3584]\n"
           "    offset 4      version  - int, MAIN_CFG_VERSION (5)\n"
           "    offset 0x5E0  initCommandLine[1568] - the startup command\n"
           "\n"
           "CRC note: OBK's Tiny_CRC8 (src/tiny_crc8.c) is written with plain `char`, and\n"
           "this firmware is built with *signed* char, so `crc >>= 1` is an arithmetic\n"
           "shift. Computing it as unsigned yields a different byte and the config is\n"
           "rejected with \"Config crc or ident mismatch\", so the signed form is required.\n"
           "\n"
           "positional arguments:\n"
           "  input                 Base BK7231 flash image\n"
           "  output                Image to write with the config injected\n"
           "\n"
           "options:\n"
           "  -h, --help            show this help message and exit\n"
           "  -c, --command COMMAND\n"
           "                        Startup command placed in initCommandLine, e.g.\n"
           "                        \"uartInit 9600; uartSendHex 55AA00000000FF\"\n"
           "  --ssid SSID           Wi-Fi SSID for OBK to join as a station (wifi_ssid).\n"
           "                        Without it OBK starts its own open access point.\n"
           "  --mqtt-host MQTT_HOST\n"
           "                        MQTT broker host OBK connects to once it is on the\n"
           "                        network.\n"
           "  --mqtt-client MQTT_CLIENT\n"
           "                        MQTT client id (mqtt_clientId). OBK publishes under\n"
           "                        <client id>/...\n"
           "  --mqtt-port MQTT_PORT\n"
           "                        MQTT broker port (default 1883).\n"
           "  --pass PASSWORD       Wi-Fi password for --ssid; omit or \"\" for an open\n"
           "                        network.\n"
           "  --addr ADDR           Physical config address. Default: 0x1E1000, or\n"
           "                        0x1D1000 when the filename looks like a BK7231N/M\n"
           "                        image.\n"
           "  --flag N              Set OBK config flag bit N (0..63). Repeatable. E.g.\n"
           "                        --flag 31 enables the UART1 command console.\n");
}

static int arg_error(const std::string &msg)
{
    err_write(USAGE);
    err_write(strf("%s: error: %s\n", PROG, msg.c_str()));
    return 2;
}

/* argparse's _negative_number_matcher: '-' then an optional '.' then a digit. */
static bool looks_negative_number(const std::string &a)
{
    size_t i = 1;
    if (a.size() > i && a[i] == '.')
        i++;
    return a.size() > i && a[i] >= '0' && a[i] <= '9';
}

/* One classified command-line word (argparse's _parse_optional). */
struct Classified {
    bool is_option;     /* 'O' */
    int opt;            /* option id, -1 = unknown option (an "extra") */
    bool has_explicit;  /* value glued on with '=' or after a short option */
    std::string explicit_arg;
    bool short_glued;   /* "-cVALUE" form (relevant for -h combining) */
};

static bool classify(const std::string &a, Classified &c, std::string *ambiguous)
{
    c.is_option = false;
    c.opt = -1;
    c.has_explicit = false;
    c.explicit_arg.clear();
    c.short_glued = false;
    if (a.empty() || a[0] != '-')
        return true;
    for (int i = 0; i < NOPTS; i++)
        if (a == OPTS[i].name) {
            c.is_option = true;
            c.opt = OPTS[i].id;
            return true;
        }
    if (a.size() == 1)
        return true;
    size_t eq = a.find('=');
    if (eq != std::string::npos) {
        std::string name = a.substr(0, eq);
        for (int i = 0; i < NOPTS; i++)
            if (name == OPTS[i].name) {
                c.is_option = true;
                c.opt = OPTS[i].id;
                c.has_explicit = true;
                c.explicit_arg = a.substr(eq + 1);
                return true;
            }
    }
    /* _get_option_tuples: prefix matches */
    std::vector<int> hits;
    std::vector<std::string> hit_names;
    std::vector<std::string> hit_args;
    std::vector<bool> hit_has, hit_glued;
    if (a.size() >= 2 && a[1] == '-') {
        std::string prefix = a, ex;
        bool has = false;
        if (eq != std::string::npos) {
            prefix = a.substr(0, eq);
            ex = a.substr(eq + 1);
            has = true;
        }
        for (int i = 0; i < NOPTS; i++)
            if (std::string(OPTS[i].name).compare(0, prefix.size(), prefix) == 0) {
                hits.push_back(OPTS[i].id);
                hit_names.push_back(OPTS[i].name);
                hit_args.push_back(ex);
                hit_has.push_back(has);
                hit_glued.push_back(false);
            }
    } else {
        std::string short_prefix = a.substr(0, 2), short_ex = a.substr(2);
        for (int i = 0; i < NOPTS; i++) {
            std::string name = OPTS[i].name;
            if (name == short_prefix) {
                hits.push_back(OPTS[i].id);
                hit_names.push_back(name);
                hit_args.push_back(short_ex);
                hit_has.push_back(true);
                hit_glued.push_back(true);
            } else if (name.compare(0, a.size(), a) == 0) {
                hits.push_back(OPTS[i].id);
                hit_names.push_back(name);
                hit_args.push_back("");
                hit_has.push_back(false);
                hit_glued.push_back(false);
            }
        }
    }
    if (hits.size() > 1) {
        std::string names;
        for (size_t i = 0; i < hit_names.size(); i++)
            names += (i ? ", " : "") + hit_names[i];
        std::string prefix = eq != std::string::npos ? a.substr(0, eq) : a;
        *ambiguous = strf("ambiguous option: %s could match %s", prefix.c_str(), names.c_str());
        return false;
    }
    if (hits.size() == 1) {
        c.is_option = true;
        c.opt = hits[0];
        c.has_explicit = hit_has[0];
        c.explicit_arg = hit_args[0];
        c.short_glued = hit_glued[0];
        return true;
    }
    if (looks_negative_number(a))
        return true; /* no option looks like a negative number */
    if (a.find(' ') != std::string::npos)
        return true;
    c.is_option = true; /* an unknown option: an "extra" */
    return true;
}

struct Args {
    StrList positionals;
    std::string command, ssid, password, mqtt_host, mqtt_client, addr;
    bool have_ssid, have_pass, have_mqtt_host, have_mqtt_client, have_addr;
    long long mqtt_port;
    std::vector<long long> flags;
    Args()
        : have_ssid(false), have_pass(false), have_mqtt_host(false), have_mqtt_client(false), have_addr(false),
          mqtt_port(1883)
    {
    }
};

/* Python int(str) for type=int (base 10, whitespace, '+'/'-', underscores). */
static bool py_int10(const std::string &s, long long &out)
{
    return parse_int(s, 10, out);
}

/* Returns -1 to continue, otherwise the process exit code. */
static int parse_args(const StrList &argv, Args &args)
{
    /* Classify everything first: argparse reports an ambiguous prefix before
     * any other error. */
    std::vector<Classified> cls(argv.size());
    size_t dashdash = argv.size();
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i] == "--") {
            dashdash = i;
            break;
        }
        std::string amb;
        if (!classify(argv[i], cls[i], &amb))
            return arg_error(amb);
    }
    StrList extras;
    bool help = false;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i == dashdash) {
            for (size_t j = i + 1; j < argv.size(); j++)
                args.positionals.push_back(argv[j]);
            break;
        }
        const Classified &c = cls[i];
        if (!c.is_option) {
            args.positionals.push_back(argv[i]);
            continue;
        }
        if (c.opt < 0) {
            extras.push_back(argv[i]);
            continue;
        }
        int opt = c.opt;
        if (opt == OPT_HELP) {
            if (c.has_explicit) {
                if (!c.short_glued)
                    return arg_error(strf("argument %s: ignored explicit argument '%s'", opt_display(opt),
                                          c.explicit_arg.c_str()));
                /* "-hX..." combines short flags: -h then -X... */
                std::string rest = c.explicit_arg;
                bool next_is_value = i + 1 < argv.size() && i + 1 != dashdash && !cls[i + 1].is_option;
                if (!rest.empty() && rest[0] == 'c' && rest.size() == 1 && !next_is_value)
                    return arg_error("argument -c/--command: expected one argument");
                if (!rest.empty() && (rest[0] == 'c' || rest[0] == 'h')) {
                    /* help runs first in argparse's action order */
                    help = true;
                    break;
                }
                return arg_error(strf("argument %s: ignored explicit argument '%s'", opt_display(opt), rest.c_str()));
            }
            help = true;
            break; /* the help action exits at once */
        }
        std::string value;
        if (c.has_explicit) {
            value = c.explicit_arg;
        } else {
            if (i + 1 >= argv.size() || i + 1 == dashdash || cls[i + 1].is_option)
                return arg_error(strf("argument %s: expected one argument", opt_display(opt)));
            value = argv[++i];
        }
        switch (opt) {
        case OPT_COMMAND: args.command = value; break;
        case OPT_SSID: args.ssid = value; args.have_ssid = true; break;
        case OPT_PASS: args.password = value; args.have_pass = true; break;
        case OPT_MQTT_HOST: args.mqtt_host = value; args.have_mqtt_host = true; break;
        case OPT_MQTT_CLIENT: args.mqtt_client = value; args.have_mqtt_client = true; break;
        case OPT_ADDR: args.addr = value; args.have_addr = true; break;
        case OPT_MQTT_PORT:
        case OPT_FLAG: {
            long long n;
            if (!py_int10(value, n))
                return arg_error(strf("argument %s: invalid int value: '%s'", opt_display(opt), value.c_str()));
            if (opt == OPT_MQTT_PORT)
                args.mqtt_port = n;
            else
                args.flags.push_back(n);
            break;
        }
        }
    }
    if (help) {
        print_help();
        return 0;
    }
    if (args.positionals.size() < 2) {
        return arg_error(args.positionals.empty() ? "the following arguments are required: input, output"
                                                  : "the following arguments are required: output");
    }
    for (size_t i = 2; i < args.positionals.size(); i++)
        extras.push_back(args.positionals[i]);
    if (!extras.empty()) {
        std::string s;
        for (size_t i = 0; i < extras.size(); i++)
            s += (i ? " " : "") + extras[i];
        return arg_error("unrecognized arguments: " + s);
    }
    return -1;
}

static std::string py_hex06(long long v)
{
    /* Python "%06X" % v: the width includes a minus sign. */
    if (v < 0)
        return strf("-%05llX", (unsigned long long)(-v));
    return strf("%06llX", (unsigned long long)v);
}

int make_obk_config_main(const StrList &argv)
{
    Args args;
    int rc = parse_args(argv, args);
    if (rc >= 0)
        return rc;
    const std::string &input = args.positionals[0], &output = args.positionals[1];

    Bytes raw;
    if (!read_file(input, raw)) {
        err_write(strf("FileNotFoundError: cannot read %s\n", input.c_str()));
        return 1;
    }
    long long addr;
    if (!args.addr.empty()) { /* "if args.addr:" - an empty --addr means the default */
        if (!parse_int(args.addr, 0, addr)) {
            err_write(strf("ValueError: invalid literal for int() with base 0: '%s'\n", args.addr.c_str()));
            return 1;
        }
    } else {
        std::string name = upper(path_basename(input));
        addr = (contains(name, "7231N") || contains(name, "7231M")) ? CONFIG_ADDR_N : CONFIG_ADDR;
    }

    std::string err;
    Bytes cfg;
    if (!build_config(args.command, args.flags, args.ssid, args.password, args.mqtt_host, args.mqtt_port,
                      args.mqtt_client, cfg, &err)) {
        err_write(err + "\n");
        return 1;
    }
    Bytes out = inject(raw, addr, cfg);
    if (!write_file(output, out)) {
        err_write(strf("OSError: cannot write %s\n", output.c_str()));
        return 1;
    }

    printf("Wrote %s (%llu bytes, was %llu)\n", output.c_str(), (unsigned long long)out.size(),
           (unsigned long long)raw.size());
    printf("  config addr   : 0x%s (physical)\n", py_hex06(addr).c_str());
    printf("  config crc    : 0x%02x\n", (u8)cfg[3]);
    printf("  startup cmd   : %s\n", args.command.c_str());
    if (!args.flags.empty()) {
        std::string fs;
        for (size_t i = 0; i < args.flags.size(); i++)
            fs += (i ? ", " : "") + strf("%lld", args.flags[i]);
        printf("  flags set     : %s\n", fs.c_str());
    }
    if (args.have_ssid)
        printf("  station ssid  : %s (%s)\n", args.ssid.c_str(), args.password.empty() ? "open" : "with password");
    if (!args.mqtt_host.empty())
        printf("  mqtt broker   : %s:%lld\n", args.mqtt_host.c_str(), args.mqtt_port);
    if (!args.mqtt_client.empty())
        printf("  mqtt client id: %s\n", args.mqtt_client.c_str());
    return 0;
}

/* The arguments as UTF-8 (Python reads the wide command line on Windows, so a
 * non-ASCII SSID/password is stored as its UTF-8 bytes either way). */
static StrList utf8_args(int argc, char **argv)
{
    StrList out;
#ifdef _WIN32
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 1; i < n; i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, 0, 0, 0, 0);
            std::string s(len > 0 ? (size_t)len : 1, '\0');
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, 0, 0);
            s.resize(len > 0 ? (size_t)len - 1 : 0);
            out.push_back(s);
        }
        LocalFree(w);
        return out;
    }
#endif
    for (int i = 1; i < argc; i++)
        out.push_back(argv[i]);
    return out;
}

int main(int argc, char **argv) { return make_obk_config_main(utf8_args(argc, argv)); }
