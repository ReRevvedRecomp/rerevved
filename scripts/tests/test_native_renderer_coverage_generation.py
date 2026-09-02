from __future__ import annotations

import copy
import hashlib
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATOR_PATH = ROOT / "scripts" / "gen-native-renderer-coverage.py"
INPUT = ROOT / "config" / "native_renderer_fixture_0001.toml"
HOOK_OUTPUT = ROOT / "config" / "native_renderer_coverage_hooks.toml"
INCLUDE_OUTPUT = ROOT / "src" / "native_renderer_coverage_hooks.inc"
EXISTING_HOOKS = ROOT / "config" / "rerevved_hooks.toml"


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "native_renderer_coverage_generator", GENERATOR_PATH
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


generator = load_generator()


class NativeRendererCoverageGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data, cls.input_bytes, cls.existing = generator.load_and_validate(
            INPUT, EXISTING_HOOKS
        )
        cls.input_sha256 = hashlib.sha256(cls.input_bytes).hexdigest()

    def test_happy_path_has_accepted_snapshot_and_generated_contract(self) -> None:
        hooks, include, digest = generator.generate_outputs(
            self.data, self.input_bytes
        )
        self.assertEqual(digest, self.input_sha256)
        self.assertEqual(hooks, HOOK_OUTPUT.read_bytes())
        self.assertEqual(include, INCLUDE_OUTPUT.read_bytes())
        self.assertRegex(include, rb"kObserverSegmentCount\s+= 8;")
        self.assertRegex(include, rb"kObserverByteBudget\s+= 3456;")
        for name, value in (
            (b"kRoleWrapper[]", b'"wrapper"'),
            (b"kRoleLoweringBoundary[]", b'"lowering-boundary"'),
            (b"kValuePhase[]", b'"value"'),
            (b"kSiteFixedSelection[]", b'"site-fixed"'),
            (b"kUnmappedInputSelection[]", b'"unmapped-input"'),
        ):
            matching_lines = [line for line in include.splitlines() if name in line]
            self.assertEqual(len(matching_lines), 1)
            self.assertIn(value, matching_lines[0])
        self.assertRegex(include, rb"kPrimitiveDomainId\[\]\s+= \"primitive-4\";")
        self.assertRegex(include, rb"kUnknownDomainId\[\]\s+= \"unknown\";")
        self.assertRegex(include, rb"kContractId\[\]\s+= \"NRD-CONTRACT-0001\";")
        self.assertRegex(include, rb"kCounterRowsPerSegment\s+= 4;")
        self.assertRegex(include, rb"kSegmentCounterRowCount\s+= 32;")
        self.assertIn(b"namespace rerevved::native_renderer::generated", include)
        self.assertIn(b"ReRevvedNativeRendererCoverageSite82303E3C", include)
        self.assertIn(b"ReRevvedNativeRendererCoverageSite82303E8C", include)
        self.assertIn(
            b"rerevved::native_renderer::RecordSiteFixedValue(site_index, 4);",
            include,
        )

    def test_check_passes_and_does_not_write(self) -> None:
        before = {path: path.read_bytes() for path in (HOOK_OUTPUT, INCLUDE_OUTPUT)}
        completed = subprocess.run(
            [sys.executable, str(GENERATOR_PATH), "--check"],
            cwd=ROOT,
            check=False,
            capture_output=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode())
        self.assertIn(b"checked", completed.stdout)
        self.assertEqual(before, {path: path.read_bytes() for path in before})

    def test_check_rejects_drift_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            hooks = directory / "hooks.toml"
            include = directory / "hooks.inc"
            hooks.write_bytes(HOOK_OUTPUT.read_bytes() + b"drift\n")
            include.write_bytes(INCLUDE_OUTPUT.read_bytes())
            completed = subprocess.run(
                [
                    sys.executable,
                    str(GENERATOR_PATH),
                    "--check",
                    "--hooks-output",
                    str(hooks),
                    "--include-output",
                    str(include),
                ],
                cwd=ROOT,
                check=False,
                capture_output=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn(b"drift", completed.stderr)
            self.assertTrue(hooks.read_bytes().endswith(b"drift\n"))

    def test_duplicate_hook_addresses_fail(self) -> None:
        data = copy.deepcopy(self.data)
        operation = data["snapshot"]["operations"][0]
        operation["hook_sites"][1]["address"] = operation["hook_sites"][0]["address"]
        with self.assertRaises(generator.ValidationError):
            generator._validate_snapshot(data)

    def test_existing_hook_collision_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            existing = Path(directory) / "rerevved_hooks.toml"
            existing.write_text(
                '[[midasm_hook]]\naddress = 0x82303E3C\nname = "already-there"\n',
                encoding="ascii",
            )
            with self.assertRaises(generator.ValidationError):
                generator.load_and_validate(INPUT, existing)

    def test_invalid_join_image_pointer_and_partial_surface_fail(self) -> None:
        mutations = []
        for key, value in (
            ("runtime_join_key", "d3d:0x826A3560"),
            ("image_sha256", "0" * 64),
            ("surface", "complete"),
        ):
            candidate = copy.deepcopy(self.data)
            if key == "runtime_join_key":
                candidate["snapshot"]["operations"][0][key] = value
            else:
                candidate["snapshot"][key] = value
            mutations.append(candidate)
        pointer = copy.deepcopy(self.data)
        pointer["snapshot"]["source_pointers"]["surface"] = "#/snapshot/surface"
        mutations.append(pointer)
        for candidate in mutations:
            with self.subTest(candidate=candidate):
                with self.assertRaises(generator.ValidationError):
                    generator._validate_snapshot(candidate)

    def test_null_domain_requires_absent_value_and_explicit_kind(self) -> None:
        missing_kind = copy.deepcopy(self.data)
        del missing_kind["snapshot"]["operations"][0]["value_domains"][1][
            "value_kind"
        ]
        with self.assertRaises(generator.ValidationError):
            generator._validate_snapshot(missing_kind)

        typed_null = copy.deepcopy(self.data)
        typed_null["snapshot"]["operations"][0]["value_domains"][1]["value"] = None
        with self.assertRaises(generator.ValidationError):
            generator._validate_snapshot(typed_null)

    def test_reversed_input_hooks_are_emitted_in_numeric_order(self) -> None:
        data = copy.deepcopy(self.data)
        data["snapshot"]["operations"][0]["hook_sites"].reverse()
        hooks, include, _ = generator.generate_outputs(data, self.input_bytes)
        self.assertEqual(hooks, HOOK_OUTPUT.read_bytes())
        self.assertLess(
            include.index(b"kSiteIndex82303E3C"),
            include.index(b"kSiteIndex82303E8C"),
        )

    def test_generation_is_byte_deterministic(self) -> None:
        first = generator.generate_outputs(self.data, self.input_bytes)
        second = generator.generate_outputs(
            copy.deepcopy(self.data), bytes(self.input_bytes)
        )
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
