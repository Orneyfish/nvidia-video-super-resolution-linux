"""Versions and the plain-text diagnostics report. No secrets: no cookies, no handoff.json contents, home shortened."""

import glob
import os
import platform
import re

from . import __version__
from . import conf
from . import gpu as gpumod
from . import paths
from . import proc
from . import sr


def vfx_wheel_version(libdirs):
    """Version of the nvidia-vfx wheel read from its dist-info METADATA (nothing is imported)."""
    for d in libdirs or []:
        site = os.path.dirname(os.path.normpath(d))
        for meta in sorted(glob.glob(os.path.join(site, "nvidia[_-]vfx-*.dist-info", "METADATA"))):
            try:
                with open(meta, "r", encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if line.startswith("Version:"):
                            return line.split(":", 1)[1].strip()
            except OSError:
                continue
    return None


def first_line(text):
    for line in (text or "").splitlines():
        if line.strip():
            return re.split(r" Copyright| \(", line.strip())[0]  # "mpv 0.41.0 Copyright ..." -> "mpv 0.41.0"
    return None


def tool_version(argv, run=proc.run):
    r = run(argv, timeout=10)
    return first_line(r.out) if r.ok else None


def collect_versions(status=None, run=proc.run, root=None):
    """name -> text (or None). Slow (spawns processes): call from a worker thread. No GPU access."""
    status = status or sr.compute_status()
    libnv = status.check("libnvvsr")
    lib_path = libnv.detail.split(" (development build)")[0] if libnv.ok else None
    libdirs = sr.read_libdir()
    v = {
        "NV Control": __version__,
        "libnvvsr": sr.libnvvsr_version(lib_path, run=run),
        "NVIDIA kernel driver": gpumod.driver_version_cheap(root),
        "NVIDIA VFX wheel": vfx_wheel_version(libdirs),
        "Private mpv": None,
        "System mpv": tool_version(["mpv", "--no-config", "--version"], run),
        "VLC": tool_version(["vlc", "--version"], run),
        "Kernel": platform.release(),
        "Python": platform.python_version(),
    }
    mpv = paths.private_mpv()
    if os.path.isfile(mpv):
        v["Private mpv"] = tool_version([mpv, "--no-config", "--version"], run)
    return v


def redact(text, home=None):
    home = home or paths.home()
    if home and home != "/":
        text = text.replace(home, "~")
    return text


def _settings_lines(path, options):
    loaded = conf.load_settings(path, options)
    if not loaded.exists:
        return ["  (file does not exist, defaults apply)"]
    out = [f"  {k} = {loaded.raw[k]}" for k in loaded.raw if loaded.raw[k] is not None]
    return out or ["  (no overrides, defaults apply)"]


def build_report(*, versions, status, runtime, sample, hybrid, power_src, toolkit_versions=None, extra_lines=()):
    """The text copied by "Copy diagnostics". `sample` is the last nvidia-smi reading (or None: never queried)."""
    lines = ["NV Control diagnostics", "=" * 22, ""]
    lines.append("Versions")
    for k, v in versions.items():
        lines.append(f"  {k}: {v or 'not found'}")
    for k, v in (toolkit_versions or {}).items():
        lines.append(f"  {k}: {v}")
    lines += ["", "GPU"]
    lines.append(f"  hybrid mode: {hybrid or 'unknown (supergfxctl not available)'}")
    lines.append(f"  power source: {power_src}")
    lines.append(f"  runtime power: status={runtime.status or 'unknown'} control={runtime.control or '?'} "
                 f"state={runtime.power_state or '?'}")
    if sample:
        lines.append(f"  last reading: {sample.name}, driver {sample.driver}, "
                     f"{_fmt(sample.mem_used_mib)}/{_fmt(sample.mem_total_mib)} MiB, util {_fmt(sample.util_pct)} %, "
                     f"{_fmt(sample.temp_c)} C, {_fmt(sample.power_w)}/{_fmt(sample.power_limit_w)} W, "
                     f"pstate {sample.pstate or '?'}")
    else:
        lines.append("  last reading: none (the GPU is not queried while it sleeps)")
    lines += ["", f"Super resolution: {status.headline} ({status.mode})"]
    if status.version:
        lines.append(f"  installed version: {status.version}")
    for c in status.checks:
        mark = {True: "ok", False: "MISSING", None: "unknown"}[c.ok]
        lines.append(f"  [{mark}] {c.label}" + (f": {c.detail}" if c.detail else ""))
    lines += ["", "mpv settings (vsr.conf overrides)"] + _settings_lines(paths.mpv_vsr_conf(), conf.MPV_OPTIONS)
    vlc_lines = _settings_lines(paths.vlcrc(), conf.VLC_OPTIONS)
    lines += ["", "VLC settings (nvvsr-* in vlcrc)"] + vlc_lines
    lines += list(extra_lines)
    lines += ["", "Legacy Chrome handoff configured: "
              + ("yes" if os.path.isfile(os.path.join(paths.config_home(), "nvvsr", "handoff.json")) else "no")
              + " (contents not included)"]
    return redact("\n".join(lines) + "\n")


def _fmt(v):
    return "?" if v is None else (f"{v:g}" if isinstance(v, float) else str(v))
