from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOOK_CONFIG = ROOT / "config" / "rerevved_hooks.toml"
COVERAGE_HOOK_CONFIG = ROOT / "config" / "native_renderer_coverage_hooks.toml"
COVERAGE_HOOK_SOURCE = ROOT / "src" / "native_renderer_coverage_hooks.inc"
HOOK_SOURCES = [
    ROOT / "src" / "rerevved_hooks.cpp",
    ROOT / "src" / "great_general_attachment.cpp",
    ROOT / "src" / "unique_era_abilities_hooks.cpp",
    ROOT / "src" / "unique_unit_rules_hooks.cpp",
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
        "address": 0x826A6460,
        "name": "ReRevvedObserveNativeDevicePublication",
        "registers": ["r11", "r31"],
    },
    {
        "address": 0x82305104,
        "name": "ReRevvedObserveNativeTexturePublication",
        "registers": ["r22", "r3"],
    },
    {
        "address": 0x8250AE84,
        "name": "ReRevvedObserveNativeResolveProviderIdentity",
        "registers": ["r3"],
    },
    {
        "address": 0x82518614,
        "name": "ReRevvedObserveNativeExplicitBufferFactoryStore",
        "registers": ["r28", "r29", "r30", "r3"],
    },
    {
        "address": 0x8269C7A8,
        "name": "ReRevvedTraceReservationEnter",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x8269C80C,
        "name": "ReRevvedTraceReservationReturn",
        "registers": ["r3", "r29", "r31"],
    },
    {
        "address": 0x826A4638,
        "name": "ReRevvedTraceVdSwapOwnerEnter",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x826A4C0C,
        "name": "ReRevvedTraceVdSwapOwnerReturn",
        "registers": ["r31"],
    },
    {
        "address": 0x8269E520,
        "name": "ReRevvedObserveRendererResolve",
        "registers": ["r4", "r6", "r8", "r9", "lr"],
    },
    {
        "address": 0x8269F360,
        "name": "ReRevvedTraceResolveReturn",
    },
    {
        "address": 0x826A4884,
        "name": "ReRevvedObserveRendererSwapSource",
        "registers": ["r3", "r4", "r30", "r31"],
    },
    {
        "address": 0x826A4888,
        "name": "ReRevvedTraceVdSwapReturn",
        "registers": ["r3", "r30", "r31"],
    },
    {
        "address": 0x826A4890,
        "name": "ReRevvedTraceVdSwapPublished",
        "registers": ["r30", "r31"],
    },
    {
        "address": 0x826A4150,
        "name": "ReRevvedTracePreSwapEnter",
    },
    {
        "address": 0x826A4324,
        "name": "ReRevvedTracePreSwapReturn",
    },
    {
        "address": 0x8269CD20,
        "name": "ReRevvedTraceEmitterCd20Enter",
    },
    {
        "address": 0x8269CE78,
        "name": "ReRevvedTraceEmitterCd20Return",
    },
    {
        "address": 0x8269BF40,
        "name": "ReRevvedTraceEmitterBf40Enter",
    },
    {
        "address": 0x8269C12C,
        "name": "ReRevvedTraceEmitterBf40Return",
    },
    {
        "address": 0x826A3FB8,
        "name": "ReRevvedTraceCallbackEnter",
    },
    {
        "address": 0x826A4148,
        "name": "ReRevvedTraceCallbackReturn",
    },
    {
        "address": 0x826ABEA8,
        "name": "ReRevvedTraceOrdinaryCallerEnter",
    },
    {
        "address": 0x826AC018,
        "name": "ReRevvedTraceOrdinaryCallerReturn",
    },
    {
        "address": 0x82517E38,
        "name": "ReRevvedTraceAlternateCallerEnter",
    },
    {
        "address": 0x82517EB4,
        "name": "ReRevvedTraceAlternateCallerReturn",
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
        "address": 0x82CF225C,
        "name": "ReRevvedApplyUniqueUnitBaseAttack",
        "registers": ["r28", "r29", "r27"],
    },
    {
        "address": 0x82CF21D8,
        "name": "ReRevvedApplyUniqueUnitBaseDefense",
        "registers": ["r29", "r30", "r31"],
    },
    {
        "address": 0x82CF0D6C,
        "name": "ReRevvedApplyUniqueEraAbilityCell",
        "registers": ["r4", "r9", "r11"],
    },
    {
        "address": 0x82D1B758,
        "name": "ReRevvedApplyBarbarianVillageCityReplacement",
        "registers": ["r10"],
    },
    {
        "address": 0x82D2127C,
        "name": "ReRevvedBeginHorsebackRidingOwnershipCheck",
        "registers": ["r31"],
    },
    {
        "address": 0x82D21280,
        "name": "ReRevvedEndHorsebackRidingOwnershipCheck",
        "registers": ["r31"],
    },
    {
        "address": 0x82D212A4,
        "name": "ReRevvedSelectHorsebackRidingAbility",
        "registers": ["r3"],
    },
    {
        "address": 0x82D212C0,
        "name": "ReRevvedSelectHorsebackRidingTechnology",
        "registers": ["r4"],
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

COVERAGE_HOOKS = [
    {
        "address": 0x82303E3C,
        "name": "ReRevvedNativeRendererCoverageSite82303E3C",
        "registers": [],
    },
    {
        "address": 0x82303E8C,
        "name": "ReRevvedNativeRendererCoverageSite82303E8C",
        "registers": [],
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

    def test_coverage_hooks_are_separate_and_collision_free(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            permanent = tomllib.load(stream)["midasm_hook"]
        with COVERAGE_HOOK_CONFIG.open("rb") as stream:
            coverage = tomllib.load(stream)["midasm_hook"]

        self.assertEqual(coverage, COVERAGE_HOOKS)
        permanent_addresses = {hook["address"] for hook in permanent}
        coverage_addresses = {hook["address"] for hook in coverage}
        self.assertTrue(permanent_addresses.isdisjoint(coverage_addresses))

        source = COVERAGE_HOOK_SOURCE.read_text(encoding="ascii")
        source_names = set(
            re.findall(r"^void (ReRevved\w+)\s*\(", source, re.MULTILINE)
        )
        self.assertEqual(source_names, {hook["name"] for hook in coverage})

    def test_generated_coverage_hook_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "\t// bl 0x826a3568\n"
                "\tReRevvedNativeRendererCoverageSite82303E3C();\n"
                "\tctx.lr = 0x82303E40;"
            ),
            (
                "\t// bl 0x826a3568\n"
                "\tReRevvedNativeRendererCoverageSite82303E8C();\n"
                "\tctx.lr = 0x82303E90;"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)

    def test_generated_ring_hook_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        prototype = (
            "extern void ReRevvedObserveRendererResolve(PPCRegister& r4, "
            "PPCRegister& r6, PPCRegister& r8, PPCRegister& r9, uint64_t lr);"
        )
        self.assertEqual(generated.count(prototype), 1)
        expected = (
            "// bl 0x82e923e4\n"
            "\tReRevvedCompatRingInitializeBegin(ctx.r3, ctx.r4);\n"
            "\tctx.lr = 0x8269CAE4;\n"
            "\t__imp__VdInitializeRingBuffer(ctx, base);\n"
            "\t// rlwinm r11,r25,23,9,31\n"
            "\tReRevvedCompatRingInitializeEnd();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_generated_native_device_observer_preserves_call(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\t// stw r31,0(r11)\n"
            "\tREX_STORE_U32(ctx.r11.u32 + 0, ctx.r31.u32);\n"
            "\t// bl 0x82e924c4\n"
            "\tReRevvedObserveNativeDevicePublication(ctx.r11, ctx.r31);\n"
            "\tctx.lr = 0x826A6464;\n"
            "\t__imp__ExGetXConfigSetting(ctx, base);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_native_device_observer_is_read_only_and_native_gated(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        observer = source.split(
            "void ReRevvedObserveNativeDevicePublication", 1
        )[1].split("void ReRevvedRememberGfxRenderConfig", 1)[0]
        self.assertIn('REXCVAR_GET(renderer) != "native"', observer)
        self.assertIn("IsGuestReadableRange", observer)
        self.assertIn("ReadGuestU32", observer)
        self.assertIn("PublishGuestDevice", observer)
        self.assertNotIn("WriteGuest", observer)

    def test_generated_native_texture_observer_preserves_store(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\t// stw r3,0(r19)\n"
            "\tREX_STORE_U32(ctx.r19.u32 + 0, ctx.r3.u32);\n"
            "\t// cmplwi r3,0\n"
            "\tReRevvedObserveNativeTexturePublication(ctx.r22, ctx.r3);\n"
            "\tctx.cr0.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_native_texture_observer_is_read_only_and_native_gated(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        observer = source.split(
            "void ReRevvedObserveNativeTexturePublication", 1
        )[1].split("void ReRevvedObserveRendererResolve", 1)[0]
        self.assertIn('REXCVAR_GET(renderer) != "native"', observer)
        self.assertIn("IsGuestReadableRange", observer)
        self.assertIn("ReadGuestU32", observer)
        self.assertIn("ObserveGuestTexture", observer)
        self.assertNotIn("texture_address, backend_address", observer)
        self.assertNotIn("WriteGuest", observer)

    def test_generated_native_explicit_buffer_observer_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        provider = (
            "loc_8250AE84:\n"
            "\t// stw r27,116(r1)\n"
            "\tReRevvedObserveNativeResolveProviderIdentity(ctx.r3);\n"
            "\tREX_STORE_U32(ctx.r1.u32 + 116, ctx.r27.u32);"
        )
        factory = (
            "\t// stw r3,0(r29)\n"
            "\tREX_STORE_U32(ctx.r29.u32 + 0, ctx.r3.u32);\n"
            "\t// addi r29,r29,4\n"
            "\tReRevvedObserveNativeExplicitBufferFactoryStore("
            "ctx.r28, ctx.r29, ctx.r30, ctx.r3);\n"
            "\tctx.r29.s64 = ctx.r29.s64 + 4;"
        )
        for placement in (provider, factory):
            self.assertEqual(generated.count(placement), 1)

    def test_native_explicit_buffer_observers_are_read_only_and_bounded(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        provider = source.split(
            "void ReRevvedObserveNativeResolveProviderIdentity", 1
        )[1].split("void ReRevvedObserveNativeExplicitBufferFactoryStore", 1)[0]
        factory = source.split(
            "void ReRevvedObserveNativeExplicitBufferFactoryStore", 1
        )[1].split("void ReRevvedObserveRendererResolve", 1)[0]
        for observer in (provider, factory):
            self.assertIn('REXCVAR_GET(renderer) != "native"', observer)
            self.assertIn("IsGuestReadableRange", observer)
            self.assertNotIn("WriteGuest", observer)
            self.assertNotIn("TranslateVirtual<uint8_t*>", observer)
        self.assertIn("ReadGuestU32", source)
        self.assertIn("std::atomic_flag", source)
        self.assertIn("native_resolve_provider_match_log_count", source)
        self.assertIn("native_resolve_provider_mismatch_log_count", source)
        self.assertIn("vptr != kExplicitBuffersVtable", provider)
        self.assertIn("vptr_matches", factory)
        self.assertIn("native_explicit_factory_log_count", source)

    def test_generated_resolve_observer_preserves_body_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "DEFINE_REX_FUNC(sub_8269E520) {\n"
            "\tREX_FUNC_PROLOGUE();\n"
            "\tPPCRegister temp{};\n"
            "\tuint32_t ea{};\n"
            "\t// mflr r12\n"
            "\tReRevvedObserveRendererResolve("
            "ctx.r4, ctx.r6, ctx.r8, ctx.r9, ctx.lr);\n"
            "\tctx.r12.u64 = ctx.lr;"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_generated_swap_observer_preserves_call_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\t// bl 0x82e92454\n"
            "\tReRevvedObserveRendererSwapSource("
            "ctx.r3, ctx.r4, ctx.r30, ctx.r31);\n"
            "\tctx.lr = 0x826A4888;\n"
            "\t__imp__VdSwap(ctx, base);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_resolve_swap_observers_are_read_only_and_bounded(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        observers = source.split(
            "void ReRevvedObserveRendererResolve", 1
        )[1].split("void ReRevvedRememberGfxRenderConfig", 1)[0]
        state = (
            ROOT / "src" / "gpu" / "diagnostics" / "native_renderer_guest_state.cpp"
        ).read_text(
            encoding="utf-8"
        )

        self.assertIn("ReadGuestFetchDescriptor", observers)
        self.assertIn("ObserveGuestResolve", observers)
        self.assertIn("ObserveGuestSwap", observers)
        self.assertNotIn("WriteGuest", observers)
        self.assertRegex(
            source,
            r"void ReRevvedObserveRendererResolve\(PPCRegister& r4,\s*"
            r"PPCRegister& r6,\s*PPCRegister& r8,\s*PPCRegister& r9,\s*"
            r"uint64_t\s+lr\)",
        )
        self.assertRegex(
            state,
            r"constexpr std::size_t\s+kResolveHistorySize\s*=\s*64;",
        )
        self.assertIn("std::array<GuestResolveRecord, kResolveHistorySize>", state)

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

    def test_generated_unique_unit_rule_hooks_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "\t// cmpwi cr6,r27,0\n"
                "\tReRevvedApplyUniqueUnitBaseAttack("
                "ctx.r28, ctx.r29, ctx.r27);\n"
                "\tctx.cr6.compare<int32_t>(ctx.r27.s32, 0, ctx.xer);"
            ),
            (
                "\t// cmpwi cr6,r5,0\n"
                "\tReRevvedApplyUniqueUnitBaseDefense("
                "ctx.r29, ctx.r30, ctx.r31);\n"
                "\tctx.cr6.compare<int32_t>(ctx.r5.s32, 0, ctx.xer);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)
        self.assertNotIn("ReRevvedBeginEffective", generated)
        self.assertNotIn("ReRevvedFinishEffective", generated)

    def test_generated_unique_era_ability_hook_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placement = (
            "\t// cmpw cr6,r11,r3\n"
            "\tReRevvedApplyUniqueEraAbilityCell("
            "ctx.r4, ctx.r9, ctx.r11);\n"
            "\tctx.cr6.compare<int32_t>(ctx.r11.s32, ctx.r3.s32, ctx.xer);"
        )
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF0CB0)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        exact_mode, cumulative_mode = function.split("loc_82CF0D0C:", 1)
        self.assertNotIn("ReRevvedApplyUniqueEraAbilityCell", exact_mode)
        self.assertEqual(
            cumulative_mode.count("ReRevvedApplyUniqueEraAbilityCell"), 1
        )
        self.assertIn("if (ctx.cr6.eq) goto loc_82CF0D0C;", exact_mode)

    def test_generated_horseback_riding_consumer_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        function = generated.split("DEFINE_REX_FUNC(sub_82D1EAB0)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        placements = [
            (
                "\t// lwz r11,72(r31)\n"
                "\tReRevvedBeginHorsebackRidingOwnershipCheck(ctx.r31);\n"
                "\tctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 72);\n"
                "\t// slw r10,r10,r26\n"
                "\tReRevvedEndHorsebackRidingOwnershipCheck(ctx.r31);"
            ),
            (
                "\t// bl 0x82cf0cb0\n"
                "\tReRevvedSelectHorsebackRidingAbility(ctx.r3);\n"
                "\tctx.lr = 0x82D212A8;\n"
                "\tsub_82CF0CB0(ctx, base);"
            ),
            (
                "\t// mr r3,r26\n"
                "\tReRevvedSelectHorsebackRidingTechnology(ctx.r4);\n"
                "\tctx.r3.u64 = ctx.r26.u64;\n"
                "\t// bl 0x82d09208"
            ),
        ]
        for placement in placements:
            self.assertEqual(function.count(placement), 1)
        self.assertIn("ctx.r5.s64 = 0;", function)
        self.assertIn("ctx.r3.s64 = 17;", function)
        self.assertIn("sub_82D09208(ctx, base);", function)

    def test_generated_mongolian_village_gate_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        function = generated.split("DEFINE_REX_FUNC(sub_82D1B400)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        placement = (
            "\t// cmpwi cr6,r10,14\n"
            "\tReRevvedApplyBarbarianVillageCityReplacement(ctx.r10);\n"
            "\tctx.cr6.compare<int32_t>(ctx.r10.s32, 14, ctx.xer);"
        )
        self.assertEqual(function.count(placement), 1)
        self.assertIn("if (!ctx.cr6.eq) goto loc_82D1B8BC;", function)
        fallback = function.split("loc_82D1B8BC:", 1)[1]
        self.assertIn("ctx.r10.s64 = -2096168960;", fallback)


if __name__ == "__main__":
    unittest.main()
