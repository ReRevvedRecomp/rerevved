from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path

from generated_scan import WindowCollector, chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/raging_barbarians_hooks.toml"
SOURCE = ROOT / "src/raging_barbarians_hooks.cpp"

NAMES = (
    "ApplyRagingBarbariansFecundity",
    "ObserveRagingBarbariansCredit",
    "ObserveRagingBarbariansCreation",
    "ApplyRagingBarbariansAggression",
    "ObserveRagingBarbariansDeparture",
)
HOOK_CALLS = {name: re.compile(rf"^\s*{name}\(ctx\.") for name in NAMES}


def scan_generated(paths):
    counts = dict.fromkeys(NAMES, 0)
    collector = WindowCollector()
    for lines in chunk_lines(paths):
        previous: deque[str] = deque(maxlen=40)
        for line in lines:
            collector.advance(line)
            if "RagingBarbarians" in line:
                for name, call in HOOK_CALLS.items():
                    if call.search(line):
                        counts[name] += 1
                        collector.start(name, [*previous, line], 40)
            previous.append(line)
        collector.end_chunk()
    return counts, collector.windows


class RagingBarbariansContractTests(unittest.TestCase):
    def test_native_hook_boundaries(self):
        hooks = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["midasm_hook"]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x82D153E4,
                    "name": "ApplyRagingBarbariansFecundity",
                    "registers": ["r10", "r31", "r24"],
                },
                {
                    "address": 0x82D1544C,
                    "name": "ObserveRagingBarbariansCredit",
                    "registers": ["r30", "r11", "r31", "r24"],
                },
                {
                    "address": 0x82D15AB4,
                    "name": "ObserveRagingBarbariansCreation",
                    "registers": ["r3", "r26", "r25", "r31", "r1"],
                },
                {
                    "address": 0x82CB3F44,
                    "name": "ApplyRagingBarbariansAggression",
                    "registers": ["r11", "r3"],
                },
                {
                    "address": 0x82CC1308,
                    "name": "ObserveRagingBarbariansDeparture",
                    "registers": ["r9", "r3", "r20", "r26", "r10"],
                },
            ],
        )

    def test_consumer_only_write_and_default_off_trace(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertRegex(
            source, r"REXCVAR_DEFINE_BOOL\(raging_barbarians_trace, false,"
        )
        self.assertNotIn("REX_STORE_", source)
        self.assertNotIn("TranslateVirtual<uint8_t*>", source)
        self.assertNotIn("0x830E8FD0", source)
        self.assertEqual(
            re.findall(r"\b(\w+)\.s64\s*=", source), ["fecundity", "aggression"]
        )
        for counter in (
            "override_hits={}",
            "creations={}",
            "failures={}",
            "aggression_override_hits={}",
            "weighted_defenders={}",
            "R={}",
            "threshold_hold={}",
            "retain_selected={}",
        ):
            self.assertIn(counter, source)
        self.assertIn("IsCreationResultValid(owner.s32, type.s32, result.s32)", source)

    def test_title_setting_and_freeze_ingress(self):
        settings = (ROOT / "src/raging_barbarians_settings.cpp").read_text(
            encoding="utf-8"
        )
        overlay = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")
        self.assertIn(
            'REXCVAR_DEFINE_BOOL(raging_barbarians, false, "ReRevved",', settings
        )
        self.assertEqual(settings.count("REXCVAR_GET(raging_barbarians)"), 1)
        freeze, evaluate = settings.split("Evaluation Evaluate(", 1)
        self.assertIn(
            "frozenEnabled.store(!standardRules && REXCVAR_GET(raging_barbarians)",
            freeze,
        )
        self.assertNotIn("REXCVAR_GET", evaluate)
        self.assertIn("enabled ? 2 : nativeValue", evaluate)
        self.assertIn('{ "", { "raging_barbarians" } }', overlay)
        self.assertIn('{ "raging_barbarians", Audience::kPlayer }', overlay)
        self.assertIn('{ "raging_barbarians", "Raging Barbarians" }', overlay)
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        setup = runtime.split("void RunNativeNewGameSetup(", 1)[1].split(
            "} // namespace", 1
        )[0]
        self.assertEqual(
            setup.count("raging_barbarians::FreezeForGame(standardRules);"), 1
        )
        self.assertLess(
            setup.index("raging_barbarians::FreezeForGame(standardRules);"),
            setup.index("__imp__sub_82D25170(context, base)"),
        )
        self.assertIn("REX_HOOK_RAW(sub_82D25170)", runtime)
        hooks = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        load = hooks.split("void ResetAiCivilizationsOnLoad()", 1)[1]
        self.assertEqual(load.count("FreezeRagingBarbariansForGame();"), 1)
        self.assertIn("removedMask.store(0);", load)
        config = tomllib.loads(
            (ROOT / "config/ai_civilizations_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertIn(
            {"address": 0x82D86674, "name": "ResetAiCivilizationsOnLoad"},
            config["midasm_hook"],
        )

    def test_no_rules_bundle_plumbing(self):
        for name in (
            "src/mod_script_rules.cpp",
            "src/rules_bundle.h",
            "src/rules_bundle.cpp",
            "tests/mod_script_rules_test.cpp",
            "docs/modding-api.md",
        ):
            self.assertNotIn(
                "raging_barbarians", (ROOT / name).read_text(encoding="utf-8")
            )

    def test_generated_preservation(self):
        generated, (counts, windows) = scan_once("raging_barbarians", scan_generated)
        if not generated:
            self.skipTest("generated guest code is not present")
        names = NAMES
        for name in names:
            self.assertLessEqual(
                counts[name], 1, f"{name} must occur once across all chunks"
            )
        self.assertEqual(counts, dict.fromkeys(names, 1))
        self.assertEqual(set(windows), set(names))
        fecundity = windows[names[0]]
        self.assertIn(
            "ctx.r10.s64 = ctx.r11.s16;\n\t// lhzx r5,r31,r9\n\t"
            "ApplyRagingBarbariansFecundity(ctx.r10, ctx.r31, ctx.r24);\n\t"
            "ctx.r5.u64 = REX_LOAD_U16(ctx.r31.u32 + ctx.r9.u32);",
            fecundity,
        )
        self.assertIn("ctx.r4.s64 = ctx.r10.s64 + 1;", fecundity)
        credit = windows[names[1]]
        self.assertIn("if (ctx.cr6.eq) goto loc_82D1544C;", credit)
        self.assertIn("ctx.r30.u64 = ctx.r22.u64;", credit)
        creation = windows[names[2]]
        self.assertIn("sub_82D11750(ctx, base);", creation)
        self.assertIn(
            "ObserveRagingBarbariansCreation(ctx.r3, ctx.r26, ctx.r25, ctx.r31, ctx.r1);\n\t"
            "ctx.r27.u64 = ctx.r3.u64;",
            creation,
        )
        self.assertIn("ctx.cr6.compare<int32_t>(ctx.r27.s32, -1, ctx.xer);", creation)
        self.assertIn("if (ctx.cr6.eq) goto loc_82D16388;", creation)
        aggression = windows[names[3]]
        self.assertIn("ctx.r11.u64 = REX_LOAD_U16(ctx.r11.u32 + -28720);", aggression)
        self.assertIn(
            "ctx.cr6.compare<int32_t>(ctx.r7.s32, ctx.r9.s32, ctx.xer);",
            aggression,
        )
        self.assertIn(
            "if (!ctx.cr6.eq) goto loc_82CB3F5C;\n\t// extsh r10,r11\n\t"
            "ApplyRagingBarbariansAggression(ctx.r11, ctx.r3);\n\t"
            "ctx.r10.s64 = ctx.r11.s16;",
            aggression,
        )
        self.assertIn("ctx.cr6.compare<int32_t>(ctx.r10.s32, 0, ctx.xer);", aggression)
        self.assertIn("ctx.r3.s64 = 99;", aggression)
        self.assertIn(
            "ctx.cr6.compare<int32_t>(ctx.r7.s32, ctx.r10.s32, ctx.xer);",
            aggression,
        )
        self.assertIn("if (!ctx.cr6.eq) goto loc_82CB3F78;", aggression)
        self.assertIn("ctx.r11.s64 = ctx.r11.s16;", aggression)
        self.assertIn(
            "ctx.r3.u64 = static_cast<uint64_t>(3) - ctx.r11.u64;", aggression
        )
        self.assertIn("__restgprlr_16(ctx, base);\n\treturn;", aggression)
        departure = windows[names[4]]
        self.assertIn(
            "ctx.r9.u64 = REX_LOAD_U8(ctx.r10.u32 + ctx.r11.u32);\n\t"
            "// cmpw cr6,r9,r3\n\t"
            "ObserveRagingBarbariansDeparture(ctx.r9, ctx.r3, ctx.r20, ctx.r26, ctx.r10);\n\t"
            "ctx.cr6.compare<int32_t>(ctx.r9.s32, ctx.r3.s32, ctx.xer);",
            departure,
        )
        self.assertIn("if (!ctx.cr6.gt) goto loc_82CC1320;", departure)
        self.assertIn(
            "ctx.r10.u64 = REX_LOAD_U8(ctx.r20.u32 + ctx.r11.u32);", departure
        )
        self.assertIn("if (ctx.cr6.eq) goto loc_82CC1424;", departure)
        self.assertIn(
            "REX_STORE_U16(ctx.r20.u32 + ctx.r8.u32, ctx.r11.u16);", departure
        )


if __name__ == "__main__":
    unittest.main()
