import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from prepared_expectations import RESULT_FIELDS, RGBA8_FORMS, RGBA8_RANGES

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "rgba8_matrix_runner",
    Path(__file__).resolve().parents[1]
    / "gpu"
    / "test-native-prepared-rgba8-matrix.py",
)
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def receipt(helper, form):
    name, fmt, width, height, pitch, max_level = form[:6]
    floating = fmt == 36
    regions = name == "r8-1024-regions"
    tiled, packed = (True, False) if floating else form[6:8]
    base, mip, tail, declared, offset = helper.RANGES[helper.FORMS.index(form)]
    levels = max_level + 1
    result = {
        "schema_version": 1,
        "form": name,
        "scene_ids": form[-1],
        "width": width,
        "height": height,
        "fetch_pitch": pitch,
        "prepared_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": 0 if fmt == 2 else 2,
        "prepared_swizzle": 0xB68 if floating else 0x124 if fmt == 2 else 0x60A,
        "prepared_swizzled_signs": 0x55 if floating else 0,
        "prepared_mip_min": 0,
        "prepared_mip_max": max_level,
        "prepared_packed_mips": packed,
        "prepared_tiled": tiled,
        "prepared_base_extent": base,
        "prepared_mip_extent": mip,
        "prepared_packed_tail_offset": tail,
        "declared_mip_min": 0,
        "declared_mip_max": declared,
        "effective_mip_max": max_level,
        "zero_mip_page_clamped": declared > max_level,
        "lod0_byte_offset": offset,
        "clamp_source": (
            "SDK ab2531a texture/util.cpp:77; texture/cache.cpp:1429; "
            "title native_texture_upload.cpp:226-269"
        ),
        "pixel_tolerance": 0,
        "completed_pairs": 4 if regions else levels,
        "cpu_update_levels": levels,
        "cpu_update_variant_checks": 0,
        "native_mutation_count": 7 if regions else levels,
        "sensitivity_checks": 8 if regions else 5 if floating else 2 * levels,
        "omission_count": levels,
        "viewport_width": min(512, width),
        "viewport_height": min(512, height),
        "device_failure_publications": 0,
        "output_planes": 2,
        "prepared_negative_probe_count": 6,
        **dict.fromkeys(RESULT_FIELDS, True),
    }
    if regions:
        result.update(
            spatial_windows=4,
            region_update_count=3,
            full_backing_snapshots=4,
            ordered_region_model=True,
            intermediate_prepared_owners_unchanged=True,
            uv_bias_base_texels=0.25,
            pixel_center_conventions_match=True,
        )
    if floating:
        result.update(
            finite_binary32_bits=True,
            constant_one_readouts=3,
            localized_update=True,
            uv_bias_base_texels=0.25,
            pixel_center_conventions_match=True,
            base_row_bytes=2048,
            base_stride_bytes=1048576,
        )
    for level in range(levels):
        result.update(
            {
                f"omission_{level}_level": level,
                f"omission_{level}_stage": "BuildNativeGameplayDrawRecipe",
                f"omission_{level}_output_exists": False,
                f"omission_{level}_error": "incomplete payload",
            }
        )
    for index in range(6):
        result.update(
            {
                f"prepared_negative_{index}_rejected": True,
                f"prepared_negative_{index}_no_admitted_event": True,
                f"prepared_negative_{index}_error": "rejected before admission",
            }
        )
    return result


def output_plane(path, copies=1):
    if path.name.startswith(("mutated-", "readout-zero-")):
        return path.stem.replace("-candidate", "").replace("-control", "").encode()
    return b"old" if "baseline" in path.name else b"new"


def check_shader_anchors(test, helper):
    for count in (0, 2):
        module = SimpleNamespace(
            CONTROL_PIXEL="return first + second;",
            CONTROL_VERTEX="    output.lod = input.lod;" * count,
        )
        spec = SimpleNamespace(loader=SimpleNamespace(exec_module=lambda module: None))
        with (
            test.subTest(count=count),
            patch.object(
                helper.importlib.util, "spec_from_file_location", return_value=spec
            ),
            patch.object(
                helper.importlib.util, "module_from_spec", return_value=module
            ),
        ):
            with test.assertRaisesRegex(ValueError, "LOD anchor"):
                helper.shared_runner()


def check_prepared_uv(test, helper):
    form = helper.FORMS[0]
    name, fmt, width, height, pitch, level = form[:6]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fixture = dict(
            form=name,
            width=width,
            height=height,
            pitch=pitch,
            mip_levels=level + 1,
            format_value=fmt,
            uv_bias_base_texels=0.25,
        )
        prepared = dict(
            schema_version=1,
            fixture=fixture,
            raw_inputs=str(root),
            candidate_catalog=str(root),
            control_catalog=str(root),
            candidate_source=str(root / "candidate.hlsl"),
            control_vertex_source=str(root / "control.hlsl"),
        )
        (root / "candidate.hlsl").write_text("candidate", encoding="ascii")
        (root / "control.hlsl").write_text(
            helper.shared_runner().CONTROL_VERTEX, encoding="ascii"
        )
        storage = "r32-float" if fmt == 36 else "rgba8-r8"
        for file in (
            "registers.le.bin",
            "vertex-ucode.le.bin",
            "pixel-ucode.le.bin",
            f"texture-base.{storage}.bin",
            f"texture-mips.{storage}.bin",
        ):
            (root / file).write_bytes(b"fixture")
        vertices = bytearray(3 * 56)
        u, v = 0.25 / width, 0.25 / height
        for index, uv in enumerate(((u, 1 + v), (u, -1 + v), (2 + u, 1 + v))):
            struct.pack_into(">ff", vertices, index * 56 + 16, *uv)
        (root / "vertices.bin").write_bytes(vertices)
        (root / "fixture.json").write_text(json.dumps(fixture), encoding="ascii")
        path = root / "prepared.json"
        path.write_text(json.dumps(prepared), encoding="ascii")
        args = SimpleNamespace(prepared=path)
        helper.check_prepared(args)
        fixture["uv_bias_base_texels"] = 0
        (root / "fixture.json").write_text(json.dumps(fixture), encoding="ascii")
        with test.assertRaisesRegex(ValueError, "quarter-texel"):
            helper.check_prepared(args)
        fixture["uv_bias_base_texels"] = 0.25
        (root / "fixture.json").write_text(json.dumps(fixture), encoding="ascii")
        struct.pack_into(">f", vertices, 16, 0)
        (root / "vertices.bin").write_bytes(vertices)
        with test.assertRaisesRegex(ValueError, "quarter-texel"):
            helper.check_prepared(args)


class Rgba8MatrixRunnerTests(unittest.TestCase):
    def test_complete_catalog_matches_native_fixture_expectations(self):
        self.assertEqual(HELPER.FORMS, RGBA8_FORMS)
        self.assertEqual(HELPER.RANGES, RGBA8_RANGES)

    def test_each_required_receipt_flag_is_enforced(self):
        form = RGBA8_FORMS[0]
        for field in RESULT_FIELDS:
            data = receipt(HELPER, form)
            del data[field]
            with (
                self.subTest(field=field),
                patch.object(HELPER, "load_toml", return_value=data),
            ):
                with self.assertRaisesRegex(ValueError, f"^{form[0]}: {field} is "):
                    HELPER.check_form(Path("unused"), form)

    def test_run_and_prepare_forward_the_matrix_family(self):
        args = object()
        with (
            patch.object(HELPER, "check_prepared"),
            patch.object(HELPER, "check_results"),
            patch.object(HELPER, "shared_runner") as shared,
        ):
            HELPER.run_test(args)
            shared.return_value.run_test.assert_called_once_with(
                args, "native-prepared-rgba8-matrix"
            )
            shared.return_value.prepare.side_effect = RuntimeError("stop")
            with self.assertRaises(RuntimeError):
                HELPER.prepare(args)
            shared.return_value.prepare.assert_called_once_with(
                args, "native-prepared-rgba8-matrix"
            )

    def test_control_vertex_requires_one_anchor(self):
        check_shader_anchors(self, HELPER)

    def test_prepared_checks_metadata_and_vertex_uv_bias(self):
        check_prepared_uv(self, HELPER)

    def test_explicit_rows_preserve_old_counts_and_add_four_windows(self):
        self.assertEqual(len(HELPER.FORMS), 10)
        self.assertEqual(sum(form[5] + 1 for form in HELPER.FORMS[:9]), 18)
        self.assertEqual(
            HELPER.FORMS[-1],
            ("r8-1024-regions", 2, 1024, 1024, 32, 0, False, False, True, "135"),
        )
        self.assertEqual(HELPER.RANGES[-1], (1048576, 0, 0, 0, 0))

    def test_all_rows_accept_complete_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            for form in HELPER.FORMS:
                with (
                    self.subTest(form=form[0]),
                    patch.object(
                        HELPER, "load_toml", return_value=receipt(HELPER, form)
                    ),
                    patch.object(
                        HELPER, "check_original", return_value=(b"old", b"new")
                    ),
                    patch.object(HELPER, "plane", side_effect=output_plane),
                ):
                    HELPER.check_form(Path(temporary), form)

    def test_region_receipt_rejects_missing_coverage_or_lifetime(self):
        form = HELPER.FORMS[-1]
        for key, value in (
            ("completed_pairs", 1),
            ("region_update_count", 2),
            ("full_backing_snapshots", 3),
            ("intermediate_prepared_owners_unchanged", False),
            ("prepared_tiled", True),
            ("prepared_packed_mips", True),
            ("prepared_base_extent", 1048575),
            ("lifetime_sdk_copy_retired", False),
            ("device_failure_publications", 1),
            ("pixel_center_conventions_match", False),
            ("pixel_tolerance", 1),
        ):
            data = receipt(HELPER, form)
            data[key] = value
            with (
                self.subTest(key=key),
                patch.object(HELPER, "load_toml", return_value=data),
            ):
                with self.assertRaisesRegex(ValueError, key):
                    HELPER.check_form(Path("unused"), form)

    def test_each_region_control_must_change_both_routes(self):
        form = HELPER.FORMS[-1]
        cases = [
            (kind, 0)
            for kind in (
                "omit-write-0",
                "omit-write-1",
                "omit-write-2",
                "reverse-overlap",
            )
        ]
        cases.extend(("omit-write-1", window) for window in range(1, 4))
        for kind, window in cases:
            for route in ("candidate", "control"):
                bad_name = f"mutated-{kind}-{route}-{window}.bin"
                with (
                    self.subTest(kind=kind, route=route),
                    tempfile.TemporaryDirectory() as temporary,
                    patch.object(
                        HELPER, "load_toml", return_value=receipt(HELPER, form)
                    ),
                    patch.object(
                        HELPER, "check_original", return_value=(b"old", b"new")
                    ),
                    patch.object(
                        HELPER,
                        "plane",
                        side_effect=lambda path, copies=1: (
                            b"new" if path.name == bad_name else output_plane(path)
                        ),
                    ),
                ):
                    with self.assertRaisesRegex(ValueError, "mutation"):
                        HELPER.check_form(Path(temporary), form)

    def test_region_controls_must_be_pairwise_distinct(self):
        form = HELPER.FORMS[-1]
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.object(HELPER, "load_toml", return_value=receipt(HELPER, form)),
            patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
            patch.object(
                HELPER,
                "plane",
                side_effect=lambda path, copies=1: (
                    b"duplicate"
                    if path.name.startswith("mutated-")
                    else output_plane(path)
                ),
            ),
        ):
            with self.assertRaisesRegex(ValueError, "duplicates"):
                HELPER.check_form(Path(temporary), form)

    def test_region_resolve_destinations_must_be_disjoint(self):
        data = dict(
            window_x=0,
            window_y=0,
            schema_version=1,
            cpu_partial_overlap_writes=True,
            source_raw_bytes_frozen=True,
            prepared_copy_count=4,
        )
        for step in range(4):
            data[f"prepared_copy_{step}_planned_address"] = 0x100000
            data[f"prepared_copy_{step}_planned_length"] = 0x1000
        with patch.object(HELPER, "load_toml", return_value=data):
            with self.assertRaisesRegex(ValueError, "resolve plans overlap"):
                HELPER.check_original(Path("unused"), 0, HELPER.FORMS[-1])
        for step in range(4):
            data[f"prepared_copy_{step}_planned_address"] += step * 0x1000
        data["source_texture_fetch_words"] = list(HELPER.FETCH_WORDS[-1])
        with (
            patch.object(HELPER, "load_toml", return_value=data),
            patch.object(HELPER, "plane", side_effect=output_plane),
        ):
            self.assertEqual(
                HELPER.check_original(Path("unused"), 0, HELPER.FORMS[-1]),
                (b"old", b"new"),
            )
            data["cpu_update_guest_length"] = 1048576
            with self.assertRaisesRegex(ValueError, "contiguous-update fields"):
                HELPER.check_original(Path("unused"), 0, HELPER.FORMS[-1])

    def test_each_window_and_callback_rejection_is_required(self):
        form = HELPER.FORMS[-1]
        for key in (
            "omission_0_output_exists",
            "prepared_negative_5_no_admitted_event",
        ):
            data = receipt(HELPER, form)
            data[key] = not data[key]
            with (
                self.subTest(key=key),
                tempfile.TemporaryDirectory() as temporary,
                patch.object(HELPER, "load_toml", return_value=data),
                patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
                patch.object(HELPER, "plane", side_effect=output_plane),
            ):
                with self.assertRaisesRegex(ValueError, key):
                    HELPER.check_form(Path(temporary), form)
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.object(HELPER, "load_toml", return_value=receipt(HELPER, form)),
            patch.object(
                HELPER, "check_original", return_value=(b"old", b"new")
            ) as original,
            patch.object(HELPER, "plane", side_effect=output_plane),
        ):
            HELPER.check_form(Path(temporary), form)
            self.assertEqual(
                [call.args[1] for call in original.call_args_list], [0, 1, 2, 3]
            )

    def test_control_vertex_applies_shared_half_pixel(self):
        source = HELPER.shared_runner().CONTROL_VERTEX
        self.assertIn("packoffset(c18.y)", source)
        self.assertIn(
            "output.position.xy += halfPixelOffset * output.position.w;", source
        )

    def test_duplicate_window_rejects_before_pixel_comparison(self):
        with patch.object(
            HELPER, "load_toml", return_value={"window_x": 0, "window_y": 0}
        ):
            with self.assertRaisesRegex(ValueError, "window_x"):
                HELPER.check_original(Path("unused"), 1, HELPER.FORMS[-1])


if __name__ == "__main__":
    unittest.main()
