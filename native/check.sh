#!/bin/sh
# Run every check against the local build tree (see README.md).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$root/build/work
build=${MOYOU_BUILD:-$root/build/qt}
export MOYOU_REFERENCE=$work/reference MOYOU_NATIVE_SHADERS=$work/native-shaders-hdr \
    MOYOU_CPU_CORE=$work/cpu-particles MOYOU_NATIVE_BASELINE=$work/native-verified \
    MOYOU_PREVIEW=$build/moyou-preview MOYOU_ROPE_CHECKPOINT=$build/moyou-rope-checkpoint \
    MOYOU_SURFACE_CHECKPOINT=$build/moyou-surface-checkpoint
cd "$root/native"
PYTHONPATH=tools exec python3 -m unittest discover -s tests "$@"
