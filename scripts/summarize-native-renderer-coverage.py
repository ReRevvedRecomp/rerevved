"""Validate and summarize one native-renderer coverage bundle.

Only the Python standard library is used. JSON is loaded with duplicate-key
detection because an evidence bundle must not depend on parser key ordering.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


RUN_SCHEMA = "rerevved.native_renderer.run.v2"
COVERAGE_SCHEMA = "rerevved.native_renderer.coverage.v1"
SERIES_SCHEMA = "rerevved.native_renderer.series.v1"
OBSERVER_BYTE_BUDGET = 3456
RUN_ID_RE = re.compile(r"^NRD-RUN-\d{8}-\d{4}$")
TRANSITION_RE = re.compile(r"^NRD-TRANS-(?:000[1-9]|001[01])$")
DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
OPERATION_ID = "NRD-OP-0002"
RUNTIME_JOIN_KEY = "d3d:0x826A3568"
CONTRACT_ID = "NRD-CONTRACT-0001"
LOCKED_SDK_COMMIT = "6ae32f375caefc8d2f6a98f7b14015cfce40fbb3"
LOCKED_SDK_VERSION = "0.11.0-dev.g6ae32f3"
COMPILED_INPUT_DIGEST = "2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303"
ACCEPTED_FIXTURES = {
    "NRD-TRANS-0001": {"NRD-FIX-0001"},
    "NRD-TRANS-0002": {"NRD-FIX-0002"},
    "NRD-TRANS-0003": {"NRD-FIX-0002"},
    "NRD-TRANS-0004": {"NRD-FIX-0003"},
    "NRD-TRANS-0007": {"NRD-FIX-0003"},
    "NRD-TRANS-0010": {"NRD-FIX-0001"},
}
ACCEPTED_FIXTURE_SHA256 = {
    "NRD-FIX-0001": "2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303",
    "NRD-FIX-0002": "a1bcefa50427ec719fe4d5721cb9438ee3f44ec7c09db48fdc73c3d326e9d684",
    "NRD-FIX-0003": "06e885f11044153d3ddbb7259eeffceb14aa0600d79b6083b526e646630e463e",
}
LOCKED_NEW_GAME_ROUTES = {
    "NRD-TRANS-0002": {
        "expected_marks": ["new-game-setup-romans"],
        "expected_screenshots": [
            "main-menu.png",
            "single-player-menu.png",
            "difficulty-warlord.png",
            "civilization-romans.png",
        ],
        "stop_conditions": [
            "choose Single Player, New Game, Warlord, and highlight Romans without confirming",
            "close immediately after civilization-romans.png capture without confirming Romans, entering gameplay, or saving",
        ],
    },
    "NRD-TRANS-0003": {
        "expected_marks": ["first-settled-human-turn-map"],
        "expected_screenshots": [
            "main-menu.png",
            "single-player-menu.png",
            "difficulty-warlord.png",
            "civilization-romans.png",
            "first-settled-human-turn-map.png",
        ],
        "stop_conditions": [
            "choose Single Player, New Game, Warlord, and confirm Romans",
            "close immediately after first-settled-human-turn-map.png capture before gameplay input or saving",
        ],
    },
}
ACCEPTED_BASE_XEX_SHA256 = "b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd"
ACCEPTED_TITLE_UPDATE_SHA256 = "c1fc6149a63550987d991efdbb80e3697845a9a49d3f2ec180ea9817db8d12d4"
DOMAIN_IDS = {0: "primitive-4", 1: "unknown"}
SITE_ADDRESSES = {0: 0x82303E3C, 1: 0x82303E8C}
BEGIN_SENTINEL = "NRD-COVERAGE-BEGIN"
END_SENTINEL = "NRD-COVERAGE-END"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
TEMP_SUFFIXES = (".tmp", ".part", ".partial", ".incomplete")
ANOMALY_NAMES = {
    9: "invalid_segment",
    10: "invalid_checkpoint_mark",
    11: "counter_saturated",
    14: "finalization_drain_timeout",
    15: "checkpoint_sequence",
}
U64_MAX = (1 << 64) - 1
U32_MAX = (1 << 32) - 1
I32_MIN = -(1 << 31)
I32_MAX = (1 << 31) - 1

DIAGNOSTIC_SOURCE_HASHES = {
    "src/filesystem/virtual_file_system.cpp":
        "461fd8f5d9b0077847ac477a6f106556bae4c4af0a3c7f70c4467946966a2d51",
    "src/graphics/command_processor.cpp":
        "543898c0f86969fef256c5c267503ff935a74b1f9ec1bd460e949e43f412e3fe",
    "src/graphics/d3d12/pipeline_cache.cpp":
        "6a4bc1393947ef6124d0a39f44ab332bb8620a4652d81af233b7186530833a9e",
    "src/graphics/d3d12/command_processor.cpp":
        "f2a23b810e123c46c63237bb38303668ec18515450ef2bda95c45cc2ef1710c0",
}


def _diagnostic_entry(
    stable_id: str,
    source: str,
    pattern: str,
    *,
    presence_only: bool = False,
) -> dict[str, Any]:
    source_path = source.rsplit(":", 1)[0]
    return {
        "id": stable_id,
        "source": source,
        "source_sha256": DIAGNOSTIC_SOURCE_HASHES[source_path],
        "pattern": re.compile(pattern),
        "presence_only": presence_only,
    }


DIAGNOSTIC_REGISTRY = (
    _diagnostic_entry(
        "NRD-SDK-FAIL-0001",
        "src/graphics/command_processor.cpp:625,645,658",
        r"^\*\*\*\* (?:PRIMARY RINGBUFFER|INDIRECT RINGBUFFER|ExecutePacket): Failed to execute packet\.$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0002",
        "src/graphics/d3d12/pipeline_cache.cpp:1067",
        r"^Shader [0-9A-F]{16} translation failed; marking as ignored$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0003",
        "src/graphics/d3d12/pipeline_cache.cpp:3034,3038",
        r"^Failed to create graphics pipeline with VS [0-9A-F]{16}(?:, PS [0-9A-F]{16})?$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0004",
        "src/graphics/d3d12/command_processor.cpp:2025",
        r"^IssueSwap: RequestSwapTexture failed - fetch0:(?: [0-9A-F]{8}){6}$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0005",
        "src/graphics/d3d12/command_processor.cpp:3512",
        r"^(?:Out-of-submission queue operation fence (?:Signal|event arming|wait|completion)|Submission fence (?:Signal|completion|event arming|wait)) failed with HRESULT 0x[0-9A-F]{8}$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0006",
        "src/graphics/d3d12/command_processor.cpp:3554",
        r"^D3D12 device removed: HRESULT 0x[0-9A-F]{8} - (?:Unknown|DEVICE_HUNG \(TDR - GPU command took too long\)|DEVICE_REMOVED \(driver internal error or hot-unplug\)|DEVICE_RESET \(bad GPU command\)|DRIVER_INTERNAL_ERROR|INVALID_CALL)$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0007",
        "src/graphics/d3d12/command_processor.cpp:3570",
        r"^DRED breadcrumb: completed [0-9]+ of [0-9]+ ops$",
        presence_only=True,
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0008",
        "src/graphics/d3d12/command_processor.cpp:3584",
        r"^DRED page fault at VA 0x[0-9A-F]{16}$",
        presence_only=True,
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0009",
        "src/graphics/d3d12/command_processor.cpp:2501",
        r"^Failed to write the (?:swap source|texture-load scratch|diagnostic buffer) capture$",
        presence_only=True,
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0010",
        "src/filesystem/virtual_file_system.cpp:127",
        r"^ResolvePath\(\\Device\) failed - device not found$",
    ),
    _diagnostic_entry(
        "NRD-SDK-FAIL-0011",
        "src/filesystem/virtual_file_system.cpp:127",
        r"^ResolvePath\(UPDATE:\\\) failed - device not found$",
    ),
)
PINNED_DIAGNOSTICS = {entry["id"]: entry for entry in DIAGNOSTIC_REGISTRY}
BOUNDED_DIAGNOSTIC_MAX_COUNTS = {
    ("NRD-FIX-0001", "NRD-TRANS-0001"): {
        "NRD-SDK-FAIL-0010": 1,
        "NRD-SDK-FAIL-0011": 4,
    },
    ("NRD-FIX-0001", "NRD-TRANS-0010"): {
        "NRD-SDK-FAIL-0010": 1,
        "NRD-SDK-FAIL-0011": 4,
    },
    ("NRD-FIX-0002", "NRD-TRANS-0002"): {
        "NRD-SDK-FAIL-0010": 1,
        "NRD-SDK-FAIL-0011": 4,
    },
}


class CoverageError(ValueError):
    """Raised when a bundle must remain blocked."""


def _unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CoverageError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json_unique(path: Path | str) -> Any:
    path = Path(path)
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique)
    except (OSError, UnicodeError, json.JSONDecodeError, CoverageError) as exc:
        raise CoverageError(f"invalid JSON {path}: {exc}") from exc


def _exact(value: Any, label: str, fields: set[str]) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise CoverageError(f"{label} must be an object")
    actual = set(value)
    if actual != fields:
        missing = sorted(fields - actual)
        extra = sorted(actual - fields)
        details = []
        if missing:
            details.append("missing " + ", ".join(missing))
        if extra:
            details.append("unknown " + ", ".join(extra))
        raise CoverageError(f"{label} schema differs ({'; '.join(details)})")
    return value


def _text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or not value.strip():
        raise CoverageError(f"{label} must be a non-empty string")
    return value


def _int(
    value: Any,
    label: str,
    minimum: int = 0,
    maximum: int | None = None,
) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < minimum
        or (maximum is not None and value > maximum)
    ):
        bound = f"{minimum}..{maximum}" if maximum is not None else f">= {minimum}"
        raise CoverageError(f"{label} must be an integer {bound}")
    return value


def _sha(value: Any, label: str) -> str:
    value = _text(value, label)
    if not DIGEST_RE.fullmatch(value):
        raise CoverageError(f"{label} must be lowercase SHA-256 hex")
    return value


def _commit(value: Any, label: str) -> str:
    value = _text(value, label)
    if not COMMIT_RE.fullmatch(value):
        raise CoverageError(f"{label} must be lowercase Git commit hex")
    return value


def _marks(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or len(value) > 6:
        raise CoverageError(f"{label} must contain at most six ordered marks")
    values = [_text(item, label + " entry") for item in value]
    if any(
        item in {".", ".."}
        or "/" in item
        or "\\" in item
        or re.match(r"^[A-Za-z]:", item)
        for item in values
    ):
        raise CoverageError(f"{label} entries must be non-empty unique leaf names")
    if len(set(values)) != len(values):
        raise CoverageError(f"{label} contains duplicate marks")
    return values


def _relative(root: Path, value: Any, label: str) -> tuple[str, Path]:
    raw = _text(value, label).replace("\\", "/")
    path = Path(raw)
    if path.is_absolute() or re.match(r"^[A-Za-z]:", raw):
        raise CoverageError(f"{label} must be relative")
    parts = [part for part in raw.split("/") if part]
    if not parts or any(part == ".." for part in parts):
        raise CoverageError(f"{label} escapes run root")
    relative = "/".join(parts)
    resolved = (root / Path(*parts)).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise CoverageError(f"{label} escapes run root") from exc
    return relative, resolved


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise CoverageError(f"cannot hash {path}: {exc}") from exc
    return digest.hexdigest()


def _artifact(root: Path, value: Any, label: str) -> dict[str, str]:
    item = _exact(value, label, {"path", "sha256"})
    relative, path = _relative(root, item["path"], label + " path")
    expected = _sha(item["sha256"], label + " sha256")
    if not path.is_file():
        raise CoverageError(f"missing {label}: {relative}")
    actual = _hash(path)
    if actual != expected:
        raise CoverageError(f"{label} hash mismatch: {relative}")
    return {"path": relative, "sha256": expected}


def _inventory(root: Path) -> list[dict[str, str]]:
    if not root.is_dir():
        raise CoverageError(f"run root is not a directory: {root}")
    result = []
    for path in sorted(root.rglob("*"), key=lambda p: p.as_posix().lower()):
        if not path.is_file() and not path.is_symlink():
            continue
        relative, resolved = _relative(root, path.relative_to(root).as_posix(), "file path")
        if relative.lower().endswith(TEMP_SUFFIXES):
            raise CoverageError(f"incomplete temporary file is not allowed: {relative}")
        if not resolved.is_file():
            raise CoverageError(f"file is not readable: {relative}")
        result.append({"path": relative, "sha256": _hash(resolved)})
    return result


def _check_log(
    root: Path,
    item: Any,
    state: Mapping[str, Any],
) -> tuple[dict[str, str], list[dict[str, Any]]]:
    item = _exact(item, "run log", {"path", "sha256"})
    relative, path = _relative(root, item["path"], "log path")
    if relative != "coverage.log":
        raise CoverageError(f"log path differs from the isolated runner layout: {relative}")
    expected = _sha(item["sha256"], "log sha256")
    if not path.is_file():
        raise CoverageError(f"log is missing: {relative}")
    if _hash(path) != expected:
        raise CoverageError(f"log hash mismatch: {relative}")
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CoverageError(f"cannot read log {relative}: {exc}") from exc
    parsed_lines = _parse_log_lines(text)
    begin_lines = [
        index for index, (_, message) in enumerate(parsed_lines) if message == BEGIN_SENTINEL
    ]
    end_lines = [
        index for index, (_, message) in enumerate(parsed_lines) if message == END_SENTINEL
    ]
    if len(begin_lines) != 1 or len(end_lines) != 1:
        raise CoverageError(
            f"log requires one complete sentinel pair ({len(begin_lines)}/{len(end_lines)})"
        )
    if begin_lines[0] > end_lines[0]:
        raise CoverageError("log sentinels are out of order")
    if any(parsed_lines[index][0] != "info" for index in begin_lines + end_lines):
        raise CoverageError("coverage sentinels must use the info log level")
    counts: dict[str, int] = {}
    sources: dict[str, str] = {}
    line_index = 0
    while line_index < len(parsed_lines):
        level, message = parsed_lines[line_index]
        mapped = map_sdk_diagnostic(message)
        if mapped is not None and level not in {"error", "critical"}:
            raise CoverageError("mapped SDK diagnostic was not emitted at error level")
        if mapped is not None:
            stable_id = mapped["id"]
            counts[stable_id] = counts.get(stable_id, 0) + 1
            sources[stable_id] = mapped["source"]
            if stable_id == "NRD-SDK-FAIL-0007":
                breadcrumb = DRED_BREADCRUMB_RE.fullmatch(message)
                if breadcrumb is None:
                    raise CoverageError("malformed DRED breadcrumb diagnostic")
                completed = int(breadcrumb[1])
                total = int(breadcrumb[2])
                if completed > U32_MAX or total > U32_MAX:
                    raise CoverageError("DRED breadcrumb counts exceed the locked SDK type")
                effective_last = min(completed, total)
                start = max(effective_last - 3, 0)
                end = min(effective_last + 1, total)
                detail_count = end - start
                if completed == 0 or total == 0 or not 1 <= detail_count <= 4:
                    raise CoverageError("malformed DRED breadcrumb detail count")
                fault_index = effective_last if completed < total else None
                for detail_offset in range(detail_count):
                    line_index += 1
                    if line_index >= len(parsed_lines):
                        raise CoverageError("truncated DRED breadcrumb details")
                    detail_level, detail_message = parsed_lines[line_index]
                    detail = DRED_DETAIL_RE.fullmatch(detail_message)
                    if detail_level not in {"error", "critical"} or detail is None:
                        raise CoverageError("malformed DRED breadcrumb detail")
                    detail_index = int(detail[1])
                    detail_op_type = int(detail[2])
                    if detail_op_type < I32_MIN or detail_op_type > I32_MAX:
                        raise CoverageError("DRED breadcrumb op type exceeds the locked SDK type")
                    if detail_index != start + detail_offset:
                        raise CoverageError("DRED breadcrumb detail indexes are not ordered")
                    has_fault = detail[3] is not None
                    if has_fault != (fault_index == detail_index):
                        raise CoverageError("DRED breadcrumb fault marker is misplaced")
        elif level in {"error", "critical"}:
            raise CoverageError(f"unknown SDK diagnostic remains blocked: {message}")
        line_index += 1
    bounded_ids = {"NRD-SDK-FAIL-0010", "NRD-SDK-FAIL-0011"}
    bounded_max_counts = BOUNDED_DIAGNOSTIC_MAX_COUNTS.get(
        (state["fixture_id"], state["transition_id"])
    )
    for stable_id in bounded_ids:
        count = counts.get(stable_id, 0)
        if count and (
            bounded_max_counts is None
            or count > bounded_max_counts[stable_id]
        ):
            raise CoverageError("filesystem probe diagnostic exceeds its accepted fixture bound")
    diagnostics = []
    for stable_id in sorted(counts):
        entry = PINNED_DIAGNOSTICS[stable_id]
        diagnostics.append(
            {
                "id": stable_id,
                "count": counts[stable_id],
                "source": sources[stable_id],
                "evidence": {
                    "source": entry["source"],
                    "source_sha256": entry["source_sha256"],
                    "sdk_commit": LOCKED_SDK_COMMIT,
                    "sdk_version": LOCKED_SDK_VERSION,
                    "presence_only": entry["presence_only"],
                },
            }
        )
    return {"path": relative, "sha256": expected}, diagnostics


def _bool(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise CoverageError(f"{label} must be boolean")
    return value


def _snapshot_row(value: Any, label: str, include_mark: bool) -> Mapping[str, Any]:
    fields = {
        "frame_sequence", "valid_fields", "gameplay_active", "interface_update",
        "active_player", "human_player_mask", "turn_owner_known", "human_turn",
        "available", "civilization", "era", "year", "turn",
    }
    fields = fields | ({"mark", "accepted", "segment"} if include_mark else {"index", "accepted"})
    row = _exact(value, label, fields)
    _int(row["frame_sequence"], label + " frame_sequence", 0, U64_MAX)
    _int(row["valid_fields"], label + " valid_fields", 0, U32_MAX)
    for name in ("gameplay_active", "interface_update", "turn_owner_known", "human_turn", "available", "accepted"):
        _bool(row[name], label + " " + name)
    _int(row["active_player"], label + " active_player", I32_MIN, I32_MAX)
    _int(row["human_player_mask"], label + " human_player_mask", 0, U32_MAX)
    for name in ("civilization", "era", "year", "turn"):
        _int(row[name], label + " " + name, I32_MIN, I32_MAX)
    if include_mark:
        _int(row["mark"], label + " mark", 0, 5)
        _int(row["segment"], label + " segment", 0, 7)
    else:
        _int(row["index"], label + " index", 0, 7)
    return row


def _counter_rows(value: Any) -> tuple[list[dict[str, int | str]], int]:
    if not isinstance(value, list) or len(value) != 32:
        raise CoverageError("coverage counters must contain exactly 32 rows")
    rows: list[dict[str, int | str]] = []
    for index, item in enumerate(value):
        item = _exact(
            item,
            f"counter[{index}]",
            {
                "segment", "operation", "operation_id", "runtime_join_key",
                "contract_id", "domain", "domain_id", "site", "site_address",
                "count",
            },
        )
        segment = _int(item["segment"], "counter segment", 0, 7)
        operation = _int(item["operation"], "counter operation", 0, 0)
        domain = _int(item["domain"], "counter domain", 0, 1)
        site = _int(item["site"], "counter site", 0, 1)
        row: dict[str, int | str] = {
            "segment": segment,
            "operation": operation,
            "operation_id": _text(item["operation_id"], "counter operation_id"),
            "runtime_join_key": _text(
                item["runtime_join_key"], "counter runtime_join_key"
            ),
            "contract_id": _text(item["contract_id"], "counter contract_id"),
            "domain": domain,
            "domain_id": _text(item["domain_id"], "counter domain_id"),
            "site": site,
            "site_address": _int(item["site_address"], "counter site_address", 0, U32_MAX),
            "count": _int(item["count"], "counter count", 0, U64_MAX),
        }
        expected_segment = index // 4
        expected_domain = (index % 4) // 2
        expected_site = index % 2
        if (segment, operation, domain, site) != (
            expected_segment, 0, expected_domain, expected_site
        ):
            raise CoverageError("counter rows are not isolated and ordered by segment")
        if row["operation_id"] != OPERATION_ID:
            raise CoverageError("counter operation_id differs from the accepted operation")
        if row["runtime_join_key"] != RUNTIME_JOIN_KEY:
            raise CoverageError("counter runtime_join_key differs from the accepted operation")
        if row["contract_id"] != CONTRACT_ID:
            raise CoverageError("counter contract_id differs from the accepted operation")
        if row["domain_id"] != DOMAIN_IDS[domain]:
            raise CoverageError("counter domain_id differs from the accepted domain")
        if row["site_address"] != SITE_ADDRESSES[site]:
            raise CoverageError("counter site_address differs from the accepted site")
        if row["count"] > 0:
            rows.append(row)
    return rows, sum(int(row["count"]) for row in rows)


def map_sdk_diagnostic(
    message: str,
) -> Mapping[str, str] | None:
    """Return a match from the built-in source-pinned SDK diagnostic registry."""

    if not isinstance(message, str):
        raise TypeError("diagnostic message must be a string")
    for entry in DIAGNOSTIC_REGISTRY:
        if entry["pattern"].fullmatch(message):
            return {
                "id": entry["id"],
                "source": entry["source"],
            }
    return None


def operation_matrix(
    rows: Iterable[Mapping[str, Any]],
) -> dict[str, dict[str, dict[str, list[int]]]]:
    matrix: dict[str, dict[str, dict[str, set[int]]]] = {}
    for index, raw in enumerate(rows):
        row = _exact(
            raw,
            f"matrix row[{index}]",
            {
                "segment", "operation", "operation_id", "runtime_join_key",
                "contract_id", "domain", "domain_id", "site", "site_address",
                "count",
            },
        )
        segment = str(_int(row["segment"], "matrix segment", 0, 7))
        operation = str(_int(row["operation"], "matrix operation", 0, 0))
        domain = str(_int(row["domain"], "matrix domain", 0, 1))
        site = _int(row["site"], "matrix site", 0, 1)
        count = _int(row["count"], "matrix count", 0, U64_MAX)
        if (
            operation != "0"
            or domain not in {"0", "1"}
            or row["operation_id"] != OPERATION_ID
            or row["runtime_join_key"] != RUNTIME_JOIN_KEY
            or row["contract_id"] != CONTRACT_ID
            or row["domain_id"] != DOMAIN_IDS[int(domain)]
            or site not in SITE_ADDRESSES
            or row["site_address"] != SITE_ADDRESSES[site]
        ):
            raise CoverageError("matrix row differs from the accepted counter contract")
        if count == 0:
            continue
        matrix.setdefault(operation, {}).setdefault(domain, {}).setdefault(segment, set()).add(site)
    return {
        operation: {
            domain: {
                segment: sorted(sites)
                for segment, sites in sorted(segments.items(), key=lambda item: int(item[0]))
            }
            for domain, segments in sorted(domains.items(), key=lambda item: int(item[0]))
        }
        for operation, domains in sorted(matrix.items())
    }


def matrix_union_intersection(
    series: Iterable[Iterable[Mapping[str, Any]]],
) -> dict[str, dict[str, dict[str, dict[str, list[int]]]]]:
    """Reduce matching operation/domain/segment/site rows in stable order."""

    matrices = [operation_matrix(rows) for rows in series]
    if not matrices:
        return {"union": {}, "intersection": {}}
    union: dict[str, dict[str, dict[str, list[int]]]] = {}
    intersection: dict[str, dict[str, dict[str, list[int]]]] = {}
    operations = sorted({key for matrix in matrices for key in matrix})
    for operation in operations:
        domains = sorted(
            {
                domain
                for matrix in matrices
                if operation in matrix
                for domain in matrix[operation]
            },
            key=int,
        )
        for domain in domains:
            segments = sorted(
                {
                    segment
                    for matrix in matrices
                    if operation in matrix and domain in matrix[operation]
                    for segment in matrix[operation][domain]
                },
                key=int,
            )
            for segment in segments:
                matching = [
                    set(matrix[operation][domain][segment])
                    for matrix in matrices
                    if operation in matrix
                    and domain in matrix[operation]
                    and segment in matrix[operation][domain]
                ]
                union.setdefault(operation, {}).setdefault(domain, {})[segment] = sorted(
                    set().union(*matching)
                )
                if len(matching) == len(matrices):
                    intersection.setdefault(operation, {}).setdefault(domain, {})[
                        segment
                    ] = sorted(set.intersection(*matching))
    return {"union": union, "intersection": intersection}


def _string_list(value: Any, label: str) -> list[str]:
    if not isinstance(value, list):
        raise CoverageError(f"{label} must be a list")
    result = [_text(item, f"{label} entry") for item in value]
    if len(set(result)) != len(result):
        raise CoverageError(f"{label} contains duplicates")
    return result


def _safe_leaf_list(
    value: Any,
    label: str,
    maximum: int,
    *,
    allow_empty: bool = False,
) -> list[str]:
    if not isinstance(value, list) or (not allow_empty and not value) or len(value) > maximum:
        requirement = f"one through {maximum}" if not allow_empty else f"at most {maximum}"
        raise CoverageError(f"{label} must contain {requirement} safe leaf names")
    result = [_text(item, f"{label} entry") for item in value]
    if any(
        item in {".", ".."}
        or "/" in item
        or "\\" in item
        or re.match(r"^[A-Za-z]:", item)
        for item in result
    ):
        raise CoverageError(f"{label} entries must be non-empty unique leaf names")
    if len(set(result)) != len(result):
        raise CoverageError(f"{label} contains duplicates")
    return result


def _utc(value: Any, label: str) -> str:
    value = _text(value, label)
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z", value):
        raise CoverageError(f"{label} must be an ISO-8601 UTC timestamp")
    try:
        datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise CoverageError(f"{label} is not a real UTC timestamp") from exc
    return value


def _path_is_beneath(path: str, directory: str) -> bool:
    return path.startswith(directory.rstrip("/") + "/")


LOG_LINE_RE = re.compile(
    r"^\[(?P<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\] "
    r"\[(?P<level>trace|debug|info|warning|error|critical)\] "
    r"\[(?P<logger>[^\]]+)\] \[t(?P<thread>\d+)\] (?P<message>.+)$"
)
DRED_BREADCRUMB_RE = re.compile(
    r"^DRED breadcrumb: completed ((?:0|[1-9][0-9]*)) of ((?:0|[1-9][0-9]*)) ops$"
)
DRED_DETAIL_RE = re.compile(
    r"^  \[((?:0|[1-9][0-9]*))\] op type ((?:0|-?[1-9][0-9]*))( <-- FAULT)?$"
)
D3D12_FEATURE_PARENT = "Direct3D 12 device and OS features:"
D3D12_FEATURE_CONTINUATION_RES = (
    re.compile(r"^\* Max GPU virtual address bits per resource: (?P<number>0|[1-9][0-9]*)$"),
    re.compile(r"^\* Non-zeroed heap creation: (?:yes|no)$"),
    re.compile(r"^\* Pixel-shader-specified stencil reference: (?:yes|no)$"),
    re.compile(r"^\* Programmable sample positions: tier (?P<number>0|[1-9][0-9]*)$"),
    re.compile(r"^\* Rasterizer-ordered views: (?:yes|no)$"),
    re.compile(r"^\* Resource binding: tier (?P<number>0|[1-9][0-9]*)$"),
    re.compile(r"^\* Tiled resources: tier (?P<number>0|[1-9][0-9]*)$"),
    re.compile(r"^\* Unaligned block-compressed textures: (?:yes|no)$"),
)


def _parse_log_line(line: str) -> tuple[str, str]:
    match = LOG_LINE_RE.fullmatch(line)
    if match is None:
        raise CoverageError("coverage log line does not use the locked SDK envelope")
    try:
        datetime.strptime(match["timestamp"], "%Y-%m-%d %H:%M:%S.%f")
    except ValueError as exc:
        raise CoverageError("coverage log timestamp is invalid") from exc
    return match["level"], match["message"]


def _parse_log_lines(text: str) -> list[tuple[str, str]]:
    lines = text.splitlines()
    parsed_lines: list[tuple[str, str]] = []
    line_index = 0
    while line_index < len(lines):
        level, message = _parse_log_line(lines[line_index])
        parsed_lines.append((level, message))
        if message == D3D12_FEATURE_PARENT:
            if level != "info":
                raise CoverageError("locked SDK device-feature parent must use info level")
            for continuation_re in D3D12_FEATURE_CONTINUATION_RES:
                line_index += 1
                if line_index >= len(lines):
                    raise CoverageError("locked SDK device-feature continuation is truncated")
                continuation = continuation_re.fullmatch(lines[line_index])
                if continuation is None:
                    raise CoverageError("locked SDK device-feature continuation is malformed")
                number = continuation.groupdict().get("number")
                if number is not None and int(number) > U32_MAX:
                    raise CoverageError("locked SDK device-feature value exceeds uint32")
        line_index += 1
    return parsed_lines


def _validate_run(root: Path, raw: Any) -> tuple[Mapping[str, Any], dict[str, Any]]:
    fields = {
        "schema", "run_id", "fixture_id", "fixture", "fixture_sha256",
        "fixture_staged_path",
        "transition_id", "input_digest", "expected_marks", "repeat",
        "cache_class", "cache_seed_sha256", "title_commit", "title_dirty",
        "sdk_commit", "sdk_dirty",
        "sdk_version", "executable", "executable_sha256", "base_xex",
        "base_xex_sha256", "title_update", "title_update_sha256",
        "xenos_enabled", "rov_enabled", "renderer_config", "host_graphics",
        "readiness", "output_root", "output_directory", "screenshot_directory",
        "shader_directory", "user_data_directory", "cache_directory",
        "save_directory", "checkpoint", "timing",
        "command_result", "operator_review", "log", "artifacts",
        "screenshots", "saves",
    }
    run = _exact(raw, "run.json", fields)
    if run["schema"] != RUN_SCHEMA:
        raise CoverageError("run schema differs from the accepted version")
    run_id = _text(run["run_id"], "run_id")
    if not RUN_ID_RE.fullmatch(run_id):
        raise CoverageError("run_id is malformed")
    fixture_id = _text(run["fixture_id"], "fixture_id")
    if not re.fullmatch(r"NRD-FIX-000[1-6]", fixture_id):
        raise CoverageError("fixture_id is malformed")
    transition = _text(run["transition_id"], "transition_id")
    if not TRANSITION_RE.fullmatch(transition):
        raise CoverageError("transition_id is malformed")
    if fixture_id not in ACCEPTED_FIXTURES.get(transition, set()):
        raise CoverageError("fixture_id is not accepted for transition_id")
    digest = _sha(run["input_digest"], "input_digest")
    if digest != COMPILED_INPUT_DIGEST:
        raise CoverageError("input_digest differs from the compiled observer input")
    fixture_sha = _sha(run["fixture_sha256"], "fixture_sha256")
    if fixture_sha != ACCEPTED_FIXTURE_SHA256.get(fixture_id, fixture_sha):
        raise CoverageError("fixture SHA-256 differs from the accepted fixture")
    fixture_staged_path = run["fixture_staged_path"]
    if fixture_id == "NRD-FIX-0003":
        staged_relative, staged_path = _relative(
            root, fixture_staged_path, "fixture_staged_path"
        )
        if staged_relative != "user-data/save5.sve":
            raise CoverageError("save-copy fixture path differs from the runner")
        if not staged_path.is_file() or _hash(staged_path) != fixture_sha:
            raise CoverageError("staged save-copy fixture differs from its accepted input")
        fixture_staged_path = staged_relative
    elif fixture_staged_path is not None:
        raise CoverageError("non-save fixture must not have a staged path")
    repeat = _int(run["repeat"], "repeat", 1)
    if repeat not in {1, 2}:
        raise CoverageError("repeat must be 1 or 2")
    cache_class = run["cache_class"]
    if cache_class not in {"cold", "warm"}:
        raise CoverageError("cache_class must be cold or warm")
    cache_seed = run["cache_seed_sha256"]
    if cache_class == "cold":
        if cache_seed is not None:
            raise CoverageError("cold cache must not carry a seed digest")
    else:
        _sha(cache_seed, "cache_seed_sha256")
    _commit(run["title_commit"], "title_commit")
    if run["title_dirty"] is not False:
        raise CoverageError("title checkout must be clean")
    if _commit(run["sdk_commit"], "sdk_commit") != LOCKED_SDK_COMMIT:
        raise CoverageError("run SDK commit differs from the accepted lock")
    if run["sdk_dirty"] is not False:
        raise CoverageError("SDK checkout must be clean")
    if _text(run["sdk_version"], "sdk_version") != LOCKED_SDK_VERSION:
        raise CoverageError("run SDK version differs from the accepted lock")
    input_paths: dict[str, str] = {}
    for field in ("fixture", "executable", "base_xex", "title_update"):
        relative = _text(run[field], field).replace("\\", "/")
        if (
            Path(relative).is_absolute()
            or re.match(r"^[A-Za-z]:", relative)
            or not relative
            or ".." in relative.split("/")
            or any(part in {"", "."} for part in relative.split("/"))
        ):
            raise CoverageError(f"{field} must be relative")
        input_paths[field] = relative
    expected_input_paths = {
        "executable": "out/build/win-amd64-release/rerevved.exe",
        "base_xex": "game/default.xex",
        "title_update": "game/default.xexp",
    }
    if any(input_paths[field] != expected for field, expected in expected_input_paths.items()):
        raise CoverageError("fixed title input path differs from the runner")
    expected_fixture_paths = {
        "NRD-FIX-0001": "config/native_renderer_fixture_0001.toml",
        "NRD-FIX-0002": "config/native_renderer_fixture_0002.json",
    }
    if (
        fixture_id in expected_fixture_paths
        and input_paths["fixture"] != expected_fixture_paths[fixture_id]
    ):
        raise CoverageError(f"{fixture_id} recipe descriptor path differs from the runner")
    _sha(run["executable_sha256"], "executable_sha256")
    if _sha(run["base_xex_sha256"], "base_xex_sha256") != ACCEPTED_BASE_XEX_SHA256:
        raise CoverageError("base XEX SHA-256 differs from the accepted input")
    if _sha(run["title_update_sha256"], "title_update_sha256") != ACCEPTED_TITLE_UPDATE_SHA256:
        raise CoverageError("title update SHA-256 differs from the accepted input")
    if run["xenos_enabled"] is not True or run["rov_enabled"] is not True:
        raise CoverageError("xenos and ROV must both be enabled")
    renderer = _exact(
        run["renderer_config"],
        "renderer_config",
        {
            "guest_width", "guest_height", "output_width", "output_height",
            "resolution_scale", "window_mode", "combat_speed",
            "xenos_enabled", "rov_enabled", "configuration_digest",
        },
    )
    if _int(renderer["guest_width"], "guest_width", 1) != 1280 or _int(
        renderer["guest_height"], "guest_height", 1
    ) != 720:
        raise CoverageError("guest dimensions must be 1280x720")
    _int(renderer["output_width"], "output_width", 1, 16384)
    _int(renderer["output_height"], "output_height", 1, 16384)
    resolution_scale = _int(renderer["resolution_scale"], "resolution_scale", 1, 8)
    if resolution_scale > 8:
        raise CoverageError("resolution_scale must be an integer from 1 through 8")
    if renderer["window_mode"] not in {"windowed", "borderless"}:
        raise CoverageError("window_mode is not accepted by the runner")
    if renderer["combat_speed"] not in {"normal", "fast"}:
        raise CoverageError("combat_speed is not accepted by the runner")
    if renderer["xenos_enabled"] is not True or renderer["rov_enabled"] is not True:
        raise CoverageError("renderer_config must record xenos and ROV enabled")
    renderer_lines = [
        "xenos_enabled=true",
        "rov_enabled=true",
        "guest_width=1280",
        "guest_height=720",
        f"output_width={renderer['output_width']}",
        f"output_height={renderer['output_height']}",
        f"resolution_scale={renderer['resolution_scale']}",
        f"window_mode={renderer['window_mode']}",
        f"combat_speed={renderer['combat_speed']}",
    ]
    renderer_digest = hashlib.sha256(
        ("\n".join(renderer_lines) + "\n").encode("utf-8")
    ).hexdigest()
    if _sha(renderer["configuration_digest"], "configuration_digest") != renderer_digest:
        raise CoverageError("renderer configuration digest mismatch")
    graphics = _exact(
        run["host_graphics"],
        "host_graphics",
        {
            "os_build", "gpu_name", "gpu_vendor_id", "gpu_device_id",
            "driver_version", "d3d_feature_level",
        },
    )
    for field in graphics:
        _text(graphics[field], "host_graphics " + field)
    for field in ("gpu_vendor_id", "gpu_device_id"):
        if not re.fullmatch(r"(?:0x)?[0-9a-f]{4,8}", graphics[field]):
            raise CoverageError(f"host_graphics {field} is not a lowercase hexadecimal identifier")
    readiness = _exact(
        run["readiness"],
        "readiness",
        {
            "owner_ready", "overlay_policy", "overlays_closed_before_launch",
            "start_invariant", "authorized_skip_boundary", "expected_screenshots",
            "stop_conditions",
        },
    )
    if readiness["owner_ready"] is not True:
        raise CoverageError("owner readiness was not acknowledged")
    if readiness["overlay_policy"] != "closed" or readiness["overlays_closed_before_launch"] is not True:
        raise CoverageError("overlay closure policy was not accepted")
    start_invariant = _text(readiness["start_invariant"], "start_invariant")
    authorized_skip_boundary = _text(
        readiness["authorized_skip_boundary"], "authorized_skip_boundary"
    )
    expected_screenshots = _safe_leaf_list(
        readiness["expected_screenshots"], "expected_screenshots", 6
    )
    stop_conditions = _string_list(readiness["stop_conditions"], "stop_conditions")
    if not 1 <= len(stop_conditions) <= 8:
        raise CoverageError("stop_conditions must contain one through eight entries")
    expected_marks = _marks(run["expected_marks"], "expected_marks")
    if fixture_id == "NRD-FIX-0002":
        locked_route = LOCKED_NEW_GAME_ROUTES[transition]
        if (
            start_invariant != "process absent"
            or authorized_skip_boundary
            != "boot intro movie only after owner readiness; do not skip any setup panel"
            or expected_marks != locked_route["expected_marks"]
            or expected_screenshots != locked_route["expected_screenshots"]
            or stop_conditions != locked_route["stop_conditions"]
        ):
            raise CoverageError("NRD-FIX-0002 plan differs from the owner-locked route")
    directories: dict[str, str] = {}
    for field in (
        "output_root", "output_directory", "screenshot_directory",
        "shader_directory", "user_data_directory", "cache_directory",
        "save_directory",
    ):
        relative, path = _relative(root, run[field], field)
        if path.is_symlink():
            raise CoverageError(f"planned {field} must not be a reparse point: {relative}")
        if not path.is_dir():
            raise CoverageError(f"planned {field} is missing: {relative}")
        directories[field] = relative
    expected_directories = {
        "output_root": ".",
        "output_directory": "observer",
        "screenshot_directory": "screenshots",
        "shader_directory": "shaders",
        "user_data_directory": "user-data",
        "cache_directory": "cache/" + cache_class,
        "save_directory": "user-data",
    }
    if directories != expected_directories:
        raise CoverageError("run directories differ from the fixed isolated layout")
    timing = _exact(run["timing"], "timing", {"started_utc", "ended_utc"})
    started = _utc(timing["started_utc"], "started_utc")
    ended = _utc(timing["ended_utc"], "ended_utc")
    if datetime.fromisoformat(ended[:-1] + "+00:00") < datetime.fromisoformat(
        started[:-1] + "+00:00"
    ):
        raise CoverageError("run timing is reversed")
    command = _exact(
        run["command_result"], "command_result", {"exit_code", "classification"}
    )
    if _int(command["exit_code"], "exit_code") != 0 or command["classification"] != "accepted":
        raise CoverageError("run command did not complete normally")
    review = _exact(
        run["operator_review"],
        "operator_review",
        {"overlays_remained_closed", "reached_marks", "unexpected_errors"},
    )
    if review["overlays_remained_closed"] is not True:
        raise CoverageError("operator overlay review did not pass")
    if _marks(review["reached_marks"], "reached_marks") != expected_marks:
        raise CoverageError("operator reached marks differ from expected marks")
    if _string_list(review["unexpected_errors"], "unexpected_errors"):
        raise CoverageError("operator recorded unexpected errors")
    if run["checkpoint"] != "complete":
        raise CoverageError("run checkpoint is not complete")
    return run, {
        "run_id": run_id,
        "fixture_id": fixture_id,
        "fixture_sha256": fixture_sha,
        "fixture_staged_path": fixture_staged_path,
        "transition_id": transition,
        "input_digest": digest,
        "expected_marks": expected_marks,
        "expected_screenshots": expected_screenshots,
        "repeat": repeat,
        "cache_class": cache_class,
        "cache_seed_sha256": cache_seed,
        "directories": directories,
        "series_identity": {
            "fixture_id": fixture_id,
            "fixture": input_paths["fixture"],
            "fixture_sha256": fixture_sha,
            "fixture_staged_path": fixture_staged_path,
            "transition_id": transition,
            "input_digest": digest,
            "cache_class": cache_class,
            "cache_seed_sha256": cache_seed,
            "title_commit": run["title_commit"],
            "title_dirty": run["title_dirty"],
            "sdk_commit": run["sdk_commit"],
            "sdk_dirty": run["sdk_dirty"],
            "sdk_version": run["sdk_version"],
            "executable": input_paths["executable"],
            "executable_sha256": run["executable_sha256"],
            "base_xex": input_paths["base_xex"],
            "base_xex_sha256": run["base_xex_sha256"],
            "title_update": input_paths["title_update"],
            "title_update_sha256": run["title_update_sha256"],
            "xenos_enabled": run["xenos_enabled"],
            "rov_enabled": run["rov_enabled"],
            "renderer_config": dict(renderer),
            "host_graphics": dict(graphics),
            "expected_marks": expected_marks,
            "expected_screenshots": expected_screenshots,
            "input_paths": input_paths,
            "start_invariant": readiness["start_invariant"],
            "authorized_skip_boundary": readiness["authorized_skip_boundary"],
            "stop_conditions": list(readiness["stop_conditions"]),
        },
    }


def _validate_artifacts(
    root: Path,
    run: Mapping[str, Any],
    state: Mapping[str, Any],
    log: Mapping[str, str],
) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    if not isinstance(run["artifacts"], list):
        raise CoverageError("run artifacts must be a list")
    artifacts = [
        _artifact(root, item, f"artifact[{index}]")
        for index, item in enumerate(run["artifacts"])
    ]
    paths = [item["path"] for item in artifacts]
    if len(paths) != len(set(paths)):
        raise CoverageError("run artifacts contain duplicate paths")
    coverage_path = state["directories"]["output_directory"] + "/coverage.json"
    if paths != [coverage_path]:
        raise CoverageError("artifacts must contain only observer/coverage.json")
    screenshots = [
        _artifact(root, item, f"screenshot[{index}]")
        for index, item in enumerate(run["screenshots"])
    ] if isinstance(run["screenshots"], list) else None
    saves = [
        _artifact(root, item, f"save[{index}]")
        for index, item in enumerate(run["saves"])
    ] if isinstance(run["saves"], list) else None
    if screenshots is None or saves is None:
        raise CoverageError("screenshots and saves must be lists")
    if state["fixture_id"] == "NRD-FIX-0002" and saves:
        raise CoverageError("NRD-FIX-0002 prohibits save output")
    if state["fixture_staged_path"] is not None and any(
        item["path"] == state["fixture_staged_path"] for item in saves
    ):
        raise CoverageError("staged fixture must not be classified as a save output")
    listed_paths = [item["path"] for item in artifacts + screenshots + saves]
    if len(listed_paths) != len(set(listed_paths)):
        raise CoverageError("run artifact records contain duplicate paths")
    if len(screenshots) != len(state["expected_screenshots"]):
        raise CoverageError("screenshot count differs from the run plan")
    for kind, rows, directory in (
        ("screenshot", screenshots, state["directories"]["screenshot_directory"]),
        ("save", saves, state["directories"]["save_directory"]),
    ):
        for row in rows:
            if not _path_is_beneath(row["path"], directory):
                raise CoverageError(f"{kind} is outside its dedicated directory")
            if kind == "screenshot" and Path(row["path"]).suffix.lower() != ".png":
                raise CoverageError("screenshots must use .png")
            if kind == "screenshot":
                screenshot_path = root / Path(*row["path"].split("/"))
                with screenshot_path.open("rb") as screenshot_file:
                    signature = screenshot_file.read(len(PNG_SIGNATURE))
                if signature != PNG_SIGNATURE:
                    raise CoverageError("screenshots must have a PNG signature")
            if kind == "save" and Path(row["path"]).suffix.lower() != ".sve":
                raise CoverageError("save outputs must use .sve")
    expected_screenshot_paths = [
        state["directories"]["screenshot_directory"] + "/" + name
        for name in state["expected_screenshots"]
    ]
    if [item["path"] for item in screenshots] != expected_screenshot_paths:
        raise CoverageError("screenshots differ from the exact ordered run plan")
    shader_directory = state["directories"]["shader_directory"]
    shader_path = root / Path(*shader_directory.split("/"))
    if any(path.is_file() or path.is_symlink() for path in shader_path.rglob("*")):
        raise CoverageError("shader artifacts have no accepted producer in this candidate")
    allowed_paths = {coverage_path}
    allowed_paths.update(item["path"] for item in screenshots)
    allowed_paths.update(item["path"] for item in saves)
    inventory = _inventory(root)
    inventory_by_path = {item["path"]: item for item in inventory}
    inventory_paths = set(inventory_by_path)
    expected_files = {"run.json", log["path"], *allowed_paths}
    ignored_runtime = {
        path
        for path in inventory_paths
        if _path_is_beneath(path, state["directories"]["cache_directory"])
        or _path_is_beneath(path, state["directories"]["user_data_directory"])
    } - {item["path"] for item in saves}
    unrecorded_runtime = ignored_runtime - {
        path for path in (state["fixture_staged_path"],) if path is not None
    }
    if any(Path(path).suffix.lower() == ".sve" for path in unrecorded_runtime):
        raise CoverageError("an unrecorded save output is present")
    evidence_paths = inventory_paths - ignored_runtime
    if evidence_paths != expected_files:
        extra = sorted(evidence_paths - expected_files)
        missing = sorted(expected_files - evidence_paths)
        raise CoverageError(
            f"run file allowlist differs (extra={extra}; missing={missing})"
        )
    evidence_inventory = [inventory_by_path[path] for path in sorted(evidence_paths)]
    return sorted(artifacts, key=lambda item: item["path"]), evidence_inventory


def _segment_label(segment: int, expected_marks: Sequence[str]) -> str:
    if segment == 0:
        return "start"
    if segment == 7:
        return "final"
    if 1 <= segment <= len(expected_marks):
        return expected_marks[segment - 1]
    raise CoverageError("observed counter has no named accepted checkpoint")


def _operation_rows(
    state: Mapping[str, Any],
    counters: Sequence[Mapping[str, Any]],
    coverage_artifact: Mapping[str, str],
    run_record_sha256: str,
) -> list[dict[str, Any]]:
    result = []
    for domain, discriminator in DOMAIN_IDS.items():
        domain_rows = [row for row in counters if row["domain"] == domain]
        count = sum(int(row["count"]) for row in domain_rows)
        observed_segments = sorted(
            {int(row["segment"]) for row in domain_rows if int(row["count"]) > 0}
        )
        if count and domain == 0:
            outcome = "observed"
            qualification = "accepted bounded counter"
            first = _segment_label(observed_segments[0], state["expected_marks"])
            last = _segment_label(observed_segments[-1], state["expected_marks"])
        elif count and domain == 1:
            outcome = "blocked"
            qualification = "unmapped-input discriminator is unsupported"
            first = _segment_label(observed_segments[0], state["expected_marks"])
            last = _segment_label(observed_segments[-1], state["expected_marks"])
        elif count == 0:
            outcome = "blocked"
            qualification = "site-local partial snapshot; no global absence claim"
            first = None
            last = None
        else:
            outcome = "blocked"
            qualification = "diagnostic did not surround a complete guest lifetime"
            first = None
            last = None
        result.append(
            {
                "operation_id": OPERATION_ID,
                "runtime_join_key": RUNTIME_JOIN_KEY,
                "contract_ids": [CONTRACT_ID],
                "fixture_id": state["fixture_id"],
                "transition_id": state["transition_id"],
                "run_id": state["run_id"],
                "cache_class": state["cache_class"],
                "repeat": state["repeat"],
                "discriminator": discriminator,
                "count": count,
                "first_checkpoint": first,
                "last_checkpoint": last,
                "outcome": outcome,
                "qualification": qualification,
                "evidence": {
                    "coverage": dict(coverage_artifact),
                    "run_record": {"path": "run.json", "sha256": run_record_sha256},
                },
            }
        )
    return result


def summarize_run(
    run_root: Path | str,
) -> dict[str, Any]:
    root = Path(run_root)
    run, state = _validate_run(root, load_json_unique(root / "run.json"))
    coverage_raw = load_json_unique(root / "observer" / "coverage.json")
    coverage = _exact(
        coverage_raw,
        "coverage.json",
        {
            "schema", "run_id", "transition_id", "input_digest", "xenos_enabled",
            "rov_enabled", "observer_byte_budget", "operation_metadata",
            "transition_attribution_valid", "exit_class",
            "lifetime_evaluation", "complete", "incomplete", "recovered_incomplete",
            "counters", "counter_failures", "segments", "checkpoints",
            "anomalies",
        },
    )
    if coverage["schema"] != COVERAGE_SCHEMA:
        raise CoverageError("coverage schema differs from the accepted version")
    run_id = state["run_id"]
    transition = state["transition_id"]
    if coverage["run_id"] != run_id or coverage["transition_id"] != transition:
        raise CoverageError("coverage run/transition mismatch")
    digest = state["input_digest"]
    if coverage["input_digest"] != digest:
        raise CoverageError("coverage input_digest mismatch")
    metadata = _exact(
        coverage["operation_metadata"],
        "operation_metadata",
        {
            "operation_id", "runtime_join_key", "roles", "contract_ids",
            "hook_sites", "registers", "value_domains",
        },
    )
    expected_metadata = {
        "operation_id": OPERATION_ID,
        "runtime_join_key": RUNTIME_JOIN_KEY,
        "roles": ["wrapper", "lowering-boundary"],
        "contract_ids": [CONTRACT_ID],
        "hook_sites": [
            {"address": SITE_ADDRESSES[0], "phase": "value", "discriminator": "primitive-4"},
            {"address": SITE_ADDRESSES[1], "phase": "value", "discriminator": "primitive-4"},
        ],
        "registers": [],
        "value_domains": [
            {"id": "primitive-4", "value": 4, "selection": "site-fixed"},
            {"id": "unknown", "value": None, "selection": "unmapped-input"},
        ],
    }
    if metadata != expected_metadata:
        raise CoverageError("operation metadata differs from the accepted operation snapshot")
    if coverage["transition_attribution_valid"] is not True:
        raise CoverageError("transition attribution was poisoned")
    expected_marks = state["expected_marks"]
    if run["xenos_enabled"] is not True or run["rov_enabled"] is not True:
        raise CoverageError("xenos and ROV must both be enabled")
    if coverage["xenos_enabled"] is not True or coverage["rov_enabled"] is not True:
        raise CoverageError("coverage xenos/ROV flags are not enabled")
    if coverage["observer_byte_budget"] != OBSERVER_BYTE_BUDGET:
        raise CoverageError("observer byte budget differs from the accepted budget")
    exit_class = coverage["exit_class"]
    lifetime_evaluation = coverage["lifetime_evaluation"]
    if exit_class not in {"guest_complete", "window_close", "shutdown"}:
        raise CoverageError("coverage exit_class is unknown")
    expected_lifetime = "evaluated" if exit_class == "guest_complete" else "not-evaluated"
    if lifetime_evaluation != expected_lifetime:
        raise CoverageError("coverage lifetime_evaluation does not match exit_class")
    _bool(coverage["complete"], "coverage complete")
    _bool(coverage["incomplete"], "coverage incomplete")
    _bool(coverage["recovered_incomplete"], "coverage recovered_incomplete")
    if (
        coverage["complete"] is not True
        or coverage["incomplete"] is not False
        or coverage["recovered_incomplete"] is not False
    ):
        raise CoverageError("coverage is incomplete")
    failures = _exact(coverage["counter_failures"], "counter_failures", {"saturated", "rejected_in_flight"})
    if _int(failures["saturated"], "saturated", 0, U64_MAX) != 0:
        raise CoverageError("counter saturation blocks coverage")
    if _int(failures["rejected_in_flight"], "rejected_in_flight", 0, U64_MAX) != 0:
        raise CoverageError("rejected in-flight observations block coverage")
    counters, count = _counter_rows(coverage["counters"])
    segments = coverage["segments"]
    if not isinstance(segments, list) or len(segments) != 8:
        raise CoverageError("coverage must contain exactly eight segments")
    segment_rows = []
    prior_frame = -1
    for index, segment in enumerate(segments):
        row = _snapshot_row(segment, f"segment[{index}]", False)
        if row["index"] != index:
            raise CoverageError("segment indexes are not ordered")
        if row["accepted"] and row["frame_sequence"] < prior_frame:
            raise CoverageError("accepted segment frame sequence is reversed")
        if row["accepted"]:
            prior_frame = row["frame_sequence"]
        segment_rows.append(row)
    expected_segments = {0, 7, *range(1, len(expected_marks) + 1)}
    accepted_segments = {
        index for index, row in enumerate(segment_rows) if row["accepted"]
    }
    if accepted_segments != expected_segments:
        raise CoverageError("accepted segments do not match start, marks, and final")
    for index, row in enumerate(segment_rows):
        if not row["accepted"] and any(
            row[field] != default
            for field, default in (
                ("frame_sequence", 0),
                ("valid_fields", 0),
                ("gameplay_active", False),
                ("interface_update", False),
                ("active_player", -1),
                ("human_player_mask", 0),
                ("turn_owner_known", False),
                ("human_turn", False),
                ("available", False),
                ("civilization", 0),
                ("era", 0),
                ("year", 0),
                ("turn", 0),
            )
        ):
            raise CoverageError(f"unaccepted segment[{index}] contains evidence")
    checkpoints = coverage["checkpoints"]
    if not isinstance(checkpoints, list) or len(checkpoints) != 6:
        raise CoverageError("coverage must contain exactly six checkpoints")
    accepted = 0
    accepted_checkpoint_rows = []
    prior_segment = 0
    for index, checkpoint in enumerate(checkpoints):
        row = _snapshot_row(checkpoint, f"checkpoint[{index}]", True)
        if row["mark"] != index:
            raise CoverageError("checkpoint marks are not ordered")
        if row["segment"] >= 8:
            raise CoverageError("checkpoint segment is outside the segment table")
        should_accept = index < len(expected_marks)
        if row["accepted"] != should_accept:
            raise CoverageError("checkpoint acceptance differs from expected marks")
        if row["accepted"]:
            if row["segment"] != index + 1 or row["segment"] <= prior_segment:
                raise CoverageError("accepted checkpoint segments are not strictly ordered")
            if not segment_rows[row["segment"]]["accepted"]:
                raise CoverageError("accepted checkpoint has no accepted segment")
            comparable = {
                key: value
                for key, value in row.items()
                if key not in {"mark", "segment"}
            }
            segment_comparable = {
                key: value
                for key, value in segment_rows[row["segment"]].items()
                if key != "index"
            }
            if comparable != segment_comparable:
                raise CoverageError("checkpoint snapshot differs from its segment snapshot")
            prior_segment = row["segment"]
            accepted += 1
            accepted_checkpoint_rows.append(row)
        elif row["segment"] != 0 or any(
            row[field] != default
            for field, default in (
                ("frame_sequence", 0),
                ("valid_fields", 0),
                ("gameplay_active", False),
                ("interface_update", False),
                ("active_player", -1),
                ("human_player_mask", 0),
                ("turn_owner_known", False),
                ("human_turn", False),
                ("available", False),
                ("civilization", 0),
                ("era", 0),
                ("year", 0),
                ("turn", 0),
            )
        ):
            raise CoverageError("unaccepted checkpoint contains evidence")
    if accepted != len(expected_marks):
        raise CoverageError("checkpoint count does not match expected_marks")
    locked_checkpoint_states = {
        "NRD-TRANS-0002": {
            "valid_fields": 5,
            "gameplay_active": False,
            "interface_update": True,
            "active_player": -1,
            "human_player_mask": 0,
            "turn_owner_known": False,
            "human_turn": False,
            "available": False,
            "civilization": -1,
            "era": -1,
            "year": I32_MIN,
            "turn": -1,
        },
        "NRD-TRANS-0003": {
            "valid_fields": 127,
            "gameplay_active": True,
            "interface_update": True,
            "active_player": 0,
            "human_player_mask": 1,
            "turn_owner_known": True,
            "human_turn": True,
            "available": True,
            "civilization": 0,
            "era": 0,
            "year": -4000,
            "turn": 0,
        },
    }
    if transition in locked_checkpoint_states:
        expected_state = locked_checkpoint_states[transition]
        for label, snapshot in (
            ("checkpoint", accepted_checkpoint_rows[0]),
            ("final segment", segment_rows[7]),
        ):
            if any(snapshot[field] != value for field, value in expected_state.items()):
                raise CoverageError(
                    f"{transition} {label} differs from the owner-locked invariant"
                )
    if any(int(row["segment"]) not in accepted_segments for row in counters):
        raise CoverageError("counter evidence references an unaccepted segment")
    anomalies = coverage["anomalies"]
    if not isinstance(anomalies, list):
        raise CoverageError("anomalies must be a list")
    anomaly_ids: set[int] = set()
    anomaly_rows = []
    prior_anomaly_id = 0
    for index, anomaly in enumerate(anomalies):
        anomaly = _exact(anomaly, f"anomaly[{index}]", {"id", "name", "count"})
        anomaly_id = _int(anomaly["id"], "anomaly id", 1, max(ANOMALY_NAMES))
        if anomaly_id not in ANOMALY_NAMES or anomaly["name"] != ANOMALY_NAMES[anomaly_id]:
            raise CoverageError("anomaly ID/name differs from the stable table")
        if anomaly_id <= prior_anomaly_id:
            raise CoverageError("anomaly rows are not in stable ID order")
        if anomaly_id in anomaly_ids:
            raise CoverageError("duplicate anomaly ID")
        anomaly_ids.add(anomaly_id)
        prior_anomaly_id = anomaly_id
        _int(anomaly["count"], "anomaly count", 1, U64_MAX)
        anomaly_rows.append(dict(anomaly))
    anomaly_ids_present = {row["id"] for row in anomaly_rows}
    if anomaly_ids_present & {9, 10, 15}:
        raise CoverageError("checkpoint attribution anomaly blocks a valid transition")
    if 11 in anomaly_ids_present or 14 in anomaly_ids_present:
        raise CoverageError("counter or finalization anomaly blocks complete coverage")
    log, diagnostics = _check_log(root, run["log"], state)
    artifacts, inventory = _validate_artifacts(root, run, state, log)
    matrix = operation_matrix(counters)
    lifetime_claim = "not-applicable"
    zero_absence_claim = "blocked" if count == 0 else "not-applicable"
    coverage_artifact = next(
        item for item in artifacts if item["path"] == state["directories"]["output_directory"] + "/coverage.json"
    )
    run_record_sha256 = _hash(root / "run.json")
    operations = _operation_rows(state, counters, coverage_artifact, run_record_sha256)
    return {
        "schema": COVERAGE_SCHEMA,
        "run_id": run_id,
        "transition_id": transition,
        "input_digest": digest,
        "observer_byte_budget": coverage["observer_byte_budget"],
        "exit_class": exit_class,
        "lifetime_evaluation": lifetime_evaluation,
        "lifetime_claim": lifetime_claim,
        "zero_absence_claim": zero_absence_claim,
        "expected_marks": expected_marks,
        "fixture_id": state["fixture_id"],
        "fixture_sha256": state["fixture_sha256"],
        "cache_class": state["cache_class"],
        "repeat": state["repeat"],
        "count": count,
        "counters": counters,
        "matrix": matrix,
        "operations": operations,
        "anomalies": anomaly_rows,
        "log": log,
        "artifacts": sorted(artifacts, key=lambda item: item["path"]),
        "inventory": inventory,
        "diagnostics": diagnostics,
        "series_identity": state["series_identity"],
    }


def summarize_series(
    run_roots: Iterable[Path | str],
) -> dict[str, Any]:
    roots = list(run_roots)
    if len(roots) != 2:
        raise CoverageError("a matching series requires exactly two runs")
    summaries = [summarize_run(root) for root in roots]
    if summaries[0]["series_identity"] != summaries[1]["series_identity"]:
        raise CoverageError("series identity differs between repeats")
    if {item["repeat"] for item in summaries} != {1, 2}:
        raise CoverageError("matching series requires repeats 1 and 2")
    if len({item["run_id"] for item in summaries}) != 2:
        raise CoverageError("matching series run IDs must be unique")
    ordered = sorted(summaries, key=lambda item: item["repeat"])
    matrix = matrix_union_intersection(item["counters"] for item in ordered)
    discriminator_sets = [
        {row["discriminator"] for row in item["operations"] if row["outcome"] == "observed"}
        for item in ordered
    ]
    union = sorted(discriminator_sets[0] | discriminator_sets[1])
    intersection = sorted(discriminator_sets[0] & discriminator_sets[1])
    return {
        "schema": SERIES_SCHEMA,
        "series_identity": ordered[0]["series_identity"],
        "runs": [
            {"run_id": item["run_id"], "repeat": item["repeat"]} for item in ordered
        ],
        "operation_id": OPERATION_ID,
        "runtime_join_key": RUNTIME_JOIN_KEY,
        "contract_ids": [CONTRACT_ID],
        "matrix": matrix,
        "discriminators": {
            "union": union,
            "intersection": intersection,
            "stable_observed": intersection,
            "incidental": sorted(set(union) - set(intersection)),
        },
    }


def deterministic_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_roots", nargs="*", type=Path)
    parser.add_argument("--run-root", dest="run_root_options", action="append", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    roots = (args.run_root_options or []) + args.run_roots
    if not roots:
        parser.error("a run root is required")
    try:
        if args.output:
            output = args.output.resolve(strict=False)
            first_root = roots[0].resolve()
            try:
                output.relative_to(first_root)
            except ValueError as exc:
                raise CoverageError(
                    "--output must be a fresh summary.json contained by the first run root"
                ) from exc
            observer = (first_root / "observer").resolve()
            if (
                output.exists()
                or output == first_root
                or output.name != "summary.json"
                or output == observer
                or observer in output.parents
            ):
                raise CoverageError(
                    "--output must be a fresh summary.json outside observer output"
                )
        value = (
            summarize_run(roots[0])
            if len(roots) == 1
            else summarize_series(roots)
        )
        rendered = deterministic_json(value)
        if args.output:
            args.output.write_text(rendered + "\n", encoding="ascii", newline="\n")
        else:
            print(rendered)
    except (CoverageError, OSError) as exc:
        print(f"coverage blocked: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
