# vsr.vpy - NVIDIA RTX video super resolution for mpv's --vf=vapoursynth.
#
#   mpv --vf=vapoursynth=file=/path/to/vsr.vpy:buffered-frames=4:concurrent-frames=2[:user-data=quality=high;pool=2]
#
# mpv injects video_in (the decoded video), video_in_dw/dh (its display size), container_fps,
# display_res (width, height of the display; 0,0 when unknown) and user_data (the vf option).
# user_data is "key=value;key=value": quality (auto|bicubic|low|medium|high|ultra|streaming_medium|
# streaming_ultra), strength (0..1), pool (1..4), matrix (auto|709|601), range (0 full|1 limited).
#
# The source is upscaled to fit the display, keeping its aspect ratio (never downscaled). Whenever
# anything is unsuitable or fails (no plugin, no GPU, bad format, ...) the script outputs video_in
# unchanged and says why on stderr. HDR and interlaced video cannot be recognised here (mpv only
# provides those frame properties per frame, not when the script is evaluated); vsr.lua keeps them
# out of the filter.
#
# Environment: NVVSR_PLUGIN = path of libvsnvvsr.so (default: ../vapoursynth/build next to this script),
#              NVVSR_DISPLAY = "WxH", used when mpv cannot tell the display size (e.g. --vo=null).
import os
import sys

import vapoursynth as vs

core = vs.core
MAX_SCALE = 4.0  # libnvvsr limit; mpv scales any remainder to the display


def log(msg):
    print("vsr.vpy: " + msg, file=sys.stderr, flush=True)


def parse_user_data(text):
    opts = {}
    for item in (text or "").replace(",", ";").split(";"):
        if "=" in item:
            key, _, value = item.partition("=")
            opts[key.strip().lower()] = value.strip()
        elif item.strip():
            log("ignoring user-data item %r (expected key=value)" % item.strip())
    return opts


def load_plugin():
    path = os.environ.get("NVVSR_PLUGIN")
    if not path:
        if hasattr(core, "nvvsr"):  # already installed in a VapourSynth plugin directory
            return
        try:
            here = os.path.dirname(os.path.abspath(__file__))
        except NameError:
            here = os.getcwd()
        path = os.path.join(here, "..", "vapoursynth", "build", "libvsnvvsr.so")
    core.std.LoadPlugin(os.path.normpath(path))


def display_size():
    width, height = (display_res if "display_res" in globals() else (0, 0))
    if not (width and height):
        env = os.environ.get("NVVSR_DISPLAY", "")
        if "x" in env:
            width, _, height = env.partition("x")
            width, height = int(width), int(height)
    return int(width or 0), int(height or 0)


def even(x):
    return max(2, 2 * int(round(x / 2.0)))


def upscale(src):
    """Return the upscaled clip, or `src` itself when there is nothing to do."""
    fmt = src.format
    if fmt is None or src.width == 0 or src.height == 0:
        log("variable format/size, pass-through")
        return src
    if (fmt.color_family != vs.YUV or fmt.sample_type != vs.INTEGER or fmt.bits_per_sample != 8
            or fmt.subsampling_w != 1 or fmt.subsampling_h != 1):
        log("%s is not 8-bit 4:2:0, pass-through" % fmt.name)
        return src

    dw, dh = display_size()
    if not (dw > 0 and dh > 0):
        log("display size unknown (set NVVSR_DISPLAY=WxH), pass-through")
        return src
    # Scale the pixel grid by the factor that fits the displayed picture (anamorphic sources included).
    vw = globals().get("video_in_dw") or src.width
    vh = globals().get("video_in_dh") or src.height
    scale = min(dw / vw, dh / vh, MAX_SCALE)
    out_w = max(even(src.width * scale), src.width)
    out_h = max(even(src.height * scale), src.height)
    if out_w == src.width and out_h == src.height:
        return src  # already at least display size: nothing to gain

    opts = parse_user_data(globals().get("user_data", ""))
    args = {"width": out_w, "height": out_h, "format": "yuv420p8"}  # mpv accepts YUV output only
    fps = float(globals().get("container_fps") or 0)
    if fps > 0:
        args["fps"] = fps
    if "quality" in opts:
        args["quality"] = opts["quality"]
    if "strength" in opts:
        args["strength"] = float(opts["strength"])
    if "pool" in opts:
        args["pool"] = min(max(int(opts["pool"]), 1), 4)
    if "matrix" in opts:
        args["matrix"] = opts["matrix"]
    if "range" in opts:
        args["range"] = int(opts["range"])
    unknown = set(opts) - {"quality", "strength", "pool", "matrix", "range"}
    if unknown:
        log("ignoring unknown user-data keys: " + ", ".join(sorted(unknown)))

    load_plugin()
    return core.nvvsr.Upscale(src, **args)


try:
    out = upscale(video_in)
except Exception as exc:  # any problem: the video plays unscaled
    log("super resolution disabled, pass-through (%s: %s)" % (type(exc).__name__, exc))
    out = video_in
out.set_output()
