from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/silent_player_hooks.toml"
HOOKS = ROOT / "src/silent_player_hooks.cpp"

EXPECTED_HOOKS = (
    (0x82A11D04, "NotePlayerHeard", "r11"),
    (0x82A148E8, "CheckSilentPlayer", "r3"),
)
# Function, the instruction at the hook, and the instruction that follows it.
SITES = {
    "NotePlayerHeard": (
        0x82A11B00,
        "lwz r11,12(r11)",
        "ctx.r11.u64 = REX_LOAD_U32(ctx.r11.u32 + 12);",
    ),
    "CheckSilentPlayer": (0x82A148E8, "mflr r12", "ctx.r12.u64 = ctx.lr;"),
}


class SilentPlayerContractTests(unittest.TestCase):
    def test_hooks_match_the_config(self):
        hooks = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["midasm_hook"]
        self.assertEqual(
            hooks,
            [
                {"address": address, "name": name, "registers": registers.split()}
                for address, name, registers in EXPECTED_HOOKS
            ],
        )
        source = HOOKS.read_text(encoding="utf-8")
        for _, name, _ in EXPECTED_HOOKS:
            self.assertEqual(
                len(re.findall(r"^void %s\(PPCRegister& \w+\)$" % name, source, re.M)),
                1,
            )

    def test_only_the_timeout_closes_and_hooks_are_registered(self):
        source = HOOKS.read_text(encoding="utf-8")
        self.assertNotIn("store_and_swap", source)
        # The drop repeats the hard close sub_82A14BF8 without its free.
        steps = [
            source.index("__imp__sub_%s(call, base)" % a)
            for a in ("82A14C78", "82A14218", "82A15268")
        ]
        self.assertEqual(steps, sorted(steps))
        self.assertEqual(source.count("REX_HOOK_RAW(sub_82A148E8)"), 1)
        self.assertIn("__imp__sub_82A148E8(ctx, base);", source)
        manifest = (ROOT / "rerevved_manifest.toml").read_text(encoding="utf-8")
        self.assertEqual(manifest.count('"config/silent_player_hooks.toml"'), 1)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(cmake.count("src/silent_player_hooks.cpp"), 1)
        tests = (ROOT / "tests/rerevved_tests.cmake").read_text(encoding="utf-8")
        self.assertEqual(tests.count("tests/silent_player_test.cpp"), 1)

    def test_generated_sites(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        if "NotePlayerHeard" not in text:
            self.skipTest("silent player hooks require owner codegen")
        for _, name, registers in EXPECTED_HOOKS:
            address, instruction, following = SITES[name]
            function = text.split("DEFINE_REX_FUNC(sub_%08X)" % address, 1)[1].split(
                "DEFINE_REX_FUNC", 1
            )[0]
            call = "%s(%s);" % (name, ", ".join("ctx." + r for r in registers.split()))
            self.assertIn(
                "\t// %s\n\t%s\n\t%s\n" % (instruction, call, following), function
            )
            self.assertEqual(text.count("\t" + call), 1, name)


if __name__ == "__main__":
    unittest.main()
