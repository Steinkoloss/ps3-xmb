#!/bin/sh
# Install the wallpaper module and package for the current user (Plasma 6). ../../install.sh
# runs this after building and importing; the firmware data is in ~/.local/share/ps3-xmb.
#   module  -> ~/.local/lib/qt6/qml/PS3Xmb
#   package -> ~/.local/share/plasma/wallpapers/org.ps3xmb.waves
#   env     -> ~/.config/environment.d/ps3-xmb.conf (module path for the plasmashell service)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${MOYOU_BUILD:-$root/build/release}
qml=$HOME/.local/lib/qt6/qml

test -f "$build/qml/PS3Xmb/libmoyou-qml.so" || { echo "Build $build first (README.md)" >&2; exit 1; }

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

