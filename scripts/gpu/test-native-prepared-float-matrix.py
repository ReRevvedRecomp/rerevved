"""Prepare, run and validate the finite tiled R32_FLOAT form.

The exact signed fetch reads R and three constant-one selections at LOD0.
All forms use quarter-texel UV bias and a weighted component readout.
"""

import argparse
import importlib.util
import json
import subprocess
import sys
import tomllib
from pathlib import Path

# name, format, width, height, raw pitch, effective max, scene IDs
FORMS = (("r32-float-512-tiled", 36, 512, 512, 16, 0, "52"),)
RANGES = ((1048576, 0, 0, 0, 0),)
FETCH_WORDS = ((0x84000156, 0x185920A4, 0x003FE1FF, 0x000016D1, 0, 0x200),)
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


FAMILY = "native-prepared-float-matrix"


def shared_runner():
    """Load preparation helpers without creating bytecode beside tracked files."""
    sys.dont_write_bytecode = True
    source = Path(__file__).with_name("test-native-prepared.py")
    spec = importlib.util.spec_from_file_location("prepared_float_recipe", source)
    if spec is None or spec.loader is None:
        raise ValueError("shared recipe runner is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONTROL_PIXEL = module.CONTROL_PIXEL.replace(
        "return first + second;",
        "return dot(first + second, "
        "float4(0.0625, 0.0078125, 0.0546875, 0.0625)).xxxx;",
    )
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


def check_original(root: Path, index: int, form: tuple) -> tuple[bytes, bytes]:
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
    words = receipt.get("source_texture_fetch_words", [])
    expected_words = FETCH_WORDS[FORMS.index(form)]
    require(len(words) == 6, f"Original {index} lost exact fetch words")
    for word, (actual, expected) in enumerate(zip(words, expected_words, strict=True)):
        mask = 0xFFF if word in (1, 5) else 0xFFFFFFFF
        require(
            type(actual) is int and actual & mask == expected & mask,
            f"Original {index} changed descriptor word {word}",
        )
    require(words[1] >> 12 != 0, f"Original {index} lost base page")
    require(
        (words[5] >> 12 != 0) == (form[5] != 0),
        f"Original {index} changed zero/separate mip page identity",
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
    name, fmt, width, height, pitch, max_level, ids = form
    levels = max_level + 1
    result = load_toml(root / "result.toml")
    base_extent, mip_extent, tail_offset, declared_max, lod0_offset = RANGES[
        FORMS.index(form)
    ]
    expected = {
        "schema_version": 1,
        "form": name,
        "scene_ids": ids,
        "width": width,
        "height": height,
        "fetch_pitch": pitch,
        "prepared_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": 2,
        "prepared_swizzle": 0xB68,
        "prepared_swizzled_signs": 0x55,
        "prepared_mip_min": 0,
        "prepared_mip_max": max_level,
        "prepared_packed_mips": False,
        "prepared_tiled": True,
        "prepared_base_extent": base_extent,
        "prepared_mip_extent": mip_extent,
        "prepared_packed_tail_offset": tail_offset,
        "declared_mip_min": 0,
        "declared_mip_max": declared_max,
        "effective_mip_max": max_level,
        "zero_mip_page_clamped": declared_max > max_level,
        "lod0_byte_offset": lod0_offset,
        "clamp_source": (
            "SDK ab2531a texture/util.cpp:77; texture/cache.cpp:1429; "
            "title native_texture_upload.cpp:226-269"
        ),
        "pixel_tolerance": 0,
        "completed_pairs": levels,
        "cpu_update_levels": levels,
        "cpu_update_variant_checks": 0,
        "native_mutation_count": levels,
        "sensitivity_checks": 5,
        "omission_count": levels,
        "viewport_width": min(512, width),
        "viewport_height": min(512, height),
        "device_failure_publications": 0,
        "output_planes": 2,
    }
    expected.update(
        finite_binary32_bits=True,
        constant_one_readouts=3,
        localized_update=True,
        uv_bias_base_texels=0.25,
        pixel_center_conventions_match=True,
        base_row_bytes=2048,
        base_stride_bytes=1048576,
    )
    expected.update(dict.fromkeys(TRUE_RESULT_FIELDS, True))
    expect_fields(result, expected, name)
    for level in range(levels):
        baseline, changed = check_original(root, level, form)
        for route in ("candidate", "control"):
            require(
                plane(root / f"{route}-baseline-{level}.bin", 2) == baseline,
                f"{name} {route} baseline LOD {level} differs from the CPU oracle",
            )
            require(
                plane(root / f"{route}-{level}.bin", 2) == changed,
                f"{name} {route} changed LOD {level} differs from the CPU oracle",
            )
        mutation_kinds = ("value",)
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

    final = plane(root / "candidate-0.bin", 2)
    constant_controls = []
    for component in (1, 2, 3):
        candidate = plane(root / f"readout-zero-{component}-candidate.bin", 2)
        control = plane(root / f"readout-zero-{component}-control.bin", 2)
        require(candidate == control, "constant-one control routes differ")
        require(candidate != final, "constant-one control is insensitive")
        require(
            candidate not in constant_controls,
            "constant-one controls are not pairwise distinct",
        )
        constant_controls.append(candidate)

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
        {"schema_version": 1, "form_count": 1, "pixel_tolerance": 0},
        "matrix",
    )
    expect_fields(
        matrix.get("completion", {}), {"completed_forms": 1}, "matrix completion"
    )
    rows = matrix.get("forms", [])
    require(len(rows) == len(FORMS), "matrix must have exactly 1 form results")
    for row, form in zip(rows, FORMS, strict=True):
        name = form[0]
        expect_fields(
            row, {"name": name, "result": f"{name}/result.toml"}, "matrix row"
        )
        check_form(output / name, form)
    print(
        "R32_FLOAT matrix schema, zero-tolerance pixels and sensitivity controls: PASS"
    )


def check_prepared(args: argparse.Namespace) -> None:
    shared = shared_runner()
    prepared = json.loads(args.prepared.read_text(encoding="ascii"))
    expect_fields(prepared, {"schema_version": 1}, "prepared matrix inputs")
    expect_fields(
        prepared.get("fixture", {}),
        {
            "form": "r32-float-512-tiled",
            "width": 512,
            "height": 512,
            "mip_levels": 1,
            "pitch": 16,
            "format_value": 36,
        },
        "shader-generation fixture",
    )
    shared.validate_uv_bias(prepared, 512, 512)
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
        "texture-base.r32-float.bin",
        "texture-mips.r32-float.bin",
        "fixture.json",
    ):
        shared.require_file(Path(prepared["raw_inputs"]) / name, name)
    print("R32_FLOAT matrix prepared input schema: PASS")


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
        raise SystemExit(f"native prepared R32_FLOAT matrix: {error}") from error
