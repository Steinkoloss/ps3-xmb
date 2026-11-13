#!/usr/bin/env python3
"""Import everything the wallpaper needs from your own PS3 firmware update, in one step.

  python3 tools/import_data.py /path/to/PS3UPDAT.PUP --output ~/.local/share/ps3-xmb

Reads Sony's unmodified 4.93 update (PS3UPDAT.PUP) and writes, all checked by SHA-256:
  reference/                    menus, textures, shaders and the decrypted renderer
  shaders/                      the renderer's RSX shaders translated to GLSL
  cpu/renderer-image.bin        the renderer loaded at its address, relocated
Nothing is downloaded or executed. The output is your firmware's data: keep it local.
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_firmware import write_reference  # noqa: E402
from renderer_image import renderer_image  # noqa: E402
from translate_shaders import translate  # noqa: E402


def import_data(firmware, output):
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ps3-xmb-import-", dir=output.parent) as temporary:
        stage = Path(temporary) / "data"
        stage.mkdir()
        print("Checking and unpacking the firmware...", flush=True)
        write_reference(firmware, stage / "reference")
        print("Translating shaders...", flush=True)
        translate(stage / "reference", stage / "shaders")
        print("Relocating the renderer...", flush=True)
        (stage / "cpu").mkdir()
        (stage / "cpu" / "renderer-image.bin").write_bytes(
            renderer_image((stage / "reference" / "custom_render_plugin.elf").read_bytes()))
        old = Path(temporary) / "old"
        if output.exists():
            os.rename(output, old)
        os.rename(stage, output)
        if old.exists():
            shutil.rmtree(old)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("firmware", type=Path, help="PS3UPDAT.PUP, firmware 4.93")
    parser.add_argument("--output", type=Path, default=Path("~/.local/share/ps3-xmb"))
    args = parser.parse_args()
    try:
        print(f"Imported into {import_data(args.firmware.expanduser(), args.output)}")
    except (ValueError, OSError, KeyError, struct.error) as error:
        parser.exit(1, f"Import failed: {error}\n")


if __name__ == "__main__":
    main()
