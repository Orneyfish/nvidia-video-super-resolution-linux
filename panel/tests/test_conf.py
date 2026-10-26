import os
import re
import tempfile
import unittest

import support
from nvc import conf


class ConfFileEdits(unittest.TestCase):
    def test_roundtrip_is_identity(self):
        for text in ("", "a=1\n", "# c\n\na=1\nb = 2\n\n", "a=1", "x=1\r\ny=2\r\n"):
            self.assertEqual(conf.ConfFile(text).render(), text)

    def test_set_replaces_active_keeps_everything_else(self):
        c = conf.ConfFile("# top\nquality = high\nunknown=keep me\n#osd=yes\n")
        c.set("quality", "low")
        self.assertEqual(c.render(), "# top\nquality = low\nunknown=keep me\n#osd=yes\n")

    def test_set_uncomments_documented_default_in_place(self):
        c = conf.ConfFile("# doc\n#quality=auto\n# more doc\n")
        c.set("quality", "high")
        self.assertEqual(c.render(), "# doc\nquality=high\n# more doc\n")

    def test_prose_comment_is_not_taken_for_a_default(self):
        c = conf.ConfFile("# quality is great\n# Quality: auto | low\n")
        c.set("quality", "low")
        self.assertEqual(c.render(), "# quality is great\n# Quality: auto | low\nquality=low\n")

    def test_last_assignment_wins_like_the_player(self):
        c = conf.ConfFile("a=1\na=2\n")
        self.assertEqual(c.get("a"), "2")
        c.set("a", "3")
        self.assertEqual(c.render(), "a=1\na=3\n")

    def test_unset_restores_documented_default(self):
        c = conf.ConfFile("quality=high\nother=1\n")
        c.unset("quality", "auto")
        self.assertEqual(c.render(), "#quality=auto\nother=1\n")
        c = conf.ConfFile("#quality=auto\nquality=high\n")
        c.unset("quality", "auto")
        self.assertEqual(c.render(), "#quality=auto\n")

    def test_section_append_goes_into_existing_section(self):
        c = conf.ConfFile("[a]\nx=1\n\n[nvvsr]\nfoo=1\n\n[z]\nq=1\n")
        c.set("nvvsr-quality", "high", section="nvvsr")
        self.assertEqual(c.render(), "[a]\nx=1\n\n[nvvsr]\nfoo=1\nnvvsr-quality=high\n\n[z]\nq=1\n")

    def test_section_created_when_missing(self):
        c = conf.ConfFile("[core]\nv=1\n")
        c.set("nvvsr-enable", "0", section="nvvsr")
        self.assertEqual(c.render(), "[core]\nv=1\n\n[nvvsr]\nnvvsr-enable=0\n")

    def test_multiline_value_rejected(self):
        with self.assertRaises(ValueError):
            conf.ConfFile("").set("a", "x\ny")


class Values(unittest.TestCase):
    def opt(self, key, options=conf.MPV_OPTIONS):
        return next(o for o in options if o.key == key)

    def test_parse_and_validate(self):
        self.assertIs(conf.parse_value(self.opt("ac_only"), " No "), False)
        self.assertEqual(conf.parse_value(self.opt("pool"), "3"), 3)
        self.assertEqual(conf.parse_value(self.opt("osd_opacity"), "0.7"), 0.7)
        self.assertEqual(conf.parse_value(self.opt("osd_position"), "4"), 4)
        self.assertEqual(conf.parse_value(self.opt("strength"), ""), "")
        self.assertEqual(conf.parse_value(self.opt("quality"), "High"), "high")
        for key, bad in (("pool", "9"), ("pool", "1.5"), ("osd_opacity", "0"), ("ac_only", "maybe"),
                         ("quality", "ultraa"), ("strength", "2"), ("min_height", "x")):
            with self.assertRaises(ValueError, msg=(key, bad)):
                conf.parse_value(self.opt(key), bad)

    def test_format(self):
        self.assertEqual(conf.format_value(self.opt("ac_only"), True), "yes")
        self.assertEqual(conf.format_value(self.opt("nvvsr-ac-only", conf.VLC_OPTIONS), False, "01"), "0")
        self.assertEqual(conf.format_value(self.opt("osd_opacity"), 0.55000001), "0.55")
        self.assertEqual(conf.format_value(self.opt("osd_duration"), 2.0), "2")
        self.assertEqual(conf.format_value(self.opt("pool"), 2.0), "2")

    def test_option_tables_match_the_players_own_files(self):
        """The defaults here must not drift from mpv/script-opts/vsr.conf and vlc/src/plugin.c."""
        with open(os.path.join(support.REPO, "mpv", "script-opts", "vsr.conf")) as f:
            documented = dict(re.findall(r"^#([a-z_0-9]+)=(.*)$", f.read(), re.M))
        for o in conf.MPV_OPTIONS:
            self.assertIn(o.key, documented, o.key)
            self.assertEqual(conf.parse_value(o, documented[o.key]), o.default, o.key)
        with open(os.path.join(support.REPO, "vlc", "src", "plugin.c")) as f:
            src = f.read()
        for o in conf.VLC_OPTIONS:
            m = re.search(r'NV_CFG "%s", ("?[^,]*"?),' % re.escape(o.key[len("nvvsr-"):]), src)
            self.assertIsNotNone(m, o.key)
            raw = m.group(1).strip('"')
            want = {"true": True, "false": False}.get(raw, int(raw) if re.fullmatch(r"\d+", raw) else raw)
            self.assertEqual(o.default, want, o.key)


class SaveChanges(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "mpv", "vsr.conf")

    def write(self, text):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w") as f:
            f.write(text)

    def read(self, path=None):
        with open(path or self.path) as f:
            return f.read()

    def save(self, loaded, **edits):
        return conf.save_changes(self.path, conf.MPV_OPTIONS, {**loaded.values, **edits}, loaded)

    def test_new_file_is_created_with_only_the_changed_key(self):
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.assertFalse(loaded.exists)
        self.assertEqual(loaded.values["quality"], "auto")
        r = self.save(loaded, quality="high")
        self.assertTrue(r.ok and r.wrote)
        self.assertEqual(self.read(), "quality=high\n")
        self.assertIsNone(r.backup)

    def test_unchanged_values_write_nothing(self):
        self.write("# keep\n#quality=auto\n")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        r = self.save(loaded)
        self.assertTrue(r.ok)
        self.assertFalse(r.wrote)
        self.assertFalse(os.path.exists(self.path + ".bak-nvvsr"))

    def test_edit_keeps_comments_and_unknown_keys_and_backs_up_once(self):
        original = "# my notes\nfuture_option=7\n#quality=auto\nosd=no\n"
        self.write(original)
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        r = self.save(loaded, quality="ultra", osd=True)
        self.assertEqual(self.read(), "# my notes\nfuture_option=7\nquality=ultra\n#osd=yes\n")  # yes is the default
        self.assertEqual(self.read(self.path + ".bak-nvvsr"), original)
        self.assertEqual(r.backup, self.path + ".bak-nvvsr")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        r = self.save(loaded, quality="low")
        self.assertIsNone(r.backup)  # the first backup (the user's original) is never overwritten
        self.assertEqual(self.read(self.path + ".bak-nvvsr"), original)

    def test_back_to_default_restores_the_documented_line(self):
        self.write("quality=high\n")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.save(loaded, quality="auto")
        self.assertEqual(self.read(), "#quality=auto\n")

    def test_external_edit_of_other_keys_is_merged_not_overwritten(self):
        self.write("quality=high\n")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.write("quality=high\nmy_new_key=1\n")  # edited elsewhere after we loaded
        r = self.save(loaded, osd=False)
        self.assertTrue(r.ok)
        self.assertEqual(self.read(), "quality=high\nmy_new_key=1\nosd=no\n")

    def test_external_edit_of_the_same_key_is_a_conflict(self):
        self.write("quality=high\n")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.write("quality=low\n")
        r = self.save(loaded, quality="ultra")
        self.assertFalse(r.ok)
        self.assertEqual(r.conflicts, ["quality"])
        self.assertEqual(self.read(), "quality=low\n")  # untouched
        r = conf.save_changes(self.path, conf.MPV_OPTIONS, {**loaded.values, "quality": "ultra"}, loaded, force=True)
        self.assertTrue(r.ok)
        self.assertEqual(self.read(), "quality=ultra\n")

    def test_symlinked_dotfile_stays_a_symlink(self):
        real = os.path.join(self.tmp.name, "dotfiles", "vsr.conf")
        os.makedirs(os.path.dirname(real))
        with open(real, "w") as f:
            f.write("#quality=auto\n")
        os.makedirs(os.path.dirname(self.path))
        os.symlink(real, self.path)
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.save(loaded, quality="high")
        self.assertTrue(os.path.islink(self.path))
        self.assertEqual(self.read(real), "quality=high\n")

    def test_file_mode_is_kept_and_no_temp_files_are_left(self):
        self.write("#quality=auto\n")
        os.chmod(self.path, 0o600)
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.save(loaded, quality="low")
        self.assertEqual(os.stat(self.path).st_mode & 0o777, 0o600)
        self.assertEqual(sorted(os.listdir(os.path.dirname(self.path))), ["vsr.conf", "vsr.conf.bak-nvvsr"])

    def test_invalid_file_value_loads_as_default_with_a_problem(self):
        self.write("pool=99\nquality=nonsense\n")
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        self.assertEqual((loaded.values["pool"], loaded.values["quality"]), (1, "auto"))
        self.assertEqual(len(loaded.problems), 2)

    def test_unwritable_directory_reports_an_error_without_a_traceback(self):
        self.path = "/proc/nvc-test/vsr.conf"
        loaded = conf.load_settings(self.path, conf.MPV_OPTIONS)
        r = self.save(loaded, quality="high")
        self.assertFalse(r.ok)
        self.assertIn("could not write", r.error)


class VlcSave(unittest.TestCase):
    def test_vlcrc_edit_uses_documented_default_lines_and_01_booleans(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "vlcrc")
            with open(p, "w") as f:
                f.write(support.VLCRC_SAMPLE)
            loaded = conf.load_settings(p, conf.VLC_OPTIONS)
            edited = {**loaded.values, "nvvsr-quality": "ultra", "nvvsr-ac-only": False, "nvvsr-min-height": 480}
            r = conf.save_changes(p, conf.VLC_OPTIONS, edited, loaded, section="nvvsr", bool_style="01")
            self.assertTrue(r.wrote)
            with open(p) as f:
                out = f.read()
            self.assertIn("\nnvvsr-quality=ultra\n", out)
            self.assertIn("\nnvvsr-ac-only=0\n", out)
            self.assertIn("\n[nvvsr] # NVIDIA VSR\n", out)
            self.assertIn("nvvsr-min-height=480\n", out)
            self.assertEqual(out.count("[nvvsr]"), 1)  # no second section
            self.assertLess(out.index("nvvsr-min-height=480"), out.index("[zzz-other]"))
            self.assertIn("avcodec-hw=any", out)


if __name__ == "__main__":
    unittest.main()
