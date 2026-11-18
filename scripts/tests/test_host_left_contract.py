from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/host_left_hooks.toml"
HOOKS = ROOT / "src/host_left_hooks.cpp"

EXPECTED_HOOKS = (
    (0x82CA9C3C, "NoteHostMigrated", "r31", "mtctr r10", "ctx.lr = 0x82CA9C44;"),
    (0x82CA9C70, "NoteHostLost", "r31", "mtctr r10", "ctx.lr = 0x82CA9C78;"),
)
SCOPE_HOOKS = (
    (0x82A116A8, "EnterSocketError", "mflr r12"),
    (0x82A116FC, "LeaveSocketError", "addi r1,r1,96"),
)


class HostLeftContractTests(unittest.TestCase):
    def test_hooks_match_the_config(self):
        hooks = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["midasm_hook"]
        self.assertEqual(
            hooks,
            [
                {"address": address, "name": name, "registers": registers.split()}
                for address, name, registers, _, _ in EXPECTED_HOOKS
            ]
            + [
                {"address": address, "name": name, "registers": []}
                for address, name, _ in SCOPE_HOOKS
            ],
        )
        source = HOOKS.read_text(encoding="utf-8")
        for _, name, _, _, _ in EXPECTED_HOOKS:
            self.assertEqual(
                len(re.findall(r"^void %s\(PPCRegister& \w+\)$" % name, source, re.M)),
                1,
            )
        for _, name, _ in SCOPE_HOOKS:
            self.assertEqual(len(re.findall(r"^void %s\(\)$" % name, source, re.M)), 1)

    def test_notice_is_read_only_and_registered(self):
        source = HOOKS.read_text(encoding="utf-8")
        self.assertNotRegex(source, r"\.(u32|s32|u64|s64)\s*=[^=]")
        self.assertNotIn("store_and_swap", source)
        manifest = (ROOT / "rerevved_manifest.toml").read_text(encoding="utf-8")
        self.assertEqual(manifest.count('"config/host_left_hooks.toml"'), 1)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(cmake.count("src/host_left_hooks.cpp"), 1)

    def test_generated_sites(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        if "NoteHostMigrated" not in text:
            self.skipTest("host left hooks require owner codegen")
        function = text.split("DEFINE_REX_FUNC(sub_82CA9B60)", 1)[1].split(
            "DEFINE_REX_FUNC", 1
        )[0]
        for _, name, registers, instruction, call_return in EXPECTED_HOOKS:
            call = "%s(%s);" % (name, ", ".join("ctx." + r for r in registers.split()))
            site = function.split("\t// %s\n\t%s\n" % (instruction, call), 1)
            self.assertEqual(len(site), 2, name)
            self.assertIn(call_return, site[1][:160])
            self.assertEqual(text.count("\t" + call), 1, name)
        handler = text.split("DEFINE_REX_FUNC(sub_82A116A8)", 1)[1].split(
            "DEFINE_REX_FUNC", 1
        )[0]
        for _, name, instruction in SCOPE_HOOKS:
            self.assertIn("\t// %s\n\t%s();\n" % (instruction, name), handler)
            self.assertEqual(text.count("\t%s();" % name), 1, name)


if __name__ == "__main__":
    unittest.main()
