# NVIDIA video super resolution for Linux

AI upscaling (NVIDIA VFX SDK, Video Super Resolution) of video played fullscreen in **mpv, VLC and (planned) Chrome**, running on the RTX GPU. Linux has no driver-level RTX VSR, so one shared core is plugged into each player and turns on automatically in fullscreen. Full plan: `~/.claude/plans/i-want-to-implement-composed-bonbon.md`.

```
decoded frame → libnvvsr (YUV→RGBA, NvVFX VSR, RGBA→planar) → display-sized frame
   ▲ VapourSynth plugin (mpv)   ▲ VLC 3 vout wrapper   ▲ Chrome: in-page WebGPU extension (prototype in webgpu/)
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

## Removed: Chrome -> mpv handoff (was `browser/`, removed 2026-10-26)
- It worked end to end (YouTube, Chrome 156) but the user rejected the design (a separate mpv window) and the Python rule removed its host. Code and findings are in git history (`49d2488` and the commit before this note). Lessons kept: YouTube/Vimeo refuse anonymous yt-dlp (cookies needed); Hyprland (`on_focus_under_fullscreen = 2`) demotes a new window's `--fullscreen` while another window leaves fullscreen; players such as YouTube's resume playback after a single `pause()`. The handoff's home files (host manifest, `handoff.json`, log) were deleted; the other extension's manifest in `~/.config/google-chrome/NativeMessagingHosts/` was never touched.

## Browser SR spike (2026-10-25; scripts and details in `tools/spikes/browser-sr/`)
- The user rejected the separate-mpv-window handoff and wants SR inside the browser, ideally system-level. Measured with Chrome 156 on native Wayland in a headless `cage`: Chrome submits only **window-sized** dmabuf buffers (even with HW-decode/overlay-delegation flags the one subsurface carries a full-window buffer), so a compositor/driver hook cannot see source-resolution video -> not viable for Chrome now (HW decode was not confirmed active in that run).
- **WebGPU works in this Chrome under native Wayland and picks the NVIDIA GPU** (`vendor: nvidia, architecture: ampere`). So an in-page route (extension content script on every site: `requestVideoFrameCallback` + `importExternalTexture` -> WebGPU upscale -> canvas over the video, audio untouched) is feasible with no second window and no per-site allowlist, but it runs a WebGPU model, not NVIDIA's VSR network (whose weights live in libnvidia-ngx). NVIDIA's VSR in-page would need a local daemon and ~0.5 GB/s of frame copies.

## Layout
`core/` libnvvsr (C++17, no nvcc for 8-bit: colour conversion via `NvCVImage_Transfer`; `core/include/nvvsr.h` is the **only** interface front-ends may use) · `vapoursynth/` · `mpv/` · `vlc/` · `webgpu/` (in-page upscaler prototype) · `tools/` (`libdir.sh`, `llm/ask-local`, `spikes/`) · `third_party/nvidia-vfx/` (unzipped wheel libraries, gitignored) · `install/` (user-level installer, `build-mpv-vsr.sh`, tests) · `panel/` (GTK control panel) · `docs/install-layout.md` (prefix layout + install.json schema).

## Commands
- Build: `cmake -B build && cmake --build build`; test: `ctest --test-dir build`
- Libraries for tests and benches: `export NVVSR_LIBDIR=$(tools/libdir.sh)` (the unzipped nvidia-vfx wheel; unload Ollama before any GPU test, see below)
- Ask a local model: `tools/llm/ask-local --model qwen2.5-coder:7b-64k "prompt" --file f.c`
- Install / remove (user level, no sudo; see README.md): `install/install.sh check|install|uninstall|status [--dry-run] [--yes]`; installer tests (GPU-free): `install/tests/run.sh`

## Rules
- **No Python (user rule, 2026-10-26): all project code is C or C++** (shell for glue; JS/WGSL only where the browser forces it; Lua for mpv's script until replaced). That includes tests, tools and one-liners: no `python3 -c`, no pip, no venv. Two unavoidable third-party exceptions: the NVIDIA libraries ship only inside the `nvidia-vfx` Python wheel (we only unzip it) and mpv's video-filter hook is VapourSynth, which needs a tiny `.vpy` stub (all logic lives in the C++ plugin). Put this rule in every agent brief.
- Never commit or redistribute NVIDIA SDK/model binaries (they are gitignored, incl. `third_party/nvidia-vfx/`).
- Every failure path falls back to pass-through; the player must never crash or stall.
- Confirm with the user before pacman/system changes. Commits are authorised (user, 2026-10-25): per-component commits of verified work, never push, never stage `mempalace.yaml` / `.claude/.mempalace_setup`; git identity is not configured, use `git -c user.name=... -c user.email=...` per commit.
- Per-frame budget at 60 fps is 16.6 ms; target SR ≤ 8 ms at the default quality.

## Coding style (ponytail)

Rules adapted from ponytail (https://github.com/dietrichgebert/ponytail, MIT, AGENTS.md): write the smallest change that fully solves the task.

- Before writing, read the task and the code it touches. List every place the change must reach (callers, tests, fixtures, config, exports) and what it could break for users. That is scope; extra features are not.
- Take the first option that fully works: (1) does it need to exist? skip unrequested features and options and name them in one line; (2) already in this codebase? use it the way the surrounding code does; (3) standard library or platform feature; (4) an installed dependency (never add one for a few lines); (5) one line a reader gets at a glance; (6) otherwise the minimum code that works.
- Be lazy about the solution, never about the change: finish every part the task needs, including the callers, tests and fixtures it breaks.
- No abstraction, wrapper, option, config or "for later" code nobody asked for. Deletion beats addition. Keep the existing structure and conventions. The shortest working diff wins once you know everything it must touch.
- Comment only the why the code cannot show, in one line. A known shortcut gets `shortcut: <the limit>, <when to upgrade>`.
- Bug fix: grep every caller of the function you touch, then fix the root cause once in shared code. Code you move or merge keeps its error handling and validation.
- New non-trivial logic (a branch, a loop, a parser, money, security, a whole new script or app) leaves one small test or an assert-based self-check.
- Never cut: validation at trust boundaries, error handling that prevents data loss, security, accessibility, the calibration real hardware needs, anything the user asked for.
- End each reply with one or two lines: what you skipped or did not check, and any risk.
- Background agents get these rules in their brief.

## Model distribution
- **Haiku**: mechanical work (installs, checks, benches, tests, log reading, docs, packaging, simple JS and shell).
- **Sonnet**: real engineering (CUDA/C++ core, VapourSynth plugin, mpv Lua) and orchestration. VLC upkeep is Haiku; a genuinely hard VLC change can be given Sonnet for that one spawn.
- **Opus is off limits.** Only the `architect` agent may use it, and only if (a) the Phase 0 gate fails and the core needs redesign, or (b) a Sonnet agent failed the same bug twice. Ask the user before each use.
- **Local Ollama models** do token-heavy grunt work through `tools/llm/ask-local`; a Claude agent always reviews, compiles or tests their output.

  | Model | Use for |
  |---|---|
  | `qwen2.5-coder:1.5b-64k` | summarise long logs (build, `mpv -v`, `vlc -vv`) |
  | `qwen2.5-coder:7b-64k` | boilerplate (CMake, manifests, module skeletons), first-pass diff review |
  | `qwen2.5-coder:14b-64k` | harder drafts (VS plugin skeleton, CUDA kernel first cuts); slow, partly CPU-offloaded |
  | `gemma4:e4b` | prose (README, docs) |
- **VRAM rule:** Ollama and VSR share the 8 GB GPU. Before any bench/perf test run `ollama ps` and `ollama stop <model>`; never bench while a model is resident.

## Task sizing (user rule, 2026-10-27: small tasks for each subagent)
Every subagent brief is one small task:
- **One deliverable** in named files; touch nothing else. One agent per directory/file at a time.
- **At most ~300 lines** of new code or one test group. Bigger work is split into a chain of tasks, each leaving the tree building and its tests passing.
- **One verify command** that says when it is done (name it, with the expected count). Done means that command passes.
- **Stop rule:** the same problem fails twice -> stop and report. No scope growth.
- **Report <= 10 lines:** files changed, verify result with counts, what was skipped, risk.
- **Model:** Haiku by default; Sonnet for real engineering (GTK window, config editor, plugin C++). No GPU unless the task says so; functional GPU runs may overlap, timing runs never (one at a time, Ollama idle).
- Start the brief with: "Read CLAUDE.md (Rules, Coding style, Task sizing). No Python, not even one-liners. No commits, no sudo, real home untouched (scratch HOME under the scratchpad)."

## Agents (`.claude/agents/`)
| Agent | Model | Owns |
|---|---|---|
| `cuda-core` | sonnet | `core/` |
| `mpv-vapoursynth` | sonnet | `vapoursynth/`, `mpv/` |
| `vlc-plugin` | haiku | `vlc/` |
| `perf-verifier` | haiku | read-only benches/tests/A-B; enforces VRAM rule |
| `docs-packaging` | haiku | `install/`, README, this file |
| `architect` | opus | escalation only, see above |

Agents are for recurring roles; most tasks are one-off briefs to a general-purpose agent following Task sizing. `docs-packaging` owns `install/`, README and this file.
