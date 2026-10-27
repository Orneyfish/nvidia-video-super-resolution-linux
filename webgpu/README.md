# webgpu/ — in-page video upscaler (prototype)

Upscales a `<video>` inside the page with WebGPU, no second window and no change to the player: `VideoUpscaler` in
`src/upscaler.js` follows `video.requestVideoFrameCallback`, imports each frame with `importExternalTexture`, upscales it
with plain render passes and draws it on a `<canvas>` placed over the video. Audio is untouched; no build step, no network.

It is **not** NVIDIA's VSR network (that lives in libnvidia-ngx and cannot run in a page). It runs open models:
bilinear (what the browser does anyway), AMD FSR 1.0 (EASU + RCAS, MIT) and Anime4K CNN in sizes S and M with an optional
"restore" pass (MIT); licences in `LICENSE-FSR.txt` and `LICENSE-Anime4K.txt`, weights extracted by
`tools/extract-anime4k.cpp` into `src/models/`. It exists because Chrome on Linux hands the compositor window-sized
buffers only, so no driver or compositor hook can see the source-resolution video (`tools/spikes/browser-sr/`); an
extension content script can reach the `<video>` on any site. **There is no extension wrapper yet** (find the video,
create the canvas, call `VideoUpscaler`, fall back to the plain video on any `UpscalerError`).

```js
const up = await VideoUpscaler.create(video, canvas, { mode: 'cnn', cnnSize: 'S' });  // 'bilinear' | 'fsr' | 'cnn'
up.start(); up.stats(); up.setMode('fsr'); up.destroy();
```

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
- Nothing has run on a real site yet.

## Recommendation

On this hybrid laptop use the default (Intel) path: it keeps the RTX asleep and holds 30 fps in every mode tried. Default
mode **CNN-S** (best quality per millisecond), FSR as the cheaper fallback, never the restore variants (they lower the
score on natural video), CNN-M only when there is headroom. Expect a modest gain, about 1 dB and 0.01 SSIM over bilinear:
Anime4K is tuned for anime. This is the fallback for sites; mpv and VLC use NVIDIA's far stronger network.

## Running the tests

The GPU must be idle (no compute apps, no Ollama model); no window appears on the desktop.

```sh
webgpu/test/make-media.sh DIR     # test clips from images that ship with the OS, ~25 min, resumable
NVVSR_SCRATCH=DIR webgpu/test/run.sh [--config intel|nvidia] [--seconds N] [--only STR] [--timeout S] SUITE...
# suites: probe selfcheck cost pipeline quality compat present (default: all)
```

`test/server.cpp` (C++) serves the pages and clips and collects results, `test/harness.js` and `test/suites.js` hold the
suites, `test/metrics.sh` computes PSNR/SSIM with ffmpeg. Results are copied to `test/results/<suite>-<config>.{json,md}`.
