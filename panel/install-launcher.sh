#!/bin/sh
# Installs (or removes) the NV Control launcher: ~/.local/share/applications/org.nvvsr.Control.desktop and its icon.
# usage: install-launcher.sh [--uninstall] [--home DIR]      (--home is for tests; default is $HOME)
# Idempotent. Never touches anything outside <home>/.local/share. shortcut: XDG_DATA_HOME is ignored on purpose, so
# --home can never write into the real data directory.
set -eu

ID=org.nvvsr.Control
uninstall=0
home=${HOME:-}
while [ $# -gt 0 ]; do
  case $1 in
    --uninstall) uninstall=1 ;;
    --home) [ $# -ge 2 ] || { echo "--home needs a directory" >&2; exit 2; }; home=$2; shift ;;
    -h|--help) sed -n '2,3p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$home" ] || { echo "no home directory (set HOME or pass --home)" >&2; exit 2; }

here=$(cd "$(dirname "$0")" && pwd -P)
apps=$home/.local/share/applications
icons=$home/.local/share/icons/hicolor/scalable/apps
desktop=$apps/$ID.desktop
icon=$icons/$ID.svg

refresh() { command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" 2>/dev/null || true; }

if [ "$uninstall" = 1 ]; then
  rm -f "$desktop" "$icon"
  [ -d "$apps" ] && refresh
  echo "removed $desktop and $icon (if they existed)"
  exit 0
fi

case $here in
  *[\"\`\$\\%]*) echo "refusing a path with quotes, \$, backslash or %: $here" >&2; exit 1 ;;
  *[!A-Za-z0-9_./+-]*) exec_path="\"$here/main.py\"" ;;
  *) exec_path="$here/main.py" ;;
esac
[ -f "$here/main.py" ] && [ -f "$here/icons/hicolor/scalable/apps/$ID.svg" ] || { echo "main.py or the icon is missing in $here" >&2; exit 1; }

mkdir -p "$apps" "$icons"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/$ID.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=NV Control
GenericName=GPU control panel
Comment=NVIDIA GPU status, power and super resolution settings
Exec=/usr/bin/python3 $exec_path
Icon=$ID
Terminal=false
Categories=System;Settings;HardwareSettings;
Keywords=NVIDIA;GPU;Super Resolution;
StartupWMClass=$ID
DESKTOP
if command -v desktop-file-validate >/dev/null 2>&1; then
  desktop-file-validate "$tmp/$ID.desktop" || { echo "desktop-file-validate rejected the launcher; nothing installed" >&2; exit 1; }
fi

changed=0
if ! cmp -s "$tmp/$ID.desktop" "$desktop" 2>/dev/null; then
  install -m 755 "$tmp/$ID.desktop" "$desktop"; changed=1
fi
if ! cmp -s "$here/icons/hicolor/scalable/apps/$ID.svg" "$icon" 2>/dev/null; then
  install -m 644 "$here/icons/hicolor/scalable/apps/$ID.svg" "$icon"; changed=1
fi
[ "$changed" = 1 ] && { refresh; echo "installed $desktop"; } || echo "already up to date: $desktop"
