"""Fixtures shared by the tests: fake sysfs trees, fake tools, scratch homes. Nothing here touches the real GPU."""

import json
import os
import shutil
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = os.path.dirname(HERE)
REPO = os.path.dirname(PANEL)
if PANEL not in sys.path:
    sys.path.insert(0, PANEL)

GPU_CSV = ("NVIDIA GeForce RTX 3070 Ti Laptop GPU, 615.78.08, 1312, 8192, 12, 47, 18.52, 125.00, 1230, 6000, 4, 16, "
           "[N/A], P0, Disabled\n")
APPS_CSV = "4242, /usr/bin/python3, 512\n3103, /usr/lib/chromium/chrome, 220\n"
POWER_Q = """\

==============NVSMI LOG==============

Timestamp                                 : Sun Oct 26 12:00:00 2026
Driver Version                            : 615.78.08
CUDA Version                              : 13.2

Attached GPUs                             : 1
GPU 00000000:01:00.0
    Performance State                     : P0
    Clocks Event Reasons
        Idle                              : Not Active
        Applications Clocks Setting       : Not Active
        SW Power Cap                      : Active
        HW Slowdown                       : Not Active
            HW Thermal Slowdown           : Not Active
            HW Power Brake Slowdown       : Not Active
        Sync Boost                        : Not Active
        SW Thermal Slowdown               : Not Active
        Display Clock Setting             : Not Active
    GPU Power Readings
        Average Power Draw                : 18.40 W
        Instantaneous Power Draw          : 18.52 W
        Current Power Limit               : 125.00 W
        Requested Power Limit             : 125.00 W
        Default Power Limit               : 125.00 W
        Min Power Limit                   : 1.00 W
        Max Power Limit                   : 150.00 W
    Power Samples
        Duration                          : 2.38 sec
        Number of Samples                 : 119
        Max                               : 20.00 W
    GPU Memory Power Readings
        Average Power Draw                : N/A
    Clocks
        Graphics                          : 1230 MHz
        SM                                : 1230 MHz
        Memory                            : 6000 MHz
        Video                             : 1110 MHz
    Max Clocks
        Graphics                          : 2100 MHz
        SM                                : 2100 MHz
        Memory                            : 6001 MHz
        Video                             : 1950 MHz
"""

SCHEME = {"name": "test", "flavour": "default", "mode": "dark", "variant": "tonalspot", "colours": {
    "surface": "141218", "surfaceContainer": "211f26", "surfaceContainerHigh": "2b2930", "surfaceContainerHighest": "36343b",
    "surfaceContainerLow": "1d1b20", "onSurface": "e6e0e9", "onSurfaceVariant": "cac4d0", "primary": "7fd9c3",
    "onPrimary": "00382d", "primaryContainer": "005143", "onPrimaryContainer": "9cf5df", "secondaryContainer": "334b44",
    "onSecondaryContainer": "cde8df", "tertiary": "a8c8ff", "outline": "938f99", "outlineVariant": "49454f",
    "error": "f2b8b5", "onError": "601410", "errorContainer": "8c1d18", "onErrorContainer": "f9dedc",
    "success": "b5ccba", "onSuccess": "213528"}}


def write(path, text="", mode=None):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    if mode is not None:
        os.chmod(path, mode)
    return path


def make_sysfs(root, runtime_status="active", online="1", with_nvidia=True, power="d0"):
    """A /sys look-alike: one NVIDIA GPU (+ its audio function), one Intel GPU, AC adapter and battery."""
    dev = os.path.join(root, "bus", "pci", "devices")
    if with_nvidia:
        g = os.path.join(dev, "0000:01:00.0")
        write(os.path.join(g, "vendor"), "0x10de\n")
        write(os.path.join(g, "class"), "0x030000\n")
        write(os.path.join(g, "device"), "0x24a0\n")
        write(os.path.join(g, "boot_vga"), "0\n")
        write(os.path.join(g, "power", "runtime_status"), runtime_status + "\n")
        write(os.path.join(g, "power", "control"), "auto\n")
        write(os.path.join(g, "power", "autosuspend_delay_ms"), "5000\n")
        write(os.path.join(g, "power_state"), ("D3cold" if runtime_status == "suspended" else "D0") + "\n")
        write(os.path.join(g, "d3cold_allowed"), "1\n")
        os.makedirs(os.path.join(root, "bus", "pci", "drivers", "nvidia"), exist_ok=True)
        os.symlink("../../../../bus/pci/drivers/nvidia", os.path.join(g, "driver"))
        a = os.path.join(dev, "0000:01:00.1")
        write(os.path.join(a, "vendor"), "0x10de\n")
        write(os.path.join(a, "class"), "0x040300\n")
        write(os.path.join(root, "module", "nvidia", "version"), "615.78.08\n")
    i = os.path.join(dev, "0000:00:02.0")
    write(os.path.join(i, "vendor"), "0x8086\n")
    write(os.path.join(i, "class"), "0x030000\n")
    write(os.path.join(root, "class", "power_supply", "AC0", "type"), "Mains\n")
    write(os.path.join(root, "class", "power_supply", "AC0", "online"), online + "\n")
    write(os.path.join(root, "class", "power_supply", "BAT0", "type"), "Battery\n")
    return root


def make_fake_bin(bindir, log, term_log=None):
    """Fake nvidia-smi / supergfxctl / pgrep / mpv / vlc / terminals. nvidia-smi logs every call to `log`."""
    os.makedirs(bindir, exist_ok=True)
    data = os.path.join(bindir, "data")
    write(os.path.join(data, "gpu.csv"), GPU_CSV)
    write(os.path.join(data, "apps.csv"), APPS_CSV)
    write(os.path.join(data, "power.txt"), POWER_Q)
    x = stat.S_IRWXU
    write(os.path.join(bindir, "nvidia-smi"), f"""#!/bin/sh
echo "$@" >> "{log}"
case "$*" in
  *--query-gpu*) cat "{data}/gpu.csv" ;;
  *--query-compute-apps*) cat "{data}/apps.csv" ;;
  *"-q -d"*) cat "{data}/power.txt" ;;
  *) echo "fake nvidia-smi: unsupported: $*" >&2; exit 2 ;;
esac
""", x)
    write(os.path.join(bindir, "supergfxctl"), "#!/bin/sh\necho Hybrid\n", x)
    write(os.path.join(bindir, "pgrep"), "#!/bin/sh\nexit 1\n", x)
    write(os.path.join(bindir, "mpv"), "#!/bin/sh\necho 'mpv 0.41.0 Copyright (c) fake'\n", x)
    write(os.path.join(bindir, "vlc"), "#!/bin/sh\necho 'VLC version 3.0.24 Vetinari (fake)'\n", x)
    tl = term_log or os.path.join(bindir, "terminal.log")
    for t in ("kitty", "alacritty"):
        write(os.path.join(bindir, t), f'#!/bin/sh\necho "{t} $@" >> "{tl}"\n', x)
    return bindir


def build_fake_lib(dest_dir):
    """Compile tests/fake_nvvsr.c to libnvvsr.so. Returns the path or None when there is no compiler."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        return None
    os.makedirs(dest_dir, exist_ok=True)
    out = os.path.join(dest_dir, "libnvvsr.so")
    r = subprocess.run([cc, "-shared", "-fPIC", "-o", out, os.path.join(HERE, "fake_nvvsr.c")], capture_output=True)
    return out if r.returncode == 0 else None


def make_home(home, repo=None, with_install_json=True, fake_lib=None):
    """A scratch HOME with fake configs; returns dict of the paths used."""
    cfg = os.path.join(home, ".config")
    vfx = os.path.join(home, "venv", "site-packages", "nvvfx", "libs")
    wheel = os.path.join(home, "venv", "site-packages", "nvidia_vfx.libs")
    write(os.path.join(vfx, "libnvVFXVideoSuperRes.so"), "")
    write(os.path.join(wheel, "libcudart.so.12"), "")
    write(os.path.join(home, "venv", "site-packages", "nvidia_vfx-0.2.0.0.dist-info", "METADATA"),
          "Metadata-Version: 2.2\nName: nvidia-vfx\nVersion: 0.2.0.0\n")
    write(os.path.join(cfg, "nvvsr", "libdir"), f"{vfx}:{wheel}\n")
    write(os.path.join(cfg, "nvvsr", "handoff.json"), '{"use_chrome_cookies": true, "SECRET": "do-not-leak"}\n')
    write(os.path.join(cfg, "vapoursynth", "vapoursynth.toml"),
          '"/usr/lib/libvsscript.so" = ["/usr/bin/python","/usr/lib/libpython3.so","1"]\n')
    shutil.copy(os.path.join(REPO, "mpv", "script-opts", "vsr.conf"), write(os.path.join(cfg, "mpv", "script-opts", "vsr.conf")))
    write(os.path.join(cfg, "vlc", "vlcrc"), VLCRC_SAMPLE)
    write(os.path.join(home, ".local", "state", "caelestia", "scheme.json"), json.dumps(SCHEME))
    mpv = write(os.path.join(home, ".local", "opt", "mpv-vsr", "bin", "mpv"),
                "#!/bin/sh\ncase \"$*\" in *--vf=help*) printf 'Available video filters:\\n  format  Force format\\n  vapoursynth  VapourSynth\\n';; "
                "*) echo 'mpv 0.41.0-vsr (fake)';; esac\n", stat.S_IRWXU)
    write(os.path.join(home, ".local", "bin", "mpv-sr"), "#!/bin/sh\nexec true\n", stat.S_IRWXU)
    if repo:
        for rel in ("build/core/libnvvsr.so", "vapoursynth/build/libvsnvvsr.so", "vlc/build/libnvvsr_plugin.so"):
            p = os.path.join(repo, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            if rel.endswith("libnvvsr.so") and fake_lib:
                shutil.copy(fake_lib, p)
            else:
                write(p, "")
    if with_install_json:
        rec = {"lib": os.path.join(home, ".local", "lib", "nvvsr"), "mpv_wrapper": os.path.join(home, ".local", "bin", "mpv-sr"),
               "private_mpv": mpv}
        if repo:  # pretend the installer put the three libraries where the fake repo build has them
            rec.update(libnvvsr=os.path.join(repo, "build/core/libnvvsr.so"),
                       vapoursynth_plugin=os.path.join(repo, "vapoursynth/build/libvsnvvsr.so"),
                       vlc_plugin=os.path.join(repo, "vlc/build/libnvvsr_plugin.so"))
        write(os.path.join(home, ".local", "share", "nvvsr", "install.json"), json.dumps({
            "schema": 1, "version": "0.1.0", "installed_at": "2026-10-26T10:00:00+00:00",
            "updated_at": "2026-10-26T10:00:00+00:00", "git_commit": None, "git_dirty": False,
            "repo": repo or "/nonexistent/repo", "prefix": os.path.join(home, ".local"), "home": home,
            "nvvsr_libdir": f"{vfx}:{wheel}", "paths": rec,
            "private_mpv": {"path": mpv, "vapoursynth": True, "built_by_installer": False},
            "components": {"core": {"status": "installed", "detail": ""},
                           "chrome_handoff": {"status": "skipped", "detail": "not requested"}},
            "entries": [], "future_unknown_key": {"x": 1}}))
    return {"cfg": cfg, "vfx": vfx, "wheel": wheel, "mpv_conf": os.path.join(cfg, "mpv", "script-opts", "vsr.conf"),
            "vlcrc": os.path.join(cfg, "vlc", "vlcrc"), "mpv": mpv}


VLCRC_SAMPLE = """\
### vlc 3.0.24 configuration file ###

[core] # core program
# Fullscreen video output (boolean)
#fullscreen=0

# Audio volume
volume=256

[avcodec] # AVCodec audio/video decoder
# Direct rendering (boolean)
#avcodec-dr=1
avcodec-hw=any

[nvvsr] # NVIDIA VSR
# Enable NVIDIA super resolution (boolean)
#nvvsr-enable=1
# Quality (string)
#nvvsr-quality=auto
# Only on mains power (boolean)
#nvvsr-ac-only=1

[zzz-other] # something after
#zzz-key=1
"""
