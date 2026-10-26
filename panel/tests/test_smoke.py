"""Starts the real app inside a HEADLESS cage compositor (never the user's desktop) with a fake sysfs, a fake
nvidia-smi and a scratch HOME, lets it cycle through every page, and checks the result and the log.

Set NVC_SKIP_SMOKE=1 to skip (it needs cage and python3-gi; takes about 12 s per scenario).
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

import support

NEEDED = ("cage",)
SKIP = bool(os.environ.get("NVC_SKIP_SMOKE")) or any(shutil.which(t) is None for t in NEEDED)

# cage/wlroots log lines ("00:00:00.000 [INFO] ...", including its failed XWayland socket bind) and the message GTK
# prints when cage exits the moment the window is gone are not bugs of the app
BENIGN = (r"^\d\d:\d\d:\d\d\.\d+ \[", r"Lost connection to Wayland compositor")


def slurp(path):
    with open(path) as f:
        return f.read()


def run_app(runtime_status="active", seconds=10, extra_env=None):
    base = tempfile.mkdtemp(prefix="nvc-smoke-", dir="/dev/shm" if os.path.isdir("/dev/shm") else None)
    try:
        home = os.path.join(base, "home")
        repo = os.path.join(base, "repo")
        rt = os.path.join(base, "run")
        os.makedirs(home)
        os.makedirs(rt, mode=0o700)
        fakelib = support.build_fake_lib(os.path.join(base, "fakelib"))
        paths_ = support.make_home(home, repo, fake_lib=fakelib)
        sysfs = support.make_sysfs(os.path.join(base, "sys"), runtime_status)
        log = os.path.join(base, "nvidia-smi.log")
        term_log = os.path.join(base, "terminal.log")
        bindir = support.make_fake_bin(os.path.join(base, "bin"), log, term_log)
        result = os.path.join(base, "result.json")
        env = {k: os.environ[k] for k in ("LANG", "LC_ALL", "TERM") if k in os.environ}
        env.update({
            "PATH": bindir + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": home, "XDG_RUNTIME_DIR": rt, "NVC_SYSFS_ROOT": sysfs, "NVC_REPO_DIR": repo,
            "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_LIBINPUT_NO_DEVICES": "1",
            "GSK_RENDERER": "cairo", "GDK_BACKEND": "wayland", "LIBGL_ALWAYS_SOFTWARE": "1",
            "PYTHONDONTWRITEBYTECODE": "1", "GTK_A11Y": "none", "GSETTINGS_BACKEND": "memory", "NO_AT_BRIDGE": "1",
        })
        env.update(extra_env or {})
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "HYPRLAND_INSTANCE_SIGNATURE"):
            assert k not in env, k  # the real session must never leak into the test
        cmd = ["cage", "--", sys.executable, os.path.join(support.PANEL, "main.py"),
               "--smoke", str(seconds), result]
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120, cwd=base)
        out = {"rc": p.returncode, "log": p.stdout + "\n" + p.stderr, "home": home, "paths": paths_,
               "result": None, "smi_calls": [], "terminal": []}
        if os.path.exists(result):
            out["result"] = json.loads(slurp(result))
        out["smi_calls"] = slurp(log).splitlines() if os.path.exists(log) else []
        out["terminal"] = slurp(term_log).splitlines() if os.path.exists(term_log) else []
        out["files"] = {n: slurp(paths_[n]) for n in ("mpv_conf", "vlcrc")}
        out["baks"] = [os.path.exists(paths_["mpv_conf"] + ".bak-nvvsr"), os.path.exists(paths_["vlcrc"] + ".bak-nvvsr")]
        return out
    finally:
        shutil.rmtree(base, ignore_errors=True)


def problems(log):
    bad = []
    for line in log.splitlines():
        if not line.strip():
            continue
        if "Traceback" in line or re.search(r"\b(Warning|WARNING|CRITICAL|ERROR|Error)\b", line) or "Gtk-" in line \
                or "Adw-" in line or "GLib-" in line or "Gdk-" in line or "Pango" in line:
            if not any(re.search(b, line) for b in BENIGN):
                bad.append(line)
    return bad


@unittest.skipIf(SKIP, "cage not available or NVC_SKIP_SMOKE set")
class SmokeAwake(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.o = run_app("active", seconds=13)

    def test_exit_and_log_clean(self):
        self.assertEqual(self.o["rc"], 0, self.o["log"][-2000:])
        self.assertIsNotNone(self.o["result"], self.o["log"][-2000:])
        self.assertEqual(problems(self.o["log"]), [], self.o["log"][-3000:])

    def test_all_pages_visited_without_errors(self):
        r = self.o["result"]
        self.assertEqual(set(r["pages"]), {"overview", "sr", "power", "about"})
        self.assertEqual(r["errors"], [])
        self.assertEqual(r["icons_missing"], [])

    def test_overview_polled_the_fake_gpu_but_not_in_a_loop(self):
        r = self.o["result"]
        self.assertIn("RTX 3070 Ti", r["overview_title"])
        self.assertEqual(r["mem_tile"], "1.3 / 8.0 GiB")
        self.assertEqual(r["hybrid"], "Hybrid")
        gpu_calls = [c for c in self.o["smi_calls"] if "--query-gpu" in c]
        self.assertGreaterEqual(len(gpu_calls), 1)
        self.assertLessEqual(len(self.o["smi_calls"]), 16, self.o["smi_calls"])

    def test_power_page_read_limits(self):
        self.assertEqual(self.o["result"]["power_limit_row"], "125 W")

    def test_sr_status_and_settings_saved_to_native_files(self):
        r = self.o["result"]
        self.assertEqual(r["sr_headline"], "Ready for mpv and VLC")
        self.assertEqual(r["mpv_save"][:2], [True, True], r["mpv_save"])
        self.assertEqual(r["vlc_save"][:2], [True, True], r["vlc_save"])
        mpv = self.o["files"]["mpv_conf"]
        self.assertIn("quality=high", mpv)
        self.assertIn("osd_opacity=0.7", mpv)
        self.assertIn("# vsr.conf - options of mpv/vsr.lua", mpv)  # comments kept
        vlc = self.o["files"]["vlcrc"]
        self.assertIn("nvvsr-quality=high", vlc)
        self.assertIn("nvvsr-ac-only=0", vlc)
        self.assertIn("avcodec-hw=any", vlc)
        self.assertEqual(self.o["baks"], [True, True])

    def test_probe_button_works_with_the_fake_library(self):
        self.assertEqual(self.o["result"]["probe"], [True, "Super resolution works"])

    def test_versions_filled(self):
        v = self.o["result"]["versions"]
        self.assertEqual(v["libnvvsr"], "9.9.9-fake (API 1)")
        self.assertEqual(v["NVIDIA VFX wheel"], "0.2.0.0")
        self.assertEqual(v["NVIDIA kernel driver"], "615.78.08")

    def test_narrow_width_reveals_the_bottom_switcher_bar(self):
        r = self.o["result"]
        self.assertFalse(r["bar_reveal_wide"])
        self.assertTrue(r["bar_reveal_narrow"])

    def test_terminal_opens_only_on_click_with_exactly_the_shown_command(self):
        r = self.o["result"]
        self.assertEqual(r["persist_cmd"], "sudo nvidia-smi -pm 1")
        self.assertEqual(self.o["terminal"], ["kitty --hold -e sudo nvidia-smi -pm 1"])  # one click, nothing else

    def test_copied_diagnostics_have_no_secrets(self):
        report = self.o["result"]["report"]
        self.assertIn("Ready for mpv and VLC", report)
        self.assertNotIn("do-not-leak", report)
        self.assertNotIn(self.o["home"], report)


@unittest.skipIf(SKIP, "cage not available or NVC_SKIP_SMOKE set")
class SmokeAsleep(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.o = run_app("suspended", seconds=8)

    def test_gpu_is_never_queried_while_asleep(self):
        self.assertEqual(self.o["rc"], 0, self.o["log"][-2000:])
        self.assertEqual(self.o["smi_calls"], [], "nvidia-smi was called while runtime_status=suspended")

    def test_asleep_card_shown_and_log_clean(self):
        self.assertTrue(self.o["result"]["asleep_card"])
        self.assertEqual(problems(self.o["log"]), [], self.o["log"][-3000:])
        self.assertEqual(self.o["result"]["errors"], [])


if __name__ == "__main__":
    unittest.main()
