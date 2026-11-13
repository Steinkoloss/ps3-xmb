#!/bin/sh
# PS3 XMB wallpaper for KDE Plasma 6 (GPL-2.0-or-later). Run this file to install it:
#   sh ps3-xmb-installer.run                  downloads Sony's free PS3 system software
#                                             (about 200 MB) and takes the renderer's data
#   sh ps3-xmb-installer.run PS3UPDAT.PUP     uses an update file you already have
# No Sony files are included. Needs Plasma 6, Qt @QT_VERSION@ or newer, python3, openssl.
set -e
need=@QT_VERSION@
qt_version() {   # from the real file name of libQt6Core.so.6 (libQt6Core.so.6.9.1)
    lib=$(ldconfig -p 2>/dev/null | awk '/libQt6Core\.so\.6 /{print $NF; exit}')
    [ -n "$lib" ] && basename "$(readlink -f "$lib")" | sed -n 's/^libQt6Core\.so\.//p'
}
have=$(qt_version || true)
if [ -n "$have" ] && [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | head -n1)" != "$need" ]; then
    echo "PS3 XMB: this build needs Qt $need or newer; this system has Qt $have." >&2
    exit 1
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
line=$(awk '/^__PAYLOAD_BELOW__$/ { print NR + 1; exit }' "$0")
tail -n +"$line" "$0" | tar -xzf - -C "$work"
firmware=
[ -n "$1" ] && firmware=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
sh "$work/setup.sh" "$work/module" "$work/package" "$work/tools" $firmware
exit 0
__PAYLOAD_BELOW__
