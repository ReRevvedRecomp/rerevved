import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from prepared_expectations import CUBE_FIELDS, CUBE_FORMS

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location(
    "cube_matrix_runner",
    Path(__file__).resolve().parents[1] / "gpu" / "test-native-prepared-cube-matrix.py",
)
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def form_receipt(form):
    name, ids, fmt, endian, swizzle, signs, base, mips = form
    width, levels, pitch = (128, 8, 4) if fmt == 20 else (64, 7, 2)
    channels = 3 if fmt == 6 else 4
    result = {
        "schema_version": 2,
        "form": name,
        "scene_ids": ids,
        "width": width,
        "height": width,
        "faces": 6,
        "channels": 4,
        "data_channels": channels,
        "mip_levels": levels,
        "fetch_pitch": pitch,
        "prepared_format_value": fmt,
        "prepared_endianness_value": endian,
        "prepared_swizzle": swizzle,
        "prepared_swizzled_signs": signs,
        "prepared_base_extent": base,
        "prepared_mip_extent": mips,
        "prepared_depth": 6,
        "prepared_mip_min": 0,
        "prepared_mip_max": levels - 1,
        "prepared_packed_mips": True,
        "prepared_tiled": False,
        "pixel_tolerance": 0,
        "completed_lods": levels,
        "coverage_groups": 6 * levels * channels,
        "native_mutation_count": 6 * levels * channels,
        "alpha_invariance_count": levels if fmt == 6 else 0,
        "face_perturbation_count": 6 * levels,
        "omission_count": levels,
        "face_level_omission_count": 6 * levels,
        "semantic_controls": 3,
        "device_failure_publications": 0,
        "output_planes": 2,
        "interpolated_oracle_claim": False,
        "interpolated_parity_cases": 2 * levels if fmt == 20 else 0,
        **dict.fromkeys(CUBE_FIELDS, True),
    }
    if fmt == 20:
        result.update(
            parity_kind="endpoint_oracle",
            endpoint_alpha_codes=[0, 1],
            endpoint_color_codes=[0, 1],
            color_mode="c0_gt_c1",
        )
    return result


def original_receipt(form, level):
    fmt = form[2]
    negatives = (
        HELPER.NEGATIVES[:-1] + ("other_cube_format",)
        if fmt == 20
        else HELPER.NEGATIVES
    )
    if fmt == 32:
        negatives += ("biased_float_signs", "gamma_float_signs")
    return {
        "schema_version": 2,
        "half_pixel_offset": True,
        "level": level,
        "faces": 6,
        "channels": 4,
        "data_channels": 3 if fmt == 6 else 4,
        "update_kind": 1,
        "prepared_draws": 48,
        "cpu_partial_overlap_writes": True,
        "source_raw_bytes_frozen": True,
        "cpu_update_overlap_written": True,
        "cpu_update_outside_unchanged": True,
        "failure": "",
        "collector_error": "",
        "source_texture_fetch_words": list(
            HELPER.FETCH_WORDS[HELPER.FORMS.index(form)]
        ),
        "baseline_copy_id": 1,
        "baseline_swap_id": 2,
        "completion_copy_id": 3,
        "completion_swap_id": 4,
        "negatives": [
            {
                "name": name,
                "rejected": True,
                "no_admitted_event": True,
                "error": "unsupported input",
            }
            for name in negatives
        ],
    }


def synthetic_plane(path, copies=1):
    del copies
    name = path.name
    if name.startswith("mutated-"):
        return name.rsplit("-", 1)[0].encode()
    if name.startswith("face-"):
        return name.rsplit("-", 1)[0].encode()
    return b"old" if "baseline" in name else b"new"


def synthetic_changed(actual, expected, face, channel, width=64):
    del expected, width
    name = actual.decode()
    if name.startswith("face-"):
        return face == int(name.split("-")[-1])
    if name.startswith("mutated-"):
        return (face, channel) == tuple(map(int, name.split("-")[-2:]))
    return False


class CubeMatrixRunnerTests(unittest.TestCase):
    def test_complete_catalog_matches_native_fixture_expectations(self):
        self.assertEqual(HELPER.FORMS, CUBE_FORMS)

    def test_each_required_receipt_flag_is_enforced(self):
        form = CUBE_FORMS[0]
        for field in CUBE_FIELDS:
            data = form_receipt(form)
            del data[field]
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, f"^{form[0]}: {field} is "):
                    self.check_form(form, result=data)

    def test_run_and_prepare_forward_the_matrix_family(self):
        args = object()
        with (
            patch.object(HELPER, "check_prepared"),
            patch.object(HELPER, "check_results"),
            patch.object(HELPER, "shared_runner") as shared,
        ):
            HELPER.run_test(args)
            shared.return_value.run_test.assert_called_once_with(
                args, "native-prepared-cube-matrix"
            )
            shared.return_value.prepare.side_effect = RuntimeError("stop")
            with self.assertRaises(RuntimeError):
                HELPER.prepare(args)
            shared.return_value.prepare.assert_called_once_with(
                args, "native-prepared-cube-matrix"
            )

    def check_form(self, form, result=None, original=None, planes=synthetic_plane):
        result = form_receipt(form) if result is None else result

        def load(path):
            if path.name == "result.toml":
                return result
            level = int(path.stem.split("-")[-1])
            return original if original is not None else original_receipt(form, level)

        with (
            patch.object(HELPER, "load_toml", side_effect=load),
            patch.object(HELPER, "plane", side_effect=planes),
            patch.object(HELPER, "changed_panel", side_effect=synthetic_changed),
            patch.object(HELPER, "check_alpha_invariance") as alpha,
            patch.object(HELPER, "check_interpolated") as interpolation,
        ):
            HELPER.check_form(Path("synthetic"), form)
        return alpha.call_count, interpolation.call_args_list

    def test_forms_and_control_counts(self):
        self.assertEqual(len(HELPER.FORMS), 3)
        expected = ((126, 42, 7, 42), (168, 42, 7, 42), (192, 48, 8, 48))
        for form, counts in zip(HELPER.FORMS, expected, strict=True):
            with self.subTest(form=form[0]):
                receipt = form_receipt(form)
                self.assertEqual(
                    tuple(
                        receipt[key]
                        for key in (
                            "native_mutation_count",
                            "face_perturbation_count",
                            "omission_count",
                            "face_level_omission_count",
                        )
                    ),
                    counts,
                )
                alpha, cases = self.check_form(form)
                self.assertEqual(alpha, 7 if form[2] == 6 else 0)
                self.assertEqual(
                    [(call.args[1], call.args[2]) for call in cases],
                    [(level, mode) for level in range(8) for mode in range(2)]
                    if form[2] == 20
                    else [],
                )
        self.assertEqual(HELPER.FORMS[2][1], "63")
        self.assertEqual(HELPER.FETCH_WORDS[2][0], 0x01000002)

    def test_every_result_field_is_required_and_typed(self):
        for form in HELPER.FORMS:
            result = form_receipt(form)
            for key, value in result.items():
                with self.subTest(form=form[0], field=key):
                    stale = dict(result)
                    stale[key] = str(value) if not isinstance(value, str) else "stale"
                    with self.assertRaises(ValueError):
                        self.check_form(form, result=stale)

    def test_bc3_mutation_requires_independent_endpoint_oracle(self):
        def planes(path, copies=1):
            if path.name == "mutated-channel-0-0-0-expected.bin":
                return b"wrong oracle"
            return synthetic_plane(path, copies)

        with self.assertRaisesRegex(ValueError, "endpoint mutation/oracle"):
            self.check_form(HELPER.FORMS[2], planes=planes)

    def test_bc3_negative_inventory_and_fetch_tuple_are_exact(self):
        receipt = original_receipt(HELPER.FORMS[2], 0)
        receipt["negatives"][-1]["name"] = "bc3_cube"
        with self.assertRaisesRegex(ValueError, "negative inventory"):
            self.check_form(HELPER.FORMS[2], original=receipt)
        receipt = original_receipt(HELPER.FORMS[2], 0)
        receipt["source_texture_fetch_words"][4] ^= 0x40
        with self.assertRaisesRegex(ValueError, "descriptor word 4"):
            self.check_form(HELPER.FORMS[2], original=receipt)

    def test_changed_panel_uses_each_form_geometry(self):
        for width in (64, 128):
            expected = bytes(HELPER.PLANE_BYTES)
            actual = bytearray(expected)
            actual[((3 * width + width - 1) * 1280 + 6 * width - 1) * 4] = 1
            self.assertTrue(HELPER.changed_panel(actual, expected, 5, 3, width))
            self.assertFalse(HELPER.changed_panel(actual, expected, 4, 3, width))

    def test_interpolated_parity_has_no_oracle_and_same_device(self):
        receipt = {
            "schema_version": 1,
            "parity_kind": "device_interpolated",
            "interpolated_oracle_claim": False,
            "alpha_mode": "a0_gt_a1",
            "sampled_lod": 7,
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
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for prefix in ("baseline-", ""):
                (root / f"original-7-{prefix}expected.rgba8.bin").touch()
            with (
                patch.object(HELPER, "load_toml", return_value=receipt),
                patch.object(HELPER, "plane", return_value=b"same"),
            ):
                HELPER.check_interpolated(root, 7, 0)
                receipt["prepared_luid_low"] = 124
                with self.assertRaisesRegex(ValueError, "device identities"):
                    HELPER.check_interpolated(root, 7, 0)
                receipt["prepared_luid_low"] = 123
                receipt["interpolated_oracle_claim"] = True
                with self.assertRaisesRegex(ValueError, "interpolated_oracle_claim"):
                    HELPER.check_interpolated(root, 7, 0)
                receipt["interpolated_oracle_claim"] = False
                (root / "original-7-expected.rgba8.bin").write_bytes(b"oracle")
                with self.assertRaisesRegex(ValueError, "oracle plane"):
                    HELPER.check_interpolated(root, 7, 0)


if __name__ == "__main__":
    unittest.main()
