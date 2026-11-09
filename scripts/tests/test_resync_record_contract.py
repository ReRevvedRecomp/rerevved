from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/resync_record_hooks.toml"
HOOKS = ROOT / "src/resync_record_hooks.cpp"
RECORD = ROOT / "src/resync_record.cpp"
RECORD_HEADER = ROOT / "src/resync_record.h"

EXPECTED_HOOKS = (
    (0x82CDEC68, "ObserveResyncExchangeScalar", "r31 r3", "cmpw cr6,r31,r3"),
    (0x82CDEC94, "ObserveResyncExchangeSeed", "r15 r11", "cmplw cr6,r15,r11"),
    (0x82CDECB4, "ReportResyncExchange", "", "lis r11,-32000"),
    (0x82D90088, "ObserveResyncSnapshotQueued", "r1 r3 r31", "li r5,2"),
)

WRAPPERS = {
    "sub_82CD4CF0": "after",
    "sub_82D91BD0": "before",
    "sub_82D895B8": "before",
}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def wrapper_body(source: str, name: str) -> str:
    match = re.search(r"REX_HOOK_RAW\(%s\)\s*\{(.*?)\n\}" % name, source, re.S)
    assert match, name
    return match[1]


class ResyncRecordContractTests(unittest.TestCase):
    def test_midasm_hooks_are_read_only_observers(self):
        hooks = tomllib.loads(read(CONFIG))["midasm_hook"]
        expected = []
        for address, name, registers, _ in EXPECTED_HOOKS:
            row = {"address": address, "name": name}
            if registers:
                row["registers"] = registers.split()
            expected.append(row)
        self.assertEqual(hooks, expected)
        source = read(HOOKS)
        for _, name, registers, _ in EXPECTED_HOOKS:
            self.assertEqual(len(re.findall(r"^void %s\(" % name, source, re.M)), 1)
            parameters = re.search(r"^void %s\((.*?)\)" % name, source, re.M)[1]
            self.assertEqual(
                parameters.count("PPCRegister&"), len(registers.split()), name
            )

    def test_hooks_stay_clear_of_the_capital_grace_guard(self):
        grace_source = read(ROOT / "src/capital_grace_hooks.cpp")
        guarded = {
            int(address, 16)
            for address in re.findall(r"\{ (0x[0-9A-F]{8}), 0x", grace_source)
        }
        self.assertIn(0x82CDDF34, guarded)
        grace_hooks = tomllib.loads(read(ROOT / "config/capital_grace_hooks.toml"))
        taken = {row["address"] for row in grace_hooks["midasm_hook"]}
        ours = {row[0] for row in EXPECTED_HOOKS}
        self.assertFalse(ours & (guarded | taken))

    def test_wrappers_call_the_original_once_and_in_order(self):
        source = read(HOOKS)
        for name, when in WRAPPERS.items():
            body = wrapper_body(source, name)
            self.assertEqual(body.count(f"__imp__{name}(ctx, base);"), 1, name)
            self.assertEqual(source.count(f"REX_EXTERN(__imp__{name});"), 1, name)
            original = body.index(f"__imp__{name}")
            logger = re.search(r"\blog(Ledger|Target|Callback)\(", body).start()
            self.assertEqual(original < logger, when == "after", name)
            self.assertNotIn("ctx.r3 =", body)
            self.assertNotIn("ctx.r3.", body.replace("ctx.r3.u32", ""))

    def test_lines_are_default_on_info_and_network_only(self):
        source = read(HOOKS)
        self.assertNotIn("REXCVAR", source)
        self.assertNotRegex(source, r"REXLOG_(WARN|ERROR|DEBUG|TRACE)")
        lines = re.findall(r'REXLOG_INFO\("(Resync [a-z ]+):', source)
        self.assertEqual(
            sorted(set(lines)),
            [
                "Resync cities",
                "Resync exchange",
                "Resync ledger",
                "Resync snapshot callback",
                "Resync snapshot queued",
                "Resync target",
            ],
        )
        self.assertGreaterEqual(source.count("NetworkGameActive()"), 5)
        self.assertGreaterEqual(source.count("catch (...)"), 7)
        for forbidden in ("sub_82CD4CF0(", "xuid", "address="):
            self.assertNotIn(forbidden, source.replace("__imp__sub_82CD4CF0(", ""))

    def test_ledger_limits(self):
        header = read(RECORD_HEADER)
        self.assertRegex(header, r"kCapacity = 32;")
        self.assertRegex(header, r"kMaxPlayers = 32;")
        source = read(HOOKS)
        self.assertIn("dedupe().Admit(turn, caller, scalar)", source)
        self.assertLess(
            source.index("dedupe().Admit(turn, caller, scalar)"),
            source.index("record::ComputeLedger"),
        )
        self.assertIn("0x8312B8DC", source)

    def test_load_line_carries_digest_and_pending_count(self):
        source = read(ROOT / "src/ai_civilizations_hooks.cpp")
        function = source.split("void logLoadCase(", 1)[1].split("\n}\n", 1)[0]
        for fragment in ("overrides_digest=0x{:08X}", "apollo_pending_before_reset={}"):
            self.assertEqual(function.count(fragment), 3, fragment)
        hook = source.split("void ResetAiCivilizationsOnLoad()", 1)[1]
        self.assertLess(
            hook.index("apolloPendingAtLoad.store(ApolloPendingCount())"),
            hook.index("session::ResetOnLoad("),
        )
        self.assertLess(
            hook.index("apolloPendingAtLoad.store(ApolloPendingCount())"),
            hook.index("ResetApolloDiscovery()"),
        )

    def test_files_are_registered_and_ascii(self):
        manifest = read(ROOT / "rerevved_manifest.toml")
        self.assertIn('"config/resync_record_hooks.toml"', manifest)
        cmake = read(ROOT / "CMakeLists.txt")
        tests = read(ROOT / "tests/rerevved_tests.cmake")
        for name in ("resync_record.cpp", "resync_record.h", "resync_record_hooks.cpp"):
            self.assertIn(f"src/{name}", cmake)
        self.assertIn("src/resync_record.cpp", tests)
        self.assertIn("tests/resync_record_test.cpp", tests)
        self.assertNotIn("resync_record_hooks.cpp", tests)
        for path in (
            CONFIG,
            HOOKS,
            RECORD,
            RECORD_HEADER,
            ROOT / "tests/resync_record_test.cpp",
            Path(__file__),
        ):
            read(path).encode("ascii")

    def test_generated_hook_sites(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        if "ObserveResyncExchangeScalar" not in text:
            self.skipTest("resync record hooks require owner codegen")
        for _, name, registers, following in EXPECTED_HOOKS:
            arguments = ", ".join(f"ctx.{register}" for register in registers.split())
            call = f"{name}({arguments});"
            site = re.compile(
                r"\t// %s\n\t%s\n\t(?!//)\S" % (re.escape(following), re.escape(call))
            )
            self.assertEqual(len(site.findall(text)), 1, name)


if __name__ == "__main__":
    unittest.main()
