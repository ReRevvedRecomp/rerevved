"""Validate the files in a completed SDK draw capture and record their digests."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


def summarize(capture: Path) -> dict:
    manifest_bytes = (capture / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if (
        not isinstance(manifest, dict)
        or manifest.get("schema_version") != 1
        or manifest.get("status") != "complete"
    ):
        raise ValueError("capture has no completed version-1 manifest")
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries:
        raise ValueError("capture has no file inventory")

    files = {}
    for entry in entries:
        name = entry["name"]
        size = entry["size_bytes"]
        if (
            not isinstance(name, str)
            or not name
            or "/" in name
            or "\\" in name
            or ":" in name
            or name in {".", ".."}
            or name in files
        ):
            raise ValueError("capture file names must be unique local basenames")
        if type(size) is not int or size <= 0:
            raise ValueError(f"invalid size for {name}")
        path = capture / name
        if path.stat().st_size != size:
            raise ValueError(f"capture file size mismatch: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        files[name] = {"size_bytes": size, "sha256": digest}

    for name in (
        "registers.bin",
        "vertex.ucode.bin",
        "pixel.ucode.bin",
        "edram_before.bin",
        "edram_after.bin",
    ):
        if name not in files:
            raise ValueError(f"capture is missing {name}")
    for name in ("registers.bin", "vertex.ucode.bin", "pixel.ucode.bin"):
        if files[name]["size_bytes"] % 4:
            raise ValueError(f"capture dwords are truncated: {name}")
    if (
        files["edram_before.bin"]["size_bytes"]
        != files["edram_after.bin"]["size_bytes"]
    ):
        raise ValueError("EDRAM snapshots have different sizes")

    changed_bytes = 0
    with (
        (capture / "edram_before.bin").open("rb") as before,
        (capture / "edram_after.bin").open("rb") as after,
    ):
        while before_chunk := before.read(1024 * 1024):
            after_chunk = after.read(len(before_chunk))
            changed_bytes += sum(
                a != b for a, b in zip(before_chunk, after_chunk, strict=True)
            )

    return {
        "schema_version": 1,
        "source_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "files": files,
        "edram_changed_bytes": changed_bytes,
        "scope": "file integrity and EDRAM byte difference; not native replay or visible parity",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        result = summarize(args.capture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Invalid draw capture: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
