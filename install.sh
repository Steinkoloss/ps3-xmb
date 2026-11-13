#!/bin/sh
# Build and install the PS3 XMB wallpaper for KDE Plasma 6 from this source checkout.
# (Prefer the prebuilt ps3-xmb-installer.run from the releases page: download, run.)
#
#   ./install.sh                           first time: also downloads Sony's PS3 system
#                                          software (about 200 MB) and takes the renderer's
#                                          data from it; later: rebuild and reinstall
#   ./install.sh /path/to/PS3UPDAT.PUP     use an update file you already have
set -e
root=$(cd "$(dirname "$0")" && pwd)
build=$root/build/release
command -v cmake >/dev/null 2>&1 || { echo "install.sh: cmake is missing (see README.md)" >&2; exit 1; }
echo "Building..."
generator=
command -v ninja >/dev/null 2>&1 && generator="-G Ninja"
# shellcheck disable=SC2086
cmake -S "$root/native" -B "$build" $generator -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" --parallel --target moyou-qml moyou-preview
exec sh "$root/native/plasma/setup.sh" "$build/qml/PS3Xmb" "$root/native/plasma/package" "$root/native/tools" "$@"
