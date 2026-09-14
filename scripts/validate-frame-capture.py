"""Validate a completed guest frame journal and hash its private capture files."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate(capture: Path) -> dict:
    capture = capture.resolve()
    manifest_bytes = (capture / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    require(
        manifest["schema_version"] == 1
        and manifest["status"] == "complete"
        and manifest["valid"] is True
        and manifest["failure_reason"] is None,
        "capture has no completed version-1 manifest",
    )
    files = {}
    for entry in manifest["files"]:
        name, size = entry["path"], entry["size"]
        require(
            isinstance(name, str)
            and bool(name)
            and not any(char in name for char in "/\\:")
            and name not in {".", ".."}
            and name not in files,
            "capture file names must be unique local basenames",
        )
        path = capture / name
        require(path.resolve().parent == capture, "capture file escapes directory")
        require(type(size) is int and size > 0, f"invalid file size: {name}")
        require(path.stat().st_size == size, f"capture file size mismatch: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        files[name] = {"kind": entry["kind"], "size": size, "sha256": digest}

    require(files.get("journal.json", {}).get("kind") == "journal", "journal missing")
    rdc = [name for name, file in files.items() if file["kind"] == "renderdoc"]
    require(len(rdc) == 1 and rdc[0].endswith(".rdc"), "one RenderDoc file required")
    require(
        Path(manifest["renderdoc"]["capture_path"]).name == rdc[0],
        "RenderDoc path disagrees with inventory",
    )
    journal = json.loads((capture / "journal.json").read_bytes())
    require(
        journal["schema_version"] == 1 and journal["complete"] is True,
        "guest journal is incomplete",
    )
    for field in ("frame", "start_submission", "end_frame", "end_submission"):
        require(
            type(manifest[field]) is int
            and manifest[field] >= 0
            and journal[field] == manifest[field],
            f"journal boundary disagrees with manifest: {field}",
        )
    require(
        journal["end_frame"] == journal["frame"] + 1,
        "capture must span one guest frame",
    )
    require(
        journal["end_submission"] > journal["start_submission"],
        "capture has no submitted frame interval",
    )
    events = journal["events"]
    require(
        bool(events) and len(events) == manifest["event_count"], "event count mismatch"
    )
    require(
        len(events) <= manifest["bounds"]["max_events"]
        and 0 < manifest["journal_bytes"] <= manifest["bounds"]["max_bytes"],
        "journal exceeds capture bounds",
    )
    require(
        sum(event["kind"] == "swap" for event in events) == 1
        and events[-1]["kind"] == "swap",
        "journal must end with exactly one swap",
    )
    used_files = {"journal.json", rdc[0]}
    pairs = Counter()
    outcomes = Counter()
    textures = Counter()
    failed_guest_events = []
    previous_submission = journal["start_submission"]
    for expected_id, event in enumerate(events, 1):
        kind = event["kind"]
        require(kind in {"draw", "copy", "swap"}, "unknown guest event kind")
        require(event["id"] == expected_id, "guest event IDs are not contiguous")
        require(
            event["marker"] == f"REXGLUE_FRAME_{kind.upper()}_{expected_id:016X}",
            "guest event marker disagrees with identity",
        )
        require(
            event["frame"] == journal["frame"]
            and previous_submission <= event["submission"] < journal["end_submission"],
            "guest event lies outside ordered capture boundaries",
        )
        previous_submission = event["submission"]
        registers = event["registers"]
        register_file = files.get(registers["file"], {})
        require(
            registers["encoding"] == "little_endian_dwords"
            and registers["count"] == 0x5003
            and registers["bytes"] == 0x5003 * 4
            and register_file.get("kind") == "registers"
            and register_file.get("size") == registers["bytes"],
            "guest register snapshot is missing or truncated",
        )
        used_files.add(registers["file"])
        outcome = event["outcome"]
        require(outcome["kind"] != "journal_incomplete", "event outcome incomplete")
        require(type(outcome["success"]) is bool, "event result is not recorded")
        if not outcome["success"]:
            failed_guest_events.append({"id": event["id"], "kind": kind})
        outcomes[f"{kind}:{outcome['kind']}"] += 1
        if kind == "draw":
            for stage in ("vertex", "pixel"):
                shader = event["shaders"][stage]
                if shader["file"] is not None:
                    file = files.get(shader["file"], {})
                    require(
                        file.get("kind") == "shader" and file.get("size", 0) % 4 == 0,
                        "guest shader is missing or truncated",
                    )
                    used_files.add(shader["file"])
            if outcome["host_issued"]:
                require(
                    outcome["kind"] == "issued",
                    "issued guest draw has inconsistent outcome",
                )
                require(
                    event["shaders"]["vertex"]["file"] is not None, "issued VS missing"
                )
                host = event["host"]
                pairs[(host["vertex_shader_hash"], host["pixel_shader_hash"])] += 1
                textures[host["used_texture_mask"]] += 1
        elif kind == "swap":
            require(
                outcome["kind"] == "completed" and outcome["success"] is True,
                "ending swap did not complete",
            )
    require(used_files == set(files), "file inventory has unreferenced payloads")
    return {
        "schema_version": 1,
        "source_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        "frame": journal["frame"],
        "end_frame": journal["end_frame"],
        "start_submission": journal["start_submission"],
        "end_submission": journal["end_submission"],
        "event_count": len(events),
        "outcomes": dict(sorted(outcomes.items())),
        "failed_guest_events": failed_guest_events,
        "host_shader_pairs": [
            {"vertex": pair[0], "pixel": pair[1], "draw_count": count}
            for pair, count in pairs.most_common()
        ],
        "texture_masks": [
            {"mask": mask, "draw_count": count}
            for mask, count in textures.most_common()
        ],
        "frontbuffer": events[-1]["frontbuffer"],
        "files": files,
        "scope": "Journal integrity and file digests; GPU replay and marker joins require RenderDoc inspection.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        result = validate(args.capture)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Invalid frame capture: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
