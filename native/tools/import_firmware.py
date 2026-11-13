#!/usr/bin/env python3
"""Prepare verified original inputs for the native XMB renderer.

Example:
  python3 tools/import_firmware.py /path/to/PS3UPDAT.PUP --output /path/to/reference

The supported input is Sony's unmodified 4.93 update. This command performs no
download, installation, desktop configuration, or execution of firmware code.
Its output contains local firmware data and must stay outside version control.
"""

import argparse
import collections
import hashlib
import io
import json
import os
import struct
import tarfile
import tempfile
from pathlib import Path

from qrc import resources
from sce import application_elf, package_sections, region, unpack


# The update the inputs were verified against. Another update is accepted when every file
# taken from it matches firmware_inputs.json (the renderer has not changed since).
PUP_SHA256 = "158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365"
EXPECTED_INPUTS = json.loads((Path(__file__).resolve().parent / "firmware_inputs.json").read_text())["files"]
RENDERER_SHA256 = "63cdee01a142bd4b4cf565da7a866513bc94e5f58e5bf2ece5b630007c3aa088"
SPLINE_SHA256 = "587f66dc18f88b6090db30bbbf8a54d569a9e2815271beeefd3b44b6b2f84eaa"
PARTICLES_SHA256 = "7861469441ff1d338ad503eeaa450faeb70c42f8f59764bcbbdeb24187061ea1"
TARGETS = {
    "dev_flash/vsh/resource/qgl/lines.qrc": "original/lines.qrc",
    "dev_flash/vsh/module/custom_render_plugin.sprx": "original/custom_render_plugin.sprx",
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def update_archive(data):
    if data[:8] != b"SCEUF\0\0\0":
        raise ValueError("Invalid PUP magic")
    _, _, count, header_size, data_size = unpack(data, 8, ">5Q")
    if count != 9 or header_size + data_size != len(data):
        raise ValueError("Unexpected PUP layout")
    for index in range(count):
        identity, offset, length = unpack(data, 48 + index * 32, ">3Q")
        if identity == 0x300:
            return region(data, offset, length)
    raise ValueError("PUP does not contain an update archive")


def renderer_inputs(pup):
    found = {}
    archive = update_archive(pup)
    with tarfile.open(fileobj=io.BytesIO(archive)) as outer:
        for entry in outer:
            if not entry.isfile() or not Path(entry.name).name.startswith("dev_flash_"):
                continue
            package = outer.extractfile(entry).read()
            for section in package_sections(package):
                try:
                    inner = tarfile.open(fileobj=io.BytesIO(section.data))
                except tarfile.TarError:
                    continue
                with inner:
                    for member in inner:
                        if member.name not in TARGETS or not member.isfile():
                            continue
                        target = TARGETS[member.name]
                        if target in found:
                            raise ValueError("Repeated original renderer input")
                        found[target] = inner.extractfile(member).read()
    if set(found) != set(TARGETS.values()):
        raise ValueError("The original renderer or its resource container is missing")
    found["custom_render_plugin.elf"] = application_elf(found["original/custom_render_plugin.sprx"])
    if digest(found["custom_render_plugin.elf"]) != RENDERER_SHA256:
        raise ValueError("Decrypted renderer does not match the verified image")
    for name, content in resources(found["original/lines.qrc"]):
        found[f"lines/{name}"] = content
    expected = {
        "lines/spurs/moyou/spline/spline.elf": SPLINE_SHA256,
        "lines/spurs/particles/particles/particles.elf": PARTICLES_SHA256,
    }
    for name, sha in expected.items():
        if digest(found.get(name, b"")) != sha:
            raise ValueError(f"Original simulation program does not match: {name}")
    if len(found) != 162:
        raise ValueError("Original renderer input inventory is incomplete")
    for name, content in found.items():
        expected = EXPECTED_INPUTS.get(name)
        if not expected or expected["sha256"] != digest(content) or expected["size"] != len(content):
            raise ValueError(f"This update's renderer differs from the verified 4.93 one ({name})")
    return found


def write_reference(pup_path, output):
    output = output.resolve()
    if output.exists():
        raise FileExistsError("Output already exists; use a new directory to preserve the reference")
    pup = pup_path.read_bytes()
    inputs = renderer_inputs(pup)
    manifest = {
        "schema": "moyou.original-inputs.v1",
        "firmware": "4.93 CEX",
        "firmware_sha256": PUP_SHA256,          # content identical to this update's
        "source_sha256": digest(pup),
        "source": "https://www.playstation.com/en-us/support/hardware/ps3/system-software/",
        "files": {name: {"size": len(content), "sha256": digest(content)}
                  for name, content in sorted(inputs.items())},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".moyou-import-", dir=output.parent) as temporary:
        stage = Path(temporary) / "reference"
        stage.mkdir()
        for name, content in inputs.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", newline="\n")
        os.rename(stage, output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = write_reference(args.firmware, args.output)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f"Import failed: {error}\n")
    counts = collections.Counter(Path(name).suffix for name in manifest["files"] if name.startswith("lines/"))
    print(f"Verified original {manifest['firmware']} renderer inputs: {dict(sorted(counts.items()))}")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
