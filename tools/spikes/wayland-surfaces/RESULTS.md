# Which players give the compositor a source-size picture? (2026-10-30, Hyprland 0.57.0, mpv 0.41.0, VLC 3.0.24, Chrome 156)

`probe.sh LABEL SECONDS COMMAND...` runs a player fullscreen on a 1280x720 headless `cage` output with a 640x360 clip and
`WAYLAND_DEBUG=client`, and counts the buffers it creates and the viewport destinations it sets. Nothing appears on the desktop.

| player / output | buffers submitted | viewport | compositor sees the source-size frames? |
|---|---|---|---|
| mpv `--vo=gpu` (default) | 3 x 1280x720 | 1280x720 | no, the player scales |
| mpv `--vo=gpu-next` | 3 x 1280x720 | 1280x720 | no |
| mpv `--vo=dmabuf-wayland` | 15 x **640x360** (dmabuf) | 1280x720 | **yes**, the compositor scales (wp_viewporter) |
| VLC default (GL) | 5 x 1280x720 (+1 x 640x360) | none | no |
| VLC `--vout=wl_shm` | 421 x **640x360** (shm, CPU copies) | none | yes, but through CPU memory |
| Chrome (kiosk, video element) | 3 x 1280x720 | none | no (same as the 2026-10-25 spike) |

Conclusion: a compositor plugin (the Wayland analogue of Windows' driver hook) would only see source-size frames from
mpv with `--vo=dmabuf-wayland` and from VLC's `wl_shm` output; every default player, and Chrome, scales the video itself before
the compositor gets it. So a general driver or compositor level upscaler is not possible on Linux today; per-player
integration (what mpv/VLC/Chrome already have here) is the working design. What a system level service can still do: manage the
GPU (wake it before an upscaler starts, release it after) and start helper daemons on demand.
