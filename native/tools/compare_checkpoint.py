#!/usr/bin/env python3
"""Compare canonical simulation/GPU buffers against an immutable reference.

The capture adapters must emit the same byte order, channel order, layout and
external-input identity. This check applies no color conversion or tolerance.
The reference must originate from the original renderer, not from this port.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def read_checkpoint(path):
    document = json.loads(path.read_text())
    if document.get("schema") != "moyou.checkpoint.v1":
        raise ValueError("Unsupported checkpoint schema")
    for key in ("firmware_sha256", "inputs_sha256"):
        value = document.get(key)
        if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError(f"Invalid checkpoint identity: {key}")
    if type(document.get("frame")) is not int or document["frame"] < 0:
        raise ValueError("Invalid checkpoint frame index")
    buffers = document.get("buffers")
    if not isinstance(buffers, dict) or not buffers:
        raise ValueError("Checkpoint has no buffers")
    payloads = {}
    for name, entry in buffers.items():
        filename = entry.get("path", "")
        relative = PurePosixPath(filename)
        if (not filename or str(relative) != filename or relative.is_absolute()
                or ".." in relative.parts or "\\" in filename or ":" in filename):
            raise ValueError("Unsafe checkpoint buffer path")
        location = path.parent / filename
        if location.is_symlink() or not location.resolve().is_relative_to(path.parent.resolve()):
            raise ValueError("Checkpoint buffer escapes its directory")
        payload = location.read_bytes()
        if not entry.get("format") or not isinstance(entry["format"], str):
            raise ValueError("Checkpoint buffer has no canonical format")
        if entry.get("size") != len(payload) or entry.get("sha256") != hashlib.sha256(payload).hexdigest():
            raise ValueError(f"Checkpoint buffer was modified: {name}")
        payloads[name] = payload
    return document, payloads


def compare(reference_path, candidate_path):
    reference, expected = read_checkpoint(reference_path)
    candidate, actual = read_checkpoint(candidate_path)
    for field in ("firmware_sha256", "inputs_sha256", "frame"):
        if reference[field] != candidate[field]:
            raise ValueError(f"Cannot compare different {field}")
    if expected.keys() != actual.keys():
        raise ValueError("Checkpoint buffer inventories differ")
    differences = []
    for name, left in expected.items():
        a = reference["buffers"][name]
        b = candidate["buffers"][name]
        for key in ("format", "width", "height", "stride"):
            if a.get(key) != b.get(key):
                raise ValueError(f"Checkpoint buffer layouts differ: {name}/{key}")
        right = actual[name]
        if left == right:
            continue
        offset = next((index for index, (x, y) in enumerate(zip(left, right)) if x != y), min(len(left), len(right)))
        differences.append({"buffer": name, "first_byte": offset,
                            "reference_size": len(left), "candidate_size": len(right)})
    return differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    try:
        differences = compare(args.reference, args.candidate)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(2, f"Checkpoint validation failed: {error}\n")
    if differences:
        parser.exit(1, json.dumps(differences, indent=2) + "\n")
    print("All canonical buffers match byte for byte")


if __name__ == "__main__":
    main()
