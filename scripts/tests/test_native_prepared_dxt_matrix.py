import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from prepared_expectations import DXT_FORMS, DXT_RANGES, RESULT_FIELDS

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "dxt_matrix_runner",
    Path(__file__).resolve().parents[1] / "gpu" / "test-native-prepared-dxt-matrix.py",
)
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def form_receipt(form):
    name, fmt, width, height, pitch, max_level, tiled, packed, endian, ids = form
    levels = max_level + 1
    bc2 = fmt == 19
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
        "prepared_swizzle": 0x688,
        "prepared_swizzled_signs": 0,
        "prepared_mip_min": 0,
        "prepared_mip_max": max_level,
        "prepared_packed_mips": packed,
        "prepared_tiled": tiled,
        "prepared_base_extent": base,
        "prepared_mip_extent": mip,
        "prepared_packed_tail_offset": tail,
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
        "prepared_negative_probe_count": 6,
        **dict.fromkeys(RESULT_FIELDS, True),
    }
    if tiled:
        starts, pitches, packed_x, packed_y = HELPER.TILED_LAYOUTS[name]
        receipt.update(
            stored_level_starts=starts,
            block_pitches=pitches,
            packed_block_x=packed_x,
            packed_block_y=packed_y,
            tiled_storage_guards=True,
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


class DxtMatrixRunnerTests(unittest.TestCase):
    def test_complete_catalog_matches_native_fixture_expectations(self):
        self.assertEqual(HELPER.FORMS, DXT_FORMS)
        self.assertEqual(HELPER.RANGES, DXT_RANGES)

    def test_each_required_receipt_flag_is_enforced(self):
        form = DXT_FORMS[0]
        for field in RESULT_FIELDS:
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
                args, "native-prepared-dxt-matrix"
            )
            shared.return_value.prepare.side_effect = RuntimeError("stop")
            with self.assertRaises(RuntimeError):
                HELPER.prepare(args)
            shared.return_value.prepare.assert_called_once_with(
                args, "native-prepared-dxt-matrix"
            )

    def test_inventory_and_added_control_counts(self):
        self.assertEqual(len(HELPER.FORMS), 18)
        added = HELPER.FORMS[14:]
        self.assertEqual(
            [f[1:9] for f in added],
            [
                (18, 128, 64, 4, 7, True, True, 0),
                (18, 256, 128, 8, 8, True, True, 0),
                (19, 128, 64, 4, 7, True, True, 0),
                (19, 32, 64, 4, 0, True, False, 0),
            ],
        )
        self.assertEqual(sum(f[5] + 1 for f in added), 26)
        self.assertEqual(sum((2 if f[1] == 19 else 1) * (f[5] + 1) for f in added), 35)
        self.assertEqual(sum(2 * (f[5] + 1) for f in added if f[1] == 19), 18)
        self.assertEqual(
            HELPER.RANGES[14:],
            (
                (8192, 16384, 8192),
                (16384, 24576, 16384),
                (16384, 32768, 16384),
                (16384, 0, 0),
            ),
        )

    def test_all_form_results_pass_complete_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            for form in HELPER.FORMS:
                with (
                    self.subTest(form=form[0]),
                    patch.object(HELPER, "load_toml", return_value=form_receipt(form)),
                    patch.object(
                        HELPER, "check_original", return_value=(b"old", b"new")
                    ),
                    patch.object(HELPER, "plane", side_effect=output_plane),
                ):
                    HELPER.check_form(Path(temporary), form)

    def test_tiled_neighbors_and_missing_controls_reject(self):
        for form in HELPER.FORMS[14:]:
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
                ("native_mutation_count", 0),
                ("omission_count", 0),
                ("pixel_tolerance", 1),
            ):
                receipt = form_receipt(form)
                receipt[field] = value
                with (
                    self.subTest(form=form[0], field=field),
                    patch.object(HELPER, "load_toml", return_value=receipt),
                ):
                    with self.assertRaisesRegex(ValueError, field):
                        HELPER.check_form(Path("unused"), form)


if __name__ == "__main__":
    unittest.main()
