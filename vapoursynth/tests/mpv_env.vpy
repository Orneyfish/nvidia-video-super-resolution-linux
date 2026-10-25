# Runs mpv/vsr.vpy under vspipe, with the variables mpv would inject. Environment:
#   T_* as for clip.vpy (the source, without Upscale), T_DISPLAY=WxH (display_res), T_USERDATA (user_data),
#   T_VSR (path of vsr.vpy), T_FPS (container_fps, default 24)
import os
import runpy

env = os.environ.get
here = os.path.dirname(os.path.abspath(__file__))
src = runpy.run_path(os.path.join(here, "clip.vpy"))["clip"]
dw, _, dh = env("T_DISPLAY", "0x0").partition("x")
runpy.run_path(env("T_VSR"), init_globals=dict(
    video_in=src, video_in_dw=src.width, video_in_dh=src.height, container_fps=float(env("T_FPS", "24")),
    display_fps=60.0, display_res=(int(dw), int(dh)), user_data=env("T_USERDATA", "")))
