from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path

from generated_scan import chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]

HOOK_CALL = re.compile(r"^\s*ApplyCityFoundingUnit\(")


def scan_generated(paths):
    windows: list[str] = []
    for lines in chunk_lines(paths):
        previous: deque[str] = deque(maxlen=4)
        stream = iter(lines)
        for line in stream:
            if "ApplyCityFoundingUnit(" in line and HOOK_CALL.search(line):
                windows.append("".join([*previous, line, next(stream, "")]))
            previous.append(line)
    return windows


class CityFoundingUnitContractTests(unittest.TestCase):
    def test_success_tail_and_exact_register_inputs(self) -> None:
        config = tomllib.loads(
            (ROOT / "config/city_effect_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertEqual(
            [
                hook
                for hook in config["midasm_hook"]
                if hook["name"] == "ApplyCityFoundingUnit"
            ],
            [
                {
                    "address": 0x82D1A314,
                    "name": "ApplyCityFoundingUnit",
                    "registers": ["r1", "r16", "r20", "r13", "fpscr"],
                }
            ],
        )

    def test_bridge_preserves_continuation_and_has_no_enable_cvar(self) -> None:
        source = (ROOT / "src/city_founding_unit_hooks.cpp").read_text(encoding="utf-8")
        self.assertRegex(
            source, r"REXCVAR_DEFINE_BOOL\(city_founding_unit_trace, false,"
        )
        self.assertEqual(source.count("REXCVAR_DEFINE_"), 1)
        self.assertIn("PPCContext call{};", source)
        self.assertIn("call.r1.u64 = stack_.u32 - 0x70;", source)
        self.assertRegex(source, r"call\.r13\s*= threadPointer_;")
        self.assertIn("HostFloatScope", source)
        self.assertIn("Platform::setcsr(saved)", source)
        self.assertEqual(source.count("sub_82D11750(call,"), 1)
        body = source.split("void ApplyCityFoundingUnit(", 1)[1]
        self.assertNotRegex(
            body,
            r"(?m)^\s*(?:stack|citySlot|owner|threadPointer|fpscr)(?:\.\w+)?\s*=",
        )
        for field in ("gate={}", "creator_result={}", "loyalty_write={}", "no retry"):
            self.assertIn(field, source)

    def test_bundle_appends_kind_and_preserves_atomic_staging(self) -> None:
        host = (ROOT / "src/mod_script_rules.cpp").read_text(encoding="utf-8")
        schema = host.split("const Schema schemas[] = {", 1)[1].split("};", 1)[0]
        self.assertEqual(
            re.findall(r'^\s*\{ "([a-z_]+)", \{', schema, re.M),
            [
                "terrain_yield",
                "unit_combat",
                "unique_unit_scalar",
                "unit_movement",
                "unit_effect",
                "unit_production_cost",
                "era_technology_ability",
                "nation_select_text",
                "building_yield",
                "wonder_completion_technology",
                "city_founding_unit",
            ],
        )
        self.assertIn('{ "city_founding_unit", { "unit", "upgrades" } }', schema)
        bundle = (ROOT / "src/rules_bundle.cpp").read_text(encoding="utf-8")
        self.assertIn("Registry<city_founding_unit::Rule>", bundle)
        stages = bundle.split("std::tuple<Stage<TerrainYieldRule>", 1)[1].split(
            " stages;", 1
        )[0]
        self.assertTrue(stages.endswith("Stage<city_founding_unit::Rule>>"))

    def test_generated_displaced_instruction_remains_once(self) -> None:
        chunks, windows = scan_once("city_founding_unit", scan_generated)
        if not windows:
            manifest = tomllib.loads(
                (ROOT / "rerevved_manifest.toml").read_text(encoding="utf-8")
            )
            registered = (
                "config/city_effect_hooks.toml" in manifest["entrypoint"]["includes"]
            )
            if not chunks or not registered:
                self.skipTest("city founding unit hook has not been generated")
        self.assertEqual(len(windows), 1)
        self.assertIn("sub_82CBE528(ctx, base);", windows[0])
        self.assertIn("// mr r3,r16", windows[0])
        self.assertIn(
            "ApplyCityFoundingUnit(ctx.r1, ctx.r16, ctx.r20, ctx.r13, ctx.fpscr);\n"
            "\tctx.r3.u64 = ctx.r16.u64;",
            windows[0],
        )


if __name__ == "__main__":
    unittest.main()
