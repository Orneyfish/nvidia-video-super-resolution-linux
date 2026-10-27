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

So the plan that fits is: Chrome decodes on NVDEC with the flags above and shows a plain canvas; a local C++ daemon runs
libnvvsr on the RTX; the extension sends each decoded frame as I420 (`VideoFrame.copyTo`, 3.1 MB at 1080p) over a localhost
WebSocket and draws the upscaled I420 reply (5.5 MB at 1440p) from a `VideoFrame`: about 260 MB/s at 30 fps. Open risks:
the page's CSP for WebSockets from a content script, the cost of reading a VA-API frame back, and the 1 to 2 frame lag
behind the audio.

`test/ext_drive.cpp` (C++) drives Chrome through `--remote-debugging-pipe`: `--load-extension` is gone in Chrome 156 and
remote debugging is refused on the default profile directory, so it uses `Extensions.loadUnpacked` on a scratch profile:
`ext_drive CHROME webgpu/extension URL OUT_DIR WAIT_LOAD_S WAIT_FS_S --user-data-dir=DIR --ignore-gpu-blocklist ...`; it opens one
window, presses `f`, and writes the page state and two screenshots (canvas on and hidden) to OUT_DIR.

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
