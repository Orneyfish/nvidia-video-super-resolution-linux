# Phase 0 results (2026-10-25)

Hardware: RTX 3070 Ti Laptop (sm_86, 8 GB), driver 615.78.08, wheel `nvidia-vfx` 0.2.0.0 (VFX SDK 1.3).
Method: `core/tests/vsr_smoke.cpp` (C++, vendored MIT headers + wheel libs, no CUDA toolkit), synthetic
RGBA8 frames, GPU-resident, 100 iterations each, output 2560x1440. "SR" = `NvVFX_Run` sync. "Round trip" =
pinned-host upload + SR + download. Raw numbers: `tools/phase0-bench.txt`.

## Gate: PASSED. The core is C++ (dlopen of the wheel libs); the Python fallback is not needed.

SR ms/frame (60 fps budget is 16.6 ms total; target SR <= 8 ms):

| Source | Low(1) | Medium(2) | High(3) | Ultra(4) | Stream-Med(21) | Stream-Ultra(23) |
|---|---|---|---|---|---|---|
| 854x480   | 1.14 | 1.12 | 2.05 | 2.76 | 1.34 | 3.93 |
| 1280x720  | 1.39 | 1.75 | 3.68 | 5.12 | 2.18 | 7.90 |
| 1920x1080 | 2.74 | 3.56 | **8.16** | 11.50 | 4.63 | 17.92 |

- VRAM: ~653 MiB total GPU use while running 1080p ULTRA. Plenty of room next to Ollama if not resident.
- Whole GPU draws ~150 W at 1395 MHz during the loop: the battery warning in the plan is real. Keep `ac_only`.

## Decisions that follow
- **Auto quality by source height** (60 fps content): <=540p Ultra, 541-800p High, >800p (1080p) Medium or
  Streaming-Medium. High at 1080p is at the budget edge (8.2 ms + conversions), so it is not the default there.
  Adaptive step-down in `vsr.lua` still applies.
- **Stateless**: output of a frame is identical on a long-running effect and a fresh one (`--state`: max diff 0).
  Frames can be processed out of order / in parallel with an effect pool; no ordering constraint.
- Non-integer ratio (1080p -> 1440p, 1.33x) and 3x (480p -> 1440p) are accepted.
- cuDNN stub question is moot: effect creates, loads and runs with only the wheel's bundled libs.
- Input/output must be chunky RGBA8 (or RGB10A2); the core does YUV->RGBA itself.

## Practical notes for the core
- Headers: vendored in `core/third_party/maxine/` (v0.7.6, MIT). They lack `NvVFX_CudaStreamSynchronize`
  (exported by the 1.3 lib; declared locally in the test), the `VideoSuperRes` selector and the
  `QualityLevel` / `ImageEncodingMode` parameter names, which the core defines itself.
- Libraries must be found at run time: set `LD_LIBRARY_PATH` to `<venv>/site-packages/nvvfx/libs:<venv>/site-packages/nvidia_vfx.libs`
  or dlopen by absolute path (use the preload order in `nvvfx/_lib_loader.py`). Load only the VSR chain
  (skip the 218 MB frame-generation lib).

## Still open
- RGB10A2 (10-bit) path: not tested yet.
- Real-content quality check (A/B screenshots) is Phase 3 `tools/compare.sh`.
- System items (user's call): `sudo pacman -S yt-dlp` (Chrome handoff, Phase 5); `ffnvcodec-headers` (optional, restores mpv's native cuda interop in the private build); `cuda` is now OPTIONAL (see below).

## Colour conversion needs no custom CUDA kernels (8-bit)
`NvCVImage_Transfer` (SDK, uses the wheel's bundled NPP) converts on the GPU, CPU<->GPU, chunky or planar:
YUV420 (NV12 or I420) -> RGB/RGBA and RGB/RGBA -> YUV420/RGB, with `colorspace = NVCV_709|NVCV_601 | NVCV_VIDEO_RANGE|NVCV_FULL_RANGE | NVCV_CHROMA_*`. So the 8-bit core is plain C++17 (no nvcc).
A CUDA toolkit is only needed later for 10-bit (P010 / RGB10A2) kernels and `compute-sanitizer`.

## mpv (private build, ~/.local/opt/mpv-vsr/bin/mpv)
- mpv 0.41.0 + `vapoursynth-script.pc` shim: `--vf=vapoursynth` works. Same wayland/vulkan/libplacebo/nvdec list as system mpv; rebuilt 2026-10-25 16:15 against VapourSynth R81 and ffnvcodec-headers 13.1.15: now has `vapoursynth`, `ffnvcodec`, `cuda-hwaccel`, `cuda-interop`. Rebuild recipe: meson setup with `PKG_CONFIG_PATH=<scratch>/pcshim` (a `vapoursynth-script.pc` shim: Requires vapoursynth, Libs -lvapoursynth-script) and `-Dvapoursynth=enabled`, prefix `~/.local/opt/mpv-vsr` plus Arch's package flags (`-Dlibmpv=true -Dgl-x11=enabled -Dcaca=disabled -Dcdda=enabled -Ddrm=enabled -Ddvbin=enabled -Ddvdnav=enabled -Dlibarchive=enabled -Dopenal=enabled -Dsdl2-audio=enabled -Dsdl2-video=enabled -Dsdl2-gamepad=enabled`, `-Dbuildtype=release`); Phase 6 should turn this into `install/build-mpv-vsr.sh`. `nvdec-copy` (what SR uses) does not need ffnvcodec.
- mpv dlopens `/usr/lib/libvsscript.so` and looks that key up in `$XDG_CONFIG_HOME/vapoursynth/vapoursynth.toml`; the file created by `vapoursynth config` has only the python-package key, so mpv fails with "Could not initialize VapourSynth scripting". Fix = add the line `"/usr/lib/libvsscript.so" = ["/usr/bin/python","/usr/lib/libpython3.14.so.1.0","<same timestamp as other entry>"]`. NOT applied to ~/.config yet; `install.sh` (Phase 6) must do it with the user's knowledge. For tests use a scratch `XDG_CONFIG_HOME` together with `mpv --no-config`.
- mpv's vapoursynth filter requires the .vpy to read `video_in` (a BlankClip script aborts: missing `_MP_IMAGE`).
- Note: `~/.config/vapoursynth/vapoursynth.toml` (python key only) was created on 2026-10-25 15:14 during the mpv build step.
