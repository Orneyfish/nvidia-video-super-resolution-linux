# Install layout and `install.json`

What `install/install.sh` puts where, and the manifest that records it. Everything is under the user's home; nothing is
written under `/usr` and nothing is run with sudo. `<data>` is `$XDG_DATA_HOME` (default `~/.local/share`), `<config>` is
`$XDG_CONFIG_HOME` (default `~/.config`). `--home DIR` makes `DIR` the home and derives both from it.

## The prefix `<data>/nvvsr/` (override: `--prefix DIR`)

| Path | Content |
|---|---|
| `bin/nvvsr_probe` | GPU probe (core) |
| `bin/nvcontrol` | the control panel, `--with-panel` only (built into `<repo>/build/panel`, copied here) |
| `bin/nvvsr_webd` | the Chrome upscaling daemon, `--with-chrome` only (built into `<repo>/build/webd`, copied here) |
| `lib/libnvvsr.so.0.1.0`, `libnvvsr.so.0`, `libnvvsr.so` | the core (the two names are symlinks) |
| `lib/libvsnvvsr.so` | VapourSynth plugin `core.nvvsr` (mpv) |
| `lib/libnvvsr_plugin.so` | VLC 3 module (the copy VLC loads is in the VLC plugin directory, below) |
| `mpv/vsr.lua`, `mpv/vsr.vpy`, `mpv/script-opts/vsr.conf` | the mpv script, its VapourSynth script, the commented option template |
| `vfx/` | the `nvidia-vfx` wheel unzipped, only `nvvfx/libs/*` and `nvidia_vfx.libs/*` (no Python involved) |
| `build/` | CMake build trees (`core`, `vapoursynth`, `vlc`) and `logs/`; rebuilt only when a source is newer (the panel builds in `<repo>/build/panel`) |
| `install.json` | the manifest below |

`libvsnvvsr.so` and `libnvvsr_plugin.so` are linked with RUNPATH `<prefix>/lib:$ORIGIN`, so `libnvvsr.so.0` is found
in the prefix wherever the file is copied to (the installer checks it with `ldd`).

## Outside the prefix

| Path | What | If it already exists |
|---|---|---|
| `<config>/nvvsr/libdir` | the two wheel library directories, colon separated (read by libnvvsr; env `NVVSR_LIBDIR` wins) | backed up, replaced |
| `<config>/mpv/script-opts/vsr.conf` | copy of the option template, **only if absent** (the panel edits it later) | left alone |
| `<config>/vapoursynth/vapoursynth.toml` | one appended line, `"<libvsscript.so>" = ["<python>","<libpython>","<mtime of libpython>"]`; python and libpython are copied from an existing entry, otherwise taken from `sysconfig` | backed up; an existing entry for that key is never touched, no other line is rewritten |
| `~/.local/bin/mpv-sr` | `exec <private mpv> --script=<prefix>/mpv/vsr.lua "$@"` (does not shadow `mpv`) | backed up, replaced |
| `~/.local/bin/vlc-sr` | sets `VLC_PLUGIN_PATH=<data>/vlc/plugins` and execs `vlc` (does not shadow `vlc`) | backed up, replaced |
| `<data>/vlc/plugins/video_output/libnvvsr_plugin.so` | the VLC module | backed up, replaced |
| `~/.local/share/applications/org.nvvsr.Control.desktop` (+ icon in `~/.local/share/icons/hicolor/scalable/apps/`) | the panel's launcher, `Exec=<prefix>/bin/nvcontrol`; `--with-panel` only, written by `panel/install-launcher.sh --bin --home` | replaced |
| `<config>/systemd/user/nvvsr-webd.service` | the daemon's systemd user unit, `ExecStart=<prefix>/bin/nvvsr_webd 41777 --origin https://www.youtube.com`; `--with-chrome` only. Installing runs `systemctl --user daemon-reload` and `enable --now nvvsr-webd.service`; if that fails the component is still installed, with a warning and the two commands to run | replaced |

The legacy Chrome handoff was removed from the installer; it never writes under `<config>/google-chrome`.

The private mpv (`~/.local/opt/mpv-vsr/bin/mpv`) is detected (`--vf=help` must list `vapoursynth`) or built by
`install/build-mpv-vsr.sh` (`install.sh install --build-mpv`); it is outside the manifest's file list and is only
removed by `uninstall --purge-mpv`.

VLC 3.0.24 has **no per-user plugin directory**: it scans `/usr/lib/vlc/plugins` and `$VLC_PLUGIN_PATH` only (checked: a
module in `~/.local/share/vlc/plugins` is not listed). The module still goes to the conventional user directory, and
`vlc-sr` adds that directory through `VLC_PLUGIN_PATH`.

Backups are `<file>.bak-nvvsr-<UTC yyyymmddThhmmss>` next to the file, made with `cp -a` before the file is first
changed.

## `install.json` (schema 1)

Written atomically after every step (so a failed run can still be uninstalled) and left untouched when a run changes
nothing. Readers must ignore unknown keys.

```json
{
  "schema": 1,
  "version": "0.1.0",                     // libnvvsr version (core/CMakeLists.txt)
  "installed_at": "2026-10-26T09:15:02+00:00",
  "updated_at": "2026-10-26T09:15:02+00:00",
  "git_commit": "c9ef64f...",             // or null
  "git_dirty": false,                     // tracked files differed from HEAD
  "repo": "/home/u/Projects/nvidia-video-super-resolution",
  "prefix": "/home/u/.local/share/nvvsr",
  "home": "/home/u",
  "config_home": "/home/u/.config",
  "data_home": "/home/u/.local/share",
  "nvvsr_libdir": "<prefix>/vfx/nvvfx/libs:<prefix>/vfx/nvidia_vfx.libs",  // or null
  "wheel_version": "0.2.0.0",              // read from the wheel's METADATA; the panel shows it; or null
  "paths": {                               // absolute; keys that do not apply are absent
    "lib": "<prefix>/lib", "libnvvsr": "<prefix>/lib/libnvvsr.so.0", "nvvsr_probe": "<prefix>/bin/nvvsr_probe", "panel": "<prefix>/bin/nvcontrol",
    "vapoursynth_plugin": "<prefix>/lib/libvsnvvsr.so", "vlc_plugin": "<data>/vlc/plugins/video_output/libnvvsr_plugin.so",
    "vlc_plugin_dir": "<data>/vlc/plugins", "mpv_dir": "<prefix>/mpv", "mpv_script": "<prefix>/mpv/vsr.lua",
    "mpv_wrapper": "~/.local/bin/mpv-sr", "vlc_wrapper": "~/.local/bin/vlc-sr", "private_mpv": "~/.local/opt/mpv-vsr/bin/mpv",
    "vsr_conf": "<config>/mpv/script-opts/vsr.conf", "libdir_file": "<config>/nvvsr/libdir", "vfx": "<prefix>/vfx",
    "vapoursynth_toml": "<config>/vapoursynth/vapoursynth.toml", "webd": "<prefix>/bin/nvvsr_webd", "webd_unit": "<config>/systemd/user/nvvsr-webd.service", "install_json": "<prefix>/install.json"
  },
  "private_mpv": { "path": "...", "vapoursynth": true, "built_by_installer": false },
  "wheel": { "name": "nvidia-vfx", "sha256": "0252866a...", "installed": true },
  "components": {                          // status: installed | partial | skipped | failed
    "core": {"status": "installed", "detail": "..."}, "vapoursynth": {...}, "mpv": {...}, "vlc": {...},
    "wheel": {...}, "panel": {...},
    "webd": {"status": "installed", "detail": "nvvsr_webd <prefix>/bin/nvvsr_webd; unit <config>/systemd/user/nvvsr-webd.service; service_enabled=true"}
  },
  "entries": [                             // everything created or changed, in order; uninstall walks it backwards
    { "type": "dir|file|symlink|line|tree", "path": "/abs", "component": "mpv",
      "created": true,                     // false: the path existed before the installer
      "backup": null,                      // absolute path of the pre-install copy, if one was made
      "sha256": "..." ,                    // file/line: hash of the file as the installer left it (null otherwise)
      "target": "libnvvsr.so.0.1.0",       // symlink only
      "line": "\"/usr/lib/libvsscript.so\" = [...]" }   // line only: the exact appended text
  ]
}
```

`mpv` is `partial` when the private mpv is missing or lacks `vapoursynth` (the files are installed, `mpv-sr` says what
is missing). Entry types: `dir` a directory the installer created (removed only if empty), `file` a file written,
`symlink`, `line` one line appended to an existing (or created) file, `tree` a directory removed recursively (`vfx`
only if it has `nvvfx/libs`; `build`; always inside the prefix).

## Uninstall

`install.sh uninstall` reads `install.json` and walks `entries` backwards:

- a **file** still equal to its recorded `sha256` is replaced by its `backup` (moved back, so content, mode and mtime
  are the original) or deleted when it had none; a file you changed after the install is **kept** (and reported);
- the **line** is removed by exact match (the whole file is restored from the backup when it is untouched since);
- trees go; directories go if empty (a `plugins.dat` VLC left in a plugin directory we created is deleted first);
- with the panel component present, `panel/install-launcher.sh --uninstall --home <home>` removes the desktop entry and
  icon first (they are not in `entries`; `nvcontrol` itself is an ordinary file entry);
- with the webd component present, `systemctl --user disable --now nvvsr-webd.service` and `daemon-reload` run first (failures are ignored); the unit and binary are ordinary file entries;
- the manifest goes last; `~/.local/opt/mpv-vsr` stays unless `--purge-mpv`; a manifest entry outside its recorded
  `home` or containing `..` makes the uninstall refuse.

## Environment

| Variable | Effect |
|---|---|
| `NVVSR_INSTALL_SKIP_GPU_CHECK=1` | GPU-free mode: no driver query (`/proc/driver/nvidia/version`, `nvidia-smi`), no `nvvsr_probe` |
| `NVVSR_INSTALL_SKIP_MPV_BUILD=1` | `--build-mpv` only warns |
| `NVVSR_PANEL_BIN=<exe>` | `--with-panel` installs this executable as `bin/nvcontrol` instead of building the panel (tests) |
| `NVVSR_WEBD_BIN=<exe>` | `--with-chrome` installs this executable as `bin/nvvsr_webd` instead of building `webgpu/daemon` (tests) |
| `NVVSR_WHEEL_URL`, `NVVSR_WHEEL_SHA256` | test hooks: the wheel's download URL (default: the pinned file on `https://pypi.nvidia.com/nvidia-vfx/`, any curl URL works) and its sha256 (default: the pinned one) |

## For the panel

The panel reads `install.json` and the config files above; it does not write them (`vsr.conf` excepted). `--with-panel`
builds `nvcontrol` (`cmake -S panel -B build/panel`, Release; `NVVSR_PANEL_BIN=<exe>` installs that file instead),
copies it to `<prefix>/bin/nvcontrol` (a recorded file entry), then runs `panel/install-launcher.sh --bin
<prefix>/bin/nvcontrol --home <home>`. The launcher's files are not tracked in `install.json`; it undoes itself with
`--uninstall --home <home>`, which uninstall runs.
