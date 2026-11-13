#!/bin/sh
# Install the PS3 XMB wallpaper for the current user (KDE Plasma 6), from a built module.
# Used by ../../install.sh (source checkout) and by the self-extracting installer.
#
#   setup.sh MODULE_DIR PACKAGE_DIR TOOLS_DIR [PS3UPDAT.PUP]
#
#   data    -> ~/.local/share/ps3-xmb    taken from Sony's update (downloaded unless given)
#   module  -> ~/.local/lib/qt6/qml/PS3Xmb
#   package -> ~/.local/share/plasma/wallpapers/org.ps3xmb.waves
#   env     -> ~/.config/environment.d/ps3-xmb.conf (module path for the plasmashell service)
# PS3_XMB_KEEP_WALLPAPER=1 installs without switching the screens to it.
set -e
module=$1 package=$2 tools=$3 firmware=$4
data=${XDG_DATA_HOME:-$HOME/.local/share}/ps3-xmb
qml=$HOME/.local/lib/qt6/qml

fail() { echo "PS3 XMB: $*" >&2; exit 1; }
[ -f "$module/libmoyou-qml.so" ] || fail "no built module in $module"
command -v kpackagetool6 >/dev/null 2>&1 || fail "KDE Plasma 6 is required (kpackagetool6 not found)"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
python3 -c "import cryptography" 2>/dev/null || command -v openssl >/dev/null 2>&1 ||
    fail "the openssl command (or Python's cryptography) is required"

if [ -n "$firmware" ]; then
    [ -f "$firmware" ] || fail "no such file: $firmware"
    python3 "$tools/import_data.py" "$firmware" --output "$data"
elif [ ! -f "$data/cpu/renderer-image.bin" ]; then
    python3 "$tools/import_data.py" --output "$data"
fi

mkdir -p "$qml"
rm -rf "$qml/PS3Xmb"
cp -r "$module" "$qml/PS3Xmb"
if kpackagetool6 --type Plasma/Wallpaper --show org.ps3xmb.waves >/dev/null 2>&1; then
    kpackagetool6 --type Plasma/Wallpaper --upgrade "$package" >/dev/null
else
    kpackagetool6 --type Plasma/Wallpaper --install "$package" >/dev/null
fi

# plasmashell runs as a systemd user service: environment.d reaches it on every start.
environment=${XDG_CONFIG_HOME:-$HOME/.config}/environment.d
mkdir -p "$environment"
printf 'QML_IMPORT_PATH=%s\n' "$qml" > "$environment/ps3-xmb.conf"
rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/plasma-workspace/env/ps3-xmb.sh"
systemctl --user set-environment QML_IMPORT_PATH="$qml" 2>/dev/null || true

if ! systemctl --user is-active --quiet plasma-plasmashell 2>/dev/null; then
    echo "Installed. Log in to Plasma and pick PS3 XMB Waves in Desktop and Wallpaper."
    exit 0
fi
echo "Restarting the Plasma shell to load the wallpaper..."
systemctl --user restart plasma-plasmashell
if [ -n "$PS3_XMB_KEEP_WALLPAPER" ] || ! command -v dbus-send >/dev/null 2>&1; then
    echo "Installed. Pick PS3 XMB Waves in Desktop and Wallpaper."
    exit 0
fi
script='var a = desktops(); for (var i = 0; i < a.length; ++i) a[i].wallpaperPlugin = "org.ps3xmb.waves"; print(a.length);'
tries=0
until dbus-send --session --print-reply --dest=org.kde.plasmashell /PlasmaShell \
        org.kde.PlasmaShell.evaluateScript string:"$script" 2>/dev/null | grep -q 'string "[1-9]'; do
    tries=$((tries + 1))
    [ $tries -ge 30 ] && { echo "Installed. Pick PS3 XMB Waves in Desktop and Wallpaper."; exit 0; }
    sleep 1
done
echo "Done: PS3 XMB Waves is now the wallpaper on every screen."
