"""Frame property and colour handling checks of the nvvsr plugin (run by run.sh).

  python -I props_check.py /path/to/libvsnvvsr.so

Prints one "PASS name" / "FAIL name: why" line per check; exit status = number of failures.
"""
import sys

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(sys.argv[1])
failures = 0


def report(name, ok, why=""):
    global failures
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": " + why))
    failures += 0 if ok else 1


def source(**props):
    # 4:2:0 clip with strongly coloured chroma so that the YUV->RGB matrix matters
    clip = core.std.BlankClip(width=640, height=360, format=vs.YUV420P8, length=2, fpsnum=24, fpsden=1, color=[100, 90, 170])
    return core.std.SetFrameProps(clip, _DurationNum=1001, _DurationDen=24000, _AbsoluteTime=0.5, _FieldBased=0, **props)


def up(clip, **kwargs):
    return core.nvvsr.Upscale(clip, width=1280, height=720, quality="bicubic", **kwargs)


def payload(clip):
    f = clip.get_frame(1)
    return b"".join(bytes(f[p]) for p in range(f.format.num_planes))


# --- RGB output properties -------------------------------------------------------------------
out = up(source(_Matrix=1, _Range=0, _ChromaLocation=1, _Transfer=1)).get_frame(1)
p = out.props
report("rgb: format RGB24 1280x720", out.format.name == "RGB24" and (out.width, out.height) == (1280, 720), out.format.name)
report("rgb: _Matrix=0 _Range=full", p.get("_Matrix") == 0 and p.get("_Range") == 1, str(dict(p)))
report("rgb: _ChromaLocation dropped", "_ChromaLocation" not in p, str(dict(p)))
report("rgb: durations copied", p.get("_DurationNum") == 1001 and p.get("_DurationDen") == 24000, str(dict(p)))
report("rgb: other props kept", p.get("_AbsoluteTime") == 0.5 and p.get("_FieldBased") == 0 and p.get("_Transfer") == 1, str(dict(p)))
report("rgb: clip fps/length copied", (up(source()).fps_num, up(source()).fps_den, up(source()).num_frames) == (24, 1, 2), "")

# --- YUV output properties -------------------------------------------------------------------
out = up(source(_Matrix=5, _Range=1, _ChromaLocation=1), format="yuv420p8").get_frame(1)
p = out.props
report("yuv: format YUV420P8 1280x720", out.format.name == "YUV420P8" and (out.width, out.height) == (1280, 720), out.format.name)
report("yuv: matrix and range follow the source", p.get("_Matrix") == 5 and p.get("_Range") == 1, str(dict(p)))
report("yuv: _ChromaLocation=0 (left)", p.get("_ChromaLocation") == 0, str(dict(p)))
report("yuv: durations copied", p.get("_DurationNum") == 1001 and p.get("_DurationDen") == 24000, str(dict(p)))
p = up(source(), format="yuv420p8").get_frame(0).props
report("yuv: no props -> _Matrix 6 (SD size rule), limited", p.get("_Matrix") == 6 and p.get("_Range") == 0, str(dict(p)))

# --- colour selection: arguments, _Matrix, _ColorSpace (the name mpv uses), _Range -------
m709 = payload(up(source(), matrix="709"))
m601 = payload(up(source(), matrix="601"))
report("matrix: 601 and 709 differ on coloured input", m709 != m601, "identical output")
report("matrix: _Matrix=1 equals matrix=709", payload(up(source(_Matrix=1))) == m709, "")
report("matrix: _Matrix=5 equals matrix=601", payload(up(source(_Matrix=5))) == m601, "")
report("matrix: _Matrix=6 equals matrix=601", payload(up(source(_Matrix=6))) == m601, "")
report("matrix: mpv's _ColorSpace=1 equals matrix=709", payload(up(source(_ColorSpace=1))) == m709, "")
report("matrix: mpv's _ColorSpace=6 equals matrix=601", payload(up(source(_ColorSpace=6))) == m601, "")
report("matrix: no property, 640x360 -> BT.601 by size", payload(up(source())) == m601, "")
report("matrix: argument beats the property", payload(up(source(_Matrix=1), matrix="601")) == m601, "")
full = payload(up(source(), matrix="709", range=0))
lim = payload(up(source(), matrix="709", range=1))
report("range: full and limited differ", full != lim, "identical output")
report("range: _Range=1 (full) equals range=0", payload(up(source(_Range=1), matrix="709")) == full, "")
report("range: _Range=0 (limited) equals range=1", payload(up(source(_Range=0), matrix="709")) == lim, "")
report("range: default is limited", payload(up(source(), matrix="709")) == lim, "")
report("range: argument beats the property", payload(up(source(_Range=0), matrix="709", range=0)) == full, "")

# --- the three-way colour cache: frames with different properties in one clip ------------------
mixed = core.std.Interleave([source(_Matrix=1), source(_Matrix=5)])  # 709, 601, 709, 601
mix = up(mixed)
ok = all(bytes(mix.get_frame(n)[0]) == bytes((up(source(_Matrix=1)) if n % 2 == 0 else up(source(_Matrix=5))).get_frame(0)[0]) for n in range(4))
report("matrix: alternating per-frame properties re-create the context correctly", ok, "")

# --- stress: alternating matrices (forces contexts to be re-created) under heavy concurrency -----
base = core.std.Interleave([source(_Matrix=1), source(_Matrix=5)])
base = core.std.Loop(base, 60)  # 240 frames: 709, 601, 709, 601, ...
want = [b"".join(bytes(f[p]) for p in range(3))
        for f in (core.nvvsr.Upscale(source(_Matrix=m), width=1280, height=720, quality="medium").get_frame(0) for m in (1, 5))]
for pool in (1, 2, 4):
    stressed = core.nvvsr.Upscale(base, width=1280, height=720, quality="medium", pool=pool)
    bad = 0
    count = 0
    for n, f in enumerate(stressed.frames(prefetch=8, backlog=16)):
        bad += b"".join(bytes(f[p]) for p in range(3)) != want[n % 2]
        count += 1
    report("stress: 240 alternating-matrix frames, pool=%d, 8 requests in flight" % pool, bad == 0 and count == 240,
           "%d of %d frames differ" % (bad, count))

sys.exit(failures)
