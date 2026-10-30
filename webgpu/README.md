# webgpu/ — in-page video upscaler (prototype)

Upscales a `<video>` inside the page with WebGPU, no second window and no change to the player: `VideoUpscaler` in
`src/upscaler.js` follows `video.requestVideoFrameCallback`, imports each frame with `importExternalTexture`, upscales it
with plain render passes and draws it on a `<canvas>` placed over the video. Audio is untouched; no build step, no network.

It is **not** NVIDIA's VSR network (that lives in libnvidia-ngx and cannot run in a page). It runs open models:
bilinear (what the browser does anyway), AMD FSR 1.0 (EASU + RCAS, MIT) and Anime4K CNN in sizes S and M with an optional
"restore" pass (MIT); licences in `LICENSE-FSR.txt` and `LICENSE-Anime4K.txt`, weights extracted by
`tools/extract-anime4k.cpp` into `src/models/`. It exists because Chrome on Linux hands the compositor window-sized
buffers only, so no driver or compositor hook can see the source-resolution video (`tools/spikes/browser-sr/`); an
extension content script can reach the `<video>` on any site; that wrapper is `extension/` (below).

```js
const up = await VideoUpscaler.create(video, canvas, { mode: 'cnn', cnnSize: 'S' });  // 'bilinear' | 'fsr' | 'cnn'
up.start(); up.stats(); up.setMode('fsr'); up.destroy();
```

## Extension (`extension/`, MV3, prototype)

`content.js` (about 60 lines) runs on every page and frame. On `fullscreenchange` it finds the biggest `<video>` in the
fullscreen element and, if the picture is smaller than the screen and the machine is on AC power, lays a canvas over it and
runs `VideoUpscaler` (Anime4K CNN-S). Leaving fullscreen, `Alt+S`, an error or DRM video give back the plain video; a small
semi-transparent "SR" / "SR off" / "SR failed" badge shows for 1.5 s. **NVIDIA only (user rule: never the iGPU):** it
requests the `high-performance` adapter and switches itself off unless that is an NVIDIA GPU. `extension/src` is a symlink
to `../src`. These are CUDA-core render passes through WebGPU, not tensor cores and not NVIDIA's VSR network (see "NVIDIA
decode and tensor cores" below).

Load it: `chrome://extensions` -> Developer mode -> Load unpacked -> this repository's `webgpu/extension`. Chrome needs
WebGPU on Linux: with `--ignore-gpu-blocklist` (already in `~/.config/chrome-flags.conf` here) both adapters appear, without
it `requestAdapter()` returns null and the extension logs `no-adapter` and does nothing.

Tested 2026-10-28 on YouTube ("Me at the zoo", 320x240) in a real Chrome window on the live Hyprland desktop
(`test/ext_drive.cpp`): with the Intel adapter (before the NVIDIA-only rule) `[nvvsr] on low-power 320x240`, fullscreen box
1920x1440, canvas 1280x960 (the 4x cap), and a screenshot with the canvas differs from one without it (38.1 dB PSNR between
them; a little crisper on edges). In a normal Chrome session the NVIDIA adapter cannot import Chrome's video frames
(`import-failed`, "Requested allocation size is smaller than the image requires") because Chrome composites on the iGPU.
With Chrome's whole stack on the RTX (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`, `--use-angle=vulkan
--use-vulkan=native --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE`) the extension runs on the NVIDIA adapter
(`[nvvsr] on nvidia 320x240`). Not yet tried: Crunchyroll (DRM video cannot be imported, it must fall back to the plain
video) and the YouTube web app in the user's own profile.

## NVIDIA decode and tensor cores (measured 2026-10-28, Chrome 156)

The user wants NVDEC for decoding and tensor cores for the upscaling, no iGPU. In Chrome on this machine:

- **NVDEC works** through VA-API (`libva-nvidia-driver` is installed): `LIBVA_DRIVER_NAME=nvidia NVD_BACKEND=direct
  __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia`, `--ozone-platform=wayland --use-gl=angle --use-angle=gl
  --enable-features=VaapiOnNvidiaGPUs,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL,VaapiIgnoreDriverChecks
  --disable-features=UseChromeOSDirectVideoDecoder` makes `mediaCapabilities` report H.264 and VP9 as power efficient
  (hardware). Real decoder load was not measured (the 240p test clip is too small for `nvidia-smi dmon` to show it).
- **VA-API decode and WebGPU on the RTX exclude each other**: WebGPU needs the Vulkan feature, and with it Chrome turns
  VA-API off (`powerEfficient` false in every combination tried); without Vulkan `requestAdapter()` is null.
- WebGPU shaders would not use tensor cores anyway. The only tensor-core upscaler here is NVIDIA's VSR network (libnvvsr).

So the route that fits (no WebGPU anywhere): Chrome decodes on NVDEC with the flags above; the local C++ daemon
`nvvsr_webd` (`webgpu/daemon`) runs libnvvsr on the RTX; `extension/content.js` copies each decoded frame with
`VideoFrame.copyTo` over a localhost WebSocket (3.1 MB at 1080p, one request in flight) and draws the upscaled I420 reply
(5.5 MB at 1440p) on a 2D canvas over the video: about 260 MB/s at 30 fps. A content script's WebSocket carries the page's
origin, so the daemon must be told which sites may use it:
`NVVSR_LIBDIR=$(tools/libdir.sh) webgpu/daemon/build/nvvsr_webd 41777 --origin https://www.youtube.com`
(build: `cmake -S webgpu/daemon -B webgpu/daemon/build -DCMAKE_BUILD_TYPE=Release && cmake --build webgpu/daemon/build -j`).
`#nvvsr-debug` in the page URL makes the script log its stats every 2 s.

Measured 2026-10-28 (Chrome 156, the NVDEC flags above, scratch profile, ext_drive; the page was a local test page
`http://127.0.0.1:PORT` with 30 fps H.264 clips, daemon started with `--origin http://127.0.0.1:PORT`):

- **320x240 works end to end.** Chrome gives **NV12** frames (so `copyTo` with `format: 'I420'` is not an option; the script
  copies NV12 and de-interleaves the chroma in JS). Output 1280x960 (the 4x cap), 30 fps in and 30 fps out, 21 frames skipped
  while the daemon creates its context (~0.7 s) then 0 drops, 0 daemon `DROP`/`ERR`; round trip send -> draw p50 9.3 to
  10.4 ms, p95 11.2 to 13.0 ms; `copyTo` 7.0 to 8.1 ms (a GPU read-back, large for 115 KB); draw 1.1 to 1.3 ms; the picture on
  screen trails `video.currentTime` by 2.7 to 4.1 ms (median). The WebSocket from a content script on an `http://127.0.0.1`
  page connected (page origin accepted by the daemon). Screenshots with and without the canvas differ (PSNR 40.4 dB, a little
  crisper, same colours).
- **1280x720 fails in this Chrome configuration, before the extension can help.** `copyTo` throws `InvalidStateError: Failed
  to read VideoFrame data`; the GPU process logs `eglCreateImage failed with 0x3009` (EGL_BAD_MATCH), `Unable to initialize
  binding from pixmap` and `glReadbackImagePixels: Unknown mailbox` for every frame, and an in-page screenshot of the plain
  video is white: Chrome cannot import the NVDEC dmabufs into GL at this size (240p worked). The extension then reports
  "SR failed" and leaves the plain video. Cause not diagnosed. Next thing to try (a guess, not run): drop
  `AcceleratedVideoDecodeLinuxZeroCopyGL` so the frames are copied instead of imported as zero-copy dmabufs.
- **Not measured:** 1080p, decoder and SM load (`nvidia-smi dmon`), renderer CPU, and YouTube itself: YouTube answered the
  scripted scratch profile with its "unusual traffic" reCAPTCHA page (no workaround attempted), so a page CSP on YouTube
  remains untested. Until 720p and 1080p frames can be read back, the route is **not proven for typical video sizes**.
- The "Extension" section above still describes the old WebGPU `content.js` (superseded; `extension/src` and
  `web_accessible_resources` are gone).
- **Cause found 2026-10-30 (the user saw white frames and continuous repainting in their own Chrome through `tools/nvchrome`):**
  with the NVDEC flags a 1080p YouTube video renders as a WHITE page (in-page screenshot 17 KB instead of ~1 MB) even though
  the decoder engine is at 5-11 % and `mediaCapabilities` says hardware. Dropping `AcceleratedVideoDecodeLinuxZeroCopyGL` does
  not help. Adding `__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json` (Chrome's EGL on the RTX too)
  gives a correct picture (944 KB screenshot) but Chrome then reports no hardware decode. So NVDEC decoding and a correct
  picture have not been obtained together in Chrome 156 here; the upscaling route needs readable frames, so it is blocked on
  this, not on the daemon (which works: 240p end to end at 30 fps, ~10 ms round trip). Ideas not tried: `--render-node-override`
  for the NVIDIA node (`/dev/dri/renderD129`), `NVD_BACKEND=egl`, Chrome's Vulkan video decode, a newer `libva-nvidia-driver`.
- The launchers were reverted to plain Chrome on 2026-10-30; `~/.local/bin/nvchrome` was removed.

`test/ext_drive.cpp` (C++) drives Chrome through `--remote-debugging-pipe`: `--load-extension` is gone in Chrome 156 and
remote debugging is refused on the default profile directory, so it uses `Extensions.loadUnpacked` on a scratch profile:
`ext_drive CHROME webgpu/extension URL OUT_DIR WAIT_LOAD_S WAIT_FS_S --user-data-dir=DIR --ignore-gpu-blocklist ...`; it opens one
window, presses `f`, and writes the page state and two screenshots (canvas on and hidden) to OUT_DIR.

### The route that works in Hybrid mode: CPU decode + tensor-core upscaling (measured 2026-10-30)

NVDEC decoding and a correct picture cannot be had together in Chrome 156 in Hybrid mode (Chrome's GL runs on the GPU the
compositor uses, the Intel one, and cannot import NVIDIA-decoded dmabufs; with NVIDIA EGL/GLX Chrome switches VA-API off). The
alternative that needs no MUX switch: leave Chrome's decoding alone (default flags, CPU) and upscale on the RTX through the
daemon. Software-decoded frames are already in system memory, so `copyTo` is a memory copy, not a GPU read-back.

Measured in a headless `cage` (compositor on Intel, `gles2`), default Chrome flags, extension `content.js`, daemon on the RTX,
test page with 30 fps H.264 clips and `#nvvsr-box=2560x1440` (pretend the video fills a 2560x1440 screen); one request in flight:

| source -> output | fps in / out | frames | daemon DROP / ERR | RTT p50 / p95 | `copyTo` | draw | lag behind the video |
|---|---|---|---|---|---|---|---|
| 720p -> 2560x1440 | ~30 / ~28 | 390 sent, 390 received | 0 / 0 | not saved (first run), same order as 1080p | | | |
| 1080p -> 2560x1440 | 30 / 28-29 | 383 sent, 383 received | 0 / 0 | 13.4-14.3 / 14.8-18.9 ms | 10.5-11.3 ms | 1.2 ms | 6-12 ms (median) |

About 7 % of the frames were skipped on the Chrome side (a request was still in flight), none by the daemon. The lag is well
under one frame, so lip sync is not an issue. `copyTo` is dominated by the JavaScript NV12 -> I420 de-interleave: sending NV12
as it is (libnvvsr takes `NVVSR_IN_NV12`) should remove most of it, and a second request in flight is needed for 60 fps (RTT p95
~18 ms is above the 16.6 ms budget). Not verified: real YouTube (its page CSP for the content script's WebSocket; the scripted
fullscreen key did not start in the headless runs), real DRM sites (they cannot work), the tensor-core load in `nvidia-smi`
(the sample missed the playback), and the real Hybrid desktop session.

**After the speed-ups (2026-10-30, same setup):** the extension sends Chrome's NV12 frames as they are (header flag bit 1; the
daemon takes `NVVSR_IN_NV12`, output byte-identical to the I420 route) and keeps up to two requests in flight:

| source -> output | fps in / out | skipped | daemon DROP / ERR | RTT p50 (p95 max) | `copyTo` | draw | lag (median) | RTX sm / mem during playback |
|---|---|---|---|---|---|---|---|---|
| 720p30 -> 1440p | 29.3 / 28.6 | 10 of 410 | 0 / 0 | 12.3 (19.9) ms | 6.6 ms | 1.3 ms | 8.7 ms | 17 % / 7 % |
| 1080p30 -> 1440p | 29.3 / 29.3 | 0 of 410 | 0 / 0 | 13.6 (24.5) ms | 10.1 ms | 1.3 ms | 7.5 ms | 15 % / 5 % |
| 1080p60 -> 1440p | 55.1 / 51.9 | 45 of 772 | 0 / 0 | 22.5 (27.4) ms | 8.7 ms | 1.3 ms | 7.7 ms | 25 % / 8 % |

The 60 fps clip itself only plays at ~55 fps in this headless harness (plain video managed ~45 earlier), so 52 out of 55 is the
upscaler keeping up. `copyTo` did not shrink much at 1080p (11 -> 10 ms): the JavaScript de-interleave was not the main cost,
reading the frame back from the page's GPU texture is. The RTX load (`nvidia-smi dmon`) shows the work happening on the
GPU; the decoder engine stays at 0 % (CPU decode). The daemon starts with a systemd user unit template,
`daemon/nvvsr-webd.service` (not installed anywhere).

Debug options in the page URL hash: `#nvvsr-debug[=PORT]` (log the stats; with PORT also post them to `test/server.cpp`) and
`#nvvsr-box=WxH` (pretend the video box is WxH device pixels) and `#nvvsr-force` (start 8 s after load without fullscreen).

**How much sharper is it? (user: "SR active but the sharpness is not there at all", measured 2026-10-30)** The upscaling is
really applied (Playwright run `test/pw_compare.js`, real YouTube 720p at device scale 2: canvas on vs off differ, PSNR
37.5 dB between them), but the gain of NVIDIA's network on compressed video is small. Objective test (4 photos -> 2560x1440
reference -> 1280x720 -> H.264 CRF 27 -> 2x upscale, mean PSNR / SSIM against the reference; tool `vlc/build/nvvsr_vlc_ref`):
ffmpeg bicubic 38.18 dB / 0.9469, libnvvsr bicubic 38.20 / 0.9475, HIGH 38.55 / 0.9502, **ULTRA 38.72 / 0.9517**,
STREAMING_MEDIUM 38.30 / 0.9496, STREAMING_ULTRA 38.38 / **0.9521**. The default `auto` picks HIGH at 720p; ULTRA gains ~0.2 dB
more. Viewed side by side the network cleans edges a little but cannot restore detail the compression removed; a plain
unsharp mask after the upscale makes edges look crisper (more halos, slightly lower SSIM). Larger gains are expected for
lower-resolution sources (360p/480p) than for 720p/1080p on a 1440p screen.

**Why the socket lives in an extension iframe (found 2026-10-30):** on real YouTube the extension showed "SR failed": a public
page origin (https://www.youtube.com) cannot open a WebSocket to `127.0.0.1`, not even from its content script
("daemon not reachable within 1.5 s"; the same code connected from a page on `http://127.0.0.1`). `content.js` therefore injects
`bridge.html` (an extension-origin iframe) and `bridge.js` opens the socket to `ws://127.0.0.1:41777` from there; the frames
cross a `MessageChannel` as ArrayBuffers. The daemon now sees `Origin: chrome-extension://...`, which it accepts by default (the
`--origin https://www.youtube.com` of the service becomes unnecessary). The manifest enables the extension only on
`https://*.youtube.com/*` and `http://127.0.0.1/*` (the test pages); `web_accessible_resources` uses the same list, so other pages cannot
embed the bridge and borrow the daemon. To enable another site add it to both lists in `manifest.json` and reload the extension.
Real YouTube, headless (`#nvvsr-force`): 480p -> 2560x1438, 30 fps in and out, RTT p50 ~19 ms, daemon DROP/ERR 0.

## Results (Chrome 156, headless `cage`, 2560x1440 canvas, RTX 3070 Ti Laptop + Intel Iris Xe)

`test/run.sh --config intel` = plain Chrome, WebGPU on the iGPU that composites (the RTX stays asleep).
`--config nvidia` = Chrome's whole GPU stack on the RTX, the only way found to let an NVIDIA WebGPU adapter exchange
frames with Chrome on this hybrid laptop. Raw tables: `test/results/*.md`.

GPU time of the upscale passes alone (median ms per frame, isolated):

| source -> 1440p | mode | Intel iGPU | RTX |
|---|---|---|---|
| 720p | FSR / CNN-S / CNN-M | 2.53 / 2.0 / 4.31 | 0.27 / 0.27 / 0.55 |
| 1080p | FSR / CNN-S / CNN-M | 2.61 / 5.68 / 11.0 | 0.28 / 0.73 / 1.34 |
| 720p | CNN-M with restore | 8.19 | 1.06 |

Playback of 15 s clips at 30 fps: every mode tried (bilinear, FSR, CNN-S, CNN-M at 720p; bilinear, FSR, CNN-S at 1080p;
FSR, CNN-S at 480p) renders about 30 fps with 0 busy drops on both configs; per-frame GPU time in the pipeline is 7 to 11 ms
on Intel and 3.2 to 9.4 ms on the RTX (low GPU clocks, copy included). On the RTX config the card draws 22 to 27 W
(18.6 W for plain video) because Chrome itself runs on it; on the Intel config the RTX is untouched. At 60 fps even plain
video only reaches about 45 fps in this harness (software compositor), so **60 fps could not be judged**.

Quality against the original 2560x1440 frame (mean PSNR dB / SSIM over 6 images; the clip is the original reduced to
half size, 1080p, 480p, or half size plus H.264 CRF 27 "lrq"):

| input | bilinear | FSR | CNN-S | CNN-M | CNN-M + restore |
|---|---|---|---|---|---|
| half size | 35.00 / 0.930 | 35.71 / 0.939 | 36.42 / 0.946 | 36.58 / 0.945 | 33.57 / 0.927 |
| half size, lossy (lrq) | 33.72 / 0.910 | 34.03 / 0.915 | 34.49 / 0.921 | 34.62 / 0.921 | 32.42 / 0.908 |
| 1080p (1.33x) | 40.12 / 0.973 | 39.23 / 0.961 | 41.00 / 0.975 | 41.12 / 0.975 | 38.54 / 0.968 |
| 480p (3x) | 34.14 / 0.926 | 34.63 / 0.929 | 35.11 / 0.936 | 35.30 / 0.937 | 32.86 / 0.919 |

Compatibility: cross-origin video needs CORS headers plus `crossorigin="anonymous"` (else `importExternalTexture` throws, code
`cross-origin`); same-origin and MSE/blob video, rotated video, same- and cross-origin iframes, picture-in-picture and
fullscreen (video or container) work; an insecure origin has no `navigator.gpu`.

## Known problems

- **Colour:** clips tagged BT.601 or BT.2020 come out up to 70 to 90 levels off (BT.709 within 2). The error is the same for
  bilinear and FSR, so it comes from the `importExternalTexture` path, not the upscaler; not understood yet.
- **Geometry** (contain/cover/fill/rotate/mirror) is not verified: the screenshot PSNR of that test returned NaN in all six scenes.
- **EME:** the ClearKey test timed out (`waitingforkey`); real DRM video cannot be imported anyway.
- Only YouTube has been tried (signed out, scratch profile); fullscreen handling and the canvas position on other players
  (Crunchyroll's player runs in an iframe) are untested.

## Recommendation

The user wants the RTX only (NVDEC + tensor cores), so the Intel adapter is not used by the extension, even though it held
30 fps in every mode tried and would keep the RTX asleep. The in-page WebGPU shaders (default **CNN-S**, FSR as the cheaper
fallback, never the restore variants, which lower the score on natural video) gain only about 1 dB and 0.01 SSIM over
bilinear (Anime4K is tuned for anime) and cannot be combined with NVDEC in Chrome; the NVIDIA VSR daemon described above
is the route that matches the requirement, and mpv and VLC already use that network.

## Running the tests

The GPU must be idle (no compute apps, no Ollama model); no window appears on the desktop.

```sh
webgpu/test/make-media.sh DIR     # test clips from images that ship with the OS, ~25 min, resumable
NVVSR_SCRATCH=DIR webgpu/test/run.sh [--config intel|nvidia] [--seconds N] [--only STR] [--timeout S] SUITE...
# suites: probe selfcheck cost pipeline quality compat present (default: all)
```

`test/server.cpp` (C++) serves the pages and clips and collects results, `test/harness.js` and `test/suites.js` hold the
suites, `test/metrics.sh` computes PSNR/SSIM with ffmpeg. Results are copied to `test/results/<suite>-<config>.{json,md}`.
