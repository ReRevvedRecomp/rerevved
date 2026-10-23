from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from cmake_sources import rerevved_tests_sources

ROOT = Path(__file__).resolve().parents[2]


def split_developer_cmake(text: str) -> tuple[str, str]:
    """CMake text outside and inside its if(REREVVED_DEVELOPER) blocks."""
    shipping: list[str] = []
    developer: list[str] = []
    depth = 0
    for line in text.splitlines():
        stripped = line.strip()
        if depth:
            developer.append(line)
            if stripped.startswith("if("):
                depth += 1
            elif stripped.startswith("endif("):
                depth -= 1
        elif stripped == "if(REREVVED_DEVELOPER)":
            depth = 1
            developer.append(line)
        else:
            shipping.append(line)
    return "\n".join(shipping), "\n".join(developer)


SOURCE = ROOT / "src/gpu/guest_d3d/tiling_restore_consume.cpp"
HOOKS = ROOT / "src/gpu/guest_d3d/ring_stub_hooks.cpp"


class TilingRestoreContractTests(unittest.TestCase):
    def test_single_raw_owner_and_original(self) -> None:
        owners = [
            path
            for path in (ROOT / "src").rglob("*.cpp")
            if re.search(r"REX_HOOK_RAW\(sub_8269B118\)", path.read_text())
        ]
        self.assertEqual(owners, [HOOKS])
        source = HOOKS.read_text()
        self.assertIn("REX_EXTERN(__imp__sub_8269B118);", source)
        self.assertIn("REX_EXTERN(sub_8269B118);", source)
        self.assertIn("callGuest<sub_8269B118>(ctx, base, 0x826AC2A0, g);", source)
        wrapper = source.split("REX_HOOK_RAW(sub_8269B118)", 1)[1]
        wrapper = wrapper.split("REX_HOOK_RAW", 1)[0]
        # Only a developer build can replace or check B118; every other build
        # forwards to the Original once.
        developer, tail = wrapper.split("#endif", 1)
        self.assertIn("#if REREVVED_DEVELOPER", developer)
        self.assertIn("if (guest::TilingRestoreReportingRequested())", developer)
        self.assertEqual(tail.count("__imp__sub_8269B118(ctx, base);"), 1)
        self.assertNotIn("TilingRestore", tail)
        self.assertNotIn("RingStubLease", wrapper)
        enabled = developer.split("guest::NoteTilingRestoreEnter();", 1)[1]
        self.assertEqual(enabled.count("__imp__sub_8269B118(ctx, base);"), 1)
        self.assertLess(
            enabled.index("ConsumeTilingRestore("),
            enabled.index("TilingRestoreExitScope"),
        )
        self.assertRegex(
            enabled,
            r"NoteTilingRestoreOriginal\(\);\s*__imp__sub_8269B118\(ctx, base\);",
        )
        for table in (
            "src/gpu/guest_d3d/route_entries.inc",
            "src/gpu/guest_d3d/guest_api_entries.inc",
        ):
            self.assertNotRegex((ROOT / table).read_text(), r"ENTRY\(8269B118,")
        manifest = tomllib.loads((ROOT / "rerevved_manifest.toml").read_text())
        for include in manifest["entrypoint"]["includes"]:
            config = tomllib.loads((ROOT / include).read_text())
            self.assertNotIn(
                0x8269B118,
                [hook["address"] for hook in config.get("midasm_hook", [])],
            )

    def test_defaults_publication_and_diagnostics(self) -> None:
        source = SOURCE.read_text()
        for flag in (
            "native_consume_tiling_restore",
            "native_tiling_restore_exit_check",
        ):
            self.assertRegex(source, rf"REXCVAR_DEFINE_BOOL\({flag},\s*false,")
        self.assertEqual(source.count("Lifecycle::kInitOnly"), 2)
        self.assertIn("const RingStubLease lease;", source)
        self.assertIn("lease && RingStubsReady(ctx.r3.u32)", source)
        self.assertIn("HeapType::kGuestPhysical", source)
        self.assertIn("HeapType::kGuestVirtual", source)
        self.assertIn("CopyGuestGuarded(bytes.data()", source)
        self.assertIn("tr_replay_fault: B118 commit failed; Original forbidden", source)
        wrapper = HOOKS.read_text().split("REX_HOOK_RAW(sub_8269B118)", 1)[1]
        self.assertIn("DeveloperRecorderActive()", wrapper)
        for forbidden in (
            "CountRingStub(",
            "ObserveCompletion",
            "PublishedCompletionProbe",
            "RecordDirectSubmit",
            "RecordDeferredKick",
        ):
            self.assertNotIn(forbidden, source)

    def test_wiring_and_reporting(self) -> None:
        shipping, developer = split_developer_cmake(
            (ROOT / "CMakeLists.txt").read_text()
        )
        self.assertEqual(
            shipping.count("src/gpu/guest_d3d/tiling_restore_consume.cpp"), 0
        )
        self.assertEqual(
            developer.count("src/gpu/guest_d3d/tiling_restore_consume.cpp"), 1
        )
        test_sources = rerevved_tests_sources()
        for path in (
            "src/gpu/guest_d3d/tiling_restore_consume.cpp",
            "src/gpu/guest_d3d/ring_stub_hooks.cpp",
            "tests/gpu/tiling_restore_test.cpp",
            "tests/gpu/ring_stub_route_test.cpp",
        ):
            self.assertIn(path, test_sources)
        fixture = (ROOT / "tests/gpu/tiling_restore_test.cpp").read_text()
        self.assertRegex(fixture, r'TEST_CASE\("tiling_restore", "\[ctest\]')
        # The fatal replay fault runs as a helper child of rerevved_tests.
        self.assertIn('"tiling-restore.fault-child"', fixture)
        self.assertRegex(fixture, r"REQUIRE\(status == 73\)")
        app = (ROOT / "src/app.cpp").read_text()
        self.assertEqual(app.count("ReportTilingRestore(true);"), 2)
        self.assertIn("phases += gpu::guest_d3d::ReportTilingRestore();", app)

    def test_generated_instruction_inventory(self) -> None:
        for path in (ROOT / "generated/default").glob("rerevved_recomp.*.cpp"):
            source = path.read_text()
            if "DEFINE_REX_FUNC(sub_8269B118)" in source:
                body = source.split("DEFINE_REX_FUNC(sub_8269B118)", 1)[1].split(
                    "\n}", 1
                )[0]
                instructions = re.findall(r"// ([^\r\n]+)", body)
                self.assertEqual(
                    [line.strip() for line in instructions],
                    [
                        "lwz r11,13412(r3)",
                        "lwz r10,13408(r3)",
                        "lwz r9,13416(r3)",
                        "addi r8,r11,-160",
                        "lwz r7,13420(r3)",
                        "lwz r6,13424(r3)",
                        "lwz r5,13428(r3)",
                        "stw r10,48(r3)",
                        "stw r11,52(r3)",
                        "stw r9,14928(r3)",
                        "stw r7,14924(r3)",
                        "stw r6,14916(r3)",
                        "stw r5,14920(r3)",
                        "stw r8,56(r3)",
                        "blr",
                    ],
                )
                return
        self.skipTest("generated B118 body is unavailable")


if __name__ == "__main__":
    unittest.main()
