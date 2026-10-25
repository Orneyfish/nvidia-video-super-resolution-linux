# VLC 3 module: NVIDIA RTX video super resolution

`libnvvsr_plugin.so` upscales the video VLC 3.0.x shows while it is **fullscreen** (SDR 8-bit 4:2:0, smaller than the
screen, on mains power) using `libnvvsr` (`core/include/nvvsr.h` is the only core header used). Anything else is
shown exactly as VLC would show it without the module.

It contains two entry points that share the code in `src/common.c`:

| capability | name | use |
|---|---|---|
| `vout display` | `nvvsr` | **playback**: `vlc --vout=nvvsr` (or picked automatically, see below) |
| `video filter` | `nvvsr` | size-changing filter for transcode/sout chains: `--sout '#transcode{vfilter=nvvsr{width=2560,height=1440},...}'` |

## Design answers (VLC 3.0.24, verified against the 3.0.x source and by tests)

**(1) Can a "video filter" module change the picture size so the vout accepts it? No, not in the player.**
`croppadd` shows the pattern (`b_allow_fmt_out_change`, `fmt_out` != `fmt_in`) and `filter_chain` accepts it, but the vout
(`src/video_output/video_output.c`, `ThreadChangeFilters`) compares the chain's output with the decoder's format after it
built the chains and, if they differ, *appends a converter that scales the picture back* ("Adding a filter to compensate
for format changes"). Measured with the stock filter (test 3): `croppadd` 1280x620 is scaled back to 1280x720 by
swscale. So a filter that outputs 2560x1440 would only waste the GPU. The display side cannot help either: the display
chain converts `vd->source` -> `vd->fmt`, and stock displays never ask for a larger `fmt`. The "video converter" capability
has the same problem (it is only used for those two compensations).
What does work is a **`vout display` wrapper**: a display is told the size of the pictures it shows by its own
`source`/`fmt`, and VLC lets a display change `fmt` at run time (`VOUT_DISPLAY_EVENT_PICTURES_INVALID` ->
`VOUT_DISPLAY_RESET_PICTURES`, the protocol the Direct3D displays use). The wrapper opens VLC's real output (gl, xcb,
wayland, ...) as a nested `vout_display_t` with `module_need("vout display")`, forwards events/windows/controls, and
shows it pictures of the super-resolved size. The picture the core hands to the wrapper stays at the decoder's size, so
subtitles, OSD, aspect ratio, crop, snapshots and hardware surfaces (VAAPI/VDPAU are converted to I420 by VLC's own
converters) keep working. The video filter is kept for what a filter can do: transcode/sout chains (`p_uf_chain` allows
format changes). In the vout it refuses to load (with a warning) instead of upscaling pictures that are scaled back.

**(2) Fullscreen gating.** The vout object has a `fullscreen` variable (`vout_intf.c`); the Qt GUI, the F key and
double click all toggle it, and `oldrc`'s `f` command does too (that is how the tests toggle it headlessly). The
wrapper registers `var_AddCallback(vout, "fullscreen", ...)` (an atomic flag) and polls its policy every 50 ms from the
display's `manage()`. Switching is: policy changes -> (the nvvsr context is created ahead of time in a worker thread)
-> after a 0.2-0.3 s settle time the wrapper sends `PICTURES_INVALID` -> the core calls `control(RESET_PICTURES)` ->
the wrapper closes the nested display and reopens it (windowed: decoder format, `vd->fmt` = the real display's own, all
calls are plain forwards, zero-copy VAAPI interop included; SR: source of the SR size, `vd->fmt` = 8-bit 4:2:0 at the
decoder's size) and updates `vd->fmt`/`vd->info`. The core rebuilds its conversion chain and pools by itself. The VLC
window is not touched, only the display inside it is reopened (about 4-20 ms with a software GL display in the tests).
So no interface/lua helper is needed. Pass-through is therefore cheap by construction: it is not a mode of the
converter, it is the real display with forwarding calls. Cost of the wrapper: it must set `has_pictures_invalid`, which
makes the core copy each decoded picture once into the display's pool instead of decoding straight into it.

**(3) Target size.** `--nvvsr-width/--nvvsr-height` (0 = the size of the video window, which is the screen while
fullscreen; headless runs and the video filter need them). The source (with its pixel aspect ratio) is fitted into the
panel like `mpv/vsr.vpy`: `scale = min(pw/dw, ph/dh)` capped at 4, even sizes >= the source. The panel size is
re-evaluated when the window size changes (hysteresis 2 %, at most one size change every 2 s).

## Eligibility (mirrors `mpv/vsr.lua` where VLC exposes the data)

Upscaled only if: fullscreen (`--nvvsr-fullscreen-only`, default on), mains power (`--nvvsr-ac-only`, default on;
Mains/online USB-C supplies count, no battery info = mains, polled every 30 s), source height >= 360
(`--nvvsr-min-height`), source smaller than the panel by >= 2 % when fitted (`--nvvsr-min-gain`), 8-bit 4:2:0 (I420,
J420, YV12, NV12, VAAPI/VDPAU surfaces), SDR (no PQ/HLG), no BT.2020, not rotated, even size/crop offsets, not
interlaced (3 interlaced pictures in a row without a deinterlacer turn it off), 16..8192 pixels. 10-bit, 4:2:2/4:4:4 and
anything else is passed through (not converted: unlike mpv, `convert_10bit` does not exist here). Quality
`--nvvsr-quality auto|bicubic|low|medium|high|ultra|streaming_medium|streaming_ultra` (auto = `nvvsr_auto_quality`).
One `nvvsr_ctx` per video; it is created in the background when the policy first wants SR and destroyed when SR stops
(the core parks the SDK effect, so re-entering costs ~10 ms and ~0 VRAM growth).

## Selecting the module

- `--vout=nvvsr`: explicit. Videos SR cannot help are still shown through it, unchanged.
- No `--vout` (auto): the wrapper has score 300 (`-DNVVSR_VLC_DISPLAY_SCORE=0` at configure time makes it
  explicit-only) and is tried first; it declines (the normal output is used, no wrapper at all) when the source is not
  eligible, when `--no-nvvsr-enable` is given or when a probe already failed in this process. `--nvvsr-vout=gl` (etc.)
  names the real output (default: VLC's usual automatic choice).
- `--vout=<anything else>` bypasses the module entirely.

## Failure handling

Every failure ends in the original video and one `msg_Warn`: library/driver/GPU missing (`nvvsr_probe`, run in a
worker thread: once per process, then the module declines automatically), `nvvsr_create` failing, `nvvsr_process`
failing (that frame is produced by a CPU bilinear upscale, then the display goes back to the source size within one
poll, no retry), the real display refusing the bigger size, unsupported/odd formats (never wrapped), no output picture,
picture not matching the negotiated format. Nothing in the module can stall playback: the SDK work happens off the
vout thread except `nvvsr_process` itself (5 ms).

## Build and test

```
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core -j     # once
cmake -S vlc -B vlc/build -DCMAKE_BUILD_TYPE=Release && cmake --build vlc/build -j         # -> vlc/build/libnvvsr_plugin.so
vlc/tests/run.sh [--cage] [--bench] [--only TAG...]    # PASS/FAIL per check, exit status = failures
```
`run.sh` builds its own test build (`vlc/build-test`, with the test hooks) and runs VLC completely headless: dummy
interface, no `DISPLAY`/`WAYLAND_DISPLAY`/D-Bus, scratch `HOME`/XDG, scratch plugin directory + `vlc-cache-gen`. The
pictures VLC shows are captured by wrapping VLC's own `yuv` video output and compared byte for byte with libnvvsr called
directly on the same decoded frames (`vlc/build-test/nvvsr_vlc_ref`, `tests/compare.py`). `--cage` additionally runs VLC
with a real OpenGL output inside a headless `cage` compositor (`WLR_BACKENDS=headless`, never the desktop).
Note: `--vout=dummy` is NOT headless on a desktop session: VLC 3 still opens a window through the vout window modules
(xdg_shell/xcb_window) before the display module discards it, so the harness removes the display variables.

Installing into VLC is Phase 6; to try it by hand without installing anything:
```
mkdir -p ~/vsr-vlc/plugins/video_output && ln -sf $PWD/vlc/build/libnvvsr_plugin.so ~/vsr-vlc/plugins/video_output/
/usr/lib/vlc/vlc-cache-gen ~/vsr-vlc/plugins
NVVSR_LIBDIR=<venv>/lib/python3*/site-packages/nvvfx/libs:<venv>/lib/python3*/site-packages/nvidia_vfx.libs \
  VLC_PLUGIN_PATH=~/vsr-vlc/plugins vlc -vv --vout=nvvsr video.mp4 2>&1 | grep -i nvvsr
```
Press `f` (or double click) for fullscreen: the log shows `super resolution on: 1280x720 -> 2560x1440`; `f` again shows
`super resolution off (not fullscreen)`. `--nvvsr-quality=bicubic` is the same pipeline without the network (A/B).

## Limitations

- Fullscreen is VLC's own `fullscreen` state (F key, double click, menu, `--fullscreen`). A fullscreen requested only
  by the compositor (a Hyprland keybind) is not seen; use `--no-nvvsr-fullscreen-only` to upscale in any window larger
  than the video.
- The picture goes GPU -> host -> display; the nested display uploads it again (about 5.5 MB per 1440p frame).
  Hardware-decoded frames are first downloaded by VLC's converter (as with mpv's `*-copy`).
- Subtitles/OSD are blended into the decoder-size picture before SR (they are upscaled with it).
- While paused VLC redisplays the last picture every ~80 ms, so SR runs on it each time.
- Only tested with the `dummy`, `yuv` and (inside headless cage/XWayland) `gl`/`glx`/`egl_x11` outputs. Not yet
  tested in a real Qt window on the user's Hyprland session (steps above), nor with VAAPI surfaces on a real display.
- `nvvsr_create` needs >= 2 MiB of stack (it crashes in libnvidia-ngx on VLC's 1 MiB thread stacks), so the module always
  creates contexts on its own 8 MiB thread (`common.c: nv_engine_open`). Worth fixing inside the core.
