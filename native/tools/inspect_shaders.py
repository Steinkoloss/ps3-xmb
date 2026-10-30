#!/usr/bin/env python3
"""Decode all original RSX shaders into explicit, lossless instruction records."""

import argparse
import collections
import hashlib
import json
from pathlib import Path

from cg import Program


def inspect(reference, output):
    manifest = json.loads((reference / "manifest.json").read_text())
    if manifest.get("schema") != "moyou.original-inputs.v1":
        raise ValueError("Expected an original-input reference manifest")
    decoded = {}
    inventory = collections.Counter()
    for name, identity in manifest["files"].items():
        if not name.endswith((".fpo", ".vpo")):
            continue
        data = (reference / name).read_bytes()
        if len(data) != identity["size"] or hashlib.sha256(data).hexdigest() != identity["sha256"]:
            raise ValueError(f"Original shader was modified: {name}")
        program = Program(data)
        decoded[name] = {"sha256": identity["sha256"], **program.to_dict()}
        if program.profile == 7004:
            inventory.update(instruction.mnemonic for instruction in program.instructions)
    result = {
        "schema": "moyou.rsx-instructions.v1",
        "firmware_sha256": manifest["firmware_sha256"],
        "programs": decoded,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return len(decoded), inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        count, inventory = inspect(args.reference, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Shader inspection failed: {error}\n")
    print(f"Decoded {count} verified original programs")
    print(dict(sorted(inventory.items())))


if __name__ == "__main__":
    main()
