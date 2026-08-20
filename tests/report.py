"""Render the self-test results as a single, self-contained HTML report.

The report lists every test with its pass/fail status, wall time, an approximate
executed-instruction count, and a download link for the flash dump it ran. Each
row expands to show a longer description and the full captured UART log, with the
strings the test asserts highlighted where they were found.

The output is one static file (inline CSS/JS, no external requests), so it works
opened locally and published to GitHub Pages unchanged.
"""
import html


def _fmt_int(n):
    return "{:,}".format(n) if isinstance(n, int) else "n/a"


def _fmt_secs(s):
    return "%.0fs" % s if s >= 1 else "%.1fs" % s


def _fmt_compact(secs):
    """Short runtime for the stat card: '13m 13s' past a minute, else '45.7s'."""
    if secs >= 60:
        return "%dm %02ds" % (int(secs // 60), int(round(secs % 60)))
    return "%.1fs" % secs


def _fmt_total(secs):
    """Header total: 'X minute(s) SS.SS seconds' past a minute, else 'SS.SS seconds'."""
    if secs >= 60:
        m = int(secs // 60)
        return "%d minute%s %.2f seconds" % (m, "" if m == 1 else "s", secs - m * 60)
    return "%.2f seconds" % secs


def _highlight(log_text, found_strings):
    """HTML-escape the log, then mark each found assertion string within it."""
    escaped = html.escape(log_text)
    # Longest first so a short string never clobbers a longer one it sits inside.
    for s in sorted({s for s in found_strings if s}, key=len, reverse=True):
        needle = html.escape(s)
        escaped = escaped.replace(needle, '<mark class="hit">' + needle + "</mark>")
    return escaped


def _tag_class(tag):
    """CSS class for a tag pill: colour by group, with the two firmware
    sources called out separately since that is the first thing you scan for."""
    name, group = tag["name"], tag.get("group", "feature")
    if group == "source":
        return "tag src-" + {"OpenBeken": "obk", "Tuya": "tuya"}.get(name, "cli")
    return "tag " + {"chip": "chip", "state": "state", "data": "data"}.get(group, "feat")


def _tags_html(r):
    return "".join('<span class="%s">%s</span>' % (_tag_class(t), html.escape(t["name"]))
                   for t in r.get("tags", []))


def _tuya_html(r):
    """Two columns: the raw stored record, and a plain-language reading of it."""
    cfg = r.get("tuya_config")
    if not cfg:
        return ('<p class="empty">No Tuya <code>user_param_key</code> record found in this dump. '
                'Newer firmware encrypts its KV storage with a per-device key, so the block is '
                'only recoverable from images that keep it in plaintext.</p>')
    rows = "".join(
        '<tr><td><code>%s</code></td><td><code>%s</code></td></tr>'
        % (html.escape(k), html.escape(v)) for k, v in cfg["pairs"])
    human = "".join(
        '<tr><td>%s</td><td><b>%s</b></td></tr>'
        % (html.escape(lab), html.escape(str(val))) for lab, val in cfg["human"])
    return """
      <p class="cfg-note">Recovered from flash at <code>0x%06X</code> - %d keys.</p>
      <div class="cfg-cols">
        <div class="cfg-col">
          <div class="cfg-h">Raw record</div>
          <pre class="log cfg-raw">%s</pre>
          <table class="cfg-t">%s</table>
        </div>
        <div class="cfg-col">
          <div class="cfg-h">Interpretation</div>
          <table class="cfg-t">%s</table>
        </div>
      </div>""" % (cfg["offset"], len(cfg["pairs"]),
                   html.escape(cfg["raw"]), rows, human)


def _gpio_html(r):
    """Third tab: every pin, and whether the firmware touched it."""
    per = r.get("periph")
    if not per:
        return ('<p class="empty">No GPIO or PWM writes were observed during this run.</p>')
    rows = []
    for pin, alias, state, detail in per["gpio"]:
        cls = "set" if state == "set by firmware" else "factory"
        rows.append('<tr class="%s"><td><code>P%d</code></td><td><code>%s</code></td>'
                    '<td>%s</td></tr>'
                    % (cls, pin, html.escape(alias or ""), html.escape(detail)))
    touched = sum(1 for _p, _a, st, _d in per["gpio"] if st != "factory")
    extra = ""
    if per["func"]:
        extra = ('<div class="cfg-h">Function select</div><table class="cfg-t">%s</table>'
                 % "".join('<tr><td><code>%s</code></td><td><code>%s</code></td></tr>'
                           % (html.escape(n), html.escape(v)) for n, v in per["func"]))
    pwm = ""
    # Only decode channels when a PWM-capable pad is actually in second
    # function. Otherwise these registers belong to the FreeRTOS tick, which
    # runs a PWM channel in PMODE_TIMER on every boot - decoding those as a
    # light gives absurdities like "0.1 Hz" from what is really a timer reload
    # value, which is exactly what this report used to print.
    if per.get("pwm_channels") and per.get("pwm_pins"):
        # The decoded view is the point of capturing PWM at all: a period of
        # 0x54A2 means nothing, "1200 Hz at 42% duty" is the bulb's actual
        # output. Frequency = 26 MHz / period; duty = duty/period.
        # The pad column only names a pin when that pin is genuinely in second
        # function. Channel numbering comes from an inferred base offset, so
        # printing the canonical pad regardless would assert things like
        # "PWM3 -> P9" on an image whose actual PWM pads are P6 and P8.
        pads = set(per.get("pwm_pins") or [])
        # PWM_CTL's enable bits name the active channels from a different
        # register than the period/duty pair, so agreement between the two is
        # real corroboration rather than the decode agreeing with itself.
        import periph as _p
        ctl_on = _p.ctl_channels(per.get("pwm_ctl") or 0)
        decoded = [c[0] for c in per["pwm_channels"]]
        ctl_note = ""
        if ctl_on:
            # Subset, not equality. A channel can be configured with a real
            # period and still not be running - a CW bulb sitting at full warm
            # white leaves its cold channel at 0% duty and switched off in CTL,
            # so CTL is a strict subset of the channels whose registers were
            # written. Only a channel CTL claims is enabled but that we failed
            # to decode indicates something actually wrong.
            missing = [c for c in ctl_on if c not in decoded]
            if missing:
                agree = ("but %s could NOT be decoded below"
                         % ", ".join("PWM%d" % c for c in missing))
            else:
                agree = "consistent with the channels decoded below"
            ctl_note = ('<p class="cfg-note">PWM_CTL = 0x%X enables %s, %s.</p>'
                        % (per.get("pwm_ctl") or 0,
                           ", ".join("PWM%d" % c for c in ctl_on), agree))
        canon = {0: 6, 1: 7, 2: 8, 3: 9, 4: 24, 5: 26}
        chans = "".join(
            '<tr><td><code>PWM%d</code></td><td>%s</td>'
            '<td>%s</td><td>%s</td><td><code>0x%X</code></td></tr>'
            % (ch,
               ("<code>P%d</code>" % canon[ch]) if canon.get(ch) in pads
               else '<span class="cfg-unk">not identified</span>',
               ("%.1f Hz" % freq) if freq else "-",
               ("%.1f%%" % pct) if pct is not None else "-", period)
            for ch, period, _duty, freq, pct in per["pwm_channels"])
        pwm += ("""
          <div class="cfg-h">PWM channels</div>%s
          <table class="cfg-t"><tr><th>Ch</th><th>Pad</th><th>Freq</th>
          <th>Duty</th><th>Period</th></tr>%s</table>
          <p class="cfg-note">Pads in second function on this image: %s.
          A channel is only paired with a pad when that pad is one of them.</p>"""
          % (ctl_note, chans, ", ".join("P%d" % p for p in sorted(pads)) or "none"))
    if per["pwm_regs"]:
        base = per.get("pwm_base")
        if per.get("pwm_layout") == "new":
            note = ("BK7231N-family image: registers are the pwm_new block at "
                    "0x802B00 (capture offsets 0x100+), three groups of two "
                    "channels with T1..T4 edge registers. Duty is reconstructed "
                    "by replaying the level toggles, not read from a duty "
                    "register.")
        elif per.get("pwm_pins") and not per.get("pwm_channels"):
            note = ("Offsets from 0x802A00. PWM-capable pads ARE in second "
                    "function here, but every write landed below +0x80. Only "
                    "the +0x80 / stride-12 layout is confirmed against "
                    "hardware, and below it the PWM channel registers cannot "
                    "be told apart from the timer registers sharing this page "
                    "- so the raw writes are shown and no frequency is "
                    "claimed.")
        elif not per.get("pwm_pins"):
            note = ("Offsets from 0x802A00. No PWM-capable pad is in second "
                    "function here, so these writes are the FreeRTOS tick - "
                    "fclk_init() runs a PWM channel in PMODE_TIMER with duty 0 "
                    "and no pin - rather than a PWM output. They are shown raw "
                    "for that reason, not decoded as channels.")
        elif base:
            note = ("Offsets are from 0x802A00. The firmware wrote at or above "
                    "+0x80, so PWM_BASE is the shifted layout "
                    "(PWM_NEW_BASE + 0x80). Channel numbering is inferred from "
                    "that base and may not line up with the pad names.")
        else:
            note = ("Offsets are from 0x802A00, which is where this image put "
                    "PWM_BASE.")
        pwm += ("""
          <div class="cfg-h">PWM registers written</div>
          <table class="cfg-t">%s</table>
          <p class="cfg-note">%s</p>"""
          % ("".join('<tr><td><code>%s</code></td><td><code>%s</code></td></tr>'
                     % (html.escape(o), html.escape(v)) for o, v in per["pwm_regs"]),
             html.escape(note)))
    return ('<p class="cfg-note">%d of %d pins configured by firmware; the rest are '
            'left at their factory (reset) state.</p>'
            '<div class="cfg-cols"><div class="cfg-col">'
            '<div class="cfg-h">Pins</div><table class="cfg-t gpio-t">%s</table></div>'
            '<div class="cfg-col">%s%s</div></div>'
            % (touched, len(per["gpio"]), "".join(rows), extra, pwm))


def _test_card(r):
    ok = r["passed"]
    status_cls = "pass" if ok else "fail"
    status_txt = "PASS" if ok else "FAIL"

    checks_html = []
    for c in r["checks"]:
        mark = "✓" if c["found"] else "✗"
        cls = "found" if c["found"] else "missing"
        # Show a count only for repeat assertions (required > 1), e.g. 10/10.
        tally = ""
        if c.get("required", 1) > 1:
            tally = '<span class="count">%d/%d</span>' % (c.get("count", 0), c["required"])
        checks_html.append(
            '<li class="%s"><span class="chk">%s</span><code>%s</code>%s</li>'
            % (cls, mark, html.escape(c["string"]), tally)
        )

    insns = "≈ %s instrs" % _fmt_int(r["insns"]) if r["insns"] is not None else ""
    timed_out = ' <span class="warn">(timed out)</span>' if r.get("timed_out") else ""

    dump = ""
    if r.get("dump_url"):
        dump = '<a class="dump" href="%s" download>⤓ %s</a>' % (
            html.escape(r["dump_url"]),
            html.escape(r["binary_name"]),
        )

    args = html.escape(" ".join(r.get("args", [])))

    return """
    <details class="card {status_cls}" data-tags="{tagdata}">
      <summary>
        <span class="dot"></span>
        <span class="head">
          <span class="row1">
            <span class="title">{name}</span>
            <span class="badges">
              <span class="badge status">{status_txt}</span>
              <span class="badge">{secs}{timed_out}</span>
              <span class="badge muted">{insns}</span>
            </span>
          </span>
          <span class="tags">{tags}</span>
        </span>
      </summary>
      <div class="body">
        <p class="desc">{desc}</p>
        <div class="meta">
          {dump}
          <span class="args"><code>main.py &lt;dump&gt; {args}</code></span>
        </div>
        <div class="checks">
          <div class="checks-title">Assertions</div>
          <ul>{checks}</ul>
        </div>
        <div class="tabbar">
          <button class="tab-btn active" type="button" data-tab="log">UART / log output</button>
          <button class="tab-btn{tuya_dis}" type="button" data-tab="tuya">Tuya config</button>
          <button class="tab-btn{gpio_dis}" type="button" data-tab="gpio">GPIO / PWM</button>
          <button class="copy-btn" type="button">Copy</button>
        </div>
        <div class="tab-panel" data-panel="log"><pre class="log">{log}</pre></div>
        <div class="tab-panel hidden" data-panel="tuya">{tuya}</div>
        <div class="tab-panel hidden" data-panel="gpio">{gpio}</div>
      </div>
    </details>
    """.format(
        status_cls=status_cls,
        status_txt=status_txt,
        name=html.escape(r["name"]),
        secs=_fmt_secs(r["elapsed"]),
        timed_out=timed_out,
        insns=insns,
        desc=html.escape(r.get("description") or "(no description)"),
        dump=dump,
        args=args,
        checks="".join(checks_html),
        tags=_tags_html(r),
        tagdata=html.escape("|".join("%s:%s" % (t.get("group", "feature"), t["name"])
                                     for t in r.get("tags", []))),
        tuya=_tuya_html(r),
        tuya_dis="" if r.get("tuya_config") else " disabled",
        gpio=_gpio_html(r),
        gpio_dis="" if r.get("periph") else " disabled",
        log=_highlight(r.get("output", ""), [c["string"] for c in r["checks"] if c["found"]]),
    )


def _filters_html(results):
    """Facet bar: every tag with a live count, grouped."""
    order = ["source", "chip", "state", "feature", "data"]
    groups = {}
    for r in results:
        for t in r.get("tags", []):
            g = t.get("group", "feature")
            groups.setdefault(g, {}).setdefault(t["name"], 0)
            groups[g][t["name"]] += 1
    if not groups:
        return ""
    blocks = []
    for g in order:
        if g not in groups:
            continue
        chips = "".join(
            '<button class="fchip %s" type="button" data-group="%s" data-tag="%s">'
            '%s<span class="fcount">%d</span></button>'
            % (_tag_class({"name": n, "group": g}).replace("tag ", ""),
               html.escape(g), html.escape(n), html.escape(n), c)
            for n, c in sorted(groups[g].items(), key=lambda kv: (-kv[1], kv[0])))
        blocks.append('<div class="fgroup"><span class="flabel">%s</span>%s</div>'
                      % (html.escape(g), chips))
    return """
  <div class="filters">
    <div class="fhead">
      <span class="fhint">Filter by tag - none selected shows everything.
        Picking two in the same row widens; picking across rows narrows.</span>
      <button class="fclear" type="button" hidden>Clear all</button>
    </div>
    %s
    <div class="fstatus" hidden></div>
  </div>""" % "".join(blocks)


def generate(results, meta, out_path):
    """Render results to a single HTML file at out_path. Returns out_path."""
    passed = sum(1 for r in results if r["passed"])
    failed = len(results) - passed
    overall_cls = "pass" if failed == 0 else "fail"

    commit_html = ""
    if meta.get("commit"):
        short = meta["commit"][:8]
        if meta.get("repo"):
            commit_html = 'commit <a href="https://github.com/%s/commit/%s"><code>%s</code></a>' % (
                html.escape(meta["repo"]), html.escape(meta["commit"]), short)
        else:
            commit_html = "commit <code>%s</code>" % short

    run_html = ""
    if meta.get("run_url"):
        run_html = ' · <a href="%s">CI run</a>' % html.escape(meta["run_url"])

    chips = meta.get("chips") or sorted({r["chip"] for r in results if r.get("chip")})

    page = _PAGE.format(
        overall_cls=overall_cls,
        passed=passed,
        failed=failed,
        total=len(results),
        runtime=_fmt_compact(meta.get("total_time", 0)),
        chips=len(chips),
        # Hover the Chips card to see which parts are covered.
        chips_list=html.escape(", ".join(chips)),
        total_time=_fmt_total(meta.get("total_time", 0)),
        generated=html.escape(meta.get("generated_at", "")),
        commit_html=commit_html,
        run_html=run_html,
        filters=_filters_html(results),
        cards="".join(_test_card(r) for r in results),
        script=_SCRIPT,
    )
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(page)
    return out_path


_PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Beken Emulator · Self-Test Report</title>
<style>
  :root {{
    --bg:#f6f7f9; --card:#ffffff; --fg:#1b1f24; --muted:#5b6470; --line:#e3e6ea;
    --pass:#1a7f37; --fail:#cf222e; --passbg:#dafbe1; --failbg:#ffebe9;
    --accent:#0969da;
    --t-obk:#0a5ca8;  --t-obk-bg:#dceafb;
    --t-tuya:#8a5200; --t-tuya-bg:#fdf0d5;
    --t-feat:#5a3ea8; --t-feat-bg:#ece7fb;
  }}
  @media (prefers-color-scheme: dark) {{
    :root {{
      --bg:#0d1117; --card:#161b22; --fg:#e6edf3; --muted:#8b949e; --line:#30363d;
      --pass:#3fb950; --fail:#f85149; --passbg:#12261a; --failbg:#2b1214;
      --accent:#4493f8;
      --t-obk:#79b8ff;  --t-obk-bg:#12283f;
      --t-tuya:#e3b341; --t-tuya-bg:#3a2d12;
      --t-feat:#b39dfb; --t-feat-bg:#2a2340;
    }}
  }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--fg);
    font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif; }}
  .wrap {{ max-width:1000px; margin:0 auto; padding:28px 18px 60px; }}
  header h1 {{ margin:0 0 4px; font-size:22px; }}
  .sub {{ color:var(--muted); font-size:13px; margin-bottom:18px; }}
  .sub a {{ color:var(--accent); text-decoration:none; }}
  .summary {{ display:flex; gap:10px; flex-wrap:wrap; margin:0 0 22px; }}
  .stat {{ background:var(--card); border:1px solid var(--line); border-radius:10px;
    padding:12px 16px; min-width:96px; }}
  .stat .n {{ font-size:24px; font-weight:700; }}
  .stat .l {{ font-size:12px; color:var(--muted); text-transform:uppercase; letter-spacing:.04em; }}
  .stat.pass .n {{ color:var(--pass); }} .stat.fail .n {{ color:var(--fail); }}
  .filters {{ background:var(--card); border:1px solid var(--line); border-radius:10px;
    padding:12px 14px; margin:0 0 16px; }}
  .fhead {{ display:flex; align-items:center; justify-content:space-between; gap:12px;
    margin-bottom:8px; }}
  .fhint {{ font-size:12px; color:var(--muted); }}
  .fclear {{ font-size:12px; padding:3px 10px; border-radius:6px; border:1px solid var(--line);
    background:var(--bg); color:var(--fg); cursor:pointer; }}
  .fgroup {{ display:flex; align-items:baseline; gap:6px; flex-wrap:wrap; margin:5px 0; }}
  .flabel {{ font-size:10.5px; text-transform:uppercase; letter-spacing:.05em;
    color:var(--muted); width:56px; flex:0 0 auto; }}
  .fchip {{ font-size:11.5px; padding:2px 8px; border-radius:20px; cursor:pointer;
    border:1px solid var(--line); background:var(--bg); color:var(--muted);
    display:inline-flex; gap:6px; align-items:center; }}
  .fchip:hover {{ color:var(--fg); border-color:var(--accent); }}
  .fchip.on {{ background:var(--accent); border-color:var(--accent); color:#fff; font-weight:700; }}
  .fchip.zero {{ opacity:.35; }}
  .fcount {{ font-size:10px; opacity:.8; }}
  .fstatus {{ margin-top:8px; font-size:12px; color:var(--muted); }}
  .cfg-unk {{ color:var(--muted); font-style:italic; }}
  .card {{ background:var(--card); border:1px solid var(--line); border-radius:10px;
    margin:10px 0; overflow:hidden; }}
  .card.fail {{ border-color:var(--fail); }}
  summary {{ display:flex; align-items:center; gap:10px; padding:12px 16px; cursor:pointer;
    list-style:none; }}
  summary::-webkit-details-marker {{ display:none; }}
  .dot {{ width:10px; height:10px; border-radius:50%; flex:0 0 auto; background:var(--pass); }}
  .card.fail .dot {{ background:var(--fail); }}
  .head {{ flex:1; display:flex; flex-direction:column; gap:6px; min-width:0; }}
  .row1 {{ display:flex; align-items:center; gap:10px; flex-wrap:wrap; }}
  .title {{ font-weight:600; flex:1; min-width:0; }}
  /* Second row: what the test covers, at a glance. */
  .tags {{ display:flex; gap:5px; flex-wrap:wrap; }}
  .tag {{ font-size:10.5px; line-height:1.5; padding:1px 7px; border-radius:4px;
    font-weight:700; letter-spacing:.02em; white-space:nowrap; border:1px solid transparent; }}
  .tag.src-obk  {{ background:var(--t-obk-bg);  color:var(--t-obk); }}
  .tag.src-tuya {{ background:var(--t-tuya-bg); color:var(--t-tuya); }}
  .tag.src-cli  {{ background:var(--bg); color:var(--muted); border-color:var(--line); }}
  .tag.chip     {{ background:transparent; color:var(--muted); border-color:var(--line); }}
  .tag.state    {{ background:var(--passbg); color:var(--pass); }}
  .tag.feat     {{ background:var(--t-feat-bg); color:var(--t-feat); }}
  .tag.data     {{ background:transparent; color:var(--accent); border-color:var(--accent); }}
  .badges {{ display:flex; gap:6px; align-items:center; flex-wrap:wrap; }}
  .badge {{ font-size:12px; padding:2px 8px; border-radius:20px; background:var(--bg);
    border:1px solid var(--line); color:var(--muted); white-space:nowrap; }}
  .badge.status {{ font-weight:700; }}
  .card.pass .badge.status {{ background:var(--passbg); color:var(--pass); border-color:transparent; }}
  .card.fail .badge.status {{ background:var(--failbg); color:var(--fail); border-color:transparent; }}
  .warn {{ color:var(--fail); }}
  .body {{ padding:2px 16px 16px; border-top:1px solid var(--line); }}
  .desc {{ color:var(--fg); margin:12px 0; }}
  .meta {{ display:flex; gap:14px; flex-wrap:wrap; align-items:center; margin-bottom:12px; }}
  a.dump {{ color:var(--accent); text-decoration:none; font-weight:600; font-size:13px;
    border:1px solid var(--line); padding:4px 10px; border-radius:8px; }}
  a.dump:hover {{ border-color:var(--accent); }}
  .args code {{ color:var(--muted); font-size:12px; }}
  .checks-title, .log-title {{ font-size:12px; text-transform:uppercase; letter-spacing:.04em;
    color:var(--muted); margin:14px 0 6px; }}
  .log-head {{ display:flex; align-items:center; justify-content:space-between; margin:14px 0 6px; }}
  .tabbar {{ display:flex; align-items:center; gap:4px; margin:16px 0 0;
    border-bottom:1px solid var(--line); }}
  .tab-btn {{ font-size:12px; padding:6px 12px; border:1px solid transparent; border-bottom:none;
    background:none; color:var(--muted); cursor:pointer; border-radius:6px 6px 0 0; }}
  .tab-btn:hover:not(.disabled) {{ color:var(--fg); }}
  .tab-btn.active {{ background:var(--bg); border-color:var(--line); color:var(--fg);
    font-weight:700; margin-bottom:-1px; }}
  .tab-btn.disabled {{ opacity:.45; cursor:default; }}
  .tabbar .copy-btn {{ margin-left:auto; }}
  .tab-panel {{ padding-top:10px; }}
  .tab-panel.hidden {{ display:none; }}
  .empty {{ color:var(--muted); font-size:13px; }}
  .cfg-note {{ color:var(--muted); font-size:12px; margin:2px 0 10px; }}
  .cfg-cols {{ display:flex; gap:16px; flex-wrap:wrap; }}
  .cfg-col {{ flex:1 1 300px; min-width:0; }}
  .cfg-h {{ font-size:12px; text-transform:uppercase; letter-spacing:.04em;
    color:var(--muted); margin-bottom:6px; }}
  .cfg-raw {{ max-height:120px; font-size:11px; }}
  .cfg-t {{ width:100%; border-collapse:collapse; font-size:12px; }}
  .cfg-t td {{ padding:2px 6px; border-bottom:1px solid var(--line); vertical-align:top; }}
  .cfg-t td:first-child {{ color:var(--muted); white-space:nowrap; }}
  .gpio-t tr.factory td {{ opacity:.45; }}
  .gpio-t tr.set td {{ color:var(--fg); }}
  .gpio-t tr.set td:nth-child(2) {{ color:var(--pass); font-weight:700; }}
  .log-head .log-title {{ margin:0; }}
  .copy-btn {{ font-size:12px; padding:3px 10px; border-radius:6px; border:1px solid var(--line);
    background:var(--card); color:var(--fg); cursor:pointer; }}
  .copy-btn:hover {{ border-color:var(--accent); color:var(--accent); }}
  .copy-btn.copied {{ color:var(--pass); border-color:var(--pass); }}
  .checks ul {{ list-style:none; margin:0; padding:0; }}
  .checks li {{ display:flex; gap:8px; align-items:baseline; padding:2px 0; }}
  .checks .chk {{ font-weight:700; width:14px; flex:0 0 auto; }}
  .checks li.found .chk {{ color:var(--pass); }}
  .checks li.missing .chk {{ color:var(--fail); }}
  .checks li.missing code {{ color:var(--fail); }}
  .checks .count {{ font-size:11px; padding:1px 7px; border-radius:20px; background:var(--passbg);
    color:var(--pass); font-weight:700; }}
  .checks li.missing .count {{ background:var(--failbg); color:var(--fail); }}
  code {{ font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; font-size:12.5px; }}
  pre.log {{ background:var(--bg); border:1px solid var(--line); border-radius:8px;
    padding:12px; overflow:auto; max-height:460px; font-size:12px; line-height:1.45;
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; white-space:pre-wrap;
    word-break:break-word; }}
  /* Asserted strings, shown where matched in the log: bold green, not a marker. */
  mark.hit {{ background:var(--passbg); color:var(--pass); font-weight:700; padding:0 2px;
    border-radius:3px; }}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>Beken Emulator · Self-Test Report</h1>
    <div class="sub">{generated} · total {total_time} · {commit_html}{run_html}</div>
  </header>
  <div class="summary">
    <div class="stat"><div class="n">{total}</div><div class="l">Tests</div></div>
    <div class="stat pass"><div class="n">{passed}</div><div class="l">Passed</div></div>
    <div class="stat fail"><div class="n">{failed}</div><div class="l">Failed</div></div>
    <div class="stat"><div class="n">{runtime}</div><div class="l">Runtime</div></div>
    <div class="stat" title="{chips_list}"><div class="n">{chips}</div><div class="l">Chips</div></div>
  </div>
  {filters}
  {cards}
</div>
{script}
</body>
</html>
"""


# Wires up the per-log Copy buttons. Kept as a substituted value (not part of the
# .format template) so its JS braces need no escaping. Uses the async clipboard
# API where available and falls back to a hidden textarea + execCommand so it
# also works from a file:// URL.
_SCRIPT = """<script>

// --- tag filtering -------------------------------------------------------
// Selected tags in the same group are OR-ed (two chips widens the result);
// different groups are AND-ed (chip + feature narrows it). That is what makes
// "BK7231N" + "BK7231T" show both families instead of nothing.
(function () {
  var bar = document.querySelector('.filters');
  if (!bar) return;
  var chips = Array.prototype.slice.call(bar.querySelectorAll('.fchip'));
  var cards = Array.prototype.slice.call(document.querySelectorAll('.card'));
  var status = bar.querySelector('.fstatus');
  var clear = bar.querySelector('.fclear');
  var sel = {};   // group -> Set of selected tag names

  cards.forEach(function (c) {
    c._tags = {};
    (c.getAttribute('data-tags') || '').split('|').forEach(function (t) {
      if (!t) return;
      var i = t.indexOf(':');
      var g = t.slice(0, i), n = t.slice(i + 1);
      (c._tags[g] = c._tags[g] || []).push(n);
    });
  });

  function matches(card, selection) {
    for (var g in selection) {
      if (!selection[g] || !selection[g].size) continue;
      var have = card._tags[g] || [];
      var hit = false;
      selection[g].forEach(function (n) { if (have.indexOf(n) >= 0) hit = true; });
      if (!hit) return false;      // this group is AND-ed against the others
    }
    return true;
  }

  function apply() {
    var shown = 0;
    cards.forEach(function (c) {
      var ok = matches(c, sel);
      c.hidden = !ok;
      if (ok) shown++;
    });
    // Facet counts: for each chip, how many cards WOULD show if it were the
    // choice in its group - i.e. apply every other group's filter, then count.
    chips.forEach(function (ch) {
      var g = ch.getAttribute('data-group'), n = ch.getAttribute('data-tag');
      var probe = {};
      for (var k in sel) if (k !== g) probe[k] = sel[k];
      var c = cards.filter(function (card) {
        return matches(card, probe) && (card._tags[g] || []).indexOf(n) >= 0;
      }).length;
      ch.querySelector('.fcount').textContent = c;
      ch.classList.toggle('zero', c === 0);
    });
    var any = Object.keys(sel).some(function (g) { return sel[g] && sel[g].size; });
    clear.hidden = !any;
    status.hidden = !any;
    if (any) status.textContent = 'Showing ' + shown + ' of ' + cards.length + ' tests.';
  }

  chips.forEach(function (ch) {
    ch.addEventListener('click', function () {
      var g = ch.getAttribute('data-group'), n = ch.getAttribute('data-tag');
      sel[g] = sel[g] || new Set();
      if (sel[g].has(n)) { sel[g].delete(n); ch.classList.remove('on'); }
      else { sel[g].add(n); ch.classList.add('on'); }
      apply();
    });
  });
  clear.addEventListener('click', function () {
    sel = {};
    chips.forEach(function (c) { c.classList.remove('on'); });
    apply();
  });
  apply();
})();

document.querySelectorAll('.tab-btn').forEach(function (btn) {
  btn.addEventListener('click', function () {
    if (btn.classList.contains('disabled')) return;
    var body = btn.closest('.body');
    body.querySelectorAll('.tab-btn').forEach(function (b) { b.classList.remove('active'); });
    btn.classList.add('active');
    body.querySelectorAll('.tab-panel').forEach(function (p) {
      p.classList.toggle('hidden', p.getAttribute('data-panel') !== btn.getAttribute('data-tab'));
    });
  });
});
document.querySelectorAll('.copy-btn').forEach(function (btn) {
  btn.addEventListener('click', function () {
    var body = btn.closest('.body');
    var vis = body.querySelector('.tab-panel:not(.hidden)');
    var pre = (vis && vis.querySelector('pre')) || body.querySelector('pre.log');
    if (!pre) return;
    var text = pre.innerText;
    var done = function () {
      btn.textContent = 'Copied!';
      btn.classList.add('copied');
      setTimeout(function () { btn.textContent = 'Copy'; btn.classList.remove('copied'); }, 1200);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done, function () { fallbackCopy(text); done(); });
    } else {
      fallbackCopy(text);
      done();
    }
  });
});
function fallbackCopy(text) {
  var ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  try { document.execCommand('copy'); } catch (e) {}
  document.body.removeChild(ta);
}
</script>"""
