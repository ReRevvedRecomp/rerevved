"""Prepare, run and validate the 18 exact unsigned BC1/BC2 forms.

The accepted rectangular BC2 runner owns the shared shader recipe and catalog
preparation. This runner reuses those routines with the matrix's emitted words.
Expected runtime is 30-90 minutes for the Original update runs plus native
controls; this is an unmeasured budget, not a performance acceptance claim.
"""

import argparse
import importlib.util
import json
import subprocess
import sys
import tomllib
from pathlib import Path

FORMS = (
    ("bc1-8-single", 18, 8, 8, 4, 0, False, False, 1, "20"),
    ("bc1-32-packed", 18, 32, 32, 4, 5, False, True, 1, "88"),
    ("bc1-512x256-packed", 18, 512, 256, 16, 9, False, True, 1, "79,94,97"),
    ("bc1-512-single", 18, 512, 512, 16, 0, False, False, 1, "13"),
    ("bc1-1024-single", 18, 1024, 1024, 32, 0, False, False, 1, "68"),
    ("bc1-1024-packed", 18, 1024, 1024, 32, 10, False, True, 1, "56,58,60"),
    ("bc1-2048-single", 18, 2048, 2048, 64, 0, False, False, 1, "25"),
    ("bc1-4096-single", 18, 4096, 4096, 128, 0, False, False, 1, "26"),
    ("bc2-128x64-packed", 19, 128, 64, 4, 7, False, True, 1, "91"),
    ("bc2-256x128-packed", 19, 256, 128, 8, 8, False, True, 1, "89"),
    ("bc2-256-single", 19, 256, 256, 8, 0, False, False, 1, "140"),
    ("bc2-256-packed", 19, 256, 256, 8, 8, False, True, 1, "7,8,51,113,115"),
    ("bc2-512x256-packed", 19, 512, 256, 16, 9, False, True, 1, "78,93,96"),
    ("bc2-512-packed", 19, 512, 512, 16, 9, False, True, 1, "64,65,66"),
    ("bc1-128x64-tiled-packed", 18, 128, 64, 4, 7, True, True, 0, "35"),
    ("bc1-256x128-tiled-packed", 18, 256, 128, 8, 8, True, True, 0, "36"),
    ("bc2-128x64-tiled-packed", 19, 128, 64, 4, 7, True, True, 0, "37"),
    (
        "bc2-32x64-tiled-padded-single",
        19,
        32,
        64,
        4,
        0,
        True,
        False,
        0,
        "writer-bc2-padded",
    ),
)
# Frozen Prepared base/mip/tail extents are checked by the C++ fixture.
# Linear bases retain covered legacy ranges; selected tiled bases include padding.
RANGES = (
    (0x200, 0, 0),
    (0x800, 0x340, 0),
    (0x10000, 0x8740, 0x8000),
    (0x20000, 0, 0),
    (0x80000, 0, 0),
    (0x80000, 0x2E340, 0x2E000),
    (0x200000, 0, 0),
    (0x800000, 0, 0),
    (0x2000, 0x4E80, 0x4000),
    (0x8000, 0x8E80, 0x8000),
    (0x10000, 0, 0),
    (0x10000, 0xC680, 0xC000),
    (0x20000, 0x10E80, 0x10000),
    (0x40000, 0x1C680, 0x1C000),
    (8192, 16384, 8192),
    (16384, 24576, 16384),
    (16384, 32768, 16384),
    (16384, 0, 0),
)
TILED_LAYOUTS = {
    "bc1-128x64-tiled-packed": (
        [0, 0, 8192, 8192, 8192, 8192, 8192, 8192],
        [32] * 8,
        [0, 0, 0, 0, 0, 4, 2, 1],
        [0, 0, 4, 2, 1, 0, 0, 0],
    ),
    "bc1-256x128-tiled-packed": (
        [0, 0, 8192, 16384, 16384, 16384, 16384, 16384, 16384],
        [64] + [32] * 8,
        [0, 0, 0, 0, 0, 0, 4, 2, 1],
        [0, 0, 0, 4, 2, 1, 0, 0, 0],
    ),
    "bc2-128x64-tiled-packed": (
        [0, 0, 16384, 16384, 16384, 16384, 16384, 16384],
        [32] * 8,
        [0, 0, 0, 0, 0, 4, 2, 1],
        [0, 0, 4, 2, 1, 0, 0, 0],
    ),
    "bc2-32x64-tiled-padded-single": ([0], [32], [0], [0]),
}
PLANE_BYTES = 1280 * 720 * 4
TRUE_RESULT_FIELDS = (
    "half_pixel_offset",
    "prepared_collector",
    "raw_old_new_copies",
    "fresh_texture_version_owner",
    "changed_completed_outputs",
    "independent_raw_oracle",
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


FAMILY = "native-prepared-dxt-matrix"


def shared_runner():
    """Load preparation helpers without creating bytecode beside tracked files."""
    sys.dont_write_bytecode = True
    source = Path(__file__).with_name("test-native-prepared.py")
    spec = importlib.util.spec_from_file_location("prepared_dxt_recipe", source)
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
        require(
            type(actual) is type(value) and actual == value,
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
    root: Path, index: int, isolated: bool = False
) -> tuple[bytes, bytes]:
    receipt = load_toml(root / f"original-{index}.toml")
    expect_fields(
        receipt,
        {
            "schema_version": 1,
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
    bc2 = fmt == 19
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
        "uv_bias_base_texels": 0.25,
        "pixel_center_conventions_match": width <= 512,
        "completed_pairs": levels,
        "cpu_update_levels": levels,
        "cpu_update_variant_checks": 2 * levels if bc2 else 0,
        "native_mutation_count": (2 if bc2 else 1) * levels,
        "sensitivity_checks": (3 if bc2 else 2) * levels,
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
        mutation_kinds = ("color", "alpha") if bc2 else ("color",)
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
        if bc2:
            for kind_index, kind in enumerate(("color-only", "alpha-only")):
                baseline, changed = check_original(
                    root, levels + level * 2 + kind_index, isolated=True
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


def check_results(args: argparse.Namespace) -> None:
    output = args.output.resolve(strict=True)
    matrix = load_toml(output / "matrix.toml")
    expect_fields(
        matrix,
        {
            "schema_version": 1,
            "form_count": len(FORMS),
            "pixel_tolerance": 0,
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
    print("DXT matrix schema, zero-tolerance pixels and sensitivity controls: PASS")


def check_prepared(args: argparse.Namespace) -> None:
    shared = shared_runner()
    prepared = json.loads(args.prepared.read_text(encoding="ascii"))
    expect_fields(prepared, {"schema_version": 1}, "prepared matrix inputs")
    expect_fields(
        prepared.get("fixture", {}),
        {
            "form": "bc1-8-single",
            "uv_bias_base_texels": 0.25,
            "width": 8,
            "height": 8,
            "mip_levels": 1,
            "pitch": 4,
            "format_value": 18,
        },
        "shader-generation fixture",
    )
    shared.validate_uv_bias(prepared, 8, 8)
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
        "texture-base.dxt.bin",
        "texture-mips.dxt.bin",
        "fixture.json",
    ):
        shared.require_file(Path(prepared["raw_inputs"]) / name, name)
    print("DXT matrix prepared input schema: PASS")


def prepare(args: argparse.Namespace) -> None:
    shared_runner().prepare(args, FAMILY)


def run_test(args: argparse.Namespace) -> None:
    check_prepared(args)
    shared_runner().run_test(args, FAMILY)
    check_results(args)


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
    subparsers.choices["prepare"].set_defaults(function=prepare)
    subparsers.choices["run"].set_defaults(function=run_test)
    subparsers.choices["check"].set_defaults(function=check_prepared)
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
        raise SystemExit(f"native prepared DXT matrix: {error}") from error
