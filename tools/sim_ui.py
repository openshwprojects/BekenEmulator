"""Tkinter front-end for the BK7231 emulator: two UART consoles you can type into.

Run it with no arguments (or point it straight at a dump):

    python tools/sim_ui.py
    python tools/sim_ui.py firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin

Pick any Beken flash dump, press Run, and watch UART1 and UART2 in their own
panes. Anything typed in the send bar goes into UART1's receive FIFO through
BekenEmulator.feed_uart1(), i.e. the same RX interrupt path --uart1-rx and the
simulated TuyaMCU peer use - the firmware cannot tell the difference.

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
import os
import sys
import threading
import time
import traceback

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, ROOT_DIR)

from src.crypto import KNOWN_KEYS, parse_key
from src.emulator import CHIP_FAMILIES
from src.main import build_emulator, parse_hex_blobs

FIRMWARE_DIR = os.path.join(ROOT_DIR, "firmwares")
# Image shipped with OBK config flag 31 already set, so its UART1 is a command
# console the moment it boots - the one dump where typing works out of the box.
DEFAULT_IMAGE = os.path.join(
    FIRMWARE_DIR, "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin")

POLL_MS = 60            # how often the UI drains the emulator's output buffer
MAX_LINES = 4000        # per pane; older lines are dropped so a long run is bounded

# The block hook raises the guest's periodic timer every 10000 instructions and
# that tick is 2 ms of device time, so a guest second costs 500 ticks. Used only
# to label the status bar - it is an estimate, like insn_count itself.
GUEST_SECOND_INSNS = 10000 * (1000 // 2)

# OpenBeken enables UART1 RX early but only registers its console callback
# around 2M instructions in; before that the ISR drops what it reads. The same
# number is build_emulator()'s default hold-off, so anything typed earlier is
# queued and delivered here rather than lost - the indicator just says when the
# firmware actually starts listening.
CONSOLE_READY_INSNS = 2_000_000

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
QUICK_COMMANDS = [
    "setChannel 1 1",
    "setChannel 1 5*2",
    "setChannel 1 $CH1+2",
    "addChannel 1 7",
    "setChannel 1 0",
    "toggleChannel 1",
    "getChannel 1",
]
QUICK_PER_ROW = 4       # they wrap rather than run off the end of the window

# Firmware log line the "suppress" checkbox filters out of the UART2 pane.
# OpenBeken prints it on every idle tick, which crowds out everything else.
QUICKTICK = "Info:MAIN:quicktick"


def guess_chip(path):
    """Pick the -chip identity a dump's filename implies, as the CLI docs do."""
    name = os.path.basename(path).upper()
    if "7238" in name:
        return "BK7238"
    if "7252N" in name:
        return "BK7252N"
    if "7252" in name:
        return "BK7252"
    return "BK7231"


class SimulatorUI:
    def __init__(self, root, initial_image=None):
        self.root = root
        root.title("BekenSimulator")
        root.minsize(900, 600)

        self.emu = None
        self.thread = None
        self.started_at = None
        self.finished = None          # message from the worker when it ends
        self.error = None             # exception text from the worker

        # Bytes the firmware has transmitted, per port, waiting to be shown.
        # The worker appends under the lock; the UI thread swaps and clears.
        self._out = {1: bytearray(), 2: bytearray()}
        self._out_lock = threading.Lock()
        self._tx_count = {1: 0, 2: 0}
        self._hex_col = 0             # column tracker for UART1 hex formatting
        # Tail of UART2 output past the last newline. The quicktick filter works
        # a line at a time, but bytes arrive on arbitrary boundaries, so a
        # half-line is held here until its newline turns up.
        self._u2_partial = ""
        self.quick_buttons = []

        self._last_insns = 0
        self._last_rate_at = None
        self._rate = 0.0

        self.history = []
        self.history_pos = None

        self._build(initial_image or DEFAULT_IMAGE)
        self.root.after(POLL_MS, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------------------------------------------------------- layout
    def _build(self, initial_image):
        mono = ("Consolas", 9) if sys.platform == "win32" else "TkFixedFont"

        cfg = ttk.LabelFrame(self.root, text="image", padding=6)
        cfg.pack(fill="x", padx=6, pady=(6, 3))

        self.var_image = tk.StringVar(value=initial_image if os.path.exists(initial_image) else "")
        ttk.Entry(cfg, textvariable=self.var_image).pack(side="left", fill="x", expand=True)
        ttk.Button(cfg, text="Browse...", command=self._browse, width=10).pack(side="left", padx=(4, 10))

        ttk.Label(cfg, text="chip").pack(side="left")
        self.var_chip = tk.StringVar(value=guess_chip(initial_image))
        ttk.Combobox(cfg, textvariable=self.var_chip, values=sorted(CHIP_FAMILIES),
                     state="readonly", width=9).pack(side="left", padx=(3, 10))

        ttk.Label(cfg, text="key").pack(side="left")
        self.var_key = tk.StringVar(value="TUYA")
        ttk.Combobox(cfg, textvariable=self.var_key, values=["(none)"] + sorted(KNOWN_KEYS),
                     state="readonly", width=9).pack(side="left", padx=(3, 10))

        self.var_boot = tk.BooleanVar(value=False)
        ttk.Checkbutton(cfg, text="boot ROM", variable=self.var_boot).pack(side="left")
        self.var_tuyamcu = tk.BooleanVar(value=False)
        ttk.Checkbutton(cfg, text="TuyaMCU peer", variable=self.var_tuyamcu).pack(side="left", padx=(8, 0))

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

        self.var_state = tk.StringVar(value="idle - pick an image and press Run")
        state = ttk.Label(self.root, textvariable=self.var_state, anchor="w",
                          relief="sunken", padding=4)
        state.pack(fill="x", padx=6, pady=4)

        panes = ttk.PanedWindow(self.root, orient="horizontal")
        panes.pack(fill="both", expand=True, padx=6)

        left = ttk.LabelFrame(panes, text="UART1 - MCU link / command console", padding=4)
        self.txt1 = self._console(left, mono)
        opts = ttk.Frame(left)
        opts.pack(fill="x", pady=(4, 0))
        self.var_hex = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="show as hex", variable=self.var_hex).pack(side="left")
        ttk.Button(opts, text="Copy", width=7,
                   command=lambda: self._copy(self.txt1, "UART1")).pack(side="right")
        panes.add(left, weight=1)

        right = ttk.LabelFrame(panes, text="UART2 - firmware debug log", padding=4)
        self.txt2 = self._console(right, mono)
        ropts = ttk.Frame(right)
        ropts.pack(fill="x", pady=(4, 0))
        self.var_hide_tick = tk.BooleanVar(value=False)
        ttk.Checkbutton(ropts, text="suppress %s" % QUICKTICK,
                        variable=self.var_hide_tick).pack(side="left")
        ttk.Button(ropts, text="Copy", width=7,
                   command=lambda: self._copy(self.txt2, "UART2")).pack(side="right")
        panes.add(right, weight=1)

        send = ttk.LabelFrame(self.root, text="send to UART1", padding=6)
        send.pack(fill="x", padx=6, pady=(4, 6))

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

    def _console(self, parent, mono):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        bar = ttk.Scrollbar(frame, orient="vertical")
        txt = tk.Text(frame, wrap="char", height=18, width=48, font=mono,
                      state="disabled", yscrollcommand=bar.set,
                      background="#101216", foreground="#d8dde3",
                      insertbackground="#d8dde3")
        bar.config(command=txt.yview)
        bar.pack(side="right", fill="y")
        txt.pack(side="left", fill="both", expand=True)
        txt.tag_configure("sent", foreground="#7fd6a0")
        txt.tag_configure("note", foreground="#e0a24a")
        return txt

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
        for w in (self.txt1, self.txt2):
            w.configure(state="normal")
            w.delete("1.0", "end")
            w.configure(state="disabled")
        self._hex_col = 0

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
        self.var_state.set("saved to %s" % path)

    def _browse(self):
        path = filedialog.askopenfilename(
            initialdir=FIRMWARE_DIR if os.path.isdir(FIRMWARE_DIR) else ROOT_DIR,
            filetypes=[("Flash dumps", "*.bin"), ("All files", "*.*")])
        if path:
            self.var_image.set(path)
            self.var_chip.set(guess_chip(path))

    # --------------------------------------------------------------- run/stop
    def _sink(self, port, byte):
        """Called from the emulation thread for every transmitted byte."""
        with self._out_lock:
            self._out[port].append(byte)

    def _on_run(self):
        if self.thread is not None and self.thread.is_alive():
            return
        path = self.var_image.get().strip()
        if not path or not os.path.isfile(path):
            messagebox.showerror("BekenSimulator", "Pick a flash dump first.")
            return

        self._clear()
        self._tx_count = {1: 0, 2: 0}
        self._last_insns = 0
        self._rate = 0.0
        self._last_rate_at = None
        self.finished = self.error = None
        self.emu = None
        self.started_at = time.time()

        self.btn_run.config(state="disabled")
        self.btn_stop.config(state="normal")
        self.btn_pause.config(state="normal", text="Pause")
        self.var_state.set("loading %s ..." % os.path.basename(path))

        # Read every Tk variable HERE, on the main thread. Tkinter is not
        # thread-safe, so the worker gets plain values, never widgets.
        cfg = {"chip": self.var_chip.get(), "key": self.var_key.get(),
               "boot": self.var_boot.get(), "tuyamcu": self.var_tuyamcu.get()}
        self.thread = threading.Thread(target=self._worker, args=(path, cfg), daemon=True)
        self.thread.start()

    def _worker(self, path, cfg):
        """Load, set up and run the emulator. Never touches Tk - see cfg."""
        try:
            key = None if cfg["key"] == "(none)" else parse_key(cfg["key"])
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
                uart_sink=self._sink,
                # Writing app_decrypted.bin into the working directory is a
                # command-line convenience, not something a GUI should do.
                save_decrypted=False)
            emu.setup()
            self.emu = emu
            emu.run()
            self.finished = "emulation finished"
        except Exception as e:            # bad key, unreadable image, Unicorn error
            self.error = "%s: %s" % (type(e).__name__, e)

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
        if self.emu is not None:
            self.emu.stop()
        self.btn_stop.config(state="disabled")
        self.btn_pause.config(state="disabled", text="Pause")

    def _on_close(self):
        if self.emu is not None:
            self.emu.stop()
        self.root.destroy()

    # ------------------------------------------------------------------ send
    def _console_ready(self):
        return (self.emu is not None and self.thread is not None
                and self.thread.is_alive()
                and self.emu.state.insn_count >= CONSOLE_READY_INSNS)

    def _send_bytes(self, payload):
        """Put raw bytes on UART1 and show them in the pane."""
        if self.emu is None:
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
        if self.emu is None:
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

            if 1 in chunks:
                self._tx_count[1] += len(chunks[1])
                self._append(self.txt1, self._render_uart1(chunks[1]))
            if 2 in chunks:
                self._tx_count[2] += len(chunks[2])
                self._append(self.txt2, self._render_uart2(chunks[2]))

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

        if self.error:
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
            return

        insns = self.emu.state.insn_count
        now = time.time()
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
        else:
            what = "running"

        self.var_state.set(
            "%s   %s insns (%.0fk/s)   ~%.1f s guest   %.0f s wall   "
            "PC 0x%08x   TX u1 %d / u2 %d   RX queued %d   console %s"
            % (what, self._fmt_insns(), self._rate / 1000.0,
               insns / float(GUEST_SECOND_INSNS), now - self.started_at,
               self.emu.state.last_pc, self._tx_count[1], self._tx_count[2],
               len(self.emu.uart1_rx), "ready" if ready else "not listening yet"))

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
        self.btn_run.config(state="normal")
        self.btn_stop.config(state="disabled")
        self.btn_pause.config(state="disabled", text="Pause")
        self._set_send_enabled(False)


def main():
    initial = sys.argv[1] if len(sys.argv) > 1 else None
    root = tk.Tk()
    SimulatorUI(root, initial)
    root.mainloop()


if __name__ == "__main__":
    main()
