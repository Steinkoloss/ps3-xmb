#!/bin/sh
# Install the PS3 XMB wallpaper for the current user (Plasma 6).
#   data    -> ~/.local/share/ps3-xmb          (firmware-derived; local only)
#   module  -> ~/.local/lib/qt6/qml/PS3Xmb
#   package -> ~/.local/share/plasma/wallpapers/org.ps3xmb.waves
#   env     -> ~/.config/environment.d/ps3-xmb.conf (module path for the plasmashell service)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$root/build/work
build=${MOYOU_BUILD:-$root/build/qt}
data=${XDG_DATA_HOME:-$HOME/.local/share}/ps3-xmb
qml=$HOME/.local/lib/qt6/qml

test -f "$build/qml/PS3Xmb/libmoyou-qml.so" || { echo "Build $build first (README.md)" >&2; exit 1; }

rm -rf "$data.new"
mkdir -p "$data.new/cpu" "$data.new/spline"
cp -r "$work/native-shaders-hdr" "$data.new/shaders"
cp -r "$work/reference" "$data.new/reference"
cp "$work/cpu-particles/renderer-image.bin" "$work/cpu-particles/math-image.bin" "$data.new/cpu/"
cp "$work/native-verified/spline/original-ls.bin" "$data.new/spline/"
rm -rf "$data" && mv "$data.new" "$data"

mkdir -p "$qml"
rm -rf "$qml/PS3Xmb"
cp -r "$build/qml/PS3Xmb" "$qml/PS3Xmb"

package=$root/native/plasma/package
if kpackagetool6 --type Plasma/Wallpaper --show org.ps3xmb.waves >/dev/null 2>&1; then
    kpackagetool6 --type Plasma/Wallpaper --upgrade "$package"
else
    kpackagetool6 --type Plasma/Wallpaper --install "$package"
fi

# plasmashell runs as a systemd user service: environment.d reaches it on every start.
environment=${XDG_CONFIG_HOME:-$HOME/.config}/environment.d
mkdir -p "$environment"
printf 'QML_IMPORT_PATH=%s\n' "$qml" > "$environment/ps3-xmb.conf"
rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/plasma-workspace/env/ps3-xmb.sh"
systemctl --user set-environment QML_IMPORT_PATH="$qml" 2>/dev/null || true

echo "Installed. Restart plasmashell (systemctl --user restart plasma-plasmashell) or log in again,"
echo "then pick 'PS3 XMB Waves' under Desktop and Wallpaper."
