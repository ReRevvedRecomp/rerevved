"""Prepare, check, run and validate the three exact CPU cube forms."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import tomllib
from pathlib import Path

FORMS = (
    ("rgba8-cube-64-packed", "38", 6, 2, 0xA0A, 0, 98304, 94080),
    ("rgba16f-cube-64-packed", "39,40,41", 32, 1, 0x688, 0x55, 196608, 94208),
    ("bc3-cube-128-packed", "63", 20, 1, 0x688, 0, 98304, 280192),
)
FETCH_WORDS = (
    (0x00824802, 0x1BB5B086, 0x1407E03F, 0x00A81414, 0x183, 0x1BB43E18),
    (0x00824956, 0x1BA0B060, 0x1407E03F, 0x00A80D11, 0x183, 0x1B9F3E18),
    (0x01000002, 0x19F39054, 0x140FE07F, 0x00000D10, 0x1C0, 0x19EF1E00),
)
PLANE_BYTES = 1280 * 720 * 4
TRUE_FIELDS = (
    "half_pixel_offset",
    "prepared_collector",
    "prepared_command_feed",
    "independent_raw_oracle",
    "all_levels_decoded",
    "all_channels_compared",
    "raw_old_new_copies",
    "fresh_texture_version_owner",
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
NEGATIVES = (
    "incomplete_cpu_inputs",
    "memexport_provenance",
    "gpu_dirty_texture",
    "truncated_mip_metadata",
    "guarded_unreadable_mip_tail",
    "owned_byte_budget",
    "wrong_normalized_depth",
    "raw_stack_depth",
    "raw_normalized_dimension",
    "non_square",
    "wrong_face_count",
    "missing_final_face_tail",
    "wrong_signed_binding",
    "exponent_adjustment",
    "bc3_cube",
)
CONTROL_PIXEL = """TextureCube<float4> sourceTexture : register(t0, space2);
SamplerState sourceSampler : register(s0, space3);
cbuffer PixelShaderConstants : register(b1, space4) {
    float4 guestPixelConstants[256];
};
struct PSInput { float4 position : SV_Position; float4 lod : TEXCOORD0; };
float4 main(PSInput input) : SV_Target0 {
    float4 sampleValue = sourceTexture.SampleLevel(sourceSampler, input.lod.xyz, input.lod.w);
    float channel = dot(sampleValue, guestPixelConstants[1]);
    float readout = channel * guestPixelConstants[2].x + guestPixelConstants[2].y;
    return float4(readout, readout, readout, 1.0);
}
"""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def expect_fields(data: dict, expected: dict, label: str) -> None:
    for key, value in expected.items():
        require(
            type(data.get(key)) is type(value) and data[key] == value,
            f"{label}: {key} is {data.get(key)!r}, expected {value!r}",
        )


FAMILY = "native-prepared-cube-matrix"


def shared_runner():
    sys.dont_write_bytecode = True
    source = Path(__file__).with_name("test-native-prepared.py")
    spec = importlib.util.spec_from_file_location("prepared_cube_recipe", source)
    require(
        spec is not None and spec.loader is not None, "shared recipe runner missing"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONTROL_PIXEL = CONTROL_PIXEL
    old_recipe, old_catalog = module.write_recipe, module.write_catalog

    def recipe(output, paths, tools):
        path = old_recipe(output, paths, tools)
        data = json.loads(path.read_text(encoding="ascii"))
        data["texture_dimensions"] = ["cube"]
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="ascii")
        return path

    def catalog(path, interface, vertex, pixel):
        old_catalog(path, interface, vertex, pixel)
        text = path.read_text(encoding="ascii")
        require('texture_dimensions = ["2d"]' in text, "shared catalog shape changed")
        path.write_text(
            text.replace(
                'texture_dimensions = ["2d"]', 'texture_dimensions = ["cube"]'
            ),
            encoding="ascii",
        )

    module.write_recipe, module.write_catalog = recipe, catalog
    return module


def load_toml(path: Path) -> dict:
    return tomllib.loads(path.read_text(encoding="ascii"))


def plane(path: Path, copies: int = 1) -> bytes:
    data = path.read_bytes()
    require(len(data) == PLANE_BYTES * copies, f"incomplete cube plane: {path}")
    if copies == 2:
        require(
            data[:PLANE_BYTES] == data[PLANE_BYTES:], f"unequal cube RT0 planes: {path}"
        )
    return data[:PLANE_BYTES]


def changed_panel(
    actual: bytes, expected: bytes, face: int, channel: int, width: int = 64
) -> bool:
    for y in range(channel * width, (channel + 1) * width):
        start = (y * 1280 + face * width) * 4
        if actual[start : start + width * 4] != expected[start : start + width * 4]:
            return True
    return False


def check_form(root: Path, form: tuple) -> None:
    name, ids, fmt, endian, swizzle, signs, base_extent, mip_extent = form
    width, levels, pitch = (128, 8, 4) if fmt == 20 else (64, 7, 2)
    data_channels = 3 if fmt == 6 else 4
    result = load_toml(root / "result.toml")
    expected = {
        "schema_version": 2,
        "form": name,
        "scene_ids": ids,
        "width": width,
        "height": width,
        "faces": 6,
        "channels": 4,
        "data_channels": data_channels,
        "mip_levels": levels,
        "fetch_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": endian,
        "prepared_swizzle": swizzle,
        "prepared_swizzled_signs": signs,
        "prepared_base_extent": base_extent,
        "prepared_mip_extent": mip_extent,
        "prepared_depth": 6,
        "prepared_mip_min": 0,
        "prepared_mip_max": levels - 1,
        "prepared_packed_mips": True,
        "prepared_tiled": False,
        "pixel_tolerance": 0,
        "completed_lods": levels,
        "coverage_groups": 6 * levels * data_channels,
        "native_mutation_count": 6 * levels * data_channels,
        "alpha_invariance_count": levels if fmt == 6 else 0,
        "face_perturbation_count": 6 * levels,
        "omission_count": levels,
        "face_level_omission_count": 6 * levels,
        "semantic_controls": 3,
        "device_failure_publications": 0,
        "output_planes": 2,
        "interpolated_oracle_claim": False,
        "interpolated_parity_cases": 2 * levels if fmt == 20 else 0,
    }
    if fmt == 20:
        expected.update(
            {
                "parity_kind": "endpoint_oracle",
                "endpoint_alpha_codes": [0, 1],
                "endpoint_color_codes": [0, 1],
                "color_mode": "c0_gt_c1",
            }
        )
    expected.update(dict.fromkeys(TRUE_FIELDS, True))
    expect_fields(result, expected, name)
    for level in range(levels):
        receipt = load_toml(root / f"original-{level}.toml")
        expect_fields(
            receipt,
            {
                "schema_version": 2,
                "half_pixel_offset": True,
                "level": level,
                "faces": 6,
                "channels": 4,
                "data_channels": data_channels,
                "update_kind": 1,
                "prepared_draws": 48,
                "cpu_partial_overlap_writes": True,
                "source_raw_bytes_frozen": True,
                "cpu_update_overlap_written": True,
                "cpu_update_outside_unchanged": True,
                "failure": "",
                "collector_error": "",
            },
            f"{name} Original LOD {level}",
        )
        for phase in ("baseline-", ""):
            oracle = plane(root / f"original-{level}-{phase}expected.rgba8.bin")
            require(
                plane(root / f"original-{level}-{phase}terminal.rgba8.bin") == oracle,
                f"{name} Original/oracle mismatch at LOD {level}",
            )
            for route in ("candidate", "control"):
                require(
                    plane(root / f"{route}-{phase}{level}.bin", 2) == oracle,
                    f"{name} {route}/oracle mismatch at LOD {level}",
                )
        expected_plane = plane(root / f"original-{level}-expected.rgba8.bin")
        require(
            expected_plane
            != plane(root / f"original-{level}-baseline-expected.rgba8.bin"),
            f"{name} CPU update is insensitive at LOD {level}",
        )
        words = receipt.get("source_texture_fetch_words", [])
        frozen = FETCH_WORDS[FORMS.index(form)]
        require(len(words) == 6, "cube receipt lacks six fetch words")
        for index, (actual, wanted) in enumerate(zip(words, frozen, strict=True)):
            mask = 0xFFF if index in (1, 5) else 0xFFFFFFFF
            require(
                type(actual) is int and actual & mask == wanted & mask,
                f"{name} descriptor word {index} changed",
            )
        require(
            words[1] >> 12 and words[5] >> 12 and words[1] >> 12 != words[5] >> 12,
            "cube base/mip pages are missing or aliased",
        )
        for key in (
            "baseline_copy_id",
            "baseline_swap_id",
            "completion_copy_id",
            "completion_swap_id",
        ):
            require(
                type(receipt.get(key)) is int and receipt[key] > 0,
                f"cube completion lacks {key}",
            )
        require(
            receipt["baseline_copy_id"] != receipt["completion_copy_id"]
            and receipt["baseline_swap_id"] != receipt["completion_swap_id"],
            "cube completion identities were reused",
        )
        negatives = (
            NEGATIVES[:-1] + ("other_cube_format",) if fmt == 20 else NEGATIVES
        ) + (("biased_float_signs", "gamma_float_signs") if fmt == 32 else ())
        probes = receipt.get("negatives", [])
        require(
            tuple(probe.get("name") for probe in probes) == negatives,
            "cube callback negative inventory changed",
        )
        for probe in probes:
            expect_fields(
                probe, {"rejected": True, "no_admitted_event": True}, probe["name"]
            )
            require(
                isinstance(probe.get("error"), str) and probe["error"],
                "cube callback negative lost its error",
            )
        for face in range(6):
            for channel in range(data_channels):
                key = f"channel-{level}-{face}-{channel}"
                a = plane(root / f"mutated-{key}-candidate.bin", 2)
                b = plane(root / f"mutated-{key}-control.bin", 2)
                require(
                    a == b and changed_panel(a, expected_plane, face, channel, width),
                    f"cube insensitive mutation: {key}",
                )
                if fmt == 20:
                    require(
                        a == plane(root / f"mutated-{key}-expected.bin"),
                        "BC3 endpoint mutation/oracle mismatch",
                    )
                for other in range(24):
                    if other != channel * 6 + face:
                        require(
                            not changed_panel(
                                a, expected_plane, other % 6, other // 6, width
                            ),
                            f"cube mutation {key} changed another panel",
                        )
            a = plane(root / f"face-{level}-{face}-candidate.bin", 2)
            b = plane(root / f"face-{level}-{face}-control.bin", 2)
            require(a == b, "cube face perturbation routes differ")
            if fmt == 20:
                require(
                    a == plane(root / f"face-{level}-{face}-expected.bin"),
                    "BC3 endpoint face/oracle mismatch",
                )
            for channel in range(data_channels):
                require(
                    changed_panel(a, expected_plane, face, channel, width),
                    "cube face perturbation is insensitive",
                )
        if fmt == 6:
            check_alpha_invariance(root / f"alpha-invariance-{level}", level)
    semantic = (
        ("color-selector-complement", "alpha-selector-complement", "selector-transpose")
        if fmt == 20
        else (
            ("wrong-endian", "no-swizzle", "double-swizzle")
            if fmt == 6
            else ("wrong-endian", "clamp-negative", "clamp-unit")
        )
    )
    expected_plane = plane(root / "original-0-expected.rgba8.bin")
    for kind in semantic:
        channel = 3 if kind == "alpha-selector-complement" else 0
        a = plane(root / f"mutated-{kind}-0-0-{channel}-candidate.bin", 2)
        b = plane(root / f"mutated-{kind}-0-0-{channel}-control.bin", 2)
        if fmt == 20:
            require(
                a == plane(root / f"mutated-{kind}-0-0-{channel}-expected.bin"),
                "BC3 semantic/oracle mismatch",
            )
        require(
            a == b and changed_panel(a, expected_plane, 0, channel, width),
            f"cube semantic control is insensitive: {kind}",
        )
    if fmt == 20:
        for level in range(levels):
            for mode in range(2):
                check_interpolated(root / f"interpolated-{level}-{mode}", level, mode)


def check_interpolated(root: Path, level: int, mode: int) -> None:
    receipt = load_toml(root / "parity.toml")
    expect_fields(
        receipt,
        {
            "schema_version": 1,
            "parity_kind": "device_interpolated",
            "interpolated_oracle_claim": False,
            "alpha_mode": ("a0_gt_a1", "a0_lt_a1")[mode],
            "sampled_lod": level,
            "alpha_codes": list(range(8)),
            "color_codes": list(range(4)),
            "pixel_tolerance": 0,
            "half_pixel_offset": True,
            "uv_bias_base_texels": 0.25,
            "pixel_center_conventions_match": True,
            "same_device": True,
            "original_control_prepared_equal": True,
        },
        "BC3 cube interpolated parity",
    )
    for part in ("low", "high"):
        values = [
            receipt.get(f"{route}_luid_{part}")
            for route in ("original", "control", "prepared")
        ]
        require(
            all(type(v) is int for v in values) and values[0] == values[1] == values[2],
            "BC3 interpolated device identities differ",
        )
    for phase in ("baseline", "updated"):
        prefix = "baseline-" if phase == "baseline" else ""
        original = plane(root / f"original-{level}-{prefix}terminal.rgba8.bin")
        for route in ("candidate", "control"):
            require(
                plane(root / f"{route}-{phase}.bin", 2) == original,
                "BC3 interpolated Original/control/Prepared mismatch",
            )
        require(
            (root / f"original-{level}-{prefix}expected.rgba8.bin").stat().st_size == 0,
            "BC3 interpolated case produced an oracle plane",
        )


def check_alpha_invariance(root: Path, level: int) -> None:
    receipt = load_toml(root / f"original-{level}.toml")
    expect_fields(
        receipt,
        {
            "schema_version": 2,
            "half_pixel_offset": True,
            "level": level,
            "faces": 6,
            "channels": 4,
            "data_channels": 3,
            "update_kind": 2,
            "prepared_draws": 48,
            "cpu_partial_overlap_writes": True,
            "source_raw_bytes_frozen": True,
            "cpu_update_overlap_written": True,
            "cpu_update_outside_unchanged": True,
            "failure": "",
            "collector_error": "",
        },
        f"cube alpha invariance LOD {level}",
    )
    changed_bytes = 0
    offsets = (0, 0, 49216, 49184, 49168, 51200, 50176)
    allowed = {
        offsets[level] + face * (16384 if level == 0 else 8192) + y * 256 + x * 4
        for face in range(6)
        for y in range(64 >> level)
        for x in range(64 >> level)
    }
    for bank in ("base", "mips"):
        before = (root / f"original-{level}-old-{bank}.bin").read_bytes()
        after = (root / f"original-{level}-new-{bank}.bin").read_bytes()
        require(
            len(before) == len(after)
            and len(before) >= (98304 if bank == "base" else 94080),
            "cube alpha control changed allocation size",
        )
        changed = {
            i for i, (a, b) in enumerate(zip(before, after, strict=True)) if a != b
        }
        require(
            changed == (allowed if (bank == "base") == (level == 0) else set()),
            "cube alpha control did not mutate exactly the selected guest alpha bytes",
        )
        changed_bytes += len(changed)
    require(changed_bytes > 0, "cube alpha control left raw bytes unchanged")
    baseline = plane(root / f"original-{level}-baseline-expected.rgba8.bin")
    for phase in ("baseline-", ""):
        require(
            plane(root / f"original-{level}-{phase}expected.rgba8.bin") == baseline,
            "cube guest alpha changed the oracle",
        )
        require(
            plane(root / f"original-{level}-{phase}terminal.rgba8.bin") == baseline,
            "cube guest alpha changed Original output",
        )
    for route in ("candidate", "control"):
        for phase in ("baseline", "updated"):
            require(
                plane(root / f"{route}-{phase}.bin", 2) == baseline,
                f"cube guest alpha changed {route} output",
            )


def check_results(args: argparse.Namespace) -> None:
    root = args.output.resolve(strict=True)
    matrix = load_toml(root / "matrix.toml")
    expect_fields(
        matrix,
        {
            "schema_version": 2,
            "form_count": 3,
            "pixel_tolerance": 0,
            "half_pixel_offset": True,
        },
        "cube matrix",
    )
    expect_fields(
        matrix.get("completion", {}), {"completed_forms": 3}, "cube completion"
    )
    require(
        matrix.get("forms")
        == [{"name": f[0], "result": f"{f[0]}/result.toml"} for f in FORMS],
        "cube matrix forms changed",
    )
    for form in FORMS:
        check_form(root / form[0], form)
    print(
        "Cube matrix: 22 positives, 486 mutations, 7 alpha invariance controls, 132 face perturbations, 16 interpolated cases and exact parity passed"
    )


def check_prepared(args: argparse.Namespace) -> None:
    shared = shared_runner()
    prepared = json.loads(
        shared.require_file(args.prepared, "prepared metadata").read_text(
            encoding="ascii"
        )
    )
    expect_fields(prepared, {"schema_version": 1}, "cube prepared inputs")
    expect_fields(
        prepared.get("fixture", {}),
        {
            "schema_version": 1,
            # The shared rectangular runner recognizes matrix fixtures by "form".
            "form": "cube-matrix",
            "form_count": 3,
            "width": 64,
            "height": 64,
            "faces": 6,
            "mip_levels": 7,
            "pitch": 2,
            "panel_width": 64,
            "panel_height": 64,
            "panel_columns": 6,
            "panel_rows": 4,
            "half_pixel_offset": True,
            "pixel_tolerance": 0,
            "interpolated_oracle_claim": False,
        },
        "cube fixture",
    )
    raw = Path(prepared["raw_inputs"])
    for name in (
        "texture-base.cube.bin",
        "texture-mips.cube.bin",
        "fixture.json",
        "registers.le.bin",
        "vertex-ucode.le.bin",
        "pixel-ucode.le.bin",
    ):
        shared.require_file(raw / name, name)
    for field in ("candidate_catalog", "control_catalog"):
        catalog = load_toml(Path(prepared[field]) / "shaders.toml")
        entries = catalog.get("shaders", [])
        require(
            len(entries) == 1 and entries[0].get("texture_dimensions") == ["cube"],
            "cube catalog dimension changed",
        )
    shared.require_file(Path(prepared["candidate_source"]), "candidate source")
    print("Cube prepared schema passed")


def prepare(args: argparse.Namespace) -> None:
    shared_runner().prepare(args, FAMILY)
    check_prepared(argparse.Namespace(prepared=args.output / "prepared.json"))


def run_test(args: argparse.Namespace) -> None:
    check_prepared(args)
    shared_runner().run_test(args, FAMILY)
    check_results(args)


def parser() -> argparse.ArgumentParser:
    root = shared_runner().parser()
    root.description = __doc__
    sub = next(
        action
        for action in root._actions
        if isinstance(action, argparse._SubParsersAction)
    )
    sub.choices["prepare"].set_defaults(function=prepare)
    sub.choices["check"].set_defaults(function=check_prepared)
    sub.choices["run"].set_defaults(function=run_test)
    results = sub.add_parser("check-results")
    results.add_argument("--output", type=Path, required=True)
    results.set_defaults(function=check_results)
    return root


if __name__ == "__main__":
    args = parser().parse_args()
    try:
        args.function(args)
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        subprocess.CalledProcessError,
    ) as error:
        raise SystemExit(f"native prepared cube matrix: {error}") from error
