#!/usr/bin/env python3
"""NV Control: a small control panel for the NVIDIA GPU and the super resolution (SR) players of this project.

GTK4 + libadwaita, themed from caelestia's Material You palette. Pages: Overview (GPU state), Super Resolution
(install status, probe, mpv/VLC settings), Power & performance, About / diagnostics.

Rules the code follows:
- The dGPU may be runtime-suspended (hybrid laptop). nvidia-smi wakes it, so it is only called while the window is
  visible, the Overview page is showing and /sys/.../runtime_status says the GPU is awake (or on a button click).
- Nothing blocks the GTK main loop: subprocesses run in threads and report back with GLib.idle_add.
- Privileged commands are never run by the app: they are shown and opened in a terminal for the user to run.
"""

import argparse
import json
import os
import sys
import threading
import time

APP_DIR = os.path.dirname(os.path.abspath(__file__))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, GLib, GObject, Gtk  # noqa: E402

import nvc  # noqa: E402
from nvc import conf, diag, gpu as gpumod, paths, proc, sr, theme  # noqa: E402
from ui import (BadgePreview, ConfEditor, Tile, chip, fmt_mib, fmt_num, hint, icon_image, kv_row, make_page,  # noqa: E402
                run_async, short_path, vbox, vlc_running)

APP_ID = nvc.APP_ID  # tests / dev runs may override this attribute before main()
ICONS_DIR = os.path.join(APP_DIR, "icons")
POLL_CHOICES = (0, 1, 2, 5, 10)  # seconds, 0 = manual
DEFAULT_POLL = 2
COMPUTE_APPS_EVERY = 6.0  # seconds between process-list queries while polling


class Settings:
    """The app's own small settings file (~/.config/nvvsr/panel.json)."""

    def __init__(self):
        self.poll_interval = DEFAULT_POLL
        try:
            with open(paths.panel_settings_file(), "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and data.get("poll_interval") in POLL_CHOICES:
                self.poll_interval = data["poll_interval"]
        except (OSError, ValueError):
            pass

    def save(self):
        p = os.path.realpath(paths.panel_settings_file())
        try:
            conf.atomic_write(p, json.dumps({"poll_interval": self.poll_interval}, indent=2) + "\n", 0o644)
        except OSError:
            pass


# ------------------------------------------------------------------------------------------------------------
# Overview
# ------------------------------------------------------------------------------------------------------------

class OverviewPage:
    def __init__(self, win):
        self.win = win
        box = vbox(18)

        # hero
        self.hero_title = Gtk.Label(label="NVIDIA GPU", xalign=0, css_classes=["title-3"], wrap=True)
        self.hero_sub = Gtk.Label(label="", xalign=0, css_classes=["nvc-muted"], wrap=True)
        self.state_chip = chip("Checking", None)
        hero = Gtk.Box(spacing=14, css_classes=["nvc-hero"])
        hero.append(icon_image("nvc-gauge-symbolic"))
        hero.get_first_child().set_pixel_size(40)
        texts = vbox(2)
        texts.set_hexpand(True)
        texts.append(self.hero_title)
        texts.append(self.hero_sub)
        hero.append(texts)
        hero.append(self.state_chip)
        box.append(hero)

        # asleep card
        self.asleep = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, css_classes=["card"],
                              margin_top=0, visible=False)
        inner = Gtk.Box(spacing=14, margin_top=16, margin_bottom=16, margin_start=16, margin_end=16)
        inner.append(icon_image("nvc-sleep-symbolic"))
        inner.get_first_child().set_pixel_size(32)
        t = vbox(4)
        t.set_hexpand(True)
        self.asleep_title = Gtk.Label(label="GPU asleep (runtime D3) - not polled", xalign=0, wrap=True,
                                      css_classes=["heading"])
        t.append(self.asleep_title)
        t.append(hint("Reading the GPU wakes it and costs battery, so nothing is queried while it sleeps. "
                      "Use the button to read it once."))
        inner.append(t)
        self.wake_btn = Gtk.Button(label="Wake and read", css_classes=["suggested-action"], valign=Gtk.Align.CENTER)
        self.wake_btn.connect("clicked", lambda *_: win.query_now(wake=True))
        inner.append(self.wake_btn)
        self.asleep.append(inner)
        box.append(self.asleep)

        # metrics
        self.tiles = {
            "mem": Tile("Memory"), "util": Tile("Utilisation"), "temp": Tile("Temperature"), "power": Tile("Power"),
        }
        self.flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, min_children_per_line=1,
                                max_children_per_line=4, column_spacing=12, row_spacing=12, homogeneous=True)
        for i, tile in enumerate(self.tiles.values()):
            self.flow.append(tile)
            self.flow.get_child_at_index(i).set_focusable(False)
        box.append(self.flow)
        self.error = hint("", error=True)
        self.error.set_visible(False)
        box.append(self.error)

        # details
        details = Adw.PreferencesGroup(title="Details")
        self.rows = {}
        for key, title in (("gclk", "Graphics clock"), ("mclk", "Memory clock"), ("pstate", "Performance state"),
                           ("pcie", "PCIe link"), ("fan", "Fan"), ("hybrid", "Hybrid graphics mode (supergfxctl)"),
                           ("source", "Power source"), ("runtime", "Runtime power management")):
            row, lbl = kv_row(title)
            details.add(row)
            self.rows[key] = lbl
        box.append(details)

        # processes
        self.procs = Adw.PreferencesGroup(
            title="Compute processes",
            description="Graphics-only clients (OpenGL, Vulkan, WebGPU) are not listed by NVIDIA's compute query.")
        self.proc_rows = []
        box.append(self.procs)

        # settings
        prefs = Adw.PreferencesGroup(title="Polling")
        self.poll_row = Adw.ComboRow(
            title="Refresh every",
            subtitle="Only while this page is visible and the GPU is awake. Polling keeps the GPU awake.",
            model=Gtk.StringList.new(["Off (manual refresh)", "1 second", "2 seconds", "5 seconds", "10 seconds"]))
        self.poll_row.set_selected(POLL_CHOICES.index(win.settings.poll_interval))
        self.poll_row.connect("notify::selected", self._poll_changed)
        prefs.add(self.poll_row)
        box.append(prefs)

        self.widget = make_page(box)
        self.set_processes(None)

    def _poll_changed(self, row, _p):
        self.win.settings.poll_interval = POLL_CHOICES[row.get_selected()]
        self.win.settings.save()

    # -- updates --------------------------------------------------------------------------------------------
    def set_state(self, kind, text):
        for c in ("nvc-chip-ok", "nvc-chip-warn", "nvc-chip-bad"):
            self.state_chip.remove_css_class(c)
        if kind:
            self.state_chip.add_css_class(f"nvc-chip-{kind}")
        self.state_chip.set_label(text)

    def set_static(self, gpu, runtime, hybrid, source, driver):
        self.hero_sub.set_label(" - ".join(x for x in (
            f"Driver {driver}" if driver else "", f"PCI {gpu.addr}" if gpu else "") if x) or "No NVIDIA GPU found")
        self.rows["hybrid"].set_label(hybrid or "unknown")
        self.rows["source"].set_label({"ac": "AC power", "battery": "Battery", "unknown": "unknown"}[source])
        bits = [runtime.status or "unknown", f"control: {runtime.control}" if runtime.control else "",
                runtime.power_state or ""]
        self.rows["runtime"].set_label(", ".join(b for b in bits if b))

    def show_asleep(self, asleep):
        self.asleep.set_visible(asleep)

    def set_stale(self, stale):
        (self.flow.add_css_class if stale else self.flow.remove_css_class)("nvc-stale")

    def set_error(self, text):
        self.error.set_label(text or "")
        self.error.set_visible(bool(text))

    def set_sample(self, s):
        if s.name:
            self.hero_title.set_label(s.name)
        mem_frac = s.mem_used_mib / s.mem_total_mib if s.mem_used_mib is not None and s.mem_total_mib else None
        gib = s.mem_total_mib is not None and s.mem_used_mib is not None
        self.tiles["mem"].set(f"{s.mem_used_mib / 1024:.1f} / {s.mem_total_mib / 1024:.1f} GiB" if gib else "-",
                              "" if mem_frac is None else f"{mem_frac * 100:.0f} % used", mem_frac,
                              "nvc-bad" if mem_frac and mem_frac > 0.92 else None)
        self.tiles["util"].set(fmt_num(s.util_pct, 0, " %"), "GPU engine", (s.util_pct or 0) / 100.0)
        self.tiles["temp"].set(fmt_num(s.temp_c, 0, " °C"), "", (s.temp_c or 0) / 100.0,
                               "nvc-bad" if s.temp_c and s.temp_c >= 85 else "nvc-warn" if s.temp_c and s.temp_c >= 75 else None)
        pf = s.power_w / s.power_limit_w if s.power_w is not None and s.power_limit_w else None
        self.tiles["power"].set(fmt_num(s.power_w, 1, " W"),
                                f"limit {fmt_num(s.power_limit_w, 0, ' W')}" if s.power_limit_w else "", pf)
        self.rows["gclk"].set_label(fmt_num(s.clock_gr_mhz, 0, " MHz"))
        self.rows["mclk"].set_label(fmt_num(s.clock_mem_mhz, 0, " MHz"))
        self.rows["pstate"].set_label(s.pstate or "-")
        self.rows["pcie"].set_label(f"Gen {s.pcie_gen} x{s.pcie_width}" if s.pcie_gen and s.pcie_width else "-")
        self.rows["fan"].set_label(fmt_num(s.fan_pct, 0, " %") if s.fan_pct is not None else "not reported")
        self.set_error(None)

    def set_processes(self, apps):
        for r in self.proc_rows:
            self.procs.remove(r)
        self.proc_rows = []
        if apps is None:
            rows = [Adw.ActionRow(title="Not read yet", css_classes=["nvc-muted"])]
        elif not apps:
            rows = [Adw.ActionRow(title="No compute processes", css_classes=["nvc-muted"])]
        else:
            rows = []
            for a in apps:
                r = Adw.ActionRow(title=GLib.markup_escape_text(os.path.basename(a.name) or a.name),
                                  subtitle=f"PID {a.pid}")
                r.add_suffix(Gtk.Label(label=fmt_mib(a.mem_mib), css_classes=["nvc-muted"]))
                rows.append(r)
        for r in rows:
            self.procs.add(r)
        self.proc_rows = rows


# ------------------------------------------------------------------------------------------------------------
# Super Resolution page
# ------------------------------------------------------------------------------------------------------------

class SrPage:
    def __init__(self, win):
        self.win = win
        self.status = None
        box = vbox(18)

        self.install = Adw.PreferencesGroup(title="Installation")
        self.install_rows = []
        box.append(self.install)

        check = Adw.PreferencesGroup(title="Check")
        self.check_row = Adw.ActionRow(title="Check that super resolution works",
                                       subtitle="Loads the NVIDIA libraries and checks the GPU. This wakes the GPU for a moment.")
        self.spinner = Gtk.Spinner(valign=Gtk.Align.CENTER)
        self.check_btn = Gtk.Button(label="Check SR works", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
        self.check_btn.connect("clicked", lambda *_: self.run_check())
        self.check_row.add_suffix(self.spinner)
        self.check_row.add_suffix(self.check_btn)
        check.add(self.check_row)
        self.check_result = hint("")
        self.check_result.set_selectable(True)
        self.check_result.set_visible(False)
        check.add(self.check_result)
        box.append(check)

        self.preview = BadgePreview()
        self.mpv = ConfEditor(win, "mpv (vsr.conf)", conf.MPV_OPTIONS, paths.mpv_vsr_conf, noun="mpv settings",
                              on_change=self._preview)
        cap = Gtk.Label(label="Badge preview: a 1280x720 frame with the badge drawn at 2x size.", xalign=0,
                        css_classes=["nvc-hint"], wrap=True)
        pv = vbox(2)
        pv.append(cap)
        pv.append(Adw.Clamp(maximum_size=520, child=self.preview))
        self.mpv.insert_extra(pv)
        box.append(self.mpv.widget)

        self.vlc = ConfEditor(win, "VLC (vlcrc)", conf.VLC_OPTIONS, paths.vlcrc, section="nvvsr", bool_style="01",
                              guard=vlc_running, noun="VLC settings")
        box.append(self.vlc.widget)

        other = Adw.PreferencesGroup(title="Browsers")
        legacy = Adw.ActionRow(title="Legacy: Chrome handoff to mpv",
                               subtitle="Superseded design that opened videos in a separate mpv window. No settings here.")
        legacy.add_suffix(chip("Legacy"))
        other.add(legacy)
        soon = Adw.ActionRow(title="Browser (in-page WebGPU)", subtitle="Coming soon", sensitive=False)
        soon.add_suffix(chip("Coming soon"))
        other.add(soon)
        box.append(other)

        self.widget = make_page(box)
        self._preview(self.mpv.current_values())

    def _preview(self, v):
        self.preview.update(bool(v.get("osd")), v.get("osd_size", 11), float(v.get("osd_opacity", 0.55)),
                            int(v.get("osd_position", 9)))

    # -- status ---------------------------------------------------------------------------------------------
    def refresh(self):
        def job():
            info = sr.read_install_json()
            repo = paths.repo_dir(APP_DIR)
            mpv_vs = sr.mpv_has_vapoursynth(paths.private_mpv()) if os.path.isfile(paths.private_mpv()) else None
            return sr.compute_status(info, repo, mpv_vs), info

        def done(res):
            if isinstance(res, Exception):
                self.show_status_error(str(res))
                return
            self.status, info = res
            self.show_status(self.status, info)

        run_async(job, done)
        self.mpv.reload()
        self.vlc.reload()
        run_async(vlc_running, lambda r: self.vlc.set_banner(r if isinstance(r, str) else ""))

    def show_status_error(self, text):
        self._clear_status()
        self.install.add(Adw.ActionRow(title="Could not read the installation", subtitle=text))

    def _clear_status(self):
        for r in self.install_rows:
            self.install.remove(r)
        self.install_rows = []

    def show_status(self, st, info):
        self._clear_status()
        good = st.mpv_ready or st.vlc_ready
        mode = {"installed": "Installed" + (f" (version {st.version})" if st.version else ""),
                "dev": "Development checkout (build directories)", "missing": "Not installed"}[st.mode]
        head = Adw.ActionRow(title=st.headline, subtitle=mode)
        head.add_prefix(icon_image("nvc-check-symbolic" if good else "nvc-warning-symbolic", "nvc-ok" if good else "nvc-warn"))
        head.add_suffix(chip("mpv ready" if st.mpv_ready else "mpv incomplete", "ok" if st.mpv_ready else "warn"))
        head.add_suffix(chip("VLC ready" if st.vlc_ready else "VLC incomplete", "ok" if st.vlc_ready else "warn"))
        rows = [head]
        for c in st.checks:
            r = Adw.ActionRow(title=c.label, subtitle=GLib.markup_escape_text(c.detail if c.ok else (c.hint or c.detail)))
            r.add_prefix(icon_image("nvc-check-symbolic" if c.ok else "nvc-warning-symbolic", "nvc-ok" if c.ok else "nvc-warn"))
            r.set_subtitle_selectable(True)
            rows.append(r)
        comps = info.components if info else {}
        problem = [(n, s, d) for n, (s, d) in comps.items() if s in ("failed", "partial", "skipped")]
        for n, s, d in problem:
            r = Adw.ActionRow(title=GLib.markup_escape_text(f"Installer: {n} {s}"), subtitle=GLib.markup_escape_text(d))
            r.add_prefix(icon_image("nvc-info-symbolic"))
            rows.append(r)
        for r in rows:
            self.install.add(r)
        self.install_rows = rows

    # -- probe ----------------------------------------------------------------------------------------------
    def run_check(self, done=None):
        if not self.check_btn.get_sensitive():
            return
        self.check_btn.set_sensitive(False)
        self.spinner.start()
        self.check_result.set_visible(False)
        lib = None
        if self.status and self.status.ok("libnvvsr"):
            lib = self.status.check("libnvvsr").detail.split(" (development build)")[0]

        def job():
            return sr.run_probe(lib, sr.libdir_env())

        def finished(res):
            self.spinner.stop()
            self.check_btn.set_sensitive(True)
            if isinstance(res, Exception):
                res = sr.ProbeResult(False, "The check failed", str(res))
            text = res.summary + (f": {res.detail}" if res.detail else "")
            self.check_result.set_label(text)
            for c in ("nvc-error", "nvc-hint", "nvc-ok"):
                self.check_result.remove_css_class(c)
            self.check_result.add_css_class("nvc-ok" if res.ok else "nvc-error")
            self.check_result.set_visible(True)
            if done:
                done(res)

        run_async(job, finished)


# ------------------------------------------------------------------------------------------------------------
# Power & performance page
# ------------------------------------------------------------------------------------------------------------

class PowerPage:
    def __init__(self, win):
        self.win = win
        self.info, self.persistence = None, None
        box = vbox(18)

        self.asleep = Gtk.Box(spacing=14, css_classes=["card"], visible=False)
        inner = Gtk.Box(spacing=14, margin_top=16, margin_bottom=16, margin_start=16, margin_end=16, hexpand=True)
        inner.append(icon_image("nvc-sleep-symbolic"))
        inner.get_first_child().set_pixel_size(32)
        t = vbox(4)
        t.set_hexpand(True)
        t.append(Gtk.Label(label="GPU asleep (runtime D3) - not read", xalign=0, css_classes=["heading"]))
        t.append(hint("The limits come from nvidia-smi, which would wake the GPU."))
        inner.append(t)
        btn = Gtk.Button(label="Wake and read", css_classes=["suggested-action"], valign=Gtk.Align.CENTER)
        btn.connect("clicked", lambda *_: self.refresh(force=True))
        inner.append(btn)
        self.asleep.append(inner)
        box.append(self.asleep)

        self.error = hint("", error=True)
        self.error.set_visible(False)
        box.append(self.error)

        read = Adw.PreferencesGroup(title="Limits and clocks")
        self.rows = {}
        for key, title in (("limit", "Power limit"), ("default", "Default power limit"), ("range", "Allowed range"),
                           ("draw", "Power draw"), ("gclk", "Graphics clock"), ("mclk", "Memory clock"),
                           ("limiters", "Clock limiters active"), ("persist", "Persistence mode")):
            row, lbl = kv_row(title)
            read.add(row)
            self.rows[key] = lbl
        box.append(read)

        change = Adw.PreferencesGroup(
            title="Change (needs root)",
            description="These open a terminal with the exact command. You review it and type your sudo password there; "
                        "NV Control never runs them itself.")
        self.limit_row = Adw.SpinRow(title="Power limit (W)", subtitle="Wake and read first to learn the allowed range",
                                     adjustment=Gtk.Adjustment(value=100, lower=1, upper=1000, step_increment=1,
                                                               page_increment=10), digits=0, sensitive=False)
        self.limit_row.connect("notify::value", lambda *_: self._update_commands())
        change.add(self.limit_row)
        self.limit_cmd = Gtk.Label(xalign=0, selectable=True, css_classes=["nvc-cmd"], valign=Gtk.Align.CENTER)
        self.limit_btn = Gtk.Button(label="Run in terminal", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
        self.limit_btn.connect("clicked", lambda *_: self._run_limit())
        self.default_btn = Gtk.Button(label="Use default", valign=Gtk.Align.CENTER)
        self.default_btn.connect("clicked", lambda *_: self._use_default())
        r = Adw.ActionRow(title="Command")
        r.add_suffix(self.limit_cmd)
        r.add_suffix(self.default_btn)
        r.add_suffix(self.limit_btn)
        change.add(r)

        self.persist_row = Adw.ActionRow(
            title="Persistence mode",
            subtitle="Keeps the driver loaded between runs. On this hybrid laptop it can stop the GPU from "
                     "sleeping, so leave it off unless you need it.")
        self.persist_cmd = Gtk.Label(xalign=0, selectable=True, css_classes=["nvc-cmd"], valign=Gtk.Align.CENTER)
        self.persist_btn = Gtk.Button(label="Run in terminal", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
        self.persist_btn.connect("clicked", lambda *_: self._run_persist())
        self.persist_row.add_suffix(self.persist_cmd)
        self.persist_row.add_suffix(self.persist_btn)
        change.add(self.persist_row)
        box.append(change)

        limits = Adw.PreferencesGroup(title="Not available here")
        for title, sub in (
            ("Fan control", "The laptop firmware controls the fan; NVIDIA exposes no fan control for this GPU."),
            ("Clock and voltage offsets", "These need nvidia-settings (NV-CONTROL), which only works under X11. Not available on Wayland."),
            ("nvidia-settings", "X11 only, so NV Control does not use it. A power limit set here resets on reboot, "
                                "and the firmware may clamp it."),
        ):
            limits.add(Adw.ActionRow(title=title, subtitle=sub))
        box.append(limits)

        self.widget = make_page(box)
        self._update_commands()

    # -- data -----------------------------------------------------------------------------------------------
    def refresh(self, force=False):
        self.win.runtime = gpumod.read_runtime(self.win.gpu)
        state = gpumod.gpu_state(self.win.runtime.status)
        if state == "asleep" and not force:
            self.asleep.set_visible(True)
            return
        self.asleep.set_visible(False)

        def job():
            detail, err = gpumod.query_power_page()
            sample, _ = gpumod.query_gpu() if detail else (None, None)
            return detail, err, sample

        def done(res):
            if isinstance(res, Exception):
                self.show_error(str(res))
                return
            detail, err, sample = res
            if detail is None:
                self.show_error(f"nvidia-smi failed: {err}")
                return
            self.error.set_visible(False)
            self.show(detail, sample)
            if sample:
                self.win.on_sample(sample)

        run_async(job, done)

    def show_error(self, text):
        self.error.set_label(text)
        self.error.set_visible(True)

    def show(self, d, sample):
        self.info = d.power
        p = d.power
        w = lambda v: "-" if v is None else f"{v:g} W"
        self.rows["limit"].set_label(w(p.limit_w))
        self.rows["default"].set_label(w(p.default_w))
        self.rows["range"].set_label("-" if p.min_w is None or p.max_w is None else f"{p.min_w:g} to {p.max_w:g} W")
        self.rows["draw"].set_label(w(p.draw_w))
        clk = lambda cur, mx: "-" if cur is None else f"{cur} MHz" + (f" (max {mx} MHz)" if mx else "")
        self.rows["gclk"].set_label(clk(d.clocks.get("graphics"), d.max_clocks.get("graphics")))
        self.rows["mclk"].set_label(clk(d.clocks.get("memory"), d.max_clocks.get("memory")))
        self.rows["limiters"].set_label(", ".join(d.limiters) if d.limiters else
                                        ("none" if d.has_limiter_info else "not reported"))
        persistence = sample.persistence if sample else None
        self.persistence = persistence
        self.rows["persist"].set_label({True: "Enabled", False: "Disabled", None: "unknown"}[persistence])
        if p.min_w is not None and p.max_w is not None:
            adj = self.limit_row.get_adjustment()
            adj.set_lower(p.min_w)
            adj.set_upper(p.max_w)
            self.limit_row.set_value(p.limit_w or p.default_w or p.max_w)
            self.limit_row.set_sensitive(True)
            self.limit_row.set_subtitle(f"Allowed range {p.min_w:g} to {p.max_w:g} W")
        self._update_commands()

    # -- terminal commands ----------------------------------------------------------------------------------
    def _limit_command(self):
        try:
            return gpumod.power_limit_command(self.limit_row.get_value(), self.info)
        except ValueError:
            return None

    def _update_commands(self):
        cmd = self._limit_command()
        self.limit_cmd.set_label(proc.display_command(cmd) if cmd else "-")
        self.limit_btn.set_sensitive(cmd is not None and self.info is not None)
        self.default_btn.set_sensitive(self.info is not None and self.info.default_w is not None)
        enabled = self.persistence
        pcmd = gpumod.persistence_command(not enabled)
        self.persist_cmd.set_label(proc.display_command(pcmd))
        self.persist_btn.set_label("Disable in terminal" if enabled else "Enable in terminal")

    def _use_default(self):
        if self.info and self.info.default_w is not None:
            self.limit_row.set_value(self.info.default_w)

    def _run_limit(self):
        cmd = self._limit_command()
        if cmd:
            self.win.run_in_terminal(cmd, self.refresh)

    def _run_persist(self):
        cmd = gpumod.persistence_command(not self.persistence)
        self.win.run_in_terminal(cmd, self.refresh)


# ------------------------------------------------------------------------------------------------------------
# About page
# ------------------------------------------------------------------------------------------------------------

class AboutPage:
    NAMES = ("NV Control", "libnvvsr", "NVIDIA kernel driver", "NVIDIA VFX wheel", "Private mpv", "System mpv",
             "VLC", "GTK", "libadwaita", "Python", "Kernel")

    def __init__(self, win):
        self.win = win
        self.versions = {}
        box = vbox(18)
        grp = Adw.PreferencesGroup(title="Versions")
        self.labels = {}
        for n in self.NAMES:
            row, lbl = kv_row(n)
            grp.add(row)
            self.labels[n] = lbl
        box.append(grp)

        actions = Adw.PreferencesGroup(title="Diagnostics")
        r = Adw.ActionRow(title="Copy diagnostics", subtitle="Plain text: versions, GPU state, install status and settings. "
                                                              "No cookies or other secrets; your home directory is shortened.")
        self.copy_btn = Gtk.Button(label="Copy diagnostics", valign=Gtk.Align.CENTER, css_classes=["suggested-action"])
        self.copy_btn.connect("clicked", lambda *_: self.copy_diagnostics())
        r.add_suffix(self.copy_btn)
        actions.add(r)
        self.readme = paths.repo_dir(APP_DIR)
        readme = os.path.join(self.readme, "README.md") if self.readme else None
        rr = Adw.ActionRow(title="README", subtitle=short_path(readme) if readme and os.path.isfile(readme)
                           else "No README.md found next to the application")
        open_btn = Gtk.Button(label="Open", valign=Gtk.Align.CENTER, sensitive=bool(readme and os.path.isfile(readme)))
        open_btn.connect("clicked", lambda *_: self._open(readme))
        rr.add_suffix(open_btn)
        actions.add(rr)
        box.append(actions)
        box.append(hint("NV Control is an independent tool for this machine. It is not affiliated with or endorsed by NVIDIA."))
        self.widget = make_page(box)
        self.set_toolkits()

    def _open(self, path):
        try:
            Gtk.FileLauncher.new(Gio.File.new_for_path(path)).launch(self.win, None, lambda *a: None)
        except Exception:  # noqa: BLE001
            self.win.toast("Could not open the README")

    def set_toolkits(self):
        self.labels["GTK"].set_label(f"{Gtk.get_major_version()}.{Gtk.get_minor_version()}.{Gtk.get_micro_version()}")
        self.labels["libadwaita"].set_label(f"{Adw.get_major_version()}.{Adw.get_minor_version()}.{Adw.get_micro_version()}")

    def refresh(self):
        def done(res):
            if isinstance(res, Exception):
                return
            self.versions = res
            for k, v in res.items():
                if k in self.labels:
                    self.labels[k].set_label(v or "not found")

        run_async(lambda: diag.collect_versions(), done)

    def copy_diagnostics(self, done=None):
        w = self.win

        def job():
            st = sr.compute_status(sr.read_install_json(), paths.repo_dir(APP_DIR))
            versions = diag.collect_versions(st)
            hybrid = gpumod.supergfxctl_mode()
            return st, versions, hybrid

        def finished(res):
            if isinstance(res, Exception):
                w.toast("Could not build the diagnostics")
                return
            st, versions, hybrid = res
            text = diag.build_report(
                versions=versions, status=st, runtime=w.runtime, sample=w.sample, hybrid=hybrid,
                power_src=gpumod.power_source(), toolkit_versions={
                    "GTK": self.labels["GTK"].get_label(), "libadwaita": self.labels["libadwaita"].get_label()})
            Gdk.Display.get_default().get_clipboard().set(text)
            w.toast("Diagnostics copied")
            if done:
                done(text)

        run_async(job, finished)


# ------------------------------------------------------------------------------------------------------------
# Window and application
# ------------------------------------------------------------------------------------------------------------

class MainWindow(Adw.ApplicationWindow):
    def __init__(self, **kw):
        super().__init__(**kw)
        self.set_title(nvc.APP_NAME)
        self.set_default_size(960, 700)
        self.set_size_request(340, 420)
        self.settings = Settings()
        self.gpu = (gpumod.find_nvidia_gpus() or [None])[0]
        self.runtime = gpumod.read_runtime(self.gpu)
        self.sample, self.apps_mono = None, 0.0
        self.in_flight, self.last_query_mono = False, None
        self.hybrid, self.power_src = None, gpumod.power_source()
        self._mapped, self._surface_hidden, self._state_handler, self._tick_id = False, False, None, 0

        self.toast_overlay = Adw.ToastOverlay()
        self.set_content(self.toast_overlay)
        self.toolbar_view = Adw.ToolbarView()
        self.toast_overlay.set_child(self.toolbar_view)

        self.stack = Adw.ViewStack()
        self.overview = OverviewPage(self)
        self.sr = SrPage(self)
        self.power = PowerPage(self)
        self.about = AboutPage(self)
        for page, name, title, icon in ((self.overview, "overview", "Overview", "nvc-gauge-symbolic"),
                                        (self.sr, "sr", "Super Resolution", "nvc-sparkle-symbolic"),
                                        (self.power, "power", "Power", "nvc-bolt-symbolic"),
                                        (self.about, "about", "About", "nvc-info-symbolic")):
            self.stack.add_titled_with_icon(page.widget, name, title, icon)

        self.switcher_title = Adw.ViewSwitcherTitle(stack=self.stack, title=nvc.APP_NAME)
        header = Adw.HeaderBar(title_widget=self.switcher_title)
        refresh = Gtk.Button(icon_name="nvc-refresh-symbolic", tooltip_text="Refresh (F5)")
        refresh.update_property([Gtk.AccessibleProperty.LABEL], ["Refresh"])
        refresh.connect("clicked", lambda *_: self.refresh_current())
        header.pack_end(refresh)
        self.toolbar_view.add_top_bar(header)
        self.switcher_bar = Adw.ViewSwitcherBar(stack=self.stack)
        self.switcher_title.bind_property("title-visible", self.switcher_bar, "reveal", GObject.BindingFlags.SYNC_CREATE)
        self.toolbar_view.add_bottom_bar(self.switcher_bar)
        self.toolbar_view.set_content(self.stack)

        self._actions()
        self.stack.connect("notify::visible-child-name", self._page_changed)
        self.connect("map", self._on_map)
        self.connect("unmap", lambda *_: setattr(self, "_mapped", False))
        self.connect("close-request", self._on_close)
        self.overview.set_static(self.gpu, self.runtime, None, self.power_src, gpumod.driver_version_cheap())
        self.overview.set_state(*self._state_chip())
        self.overview.show_asleep(gpumod.gpu_state(self.runtime.status) == "asleep")
        run_async(gpumod.supergfxctl_mode, self._hybrid_done)
        self._tick_id = GLib.timeout_add_seconds(1, self._tick)

    # -- actions --------------------------------------------------------------------------------------------
    def _actions(self):
        act = Gio.SimpleAction.new("page", GLib.VariantType.new("s"))
        act.connect("activate", lambda a, p: self.stack.set_visible_child_name(p.get_string()))
        self.add_action(act)
        r = Gio.SimpleAction.new("refresh", None)
        r.connect("activate", lambda *_: self.refresh_current())
        self.add_action(r)

    def toast(self, text):
        self.toast_overlay.add_toast(Adw.Toast(title=text, timeout=3))

    def run_in_terminal(self, command, on_done=None):
        argv = proc.terminal_argv(command)
        if not argv:
            self.toast("No terminal emulator found (kitty or alacritty)")
            return
        try:
            p = Gio.Subprocess.new(argv, Gio.SubprocessFlags.NONE)
        except GLib.Error as e:
            self.toast(f"Could not open a terminal: {e.message}")
            return

        def waited(source, result):
            try:
                source.wait_finish(result)
            except GLib.Error:
                pass
            if on_done:
                GLib.idle_add(on_done)

        p.wait_async(None, waited)

    # -- visibility -----------------------------------------------------------------------------------------
    def _on_map(self, *_):
        self._mapped = True
        surface = self.get_surface()
        if surface is not None and not self._state_handler:
            self._state_handler = surface.connect("notify::state", self._surface_state)
        self._page_changed()

    def _surface_state(self, surface, _p):
        hidden = Gdk.ToplevelState.MINIMIZED
        suspended = getattr(Gdk.ToplevelState, "SUSPENDED", 0)
        self._surface_hidden = bool(surface.get_state() & (hidden | suspended))

    def window_visible(self):
        return self._mapped and not self._surface_hidden

    def _on_close(self, *_):
        if self._tick_id:
            GLib.source_remove(self._tick_id)
            self._tick_id = 0
        return False

    # -- GPU polling ----------------------------------------------------------------------------------------
    def _state_chip(self):
        st = gpumod.gpu_state(self.runtime.status)
        if self.gpu is None:
            return "bad", "No NVIDIA GPU"
        return {"asleep": ("warn", "Asleep"), "awake": ("ok", "Awake"), "unknown": (None, "State unknown")}[st]

    def _hybrid_done(self, res):
        self.hybrid = res if isinstance(res, str) else None
        self.overview.rows["hybrid"].set_label(self.hybrid or "unknown")

    def _tick(self):
        if not self.window_visible():
            return GLib.SOURCE_CONTINUE
        on_overview = self.stack.get_visible_child_name() == "overview"
        if not on_overview:
            return GLib.SOURCE_CONTINUE
        self.runtime = gpumod.read_runtime(self.gpu)
        self.power_src = gpumod.power_source()
        state = gpumod.gpu_state(self.runtime.status)
        self.overview.set_static(self.gpu, self.runtime, self.hybrid, self.power_src,
                                 (self.sample.driver if self.sample else None) or gpumod.driver_version_cheap())
        self.overview.set_state(*self._state_chip())
        since = None if self.last_query_mono is None else time.monotonic() - self.last_query_mono
        decision = gpumod.poll_decision(
            window_visible=True, page_active=True, state=state if self.gpu else "asleep",
            in_flight=self.in_flight, interval_s=self.settings.poll_interval, since_last_s=since)
        self.overview.show_asleep(decision == "asleep" and self.gpu is not None)
        self.overview.set_stale(decision == "asleep")
        if decision == "query":
            self.query_now()
        return GLib.SOURCE_CONTINUE

    def query_now(self, wake=False):
        """One nvidia-smi read. Only reached when the GPU is awake or the user clicked 'Wake and read'."""
        if self.in_flight:
            return
        self.in_flight = True
        want_apps = wake or time.monotonic() - self.apps_mono >= COMPUTE_APPS_EVERY

        def job():
            sample, err = gpumod.query_gpu()
            apps = None
            if sample is not None and want_apps:
                apps, _ = gpumod.query_compute_apps()
            return sample, err, apps

        def done(res):
            self.in_flight = False
            self.last_query_mono = time.monotonic()
            if isinstance(res, Exception):
                self.overview.set_error(f"Could not read the GPU: {res}")
                return
            sample, err, apps = res
            if sample is None:
                self.overview.set_error(f"nvidia-smi failed: {err}")
                return
            self.on_sample(sample)
            if apps is not None:
                self.apps_mono = time.monotonic()
                self.overview.set_processes(apps)
            self.runtime = gpumod.read_runtime(self.gpu)
            self.overview.show_asleep(False)
            self.overview.set_stale(False)
            self.overview.set_state(*self._state_chip())

        run_async(job, done)

    def on_sample(self, sample):
        self.sample = sample
        self.overview.set_sample(sample)

    # -- navigation / refresh -------------------------------------------------------------------------------
    def _page_changed(self, *_):
        name = self.stack.get_visible_child_name()
        if not self.window_visible():
            return
        if name == "overview":
            self._tick()
        elif name == "sr":
            self.sr.refresh()
        elif name == "power":
            self.power.refresh()
        elif name == "about":
            self.about.refresh()

    def refresh_current(self):
        name = self.stack.get_visible_child_name()
        if name == "overview":
            self.runtime = gpumod.read_runtime(self.gpu)
            if gpumod.gpu_state(self.runtime.status) != "asleep":
                self.query_now(wake=True)
            else:
                self._tick()
                self.toast("GPU is asleep: use Wake and read to query it")
        else:
            self._page_changed()

    # -- smoke test -----------------------------------------------------------------------------------------
    def run_smoke(self, seconds, out_path):
        """Headless self test: cycle the pages, save settings, narrow the window, write a JSON result, quit."""
        result = {"pages": [], "errors": [], "icons_missing": [], "bar_reveal_wide": None, "bar_reveal_narrow": None}
        theme_icons = Gtk.IconTheme.get_for_display(Gdk.Display.get_default())
        for n in ("nvc-gauge-symbolic", "nvc-sparkle-symbolic", "nvc-bolt-symbolic", "nvc-info-symbolic",
                  "nvc-refresh-symbolic", "nvc-check-symbolic",
                  "nvc-warning-symbolic", "nvc-sleep-symbolic", nvc.APP_ID):
            if not theme_icons.has_icon(n):
                result["icons_missing"].append(n)
        step_s = max(0.6, seconds / 12.0)
        clamp = Adw.Clamp(maximum_size=400, tightening_threshold=400)

        def narrow():
            self.toast_overlay.set_child(None)
            clamp.set_child(self.toolbar_view)
            self.toast_overlay.set_child(clamp)

        def probe():
            self.sr.run_check(done=lambda r: result.update(probe=[r.ok, r.summary]))

        def save_settings():
            self.sr.mpv.set_value("quality", "high")
            self.sr.mpv.set_value("osd_opacity", 0.7)
            self.sr.mpv.save(done=lambda r: result.update(mpv_save=[r.ok, r.wrote, r.error]))
            self.sr.vlc.set_value("nvvsr-quality", "high")
            self.sr.vlc.set_value("nvvsr-ac-only", False)
            self.sr.vlc.save(done=lambda r: result.update(vlc_save=[r.ok, r.wrote, r.error]))

        def click_persistence():  # what a click on the button does; the (fake) terminal logs its argv
            result["persist_cmd"] = self.power.persist_cmd.get_label()
            self.power.persist_btn.emit("clicked")

        steps = [
            ("overview", None), ("sr", None), ("sr", probe), ("sr", save_settings), ("power", None),
            ("power", click_persistence),
            ("about", lambda: self.about.copy_diagnostics(done=lambda t: result.update(report=t))),
            ("about", lambda: result.update(bar_reveal_wide=self.switcher_bar.get_reveal())), ("about", narrow),
            ("overview", lambda: result.update(bar_reveal_narrow=self.switcher_bar.get_reveal())),
            ("overview", None),
        ]
        idx = {"i": 0}

        def step():
            if idx["i"] >= len(steps):
                result["asleep_card"] = self.overview.asleep.get_visible()
                result["overview_title"] = self.overview.hero_title.get_label()
                result["mem_tile"] = self.overview.tiles["mem"].value.get_label()
                result["hybrid"] = self.overview.rows["hybrid"].get_label()
                result["power_limit_row"] = self.power.rows["limit"].get_label()
                result["sr_headline"] = self.sr.status.headline if self.sr.status else None
                result["versions"] = dict(self.about.versions)
                with open(out_path, "w") as f:
                    json.dump(result, f, indent=1, default=str)
                self.get_application().quit()
                return GLib.SOURCE_REMOVE
            name, action = steps[idx["i"]]
            idx["i"] += 1
            self.stack.set_visible_child_name(name)
            result["pages"].append(name)
            if action:
                try:
                    action()
                except Exception as e:  # noqa: BLE001
                    result["errors"].append(repr(e))
            return GLib.SOURCE_CONTINUE

        GLib.timeout_add(int(step_s * 1000), step)


class Application(Adw.Application):
    def __init__(self, smoke=None):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.DEFAULT_FLAGS)
        self.smoke, self._smoke_started = smoke, False
        self.set_accels_for_action("win.refresh", ["F5", "<Control>r"])
        for i, name in enumerate(("overview", "sr", "power", "about"), 1):
            self.set_accels_for_action(f"win.page::{name}", [f"<Alt>{i}", f"<Control>{i}"])
        self.set_accels_for_action("app.quit", ["<Control>q"])
        q = Gio.SimpleAction.new("quit", None)
        q.connect("activate", lambda *_: self.quit())
        self.add_action(q)

    def do_startup(self):
        Adw.Application.do_startup(self)
        display = Gdk.Display.get_default()
        Gtk.IconTheme.get_for_display(display).add_search_path(ICONS_DIR)
        Gtk.Window.set_default_icon_name(nvc.APP_ID)
        colours, mode = theme.load_scheme()
        Adw.StyleManager.get_default().set_color_scheme(
            Adw.ColorScheme.FORCE_LIGHT if mode == "light" else Adw.ColorScheme.FORCE_DARK)
        css = Gtk.CssProvider()
        css.load_from_string(theme.build_material_css(colours))
        Gtk.StyleContext.add_provider_for_display(display, css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def do_activate(self):
        win = self.props.active_window or MainWindow(application=self)
        win.present()
        if self.smoke and not self._smoke_started:
            self._smoke_started = True
            win.run_smoke(*self.smoke)


def main(argv=None):
    ap = argparse.ArgumentParser(description="NV Control")
    ap.add_argument("--smoke", nargs=2, metavar=("SECONDS", "RESULT_JSON"), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    smoke = (float(args.smoke[0]), args.smoke[1]) if args.smoke else None
    return Application(smoke).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
