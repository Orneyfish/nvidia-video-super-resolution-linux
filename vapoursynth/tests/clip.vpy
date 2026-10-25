# Test clip for run.sh, configured through environment variables (all optional):
#   T_W, T_H, T_N          size and frame count of the source (default 1280x720, 10)
#   T_SRC_FORMAT           vapoursynth format constant of the source (default YUV420P8)
#   T_PATTERN=1            deterministic test pattern instead of a flat colour (YUV420P8 only)
#   T_NEUTRAL=1            pattern with neutral chroma (grey picture)
#   T_PROP_MATRIX, T_PROP_COLORSPACE, T_PROP_RANGE (_Range: 1 full, 0 limited), T_PROP_CHROMALOC   frame properties to set
#   T_VARIABLE=1           variable-size source (640x360 spliced with the T_W x T_H clip)
#   T_UP=1                 apply core.nvvsr.Upscale; its arguments come from
#   T_UP_WIDTH, T_UP_HEIGHT, T_UP_QUALITY, T_UP_STRENGTH, T_UP_MATRIX, T_UP_RANGE, T_UP_POOL, T_UP_FPS, T_UP_FORMAT
#   NVVSR_PLUGIN           plugin path
import os

import numpy as np
import vapoursynth as vs

core = vs.core
env = os.environ.get
W, H, N = int(env("T_W", "1280")), int(env("T_H", "720")), int(env("T_N", "10"))
fmt = getattr(vs, env("T_SRC_FORMAT", "YUV420P8"))
neutral = env("T_NEUTRAL") == "1"

clip = core.std.BlankClip(width=W, height=H, format=fmt, length=N, fpsnum=24, fpsden=1, color=[90, 128, 128] if fmt == vs.YUV420P8 else None)


def pattern(n, f):
    out = f.copy()
    yy, xx = np.mgrid[0:H, 0:W]
    y = (16 + ((xx * 7 + yy * 3 + n * 11) % 220) // 1 + 12 * (((xx // 16) + (yy // 16)) % 2)).clip(16, 235)
    np.asarray(out[0])[:] = y.astype(np.uint8)
    cy, cx = np.mgrid[0:H // 2, 0:W // 2]
    if neutral:
        np.asarray(out[1])[:] = 128
        np.asarray(out[2])[:] = 128
    else:
        np.asarray(out[1])[:] = (64 + (cx * 3 + n) % 128).astype(np.uint8)
        np.asarray(out[2])[:] = (64 + (cy * 5 + 2 * n) % 128).astype(np.uint8)
    return out


if env("T_PATTERN") == "1":
    clip = core.std.ModifyFrame(clip, clip, pattern)

if env("T_VARIABLE") == "1":
    clip = core.std.Splice([core.std.BlankClip(clip, width=640, height=360, length=2), clip], mismatch=True)

props = {}
for key, var in (("_Matrix", "T_PROP_MATRIX"), ("_ColorSpace", "T_PROP_COLORSPACE"), ("_Range", "T_PROP_RANGE"),
                 ("_ChromaLocation", "T_PROP_CHROMALOC")):
    if env(var) is not None:
        props[key] = int(env(var))
if props:
    clip = core.std.SetFrameProps(clip, **props)
clip = core.std.SetFrameProps(clip, _DurationNum=1001, _DurationDen=24000, _AbsoluteTime=0.5, _FieldBased=0)

if env("T_UP") == "1":
    if env("NVVSR_PLUGIN"):
        core.std.LoadPlugin(env("NVVSR_PLUGIN"))
    args = {}
    for arg, var, conv in (("width", "T_UP_WIDTH", int), ("height", "T_UP_HEIGHT", int), ("quality", "T_UP_QUALITY", str),
                           ("strength", "T_UP_STRENGTH", float), ("matrix", "T_UP_MATRIX", str), ("range", "T_UP_RANGE", int),
                           ("pool", "T_UP_POOL", int), ("fps", "T_UP_FPS", float), ("format", "T_UP_FORMAT", str)):
        if env(var) is not None:
            args[arg] = conv(env(var))
    clip = core.nvvsr.Upscale(clip, **args)

clip.set_output()
