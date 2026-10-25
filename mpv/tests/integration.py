"""Headless integration tests of mpv/vsr.lua in the private mpv (see run.sh, which sets the environment).

Every scenario starts its own mpv with --vo=null (no window), drives it over the JSON IPC socket and checks the
filter list, the output size and the script's published state (user-data/vsr/*). Prints PASS/FAIL per check;
the exit status is the number of failed checks. Run with python3 -I.
"""
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mpvipc import Mpv  # noqa: E402

E = os.environ
REPO, SCR, MPV, MEDIA = E["T_REPO"], E["T_SCR"], E["T_MPV"], E["T_MEDIA"]
VSR_LUA = os.path.join(REPO, "mpv", "vsr.lua")
HOOK_PLUGIN = E.get("T_HOOK_PLUGIN", "")
ONLY = [a for a in sys.argv[1:] if not a.startswith("--")]
BENCH = "--bench" in sys.argv
SOCK = os.path.join(SCR, "m.sock")
LOG = os.path.join(SCR, "mpv.log")

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print("PASS " + name, flush=True)
    else:
        failed += 1
        print("FAIL %s: %s" % (name, detail), flush=True)
    return bool(cond)


def section(title):
    print("== " + title, flush=True)


def wanted(tag):
    return not ONLY or any(o in tag for o in ONLY)


class Run:
    """One mpv instance. files are names inside MEDIA (or absolute paths)."""

    def __init__(self, files, fs=True, audio=False, opts=None, extra=(), env=None, script=VSR_LUA,
                 config_dir=None, display="2560x1440"):
        self.files = [files] if isinstance(files, str) else list(files)
        sopts = {"vsr-confirm_timeout": "3"}
        sopts.update(opts or {})
        args = ["--vo=null", "--ao=null", "--idle=yes", "--keep-open=yes", "--msg-level=all=info,vsr=v",
                "--osd-level=1"]
        args += ["--no-config"] if config_dir is None else ["--config-dir=" + config_dir]
        if not audio:
            args.append("--no-audio")
        if fs:
            args.append("--fs")
        if script:
            args.append("--script=" + script)
        args.append("--script-opts=" + ",".join("%s=%s" % kv for kv in sopts.items()))
        args += list(extra)
        args += [f if os.path.isabs(f) else os.path.join(MEDIA, f) for f in self.files]
        e = dict(os.environ)
        e.update({"NVVSR_LIBDIR": E["NVVSR_LIBDIR"], "NVVSR_PLUGIN": E["NVVSR_PLUGIN"]})
        if display:
            e["NVVSR_DISPLAY"] = display
        else:
            e.pop("NVVSR_DISPLAY", None)
        e.update(env or {})
        self.m = Mpv(MPV, args, SOCK, env=e, log=LOG)
        self.t0 = time.time()

    # -- state helpers
    def state(self): return self.m.get("user-data/vsr/state")
    def reason(self): return self.m.get("user-data/vsr/reason") or ""
    def labels(self): return self.m.vf_labels()
    def out(self): return "%sx%s" % (self.m.get("video-out-params/w"), self.m.get("video-out-params/h"))
    def get(self, p, d=None): return self.m.get(p, d)
    def cmd(self, *a): return self.m.command(*a)
    def key(self, k): return self.m.command("keypress", k).get("error")
    def hwdec(self): return self.m.get("hwdec")
    def alive(self): return self.m.alive()

    def wait_state(self, want, timeout=15):
        return self.m.wait_for(lambda: self.state() == want, timeout)

    def wait_decided(self, timeout=10):
        """Waits until the script has evaluated the file once; returns the state."""
        self.m.wait_for(lambda: self.reason() not in ("", "file loaded", "not started"), timeout)
        return self.state()

    def quiet(self, secs=1.0):
        time.sleep(secs)

    def log(self):
        try:
            with open(LOG, "rb") as f:
                return f.read().decode(errors="replace")
        except OSError:
            return ""

    def close(self):
        self.m.close()

    def __enter__(self): return self
    def __exit__(self, *a): self.close()


def no_lua_errors(name, r):
    log = r.log()
    bad = [l for l in log.splitlines() if "Lua error" in l or "stack traceback" in l]
    check(name + ": no Lua errors in the mpv log", not bad, bad[:2])
    check(name + ": mpv still running", r.m.alive())


# ------------------------------------------------------------------------------------------------
def classification():
    section("1. which video gets SR (display 2560x1440, fullscreen)")
    # name, file, expected state, reason fragment, labels, output size
    cases = [
        ("720p30 H.264", "v720p30.mp4", "on", "", ["nvvsr"], "2560x1440"),
        ("1080p60 H.264", "v1080p60.mp4", "on", "", ["nvvsr"], "2560x1440"),
        ("854x480 (aspect fit, even size)", "v480p.mp4", "on", "", ["nvvsr"], "2560x1438"),
        ("10-bit HEVC SDR (mpv converts to 8-bit)", "v720p10b_hevc.mp4", "on", "", ["nvvsr-fmt", "nvvsr"], "2560x1440"),
        ("10-bit H.264 SDR (mpv converts to 8-bit)", "v720p10b_h264.mp4", "on", "", ["nvvsr-fmt", "nvvsr"], "2560x1440"),
        ("4:4:4 H.264 (mpv converts to 4:2:0)", "v720p444.mp4", "on", "", ["nvvsr-fmt", "nvvsr"], "2560x1440"),
        ("240p (below min_height)", "v240p.mp4", "off", "too small", [], "320x240"),
        ("1440p (needs no SR)", "v1440p.mp4", "off", "not smaller", [], "2560x1440"),
        ("HDR10 (PQ, BT.2020)", "hdr_pq.mp4", "off", "HDR", [], "1280x720"),
        ("HLG", "hdr_hlg.mp4", "off", "HDR", [], "1280x720"),
        ("SDR tagged BT.2020", "sdr_bt2020.mp4", "off", "BT.2020", [], "1280x720"),
        ("interlaced 1080i", "v1080i.mp4", "off", "interlaced", [], "1920x1080"),
        ("interlaced 576i", "v576i.mp4", "off", "interlaced", [], "720x576"),
    ]
    for name, f, state, frag, labels, size in cases:
        if not wanted("class") and not wanted(f):
            continue
        with Run(f) as r:
            if state == "on":
                got = r.wait_state("on", 20)
                check("%s: SR on" % name, got, "state=%s reason=%s" % (r.state(), r.reason()))
                check("%s: filters %s" % (name, labels), r.labels() == labels, r.labels())
                check("%s: output %s" % (name, size), r.out() == size, r.out())
                if "nvvsr-fmt" in labels:
                    check("%s: filter input is 8-bit 4:2:0" % name, r.get("video-out-params/pixelformat") == "yuv420p",
                          r.get("video-out-params/pixelformat"))
            else:
                r.wait_decided()
                r.quiet(1.0)
                check("%s: SR stays off (%s)" % (name, frag), r.state() == "off" and frag in r.reason(),
                      "state=%s reason=%s" % (r.state(), r.reason()))
                check("%s: no filter, output %s" % (name, size), r.labels() == [] and r.out() == size, "%s %s" % (r.labels(), r.out()))
            no_lua_errors(name, r)

    for name, f, extra, frag in [
        ("audio-only file", "audio.m4a", {}, "no video"),
        ("audio file with cover art", "art.m4a", {}, "image or cover art"),
        ("still image", "still.png", {"extra": ["--image-display-duration=inf"]}, "image or cover art"),
    ]:
        if not wanted("class") and not wanted(f):
            continue
        with Run(f, audio=True, **extra) as r:
            r.wait_decided()
            r.quiet(0.8)
            check("%s: SR stays off (%s)" % (name, frag), r.state() == "off" and frag in r.reason() and r.labels() == [],
                  "state=%s reason=%s vf=%s" % (r.state(), r.reason(), r.labels()))
            no_lua_errors(name, r)

    if wanted("class") or wanted("window"):
        with Run("v720p30.mp4", fs=False) as r:
            r.wait_decided()
            r.quiet(1.0)
            check("windowed playback: SR off (not fullscreen), no filter", r.state() == "off" and "not fullscreen" in r.reason()
                  and r.labels() == [], "%s %s %s" % (r.state(), r.reason(), r.labels()))
        with Run("v720p30.mp4", display=None) as r:
            r.wait_decided()
            r.quiet(1.0)
            check("display size unknown (no NVVSR_DISPLAY, vo=null): SR off with that reason",
                  r.state() == "off" and "display size unknown" in r.reason(), "%s %s" % (r.state(), r.reason()))
        with Run("v720p30.mp4", opts={"vsr-min_height": "1000"}) as r:
            r.wait_decided()
            check("--script-opts vsr-min_height=1000 excludes 720p", r.state() == "off" and "too small" in r.reason(), r.reason())
        with Run("v720p30.mp4", opts={"vsr-enabled": "no"}) as r:
            r.wait_decided()
            r.quiet(0.8)
            check("enabled=no: nothing happens by itself", r.state() == "off" and r.labels() == [] and "vsr.conf" in r.reason(),
                  "%s %s" % (r.state(), r.reason()))
            r.key("ctrl+s")
            check("enabled=no: the toggle key still turns SR on for the file", r.wait_state("on", 15), "%s %s" % (r.state(), r.reason()))


def fullscreen_cycle():
    if not (wanted("fullscreen") or wanted("hwdec")):
        return
    section("2. fullscreen on/off, user filters, hwdec")
    with Run("v720p30.mp4", extra=["--vf=@user:lavfi=[eq=brightness=0.02]"]) as r:
        r.wait_state("on", 20)
        check("user's own vf entry kept, @nvvsr appended after it", r.labels() == ["user", "nvvsr"], r.labels())
        user_before = [f for f in r.get("vf") if f["label"] == "user"]
        r.m.set("fullscreen", False)
        check("leaving fullscreen: state off", r.m.wait_for(lambda: r.state() == "off", 8), r.state())
        check("leaving fullscreen: @nvvsr removed, user's entry untouched",
              r.labels() == ["user"] and [f for f in r.get("vf") if f["label"] == "user"] == user_before, r.get("vf"))
        check("leaving fullscreen: output is the source size again", r.m.wait_for(lambda: r.out() == "1280x720", 4), r.out())
        check("reason says why", "not fullscreen" in r.reason(), r.reason())
        r.m.set("fullscreen", True)
        check("entering fullscreen again: SR back on", r.wait_state("on", 15), "%s %s" % (r.state(), r.reason()))
        check("... with the same filter list and size", r.labels() == ["user", "nvvsr"] and r.out() == "2560x1440", "%s %s" % (r.labels(), r.out()))
        # user's `vf clr`: respected until the next file
        r.cmd("vf", "clr", "")
        check("vf clr by the user: SR state off and it stays off", r.m.wait_for(lambda: r.state() == "off", 6) and
              "removed by user" in r.reason(), "%s %s" % (r.state(), r.reason()))
        r.m.set("fullscreen", False); time.sleep(0.6); r.m.set("fullscreen", True); time.sleep(1.5)
        check("... even after a fullscreen toggle", r.state() == "off" and r.labels() == [], "%s %s" % (r.state(), r.labels()))
        no_lua_errors("fullscreen cycle", r)

    def run_hwdec(title, hw, expect_target, expect_current, opts=None):
        with Run("v720p30.mp4", extra=["--hwdec=" + hw], opts=opts) as r:
            samples = []
            deadline = time.time() + 20
            while time.time() < deadline and r.state() != "on":
                samples.append(r.get("time-pos") or 0); time.sleep(0.1)
            check("%s: SR on" % title, r.state() == "on", "%s %s" % (r.state(), r.reason()))
            time.sleep(1.0)
            check("%s: hwdec while SR runs = %s" % (title, expect_target), r.hwdec() == expect_target, r.hwdec())
            if expect_current:
                check("%s: decoder in use = %s" % (title, expect_current), r.get("hwdec-current") == expect_current, r.get("hwdec-current"))
            was = r.get("user-data/vsr/hwdec")
            check("%s: original value remembered (%s)" % (title, hw), was == (hw if expect_target != [hw] else ""), was)
            for _ in range(10):
                samples.append(r.get("time-pos") or 0); time.sleep(0.1)
            r.m.set("fullscreen", False)
            r.m.wait_for(lambda: r.state() == "off", 8)
            for _ in range(15):
                samples.append(r.get("time-pos") or 0); time.sleep(0.1)
            check("%s: hwdec restored to [%s] when SR stops" % (title, hw), r.hwdec() == [hw], r.hwdec())
            back = max([a - b for a, b in zip(samples, samples[1:])] + [0])
            check("%s: playback position kept (largest step back %.2f s, advanced %.1f s)" % (title, back, samples[-1] - samples[0]),
                  back < 0.5 and samples[-1] - samples[0] > 1.5, samples[:3] + samples[-3:])
            no_lua_errors(title, r)

    # --vo=null has no hardware interop, so a non-copy hwdec decodes in software here ("hwdec-current" is "no"):
    # the option handling is what is tested, not the decoder.
    run_hwdec("hwdec=nvdec", "nvdec", ["nvdec-copy"], "nvdec-copy")
    run_hwdec("hwdec=vaapi", "vaapi", ["vaapi-copy"], None)
    with Run("v720p30.mp4", extra=["--hwdec=auto"]) as r:
        r.wait_state("on", 20); time.sleep(0.8)
        check("hwdec=auto -> auto-copy while SR runs (decoding on nvdec-copy)", r.hwdec() == ["auto-copy"] and r.get("hwdec-current") == "nvdec-copy",
              "%s %s" % (r.hwdec(), r.get("hwdec-current")))
        r.m.set("fullscreen", False); r.m.wait_for(lambda: r.state() == "off", 8); time.sleep(0.5)
        check("hwdec=auto restored", r.hwdec() == ["auto"], r.hwdec())
    for hw in ("nvdec-copy", "no"):
        with Run("v720p30.mp4", extra=["--hwdec=" + hw]) as r:
            r.wait_state("on", 20); time.sleep(0.5)
            check("hwdec=%s: left alone (already sw-memory)" % hw, r.hwdec() == [hw] and r.get("user-data/vsr/hwdec") == "", "%s %s" % (r.hwdec(), r.get("user-data/vsr/hwdec")))
    with Run("v720p30.mp4", extra=["--hwdec=nvdec"], opts={"vsr-hwdec": "keep"}) as r:
        r.wait_state("on", 20); time.sleep(0.5)
        check("hwdec option 'keep': mpv's setting untouched, SR still on", r.hwdec() == ["nvdec"] and r.state() == "on", "%s %s" % (r.hwdec(), r.state()))
    with Run("v720p30.mp4", extra=["--hwdec=nvdec"], opts={"vsr-hwdec": "vaapi-copy"}) as r:
        r.wait_state("on", 20); time.sleep(0.5)
        check("hwdec option with an explicit value is used while SR runs", r.hwdec() == ["vaapi-copy"] and r.get("hwdec-current") == "vaapi-copy",
              "%s %s" % (r.hwdec(), r.get("hwdec-current")))
    with Run("v720p30.mp4", extra=["--hwdec=nvdec"]) as r:
        r.wait_state("on", 20); time.sleep(0.5)
        r.m.set("hwdec", "no")  # the user changes hwdec (Ctrl+h) while SR runs
        time.sleep(0.5)
        r.m.set("fullscreen", False); r.m.wait_for(lambda: r.state() == "off", 8); time.sleep(0.5)
        check("a hwdec change made by the user meanwhile is not overwritten on restore", r.hwdec() == ["no"], r.hwdec())


def keys():
    if not wanted("keys"):
        return
    section("3. key bindings: toggle, quality, stats")
    with Run("v720p30.mp4") as r:
        r.wait_state("on", 20)
        check("Ctrl+s accepted", r.key("ctrl+s") == "success")
        check("toggle off: filter removed, state off, 'turned off by user'",
              r.m.wait_for(lambda: r.state() == "off", 6) and r.labels() == [] and "user" in r.reason(), "%s %s %s" % (r.state(), r.labels(), r.reason()))
        check("toggle off: original size", r.m.wait_for(lambda: r.out() == "1280x720", 4), r.out())
        r.m.set("fullscreen", False); time.sleep(0.6); r.m.set("fullscreen", True); time.sleep(2.0)
        check("manual off sticks across a fullscreen toggle", r.state() == "off" and r.labels() == [], "%s %s" % (r.state(), r.labels()))
        r.cmd("script-binding", "vsr/toggle")
        check("script-binding vsr/toggle turns it on again", r.wait_state("on", 15) and r.out() == "2560x1440", "%s %s" % (r.state(), r.out()))
        r.key("ctrl+s")
        r.m.wait_for(lambda: r.state() == "off", 6)
        r.cmd("loadfile", os.path.join(MEDIA, "v480p.mp4"), "replace")
        check("next file: manual off no longer applies, SR on again", r.wait_state("on", 20) and r.out() == "2560x1438", "%s %s" % (r.state(), r.out()))

        ud = lambda: next((f["params"].get("user-data") for f in r.get("vf") if f["label"] == "nvvsr"), None)
        check("auto quality for 480p is ultra", ud() == "quality=auto;pool=1" and r.get("user-data/vsr/quality") == "auto/ultra",
              "%s %s" % (ud(), r.get("user-data/vsr/quality")))
        r.key("ctrl+shift+s")
        check("Ctrl+Shift+s: quality ultra, filter re-added", r.m.wait_for(lambda: ud() == "quality=ultra;pool=1" and r.state() == "on", 15),
              "%s %s" % (ud(), r.state()))
        r.key("ctrl+shift+s")
        check("Ctrl+Shift+s again: high", r.m.wait_for(lambda: ud() == "quality=high;pool=1" and r.state() == "on", 15), "%s %s" % (ud(), r.state()))
        check("... still one @nvvsr filter and the right size", r.labels() == ["nvvsr"] and r.out() == "2560x1438", "%s %s" % (r.labels(), r.out()))
        r.key("alt+s")
        stats = r.m.wait_for(lambda: r.get("user-data/vsr/stats"), 4)
        check("Alt+s: stats overlay renders (state, sizes, quality, fps, drops, hwdec)",
              stats and all(k in stats for k in ("state:   on", "854x480 -> 2560x1438", "quality: high", "fps:", "drops:", "hwdec:", "power:")), stats)
        r.key("alt+s")
        no_lua_errors("keys", r)


def failures():
    if not wanted("fail"):
        return
    section("4. failure paths fall back to the original video, no retry loop")
    def failed_once(title, r, wait=14):
        ok_ = r.wait_state("failed", wait)
        check("%s: state failed" % title, ok_, "%s %s" % (r.state(), r.reason()))
        r.quiet(3.0)
        check("%s: filter removed, original size, still playing" % title,
              r.labels() == [] and r.out() == "1280x720" and r.state() == "failed", "%s %s %s" % (r.labels(), r.out(), r.state()))
        t1 = r.get("time-pos"); time.sleep(1.0); t2 = r.get("time-pos")
        check("%s: playback keeps advancing" % title, t1 is not None and t2 is not None and t2 > t1, "%s %s" % (t1, t2))
        log = r.log()
        check("%s: logged once, no retry loop (1x 'SR failed', 1x 'SR on')" % title,
              log.count("SR failed:") == 1 and log.count("[vsr] SR on:") <= 1, "failed=%d on=%d" % (log.count("SR failed:"), log.count("[vsr] SR on:")))
        no_lua_errors(title, r)
        return log

    with Run("v720p30.mp4", env={"NVVSR_LIBDIR": "/nonexistent/dir"}) as r:
        log = failed_once("bogus NVVSR_LIBDIR (script passes the video through)", r)
        check("bogus NVVSR_LIBDIR: reason mentions the missing output scaling", "no scaled frames" in r.reason(), r.reason())
        # retry on the next file (default), same failure again
        r.cmd("loadfile", os.path.join(MEDIA, "v480p.mp4"), "replace")
        r.m.wait_for(lambda: r.log().count("SR failed:") >= 2, 14)
        check("retry_on_next_file=yes: the next file tries again (and fails again)", r.log().count("SR failed:") == 2, r.log().count("SR failed:"))
    with Run("v720p30.mp4", env={"NVVSR_LIBDIR": "/nonexistent/dir"}) as r:
        r.wait_state("failed", 14)
        r.key("ctrl+s")
        check("after a failure the toggle key retries once (and fails again, no loop)",
              r.m.wait_for(lambda: r.log().count("SR failed:") == 2, 14) and r.wait_state("failed", 14) and (r.quiet(3.0) or True) and r.log().count("SR failed:") == 2,
              r.log().count("SR failed:"))
    with Run("v720p30.mp4", env={"NVVSR_LIBDIR": "/nonexistent/dir"}, opts={"vsr-retry_on_next_file": "no"}) as r:
        r.wait_state("failed", 14)
        r.cmd("loadfile", os.path.join(MEDIA, "v480p.mp4"), "replace")
        r.m.wait_for(lambda: r.get("filename") == "v480p.mp4", 5); r.quiet(2.5)
        check("retry_on_next_file=no: next file does not try SR at all",
              r.log().count("[vsr] SR on:") == 1 and r.labels() == [] and r.state() in ("off", "failed"),
              "%d %s %s" % (r.log().count("[vsr] SR on:"), r.labels(), r.state()))
    with Run("v720p30.mp4", env={"NVVSR_PLUGIN": "/nonexistent/libvsnvvsr.so"}) as r:
        failed_once("missing plugin file", r)
    if HOOK_PLUGIN:
        with Run("v720p30.mp4", env={"NVVSR_PLUGIN": HOOK_PLUGIN, "NVVSR_VS_FAIL_AFTER": "60"}) as r:
            check("test-hook plugin: SR on at first", r.wait_state("on", 20), "%s %s" % (r.state(), r.reason()))
            check("test-hook plugin: frame errors after frame 60 -> state failed within 6 s", r.wait_state("failed", 10), "%s %s" % (r.state(), r.reason()))
            r.quiet(2.0)
            log = r.log()
            check("test-hook plugin: mpv disabled the filter, we removed it, original size, one 'SR failed'",
                  "Disabling filter nvvsr because it has failed" in log and r.labels() == [] and r.out() == "1280x720" and log.count("SR failed:") == 1,
                  "%s %s %d" % (r.labels(), r.out(), log.count("SR failed:")))
            t1 = r.get("time-pos"); time.sleep(1.0)
            check("test-hook plugin: playback goes on", (r.get("time-pos") or 0) > (t1 or 0), "%s -> %s" % (t1, r.get("time-pos")))
            no_lua_errors("test-hook plugin", r)
    else:
        print("SKIP test-hook plugin cases (no hook build)")

    # vsr.vpy missing: copy of vsr.lua alone in a directory
    lone = os.path.join(SCR, "lone")
    shutil.rmtree(lone, ignore_errors=True); os.makedirs(lone)
    shutil.copy(VSR_LUA, lone)
    with Run("v720p30.mp4", script=os.path.join(lone, "vsr.lua")) as r:
        r.wait_state("failed", 10)
        check("vsr.vpy not found: failed with that reason, video unchanged", r.state() == "failed" and "vsr.vpy not found" in r.reason() and r.labels() == [] and r.out() == "1280x720",
              "%s %s %s" % (r.state(), r.reason(), r.labels()))


def odd_paths():
    if not wanted("path"):
        return
    section("5. installation layouts")
    weird = os.path.join(SCR, "dir with space:colon")
    shutil.rmtree(weird, ignore_errors=True); os.makedirs(weird)
    shutil.copy(VSR_LUA, weird); shutil.copy(os.path.join(REPO, "mpv", "vsr.vpy"), weird)
    with Run("v720p30.mp4", script=os.path.join(weird, "vsr.lua")) as r:
        check("script directory with a space and a colon: SR on (file= is length-prefixed)", r.wait_state("on", 20) and r.out() == "2560x1440",
              "%s %s" % (r.state(), r.reason()))
        f = next((x["params"].get("file") for x in r.get("vf") if x["label"] == "nvvsr"), None)
        check("... vsr.vpy found next to vsr.lua", f == os.path.join(weird, "vsr.vpy"), f)

    cfg = os.path.join(SCR, "cfg")
    shutil.rmtree(cfg, ignore_errors=True)
    os.makedirs(os.path.join(cfg, "scripts")); os.makedirs(os.path.join(cfg, "script-opts"))
    shutil.copy(VSR_LUA, os.path.join(cfg, "scripts")); shutil.copy(os.path.join(REPO, "mpv", "vsr.vpy"), os.path.join(cfg, "scripts"))
    shutil.copy(os.path.join(REPO, "mpv", "script-opts", "vsr.conf"), os.path.join(cfg, "script-opts"))
    open(os.path.join(cfg, "mpv.conf"), "w").write("")
    with Run("v720p30.mp4", script=None, config_dir=cfg, opts={}) as r:
        check("auto-loaded from <config>/scripts with the shipped vsr.conf (all defaults commented): SR on",
              r.wait_state("on", 20) and r.out() == "2560x1440", "%s %s" % (r.state(), r.reason()))
    with open(os.path.join(cfg, "script-opts", "vsr.conf"), "w") as f:
        f.write("# test\nmin_height=1000\nac_only=no\nquality=high\n")
    with Run("v720p30.mp4", script=None, config_dir=cfg, opts={}) as r:
        r.wait_decided()
        check("script-opts/vsr.conf is read (min_height=1000 excludes 720p)", r.state() == "off" and "too small (720p < 1000p)" in r.reason(), r.reason())
    with open(os.path.join(cfg, "script-opts", "vsr.conf"), "w") as f:
        f.write("quality=high\n")
    with Run("v480p.mp4", script=None, config_dir=cfg, opts={}) as r:
        r.wait_state("on", 20)
        ud = next((x["params"].get("user-data") for x in r.get("vf") if x["label"] == "nvvsr"), None)
        check("vsr.conf quality=high reaches the filter's user-data", ud == "quality=high;pool=1", ud)
    with Run("v480p.mp4", script=None, config_dir=cfg, opts={"vsr-quality": "low", "vsr-pool": "2", "vsr-strength": "0.5"}) as r:
        on = r.wait_state("on", 20)
        ud = next((x["params"].get("user-data") for x in r.get("vf") if x["label"] == "nvvsr"), None)
        check("--script-opts overrides vsr.conf (quality=low, pool=2, strength=0.5) and the plugin accepts it",
              on and ud == "quality=low;pool=2;strength=0.5" and r.out() == "2560x1438", "%s %s %s" % (on, ud, r.out()))
    with Run("v480p.mp4", opts={"vsr-quality": "streaming_ultra"}) as r:
        on = r.wait_state("on", 20)
        ud = next((x["params"].get("user-data") for x in r.get("vf") if x["label"] == "nvvsr"), None)
        check("quality=streaming_ultra is passed through and accepted", on and ud == "quality=streaming_ultra;pool=1", "%s %s" % (on, ud))


def power():
    if not wanted("power"):
        return
    section("6. AC / battery")
    pdir = os.path.join(SCR, "power")
    shutil.rmtree(pdir, ignore_errors=True)
    for name, typ, online in (("ADP0", "Mains", "1"), ("BAT0", "Battery", None), ("ucsi", "USB", "0")):
        os.makedirs(os.path.join(pdir, name))
        open(os.path.join(pdir, name, "type"), "w").write(typ + "\n")
        if online is not None:
            open(os.path.join(pdir, name, "online"), "w").write(online + "\n")

    def setac(v, name="ADP0"):
        open(os.path.join(pdir, name, "online"), "w").write("%d\n" % v)

    opts = {"vsr-power_dir": pdir, "vsr-check_interval": "1"}
    setac(0)
    with Run("v720p30.mp4", opts=opts) as r:
        r.wait_decided()
        r.quiet(1.0)
        check("on battery at start: SR off with reason 'on battery', no filter", r.state() == "off" and r.reason() == "on battery" and r.labels() == [],
              "%s %s %s" % (r.state(), r.reason(), r.labels()))
        r.quiet(2.5)
        check("'SR off: on battery' is logged exactly once", r.log().count("SR off: on battery (ac_only=yes)") == 1, r.log().count("SR off: on battery"))
        setac(1)
        check("AC plugged in: SR turns on within the poll interval", r.wait_state("on", 15), "%s %s" % (r.state(), r.reason()))
        setac(0)
        check("AC unplugged: SR turns off, filter removed", r.m.wait_for(lambda: r.state() == "off" and r.labels() == [], 8), "%s %s" % (r.state(), r.labels()))
        check("... reason 'on battery', output back to the source size", r.reason() == "on battery" and r.m.wait_for(lambda: r.out() == "1280x720", 4), "%s %s" % (r.reason(), r.out()))
        setac(0); setac(1, "ucsi")
        check("a USB-C PD charger counts as AC", r.wait_state("on", 15), "%s %s" % (r.state(), r.reason()))
        no_lua_errors("power", r)
    setac(0)
    with Run("v720p30.mp4", opts=dict(opts, **{"vsr-ac_only": "no"})) as r:
        check("ac_only=no: SR runs on battery", r.wait_state("on", 20), "%s %s" % (r.state(), r.reason()))


def adaptive():
    if not wanted("adaptive"):
        return
    section("7. adaptive quality step-down and pause handling")
    # A deliberately heavy CPU filter (bm3d, ~60 ms per 720p frame) in front of SR makes the chain miss its 33 ms
    # budget on any machine, so frames are dropped at the VO; stepping SR down cannot help, which exercises the
    # complete ladder down to the floor.
    heavy = ["--vf=@heavy:lavfi=[bm3d=sigma=10]"]
    quick = {"vsr-adaptive_window": "3", "vsr-adaptive_drops": "5", "vsr-settle": "1"}
    with Run("v720p30.mp4", extra=heavy, opts=quick) as r:
        seen, uds = [], []
        deadline = time.time() + 45
        drops_seen = 0
        while time.time() < deadline:
            q = r.get("user-data/vsr/quality")
            if q and (not seen or seen[-1] != q):
                seen.append(q)
            drops_seen = max(drops_seen, r.get("frame-drop-count") or 0)
            ud = next((f["params"].get("user-data") for f in (r.get("vf") or []) if f["label"] == "nvvsr"), None)
            if ud and (not uds or uds[-1] != ud):
                uds.append(ud)
            if r.state() == "failed":
                break
            time.sleep(0.25)
        log = r.log()
        order = ["auto/high", "medium", "low", "bicubic"]
        check("step-down ladder auto/high > medium > low > bicubic, never up (seen %s)" % seen, [s for s in seen if s in order] == order, seen)
        check("steps counted (3 step-downs)", r.get("user-data/vsr/steps") == 3, r.get("user-data/vsr/steps"))
        check("frames really were dropped (frame-drop-count max %s)" % drops_seen, drops_seen > 5)
        check("at the floor SR is switched off for the file; the heavy user filter stays; video keeps playing",
              r.state() == "failed" and "cannot keep up" in r.reason() and r.labels() == ["heavy"], "%s %s %s" % (r.state(), r.reason(), r.labels()))
        check("each step is logged", log.count("SR restarted, quality ") == 3 and "cannot keep up" in log, log.count("SR restarted, quality "))
        check("... each as a remove+re-add of the filter with new user-data", uds == ["quality=auto;pool=1", "quality=medium;pool=1", "quality=low;pool=1", "quality=bicubic;pool=1"], uds)
        t1 = r.get("time-pos"); time.sleep(1.0)
        check("playback continues after giving up", (r.get("time-pos") or 0) > (t1 or 0))
        no_lua_errors("adaptive", r)
    with Run("v720p30.mp4", extra=heavy, opts=dict(quick, **{"vsr-adaptive": "no"})) as r:
        r.wait_state("on", 20); time.sleep(8)
        check("adaptive=no: no step-down despite %s dropped frames" % r.get("frame-drop-count"),
              r.state() == "on" and r.get("user-data/vsr/steps") == 0 and (r.get("frame-drop-count") or 0) > 5,
              "%s %s %s" % (r.state(), r.get("user-data/vsr/steps"), r.get("frame-drop-count")))
    with Run("v720p30.mp4", extra=["--pause"], opts=quick) as r:
        r.wait_state("starting", 20)
        time.sleep(5)
        check("paused while starting: not declared failed after 5 s (confirm_timeout 3 s)", r.state() in ("starting", "on"), "%s %s" % (r.state(), r.reason()))
        r.m.set("pause", False)
        check("... resumes and reaches 'on'", r.wait_state("on", 15), "%s %s" % (r.state(), r.reason()))
        r.m.set("pause", True); time.sleep(4); r.m.set("pause", False); time.sleep(2)
        check("pausing while on does not step down or fail", r.state() == "on" and r.get("user-data/vsr/steps") == 0, "%s %s" % (r.state(), r.get("user-data/vsr/steps")))


def lifecycle():
    if not wanted("lifecycle"):
        return
    section("8. no stale filter or hwdec change between files")
    with Run(["v720p30.mp4", "hdr_pq.mp4", "v1440p.mp4"], extra=["--hwdec=nvdec", "--keep-open=no", "--end=3"]) as r:
        r.wait_state("on", 20)
        check("file 1: SR on, hwdec switched", r.hwdec() == ["nvdec-copy"], r.hwdec())
        r.m.wait_for(lambda: r.get("playlist-pos") == 1, 12)
        r.quiet(0.2)
        check("file 2 (HDR) starts with no @nvvsr filter and the user's hwdec back", r.labels() == [] and r.hwdec() == ["nvdec"], "%s %s" % (r.labels(), r.hwdec()))
        r.wait_decided(); r.quiet(1.0)
        check("file 2 (HDR): SR off, reason HDR", r.state() == "off" and "HDR" in r.reason(), "%s %s" % (r.state(), r.reason()))
        r.m.wait_for(lambda: r.get("playlist-pos") == 2, 12)
        r.quiet(1.0)
        check("file 3 (1440p): still nothing left behind", r.labels() == [] and r.hwdec() == ["nvdec"] and r.state() == "off", "%s %s %s" % (r.labels(), r.hwdec(), r.state()))
        no_lua_errors("lifecycle", r)
    with Run("v720p30.mp4") as r:
        r.wait_state("on", 20)
        r.m.set("user-data/vsr/display", "1920x1080")  # stands in for a move to a 1080p display (vo=null has none)
        check("display change while on: the filter is restarted for the new size (state back to on)",
              r.m.wait_for(lambda: "display changed to 1920x1080" in r.log(), 6) and r.wait_state("on", 10) and r.labels() == ["nvvsr"], "%s %s" % (r.state(), r.labels()))
        r.m.set("user-data/vsr/display", "1280x720")
        check("display change to 720p (nothing to gain): SR off", r.m.wait_for(lambda: r.state() == "off" and "not smaller" in r.reason(), 6) and r.labels() == [], "%s %s" % (r.state(), r.reason()))
        r.m.set("user-data/vsr/display", "2560x1440")
        check("display back to 1440p: SR on again", r.wait_state("on", 10), r.state())
        for i in range(3):
            r.m.set("fullscreen", False); time.sleep(1.0); r.m.set("fullscreen", True); time.sleep(1.2)
        check("3 quick fullscreen toggles: ends consistently on", r.wait_state("on", 15) and r.labels() == ["nvvsr"] and r.out() == "2560x1440", "%s %s %s" % (r.state(), r.labels(), r.out()))
    with Run("v720p30.mp4") as r:
        r.wait_state("on", 20)
        for i in range(8):
            r.m.set("fullscreen", False); time.sleep(0.9); r.m.set("fullscreen", True); time.sleep(1.1)
            if r.state() == "failed":
                break
        r.quiet(1.0)
        check("flapping guard: 7th enable within 30 s is refused (state failed, filter gone, video plays)",
              r.state() == "failed" and "too many restarts" in r.reason() and r.labels() == [] and r.alive(), "%s %s %s" % (r.state(), r.reason(), r.labels()))


def bench():
    section("9. real-time playback (--vo=null, 15 s each, default framedrop)")
    for name, f, adaptive_ in (("720p30", "v720p30.mp4", "no"), ("1080p60", "v1080p60.mp4", "no"), ("1080p60 adaptive=yes", "v1080p60.mp4", "yes")):
        with Run(f, extra=["--end=15"], opts={"vsr-adaptive": adaptive_}) as r:
            if not r.wait_state("on", 20):
                print("BENCH %s: SR did not start (%s %s)" % (name, r.state(), r.reason()))
                continue
            t_on = time.time()
            t_start = r.get("time-pos")
            fps = []
            while r.alive() and (r.get("time-pos") or 0) < 14.5 and time.time() - t_on < 40:
                v = r.get("estimated-vf-fps")
                if v and (r.get("time-pos") or 0) - (t_start or 0) > 2:
                    fps.append(v)
                time.sleep(0.5)
            wall = time.time() - r.t0
            print("BENCH %s: quality %s, container %.2f fps, estimated-vf-fps mean %.2f (min %.2f), dropped: vo=%s decoder=%s delayed=%s, steps=%s, wall %.1f s for %.1f s of video"
                  % (name, r.get("user-data/vsr/quality"), r.get("container-fps") or 0, sum(fps) / max(len(fps), 1), min(fps or [0]),
                     r.get("frame-drop-count"), r.get("decoder-frame-drop-count"), r.get("vo-delayed-frame-count"),
                     r.get("user-data/vsr/steps"), wall, r.get("time-pos") or 0), flush=True)


def bench_untimed():
    section("9b. throughput headroom (--untimed: as fast as the chain goes)")
    for name, f in (("720p30", "long720p30.mp4"), ("1080p60", "v1080p60.mp4")):
        with Run(f, extra=["--end=15", "--untimed"] if "1080" in f else ["--end=60", "--untimed"], opts={"vsr-adaptive": "no"}) as r:
            if not r.wait_state("on", 20):
                print("BENCH %s untimed: SR did not start (%s %s)" % (name, r.state(), r.reason()))
                continue
            fps_c = r.get("container-fps") or 0
            first = None
            last = None
            while r.alive():
                pos = r.get("time-pos")
                if pos is None:
                    break
                now = time.time()
                if first is None and pos > 1.0:
                    first = (now, pos)
                last = (now, pos)
                if pos >= (14.4 if "1080" in f else 59.4):
                    break
                time.sleep(0.1)
            if first and last and last[0] > first[0]:
                speed = (last[1] - first[1]) / (last[0] - first[0])
                print("BENCH %s untimed: %.1fx real time = %.0f fps through mpv + SR (quality %s), dropped: vo=%s decoder=%s"
                      % (name, speed, speed * fps_c, r.get("user-data/vsr/quality"), r.get("frame-drop-count"), r.get("decoder-frame-drop-count")), flush=True)
            else:
                print("BENCH %s untimed: no measurement" % name)


def main():
    if BENCH:
        bench()
        bench_untimed()
        return
    classification()
    fullscreen_cycle()
    keys()
    failures()
    odd_paths()
    power()
    adaptive()
    lifecycle()
    print("\nSUMMARY: %d passed, %d failed" % (passed, failed))


main()
sys.exit(failed)
