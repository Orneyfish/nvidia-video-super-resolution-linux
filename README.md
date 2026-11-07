# NVIDIA video super resolution for Linux

AI upscaling of video with NVIDIA's VFX SDK (the engine behind RTX Video Super Resolution), run on the RTX GPU. Linux has
no driver-level RTX VSR, so one small core (`libnvvsr`) is plugged into each player and switches on by itself when the
video is fullscreen and smaller than the screen.

```
decoded frame -> libnvvsr (YUV->RGBA, NvVFX VSR, RGBA->YUV) -> display-sized frame
   mpv: VapourSynth plugin + vsr.lua      VLC 3: vout display wrapper
```

## Status

| Player | State | Limits |
|---|---|---|
| **mpv** (`mpv-sr`) | Works. Tested headless and in a real fullscreen window on Hyprland. | Needs a private mpv with `--vf=vapoursynth` (Arch's mpv is built without it; `install/build-mpv-vsr.sh` builds one, your mpv is left alone). SDR 8-bit 4:2:0 only; 10-bit/4:2:2/4:4:4 are converted to 8-bit first. HDR, interlaced and rotated video are not touched. |
| **VLC 3.0** (`vlc-sr`) | Works. Tested headless, in a headless `cage` compositor and in a real Qt window on Hyprland (fullscreen on/off through VLC's own fullscreen switches it on and off). | VLC 3 has no per-user plugin directory: use `vlc-sr`, which sets `VLC_PLUGIN_PATH`. Fullscreen means VLC's own fullscreen (F, double click), not a compositor keybind. Subtitles are upscaled with the picture. 10-bit passes through. |
| **Chrome** | **No in-browser super resolution yet.** Compositor/driver-level SR is not possible for Chrome (it submits window-sized buffers); an in-page approach is being evaluated. | The legacy Chrome handoff (`browser/`, `--with-chrome-handoff`) was removed: its design was rejected. |
| **Control panel** (`panel/`) | NV Control, a GTK4/libadwaita app (C++): GPU state, "does SR work" check, mpv and VLC settings, power. `--with-panel` builds it and installs `nvcontrol` and a launcher; see `panel/README.md`. | Never queries the GPU while it sleeps (battery). |

Gains are largest for 480p-720p sources on a 1440p panel. Per frame, 1080p to 1440p costs about 5 ms (Medium) to 9.5 ms
(High); 720p High 4.8 ms, Ultra 6.3 ms. `auto` quality: up to 540p Ultra, about 720p High, 1080p Medium. If the GPU cannot
keep up, quality steps down by itself (High, Medium, Low, bicubic, off).

### Hybrid graphics and battery

On a laptop like the development machine (Intel Iris Xe drives the panel, the RTX is compute only) every frame crosses the
PCIe bus to the RTX and back, and the RTX wakes up. It draws about 150 W while working, so super resolution runs **on AC
power only** by default (`ac_only`, mains and USB-C PD count as AC). It also shares its 8 GB with anything else on the
GPU: a resident Ollama model (`ollama ps`, `ollama stop <model>`) will starve it.

## Requirements

Linux x86_64 (developed on CachyOS/Hyprland; the installer prints Arch package names), an NVIDIA RTX GPU with driver
>= 570.190, cmake >= 3.20, a C/C++ compiler with make, pkgconf, curl, unzip, sha256sum, jq, VapourSynth (headers),
VLC 3.0 with its headers (Arch's `vlc` package), ffmpeg recommended, about 1.5 GB of disk, and network for the
443 MB `nvidia-vfx` wheel from `pypi.nvidia.com` (pinned to a sha256, refused if it differs; NVIDIA's licence applies
to it, this repository does not contain it). Building the private mpv needs meson, ninja and mpv's build libraries
(`install.sh check` lists what is missing).

## Install

```sh
install/install.sh check                      # what is present/missing; prints the sudo pacman -S line, never runs it
install/install.sh install --dry-run          # the plan: every file, backup and line that would change
install/install.sh install                    # lists the changes, asks, then builds and installs
install/install.sh install --yes --build-mpv  # no prompts; also builds the private mpv (10+ minutes) if it is missing
install/install.sh status
install/install.sh uninstall                  # removes exactly what install.json lists, restores backups (same --prefix/--home)
```

Options: `--prefix DIR`, `--home DIR`, `--no-mpv`, `--no-vlc`, `--skip-wheel`, `--mpv-bin PATH`, `--build-mpv`,
`--with-panel`, `--with-chrome`, `--purge-mpv` (uninstall). `--with-chrome` installs the Chrome upscaling daemon as a systemd user service; see webgpu/README.md. Without `--yes`
and without a terminal the installer refuses. It never uses sudo or pacman, writes nothing outside your home, backs up
every existing file it changes as `<file>.bak-nvvsr-<time>`, and a second run changes nothing. What goes where, and the
`install.json` schema: [docs/install-layout.md](docs/install-layout.md).

Afterwards: `mpv-sr video.mkv` and `vlc-sr video.mkv` (put `~/.local/bin` on your `PATH`); your normal `mpv` and `vlc`
are untouched. Toggle fullscreen and super resolution switches on by itself.

## Chrome and Crunchyroll on the NVIDIA GPU

**Crunchyroll** (the castlabs Electron desktop app): `tools/nvcrunchyroll` (experimental, installed nowhere, no launcher uses it)
starts it with its GPU process on the RTX through Vulkan (flags in `tools/electron-castlab-flags.conf`, to be copied to
`~/.config/electron-castlab-flags.conf`); it renders correctly. Its video is Widevine-protected, so decoding stays in the CDM
and its frames cannot be read: no upscaling is possible for it. A client rendering on the NVIDIA GPU crashed Hyprland 0.57.0 on
2026-10-30, so try this launcher only inside a headless `cage`, never on the live compositor. On the development machine
Super+Alt+C is back on plain `crunchyroll` and the flags file is set aside (`*.bak-nvvsr-<time>`).

**Chrome: not solved.** `tools/nvchrome` (experimental, not installed, no launcher uses it) gets NVDEC decoding through VA-API
(`libva-nvidia-driver`; decoder engine 5-11 % on 1080p60) but Chrome then cannot import the decoded frames into its Intel-GL
compositor and video of 720p or more renders white with continuous repaints. With Chrome's EGL on the RTX the picture is
correct but Chrome turns VA-API off (software decode). Details and the plan for upscaling in Chrome (the `nvvsr_webd` daemon,
`webgpu/daemon/`): `webgpu/README.md`.

## Using it (mpv)

| Key | Action |
|---|---|
| `Ctrl+s` | super resolution on/off for this file (A/B compare) |
| `Ctrl+Shift+s` | cycle quality: auto, ultra, high, medium, low, bicubic |
| `Alt+s` | stats overlay (state, quality, sizes, drops, power) |
| `Ctrl+Alt+s` | the corner badge on/off |

The only on-screen feedback is a small, semi-transparent word in a corner ("SR", "SR off", "SR failed", "SR off, battery").
Options live in `~/.config/mpv/script-opts/vsr.conf` (created from the commented template, never overwritten):
`osd=no` removes the badge, `osd_size`, `osd_opacity`, `osd_position` (numpad layout, 9 = top right), `osd_duration`;
`quality`, `ac_only`, `min_height`, `hwdec` and the rest are described there. VLC takes `--nvvsr-quality`,
`--nvvsr-ac-only`, `--no-nvvsr-fullscreen-only` and friends (`vlc/README.md`).

## Troubleshooting

- **"Could not initialize VapourSynth scripting"** (mpv): `vapoursynth.toml` needs an entry for `libvsscript.so`. The
  installer appends it; `install.sh status` says if it is missing. After a libpython upgrade its timestamp is stale: delete
  that line from `~/.config/vapoursynth/vapoursynth.toml` and re-run `install`.
- **SR failed / nothing happens**: `~/.config/nvvsr/libdir` must name the wheel's two library directories (or set
  `NVVSR_LIBDIR`). Run `mpv-sr -v video.mkv 2>&1 | grep -i vsr` and look at the first "SR failed" line.
- **Out of memory / slow**: `ollama ps`; stop any model (see above). One context costs about 250 MiB; `pool=1` is enough.
- **`h264: Failed setup for format vulkan` x6** at the moment SR switches on: harmless, mpv probes a Vulkan copy path
  before using `nvdec-copy`. `hwdec=nvdec-copy` in `vsr.conf` skips the probe on NVIDIA.
- **VLC shows no effect**: start it with `vlc-sr`; a plain `vlc` cannot see the module. To make every VLC start use it, put
  `VLC_PLUGIN_PATH=~/.local/share/vlc/plugins` in your session environment.

## Tests

All are headless; only the first needs no GPU.

```sh
install/tests/run.sh                   # installer, uninstaller, install.json, RUNPATH, VLC finds the module (no GPU)
NVVSR_LIBDIR=... ctest --test-dir build/core   # core (GPU)
NVVSR_LIBDIR=... ctest --test-dir vapoursynth/build   # plugin (GPU)
mpv/tests/run.sh [--unit]              # mpv script: unit tests, then the headless mpv groups (--unit needs no GPU;
                                       #   --bench prints throughput; --only GROUP... picks groups)
vlc/tests/run.sh                       # VLC module (GPU)
ctest --test-dir build/panel           # control panel (no GPU); panel/tests/smoke_*.sh are the headless UI tests
```

Layout: `core/` libnvvsr (the only interface is `core/include/nvvsr.h`), `vapoursynth/`, `mpv/`, `vlc/`, `webgpu/`
(in-page upscaler prototype for browsers), `panel/`, `install/`, `tools/`. Project notes and measured facts: `CLAUDE.md`, `tools/phase0-results.md`.
