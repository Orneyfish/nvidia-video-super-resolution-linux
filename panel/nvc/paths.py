"""Where things live. Everything is resolved at call time so tests can point HOME / NVC_SYSFS_ROOT elsewhere."""

import os


def home():
    return os.environ.get("HOME") or os.path.expanduser("~")


def sysfs_root():
    return os.environ.get("NVC_SYSFS_ROOT") or "/sys"


def config_home():
    x = os.environ.get("XDG_CONFIG_HOME")
    return x if x and os.path.isabs(x) else os.path.join(home(), ".config")


def data_home():
    x = os.environ.get("XDG_DATA_HOME")
    return x if x and os.path.isabs(x) else os.path.join(home(), ".local", "share")


def state_home():
    x = os.environ.get("XDG_STATE_HOME")
    return x if x and os.path.isabs(x) else os.path.join(home(), ".local", "state")


def expand(path):
    """Expand a leading ~ against HOME (not the passwd entry)."""
    if path == "~":
        return home()
    if path.startswith("~/"):
        return os.path.join(home(), path[2:])
    return path


def mpv_vsr_conf():
    return os.path.join(config_home(), "mpv", "script-opts", "vsr.conf")


def vlcrc():
    return os.path.join(config_home(), "vlc", "vlcrc")


def nvvsr_libdir_file():
    return os.path.join(config_home(), "nvvsr", "libdir")


def panel_settings_file():
    return os.path.join(config_home(), "nvvsr", "panel.json")


def install_json():
    return os.path.join(data_home(), "nvvsr", "install.json")


def vapoursynth_toml():
    return os.path.join(config_home(), "vapoursynth", "vapoursynth.toml")


def scheme_json():
    return os.path.join(state_home(), "caelestia", "scheme.json")


def private_mpv():
    return os.path.join(home(), ".local", "opt", "mpv-vsr", "bin", "mpv")


def repo_dir(app_dir):
    """The development checkout this app lives in (or NVC_REPO_DIR), or None."""
    forced = os.environ.get("NVC_REPO_DIR")
    if forced:
        return forced if os.path.isdir(forced) else None
    cand = os.path.dirname(os.path.abspath(app_dir))
    return cand if os.path.isfile(os.path.join(cand, "core", "include", "nvvsr.h")) else None
