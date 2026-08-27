from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOOK_CONFIG = ROOT / "config" / "rerevved_hooks.toml"
HOOK_SOURCES = [
    ROOT / "src" / "compat_hooks.cpp",
    ROOT / "src" / "great_general_attachment.cpp",
]
GENERAL_SOURCE = ROOT / "src" / "great_general_attachment.cpp"
GENERATED = ROOT / "generated" / "default"

EXPECTED_HOOKS = [
    {
        "address": 0x82C7DF58,
        "name": "ReRevvedPublishGameplayState",
    },
    {
        "address": 0x8269CAE0,
        "name": "ReRevvedCompatRingInitializeBegin",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x8269CAE4,
        "name": "ReRevvedCompatRingInitializeEnd",
    },
    {
        "address": 0x82245050,
        "name": "ReRevvedRememberGfxRenderConfig",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x82302E90,
        "name": "ReRevvedHandleGfxRenderCapsBegin",
        "registers": ["r3", "r4", "lr"],
    },
    {
        "address": 0x82302F0C,
        "name": "ReRevvedHandleGfxRenderCapsEnd",
        "registers": ["r3", "r31"],
    },
    {
        "address": 0x82D7F934,
        "name": "ReRevvedApplyCombatPaceOverride",
    },
    {
        "address": 0x82CE2938,
        "name": "ReRevvedFixRushCostDisplay",
        "registers": ["r27", "r30", "r31", "r6", "r7", "r11"],
    },
    {
        "address": 0x82D17A9C,
        "name": "ReRevvedFixRushCostApply",
        "registers": [
            "r25",
            "r26",
            "r28",
            "r3",
            "r6",
            "r8",
        ],
    },
    {
        "address": 0x82CBF534,
        "name": "ReRevvedFixGreatGeneralBorderCompletion",
    },
    {
        "address": 0x82CDFA64,
        "name": "ReRevvedFixGreatGeneralPostCombat",
        "registers": ["r31", "r15"],
    },
    {
        "address": 0x82CDFCC4,
        "name": "ReRevvedFixGreatGeneralPostCombat",
        "registers": ["r31", "r26"],
    },
]


class HookContractTests(unittest.TestCase):
    def test_only_verified_hooks_are_configured(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["midasm_hook"], EXPECTED_HOOKS)

    def test_configured_names_match_compat_hook_functions(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)
        source = "\n".join(
            path.read_text(encoding="utf-8") for path in HOOK_SOURCES
        )
        source_names = set(
            re.findall(r"^void (ReRevved\w+)\s*\(", source, re.MULTILINE)
        )
        hook_names = {hook["name"] for hook in config["midasm_hook"]}

        self.assertEqual(
            source_names - {"ReRevvedCompatNullOptionalDispatch"},
            hook_names,
        )

    def test_generated_ring_hook_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "// bl 0x82e923e4\n"
            "\tReRevvedCompatRingInitializeBegin(ctx.r3, ctx.r4);\n"
            "\tctx.lr = 0x8269CAE4;\n"
            "\t__imp__VdInitializeRingBuffer(ctx, base);\n"
            "\t// rlwinm r11,r25,23,9,31\n"
            "\tReRevvedCompatRingInitializeEnd();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_combat_speed_contract(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        override = source.split(
            "void ReRevvedApplyCombatPaceOverride", 1
        )[1].split("void ReRevvedCompatNullOptionalDispatch", 1)[0]

        definitions = re.findall(
            r'REXCVAR_DEFINE_STRING\(combat_speed,\s*"normal",\s*'
            r'"ReRevved/Combat",\s*"Combat presentation speed"\)\s*'
            r'\.allowed\(\{\s*"normal",\s*"fast"\s*\}\);',
            source,
        )
        self.assertEqual(len(definitions), 1)
        self.assertIn('REXCVAR_GET(combat_speed) != "fast"', override)
        self.assertIn("constexpr float    kNativeStandard    = 2.0f;", override)
        self.assertIn("constexpr float    kNativeAlternate   = 1.5f;", override)
        self.assertIn("constexpr float    kNativeFast        = 0.5f;", override)
        self.assertIn(
            "selected != kNativeStandard && selected != kNativeAlternate",
            override,
        )
        self.assertEqual(override.count("WriteGuestU32Safely("), 1)
        self.assertNotIn("REXLOG_", override)

    def test_generated_combat_speed_hook_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\tctx.lr = 0x82D7F934;\n"
            "\tsub_82D66B20(ctx, base);\n"
            "\t// lwz r11,-3448(r20)\n"
            "\tReRevvedApplyCombatPaceOverride();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_rush_cost_repair_has_no_diagnostic_surface(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        self.assertNotIn("rush_cost_probe", source)
        self.assertNotIn("Rush-cost probe", source)

    def test_great_general_fix_is_bounded_and_always_on(self) -> None:
        source = GENERAL_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("great_general_attachment_fix", source)
        self.assertNotIn("REXCVAR_", source)
        self.assertNotIn("FixEnabled", source)
        self.assertRegex(
            source,
            r"void ReRevvedFixGreatGeneralBorderCompletion\(\)\s*"
            r"\{\s*RepairAllPairs\(\);\s*\}",
        )
        self.assertRegex(
            source,
            r"void ReRevvedFixGreatGeneralPostCombat\(PPCRegister& player,\s*"
            r"PPCRegister& unit\)\s*\{\s*"
            r"RepairPairsForCarrier\(player\.s32, unit\.s32\);\s*\}",
        )
        for offset in ["0x00", "0x01", "0x0C", "0x1C", "0x1E", "0x50"]:
            self.assertIn(offset, source)
        self.assertNotIn("REX_STORE_", source)
        self.assertEqual(source.count("TranslateVirtual<uint8_t*>"), 1)
        self.assertEqual(source.count("WriteCoordinates("), 2)
        self.assertNotIn("REXLOG_", source)
        self.assertNotIn("0x26", source)
        self.assertNotIn("unload", source.lower())

    def test_generated_great_general_fix_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "loc_82CBF534:\n"
                "\t// lwz r11,112(r1)\n"
                "\tReRevvedFixGreatGeneralBorderCompletion();"
            ),
            (
                "\tctx.lr = 0x82CDFA64;\n"
                "\tsub_82CD69B8(ctx, base);\n"
                "\t// b 0x82ce16ac\n"
                "\tReRevvedFixGreatGeneralPostCombat(ctx.r31, ctx.r15);"
            ),
            (
                "loc_82CDFCC4:\n"
                "\t// lbzx r11,r30,r28\n"
                "\tReRevvedFixGreatGeneralPostCombat(ctx.r31, ctx.r26);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)
        self.assertNotIn("ReRevvedProbeGreatGeneral", generated)

    def test_generated_rush_cost_hooks_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "\t// cmpwi cr6,r30,29\n"
                "\tReRevvedFixRushCostDisplay(ctx.r27, ctx.r30, ctx.r31, "
                "ctx.r6, ctx.r7, ctx.r11);"
            ),
            (
                "\t// cmpwi cr6,r25,119\n"
                "\tReRevvedFixRushCostApply(ctx.r25, ctx.r26, ctx.r28, "
                "ctx.r3, ctx.r6, ctx.r8);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)


if __name__ == "__main__":
    unittest.main()
