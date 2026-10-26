"""Super resolution install status and the "does it work" probe.

`install.json` (~/.local/share/nvvsr/install.json, written by the installer) is read tolerantly: unknown keys
are ignored and every component is verified on disk anyway. Without it the repo's build directories are used
(development checkout).
"""

import json
import os
import re
import shutil
import sys
from dataclasses import dataclass, field

from . import paths
from . import proc

LIBDIR_SENTINEL = "libnvVFXVideoSuperRes.so"


@dataclass
class InstallInfo:
    """install.json written by install/install.sh (schema 1). Unknown keys are ignored."""
    path: str
    present: bool = False
    data: dict = field(default_factory=dict)
    error: "str | None" = None

    @property
    def paths(self):
        p = self.data.get("paths")
        return {k: v for k, v in p.items() if isinstance(v, str)} if isinstance(p, dict) else {}

    @property
    def version(self):
        v = self.data.get("version")
        return v if isinstance(v, str) else None

    @property
    def components(self):
        """{name: (status, detail)}"""
        comps = self.data.get("components")
        return {str(n): (str(c.get("status", "")), str(c.get("detail") or ""))
                for n, c in comps.items() if isinstance(c, dict)} if isinstance(comps, dict) else {}


def read_install_json(path=None):
    path = path or paths.install_json()
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return InstallInfo(path)
    except (OSError, ValueError) as e:
        return InstallInfo(path, present=True, error=f"unreadable ({e.__class__.__name__})")
    if not isinstance(data, dict):
        return InstallInfo(path, present=True, error="not a JSON object")
    return InstallInfo(path, present=True, data=data)


@dataclass
class Check:
    id: str
    label: str
    ok: "bool | None"            # None: could not be determined
    detail: str = ""
    hint: str = ""


@dataclass
class SrStatus:
    mode: str                    # installed | dev | missing
    version: "str | None"
    checks: list
    repo: "str | None" = None

    def check(self, cid):
        return next(c for c in self.checks if c.id == cid)

    def ok(self, cid):
        return self.check(cid).ok is True

    @property
    def mpv_ready(self):
        return all(self.ok(c) for c in ("libnvvsr", "libdir", "vsplugin", "mpv", "vsscript"))

    @property
    def vlc_ready(self):
        return all(self.ok(c) for c in ("libnvvsr", "libdir", "vlc"))

    @property
    def headline(self):
        if self.mpv_ready and self.vlc_ready:
            return "Ready for mpv and VLC"
        if self.mpv_ready:
            return "Ready for mpv"
        if self.vlc_ready:
            return "Ready for VLC"
        if self.mode == "missing":
            return "Not installed"
        return "Incomplete installation"


def read_libdir(path=None):
    """Directories listed in ~/.config/nvvsr/libdir (first meaningful line, colon separated), expanded."""
    path = path or paths.nvvsr_libdir_file()
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if s and not s.startswith("#"):
                    return [paths.expand(p) for p in s.split(":") if p]
    except OSError:
        pass
    return None


def libdir_env(path=None):
    """The NVVSR_LIBDIR value for the probe: the file's first line, expanded, or None."""
    dirs = read_libdir(path)
    return ":".join(dirs) if dirs else None


def vsscript_entry(toml_path=None):
    """(present, detail) for a libvsscript.so entry in vapoursynth.toml."""
    toml_path = toml_path or paths.vapoursynth_toml()
    try:
        with open(toml_path, "rb") as f:
            raw = f.read()
    except OSError:
        return False, "no vapoursynth.toml"
    try:
        import tomllib
        data = tomllib.loads(raw.decode("utf-8", "replace"))
        keys = [k for k in data if isinstance(k, str)]
    except Exception:
        keys = re.findall(r'^\s*"?([^"=\s]+libvsscript\.so[^"=\s]*)"?\s*=', raw.decode("utf-8", "replace"), re.M)
    hit = next((k for k in keys if k.endswith("libvsscript.so")), None)
    return (True, hit) if hit else (False, "no libvsscript.so entry")


def compute_status(info=None, repo=None, mpv_has_vapoursynth=None):
    """Verify every component on disk. Paths come from install.json when present, else from the dev checkout `repo`."""
    info = info or read_install_json()
    rec = info.paths
    home = paths.home()

    def first(recorded, *dev, exe=False):
        cands = [recorded] if recorded else []
        cands += [os.path.join(repo, *d.split("/")) for d in dev] if repo else []
        return next((c for c in cands if os.path.isfile(c) and (not exe or os.access(c, os.X_OK))), None)

    found = {
        "libnvvsr": first(rec.get("libnvvsr"), "build/core/libnvvsr.so"),
        "vsplugin": first(rec.get("vapoursynth_plugin"), "vapoursynth/build/libvsnvvsr.so"),
        "vlc": first(rec.get("vlc_plugin"), "vlc/build/libnvvsr_plugin.so"),
        "mpv": first(rec.get("private_mpv"), exe=True) or first(paths.private_mpv(), exe=True),
        "mpv_sr": first(rec.get("mpv_wrapper"), exe=True) or first(os.path.join(home, ".local", "bin", "mpv-sr"), exe=True),
    }

    def tag(p):
        return f"{p} (development build)" if p and repo and p.startswith(repo + os.sep) else (p or "")

    def check(cid, label, ok, detail, hint=""):
        return Check(cid, label, ok, detail if ok else "", "" if ok else hint)

    libdirs = read_libdir()
    libdir_label = "NVIDIA VFX library path (~/.config/nvvsr/libdir)"
    if libdirs is None:
        libdir = check("libdir", libdir_label, False, "",
                       "Write the nvvfx/libs and nvidia_vfx.libs directories of the VFX wheel, colon separated.")
    elif any(not os.path.isdir(d) for d in libdirs):
        libdir = check("libdir", libdir_label, False, "", "A directory listed in the file does not exist; fix it or reinstall the wheel.")
    elif not any(os.path.isfile(os.path.join(d, LIBDIR_SENTINEL)) for d in libdirs):
        libdir = check("libdir", libdir_label, False, "", f"{LIBDIR_SENTINEL} is not in the listed directories (use <venv>/.../nvvfx/libs).")
    else:
        libdir = check("libdir", libdir_label, True, ":".join(libdirs))
    toml_ok, toml_detail = vsscript_entry()
    mpv_label = "Private mpv with the VapourSynth filter"
    if not found["mpv"]:
        mpv = check("mpv", mpv_label, False, "", "The distro mpv has no --vf=vapoursynth; build the private mpv (install/build-mpv-vsr.sh).")
    elif mpv_has_vapoursynth is False:
        mpv = check("mpv", mpv_label, False, "", f"{found['mpv']} has no vapoursynth filter; rebuild it with VapourSynth.")
    else:
        mpv = check("mpv", mpv_label, True, found["mpv"] + ("" if mpv_has_vapoursynth else " (filter list not checked)"))
    checks = [
        check("libnvvsr", "libnvvsr (core library)", bool(found["libnvvsr"]), tag(found["libnvvsr"]),
              "Build it (cmake -S core -B build/core) or run install/install.sh."),
        libdir,
        check("vsplugin", "VapourSynth plugin (nvvsr.Upscale)", bool(found["vsplugin"]), tag(found["vsplugin"]),
              "Build it (cmake -S vapoursynth -B vapoursynth/build) or run install/install.sh."),
        mpv,
        check("mpv_sr", "mpv-sr wrapper", bool(found["mpv_sr"]), found["mpv_sr"] or "", "Created by install/install.sh in ~/.local/bin."),
        check("vsscript", "VapourSynth libvsscript entry (vapoursynth.toml)", toml_ok, toml_detail,
              "Add a libvsscript.so entry to ~/.config/vapoursynth/vapoursynth.toml (tools/phase0-results.md)."),
        check("vlc", "VLC module (libnvvsr_plugin.so)", bool(found["vlc"]), tag(found["vlc"]),
              "Build it (cmake -S vlc -B vlc/build) or run install/install.sh."),
    ]
    if info.present and info.error is None:
        mode = "installed"
    elif any(found[k] for k in ("libnvvsr", "vsplugin", "vlc")):
        mode = "dev"
    else:
        mode = "missing"
    return SrStatus(mode=mode, version=info.version, checks=checks, repo=repo)


def mpv_has_vapoursynth(mpv_bin, run=proc.run):
    """True/False from `mpv --no-config --vf=help` (does not touch the GPU), None when it cannot run."""
    r = run([mpv_bin, "--no-config", "--vf=help"], timeout=8)
    if r.rc is None:
        return None
    return bool(re.search(r"^\s*vapoursynth\b", r.out + "\n" + r.err, re.M))


# --------------------------------------------------------------------------------------------------------------
# libnvvsr through ctypes, always in a child process (a crash in the NVIDIA libraries must not take the UI down)
# --------------------------------------------------------------------------------------------------------------

_PROBE_CODE = r"""
import ctypes, json, sys
lib = ctypes.CDLL(sys.argv[1])
lib.nvvsr_probe.restype = ctypes.c_int
lib.nvvsr_strerror.restype = ctypes.c_char_p
lib.nvvsr_strerror.argtypes = [ctypes.c_int]
lib.nvvsr_last_error.restype = ctypes.c_char_p
rc = lib.nvvsr_probe()
msg = lib.nvvsr_strerror(rc)
last = lib.nvvsr_last_error()
print(json.dumps({"rc": rc, "status": msg.decode("utf-8", "replace") if msg else "",
                  "detail": last.decode("utf-8", "replace") if last else ""}))
"""

_VERSION_CODE = r"""
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.nvvsr_version.restype = ctypes.c_char_p
lib.nvvsr_api_version.restype = ctypes.c_int
print(lib.nvvsr_version().decode() + " (API " + str(lib.nvvsr_api_version()) + ")")
"""


def _python():
    return sys.executable or shutil.which("python3") or "python3"


@dataclass
class ProbeResult:
    ok: bool
    summary: str
    detail: str = ""


def run_probe(lib_path, libdir=None, run=proc.run, timeout=90):
    """nvvsr_probe() in a subprocess. This touches the GPU (CUDA init): call it only when the user asks."""
    if not lib_path:
        return ProbeResult(False, "libnvvsr was not found", "Build or install it first.")
    env = proc.clean_env({"NVVSR_LIBDIR": libdir} if libdir else None)
    r = run([_python(), "-I", "-c", _PROBE_CODE, lib_path], timeout=timeout, env=env)
    if r.timed_out:
        return ProbeResult(False, "The check timed out", "The GPU or driver did not answer within %d s." % timeout)
    if r.rc is None:
        return ProbeResult(False, "The check could not start", r.error_text)
    line = next((l for l in reversed(r.out.strip().splitlines()) if l.startswith("{")), "")
    try:
        data = json.loads(line)
        rc = int(data["rc"])
    except (ValueError, KeyError, TypeError):
        if r.rc < 0:
            return ProbeResult(False, "The check crashed (signal %d)" % -r.rc, r.err.strip()[-300:])
        return ProbeResult(False, "The check failed", (r.err or r.out).strip()[-300:] or "no output")
    if rc == 0:
        return ProbeResult(True, "Super resolution works", data.get("detail", ""))
    return ProbeResult(False, "Not usable: " + (data.get("status") or "error %d" % rc), data.get("detail", ""))


def libnvvsr_version(lib_path, run=proc.run):
    """'0.1.0 (API 1)' or None. Loads the library only; no GPU access, no contexts."""
    if not lib_path:
        return None
    r = run([_python(), "-I", "-c", _VERSION_CODE, lib_path], timeout=15)
    return r.out.strip().splitlines()[-1] if r.ok and r.out.strip() else None
