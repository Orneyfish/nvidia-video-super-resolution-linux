# NV Control

A GTK4 + libadwaita app (C++17, binary `nvcontrol`, app id `org.nvvsr.Control`) for the video super resolution project: it shows the state of the NVIDIA GPU, checks that SR works, and edits the mpv and VLC SR settings. Colours follow `~/.local/state/caelestia/scheme.json` when it exists.

Needs gtk4, libadwaita, json-glib, cmake and a C++17 compiler; the tests also need `cage` and `jq`.

## Pages

- **Overview**: GPU name, driver, PCI address, a state chip (Awake / Asleep / Stale / No GPU), tiles for memory, utilisation, temperature and power, detail rows and the compute-process list. "Refresh every" (Off / 1 / 2 / 5 / 10 s) is saved in `panel.json`.
- **Super Resolution**: install status with a fix hint per missing part, a "Check SR works" button (runs `nvvsr_probe`, only when pressed), and editors for mpv's `vsr.conf` and VLC's `vlcrc` with a live preview of the on-screen badge. The editors keep comments, write `<file>.bak-nvvsr` before the first change, and the VLC editor refuses to write while VLC runs (VLC rewrites `vlcrc` on exit).
- **Power**: runtime power state of the dGPU, AC or battery, `supergfxctl` mode.
- **About**: versions (panel, libnvvsr, NVIDIA wheel, driver, mpv, VLC, GTK, libadwaita) and a copyable diagnostics report with the home directory shortened to `~`.

## Why it does not wake the GPU

On a hybrid-graphics laptop every `nvidia-smi` call wakes the RTX GPU. The panel reads the sysfs `runtime_status` first and polls `nvidia-smi` only while the window is visible, the Overview page is shown and the GPU is already awake. While it sleeps, the Overview shows a card with a "Wake and read" button (one query per press).

## Build and test

```sh
cmake -S panel -B build/panel -DCMAKE_BUILD_TYPE=Release && cmake --build build/panel -j
ctest --test-dir build/panel        # 13 programs, no GPU
```

Options: `-DNVC_BUILD_APP=OFF` builds only the GTK-free library and its tests; `-DNVC_SANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer.

## Headless UI tests

No window opens on the desktop and the real GPU is never queried. `tests/run_cage.sh [--home DIR] -- COMMAND...` runs a command in a headless `cage` compositor with a scratch HOME; `tests/fake_env.sh [--runtime active|suspended] [--ac 0|1] [--vlc-running] [--calls FILE] [--home DIR] -- COMMAND...` adds a fake sysfs and fake `nvidia-smi`, `supergfxctl`, `pgrep` and `kitty`, and logs their calls.

```sh
for s in poll about power sr overview; do panel/tests/smoke_$s.sh build/panel/nvcontrol; done
panel/tests/smoke_editor.sh build/panel/editor_harness
```

Each script prints how many checks passed and failed. `nvcontrol --smoke SECONDS RESULT_JSON` is the hook the smoke scripts use: it drives every page and writes a JSON summary.

## Launcher

```sh
panel/install-launcher.sh [--uninstall] [--home DIR]
```

Installs `~/.local/share/applications/org.nvvsr.Control.desktop` and its icon; it touches nothing outside `~/.local/share`. `install/install.sh --with-panel` calls it.
