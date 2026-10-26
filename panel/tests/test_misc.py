import json
import os
import shutil
import subprocess
import tempfile
import unittest

import support
from nvc import proc, theme

LAUNCHER = os.path.join(support.PANEL, "install-launcher.sh")


class Proc(unittest.TestCase):
    def test_run_never_raises(self):
        self.assertTrue(proc.run(["nvc-no-such-tool"]).missing)
        self.assertEqual(proc.run(["nvc-no-such-tool"]).error_text, "nvc-no-such-tool not found")
        self.assertTrue(proc.run(["sleep", "5"], timeout=0.2).timed_out)
        r = proc.run(["sh", "-c", "echo out; echo oops >&2; exit 3"])
        self.assertEqual((r.rc, r.out.strip(), r.error_text), (3, "out", "oops"))

    def test_terminal_choice_and_command_are_never_a_shell_string(self):
        which = lambda name: "/usr/bin/" + name if name == "alacritty" else None
        self.assertEqual(proc.terminal_argv(["sudo", "nvidia-smi", "-pl", "120"], which),
                         ["alacritty", "--hold", "-e", "sudo", "nvidia-smi", "-pl", "120"])
        both = lambda name: "/usr/bin/" + name
        self.assertEqual(proc.terminal_argv(["id"], both)[:3], ["kitty", "--hold", "-e"])
        self.assertIsNone(proc.terminal_argv(["id"], lambda n: None))

    def test_display_command_quotes(self):
        self.assertEqual(proc.display_command(["sudo", "nvidia-smi", "-pl", "120"]), "sudo nvidia-smi -pl 120")
        self.assertEqual(proc.display_command(["echo", "a b"]), "echo 'a b'")


class Theme(unittest.TestCase):
    def write(self, text):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        p = os.path.join(d, "scheme.json")
        with open(p, "w") as f:
            f.write(text)
        return p

    def test_caelestia_nested_scheme(self):
        colours, mode = theme.load_scheme(self.write(json.dumps(support.SCHEME)))
        self.assertEqual((colours["primary"], mode), ("#7fd9c3", "dark"))
        self.assertIn("surfaceContainer", colours)

    def test_flat_scheme_and_light_mode(self):
        colours, mode = theme.load_scheme(self.write(json.dumps({"mode": "light", "surface": "ffffff", "primary": "#123456"})))
        self.assertEqual((colours["primary"], colours["surface"], mode), ("#123456", "#ffffff", "light"))

    def test_missing_or_broken_falls_back(self):
        for p in ("/nonexistent/scheme.json", self.write("{not json"), self.write('{"colours": {}}'), self.write("[]")):
            colours, mode = theme.load_scheme(p)
            self.assertEqual((colours, mode), (theme.FALLBACK_MATERIAL_COLORS, "dark"))

    def test_css_sets_accent_buttons_explicitly(self):
        css = theme.build_material_css(theme.FALLBACK_MATERIAL_COLORS)
        self.assertIn("button.suggested-action { background-color: #f7b6b9; color: #603337; }", css)
        self.assertIn("button.destructive-action { background-color: #f97758;", css)
        self.assertNotIn("{col(", css)


class Launcher(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = self.tmp.name
        self.desktop = os.path.join(self.home, ".local/share/applications/org.nvvsr.Control.desktop")
        self.icon = os.path.join(self.home, ".local/share/icons/hicolor/scalable/apps/org.nvvsr.Control.svg")
        # a hostile environment: the real data dir must never be used when --home is given
        self.env = {**os.environ, "HOME": "/nonexistent-home", "XDG_DATA_HOME": "/nonexistent-xdg"}

    def run_sh(self, *args):
        return subprocess.run(["sh", LAUNCHER, *args], capture_output=True, text=True, env=self.env)

    def test_install_is_correct_idempotent_and_removable(self):
        r = self.run_sh("--home", self.home)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.desktop) as f:
            text = f.read()
        self.assertIn(f"Exec=/usr/bin/python3 {support.PANEL}/main.py\n", text)
        for line in ("Icon=org.nvvsr.Control", "StartupWMClass=org.nvvsr.Control", "Terminal=false",
                     "Categories=System;Settings;HardwareSettings;"):
            self.assertIn(line + "\n", text)
        self.assertTrue(os.path.isfile(self.icon))
        mtime = os.stat(self.desktop).st_mtime_ns
        r = self.run_sh("--home", self.home)
        self.assertIn("already up to date", r.stdout)
        self.assertEqual(os.stat(self.desktop).st_mtime_ns, mtime)
        self.assertEqual(self.run_sh("--uninstall", "--home", self.home).returncode, 0)
        self.assertFalse(os.path.exists(self.desktop) or os.path.exists(self.icon))
        self.assertEqual(self.run_sh("--uninstall", "--home", self.home).returncode, 0)  # twice is fine

    def test_repairs_a_stale_launcher(self):
        self.run_sh("--home", self.home)
        with open(self.desktop, "w") as f:
            f.write("[Desktop Entry]\nType=Application\nName=old\nExec=true\n")
        self.assertIn("installed", self.run_sh("--home", self.home).stdout)
        with open(self.desktop) as f:
            self.assertIn("Name=NV Control", f.read())

    def test_bad_arguments(self):
        self.assertEqual(self.run_sh("--bogus").returncode, 2)
        self.assertEqual(self.run_sh("--home").returncode, 2)

    def test_nothing_is_written_outside_the_given_home(self):
        self.run_sh("--home", self.home)
        for root, _, files in os.walk(self.home):
            for f in files:
                self.assertTrue(root.startswith(os.path.join(self.home, ".local")), os.path.join(root, f))


if __name__ == "__main__":
    unittest.main()
