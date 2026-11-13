#!/usr/bin/env python3
"""Import everything the wallpaper needs from your own PS3 firmware update, in one step.

  python3 tools/import_data.py [/path/to/PS3UPDAT.PUP] [--output ~/.local/share/ps3-xmb]

Without a file, downloads the current PS3UPDAT.PUP from Sony's PS3 update server (about
200 MB), uses it and deletes it. Every file taken from the update must match the verified 4.93
renderer (tools/firmware_inputs.json). Writes:
  reference/                    menus, textures, shaders and the decrypted renderer
  shaders/                      the renderer's RSX shaders translated to GLSL
  cpu/renderer-image.bin        the renderer loaded at its address, relocated
No firmware code is executed. The output is your firmware's data: keep it local.
"""
import argparse
import os
import shutil
import struct
import tarfile
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_firmware import write_reference  # noqa: E402
from renderer_image import renderer_image  # noqa: E402
from translate_shaders import translate  # noqa: E402


UPDATE_LIST = "http://fus01.ps3.update.playstation.net/update/ps3/list/{region}/ps3-updatelist.txt"
AGENT = {"User-Agent": "ps3-xmb-importer"}


def update_url(region="us"):
    """The full system update (PS3UPDAT.PUP) listed on Sony's update server."""
    url = os.environ.get("PS3_XMB_UPDATE_LIST", UPDATE_LIST.format(region=region))
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=30) as response:
        listing = response.read().decode("ascii", "replace")
    for line in listing.splitlines():
        for field in line.split(";"):
            if field.startswith("CDN=") and field.endswith("/PS3UPDAT.PUP"):
                return field[4:]
    raise ValueError("Sony's update list has no full system update")


def download(url, destination):
    with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=60) as response, \
            open(destination, "wb") as file:
        total, done, shown = int(response.headers.get("Content-Length") or 0), 0, -1
        while chunk := response.read(1 << 20):
            file.write(chunk)
            done += len(chunk)
            if total and done * 100 // total != shown:
                shown = done * 100 // total
                print(f"\rDownloading PS3 system software from Sony... {shown}%", end="", flush=True)
    print()


def import_data(firmware, output):
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ps3-xmb-import-", dir=output.parent) as temporary:
        stage = Path(temporary) / "data"
        stage.mkdir()
        if firmware is None:
            firmware = Path(temporary) / "PS3UPDAT.PUP"
            download(update_url(), firmware)
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
    parser.add_argument("firmware", type=Path, nargs="?", help="PS3UPDAT.PUP (default: download from Sony)")
    parser.add_argument("--output", type=Path, default=Path("~/.local/share/ps3-xmb"))
    args = parser.parse_args()
    try:
        print(f"Imported into {import_data(args.firmware.expanduser() if args.firmware else None, args.output)}")
    except (ValueError, OSError, KeyError, struct.error, tarfile.TarError) as error:
        parser.exit(1, f"Import failed: {error}\n")


if __name__ == "__main__":
    main()
