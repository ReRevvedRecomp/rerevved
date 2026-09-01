#!/usr/bin/env python3
"""Validate and generate the focused native-renderer observer hooks."""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tomllib
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = REPO / "config" / "native_renderer_coverage.toml"
DEFAULT_HOOK_OUTPUT = REPO / "config" / "native_renderer_coverage_hooks.toml"
DEFAULT_INCLUDE_OUTPUT = REPO / "src" / "native_renderer_coverage_hooks.inc"
DEFAULT_EXISTING_HOOKS = REPO / "config" / "rerevved_hooks.toml"

RESEARCH_COMMIT = "200e737fd13d9368c86d95ac154cbaf66a34dcc9"
AGGREGATE_SHA256 = "3e2ff06a0dccfaad7f5e64729e40b6255ced051b897f318ba13a0d21344ec6c7"
IMAGE_SHA256 = "5C7A8C3AD9B6A9D39CC9BBF3DA5AB23015A568C65C723D298F846086324C4680"
RUNTIME_JOIN_KEY = "d3d:0x826A3568"
CONTRACT_ID = "NRD-CONTRACT-0001"
HOOK_ADDRESSES = (0x82303E3C, 0x82303E8C)
PRIMITIVE_VALUE = 4
SEGMENT_COUNT = 8
OPERATION_COUNT = 1
VALUE_DOMAIN_COUNT = 2
HOOK_SITE_COUNT = len(HOOK_ADDRESSES)
# Each segment owns one semantic row for every operation/domain/site tuple.
COUNTER_ROWS_PER_SEGMENT = OPERATION_COUNT * VALUE_DOMAIN_COUNT * HOOK_SITE_COUNT
COUNTER_ROW_COUNT = SEGMENT_COUNT * COUNTER_ROWS_PER_SEGMENT
# sizeof(Observer) for the fixed-layout observer in native_renderer_coverage.h
# on the supported 64-bit build. Keep this in step with ObjectSizeBytes().
OBSERVER_BYTE_BUDGET = 3456

ADDRESS_RE = re.compile(r"^0x[0-9A-F]{8}$")
JOIN_RE = re.compile(r"^d3d:0x[0-9A-F]{8}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

TOP_LEVEL_POINTERS = {
    "schema_version": "#/partialExport/schema_version",
    "image_sha256": "#/partialExport/image_sha256",
    "surface": "#/partialExport/surface",
}
OPERATION_POINTERS = {
    "operation_id": "#/partialExport/operations/0/operation_id",
    "runtime_join_key": "#/partialExport/operations/0/runtime_join_key",
    "roles": "#/partialExport/operations/0/roles",
    "contract_ids": "#/partialExport/operations/0/contract_ids",
    "hook_sites": "#/partialExport/operations/0/hook_sites",
    "registers": "#/partialExport/operations/0/registers",
    "value_domains": "#/partialExport/operations/0/value_domains",
    "claim_refs": "#/partialExport/operations/0/claim_refs",
}
CLAIM_REFS = [
    "#/surface/1",
    "#/contracts/0",
    "#/completenessRoots/0",
    "RVA-SYM-0255",
    "RVA-REL-0349",
    "gfx-draw-producer.json#/drawSubmission/issue",
    "native-renderer-d3d-draw-lowering-frontier.json#/committedExecutorReferenceReplay17",
]


class ValidationError(ValueError):
    """Raised when the reviewed input is not the accepted snapshot."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def _require_keys(value: Any, expected: set[str], label: str) -> None:
    _require(isinstance(value, dict), f"{label} must be a table")
    _require(set(value) == expected, f"{label} keys differ from the accepted schema")


def _require_string(value: Any, expected: str, label: str) -> None:
    _require(type(value) is str and value == expected, f"{label} differs from the accepted value")


def _address_value(value: Any, label: str) -> int:
    if type(value) is int:
        _require(0 <= value <= 0xFFFFFFFF, f"{label} is outside uint32")
        return value
    if type(value) is str and ADDRESS_RE.fullmatch(value):
        return int(value[2:], 16)
    raise ValidationError(f"{label} is not an eight-digit hexadecimal address")


def _input_address_value(value: Any, label: str) -> int:
    _require(type(value) is int, f"{label} must be a TOML hexadecimal integer")
    return _address_value(value, label)


def _address_text(value: int) -> str:
    return f"0x{value:08X}"


def _resolve_pointer(document: dict[str, Any], pointer: str) -> Any:
    _require(pointer.startswith("#/"), f"invalid source pointer: {pointer}")
    current: Any = document
    for part in pointer[2:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, list):
            _require(part.isdigit(), f"source pointer list index is invalid: {pointer}")
            index = int(part)
            _require(index < len(current), f"source pointer is out of range: {pointer}")
            current = current[index]
        else:
            _require(isinstance(current, dict) and part in current, f"source pointer component is missing: {pointer}")
            current = current[part]
    return current


def _deep_equal(left: Any, right: Any) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(_deep_equal(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(_deep_equal(a, b) for a, b in zip(left, right))
    return left == right


def _snapshot_document(data: dict[str, Any]) -> dict[str, Any]:
    """Expose the TOML snapshot as the accepted partial-export shape."""
    snapshot = data["snapshot"]
    return {"partialExport": snapshot}


def _validate_pointers(document: dict[str, Any], pointers: dict[str, Any], expected: dict[str, str], container: dict[str, Any], label: str) -> None:
    _require_keys(pointers, set(expected), f"{label} source_pointers")
    for field, pointer in expected.items():
        _require_string(pointers.get(field), pointer, f"{label} {field} source pointer")
        _require(_deep_equal(_resolve_pointer(document, pointer), container[field]), f"{label} {field} source pointer value differs")


def _validate_domain(domain: Any, label: str) -> None:
    _require(isinstance(domain, dict), f"{label} must be a table")
    value_kind = domain.get("value_kind")
    if domain.get("id") == "primitive-4":
        _require_keys(domain, {"id", "value_kind", "value", "selection"}, label)
        _require_string(value_kind, "integer", f"{label} value_kind")
        _require(type(domain.get("value")) is int and domain["value"] == PRIMITIVE_VALUE, f"{label} value differs from 4")
        _require_string(domain.get("selection"), "site-fixed", f"{label} selection")
    elif domain.get("id") == "unknown":
        _require_keys(domain, {"id", "value_kind", "selection"}, label)
        _require_string(value_kind, "null", f"{label} value_kind")
        _require_string(domain.get("selection"), "unmapped-input", f"{label} selection")
    else:
        raise ValidationError(f"{label} has an unexpected domain ID")


def _validate_snapshot(data: dict[str, Any]) -> dict[str, Any]:
    _require_keys(data, {"snapshot", "observer"}, "input")
    snapshot = data["snapshot"]
    observer = data["observer"]
    _require_keys(snapshot, {"schema_version", "published_research_commit", "aggregate_sha256", "image_sha256", "surface", "source_pointers", "operations"}, "snapshot")
    _require_keys(observer, {"segment_count"}, "observer")
    _require(type(snapshot["schema_version"]) is int and snapshot["schema_version"] == 1, "snapshot schema_version differs from 1")
    _require_string(snapshot["published_research_commit"], RESEARCH_COMMIT, "published_research_commit")
    _require(COMMIT_RE.fullmatch(snapshot["published_research_commit"]) is not None, "published_research_commit is not lowercase full hex")
    _require_string(snapshot["aggregate_sha256"], AGGREGATE_SHA256, "aggregate_sha256")
    _require(SHA256_RE.fullmatch(snapshot["aggregate_sha256"]) is not None, "aggregate_sha256 is not lowercase SHA-256")
    _require_string(snapshot["image_sha256"], IMAGE_SHA256, "image_sha256")
    _require(re.fullmatch(r"[0-9A-F]{64}", snapshot["image_sha256"]) is not None, "image_sha256 is not uppercase SHA-256")
    _require_string(snapshot["surface"], "partial", "surface")
    _require(type(observer["segment_count"]) is int and observer["segment_count"] == SEGMENT_COUNT, "observer segment count differs from 8")

    document = _snapshot_document(data)
    _validate_pointers(document, snapshot["source_pointers"], TOP_LEVEL_POINTERS, snapshot, "snapshot")
    operations = snapshot["operations"]
    _require(type(operations) is list and len(operations) == 1, "snapshot must contain one operation")
    operation = operations[0]
    _require_keys(operation, set(OPERATION_POINTERS) | {"source_pointers"}, "operation")
    _require_string(operation["operation_id"], "NRD-OP-0002", "operation_id")
    _require_string(operation["runtime_join_key"], RUNTIME_JOIN_KEY, "runtime_join_key")
    _require(JOIN_RE.fullmatch(operation["runtime_join_key"]) is not None, "runtime_join_key is malformed")
    _require(operation["roles"] == ["wrapper", "lowering-boundary"], "operation roles differ from accepted values")
    _require(operation["contract_ids"] == [CONTRACT_ID], "operation contract_ids differ from accepted values")
    _require(operation["registers"] == [], "operation registers must be empty")
    _require(operation["claim_refs"] == CLAIM_REFS, "operation claim_refs differ from the accepted coverage snapshot")
    _validate_pointers(document, operation["source_pointers"], OPERATION_POINTERS, operation, "operation")

    hooks = operation["hook_sites"]
    _require(type(hooks) is list and len(hooks) == len(HOOK_ADDRESSES), "operation must contain two hook sites")
    addresses: list[int] = []
    for index, hook in enumerate(hooks):
        _require_keys(hook, {"address", "phase", "discriminator"}, f"hook_sites[{index}]")
        address = _input_address_value(
            hook["address"], f"hook_sites[{index}] address"
        )
        _require(address in HOOK_ADDRESSES, f"hook_sites[{index}] address differs from the accepted coverage snapshot")
        _require_string(hook["phase"], "value", f"hook_sites[{index}] phase")
        _require_string(hook["discriminator"], "primitive-4", f"hook_sites[{index}] discriminator")
        _require(address not in addresses, f"duplicate hook address: {_address_text(address)}")
        addresses.append(address)
    _require(set(addresses) == set(HOOK_ADDRESSES), "hook addresses differ from the accepted coverage snapshot")

    domains = operation["value_domains"]
    _require(type(domains) is list and len(domains) == 2, "operation must contain primitive-4 and unknown domains")
    for index, domain in enumerate(domains):
        _validate_domain(domain, f"value_domains[{index}]")
    _require({domain["id"] for domain in domains} == {"primitive-4", "unknown"}, "value domain IDs differ from accepted values")
    return data


def _load_toml(path: Path) -> dict[str, Any]:
    try:
        raw = path.read_bytes()
        text = raw.decode("ascii")
        value = tomllib.loads(text)
    except UnicodeDecodeError as exc:
        raise ValidationError(f"input is not deterministic ASCII: {path}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise ValidationError(f"invalid TOML in {path}: {exc}") from exc
    _require(isinstance(value, dict), f"TOML root is not a table: {path}")
    return value


def load_and_validate(path: Path = DEFAULT_INPUT, existing_hooks: Path = DEFAULT_EXISTING_HOOKS) -> tuple[dict[str, Any], bytes, dict[str, Any]]:
    raw = path.read_bytes()
    data = _load_toml(path)
    _validate_snapshot(data)
    existing = _load_toml(existing_hooks)
    _validate_existing_hooks(existing)
    _validate_collisions(data, existing)
    return data, raw, existing


def _validate_existing_hooks(data: dict[str, Any]) -> None:
    _require(set(data) == {"midasm_hook"}, "rerevved_hooks.toml has an unexpected schema")
    hooks = data["midasm_hook"]
    _require(type(hooks) is list, "rerevved_hooks.toml midasm_hook must be an array")
    addresses: set[int] = set()
    for index, hook in enumerate(hooks):
        _require(isinstance(hook, dict), f"existing hook {index} is not a table")
        _require(set(hook) <= {"address", "name", "registers"}, f"existing hook {index} has unexpected keys")
        _require("address" in hook and "name" in hook, f"existing hook {index} lacks address or name")
        address = _address_value(hook["address"], f"existing hook {index} address")
        _require(address not in addresses, f"duplicate existing hook address: {_address_text(address)}")
        addresses.add(address)
        _require(type(hook["name"]) is str and bool(hook["name"]), f"existing hook {index} has invalid name")
        if "registers" in hook:
            _require(type(hook["registers"]) is list and all(type(reg) is str for reg in hook["registers"]), f"existing hook {index} registers are invalid")


def _validate_collisions(data: dict[str, Any], existing: dict[str, Any]) -> None:
    existing_addresses = {_address_value(hook["address"], "existing hook address") for hook in existing["midasm_hook"]}
    existing_names = {hook["name"] for hook in existing["midasm_hook"]}
    operation = data["snapshot"]["operations"][0]
    for hook in operation["hook_sites"]:
        address = _address_value(hook["address"], "hook address")
        _require(address not in existing_addresses, f"hook address collides with rerevved_hooks.toml: {_address_text(address)}")
        name = _wrapper_name(address)
        _require(name not in existing_names, f"hook name collides with rerevved_hooks.toml: {name}")


def _wrapper_name(address: int) -> str:
    return f"ReRevvedNativeRendererCoverageSite{address:08X}"


def _canonical_hooks(data: dict[str, Any]) -> list[tuple[int, str]]:
    hooks = data["snapshot"]["operations"][0]["hook_sites"]
    return sorted((_address_value(hook["address"], "hook address"), _wrapper_name(_address_value(hook["address"], "hook address"))) for hook in hooks)


def _aligned_declarations(declarations: list[tuple[str, str, str]]) -> list[str]:
    type_width = max(len(value_type) for value_type, _, _ in declarations)
    name_width = max(len(name) for _, name, _ in declarations)
    return [
        f"inline constexpr {value_type:<{type_width}} {name:<{name_width}} = {value};"
        for value_type, name, value in declarations
    ]


def generate_hook_toml(data: dict[str, Any], input_sha256: str) -> bytes:
    _validate_snapshot(data)
    lines = [
        "# Generated by scripts/gen-native-renderer-coverage.py.",
        f"# Input SHA-256: {input_sha256}",
        "# Do not edit; regenerate from config/native_renderer_coverage.toml.",
        "",
    ]
    for address, name in _canonical_hooks(data):
        lines.extend([
            "[[midasm_hook]]",
            f"address = {_address_text(address)}",
            f'name = "{name}"',
            "registers = []",
            "",
        ])
    return "\n".join(lines).encode("ascii")


def generate_include(data: dict[str, Any], input_sha256: str) -> bytes:
    _validate_snapshot(data)
    hooks = _canonical_hooks(data)
    lines = [
        "// Generated by scripts/gen-native-renderer-coverage.py.",
        f"// Input SHA-256: {input_sha256}",
        "// Do not edit; regenerate from config/native_renderer_coverage.toml.",
        "",
        "#include <cstdint>",
        "",
        "namespace rerevved::native_renderer",
        "{",
        "",
        "void RecordSiteFixedValue(std::uint32_t site_index, std::int64_t value) noexcept;",
        "",
        "} // namespace rerevved::native_renderer",
        "",
        "namespace rerevved::native_renderer::generated",
        "{",
        "",
    ]
    declarations = [
        ("std::uint32_t", "kObserverSegmentCount", str(SEGMENT_COUNT)),
        ("std::uint32_t", "kObserverByteBudget", str(OBSERVER_BYTE_BUDGET)),
        ("std::uint32_t", "kOperationCount", str(OPERATION_COUNT)),
        ("std::uint32_t", "kValueDomainCount", str(VALUE_DOMAIN_COUNT)),
        ("std::uint32_t", "kHookSiteCount", str(len(hooks))),
        ("std::uint32_t", "kCounterRowsPerSegment", str(COUNTER_ROWS_PER_SEGMENT)),
        ("std::uint32_t", "kSegmentCounterRowCount", str(COUNTER_ROW_COUNT)),
        ("std::uint32_t", "kPrimitiveValue", str(PRIMITIVE_VALUE)),
        ("char", "kPrimitiveDomainId[]", '"primitive-4"'),
        ("char", "kUnknownDomainId[]", '"unknown"'),
        ("char", "kRoleWrapper[]", '"wrapper"'),
        ("char", "kRoleLoweringBoundary[]", '"lowering-boundary"'),
        ("char", "kValuePhase[]", '"value"'),
        ("char", "kSiteFixedSelection[]", '"site-fixed"'),
        ("char", "kUnmappedInputSelection[]", '"unmapped-input"'),
        ("char", "kContractId[]", f'"{CONTRACT_ID}"'),
        ("char", "kInputSha256[]", f'"{input_sha256}"'),
        ("char", "kOperationId[]", '"NRD-OP-0002"'),
        ("char", "kRuntimeJoinKey[]", f'"{RUNTIME_JOIN_KEY}"'),
    ]
    for index, (address, _) in enumerate(hooks):
        declarations.extend([
            ("std::uint32_t", f"kSiteIndex{address:08X}", str(index)),
            ("std::uint32_t", f"kSiteAddress{address:08X}", _address_text(address)),
        ])
    lines.extend(_aligned_declarations(declarations))
    lines.extend([
        "",
        "} // namespace rerevved::native_renderer::generated",
        "",
    ])
    for address, name in hooks:
        lines.extend([
            f"void {name}() noexcept",
            "{",
            f"    constexpr std::uint32_t site_index = rerevved::native_renderer::generated::kSiteIndex{address:08X};",
            "    rerevved::native_renderer::RecordSiteFixedValue(site_index, 4);",
            "}",
            "",
        ])
    return "\n".join(lines).encode("ascii")


def generate_outputs(data: dict[str, Any], input_bytes: bytes) -> tuple[bytes, bytes, str]:
    _validate_snapshot(data)
    input_sha256 = hashlib.sha256(input_bytes).hexdigest()
    return generate_hook_toml(data, input_sha256), generate_include(data, input_sha256), input_sha256


def _write_if_changed(path: Path, content: bytes) -> bool:
    if path.exists() and path.read_bytes() == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return True


def _check_output(path: Path, expected: bytes) -> None:
    _require(path.is_file(), f"generated output is missing: {path}")
    _require(path.read_bytes() == expected, f"generated output drift: {path}")


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--existing-hooks", type=Path, default=DEFAULT_EXISTING_HOOKS)
    parser.add_argument("--hooks-output", type=Path, default=DEFAULT_HOOK_OUTPUT)
    parser.add_argument("--include-output", type=Path, default=DEFAULT_INCLUDE_OUTPUT)
    parser.add_argument("--check", action="store_true", help="verify outputs without writing files")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        data, input_bytes, _ = load_and_validate(args.input, args.existing_hooks)
        hook_bytes, include_bytes, input_sha256 = generate_outputs(data, input_bytes)
        if args.check:
            _check_output(args.hooks_output, hook_bytes)
            _check_output(args.include_output, include_bytes)
            action = "checked"
        else:
            _write_if_changed(args.hooks_output, hook_bytes)
            _write_if_changed(args.include_output, include_bytes)
            action = "generated"
        hook_count = len(data["snapshot"]["operations"][0]["hook_sites"])
        domain_count = len(data["snapshot"]["operations"][0]["value_domains"])
        print(
            f"native-renderer-coverage: {action}; hooks={hook_count}; operations=1; "
            f"domains={domain_count}; segments={SEGMENT_COUNT}; "
            f"input_sha256={input_sha256}; observer_byte_budget={OBSERVER_BYTE_BUDGET}"
        )
        return 0
    except (OSError, ValidationError, tomllib.TOMLDecodeError) as exc:
        print(f"native-renderer-coverage: error: {exc}", file=sys.stderr)
        return 1
if __name__ == "__main__":
    raise SystemExit(main())
