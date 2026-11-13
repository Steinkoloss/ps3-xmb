# PS3 XMB background

The PlayStation 3 XMB wave background (firmware 4.93) as a KDE Plasma 6 wallpaper. The
console's own renderer is reimplemented in native code and runs the original shaders,
parameters and simulation from your firmware update. Nothing is redrawn by hand and no
recorded video is played back.

## Install

Download `ps3-xmb-installer.run` from the Releases page and run it:

```bash
sh ps3-xmb-installer.run
```

That's it. It needs KDE Plasma 6, `python3` and `openssl` (both preinstalled on most
desktops). The installer downloads Sony's free PS3 system software update (about 200 MB)
directly from Sony's update server, takes the renderer's shaders, parameters and textures
out of it into `~/.local/share/ps3-xmb` (every file checked by SHA-256), deletes the
download, installs the wallpaper and switches every screen to it. None of Sony's files are
part of this project. Already have `PS3UPDAT.PUP` (4.93)? `sh ps3-xmb-installer.run
PS3UPDAT.PUP` uses it instead. Set `PS3_XMB_KEEP_WALLPAPER=1` to install without switching.

### From source

Install the build dependencies (Qt 6.9 or newer), then run `./install.sh` (same options):
- Arch: `sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative`
- Fedora: `sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtdeclarative-devel`
- Debian/Ubuntu: `sudo apt install build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev libgl-dev libopengl-dev`

### Multiple screens

Screens using the wallpaper share one simulation and show one scene across the desktop. The
layout is physical: each screen keeps its real size (from its reported millimetres, 96 dpi if
unknown), and screens that touch in the display settings touch in the scene, aligned at the
nearer edge. The scene's axis runs along the line through the screen centres, snapped to
exactly horizontal or vertical within 20° (setting "Across screens": Automatic, Always level,
Exact angle). It follows layout changes live. Settings are shared by all screens
(`~/.config/ps3-xmb.conf`).

## Development

- `native/` — renderer (Qt/OpenGL), firmware import and shader translation tools, tests.

Firmware data and every file derived from it stay in `build/` (ignored). Do not commit them.

### Build and run

```bash
python3 native/tools/import_data.py /path/to/PS3UPDAT.PUP --output build/data
cmake -S native -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
build/release/moyou-preview --shaders build/data/shaders --reference build/data/reference \
  --cpu build/data/cpu --preset day
```

`moyou-preview --screens "x,y,w,h;…" --screen N` renders one screen of a multi-screen layout
(mm); `--output frame.png` renders offscreen.

### Verifying against the original programs

The native code was ported from translations of the firmware's own CPU and SPU programs and
is checked against them byte for byte. Those translations are firmware-derived and stay
local. To build with them, point CMake at them; this enables `MOYOU_VERIFY_NATIVE=1` (run the
original beside every native stage and report differences) and the `*=original` switches
(`MOYOU_PARTICLE_FRAME`, `MOYOU_PARTICLES`, `MOYOU_SURFACE`, `MOYOU_LIBM`).

```bash
W=$PWD/build/work
cmake -S native -B build/qt -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMOYOU_CPU_DIRECTORY=$W/cpu-particles -DMOYOU_SPU_DIRECTORY=$W/native-verified \
  -DMOYOU_RECOMPILER_DIRECTORY=$PWD/build/third_party/ps3recomp
cmake --build build/qt
native/check.sh
```

| Local path (`build/`) | Contents |
|---|---|
| `inputs/PS3-XMB-4.93.PUP` | Sony 4.93 update, sha256 `158471fd…cf365` |
| `third_party/ps3recomp` | ps3recomp at `4e8f3ca8` (MIT) |
| `work/reference` | `import_firmware.py` output |
| `work/native-verified` | `lift_simulation.py` output (SPU translations) |
| `work/cpu-particles` | `lift_rope.py` output (PPU translations, math image) |
| `work/native-shaders-hdr` | `translate_shaders.py` output |
| `work/decompiler` | Ghidra project used by `PrepareMoyou.java` / `ExportMoyou.java` |

## License

GPL-2.0-or-later (see `LICENSE`). Parts from other projects keep their notices:
`native/native/CellEstimate.h` (RPCS3, GPL-2.0-or-later), `native/native/SpuEstimate.h`
(after ps3recomp, MIT), `native/tools/sce.py` (format and public keys after RPCS3,
GPL-2.0-or-later). Firmware data
is not included and is not covered by this license; users supply their own update file.
