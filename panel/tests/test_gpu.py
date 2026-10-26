import os
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from nvc import gpu
from nvc.proc import CmdResult


def fake_run(out="", rc=0, record=None):
    def run(argv, timeout=10, env=None):
        if record is not None:
            record.append(argv)
        return CmdResult(argv, rc=rc, out=out, err="" if rc == 0 else "boom\nGPU is lost")
    return run


class Sysfs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name

    def test_finds_only_the_nvidia_display_controller(self):
        support.make_sysfs(self.root)
        gpus = gpu.find_nvidia_gpus(self.root)
        self.assertEqual([g.addr for g in gpus], ["0000:01:00.0"])  # not the HDMI audio function, not Intel

    def test_no_nvidia_gpu(self):
        support.make_sysfs(self.root, with_nvidia=False)
        self.assertEqual(gpu.find_nvidia_gpus(self.root), [])
        self.assertEqual(gpu.find_nvidia_gpus(os.path.join(self.root, "missing")), [])

    def test_runtime_status_and_state(self):
        for status, state in (("suspended", "asleep"), ("suspending", "asleep"), ("active", "awake"),
                              ("resuming", "awake"), ("unsupported", "awake")):
            with tempfile.TemporaryDirectory() as d:
                support.make_sysfs(d, status)
                rt = gpu.read_runtime(gpu.find_nvidia_gpus(d)[0])
                self.assertEqual((rt.status, gpu.gpu_state(rt.status)), (status, state))
                self.assertEqual(rt.control, "auto")
        self.assertEqual(gpu.gpu_state(None), "unknown")
        self.assertEqual(gpu.read_runtime(None).status, None)

    def test_power_source_and_driver(self):
        support.make_sysfs(self.root, online="1")
        self.assertEqual(gpu.power_source(self.root), "ac")
        self.assertEqual(gpu.driver_version_cheap(self.root), "615.78.08")
        with tempfile.TemporaryDirectory() as d:
            support.make_sysfs(d, online="0")
            self.assertEqual(gpu.power_source(d), "battery")
        self.assertEqual(gpu.power_source(os.path.join(self.root, "nope")), "unknown")


class PollPolicy(unittest.TestCase):
    base = dict(window_visible=True, page_active=True, state="awake", in_flight=False, interval_s=2, since_last_s=None)

    def d(self, **kw):
        return gpu.poll_decision(**{**self.base, **kw})

    def test_queries_only_when_visible_on_overview_and_awake(self):
        self.assertEqual(self.d(), "query")
        self.assertEqual(self.d(window_visible=False), "hidden")
        self.assertEqual(self.d(page_active=False), "hidden")
        self.assertEqual(self.d(state="asleep"), "asleep")  # never query a sleeping GPU
        self.assertEqual(self.d(state="asleep", window_visible=False), "hidden")
        self.assertEqual(self.d(state="unknown"), "query")

    def test_rate_limit_and_manual_mode(self):
        self.assertEqual(self.d(since_last_s=0.5), "wait")
        self.assertEqual(self.d(since_last_s=2.0), "query")
        self.assertEqual(self.d(in_flight=True), "wait")
        self.assertEqual(self.d(interval_s=0), "wait")


class SmiParsers(unittest.TestCase):
    def test_csv_full_row(self):
        s = gpu.parse_gpu_csv(support.GPU_CSV)
        self.assertEqual((s.name, s.driver), ("NVIDIA GeForce RTX 3070 Ti Laptop GPU", "615.78.08"))
        self.assertEqual((s.mem_used_mib, s.mem_total_mib, s.util_pct, s.temp_c), (1312, 8192, 12, 47))
        self.assertEqual((s.power_w, s.power_limit_w, s.clock_gr_mhz, s.clock_mem_mhz), (18.52, 125, 1230, 6000))
        self.assertEqual((s.pcie_gen, s.pcie_width, s.fan_pct, s.pstate, s.persistence), (4, 16, None, "P0", False))

    def test_csv_twelve_documented_columns_and_na(self):
        s = gpu.parse_gpu_csv("NVIDIA X, 615.78.08, 1, 8192, [N/A], [Not Supported], N/A, 125.00, 210, 405, 1, 16\n",
                              fields=gpu.QUERY_FIELDS[:12])
        self.assertIsNone(s.util_pct)
        self.assertIsNone(s.temp_c)
        self.assertIsNone(s.power_w)
        self.assertEqual(s.pcie_gen, 1)
        self.assertIsNone(s.persistence)

    def test_csv_garbage(self):
        self.assertIsNone(gpu.parse_gpu_csv(""))
        self.assertIsNone(gpu.parse_gpu_csv("No devices were found\n"))

    def test_compute_apps(self):
        apps = gpu.parse_compute_apps(support.APPS_CSV)
        self.assertEqual([(a.pid, a.name, a.mem_mib) for a in apps],
                         [(4242, "/usr/bin/python3", 512), (3103, "/usr/lib/chromium/chrome", 220)])
        self.assertEqual(gpu.parse_compute_apps("No running compute processes found\n"), [])
        self.assertEqual(gpu.parse_compute_apps("1, name with, comma, 7\n")[0].name, "name with, comma")

    def test_power_page(self):
        d = gpu.parse_power_page(support.POWER_Q)
        p = d.power
        self.assertEqual((p.draw_w, p.limit_w, p.default_w, p.min_w, p.max_w), (18.52, 125, 125, 1, 150))
        self.assertEqual(d.clocks, {"graphics": 1230, "sm": 1230, "memory": 6000, "video": 1110})
        self.assertEqual(d.max_clocks["graphics"], 2100)
        self.assertEqual(d.limiters, ["SW Power Cap"])
        self.assertTrue(d.has_limiter_info)

    def test_power_page_older_driver_names(self):
        old = "GPU 00000000:01:00.0\n    Power Readings\n        Power Management : Supported\n" \
              "        Power Draw : 10.00 W\n        Power Limit : 80.00 W\n        Default Power Limit : 80.00 W\n"
        p = gpu.parse_power_page(old).power
        self.assertEqual((p.draw_w, p.limit_w, p.management), (10, 80, "Supported"))
        self.assertIsNone(p.max_w)

    def test_query_functions_use_one_call_each_and_report_errors(self):
        calls = []
        s, err = gpu.query_gpu(fake_run(support.GPU_CSV, record=calls))
        self.assertEqual((s.name[:6], err, len(calls)), ("NVIDIA", None, 1))
        self.assertEqual(calls[0][0], "nvidia-smi")
        self.assertIn("--format=csv,noheader,nounits", calls[0])
        s, err = gpu.query_gpu(fake_run("", rc=9))
        self.assertIsNone(s)
        self.assertEqual(err, "GPU is lost")
        s, err = gpu.query_gpu(lambda argv, timeout=10, env=None: CmdResult(argv, missing=True))
        self.assertEqual(err, "nvidia-smi not found")

    def test_supergfxctl(self):
        self.assertEqual(gpu.supergfxctl_mode(fake_run("Hybrid\n")), "Hybrid")
        self.assertIsNone(gpu.supergfxctl_mode(fake_run("", rc=1)))


class Commands(unittest.TestCase):
    def test_power_limit_command_validates(self):
        info = gpu.parse_power_page(support.POWER_Q).power
        self.assertEqual(gpu.power_limit_command(120, info), ["sudo", "nvidia-smi", "-pl", "120"])
        self.assertEqual(gpu.power_limit_command(99.0, None), ["sudo", "nvidia-smi", "-pl", "99"])
        for bad in (0, -5, 151, "abc", "99.7", None):
            with self.assertRaises(ValueError, msg=bad):
                gpu.power_limit_command(bad, info)

    def test_persistence_command(self):
        self.assertEqual(gpu.persistence_command(True), ["sudo", "nvidia-smi", "-pm", "1"])
        self.assertEqual(gpu.persistence_command(False), ["sudo", "nvidia-smi", "-pm", "0"])


if __name__ == "__main__":
    unittest.main()


class RealDriverOutput(unittest.TestCase):
    """`nvidia-smi -q -d POWER,CLOCK,PERFORMANCE` as printed by driver 615.78.08 on the RTX 3070 Ti Laptop."""

    def test_power_limits_nested_under_ceiling_are_found(self):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "smi_q_power_driver615.txt")
        with open(path) as f:
            info = gpu.parse_power_page(f.read())
        p = info.power
        self.assertEqual((p.limit_w, p.default_w, p.min_w, p.max_w), (150.0, 115.0, 1.0, 150.0))
        self.assertIsNone(p.requested_w)
        self.assertEqual(p.draw_w, 25.94)
        self.assertEqual(info.max_clocks["graphics"], 1785)
        self.assertEqual(info.limiters, ["Idle"])
