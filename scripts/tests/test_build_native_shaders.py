import hashlib
import importlib.util
import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "build_native_shaders",
    Path(__file__).resolve().parents[1] / "build-native-shaders.py",
)
SHADERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHADERS)


class NativeShaderInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

        def asset(name, data):
            (self.root / name).write_bytes(data)
            return {"file": name, "sha256": hashlib.sha256(data).hexdigest()}

        registers = [0] * 0x5003
        registers[0x2000] = 0x0A010280
        self.recipe = {
            "schema_version": 1,
            "output": "generated",
            "texture_fetches": [2, 0, 1],
            "registers": asset("registers.bin", struct.pack("<20483I", *registers)),
            "shared_header": asset("common.h", b"header"),
            "tools": {
                name: asset(name + suffix, b"pinned tool")
                for name, suffix in [
                    ("adapter", ".exe"),
                    ("vertex_compiler", ".exe"),
                    ("pixel_compiler", ".exe"),
                    ("dxcompiler", ".dll"),
                    ("dxil", ".dll"),
                ]
            },
            "shaders": [
                {
                    "stage": stage,
                    "hash": "0123456789ABCDEF",
                    "byte_order": "little",
                    "microcode": asset(stage + ".bin", bytes(12)),
                }
                for stage in ("vs", "ps")
            ],
        }
        self.path = self.root / "recipe.json"
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")

    def test_check_preserves_inputs_and_runs_no_tools(self):
        with patch.object(SHADERS.subprocess, "run") as run:
            SHADERS.generate(self.path, check=True)
        run.assert_not_called()
        self.assertFalse((self.root / "generated").exists())

    def test_changed_tool_rejected_before_output_creation(self):
        (self.root / "adapter.exe").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            SHADERS.generate(self.path)
        self.assertFalse((self.root / "generated").exists())

    def test_default_check_does_not_decode_unused_header(self):
        (self.root / "common.h").write_bytes(b"\xff")
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with patch.object(SHADERS.subprocess, "run") as run:
            SHADERS.generate(self.path, check=True)
        run.assert_not_called()
        self.assertFalse((self.root / "generated").exists())

    def test_standalone_dxc_uses_pinned_dll_and_stage_profile(self):
        self.recipe["tools"]["dxc"] = self.recipe["tools"].pop("vertex_compiler")
        self.recipe["tools"].pop("pixel_compiler")
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with patch.object(SHADERS.subprocess, "run") as run:
            SHADERS.generate(self.path, check=True)
            run.assert_not_called()
        tools = {
            name: self.root / item["file"]
            for name, item in self.recipe["tools"].items()
        }
        for stage in ("vs", "ps"):
            args = SHADERS.compiler_arguments(
                tools, stage, "shader source.hlsl", "shader output.dxil"
            )
            self.assertEqual(args[0], str(tools["dxc"]))
            self.assertEqual(args[args.index("-T") + 1], f"{stage}_6_0")
            self.assertEqual(args[args.index("-E") + 1], "main")
            self.assertIn("-DREBLUE_RECOMP", args)
            self.assertEqual(
                args[-3:], ["-Fo", "shader output.dxil", "shader source.hlsl"]
            )

        (self.root / "other").mkdir()
        (self.root / "other/dxc.exe").write_bytes(b"pinned tool")
        self.recipe["tools"]["dxc"]["file"] = "other/dxc.exe"
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "beside the pinned"):
            SHADERS.generate(self.path, check=True)
        self.assertFalse((self.root / "generated").exists())

    def test_compiler_interfaces_cannot_be_mixed(self):
        self.recipe["tools"]["dxc"] = self.recipe["tools"]["vertex_compiler"]
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "one compiler interface"):
            SHADERS.generate(self.path, check=True)

    def spirv_recipe(self):
        self.recipe["output_format"] = "spirv"
        self.recipe["tools"]["dxc"] = self.recipe["tools"].pop("vertex_compiler")
        self.recipe["tools"].pop("pixel_compiler")
        (self.root / "spirv-val.exe").write_bytes(b"validator")
        self.recipe["tools"]["spirv_val"] = {
            "file": "spirv-val.exe",
            "sha256": SHADERS.digest(self.root / "spirv-val.exe"),
        }
        (self.root / "common.h").write_text(
            "#ifdef __spirv__\n#endif\n"
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            encoding="ascii",
        )
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")

    @staticmethod
    def interface_source(stage):
        bindings = "".join(
            f"register({register}, space{space});\n"
            for register, space in [
                ("t0", 0),
                ("t0", 1),
                ("t0", 2),
                ("s0", 3),
                ("b0" if stage == "vs" else "b1", 4),
                ("b2", 4),
            ]
        )
        direction = "out" if stage == "vs" else "in"
        parameters = [f"{direction} float4 tex{i} : TEXCOORD{i}" for i in range(16)]
        if stage == "vs":
            parameters.insert(0, "in float4 input : TEXCOORD2")
        parameters.append("out float4 position : SV_Position")
        return bindings + "void main(\n" + ",\n".join(parameters) + ")\n{\n}\n"

    def test_spirv_preflight_rejects_invalid_format_and_tools(self):
        for output_format in (None, "SPIRV", "dxbc", []):
            self.recipe["output_format"] = output_format
            self.path.write_text(json.dumps(self.recipe), encoding="ascii")
            with patch.object(SHADERS.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "output format"):
                    SHADERS.generate(self.path)
                run.assert_not_called()
            self.assertFalse((self.root / "generated").exists())
        self.recipe["output_format"] = "spirv"
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "standalone"):
            SHADERS.generate(self.path)
        self.spirv_recipe()
        self.recipe["tools"].pop("spirv_val")
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "spirv_val"):
            SHADERS.generate(self.path, check=True)
        self.assertFalse((self.root / "generated").exists())

    def test_spirv_flags_and_locations_preserve_descriptor_contract(self):
        for stage in ("vs", "ps"):
            source = self.interface_source(stage)
            adapted = SHADERS.spirv_source(source, stage)
            for index in range(16):
                self.assertIn(f"[[vk::location({index})]]", adapted)
            self.assertEqual(source.count("register("), adapted.count("register("))
            args = SHADERS.compiler_arguments(
                {"dxc": "dxc.exe"}, stage, "s", "o", "spirv"
            )
            self.assertIn("-fspv-target-env=vulkan1.2", args)
            self.assertIn("-fvk-use-dx-layout", args)
            self.assertNotIn("-Qstrip_reflect", args)
            self.assertNotIn("-Qstrip_debug", args)
            self.assertEqual(
                SHADERS.compiler_arguments({"dxc": "dxc.exe"}, stage, "s", "o"),
                [
                    "dxc.exe",
                    "-T",
                    f"{stage}_6_0",
                    "-E",
                    "main",
                    "-HV",
                    "2021",
                    "-all-resources-bound",
                    "-Wno-ignored-attributes",
                    "-Qstrip_reflect",
                    "-Qstrip_debug",
                    "-DREBLUE_RECOMP",
                    "-Fo",
                    "o",
                    "s",
                ],
            )
            self.assertEqual("-fvk-invert-y" in args, stage == "vs")
            self.assertEqual("-fvk-use-dx-position-w" in args, stage == "ps")
            for before, after, message in [
                ("TEXCOORD15", "COLOR0", "semantic"),
                ("TEXCOORD15", "TEXCOORD16", "semantic"),
                ("float4 tex15", "float2 tex15", "semantic"),
                ("TEXCOORD15", "TEXCOORD14", "duplicate"),
                ("space4", "space5", "bindings"),
            ]:
                with self.assertRaisesRegex(ValueError, message):
                    SHADERS.spirv_source(source.replace(before, after), stage)
        self.assertEqual(
            SHADERS.spirv_shared_header("#ifdef __spirv__\n#else\n#endif"),
            "#if 0 // Raw adapter uses uniform buffers.\n#else\n#endif",
        )

    def test_spirv_validation_blocks_metadata_and_outputs_are_separate(self):
        self.spirv_recipe()
        for fail_validation in (True, False):
            self.recipe["output"] = "failed" if fail_validation else "passed"
            self.path.write_text(json.dumps(self.recipe), encoding="ascii")
            output = self.root / self.recipe["output"]

            def run(args, **kwargs):
                if "--raw" in args:
                    Path(args[5]).write_text(
                        self.interface_source(args[2]), encoding="ascii"
                    )
                elif "-Fo" in args:
                    Path(args[args.index("-Fo") + 1]).write_bytes(b"spirv")
                elif "--target-env" in args:
                    self.assertEqual(
                        args[1:4],
                        ["--target-env", "vulkan1.2", "--scalar-block-layout"],
                    )
                    if fail_validation:
                        return subprocess.CompletedProcess(
                            args, 1, "", "invalid module"
                        )
                return subprocess.CompletedProcess(args, 0, "", "")

            with (
                patch.object(SHADERS.subprocess, "run", side_effect=run),
                patch.object(
                    SHADERS,
                    "specialize_pixel_source",
                    side_effect=lambda text, *args: text,
                ),
            ):
                if fail_validation:
                    with self.assertRaises(subprocess.CalledProcessError):
                        SHADERS.generate(self.path)
                    self.assertFalse((output / "generation.spirv.json").exists())
                else:
                    SHADERS.generate(self.path)
                    metadata = json.loads(
                        (output / "generation.spirv.json").read_text()
                    )
                    self.assertEqual(
                        metadata["required_features"], ["scalarBlockLayout"]
                    )
                    self.assertTrue(
                        all(
                            "spirv_sha256" in record and "dxil_sha256" not in record
                            for record in metadata["shaders"]
                        )
                    )
                self.assertFalse((output / "generation.json").exists())
                self.assertFalse(list(output.glob("*.dxil")))

    def test_world_z_ordering_is_limited_to_selected_spirv_vertex(self):
        selected_hash = "C3BAB94E67553E12"
        block = (
            "\t\tps = cos(r0.y);\n"
            "\t\tr0.z = ps;\n"
            "\t\tr3.x = r1.w * r0.z;\n"
            "\t\tps = sin(r0.y);\n"
            "\t\tr0.z = ps;\n"
            "\t\tr3.w = r0.z * -r1.w;\n"
            "\t\tps = sin(r0.x);\n"
            "\t\tr3.z = ps;\n"
            "\t\tr0.xyz = r3.wyx * r3.yzy;\n"
        )
        self.spirv_recipe()
        self.recipe["shaders"][1]["hash"] = selected_hash
        for output_format, vertex_hash in (
            ("dxil", selected_hash),
            ("spirv", selected_hash),
            ("spirv", "0123456789ABCDEF"),
        ):
            self.recipe["output_format"] = output_format
            self.recipe["output"] = f"{output_format}-{vertex_hash}"
            self.recipe["shaders"][0]["hash"] = vertex_hash
            self.path.write_text(json.dumps(self.recipe), encoding="ascii")
            sources = {}

            def run(args, **kwargs):
                if "--raw" in args:
                    sources[args[2]] = self.interface_source(args[2]).replace(
                        "{\n}\n", "{\n" + block + "}\n"
                    )
                    Path(args[5]).write_text(sources[args[2]], encoding="ascii")
                elif "-Fo" in args:
                    Path(args[args.index("-Fo") + 1]).write_bytes(b"compiled")
                return subprocess.CompletedProcess(args, 0, "", "")

            with (
                patch.object(SHADERS.subprocess, "run", side_effect=run),
                patch.object(
                    SHADERS,
                    "specialize_pixel_source",
                    side_effect=lambda text, *args: text,
                ),
            ):
                SHADERS.generate(self.path)
            output = self.root / self.recipe["output"]
            for stage, shader_hash in (("vs", vertex_hash), ("ps", selected_hash)):
                source = output / f"{stage}_{shader_hash}.executable.hlsl"
                if output_format == "dxil" and stage == "vs":
                    source = output / f"{stage}_{shader_hash}.hlsl"
                text = source.read_text(encoding="ascii")
                changed = (
                    output_format == "spirv"
                    and stage == "vs"
                    and shader_hash == selected_hash
                )
                self.assertEqual("float worldZCosB" in text, changed)
                if changed:
                    self.assertIn("r3.x = r3.y * r1.w;", text)
                    self.assertIn(
                        "r0.xyz = r3.wyx * float3(r3.y, r3.z, worldZCosB);", text
                    )
                    self.assertNotIn("r3.x = r1.w * r0.z;", text)
                else:
                    self.assertIn(block, text)
                if output_format == "dxil":
                    self.assertEqual(text, sources[stage])

        for unsupported in (block.replace("sin(r0.x)", "sin(r0.z)"), block * 2):
            with self.assertRaisesRegex(ValueError, "unexpected translated"):
                SHADERS.specialize_spirv_vertex(unsupported, selected_hash)

    def test_wrapper_compiler_keeps_pinned_dll_argument(self):
        tools = {
            name: self.root / item["file"]
            for name, item in self.recipe["tools"].items()
        }
        self.assertEqual(
            SHADERS.compiler_arguments(tools, "ps", "source.hlsl", "output.dxil"),
            [
                str(tools["pixel_compiler"]),
                str(tools["dxcompiler"]),
                "source.hlsl",
                "output.dxil",
            ],
        )

    def test_default_dxil_records_and_sources_are_preserved(self):
        (self.root / "common.h").write_text(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            encoding="ascii",
        )
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        translated = "unchanged translated HLSL"

        def run(args, **kwargs):
            if "--raw" in args:
                Path(args[5]).write_text(translated, encoding="ascii")
            else:
                Path(args[3]).write_bytes(b"dxil")
            return subprocess.CompletedProcess(args, 0, "", "")

        with (
            patch.object(SHADERS.subprocess, "run", side_effect=run),
            patch.object(
                SHADERS, "specialize_pixel_source", side_effect=lambda text, *args: text
            ),
        ):
            SHADERS.generate(self.path)
        output = self.root / "generated"
        metadata = json.loads((output / "generation.json").read_text())
        self.assertEqual(
            set(metadata),
            {
                "recipe",
                "generated_pixel_parameters",
                "shaders",
            },
        )
        for record in metadata["shaders"]:
            self.assertEqual(
                set(record),
                {
                    "stage",
                    "hash",
                    "microcode_sha256",
                    "dxil_sha256",
                },
            )
        for source in output.glob("*.hlsl"):
            self.assertEqual(source.read_text(), translated)
        self.assertFalse((output / "generation.spirv.json").exists())
        self.assertFalse(list(output.glob("*.spv")))

    def test_screen_space_position_marks_only_the_specialized_vertex_record(self):
        (self.root / "common.h").write_text(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            encoding="ascii",
        )
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        half_pixel = "\toPos.xy += g_HalfPixelOffset * oPos.w;\n"
        # The movie quad's clip and viewport-transform words, then one change each.
        for clip, transform, expected in (
            (0x90000, 0x400, True),
            (0x80000, 0x400, False),
            (0x90000, 0x43F, False),
        ):
            registers = [0] * 0x5003
            registers[0x2000] = 0x0A010280
            registers[0x2204] = clip
            registers[0x2206] = transform
            data = struct.pack("<20483I", *registers)
            (self.root / "registers.bin").write_bytes(data)
            self.recipe["registers"]["sha256"] = hashlib.sha256(data).hexdigest()
            self.recipe["output"] = f"screen-{clip:x}-{transform:x}"
            self.path.write_text(json.dumps(self.recipe), encoding="ascii")

            def run(args, **kwargs):
                if "--raw" in args:
                    Path(args[5]).write_text(half_pixel, encoding="ascii")
                else:
                    Path(args[3]).write_bytes(b"dxil")
                return subprocess.CompletedProcess(args, 0, "", "")

            with (
                self.subTest(clip=clip, transform=transform),
                patch.object(SHADERS.subprocess, "run", side_effect=run),
                patch.object(
                    SHADERS,
                    "specialize_pixel_source",
                    side_effect=lambda text, *args: text,
                ),
            ):
                SHADERS.generate(self.path)
                output = self.root / self.recipe["output"]
                records = {
                    record["stage"]: record
                    for record in json.loads((output / "generation.json").read_text())[
                        "shaders"
                    ]
                }
                self.assertEqual(
                    records["vs"].get("screen_space_position"),
                    True if expected else None,
                )
                self.assertNotIn("screen_space_position", records["ps"])
                vertex = output / "vs_0123456789ABCDEF.executable.hlsl"
                self.assertEqual(vertex.exists(), expected)
                if expected:
                    self.assertIn(
                        "oPos.xy * float2(1.0 / 640.0, -1.0 / 360.0)",
                        vertex.read_text(encoding="ascii"),
                    )

    def test_user_clip_plane_zero_adds_one_trailing_clip_distance(self):
        (self.root / "common.h").write_text(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            encoding="ascii",
        )
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        vertex_source = (
            "cbuffer SharedConstants : register(b2, space4)\n{\n"
            "\tDEFINE_SHARED_CONSTANTS();\n};\n"
            "void main(\n\tout float4 oPos : SV_Position,\n"
            "\tout float4 oTexCoord15 : TEXCOORD15)\n{\n"
            "\toPos.xy += g_HalfPixelOffset * oPos.w;\n}\n"
        )
        # The hall reflection's PA_CL_CLIP_CNTL, the default, plane 0 with
        # clipping off, then plane 1, two planes and plane 0 cull-only.
        for clip, expected in (
            (0x80001, True),
            (0x80000, False),
            (0x90001, False),
            (0x80002, ValueError),
            (0x80003, ValueError),
            (0xA0001, ValueError),
        ):
            registers = [0] * 0x5003
            registers[0x2000] = 0x0A010280
            registers[0x2204] = clip
            registers[0x2206] = 0x43F
            data = struct.pack("<20483I", *registers)
            (self.root / "registers.bin").write_bytes(data)
            self.recipe["registers"]["sha256"] = hashlib.sha256(data).hexdigest()
            self.recipe["output"] = f"clip-{clip:x}"
            self.path.write_text(json.dumps(self.recipe), encoding="ascii")

            def run(args, **kwargs):
                if "--raw" in args:
                    Path(args[5]).write_text(vertex_source, encoding="ascii")
                else:
                    Path(args[3]).write_bytes(b"dxil")
                return subprocess.CompletedProcess(args, 0, "", "")

            with (
                self.subTest(clip=clip),
                patch.object(SHADERS.subprocess, "run", side_effect=run),
                patch.object(
                    SHADERS,
                    "specialize_pixel_source",
                    side_effect=lambda text, *args: text,
                ),
            ):
                if expected is ValueError:
                    with self.assertRaisesRegex(
                        ValueError, "unsupported user clip planes"
                    ):
                        SHADERS.generate(self.path)
                    continue
                SHADERS.generate(self.path)
                output = self.root / self.recipe["output"]
                records = {
                    record["stage"]: record
                    for record in json.loads((output / "generation.json").read_text())[
                        "shaders"
                    ]
                }
                self.assertEqual(
                    records["vs"].get("user_clip_plane"), True if expected else None
                )
                self.assertNotIn("user_clip_plane", records["ps"])
                vertex = output / "vs_0123456789ABCDEF.executable.hlsl"
                self.assertEqual(vertex.exists(), expected)
                if expected:
                    text = vertex.read_text(encoding="ascii")
                    self.assertIn(
                        "\tfloat4 g_UserClipPlane0 : packoffset(c29);\n", text
                    )
                    self.assertIn(
                        "\tout float4 oTexCoord15 : TEXCOORD15,\n"
                        "\tout float oClipDistance0 : SV_ClipDistance0)",
                        text,
                    )
                    self.assertIn(
                        "\toClipDistance0 = dot(oPos, g_UserClipPlane0);\n"
                        "\toPos.xy += g_HalfPixelOffset",
                        text,
                    )

    def test_recipe_color_targets_reach_pixel_specialization(self):
        registers = [0] * 0x5003
        registers[0x2000] = 0x0A010280
        registers[0x2003] = 0x0002021C
        data = struct.pack("<20483I", *registers)
        (self.root / "registers.bin").write_bytes(data)
        self.recipe["registers"]["sha256"] = hashlib.sha256(data).hexdigest()
        (self.root / "common.h").write_text(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            encoding="ascii",
        )
        self.recipe["shared_header"]["sha256"] = SHADERS.digest(self.root / "common.h")
        self.recipe["color_targets"] = 2
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        seen = []

        def run(args, **kwargs):
            if "--raw" in args:
                Path(args[5]).write_text("translated", encoding="ascii")
            else:
                Path(args[3]).write_bytes(b"dxil")
            return subprocess.CompletedProcess(args, 0, "", "")

        def specialize(text, *args):
            seen.append(args[-1])
            return text

        with (
            patch.object(SHADERS.subprocess, "run", side_effect=run),
            patch.object(SHADERS, "specialize_pixel_source", side_effect=specialize),
        ):
            SHADERS.generate(self.path)
        self.assertEqual(seen, [2])
        # The count is validated before any output exists.
        self.recipe["color_targets"] = 3
        self.recipe["output"] = "second"
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "color target count"):
            SHADERS.generate(self.path, check=True)
        self.assertFalse((self.root / "second").exists())

    def test_existing_output_preserved(self):
        output = self.root / "generated"
        output.mkdir()
        sentinel = output / "vs.dxil"
        sentinel.write_bytes(b"accepted")
        with self.assertRaisesRegex(ValueError, "fresh directory"):
            SHADERS.generate(self.path)
        self.assertEqual(sentinel.read_bytes(), b"accepted")

    def test_check_rejects_invalid_vertex_clause_before_output(self):
        self.recipe["rasterized_depth"] = True
        # EXEC addresses one instruction beyond this single CF pair.
        raw = struct.pack("<3I", 0x1FFF, 0x1000, 0)
        (self.root / "vs.bin").write_bytes(raw)
        self.recipe["shaders"][0]["microcode"]["sha256"] = hashlib.sha256(
            raw
        ).hexdigest()
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")
        for check in (True, False):
            with (
                self.subTest(check=check),
                self.assertRaisesRegex(ValueError, "instruction extent"),
            ):
                SHADERS.generate(self.path, check=check)
            self.assertFalse((self.root / "generated").exists())

    def test_thirteen_texture_fetches_emit_bounded_binding_tables(self):
        slots = list(range(13))
        header = SHADERS.render_shared_header(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            slots,
            True,
        )
        self.assertIn("g_Texture2DDescriptorHeap[13]", header)
        self.assertIn("g_SamplerDescriptorHeap[13]", header)
        for descriptor, fetch in enumerate(slots):
            self.assertIn(
                f"#define s{fetch}_Texture2DDescriptorIndex {descriptor}\n",
                header,
            )
            self.assertIn(
                f"#define s{fetch}_SamplerDescriptorIndex {descriptor}\n",
                header,
            )

    def test_duplicate_out_of_range_and_non_integer_slots_rejected(self):
        for slots in ([1, 1], [-1], [32], ["1"], [True]):
            with self.subTest(slots=slots):
                with self.assertRaisesRegex(
                    ValueError, "unsupported shader texture interface"
                ):
                    SHADERS.texture_layout({"texture_fetches": slots})

    def test_mixed_cube_bindings_keep_packed_indices(self):
        slots, independent, dimensions = SHADERS.texture_layout(
            {
                "texture_fetches": [0, 3, 8],
                "texture_dimensions": ["2d", "cube", "2d"],
                "independent_samplers": True,
            }
        )
        header = SHADERS.render_shared_header(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "TextureCube<float4> g_TextureCubeDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            slots,
            independent,
            dimensions,
        )
        self.assertIn("g_TextureCubeDescriptorHeap[3]", header)
        self.assertIn("g_Texture2DDescriptorHeap[3]", header)
        self.assertIn("#define s3_TextureCubeDescriptorIndex 1\n", header)
        self.assertNotIn("#define s3_Texture2DDescriptorIndex", header)
        self.assertIn("#define s8_Texture2DDescriptorIndex 2\n", header)
        self.assertIn("#define s3_SamplerDescriptorIndex 1\n", header)

    def test_unknown_or_mismatched_dimensions_rejected(self):
        for dimensions in (["3d"], [], ["2d", "cube"], "2d", [None]):
            with self.subTest(dimensions=dimensions):
                with self.assertRaisesRegex(ValueError, "texture dimensions"):
                    SHADERS.texture_layout(
                        {"texture_fetches": [0], "texture_dimensions": dimensions}
                    )

    def test_independent_sampler_mapping(self):
        header = SHADERS.render_shared_header(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            [2, 0, 1],
            True,
        )
        self.assertIn("#define s2_Texture2DDescriptorIndex 0\n", header)
        self.assertIn("#define s2_SamplerDescriptorIndex 0\n", header)
        self.assertIn("#define s0_Texture2DDescriptorIndex 1\n", header)
        self.assertIn("#define s0_SamplerDescriptorIndex 1\n", header)
        self.assertIn("#define s1_Texture2DDescriptorIndex 2\n", header)
        self.assertIn("#define s1_SamplerDescriptorIndex 2\n", header)

    def test_legacy_shared_sampler_mapping(self):
        header = SHADERS.render_shared_header(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            [2, 0, 1],
            False,
        )
        self.assertIn("g_Texture2DDescriptorHeap[3]", header)
        self.assertIn("g_SamplerDescriptorHeap[1]", header)
        for fetch in (2, 0, 1):
            self.assertIn(f"#define s{fetch}_SamplerDescriptorIndex 0\n", header)

    def test_empty_texture_layout_uses_nonzero_dummy_heaps(self):
        header = SHADERS.render_shared_header(
            "Texture2D<float4> g_Texture2DDescriptorHeap[1];\n"
            "SamplerState g_SamplerDescriptorHeap[1];\n",
            [],
            True,
        )
        self.assertIn("g_Texture2DDescriptorHeap[1]", header)
        self.assertIn("g_SamplerDescriptorHeap[1]", header)

    @staticmethod
    def pixel_source():
        return (
            "float4 c[224];\n"
            "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;\n"
            "uint g_SpecConstants();\n"
            + "".join(f"\tin float4 iTexCoord{i} : TEXCOORD{i},\n" for i in range(16))
            + "\tout float oDepth : SV_Depth)\n"
            "\toDepth = 0.0;\n"
            "clip(oC0.w - g_AlphaThreshold);\n"
            + "".join(f"\tfloat4 r{i} = iTexCoord{i};\n" for i in range(16))
        )

    def test_gameplay_keeps_rasterized_depth_and_strict_alpha_comparison(self):
        words = [0] * 0x5003
        words[0x2202] = 0x8700000C
        depth, alpha = SHADERS.pixel_state({"rasterized_depth": True}, words)
        source = SHADERS.specialize_pixel_source(
            self.pixel_source(), words, depth, alpha
        )
        self.assertNotIn("SV_Depth", source)
        self.assertNotIn("oDepth", source)
        self.assertIn("if (!(oC0.w > g_AlphaThreshold)) discard;", source)
        self.assertIn("uint g_SpecConstants() { return 2; }", source)
        self.assertIn("oCoverage = 0x9;", source)

    def test_single_sample_uses_native_coverage_bit(self):
        words = [0] * 0x5003
        words[0x2000] = 0x14000500
        self.assertEqual(
            SHADERS.target_state({"color_format": 0, "color_exp_bias": 0}, words),
            (1, 1, 0, 0),
        )
        source = SHADERS.specialize_pixel_source(
            self.pixel_source(), words, True, False, 1, 0, 0
        )
        self.assertIn("oCoverage = 0x1;", source)
        self.assertNotIn("oCoverage = 0x9;", source)

    def test_generated_pixel_address_replaces_the_selected_interpolator(self):
        words = [0] * 0x5003
        words[0x2180] = 1 << 18
        words[0x2181] = 1 << 8
        source = SHADERS.specialize_pixel_source(
            self.pixel_source(), words, True, False, 1
        )
        self.assertIn("float4 r1 = float4(abs(floor(iPos.xy)), 0.0, 0.0);", source)
        self.assertIn("asuint(r1.x) | (iFace ? 0u : 0x80000000u)", source)
        self.assertNotIn("float4 r1 = iTexCoord1;", source)
        self.assertIn("float4 r0 = iTexCoord0;", source)
        words[0x2180] = 0
        ordinary = SHADERS.specialize_pixel_source(
            self.pixel_source(), words, True, False, 1
        )
        self.assertIn("float4 r1 = iTexCoord1;", ordinary)
        self.assertNotIn("floor(iPos.xy)", ordinary)

    def test_generated_pixel_parameter_rejects_an_unrepresented_register(self):
        words = [0] * 0x5003
        words[0x2180] = 1 << 18
        words[0x2181] = 16 << 8
        with self.assertRaisesRegex(ValueError, "pixel parameter register"):
            SHADERS.specialize_pixel_source(self.pixel_source(), words, True, False)

    def test_signed_16_16_target_scales_before_return(self):
        words = [0] * 0x5003
        words[0x2000] = 0x14000500
        words[0x2001] = 0x00040000
        self.assertEqual(
            SHADERS.target_state({"color_format": 4, "color_exp_bias": 0}, words),
            (1, 1, 4, 0),
        )
        source = SHADERS.specialize_pixel_source(
            self.pixel_source() + "return;\n", words, True, False, 1, 4, 0
        )
        self.assertIn("oC0.xy *= exp2(0.0 - 5.0);", source)
        self.assertLess(source.index("oC0.xy *="), source.index("return;"))

    def test_rgb10_target_preserves_normalized_output_and_coverage(self):
        words = [0] * 0x5003
        words[0x2000] = 0x0A010280
        words[0x2001] = 0x00020000
        self.assertEqual(
            SHADERS.target_state({"color_format": 2, "color_exp_bias": 0}, words),
            (4, 9, 2, 0),
        )
        source = self.pixel_source() + "return;\n"
        self.assertEqual(
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 2, 0),
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 0, 0),
        )
        words[0x2001] = 0x00120000
        with self.assertRaisesRegex(ValueError, "exponent"):
            SHADERS.target_state({"color_format": 2, "color_exp_bias": 1}, words)
        with self.assertRaisesRegex(ValueError, "target specialization"):
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 2, 1)

    def test_target_state_rejects_unknown_format_exp_bias_and_sample(self):
        words = [0] * 0x5003
        words[0x2000] = 0x14000500
        words[0x2001] = 0x00070000
        with self.assertRaisesRegex(ValueError, "target format"):
            SHADERS.target_state({}, words)
        words[0x2001] = 0x00B00000
        with self.assertRaisesRegex(ValueError, "exponent"):
            SHADERS.target_state({}, words)
        words[0x2001] = 0
        with self.assertRaisesRegex(ValueError, "sample specialization"):
            SHADERS.target_state({"sample_count": 4, "sample_mask": 9}, words)

    def test_second_color_target_becomes_an_output(self):
        words = [0] * 0x5003
        words[0x2003] = 0x0002021C
        self.assertEqual(SHADERS.color_targets({"color_targets": 2}, words), 2)
        source = (
            self.pixel_source()
            + "\tout float4 oC0 : SV_Target0,\n\tfloat4 oC1 = 0.0;\n\toC1 = r1;\n"
        )
        two = SHADERS.specialize_pixel_source(source, words, True, False, 9, 0, 0, 2)
        self.assertIn(
            "\tout float4 oC0 : SV_Target0,\n\tout float4 oC1 : SV_Target1,", two
        )
        self.assertNotIn("float4 oC1 = 0.0;", two)
        self.assertIn("\toC1 = 0.0;", two)
        # One target leaves the translation byte-identical to the default.
        self.assertEqual(SHADERS.color_targets({}, words), 1)
        self.assertEqual(
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 0, 0, 1),
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 0, 0),
        )
        self.assertNotIn(
            "SV_Target1",
            SHADERS.specialize_pixel_source(source, words, True, False, 9, 0, 0),
        )

    def test_second_color_target_format_and_count_are_bounded(self):
        words = [0] * 0x5003
        words[0x2003] = 0x00040000
        with self.assertRaisesRegex(ValueError, "second shader target format"):
            SHADERS.color_targets({"color_targets": 2}, words)
        words[0x2003] = 0x00120000
        with self.assertRaisesRegex(ValueError, "second shader target format"):
            SHADERS.color_targets({"color_targets": 2}, words)
        words[0x2003] = 0
        for count in (0, 3, True, "2"):
            with self.assertRaisesRegex(ValueError, "color target count"):
                SHADERS.color_targets({"color_targets": count}, words)

    def test_guest_depth_export_cannot_be_discarded(self):
        with self.assertRaisesRegex(ValueError, "guest depth export"):
            SHADERS.specialize_pixel_source(
                self.pixel_source() + "oDepth = r0.x;\n", [0] * 0x5003, True, False
            )

    def test_greater_equal_alpha_keeps_equal_alpha(self):
        words = [0] * 0x5003
        words[0x2202] = 0x8700000E
        depth, alpha = SHADERS.pixel_state({"rasterized_depth": True}, words)
        source = SHADERS.specialize_pixel_source(
            self.pixel_source(), words, depth, alpha
        )
        self.assertIn("if (!(oC0.w >= g_AlphaThreshold)) discard;", source)
        self.assertIn("uint g_SpecConstants() { return 2; }", source)

    def test_other_alpha_comparisons_require_implementation(self):
        words = [0] * 0x5003
        words[0x2202] = 0x8700000B
        with self.assertRaisesRegex(ValueError, "alpha comparison"):
            SHADERS.pixel_state({"rasterized_depth": True}, words)

    def test_vertex_fetch_uses_explicit_zero_lod_without_derivatives(self):
        words = [
            1 | (1 << 12) | (1 << 16),
            2 << 12,
            0,
            1 | (16 << 20),
            1 << 28,
            1 << 14,
        ]
        header = (
            "return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], "
            "texCoord + offset / getTexture2DDimensions(texture));"
        )
        result = SHADERS.vertex_texture_header(header, words)
        self.assertIn("texture.SampleLevel(", result)
        self.assertTrue(result.endswith(", 0.0);"))
        words[4] |= 1 << 29
        with self.assertRaisesRegex(ValueError, "vertex texture sampling mode"):
            SHADERS.vertex_texture_header(header, words)

    def test_fetch_clause_cannot_point_outside_shader(self):
        with self.assertRaisesRegex(ValueError, "instruction extent"):
            SHADERS.fetch_instructions([7 | (1 << 12), 2 << 12, 0])


GOLDENS = Path(__file__).with_name("native_shader_dxil_goldens.json")
GOLDEN_HEADER = (
    "#ifdef __spirv__\n"
    "#define SHARED_CONSTANT_ADDRESSES 1\n"
    "#endif\n"
    "Texture2D<float4> g_Texture2DDescriptorHeap[1] : register(t0, space0);\n"
    "Texture3D<float4> g_Texture3DDescriptorHeap[1] : register(t0, space1);\n"
    "TextureCube<float4> g_TextureCubeDescriptorHeap[1] : register(t0, space2);\n"
    "SamplerState g_SamplerDescriptorHeap[1] : register(s0, space3);\n"
)
GOLDEN_VERTEX = (
    "cbuffer VertexShaderConstants : register(b0, space4)\n{\n\tfloat4 c[256];\n};\n"
    "cbuffer SharedConstants : register(b2, space4)\n{\n"
    "\tDEFINE_SHARED_CONSTANTS();\n};\n"
    "void main(\n\tin float4 iTexCoord0 : TEXCOORD0,\n"
    "\tout float4 oPos : SV_Position,\n"
    + "".join(f"\tout float4 oTexCoord{i} : TEXCOORD{i},\n" for i in range(15))
    + "\tout float4 oTexCoord15 : TEXCOORD15)\n{\n"
    "\toPos = iTexCoord0;\n"
    "\toPos.xy += g_HalfPixelOffset * oPos.w;\n}\n"
)
GOLDEN_PIXEL = (
    "cbuffer PixelShaderConstants : register(b1, space4)\n{\n\tfloat4 c[224];\n};\n"
    "cbuffer SharedConstants : register(b2, space4)\n{\n"
    "\tDEFINE_SHARED_CONSTANTS();\n};\n"
    "[[vk::constant_id(0)]] const uint g_SpecConstants = 0;\n"
    "uint g_SpecConstants();\n"
    "void main(\n\tin float4 iPos : SV_Position,\n\tin uint iFace : SV_IsFrontFace,\n"
    + "".join(f"\tin float4 iTexCoord{i} : TEXCOORD{i},\n" for i in range(16))
    + "\tout float4 oC0 : SV_Target0,\n\tout float oDepth : SV_Depth)\n{\n"
    + "".join(f"\tfloat4 r{i} = iTexCoord{i};\n" for i in range(16))
    + "\tfloat4 oC1 = 0.0;\n\toC0 = r0;\n\toC1 = r1;\n\toDepth = 0.0;\n"
    "\tclip(oC0.w - g_AlphaThreshold);\n\treturn;\n}\n"
)
# One recipe per DXIL shape the generator specializes. The registers start
# from a 4X target with RB_COLOR_INFO 0; each case names the words it changes.
GOLDEN_CASES = {
    "plain": ({}, {}),
    "screen_space_position": ({}, {0x2204: 0x90000, 0x2206: 0x400}),
    "user_clip_plane": ({}, {0x2204: 0x80001}),
    "alpha_tested": ({"rasterized_depth": True}, {0x2202: 0x8700000C}),
    "two_color_targets": ({"color_targets": 2}, {0x2003: 0x0002021C}),
    "rasterized_depth": (
        {
            "rasterized_depth": True,
            "texture_fetches": [0, 1],
            "texture_dimensions": ["2d", "cube"],
        },
        {},
    ),
    "single_sample_signed_target_pixel_address": (
        {
            "rasterized_depth": True,
            "color_format": 4,
            "color_exp_bias": 0,
            "sample_count": 1,
            "sample_mask": 1,
        },
        {
            0x2000: 0x14000500,
            0x2001: 0x00040000,
            0x2180: 1 << 18,
            0x2181: 1 << 8,
        },
    ),
    "wrapper_compilers": ({}, {}),
}


def golden_run(args, **kwargs):
    """Stand in for the pinned tools with deterministic outputs.

    The adapter inlines the stage header ahead of a fixed body, as the raw
    adapter does. The compiler output digests its flags and source text.
    """
    if "--raw" in args:
        stage, source, header = args[2], Path(args[5]), Path(args[6])
        body = GOLDEN_VERTEX if stage == "vs" else GOLDEN_PIXEL
        source.write_text(
            header.read_text(encoding="ascii") + f"// {args[7]}\n" + body,
            encoding="ascii",
        )
        return subprocess.CompletedProcess(args, 0, f"raw {stage} {args[7]}\n", "")
    if "-Fo" in args:
        output = args[args.index("-Fo") + 1]
        source = args[-1]
        flags = [Path(args[0]).name] + [
            arg for arg in args[1:] if arg not in (output, source)
        ]
    else:
        source, output = args[2], args[3]
        flags = [Path(arg).name for arg in args[:2]]
    payload = "\n".join(flags).encode() + b"\0" + Path(source).read_bytes()
    Path(output).write_bytes(b"FAKE" + hashlib.sha256(payload).digest())
    return subprocess.CompletedProcess(args, 0, "", "")


def golden_recipe(root, name):
    """Write the named golden recipe and its pinned inputs under root."""
    options, changed = GOLDEN_CASES[name]

    def asset(file_name, data):
        (root / file_name).write_bytes(data)
        return {"file": file_name, "sha256": hashlib.sha256(data).hexdigest()}

    registers = [0] * 0x5003
    registers[0x2000] = 0x0A010280
    for index, value in changed.items():
        registers[index] = value
    if name == "wrapper_compilers":
        compilers = [("vertex_compiler", ".exe"), ("pixel_compiler", ".exe")]
    else:
        compilers = [("dxc", ".exe")]
    recipe = {
        "schema_version": 1,
        "output": "generated",
        "texture_fetches": [0],
        "independent_samplers": True,
        "texture_dimensions": ["2d"],
        "registers": asset("registers.le.bin", struct.pack("<20483I", *registers)),
        "shared_header": asset("shader_common.h", GOLDEN_HEADER.encode("ascii")),
        "tools": {
            tool: asset(tool + suffix, f"pinned {tool}".encode())
            for tool, suffix in [
                ("adapter", ".exe"),
                *compilers,
                ("dxcompiler", ".dll"),
                ("dxil", ".dll"),
            ]
        },
        "shaders": [
            {
                "stage": "vs",
                "hash": "0123456789ABCDEF",
                "byte_order": "little",
                "microcode": asset("vs.ucode.le.bin", bytes(12)),
            },
            {
                "stage": "ps",
                "hash": "FEDCBA9876543210",
                "byte_order": "big",
                "microcode": asset("ps.ucode.be.bin", bytes(range(1, 13))),
            },
        ],
        **options,
    }
    return recipe


def golden_outputs(root, recipe):
    """Run generate with golden tools; return normalized generation and files."""
    path = root / "shader-recipe.json"
    path.write_text(json.dumps(recipe, indent=2) + "\n", encoding="ascii")
    with patch.object(SHADERS.subprocess, "run", side_effect=golden_run):
        SHADERS.generate(path)
    output = root / recipe["output"]
    files = {
        item.relative_to(output).as_posix(): hashlib.sha256(
            item.read_bytes()
        ).hexdigest()
        for item in sorted(output.rglob("*"))
        if item.is_file() and item.name != "generation.json"
    }
    # The recipe path is absolute; everything else is fixed by the fixture.
    generation = (output / "generation.json").read_text(encoding="ascii")
    generation = generation.replace(json.dumps(str(path.resolve()))[1:-1], "<recipe>")
    return generation, files


class NativeShaderDxilGoldenTests(unittest.TestCase):
    """Pin DXIL generation bytes for each specialized recipe shape."""

    def test_dxil_generation_matches_goldens(self):
        goldens = json.loads(GOLDENS.read_text(encoding="ascii"))
        self.assertEqual(set(goldens), set(GOLDEN_CASES))
        for name in GOLDEN_CASES:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                generation, files = golden_outputs(root, golden_recipe(root, name))
                self.assertEqual(generation, goldens[name]["generation"])
                self.assertEqual(files, goldens[name]["files"])


SPIRV_RECORD_KEYS = {"spirv_sha256"}
WORLD_Z_BLOCK = (
    "\t\tps = cos(r0.y);\n"
    "\t\tr0.z = ps;\n"
    "\t\tr3.x = r1.w * r0.z;\n"
    "\t\tps = sin(r0.y);\n"
    "\t\tr0.z = ps;\n"
    "\t\tr3.w = r0.z * -r1.w;\n"
    "\t\tps = sin(r0.x);\n"
    "\t\tr3.z = ps;\n"
    "\t\tr0.xyz = r3.wyx * r3.yzy;\n"
)


def format_recipe(root, name, output_format, **changes):
    """Return a golden recipe that also pins spirv-val for SPIR-V output."""
    recipe = golden_recipe(root, name)
    validator = root / "spirv-val.exe"
    validator.write_bytes(b"pinned spirv_val")
    recipe["tools"]["spirv_val"] = {
        "file": validator.name,
        "sha256": hashlib.sha256(validator.read_bytes()).hexdigest(),
    }
    recipe["output_format"] = output_format
    recipe.update(changes)
    return recipe


def spirv_run(failing=(), world_z=False):
    """Golden tools plus a validator that fails for the named artifacts."""

    def run(args, **kwargs):
        if "--target-env" in args:
            if Path(args[-1]).name in failing:
                return subprocess.CompletedProcess(args, 1, "", "invalid module")
            return subprocess.CompletedProcess(args, 0, "", "")
        result = golden_run(args, **kwargs)
        if world_z and "--raw" in args and args[2] == "vs":
            source = Path(args[5])
            source.write_text(
                source.read_text(encoding="ascii").replace(
                    "\toPos = iTexCoord0;\n", "\toPos = iTexCoord0;\n" + WORLD_Z_BLOCK
                ),
                encoding="ascii",
            )
        return result

    return run


def write_and_generate(root, recipe, run):
    path = root / "shader-recipe.json"
    path.write_text(json.dumps(recipe, indent=2) + "\n", encoding="ascii")
    with patch.object(SHADERS.subprocess, "run", side_effect=run):
        SHADERS.generate(path)
    return root / recipe["output"]


def dxil_half(generation):
    """Exclude the dual-format outputs that identify the format."""
    generation = {key: value for key, value in generation.items() if key != "spirv"}
    generation["shaders"] = [
        {key: value for key, value in record.items() if key not in SPIRV_RECORD_KEYS}
        for record in generation["shaders"]
    ]
    return generation


class NativeShaderDualOutputTests(unittest.TestCase):
    """One run emits the DXIL files and a validated SPIR-V variant."""

    def test_dual_dxil_half_matches_goldens_and_spirv_matches_legacy(self):
        goldens = json.loads(GOLDENS.read_text(encoding="ascii"))
        for name in GOLDEN_CASES:
            if name == "wrapper_compilers":
                continue
            with self.subTest(case=name), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                output = write_and_generate(
                    root, format_recipe(root, name, "dual"), spirv_run()
                )
                legacy = write_and_generate(
                    root,
                    format_recipe(root, name, "spirv", output="legacy"),
                    spirv_run(),
                )
                files = {
                    item.name: hashlib.sha256(item.read_bytes()).hexdigest()
                    for item in output.iterdir()
                    if item.is_file() and item.name != "generation.json"
                }
                self.assertEqual(files, goldens[name]["files"])
                generation = json.loads((output / "generation.json").read_text())
                golden = json.loads(goldens[name]["generation"])
                generation["recipe"] = "<recipe>"
                self.assertEqual(dxil_half(generation), dxil_half(golden))
                features = ["scalarBlockLayout"]
                if name == "user_clip_plane":
                    features.append("shaderClipDistance")
                self.assertEqual(
                    generation["spirv"],
                    {
                        "target_env": "vulkan1.2",
                        "required_features": features,
                        "spirv_val_sha256": SHADERS.digest(root / "spirv-val.exe"),
                    },
                )
                legacy_records = json.loads(
                    (legacy / "generation.spirv.json").read_text()
                )["shaders"]
                spirv = output / "spirv"
                for record, legacy_record in zip(
                    generation["shaders"], legacy_records, strict=True
                ):
                    prefix = f"{record['stage']}_{record['hash']}"
                    self.assertEqual(
                        record["spirv_sha256"], SHADERS.digest(spirv / f"{prefix}.spv")
                    )
                    # Swapping the header equals translating with the SPIR-V one.
                    for suffix in (".hlsl", ".executable.hlsl", ".spv"):
                        self.assertEqual(
                            (spirv / f"{prefix}{suffix}").read_bytes(),
                            (legacy / f"{prefix}{suffix}").read_bytes(),
                        )
                    self.assertEqual(
                        record["spirv_sha256"], legacy_record["spirv_sha256"]
                    )
                    for log in ("compile", "validation"):
                        self.assertTrue((spirv / f"{prefix}.{log}.log").exists())
                    # The SPIR-V header edit never reaches the DXIL variant.
                    marker = "#if 0 // Raw adapter"
                    self.assertIn(marker, (spirv / f"{prefix}.hlsl").read_text())
                    self.assertNotIn(marker, (output / f"{prefix}.hlsl").read_text())

    def test_spirv_face_is_bool_only_where_the_pixel_address_reads_it(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            name = "single_sample_signed_target_pixel_address"
            output = write_and_generate(
                root, format_recipe(root, name, "dual"), spirv_run()
            )
            prefix = "ps_FEDCBA9876543210.executable.hlsl"
            dxil = (output / prefix).read_text(encoding="ascii")
            spirv = (output / "spirv" / prefix).read_text(encoding="ascii")
            self.assertIn("\tin uint iFace : SV_IsFrontFace,", dxil)
            self.assertIn("(iFace ? 0u : 0x80000000u)", dxil)
            self.assertIn("\tin bool iFace : SV_IsFrontFace,", spirv)
            self.assertNotIn("uint iFace", spirv)
            plain = write_and_generate(
                root, format_recipe(root, "plain", "dual", output="plain"), spirv_run()
            )
            self.assertIn(
                "\tin uint iFace : SV_IsFrontFace,",
                (plain / "spirv" / prefix).read_text(encoding="ascii"),
            )
        with self.assertRaisesRegex(ValueError, "unexpected translated"):
            SHADERS.spirv_front_face("\tin bool iFace : SV_IsFrontFace,")

    def test_dual_user_clip_plane_lists_clip_distance_feature(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = write_and_generate(
                root, format_recipe(root, "user_clip_plane", "dual"), spirv_run()
            )
            generation = json.loads((output / "generation.json").read_text())
            self.assertIn(
                "shaderClipDistance", generation["spirv"]["required_features"]
            )
            vertex = output / "spirv/vs_0123456789ABCDEF.executable.hlsl"
            self.assertIn(
                "out float oClipDistance0 : SV_ClipDistance0)",
                vertex.read_text(encoding="ascii"),
            )
            self.assertTrue(generation["shaders"][0]["user_clip_plane"])

    def test_dual_world_z_ordering_applies_only_to_spirv(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            recipe = format_recipe(root, "plain", "dual")
            recipe["shaders"][0]["hash"] = "C3BAB94E67553E12"
            output = write_and_generate(root, recipe, spirv_run(world_z=True))
            prefix = "vs_C3BAB94E67553E12"
            dxil = (output / f"{prefix}.hlsl").read_text(encoding="ascii")
            self.assertFalse((output / f"{prefix}.executable.hlsl").exists())
            self.assertIn(WORLD_Z_BLOCK, dxil)
            self.assertNotIn("worldZCosB", dxil)
            spirv = (output / f"spirv/{prefix}.executable.hlsl").read_text()
            self.assertIn("r0.xyz = r3.wyx * float3(r3.y, r3.z, worldZCosB);", spirv)
            self.assertNotIn(WORLD_Z_BLOCK, spirv)

    def test_failed_spirv_half_writes_no_generation(self):
        for failing in ("vs_0123456789ABCDEF.spv", "ps_FEDCBA9876543210.spv"):
            with self.subTest(failing=failing), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                recipe = format_recipe(root, "plain", "dual")
                with self.assertRaises(subprocess.CalledProcessError):
                    write_and_generate(root, recipe, spirv_run(failing=(failing,)))
                output = root / "generated"
                self.assertFalse((output / "generation.json").exists())
                log = (output / "spirv" / failing).with_suffix(".validation.log")
                self.assertEqual(log.read_text(), "invalid module")
                with self.assertRaisesRegex(ValueError, "fresh directory"):
                    write_and_generate(root, recipe, spirv_run())

    def test_translation_without_its_header_prefix_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)

            def run(args, **kwargs):
                result = spirv_run()(args, **kwargs)
                if "--raw" in args:
                    source = Path(args[5])
                    source.write_text("// moved\n" + source.read_text())
                return result

            with self.assertRaisesRegex(ValueError, "does not begin with its header"):
                write_and_generate(root, format_recipe(root, "plain", "dual"), run)
            self.assertFalse((root / "generated/generation.json").exists())

    def test_vertex_only_depth_recipe_emits_both_formats(self):
        goldens = json.loads(GOLDENS.read_text(encoding="ascii"))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            recipe = format_recipe(root, "plain", "dual", depth_only=True)
            recipe["shaders"] = recipe["shaders"][:1]
            output = write_and_generate(root, recipe, spirv_run())
            generation = json.loads((output / "generation.json").read_text())
            self.assertTrue(generation["depth_only"])
            (record,) = generation["shaders"]
            self.assertEqual(record["stage"], "vs")
            self.assertTrue(SPIRV_RECORD_KEYS <= set(record))
            # The vertex DXIL equals the same vertex compiled within a pair.
            (golden,) = [
                item
                for item in json.loads(goldens["plain"]["generation"])["shaders"]
                if item["stage"] == "vs"
            ]
            dxil = {k: v for k, v in record.items() if k not in SPIRV_RECORD_KEYS}
            self.assertEqual(dxil, golden)
            self.assertEqual(
                sorted(item.name for item in (output / "spirv").glob("*.spv")),
                ["vs_0123456789ABCDEF.spv"],
            )

    def test_vertex_only_recipe_shape_is_bounded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / "shader-recipe.json"
            cases = [
                ("dxil", True, slice(1), "require dual output"),
                ("dual", "yes", slice(1), "invalid depth-only setting"),
                ("dual", True, slice(2), "requires one vertex shader"),
                ("dual", True, slice(1, 2), "requires one vertex shader"),
                ("dual", False, slice(1), "one vertex/pixel pair"),
            ]
            for output_format, depth_only, shaders, message in cases:
                with self.subTest(message=message, shaders=shaders):
                    recipe = format_recipe(root, "plain", output_format)
                    recipe["depth_only"] = depth_only
                    recipe["shaders"] = recipe["shaders"][shaders]
                    path.write_text(json.dumps(recipe), encoding="ascii")
                    with self.assertRaisesRegex(ValueError, message):
                        SHADERS.generate(path, check=True)
                    self.assertFalse((root / "generated").exists())
            # Its catalog entry cannot express a position specialization.
            for name in ("screen_space_position", "user_clip_plane"):
                with self.subTest(case=name):
                    recipe = format_recipe(root, name, "dual", depth_only=True)
                    recipe["shaders"] = recipe["shaders"][:1]
                    path.write_text(json.dumps(recipe), encoding="ascii")
                    with self.assertRaisesRegex(ValueError, "guest clipping"):
                        SHADERS.generate(path, check=True)
                    self.assertFalse((root / "generated").exists())

    def test_dual_requires_standalone_dxc_and_validator(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / "shader-recipe.json"
            recipe = format_recipe(root, "wrapper_compilers", "dual")
            path.write_text(json.dumps(recipe), encoding="ascii")
            with self.assertRaisesRegex(ValueError, "standalone"):
                SHADERS.generate(path)
            recipe = format_recipe(root, "plain", "dual")
            recipe["tools"].pop("spirv_val")
            path.write_text(json.dumps(recipe), encoding="ascii")
            with self.assertRaisesRegex(ValueError, "spirv_val"):
                SHADERS.generate(path)
            self.assertFalse((root / "generated").exists())

    def test_legacy_spirv_user_clip_plane_lists_clip_distance_feature(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = write_and_generate(
                root, format_recipe(root, "user_clip_plane", "spirv"), spirv_run()
            )
            generation = json.loads((output / "generation.spirv.json").read_text())
            self.assertEqual(generation["output_format"], "spirv")
            self.assertEqual(
                generation["required_features"],
                ["scalarBlockLayout", "shaderClipDistance"],
            )
            self.assertFalse((output / "generation.json").exists())
            self.assertFalse((output / "spirv").exists())
