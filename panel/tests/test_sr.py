import os
import shutil
import tempfile
import unittest
from unittest import mock

import support
from nvc import diag, sr
from nvc.proc import CmdResult


class Scratch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = os.path.join(self.tmp.name, "home")
        self.repo = os.path.join(self.tmp.name, "repo")
        os.makedirs(self.home)
        patch = mock.patch.dict(os.environ, {"HOME": self.home})  # restored (with anything popped below) on cleanup
        patch.start()
        self.addCleanup(patch.stop)
        for k in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"):
            os.environ.pop(k, None)


class InstallJson(Scratch):
    def test_absent(self):
        i = sr.read_install_json()
        self.assertFalse(i.present)
        self.assertEqual((i.paths, i.components, i.version), ({}, {}, None))

    def test_odd_shapes_do_not_raise(self):
        p = os.path.join(self.tmp.name, "install.json")
        for payload in ("not json", "[1,2]", '{"paths": 5, "components": {"a": "x", "b": {"status": 3}}, "version": 7}',
                        '{"paths": {"libnvvsr": 5}}'):
            with open(p, "w") as f:
                f.write(payload)
            i = sr.read_install_json(p)
            self.assertTrue(i.present)
            _ = (i.paths, i.components, i.version)
        with open(p, "w") as f:
            f.write("not json")
        self.assertIsNotNone(sr.read_install_json(p).error)

    def test_schema_one_with_unknown_keys(self):
        support.make_home(self.home, self.repo)
        i = sr.read_install_json()
        self.assertEqual(i.version, "0.1.0")
        self.assertEqual(i.components["chrome_handoff"], ("skipped", "not requested"))
        self.assertIn("libnvvsr", i.paths)


class Status(Scratch):
    def test_installed_everything_ok(self):
        support.make_home(self.home, self.repo)
        st = sr.compute_status(sr.read_install_json(), None, mpv_has_vapoursynth=True)
        self.assertEqual(st.mode, "installed")
        self.assertTrue(st.mpv_ready and st.vlc_ready, [(c.id, c.ok) for c in st.checks])
        self.assertEqual(st.headline, "Ready for mpv and VLC")

    def test_dev_mode_uses_repo_build_dirs_without_install_json(self):
        support.make_home(self.home, self.repo, with_install_json=False)
        st = sr.compute_status(sr.read_install_json(), self.repo, mpv_has_vapoursynth=None)
        self.assertEqual(st.mode, "dev")
        self.assertTrue(st.ok("libnvvsr") and st.ok("vsplugin") and st.ok("vlc"))
        self.assertIn("development build", st.check("libnvvsr").detail)
        self.assertIn("filter list not checked", st.check("mpv").detail)

    def test_nothing_installed(self):
        st = sr.compute_status(sr.read_install_json(), None)
        self.assertEqual((st.mode, st.headline), ("missing", "Not installed"))
        for c in st.checks:
            self.assertFalse(c.ok, c.id)
            self.assertTrue(c.hint, c.id)  # every missing item says how to fix it

    def test_each_failure_is_reported_on_its_own(self):
        h = support.make_home(self.home, self.repo)
        os.remove(h["mpv"])
        with open(os.path.join(h["cfg"], "vapoursynth", "vapoursynth.toml"), "w") as f:
            f.write('"/x/other.so" = ["a"]\n')
        os.remove(os.path.join(h["vfx"], "libnvVFXVideoSuperRes.so"))
        st = sr.compute_status(sr.read_install_json(), None)
        self.assertEqual([c.id for c in st.checks if not c.ok], ["libdir", "mpv", "vsscript"])
        self.assertFalse(st.mpv_ready)
        self.assertTrue(st.check("mpv").hint)
        self.assertEqual(st.headline, "Incomplete installation")

    def test_mpv_without_vapoursynth_filter(self):
        support.make_home(self.home, self.repo)
        st = sr.compute_status(sr.read_install_json(), None, mpv_has_vapoursynth=False)
        self.assertFalse(st.ok("mpv"))

    def test_vsscript_entry_and_libdir(self):
        h = support.make_home(self.home, self.repo)
        self.assertEqual(sr.vsscript_entry()[0], True)
        self.assertEqual(sr.read_libdir(), [h["vfx"], h["wheel"]])
        self.assertEqual(sr.libdir_env(), f"{h['vfx']}:{h['wheel']}")
        with open(sr.paths.nvvsr_libdir_file(), "w") as f:
            f.write("# comment\n~/x:~/y\n")
        self.assertEqual(sr.read_libdir(), [os.path.join(self.home, "x"), os.path.join(self.home, "y")])
        os.remove(sr.paths.nvvsr_libdir_file())
        self.assertIsNone(sr.libdir_env())

    def test_mpv_filter_detection(self):
        run = lambda out, rc=0: (lambda argv, timeout=10, env=None: CmdResult(argv, rc=rc, out=out))
        self.assertTrue(sr.mpv_has_vapoursynth("mpv", run("Available video filters:\n  format  x\n  vapoursynth  VS\n")))
        self.assertFalse(sr.mpv_has_vapoursynth("mpv", run("Available video filters:\n  format  x\n")))
        self.assertIsNone(sr.mpv_has_vapoursynth("mpv", lambda argv, timeout=10, env=None: CmdResult(argv, missing=True)))


@unittest.skipUnless(shutil.which("gcc") or shutil.which("cc"), "needs a C compiler for the fake libnvvsr")
class Probe(Scratch):
    def setUp(self):
        super().setUp()
        self.lib = support.build_fake_lib(os.path.join(self.tmp.name, "lib"))

    def test_probe_ok_passes_the_libdir_through_the_environment(self):
        r = sr.run_probe(self.lib, "/some/libdir")
        self.assertEqual((r.ok, r.summary), (True, "Super resolution works"))

    def test_probe_failure_shows_status_and_last_error(self):
        with mock.patch.dict(os.environ, {"FAKE_PROBE_RC": "-5"}):
            r = sr.run_probe(self.lib, "/some/libdir")
        self.assertFalse(r.ok)
        self.assertEqual(r.summary, "Not usable: no NVIDIA GPU or CUDA device")
        self.assertEqual(r.detail, "fake: no CUDA device")

    def test_probe_without_libdir_reports_the_library_message(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("NVVSR_LIBDIR", None)
            r = sr.run_probe(self.lib, None)
        self.assertFalse(r.ok)
        self.assertIn("NVVSR_LIBDIR", r.detail)

    def test_probe_of_a_broken_or_missing_library(self):
        bad = os.path.join(self.tmp.name, "bad.so")
        with open(bad, "w") as f:
            f.write("not an ELF")
        r = sr.run_probe(bad, "/x")
        self.assertFalse(r.ok)
        self.assertEqual(sr.run_probe(None).summary, "libnvvsr was not found")

    def test_version_without_gpu(self):
        self.assertEqual(sr.libnvvsr_version(self.lib), "9.9.9-fake (API 1)")
        self.assertIsNone(sr.libnvvsr_version("/nonexistent.so"))


class Diag(Scratch):
    def test_wheel_version_from_dist_info(self):
        h = support.make_home(self.home, self.repo)
        self.assertEqual(diag.vfx_wheel_version([h["vfx"], h["wheel"]]), "0.2.0.0")
        self.assertIsNone(diag.vfx_wheel_version(["/nonexistent/x"]))
        self.assertIsNone(diag.vfx_wheel_version(None))

    def test_first_line_trims_noise(self):
        self.assertEqual(diag.first_line("\nmpv 0.41.0 Copyright (c) 2000\nmore"), "mpv 0.41.0")
        self.assertEqual(diag.first_line("VLC version 3.0.24 Vetinari (revision x)"), "VLC version 3.0.24 Vetinari")
        self.assertIsNone(diag.first_line(""))

    def test_report_has_no_secrets_and_no_home_path(self):
        support.make_home(self.home, self.repo)
        from nvc import gpu
        st = sr.compute_status(sr.read_install_json(), None, True)
        text = diag.build_report(versions={"NV Control": "0.1.0", "VLC": None}, status=st, runtime=gpu.RuntimeInfo("active", "auto", "D0"),
                                 sample=gpu.parse_gpu_csv(support.GPU_CSV), hybrid="Hybrid", power_src="ac")
        self.assertNotIn("do-not-leak", text)
        self.assertNotIn("use_chrome_cookies", text)
        self.assertNotIn(self.home, text)
        for needle in ("NV Control: 0.1.0", "VLC: not found", "Ready for", "hybrid mode: Hybrid", "RTX 3070 Ti",
                       "Legacy Chrome handoff configured: yes (contents not included)", "[ok] libnvvsr"):
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()
