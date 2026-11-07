"""Tkinter front-end for the BK7231 emulator: two UART consoles you can type into.

Run it with no arguments (or point it straight at a dump):

    python tools/sim_ui.py
    python tools/sim_ui.py firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin

Pick any Beken flash dump, press Run, and watch UART1 and UART2 in their own
panes, with a third column decoding the GPIO and PWM registers as the firmware
configures them. Anything typed in the send bar goes into UART1's receive FIFO
through BekenEmulator.feed_uart1(), i.e. the same RX interrupt path --uart1-rx
and the simulated TuyaMCU peer use - the firmware cannot tell the difference.

Two things are worth knowing before the first run:

  * Typing only DOES anything if the firmware is listening on UART1. For
    OpenBeken that means config flag 31 (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS),
    which turns UART1 into a command console at boot; the bundled
    ..._obkStartupCommand_uartConsole.bin image has it set, and
    tools/make_obk_config.py --flag 31 sets it on any other image. Without it
    the bytes are delivered and the firmware simply drops them.
  * OpenBeken does not echo, and command OUTPUT goes to the UART2 debug log.
    So you type into UART1 and read the result in UART2. That is how the real
    hardware behaves; the UI echoes your own line locally so the pane still
    reads like a session.

Threading: Tk owns the main thread and every widget. The emulation runs on a
worker thread and only ever touches a lock-guarded output buffer, so no Tk call
is ever made from it. The UI polls that buffer on a timer.
"""
import json
import os
import sys
import re
import struct
import tempfile
import threading
import time
import traceback
import webbrowser

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

# PyInstaller flattens the entry script into its bundle root.  In a frozen
# build ``dirname(__file__)/..`` would therefore escape the bundle and make the
# built-in firmware buttons point at files that do not exist.  _MEIPASS is the
# root holding data collected with --add-data in both one-file and one-dir
# builds; source runs keep the repository-root calculation.
if getattr(sys, "frozen", False):
    ROOT_DIR = os.path.abspath(getattr(sys, "_MEIPASS", os.path.dirname(sys.executable)))
else:
    ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT_DIR)

from src import netstack, tuya_config
from src.crypto import KNOWN_KEYS, parse_key
from src.chips import CHIPS, chip_names
from src.emulator import SLOW_TICK_INSNS, SLOW_TICK_MS
from src.main import (WIFI_ROLES, build_emulator, check_tuyamcu_pid, check_wifi_pass,
                      parse_forwards, parse_hex_blobs, parse_http_gets, parse_mqtt_pubs)
# One decoder for the GPIO/PWM registers, shared with the self-test report -
# so the pane below and the report's tables can never disagree.
from src import periph

FIRMWARE_DIR = os.path.join(ROOT_DIR, "firmwares")
# Image shipped with OBK config flag 31 already set, so its UART1 is a command
# console the moment it boots - the one dump where typing works out of the box.
DEFAULT_IMAGE = os.path.join(
    FIRMWARE_DIR, "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin")

# Useful end-to-end runs, kept close to DEFAULT_IMAGE so this is also the one
# inventory a developer updates when a bundled dump is renamed.  These are
# deliberately profiles rather than mere paths: several families share an ID,
# encrypted OpenBeken images need their known key, and the paired cloud examples
# are pointless without the Wi-Fi peer.  A paired profile deliberately enables
# the real outbound relay: if that exact device identity is still bound to the
# user's Tuya account, the app can see it come online.  No credential is stored
# here -- each paired image's provisioning data comes from its own flash.
INTERESTING_DUMPS = (
    {"label": "OBK T", "file": "OpenBK7231T_QIO_1.18.300.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True},
    {"label": "OBK N", "file": "OpenBK7231N_QIO_1.18.300.bin",
     "chip": "BK7231N", "key": "TUYA", "wifi": True},
    {"label": "OBK 7238", "file": "OpenBK7238_QIO_1.18.300.bin",
     "chip": "BK7238", "key": "(none)", "wifi": True},
    {"label": "OBK 7252", "file": "OpenBK7252_QIO_1.18.300.bin",
     "chip": "BK7252", "key": "(none)", "wifi": True},
    {"label": "WB3S relay", "file": "BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True},
    {"label": "MOES relay", "file": "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin",
     "chip": "BK7231", "key": "(none)", "wifi": True},
    {"label": "BP5758D bulb", "file": "BK7238_Zengge_ZJ21412_BP101_RGBCCT_Bulb.bin",
     "chip": "BK7238", "key": "(none)", "wifi": True},
    {"label": "PWM bulb", "file": "BK7231T_Tuya_A60_SmartBulb_RGBCWW_5ch_PWM_1000Hz.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True},
    {"label": "BL0942 meter", "file": "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True},
    {"label": "BL0937 plug", "file": "BK7231T_Tuya_BN-LINK_Plug_WB2S_1.0.3.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True},
    {"label": "OBK sleep",
     "file": "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",
     "chip": "BK7231", "key": "TUYA", "wifi": True,
     "uart1_rx": b"DeepSleep 10\r\n"},
    {"label": "PIR sleep",
     "file": "BK7231N_Tuya_Generic_Motion_Sensor_CBU_1.0.6.bin",
     "chip": "BK7231N", "key": "TUYA", "wifi": True},
    {"label": "Rain sleep",
     "file": "BK7231N_Tuya_Rain_Sensor_CBU_1.1.9.bin",
     "chip": "BK7231N", "key": "TUYA", "wifi": True},
    {"label": "TH01 sleep",
     "file": "BK7238_Tuya_SRW-TH01_TempHum_T1-AHL_1.0.1.bin",
     "chip": "BK7238", "key": "(none)", "wifi": True},
    {"label": "HA15 cam",
     "file": "BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin",
     "chip": "BK7252", "key": "(none)", "wifi": True},
    {"label": "Pinmei cam",
     "file": "BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin",
     "chip": "BK7252N", "key": "(none)", "wifi": True,
     "uart1_rx": b"test_W291M\n"},
    {"label": "GBDQA breaker", "file": "BK7238_GBDQA_63V_16A.bin",
     "chip": "BK7238", "key": "(none)", "wifi": True, "paired": True,
     "net_out": True},
    {"label": "IR blaster",
     "file": "BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin",
     "chip": "BK7238", "key": "(none)", "wifi": True, "paired": True,
     "net_out": True},
    {"label": "LSC bulb",
     "file": "BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin",
     "chip": "BK7231N", "key": "TUYA", "wifi": True, "paired": True,
     "net_out": True},
)

POLL_MS = 60            # how often the UI drains the emulator's output buffer
MAX_LINES = 4000        # per pane; older lines are dropped so a long run is bounded
FRAME_PRIVACY_NOTE = ("Raw frames and saved PCAPs can contain identities or credentials "
                      "that firmware sends in clear.")

# Device time per emulated second, from the emulator's own tick model rather
# than a copy of its numbers. Used only to label the status bar - it is an
# estimate, like insn_count itself.
GUEST_SECOND_INSNS = SLOW_TICK_INSNS * (1000 // SLOW_TICK_MS)

LINE_ENDINGS = {"CR+LF": b"\r\n", "LF": b"\n", "CR": b"\r", "(none)": b""}

# One-click OpenBeken console commands. Sent verbatim as ASCII with whatever
# line ending is selected, so each button does exactly what its label says.
# "5*2" is deliberate: OBK's tokenizer evaluates simple arithmetic before the
# handler runs, so channel 1 ends up 10 - handy proof the line was parsed
# rather than echoed.
#
# getChannel is here because toggleChannel is SILENT: CHANNEL_Toggle() writes
# g_channelValues[] directly and calls Channel_OnChanged(), never CHANNEL_Set(),
# so it logs nothing at all - unlike setChannel, which prints "CHANNEL_Set
# channel N has changed to V". Without a read-back button a toggle looks like a
# dead button. getChannel prints "Channel N is V", which shows it worked.
#
# Clicked left to right they chain, each building on the last value:
#   1 -> 10 (5*2) -> 12 ($CH1+2) -> 19 (+7) -> 0 -> 1 (toggle) -> read back
#
# The two setPinRole buttons drive the GPIO pane rather than a channel: they
# reassign pin 1 as a plain output held high or low, so its row flips between
# "OUTPUT = 1" (reg 0x02) and "OUTPUT = 0" (reg 0x00) as you click. Like
# toggleChannel these log NOTHING on success - CMD_SetPinRole only prints on
# "Unknown pin" / "Unknown role" - so the third column is the only place their
# effect is visible, which is the point of pairing them with it.
# Pin 1 is the UART2 RX pad, which costs nothing here - the emulator's UART2 is
# the log's transmit path and does not go through the pin mux - but on real
# silicon this would take the pad away from the debug port's receive side.
QUICK_COMMANDS = [
    "setChannel 1 1",
    "setChannel 1 5*2",
    "setChannel 1 $CH1+2",
    "addChannel 1 7",
    "setChannel 1 0",
    "toggleChannel 1",
    "getChannel 1",
    "setPinRole 1 AlwaysHigh",
    "setPinRole 1 AlwaysLow",
    "DeepSleep 10",
    "backlog setPinRole 10 Btn; PinDeepSleep",
]
QUICK_PER_ROW = 4       # they wrap rather than run off the end of the window

# Firmware log line the "suppress" checkbox filters out of the UART2 pane.
# OpenBeken prints it on every idle tick, which crowds out everything else.
QUICKTICK = "Info:MAIN:quicktick"

# The network pane's heading. It carries a running count, so the user can see
# at a glance whether the bridge captured anything without scrolling.
NET_TITLE = "network - virtual Wi-Fi and IP bridge"
# Frames kept for the details window. Each is at most ~1.5 KB, so this bounds
# the window's memory at a few tens of MB on a very long run; older frames are
# dropped and the window says how many.
MAX_FRAMES = 20000
# First entry in the captured-traffic target selector.  Real addresses are
# compared as parsed IPv4 endpoints; this label can therefore never collide
# with or accidentally substring-match one.
ALL_FRAME_TARGETS = "All targets"
# How to switch the bridge on, said by both the network pane and the
# captured-traffic window while they are empty.
WIFI_HOWTO = "Enable \"Wi-Fi peer\" on the Config tab before Run"
NET_IDLE_HINT = ("Nothing on air yet. %s to attach the virtual radio and the IP bridge; this "
                 "pane then shows every frame and connection, and what the device sent "
                 "inside them." % WIFI_HOWTO)

# Where the Config tab's choices are kept between launches. A per-user
# convenience only: a missing or unreadable file just means the defaults.
CONFIG_PATH = os.path.join(os.path.expanduser("~"), ".bekensim_ui.json")

# Every Config-tab option and its default. The GUI is an integration tool, so
# its virtual router relays outbound traffic by default. The command-line
# emulator remains explicitly opt-in through --net-out.
OPTION_DEFAULTS = {
    "boot": False, "xvr_selfclear": False, "blecore": False,
    "tuyamcu": False, "tuyamcu_pid": "", "tuyamcu_raw": False,
    "wifi": True, "net_out": True,
    "wifi_role": "auto", "wifi_ssid": "", "wifi_pass": "",
    "fwd_local": "8080", "fwd_device": "80",
    "pcap": False, "pcap_path": "",
    "http_gets": "", "mqtt_pubs": "",
}
# The two options typed one-per-line in a small text box rather than an entry.
TEXT_OPTIONS = ("http_gets", "mqtt_pubs")
# Never written to the settings file: a passphrase is secret, while net_out
# should always return to the documented GUI default rather than silently
# inheriting an older version's saved choice.
UNSAVED_OPTIONS = ("wifi_pass", "net_out")


# Most BK7231 variants share one served identity.  U stays explicit because
# its second GPIO bank is a real hardware-layout distinction.
CLI_FAMILY = {"BK7231U": "BK7231U", "BK7238": "BK7238",
              "BK7252N": "BK7252N", "BK7252": "BK7252"}


def guess_chip(path):
    """Pick the -chip identity a dump's filename implies, as the CLI docs do."""
    return CLI_FAMILY.get(periph.chip_from_name(path), "BK7231")


def _same_file(a, b):
    """Whether two spellings name one file. On Windows the slashes, the case
    and a relative path can all differ for the same dump."""
    def norm(p):
        return os.path.normcase(os.path.abspath(p))
    return norm(a) == norm(b)


class SimulatorUI:
    def __init__(self, root, initial_image=None, settings_path=CONFIG_PATH):
        self.root = root
        self.settings_path = settings_path    # None: neither read nor write one
        root.title("BekenEmulator")
        root.minsize(1280, 620)

        self.emu = None
        self.thread = None
        self.started_at = None
        self.finished = None          # message from the worker when it ends
        self.error = None             # exception text from the worker
        self._active_forward_url = None  # the current run's browser endpoint

        # Bytes the firmware has transmitted, per port, waiting to be shown.
        # The worker appends under the lock; the UI thread swaps and clears.
        self._out = {1: bytearray(), 2: bytearray()}
        self._net_lines = []            # whole [WIFI]/[NET] lines, drained by _poll
        self._net_count = 0
        # Every frame the IP bridge carried: (direction, bytes, device time).
        # Bounded, because a long run carries tens of thousands of them.
        self._frames = []
        self._frames_dropped = 0
        self._detail_win = None
        self._detail_drawn_at = 0.0     # throttle for redrawing the packet list
        self._detail_drawn_count = -1
        self._net_hint_shown = False   # is the 'nothing yet' note on screen?
        self._out_lock = threading.Lock()
        self._tx_count = {1: 0, 2: 0}
        self._hex_col = 0             # column tracker for UART1 hex formatting
        # Tail of UART2 output past the last newline. The quicktick filter works
        # a line at a time, but bytes arrive on arbitrary boundaries, so a
        # half-line is held here until its newline turns up.
        self._u2_partial = ""
        self.quick_buttons = []
        self._periph_key = None       # last GPIO/PWM snapshot drawn
        # Pins the user is holding through the GPIO pane: pin -> driven level.
        # These are external pad levels, not firmware register edits.  Keeping
        # the state here lets a press survive a table redraw (a relay write can
        # otherwise replace every Treeview row while the button is held).
        self._gpio_pressed = {}
        self._gpio_selected = None
        self._adc_overrides = {}
        # The image selector describes the *next* run and may change while the
        # current firmware is still executing.  Keep the running image's
        # decoded wiring separate so a Browse cannot silently invert a held
        # button's active level.
        self._run_tuya_config = None
        self._run_image = None
        self._tuya_key = None         # (path, size, mtime) of config currently drawn
        self.tuya_config = None

        self._last_insns = 0
        self._last_reset_count = 0
        self._last_rate_at = None
        self._rate = 0.0

        self.history = []
        self.history_pos = None
        self._pending_run = False     # a dropped image waits for the current run to stop
        self._stop_requested = False
        self.drop_enabled = False
        self._drop_note = ""

        # Last session's choices. An image named on the command line (or
        # dropped on the launcher) wins over the remembered one.
        settings = self._load_settings()
        saved = settings.get("image")
        if not (isinstance(saved, str) and saved and os.path.isfile(saved)):
            saved = None
        start = initial_image or saved or DEFAULT_IMAGE
        self._build(start)
        # The remembered chip and key belong to the remembered image, so they
        # only come back along with it - however the same file is spelled.
        self._apply_settings(settings, same_image=(saved is not None and _same_file(start, saved)))
        self._refresh_tuya_config()
        self.drop_enabled = self._enable_drop()
        self._options_changed()
        self.root.after(POLL_MS, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------------------------------------------------------- layout
    def _build(self, initial_image):
        mono = ("Consolas", 9) if sys.platform == "win32" else "TkFixedFont"

        cfg = ttk.LabelFrame(self.root, text="image", padding=6)
        cfg.pack(fill="x", padx=6, pady=(6, 3))

        self.var_image = tk.StringVar(value=initial_image if os.path.exists(initial_image) else "")
        self.image_entry = ttk.Entry(cfg, textvariable=self.var_image)
        self.image_entry.pack(side="left", fill="x", expand=True)
        # Typing a path by hand is deliberately not traced per keystroke: a
        # config scan decrypts about 256 KiB.  Enter / leaving the field, the
        # Tuya tab, Browse, Drop and Run are the useful refresh boundaries.
        self.image_entry.bind("<Return>", lambda _e: self._refresh_tuya_config(force=True))
        self.image_entry.bind("<FocusOut>", lambda _e: self._refresh_tuya_config())
        ttk.Button(cfg, text="Browse...", command=self._browse, width=10).pack(side="left", padx=(4, 10))

        ttk.Label(cfg, text="chip").pack(side="left")
        self.var_chip = tk.StringVar(value=guess_chip(initial_image))
        ttk.Combobox(cfg, textvariable=self.var_chip, values=chip_names(),
                     state="readonly", width=9).pack(side="left", padx=(3, 10))

        ttk.Label(cfg, text="key").pack(side="left")
        self.var_key = tk.StringVar(value="TUYA")
        ttk.Combobox(cfg, textvariable=self.var_key, values=["(none)"] + sorted(KNOWN_KEYS),
                     state="readonly", width=9).pack(side="left", padx=(3, 10))

        # Every other choice lives on the Config tab. The variables are made
        # here, before any widget, so the summary line can follow them.
        self.opt = {}
        for name, default in OPTION_DEFAULTS.items():
            if name in TEXT_OPTIONS:
                continue                     # multi-line boxes, made on the Config tab
            var = (tk.BooleanVar(value=default) if isinstance(default, bool)
                   else tk.StringVar(value=default))
            var.trace_add("write", lambda *_a: self._options_changed())
            self.opt[name] = var
        self.opt_text = {}

        # One click selects a known-useful image with the correct chip/key and
        # starts it through the ordinary Run path.  Keeping this above the Run
        # controls makes it a launcher, while the image row still shows exactly
        # what was selected and remains fully editable afterwards.
        self.interesting_buttons = []
        self.interesting_group_labels = []
        self._pending_uart1_rx = None
        for paired, title in ((False, "Quick run:"),
                              (True, "Real Tuya reconnect (owned IDs only; sends to cloud):")):
            interesting = ttk.Frame(self.root)
            interesting.pack(fill="x", padx=6, pady=(0, 3))
            heading = ttk.Label(interesting, text=title)
            heading.grid(row=0, column=0, sticky="w", padx=(0, 4))
            self.interesting_group_labels.append(heading)
            group_profiles = [p for p in INTERESTING_DUMPS if bool(p.get("paired")) == paired]
            for idx, profile in enumerate(group_profiles):
                row, col = divmod(idx, 10)
                button = ttk.Button(
                    interesting, text=profile["label"],
                    command=lambda p=profile: self._run_interesting(p))
                button.grid(row=row, column=col + 1, sticky="w", padx=(0, 4), pady=(0, 2))
                self.interesting_buttons.append(button)

        run = ttk.Frame(self.root)
        run.pack(fill="x", padx=6)
        self.btn_run = ttk.Button(run, text="Run", command=self._on_run, width=10)
        self.btn_run.pack(side="left")
        self.btn_pause = ttk.Button(run, text="Pause", command=self._on_pause,
                                    width=10, state="disabled")
        self.btn_pause.pack(side="left", padx=4)
        self.btn_stop = ttk.Button(run, text="Stop", command=self._on_stop,
                                   width=10, state="disabled")
        self.btn_stop.pack(side="left")
        ttk.Button(run, text="Clear", command=self._clear, width=10).pack(side="left", padx=(12, 0))
        ttk.Button(run, text="Save log...", command=self._save, width=10).pack(side="left", padx=4)
        # What the next Run will use, so the choices made on the Config tab
        # stay visible while watching the Run tab.
        self.var_summary = tk.StringVar(value="")
        # width=1: the row's fill sizes it, not its text, so typing in an
        # option field does not make the whole window grow.
        ttk.Label(run, textvariable=self.var_summary, anchor="w", width=1).pack(
            side="left", fill="x", expand=True, padx=(12, 0))

        self.var_state = tk.StringVar(value="idle - pick an image and press Run")
        state = ttk.Label(self.root, textvariable=self.var_state, anchor="w",
                          relief="sunken", padding=4)
        state.pack(fill="x", padx=6, pady=4)

        # The three logs sit side by side in the top half; the network pane is
        # a full-width strip under them, in a draggable split. Its lines are
        # long (a decoded Tuya announcement is a whole JSON object), so width
        # matters more to it than height.
        # Run holds everything that shows a run; Config shapes the next one;
        # Tuya config decodes the selected image's stored device/wiring data.
        self.tabs = ttk.Notebook(self.root)
        self.tabs.pack(fill="both", expand=True, padx=6)
        self.run_tab = ttk.Frame(self.tabs)
        self.config_tab = ttk.Frame(self.tabs, padding=8)
        self.tuya_tab = ttk.Frame(self.tabs, padding=8)
        self.tabs.add(self.run_tab, text="Run")
        self.tabs.add(self.config_tab, text="Config")
        self.tabs.add(self.tuya_tab, text="Tuya config")

        split = ttk.PanedWindow(self.run_tab, orient="vertical")
        split.pack(fill="both", expand=True)

        panes = ttk.PanedWindow(split, orient="horizontal")

        left = ttk.LabelFrame(panes, text="UART1 - MCU link / command console", padding=4)
        self.txt1 = self._console(left, mono)
        opts = ttk.Frame(left)
        opts.pack(side="bottom", fill="x", pady=(4, 0), before=self.txt1.master)
        self.var_hex = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="show as hex", variable=self.var_hex).pack(side="left")
        ttk.Button(opts, text="Copy", width=7,
                   command=lambda: self._copy(self.txt1, "UART1")).pack(side="right")
        panes.add(left, weight=1)

        peri = ttk.LabelFrame(panes, text="GPIO / PWM", padding=4)
        pf = ttk.Frame(peri)
        pf.pack(fill="both", expand=True)
        pbar = ttk.Scrollbar(pf, orient="vertical")
        self.tree = ttk.Treeview(pf, columns=("pin", "name", "state", "reg"),
                                 show="headings", height=16, yscrollcommand=pbar.set)
        for key, title, width, anchor in (("pin", "Pin", 38, "e"), ("name", "Name", 74, "w"),
                                          ("state", "State", 250, "w"), ("reg", "Reg", 58, "e")):
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor=anchor, stretch=(key == "state"))
        pbar.config(command=self.tree.yview)
        pbar.pack(side="right", fill="y")
        self.tree.pack(side="left", fill="both", expand=True)
        # A ttk Treeview draws on the NATIVE light background, not the dark one
        # the console Text widgets use, so these must be dark inks - the pale
        # greens and blues that read well in the log panes are near-invisible
        # here. Contrast ratios against white, all clearing WCAG AA (4.5:1):
        #   factory 6.0:1   out 6.6:1   func 9.3:1
        self.tree.tag_configure("factory", foreground="#5b6472")   # de-emphasised
        self.tree.tag_configure("out", foreground="#0b6b3a")       # driven output
        self.tree.tag_configure("func", foreground="#14448f")      # peripheral owns the pad
        self.tree.tag_configure("pressed", foreground="#9b2c2c")   # externally driven input
        self.tree.bind("<<TreeviewSelect>>", self._gpio_selection_changed)
        self.tree.bind("<Double-1>", self._gpio_double_click)

        # Packed bottom-up ahead of the table, like the rows under the logs,
        # so a short window squeezes the table and not these.
        self.var_pwm = tk.StringVar(value="no PWM channels decoded yet")
        self.var_all_pins = tk.BooleanVar(value=False)
        ttk.Checkbutton(peri, text="show untouched pins", variable=self.var_all_pins,
                        command=self._refresh_periph).pack(side="bottom", anchor="w", before=pf)
        ttk.Label(peri, textvariable=self.var_pwm, anchor="w",
                  wraplength=300, justify="left").pack(side="bottom", fill="x", pady=(4, 0),
                                                       before=pf)

        # SARADC analog input channels (ch0..ch7 / P20..P26, battery / temp / light).
        self.adc_frame = ttk.Frame(peri)
        self.adc_frame.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.var_adc = tk.StringVar(
            value="ADC: idle (default %d / %.2fV)"
            % (periph.DEFAULT_ADC_RAW, periph.DEFAULT_ADC_RAW * 3.3 / 1023.0))
        ttk.Label(self.adc_frame, textvariable=self.var_adc, anchor="w",
                  wraplength=300, justify="left").pack(side="top", fill="x")
        adc_inputs_row = ttk.Frame(self.adc_frame)
        adc_inputs_row.pack(side="top", fill="x", pady=(2, 0))
        ttk.Label(adc_inputs_row, text="Ch:").pack(side="left", padx=(0, 2))
        self.var_adc_ch = tk.StringVar(value="3")
        self.cmb_adc_ch = ttk.Combobox(
            adc_inputs_row, textvariable=self.var_adc_ch,
            values=("0 (VBAT)", "1 (P4)", "2 (P5)", "3 (P23)", "4 (P2)", "5 (P3)",
                    "6 (P12)", "7 (P13)", "P26", "P24", "P20"),
            width=9)
        self.cmb_adc_ch.pack(side="left", padx=(0, 6))
        ttk.Label(adc_inputs_row, text="Val:").pack(side="left", padx=(0, 2))
        self.var_adc_val = tk.StringVar(value=str(periph.DEFAULT_ADC_RAW))
        adc_ent = ttk.Entry(adc_inputs_row, textvariable=self.var_adc_val, width=8)
        adc_ent.pack(side="left", padx=(0, 6))
        adc_ent.bind("<Return>", lambda _e: self._apply_adc_input())
        self.btn_adc_apply = ttk.Button(adc_inputs_row, text="Set ADC", width=9,
                                        command=self._apply_adc_input)
        self.btn_adc_apply.pack(side="right")

        # Energy Meter (BL0942 UART1 / BL0937 Pulse) simulated inputs.
        self.meter_frame = ttk.Frame(peri)
        self.meter_frame.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.var_meter = tk.StringVar(value="Meter: none detected (BL0942 UART1 ready)")
        ttk.Label(self.meter_frame, textvariable=self.var_meter, anchor="w",
                  wraplength=300, justify="left").pack(side="top", fill="x")
        meter_inputs = ttk.Frame(self.meter_frame)
        meter_inputs.pack(side="top", fill="x", pady=(2, 0))
        self.var_meter_v = tk.StringVar(value="230")
        self.var_meter_a = tk.StringVar(value="1.5")
        self.var_meter_w = tk.StringVar(value="345")
        for lbl_txt, var_obj in (("V:", self.var_meter_v),
                                 ("A:", self.var_meter_a),
                                 ("W:", self.var_meter_w)):
            ttk.Label(meter_inputs, text=lbl_txt).pack(side="left", padx=(0, 2))
            ent = ttk.Entry(meter_inputs, textvariable=var_obj, width=6)
            ent.pack(side="left", padx=(0, 6))
            ent.bind("<Return>", lambda _e: self._apply_energy_meter())
        self.btn_meter_apply = ttk.Button(meter_inputs, text="Set V/A/W", width=9,
                                          command=self._apply_energy_meter)
        self.btn_meter_apply.pack(side="right")

        # LED Driver (SM2135 / SM2235 / BP5758D / BP1658CJ / WS2812 / PWM) live color swatch.
        self.led_frame = ttk.Frame(peri)
        self.led_frame.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.led_swatch = tk.Canvas(self.led_frame, width=38, height=22,
                                    highlightthickness=1, highlightbackground="#5b6472",
                                    background=periph.LED_OFF_HEX)
        self.led_swatch.pack(side="left", padx=(0, 6))
        self.var_led = tk.StringVar(value="LED driver: none detected")
        ttk.Label(self.led_frame, textvariable=self.var_led, anchor="w",
                  wraplength=255, justify="left").pack(side="left", fill="x", expand=True)

        # Camera Sensor (GC0328C I2C + EJPEG + GDMA) live preview & frame counter.
        self.cam_frame = ttk.Frame(peri)
        self.cam_frame.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.cam_canvas = tk.Canvas(self.cam_frame, width=64, height=48,
                                    highlightthickness=1, highlightbackground="#5b6472",
                                    background="#101216")
        self.cam_canvas.pack(side="left", padx=(0, 6))
        self._cam_photo = None
        self.var_camera = tk.StringVar(value="Camera: none detected")
        ttk.Label(self.cam_frame, textvariable=self.var_camera, anchor="w",
                  wraplength=230, justify="left").pack(side="left", fill="x", expand=True)

        # Deep Sleep status & manual RTC / GPIO wakeup controls.
        self.sleep_frame = ttk.Frame(peri)
        self.sleep_frame.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.var_sleep = tk.StringVar(value="Sleep: AWAKE")
        ttk.Label(self.sleep_frame, textvariable=self.var_sleep, anchor="w",
                  wraplength=200, justify="left").pack(side="left", fill="x", expand=True)
        self.btn_wake_gpio = ttk.Button(
            self.sleep_frame, text="Wake GPIO", width=10, state="disabled",
            command=lambda: self._wake_deep_sleep("gpio"))
        self.btn_wake_gpio.pack(side="right", padx=(4, 0))
        self.btn_wake_rtc = ttk.Button(
            self.sleep_frame, text="Wake RTC", width=9, state="disabled",
            command=lambda: self._wake_deep_sleep("rtc"))
        self.btn_wake_rtc.pack(side="right")

        gpio_ctl = ttk.Frame(peri)
        gpio_ctl.pack(side="bottom", fill="x", pady=(4, 0), before=pf)
        self.var_gpio_action = tk.StringVar(
            value="Select a configured GPIO input to simulate its button.")
        ttk.Label(gpio_ctl, textvariable=self.var_gpio_action, anchor="w").pack(
            side="left", fill="x", expand=True)
        self.btn_gpio_press = ttk.Button(gpio_ctl, text="Press", width=13,
                                         state="disabled",
                                         command=self._toggle_gpio_input)
        self.btn_gpio_press.pack(side="right", padx=(6, 0))

        right = ttk.LabelFrame(panes, text="UART2 - firmware debug log", padding=4)
        self.txt2 = self._console(right, mono)
        ropts = ttk.Frame(right)
        ropts.pack(side="bottom", fill="x", pady=(4, 0), before=self.txt2.master)
        self.var_hide_tick = tk.BooleanVar(value=False)
        ttk.Checkbutton(ropts, text="suppress %s" % QUICKTICK,
                        variable=self.var_hide_tick).pack(side="left")
        ttk.Button(ropts, text="Copy", width=7,
                   command=lambda: self._copy(self.txt2, "UART2")).pack(side="right")
        panes.add(right, weight=1)

        # Added last so the peripheral table is the THIRD column, to the right
        # of both logs - the two UARTs stay side by side where they are easiest
        # to read against each other.
        panes.add(peri, weight=1)
        split.add(panes, weight=3)

        # Everything the virtual radio and the IP bridge carried. These used to
        # be mixed into the UART2 pane, where they were lost among the
        # firmware's own debug output; the emulator now hands them over whole
        # (Emulator.net_sink) so they can be shown, filtered and saved apart.
        self.net_frame = ttk.LabelFrame(split, text=NET_TITLE, padding=4)
        self.txt_net = self._console(self.net_frame, mono, height=8)
        self.txt_net.tag_configure("wifi", foreground="#8ab4f8")    # 802.11 link
        self.txt_net.tag_configure("net", foreground="#7fd6a0")     # IP and above
        self.txt_net.tag_configure("payload", foreground="#e8c06a")  # decoded contents
        # Each control row under a log is packed ahead of the log itself, so a
        # short window squeezes the text rather than hiding the controls.
        nopts = ttk.Frame(self.net_frame)
        nopts.pack(side="bottom", fill="x", pady=(4, 0), before=self.txt_net.master)
        self.var_net_ip_only = tk.BooleanVar(value=False)
        ttk.Checkbutton(nopts, text="IP traffic only (hide Wi-Fi link)",
                        variable=self.var_net_ip_only).pack(side="left")
        ttk.Button(nopts, text="Copy", width=7,
                   command=lambda: self._copy(self.txt_net, "network")).pack(side="right")
        ttk.Button(nopts, text="Details (bytes)...", width=18,
                   command=self._open_details).pack(side="right", padx=(0, 6))
        # Double-clicking a line in the pane opens the same window, which is
        # where someone reading a line reaches for the detail behind it.
        self.txt_net.bind("<Double-Button-1>", self._details_for_click)
        split.add(self.net_frame, weight=1)
        self._append(self.txt_net, NET_IDLE_HINT + "\n", "note")
        self._net_hint_shown = True

        send = ttk.LabelFrame(self.run_tab, text="send to UART1", padding=6)
        # Packed ahead of the panes so a short window squeezes the logs, not
        # the one place commands are typed.
        send.pack(side="bottom", fill="x", pady=(4, 6), before=split)

        typed = ttk.Frame(send)
        typed.pack(fill="x")

        self.var_input = tk.StringVar()
        self.entry = ttk.Entry(typed, textvariable=self.var_input, font=mono)
        self.entry.pack(side="left", fill="x", expand=True)
        self.entry.bind("<Return>", lambda _e: self._on_send())
        self.entry.bind("<Up>", self._history_back)
        self.entry.bind("<Down>", self._history_fwd)

        self.var_mode = tk.StringVar(value="ASCII")
        ttk.Combobox(typed, textvariable=self.var_mode, values=["ASCII", "HEX"],
                     state="readonly", width=7).pack(side="left", padx=(6, 4))
        self.var_ending = tk.StringVar(value="CR+LF")
        ttk.Combobox(typed, textvariable=self.var_ending, values=list(LINE_ENDINGS),
                     state="readonly", width=8).pack(side="left", padx=(0, 6))
        self.btn_send = ttk.Button(typed, text="Send", command=self._on_send,
                                   width=8, state="disabled")
        self.btn_send.pack(side="left")

        # One click sends immediately - no filling the box first.
        quick = ttk.Frame(send)
        quick.pack(fill="x", pady=(6, 0))
        ttk.Label(quick, text="quick:").grid(row=0, column=0, sticky="w", padx=(0, 4))
        for i, cmd in enumerate(QUICK_COMMANDS):
            row, col = divmod(i, QUICK_PER_ROW)
            btn = ttk.Button(quick, text=cmd, state="disabled",
                             command=lambda c=cmd: self._send_command(c))
            btn.grid(row=row, column=col + 1, sticky="w", padx=(0, 4), pady=(0, 2))
            self.quick_buttons.append(btn)

        self._ui_ready = False
        self._build_config(self.config_tab, mono)
        self._build_tuya_config(self.tuya_tab, mono)
        self._ui_ready = True
        self._options_changed()
        self.tabs.bind("<<NotebookTabChanged>>", self._tab_changed)

    # -------------------------------------------------------- Tuya config tab
    def _build_tuya_config(self, tab, mono):
        """Static, masked configuration recovered from the selected flash."""
        top = ttk.Frame(tab)
        top.pack(fill="x", pady=(0, 6))
        self.var_tuya_status = tk.StringVar(value="No image scanned yet.")
        ttk.Label(top, textvariable=self.var_tuya_status, anchor="w").pack(
            side="left", fill="x", expand=True)
        ttk.Button(top, text="Refresh", width=10,
                   command=lambda: self._refresh_tuya_config(force=True)).pack(side="right")

        panes = ttk.PanedWindow(tab, orient="horizontal")
        panes.pack(fill="both", expand=True)

        summary = ttk.LabelFrame(panes, text="Decoded summary (credentials masked)", padding=4)
        sf = ttk.Frame(summary)
        sf.pack(fill="both", expand=True)
        sbar = ttk.Scrollbar(sf, orient="vertical")
        self.tuya_tree = ttk.Treeview(sf, columns=("field", "value"), show="headings",
                                      yscrollcommand=sbar.set)
        self.tuya_tree.heading("field", text="Field")
        self.tuya_tree.heading("value", text="Value")
        self.tuya_tree.column("field", width=180, anchor="w")
        self.tuya_tree.column("value", width=390, anchor="w")
        sbar.config(command=self.tuya_tree.yview)
        sbar.pack(side="right", fill="y")
        self.tuya_tree.pack(side="left", fill="both", expand=True)
        panes.add(summary, weight=1)

        raw = ttk.LabelFrame(panes, text="Decoded user_param_key JSON (no credentials)", padding=4)
        self.txt_tuya_json = self._console(raw, mono, height=22)
        panes.add(raw, weight=1)

    def _tab_changed(self, _event=None):
        if self.tabs.select() == str(self.tuya_tab):
            self._refresh_tuya_config()

    @staticmethod
    def _tuya_json_value(value):
        """Restore obvious JSON scalars; loose Tuya strings remain strings."""
        try:
            return json.loads(value)
        except (TypeError, ValueError):
            return value

    def _refresh_tuya_config(self, force=False, path=None):
        """Decode once per selected file revision and redraw the Tuya tab.

        Only the already-redacted human summary and user_param_key pairs are
        rendered.  The raw storage records may contain an owner's SSID,
        password, tokens and PSKs, so they intentionally never reach a widget.
        """
        path = (path or self.var_image.get()).strip()
        try:
            st = os.stat(path)
            key = (os.path.normcase(os.path.abspath(path)), st.st_size,
                   getattr(st, "st_mtime_ns", int(st.st_mtime * 1e9)))
        except OSError:
            key = None
        if not force and key is not None and key == self._tuya_key:
            return self.tuya_config
        self._tuya_key = key
        self.tuya_config = None

        self.tuya_tree.delete(*self.tuya_tree.get_children())
        self.txt_tuya_json.configure(state="normal")
        self.txt_tuya_json.delete("1.0", "end")
        self.txt_tuya_json.configure(state="disabled")
        if key is None:
            self.var_tuya_status.set("Pick an existing flash dump to decode.")
            if not self._running():
                self._refresh_static_periph()
            return None

        try:
            decoded = tuya_config.extract(path)
        except Exception as exc:
            self.var_tuya_status.set("Tuya config decode failed: %s: %s"
                                     % (type(exc).__name__, exc))
            if not self._running():
                self._refresh_static_periph()
            return None
        self.tuya_config = decoded
        if not decoded:
            if tuya_config.AES is None:
                note = "Tuya config unavailable: install pycryptodome to decrypt storage."
            else:
                note = "No recoverable Tuya configuration found in this image."
            self.var_tuya_status.set(note)
            if not self._running():
                self._refresh_static_periph()
            return None

        for label, value in decoded.get("human", []):
            self.tuya_tree.insert("", "end", values=(label, value))
        safe_pairs = {name: self._tuya_json_value(value)
                      for name, value in decoded.get("pairs", [])}
        body = json.dumps(safe_pairs, indent=2, ensure_ascii=False)
        self.txt_tuya_json.configure(state="normal")
        self.txt_tuya_json.insert("1.0", body + "\n")
        self.txt_tuya_json.configure(state="disabled")

        storage = "encrypted" if decoded.get("encrypted") else "plaintext"
        self.var_tuya_status.set(
            "%s storage at 0x%06X — %d config fields, %d datapoints"
            % (storage, decoded.get("offset", 0), len(safe_pairs),
               len(decoded.get("dps", []))))
        if not self._running():
            self._refresh_static_periph()
        return decoded

    # ------------------------------------------------------------ Config tab
    def _build_config(self, tab, mono):
        """Every option the command line has for a run, grouped, each with a
        one-line note on what it is for."""
        tab.columnconfigure(0, weight=1)
        tab.columnconfigure(1, weight=1)
        self.opt_widgets = {}          # option name -> widgets that depend on another option

        def group(col, title):
            box = ttk.LabelFrame(tab, text=title, padding=8)
            box.grid(row=0, column=col, sticky="nsew", padx=(0 if col == 0 else 6, 0))
            box.columnconfigure(2, weight=1)
            return box

        def note(box, row, text):
            label = ttk.Label(box, text=text, foreground="#5b6472", wraplength=300,
                              justify="left")
            label.grid(row=row, column=2, sticky="w", padx=(10, 0), pady=3)
            return label

        def check(box, row, name, text, help_):
            w = ttk.Checkbutton(box, text=text, variable=self.opt[name])
            w.grid(row=row, column=0, columnspan=2, sticky="w", pady=3)
            note(box, row, help_)
            return w

        def entry(box, row, name, text, help_, width=20):
            ttk.Label(box, text=text).grid(row=row, column=0, sticky="w", padx=(18, 6), pady=3)
            w = ttk.Entry(box, textvariable=self.opt[name], width=width)
            w.grid(row=row, column=1, sticky="w", pady=3)
            note(box, row, help_)
            return w

        dev = group(0, "Device")
        check(dev, 0, "boot", "boot ROM",
              "Start in the bootloader at 0x0 instead of jumping into the app.")
        check(dev, 1, "xvr_selfclear", "RF calibration self-clear",
              "Clear the radio calibration bit at 0x9000f8. Many stock dumps stall "
              "early without it.")
        check(dev, 2, "blecore", "force BLE core",
              "Legacy override for N/M dumps using generic BK7231. BK7231N and BK7238 "
              "select their Bluetooth hardware automatically.")
        check(dev, 3, "tuyamcu", "TuyaMCU peer",
              "Answer the TuyaMCU serial protocol on UART1, as the device's MCU would.")
        pid = entry(dev, 4, "tuyamcu_pid", "product ID",
                    "The 16-character id the MCU reports; stock firmware checks it "
                    "against its own. Empty uses a built-in one.")
        raw = ttk.Checkbutton(dev, text="raw product info", variable=self.opt["tuyamcu_raw"])
        raw.grid(row=5, column=0, columnspan=2, sticky="w", padx=(18, 0), pady=3)
        note(dev, 5, "Send product info in the raw 16-byte form newer TuyaOS needs, not JSON.")
        self.opt_widgets["tuyamcu"] = [pid, raw]

        net = group(1, "Network")
        check(net, 0, "wifi", "Wi-Fi peer",
              "Attach a virtual radio peer and IP network. Outbound traffic is relayed through "
              "this computer by default; clear the option below for an offline sinkhole.")
        net_out = check(
            net, 1, "net_out", "real internet / Tuya cloud",
            "Relay the firmware's outbound DNS, TCP and UDP through this computer. This does "
            "not pair or add a device. It only reconnects an already-bound identity; if its "
            "credentials remain valid and the app uses the same account and region, that "
            "existing device may come online. Use only identities you own; keep the physical "
            "device offline to avoid a duplicate connection. Frame history stays complete, "
            "but real-cloud TLS/MQTT payloads remain encrypted.")
        ttk.Label(net, text="role").grid(row=2, column=0, sticky="w", padx=(18, 6), pady=3)
        role = ttk.Combobox(net, textvariable=self.opt["wifi_role"], values=list(WIFI_ROLES),
                            state="readonly", width=8)
        role.grid(row=2, column=1, sticky="w", pady=3)
        note(net, 2, "auto takes the opposite role to whatever the device turns out to be.")
        ssid = entry(net, 3, "wifi_ssid", "network name",
                     "Empty adopts whichever network the device asks for.")
        pw = entry(net, 4, "wifi_pass", "passphrase",
                   "Makes the access point WPA2 protected. A paired device only joins with "
                   "the passphrase it stored. Not saved between launches.")
        pw.configure(show="*")
        ttk.Label(net, text="forward").grid(row=5, column=0, sticky="w", padx=(18, 6), pady=3)
        fwd = ttk.Frame(net)
        fwd.grid(row=5, column=1, sticky="w", pady=3)
        ttk.Label(fwd, text="127.0.0.1:").pack(side="left")
        f_local = ttk.Entry(fwd, textvariable=self.opt["fwd_local"], width=6)
        f_local.pack(side="left")
        ttk.Label(fwd, text=" to device:").pack(side="left")
        f_dev = ttk.Entry(fwd, textvariable=self.opt["fwd_device"], width=5)
        f_dev.pack(side="left")
        self.btn_open_forward = ttk.Button(fwd, text="Open in browser",
                                           command=self._open_forward,
                                           state="disabled")
        self.btn_open_forward.pack(side="left", padx=(8, 0))
        note(net, 5, "A local port forwarded to a device port, e.g. its web panel. Clear both "
                     "to turn it off. Open works while that forward is running.")
        pcap = ttk.Checkbutton(net, text="radio capture", variable=self.opt["pcap"])
        pcap.grid(row=6, column=0, sticky="w", padx=(18, 0), pady=3)
        pfile = ttk.Frame(net)
        pfile.grid(row=6, column=1, sticky="w", pady=3)
        p_entry = ttk.Entry(pfile, textvariable=self.opt["pcap_path"], width=14)
        p_entry.pack(side="left")
        p_browse = ttk.Button(pfile, text="...", width=3, command=self._browse_pcap)
        p_browse.pack(side="left", padx=(2, 0))
        self.lbl_radio_capture_note = note(
            net, 6,
            "Save every 802.11 frame to a pcap file for Wireshark. The IP-level "
            "capture is in the network pane's Details window. " + FRAME_PRIVACY_NOTE)

        def lines_box(row, name, label, help_):
            ttk.Label(net, text=label).grid(row=row, column=0, sticky="nw", padx=(18, 6), pady=3)
            box = tk.Text(net, height=3, width=28, font=mono, undo=True)
            box.grid(row=row, column=1, sticky="w", pady=3)
            note(net, row, help_)
            self.opt_text[name] = box
            # Text has no variable to trace; <<Modified>> fires once per change
            # of its flag, so the flag is reset each time to hear the next one.
            box.bind("<<Modified>>", lambda _e, b=box: (b.edit_modified(False),
                                                        self._options_changed()))
            return box

        gets = lines_box(7, "http_gets", "HTTP GET",
                         "One path per line, requested from the device's port 80 once it "
                         "is reachable. E.g. /cm?cmnd=POWER%20ON")
        pubs = lines_box(8, "mqtt_pubs", "MQTT messages",
                         "One TOPIC=PAYLOAD per line, pushed to the device once it "
                         "subscribes to a matching topic.")
        self.opt_widgets["wifi"] = [net_out, role, ssid, pw, f_local, f_dev, pcap, p_entry, p_browse,
                                    gets, pubs]
        self.opt_widgets["pcap"] = [p_entry, p_browse]

        foot = ttk.Frame(tab)
        foot.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(10, 0))
        ttk.Button(foot, text="Reset to defaults", command=self._reset_options).pack(side="left")
        self.var_cfg_note = tk.StringVar(value="")
        ttk.Label(foot, textvariable=self.var_cfg_note, foreground="#5b6472").pack(
            side="left", padx=(12, 0))

    def _options_changed(self):
        """Keep dependent fields and the summary line in step with the options."""
        if not getattr(self, "_ui_ready", False):
            return
        wifi = self.opt["wifi"].get()
        for name, widgets in self.opt_widgets.items():
            on = self.opt[name].get()
            if name == "pcap":
                on = on and wifi            # a capture needs the radio peer at all
            for w in widgets:
                state = "normal" if on else "disabled"
                if isinstance(w, ttk.Combobox):
                    state = "readonly" if on else "disabled"
                w.configure(state=state)
                if isinstance(w, tk.Text):
                    # ttk widgets grey out when disabled; a tk.Text does not,
                    # so it is given the look by hand. configure(opt)[3] is the
                    # option's default, which keeps this right on any platform.
                    w.configure(
                        background=(w.configure("background")[3] if on
                                    else ttk.Style().lookup("TFrame", "background")),
                        foreground=w.configure("foreground")[3] if on else "gray50")
        self.var_summary.set(self._summary_text())
        self.var_cfg_note.set("Choices are remembered between launches. " + self._drop_note)

    def _summary_text(self):
        """One line naming what the next Run will use beyond the defaults."""
        o = self._option_values()
        parts = []
        if o["boot"]:
            parts.append("boot ROM")
        if o["xvr_selfclear"]:
            parts.append("RF-cal self-clear")
        if o["blecore"]:
            parts.append("BLE core")
        if o["tuyamcu"]:
            extra = [x for x in (o["tuyamcu_pid"].strip(), "raw" if o["tuyamcu_raw"] else "") if x]
            parts.append("TuyaMCU" + (" (%s)" % ", ".join(extra) if extra else ""))
        if o["wifi"]:
            extra = [o["wifi_role"], "real internet" if o["net_out"] else "offline"]
            if o["wifi_ssid"].strip():
                extra.append("'%s'" % o["wifi_ssid"])
            # Named either way: the passphrase is not remembered, so after a
            # relaunch this is where "open" shows instead of "WPA2".
            extra.append("WPA2" if o["wifi_pass"] else "open")
            if o["fwd_local"].strip() and o["fwd_device"].strip():
                extra.append(":%s to :%s" % (o["fwd_local"].strip(), o["fwd_device"].strip()))
            if o["pcap"]:
                extra.append("capturing")
            for name, label in (("http_gets", "HTTP GET"), ("mqtt_pubs", "MQTT")):
                count = sum(1 for line in o[name].splitlines() if line.strip())
                if count:
                    extra.append("%d %s" % (count, label))
            parts.append("Wi-Fi peer (%s)" % ", ".join(extra))
        return "options: " + (", ".join(parts) if parts else "none (Config tab)")

    def _collect_cfg(self):
        """Every option as plain values for the worker thread, checked first.

        Raises ValueError with a message fit for a dialog, so a typo is caught
        before anything is loaded rather than surfacing as a failed run. The
        rules themselves are the command line's (src/main.py), so both front
        ends accept exactly the same values.
        """
        o = self._option_values()

        def field(label, check, value):
            # The shared checks name the command-line flag; the dialog leads
            # with the field's name on this tab.
            try:
                return check(value)
            except ValueError as e:
                raise ValueError("%s: %s" % (label, e))

        def lines(name):
            return [line.strip() for line in o[name].splitlines() if line.strip()]

        tuyamcu = o["tuyamcu"]
        cfg = {"chip": self.var_chip.get(), "key": self.var_key.get(),
               "boot": o["boot"], "xvr_selfclear": o["xvr_selfclear"],
               "blecore": o["blecore"], "tuyamcu": tuyamcu,
               "tuyamcu_pid": (field("Product ID", check_tuyamcu_pid,
                                     o["tuyamcu_pid"].strip() or None) if tuyamcu else None),
               "tuyamcu_raw": o["tuyamcu_raw"] if tuyamcu else False,
               "wifi": o["wifi"]}
        if not o["wifi"]:
            return cfg
        # Both ports empty turns the forward off; anything else must parse
        # the way the command line's LPORT:DPORT does.
        lp, dp = o["fwd_local"].strip(), o["fwd_device"].strip()
        forwards = (field("Port forward", parse_forwards, ["%s:%s" % (lp, dp)])
                    if lp or dp else [])
        pcap = None
        if o["pcap"]:
            pcap = o["pcap_path"].strip()
            if not pcap:
                raise ValueError("Radio capture is ticked but no file is set.")
            if not os.path.isdir(os.path.dirname(os.path.abspath(pcap))):
                raise ValueError("Radio capture: the folder for %s does not exist." % pcap)
        cfg.update({"wifi_role": o["wifi_role"], "net_out": o["net_out"],
                    # Neither is stripped: an SSID is matched exactly and salts
                    # the WPA2 key, and spaces are part of a passphrase.
                    "wifi_ssid": o["wifi_ssid"] if o["wifi_ssid"].strip() else None,
                    "wifi_pass": field("Passphrase", check_wifi_pass, o["wifi_pass"] or None),
                    "forwards": forwards, "pcap": pcap,
                    "http_gets": field("HTTP GET", parse_http_gets, lines("http_gets")),
                    "mqtt_pubs": field("MQTT messages", parse_mqtt_pubs, lines("mqtt_pubs"))})
        return cfg

    def _browse_pcap(self):
        path = filedialog.asksaveasfilename(defaultextension=".pcap",
                                            filetypes=[("pcap", "*.pcap"), ("All", "*.*")])
        if path:
            self.opt["pcap_path"].set(path)

    def _update_open_forward(self):
        """The URL belongs to the active run, not fields edited for the next one."""
        on = self._forward_is_open()
        self.btn_open_forward.config(state="normal" if on else "disabled")

    def _forward_is_open(self):
        """True only when this run owns a successfully-bound host listener."""
        if (self._active_forward_url is None or not self._running()
                or self._stop_requested):
            return False
        peer = getattr(self.emu, "wifi_peer", None)
        net = getattr(peer, "net", None)
        io = getattr(net, "io", None)
        return bool(getattr(io, "listeners", {}))

    def _open_forward(self):
        """Open the active loopback port without letting a browser error stop the UI."""
        if not self._forward_is_open():
            return
        try:
            opened = webbrowser.open(self._active_forward_url, new=2)
        except Exception as exc:
            messagebox.showerror("BekenEmulator", "Could not open the browser: %s" % exc)
            return
        if not opened:
            messagebox.showerror("BekenEmulator", "Could not find a browser to open %s"
                                 % self._active_forward_url)

    def _set_text_option(self, name, value):
        """Replace a multi-line option's text. A disabled Text ignores edits,
        so it is opened for the change and put back as it was."""
        box = self.opt_text[name]
        state = box.cget("state")
        box.configure(state="normal")
        box.delete("1.0", "end")
        box.insert("1.0", value)
        box.edit_reset()          # a load is not an edit Ctrl+Z should take back
        box.configure(state=state)

    def _option_values(self):
        """Every option's current value, the text boxes included."""
        o = {name: var.get() for name, var in self.opt.items()}
        o.update((name, box.get("1.0", "end-1c")) for name, box in self.opt_text.items())
        return o

    def _set_option(self, name, value):
        if name in TEXT_OPTIONS:
            self._set_text_option(name, value)
        else:
            self.opt[name].set(value)

    def _reset_options(self):
        for name, default in OPTION_DEFAULTS.items():
            self._set_option(name, default)

    # ------------------------------------------------------- saved settings
    def _load_settings(self):
        if not self.settings_path:
            return {}
        try:
            with open(self.settings_path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return {}
        return data if isinstance(data, dict) else {}

    def _apply_settings(self, data, same_image=True):
        """Restore saved choices; anything missing or of the wrong type keeps
        its default rather than breaking the window."""
        for name, default in OPTION_DEFAULTS.items():
            value = data.get(name)
            if name in UNSAVED_OPTIONS or not isinstance(value, type(default)):
                continue
            if name == "wifi_role" and value not in WIFI_ROLES:
                continue
            self._set_option(name, value)
        if same_image:
            # Checked as str first: a list or dict here would make the
            # membership tests raise, and the window would never open.
            chip, key = data.get("chip"), data.get("key")
            if isinstance(chip, str) and chip in CHIPS:
                self.var_chip.set(chip)
            if isinstance(key, str) and (key in KNOWN_KEYS or key == "(none)"):
                self.var_key.set(key)

    def _settings_data(self):
        image = self.var_image.get().strip()
        # Absolute, so it still resolves when the GUI starts from elsewhere.
        data = {"image": os.path.abspath(image) if image else "",
                "chip": self.var_chip.get(), "key": self.var_key.get()}
        data.update((name, value) for name, value in self._option_values().items()
                    if name not in UNSAVED_OPTIONS)
        return data

    def _save_settings(self):
        if not self.settings_path:
            return
        # Written aside and swapped in, so a failed write leaves the previous
        # settings rather than a truncated file that loads as defaults.
        tmp = self.settings_path + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self._settings_data(), f, indent=1)
            os.replace(tmp, self.settings_path)
        except OSError:
            try:
                os.remove(tmp)
            except OSError:
                pass                 # a convenience; never worth an error dialog

    # ---------------------------------------------------------- drag and drop
    def _enable_drop(self):
        """Accept files dropped on the window. Needs the optional tkinterdnd2
        package and a root made by its TkinterDnD.Tk (see main()); without
        them the window simply does not take drops, and the Config tab says
        why."""
        try:
            from tkinterdnd2 import DND_FILES
        except ImportError:
            self._drop_note = "Install tkinterdnd2 (pip) to drop .bin files on the window."
            return False
        try:
            self.root.drop_target_register(DND_FILES)
            self.root.dnd_bind("<<Drop>>", self._on_drop)
        except Exception:
            self._drop_note = ("tkinterdnd2 is installed but its tkdnd library did not load, "
                               "so files cannot be dropped on the window.")
            return False
        self._drop_note = "Drop a .bin file on the window to load and run it."
        return True

    def _on_drop(self, event):
        paths = [p for p in self.root.tk.splitlist(event.data) if os.path.isfile(p)]
        if not paths:
            self.var_state.set("that drop held no file to load")
            return getattr(event, "action", None)
        self._load_image(paths[0], run=True)
        return getattr(event, "action", None)

    def _load_image(self, path, run=False, chip=None, key=None):
        """Point the window at an image and, if asked, run it - stopping a run
        in progress first, so a drop always means 'run this one now'."""
        self.var_image.set(path)
        self.var_chip.set(chip or guess_chip(path))
        if key is not None:
            self.var_key.set(key)
        self._refresh_tuya_config(path=path)
        self.tabs.select(self.run_tab)
        if not run:
            return
        if self.thread is not None and self.thread.is_alive():
            self._pending_run = True
            self._on_stop()
            self.var_state.set("stopping the current run to start %s ..." % os.path.basename(path))
        else:
            self._on_run()

    def _run_interesting(self, profile):
        """Load and immediately run one entry from :data:`INTERESTING_DUMPS`."""
        path = os.path.join(FIRMWARE_DIR, profile["file"])
        if not os.path.isfile(path):
            messagebox.showerror(
                "BekenEmulator",
                "The bundled quick-run image is missing:\n%s" % path)
            return
        self._pending_uart1_rx = profile.get("uart1_rx")
        if profile.get("wifi"):
            self.opt["wifi"].set(True)
            # A quick run must be reproducible even after the Config tab was
            # used for another network.  In particular, paired Tuya firmware
            # is a station already, so a retained ``sta`` peer role would put
            # both ends in the same role and prevent the advertised call-home
            # run.  Empty fields let the offline peer adopt the dump's stored
            # network, matching the command-line regression setup.
            self.opt["wifi_role"].set("auto")
            self.opt["wifi_ssid"].set("")
            self.opt["wifi_pass"].set("")
            # Each shortcut starts from the documented GUI default unless a
            # profile explicitly overrides it. A stale saved setting never
            # decides whether this new run can reach the network.
            self.opt["net_out"].set(bool(
                profile.get("net_out", OPTION_DEFAULTS["net_out"])))
        self._load_image(path, run=True, chip=profile["chip"], key=profile["key"])

    def _console(self, parent, mono, height=18):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        bar = ttk.Scrollbar(frame, orient="vertical")
        txt = tk.Text(frame, wrap="char", height=height, width=48, font=mono,
                      state="disabled", yscrollcommand=bar.set,
                      background="#101216", foreground="#d8dde3",
                      insertbackground="#d8dde3")
        bar.config(command=txt.yview)
        bar.pack(side="right", fill="y")
        txt.pack(side="left", fill="both", expand=True)
        txt.tag_configure("sent", foreground="#7fd6a0")
        txt.tag_configure("note", foreground="#e0a24a")
        return txt

    # ------------------------------------------------- captured traffic window
    @staticmethod
    def _filter_from_line(line):
        """A filter that narrows the packet list to the frames behind a line.

        A connection's own port is the most selective thing a [NET] line
        carries, so prefer the first ephemeral one; failing that, an address.
        Returns "" when the line names neither, which shows everything rather
        than silently hiding frames.
        """
        ports = re.findall(r"\d+\.\d+\.\d+\.\d+:(\d+)", line or "")
        for p in ports:
            if int(p) >= 1024:
                return ":" + p
        if ports:
            return ":" + ports[0]
        addrs = re.findall(r"\d+\.\d+\.\d+\.\d+", line or "")
        return addrs[0] if addrs else ""

    def _details_for_click(self, event=None):
        """Double-click in the network pane: open the bytes behind THAT line."""
        want = ""
        if event is not None:
            try:
                line = self.txt_net.get("@%d,%d linestart" % (event.x, event.y),
                                        "@%d,%d lineend" % (event.x, event.y))
                want = self._filter_from_line(line)
            except tk.TclError:
                want = ""
        self._open_details()
        if want and getattr(self, "var_frame_filter", None) is not None:
            self.var_frame_filter.set(want)
            self._refresh_details()

    def _open_details(self, _event=None):
        """A packet list over every frame the bridge carried, with its bytes.

        The pane above says what happened; this says exactly what crossed, as
        a hex dump of the frame itself. One window at a time: reopening raises
        the existing one so a double-click cannot bury the screen in windows.
        """
        if self._detail_win is not None and self._detail_win.winfo_exists():
            self._detail_win.deiconify()
            self._detail_win.lift()
            self._refresh_details()
            return
        mono = ("Consolas", 9) if sys.platform == "win32" else "TkFixedFont"
        win = tk.Toplevel(self.root)
        self._detail_win = win
        win.title("captured traffic - every frame the IP bridge carried")
        win.minsize(820, 480)

        bar = ttk.Frame(win, padding=6)
        bar.pack(fill="x")
        ttk.Label(bar, text="filter:").pack(side="left")
        self.var_frame_filter = tk.StringVar(value="")
        ent = ttk.Entry(bar, textvariable=self.var_frame_filter)
        ent.pack(side="left", fill="x", expand=True, padx=(4, 6))
        ent.bind("<Return>", lambda e: self._refresh_details())
        ttk.Button(bar, text="Apply", width=7, command=self._refresh_details).pack(side="left")
        ttk.Button(bar, text="Refresh", width=8, command=self._refresh_details).pack(side="left", padx=4)
        ttk.Label(bar, text="target host:").pack(side="left", padx=(4, 0))
        self.var_frame_target = tk.StringVar(value=ALL_FRAME_TARGETS)
        self.frame_target = ttk.Combobox(bar, textvariable=self.var_frame_target,
                                         values=(ALL_FRAME_TARGETS,), width=15,
                                         state="readonly")
        self.frame_target.pack(side="left", padx=(4, 6))
        self.frame_target.bind("<<ComboboxSelected>>", lambda _e: self._refresh_details())
        ttk.Button(bar, text="Save .pcap...", width=13,
                   command=self._save_pcap).pack(side="right")
        self.var_frame_count = tk.StringVar(value="")
        ttk.Label(win, textvariable=self.var_frame_count, anchor="w",
                  padding=(8, 0)).pack(fill="x")
        self.var_frame_privacy = tk.StringVar(value=FRAME_PRIVACY_NOTE)
        ttk.Label(win, textvariable=self.var_frame_privacy, anchor="w",
                  foreground="#8a4b08", padding=(8, 2)).pack(fill="x")

        split = ttk.PanedWindow(win, orient="vertical")
        split.pack(fill="both", expand=True, padx=6, pady=(2, 6))

        lf = ttk.Frame(split)
        lbar = ttk.Scrollbar(lf, orient="vertical")
        self.frame_tree = ttk.Treeview(lf, columns=("no", "time", "dir", "len", "what"),
                                       show="headings", height=14, yscrollcommand=lbar.set)
        for key, title, width, anchor in (("no", "#", 52, "e"), ("time", "Time", 86, "e"),
                                          ("dir", "Direction", 92, "w"), ("len", "Bytes", 56, "e"),
                                          ("what", "Frame", 420, "w")):
            self.frame_tree.heading(key, text=title)
            self.frame_tree.column(key, width=width, anchor=anchor, stretch=(key == "what"))
        lbar.config(command=self.frame_tree.yview)
        lbar.pack(side="right", fill="y")
        self.frame_tree.pack(side="left", fill="both", expand=True)
        # Dark inks on the native light Treeview background, as the GPIO table.
        self.frame_tree.tag_configure("in", foreground="#0b6b3a")    # sent by the device
        self.frame_tree.tag_configure("out", foreground="#14448f")   # sent to the device
        self.frame_tree.bind("<<TreeviewSelect>>", self._show_frame_bytes)
        split.add(lf, weight=2)

        hf = ttk.Frame(split)
        hbar = ttk.Scrollbar(hf, orient="vertical")
        self.txt_bytes = tk.Text(hf, wrap="none", height=12, font=mono, state="disabled",
                                 yscrollcommand=hbar.set, background="#101216",
                                 foreground="#d8dde3", insertbackground="#d8dde3")
        hbar.config(command=self.txt_bytes.yview)
        hbar.pack(side="right", fill="y")
        self.txt_bytes.pack(side="left", fill="both", expand=True)
        split.add(hf, weight=1)

        ttk.Button(win, text="Copy these bytes", width=18,
                   command=lambda: self._copy(self.txt_bytes, "frame bytes")).pack(
            anchor="e", padx=6, pady=(0, 6))
        win.protocol("WM_DELETE_WINDOW", win.destroy)
        self._refresh_details()

    @staticmethod
    def _frame_target_host(direction, data):
        """The remote IPv4 host for a captured frame, or ``None``.

        For a frame emitted by the device the counterpart is its destination;
        for a frame delivered to the device it is its source.  Addresses which
        cannot identify one target host are deliberately absent from the
        selector: unspecified, limited broadcast, and multicast.
        """
        endpoints = netstack.ipv4_endpoints(data)
        if endpoints is None:
            return None
        if direction == "from device":
            host = endpoints[1]
        elif direction == "to device":
            host = endpoints[0]
        else:
            return None
        if host in (b"\x00" * 4, b"\xff" * 4) or 224 <= host[0] <= 239:
            return None
        return netstack.ip2str(host)

    @classmethod
    def _target_hosts(cls, frames):
        """Unique, numerically ordered remote hosts present in *frames*."""
        hosts = {cls._frame_target_host(direction, data)
                 for direction, data, _us in frames}
        hosts.discard(None)
        return sorted(hosts, key=lambda host: tuple(int(part) for part in host.split(".")))

    def _refresh_target_hosts(self, frames):
        """Update the selector without losing a still-present selection."""
        if getattr(self, "frame_target", None) is None:
            return
        choices = [ALL_FRAME_TARGETS] + self._target_hosts(frames)
        current = self.var_frame_target.get()
        self.frame_target.configure(values=choices)
        if current not in choices:
            self.var_frame_target.set(ALL_FRAME_TARGETS)

    def _visible_frames(self):
        """The captured frames the filter box lets through, with their numbers."""
        with self._out_lock:
            frames = list(self._frames)
        base = self._frames_dropped
        self._refresh_target_hosts(frames)
        needle = (getattr(self, "var_frame_filter", None).get().strip().lower()
                  if getattr(self, "var_frame_filter", None) else "")
        target = (getattr(self, "var_frame_target", None).get()
                  if getattr(self, "var_frame_target", None) else ALL_FRAME_TARGETS)
        out = []
        for i, (direction, data, us) in enumerate(frames):
            what = netstack.describe_frame(data)
            if target != ALL_FRAME_TARGETS and self._frame_target_host(direction, data) != target:
                continue
            if needle and needle not in what.lower() and needle not in direction.lower():
                continue
            out.append((base + i + 1, direction, data, us, what))
        return out

    def _refresh_details(self):
        if self._detail_win is None or not self._detail_win.winfo_exists():
            return
        rows = self._visible_frames()
        # The list is rebuilt wholesale, which would otherwise throw away what
        # the reader was looking at every time it refreshes during a run.
        was_selected = self.frame_tree.selection()
        was_at = self.frame_tree.yview()[0]
        self.frame_tree.delete(*self.frame_tree.get_children())
        # Only the tail is drawn: a Treeview with tens of thousands of rows is
        # slow to fill and no easier to read. The count line says so plainly.
        shown = rows[-2000:]
        for no, direction, data, us, what in shown:
            self.frame_tree.insert("", "end", iid=str(no),
                                   values=(no, "%.3f s" % (us / 1e6), direction, len(data), what),
                                   tags=("in" if direction == "from device" else "out",))
        total = len(self._frames) + self._frames_dropped
        note = "%d frames captured" % total
        if len(shown) != len(rows):
            note += ", showing the last %d of %d matching" % (len(shown), len(rows))
        elif len(rows) != len(self._frames):
            note += ", %d match the filter" % len(rows)
        if self._frames_dropped:
            note += " (the earliest %d were dropped)" % self._frames_dropped
        if not total:
            note = ("Nothing captured yet. %s; every frame the bridge carries then appears "
                    "here with its bytes." % WIFI_HOWTO)
        self.var_frame_count.set(note)
        if not shown:
            return
        keep = [i for i in was_selected if self.frame_tree.exists(i)]
        if keep:
            self.frame_tree.selection_set(keep)
            self.frame_tree.yview_moveto(was_at)
        elif was_selected:
            # What was selected has been filtered out or dropped; leave the
            # view where it was rather than jumping somewhere unrelated.
            self.frame_tree.yview_moveto(was_at)
        else:
            last = str(shown[-1][0])
            self.frame_tree.selection_set(last)
            self.frame_tree.see(last)

    def _show_frame_bytes(self, _event=None):
        sel = self.frame_tree.selection()
        if not sel:
            return
        want = int(sel[0])
        for no, direction, data, us, what in self._visible_frames():
            if no != want:
                continue
            head = ("frame %d   %s   %.6f s   %d bytes\n%s\n\n"
                    % (no, direction, us / 1e6, len(data), what))
            self.txt_bytes.configure(state="normal")
            self.txt_bytes.delete("1.0", "end")
            self.txt_bytes.insert("end", head + netstack.hexdump(data) + "\n")
            self.txt_bytes.configure(state="disabled")
            return

    def _save_pcap(self):
        """Write the captured frames as an Ethernet pcap, for Wireshark."""
        rows = self._visible_frames()
        if not rows:
            messagebox.showinfo("BekenEmulator", "Nothing captured yet.")
            return
        path = filedialog.asksaveasfilename(defaultextension=".pcap",
                                            filetypes=[("pcap", "*.pcap"), ("All", "*.*")])
        if not path:
            return
        cap = netstack.EthPcap(path)
        try:
            for _no, _direction, data, us, _what in rows:
                cap.write(data, us)
        finally:
            cap.close()
        self.var_state.set("saved %d frames to %s" % (len(rows), path))

    # ------------------------------------------------------------ text panes
    def _append(self, widget, text, tag=None):
        if not text:
            return
        # Only follow the tail if the view is already at the tail, so scrolling
        # back to read something is not yanked away by new output.
        at_end = widget.yview()[1] >= 0.999
        widget.configure(state="normal")
        widget.insert("end", text, tag or ())
        lines = int(widget.index("end-1c").split(".")[0])
        if lines > MAX_LINES:
            widget.delete("1.0", "%d.0" % (lines - MAX_LINES + 1))
        widget.configure(state="disabled")
        if at_end:
            widget.see("end")

    def _render_uart1(self, data):
        """UART1 carries either console text or binary protocol frames."""
        if not self.var_hex.get():
            return data.decode("latin-1")
        out = []
        for b in data:
            out.append("%02x " % b)
            self._hex_col += 1
            if self._hex_col >= 16:
                out.append("\n")
                self._hex_col = 0
        return "".join(out)

    @staticmethod
    def _gpio_is_input(raw):
        """Whether a firmware GPIO word leaves the pad as a plain input.

        The Beken SDK's input modes all set bits 2+3 (input enable and output
        disable).  Second-function modes set bit 6 and must not be offered as
        buttons even when the peripheral itself receives data on that pad.
        """
        return raw is not None and (raw & 0x0C) == 0x0C and not (raw & 0x40)

    @staticmethod
    def _tuya_button_levels(decoded):
        """{pin: active_level} recovered from Tuya's wiring record.

        Only pin/level pairs already decoded in memory are considered.  The
        raw protected-storage records (which can carry credentials) never
        reach this path or a widget.
        """
        pairs = dict((str(k), str(v)) for k, v in (decoded or {}).get("pairs", []))
        out = {}
        button = re.compile(r"^(?:bt(?:\d+)?_pin|bt|k\d+pin_pin|key\d*_pin|"
                            r"reset_pin|total_bt_pin)$")
        for name, value in pairs.items():
            if not button.match(name):
                continue
            try:
                pin = int(value, 0)
            except ValueError:
                continue
            stem = name[:-4] if name.endswith("_pin") else name
            level = pairs.get(stem + "_lv")
            if level in ("0", "1"):
                out[pin] = int(level)
        return out

    def _gpio_press_level(self, pin, raw):
        """Return (active level, explanation) for a selected input."""
        decoded = self._run_tuya_config if self._running() else self.tuya_config
        stored = self._tuya_button_levels(decoded).get(pin)
        if stored is not None:
            return stored, "Tuya config says active %s" % ("HIGH" if stored else "LOW")
        if raw & (1 << 5):
            idle = 1 if raw & (1 << 4) else 0
            return 1 - idle, "%s input" % ("pull-up" if idle else "pull-down")
        # A floating input has no electrically knowable idle level.  LOW is a
        # deterministic, common button convention; say so rather than quietly
        # presenting a guess as decoded hardware metadata.
        return 0, "no pull; defaulting the simulated press to LOW"

    def _emu_chip_name(self):
        """Name of the running emulator's chip, or None when nothing is running."""
        chip = getattr(self.emu, "chip", None)
        return chip.name if chip is not None else None

    def _emu_gpio_pins(self):
        """GPIO pin count of the running emulator's chip (base bank if unknown)."""
        chip = getattr(self.emu, "chip", None)
        return chip.gpio_pins if chip is not None else periph.PIN_COUNT

    def _gpio_config(self, pin):
        """Return one logical pin's live config, including BK7252's upper bank."""
        if self.emu is None:
            return None
        slot = periph.gpio_slot(pin, self._emu_gpio_pins())
        return getattr(self.emu.state, "gpio_cfg", {}).get(slot)

    def _selected_gpio(self):
        sel = self.tree.selection()
        if not sel:
            return None
        iid = sel[0]
        if not iid.startswith("gpio:"):
            return None
        try:
            return int(iid.split(":", 1)[1])
        except ValueError:
            return None

    def _gpio_selection_changed(self, _event=None):
        self._gpio_selected = self._selected_gpio()
        self._update_gpio_control()
        self._update_sleep_controls()

    def _gpio_double_click(self, event):
        iid = self.tree.identify_row(event.y)
        if iid:
            self.tree.selection_set(iid)
            self._gpio_selected = self._selected_gpio()
            self._update_gpio_control()
            self._update_sleep_controls()
            if str(self.btn_gpio_press.cget("state")) != "disabled":
                self._toggle_gpio_input()
        return "break"

    def _sleep_wake_pins(self):
        """Return ``{pin: active_level}`` when the chip is in deep sleep."""
        st = getattr(self.emu, "state", None)
        if st is None or not getattr(st, "in_deep_sleep", False):
            return {}
        return dict(getattr(st, "deep_sleep_wake_pins", {}))

    def _update_sleep_controls(self):
        """Update the Deep Sleep status label and manual RTC/GPIO wake buttons."""
        st = getattr(self.emu, "state", None)
        if st is None:
            self.var_sleep.set("Sleep: AWAKE")
            self.btn_wake_rtc.config(text="Wake RTC", state="disabled")
            self.btn_wake_gpio.config(text="Wake GPIO", state="disabled")
            return

        in_sleep = bool(getattr(st, "in_deep_sleep", False))
        sleep_count = int(getattr(st, "deep_sleep_count", 0))
        rtc_sec = float(getattr(st, "deep_sleep_rtc_sec", 0.0))
        wake_pins = dict(getattr(st, "deep_sleep_wake_pins", {}))
        last_wake = getattr(st, "last_wake_reason", None)
        last_pin = getattr(st, "last_wake_pin", None)
        can_act = self._running() and not self._stop_requested

        if in_sleep:
            parts = []
            if rtc_sec > 0:
                parts.append("RTC=%.0fs" % rtc_sec)
            if wake_pins:
                p_list = ", ".join("P%d:%s" % (p, "HIGH" if lv else "LOW")
                                   for p, lv in sorted(wake_pins.items()))
                parts.append("GPIO=[%s]" % p_list)
            detail = (" (%s)" % ", ".join(parts)) if parts else ""
            self.var_sleep.set("DEEP SLEEP #%d%s" % (max(1, sleep_count), detail))
            self.btn_wake_rtc.config(
                text="Wake RTC",
                state="normal" if can_act else "disabled")
            sel = self._gpio_selected if self._gpio_selected is not None else self._selected_gpio()
            if sel in wake_pins:
                gpio_lbl = "Wake P%d" % sel
            elif wake_pins:
                gpio_lbl = "Wake P%d" % sorted(wake_pins.keys())[0]
            else:
                gpio_lbl = "Wake GPIO"
            self.btn_wake_gpio.config(
                text=gpio_lbl,
                state="normal" if can_act else "disabled")
        else:
            if sleep_count > 0 and last_wake:
                if last_wake == "gpio" and last_pin is not None:
                    self.var_sleep.set("Sleep: AWAKE (woke #%d via GPIO P%d)"
                                       % (sleep_count, last_pin))
                else:
                    self.var_sleep.set("Sleep: AWAKE (woke #%d via %s)"
                                       % (sleep_count, str(last_wake).upper()))
            else:
                self.var_sleep.set("Sleep: AWAKE")
            self.btn_wake_rtc.config(text="Wake RTC", state="disabled")
            self.btn_wake_gpio.config(text="Wake GPIO", state="disabled")

    def _wake_deep_sleep(self, reason="rtc"):
        """Manually wake the sleeping device via RTC timer or GPIO wake pin."""
        st = getattr(self.emu, "state", None)
        if (st is None or not self._running() or self._stop_requested
                or not getattr(st, "in_deep_sleep", False)):
            self._update_sleep_controls()
            return
        wake_pins = dict(getattr(st, "deep_sleep_wake_pins", {}))
        pin = None
        if reason == "gpio":
            sel = self._gpio_selected if self._gpio_selected is not None else self._selected_gpio()
            if sel in wake_pins:
                pin = sel
            elif wake_pins:
                pin = sorted(wake_pins.keys())[0]
            else:
                pin = sel
        if hasattr(self.emu, "wake_from_deep_sleep"):
            self.emu.wake_from_deep_sleep(reason=reason, pin=pin)
        self._periph_key = None
        self._refresh_periph()
        self._update_state()

    def _update_gpio_control(self):
        pin = self._gpio_selected
        if pin is None:
            pin = self._selected_gpio()
            self._gpio_selected = pin
        if pin is None:
            self.btn_gpio_press.config(text="Press", state="disabled")
            self.var_gpio_action.set("Select a configured GPIO input to simulate its button.")
            return

        if pin in self._gpio_pressed:
            level = self._gpio_pressed[pin]
            self.btn_gpio_press.config(text="Release P%d" % pin,
                                       state="normal" if (self._running() and not self._stop_requested)
                                       else "disabled")
            self.var_gpio_action.set("P%d is PRESSED %s; release it after the firmware debounces."
                                     % (pin, "HIGH" if level else "LOW"))
            return

        wake_pins = self._sleep_wake_pins()
        if pin in wake_pins:
            level = wake_pins[pin]
            self.btn_gpio_press.config(
                text="Press P%d" % pin,
                state="normal" if (self._running() and not self._stop_requested) else "disabled")
            self.var_gpio_action.set(
                "P%d: deep-sleep wake pin (active %s); Press wakes the chip."
                % (pin, "HIGH" if level else "LOW"))
            return

        raw = self._gpio_config(pin)
        if not self._gpio_is_input(raw):
            self.btn_gpio_press.config(text="Press P%d" % pin, state="disabled")
            self.var_gpio_action.set("P%d is not a plain GPIO input." % pin)
            return

        level, why = self._gpio_press_level(pin, raw)
        self.btn_gpio_press.config(text="Press P%d" % pin,
                                   state="normal" if (self._running() and not self._stop_requested)
                                   else "disabled")
        self.var_gpio_action.set("P%d: %s; Press drives %s, then click Release."
                                 % (pin, why, "HIGH" if level else "LOW"))

    def _toggle_gpio_input(self):
        """Press or release the selected pad through the emulator input API."""
        pin = self._gpio_selected if self._gpio_selected is not None else self._selected_gpio()
        if pin is None or not self._running() or self._stop_requested:
            self._update_gpio_control()
            return
        if pin in self._gpio_pressed:
            try:
                self.emu.set_gpio_input(pin, None)
            except (AttributeError, ValueError) as exc:
                self.var_state.set("GPIO release failed: %s" % exc)
                return
            del self._gpio_pressed[pin]
        else:
            wake_pins = self._sleep_wake_pins()
            raw = self._gpio_config(pin)
            if pin in wake_pins:
                level = wake_pins[pin]
            elif self._gpio_is_input(raw):
                level, _why = self._gpio_press_level(pin, raw)
            else:
                self._update_gpio_control()
                return
            try:
                self.emu.set_gpio_input(pin, level)
            except (AttributeError, ValueError) as exc:
                self.var_state.set("GPIO press failed: %s" % exc)
                return
            self._gpio_pressed[pin] = level
        self._periph_key = None
        self._refresh_periph()
        self._update_gpio_control()

    def _release_gpio_inputs(self):
        """Release every UI-held pad, including on Stop/close/new Run."""
        emu = self.emu
        for pin in list(self._gpio_pressed):
            if emu is not None:
                try:
                    emu.set_gpio_input(pin, None)
                except (AttributeError, ValueError):
                    pass
        self._gpio_pressed.clear()
        self._periph_key = None
        self._update_gpio_control()

    def _parsed_meter_values(self):
        """Return (voltage_v, current_a, power_w) from the UI entry fields."""
        def _num(var, default):
            try:
                return max(0.0, float(var.get().strip()))
            except (ValueError, AttributeError):
                return default
        return (_num(self.var_meter_v, 230.0),
                _num(self.var_meter_a, 1.5),
                _num(self.var_meter_w, 345.0))

    def _apply_energy_meter(self):
        """Validate and push the UI's V/A/W readings into the running emulator."""
        try:
            v = float(self.var_meter_v.get().strip())
            a = float(self.var_meter_a.get().strip())
            w = float(self.var_meter_w.get().strip())
            if v < 0 or a < 0 or w < 0:
                raise ValueError("values must be non-negative")
        except ValueError as exc:
            self.var_state.set("Invalid energy meter reading: %s" % exc)
            return
        decoded = self._run_tuya_config if self._running() else self.tuya_config
        info = periph.describe_peripherals(tuya_config=decoded, meter_values=(v, a, w))
        m_cfg = info.get("meter") or {}
        if self.emu is not None and hasattr(self.emu, "set_energy_meter"):
            self.emu.set_energy_meter(
                v, a, w,
                cf_pin=m_cfg.get("cf_pin"),
                cf1_pin=m_cfg.get("cf1_pin"),
                sel_pin=m_cfg.get("sel_pin"),
                sel_lv=m_cfg.get("sel_lv", 1),
            )
        self._periph_key = None
        if self.emu is not None:
            self._refresh_periph()
        else:
            self._refresh_static_periph()
        self.var_state.set("Energy meter set to %.1f V, %.2f A, %.1f W" % (v, a, w))

    def _apply_adc_input(self):
        """Parse and apply the UI's SARADC channel + sample value."""
        ch_tok = self.var_adc_ch.get().strip().split()[0] if self.var_adc_ch.get().strip() else "0"
        val_tok = self.var_adc_val.get().strip()
        chip = self._emu_chip_name() or self.var_chip.get()
        try:
            ch, raw = periph.parse_adc_spec("%s:%s" % (ch_tok, val_tok), chip=chip)
        except ValueError as exc:
            self.var_state.set("Invalid ADC input: %s" % exc)
            return
        self._adc_overrides[ch] = raw
        if self.emu is not None and hasattr(self.emu, "set_adc_input"):
            self.emu.set_adc_input(ch, raw)
        self._periph_key = None
        if self.emu is not None:
            self._refresh_periph()
        else:
            self._refresh_static_periph()
        volts = raw * 3.3 / 1023.0
        self.var_state.set("ADC %s set to %d (0x%03X, %.2fV)"
                           % (periph.adc_channel_label(ch, chip), raw, raw, volts))

    def _update_adc_widget(self, adc_info=None):
        """Update the SARADC summary line from decoded or static state."""
        if adc_info and adc_info.get("summary"):
            self.var_adc.set(adc_info["summary"])
            return
        chip = self._emu_chip_name() or self.var_chip.get()
        info = periph.describe_adc(adc_inputs=self._adc_overrides, chip=chip)
        self.var_adc.set(info["summary"])

    def _draw_led_swatch(self, led_info):
        """Render the current bulb/strip color(s) on ``self.led_swatch``."""
        self.led_swatch.delete("all")
        if not led_info:
            self.led_swatch.configure(background=periph.LED_OFF_HEX)
            return
        hex_color = led_info.get("live_hex") or led_info.get("hex") or periph.LED_OFF_HEX
        self.led_swatch.configure(background=hex_color)
        pixels = led_info.get("pixels") or [hex_color]
        w = int(self.led_swatch.cget("width") or 38)
        h = int(self.led_swatch.cget("height") or 22)
        n = len(pixels)
        if n <= 1:
            self.led_swatch.create_rectangle(0, 0, w, h, fill=hex_color, outline="")
        else:
            for idx, px in enumerate(pixels):
                x0 = int(round(idx * w / float(n)))
                x1 = int(round((idx + 1) * w / float(n)))
                self.led_swatch.create_rectangle(x0, 0, max(x0 + 1, x1), h,
                                                 fill=px, outline="")

    def _update_camera_widget(self):
        """Render the simulated DVP camera sensor preview and JPEG frame status."""
        st = getattr(self.emu, "state", None)
        if st is None:
            self.var_camera.set("Camera: none detected")
            self.cam_canvas.delete("all")
            self._cam_photo = None
            return

        cam_sensor = getattr(st, "camera_sensor", None)
        cam_w = int(getattr(st, "camera_width", 640))
        cam_h = int(getattr(st, "camera_height", 480))
        cam_i2c = int(getattr(st, "camera_i2c_writes", 0))
        cam_frames = int(getattr(st, "camera_frame_count", 0))
        cam_jpeg = bytes(getattr(st, "camera_last_jpeg", b""))
        cam_rgb = bytes(getattr(st, "camera_frame_rgb", b""))

        if cam_sensor or cam_frames > 0 or cam_i2c > 0:
            name = cam_sensor or "GC0328C"
            if cam_frames > 0:
                self.var_camera.set(
                    "Camera: %s %dx%d — %d frames (%d B JPEG, %d I2C reg)"
                    % (name, cam_w, cam_h, cam_frames, len(cam_jpeg), cam_i2c))
            else:
                self.var_camera.set(
                    "Camera: %s %dx%d (%d I2C reg configured)"
                    % (name, cam_w, cam_h, cam_i2c))
        else:
            self.var_camera.set("Camera: none detected")

        self.cam_canvas.delete("all")
        self._cam_photo = None
        if len(cam_rgb) == 64 * 48 * 3:
            try:
                ppm = b"P6\n64 48\n255\n" + cam_rgb
                self._cam_photo = tk.PhotoImage(data=ppm)
                self.cam_canvas.create_image(0, 0, anchor="nw", image=self._cam_photo)
            except tk.TclError:
                for gy in range(0, 48, 4):
                    for gx in range(0, 64, 4):
                        idx = (gy * 64 + gx) * 3
                        r, g, b = cam_rgb[idx], cam_rgb[idx + 1], cam_rgb[idx + 2]
                        self.cam_canvas.create_rectangle(
                            gx, gy, gx + 4, gy + 4,
                            fill="#%02x%02x%02x" % (r, g, b), outline="")

    def _update_periph_widgets(self, led_info, meter_info, meter_vals):
        """Update the LED driver swatch/label and Energy Meter summary label."""
        if led_info:
            self.var_led.set(led_info["summary"])
        else:
            self.var_led.set("LED driver: none detected")
        self._draw_led_swatch(led_info)

        if meter_info:
            self.var_meter.set(meter_info["summary"])
        else:
            v, a, w = meter_vals
            self.var_meter.set(
                "Meter: BL0942 UART1 ready (%.0fV %.2fA %.0fW)" % (v, a, w))

    def _refresh_static_periph(self):
        """Show Tuya-configured LED driver / Energy Meter / ADC before a run starts."""
        meter_vals = self._parsed_meter_values()
        info = periph.describe_peripherals(
            tuya_config=self.tuya_config, meter_values=meter_vals,
            adc_inputs=self._adc_overrides, chip=self.var_chip.get())
        self._update_periph_widgets(info.get("led"), info.get("meter"), meter_vals)
        self._update_adc_widget(info.get("adc"))
        self._update_camera_widget()
        self._update_sleep_controls()

    def _refresh_periph(self):
        """Redraw the GPIO/PWM table and LED/Meter widgets from live registers.

        The emulator records every GPIO and PWM register write into
        state.gpio_cfg / state.pwm_regs unconditionally (not only under
        EMU_REPORT), so this needs no capture plumbing - and the decoding is
        periph.decode_state, the very function the HTML report renders from.
        """
        if self.emu is None:
            self._refresh_static_periph()
            return
        try:
            gpio = dict(self.emu.state.gpio_cfg)
            pwm = dict(self.emu.state.pwm_regs)
            i2c_frames = dict(getattr(self.emu.state, "i2c_frames", {}))
            i2c_last_on = dict(getattr(self.emu.state, "i2c_last_on", {}))
            ws2812_spi = bytes(getattr(self.emu.state, "ws2812_spi", b""))
            bl0942_polls = int(getattr(self.emu.state, "bl0942_polls", 0))
            in_sleep = bool(getattr(self.emu.state, "in_deep_sleep", False))
            sleep_count = int(getattr(self.emu.state, "deep_sleep_count", 0))
            wake_pins = dict(getattr(self.emu.state, "deep_sleep_wake_pins", {}))
            cam_sensor = getattr(self.emu.state, "camera_sensor", None)
            cam_i2c = int(getattr(self.emu.state, "camera_i2c_writes", 0))
            cam_frames = int(getattr(self.emu.state, "camera_frame_count", 0))
            saradc_ch = getattr(self.emu.state, "saradc_channel", None)
            saradc_raw = getattr(self.emu.state, "saradc_last_raw", None)
            saradc_convs = int(getattr(self.emu.state, "saradc_conversions", 0))
            saradc_reads = int(getattr(self.emu.state, "saradc_reads", 0))
            adc_samples = dict(getattr(self.emu.state, "adc_samples", {}))
            adc_inputs = dict(getattr(self.emu, "adc_inputs", self._adc_overrides))
        except RuntimeError:
            # The emulation thread was mid-write into the dict. Registers are
            # written tens of times per boot, so simply catching the next tick
            # is cheaper and safer than locking the write hook.
            return

        meter_vals = (
            float(getattr(self.emu, "meter_v", self._parsed_meter_values()[0])),
            float(getattr(self.emu, "meter_a", self._parsed_meter_values()[1])),
            float(getattr(self.emu, "meter_w", self._parsed_meter_values()[2])),
        )
        key = (tuple(sorted(gpio.items())), tuple(sorted(pwm.items())),
               tuple(sorted(i2c_frames.items())), ws2812_spi, bl0942_polls,
               meter_vals, self.var_all_pins.get(),
               tuple(sorted(self._gpio_pressed.items())),
               in_sleep, sleep_count, tuple(sorted(wake_pins.items())),
               cam_sensor, cam_i2c, cam_frames,
               saradc_ch, saradc_raw, saradc_convs, saradc_reads,
               tuple(sorted(adc_samples.items())), tuple(sorted(adc_inputs.items())))
        if key == self._periph_key:
            self._update_gpio_control()
            self._update_sleep_controls()
            return              # nothing moved; leave the widget alone
        self._periph_key = key

        # decode_state wants the fine-grained silicon name (BK7231N, not the
        # four -chip identities), which the dump's filename carries.
        # The image field can be changed while this run remains alive.  Decode
        # the live peripheral map with the running image/chip, not the next
        # image selected in the header.
        image = self._run_image or self.var_image.get()
        tuya_cfg = self._run_tuya_config if self._running() else self.tuya_config
        if self._emu_gpio_pins() > periph.PIN_COUNT:
            chip = self.emu.chip.name
        else:
            chip = periph.chip_from_name(image) or self._emu_chip_name() or ""
        data = periph.decode_state(
            gpio, pwm, chip,
            i2c_frames=i2c_frames, i2c_last_on=i2c_last_on,
            ws2812_spi=ws2812_spi, tuya_config=tuya_cfg,
            bl0942_polls=bl0942_polls, meter_values=meter_vals,
            saradc_channel=saradc_ch, saradc_last_raw=saradc_raw,
            saradc_conversions=saradc_convs, saradc_reads=saradc_reads,
            adc_samples=adc_samples, adc_inputs=adc_inputs)
        selected = self._gpio_selected
        self.tree.delete(*self.tree.get_children())
        if not data and not (in_sleep and wake_pins):
            self.var_pwm.set("firmware has not touched GPIO or PWM yet")
            static_info = periph.describe_peripherals(
                tuya_config=tuya_cfg, meter_values=meter_vals,
                saradc_channel=saradc_ch, saradc_last_raw=saradc_raw,
                saradc_conversions=saradc_convs, saradc_reads=saradc_reads,
                adc_samples=adc_samples, adc_inputs=adc_inputs, chip=chip)
            self._update_periph_widgets(static_info.get("led"),
                                        static_info.get("meter"), meter_vals)
            self._update_adc_widget(static_info.get("adc"))
            self._update_camera_widget()
            self._update_gpio_control()
            self._update_sleep_controls()
            return

        show_all = self.var_all_pins.get()
        gpio_rows = list((data or {}).get("gpio", []))
        raw_map = dict((data or {}).get("raw", {}))
        seen_pins = {r[0] for r in gpio_rows}
        if in_sleep and wake_pins:
            for wpin in sorted(wake_pins):
                if wpin not in seen_pins:
                    gpio_rows.append((wpin, "", "factory", "INPUT (deep-sleep wake)"))
            gpio_rows.sort(key=lambda r: r[0])

        for pin, alias, state, detail in gpio_rows:
            is_wake = in_sleep and (pin in wake_pins)
            if state == "factory" and not show_all and not is_wake:
                continue
            if is_wake:
                detail += " [WAKE %s]" % ("HIGH" if wake_pins[pin] else "LOW")
            if state == "factory" and not is_wake:
                tag = "factory"
            elif "OUTPUT" in detail:
                tag = "out"
            else:
                tag = "func"
            raw = raw_map.get(pin)
            if pin in self._gpio_pressed:
                driven = self._gpio_pressed[pin]
                detail += " — PRESSED %s" % ("HIGH" if driven else "LOW")
                tag = "pressed"
            self.tree.insert("", "end", iid="gpio:%d" % pin, tags=(tag,), values=(
                pin, alias or "", detail, "" if raw is None else "0x%02X" % raw))

        for name, value in (data or {}).get("func", []):
            self.tree.insert("", "end", tags=("func",), values=("", name, "function select", value))

        if selected is not None and self.tree.exists("gpio:%d" % selected):
            self.tree.selection_set("gpio:%d" % selected)

        chans = (data or {}).get("pwm_channels") or []
        if chans:
            self.var_pwm.set("PWM (%s layout), pins %s:  %s" % (
                data["pwm_layout"], data["pwm_pins"] or "-",
                "   ".join("ch%d %.0f Hz %.0f%%" % (c[0], c[3], c[4]) for c in chans)))
        else:
            self.var_pwm.set("PWM pins %s, no channels decoded"
                             % ((data or {}).get("pwm_pins") or "-"))
        self._update_periph_widgets((data or {}).get("led"),
                                    (data or {}).get("meter"), meter_vals)
        self._update_adc_widget((data or {}).get("adc"))
        self._update_camera_widget()
        self._update_gpio_control()
        self._update_sleep_controls()

    def _render_uart2(self, data):
        """Firmware log text, optionally with the quicktick chatter dropped.

        Splitting on "\\n" only, rather than str.splitlines(), is deliberate:
        the stream is decoded latin-1, and splitlines() would also break on
        bytes like 0x85 and 0x0C that a log can legitimately contain. Any "\\r"
        stays inside its line, so CR+LF endings survive untouched.
        """
        text = data.decode("latin-1")
        if not self.var_hide_tick.get():
            # Pass through - but do not swallow a half-line held back while the
            # filter was still on.
            if self._u2_partial:
                text, self._u2_partial = self._u2_partial + text, ""
            return text

        lines = (self._u2_partial + text).split("\n")
        self._u2_partial = lines.pop()      # tail past the last newline, if any
        # Dropping the newline along with the line is the point: a suppressed
        # tick leaves no blank gap behind.
        return "".join(ln + "\n" for ln in lines if QUICKTICK not in ln)

    def _copy(self, widget, label):
        """Put a pane's whole scrollback on the clipboard.

        Whatever the pane shows is what gets copied, so a UART1 copy is hex if
        the hex box is ticked, and a UART2 copy has the quicktick lines already
        filtered out if that box is ticked. update() hands the selection to the
        window manager so it survives this window losing focus.
        """
        text = widget.get("1.0", "end-1c")
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.root.update()
        self.var_state.set("copied %d characters from %s" % (len(text), label))

    def _clear(self):
        self._u2_partial = ""
        for w in (self.txt1, self.txt2, self.txt_net):
            w.configure(state="normal")
            w.delete("1.0", "end")
            w.configure(state="disabled")
        self._hex_col = 0
        with self._out_lock:
            del self._net_lines[:]
            del self._frames[:]
        self._frames_dropped = 0
        self._net_count = 0
        self._refresh_details()
        self.net_frame.config(text=NET_TITLE)
        self._append(self.txt_net, NET_IDLE_HINT + "\n", "note")
        self._net_hint_shown = True

    def _save(self):
        path = filedialog.asksaveasfilename(defaultextension=".log",
                                            filetypes=[("Log", "*.log"), ("All", "*.*")])
        if not path:
            return
        with open(path, "w", encoding="utf-8", errors="replace") as f:
            f.write("=== UART1 ===\n")
            f.write(self.txt1.get("1.0", "end"))
            f.write("\n=== UART2 ===\n")
            f.write(self.txt2.get("1.0", "end"))
            f.write("\n=== network (virtual Wi-Fi and IP bridge) ===\n")
            f.write(self.txt_net.get("1.0", "end"))
        self.var_state.set("saved to %s" % path)

    def _browse(self):
        path = filedialog.askopenfilename(
            initialdir=FIRMWARE_DIR if os.path.isdir(FIRMWARE_DIR) else ROOT_DIR,
            filetypes=[("Flash dumps", "*.bin"), ("All files", "*.*")])
        if path:
            self._load_image(path)

    # --------------------------------------------------------------- run/stop
    def _sink(self, port, byte):
        """Called from the emulation thread for every transmitted byte."""
        with self._out_lock:
            self._out[port].append(byte)

    def _net_sink(self, line):
        """Called from the emulation thread for each [WIFI]/[NET] line."""
        with self._out_lock:
            self._net_lines.append(line)

    def _frame_sink(self, direction, frame, now_us):
        """Called from the emulation thread for every frame the bridge carried."""
        with self._out_lock:
            self._frames.append((direction, frame, now_us))
            if len(self._frames) > MAX_FRAMES:
                drop = len(self._frames) - MAX_FRAMES
                del self._frames[:drop]
                self._frames_dropped += drop

    @staticmethod
    def _net_tag(line):
        """Which ink a network line gets: the link, the IP stack, or contents."""
        if line.startswith("[WIFI]"):
            return "wifi"
        if line.startswith("[SLEEP]"):
            return "payload"
        # Lines that carry what was actually sent, rather than the fact of it.
        for mark in (" sends: ", "MQTT PUBLISH", "MQTT -> device", "Tuya LAN broadcast",
                     "HTTP GET", "answered 200"):
            if mark in line:
                return "payload"
        return "net"

    def _on_run(self):
        if self.thread is not None and self.thread.is_alive():
            return
        path = self.var_image.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showerror("BekenEmulator", "Pick a flash dump first.")
            return
        self._refresh_tuya_config(path=path)
        # Read every Tk variable HERE, on the main thread. Tkinter is not
        # thread-safe, so the worker gets plain values, never widgets.
        try:
            cfg = self._collect_cfg()
        except ValueError as e:
            self.tabs.select(self.config_tab)
            messagebox.showerror("BekenEmulator", str(e))
            return
        self._run_tuya_config = self.tuya_config
        self._run_image = path
        if getattr(self, "_pending_uart1_rx", None):
            cfg["uart1_rx"] = self._pending_uart1_rx
        self._pending_uart1_rx = None
        meter_vals = self._parsed_meter_values()
        m_info = periph.describe_peripherals(
            tuya_config=self._run_tuya_config, meter_values=meter_vals).get("meter") or {}
        cfg["meter"] = {
            "v": meter_vals[0], "a": meter_vals[1], "w": meter_vals[2],
            "cf_pin": m_info.get("cf_pin"),
            "cf1_pin": m_info.get("cf1_pin"),
            "sel_pin": m_info.get("sel_pin"),
            "sel_lv": m_info.get("sel_lv", 1),
        }
        cfg["adc_inputs"] = dict(self._adc_overrides)
        # Snapshot private cloud credentials only for the offline TLS peer.
        # A real-cloud run is a transparent TCP relay and neither needs nor
        # should retain a second copy of the dump's secret in the host stack.
        # Stored candidates stay out of widgets, logs and saved settings; the
        # offline peer uses them only to authenticate the encrypted Finished.
        # The byte-exact packet viewer is intentionally different: if firmware
        # sends credential material in clear, its warning explains that the raw
        # frame and any exported PCAP preserve those bytes.
        cfg["tls_psks"], cfg["tuya_legacy_identity_psk"] = (
            tuya_config.offline_tls_config(
                self._run_tuya_config, net_out=cfg.get("net_out")))
        self._active_forward_url = ("http://127.0.0.1:%d/" % cfg["forwards"][0][0]
                                    if cfg.get("forwards") else None)
        # Loading and setup happen on the worker. The button is enabled from
        # _update_state only after the peer has actually opened the forward.
        self._update_open_forward()
        self._save_settings()

        self._release_gpio_inputs()
        self._clear()
        self._periph_key = None
        self._tx_count = {1: 0, 2: 0}
        self._last_insns = 0
        self._last_reset_count = 0
        self._rate = 0.0
        self._last_rate_at = None
        self.finished = self.error = None
        self.emu = None
        self._stop_requested = False
        self._pending_run = False         # whatever was waiting to run, this is it
        # Bytes the previous worker queued after the last drain belong to the
        # run that just ended, not to this one. Nothing writes here now: the
        # old thread is dead (checked above) and the new one has not started.
        with self._out_lock:
            for buf in self._out.values():
                buf.clear()
        self.started_at = time.time()

        self.btn_run.config(state="disabled")
        self.btn_stop.config(state="normal")
        self.btn_pause.config(state="normal", text="Pause")
        self.var_state.set("loading %s ..." % os.path.basename(path))
        self.thread = threading.Thread(target=self._worker, args=(path, cfg), daemon=True)
        self.thread.start()

    def _worker(self, path, cfg):
        """Load, set up and run the emulator. Never touches Tk - see cfg."""
        emu = None
        try:
            key = None if cfg["key"] == "(none)" else parse_key(cfg["key"])
            extra_kw = {"uart1_rx": cfg["uart1_rx"]} if cfg.get("uart1_rx") else {}
            emu = build_emulator(
                path, key=key, chip=cfg["chip"],
                with_boot=cfg["boot"],
                # only_uart keeps the MMIO/trace chatter out; the panes show
                # UART bytes and the status bar shows the rest.
                only_uart=True,
                # The panes format their own bytes, so the emulator's stdout
                # hex tagging is left off and everything arrives via the sink.
                uart1_hex=False,
                tuyamcu=cfg["tuyamcu"],
                tuyamcu_pid=cfg["tuyamcu_pid"],
                tuyamcu_raw=cfg["tuyamcu_raw"],
                xvr_selfclear=cfg["xvr_selfclear"],
                ble_core=True if cfg["blecore"] else None,
                wifi=cfg["wifi"],
                uart_sink=self._sink,
                adc_inputs=cfg.get("adc_inputs"),
                # Writing app_decrypted.bin into the working directory is a
                # command-line convenience, not something a GUI should do.
                save_decrypted=False,
                **extra_kw)
            # Whole [WIFI]/[NET] lines go to the network pane instead of being
            # spliced into UART2's byte stream as they are on the console.
            emu.net_sink = self._net_sink
            emu.net_frame_sink = self._frame_sink
            m_cfg = cfg.get("meter")
            if m_cfg and hasattr(emu, "set_energy_meter"):
                emu.set_energy_meter(
                    m_cfg["v"], m_cfg["a"], m_cfg["w"],
                    cf_pin=m_cfg.get("cf_pin"),
                    cf1_pin=m_cfg.get("cf1_pin"),
                    sel_pin=m_cfg.get("sel_pin"),
                    sel_lv=m_cfg.get("sel_lv", 1),
                )
            if cfg["wifi"]:
                emu.attach_wifi_peer(role=cfg["wifi_role"], ssid=cfg["wifi_ssid"],
                                     passphrase=cfg["wifi_pass"], forwards=cfg["forwards"],
                                     net_out=cfg["net_out"],
                                     pcap=cfg["pcap"], http_gets=cfg["http_gets"],
                                     mqtt_pubs=cfg["mqtt_pubs"],
                                     tls_psks=(() if cfg["net_out"] else
                                               cfg.get("tls_psks", ())),
                                     tuya_legacy_identity_psk=(
                                         False if cfg["net_out"] else cfg.get(
                                             "tuya_legacy_identity_psk", False)))
            emu.setup()
            self.emu = emu
            # A Stop pressed while loading found no emulator to stop; honour
            # it now. (stop() before run() is fine: run() keeps the flag.)
            if self._stop_requested:
                emu.stop()
            emu.run()
            # run() catches what ends it early (a firmware fault, a Python
            # error in a peripheral) and only prints it, to a console the GUI
            # may not have; it keeps the reason for front ends like this one.
            if emu.run_error:
                self.error = emu.run_error
            else:
                self.finished = "emulation finished"
        except Exception as e:            # bad key, unreadable image, a setup error
            self.error = "%s: %s" % (type(e).__name__, e)
        finally:
            # The next run in this window needs the forward port back, and a
            # capture file should be complete once the run is over.
            if emu is not None:
                emu.close_network()

    def _on_pause(self):
        if self.emu is None:
            return
        if self.emu.is_paused():
            self.emu.pause(False)
            self.btn_pause.config(text="Pause")
        else:
            self.emu.pause(True)
            self.btn_pause.config(text="Resume")

    def _on_stop(self):
        self._stop_requested = True
        self._release_gpio_inputs()
        if self.emu is not None:
            self.emu.stop()
        self.btn_stop.config(state="disabled")
        self.btn_pause.config(state="disabled", text="Pause")
        self.btn_open_forward.config(state="disabled")

    def _on_close(self):
        self._save_settings()
        self._release_gpio_inputs()
        if self.emu is not None:
            self.emu.stop()
        self.root.destroy()

    # ------------------------------------------------------------------ send
    def _running(self):
        """An emulator exists and its thread is still going. self.emu stays
        set after a run ends, so it alone does not mean anything is running."""
        return (self.emu is not None and self.thread is not None
                and self.thread.is_alive())

    def _console_ready(self):
        # The emulator holds queued RX bytes for its own uart1_rx_delay (none
        # with a TuyaMCU peer, which only answers what the firmware sent), so
        # anything typed earlier is delivered rather than lost; this only says
        # when the firmware starts listening.
        return self._running() and self.emu.state.insn_count >= self.emu.uart1_rx_delay

    def _send_bytes(self, payload):
        """Put raw bytes on UART1 and show them in the pane."""
        if not self._running():
            return
        self.emu.feed_uart1(payload)
        # The firmware does not echo, so show the line locally - otherwise the
        # pane gives no sign anything was sent.
        if self.var_hex.get():
            shown = payload.hex(" ")
        else:
            shown = payload.decode("latin-1").replace("\r", "").rstrip("\n")
        self._append(self.txt1, "> %s\n" % shown, "sent")

    def _send_command(self, text):
        """Send a literal command line - what the quick buttons do.

        Always ASCII, whatever the ASCII/HEX selector says, because the button
        label IS the text being sent. The line ending still follows the
        selector, since some firmwares only accept one of them.
        """
        self._send_bytes(text.encode("latin-1", "replace")
                         + LINE_ENDINGS[self.var_ending.get()])
        self.history.append(text)
        self.history_pos = None

    def _on_send(self):
        # Enter reaches here even with the Send button disabled, so a finished
        # run must be refused here too, before the text is echoed as sent.
        if not self._running():
            return
        text = self.var_input.get()
        if not text and self.var_mode.get() == "HEX":
            return
        try:
            if self.var_mode.get() == "HEX":
                payload = parse_hex_blobs([text])[0]
            else:
                payload = text.encode("latin-1", "replace")
        except ValueError as e:
            self.var_state.set(str(e))
            return

        self._send_bytes(payload + LINE_ENDINGS[self.var_ending.get()])
        if text:
            self.history.append(text)
        self.history_pos = None
        self.var_input.set("")

    def _history_back(self, _event):
        if not self.history:
            return "break"
        self.history_pos = (len(self.history) - 1 if self.history_pos is None
                            else max(0, self.history_pos - 1))
        self.var_input.set(self.history[self.history_pos])
        self.entry.icursor("end")
        return "break"

    def _history_fwd(self, _event):
        if self.history_pos is None:
            return "break"
        self.history_pos += 1
        if self.history_pos >= len(self.history):
            self.history_pos = None
            self.var_input.set("")
        else:
            self.var_input.set(self.history[self.history_pos])
        self.entry.icursor("end")
        return "break"

    # ------------------------------------------------------------------ poll
    def _poll(self):
        # Rescheduling lives in `finally` on purpose. This loop is the only
        # thing draining the panes and refreshing the status bar, so if it ever
        # dies the window freezes with no output and no error - it just sits
        # there looking like a hung load. Whatever goes wrong, say so and keep
        # the loop alive.
        try:
            with self._out_lock:
                chunks = {p: bytes(b) for p, b in self._out.items() if b}
                for b in self._out.values():
                    b.clear()
                net_lines = self._net_lines[:]
                del self._net_lines[:]

            if 1 in chunks:
                self._tx_count[1] += len(chunks[1])
                self._append(self.txt1, self._render_uart1(chunks[1]))
            if 2 in chunks:
                self._tx_count[2] += len(chunks[2])
                self._append(self.txt2, self._render_uart2(chunks[2]))

            if net_lines:
                if self._net_hint_shown:
                    # The hint explains how to get traffic; once traffic
                    # arrives it is just a stale line above the real log.
                    self.txt_net.configure(state="normal")
                    self.txt_net.delete("1.0", "end")
                    self.txt_net.configure(state="disabled")
                    self._net_hint_shown = False
                ip_only = self.var_net_ip_only.get()
                for line in net_lines:
                    self._net_count += 1
                    tag = self._net_tag(line)
                    if ip_only and tag == "wifi":
                        continue
                    self._append(self.txt_net, line + "\n", tag)
                self.net_frame.config(text="%s - %d lines" % (NET_TITLE, self._net_count))

            # Keep an open details window current, but not at the poll rate:
            # redrawing a few thousand rows 16 times a second would make the
            # whole window crawl. Once a second, and only when something new
            # actually arrived.
            if (self._detail_win is not None and self._detail_win.winfo_exists()
                    and time.time() - self._detail_drawn_at > 1.0):
                with self._out_lock:
                    total = len(self._frames) + self._frames_dropped
                if total != self._detail_drawn_count:
                    self._detail_drawn_count = total
                    self._refresh_details()
                self._detail_drawn_at = time.time()

            self._refresh_periph()
            self._update_state()
        except Exception:
            detail = traceback.format_exc()
            self.var_state.set("UI error (see UART2 pane) - emulation continues")
            self._append(self.txt2, "\n[!] UI error in _poll:\n%s\n" % detail, "note")
            traceback.print_exc()
        finally:
            self.root.after(POLL_MS, self._poll)

    def _update_state(self):
        alive = self.thread is not None and self.thread.is_alive()

        # Wait for the thread to be gone, as for a clean finish: _finish may
        # start a dropped image's run, which refuses while a thread is alive.
        if self.error and not alive:
            self.var_state.set("ERROR  %s" % self.error)
            self._append(self.txt2, "\n[!] %s\n" % self.error, "note")
            self.error = None
            self._finish()
            return
        if self.finished and not alive:
            self.var_state.set("stopped after %s insns" % self._fmt_insns())
            self._append(self.txt2, "\n--- %s ---\n" % self.finished, "note")
            self.finished = None
            self._finish()
            return
        if self.emu is None:
            self._update_open_forward()
            self._update_gpio_control()
            self._update_sleep_controls()
            return

        self._update_open_forward()

        insns = self.emu.state.insn_count
        resets = getattr(self.emu, "reset_count", 0)
        now = time.time()
        if resets != self._last_reset_count:
            # The device clock starts again at zero after a hardware reset;
            # do not briefly display a large negative execution rate.
            self._last_reset_count = resets
            self._last_insns = insns
            self._last_rate_at = now
            self._rate = 0.0
        if self._last_rate_at is not None:
            dt = now - self._last_rate_at
            if dt >= 0.5:
                # Smoothed so the readout does not flicker between polls.
                self._rate = 0.7 * self._rate + 0.3 * ((insns - self._last_insns) / dt)
                self._last_insns, self._last_rate_at = insns, now
        else:
            self._last_insns, self._last_rate_at = insns, now

        ready = self._console_ready()
        self._set_send_enabled(ready)

        if not alive:
            what = "stopped"
        elif self.emu.is_paused():
            what = "PAUSED"
        elif getattr(self.emu.state, "in_deep_sleep", False):
            what = "DEEP SLEEP"
        else:
            what = "running"

        self.var_state.set(
            "%s   %s insns (%.0fk/s)   ~%.1f s guest   WDT resets %d   %.0f s wall   "
            "PC 0x%08x   TX u1 %d / u2 %d   RX queued %d   console %s"
            % (what, self._fmt_insns(), self._rate / 1000.0,
               insns / float(GUEST_SECOND_INSNS), resets, now - self.started_at,
               self.emu.state.last_pc, self._tx_count[1], self._tx_count[2],
               len(self.emu.uart1_rx), "ready" if ready else "not listening yet"))
        self._update_gpio_control()
        self._update_sleep_controls()

    def _fmt_insns(self):
        n = self.emu.state.insn_count if self.emu else 0
        return "%.1fM" % (n / 1e6) if n >= 1e6 else "%d" % n

    def _set_send_enabled(self, on):
        """The typed Send and every quick button share one enabled state."""
        state = "normal" if on else "disabled"
        self.btn_send.config(state=state)
        for btn in self.quick_buttons:
            btn.config(state=state)

    def _finish(self):
        self._release_gpio_inputs()
        self._update_sleep_controls()
        self.btn_run.config(state="normal")
        self.btn_stop.config(state="disabled")
        self.btn_pause.config(state="disabled", text="Pause")
        self.btn_open_forward.config(state="disabled")
        self._active_forward_url = None
        self._set_send_enabled(False)
        if self._pending_run:           # an image was dropped while this run ended
            self._pending_run = False
            self._on_run()


# ---------------------------------------------------------------------------
# Self-tests: drive the network pane the way the emulation thread does, without
# running an emulator. Run: python tools/sim_ui.py --selftest
# ---------------------------------------------------------------------------
def _run_selftests():
    n = 0

    def check(cond, msg):
        nonlocal n
        assert cond, "FAIL: " + msg
        n += 1
        print("  [PASS]", msg)

    try:
        root = tk.Tk()
    except tk.TclError as e:
        # No display (a headless CI runner without xvfb). Say so and skip,
        # rather than failing a build over something unrelated to the code.
        print("  [SKIP] no display available: %s" % e)
        return 0
    root.withdraw()
    # No settings file: the tests must not depend on, or overwrite, the
    # choices the person running them saved.
    ui = SimulatorUI(root, settings_path=None)

    def pane():
        return ui.txt_net.get("1.0", "end-1c")

    def tags_of(needle):
        return set(ui.txt_net.tag_names(ui.txt_net.search(needle, "1.0", "end"))) - {"sel"}

    print("== network pane: before a run ==")
    check(NET_IDLE_HINT.split(".")[0] in pane(), "the pane says how to switch the bridge on")
    check(ui.net_frame.cget("text") == NET_TITLE, "and its heading carries no count yet")

    print("== network pane: lines from the emulation thread ==")
    lines = [
        "[WIFI] device acts as a station (it probes for networks): peer takes the access point role",
        "[WIFI] station 4c:a9:19:14:71:50 associated to 'HojnikB WiFi' (AID 1)",
        "[NET] DHCP: device 4c:a9:19:14:71:50 got 192.168.1.100 (gateway/DNS 192.168.1.1)",
        "[NET] TCP 192.168.1.100:40127 <-> 18.185.31.196:8886 sends: TLS ClientHello (no server name)",
        '[NET] Tuya LAN broadcast from 192.168.1.100:63035: {"gwId":"bf0e","productKey":"gqmm"}',
        "[NET] MQTT -> device PUBLISH cmnd/obkSim/POWER = ON",
    ]
    for line in lines:
        ui._net_sink(line)                  # exactly what Emulator.net_log calls
    check("HojnikB" not in pane(), "a line is held until the UI drains it, not drawn from that thread")
    ui._poll()
    shown = pane()
    check(NET_IDLE_HINT.split(".")[0] not in shown,
          "the 'nothing yet' note goes once there IS something, rather than sitting above it")
    check(all(l in shown for l in lines), "after one poll every line is in the pane, verbatim")
    check(shown.index(lines[0]) < shown.index(lines[-1]), "and in the order they happened")
    check(ui.net_frame.cget("text") == "%s - 6 lines" % NET_TITLE,
          "the heading counts them, so a glance says whether anything was captured")

    print("== network pane: what each line is coloured as ==")
    check("wifi" in tags_of("device acts as a station"), "Wi-Fi link lines get the link ink")
    check("net" in tags_of("DHCP: device"), "plain IP lines get the IP ink")
    check("payload" in tags_of("TLS ClientHello"), "a line carrying what was SENT is inked apart")
    check("payload" in tags_of("Tuya LAN broadcast"), "...as is a decoded announcement")
    check("payload" in tags_of("MQTT -> device"), "...and a message pushed to the device")

    print("== network pane: the IP-only filter ==")
    ui.var_net_ip_only.set(True)
    for line in lines:
        ui._net_sink(line)
    ui._poll()
    tail = pane()[len(shown):]
    check("[WIFI]" not in tail and "DHCP: device" in tail,
          "with the filter on, link lines are hidden and IP lines still arrive")
    check(ui.net_frame.cget("text") == "%s - 12 lines" % NET_TITLE,
          "the count still reflects everything captured, not only what is shown")
    ui.var_net_ip_only.set(False)

    print("== network pane: clearing and saving ==")
    ui._clear()
    check(NET_IDLE_HINT.split(".")[0] in pane() and "HojnikB" not in pane(),
          "Clear empties the pane and puts the hint back")
    check(ui.net_frame.cget("text") == NET_TITLE, "and resets the count")
    ui._net_sink("[NET] back again")
    ui._poll()
    check("[NET] back again" in pane(), "the pane keeps working after a clear")
    fd, path = tempfile.mkstemp(suffix=".log")
    os.close(fd)
    real_dialog = filedialog.asksaveasfilename
    filedialog.asksaveasfilename = lambda **kw: path
    try:
        ui._save()
        body = open(path, encoding="utf-8").read()
    finally:
        filedialog.asksaveasfilename = real_dialog
        os.remove(path)
    check("=== network (virtual Wi-Fi and IP bridge) ===" in body and "[NET] back again" in body,
          "Save log writes the network pane out under its own heading")

    print("== captured traffic window: the frames themselves ==")
    dev = bytes.fromhex("c8478c000000")
    ours = bytes.fromhex("020000000001")
    dev_ip, our_ip = netstack.str2ip("192.168.1.100"), netstack.str2ip("192.168.1.1")
    seg = netstack.tcp_segment(dev_ip, netstack.str2ip("10.200.0.2"), 40127, 1883,
                               1, 1, netstack.TCP_ACK | netstack.TCP_PSH, 8192,
                               b"\x10\x14\x00\x04MQTT\x04\x02\x00\x3c\x00\x06obkSim")
    f_in = netstack.eth_frame(ours, dev, netstack.ETH_IP, seg)
    f_out = netstack.eth_frame(dev, ours, netstack.ETH_IP,
                               netstack.udp_packet(our_ip, dev_ip, 53, 5353, b"\xbe\xef" + bytes(20)))
    ui._frame_sink("from device", f_in, 1_500_000)
    ui._frame_sink("to device", f_out, 1_600_000)
    ui._open_details()
    check(ui._detail_win is not None and ui._detail_win.winfo_exists(),
          "the details window opens from the pane")
    check(ui.var_frame_privacy.get() == FRAME_PRIVACY_NOTE,
          "the raw-frame viewer warns that exact captures can contain clear credentials")
    rows = ui.frame_tree.get_children()
    check(len(rows) == 2, "both captured frames are listed")
    # Tk hands row values back as strings whatever went in.
    vals = [str(v) for v in ui.frame_tree.item(rows[0], "values")]
    check(vals[0] == "1" and vals[2] == "from device" and vals[3] == str(len(f_in)),
          "a row gives the frame's number, direction and exact length")
    check("TCP 192.168.1.100:40127 to 10.200.0.2:1883" in vals[4],
          "and says what the frame is")
    check("(DNS)" in ui.frame_tree.item(rows[1], "values")[4], "the other direction is listed too")

    ui.frame_tree.selection_set(rows[0])
    ui._show_frame_bytes()
    dump = ui.txt_bytes.get("1.0", "end-1c")
    check("obkSim" in dump and "MQTT" in dump,
          "selecting a frame shows its bytes, the payload readable in the dump")
    first = "%08x  %s" % (0, " ".join("%02x" % b for b in f_in[:16]))
    check(first in dump, "the hex is the frame itself, from its first byte")
    check(("%d bytes" % len(f_in)) in dump and "from device" in dump,
          "with a heading saying which frame this is")

    ui.var_frame_filter.set("DNS")
    ui._refresh_details()
    check(len(ui.frame_tree.get_children()) == 1, "the filter narrows the list")
    check("1 match" in ui.var_frame_count.get() or "matching" in ui.var_frame_count.get(),
          "and the count line says how many matched")
    ui.var_frame_filter.set("")
    ui._refresh_details()
    check(len(ui.frame_tree.get_children()) == 2, "clearing the filter brings them back")

    fd, pcap_path = tempfile.mkstemp(suffix=".pcap")
    os.close(fd)
    real_dialog = filedialog.asksaveasfilename
    filedialog.asksaveasfilename = lambda **kw: pcap_path
    try:
        ui._save_pcap()
        blob = open(pcap_path, "rb").read()
    finally:
        filedialog.asksaveasfilename = real_dialog
        os.remove(pcap_path)
    magic, vmaj, vmin, _, _, _, link = struct.unpack("<IHHiIII", blob[:24])
    check((magic, vmaj, vmin, link) == (0xA1B2C3D4, 2, 4, 1),
          "Save .pcap writes an Ethernet capture Wireshark can open")
    off, saved = 24, []
    while off + 16 <= len(blob):
        sec, usec, incl, _orig = struct.unpack("<IIII", blob[off:off + 16])
        saved.append((sec * 1_000_000 + usec, blob[off + 16:off + 16 + incl]))
        off += 16 + incl
    check(off == len(blob) and [s[1] for s in saved] == [f_in, f_out],
          "...holding the captured frames byte for byte, in order")
    check(saved[0][0] == 1_500_000 and saved[1][0] == 1_600_000,
          "each stamped with the device time it crossed")

    print("== double-clicking a line opens the bytes behind THAT line ==")
    check(SimulatorUI._filter_from_line(
        "[NET] TCP 192.168.1.100:40127 <-> 10.200.0.2:1883 sends: MQTT CONNECT") == ":40127",
        "a connection line narrows to its own port, which is unique to it")
    check(SimulatorUI._filter_from_line("[NET] DHCP: device got 192.168.1.100") == "192.168.1.100",
          "a line with no port falls back to the address it names")
    check(SimulatorUI._filter_from_line("[WIFI] device acts as a station") == "",
          "and a line naming neither shows everything rather than hiding frames")
    ui.var_frame_filter.set("")
    ui._refresh_details()
    ui._details_for_click(None)
    check(len(ui.frame_tree.get_children()) == 2,
          "opening the window without a specific line still shows every frame")

    print("== the packet list refreshing during a run ==")
    ui.frame_tree.selection_set("1")
    ui._show_frame_bytes()
    before = ui.txt_bytes.get("1.0", "end-1c")
    ui._frame_sink("from device", f_in, 1_700_000)       # more traffic arrives
    ui._refresh_details()
    check(ui.frame_tree.selection() == ("1",),
          "a refresh keeps the frame the reader had selected")
    check(ui.txt_bytes.get("1.0", "end-1c") == before,
          "...and leaves its bytes on screen rather than jumping to the newest")
    check(len(ui.frame_tree.get_children()) == 3, "while still listing what has since arrived")

    print("== captured traffic target-host selector ==")
    remote_ip = netstack.str2ip("10.200.0.2")
    reply_seg = netstack.tcp_segment(remote_ip, dev_ip, 1883, 40127, 1, 2,
                                     netstack.TCP_ACK, 8192)
    f_reply = netstack.eth_frame(dev, ours, netstack.ETH_IP, reply_seg)
    near_ip = netstack.str2ip("10.200.0.20")
    near_seg = netstack.tcp_segment(dev_ip, near_ip, 40128, 1883, 1, 0,
                                    netstack.TCP_SYN, 8192)
    f_near = netstack.eth_frame(ours, dev, netstack.ETH_IP, near_seg)
    ui._frame_sink("to device", f_reply, 1_800_000)
    ui._frame_sink("from device", f_near, 1_900_000)
    ui._refresh_details()
    choices = tuple(ui.frame_target.cget("values"))
    check(choices == (ALL_FRAME_TARGETS, "10.200.0.2", "10.200.0.20", "192.168.1.1"),
          "the target list contains each exact remote IPv4 host once, in numeric order")
    ui.var_frame_target.set("10.200.0.2")
    ui.var_frame_filter.set("")
    ui._refresh_details()
    check(len(ui.frame_tree.get_children()) == 3,
          "selecting a target keeps both traffic directions and repeated frames")
    ui.var_frame_filter.set("PSH")
    ui._refresh_details()
    check(len(ui.frame_tree.get_children()) == 2,
          "the exact target selection combines with the existing text filter")
    special = [
        ("from device", netstack.eth_frame(ours, dev, netstack.ETH_IP,
                                            netstack.udp_packet(dev_ip, bytes(4), 1, 1, b"")), 0),
        ("from device", netstack.eth_frame(ours, dev, netstack.ETH_IP,
                                            netstack.udp_packet(dev_ip, b"\xff" * 4, 1, 1, b"")), 0),
        ("from device", netstack.eth_frame(ours, dev, netstack.ETH_IP,
                                            netstack.udp_packet(dev_ip, netstack.str2ip("239.1.2.3"),
                                                                1, 1, b"")), 0),
    ]
    check(ui._target_hosts(special) == [],
          "unspecified, broadcast and multicast endpoints are not offered as target hosts")
    ui.var_frame_target.set(ALL_FRAME_TARGETS)
    ui.var_frame_filter.set("")
    ui._refresh_details()

    ui._clear()
    check(not ui.frame_tree.get_children() and "Nothing captured yet" in ui.var_frame_count.get()
          and tuple(ui.frame_target.cget("values")) == (ALL_FRAME_TARGETS,),
          "Clear empties the captured frames and target hosts as well as the pane")
    ui._detail_win.destroy()

    # --- Config tab -------------------------------------------------------
    ui._reset_options()
    cfg = ui._collect_cfg()
    check(cfg["wifi"] is True and cfg["tuyamcu"] is False and cfg["boot"] is False
          and cfg["xvr_selfclear"] is False and cfg["blecore"] is False
          and cfg["tuyamcu_pid"] is None and cfg["tuyamcu_raw"] is False
          and cfg["net_out"] is True and cfg["forwards"] == [(8080, 80)],
          "the Config defaults attach Wi-Fi with real outbound access")
    check(guess_chip("OpenBK7231U_QIO.bin") == "BK7231U"
          and guess_chip("OpenBK7231T_QIO.bin") == "BK7231",
          "chip guessing distinguishes U's extended GPIO bank from generic BK7231")
    check(str(ui.opt_text["http_gets"].cget("state")) == "normal"
          and str(ui.opt_widgets["pcap"][0].cget("state")) == "disabled"
          and str(ui.opt_widgets["tuyamcu"][0].cget("state")) == "disabled",
          "default Wi-Fi fields are active while capture and TuyaMCU fields stay gated")
    check(FRAME_PRIVACY_NOTE in ui.lbl_radio_capture_note.cget("text"),
          "the radio-PCAP option carries the same clear-credential warning")
    check(str(ui.btn_open_forward.cget("state")) == "disabled",
          "Open in browser waits until a configured forward is actually running")
    check(ui.var_summary.get() == "options: Wi-Fi peer (auto, real internet, open, :8080 to :80)",
          "the summary line shows the default Wi-Fi peer")

    print("== interesting-dump quick runs ==")
    labels = [button.cget("text") for button in ui.interesting_buttons]
    check(labels == [profile["label"] for profile in INTERESTING_DUMPS]
          and {"OBK T", "OBK N", "OBK 7238", "MOES relay",
               "OBK sleep", "PIR sleep", "Rain sleep", "TH01 sleep",
               "HA15 cam", "Pinmei cam",
               "GBDQA breaker", "IR blaster", "LSC bulb"}
          <= set(labels),
          "the launcher exposes the requested OpenBeken, relay, sleep, camera and paired-cloud profiles")
    check([label.cget("text") for label in ui.interesting_group_labels]
          == ["Quick run:", "Real Tuya reconnect (owned IDs only; sends to cloud):"],
          "proven paired callers have their own clearly labelled launcher row")
    check(all(os.path.isfile(os.path.join(FIRMWARE_DIR, p["file"]))
              and p["chip"] in CHIPS
              and (p["key"] == "(none)" or p["key"] in KNOWN_KEYS)
              for p in INTERESTING_DUMPS),
          "every quick-run profile names a bundled image and a valid chip/key combination")
    # Release builds deliberately contain only what the launcher needs rather
    # than every large regression fixture.  Keep that explicit manifest in
    # lockstep with the default image and quick-run profiles; the packaged
    # smoke test below then proves PyInstaller actually collected every entry.
    release_manifest = os.path.join(ROOT_DIR, "tools", "release_firmwares.txt")
    if os.path.isfile(release_manifest):
        with open(release_manifest, encoding="utf-8") as manifest_file:
            release_files = {
                line.strip() for line in manifest_file
                if line.strip() and not line.lstrip().startswith("#")
            }
        expected_release_files = ({os.path.basename(DEFAULT_IMAGE)}
                                  | {p["file"] for p in INTERESTING_DUMPS})
        check(release_files == expected_release_files,
              "the Windows release firmware manifest exactly matches the launcher")
    old_image, old_chip, old_key = (ui.var_image.get(), ui.var_chip.get(), ui.var_key.get())
    old_wifi = {name: ui.opt[name].get()
                for name in ("wifi", "net_out", "wifi_role", "wifi_ssid", "wifi_pass")}
    started = []
    real_run, real_refresh = ui._on_run, ui._refresh_tuya_config
    ui._on_run = lambda: started.append(
        (ui.var_image.get(), ui.var_chip.get(), ui.var_key.get(),
         ui.opt["wifi"].get(), ui.opt["net_out"].get(), ui.opt["wifi_role"].get(),
         ui.opt["wifi_ssid"].get(), ui.opt["wifi_pass"].get()))
    ui._refresh_tuya_config = lambda *args, **kwargs: None
    try:
        paired_profiles = [p for p in INTERESTING_DUMPS if p.get("paired")]
        for paired in paired_profiles:
            # Prove the shortcut is not poisoned by settings retained from a
            # previous manual run.
            ui.opt["wifi"].set(False)
            ui.opt["net_out"].set(False)
            ui.opt["wifi_role"].set("sta")
            ui.opt["wifi_ssid"].set("wrong-network")
            ui.opt["wifi_pass"].set("wrongpass")
            ui._run_interesting(paired)
        paired_runs = started[-len(paired_profiles):]
        check(len(paired_profiles) >= 3
              and all(run == (os.path.join(FIRMWARE_DIR, profile["file"]),
                              profile["chip"], profile["key"], True, True, "auto", "", "")
                      for profile, run in zip(paired_profiles, paired_runs)),
              "every paired-cloud shortcut selects its exact profile and a known-good "
              "real-cloud relay, then runs")
        moes = next(p for p in INTERESTING_DUMPS if p["label"] == "MOES relay")
        ui._run_interesting(moes)
        check(started[-1][0] == os.path.join(FIRMWARE_DIR, moes["file"])
              and started[-1][1:3] == ("BK7231", "(none)") and started[-1][4] is True,
              "ordinary shortcuts inherit the real-internet GUI default too")
    finally:
        ui._on_run, ui._refresh_tuya_config = real_run, real_refresh
        ui.var_image.set(old_image)
        ui.var_chip.set(old_chip)
        ui.var_key.set(old_key)
        for name, value in old_wifi.items():
            ui.opt[name].set(value)

    cfg = ui._collect_cfg()
    check(cfg["wifi_role"] == "auto" and cfg["forwards"] == [(8080, 80)]
          and cfg["net_out"] is True
          and cfg["wifi_ssid"] is None and cfg["wifi_pass"] is None and cfg["pcap"] is None
          and cfg["http_gets"] == [] and cfg["mqtt_pubs"] == [],
          "the default Wi-Fi peer uses auto role and forwards 8080 to 80")
    check(str(ui.opt_text["http_gets"].cget("state")) == "normal"
          and str(ui.opt_widgets["pcap"][0].cget("state")) == "disabled",
          "default Wi-Fi opens its fields, but the capture file waits for its own tick")

    ui._set_text_option("mqtt_pubs", "cmnd/obk/POWER=ON\n\n  stat/obk/x=a=b  \n")
    ui._set_text_option("http_gets", "/cm?cmnd=POWER%20ON\n \n/index\n")
    cfg = ui._collect_cfg()
    check(cfg["mqtt_pubs"] == [("cmnd/obk/POWER", b"ON"), ("stat/obk/x", b"a=b")],
          "MQTT lines parse as TOPIC=PAYLOAD; blank lines are skipped, '=' stays in a payload")
    check(cfg["http_gets"] == ["/cm?cmnd=POWER%20ON", "/index"],
          "HTTP GET paths are one per line, blank lines skipped")

    def rejects(fragment, label):
        try:
            ui._collect_cfg()
        except ValueError as e:
            check(fragment in str(e), label)
        else:
            check(False, label)

    ui._set_text_option("mqtt_pubs", "no-equals-sign")
    rejects("TOPIC=PAYLOAD", "an MQTT line without '=' is refused before the run starts")
    ui._set_text_option("mqtt_pubs", "")
    ui._set_text_option("http_gets", "/café\n/żółw")
    rejects("percent-encode", "an HTTP path a request line cannot carry is refused, not "
                              "left to end the run")
    ui._set_text_option("http_gets", "/cm?cmnd=POWER%20ON\n \n/index\n")
    ui.opt["fwd_local"].set("80x")
    rejects("port", "a port forward that is not a number is refused")
    for lp, dp in (("0", "80"), ("70000", "80"), ("8080", "")):
        ui.opt["fwd_local"].set(lp)
        ui.opt["fwd_device"].set(dp)
        rejects("ports 1-65535", "a port forward of %r to %r is refused" % (lp, dp))
    ui.opt["fwd_local"].set("")
    ui.opt["fwd_device"].set("")
    check(ui._collect_cfg()["forwards"] == [], "clearing both forward ports turns the forward off")
    ui.opt["wifi_pass"].set("short")
    rejects("8 to 63", "a WPA2 passphrase under 8 characters is refused")
    ui.opt["wifi_pass"].set("x" * 64)
    rejects("8 to 63", "a WPA2 passphrase over 63 characters is refused")
    ui.opt["wifi_pass"].set(" pass word ")
    check(ui._collect_cfg()["wifi_pass"] == " pass word ",
          "a passphrase keeps its spaces: they are part of the key")
    ui.opt["wifi_ssid"].set(" Home Net ")
    check(ui._collect_cfg()["wifi_ssid"] == " Home Net ",
          "a network name keeps its spaces: it is matched exactly and salts the WPA2 key")
    ui.opt["wifi_ssid"].set("   ")
    check(ui._collect_cfg()["wifi_ssid"] is None,
          "a network name of only spaces means 'adopt the one the device asks for'")
    ui.opt["pcap"].set(True)
    rejects("no file", "radio capture without a file is refused")
    check(str(ui.opt_widgets["pcap"][0].cget("state")) == "normal",
          "ticking radio capture opens its file field")
    ui.opt["pcap_path"].set(os.path.join(tempfile.gettempdir(), "no_such_dir_bks", "x.pcap"))
    rejects("does not exist", "a capture file in a folder that does not exist is refused")
    ui.opt["pcap_path"].set("")
    ui.opt["wifi"].set(False)
    check(str(ui.opt_widgets["pcap"][0].cget("state")) == "disabled",
          "the capture file field closes again with the Wi-Fi peer")
    box = ui.opt_text["http_gets"]
    check(str(box.cget("background")) != str(box.configure("background")[3]),
          "a disabled text box is drawn greyed, not as an editable white box")
    ui.opt["wifi"].set(True)
    check(str(box.cget("background")) == str(box.configure("background")[3]),
          "...and drawn normally again once enabled")
    ui.opt["tuyamcu"].set(True)
    ui.opt["tuyamcu_pid"].set("żółw")
    rejects("ASCII", "a product ID that is not ASCII is refused, not left to end the run")
    ui.opt["tuyamcu_pid"].set("")
    ui.opt["tuyamcu"].set(False)

    ui.opt["tuyamcu"].set(True)
    ui.opt["tuyamcu_pid"].set("  abcdefghijklmnop ")
    ui.opt["tuyamcu_raw"].set(True)
    ui.opt["pcap"].set(False)
    ui.opt["wifi_ssid"].set("HomeNet")
    summary = ui.var_summary.get()
    check("TuyaMCU (abcdefghijklmnop, raw)" in summary and "'HomeNet'" in summary
          and "Wi-Fi peer (auto" in summary, "the summary line follows the choices")
    check("WPA2" in summary and "2 HTTP GET" in summary,
          "it names WPA2 when a passphrase is set, and counts the HTTP requests")
    ui.opt["net_out"].set(True)
    check("real internet" in ui.var_summary.get(),
          "the summary makes an externally connected run unmistakable")
    ui.opt["net_out"].set(False)
    ui.opt["wifi_pass"].set("")
    check("open" in ui.var_summary.get() and "WPA2" not in ui.var_summary.get(),
          "...and says 'open' without one, which is what a relaunch leaves")
    ui._set_text_option("mqtt_pubs", "a/b=1")
    root.update()                        # <<Modified>> is queued, as when typing
    check("1 MQTT" in ui.var_summary.get(),
          "typing in a text box updates the summary too, though it has no variable")
    ui._set_text_option("mqtt_pubs", "")
    ui.opt["wifi_pass"].set(" pass word ")
    cfg = ui._collect_cfg()
    check(cfg["tuyamcu_pid"] == "abcdefghijklmnop" and cfg["tuyamcu_raw"] is True,
          "the TuyaMCU product ID is trimmed and passed on with the raw flag")

    # The worker hands every option to the same calls the command line makes.
    calls = {}

    class FakeEmu:
        def __init__(self):
            self.run_error = calls.get("run_error")

        def attach_wifi_peer(self, **kw):
            calls["peer"] = kw
            if calls.get("attach_fails"):
                raise OSError("cannot open the capture")

        def setup(self):
            calls["order"] = ["setup"]

        def stop(self):
            calls["order"].append("stop")

        def run(self):
            calls["order"].append("run")

        def close_network(self):
            calls["closed"] = calls.get("closed", 0) + 1

    def fake_build(path, **kw):
        calls["build"] = kw
        return FakeEmu()

    fd, img = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    real_build = globals()["build_emulator"]
    globals()["build_emulator"] = fake_build
    try:
        ui.opt["xvr_selfclear"].set(True)
        ui.opt["wifi_role"].set("ap")
        ui.opt["fwd_local"].set("9090")
        ui.opt["fwd_device"].set("80")
        ui.opt["wifi_pass"].set("password1")
        ui.opt["pcap"].set(True)
        ui.opt["pcap_path"].set("cap.pcap")
        ui.opt["net_out"].set(False)
        ui._set_text_option("mqtt_pubs", "cmnd/obk/POWER=OFF")
        cfg = ui._collect_cfg()
        cfg["tls_psks"] = (b"synthetic-gui-cloud-key",)
        cfg["tuya_legacy_identity_psk"] = True
        ui._stop_requested = False
        ui._worker("image.bin", cfg)
        b, p = calls["build"], calls["peer"]
        check(b["xvr_selfclear"] is True and b["tuyamcu_pid"] == "abcdefghijklmnop"
              and b["tuyamcu_raw"] is True and b["wifi"] is True and b["only_uart"] is True
              and b["ble_core"] is None,
              "the worker passes device options and leaves BLE hardware selection automatic")
        check(p == {"role": "ap", "ssid": "HomeNet", "passphrase": "password1",
                    "forwards": [(9090, 80)], "net_out": False,
                    "pcap": "cap.pcap", "http_gets": ["/cm?cmnd=POWER%20ON",
                    "/index"], "mqtt_pubs": [("cmnd/obk/POWER", b"OFF")],
                    "tls_psks": (b"synthetic-gui-cloud-key",),
                    "tuya_legacy_identity_psk": True},
              "the worker passes offline network options and private TLS mode")
        check(calls["order"] == ["setup", "run"] and ui.finished == "emulation finished",
              "a run nobody stopped goes straight from setup to run")
        check(calls["closed"] == 1, "the run's network is released when it ends, freeing the port")

        ui.emu = None
        ui._on_stop()
        check(ui._stop_requested is True, "Stop with nothing loaded yet is remembered")
        ui._worker("image.bin", cfg)
        check(calls["order"] == ["setup", "stop", "run"],
              "a Stop pressed while the image was loading reaches the emulator before it runs")

        ui.var_image.set(img)
        ui._pending_run = True
        real_refresh = ui._refresh_tuya_config
        private_cfg = {"tls_mode": 2, "ids": {"psk_key": "A" * 37}}

        def refresh_private(*_args, **_kwargs):
            ui.tuya_config = private_cfg
            return private_cfg

        ui._refresh_tuya_config = refresh_private
        try:
            ui._on_run()
        finally:
            ui._refresh_tuya_config = real_refresh
        ui.thread.join(5)
        check(calls["order"] == ["setup", "run"] and ui._stop_requested is False,
              "Run clears a Stop left over from the previous run")
        check(ui._pending_run is False, "...and uses up a dropped image that was waiting")
        check(calls["peer"]["tls_psks"] == (b"A" * 37,)
              and calls["peer"]["tuya_legacy_identity_psk"] is True
              and ui._run_tuya_config is private_cfg,
              "Run snapshots the selected image's private PSK mode for its offline TLS peer")
        check(ui._active_forward_url == "http://127.0.0.1:9090/",
              "Run binds Open in browser to that run's validated local port")

        ui.opt["net_out"].set(True)
        ui._on_run()
        ui.thread.join(5)
        check(calls["peer"]["net_out"] is True
              and calls["peer"]["tls_psks"] == ()
              and calls["peer"]["tuya_legacy_identity_psk"] is False,
              "a real-cloud Run passes no PSK copy to its transparent network relay")
        ui.opt["net_out"].set(False)

        ui.finished = ui.error = None
        calls["run_error"] = "UC_ERR_FETCH_UNMAPPED at pc 0x00000000"
        ui._worker("image.bin", cfg)
        check(ui.error == calls["run_error"] and ui.finished is None,
              "a run that ended on an error is reported as one, not as a normal finish")
        calls["run_error"] = None
        ui.error = None
        calls["attach_fails"] = True
        closed = calls["closed"]
        ui._worker("image.bin", cfg)
        check("OSError" in (ui.error or "") and calls["closed"] == closed + 1,
              "a peer that fails to start still has its network released")
        calls["attach_fails"] = False
    finally:
        globals()["build_emulator"] = real_build
        ui.emu = None
        ui.thread = None
        ui.finished = ui.error = None
        ui._stop_requested = ui._pending_run = False
        ui._active_forward_url = None
        ui.btn_open_forward.config(state="disabled")
        os.remove(img)

    # Enter in the send box runs _on_send whatever the button says, so the run
    # being over has to be checked there.
    class Feed:
        fed = []

        def feed_uart1(self, data):
            self.fed.append(data)

    ui.emu = Feed()
    history = len(ui.history)
    ui.var_input.set("help")
    ui._on_send()
    check(Feed.fed == [] and len(ui.history) == history and ui.var_input.get() == "help",
          "after a run has ended, Enter sends nothing and echoes nothing")
    ui.emu = None
    ui.var_input.set("")

    # Console readiness follows the running emulator's own RX hold-off, which
    # build_emulator drops to 0 when a TuyaMCU peer is on.
    from types import SimpleNamespace

    class Alive:                         # a worker thread that is still going
        def is_alive(self):
            return True

    print("== GPIO inputs: pressing a real emulated pad from the table ==")

    class GPIOEmu:
        def __init__(self):
            self.state = SimpleNamespace(
                # Pull-up input, pull-down input, output, second function.
                gpio_cfg={6: 0x3C, 7: 0x2C, 18: 0x02, 24: 0x78}, pwm_regs={})
            self.inputs = []
            self.stopped = False

        def set_gpio_input(self, pin, level):
            self.inputs.append((pin, level))

        def stop(self):
            self.stopped = True

    gpio_emu = GPIOEmu()
    ui.emu, ui.thread = gpio_emu, Alive()
    ui._stop_requested = False
    ui.tuya_config = None
    ui._periph_key = None
    ui._refresh_periph()
    check(all(ui.tree.exists("gpio:%d" % p) for p in (6, 7, 18, 24)),
          "configured GPIO rows have stable pin identities")

    ui.tree.selection_set("gpio:6")
    ui._gpio_selection_changed()
    check(str(ui.btn_gpio_press.cget("state")) == "normal"
          and ui.btn_gpio_press.cget("text") == "Press P6"
          and "drives LOW" in ui.var_gpio_action.get(),
          "a pull-up input offers a LOW press")
    ui._toggle_gpio_input()
    check(gpio_emu.inputs[-1] == (6, 0)
          and ui.btn_gpio_press.cget("text") == "Release P6"
          and "PRESSED LOW" in ui.tree.item("gpio:6", "values")[2],
          "Press drives the external pad low and marks it as held")

    # A relay write redraws the table while the key is held.  The selected pin
    # and the release action must survive that redraw or the input gets stuck.
    gpio_emu.state.gpio_cfg[18] = 0x00
    ui._refresh_periph()
    check(ui.tree.selection() == ("gpio:6",)
          and ui.btn_gpio_press.cget("text") == "Release P6",
          "a relay change preserves the held input and its selection")
    ui._toggle_gpio_input()
    check(gpio_emu.inputs[-1] == (6, None) and 6 not in ui._gpio_pressed,
          "Release disconnects the external drive so the hardware pull-up wins again")

    ui.tree.selection_set("gpio:7")
    ui._gpio_selection_changed()
    ui._toggle_gpio_input()
    check(gpio_emu.inputs[-1] == (7, 1), "a pull-down input gets a HIGH press")
    ui._toggle_gpio_input()

    before_inputs = list(gpio_emu.inputs)
    ui.tree.selection_set("gpio:18")
    ui._gpio_selection_changed()
    ui._toggle_gpio_input()
    check(str(ui.btn_gpio_press.cget("state")) == "disabled"
          and gpio_emu.inputs == before_inputs,
          "an output row cannot be driven through the input control")
    ui.tree.selection_set("gpio:24")
    ui._gpio_selection_changed()
    check(str(ui.btn_gpio_press.cget("state")) == "disabled",
          "a peripheral-owned pad is not mislabelled as a push button")

    levels = ui._tuya_button_levels(
        {"pairs": [("bt1_pin", "6"), ("bt1_lv", "1"),
                   ("reset_pin", "9"), ("reset_lv", "0"),
                   ("rl1_pin", "18"), ("rl1_lv", "1")]})
    check(levels == {6: 1, 9: 0},
          "Tuya button active levels are used without treating relay metadata as a button")

    # The header may already point at the next dump while the current one is
    # still running.  Its wiring must not change the live device's polarity.
    ui._run_tuya_config = {"pairs": [("bt1_pin", "6"), ("bt1_lv", "0")]}
    ui.tuya_config = {"pairs": [("bt1_pin", "6"), ("bt1_lv", "1")]}
    check(ui._gpio_press_level(6, 0x3C)[0] == 0,
          "a newly browsed image cannot change the running image's button polarity")
    ui._run_tuya_config = None
    ui.tuya_config = None

    # BK7252 puts logical P32..P39 at physical slots 48..55.  The table and
    # action API must expose the logical number, without mistaking FUNC_CFG at
    # slot 32 for a pin.
    gpio_emu.chip = CHIPS["BK7252"]
    gpio_emu.state.gpio_cfg.update({32: 0xA5A5, 48: 0x3C})
    ui._run_image = "BK7252_upper_bank.bin"
    ui._periph_key = None
    ui._refresh_periph()
    check(ui.tree.exists("gpio:32")
          and ui.tree.item("gpio:32", "values")[3] == "0x3C",
          "BK7252 slot 48 is shown as logical input P32, not FUNC_CFG")
    ui.tree.selection_set("gpio:32")
    ui._gpio_selection_changed()
    ui._toggle_gpio_input()
    check(gpio_emu.inputs[-1] == (32, 0),
          "the BK7252 upper-bank Press action drives logical P32")
    ui._toggle_gpio_input()
    gpio_emu.chip = CHIPS["BK7231"]
    del gpio_emu.state.gpio_cfg[48]
    ui._run_image = None
    ui._periph_key = None
    ui._refresh_periph()

    ui.tree.selection_set("gpio:6")
    ui._gpio_selection_changed()
    ui._toggle_gpio_input()
    ui._on_stop()
    check(gpio_emu.inputs[-1] == (6, None) and not ui._gpio_pressed and gpio_emu.stopped,
          "Stop releases every held GPIO before stopping the emulator")
    ui.emu = ui.thread = None
    ui._stop_requested = False
    ui._gpio_selected = None
    ui._run_tuya_config = None
    ui._run_image = None
    ui.tree.selection_remove(ui.tree.selection())
    ui._update_gpio_control()

    ui.thread = Alive()
    listening = SimpleNamespace(
        net=SimpleNamespace(io=SimpleNamespace(listeners={object(): 80})))
    ui.emu = SimpleNamespace(uart1_rx_delay=0, state=SimpleNamespace(insn_count=10),
                             wifi_peer=listening)
    check(ui._console_ready(), "with no RX hold-off (TuyaMCU) the console is ready at once")
    ui.emu = SimpleNamespace(uart1_rx_delay=2_000_000, state=SimpleNamespace(insn_count=10),
                             wifi_peer=listening)
    check(not ui._console_ready(), "...and with OpenBeken's hold-off it waits for it")

    opened = []
    real_browser_open = webbrowser.open
    try:
        webbrowser.open = lambda url, new=0: opened.append((url, new)) or True
        ui._active_forward_url = "http://127.0.0.1:9090/"
        ui._stop_requested = False
        ui.emu.wifi_peer.net.io.listeners.clear()
        ui._update_open_forward()
        ui._open_forward()
        check(str(ui.btn_open_forward.cget("state")) == "disabled" and not opened,
              "Open in browser stays disabled when the requested port did not bind")
        ui.emu.wifi_peer.net.io.listeners[object()] = 80
        ui._update_open_forward()
        check(str(ui.btn_open_forward.cget("state")) == "normal",
              "Open in browser becomes available once the forward is running")
        ui._open_forward()
        check(opened == [("http://127.0.0.1:9090/", 2)],
              "Open in browser sends the active loopback URL to the default browser")
        ui._stop_requested = True
        ui._update_open_forward()
        ui._open_forward()
        check(str(ui.btn_open_forward.cget("state")) == "disabled" and len(opened) == 1,
              "Stop disables the browser action and a stale click opens nothing")
    finally:
        webbrowser.open = real_browser_open
        ui._active_forward_url = None
        ui._stop_requested = False
    ui.emu = ui.thread = None

    # Saved settings round-trip through a file, minus the passphrase. The
    # images are made here, so nothing depends on what firmwares/ holds.
    def window(**kw):
        win = tk.Toplevel(root)
        win.withdraw()
        return win, SimulatorUI(win, **kw)

    with tempfile.TemporaryDirectory() as tmpdir:
        spath = os.path.join(tmpdir, "ui.json")
        dump, other = os.path.join(tmpdir, "dump.bin"), os.path.join(tmpdir, "other.bin")
        for path in (dump, other):
            with open(path, "wb") as f:
                f.write(bytes(16))

        # Tuya config tab: extraction is cached per file revision, only the
        # redacted summary and user_param_key map reach widgets, and changing
        # images replaces (rather than mixes) the result.
        real_extract = tuya_config.extract
        decoded_calls = []

        def fake_extract(path):
            decoded_calls.append(path)
            if _same_file(path, dump):
                return {"offset": 0x1EE000, "encrypted": True,
                        "raw": '{"passwd":"must-not-reach-the-widget"}',
                        "pairs": [("rl1_pin", "6"), ("module", "CB2S")],
                        "network": {"ssid": "must-not-reach-the-widget"},
                        "dps": [{"id": 1}], "ids": {"auth_key": "secret"},
                        "human": [("Storage", "encrypted (decrypted with device key)"),
                                  ("Device key", "12****************************34"),
                                  ("Paired WiFi SSID", "Ho****et"),
                                  ("-", "Relay (channel 1) on P6")]}
            return None

        try:
            tuya_config.extract = fake_extract
            ui._tuya_key = None
            ui.var_image.set(dump)
            ui._refresh_tuya_config(force=True)
            rows = [ui.tuya_tree.item(i, "values") for i in ui.tuya_tree.get_children()]
            cfg_json = ui.txt_tuya_json.get("1.0", "end-1c")
            check(any(r[0] == "Paired WiFi SSID" and r[1] == "Ho****et" for r in rows)
                  and "12****************************34" in str(rows),
                  "the Tuya tab shows the decoder's masked summary")
            check('"rl1_pin": 6' in cfg_json and '"module": "CB2S"' in cfg_json,
                  "the Tuya tab formats user_param_key as decoded JSON")
            check("must-not-reach-the-widget" not in cfg_json + str(rows),
                  "raw network credentials never reach the Tuya tab")
            before = len(decoded_calls)
            ui._refresh_tuya_config()
            check(len(decoded_calls) == before,
                  "the Tuya decoder is cached while the selected file is unchanged")
            ui._load_image(other)
            empty_note = ("No recoverable" if tuya_config.AES is not None
                          else "Tuya config unavailable")
            check(ui.tuya_config is None and not ui.tuya_tree.get_children()
                  and empty_note in ui.var_tuya_status.get(),
                  "loading another image clears stale Tuya config")
        finally:
            tuya_config.extract = real_extract

        ui.settings_path = spath
        try:
            ui.var_image.set(dump)
            ui.var_chip.set("BK7238")
            ui.var_key.set("(none)")
            ui._save_settings()
            with open(spath, encoding="utf-8") as f:
                saved = json.load(f)
            check("wifi_pass" not in saved and "net_out" not in saved
                  and saved["wifi_ssid"] == "HomeNet"
                  and saved["mqtt_pubs"] == "cmnd/obk/POWER=OFF",
                  "settings are saved without the Wi-Fi passphrase or external-access grant")
            check(not os.path.exists(spath + ".tmp") and os.path.isabs(saved["image"]),
                  "the file is swapped in whole, and the image is saved as an absolute path")

            win2, ui2 = window(settings_path=spath)
            check(ui2.opt["wifi"].get() is True and ui2.opt["wifi_ssid"].get() == "HomeNet"
                  and ui2.opt["net_out"].get() is True
                  and ui2.opt["fwd_local"].get() == "9090" and ui2.opt["wifi_pass"].get() == ""
                  and ui2.opt["wifi_role"].get() == "ap"
                  and ui2.opt_text["mqtt_pubs"].get("1.0", "end-1c") == "cmnd/obk/POWER=OFF",
                  "a new window restores the saved choices, role and text boxes included")
            check(_same_file(ui2.var_image.get(), dump) and ui2.var_chip.get() == "BK7238"
                  and ui2.var_key.get() == "(none)",
                  "...and reopens the saved image with its saved chip and key")
            try:
                ui2.opt_text["mqtt_pubs"].edit_undo()
            except tk.TclError:
                pass                    # nothing to undo: what the check wants
            check(ui2.opt_text["mqtt_pubs"].get("1.0", "end-1c") == "cmnd/obk/POWER=OFF",
                  "Ctrl+Z cannot take a restored text box back to empty")
            win2.destroy()

            # A redundant dot component is another spelling of the same file
            # on every platform. Case and slash changes are equivalent only on
            # Windows; uppercasing a POSIX path can name a different file.
            same_dump = os.path.join(os.path.dirname(dump), ".", os.path.basename(dump))
            if os.name == "nt":
                same_dump = same_dump.replace("\\", "/").upper()
            win2, ui2 = window(initial_image=same_dump, settings_path=spath)
            check(ui2.var_chip.get() == "BK7238" and ui2.var_key.get() == "(none)",
                  "the same image spelled differently still gets its chip and key back")
            win2.destroy()
            win2, ui2 = window(initial_image=other, settings_path=spath)
            check(ui2.var_image.get() == other and ui2.var_chip.get() == guess_chip(other)
                  and ui2.var_key.get() == "TUYA",
                  "the saved chip and key stay with the saved image, not one named at launch")
            win2.destroy()

            with open(spath, "w", encoding="utf-8") as f:
                f.write('{"wifi": "yes", "wifi_role": "mesh", "fwd_local": 8080, "boot": true')
            win3, ui3 = window(settings_path=spath)
            check(ui3.opt["wifi"].get() is True and ui3.opt["boot"].get() is False,
                  "a corrupt settings file means defaults, not a crash")
            with open(spath, "w", encoding="utf-8") as f:
                json.dump({"wifi": "yes", "wifi_role": "mesh", "fwd_local": 8080, "boot": True}, f)
            ui3._apply_settings(ui3._load_settings())
            check(ui3.opt["wifi"].get() is True and ui3.opt["wifi_role"].get() == "auto"
                  and ui3.opt["fwd_local"].get() == "8080" and ui3.opt["boot"].get() is True,
                  "saved values of the wrong type or out of range are ignored one by one")
            ui3._apply_settings({"wifi_role": "sta", "wifi_pass": "password1",
                                 "net_out": False})
            check(ui3.opt["wifi_role"].get() == "sta" and ui3.opt["wifi_pass"].get() == ""
                  and ui3.opt["net_out"].get() is True,
                  "a valid saved role is restored; secrets and stale network policy never are")
            with open(spath, "w", encoding="utf-8") as f:
                f.write("[]")
            check(ui3._load_settings() == {},
                  "a settings file that is valid JSON but not an object means defaults")
            win3.destroy()
            with open(spath, "w", encoding="utf-8") as f:
                json.dump({"image": dump, "chip": ["BK7231"], "key": {"x": 1}}, f)
            win3, ui3 = window(settings_path=spath)
            check(ui3.var_chip.get() == guess_chip(dump) and ui3.var_key.get() == "TUYA",
                  "a chip or key of the wrong type is ignored instead of stopping the window")
            win3.destroy()
        finally:
            ui.settings_path = None
    ui._save_settings()                  # no settings file: must neither write nor raise

    ui._reset_options()
    check(ui.opt_text["mqtt_pubs"].get("1.0", "end-1c") == ""
          and ui.opt["wifi"].get() is True and ui.opt["net_out"].get() is True
          and ui.opt["wifi_pass"].get() == "",
          "Reset to defaults restores real-internet Wi-Fi and clears optional values")

    # Dropping a file: during a run it stops that run and waits for it to end,
    # then starts; while idle it starts at once.
    started = []
    real_run = ui._on_run
    ui._on_run = lambda: started.append(ui.var_image.get())
    fd, dropped = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        ui.tabs.select(ui.config_tab)
        ui.thread = Alive()
        ui._on_drop(SimpleNamespace(data="{%s}" % dropped, action="copy"))
        check(ui.var_image.get() == dropped and ui._pending_run is True
              and ui._stop_requested is True and started == [],
              "a file dropped during a run stops that run and waits for it to end")
        check(ui.tabs.select() == str(ui.run_tab), "...showing the Run tab")
        ui._pending_run = ui._stop_requested = False
        ui._on_drop(SimpleNamespace(data="{%s}" % os.path.join(tmpdir, "gone.bin"),
                                    action="copy"))
        check(ui._pending_run is False and ui._stop_requested is False and started == [],
              "a drop holding no file is ignored")
        ui.thread = None
        ui._on_drop(SimpleNamespace(data="{%s}" % dropped, action="copy"))
        check(started == [dropped], "a file dropped while idle runs at once")

        started.clear()
        ui._pending_run = True
        ui._finish()
        check(len(started) == 1 and ui._pending_run is False,
              "a waiting dropped image starts as soon as the run before it has ended")
        ui._finish()
        check(len(started) == 1, "and only once")
    finally:
        ui._on_run = real_run
        ui.thread = None
        ui._pending_run = ui._stop_requested = False
        os.remove(dropped)

    # An error is handled only once the worker has gone: _finish may start a
    # dropped image, which Run refuses while a thread is alive.
    finishes = []
    real_finish = ui._finish
    ui._finish = lambda: finishes.append(True)
    try:
        ui.thread, ui.error = Alive(), "boom"
        ui._update_state()
        check(finishes == [] and ui.error == "boom",
              "a worker error waits for its thread to exit before the run is finished")
        ui.thread = None
        ui._update_state()
        check(finishes == [True] and ui.error is None, "...and is reported once it has")
    finally:
        ui._finish = real_finish
        ui.thread = ui.error = None

    # LED driver color swatch and Energy Meter (BL0942 UART / BL0937 Pulse) UI:
    try:
        lsc_cfg = {
            "pairs": [
                ("dmod", "7"), ("cmod", "rgbcw"),
                ("iicscl", "26"), ("iicsda", "24"),
                ("iicr", "0"), ("iicg", "1"), ("iicb", "2"),
                ("iicw", "3"), ("iicc", "4"),
                ("2235ccur", "2"), ("2235wcur", "4"),
            ]
        }
        sm2235_raw = bytes.fromhex("d82400000000000003a20000")
        bp5758_raw = bytes.fromhex("b01f0808081e1ebcbfbcbfbcbfa0a0a0a0")
        ws_spi = (b"\xf8" * 8 + b"\xc0" * 16) * 4    # 4 green WS2812 pixels

        ui.tuya_config = lsc_cfg
        ui._run_tuya_config = lsc_cfg
        ui.emu = SimpleNamespace(
            state=SimpleNamespace(
                gpio_cfg={24: 0x02, 26: 0x02},
                pwm_regs={},
                i2c_frames={(26, 24): sm2235_raw},
                i2c_last_on={(26, 24): sm2235_raw},
                ws2812_spi=b"",
                bl0942_polls=3,
            ),
            meter_v=230.0, meter_a=1.5, meter_w=345.0,
            set_energy_meter=lambda v, a, w, **kw: setattr(ui.emu, "_meter_set", (v, a, w, kw)),
        )
        ui._periph_key = None
        ui._refresh_periph()
        check("SM2235" in ui.var_led.get() and "W:91%" in ui.var_led.get()
              and ui.led_swatch.cget("background") != periph.LED_OFF_HEX,
              "SM2235 I2C frame decodes Warm White 91% and lights the color swatch")
        check("BL0942 UART1 (3 polls)" in ui.var_meter.get(),
              "BL0942 UART1 poll count and V/A/W readings show in the meter summary")

        ui.var_meter_v.set("240.5")
        ui.var_meter_a.set("2.25")
        ui.var_meter_w.set("541.0")
        ui._apply_energy_meter()
        check(getattr(ui.emu, "_meter_set", None)[:3] == (240.5, 2.25, 541.0),
              "Set V/A/W pushes updated voltage, current and power into the emulator")

        ui.tuya_config = None
        ui._run_tuya_config = None
        ui.emu.state.i2c_frames = {(24, 26): bp5758_raw}
        ui.emu.state.i2c_last_on = {(24, 26): bp5758_raw}
        ui._periph_key = None
        ui._refresh_periph()
        check("BP5758D" in ui.var_led.get() and "8/8/8/30/30 mA" in ui.var_led.get()
              and "R:100% G:100% B:100%" in ui.var_led.get(),
              "BP5758D I2C frame decodes per-channel mA currents and 100% RGB white")

        ui.emu.state.i2c_frames = {}
        ui.emu.state.i2c_last_on = {}
        ui.emu.state.ws2812_spi = ws_spi
        ui._periph_key = None
        ui._refresh_periph()
        check("WS2812 SPI" in ui.var_led.get() and "4 LEDs" in ui.var_led.get()
              and "G:100%" in ui.var_led.get(),
              "WS2812 SPI DMA buffer decodes pixel count and RGB color onto the swatch")

        # Deep sleep status & wake controls:
        wakes = []
        ui.thread = Alive()
        ui.started_at = time.time()
        ui.emu = SimpleNamespace(
            state=SimpleNamespace(
                gpio_cfg={16: 0x0C, 20: 0x0C},
                pwm_regs={},
                i2c_frames={},
                i2c_last_on={},
                ws2812_spi=b"",
                bl0942_polls=0,
                in_deep_sleep=True,
                deep_sleep_count=1,
                deep_sleep_rtc_sec=28800.0,
                deep_sleep_wake_pins={16: 1, 20: 0},
                last_wake_reason=None,
                last_wake_pin=None,
                insn_count=500000,
                last_pc=0x10000,
            ),
            uart1_rx_delay=0,
            uart1_rx=bytearray(),
            is_paused=lambda: False,
            wake_from_deep_sleep=lambda reason="rtc", pin=None: (
                wakes.append((reason, pin)),
                setattr(ui.emu.state, "in_deep_sleep", False),
                setattr(ui.emu.state, "last_wake_reason", reason),
                setattr(ui.emu.state, "last_wake_pin", pin),
            ),
            set_gpio_input=lambda pin, lv: wakes.append(("pad", pin, lv)),
        )
        ui._periph_key = None
        ui._refresh_periph()
        ui._update_state()
        check("DEEP SLEEP #1 (RTC=28800s, GPIO=[P16:HIGH, P20:LOW])" == ui.var_sleep.get()
              and ui.var_state.get().startswith("DEEP SLEEP")
              and str(ui.btn_wake_rtc.cget("state")) == "normal"
              and str(ui.btn_wake_gpio.cget("state")) == "normal"
              and ui.btn_wake_gpio.cget("text") == "Wake P16",
              "Deep sleep state displays RTC timer and GPIO wake pins and enables Wake buttons")
        ui.tree.selection_set("gpio:20")
        ui._gpio_selection_changed()
        check(ui.btn_wake_gpio.cget("text") == "Wake P20"
              and "deep-sleep wake pin (active LOW)" in ui.var_gpio_action.get(),
              "selecting a wake-enabled GPIO updates Wake button and Press prompt")
        ui._wake_deep_sleep("gpio")
        check(wakes[-1] == ("gpio", 20)
              and "woke #1 via GPIO P20" in ui.var_sleep.get()
              and str(ui.btn_wake_rtc.cget("state")) == "disabled",
              "Wake GPIO triggers emulator wakeup on the selected wake pin and updates status")

        # Camera sensor preview & EJPEG frame status:
        test_rgb = b"".join(bytes((x * 3 & 0xFF, y * 5 & 0xFF, (x + y) * 2 & 0xFF))
                            for y in range(48) for x in range(64))
        ui.emu.state.camera_sensor = "GC0328C"
        ui.emu.state.camera_width = 640
        ui.emu.state.camera_height = 480
        ui.emu.state.camera_i2c_writes = 374
        ui.emu.state.camera_frame_count = 12
        ui.emu.state.camera_last_jpeg = b"\xff\xd8" + b"\x00" * 1176 + b"\xff\xd9"
        ui.emu.state.camera_frame_rgb = test_rgb
        ui._periph_key = None
        ui._refresh_periph()
        check("Camera: GC0328C 640x480 — 12 frames (1180 B JPEG, 374 I2C reg)" == ui.var_camera.get()
              and len(ui.cam_canvas.find_all()) >= 1,
              "Camera widget displays GC0328C sensor resolution, JPEG frame count, and live preview")

        # SARADC analog input controls & live status:
        adc_pushed = []
        ui.emu.chip = SimpleNamespace(name="BK7231T", gpio_pins=32)
        ui.emu.set_adc_input = lambda ch, val: adc_pushed.append((ch, val))
        ui.var_adc_ch.set("3 (P23)")
        ui.var_adc_val.set("512")
        ui._apply_adc_input()
        check(adc_pushed == [(3, 512)] and ui._adc_overrides.get(3) == 512
              and "ADC ch3(P23) set to 512" in ui.var_state.get(),
              "Set ADC parses channel and sample value and pushes to the running emulator")
        ui.emu.state.saradc_channel = 3
        ui.emu.state.saradc_last_raw = 512
        ui.emu.state.saradc_conversions = 4
        ui.emu.state.saradc_reads = 4
        ui.emu.state.adc_samples = {3: 512}
        ui._periph_key = None
        ui._refresh_periph()
        check("ch3(P23)=512" in ui.var_adc.get() and "4 conv" in ui.var_adc.get(),
              "ADC widget shows live SARADC channel, raw sample, voltage and conversion count")
    finally:
        ui.emu = None
        ui.thread = None
        ui.tuya_config = None
        ui._run_tuya_config = None
        ui._adc_overrides.clear()

    root.destroy()
    print("\nAll %d simulator UI self-tests passed." % n)
    return n


def _run_package_smoke():
    """Exercise the two failure modes unique to a frozen Windows build.

    A normal PyInstaller Windows bootloader enables Control Flow Guard, while
    Unicorn's generated control flow requires it disabled.  Returning from a
    code hook is enough to expose that incompatibility as process status
    0xC0000409.  The file checks catch a wrong frozen resource root or an
    incomplete --add-data manifest before an executable reaches a release.
    """
    from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM

    base = 0x1000
    code = b"\x00\x00\xa0\xe1" * 3       # ARM: mov r0, r0 (three times)
    seen = []
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(base, 0x1000)
    uc.mem_write(base, code)

    def on_code(_uc, address, size, _user):
        seen.append((address, size))

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.emu_start(base, base + len(code), count=3)
    if len(seen) != 3:
        raise AssertionError("Unicorn hook smoke test ran %d of 3 instructions" % len(seen))

    required = [DEFAULT_IMAGE]
    required.extend(os.path.join(FIRMWARE_DIR, profile["file"])
                    for profile in INTERESTING_DUMPS)
    missing = [os.path.basename(path) for path in required if not os.path.isfile(path)]
    if missing:
        raise AssertionError("release firmware is missing: %s" % ", ".join(missing))

    print("Packaged runtime smoke test passed: Unicorn hooks and %d firmware images."
          % len(set(required)))


def main():
    if "--package-smoke" in sys.argv[1:]:
        _run_package_smoke()
        return
    if "--selftest" in sys.argv[1:]:
        _run_selftests()
        return
    initial = sys.argv[1] if len(sys.argv) > 1 else None
    # Dropping files on the window needs a root made by tkinterdnd2 (pip
    # install tkinterdnd2); without it everything else works the same.
    try:
        from tkinterdnd2 import TkinterDnD
        root = TkinterDnD.Tk()
    except Exception:
        # TkinterDnD.Tk() makes its Tk before loading tkdnd, so a failed load
        # leaves that Tk behind as the default root, owning every variable the
        # window creates. Drop it before making the plain one.
        stray = getattr(tk, "_default_root", None)
        if stray is not None:
            stray.destroy()
        root = tk.Tk()
    SimulatorUI(root, initial)
    root.mainloop()


if __name__ == "__main__":
    main()
