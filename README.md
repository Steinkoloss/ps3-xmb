# PS3 XMB background

Native port of the PlayStation 3 XMB wave background (firmware 4.93). The original
renderer's CPU, SPU and RSX programs are translated and executed on x86/OpenGL;
nothing is re-drawn by hand and no captured animation is replayed.

- `player/` — offline HTML player for a captured 1080p clip of the real console.
- `native/` — the port: import tools, translators, Qt/OpenGL renderer, tests.

Firmware data and every file derived from it stay in `build/` (ignored). Do not
commit them.

## Local layout (`build/`, ignored)

| Path | Contents |
|---|---|
| `inputs/PS3-XMB-4.93.PUP` | Sony 4.93 update, sha256 `158471fd…cf365` |
| `third_party/ps3recomp` | ps3recomp at `4e8f3ca8` (MIT) |
| `third_party/scetool` | scetool checkout |
| `work/reference` | `import_firmware.py` output |
| `work/native-verified` | `lift_simulation.py` output (SPU translations) |
| `work/cpu-particles` | `lift_rope.py` output (PPU translations incl. particles, current) |
| `work/native-shaders-hdr` | `translate_shaders.py` output (current) |
| `work/decompiler` | Ghidra project used by `PrepareMoyou.java` / `ExportMoyou.java` |
| `qt` | CMake build tree |

## Build

```bash
W=$PWD/build/work
cmake -S native -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOYOU_CPU_DIRECTORY=$W/cpu-particles -DMOYOU_SPU_DIRECTORY=$W/native-verified \
  -DMOYOU_RECOMPILER_DIRECTORY=$PWD/build/third_party/ps3recomp
cmake --build build/qt
```

Translated guest code builds with `MOYOU_GUEST_OPTIMIZATION` (default
`-O2;-march=x86-64-v2`) plus LTO. `-DMOYOU_GUEST_OPTIMIZATION=-O0` is the
reference build; optimized output must stay bit-identical to it (compare
`--output` PNGs and `--mesh-checkpoint` files).

## Run

```bash
W=build/work
build/qt/moyou-preview --shaders $W/native-shaders-hdr --reference $W/reference \
  --cpu $W/cpu-particles --spline $W/native-verified/spline/original-ls.bin --preset day
```

## Test

```bash
native/check.sh
```

## Performance

Measured at 1280×720 on this machine: 60 fps uses about 1.5–2 cores, the 30 fps
setting about 1 core, and a hidden or unrendered wallpaper ~0% (the simulation only
steps when the renderer takes frames). The cost is the original PPU/SPU maths:
- SPU surface evaluation runs on up to 4 threads (`MOYOU_SPU_THREADS`), bit-identical
  to one thread.
- Particles run beside the next rope/surface update; frames carry the previous
  update's particles.
- The FFD lattice is read back asynchronously; exposure tables are cached and only
  the dither noise is regenerated (native port, checked by `--noise-check`).
- Guest memory access uses a 4 KiB page permission table over the exact region test.
