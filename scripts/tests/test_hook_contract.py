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
    ROOT / "src" / "presentation_text_hooks.cpp",
    ROOT / "src" / "unique_unit_rules_hooks.cpp",
    ROOT / "src" / "unit_movement_rules_hooks.cpp",
    ROOT / "src" / "unit_production_cost_rules_hooks.cpp",
    ROOT / "src" / "unit_combat_rules_hooks.cpp",
    ROOT / "src" / "unit_effect_rules_hooks.cpp",
    ROOT / "src" / "terrain_yield_rules_hooks.cpp",
]
GENERAL_SOURCE = ROOT / "src" / "great_general_attachment.cpp"
PRESENTATION_SOURCE = ROOT / "src" / "presentation_text_hooks.cpp"
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
        "address": 0x82D77F0C,
        "name": "ReRevvedApplyLeaderNamePresentationText",
        "registers": ["r3", "r20"],
    },
    {
        "address": 0x82D77FD0,
        "name": "ReRevvedApplyCivilizationNamePresentationText",
        "registers": ["r3", "r20"],
    },
    {
        "address": 0x82D7807C,
        "name": "ReRevvedApplyEraAbilityPresentationText",
        "registers": ["r31", "r20"],
    },
    {
        "address": 0x82D781AC,
        "name": "ReRevvedApplyCivilizationTraitPresentationText",
        "registers": ["r30", "r20"],
    },
    {
        "address": 0x82D78228,
        "name": "ReRevvedApplyUniqueUnitSectionHeadingPresentationText",
        "registers": ["r3"],
    },
    {
        "address": 0x82D783B4,
        "name": "ReRevvedApplyUniqueUnitPresentationText",
        "registers": ["r3", "r27", "r20"],
    },
    {
        "address": 0x82CF2198,
        "name": "ReRevvedApplyUnitMovementBase",
        "registers": ["r30", "r28", "r3"],
    },
    {
        "address": 0x82CF1268,
        "name": "ReRevvedApplyUnitProductionCostPercent",
        "registers": ["r30", "r29", "r28"],
    },
    {
        "address": 0x82D15B84,
        "name": "ReRevvedApplyUnitEffectCreationGrants",
        "registers": ["r26", "r28", "r30"],
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
    {
        "address": 0x82CF18B4,
        "name": "ReRevvedApplyTerrainTradeBase",
        "registers": ["r9", "r29"],
    },
    {
        "address": 0x82CF1BAC,
        "name": "ReRevvedApplyTerrainProductionBase",
        "registers": ["r10", "r31"],
    },
    {
        "address": 0x82CF1D9C,
        "name": "ReRevvedApplyTerrainFoodBase",
        "registers": ["r10", "r30"],
    },
    {
        "address": 0x82CDABBC,
        "name": "ReRevvedApplyUnitCombatAttackPercent",
        "registers": ["r1", "r16"],
    },
    {
        "address": 0x82CDAC10,
        "name": "ReRevvedApplyUnitCombatDefensePercent",
        "registers": ["r1", "r17"],
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

TERRAIN_YIELD_HOOK_SITES = [
    {
        "file": "rerevved_recomp.158.cpp",
        "function": "sub_82CF17C8",
        "name": "ReRevvedApplyTerrainTradeBase",
        "registers": ["r9", "r29"],
        "sequence": (
            "\t// lbzx r7,r10,r8\n"
            "\tctx.r7.u64 = REX_LOAD_U8(ctx.r10.u32 + ctx.r8.u32);\n"
            "\t// extsb r29,r7\n"
            "\tctx.r29.s64 = ctx.r7.s8;\n"
            "\t// beq cr6,0x82cf18e4\n"
            "\tReRevvedApplyTerrainTradeBase(ctx.r9, ctx.r29);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF18E4;"
        ),
    },
    {
        "file": "rerevved_recomp.71.cpp",
        "function": "sub_82CF1AF0",
        "name": "ReRevvedApplyTerrainProductionBase",
        "registers": ["r10", "r31"],
        "sequence": (
            "\t// lbzx r6,r8,r7\n"
            "\tctx.r6.u64 = REX_LOAD_U8(ctx.r8.u32 + ctx.r7.u32);\n"
            "\t// extsb r31,r6\n"
            "\tctx.r31.s64 = ctx.r6.s8;\n"
            "\t// beq cr6,0x82cf1bfc\n"
            "\tReRevvedApplyTerrainProductionBase(ctx.r10, ctx.r31);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF1BFC;"
        ),
    },
    {
        "file": "rerevved_recomp.109.cpp",
        "function": "sub_82CF1CE8",
        "name": "ReRevvedApplyTerrainFoodBase",
        "registers": ["r10", "r30"],
        "sequence": (
            "\t// lbzx r6,r8,r7\n"
            "\tctx.r6.u64 = REX_LOAD_U8(ctx.r8.u32 + ctx.r7.u32);\n"
            "\t// extsb r30,r6\n"
            "\tctx.r30.s64 = ctx.r6.s8;\n"
            "\t// beq cr6,0x82cf1dec\n"
            "\tReRevvedApplyTerrainFoodBase(ctx.r10, ctx.r30);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF1DEC;"
        ),
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

    def test_combat_identity_uses_record_base_type(self) -> None:
        source = (
            ROOT / "src" / "unit_combat_rules_hooks.cpp"
        ).read_text(encoding="ascii")
        identity = source.split("bool TryResolveIdentity", 1)[1].split(
            "bool TryAppendForestCombatLine", 1
        )[0]
        self.assertRegex(
            identity,
            r"TryReadUnitBaseType\(player,\s*unit,\s*base_unit_type\)",
        )
        self.assertRegex(
            identity,
            r"TryResolveUnitIdentity\(\s*"
            r"civilization,\s*base_unit_type,\s*identity\)",
        )
        self.assertIn("unit_type = base_unit_type;", identity)
        self.assertNotRegex(
            identity,
            r"TryResolveUnitIdentity\(\s*civilization,\s*unit,\s*identity\)",
        )

    def test_combat_hooks_use_saved_participant_offsets(self) -> None:
        source = (
            ROOT / "src" / "unit_combat_rules_hooks.cpp"
        ).read_text(encoding="ascii")
        self.assertIn("kAttackerPlayerOffset = 1572;", source)
        self.assertIn("kAttackerUnitOffset   = 1580;", source)
        self.assertIn("kDefenderPlayerOffset = 1596;", source)
        self.assertIn("kDefenderUnitOffset   = 1604;", source)

    def test_coverage_hooks_are_separate_and_collision_free(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            permanent = tomllib.load(stream)["midasm_hook"]
        with COVERAGE_HOOK_CONFIG.open("rb") as stream:
            coverage = tomllib.load(stream)["midasm_hook"]

        self.assertEqual(coverage, COVERAGE_HOOKS)
        permanent_addresses = {hook["address"] for hook in permanent}
        coverage_addresses = {hook["address"] for hook in coverage}
        self.assertEqual(len(permanent_addresses), len(permanent))
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
            r'"ReRevved",\s*"Combat presentation speed"\)\s*'
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

    def test_generated_unit_combat_rule_hooks_join_native_calls_when_available(
        self,
    ) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        placements = [
            (
                "\t// bl 0x82cfc0a8\n"
                "\tReRevvedApplyUnitCombatAttackPercent(ctx.r1, ctx.r16);\n"
                "\tctx.lr = 0x82CDABC0;\n"
                "\tsub_82CFC0A8(ctx, base);"
            ),
            (
                "\t// bl 0x82cfbed0\n"
                "\tReRevvedApplyUnitCombatDefensePercent(ctx.r1, ctx.r17);\n"
                "\tctx.lr = 0x82CDAC14;\n"
                "\tsub_82CFBED0(ctx, base);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)
        self.assertNotIn(
            "ReRevvedApplyUnitCombatAttackPercent(ctx.r1, ctx.r26)", generated
        )
        self.assertNotIn(
            "ReRevvedApplyUnitCombatDefensePercent(ctx.r1, ctx.r23)", generated
        )

    def test_terrain_yield_hook_addresses_are_collision_free(self) -> None:
        with HOOK_CONFIG.open("rb") as stream:
            hooks = tomllib.load(stream)["midasm_hook"]

        terrain_names = {site["name"] for site in TERRAIN_YIELD_HOOK_SITES}
        terrain_hooks = [hook for hook in hooks if hook["name"] in terrain_names]
        expected = [
            {
                "address": 0x82CF18B4,
                "name": "ReRevvedApplyTerrainTradeBase",
                "registers": ["r9", "r29"],
            },
            {
                "address": 0x82CF1BAC,
                "name": "ReRevvedApplyTerrainProductionBase",
                "registers": ["r10", "r31"],
            },
            {
                "address": 0x82CF1D9C,
                "name": "ReRevvedApplyTerrainFoodBase",
                "registers": ["r10", "r30"],
            },
        ]
        self.assertEqual(terrain_hooks, expected)

    def test_generated_terrain_yield_hook_placements_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        for site in TERRAIN_YIELD_HOOK_SITES:
            path = GENERATED / site["file"]
            self.assertTrue(path.is_file(), f"missing generated map row: {path}")
            source = path.read_text(encoding="utf-8")
            function_marker = f"DEFINE_REX_FUNC({site['function']})"
            self.assertEqual(source.count(function_marker), 1)
            function = source.split(function_marker, 1)[1].split(
                "DEFINE_REX_FUNC", 1
            )[0]
            self.assertEqual(function.count(site["sequence"]), 1)

            registers = ", ".join(
                f"PPCRegister& {register}" for register in site["registers"]
            )
            prototype = f"extern void {site['name']}({registers});"
            self.assertEqual(source.count(prototype), 1)

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

    def test_era_presentation_buffer_has_native_length_header(self) -> None:
        source = PRESENTATION_SOURCE.read_text(encoding="ascii")
        parser = source.split("bool TryReplaceEraLines", 1)[1].split(
            "} // namespace", 1
        )[0]
        self.assertIn("std::array<const char*, 9>", parser)
        self.assertIn("if (index == 0)", parser)
        self.assertIn(
            "REREVVED_PRESENTATION_SURFACE_ERA_SECTION_HEADING", parser
        )
        self.assertIn("REREVVED_PRESENTATION_SURFACE_ERA_HEADING", parser)
        self.assertIn("(index - 1) / 2", parser)
        self.assertIn("(index - 2) / 2", parser)
        publish = source.split("bool TryPublishText", 1)[1].split(
            "bool TryEvaluateEraText", 1
        )[0]
        self.assertIn("include_length_header", publish)
        self.assertIn("WriteBigEndianU32", publish)
        self.assertRegex(
            publish,
            r"out\.u64\s*=\s*text_address;",
        )
        era_hook = source.split(
            "void ReRevvedApplyEraAbilityPresentationText", 1
        )[1].split("void ReRevvedApplyUniqueUnitPresentationText", 1)[0]
        self.assertRegex(
            era_hook,
            r"TryPublishText\(replacement\.data\(\),\s*"
            r"replacement\.size\(\),\s*true,",
        )

    def test_additional_presentation_hooks_preserve_native_forms(self) -> None:
        source = PRESENTATION_SOURCE.read_text(encoding="ascii")
        for name in [
            "ReRevvedApplyLeaderNamePresentationText",
            "ReRevvedApplyCivilizationNamePresentationText",
            "ReRevvedApplyCivilizationTraitPresentationText",
            "ReRevvedApplyUniqueUnitSectionHeadingPresentationText",
        ]:
            self.assertEqual(source.count(f"void {name}"), 1)
        self.assertIn(
            "REREVVED_PRESENTATION_SURFACE_LEADER_NAME", source
        )
        self.assertIn(
            "REREVVED_PRESENTATION_SURFACE_CIVILIZATION_NAME", source
        )
        self.assertIn(
            "REREVVED_PRESENTATION_SURFACE_CIVILIZATION_TRAIT", source
        )
        self.assertIn(
            "REREVVED_PRESENTATION_SURFACE_UNIQUE_UNIT_SECTION_HEADING",
            source,
        )
        trait_hook = source.split(
            "void ReRevvedApplyCivilizationTraitPresentationText", 1
        )[1].split(
            "void ReRevvedApplyUniqueUnitSectionHeadingPresentationText", 1
        )[0]
        self.assertIn("TryReplaceText(trait_text", trait_hook)
        self.assertIn("true,\n                   trait_text_buffer", trait_hook)

    def test_generated_presentation_hooks_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        unit_placement = (
            "\tctx.lr = 0x82D783B4;\n"
            "\tsub_82E6A430(ctx, base);\n"
            "\t// mr r4,r3\n"
            "\tReRevvedApplyUniqueUnitPresentationText("
            "ctx.r3, ctx.r27, ctx.r20);\n"
            "\tctx.r4.u64 = ctx.r3.u64;"
        )
        era_placement = (
            "\tctx.r31.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);\n"
            "\t// addi r29,r25,16\n"
            "\tReRevvedApplyEraAbilityPresentationText(ctx.r31, ctx.r20);\n"
            "\tctx.r29.s64 = ctx.r25.s64 + 16;"
        )
        self.assertEqual(generated.count(unit_placement), 1)
        self.assertEqual(generated.count(era_placement), 1)

    def test_generated_unit_movement_hook_preserves_ordinary_return_when_available(
        self,
    ) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        prototype = (
            "extern void ReRevvedApplyUnitMovementBase(PPCRegister& r30, "
            "PPCRegister& r28, PPCRegister& r3);"
        )
        placement = (
            "\t// add r3,r11,r26\n"
            "\tctx.r3.u64 = ctx.r11.u64 + ctx.r26.u64;\n"
            "\t// addi r1,r1,160\n"
            "\tReRevvedApplyUnitMovementBase(ctx.r30, ctx.r28, ctx.r3);"
        )
        if prototype not in generated:
            self.skipTest("generated movement hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF1F70)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(function.count("ReRevvedApplyUnitMovementBase"), 1)
        special_return, ordinary_return = function.split("loc_82CF218C:", 1)
        self.assertNotIn("ReRevvedApplyUnitMovementBase", special_return)
        self.assertIn("ReRevvedApplyUnitMovementBase", ordinary_return)

    def test_generated_unit_cost_hook_uses_shared_scalar_when_available(
        self,
    ) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        prototype = (
            "extern void ReRevvedApplyUnitProductionCostPercent("
            "PPCRegister& r30, PPCRegister& r29, PPCRegister& r28);"
        )
        placement = (
            "loc_82CF1268:\n"
            "\t// mr r3,r28\n"
            "\tReRevvedApplyUnitProductionCostPercent("
            "ctx.r30, ctx.r29, ctx.r28);\n"
            "\tctx.r3.u64 = ctx.r28.u64;"
        )
        if prototype not in generated:
            self.skipTest("generated unit production cost hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF1148)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(
            function.count("ReRevvedApplyUnitProductionCostPercent"), 1
        )
        self.assertIn("ctx.r30.s64 = static_cast<int64_t>", function)
        self.assertIn("ctx.r29.u64 = ctx.r10.u64;", function)
        self.assertIn("ctx.r3.u64 = ctx.r28.u64;", function)

    def test_generated_unit_effect_hook_preserves_native_creation_gate_when_available(
        self,
    ) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        prototype = (
            "extern void ReRevvedApplyUnitEffectCreationGrants("
            "PPCRegister& r26, PPCRegister& r28, PPCRegister& r30);"
        )
        placement = (
            "loc_82D15B84:\n"
            "\t// lwz r11,116(r1)\n"
            "\tReRevvedApplyUnitEffectCreationGrants(ctx.r26, ctx.r28, ctx.r30);\n"
            "\tctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 116);"
        )
        if prototype not in generated:
            self.skipTest("generated unit effect hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82D13978)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(function.count("ReRevvedApplyUnitEffectCreationGrants"), 1)
        self.assertIn("ctx.r3.s64 = 50;", function)
        self.assertIn("cmpwi cr6,r8,0", function)
        self.assertIn("cmpwi cr6,r10,2", function)

    def test_unit_effect_hook_resolves_catalog_identity(self) -> None:
        source = (ROOT / "src" / "unit_effect_rules_hooks.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("unit_catalog::TryResolveUnitIdentity(", source)
        self.assertIn("identity == REREVVED_UNIT_IDENTITY_BASE", source)

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
