from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/pick_loop_trace_hooks.toml"
HOOKS = ROOT / "src/pick_loop_trace_hooks.cpp"

EXPECTED_HOOKS = (
    (0x82D25CAC, "TracePickLoopGate", "", "lwz r31,-6172(r21)"),
    (0x82D25CEC, "TracePickLoopDraw", "", "mr r3,r30"),
    (0x82D25D24, "TracePickLoopPick", "r11 r31 r29", "lis r10,-31986"),
)


class PickLoopTraceContractTests(unittest.TestCase):
    def test_hooks_match_the_config(self):
        hooks = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["midasm_hook"]
        expected = []
        for address, name, registers, _ in EXPECTED_HOOKS:
            row = {"address": address, "name": name}
            if registers:
                row["registers"] = registers.split()
            expected.append(row)
        self.assertEqual(hooks, expected)
        source = HOOKS.read_text(encoding="utf-8")
        for _, name, registers, _ in EXPECTED_HOOKS:
            parameters = re.search(r"^void %s\((.*?)\)" % name, source, re.M)[1]
            self.assertEqual(parameters.count("PPCRegister&"), len(registers.split()))

    def test_trace_is_default_off_and_read_only(self):
        source = HOOKS.read_text(encoding="utf-8")
        self.assertIn("REXCVAR_DEFINE_BOOL(pick_loop_trace, false,", source)
        self.assertNotRegex(source, r"\.(u32|s32|u64|s64)\s*=[^=]")
        manifest = (ROOT / "rerevved_manifest.toml").read_text(encoding="utf-8")
        self.assertIn('"config/pick_loop_trace_hooks.toml"', manifest)
        self.assertIn(
            "src/pick_loop_trace_hooks.cpp",
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
        )

    def test_generated_sites(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        if "TracePickLoopGate" not in text:
            self.skipTest("pick loop trace hooks require owner codegen")
        for _, name, registers, following in EXPECTED_HOOKS:
            arguments = ", ".join(f"ctx.{register}" for register in registers.split())
            call = f"{name}({arguments});"
            site = re.compile(
                r"\t// %s\n\t%s\n\t(?!//)\S" % (re.escape(following), re.escape(call))
            )
            self.assertEqual(len(site.findall(text)), 1, name)


if __name__ == "__main__":
    unittest.main()
