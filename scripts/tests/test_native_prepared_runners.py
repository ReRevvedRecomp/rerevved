import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / "gpu"

# Fixed fixture expectations, independent of the runner's family table.
FIXTURES = {
    "native-pixel-lod": {
        "mip_levels": 8,
        "width": 128,
        "height": 128,
    },
    "native-prepared-bc1": {
        "mip_levels": 8,
        "width": 128,
        "height": 128,
        "output_width": 1280,
        "output_height": 720,
        "format": "k_DXT1",
        "endianness": "k8in16",
        "swizzle": 1672,
        "pitch": 4,
        "packed_mips": True,
        "packed_tail_offset": 16384,
    },
    "native-prepared-bc1-256": {
        "mip_levels": 9,
        "width": 256,
        "height": 256,
        "output_width": 1280,
        "output_height": 720,
        "viewport_width": 256.0,
        "viewport_height": 256.0,
        "viewport_registers": (128.0, 128.0, -128.0, 128.0),
        "triangle_uvs": (
            (0.0009765625, 1.0009765625),
            (0.0009765625, -0.9990234375),
            (2.0009765625, 1.0009765625),
        ),
        "format": "k_DXT1",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "02000002",
            "19FE9052",
            "001FE0FF",
            "00A80D10",
            "00000203",
            "19FE1A18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 8.0,
        "pitch": 8,
        "packed_mips": True,
        "stacked": False,
        "packed_level": 4,
        "packed_tail_offset": 24576,
        "packed_tail_extent": 832,
        "base_extent": 32768,
        "mip_extent": 25408,
        "legacy_mip_extent": 24576,
        "guest_offsets": (0, 0, 8192, 16384, 24608, 24592, 24584, 25088, 24832),
        "row_pitches": (512, 256, 256, 256, 256, 256, 256, 256, 256),
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
    },
    "native-prepared-bc1-512": {
        "mip_levels": 10,
        "width": 512,
        "height": 512,
        "output_width": 1280,
        "output_height": 720,
        "viewport_width": 512.0,
        "viewport_height": 512.0,
        "viewport_registers": (256.0, 256.0, -256.0, 256.0),
        "triangle_uvs": (
            (0.00048828125, 1.00048828125),
            (0.00048828125, -0.99951171875),
            (2.00048828125, 1.00048828125),
        ),
        "format": "k_DXT1",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "04000002",
            "19F61052",
            "003FE1FF",
            "00A80D10",
            "00000243",
            "19F51A18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 9.0,
        "pitch": 16,
        "packed_mips": True,
        "stacked": False,
        "packed_level": 5,
        "packed_tail_offset": 57344,
        "packed_tail_extent": 832,
        "base_extent": 131072,
        "mip_extent": 58176,
        "legacy_mip_extent": 57344,
        "guest_offsets": (0, 0, 32768, 40960, 49152, 57376, 57360, 57352, 57856, 57600),
        "row_pitches": (1024, 512, 256, 256, 256, 256, 256, 256, 256, 256),
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
        "caller_frame": 800,
        "caller_texture": 5,
        "first_draw": 988677,
        "required_uses": 18,
        "raw_base_address": 435556352,
        "raw_mip_address": 435490816,
    },
    "native-prepared-bc1-64": {
        "mip_levels": 7,
        "width": 64,
        "height": 64,
        "output_width": 1280,
        "output_height": 720,
        "viewport_width": 64.0,
        "viewport_height": 64.0,
        "viewport_registers": (32.0, 32.0, -32.0, 32.0),
        "triangle_uvs": (
            (0.00390625, 1.00390625),
            (0.00390625, -0.99609375),
            (2.00390625, 1.00390625),
        ),
        "format": "k_DXT1",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "01000002",
            "1BC73052",
            "0007E03F",
            "00A80D10",
            "00000183",
            "1BC6FA18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 6.0,
        "pitch": 4,
        "packed_mips": True,
        "stacked": False,
        "packed_level": 2,
        "packed_tail_offset": 8192,
        "packed_tail_extent": 832,
        "base_extent": 4096,
        "mip_extent": 9024,
        "legacy_mip_extent": 8192,
        "guest_offsets": (0, 0, 8224, 8208, 8200, 8704, 8448),
        "row_pitches": (256, 256, 256, 256, 256, 256, 256),
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
        "caller_frame": 800,
        "caller_texture": 21,
        "first_draw": 988701,
        "required_uses": 104,
        "raw_base_address": 466038784,
        "raw_mip_address": 466022400,
        "source_guest_frame": 1541,
        "source_submission": 5538,
        "source_ps_binding": 20,
        "source_ps_fetch": 1,
        "source_vertex_shader_hash": "571D6127CBF96DD1",
        "source_pixel_shader_hash": "2DEEA7E9B28316E1",
    },
    "native-prepared-bc1-rect": {
        "mip_levels": 9,
        "width": 256,
        "height": 128,
        "output_width": 1280,
        "output_height": 720,
        "viewport_width": 256.0,
        "viewport_height": 128.0,
        "viewport_registers": (128.0, 128.0, -64.0, 64.0),
        "triangle_uvs": (
            (0.0009765625, 1.001953125),
            (0.0009765625, -0.998046875),
            (2.0009765625, 1.001953125),
        ),
        "format": "k_DXT1",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "02000002",
            "169F8052",
            "000FE0FF",
            "00A80D10",
            "00000203",
            "169F2A18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 8.0,
        "pitch": 8,
        "packed_mips": True,
        "stacked": False,
        "packed_level": 3,
        "packed_tail_offset": 16384,
        "packed_tail_extent": 1856,
        "base_extent": 16384,
        "mip_extent": 18240,
        "legacy_mip_extent": 16384,
        "guest_offsets": (0, 0, 8192, 17408, 16896, 16640, 16416, 16400, 16392),
        "row_pitches": (512, 256, 256, 256, 256, 256, 256, 256, 256),
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
        "caller_frame": 800,
        "caller_texture": 6,
        "first_draw": 988683,
        "required_uses": 3,
        "raw_base_address": 379551744,
        "raw_mip_address": 379527168,
    },
    "native-prepared-bc2": {
        "mip_levels": 7,
        "width": 64,
        "height": 64,
        "output_width": 1280,
        "output_height": 720,
        "format": "k_DXT2_3",
        "endianness": "k8in16",
        "swizzle": 1672,
        "pitch": 4,
        "packed_mips": True,
        "packed_tail_offset": 16384,
        "base_extent": 8192,
        "mip_extent": 18048,
        "packed_tail_extent": 1664,
        "guest_offsets": (0, 0, 16448, 16416, 16400, 17408, 16896),
        "row_pitch": 512,
    },
    "native-prepared-bc2-128": {
        "mip_levels": 8,
        "width": 128,
        "height": 128,
        "output_width": 1280,
        "output_height": 720,
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
        "format": "k_DXT2_3",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "01000002",
            "15AA7053",
            "000FE07F",
            "00A80D10",
            "000001C3",
            "15A9BA18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 7.0,
        "pitch": 4,
        "packed_mips": True,
        "stacked": False,
        "packed_tail_offset": 32768,
        "base_extent": 16384,
        "mip_extent": 34432,
        "packed_tail_extent": 1664,
        "guest_offsets": (0, 0, 16384, 32832, 32800, 32784, 33792, 33280),
        "row_pitch": 512,
    },
    "native-prepared-bc2-rect": {
        "mip_levels": 9,
        "width": 128,
        "height": 256,
        "output_width": 1280,
        "output_height": 720,
        "presenter_alpha": 255,
        "duplicated_rt0": True,
        "synthetic_canvas": True,
        "format": "k_DXT2_3",
        "endianness": "k8in16",
        "swizzle": 1672,
        "raw_swizzle": 1672,
        "selected_raw_fetch_words": (
            "01000002",
            "168B6053",
            "001FE07F",
            "00A80D10",
            "00000203",
            "168AAA18",
        ),
        "sampler_filter": "point",
        "address_mode": "clamp_to_edge",
        "spatial_sampling": "nearest_clamp",
        "sampler_min_lod": 0.0,
        "sampler_max_lod": 8.0,
        "pitch": 4,
        "packed_mips": True,
        "stacked": False,
        "packed_tail_offset": 32768,
        "base_extent": 32768,
        "mip_extent": 36480,
        "packed_tail_extent": 3712,
        "guest_offsets": (0, 0, 16384, 32832, 32800, 32784, 34816, 33792, 33280),
        "row_pitch": 512,
    },
}

OUTPUT_FILE_COUNTS = {
    "native-prepared-bc1-256": 36,
    "native-prepared-bc1-512": 40,
    "native-prepared-bc1-64": 28,
    "native-prepared-bc1-rect": 36,
}


def load(script: str):
    spec = importlib.util.spec_from_file_location(
        "test_" + script.removesuffix(".py").replace("-", "_") + "_helper",
        SCRIPTS / script,
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_prepared(root: Path, family: str) -> Path:
    metadata = dict(FIXTURES[family], uv_bias_base_texels=0.25)
    raw = root / "inputs"
    raw.mkdir()
    for name in ("candidate", "control"):
        (root / name).mkdir()
    if "format" in metadata:
        suffix = "bc1" if metadata["format"] == "k_DXT1" else "bc2"
        for name in (f"texture-base.{suffix}.bin", f"texture-mips.{suffix}.bin"):
            (raw / name).write_bytes(b"fixture")
    (raw / "fixture.json").write_text(json.dumps(metadata), encoding="ascii")
    width, height = metadata["width"], metadata["height"]
    uvs = (
        ((0.5, 0.5),) * 3
        if family in ("native-pixel-lod", "native-prepared-bc1")
        else ((0.0, 1.0), (0.0, -1.0), (2.0, 1.0))
    )
    vertices = bytearray(3 * 56)
    for index, (u, v) in enumerate(uvs):
        struct.pack_into(
            ">ff", vertices, index * 56 + 16, u + 0.25 / width, v + 0.25 / height
        )
    (raw / "vertices.bin").write_bytes(vertices)
    source = root / "candidate.hlsl"
    source.write_text("source\n", encoding="ascii")
    prepared = root / "prepared.json"
    prepared.write_text(
        json.dumps(
            {
                "raw_inputs": str(raw),
                "candidate_catalog": str(root / "candidate"),
                "control_catalog": str(root / "control"),
                "candidate_source": str(source),
                "fixture": metadata,
            }
        ),
        encoding="ascii",
    )
    return prepared


def emit_arguments(module, family: str, root: Path) -> list:
    """Run emit_fixture against a recording stand-in and return its command."""
    test_exe = root / "rerevved_prepared_tests.exe"
    test_exe.write_bytes(b"placeholder")
    output = root / "prepared"
    output.mkdir()
    seen = []

    def fake_run(arguments, cwd=None):
        raw = Path(arguments[4])
        seen.append(list(arguments))
        assert not raw.exists()
        raw.mkdir()
        (raw / "fixture.json").write_text("{}", encoding="ascii")

    with patch.object(module, "run_command", side_effect=fake_run):
        result = module.emit_fixture(test_exe, output, family)
    assert result == output / "inputs"
    return seen[0]


class NativePreparedRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = load("test-native-prepared.py")
        cls.runners = [(cls.helper, family) for family in FIXTURES]

    def test_control_sampler_uses_replay_sampler_register_space(self):
        for helper, family in self.runners:
            with self.subTest(family=family):
                self.assertIn(
                    "SamplerState sourceSampler : register(s0, space3);",
                    helper.CONTROL_PIXEL,
                )
                self.assertNotIn(
                    "SamplerState sourceSampler : register(s0);", helper.CONTROL_PIXEL
                )

    def test_recipe_pins_every_schema_input(self):
        for helper, family in self.runners:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                output = root / "prepared"
                output.mkdir()
                for name, contents in (
                    ("registers.le.bin", b"registers"),
                    ("vertex-ucode.le.bin", b"vertex"),
                    ("pixel-ucode.le.bin", b"pixel"),
                    ("shader_common.h", b"header"),
                ):
                    (output / name).write_bytes(contents)
                recipe = helper.write_recipe(
                    output,
                    {
                        "registers": "registers.le.bin",
                        "shared_header": str((output / "shader_common.h").resolve()),
                        "vertex_microcode": "vertex-ucode.le.bin",
                        "pixel_microcode": "pixel-ucode.le.bin",
                    },
                    {},
                )
                value = json.loads(recipe.read_text(encoding="ascii"))
                self.assertEqual(value["registers"]["file"], "registers.le.bin")
                self.assertEqual(
                    value["shared_header"]["file"],
                    str((output / "shader_common.h").resolve()),
                )
                for name in ("registers", "shared_header"):
                    self.assertEqual(set(value[name]), {"file", "sha256"})
                for shader in value["shaders"]:
                    self.assertIsInstance(shader["microcode"], dict)
                    self.assertEqual(set(shader["microcode"]), {"file", "sha256"})

                catalog = output / "shaders.toml"
                helper.write_catalog(
                    catalog,
                    {
                        "vertex_word_sha256": "a" * 64,
                        "pixel_word_sha256": "b" * 64,
                        "vertex_dxil_sha256": "c" * 64,
                        "pixel_dxil_sha256": "d" * 64,
                    },
                    "vertex.dxil",
                    "pixel.dxil",
                )
                catalog_text = catalog.read_text(encoding="ascii")
                self.assertIn("vertex_word_sha256 =", catalog_text)
                self.assertIn("pixel_word_sha256 =", catalog_text)
                self.assertIn("generated_pixel_parameters = false", catalog_text)
                self.assertNotIn("vertex_guest_word_digest", catalog_text)

    def test_emit_fixture_leaves_creation_to_the_family_emit_helper(self):
        for helper, family in self.runners:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                # The helpers resolve paths; resolve the root too, because a
                # runner's TEMP can be an 8.3 short path such as RUNNER~1.
                command = emit_arguments(helper, family, Path(temporary).resolve())
                self.assertEqual(
                    command[1:4], ["--helper", f"{family}.emit", "--emit-fixture"]
                )
                self.assertEqual(Path(command[0]).name, "rerevved_prepared_tests.exe")
                self.assertEqual(Path(command[4]).name, "inputs")
                self.assertEqual(len(command), 5)

    def test_run_selects_the_family_run_helper(self):
        for helper, family in self.runners:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary).resolve()
                test_exe = root / "rerevved_prepared_tests.exe"
                test_exe.write_bytes(b"placeholder")
                prepared = root / "prepared.json"
                prepared.write_text(
                    json.dumps(
                        {
                            "candidate_catalog": "candidate",
                            "control_catalog": "control",
                            "candidate_source": "source",
                        }
                    ),
                    encoding="ascii",
                )
                output = root / "run"
                seen = []
                arguments = type(
                    "Arguments",
                    (),
                    {"test_exe": test_exe, "prepared": prepared, "output": output},
                )()
                with patch.object(
                    helper,
                    "run_command",
                    side_effect=lambda command, cwd=None: seen.append(command),
                ):
                    helper.run_test(arguments, family)
                self.assertEqual(
                    seen[0],
                    [
                        str(test_exe),
                        "--helper",
                        f"{family}.run",
                        "--candidate-catalog",
                        "candidate",
                        "--control-catalog",
                        "control",
                        "--candidate-source",
                        "source",
                        "--output",
                        str(output.resolve()),
                    ],
                )

    def test_family_metadata_is_exact(self):
        self.assertEqual(set(self.helper.FAMILIES), set(FIXTURES))
        for family, metadata in FIXTURES.items():
            with self.subTest(family=family):
                self.assertEqual(self.helper.FAMILIES[family]["metadata"], metadata)
                self.assertEqual(
                    self.helper.FAMILIES[family]["fixed_uv"],
                    family in ("native-pixel-lod", "native-prepared-bc1"),
                )
                if family in OUTPUT_FILE_COUNTS:
                    self.assertEqual(
                        self.helper.FAMILIES[family]["output_file_count"],
                        OUTPUT_FILE_COUNTS[family],
                    )

    def test_check_accepts_each_exact_fixture(self):
        for helper, family in self.runners:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                prepared = write_prepared(Path(temporary), family)
                with patch("builtins.print"):
                    helper.check(SimpleNamespace(prepared=prepared), family)

    def test_check_rejects_wrong_tail_extent(self):
        for helper, family in self.runners:
            if "packed_tail_extent" not in FIXTURES[family]:
                continue
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                prepared = write_prepared(Path(temporary), family)
                data = json.loads(prepared.read_text(encoding="ascii"))
                data["fixture"]["packed_tail_extent"] = 0x341
                prepared.write_text(json.dumps(data), encoding="ascii")
                with self.assertRaises(ValueError):
                    helper.check(SimpleNamespace(prepared=prepared), family)

    def test_check_rejects_changed_schema_fields(self):
        for helper, family in self.runners:
            with tempfile.TemporaryDirectory() as temporary:
                prepared = write_prepared(Path(temporary), family)
                data = json.loads(prepared.read_text(encoding="ascii"))
                for field, value in FIXTURES[family].items():
                    with self.subTest(family=family, field=field):
                        if isinstance(value, bool):
                            changed = int(value)
                        elif isinstance(value, tuple):
                            changed = []
                        elif isinstance(value, str):
                            changed = value + "-changed"
                        else:
                            changed = value + 1
                        data["fixture"][field] = changed
                        prepared.write_text(json.dumps(data), encoding="ascii")
                        with self.assertRaises(ValueError):
                            helper.check(SimpleNamespace(prepared=prepared), family)
                        data["fixture"][field] = value

    def test_parser_dispatches_each_family_and_command(self):
        for _, family in self.runners:
            for command, options, function in (
                (
                    "prepare",
                    [
                        "--test-exe",
                        "tests.exe",
                        "--title-root",
                        ".",
                        "--rexglue",
                        "rexglue.ps1",
                        "--adapter",
                        "adapter.exe",
                        "--dxc",
                        "dxc.exe",
                        "--dxcompiler",
                        "dxcompiler.dll",
                        "--dxil",
                        "dxil.dll",
                        "--shared-header",
                        "header.h",
                        "--output",
                        "prepared",
                    ],
                    self.helper.prepare,
                ),
                ("check", ["--prepared", "prepared.json"], self.helper.check),
                (
                    "run",
                    [
                        "--test-exe",
                        "tests.exe",
                        "--prepared",
                        "prepared.json",
                        "--output",
                        "run",
                    ],
                    self.helper.run_checked,
                ),
            ):
                with self.subTest(family=family, command=command):
                    arguments = self.helper.parser().parse_args(
                        [family, command, *options]
                    )
                    self.assertEqual(arguments.family, family)
                    self.assertIs(arguments.function, function)

    def test_run_rejects_stale_bias_before_submission(self):
        for helper, family in self.runners:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = Path(temporary)
                prepared = write_prepared(root, family)
                (root / "inputs" / "vertices.bin").write_bytes(bytes(3 * 56))
                with patch.object(helper, "run_test") as run:
                    with self.assertRaisesRegex(ValueError, "quarter-texel bias"):
                        helper.run_checked(SimpleNamespace(prepared=prepared), family)
                    run.assert_not_called()


MATRIX_RUNNERS = (
    ("test-native-prepared-dxt-matrix.py", "native-prepared-dxt-matrix"),
    ("test-native-prepared-bc3-matrix.py", "native-prepared-bc3-matrix"),
    ("test-native-prepared-rgba8-matrix.py", "native-prepared-rgba8-matrix"),
    ("test-native-prepared-float-matrix.py", "native-prepared-float-matrix"),
    ("test-native-prepared-cube-matrix.py", "native-prepared-cube-matrix"),
)


class NativePreparedMatrixFamilyTests(unittest.TestCase):
    def test_each_matrix_runs_its_own_run_helper_through_the_shared_recipe(self):
        for script, family in MATRIX_RUNNERS:
            with (
                self.subTest(family=family),
                tempfile.TemporaryDirectory() as temporary,
            ):
                matrix = load(script)
                shared = matrix.shared_runner()
                root = Path(temporary)
                test_exe = root / "rerevved_prepared_tests.exe"
                test_exe.write_bytes(b"placeholder")
                prepared = root / "prepared.json"
                prepared.write_text(
                    json.dumps(
                        {
                            "candidate_catalog": "candidate",
                            "control_catalog": "control",
                            "candidate_source": "source",
                        }
                    ),
                    encoding="ascii",
                )
                arguments = type(
                    "Arguments",
                    (),
                    {
                        "test_exe": test_exe,
                        "prepared": prepared,
                        "output": root / "run",
                    },
                )()
                seen = []
                with (
                    patch.object(matrix, "shared_runner", return_value=shared),
                    patch.object(matrix, "check_prepared"),
                    patch.object(matrix, "check_results"),
                    patch.object(
                        shared,
                        "run_command",
                        side_effect=lambda command, cwd=None: seen.append(command),
                    ),
                ):
                    matrix.run_test(arguments)
                self.assertEqual(seen[0][1:3], ["--helper", f"{family}.run"])


if __name__ == "__main__":
    unittest.main()
