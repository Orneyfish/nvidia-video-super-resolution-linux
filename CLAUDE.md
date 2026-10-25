# NVIDIA video super resolution for Linux

AI upscaling (NVIDIA VFX SDK, Video Super Resolution) of video played fullscreen in **mpv, VLC and Chrome**, running on the RTX GPU. Linux has no driver-level RTX VSR, so one shared core is plugged into each player and turns on automatically in fullscreen. Full plan: `~/.claude/plans/i-want-to-implement-composed-bonbon.md`.

```
decoded frame → libnvvsr (YUV→RGBA, NvVFX VSR, RGBA→planar) → display-sized frame
   ▲ VapourSynth plugin (mpv)   ▲ VLC 3 vout wrapper   ▲ Chrome: extension → separate mpv window (REJECTED by the user, see below)
```

## Hardware facts
- Hybrid graphics: Intel Iris Xe drives the panel (`eDP-1`, 2560×1440 @ 240 Hz); RTX 3070 Ti Laptop (Ampere sm_86, **8 GB**) is compute only. Driver 615.78.08.
- Hyprland (Wayland). Distro mpv 0.41.0 (both Arch `extra` and `cachyos-extra-v3`) has **no** `--vf=vapoursynth`: its meson needs a `vapoursynth-script.pc` that VapourSynth R80 no longer ships. We use a private build (`~/.local/opt/mpv-vsr`, via a `.pc` shim); the system mpv is left alone.
- SR engine: `nvidia-vfx` wheel (`--extra-index-url https://pypi.nvidia.com`), VFX SDK 1.3, needs driver ≥570.190.

## Phase 0 outcome (details: `tools/phase0-results.md`)
Gate passed: the core is C++ and dlopens the wheel's libs; Maxine headers are vendored in `core/third_party/maxine/` (MIT). Effect is stateless. SR ms @ 1080p->1440p: Medium 3.6, High 8.2, Ultra 11.5; @720p: High 3.7, Ultra 5.1. Default quality by source height: <=540p Ultra, ~720p High, 1080p Medium/Streaming-Medium. GPU draws ~150 W while running, so `ac_only` matters. Private mpv with VapourSynth: `~/.local/opt/mpv-vsr/bin/mpv` (needs a `libvsscript.so` entry in vapoursynth.toml, see `tools/phase0-results.md`). Run smoke test: `LD_LIBRARY_PATH=<venv>/site-packages/nvvfx/libs:<venv>/site-packages/nvidia_vfx.libs build/vsr_smoke 1280 720 2560 1440 3 100 --state`.

## Phase 1-2 outcome (libnvvsr + VapourSynth plugin; both verified)
- **Core** (`core/`, API frozen in `core/include/nvvsr.h`): whole-frame cost incl. upload/convert/download 1080p->1440p MEDIUM 4.8 ms, HIGH 9.5 ms; 720p HIGH 4.8 ms, ULTRA 6.3 ms. NVIDIA's SDK leaks ~215 MiB GPU per coexisting extra effect, so the core recycles effects: keep context pools tiny. Tests: `NVVSR_LIBDIR=<venv>/.../nvvfx/libs:<venv>/.../nvidia_vfx.libs ctest --test-dir build/core` (17 tests, `oom` needs `NVVSR_TEST_OOM=1`). Sanitizer build: configure with `-fsanitize=address,undefined`; ctest sets the required `ASAN_OPTIONS=protect_shadow_gap=0:replace_intrin=0` itself (without them cuInit fails and GPU tests silently skip) and uses `core/tests/lsan.supp` for NVIDIA-library leaks.
- **Core robustness:** `nvvsr_create` runs the SDK-heavy init on an internal 8 MiB-stack thread, because NVIDIA's NGX init segfaults on thread stacks <= 1 MiB (VLC runs plugins on 1 MiB stacks); regression test `smallstack` (1 MiB / 256 KiB / 128 KiB). 18 ctest in both the normal and the ASan/UBSan build.
- **Plugin** (`vapoursynth/`, `core.nvvsr.Upscale`, `mpv/vsr.vpy`): `vapoursynth/tests/run.sh [--bench] [--mpv]` (104 checks; `--bench` refuses to run while anything else uses the GPU). Default pool is 1: one context saturates the GPU (1080p MEDIUM ~200 fps in vspipe), each extra costs ~250 MiB.
- **mpv facts that shape Phase 3:** `--vf=vapoursynth` only accepts YUV output (so `vsr.vpy` asks the plugin for `format=yuv420p8`); mpv sets `_ColorSpace` (not `_Matrix`) and never `_Transfer`, and serves a black dummy frame if the script reads `video_in` at init -> HDR/interlace exclusion must be done in `vsr.lua`, not in the .vpy. `user-data` only parses in the length-prefixed form `user-data=%N%quality=high;pool=1` (N = byte length). The quality cannot change at runtime: remove and re-add the filter. mpv disables a vapoursynth filter that fails at runtime and keeps playing the original.
- GPU tests/benches: never run two at once, they distort each other (and `ollama` must not hold a model).

## Phase 3 outcome (`mpv/vsr.lua`, verified headless)
- Auto-enables SR when fullscreen + eligible, disables otherwise; options and key bindings documented in `mpv/script-opts/vsr.conf` (defaults: `Ctrl+s` toggle for A/B, `Ctrl+Shift+s` cycle quality, `Alt+s` stats). Pure logic is in table `M` (unit tests: `luajit mpv/tests/test_logic.lua`, 138 checks); integration: `mpv/tests/run.sh [--unit] [--bench] [--only TAG...]` (184 checks, ~5 min, headless `--vo=null`, skips itself if the GPU is busy or an Ollama model is resident).
- Eligibility: not image/albumart/audio-only, height >= 360, picture fitted to the display must be >= 2% larger than the source, SDR only (pq/hlg/bt.2020 excluded), not interlaced (needs one decoded frame, waits for `playback-restart`), `ac_only` (default yes; Mains or online USB supply, polled every 30 s).
- hwdec: while active the user's hwdec is mapped to its `-copy` variant and restored afterwards (also before the next file); `hwdec=keep` leaves it alone (hwdownload path untested on a real VO). 10-bit/4:2:2/4:4:4 sources are converted to 8-bit 4:2:0 by an `@nvvsr-fmt` filter in front (`convert_10bit=yes`); Ampere NVDEC cannot decode H.264 10-bit (mpv falls back to software).
- Failure handling: bogus libdir / missing plugin / failing filter all end in the original video, one "SR failed" log line, no retry loop. Adaptive step-down ladder auto/high -> medium -> low -> bicubic -> off (10 drops in 5 s heuristic), never up within a file.
- Measured (headless, real time, 15 s): 720p30 HIGH and 1080p60 MEDIUM: 0 drops, estimated-vf-fps = container fps; `--untimed` 217 / 211 fps. Enabling stalls playback ~0.3-0.45 s (+0.2 s when hwdec switches), disabling ~0.15 s.
- Verified in a REAL fullscreen window on this Hyprland desktop (2026-10-25, scratch config, `mpv-vsr --fs`): Hyprland reports `fullscreen=2 size=[2560,1440]`, `display-width/height` = 2560x1440, `SR on: 1280x720 -> 2560x1440, quality auto/high`, 0 drops at 30 fps, toggle/quality/stats keys work, leaving fullscreen removes the filter and restores `hwdec=auto`, re-entering brings SR back. Same-frame A/B: PSNR 40.1 dB / SSIM 0.935 vs bicubic (colour correct). Not yet verified: `hwdec=keep`, other players' behaviour with fractional scaling.
- Tuning note: with `hwdec=auto` mpv plays via Vulkan decode; when SR switches to `auto-copy` it first probes a Vulkan copy path (6x `h264: Failed setup for format vulkan` warnings, harmless) before falling back to `nvdec-copy`. `vsr-hwdec=nvdec-copy` skips the probe on NVIDIA machines (Phase 6 can set it in the installed vsr.conf).
- On-screen feedback (user feedback: the first version was "extremely weird, big, unnecessary"): a small semi-transparent corner badge with one or two words ("SR", "SR off", "SR high", "SR failed", "SR off, battery"); no resolutions on screen (they are in the log; `Alt+s` shows stats on demand). Options `osd`, `osd_size` (11), `osd_opacity` (0.55), `osd_position` (9 = top right), `osd_duration` (1.5 s); `Ctrl+Alt+s` toggles it. Automatic switch-offs (e.g. leaving fullscreen) show nothing.
- Gotchas: `Ctrl+s` overrides mpv's default "screenshot window" (input.conf still wins). On Wayland `display-width` is the logical size (scale 1 here). `NVVSR_DISPLAY=WxH` supplies the display size for headless runs.

## Phase 4 outcome (`vlc/`, VLC 3.0.24; verified headless + in a headless `cage` compositor, NOT in a real Qt window)
- A plain VLC 3 video filter cannot enlarge the picture (`ThreadChangeFilters` appends a converter that scales it back to the decoder size, measured with stock croppadd). So the plugin is a **`vout display` wrapper** (`--vout=nvvsr`, or auto-picked with score 300) that opens VLC's real display as a nested display and changes `vd->fmt` at run time (`PICTURES_INVALID` + reset). A `video filter` entry point stays for transcode chains (`#transcode{vfilter=nvvsr{width=..,height=..}}`).
- Fullscreen gating: callback on the vout's `fullscreen` variable (F key, double click, GUI) plus a 50 ms policy poll; pass-through is the plain display (zero-copy). Compositor-only fullscreen (a Hyprland keybind) is not seen; `--no-nvvsr-fullscreen-only` upscales in any window larger than the video. Options `--nvvsr-{width,height,quality,ac-only,fullscreen-only,min-height,min-gain,vout,enable}`.
- Contexts are created on the plugin's own 8 MiB thread (VLC's 1 MiB stacks crashed NGX init); the core now does this itself, the workaround is harmless.
- Tests: `vlc/tests/run.sh [--cage] [--bench]` (157 checks in the agent's run: frames byte-identical to libnvvsr, 720p vs bicubic 34.9 dB, fallbacks, gating, 720p30/1080p60 real time with 0 late/lost, SR ~5 ms/frame, ~420 MiB VRAM). Test harness runs VLC with DISPLAY/WAYLAND_DISPLAY/D-Bus unset: `--vout=dummy` is NOT headless on a desktop session (two early runs flashed a window).
- Limits: subtitles/OSD are upscaled with the video; paused video re-runs SR each ~80 ms redisplay; 10-bit/4:2:2/4:4:4 pass through; a failed probe is cached per process; GPU -> host -> display upload ~5.5 MB per 1440p frame (a device-pointer or GL-interop output would avoid it).

## Phase 5 outcome (`browser/`, Chrome -> mpv handoff: WORKS, but the user REJECTED the design: no separate window; wants SR inside the browser, ideally system/driver-level. See memory user-direction-roadmap)
- MV3 extension (`browser/extension/`, ID pinned by `key`, public key only) + Python-stdlib native host (`browser/host/nvvsr_host.py`, name `com.nvvsr.mpv_handoff`). On fullscreen of a video that would be upscaled (height < screen*dpr, >= 360, no DRM) on an allowlisted site it pauses, exits page fullscreen, sends {url,time,title}; the host starts the private mpv fullscreen at that time (`--script=mpv/vsr.lua`, `--` before the URL, ytdl format capped at `NVVSR_PANEL_H`), and returns the last `time-pos` when mpv exits; the page seeks there. Any host failure restores ordinary Chrome fullscreen. DRM sites cannot be handed off.
- Host security (all tested): http/https only (plain ASCII, no userinfo), URL after `--`, no shell, strict schema, 1 MiB frame limit, time 0..7 days, title cleaned, one handoff at a time (flock), private 0700/0600 socket, redacted rotating log; extension trusts only its own content script; max 1 handoff per 3 s per tab. Known soft spot: the host does not itself allowlist hostnames (the extension does).
- Tests: `browser/tests/run.sh` (53 Python incl. real headless mpv, 37 node). `content.js`/`background.js`/`options.js` are only lint-checked until the first real Chrome run. Host manifest is NOT installed anywhere (Phase 6: `@HOST_PATH@` substitution into `~/.config/google-chrome/NativeMessagingHosts/`, a directory that already holds another extension's manifest - never touch that file).
- Open user decision: the extension asks for optional host permission `*://*/*` (Chrome prompts per site when the user adds one to the allowlist) vs a fixed site list.
- **Real Chrome run on YouTube (2026-10-25, Chrome 156, Hyprland):** the whole chain works (click -> extension -> host -> private mpv at the page's time -> `vsr.lua` -> SR -> quit -> page seeks back, paused). Measured on Sintel (AV1 2048x872, 24 fps): SR on at `auto/medium`, output 2560x1090, 0 drops, GPU ~880 MiB / 12 %; host replied the exact final time (337.25 s). Findings: (1) **YouTube and Vimeo refuse anonymous yt-dlp** ("Sign in to confirm you're not a bot"; other player clients list formats but googlevideo returns 403), so the host needs the user's Chrome cookies: `~/.config/nvvsr/handoff.json` = `{"use_chrome_cookies": true, "cookies_browser": "chrome+gnomekeyring"}` (plain `chrome` decrypts only 260 of 643 cookies because this Chrome runs with `--password-store=gnome-libsecret`; the new `cookies_browser` setting is validated `browser[+keyring]`, env `NVVSR_COOKIES_BROWSER`). User-approved. (2) Hyprland (`on_focus_under_fullscreen = 2`) demoted mpv's `--fullscreen` to a tiled window while Chrome was still leaving fullscreen: the host's `IpcWatcher` now re-asserts fullscreen over the socket (max 4 times in the first 8 s, never after mpv was fullscreen once, off in test mode). (3) YouTube's player resumes playback and may re-enter fullscreen after the extension's single pause/exit: `content.js` now keeps the page paused and windowed while mpv runs and, on failure, restores fullscreen and playback. After editing the extension the user must click reload on chrome://extensions and reload the tab. (4) mpv started with the user's config has `hwdec=no` (software AV1 decode); fine at 24 fps, a host option for `--hwdec=auto-copy-safe` may be wanted for 4K60.
- Installed for this test (user-approved, undo steps in the project memory): `~/.config/google-chrome/NativeMessagingHosts/com.nvvsr.mpv_handoff.json`, `~/.config/nvvsr/{libdir,handoff.json}`, one line in `~/.config/vapoursynth/vapoursynth.toml` (backup `.bak-nvvsr`).

## Browser SR spike (2026-10-25; scripts and details in `tools/spikes/browser-sr/`)
- The user rejected the separate-mpv-window handoff and wants SR inside the browser, ideally system-level. Measured with Chrome 156 on native Wayland in a headless `cage`: Chrome submits only **window-sized** dmabuf buffers (even with HW-decode/overlay-delegation flags the one subsurface carries a full-window buffer), so a compositor/driver hook cannot see source-resolution video -> not viable for Chrome now (HW decode was not confirmed active in that run).
- **WebGPU works in this Chrome under native Wayland and picks the NVIDIA GPU** (`vendor: nvidia, architecture: ampere`). So an in-page route (extension content script on every site: `requestVideoFrameCallback` + `importExternalTexture` -> WebGPU upscale -> canvas over the video, audio untouched) is feasible with no second window and no per-site allowlist, but it runs a WebGPU model, not NVIDIA's VSR network (whose weights live in libnvidia-ngx). NVIDIA's VSR in-page would need a local daemon and ~0.5 GB/s of frame copies.

## Layout
`core/` libnvvsr (C++17, no nvcc for 8-bit: colour conversion via `NvCVImage_Transfer`; `core/include/nvvsr.h` is the **only** interface front-ends may use) · `vapoursynth/` · `mpv/` · `vlc/` · `browser/` · `tools/` (`check_env.py`, `bench_vfx.py`, `compare.sh`, `llm/ask-local`) · `install/`.

## Commands
- Build: `cmake -B build && cmake --build build`; test: `ctest --test-dir build`
- Bench: `.venv/bin/python tools/bench_vfx.py` (unload Ollama first, see below)
- Ask a local model: `tools/llm/ask-local --model qwen2.5-coder:7b-64k "prompt" --file f.c`

## Rules
- Never commit or redistribute NVIDIA SDK/model binaries (they are gitignored).
- Every failure path falls back to pass-through; the player must never crash or stall.
- Confirm with the user before pacman/system changes. Commits are authorised (user, 2026-10-25): per-component commits of verified work, never push, never stage `mempalace.yaml` / `.claude/.mempalace_setup`; git identity is not configured, use `git -c user.name=... -c user.email=...` per commit.
- Per-frame budget at 60 fps is 16.6 ms; target SR ≤ 8 ms at the default quality.

## Model distribution
- **Haiku**: mechanical work (installs, checks, benches, tests, log reading, docs, packaging, simple JS/Python).
- **Sonnet**: real engineering (CUDA/C++ core, VapourSynth plugin, VLC C module, mpv Lua) and orchestration.
- **Opus is off limits.** Only the `architect` agent may use it, and only if (a) the Phase 0 gate fails and the core needs redesign, or (b) a Sonnet agent failed the same bug twice. Ask the user before each use.
- **Local Ollama models** do token-heavy grunt work through `tools/llm/ask-local`; a Claude agent always reviews, compiles or tests their output.

  | Model | Use for |
  |---|---|
  | `qwen2.5-coder:1.5b-64k` | summarise long logs (build, `mpv -v`, `vlc -vv`) |
  | `qwen2.5-coder:7b-64k` | boilerplate (CMake, manifests, module skeletons), first-pass diff review |
  | `qwen2.5-coder:14b-64k` | harder drafts (VS plugin skeleton, CUDA kernel first cuts); slow, partly CPU-offloaded |
  | `gemma4:e4b` | prose (README, docs) |
- **VRAM rule:** Ollama and VSR share the 8 GB GPU. Before any bench/perf test run `ollama ps` and `ollama stop <model>`; never bench while a model is resident.

## Agents (`.claude/agents/`)
| Agent | Model | Owns |
|---|---|---|
| `env-setup` | haiku | Phase 0 installs, venv, `check_env.py` |
| `cuda-core` | sonnet | `core/` |
| `mpv-vapoursynth` | sonnet | `vapoursynth/`, `mpv/` |
| `vlc-plugin` | sonnet | `vlc/` |
| `browser-handoff` | haiku | `browser/` |
| `perf-verifier` | haiku | read-only benches/tests/A-B; enforces VRAM rule |
| `docs-packaging` | haiku | `install/`, README, this file |
| `architect` | opus | escalation only, see above |

Phase → agent: 0 `env-setup` + `perf-verifier` · 1 `cuda-core` · 2–3 `mpv-vapoursynth` · 4 `vlc-plugin` · 5 `browser-handoff` · 6 `docs-packaging` · `perf-verifier` after every phase. Phases 4 and 5 can run in parallel (worktree isolation).
