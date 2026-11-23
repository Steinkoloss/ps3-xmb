# PS3 XMB wave background: live wallpaper for Windows and Linux

The animated **PlayStation 3 XMB wave background** (the flowing ribbon and sparkles behind the
PS3 home menu) as a **live desktop wallpaper** for **Windows 10/11** and **KDE Plasma 6** on
Linux. It changes colour with the month and with the time of day, exactly like the console.

This is not a video loop or a remake: the PS3's own renderer (firmware 4.93) is reimplemented
in native code and runs the original shaders, parameters and wave simulation, taken from Sony's
free PS3 system software when you install. Nothing is redrawn by hand. It is light on the CPU:
about a quarter of one core for two screens (1440p + 1080p) at 60 fps on a Ryzen 7 5700X3D.

**[Download the latest release](https://github.com/Steinkoloss/ps3-xmb/releases/latest)**

## Install

### Windows 10/11

Download `PS3-XMB-Setup.exe` from the [latest release](https://github.com/Steinkoloss/ps3-xmb/releases/latest)
and run it (no administrator rights needed). Windows may say "Windows protected your PC" because
the installer isn't code-signed: click **More info**, then **Run anyway**. On first start the app
asks to download Sony's PS3 system software (about 200 MB, from Sony) and takes the renderer's
data from it, then draws the background behind the desktop icons across all monitors. It pauses
while a fullscreen app or game is in front and starts with Windows (tray icon: Pause, Start with
Windows, Quit). It needs a graphics driver with OpenGL 4.3 (any GPU from about 2012 on).

### Linux (KDE Plasma 6)

Download `ps3-xmb-installer.run` from the [latest release](https://github.com/Steinkoloss/ps3-xmb/releases/latest)
and run it:

```bash
sh ps3-xmb-installer.run
```

It needs Qt 6.10 or newer, `python3` and `openssl` (preinstalled on most desktops). The
installer downloads Sony's free PS3 system software update (about 200 MB) directly from Sony,
takes the renderer's shaders, parameters and textures out of it into `~/.local/share/ps3-xmb`
(every file checked by SHA-256), deletes the download, installs the wallpaper and switches every
screen to it. Already have `PS3UPDAT.PUP` (4.93)? `sh ps3-xmb-installer.run PS3UPDAT.PUP` uses
it instead. Set `PS3_XMB_KEEP_WALLPAPER=1` to install without switching.

None of Sony's files are part of this project.

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
