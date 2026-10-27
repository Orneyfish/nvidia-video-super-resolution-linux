#!/bin/bash
# Runs a command in a HEADLESS cage (tests/run_cage.sh) with a fake sysfs and fake tools, so a panel test can never
# wake the real GPU or open a window on the desktop.
#   usage: fake_env.sh [--runtime active|suspended] [--ac 0|1] [--vlc-running] [--calls FILE] [--home DIR] -- COMMAND...
# Fake sysfs root (NVC_SYSFS_ROOT): the NVIDIA GPU 0000:01:00.0 with the given runtime_status, the Intel iGPU, an AC
# supply. Fake tools first in PATH, each appending its argv to FILE (default: $TMPDIR/nvc-fake-calls.PID.log, kept; kitty logs to
# FILE.term and runs nothing): nvidia-smi (canned CSV, compute apps and the driver-615 `-q` text), supergfxctl (Hybrid),
# pgrep (finds nothing, or a VLC pid with --vlc-running), kitty. Prints "calls: FILE" on stderr first.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
runtime=active ac=1 vlc=0 calls="" home=""
while [ $# -gt 0 ]; do
  case $1 in
    --runtime) runtime=$2; shift 2 ;; --ac) ac=$2; shift 2 ;; --vlc-running) vlc=1; shift ;;
    --calls) calls=$2; shift 2 ;; --home) home=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "usage: fake_env.sh [--runtime active|suspended] [--ac 0|1] [--vlc-running] [--calls FILE] [--home DIR] -- COMMAND..." >&2; exit 2 ;;
  esac
done
[ $# -ge 1 ] || { echo "fake_env.sh: no command" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/nvc-fake.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
[ -n "$calls" ] || calls=${TMPDIR:-/tmp}/nvc-fake-calls.$$.log   # outside $work: the caller reads it after the run
: > "$calls"; : > "$calls.term"

root=$work/sys dev=$root/bus/pci/devices bin=$work/bin
mkdir -p "$dev/0000:01:00.0/power" "$dev/0000:01:00.1" "$dev/0000:00:02.0" "$root/bus/pci/drivers/nvidia" \
  "$root/module/nvidia" "$root/class/power_supply/AC0" "$root/class/power_supply/BAT0" "$bin"
g=$dev/0000:01:00.0
echo 0x10de > "$g/vendor"; echo 0x030000 > "$g/class"; echo 0x24a0 > "$g/device"; echo 0 > "$g/boot_vga"
echo "$runtime" > "$g/power/runtime_status"; echo auto > "$g/power/control"; echo 5000 > "$g/power/autosuspend_delay_ms"
[ "$runtime" = suspended ] && echo D3cold > "$g/power_state" || echo D0 > "$g/power_state"
echo 1 > "$g/d3cold_allowed"; ln -s ../../../../bus/pci/drivers/nvidia "$g/driver"
echo 0x10de > "$dev/0000:01:00.1/vendor"; echo 0x040300 > "$dev/0000:01:00.1/class"
echo 615.78.08 > "$root/module/nvidia/version"
echo 0x8086 > "$dev/0000:00:02.0/vendor"; echo 0x030000 > "$dev/0000:00:02.0/class"
echo Mains > "$root/class/power_supply/AC0/type"; echo "$ac" > "$root/class/power_supply/AC0/online"
echo Battery > "$root/class/power_supply/BAT0/type"

cat > "$bin/nvidia-smi" <<EOF
#!/bin/bash
echo "nvidia-smi \$*" >> "$calls"
case \$* in
  *--query-compute-apps*) echo "4242, /usr/bin/mpv, 512" ;;
  *--query-gpu*) echo "NVIDIA GeForce RTX 3070 Ti Laptop GPU, 615.78.08, 1312, 8192, 12, 47, 18.52, 125.00, 1230, 6000, 4, 16, [N/A], P0, Disabled" ;;
  *-q*) cat "$HERE/fixtures/smi_q_power_driver615.txt" ;;
esac
EOF
printf '#!/bin/bash\necho "supergfxctl $*" >> "%s"\n[ "$1" = -g ] && echo Hybrid\nexit 0\n' "$calls" > "$bin/supergfxctl"
if [ "$vlc" = 1 ]; then pg='echo 4711'; else pg='exit 1'; fi
printf '#!/bin/bash\necho "pgrep $*" >> "%s"\n%s\n' "$calls" "$pg" > "$bin/pgrep"
printf '#!/bin/bash\necho "kitty $*" >> "%s.term"\nexit 0\n' "$calls" > "$bin/kitty"
chmod +x "$bin"/*

echo "calls: $calls" >&2
args=()
[ -n "$home" ] && args=(--home "$home")
"$HERE/run_cage.sh" "${args[@]}" -- env PATH="$bin:$PATH" NVC_SYSFS_ROOT="$root" FAKE_CALLS="$calls" "$@"
rc=$?
exit $rc
