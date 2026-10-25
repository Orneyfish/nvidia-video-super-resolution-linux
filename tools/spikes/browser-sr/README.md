# Spike: where can super resolution hook into Chrome video on this machine? (2026-10-25)

Question: could SR run at system/compositor level ("driver level") for Chrome, so that no extension, no allowlist and
no second window are needed? That only works if the compositor receives the video at source resolution.

## Method
`capture.sh` runs Chrome 156 (native Wayland, throwaway profile) in a headless `cage` compositor on the Intel render
node, with `WAYLAND_DEBUG=1`, playing an 854x480 clip that fills a 1280x720 window.
`webgpu_probe.sh` asks `navigator.gpu.requestAdapter()` in the same setup.

## Results
| Check | Result |
|---|---|
| Default flags: subsurfaces / dmabuf buffer sizes | 0 subsurfaces; only 1280x720 (window size) buffers plus a 1000x89 UI strip |
| HW-decode + `WaylandOverlayDelegation` flags | 1 subsurface, but it carries a 1280x720 buffer (Chrome's own composited page); no 854x480 video buffer |
| Chrome's decode path in run 2 | not confirmed (logging showed nothing); VA-API may not have been active |
| WebGPU adapter, native Wayland, default flags | works: vendor `nvidia`, architecture `ampere` (the RTX 3070 Ti) |
| WebGPU with `--enable-features=Vulkan,WebGPUService --use-angle=vulkan` | works, adds `chromium-experimental-subgroup-matrix` |

## Conclusions
* A compositor/driver-level hook sees only window-sized frames from Chrome: the video is already scaled and blended into
  the page, so there is no source-resolution video to upscale. Not viable for Chrome unless Chrome starts promoting
  hardware-decoded video to its own surface (not observed here; worth re-checking in a real session with VA-API verified).
* In-page processing is possible with no separate window: the page's `<video>` is available at source resolution and
  WebGPU runs on the NVIDIA GPU in the user's Chrome.
