import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from prepared_expectations import BC3_FORMS, BC3_RANGES, RESULT_FIELDS

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "bc3_matrix_runner",
    Path(__file__).resolve().parents[1] / "gpu" / "test-native-prepared-bc3-matrix.py",
)
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def interpolation_receipt(mode):
    return {
        "schema_version": 1,
        "parity_kind": "device_interpolated",
        "oracle_claim": False,
        "alpha_mode": ("a0_gt_a1", "a0_le_a1")[mode],
        "sampled_lod": 0,
        "alpha_codes": list(range(8)),
        "color_codes": list(range(4)),
        "pixel_tolerance": 0,
        "half_pixel_offset": True,
        "uv_bias_base_texels": 0.25,
        "pixel_center_conventions_match": True,
        "same_device": True,
        "original_control_prepared_equal": True,
        **{
            f"{route}_luid_{part}": value
            for route in ("original", "control", "prepared")
            for part, value in (("low", 123), ("high", 0))
        },
    }


def prepared_inputs(root):
    metadata = {
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
    }
    prepared = {"schema_version": 1, "fixture": metadata}
    for name in ("raw_inputs", "candidate_catalog", "control_catalog"):
        path = root / name
        path.mkdir()
        prepared[name] = str(path)
    for name in ("candidate_source", "control_vertex_source", "control_pixel_source"):
        prepared[name] = str(root / f"{name}.hlsl")
    paths = HELPER.pinned_paths(prepared)
    for path in paths.values():
        path.write_bytes(b"synthetic fixture")
    paths["fixture.json"].write_text(json.dumps(metadata), encoding="ascii")
    vertices = bytearray(3 * 56)
    for index, (u, v) in enumerate(((0.0, 1.0), (0.0, -1.0), (2.0, 1.0))):
        struct.pack_into(">ff", vertices, index * 56 + 16, u + 0.25 / 64, v + 0.25 / 64)
    paths["vertices.bin"].write_bytes(vertices)
    paths["control_vertex_source"].write_text(
        HELPER.shared_runner().CONTROL_VERTEX, encoding="ascii"
    )
    prepared["sha256"] = {
        name: HELPER.shared_runner().digest(path) for name, path in paths.items()
    }
    manifest = root / "prepared.json"
    manifest.write_text(json.dumps(prepared), encoding="ascii")
    return manifest, prepared, paths


def form_receipt(form):
    name, fmt, width, height, pitch, max_level, tiled, packed, endian, ids = form
    levels = max_level + 1
    base, mip, tail = HELPER.RANGES[HELPER.FORMS.index(form)]
    receipt = {
        "schema_version": 1,
        "form": name,
        "scene_ids": ids,
        "width": width,
        "height": height,
        "fetch_pitch": pitch,
        "prepared_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": endian,
        "prepared_swizzle": 1672,
        "prepared_swizzled_signs": 0,
        "prepared_mip_min": 0,
        "prepared_mip_max": max_level,
        "prepared_packed_mips": packed,
        "prepared_tiled": tiled,
        "prepared_base_extent": base,
        "prepared_mip_extent": mip,
        "prepared_packed_tail_offset": tail,
        "pixel_tolerance": 0,
        "interpolated_oracle_claim": False,
        "uv_bias_base_texels": 0.25,
        "pixel_center_conventions_match": True,
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
        "viewport_width": width,
        "viewport_height": height,
        "device_failure_publications": 0,
        "output_planes": 2,
        "prepared_negative_probe_count": 6,
        **dict.fromkeys(RESULT_FIELDS + ("endpoint_oracle_claim",), True),
    }
    if tiled:
        starts, pitches, packed_x, packed_y = HELPER.TILED_LAYOUTS[name]
        receipt.update(
            {
                "stored_level_starts": starts,
                "block_pitches": pitches,
                "packed_block_x": packed_x,
                "packed_block_y": packed_y,
                "tiled_storage_guards": True,
            }
        )
    for level in range(levels):
        receipt.update(
            {
                f"omission_{level}_level": level,
                f"omission_{level}_stage": "BuildNativeGameplayDrawRecipe",
                f"omission_{level}_output_exists": False,
                f"omission_{level}_error": "incomplete payload",
            }
        )
    for index in range(6):
        receipt.update(
            {
                f"prepared_negative_{index}_rejected": True,
                f"prepared_negative_{index}_no_admitted_event": True,
                f"prepared_negative_{index}_error": "unsupported input",
            }
        )
    return receipt


def output_plane(path, copies=1):
    if path.name.startswith("mutated-"):
        return b"mutant"
    return b"old" if "baseline" in path.name else b"new"


class Bc3MatrixRunnerTests(unittest.TestCase):
    def test_complete_catalog_matches_native_fixture_expectations(self):
        self.assertEqual(HELPER.FORMS, BC3_FORMS)
        self.assertEqual(HELPER.RANGES, BC3_RANGES)

    def test_each_required_receipt_flag_is_enforced(self):
        form = BC3_FORMS[0]
        for field in RESULT_FIELDS + ("endpoint_oracle_claim",):
            data = form_receipt(form)
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
                args, "native-prepared-bc3-matrix"
            )
            shared.return_value.prepare.side_effect = RuntimeError("stop")
            with self.assertRaises(RuntimeError):
                HELPER.prepare(args)
            shared.return_value.prepare.assert_called_once_with(
                args, "native-prepared-bc3-matrix"
            )

    def test_forms_cover_exact_scene_ids_and_fetch_pitch(self):
        self.assertEqual(len(HELPER.FORMS), 10)
        ids = [
            int(id)
            for form in HELPER.FORMS
            for id in form[-1].split(",")
            if id.isdigit()
        ]
        self.assertEqual(
            sorted(ids),
            [
                19,
                31,
                49,
                55,
                57,
                59,
                61,
                62,
                74,
                75,
                80,
                82,
                85,
                95,
                98,
                100,
                102,
                104,
                106,
                149,
            ],
        )
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(form[1] == 20 for form in HELPER.FORMS))
        self.assertEqual(
            [form[4] for form in HELPER.FORMS], [4, 4, 8, 8, 8, 16, 16, 16, 4, 4]
        )

    def test_prepared_inputs_pass_then_each_stale_pin_rejects(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest, _, paths = prepared_inputs(Path(temporary))
            args = SimpleNamespace(prepared=manifest)
            HELPER.check_prepared(args)
            for name, path in paths.items():
                with self.subTest(name=name):
                    prior = path.read_bytes()
                    path.write_bytes(prior + b" ")
                    with self.assertRaises(ValueError):
                        HELPER.check_prepared(args)
                    path.write_bytes(prior)

    def test_stale_bias_pixel_offset_and_oracle_kind_reject_before_submission(self):
        for field, value in (
            ("uv_bias_base_texels", 0.0),
            ("half_pixel_offset", False),
            ("interpolated_oracle_claim", True),
            ("endpoint_parity_kind", "device_interpolated"),
            ("format_value", 19),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                manifest, prepared, _ = prepared_inputs(Path(temporary))
                prepared["fixture"][field] = value
                manifest.write_text(json.dumps(prepared), encoding="ascii")
                with patch.object(HELPER, "shared_runner") as shared:
                    with self.assertRaises(ValueError):
                        HELPER.run_test(SimpleNamespace(prepared=manifest))
                    shared.return_value.run_test.assert_not_called()

    def test_missing_pins_and_stale_control_reject(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest, prepared, paths = prepared_inputs(Path(temporary))
            del prepared["sha256"]["control_vs"]
            manifest.write_text(json.dumps(prepared), encoding="ascii")
            with self.assertRaises(ValueError):
                HELPER.check_prepared(SimpleNamespace(prepared=manifest))
            prepared["sha256"]["control_vs"] = HELPER.shared_runner().digest(
                paths["control_vs"]
            )
            paths["control_vertex_source"].write_text("no offset", encoding="ascii")
            prepared["sha256"]["control_vertex_source"] = HELPER.shared_runner().digest(
                paths["control_vertex_source"]
            )
            manifest.write_text(json.dumps(prepared), encoding="ascii")
            with self.assertRaisesRegex(ValueError, "half-pixel"):
                HELPER.check_prepared(SimpleNamespace(prepared=manifest))

    def test_interpolated_exact_three_route_parity_has_no_oracle(self):
        with tempfile.TemporaryDirectory() as temporary:
            with (
                patch.object(
                    HELPER,
                    "load_toml",
                    side_effect=lambda path: interpolation_receipt(int(path.stem[-1])),
                ),
                patch.object(HELPER, "plane", return_value=b"pixels"),
            ):
                HELPER.check_interpolated(Path(temporary))

    def test_interpolated_metadata_neighbors_reject(self):
        for field, value in (
            ("oracle_claim", True),
            ("pixel_tolerance", 1),
            ("half_pixel_offset", False),
            ("uv_bias_base_texels", 0.0),
            ("alpha_codes", list(range(7))),
            ("alpha_codes", [False, True, 2, 3, 4, 5, 6, 7]),
            ("color_codes", [0, 1]),
            ("color_codes", [False, True, 2, 3]),
            ("same_device", False),
            ("prepared_luid_low", 124),
            ("pixel_center_conventions_match", False),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                receipt = interpolation_receipt(0)
                receipt[field] = value
                with (
                    patch.object(HELPER, "load_toml", return_value=receipt),
                    patch.object(HELPER, "plane", return_value=b"pixels"),
                ):
                    with self.assertRaises(ValueError):
                        HELPER.check_interpolated(Path(temporary))

    def test_interpolated_changed_pixel_and_oracle_file_reject(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(
                HELPER, "load_toml", return_value=interpolation_receipt(0)
            ):
                with patch.object(
                    HELPER, "plane", side_effect=[b"original", b"different"]
                ):
                    with self.assertRaisesRegex(ValueError, "differs"):
                        HELPER.check_interpolated(root)
                (root / "interpolated-0-expected.bin").write_bytes(b"invalid claim")
                with patch.object(HELPER, "plane", return_value=b"pixels"):
                    with self.assertRaisesRegex(ValueError, "oracle planes"):
                        HELPER.check_interpolated(root)

    def test_endpoint_gate_rejects_one_byte_original_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            receipt = dict.fromkeys(
                (
                    "cpu_partial_overlap_writes",
                    "source_raw_bytes_frozen",
                    "cpu_update_overlap_written",
                    "cpu_update_outside_unchanged",
                ),
                True,
            )
            receipt["schema_version"] = 1
            receipt["first_lod"] = 0
            receipt["second_lod"] = 0
            receipt["uv_bias_base_texels"] = 0.25
            with (
                patch.object(HELPER, "load_toml", return_value=receipt),
                patch.object(HELPER, "plane", side_effect=[b"old", b"new", b"bad"]),
            ):
                with self.assertRaisesRegex(ValueError, "CPU oracle"):
                    HELPER.check_original(Path(temporary), 0)

    def test_result_schema_rejects_loosened_endpoint_gate(self):
        for field, value in (
            ("pixel_tolerance", 1),
            ("endpoint_oracle_claim", False),
            ("interpolated_oracle_claim", True),
            ("half_pixel_offset", False),
        ):
            receipt = form_receipt(HELPER.FORMS[0])
            receipt[field] = value
            with (
                self.subTest(field=field),
                patch.object(HELPER, "load_toml", return_value=receipt),
            ):
                with self.assertRaisesRegex(ValueError, field):
                    HELPER.check_form(Path("unused"), HELPER.FORMS[0])

    def test_all_form_results_pass_complete_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            for form in HELPER.FORMS:
                with (
                    self.subTest(form=form[0]),
                    patch.object(HELPER, "load_toml", return_value=form_receipt(form)),
                    patch.object(HELPER, "check_interpolated"),
                    patch.object(
                        HELPER, "check_original", return_value=(b"old", b"new")
                    ),
                    patch.object(HELPER, "plane", side_effect=output_plane),
                ):
                    HELPER.check_form(Path(temporary), form)

    def test_tiled_layout_neighbors_reject(self):
        added = HELPER.FORMS[8:]
        self.assertEqual(sum(f[5] + 1 for f in added), 9)
        self.assertEqual(sum(2 * (f[5] + 1) for f in added), 18)
        self.assertEqual(
            [f[1:9] for f in added],
            [
                (20, 128, 128, 4, 0, True, False, 0),
                (20, 128, 128, 4, 7, True, True, 0),
            ],
        )
        for form in added:
            for field, value in (
                ("prepared_tiled", False),
                ("prepared_endianness_value", 1),
                ("prepared_packed_mips", not form[7]),
                ("prepared_pitch", 1),
                ("stored_level_starts", [1]),
                ("block_pitches", [8]),
                ("packed_block_x", [1]),
                ("packed_block_y", [1]),
                ("tiled_storage_guards", False),
                ("interpolated_oracle_claim", True),
            ):
                receipt = form_receipt(form)
                receipt[field] = value
                with (
                    self.subTest(form=form[0], field=field),
                    patch.object(HELPER, "load_toml", return_value=receipt),
                ):
                    with self.assertRaisesRegex(ValueError, field):
                        HELPER.check_form(Path("unused"), form)

    def test_missing_mutation_and_omission_controls_reject(self):
        receipt = form_receipt(HELPER.FORMS[0])
        with tempfile.TemporaryDirectory() as temporary:
            with (
                patch.object(HELPER, "load_toml", return_value=receipt),
                patch.object(HELPER, "check_interpolated"),
                patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
            ):
                with patch.object(
                    HELPER,
                    "plane",
                    side_effect=lambda path, copies=1: (
                        b"new"
                        if path.name.startswith("mutated-")
                        else output_plane(path)
                    ),
                ):
                    with self.assertRaisesRegex(ValueError, "insensitive"):
                        HELPER.check_form(Path(temporary), HELPER.FORMS[0])
                receipt["omission_0_output_exists"] = True
                with patch.object(HELPER, "plane", side_effect=output_plane):
                    with self.assertRaisesRegex(ValueError, "omission_0_output_exists"):
                        HELPER.check_form(Path(temporary), HELPER.FORMS[0])

    def test_plane_requires_complete_equal_copies(self):
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.object(HELPER, "PLANE_BYTES", 4),
        ):
            path = Path(temporary) / "pixels.bin"
            path.write_bytes(b"rgbargba")
            self.assertEqual(HELPER.plane(path, 2), b"rgba")
            path.write_bytes(b"rgbargbb")
            with self.assertRaisesRegex(ValueError, "unequal RT0"):
                HELPER.plane(path, 2)
            path.write_bytes(b"rgba")
            with self.assertRaisesRegex(ValueError, "incomplete RGBA8"):
                HELPER.plane(path, 2)


if __name__ == "__main__":
    unittest.main()
