"""Executable checks for the title-owned raw shader adapter."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TITLE_ROOT = Path(__file__).resolve().parents[2]
ADAPTER_ROOT = TITLE_ROOT / "scripts" / "native-shader-adapter"
DEFAULT_EXECUTABLE = (
    TITLE_ROOT / "out" / "build" / "native-shader-adapter" / "cmake" / "raw_adapter.exe"
)
EXECUTABLE = Path(os.environ.get("NATIVE_SHADER_ADAPTER", DEFAULT_EXECUTABLE))
COMMON_HEADER = ADAPTER_ROOT / "shader_common.h"
BUILD_SCRIPT = ADAPTER_ROOT / "build_adapter.py"

PIXEL_WORDS = [
    0x00000000,
    0x2001C400,
    0x22000005,
    0x00000018,
    0x00000000,
    0x00000000,
    0x10080001,
    0x2F1FFFFB,
    0x00004000,
    0xA8878000,
    0x00C0C000,
    0x02FFFF00,
    0x4E4A0006,
    0x95A570DE,
    0x30B52E1D,
]
SUCCESSIVE_WORDS = [
    0x00000000,
    0x4001C400,
    0x22000055,
    0x00000018,
    0x00000000,
    0x00000000,
    0x10080001,
    0x2F1FFFFB,
    0x00004000,
    0x04000038,
    0x00000000,
    0x00000000,
    0x10080021,
    0x2F1FFFFB,
    0x00004000,
]


CUBE_WORDS = [
    0x01016001,
    0x10071800,
    0x28000000,
    0x0C000018,
    0x00000000,
    0x00000000,
    0xC80F0001,
    0x00A6CD00,
    0xD2000000,
    0x4C400100,
    0x000000C6,
    0x20000081,
    0xC8030001,
    0x0000C66C,
    0xCB010100,
    0xC4082021,
    0x2F000688,
    0x0000C000,
    0xC80F0003,
    0x00000000,
    0x8F020100,
    0xC8078000,
    0x00006CB1,
    0x8B030202,
]


def _pack_control_flow(instructions: list[int]) -> list[int]:
    """Packs 48-bit control-flow instructions two per three words."""
    if len(instructions) % 2:
        instructions = instructions + [0]
    words: list[int] = []
    for first, second in zip(instructions[::2], instructions[1::2]):
        words += [
            first & 0xFFFFFFFF,
            ((first >> 32) & 0xFFFF) | ((second & 0xFFFF) << 16),
            second >> 16,
        ]
    return words


def _exec(address: int, count: int = 1, sequence: int = 0, opcode: int = 1) -> int:
    return (opcode << 44) | (sequence << 16) | (count << 12) | address


SCALAR_SETP = [29 << 26, 0, 0]
SETP_CLR = [33 << 26, 0, 0]
VECTOR_DOT = [0xC80F0001, 0x00E4E4E4, 0x0F000000]
RELATIVE_DOT = [0xC80F0001, 0x80E4E4E4, 0x0F000000]


def _jump(target: int, condition: int) -> int:
    return (11 << 44) | (condition << 42) | (1 << 14) | target


def _diamond_words(
    writer_in_then: bool,
    register_lod: bool = True,
    same_condition: bool = False,
    outside_entry: bool = False,
) -> list[int]:
    # p0 = setp; if (!p0) goto 4; then; if (p0) goto 5; 4: cond_pred_exec
    # else; 5: setter and fetch; exec_end. An outside entry adds a jump from
    # before the diamond into its then-branch.
    lead = [_jump(3, 1)] if outside_entry else []
    shift = len(lead)
    flow = [
        _exec(4),
        *lead,
        _jump(4 + shift, 0),
        _exec(5),
        _jump(5 + shift, 0 if same_condition else 1),
        _exec(6, opcode=5),
        _exec(7, 2, 0b0101),
        _exec(9, opcode=2),
    ]
    then = SCALAR_SETP if writer_in_then else VECTOR_DOT
    fetch = PIXEL_WORDS[6:9]
    if not register_lod:
        # Two implicit-LOD fetches replace the setter and register LOD fetch.
        fetch = [fetch[0], fetch[1] & ~(1 << 29), fetch[2]]
    instructions = [
        SCALAR_SETP,
        then,
        VECTOR_DOT,
        PIXEL_WORDS[3:6] if register_lod else fetch,
        fetch,
        VECTOR_DOT,
    ]
    return _pack_control_flow(flow) + [w for i in instructions for w in i]


def _loop_words(
    nested: bool = False,
    end_extra: int = 0,
    start_opcode: int = 7,
    start_extra: int = 0,
    skip_target: int | None = None,
    body_target: int | None = None,
) -> list[int]:
    def loop(opcode: int, index: int, address: int, extra: int = 0) -> int:
        return (opcode << 44) | (index << 16) | address | extra

    if body_target is not None or skip_target is not None:
        # Two body clauses; the canonical targets are skip 5 and body 2.
        flow = [
            _exec(3),
            loop(
                start_opcode, 16, 5 if skip_target is None else skip_target, start_extra
            ),
            _exec(4),
            _exec(5),
            loop(8, 16, 2 if body_target is None else body_target, end_extra),
            _exec(6, opcode=2),
        ]
        instructions = [VECTOR_DOT, VECTOR_DOT, RELATIVE_DOT, VECTOR_DOT]
        return _pack_control_flow(flow) + [w for i in instructions for w in i]
    if nested:
        flow = [
            _exec(4),
            loop(start_opcode, 16, 6),
            loop(7, 17, 5),
            _exec(5),
            loop(8, 17, 3, end_extra),
            loop(8, 16, 2),
            _exec(6, opcode=2),
            0,
        ]
    else:
        flow = [
            _exec(3),
            loop(start_opcode, 16, 4, start_extra),
            _exec(4),
            loop(8, 16, 2, end_extra),
            _exec(5, opcode=2),
            0,
        ]
    instructions = [VECTOR_DOT, RELATIVE_DOT, VECTOR_DOT]
    return _pack_control_flow(flow) + [w for i in instructions for w in i]


class NativeShaderAdapterTest(unittest.TestCase):
    def test_manifest_matches_owned_inputs(self) -> None:
        manifest = json.loads(
            (ADAPTER_ROOT / "manifest.json").read_text(encoding="ascii")
        )
        for key in (
            "lod_patch",
            "write_mask_patch",
            "exec_gate_patch",
            "lod_flow_patch",
            "loop_patch",
            "shared_header",
        ):
            with self.subTest(input=key):
                item = manifest[key]
                self.assertEqual(
                    item["sha256"],
                    hashlib.sha256(
                        (ADAPTER_ROOT / item["file"]).read_bytes()
                    ).hexdigest(),
                )

    def _run(
        self,
        shader_type: str,
        words: list[int],
        registers: list[int] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], str]:
        if not EXECUTABLE.is_file():
            self.skipTest(f"fresh adapter executable is missing: {EXECUTABLE}")
        with tempfile.TemporaryDirectory(prefix="native-shader-adapter-") as directory:
            root = Path(directory)
            (root / "words.le").write_bytes(struct.pack(f"<{len(words)}I", *words))
            if registers is None:
                registers = [0] * 0x5003
            self.assertEqual(len(registers), 0x5003)
            (root / "registers.le").write_bytes(
                struct.pack(f"<{len(registers)}I", *registers)
            )
            result = subprocess.run(
                [
                    str(EXECUTABLE),
                    "--raw",
                    shader_type,
                    str(root / "words.le"),
                    str(root / "registers.le"),
                    str(root / "output.hlsl"),
                    str(COMMON_HEADER),
                    "adapter-test",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            output = (
                (root / "output.hlsl").read_text(encoding="utf-8")
                if result.returncode == 0
                else ""
            )
            return result, output

    def test_builder_rejects_existing_roots_before_writing(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="native-shader-adapter-builder-"
        ) as directory:
            tmp_path = Path(directory)
            self._test_builder_rejects_existing_roots_before_writing(tmp_path)

    def _test_builder_rejects_existing_roots_before_writing(
        self, tmp_path: Path
    ) -> None:
        existing_build = tmp_path / "existing-build"
        existing_build.mkdir()
        build_marker = existing_build / "marker"
        build_marker.write_bytes(b"keep-build")
        result = subprocess.run(
            [
                os.fspath(Path(sys.executable)),
                os.fspath(BUILD_SCRIPT),
                "--source-repo",
                os.fspath(TITLE_ROOT),
                "--build-root",
                os.fspath(existing_build),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build root must be fresh", result.stderr)
        self.assertEqual(build_marker.read_bytes(), b"keep-build")

        fresh_build = tmp_path / "fresh-build"
        existing_diagnostics = tmp_path / "existing-diagnostics"
        existing_diagnostics.mkdir()
        diagnostics_marker = existing_diagnostics / "marker"
        diagnostics_marker.write_bytes(b"keep-diagnostics")
        result = subprocess.run(
            [
                os.fspath(Path(sys.executable)),
                os.fspath(BUILD_SCRIPT),
                "--source-repo",
                os.fspath(TITLE_ROOT),
                "--build-root",
                os.fspath(fresh_build),
                "--diagnostics-root",
                os.fspath(existing_diagnostics),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("diagnostics root must be fresh", result.stderr)
        self.assertEqual(diagnostics_marker.read_bytes(), b"keep-diagnostics")
        self.assertFalse(fresh_build.exists())

    def test_canonical_cube_register_lod_restores_direction(self) -> None:
        registers = [0] * 0x5003
        registers[0x4400] = 0x3FC00000
        result, output = self._run("ps", CUBE_WORDS.copy(), registers)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("registerLod = r0.w;", output)
        self.assertIn("r1.xyzw = cube(r0.xyzz, cubeMapData);", output)
        self.assertIn(
            "tfetchCubeRegisterLod(s0_TextureCubeDescriptorIndex, s0_SamplerDescriptorIndex, r1.yxw, cubeMapData, registerLod)",
            output,
        )
        self.assertIn(
            "SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[(uint)texCoord.z], registerLod)",
            output,
        )
        self.assertIn("register(t0, space2)", output)

    def test_cube_register_lod_rejects_manipulated_chain(self) -> None:
        registers = [0] * 0x5003
        registers[0x4400] = 0x3FC00000
        for position, bit in ((7, 1 << 16), (9, 1 << 20), (12, 1 << 18), (15, 1 << 26)):
            with self.subTest(position=position):
                words = CUBE_WORDS.copy()
                words[position] ^= bit
                result, _ = self._run("ps", words, registers)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("canonical chain", result.stderr)

    def test_cube_register_lod_rejects_noncanonical_constant(self) -> None:
        result, _ = self._run("ps", CUBE_WORDS.copy())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("canonical chain", result.stderr)

    def test_cube_register_lod_rejects_an_extra_cube_operation(self) -> None:
        registers = [0] * 0x5003
        registers[0x4400] = 0x3FC00000
        words = CUBE_WORDS.copy()
        words[18:21] = words[6:9]
        result, _ = self._run("ps", words, registers)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cube operation bound", result.stderr)

    def test_supported_register_lod_emits_dynamic_scalar_swizzle(self) -> None:
        result, output = self._run("ps", PIXEL_WORDS.copy())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("registerLod = r0.x;", output)
        self.assertIn("r0.x = tfetch2DRegisterLod(", output)
        self.assertIn(", float2(0, 0), registerLod).w;", output)

    def test_successive_setters_and_non_x_swizzle_follow_execution_order(self) -> None:
        result, output = self._run("ps", SUCCESSIVE_WORDS.copy())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("registerLod = r0.x;", output)
        self.assertIn("registerLod = r1.y;", output)
        self.assertLess(
            output.index("registerLod = r0.x;"),
            output.index("registerLod = r1.y;"),
        )
        self.assertEqual(output.count("tfetch2DRegisterLod("), 3)

    def test_predicated_setter_updates_only_executing_pixels(self) -> None:
        # The SDK translator writes the LOD temp under the setter's predicate,
        # so a predicated setter keeps the earlier value elsewhere.
        for condition, gate in ((0, r"if \(!p0\)"), (1, r"if \(p0\)")):
            with self.subTest(condition=condition):
                words = SUCCESSIVE_WORDS.copy()
                words[10] |= 1 << 31
                words[11] |= condition << 31
                result, output = self._run("ps", words)
                self.assertEqual(result.returncode, 0, result.stderr)
                body = output[output.index("registerLod = r0.x;") :]
                self.assertRegex(body, gate + r"\s*\{\s*registerLod = r1\.y;\s*\}")

    def test_computed_plus_register_lod_scales_gradients(self) -> None:
        words = PIXEL_WORDS.copy()
        words[7] |= 1 << 28
        result, output = self._run("ps", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("r0.x = tfetch2DComputedRegisterLod(", output)
        self.assertIn("ddx_coarse(texCoord) * scale", output)

    def test_proven_predicated_diamond_keeps_a_dominating_setter(self) -> None:
        result, output = self._run("ps", _diamond_words(writer_in_then=False))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("switch (pc)", output)
        self.assertRegex(output, r"if \(p0\)\s*\{[^}]*\}\s*else\s*\{")
        self.assertIn("\n\tregisterLod = r0.x;", output)
        # A predicate writer in the then-branch breaks the proof.
        result, _ = self._run("ps", _diamond_words(writer_in_then=True))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "unsupported pixel register LOD: unstructured control flow", result.stderr
        )

    def test_predicated_clause_gates_its_whole_body_on_entry_p0(self) -> None:
        # As the SDK's DxbcShaderTranslator, a cexec_pred clause tests p0 once
        # on entry and skips its whole body: p0 = true; cexec_pred !p0
        # { setp_clr; set_lod 4 } must not clear p0 for a later !p0 setter.
        flow = [_exec(2, 2, 0b0100), _exec(4, 2, 0b0100, opcode=5)]
        flow += [_exec(6, 1, 0b01), _exec(7, 1, 0b01, opcode=2)]
        later = SUCCESSIVE_WORDS[9:12]
        later[1] |= 1 << 31
        instructions = [SCALAR_SETP, PIXEL_WORDS[3:6], SETP_CLR]
        instructions += [SUCCESSIVE_WORDS[9:12], later, PIXEL_WORDS[6:9]]
        words = _pack_control_flow(flow) + [w for i in instructions for w in i]
        result, output = self._run("ps", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        body = output[output.index("registerLod = r0.x;") :]
        self.assertRegex(
            body,
            r"^\s*registerLod = r0\.x;\s*if \(!p0\)\s*\{\s*p0 = [^}]*"
            r"registerLod = r1\.y;\s*\}\s*if \(!p0\)\s*\{\s*registerLod = r1\.y;\s*\}",
        )

    def test_skipped_predicated_clause_writes_no_registers(self) -> None:
        # r1 = ...; p0 = setp; cexec_pred p0 { r1 = dot }; set_lod r1.y: the
        # setter must read the dot result only where the clause ran.
        flow = [_exec(2), _exec(3, opcode=5) | (1 << 42)]
        flow += [_exec(4, 2, 0b0101, opcode=2), 0]
        instructions = [SCALAR_SETP, VECTOR_DOT, SUCCESSIVE_WORDS[9:12]]
        instructions.append(PIXEL_WORDS[6:9])
        words = _pack_control_flow(flow) + [w for i in instructions for w in i]
        result, output = self._run("ps", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            output,
            r"if \(p0\)\s*\{\s*r1\.xyzw = dot\([^;]*;\s*\}\s*registerLod = r1\.y;",
        )

    def test_conditional_end_returns_only_when_its_clause_runs(self) -> None:
        # As the SDK's ProcessExecInstructionEnd, a cexec_pred_end clause
        # breaks out only inside its gate: set_lod 0; cexec_pred_end p0
        # { @p0 set_lod 4 }; fetch must still reach the fetch.
        setter = SUCCESSIVE_WORDS[9:12]
        setter[1] |= 1 << 31
        setter[2] |= 1 << 31
        flow = [_exec(2, 2, 0b0100), _exec(4, 1, 0b01, opcode=6) | (1 << 42)]
        flow += [_exec(5, 1, 0b01, opcode=2), 0]
        instructions = [SCALAR_SETP, PIXEL_WORDS[3:6], setter, PIXEL_WORDS[6:9]]
        words = _pack_control_flow(flow) + [w for i in instructions for w in i]
        result, output = self._run("ps", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            output,
            r"if \(p0\)\s*\{\s*if \(p0\)\s*\{\s*registerLod = r1\.y;\s*\}"
            r"[^}]*clip\([^}]*\}\s*return;\s*\}\s*r0\.x = tfetch2DRegisterLod\(",
        )

    def test_shader_ending_in_a_conditional_end_keeps_its_epilogue(self) -> None:
        # The SDK completes the shader after its main loop, so the alpha test
        # still runs where a final cexece or cexec_pred_end clause is skipped.
        for name, end in (
            ("cexec_pred_end", _exec(4, opcode=6) | (1 << 42)),
            ("cexece", _exec(4, opcode=4)),
        ):
            flow = [_exec(1, 3, 0b010100), end]
            instructions = [SCALAR_SETP, PIXEL_WORDS[3:6], PIXEL_WORDS[6:9]]
            instructions.append(VECTOR_DOT)
            words = _pack_control_flow(flow) + [w for i in instructions for w in i]
            with self.subTest(end=name):
                result, output = self._run("ps", words)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(output.count("clip(oC0.w - g_AlphaThreshold);"), 2)
                tail = output[output.rindex("return;") :]
                self.assertRegex(tail, r"return;\s*\}\s*\[branch\][^}]*clip\(")

    def test_unproven_diamonds_are_rejected(self) -> None:
        for name, words in (
            ("same-condition jumps", _diamond_words(False, same_condition=True)),
            ("an outside entry", _diamond_words(False, outside_entry=True)),
        ):
            with self.subTest(case=name):
                result, _ = self._run("ps", words)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "unsupported pixel register LOD: unstructured control flow",
                    result.stderr,
                )

    def test_computed_register_lod_refuses_base_mips_and_nested_fetches(self) -> None:
        words = PIXEL_WORDS.copy()
        words[7] = (words[7] | (1 << 28)) & ~(3 << 16) | (2 << 16)
        result, _ = self._run("ps", words)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported pixel register LOD: computed mode", result.stderr)
        # A setter dominates a fetch inside a predicated jump's branch; only
        # the computed form refuses there.
        flow = [_exec(2, 2, 0b0100), _jump(3, 0), _exec(4, 1, 0b01)]
        flow += [_exec(5, opcode=2)]
        for computed in (False, True):
            fetch = PIXEL_WORDS[6:9]
            fetch[1] |= int(computed) << 28
            instructions = [SCALAR_SETP, PIXEL_WORDS[3:6], fetch, VECTOR_DOT]
            nested = _pack_control_flow(flow) + [w for i in instructions for w in i]
            with self.subTest(computed=computed):
                result, output = self._run("ps", nested)
                if computed:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        "unsupported pixel register LOD: computed mode", result.stderr
                    )
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("tfetch2DRegisterLod(", output)

    def test_diamond_without_register_lod_keeps_upstream_flow(self) -> None:
        # Previously admitted shaders must translate byte for byte as before.
        result, output = self._run(
            "ps", _diamond_words(writer_in_then=False, register_lod=False)
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("switch (pc)", output)
        self.assertNotIn("registerLod", output.split("CubeMapData)0;")[-1])

    def test_loops_read_their_constants_at_draw_time(self) -> None:
        # The SDK's DxbcShaderTranslator reads each loop constant from its
        # bool/loop buffer, so the snapshot's loop words never reach the HLSL.
        outputs = []
        for word in (0, 0x00FF0302):
            registers = [0] * 0x5003
            registers[0x4908 + 16] = word
            result, output = self._run("ps", _loop_words(), registers)
            self.assertEqual(result.returncode, 0, result.stderr)
            outputs.append(output)
        self.assertEqual(outputs[0], outputs[1])
        output = outputs[0]
        self.assertIn("uint4 g_LoopConstantsArr[8] : packoffset(c21);", output)
        self.assertIn(
            "#define g_LoopConstants(i) (g_LoopConstantsArr[(i) / 4][(i) % 4])", output
        )
        self.assertIn("int aL0 = aL;", output)
        self.assertIn("uint loopConstant0 = g_LoopConstants(16);", output)
        self.assertIn("aL = int((loopConstant0 >> 8) & 0xFFu);", output)
        self.assertIn(
            "[loop] for (uint loopCount0 = loopConstant0 & 0xFFu; loopCount0 != 0u; "
            "--loopCount0, aL += asint(loopConstant0 << 8) >> 24)",
            output,
        )
        self.assertIn("aL = aL0;", output)
        self.assertIn("c(0 + aL)", output)

    def test_repeat_loops_keep_the_outer_counter(self) -> None:
        result, output = self._run("ps", _loop_words(start_extra=1 << 13))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("int aL0 = aL;", output)
        self.assertIn("uint loopConstant0 = g_LoopConstants(16);", output)
        self.assertNotIn("aL = int((loopConstant0", output)
        self.assertIn("aL = aL0;", output)

    def test_loops_with_noncanonical_targets_are_rejected(self) -> None:
        result, _ = self._run("ps", _loop_words(skip_target=5, body_target=2))
        self.assertEqual(result.returncode, 0, result.stderr)
        # A back edge into the body's second clause or a skip short of the
        # loop end would run other clauses than the structured loop.
        for name, words in (
            ("back edge inside the body", _loop_words(body_target=3)),
            ("skip before the loop end", _loop_words(skip_target=4)),
        ):
            with self.subTest(case=name):
                result, _ = self._run("ps", words)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    "unsupported raw loop: noncanonical targets", result.stderr
                )

    def test_loop_free_shaders_declare_no_loop_constants(self) -> None:
        result, output = self._run("ps", PIXEL_WORDS.copy())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("g_LoopConstants", output)

    def test_nested_loops_restore_the_outer_counter(self) -> None:
        result, output = self._run("ps", _loop_words(nested=True))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(output.index("int aL0 = aL;"), output.index("int aL1 = aL;"))
        self.assertLess(output.index("aL = aL1;"), output.index("aL = aL0;"))
        self.assertIn("uint loopConstant1 = g_LoopConstants(17);", output)

    def test_unproven_loops_and_calls_are_rejected(self) -> None:
        cases = [
            (
                "predicated break",
                _loop_words(end_extra=1 << 21),
                "unsupported raw loop: predicated break",
            ),
            ("call", _loop_words(start_opcode=9), "raw adapter has no verified calls"),
        ]
        for name, words, message in cases:
            with self.subTest(case=name):
                result, _ = self._run("ps", words)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_ordinary_implicit_sampling_stays_on_existing_helper(self) -> None:
        words = PIXEL_WORDS.copy()
        words[2] = (words[2] & ~0xFFF) | 0x4
        words[7] &= ~(1 << 29)
        result, output = self._run("ps", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("r0.x = tfetch2D(", output)
        self.assertNotIn("r0.x = tfetch2DRegisterLod(", output)

    def test_descriptor_lod_bias_snapshot_is_rejected(self) -> None:
        for bias in (1, 0x3FF):
            with self.subTest(bias=bias):
                registers = [0] * 0x5003
                registers[0x4800 + 6 * 0 + 4] = bias << 12
                result, _ = self._run("ps", PIXEL_WORDS.copy(), registers)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("descriptor bias", result.stderr)

    def test_relative_fetch_source_or_destination_is_rejected(self) -> None:
        for relative_bit in (11, 18):
            with self.subTest(relative_bit=relative_bit):
                words = PIXEL_WORDS.copy()
                words[6] |= 1 << relative_bit
                result, _ = self._run("ps", words)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("relative fetch", result.stderr)

    def test_vertex_guarded_route_stays_unchanged(self) -> None:
        words = PIXEL_WORDS.copy()
        words[2] = (words[2] & ~0xFFF) | 0x1
        words[3:9] = [0, 0, 0, 0, 0, 0]
        result, output = self._run("vs", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("in float4 iTexCoord0 : TEXCOORD0", output)
        self.assertIn("sintTexcoord(g_SintTexcoords", output)
        self.assertNotIn(" = tfetch2DRegisterLod(", output)

    def test_vertex_fetch_without_written_components_emits_constants_only(
        self,
    ) -> None:
        # VS 2565EED4's r2 fetch: X and Y keep, Z and W zero, in bits 0..11 of
        # the fetch's second word.
        words = PIXEL_WORDS.copy()
        words[2] = (words[2] & ~0xFFF) | 0x1
        words[3:9] = [0, 0, 0, 0, 0, 0]
        words[4] = 0x93F
        result, output = self._run("vs", words)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("sintTexcoord(g_SintTexcoords", output)
        self.assertRegex(output, r"r\d+\.z = 0\.0;")
        self.assertRegex(output, r"r\d+\.w = 0\.0;")
        self.assertNotRegex(output, r"r\d+\. =")

    def test_unsupported_pixel_register_lod_modes_are_rejected(self) -> None:
        cases = [
            (
                "missing setter",
                lambda words: words.__setitem__(2, (words[2] & ~0xFFF) | 0x4),
                "no preceding dominating setter",
            ),
            (
                "relative setter",
                lambda words: words.__setitem__(3, words[3] | (1 << 11)),
                "relative setter",
            ),
            (
                "only a predicated setter",
                lambda words: words.__setitem__(4, words[4] | (1 << 31)),
                "no preceding dominating setter",
            ),
            (
                "predicated fetch",
                lambda words: words.__setitem__(7, words[7] | (1 << 31)),
                "predicated fetch",
            ),
            (
                "gradient mode",
                lambda words: words.__setitem__(8, words[8] | 1),
                "computed or gradient mode",
            ),
            (
                "lod bias",
                lambda words: words.__setitem__(8, words[8] | (1 << 2)),
                "bias or offset",
            ),
            (
                "other dimension",
                lambda words: words.__setitem__(8, (words[8] & ~(3 << 14)) | (2 << 14)),
                "texture dimension",
            ),
            (
                "setter only in a conditional clause",
                lambda words: words.__setitem__(2, (words[2] & 0x0FFFFFFF) | (3 << 28)),
                "no preceding dominating setter",
            ),
            (
                "gradient instruction",
                lambda words: words.__setitem__(3, 25),
                "gradients",
            ),
        ]
        for name, mutate, message in cases:
            with self.subTest(mode=name):
                words = PIXEL_WORDS.copy()
                mutate(words)
                result, _ = self._run("ps", words)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn(message, result.stderr, (name, result.stderr))

    def test_vertex_register_lod_is_rejected(self) -> None:
        words = PIXEL_WORDS.copy()
        words[2] = (words[2] & ~0xFFF) | 0x5
        words[3] = 1
        words[4] |= 1 << 29
        words[6] = 0
        result, _ = self._run("vs", words)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported vertex register LOD", result.stderr)


class CubeMatrixRunnerSchemaTest(unittest.TestCase):
    def setUp(self) -> None:
        sys.dont_write_bytecode = True
        path = TITLE_ROOT / "scripts" / "gpu" / "test-native-prepared-cube-matrix.py"
        spec = importlib.util.spec_from_file_location("cube_matrix_schema", path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)
        self.runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.runner)

    def test_matrix_receipt_requires_a_true_boolean_half_pixel_offset(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cube-matrix-schema-") as directory:
            root = Path(directory)
            common = "schema_version = 2\nform_count = 3\npixel_tolerance = 0\n"
            for field in ("", "half_pixel_offset = false\n", "half_pixel_offset = 1\n"):
                with self.subTest(field=field):
                    (root / "matrix.toml").write_text(common + field, encoding="ascii")
                    with self.assertRaisesRegex(ValueError, "half_pixel_offset"):
                        self.runner.check_results(argparse.Namespace(output=root))
            (root / "matrix.toml").write_text(
                common + "half_pixel_offset = true\n", encoding="ascii"
            )
            with self.assertRaisesRegex(ValueError, "completed_forms"):
                self.runner.check_results(argparse.Namespace(output=root))

    def test_coverage_counts_reject_booleans_and_missing_fields(self) -> None:
        for key, count in (
            ("native_mutation_count", 126),
            ("native_mutation_count", 168),
            ("native_mutation_count", 192),
            ("alpha_invariance_count", 7),
            ("face_perturbation_count", 42),
            ("face_level_omission_count", 42),
            ("face_level_omission_count", 48),
            ("interpolated_parity_cases", 16),
        ):
            with self.subTest(key=key):
                for receipt in ({}, {key: True}, {key: count - 1}):
                    with self.assertRaises(ValueError):
                        self.runner.expect_fields(
                            receipt, {key: count}, "cube controls"
                        )

    def test_channel_mutation_must_change_the_selected_panel(self) -> None:
        baseline = bytes(self.runner.PLANE_BYTES)
        mutation = bytearray(baseline)
        mutation[(1280 * 64 + 128) * 4] = 23
        self.assertTrue(self.runner.changed_panel(mutation, baseline, 2, 1))
        self.assertFalse(self.runner.changed_panel(mutation, baseline, 1, 1))
        self.assertFalse(self.runner.changed_panel(mutation, baseline, 2, 0))

    def test_cube_swizzle_matches_captured_descriptor(self) -> None:
        for form, words in zip(self.runner.FORMS, self.runner.FETCH_WORDS, strict=True):
            self.assertEqual(form[4], (words[3] >> 1) & 0xFFF)
        self.assertEqual(
            [(self.runner.FORMS[0][4] >> shift) & 7 for shift in (0, 3, 6, 9)],
            [2, 1, 0, 5],
        )

    def test_alpha_control_requires_guest_changes_and_unchanged_outputs(self) -> None:
        with (
            tempfile.TemporaryDirectory(prefix="cube-alpha-schema-") as directory,
            mock.patch.object(self.runner, "PLANE_BYTES", 4),
        ):
            root = Path(directory)
            (root / "original-6.toml").write_text(
                "schema_version = 2\nhalf_pixel_offset = true\nlevel = 6\nfaces = 6\n"
                "channels = 4\ndata_channels = 3\nupdate_kind = 2\nprepared_draws = 48\n"
                "cpu_partial_overlap_writes = true\nsource_raw_bytes_frozen = true\n"
                "cpu_update_overlap_written = true\ncpu_update_outside_unchanged = true\n"
                'failure = ""\ncollector_error = ""\n',
                encoding="ascii",
            )
            base = bytes(98304)
            before = bytes(94080)
            after = bytearray(before)
            for face in range(6):
                after[50176 + face * 8192] = 255
            for phase in ("old", "new"):
                (root / f"original-6-{phase}-base.bin").write_bytes(base)
                (root / f"original-6-{phase}-mips.bin").write_bytes(
                    before if phase == "old" else after
                )
            for phase in ("baseline-", ""):
                for kind in ("expected", "terminal"):
                    (root / f"original-6-{phase}{kind}.rgba8.bin").write_bytes(bytes(4))
            for route in ("candidate", "control"):
                for phase in ("baseline", "updated"):
                    (root / f"{route}-{phase}.bin").write_bytes(bytes(8))
            self.runner.check_alpha_invariance(root, 6)
            for name, contents, error in (
                ("original-6-new-mips.bin", before, "exactly the selected guest alpha"),
                (
                    "original-6-new-mips.bin",
                    after[:50177] + bytes([1]) + after[50178:],
                    "exactly the selected guest alpha",
                ),
                (
                    "original-6-terminal.rgba8.bin",
                    bytes([1, 0, 0, 0]),
                    "Original output",
                ),
                ("candidate-updated.bin", bytes([1, 0, 0, 0]) * 2, "candidate output"),
                ("control-updated.bin", bytes([1, 0, 0, 0]) * 2, "control output"),
            ):
                with self.subTest(name=name, error=error):
                    path = root / name
                    original = path.read_bytes()
                    path.write_bytes(contents)
                    with self.assertRaisesRegex(ValueError, error):
                        self.runner.check_alpha_invariance(root, 6)
                    path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
