#!/bin/sh
# Pack a built tree into ps3-xmb-installer.run: one file that installs the wallpaper.
#   native/package/make-installer.sh BUILD_DIR OUTPUT.run
# Build on the oldest Qt and glibc you want to support (CI: Fedora 42, Qt 6.9); the plugin
# runs on that Qt version or newer.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "$1" && pwd) output=$2
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -r "$build/qml/PS3Xmb" "$stage/module"
cp -r "$root/native/plasma/package" "$stage/package"
cp "$root/native/plasma/setup.sh" "$root/LICENSE" "$stage/"
mkdir "$stage/tools"
for file in import_data.py import_firmware.py renderer_image.py translate_shaders.py sce.py qrc.py cg.py \
        glsl_common.py glsl_fragment.py glsl_vertex.py firmware_inputs.json; do
    cp "$root/native/tools/$file" "$stage/tools/"
done
qt_version() {   # from the real file name of libQt6Core.so.6 (libQt6Core.so.6.9.1)
    lib=$(ldconfig -p 2>/dev/null | awk '/libQt6Core\.so\.6 /{print $NF; exit}')
    [ -n "$lib" ] && basename "$(readlink -f "$lib")" | sed -n 's/^libQt6Core\.so\.//p'
}
qt=$(qt_version)
[ -n "$qt" ] || { echo "make-installer.sh: cannot find libQt6Core" >&2; exit 1; }
{
    sed "s/@QT_VERSION@/$qt/" "$root/native/package/installer-header.sh"
    tar -C "$stage" -czf - .
} > "$output"
chmod +x "$output"
echo "$output (built against Qt $qt, $(du -h "$output" | cut -f1))"
