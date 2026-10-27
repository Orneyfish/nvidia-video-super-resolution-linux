# vsr.vpy - NVIDIA RTX video super resolution for mpv's --vf=vapoursynth. All decisions (fit to the display, never
# downscale, 8-bit 4:2:0 only, user-data options, pass-through on any problem) live in the nvvsr plugin's Auto function.
#   mpv --vf=vapoursynth=file=/path/to/vsr.vpy:buffered-frames=4:concurrent-frames=2[:user-data=quality=high;pool=2]
# Environment: NVVSR_PLUGIN = libvsnvvsr.so (default: next to this script, ../lib (installed layout), ../vapoursynth/build);
#              NVVSR_DISPLAY = "WxH", the display size when mpv cannot tell (e.g. --vo=null).
import os
import sys

import vapoursynth as vs

core = vs.core
out = video_in
try:
    path = os.environ.get("NVVSR_PLUGIN")
    if not path and not hasattr(core, "nvvsr"):  # not installed in a VapourSynth plugin directory
        here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
        cands = [os.path.join(here, d, "libvsnvvsr.so") for d in (".", "../lib", "../vapoursynth/build")]
        path = next((c for c in cands if os.path.isfile(c)), cands[-1])  # none: LoadPlugin reports the last
    if path:
        core.std.LoadPlugin(os.path.normpath(path))
    dw, dh = display_res if "display_res" in globals() else (0, 0)
    out = core.nvvsr.Auto(video_in, display_w=dw, display_h=dh, dar_w=globals().get("video_in_dw") or 0,
                          dar_h=globals().get("video_in_dh") or 0, fps=float(globals().get("container_fps") or 0),
                          options=globals().get("user_data") or "")
except Exception as exc:  # the plugin is missing or broke: the video plays unscaled
    print("vsr.vpy: pass-through (%s: %s)" % (type(exc).__name__, exc), file=sys.stderr, flush=True)
out.set_output()
