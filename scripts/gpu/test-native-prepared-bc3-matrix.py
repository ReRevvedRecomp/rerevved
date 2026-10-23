"""Prove ten BC3 CPU-upload forms with an endpoint-only CPU oracle.

Interpolated selectors require exact same-device Original/control/Prepared parity.
They have no CPU oracle claim.
"""

import argparse
import importlib.util
import json
import subprocess
import sys
import tomllib
from pathlib import Path

FORMS = (
    ("bc3-64-packed", 20, 64, 64, 4, 6, False, True, 1, "100"),
    ("bc3-128-packed", 20, 128, 128, 4, 7, False, True, 1, "74,82"),
    ("bc3-256x128-single", 20, 256, 128, 8, 0, False, False, 1, "85"),
    ("bc3-256x128-packed", 20, 256, 128, 8, 8, False, True, 1, "80"),
    ("bc3-256-packed", 20, 256, 256, 8, 8, False, True, 1, "55,57,59,102,104,106"),
    ("bc3-512x256-packed", 20, 512, 256, 16, 9, False, True, 1, "95,98"),
    ("bc3-512-single", 20, 512, 512, 16, 0, False, False, 1, "61,62,149"),
    ("bc3-512-packed", 20, 512, 512, 16, 9, False, True, 1, "19,31,49"),
    ("bc3-128-tiled-single", 20, 128, 128, 4, 0, True, False, 0, "75"),
    ("bc3-128-tiled-packed", 20, 128, 128, 4, 7, True, True, 0, "writer-bc3-packed"),
)
RANGES = (
    (0x2000, 0x4680, 0x4000),
    (0x4000, 0x8680, 0x8000),
    (0x8000, 0, 0),
    (0x8000, 0x8E80, 0x8000),
    (0x10000, 0xC680, 0xC000),
    (0x20000, 0x10E80, 0x10000),
    (0x40000, 0, 0),
    (0x40000, 0x1C680, 0x1C000),
    (16384, 0, 0),
    (16384, 49152, 32768),
)
TILED_LAYOUTS = {
    "bc3-128-tiled-single": ([0], [32], [0], [0]),
    "bc3-128-tiled-packed": (
        [0, 0, 16384, 32768, 32768, 32768, 32768, 32768],
        [32] * 8,
        [0, 0, 0, 4, 2, 1, 0, 0],
        [0, 0, 0, 0, 0, 0, 2, 1],
    ),
}
PLANE_BYTES = 1280 * 720 * 4
TRUE_RESULT_FIELDS = (
    "half_pixel_offset",
    "prepared_collector",
    "raw_old_new_copies",
    "fresh_texture_version_owner",
    "changed_completed_outputs",
    "independent_raw_oracle",
    "endpoint_oracle_claim",
    "all_levels_decoded",
    "all_channels_compared",
    "omissions_before_submission",
    "lifetime_recording_ready",
    "lifetime_native_completion",
    "lifetime_sdk_copy_retired",
    "lifetime_owner_released",
    "lifetime_owner_held_through_completion",
    "lifetime_owner_retired_after_drain",
    "device_failure_recording_ready",
    "device_failure_completion_failed",
    "device_failure_owner_held_before_remove",
    "device_failure_owner_retired_after_shutdown",
)


FAMILY = "native-prepared-bc3-matrix"


def shared_runner():
    """Load preparation helpers without creating bytecode beside tracked files."""
    sys.dont_write_bytecode = True
    source = Path(__file__).with_name("test-native-prepared.py")
    spec = importlib.util.spec_from_file_location("prepared_bc3_matrix_recipe", source)
    if spec is None or spec.loader is None:
        raise ValueError("shared recipe runner is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Match the translated candidate's shared-constant clip-space adjustment.
    anchor = "    output.lod = input.lod;"
    if module.CONTROL_VERTEX.count(anchor) != 1:
        raise ValueError("shared control vertex shader lost its LOD anchor")
    module.CONTROL_VERTEX = (
        "cbuffer SharedConstants : register(b2, space4) {\n"
        "    float2 halfPixelOffset : packoffset(c18.y);\n"
        "};\n\n"
        + module.CONTROL_VERTEX.replace(
            anchor,
            "    output.position.xy += halfPixelOffset * output.position.w;\n" + anchor,
        )
    )
    return module


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def load_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="ascii"))


def expect_fields(data: dict, expected: dict, label: str) -> None:
    for key, value in expected.items():
        actual = data.get(key)
        matches = type(actual) is type(value) and actual == value
        if matches and isinstance(value, list):
            matches = all(
                type(got) is type(want) for got, want in zip(actual, value, strict=True)
            )
        require(
            matches,
            f"{label}: {key} is {actual!r}, expected {value!r}",
        )


def plane(path: Path, copies: int = 1) -> bytes:
    value = path.read_bytes()
    require(len(value) == PLANE_BYTES * copies, f"incomplete RGBA8 output: {path}")
    if copies == 2:
        require(
            value[:PLANE_BYTES] == value[PLANE_BYTES:], f"unequal RT0 copies: {path}"
        )
    return value[:PLANE_BYTES]


def check_original(
    root: Path, index: int, isolated: bool = False, level: int | None = None
) -> tuple[bytes, bytes]:
    receipt = load_toml(root / f"original-{index}.toml")
    expect_fields(
        receipt,
        {
            "schema_version": 1,
            "first_lod": index if level is None else level,
            "second_lod": index if level is None else level,
            "uv_bias_base_texels": 0.25,
            "cpu_partial_overlap_writes": True,
            "source_raw_bytes_frozen": True,
            "cpu_update_overlap_written": True,
            "cpu_update_outside_unchanged": True,
        },
        f"Original {index}",
    )
    if isolated:
        expect_fields(
            receipt, {"cpu_update_opposite_unchanged": True}, "isolated field"
        )
    baseline = plane(root / f"original-{index}-baseline-expected.rgba8.bin")
    changed = plane(root / f"original-{index}-expected.rgba8.bin")
    require(
        plane(root / f"original-{index}-baseline-terminal.rgba8.bin") == baseline,
        f"Original baseline {index} differs from the CPU oracle",
    )
    require(
        plane(root / f"original-{index}-terminal.rgba8.bin") == changed,
        f"Original changed {index} differs from the CPU oracle",
    )
    require(baseline != changed, f"CPU update {index} did not change output")
    return baseline, changed


def check_form(root: Path, form: tuple) -> None:
    name, fmt, width, height, pitch, max_level, tiled, packed, endian, ids = form
    levels = max_level + 1
    require(fmt == 20, f"{name} is not a BC3 form")
    result = load_toml(root / "result.toml")
    base_extent, mip_extent, tail_offset = RANGES[FORMS.index(form)]
    expected = {
        "schema_version": 1,
        "form": name,
        "scene_ids": ids,
        "width": width,
        "height": height,
        "fetch_pitch": pitch,
        "prepared_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": endian,
        "prepared_swizzle": 0x688,
        "prepared_swizzled_signs": 0,
        "prepared_mip_min": 0,
        "prepared_mip_max": max_level,
        "prepared_packed_mips": packed,
        "prepared_tiled": tiled,
        "prepared_base_extent": base_extent,
        "prepared_mip_extent": mip_extent,
        "prepared_packed_tail_offset": tail_offset,
        "pixel_tolerance": 0,
        "interpolated_oracle_claim": False,
        "uv_bias_base_texels": 0.25,
        "pixel_center_conventions_match": width <= 512,
        "completed_pairs": levels,
        "parity_kind": "endpoint_oracle",
        "endpoint_alpha_codes": [0, 1],
        "endpoint_color_codes": [0, 1],
        "color_mode": "c0_gt_c1",
        "oracle_revision": "EXT_texture_compression_s3tc 1.6, 15 July 2013",
        "interpolated_parity_cases": 2,
        "cpu_update_levels": levels,
        "cpu_update_variant_checks": 2 * levels,
        "native_mutation_count": 2 * levels,
        "sensitivity_checks": 3 * levels,
        "omission_count": levels,
        "viewport_width": min(512, width),
        "viewport_height": min(512, height),
        "device_failure_publications": 0,
        "output_planes": 2,
    }
    if tiled:
        starts, pitches, packed_x, packed_y = TILED_LAYOUTS[name]
        expected.update(
            {
                "stored_level_starts": starts,
                "block_pitches": pitches,
                "packed_block_x": packed_x,
                "packed_block_y": packed_y,
                "tiled_storage_guards": True,
            }
        )
    expected.update(dict.fromkeys(TRUE_RESULT_FIELDS, True))
    expect_fields(result, expected, name)
    check_interpolated(root)
    for level in range(levels):
        baseline, changed = check_original(root, level)
        for route in ("candidate", "control"):
            require(
                plane(root / f"{route}-baseline-{level}.bin", 2) == baseline,
                f"{name} {route} baseline LOD {level} differs from the CPU oracle",
            )
            require(
                plane(root / f"{route}-{level}.bin", 2) == changed,
                f"{name} {route} changed LOD {level} differs from the CPU oracle",
            )
        mutation_kinds = ("color", "alpha")
        for kind in mutation_kinds:
            candidate = plane(root / f"mutated-{kind}-candidate-{level}.bin", 2)
            control = plane(root / f"mutated-{kind}-control-{level}.bin", 2)
            require(candidate == control, f"{name} {kind} mutation routes differ")
            require(candidate != changed, f"{name} {kind} mutation is insensitive")
        expect_fields(
            result,
            {
                f"omission_{level}_level": level,
                f"omission_{level}_stage": "BuildNativeGameplayDrawRecipe",
                f"omission_{level}_output_exists": False,
            },
            f"{name} omission {level}",
        )
        require(result.get(f"omission_{level}_error"), f"{name} omission has no error")
        require(
            not (root / f"omitted-mip-{level}.bin").exists(),
            f"{name} omission unexpectedly published pixels",
        )
        for kind_index, kind in enumerate(("color-only", "alpha-only")):
            baseline, changed = check_original(
                root, levels + level * 2 + kind_index, isolated=True, level=level
            )
            for route in ("candidate", "control"):
                states = (("baseline", baseline), ("changed", changed))
                for state, expected_plane in states:
                    value = plane(
                        root / f"variant-{kind}-{level}-{route}-{state}.bin", 2
                    )
                    require(
                        value == expected_plane,
                        f"{name} isolated {kind} {route} mismatch",
                    )
    require(
        result.get("prepared_negative_probe_count") == 6,
        f"{name} lost callback probes",
    )
    for index in range(6):
        expect_fields(
            result,
            {
                f"prepared_negative_{index}_rejected": True,
                f"prepared_negative_{index}_no_admitted_event": True,
            },
            f"{name} negative {index}",
        )
        require(
            result.get(f"prepared_negative_{index}_error"),
            f"{name} negative has no error",
        )


def check_interpolated(root: Path) -> None:
    for mode, label in enumerate(("a0_gt_a1", "a0_le_a1")):
        prefix = f"interpolated-{mode}"
        receipt = load_toml(root / f"{prefix}.toml")
        expect_fields(
            receipt,
            {
                "schema_version": 1,
                "parity_kind": "device_interpolated",
                "oracle_claim": False,
                "alpha_mode": label,
                "sampled_lod": 0,
                "alpha_codes": list(range(8)),
                "color_codes": list(range(4)),
                "pixel_tolerance": 0,
                "half_pixel_offset": True,
                "uv_bias_base_texels": 0.25,
                "pixel_center_conventions_match": True,
                "same_device": True,
                "original_control_prepared_equal": True,
            },
            prefix,
        )
        identity = (receipt.get("original_luid_low"), receipt.get("original_luid_high"))
        require(all(type(word) is int for word in identity), "missing Original device")
        for route in ("control", "prepared"):
            expect_fields(
                receipt,
                {f"{route}_luid_low": identity[0], f"{route}_luid_high": identity[1]},
                f"{prefix} {route} device",
            )
        original = plane(root / f"{prefix}-original.bin")
        for route in ("candidate", "control"):
            require(
                plane(root / f"{prefix}-{route}.bin", 2) == original,
                f"{prefix} {route} differs from same-device Original",
            )
        require(
            not list(root.glob(f"{prefix}*expected*")),
            "interpolated parity must not publish oracle planes",
        )


def check_results(args: argparse.Namespace) -> None:
    output = args.output.resolve(strict=True)
    matrix = load_toml(output / "matrix.toml")
    expect_fields(
        matrix,
        {
            "schema_version": 1,
            "form_count": len(FORMS),
            "endpoint_parity_kind": "endpoint_oracle",
            "interpolated_parity_kind": "device_interpolated",
            "pixel_tolerance": 0,
            "interpolated_oracle_claim": False,
            "uv_bias_base_texels": 0.25,
        },
        "matrix",
    )
    expect_fields(
        matrix.get("completion", {}),
        {"completed_forms": len(FORMS)},
        "matrix completion",
    )
    rows = matrix.get("forms", [])
    require(
        len(rows) == len(FORMS), f"matrix must have exactly {len(FORMS)} form results"
    )
    for row, form in zip(rows, FORMS, strict=True):
        name = form[0]
        expect_fields(
            row, {"name": name, "result": f"{name}/result.toml"}, "matrix row"
        )
        check_form(output / name, form)
    print("BC3 matrix schema, zero-tolerance pixels and sensitivity controls: PASS")


def check_prepared(args: argparse.Namespace) -> None:
    shared = shared_runner()
    prepared = json.loads(args.prepared.read_text(encoding="ascii"))
    expect_fields(prepared, {"schema_version": 1}, "prepared matrix inputs")
    expect_fields(
        prepared.get("fixture", {}),
        {
            "form": "bc3-64-packed",
            "uv_bias_base_texels": 0.25,
            "width": 64,
            "height": 64,
            "mip_levels": 7,
            "pitch": 4,
            "format_value": 20,
            "half_pixel_offset": True,
            "endpoint_parity_kind": "endpoint_oracle",
            "interpolated_parity_kind": "device_interpolated",
            "interpolated_oracle_claim": False,
        },
        "shader-generation fixture",
    )
    shared.validate_uv_bias(prepared, 64, 64)
    emitted = json.loads(
        (Path(prepared["raw_inputs"]) / "fixture.json").read_text(encoding="ascii")
    )
    expect_fields(emitted, prepared["fixture"], "emitted fixture")
    paths = pinned_paths(prepared)
    pins = prepared.get("sha256", {})
    require(set(pins) == set(paths), "prepared inputs lack exact source/catalog pins")
    for name, path in paths.items():
        require(shared.digest(path) == pins[name], f"stale prepared input: {name}")
    for key in ("raw_inputs", "candidate_catalog", "control_catalog"):
        shared.require_directory(Path(prepared[key]), key)
    shared.require_file(Path(prepared["candidate_source"]), "candidate source")
    control_source = shared.require_file(
        Path(prepared["control_vertex_source"]), "control vertex source"
    ).read_text(encoding="ascii")
    require(
        "float2 halfPixelOffset : packoffset(c18.y);" in control_source
        and "output.position.xy += halfPixelOffset * output.position.w;"
        in control_source,
        "control vertex source must apply the recipe's half-pixel offset",
    )
    for name in (
        "registers.le.bin",
        "vertex-ucode.le.bin",
        "pixel-ucode.le.bin",
        "texture-base.bc3.bin",
        "texture-mips.bc3.bin",
        "fixture.json",
    ):
        shared.require_file(Path(prepared["raw_inputs"]) / name, name)
    print("BC3 matrix prepared input schema: PASS")


def run_test(args: argparse.Namespace) -> None:
    check_prepared(args)
    shared_runner().run_test(args, FAMILY)
    check_results(args)


def pinned_paths(prepared: dict) -> dict[str, Path]:
    paths = {
        name: Path(prepared["raw_inputs"]) / name
        for name in (
            "fixture.json",
            "registers.le.bin",
            "vertex-ucode.le.bin",
            "pixel-ucode.le.bin",
            "vertices.bin",
            "indices.bin",
            "texture-base.bc3.bin",
            "texture-mips.bc3.bin",
        )
    }
    for route in ("candidate", "control"):
        root = Path(prepared[f"{route}_catalog"])
        paths[f"{route}_catalog"] = root / "shaders.toml"
        for stage in ("vs", "ps"):
            paths[f"{route}_{stage}"] = root / f"{route}.{stage}.dxil"
    for name in ("candidate_source", "control_vertex_source", "control_pixel_source"):
        paths[name] = Path(prepared[name])
    return paths


def prepare(args: argparse.Namespace) -> None:
    shared = shared_runner()
    shared.prepare(args, FAMILY)
    path = args.output / "prepared.json"
    prepared = json.loads(path.read_text(encoding="ascii"))
    prepared["sha256"] = {
        name: shared.digest(value) for name, value in pinned_paths(prepared).items()
    }
    path.write_text(json.dumps(prepared, indent=2) + "\n", encoding="ascii")
    check_prepared(argparse.Namespace(prepared=path))


def parser() -> argparse.ArgumentParser:
    # Keep the maintained recipe's tool flags and fresh-output handling.
    shared = shared_runner()
    root = shared.parser()
    root.description = __doc__
    subparsers = next(
        action
        for action in root._actions
        if isinstance(action, argparse._SubParsersAction)
    )
    subparsers.choices["run"].set_defaults(function=run_test)
    subparsers.choices["check"].set_defaults(function=check_prepared)
    subparsers.choices["prepare"].set_defaults(function=prepare)
    results = subparsers.add_parser("check-results")
    results.add_argument("--output", type=Path, required=True)
    results.set_defaults(function=check_results)
    return root


if __name__ == "__main__":
    arguments = parser().parse_args()
    try:
        arguments.function(arguments)
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        subprocess.CalledProcessError,
    ) as error:
        raise SystemExit(f"native prepared BC3 matrix: {error}") from error
