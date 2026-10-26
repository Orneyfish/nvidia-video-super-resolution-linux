"""GPU state: cheap sysfs reads first, nvidia-smi only when the GPU is already awake.

On a hybrid laptop nvidia-smi wakes the dGPU from runtime suspend (battery drain), so the decision "may I
call nvidia-smi" is made from /sys/bus/pci/devices/<addr>/power/runtime_status, which does not wake anything.
"""

import csv
import io
import os
import re
from dataclasses import dataclass, field

from . import paths
from . import proc

NVIDIA_VENDOR = "0x10de"

# One nvidia-smi call per poll. The first twelve are the documented base set.
QUERY_FIELDS = (
    "name", "driver_version", "memory.used", "memory.total", "utilization.gpu", "temperature.gpu",
    "power.draw", "power.limit", "clocks.gr", "clocks.mem", "pcie.link.gen.current", "pcie.link.width.current",
    "fan.speed", "pstate", "persistence_mode",
)


# --------------------------------------------------------------------------------------------------------------
# sysfs
# --------------------------------------------------------------------------------------------------------------

def read_text(path):
    """Stripped file content, or None. Only used on sysfs/proc attributes that never touch the hardware."""
    try:
        with open(path, "r", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return None


@dataclass
class PciGpu:
    addr: str
    path: str


@dataclass
class RuntimeInfo:
    status: "str | None" = None   # active | suspended | suspending | resuming | unsupported | None (unknown)
    control: "str | None" = None  # auto | on
    power_state: "str | None" = None  # D0 | D3hot | D3cold


def find_nvidia_gpus(root=None):
    """NVIDIA display/3D controllers (vendor 0x10de, class 0x03xxxx); the HDMI audio function (0x0403) is skipped."""
    base = os.path.join(root or paths.sysfs_root(), "bus", "pci", "devices")
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return []
    found = []
    for name in names:
        d = os.path.join(base, name)
        vendor = (read_text(os.path.join(d, "vendor")) or "").lower()
        klass = (read_text(os.path.join(d, "class")) or "").lower()
        if vendor != NVIDIA_VENDOR or not klass.startswith("0x03"):
            continue
        found.append(PciGpu(addr=name, path=d))
    return found


def read_runtime(gpu):
    """Runtime PM attributes of the PCI device. Reading these never wakes the GPU."""
    if gpu is None:
        return RuntimeInfo()
    d = gpu.path
    return RuntimeInfo(
        status=read_text(os.path.join(d, "power", "runtime_status")) or None,
        control=read_text(os.path.join(d, "power", "control")),
        power_state=read_text(os.path.join(d, "power_state")),
    )


def gpu_state(runtime_status):
    """asleep: do not query. awake: querying is free of side effects. unknown: no sysfs info (treated as awake)."""
    if runtime_status in ("suspended", "suspending"):
        return "asleep"
    if runtime_status in ("active", "resuming", "unsupported"):
        return "awake"
    return "unknown"


def poll_decision(*, window_visible, page_active, state, in_flight, interval_s, since_last_s):
    """What a 1 s tick should do. 'query' is the only answer that runs nvidia-smi.

    hidden: window hidden/minimised or another page is showing; asleep: show the sleep card, no query;
    wait: nothing to do yet (request in flight, manual mode, or interval not elapsed).
    """
    if not window_visible or not page_active:
        return "hidden"
    if state == "asleep":
        return "asleep"
    if in_flight or not interval_s or interval_s <= 0:
        return "wait"
    if since_last_s is not None and since_last_s < interval_s * 0.9:
        return "wait"
    return "query"


def driver_version_cheap(root=None):
    """Kernel module version from sysfs, without touching the GPU."""
    return read_text(os.path.join(root or paths.sysfs_root(), "module", "nvidia", "version"))


def power_source(root=None):
    """'ac' | 'battery' | 'unknown' from /sys/class/power_supply, like mpv/vsr.lua (Mains or online USB = AC)."""
    base = os.path.join(root or paths.sysfs_root(), "class", "power_supply")
    try:
        names = sorted(os.listdir(base))
    except OSError:
        return "unknown"
    have_battery = online = False
    for n in names:
        t = (read_text(os.path.join(base, n, "type")) or "").lower()
        if t == "battery":
            have_battery = True
        elif t in ("mains", "usb", "usb_c", "usb_pd", "usb_dcp", "usb_cdp", "usb_aca", "ups"):
            if read_text(os.path.join(base, n, "online")) == "1":
                online = True
    if online:
        return "ac"
    return "battery" if have_battery else "unknown"


# --------------------------------------------------------------------------------------------------------------
# nvidia-smi: --query-gpu / --query-compute-apps
# --------------------------------------------------------------------------------------------------------------

@dataclass
class GpuSample:
    name: str = ""
    driver: str = ""
    mem_used_mib: "float | None" = None
    mem_total_mib: "float | None" = None
    util_pct: "float | None" = None
    temp_c: "float | None" = None
    power_w: "float | None" = None
    power_limit_w: "float | None" = None
    clock_gr_mhz: "float | None" = None
    clock_mem_mhz: "float | None" = None
    pcie_gen: "int | None" = None
    pcie_width: "int | None" = None
    fan_pct: "float | None" = None
    pstate: "str | None" = None
    persistence: "bool | None" = None


@dataclass
class ComputeApp:
    pid: int
    name: str
    mem_mib: "float | None"


def _na(text):
    t = text.strip()
    return t == "" or t.startswith("[") or t.upper() in ("N/A", "NOT SUPPORTED", "UNKNOWN")


def _num(text):
    if _na(text):
        return None
    try:
        return float(text.strip())
    except ValueError:
        return None


def _int(text):
    v = _num(text)
    return None if v is None else int(v)


def query_gpu_argv():
    return ["nvidia-smi", "--query-gpu=" + ",".join(QUERY_FIELDS), "--format=csv,noheader,nounits"]


def parse_gpu_csv(text, fields=QUERY_FIELDS):
    """First GPU row of `--query-gpu=<fields> --format=csv,noheader,nounits` as a GpuSample, or None."""
    rows = [r for r in csv.reader(io.StringIO(text.strip()), skipinitialspace=True) if r]
    if not rows:
        return None
    row = [c.strip() for c in rows[0]]
    if len(row) < 2:
        return None
    v = dict(zip(fields, row))
    persistence = v.get("persistence_mode", "")
    return GpuSample(
        name=v.get("name", ""), driver=v.get("driver_version", ""),
        mem_used_mib=_num(v.get("memory.used", "")), mem_total_mib=_num(v.get("memory.total", "")),
        util_pct=_num(v.get("utilization.gpu", "")), temp_c=_num(v.get("temperature.gpu", "")),
        power_w=_num(v.get("power.draw", "")), power_limit_w=_num(v.get("power.limit", "")),
        clock_gr_mhz=_num(v.get("clocks.gr", "")), clock_mem_mhz=_num(v.get("clocks.mem", "")),
        pcie_gen=_int(v.get("pcie.link.gen.current", "")), pcie_width=_int(v.get("pcie.link.width.current", "")),
        fan_pct=_num(v.get("fan.speed", "")),
        pstate=None if _na(v.get("pstate", "")) else v["pstate"].strip(),
        persistence=True if persistence.lower() == "enabled" else (False if persistence.lower() == "disabled" else None),
    )


def query_gpu(run=proc.run):
    """(GpuSample | None, error text | None). This WAKES a suspended GPU: callers check runtime_status first."""
    r = run(query_gpu_argv(), timeout=8)
    if not r.ok:
        return None, r.error_text
    s = parse_gpu_csv(r.out)
    return (s, None) if s else (None, "nvidia-smi returned no GPU")


def parse_compute_apps(text):
    apps = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.lower().startswith("no running"):
            continue
        head, _, mem = line.rpartition(",")
        pid, _, name = head.partition(",")
        pid = pid.strip()
        if not pid.isdigit():
            continue
        apps.append(ComputeApp(int(pid), name.strip() or "?", _num(mem)))
    return apps


def query_compute_apps(run=proc.run):
    r = run(["nvidia-smi", "--query-compute-apps=pid,process_name,used_memory", "--format=csv,noheader,nounits"],
            timeout=8)
    if not r.ok:
        return None, r.error_text
    return parse_compute_apps(r.out), None


def supergfxctl_mode(run=proc.run):
    """Hybrid mode name ('Hybrid', 'Integrated', ...) or None. Talks to the supergfxd daemon, not to the GPU."""
    r = run(["supergfxctl", "-g"], timeout=4)
    if not r.ok:
        return None
    mode = r.out.strip().splitlines()[0].strip() if r.out.strip() else ""
    return mode or None


# --------------------------------------------------------------------------------------------------------------
# nvidia-smi -q -d POWER,CLOCK,PERFORMANCE
# --------------------------------------------------------------------------------------------------------------

_KV = re.compile(r"^(?P<key>.*?\S)\s+:(?:\s+(?P<val>.*))?$")


def parse_smi_q(text):
    """Indentation-structured `nvidia-smi -q` output as nested dicts (section -> {key: value | section})."""
    root = {}
    stack = [(-1, root)]
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("=") or raw.lstrip().startswith("Timestamp"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        line = raw.strip()
        while len(stack) > 1 and stack[-1][0] >= indent:
            stack.pop()
        parent = stack[-1][1]
        m = _KV.match(line)
        if m:
            key, val = m.group("key"), (m.group("val") or "").strip()
            parent[_unique(parent, key)] = val
        else:
            node = {}
            parent[_unique(parent, line)] = node
            stack.append((indent, node))
    return root


def _unique(d, key):
    if key not in d:
        return key
    i = 2
    while f"{key}#{i}" in d:
        i += 1
    return f"{key}#{i}"


def _watts(v):
    if not isinstance(v, str):
        return None
    m = re.match(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*W\b", v or "")
    return float(m.group(1)) if m else None


def _mhz(v):
    if not isinstance(v, str):
        return None
    m = re.match(r"^\s*([0-9]+)\s*MHz\b", v or "")
    return int(m.group(1)) if m else None


def _first_gpu_node(tree):
    for k, v in tree.items():
        if isinstance(v, dict) and k.startswith("GPU "):
            return v
    return tree


def _section(node, *needles, exclude=()):
    """First sub-section whose name contains one of the needles (case-insensitive) and none of the excludes."""
    for k, v in node.items():
        if not isinstance(v, dict):
            continue
        lk = k.lower()
        if any(n in lk for n in needles) and not any(x in lk for x in exclude):
            return v
    return None


@dataclass
class PowerInfo:
    draw_w: "float | None" = None
    limit_w: "float | None" = None
    requested_w: "float | None" = None
    default_w: "float | None" = None
    min_w: "float | None" = None
    max_w: "float | None" = None
    management: "str | None" = None


@dataclass
class DetailInfo:
    power: PowerInfo = field(default_factory=PowerInfo)
    clocks: dict = field(default_factory=dict)       # graphics/sm/memory/video -> MHz
    max_clocks: dict = field(default_factory=dict)
    limiters: list = field(default_factory=list)     # names of "Active" clock event reasons
    has_limiter_info: bool = False


def parse_power_page(text):
    """Everything the Power page shows, from one `nvidia-smi -q -d POWER,CLOCK,PERFORMANCE` output."""
    gpu = _first_gpu_node(parse_smi_q(text))
    info = DetailInfo()

    pr = _section(gpu, "power readings", exclude=("memory", "module"))
    flat = pr if pr is not None else gpu
    ceiling = flat.get("GPU Ceiling Power Limit")
    if isinstance(ceiling, dict):  # driver 615 nests current/requested/default one level down
        flat = {**flat, **ceiling}
    get = lambda *names: next((_watts(flat[n]) for n in names if n in flat and _watts(flat[n]) is not None), None)
    info.power = PowerInfo(
        draw_w=get("Instantaneous Power Draw", "Average Power Draw", "Power Draw"),
        limit_w=get("Current Power Limit", "Power Limit"),
        requested_w=get("Requested Power Limit"),
        default_w=get("Default Power Limit"),
        min_w=get("Min Power Limit"),
        max_w=get("Max Power Limit"),
        management=next((flat[k] for k in ("Power Management", "Power Management Mode")
                         if isinstance(flat.get(k), str) and flat[k]), None),
    )

    def clocks(sec):
        out = {}
        if sec:
            for label, key in (("graphics", "Graphics"), ("sm", "SM"), ("memory", "Memory"), ("video", "Video")):
                v = _mhz(sec.get(key))
                if v is not None:
                    out[label] = v
        return out

    info.clocks = clocks(gpu.get("Clocks") if isinstance(gpu.get("Clocks"), dict) else None)
    info.max_clocks = clocks(gpu.get("Max Clocks") if isinstance(gpu.get("Max Clocks"), dict) else None)

    reasons = _section(gpu, "clocks event reasons", "clocks throttle reasons")
    if reasons is not None:
        info.has_limiter_info = True
        info.limiters = [k for k, v in reasons.items() if isinstance(v, str) and v.strip().lower() == "active"]
    return info


def query_power_page(run=proc.run):
    """(DetailInfo | None, error). Wakes the GPU - only call when it is awake or the user asked."""
    r = run(["nvidia-smi", "-q", "-d", "POWER,CLOCK,PERFORMANCE"], timeout=10)
    if not r.ok:
        return None, r.error_text
    return parse_power_page(r.out), None


# --------------------------------------------------------------------------------------------------------------
# Privileged commands: only built here, run by the user in a terminal.
# --------------------------------------------------------------------------------------------------------------

def power_limit_command(watts, info=None):
    """['sudo', 'nvidia-smi', '-pl', 'W'] after checking the value against the card's min/max when known."""
    try:
        w = int(watts)
    except (TypeError, ValueError):
        raise ValueError("power limit must be a whole number of watts")
    if w <= 0:
        raise ValueError("power limit must be positive")
    if info is not None:
        if info.min_w is not None and w < int(info.min_w):
            raise ValueError(f"below the card minimum of {info.min_w:g} W")
        if info.max_w is not None and w > int(info.max_w):
            raise ValueError(f"above the card maximum of {info.max_w:g} W")
    return ["sudo", "nvidia-smi", "-pl", str(w)]


def persistence_command(enable):
    return ["sudo", "nvidia-smi", "-pm", "1" if enable else "0"]
