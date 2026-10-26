"""Round-trip editors for the players' native config files (mpv script-opts `vsr.conf`, VLC `vlcrc`).

Only the keys the user changed are touched: comments, blank lines, unknown keys and ordering stay as they are.
Writes are atomic, the first modification keeps a `.bak-nvvsr` copy, and the file is re-read right before the
write so an edit made elsewhere in the meantime is merged, not overwritten.
"""

import os
import re
import shutil
import tempfile
from dataclasses import dataclass, field

BACKUP_SUFFIX = ".bak-nvvsr"
_KV = re.compile(r"^(\s*)([A-Za-z0-9_.\-]+)(\s*=\s?)(.*?)\s*$")


class ConfFile:
    """Line-preserving key=value file. A `#key=value` line is a documented default, not an assignment."""

    def __init__(self, text=""):
        self.eol = "\r\n" if "\r\n" in text else "\n"
        self.final_eol = text.endswith("\n") or not text
        self.lines = text.replace("\r\n", "\n").split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()

    # -- reading --------------------------------------------------------------------------------------------
    def _active(self, key):
        out = []
        for i, line in enumerate(self.lines):
            if line.lstrip().startswith(("#", ";", "[")):
                continue
            m = _KV.match(line)
            if m and m.group(2) == key:
                out.append(i)
        return out

    def _commented_default(self, key):
        pat = re.compile(r"^#\s?" + re.escape(key) + r"\s*=")
        for i, line in enumerate(self.lines):
            if pat.match(line):
                return i
        return None

    def get(self, key):
        """Value of the last active assignment (what the player uses), or None."""
        idx = self._active(key)
        return _KV.match(self.lines[idx[-1]]).group(4) if idx else None

    # -- editing --------------------------------------------------------------------------------------------
    def set(self, key, value, section=None):
        value = str(value)
        if "\n" in value or "\r" in value:
            raise ValueError("value must be a single line")
        idx = self._active(key)
        if idx:
            m = _KV.match(self.lines[idx[-1]])
            self.lines[idx[-1]] = f"{m.group(1)}{key}{m.group(3)}{value}"
            return
        c = self._commented_default(key)
        if c is not None:
            self.lines[c] = f"{key}={value}"
            return
        self._append(f"{key}={value}", section)

    def unset(self, key, default=None):
        """Back to the default: the assignment becomes the documented `#key=default` line (or disappears)."""
        idx = self._active(key)
        if not idx:
            return
        for i in reversed(idx[:-1]):
            del self.lines[i]
        i = idx[-1] - (len(idx) - 1)
        if default is not None and self._commented_default(key) is None:
            self.lines[i] = f"#{key}={default}"
        else:
            del self.lines[i]

    def _append(self, line, section):
        if section:
            header = f"[{section}]"
            # vlcrc headers carry a description: "[nvvsr] # NVIDIA VSR"
            h = next((i for i, l in enumerate(self.lines) if re.match(re.escape(header) + r"\s*(#.*)?$", l)), None)
            if h is not None:
                end = next((i for i in range(h + 1, len(self.lines)) if self.lines[i].lstrip().startswith("[")),
                           len(self.lines))
                at = end
                while at > h + 1 and not self.lines[at - 1].strip():
                    at -= 1
                self.lines.insert(at, line)
                return
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines.extend([header, line])
            return
        self.lines.append(line)

    def render(self):
        text = self.eol.join(self.lines)
        return text + self.eol if (self.final_eol and self.lines) else text


# ------------------------------------------------------------------------------------------------------------
# Option tables
# ------------------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Opt:
    key: str
    kind: str            # bool | int | float | float? (may be empty) | enum | enumint | choice (free text) | str
    default: object
    label: str
    help: str = ""
    lo: "float | None" = None
    hi: "float | None" = None
    choices: tuple = ()  # ((value, label), ...) for enum/choice
    advanced: bool = False
    digits: int = 0  # spin button decimals


QUALITY_CHOICES = (
    ("auto", "Automatic"), ("bicubic", "Bicubic (no AI)"), ("low", "Low"), ("medium", "Medium"), ("high", "High"),
    ("ultra", "Ultra"), ("streaming_medium", "Streaming medium"), ("streaming_ultra", "Streaming ultra"),
)

# Mirrors mpv/script-opts/vsr.conf and M.defaults / M.normalize in mpv/vsr.lua (tests compare against the repo copy).
MPV_OPTIONS = (
    Opt("enabled", "bool", True, "Automatic super resolution",
        "Master switch for the automatic mode. With off nothing happens by itself, but the toggle key (Ctrl+s) "
        "still turns SR on for the current file."),
    Opt("ac_only", "bool", True, "Only on AC power",
        "Only run SR on AC power: the RTX draws about 150 W while it works. Mains and USB-C PD supplies count as AC."),
    Opt("quality", "enum", "auto", "Quality",
        "auto picks by source height and frame rate: up to 540p Ultra, 541-800p High, above 800p Medium (very high "
        "frame rates one level lower). The adaptive step-down starts from this level.", choices=QUALITY_CHOICES),
    Opt("min_height", "int", 360, "Smallest source height",
        "Smallest source height (pixels) worth upscaling.", lo=0, hi=4320),
    Opt("adaptive", "bool", True, "Adaptive quality",
        "When frames are dropped, SR restarts one quality level lower (Ultra, High, Medium, Low, Bicubic) and never "
        "goes back up during the same file. If Bicubic cannot keep up either, SR is switched off for the file."),
    Opt("hwdec", "choice", "copy", "Hardware decoding",
        "The VapourSynth filter needs frames in system memory. copy: while SR is active switch your hwdec to its "
        "-copy variant and restore it afterwards. keep: leave hwdec alone. Anything else is used as the hwdec "
        "value while SR is active, e.g. nvdec-copy.",
        choices=(("copy", "Use -copy variant"), ("keep", "Keep as is"),
                 ("nvdec-copy", "nvdec-copy"), ("vaapi-copy", "vaapi-copy"), ("auto-copy", "auto-copy"))),
    Opt("convert_10bit", "bool", True, "Convert 10-bit video",
        "10-bit / 4:2:2 / 4:4:4 sources: yes lets mpv convert them to 8-bit 4:2:0 in front of the filter (the SR "
        "engine is 8-bit); no skips SR for them."),
    Opt("retry_on_next_file", "bool", True, "Retry on the next file",
        "After a failure SR stays off for that file. With yes it tries again on the next file."),
    Opt("osd", "bool", True, "Status badge",
        "A small, semi-transparent badge with one or two words (SR, SR off, SR failed) in a screen corner. "
        "Ctrl+Alt+s toggles it while playing."),
    Opt("osd_size", "int", 11, "Badge text size",
        "Text size at a 1280x720 scale (11 is about 2% of the screen height).", lo=6, hi=40),
    Opt("osd_opacity", "float", 0.55, "Badge opacity", "How opaque the badge is (0.1 to 1).", lo=0.1, hi=1, digits=2),
    Opt("osd_position", "enumint", 9, "Badge corner",
        "Numpad layout: 9 top right, 7 top left, 3 bottom right, 1 bottom left, 8 top centre, 2 bottom centre.",
        choices=((9, "Top right"), (7, "Top left"), (3, "Bottom right"), (1, "Bottom left"),
                 (8, "Top centre"), (2, "Bottom centre")), lo=1, hi=9),
    Opt("osd_duration", "float", 1.5, "Badge duration (s)",
        "Seconds the badge stays up; 0 keeps it until the next message.", lo=0, hi=30, digits=1),
    Opt("pool", "int", 1, "Concurrent contexts",
        "Concurrent libnvvsr contexts (1-4). Every extra one costs about 250 MiB of VRAM, one already saturates the GPU.",
        lo=1, hi=4),
    Opt("strength", "float?", "", "Strength",
        "Strength 0..1 of the SR effect. Empty uses the SDK default.", lo=0, hi=1, digits=2),
    # advanced
    Opt("check_interval", "int", 30, "Power check interval (s)",
        "Seconds between power-source checks.", lo=1, hi=3600, advanced=True),
    Opt("slop", "float", 0.02, "Minimum gain",
        "Do nothing if the fitted picture would be less than this much (0.02 = 2%) larger than the source.",
        lo=0, hi=1, advanced=True, digits=2),
    Opt("adaptive_window", "float", 5, "Adaptive window (s)",
        "Window in which dropped frames are counted.", lo=1, hi=120, advanced=True, digits=0),
    Opt("adaptive_drops", "int", 10, "Adaptive drop threshold",
        "Dropped frames within the window that trigger a step down.", lo=1, hi=10000, advanced=True),
    Opt("settle", "float", 2, "Settle time (s)",
        "Nothing is judged for this many seconds after (re)starting SR.", lo=0, hi=60, advanced=True, digits=1),
    Opt("confirm_timeout", "float", 8, "Confirm timeout (s)",
        "SR counts as failed if no scaled frames arrive within this many seconds.", lo=1, hi=120, advanced=True,
        digits=0),
    Opt("stats_interval", "float", 1, "Stats refresh (s)",
        "Refresh interval of the Alt+s stats overlay.", lo=0.2, hi=10, advanced=True, digits=1),
    Opt("buffered_frames", "int", 4, "Buffered frames",
        "Frames mpv buffers in the filter.", lo=1, hi=16, advanced=True),
    Opt("concurrent_frames", "int", 2, "Concurrent frames",
        "Frames mpv renders concurrently in the filter.", lo=1, hi=16, advanced=True),
)

VLC_OPTIONS = (
    Opt("nvvsr-enable", "bool", True, "Enable super resolution",
        "Master switch of the nvvsr video output. When disabled VLC uses its normal video output."),
    Opt("nvvsr-quality", "enum", "auto", "Quality",
        "auto picks it from the source height and frame rate (up to 540p ultra, up to 800p high, otherwise medium). "
        "bicubic uses no neural network.", choices=QUALITY_CHOICES),
    Opt("nvvsr-fullscreen-only", "bool", True, "Only while fullscreen",
        "Upscale only while the video is fullscreen. When off it also runs in a window larger than the video."),
    Opt("nvvsr-ac-only", "bool", True, "Only on mains power",
        "Do not run super resolution on battery (the GPU draws about 150 W). Checked every 30 seconds."),
    Opt("nvvsr-min-height", "int", 360, "Smallest source height",
        "Sources lower than this many lines are left alone.", lo=0, hi=4320),
    Opt("nvvsr-min-gain", "int", 2, "Minimum gain (percent)",
        "The video must be at least this many percent smaller than the display (fitted) to be upscaled.", lo=0, hi=100),
    Opt("nvvsr-width", "int", 0, "Panel width",
        "Width of the display in pixels. 0 uses the size of the video window (the screen while fullscreen).",
        lo=0, hi=16384, advanced=True),
    Opt("nvvsr-height", "int", 0, "Panel height",
        "Height of the display in pixels, see the width.", lo=0, hi=16384, advanced=True),
    Opt("nvvsr-vout", "str", "", "Real video output",
        "Video output module that shows the pictures (for example gl, xcb_x11, wl_shm). Empty selects it "
        "automatically, like VLC would without nvvsr.", advanced=True),
)

_TRUE = ("yes", "true", "1", "on")
_FALSE = ("no", "false", "0", "off")


def parse_value(opt, text):
    """Typed value for the text found in the file. Raises ValueError when it is not valid for the option."""
    t = (text or "").strip()
    if opt.kind == "bool":
        if t.lower() in _TRUE:
            return True
        if t.lower() in _FALSE:
            return False
        raise ValueError(f"{opt.key}: expected yes or no, found '{t}'")
    if opt.kind in ("int", "float", "float?", "enumint"):
        if opt.kind == "float?" and t == "":
            return ""
        try:
            v = float(t)
        except ValueError:
            raise ValueError(f"{opt.key}: '{t}' is not a number") from None
        if v != v or (opt.lo is not None and v < opt.lo) or (opt.hi is not None and v > opt.hi):
            raise ValueError(f"{opt.key}: {t} is outside {opt.lo:g}..{opt.hi:g}")
        if opt.kind in ("int", "enumint"):
            if v != int(v):
                raise ValueError(f"{opt.key}: '{t}' is not a whole number")
            return int(v)
        return v
    if opt.kind == "enum":
        v = t.lower().replace(" ", "")
        if v not in [c[0] for c in opt.choices]:
            raise ValueError(f"{opt.key}: unknown value '{t}'")
        return v
    return t  # choice / str: any single-line text


def format_value(opt, value, bool_style="yesno"):
    if opt.kind == "bool":
        return ("1" if value else "0") if bool_style == "01" else ("yes" if value else "no")
    if opt.kind in ("float", "float?"):
        return "" if value in ("", None) else f"{round(float(value), 4):g}"
    if opt.kind in ("int", "enumint"):
        return str(int(value))
    return str(value)


def default_text(opt, bool_style="yesno"):
    return format_value(opt, opt.default, bool_style)


@dataclass
class Loaded:
    path: str
    exists: bool
    values: dict                                  # key -> typed value (file value or default)
    raw: dict                                     # key -> active text in the file, or None
    problems: list = field(default_factory=list)  # invalid values found (the default is shown instead)


def _read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            return f.read(), True
    except FileNotFoundError:
        return "", False
    except OSError:
        return "", False


def load_settings(path, options):
    text, exists = _read(path)
    conf = ConfFile(text)
    values, raw, problems = {}, {}, []
    for opt in options:
        r = conf.get(opt.key)
        raw[opt.key] = r
        if r is None:
            values[opt.key] = opt.default
            continue
        try:
            values[opt.key] = parse_value(opt, r)
        except ValueError as e:
            problems.append(str(e))
            values[opt.key] = opt.default
    return Loaded(path, exists, values, raw, problems)


@dataclass
class SaveResult:
    ok: bool
    wrote: bool = False
    conflicts: list = field(default_factory=list)
    error: "str | None" = None
    backup: "str | None" = None


def save_changes(path, options, edited, loaded, *, section=None, bool_style="yesno", force=False):
    """Apply the user's edits (key -> typed value) to the file as it is on disk right now.

    Only keys whose value differs from what was loaded are written. If such a key was changed on disk since
    it was loaded, nothing is written and the keys are returned as conflicts (unless force).
    """
    by_key = {o.key: o for o in options}
    changes = {}
    for key, value in edited.items():
        opt = by_key.get(key)
        if opt is None or value == loaded.values.get(key):
            continue
        changes[key] = format_value(opt, value, bool_style)

    real = os.path.realpath(path)  # keep dotfile symlinks intact
    for attempt in range(2):
        text, exists = _read(real)
        st = os.stat(real) if exists else None
        conf = ConfFile(text)
        conflicts = [k for k in changes if conf.get(k) != loaded.raw.get(k)]
        if conflicts and not force:
            return SaveResult(ok=False, conflicts=conflicts)
        for key, new in changes.items():
            opt = by_key[key]
            if new == default_text(opt, bool_style):
                conf.unset(key, new)  # back to the default: the line becomes the documented `#key=default`
            else:
                conf.set(key, new, section=section)
        out = conf.render()
        if exists and out == text:
            return SaveResult(ok=True, wrote=False)
        if not exists and not changes:
            return SaveResult(ok=True, wrote=False)
        try:
            if exists and os.stat(real).st_mtime_ns != st.st_mtime_ns and attempt == 0:
                continue  # changed while we were working: redo from the new content
            backup = _backup_once(real) if exists else None
            atomic_write(real, out, st.st_mode & 0o777 if st else 0o644)
        except OSError as e:
            return SaveResult(ok=False, error=f"could not write {path}: {e.strerror or e}")
        return SaveResult(ok=True, wrote=True, backup=backup)
    return SaveResult(ok=False, error="the file kept changing while saving; try again")


def _backup_once(path):
    bak = path + BACKUP_SUFFIX
    if os.path.exists(bak):
        return None
    shutil.copy2(path, bak)
    return bak


def atomic_write(path, text, mode):
    d = os.path.dirname(path) or "."
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix="." + os.path.basename(path) + ".", dir=d)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
