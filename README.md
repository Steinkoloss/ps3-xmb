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

## Plasma wallpaper

```bash
native/plasma/install.sh
```

Installs the firmware-derived data to `~/.local/share/ps3-xmb`, the QML module and the
`org.ps3xmb.waves` wallpaper (KDE Plasma 6), then pick "PS3 XMB Waves" in Desktop and
Wallpaper. Screens using it share one simulation and show one scene across the desktop. The
layout is physical: each screen keeps its real size (from its reported millimetres, 96 dpi if
unknown), and screens that touch in the display settings touch in the scene, aligned at the
nearer edge. The scene's axis runs along the line through the screen centres, snapped to
exactly horizontal or vertical within 20° (setting "Across screens": Automatic, Always level,
Exact angle), so a slightly offset pair shows a level weave between
their centres and a clearly diagonal arrangement shows it tilted. It follows layout changes live.
`moyou-preview --screens "x,y,w,h;…" --screen N` renders one screen of such a layout (mm). Settings are shared by all screens
(`~/.config/ps3-xmb.conf`): applying them in any screen's dialog changes every screen.

## Test

```bash
native/check.sh
```

## Performance

The hot stages run as native ports of the original code, checked against the translations
(`tests/test_native_stages.py`; `MOYOU_VERIFY_NATIVE=1` compares every update):
- SPU surface stages (FFD projection, bicubic patches; `SurfaceEvaluator`): equal to float
  rounding. `MOYOU_SURFACE=original` runs the translation.
- PPU particle loop 0x0205d7b0 (`RopeSolver::particleUpdate`) and the math module's
  sine/cosine: byte-identical particle state. `MOYOU_PARTICLES=original` runs the translation.

Everything else is translated code: hot VMX instructions use SSE versions
(`tools/ppu_vmx_sse.h`, staged by `lift_rope.py`, bit-identical to the ps3recomp scalar
helpers), guest memory accessors inline with their permission check, and guest code builds
for `x86-64-v3` (`MOYOU_GUEST_OPTIMIZATION`). Particles run beside the next rope/surface
update; the FFD lattice is read back asynchronously; exposure tables are cached and only the
dither noise is regenerated (native port, checked by `--noise-check`).
