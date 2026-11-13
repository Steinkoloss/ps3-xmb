#!/bin/sh
# Install the PS3 XMB wallpaper for KDE Plasma 6 from your own PS3 firmware update.
#
#   ./install.sh /path/to/PS3UPDAT.PUP     build, import the firmware data, install
#   ./install.sh                           rebuild and reinstall (keeps imported data)
#
# PS3UPDAT.PUP is Sony's system software 4.93 update; it is checked by SHA-256.
set -e
root=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-$HOME/.local/share}/ps3-xmb
build=$root/build/release

fail() { echo "install.sh: $*" >&2; exit 1; }
for tool in cmake python3 kpackagetool6; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is missing (see README.md, Install)"
done
python3 -c "import cryptography" 2>/dev/null || fail "python3 cryptography is missing (python-cryptography / python3-cryptography)"

if [ -n "$1" ]; then
    [ -f "$1" ] || fail "no such file: $1"
    python3 "$root/native/tools/import_data.py" "$1" --output "$data"
elif [ ! -f "$data/cpu/renderer-image.bin" ]; then
    fail "pass your PS3UPDAT.PUP (firmware 4.93) the first time: ./install.sh /path/to/PS3UPDAT.PUP"
fi

echo "Building..."
generator=
command -v ninja >/dev/null 2>&1 && generator="-G Ninja"
# shellcheck disable=SC2086
cmake -S "$root/native" -B "$build" $generator -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" --parallel --target moyou-qml moyou-preview

MOYOU_BUILD=$build sh "$root/native/plasma/install.sh"

if systemctl --user is-active --quiet plasma-plasmashell 2>/dev/null; then
    echo "Restarting the Plasma shell to load the wallpaper..."
    systemctl --user restart plasma-plasmashell
fi
echo "Done. Right-click the desktop > Desktop and Wallpaper > Wallpaper type: PS3 XMB Waves."
