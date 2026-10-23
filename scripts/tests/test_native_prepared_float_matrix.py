import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_native_prepared_rgba8_matrix import (
    check_prepared_uv,
    check_shader_anchors,
    output_plane,
    receipt,
)

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "float_matrix_runner",
    Path(__file__).resolve().parents[1]
    / "gpu"
    / "test-native-prepared-float-matrix.py",
)
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


class FloatMatrixRunnerTests(unittest.TestCase):
    def test_run_and_prepare_forward_the_matrix_family(self):
        args = object()
        with (
            patch.object(HELPER, "check_prepared"),
            patch.object(HELPER, "check_results"),
            patch.object(HELPER, "shared_runner") as shared,
        ):
            HELPER.run_test(args)
            shared.return_value.run_test.assert_called_once_with(
                args, "native-prepared-float-matrix"
            )
            shared.return_value.prepare.side_effect = RuntimeError("stop")
            with self.assertRaises(RuntimeError):
                HELPER.prepare(args)
            shared.return_value.prepare.assert_called_once_with(
                args, "native-prepared-float-matrix"
            )

    def test_control_vertex_requires_one_anchor(self):
        check_shader_anchors(self, HELPER)

    def test_prepared_checks_metadata_and_vertex_uv_bias(self):
        check_prepared_uv(self, HELPER)

    def test_exact_signed_tiled_row_and_control_shader(self):
        self.assertEqual(
            HELPER.FORMS, (("r32-float-512-tiled", 36, 512, 512, 16, 0, "52"),)
        )
        self.assertEqual(HELPER.FETCH_WORDS[0][0], 0x84000156)
        shared = HELPER.shared_runner()
        self.assertIn(
            "float4(0.0625, 0.0078125, 0.0546875, 0.0625)", shared.CONTROL_PIXEL
        )
        self.assertIn("packoffset(c18.y)", shared.CONTROL_VERTEX)

    def test_complete_float_result_and_all_controls_pass(self):
        form = HELPER.FORMS[0]
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.object(HELPER, "load_toml", return_value=receipt(HELPER, form)),
            patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
            patch.object(HELPER, "plane", side_effect=output_plane),
        ):
            HELPER.check_form(Path(temporary), form)

    def test_float_neighbors_and_incomplete_lifecycle_reject(self):
        form = HELPER.FORMS[0]
        for key, value in (
            ("prepared_format_value", 2),
            ("prepared_endianness_value", 0),
            ("prepared_tiled", False),
            ("prepared_swizzled_signs", 0),
            ("prepared_swizzle", 0x688),
            ("finite_binary32_bits", False),
            ("constant_one_readouts", 2),
            ("localized_update", False),
            ("base_row_bytes", 512),
            ("prepared_base_extent", 1048575),
            ("lifetime_native_completion", False),
            ("device_failure_publications", 1),
        ):
            data = receipt(HELPER, form)
            data[key] = value
            with (
                self.subTest(key=key),
                patch.object(HELPER, "load_toml", return_value=data),
            ):
                with self.assertRaisesRegex(ValueError, key):
                    HELPER.check_form(Path("unused"), form)

    def test_value_and_each_constant_control_must_change_output(self):
        form = HELPER.FORMS[0]
        for stem in (
            "mutated-value",
            "readout-zero-1",
            "readout-zero-2",
            "readout-zero-3",
        ):
            with (
                self.subTest(stem=stem),
                tempfile.TemporaryDirectory() as temporary,
                patch.object(HELPER, "load_toml", return_value=receipt(HELPER, form)),
                patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
                patch.object(
                    HELPER,
                    "plane",
                    side_effect=lambda path, copies=1: (
                        b"new" if path.name.startswith(stem) else output_plane(path)
                    ),
                ),
            ):
                with self.assertRaisesRegex(ValueError, "insensitive"):
                    HELPER.check_form(Path(temporary), form)

    def test_constant_controls_must_be_pairwise_distinct(self):
        form = HELPER.FORMS[0]
        with (
            tempfile.TemporaryDirectory() as temporary,
            patch.object(HELPER, "load_toml", return_value=receipt(HELPER, form)),
            patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
            patch.object(
                HELPER,
                "plane",
                side_effect=lambda path, copies=1: (
                    b"shared"
                    if path.name.startswith(("readout-zero-1", "readout-zero-2"))
                    else output_plane(path)
                ),
            ),
        ):
            with self.assertRaisesRegex(ValueError, "pairwise distinct"):
                HELPER.check_form(Path(temporary), form)

    def test_original_byte_mismatch_is_zero_tolerance(self):
        data = {
            "schema_version": 1,
            "cpu_partial_overlap_writes": True,
            "source_raw_bytes_frozen": True,
            "cpu_update_overlap_written": True,
            "cpu_update_outside_unchanged": True,
            "source_texture_fetch_words": list(HELPER.FETCH_WORDS[0]),
        }
        with (
            patch.object(HELPER, "load_toml", return_value=data),
            patch.object(HELPER, "plane", side_effect=[b"old", b"new", b"bad"]),
        ):
            with self.assertRaisesRegex(ValueError, "CPU oracle"):
                HELPER.check_original(Path("unused"), 0, HELPER.FORMS[0])

    def test_payload_omission_and_six_callback_probes_are_required(self):
        form = HELPER.FORMS[0]
        for key, value in (
            ("omission_0_output_exists", True),
            ("prepared_negative_probe_count", 5),
        ):
            data = receipt(HELPER, form)
            data[key] = value
            with (
                self.subTest(key=key),
                tempfile.TemporaryDirectory() as temporary,
                patch.object(HELPER, "load_toml", return_value=data),
                patch.object(HELPER, "check_original", return_value=(b"old", b"new")),
                patch.object(HELPER, "plane", side_effect=output_plane),
            ):
                with self.assertRaises(ValueError):
                    HELPER.check_form(Path(temporary), form)


if __name__ == "__main__":
    unittest.main()
