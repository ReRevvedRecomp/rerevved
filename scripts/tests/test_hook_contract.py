from __future__ import annotations

import io
import json
import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import (
    WindowCollector,
    chunk_lines,
    chunk_paths,
    chunk_texts,
    count_per_chunk,
    generated_chunks,
    generated_text,
    scan_once,
)

ROOT = Path(__file__).resolve().parents[2]
RESEARCH_PROMPT_HOOK_CONFIG = ROOT / "config" / "advisor_hooks.toml"
RESEARCH_PROMPT_SOURCE = ROOT / "src" / "research_prompt_hooks.cpp"
ADVISOR_POPUP_HOOK_CONFIG = ROOT / "config" / "advisor_hooks.toml"
ADVISOR_POPUP_SOURCE = ROOT / "src" / "advisor_popup_hooks.cpp"
ADVISOR_POPUP_TABLE = ROOT / "src" / "advisor_popups.h"
BARBARIAN_MESSAGE_HOOK_CONFIG = ROOT / "config" / "advisor_hooks.toml"
BARBARIAN_MESSAGE_SOURCE = ROOT / "src" / "barbarian_message_hooks.cpp"
BARBARIAN_MESSAGE_TABLE = ROOT / "src" / "barbarian_messages.h"
AI_CIV_HOOK_CONFIG = ROOT / "config" / "ai_civilizations_hooks.toml"
AI_CIV_SOURCE = ROOT / "src" / "ai_civilizations_hooks.cpp"
AI_CIV_TABLE = ROOT / "src" / "ai_civilizations.h"
OVERLAY_SETTINGS_SOURCE = ROOT / "src" / "overlay_settings.cpp"
NATIVE_PRESENT_HOOK_CONFIG = ROOT / "config" / "native_present_hooks.toml"
NATIVE_PRESENT_SOURCE = ROOT / "src" / "gpu" / "guest_d3d" / "present_hook.cpp"
NATIVE_PRESENT_SWAP_SOURCE = ROOT / "src" / "gpu" / "guest_d3d" / "present_swap.cpp"
RING_STUB_SOURCES = [
    ROOT / "src" / "gpu" / "guest_d3d" / "ring_stubs.cpp",
    ROOT / "src" / "gpu" / "guest_d3d" / "ring_stub_hooks.cpp",
]
MANIFEST = ROOT / "rerevved_manifest.toml"
HOOK_SOURCES = [
    ROOT / "src" / "rerevved_hooks.cpp",
    ROOT / "src" / "great_general_attachment.cpp",
    ROOT / "src" / "unique_era_abilities_hooks.cpp",
    ROOT / "src" / "nation_select_text_hooks.cpp",
    ROOT / "src" / "unique_unit_rules_hooks.cpp",
    ROOT / "src" / "unit_movement_rules_hooks.cpp",
    ROOT / "src" / "unit_production_cost_rules_hooks.cpp",
    ROOT / "src" / "unit_combat_rules_hooks.cpp",
    ROOT / "src" / "unit_effect_rules_hooks.cpp",
    ROOT / "src" / "terrain_yield_rules_hooks.cpp",
    ROOT / "src" / "building_effect_rules_hooks.cpp",
    ROOT / "src" / "wonder_effect_rules_hooks.cpp",
    ROOT / "src" / "main_menu_logo_hooks.cpp",
    ROOT / "src" / "system_link_trace_hooks.cpp",
    ROOT / "src" / "gameplay_fixes_hooks.cpp",
    ROOT / "src" / "ai_economy_hooks.cpp",
]
GENERAL_SOURCE = ROOT / "src" / "great_general_attachment.cpp"
NATION_SELECT_TEXT_SOURCE = ROOT / "src" / "nation_select_text_hooks.cpp"
APP_SOURCE = ROOT / "src" / "app.cpp"
GUEST_API_CENSUS_TABLE = ROOT / "src/gpu/guest_d3d/guest_api_entries.inc"
ROUTE_TRACE_TABLE = ROOT / "src/gpu/guest_d3d/route_entries.inc"


def read_hooks(path: Path) -> list[dict]:
    with path.open("rb") as stream:
        return tomllib.load(stream).get("midasm_hook", [])


def all_configured_hooks() -> list[dict]:
    with MANIFEST.open("rb") as stream:
        includes = tomllib.load(stream)["entrypoint"]["includes"]
    return [hook for path in includes for hook in read_hooks(ROOT / path)]


def compat_hooks() -> list[dict]:
    paths = (
        "scene_probe_hooks.toml",
        "asset_hooks.toml",
        "gameplay_hooks.toml",
        "graphics_compat_hooks.toml",
        "native_progress_hooks.toml",
        "native_tiling_probe_hooks.toml",
        "native_resource_hooks.toml",
        "native_present_hooks.toml",
        "unit_rules_hooks.toml",
        "civilization_rules_hooks.toml",
        "nation_select_text_hooks.toml",
        "terrain_hooks.toml",
        "city_effect_hooks.toml",
        "system_link_trace_hooks.toml",
        "ai_economy_hooks.toml",
    )
    hooks = []
    for path in paths:
        for hook in read_hooks(ROOT / "config" / path):
            # These declarations have their own exact feature contracts.
            if hook["name"] in {
                "ReplaceVdSwapCall",
                "ApplyCityFoundingUnit",
                "ApplyResourceIdentityOverride",
                "SkipBalancedIndiaStartingResource",
            }:
                continue
            if path == "terrain_hooks.toml" and hook["name"].startswith("Trace"):
                continue
            hooks.append(hook)
    return hooks


SCENE_ALLOCATION_HOOKS = [
    {"address": 0x8280DAC0, "registers": ["r3", "r4"], "returns": ["r3"]},
    {"address": 0x8280DB58, "registers": ["r3", "r4"], "returns": []},
]

EXPECTED_HOOKS = [
    {
        "address": 0x82CF1058,
        "name": "ApplyAiResearchCostPercent",
        "registers": ["r25", "r26", "r3"],
    },
    {
        "address": 0x82CF126C,
        "name": "ApplyAiUnitProductionCostPercent",
        "registers": ["r30", "r29", "r3"],
    },
    {
        "address": 0x82CF154C,
        "name": "ApplyAiNonUnitProductionCostPercent",
        "registers": ["r29", "r27", "r31"],
    },
    {
        "address": 0x8230575C,
        "name": "ObserveSceneRegionLocked",
        "registers": ["r1", "r29", "r22", "r31", "r20"],
    },
    {
        "address": 0x8230589C,
        "name": "ObserveSceneRegionWritten",
        "registers": ["r1", "r29", "r22", "r31", "r20"],
    },
    {
        "address": 0x82E87B90,
        "name": "ApplyAssetFileOverride",
        "registers": ["r1", "r11", "r30"],
    },
    {
        "address": 0x82C7DF58,
        "name": "PublishGameplayState",
    },
    {
        "address": 0x8269CAE0,
        "name": "CompatRingInitializeBegin",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x8269CAE4,
        "name": "CompatRingInitializeEnd",
    },
    {
        "address": 0x826A1D90,
        "name": "RecordGuestClearTarget",
        "registers": ["r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r1", "f1"],
    },
    {
        "address": 0x826A2960,
        "name": "RecordGuestClearReturn",
        "registers": ["r1"],
    },
    {
        "address": 0x8269E8A0,
        "name": "RecordGuestResolveWords",
        "registers": ["r31", "r1"],
    },
    {
        "address": 0x826A419C,
        "name": "RecordPresentFlipRequest",
        "registers": ["r31"],
    },
    {
        "address": 0x826A42BC,
        "name": "RecordPresentFlipWait",
        "registers": ["r31"],
    },
    {
        "address": 0x8269C92C,
        "name": "CloseGuestRingEpochWrites",
    },
    {
        "address": 0x82697E28,
        "name": "RecordCoherBarrier82697E28",
    },
    {
        "address": 0x82698118,
        "name": "RecordCoherBarrier82698118",
    },
    {
        "address": 0x8269BB94,
        "name": "RecordCoherBarrier8269BB94",
    },
    {
        "address": 0x8269BC54,
        "name": "RecordCoherBarrier8269BC54",
    },
    {
        "address": 0x826A44E0,
        "name": "RecordDisplayWait826A44E0",
    },
    {
        "address": 0x826A45C8,
        "name": "RecordDisplayWait826A45C8",
    },
    {
        "address": 0x826ACD84,
        "name": "RecordCoherBarrier826ACD84",
    },
    {
        "address": 0x826A9CA8,
        "name": "RecordUnmodeledProgress826A9CA8",
    },
    {
        "address": 0x826AB268,
        "name": "RecordUnmodeledProgress826AB268",
    },
    {
        "address": 0x826A9BDC,
        "name": "RecordGpuTimestamp",
        "registers": ["r30", "r31"],
    },
    {
        "address": 0x8269C4E8,
        "name": "RecordTiledSegment",
        "registers": ["r29", "r30"],
    },
    {
        "address": 0x826AC0A8,
        "name": "BeginTiledList",
    },
    {
        "address": 0x8269BDE8,
        "name": "RecordDeferredKick",
        "registers": ["r28", "r27"],
    },
    {
        "address": 0x8269BE5C,
        "name": "RecordDirectSubmit",
        "registers": ["r28", "r27"],
    },
    {
        "address": 0x826AB890,
        "name": "RecordReplaySubmit826AB890",
        "registers": ["r11", "r10"],
    },
    {
        "address": 0x826AB990,
        "name": "RecordReplaySubmit826AB990",
        "registers": ["r10", "r11"],
    },
    {
        "address": 0x826ABB90,
        "name": "MarkReplayWorker",
    },
    {
        "address": 0x826ABC48,
        "name": "ObserveTilingInterpreterExit",
    },
    {
        "address": 0x826ABD0C,
        "name": "ObserveTilingListWord",
        "registers": ["r5"],
    },
    {
        "address": 0x826ABD6C,
        "name": "ObserveTilingCallback",
        "registers": ["r11", "r31"],
    },
    {
        "address": 0x826AB770,
        "name": "ObserveTilingHandlerToken",
        "registers": ["r10"],
    },
    {
        "address": 0x826AB960,
        "name": "ObserveTilingHandlerLink",
        "registers": ["r10"],
    },
    {
        "address": 0x826AB120,
        "name": "ObserveTilingWrite826AB120",
        "registers": ["r31"],
    },
    {
        "address": 0x826AB4C8,
        "name": "ObserveTilingWrite826AB4C8",
        "registers": ["r9"],
    },
    {
        "address": 0x826AB60C,
        "name": "ObserveTilingWrite826AB60C",
        "registers": ["r3"],
    },
    {
        "address": 0x826AB6FC,
        "name": "ObserveTilingWrite826AB6FC",
        "registers": ["r8"],
    },
    {
        "address": 0x826ABF20,
        "name": "ObserveTilingWorkerIdle",
        "registers": ["r25"],
    },
    {
        "address": 0x826AC348,
        "name": "ObserveTilingSubmitGuard",
        "registers": ["r6"],
    },
    {
        "address": 0x8269B900,
        "name": "ObserveTilingPublisher",
        "registers": ["r3"],
    },
    {
        "address": 0x826A5B48,
        "name": "ObserveTilingWatchdogEnter",
        "registers": ["r3"],
    },
    {
        "address": 0x826A5B98,
        "name": "ObserveTilingWatchdogGlobal",
        "registers": ["r31"],
    },
    {
        "address": 0x826A5BC8,
        "name": "ObserveTilingWatchdogReset",
        "registers": ["r31"],
    },
    {
        "address": 0x826A5BD4,
        "name": "ObserveTilingWatchdogZero",
        "registers": ["r31"],
    },
    {
        "address": 0x8268CFDC,
        "name": "RecordDeviceSuspend",
        "registers": ["r1"],
    },
    {
        "address": 0x8268D060,
        "name": "RecordDeviceResume",
    },
    {
        "address": 0x8268CF3C,
        "name": "RecordDeviceAcquire",
    },
    {
        "address": 0x8268CF58,
        "name": "RecordDeviceRelease",
    },
    {
        "address": 0x826ABF7C,
        "name": "BeginDeferredSwap",
        "registers": ["r30"],
    },
    {
        "address": 0x826ABF94,
        "name": "EndDeferredSwap",
        "registers": ["r30"],
    },
    {
        "address": 0x826A6460,
        "name": "ObserveNativeDevicePublication",
        "registers": ["r11", "r31"],
    },
    {
        "address": 0x82305104,
        "name": "ObserveNativeTexturePublication",
        "registers": ["r22", "r3"],
    },
    {
        "address": 0x8250AE84,
        "name": "ObserveNativeResolveProviderIdentity",
        "registers": ["r3"],
    },
    {
        "address": 0x82518614,
        "name": "ObserveNativeExplicitBufferFactoryStore",
        "registers": ["r28", "r29", "r30", "r3"],
    },
    {
        "address": 0x8269F360,
        "name": "TraceResolveReturn",
        "registers": ["r1"],
    },
    {
        "address": 0x826A4884,
        "name": "ObserveRendererSwapSource",
        "registers": ["r3", "r4", "r30", "r31"],
    },
    {
        "address": 0x826AE5B8,
        "name": "ObserveNativeGuestVertexSelection",
        "registers": ["r30", "r31", "r15", "r19"],
    },
    {
        "address": 0x8268FE30,
        "name": "ObserveNativeGuestGammaTable",
        "registers": ["r31", "r1"],
    },
    {
        "address": 0x8268FEF8,
        "name": "ObserveNativeGuestPwlGamma",
        "registers": ["r31"],
    },
    {
        "address": 0x826A4888,
        "name": "TraceVdSwapReturn",
    },
    {
        "address": 0x826A4258,
        "name": "ObserveDecodedSwapInterval",
        "registers": ["r11", "r31"],
    },
    {
        "address": 0x82245050,
        "name": "RememberGfxRenderConfig",
        "registers": ["r3", "r4"],
    },
    {
        "address": 0x82302E90,
        "name": "HandleGfxRenderCapsBegin",
        "registers": ["r3", "r4", "lr"],
    },
    {
        "address": 0x82302F0C,
        "name": "HandleGfxRenderCapsEnd",
        "registers": ["r3", "r31"],
    },
    {
        "address": 0x82D7F934,
        "name": "ApplyCombatPaceOverride",
    },
    {
        "address": 0x82CE2938,
        "name": "FixRushCostDisplay",
        "registers": ["r27", "r30", "r31", "r6", "r7", "r11"],
    },
    {
        "address": 0x82CED34C,
        "name": "FixAiResearchScanStart",
        "registers": ["r10", "r11", "r18", "r26"],
    },
    {
        "address": 0x82D0A158,
        "name": "AdmitFirstDiscoveryTempleCity",
        "registers": ["r18"],
        "jump_address_on_true": 0x82D0A160,
    },
    {
        "address": 0x82D09588,
        "name": "PreserveEgyptianIrrigationReward",
        "registers": ["r14", "r16", "r25", "r22"],
        "jump_address_on_true": 0x82D09594,
    },
    {
        "address": 0x82D09BD0,
        "name": "SuppressLiteracyCourthouseReward",
        "registers": ["r9", "r18"],
        "jump_address_on_true": 0x82D0A2E4,
    },
    {
        "address": 0x82D0A254,
        "name": "SuppressStaleFirstDiscoveryCaption",
        "registers": ["r15", "r16", "r29"],
        "jump_address_on_true": 0x82D0A280,
    },
    {
        "address": 0x82D2A084,
        "name": "AdmitSpaceshipArrivalYear",
        "registers": ["r31"],
        "jump_address_on_true": 0x82D2A08C,
    },
    {
        "address": 0x82CF7090,
        "name": "SkipFailedSpyUnitCreation",
        "registers": ["r3"],
        "jump_address_on_true": 0x82CF7360,
    },
    {
        "address": 0x82CF7274,
        "name": "SkipFailedSpyUnitCreation",
        "registers": ["r3"],
        "jump_address_on_true": 0x82CF7354,
    },
    {
        "address": 0x82D11250,
        "name": "LogDominationCapitals",
        "registers": ["r27", "r9", "r10", "r11"],
    },
    {
        "address": 0x82D11264,
        "name": "ApplyDominationMinimumTurns",
        "registers": ["r9"],
    },
    {
        "address": 0x82D17A9C,
        "name": "FixRushCostApply",
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
        "name": "ApplyUniqueUnitBaseAttack",
        "registers": ["r28", "r29", "r27"],
    },
    {
        "address": 0x82CF21D8,
        "name": "ApplyUniqueUnitBaseDefense",
        "registers": ["r29", "r30", "r31"],
    },
    {
        "address": 0x82CF0D6C,
        "name": "ApplyUniqueEraAbilityCell",
        "registers": ["r4", "r9", "r11"],
    },
    {
        "address": 0x82D77F0C,
        "name": "ApplyLeaderNameNationSelectText",
        "registers": ["r3", "r20"],
    },
    {
        "address": 0x82D77FD0,
        "name": "ApplyCivilizationNameNationSelectText",
        "registers": ["r3", "r20"],
    },
    {
        "address": 0x82D7807C,
        "name": "ApplyEraAbilityNationSelectText",
        "registers": ["r31", "r20"],
    },
    {
        "address": 0x82D781AC,
        "name": "ApplyCivilizationTraitNationSelectText",
        "registers": ["r30", "r20"],
    },
    {
        "address": 0x82D78228,
        "name": "ApplyUniqueUnitSectionHeadingNationSelectText",
        "registers": ["r3"],
    },
    {
        "address": 0x82D783B4,
        "name": "ApplyUniqueUnitNationSelectText",
        "registers": ["r3", "r27", "r20"],
    },
    {
        "address": 0x82CF2198,
        "name": "ApplyUnitMovementBase",
        "registers": ["r30", "r28", "r3"],
    },
    {
        "address": 0x82CF1268,
        "name": "ApplyUnitProductionCostPercent",
        "registers": ["r30", "r29", "r28"],
    },
    {
        "address": 0x82D15B84,
        "name": "ApplyUnitEffectCreationGrants",
        "registers": ["r26", "r28", "r30"],
    },
    {
        "address": 0x82D1B758,
        "name": "ApplyBarbarianVillageCityReplacement",
        "registers": ["r10"],
    },
    {
        "address": 0x82D2127C,
        "name": "BeginHorsebackRidingOwnershipCheck",
        "registers": ["r31"],
    },
    {
        "address": 0x82D21280,
        "name": "EndHorsebackRidingOwnershipCheck",
        "registers": ["r31"],
    },
    {
        "address": 0x82D212A4,
        "name": "SelectHorsebackRidingAbility",
        "registers": ["r3"],
    },
    {
        "address": 0x82D212C0,
        "name": "SelectHorsebackRidingTechnology",
        "registers": ["r4"],
    },
    {
        "address": 0x82CBF534,
        "name": "FixGreatGeneralBorderCompletion",
    },
    {
        "address": 0x82CDFA64,
        "name": "FixGreatGeneralPostCombat",
        "registers": ["r31", "r15"],
    },
    {
        "address": 0x82CDFCC4,
        "name": "FixGreatGeneralPostCombat",
        "registers": ["r31", "r26"],
    },
    {
        "address": 0x82CF18B4,
        "name": "ApplyTerrainTradeBase",
        "registers": ["r9", "r29"],
    },
    {
        "address": 0x82CF1BAC,
        "name": "ApplyTerrainProductionBase",
        "registers": ["r10", "r31"],
    },
    {
        "address": 0x82CF1D9C,
        "name": "ApplyTerrainFoodBase",
        "registers": ["r10", "r30"],
    },
    {
        "address": 0x82D1537C,
        "name": "ApplyChineseLibraryGoldCache",
        "registers": ["r1", "r15", "r17", "r27", "r31"],
    },
    {
        "address": 0x82D15610,
        "name": "ApplyChineseLibraryGoldCredit",
        "registers": ["r1", "r8", "r17", "r31"],
    },
    {
        "address": 0x82D16344,
        "name": "GrantStonehengeReligion",
        "registers": ["r23", "r27"],
    },
    {
        "address": 0x82CDABBC,
        "name": "ApplyUnitCombatAttackPercent",
        "registers": ["r1", "r16"],
    },
    {
        "address": 0x82CDAC10,
        "name": "ApplyUnitCombatDefensePercent",
        "registers": ["r1", "r17"],
    },
    {
        "address": 0x82C9D9DC,
        "name": "TraceSystemLinkLanUpdate",
        "registers": ["r5", "r6"],
    },
    {
        "address": 0x82C9D9E0,
        "name": "TraceSystemLinkLanUpdateResult",
        "registers": ["r3"],
    },
    {
        "address": 0x82C9F438,
        "name": "TraceSystemLinkQr2Init",
        "registers": ["r5", "r8", "r9"],
    },
    {
        "address": 0x82C9F43C,
        "name": "TraceSystemLinkQr2InitResult",
        "registers": ["r3"],
    },
    {
        "address": 0x82C9D760,
        "name": "TraceSystemLinkBrowserCallback",
        "registers": ["r4"],
    },
    {
        "address": 0x82E87980,
        "name": "TraceSystemLinkAdmissionCall",
        "registers": ["r6", "r10"],
    },
    {
        "address": 0x82E87988,
        "name": "TraceSystemLinkAdmissionResult",
        "registers": ["r3"],
    },
    {
        "address": 0x82CA0A04,
        "name": "TraceSystemLinkAdmissionRole",
        "registers": ["r3"],
    },
    {
        "address": 0x82CA0AB0,
        "name": "TraceSystemLinkAdmissionHostResult",
        "registers": ["r3"],
    },
    {
        "address": 0x82CA0AFC,
        "name": "TraceSystemLinkAdmissionVersion",
        "registers": ["r1", "r3"],
    },
    {
        "address": 0x82CA0AA8,
        "name": "TraceSystemLinkAdmissionHostCall",
        "registers": ["r4", "r10"],
    },
    {
        "address": 0x82CA6A74,
        "name": "TraceSystemLinkAdmissionPeers",
        "registers": ["r30"],
    },
    {
        "address": 0x82CA6AD4,
        "name": "TraceSystemLinkAdmissionSession",
        "registers": ["r3"],
    },
    {
        "address": 0x82CA6B1C,
        "name": "TraceSystemLinkAdmissionAppCall",
        "registers": ["r10"],
    },
    {
        "address": 0x82CA6B24,
        "name": "TraceSystemLinkAdmissionAppResult",
        "registers": ["r3"],
    },
]

TERRAIN_YIELD_HOOK_SITES = [
    {
        "function": "sub_82CF17C8",
        "name": "ApplyTerrainTradeBase",
        "registers": ["r9", "r29"],
        "sequence": (
            "\t// lbzx r7,r10,r8\n"
            "\tctx.r7.u64 = REX_LOAD_U8(ctx.r10.u32 + ctx.r8.u32);\n"
            "\t// extsb r29,r7\n"
            "\tctx.r29.s64 = ctx.r7.s8;\n"
            "\t// beq cr6,0x82cf18e4\n"
            "\tApplyTerrainTradeBase(ctx.r9, ctx.r29);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF18E4;"
        ),
    },
    {
        "function": "sub_82CF1AF0",
        "name": "ApplyTerrainProductionBase",
        "registers": ["r10", "r31"],
        "sequence": (
            "\t// lbzx r6,r8,r7\n"
            "\tctx.r6.u64 = REX_LOAD_U8(ctx.r8.u32 + ctx.r7.u32);\n"
            "\t// extsb r31,r6\n"
            "\tctx.r31.s64 = ctx.r6.s8;\n"
            "\t// beq cr6,0x82cf1bfc\n"
            "\tApplyTerrainProductionBase(ctx.r10, ctx.r31);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF1BFC;"
        ),
    },
    {
        "function": "sub_82CF1CE8",
        "name": "ApplyTerrainFoodBase",
        "registers": ["r10", "r30"],
        "sequence": (
            "\t// lbzx r6,r8,r7\n"
            "\tctx.r6.u64 = REX_LOAD_U8(ctx.r8.u32 + ctx.r7.u32);\n"
            "\t// extsb r30,r6\n"
            "\tctx.r30.s64 = ctx.r6.s8;\n"
            "\t// beq cr6,0x82cf1dec\n"
            "\tApplyTerrainFoodBase(ctx.r10, ctx.r30);\n"
            "\tif (ctx.cr6.eq) goto loc_82CF1DEC;"
        ),
    },
]


class HookContractTests(unittest.TestCase):
    def test_completion_bridges_are_no_ops_in_the_shipping_build(self) -> None:
        # The generated code calls every completion bridge in both builds; only a
        # developer build observes.
        completion = tomllib.loads(
            (ROOT / "config/native_completion_probe_hooks.toml").read_text()
        )["midasm_hook"]
        source = (ROOT / "src/gpu/guest_d3d/completion_probe_hooks.cpp").read_text(
            encoding="utf-8"
        )
        shipped = shipping_text(source)
        self.assertNotIn("completion_probe.h", shipped)
        for hook in completion:
            name = hook["name"]
            with self.subTest(hook=name):
                self.assertEqual(source.count(f"void {name}("), 2)
                match = re.search(
                    r"^void " + name + r"\(([^)]*)\)\n\{\n(.*?)\}\n",
                    shipped,
                    re.MULTILINE | re.DOTALL,
                )
                self.assertIsNotNone(match)
                self.assertEqual(match.group(2).strip(), "")
                self.assertEqual(
                    match.group(1).count("PPCRegister&")
                    + match.group(1).count("std::uint64_t"),
                    len(hook.get("registers", [])),
                )

    def test_scene_allocator_wrappers_preserve_original(self) -> None:
        source = (ROOT / "src/gpu/diagnostics/scene_resource_capture.cpp").read_text(
            encoding="utf-8"
        )
        for hook in SCENE_ALLOCATION_HOOKS:
            name = f"sub_{hook['address']:08X}"
            body = source.split(f"REX_HOOK_RAW({name})", 1)[1].split("REX_HOOK_RAW", 1)[
                0
            ]
            self.assertIn(f"__imp__{name}(ctx, base)", body)
            self.assertIn("sceneCoverageSession.load", body)
            for register in hook["registers"]:
                self.assertIn(f"ctx.{register}.u32", body)

    def test_guest_api_census_entries_have_one_counting_hook(self) -> None:
        table = GUEST_API_CENSUS_TABLE.read_text(encoding="utf-8")
        lines = [
            line for line in table.splitlines() if line.startswith("GUEST_API_ENTRY(")
        ]
        entries = re.findall(
            r'^GUEST_API_ENTRY\(([0-9A-F]{8}), (\w+), "(\w+)", (\w+)\)$',
            table,
            re.M,
        )
        self.assertTrue(entries)
        self.assertEqual(len(entries), len(lines))
        route = dict(
            re.findall(
                r'^ROUTE_ENTRY\(([0-9A-F]{8}), "[^"]*", (\w+)\)$',
                ROUTE_TRACE_TABLE.read_text(encoding="utf-8"),
                re.M,
            )
        )
        sources = {
            path: path.read_text(encoding="utf-8")
            for path in sorted((ROOT / "src").rglob("*.cpp"))
        }
        # The hook holds the returned scope while Original runs.
        call = re.compile(
            r"const auto\s+guestApiCall\s*=[\s\\]*"
            r"rerevved::gpu::guest_d3d::CountGuestApiCall<0x(#+address|[0-9A-F]{8})>\(\);"
        )

        def macro(path: Path, name: str) -> str:
            text = sources[ROOT / path]
            body = re.search(
                rf"^#define {name}\(address\)((?:[^\n]*\\\n)*[^\n]*)$", text, re.M
            )
            self.assertIsNotNone(body, name)
            return body.group(1)

        # Each macro-generated hook counts its own address exactly once.
        route_path = Path("src/gpu/guest_d3d/route_hooks.cpp")
        route_hook = macro(route_path, "ROUTE_HOOK_New")
        # The developer-build body: the route trace, the effect reader and the
        # scene capture around Original. The shipping body only runs Original.
        route_run = macro(route_path, "ROUTE_HOOK_RUN")
        census_path = Path("src/gpu/guest_d3d/guest_api_hooks.cpp")
        census_hook = macro(census_path, "GUEST_API_HOOK_Census")
        for body in (route_hook, census_hook):
            self.assertEqual(body.count("CountGuestApiCall"), 1)
            self.assertEqual(call.findall(body), ["##address"])
        self.assertIn("ROUTE_HOOK_RUN(address)", route_hook)
        self.assertIn("ROUTE_HOOK_BODY(address)", route_run)
        self.assertEqual(
            macro(route_path, "ROUTE_HOOK_BODY").count(
                "__imp__sub_##address(ctx, base)"
            ),
            2,
        )
        self.assertIn("TraceRouteEntry(", route_run)
        self.assertIn("__imp__sub_##address(ctx, base)", census_hook)
        # Only the Clear wrapper may skip its original, after the entry observer
        # stacked the call and before Original runs in the census hooks; the consume is constexpr-gated
        # on that one address and counted once like any other hook.
        observe = census_hook.index("ObserveGuestProgress<0x##address>(ctx)")
        consume = census_hook.index("ConsumeGuestClear(")
        original = census_hook.index("__imp__sub_##address(ctx, base)")
        self.assertLess(observe, consume)
        self.assertLess(consume, original)
        self.assertEqual(census_hook.count("ConsumeGuestClear("), 1)
        self.assertIn(
            "if constexpr (rerevved::gpu::guest_d3d::ClearConsumeEntry(0x##address))",
            census_hook,
        )
        self.assertIn("DeveloperRecorderActive()", census_hook[consume:original])
        clear_consume = (ROOT / "src/gpu/guest_d3d/clear_consume.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("kClearEntryAddress = 0x826A2970", clear_consume)
        # The direct draw lowerer may skip its original too: after the draw scope
        # and the shader harvest observer began, before the route trace and the
        # original run, constexpr-gated on the one address, with the same
        # diagnostics gate. Its exit probe is made after the consume, so a
        # consumed call is never probed, and only the hook of that address has one.
        draw_scope = route_hook.index("ObserveNativeDraw<0x##address>(ctx, base)")
        harvest = route_hook.index("ObserveShaderHarvestEntry<0x##address>(ctx)")
        lowerer_consume = route_hook.index("ConsumeLowerer(")
        lowerer_probe = route_hook.index("ObserveLowererExit<0x##address>(ctx)")
        trace = route_hook.index("ROUTE_HOOK_RUN(address)")
        self.assertLess(draw_scope, lowerer_consume)
        self.assertLess(harvest, lowerer_consume)
        self.assertLess(lowerer_consume, lowerer_probe)
        self.assertLess(lowerer_probe, trace)
        self.assertEqual(route_hook.count("ConsumeLowerer("), 1)
        self.assertEqual(route_hook.count("ObserveLowererExit<"), 1)
        self.assertIn(
            "if constexpr (rerevved::gpu::guest_d3d::LowererConsumeEntry(0x##address))",
            route_hook,
        )
        self.assertIn(
            "DeveloperRecorderActive()", route_hook[lowerer_consume:lowerer_probe]
        )
        lowerer_derive = (ROOT / "src/gpu/guest_d3d/lowerer_derive.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("kLowererEntryAddress = 0x826A3048", lowerer_derive)
        # SetPredication may skip its original too, in the census hook after the
        # Clear consume and the progress observer, with the same diagnostics
        # gate and the ring stub check. Its exit probe sits between the consume
        # and the original, so a consumed call is never probed, and only the
        # hook of that address has one.
        predication_consume = census_hook.index("ConsumeSetPredication(")
        predication_probe = census_hook.index(
            "ObserveSetPredicationExit<0x##address>(ctx)"
        )
        self.assertLess(observe, predication_consume)
        self.assertLess(consume, predication_consume)
        self.assertLess(predication_consume, predication_probe)
        self.assertLess(predication_probe, original)
        self.assertEqual(census_hook.count("ConsumeSetPredication("), 1)
        self.assertEqual(census_hook.count("ObserveSetPredicationExit<"), 1)
        self.assertIn(
            "if constexpr (rerevved::gpu::guest_d3d::SetPredicationConsumeEntry(0x##address))",
            census_hook,
        )
        self.assertIn(
            "SetPredicationConsumeRequested() &&",
            census_hook[predication_consume - 200 : predication_consume],
        )
        self.assertIn(
            "RingStubsReady(ctx.r3.u32)",
            census_hook[predication_consume:predication_probe],
        )
        self.assertIn(
            "DeveloperRecorderActive()",
            census_hook[predication_consume:predication_probe],
        )
        set_predication_derive = (
            ROOT / "src/gpu/guest_d3d/set_predication_derive.h"
        ).read_text(encoding="utf-8")
        self.assertIn("kSetPredicationAddress = 0x8269CD20", set_predication_derive)
        self.assertIn(
            "kSetPredicationPreSwapReturn = 0x826A4178", set_predication_derive
        )
        # The pre-swap stub reaches the consume through the hooked symbol at
        # that return address, which the verdict keeps on Original.
        ring_stub_hooks = (ROOT / "src/gpu/guest_d3d/ring_stub_hooks.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "callGuest<sub_8269CD20>(ctx, base, 0x826A4178, g, 0)", ring_stub_hooks
        )
        # The window packet writer has its own hook: the consume first, the exit
        # probe next and Original last, with the diagnostics between the consume
        # and the probe, and one hook for the address.
        writer_hook = (ROOT / "src/gpu/guest_d3d/window_writer_hooks.cpp").read_text(
            encoding="utf-8"
        )
        writer_consume = writer_hook.index("ConsumeWindowWriter(")
        writer_probe = writer_hook.index("WindowWriterExitProbe ")
        writer_original = writer_hook.index("__imp__sub_8268D3A0(ctx, base)")
        self.assertLess(writer_consume, writer_probe)
        self.assertLess(writer_probe, writer_original)
        self.assertEqual(writer_hook.count("ConsumeWindowWriter("), 1)
        self.assertEqual(writer_hook.count("WindowWriterExitProbe "), 1)
        self.assertEqual(writer_hook.count("REX_HOOK_RAW(sub_8268D3A0)"), 1)
        for diagnostic in (
            "DeveloperRecorderActive",
            "ClearDeriveCheckRequested",
            "ResolveDeriveCheckRequested",
            "LowererExitCheckRequested",
        ):
            self.assertIn(diagnostic, writer_hook[writer_consume:writer_probe])
        self.assertEqual(
            sum(text.count("REX_HOOK_RAW(sub_8268D3A0)") for text in sources.values()),
            1,
        )
        writer_derive = (ROOT / "src/gpu/guest_d3d/window_writer_derive.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("kWindowWriterEntryAddress = 0x8268D3A0", writer_derive)
        for name in ("GUEST_API_HOOK_Route", "GUEST_API_HOOK_Raw"):
            self.assertEqual(macro(census_path, name).strip(), "")

        # Literal hook bodies, matched by braces, with the addresses they count.
        literal: dict[str, list[list[str]]] = {}
        counted_in_hooks = 0
        for text in sources.values():
            for match in re.finditer(r"REX_HOOK_RAW\(sub_([0-9A-F]{8})\)", text):
                open_brace = text.index("{", match.end())
                depth, index = 0, open_brace
                while True:
                    depth += {"{": 1, "}": -1}.get(text[index], 0)
                    if depth == 0:
                        break
                    index += 1
                body = text[open_brace : index + 1]
                calls = call.findall(body)
                counted_in_hooks += body.count("CountGuestApiCall")
                self.assertEqual(body.count("CountGuestApiCall"), len(calls))
                literal.setdefault(match.group(1), []).append(calls)
        for address, bodies in literal.items():
            for calls in bodies:
                with self.subTest(hook=address):
                    # A hook counts only itself, at most once.
                    self.assertIn(calls, ([], [address]))

        # No counting call outside a hook body or the two counting macros.
        total = sum(text.count("CountGuestApiCall") for text in sources.values())
        self.assertEqual(total, counted_in_hooks + 2)

        for address, _, _, hook in entries:
            with self.subTest(address=address, hook=hook):
                bodies = literal.get(address, [])
                if hook == "Route":
                    self.assertEqual(route.get(address), "New")
                    self.assertEqual(bodies, [])
                elif hook == "Raw":
                    self.assertNotEqual(route.get(address), "New")
                    self.assertEqual(bodies, [[address]])
                else:
                    self.assertEqual(hook, "Census")
                    self.assertNotIn(address, route)
                    self.assertEqual(bodies, [])

    def test_gamma_observers_preserve_original_setter_when_available(self) -> None:
        paths = chunk_paths()
        if not paths:
            self.skipTest("generated sources are not available")
        for call in (
            "ObserveNativeGuestGammaTable(ctx.r31, ctx.r1);",
            "ObserveNativeGuestPwlGamma(ctx.r31);",
        ):
            expected = (
                "\t// addi r30,r31,15004\n\t"
                + call
                + "\n\tctx.r30.s64 = ctx.r31.s64 + 15004;"
            )
            occurrences = count_per_chunk(expected)
            self.assertEqual(occurrences, 1)

    def test_gamma_observers_wrap_the_original_emitters(self) -> None:
        # The setter hooks feed the gamma observer; each emitter hook brackets
        # its Original with the observer's begin and end.
        source = (ROOT / "src/gpu/guest_d3d/draw_hooks.cpp").read_text(encoding="utf-8")

        def body(signature: str) -> str:
            start = source.index(signature)
            return source[start : source.index("\n}\n", start)]

        self.assertIn(
            "ObserveGammaSetter(r31.u32, r1.u32)",
            body("void ObserveNativeGuestGammaTable("),
        )
        self.assertIn(
            "ObserveGammaPwlSetter(r31.u32)", body("void ObserveNativeGuestPwlGamma(")
        )
        emitter = body("REX_HOOK_RAW(sub_826A7040)")
        begin = emitter.index("ObserveGammaEmissionBegin(")
        original = emitter.index("__imp__sub_826A7040(ctx, base)")
        end = emitter.index("ObserveGammaEmissionEnd(")
        self.assertLess(begin, original)
        self.assertLess(original, end)
        self.assertEqual(emitter.count("ObserveGammaEmissionBegin("), 1)
        pwl_emitter = body("REX_HOOK_RAW(sub_826A7138)")
        pwl_begin = pwl_emitter.index("ObserveGammaPwlEmissionBegin(ctx.r3.u32)")
        pwl_original = pwl_emitter.index("__imp__sub_826A7138(ctx, base)")
        # The PWL emission invalidates the observed table when it returns.
        pwl_end = pwl_emitter.index("ObserveGammaEmissionEnd(observed)")
        self.assertLess(pwl_begin, pwl_original)
        self.assertLess(pwl_original, pwl_end)

    def test_gamma_cvars_gate_the_swap_path(self) -> None:
        # Apply defaults on under the native renderer only (startup sets the
        # renderer flag before any hook runs) and the observe check defaults
        # off; with apply off the swap keeps the assumed identity ramp, and the
        # assembler adopts a changed table only on apply.
        app = (ROOT / "src/app.cpp").read_text(encoding="utf-8")
        observer = (ROOT / "src/gpu/guest_d3d/gamma_observer.cpp").read_text(
            encoding="utf-8"
        )
        for name, default in (
            ("native_gamma_observe_check", "false"),
            ("native_gamma_apply", "true"),
        ):
            self.assertRegex(
                observer,
                r"REXCVAR_DEFINE_BOOL\(\s*" + name + r",\s*" + default + r",",
            )
        # The check cvar exists in a developer build only; the shipping observe
        # gate is the apply gate.
        self.assertRegex(
            observer,
            r"#if REREVVED_DEVELOPER\s*REXCVAR_DEFINE_BOOL\(\s*native_gamma_observe_check,",
        )
        self.assertRegex(
            observer,
            r"bool GammaObserveRequested\(\) noexcept\s*\{\s*"
            r"#if REREVVED_DEVELOPER\s*"
            r"return REXCVAR_GET\(native_gamma_observe_check\) \|\| GammaApplyRequested\(\);\s*"
            r"#else\s*return GammaApplyRequested\(\);\s*#endif",
        )
        self.assertRegex(
            observer,
            r"bool GammaApplyRequested\(\) noexcept\s*\{\s*"
            r"return nativeRenderer\.load\(std::memory_order_relaxed\) "
            r"&& REXCVAR_GET\(native_gamma_apply\);",
        )
        self.assertEqual(
            len(
                re.findall(
                    r"(?://[^\n]*\s*)?"
                    r"gpu::guest_d3d::SetGammaNativeRenderer\(native\);\s*"
                    r"if \(native\)\s*\{\s*"
                    r'rex::cvar::SetFlagByName\("gpu_plugin", ""\);',
                    app,
                )
            ),
            1,
        )
        for pattern in (
            r"config\.adoptGammaWithoutWork\s*=\s*gpu::guest_d3d::GammaApplyRequested\(\);",
            r"if \(gpu::guest_d3d::GammaObserveRequested\(\)\)\s*"
            r"gammaView = gpu::guest_d3d::PublishedGammaObserver\(\)\.BeginSwap\(gamma\);",
            r"ChooseSwapGamma\(gpu::guest_d3d::GammaApplyRequested\(\), gammaView, gamma\)",
            r"frame\.gammaTable\s*=\s*gammaChoice\.table;",
            r"frame\.gammaAssumed\s*=\s*gammaChoice\.assumed;",
            r"if \(gammaView && result\.status != gpu::guest_d3d::NativeFrameStatus::Failed && "
            r"result\.status != gpu::guest_d3d::NativeFrameStatus::Stopped\)\s*"
            r"gpu::guest_d3d::PublishedGammaObserver\(\)\.EndSwap\(\*gammaView, "
            r"result\.status == gpu::guest_d3d::NativeFrameStatus::Empty\);",
            r"if \(gpu::guest_d3d::GammaObserveRequested\(\)\)\s*"
            r"phases \+= gpu::guest_d3d::FormatGammaCounts\("
            r"gpu::guest_d3d::PublishedGammaObserver\(\)\.Take\(\)\);",
        ):
            self.assertEqual(len(re.findall(pattern, app)), 1, pattern)

    def test_tiling_probe_is_default_off_and_observes_only(self) -> None:
        # native_tiling_quiesce_check is off by default and its hooks only read
        # the guest: the midasm observers forward registers, and the two wrappers
        # hold a scope that runs after the original or the stub.
        probe = (ROOT / "src/gpu/guest_d3d/tiling_probe.cpp").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            probe,
            r"REXCVAR_DEFINE_BOOL\(\s*native_tiling_quiesce_check,\s*false,",
        )
        hooks = (ROOT / "src/rerevved_hooks.cpp").read_text(encoding="utf-8")
        config = {"midasm_hook": compat_hooks()}
        names = [
            hook["name"]
            for hook in config["midasm_hook"]
            if hook["name"].startswith("ObserveTiling")
        ]
        self.assertEqual(len(names), 16)
        for name in names:
            match = re.search(
                r"^void " + name + r"\(([^)]*)\)\n\{\n(.*?)\n\}\n",
                hooks,
                re.MULTILINE | re.DOTALL,
            )
            self.assertIsNotNone(match, name)
            # Shared hooks may forward to both probes, but only observe.
            calls = match.group(2).splitlines()
            # The shipping build keeps the hook and drops the whole body.
            self.assertEqual(calls[0], "#if REREVVED_DEVELOPER", name)
            self.assertEqual(calls[-1], "#endif", name)
            calls = calls[1:-1]
            self.assertTrue(any("::ObserveTiling" in call for call in calls), name)
            for call in calls:
                self.assertIsNotNone(
                    re.fullmatch(
                        r"    rerevved::gpu::guest_d3d::"
                        r"Observe(?:Tiling|Completion)\w+\([^;()]*\);",
                        call,
                    ),
                    f"{name}: {call}",
                )
        # The interpreter entry, ring registration and submit arms ride on hooks
        # that already exist; each probe call precedes the ledger call.
        for function, probe_call, ledger_call in (
            (
                "MarkReplayWorker",
                "ObserveTilingInterpreter();",
                "completion::detail::MarkReplayWorker();",
            ),
            (
                "RecordDeferredKick",
                "NoteTilingSubmitArm(rerevved::gpu::guest_d3d::TilingArm::Deferred);",
                "completion::detail::RecordDeferredKick(",
            ),
            (
                "RecordDirectSubmit",
                "NoteTilingSubmitArm(rerevved::gpu::guest_d3d::TilingArm::Direct);",
                "completion::detail::RecordDirectSubmit(",
            ),
        ):
            hook_body = re.search(
                r"^void " + function + r"\([^)]*\)\n\{\n(.*?)\n\}\n",
                hooks,
                re.MULTILINE | re.DOTALL,
            ).group(1)
            self.assertLess(
                hook_body.index(probe_call), hook_body.index(ledger_call), function
            )
        ring_begin = re.search(
            r"^void CompatRingInitializeBegin\("
            r"\[\[maybe_unused\]\] PPCRegister& r3, \[\[maybe_unused\]\] PPCRegister& r4\)"
            r"\n\{\n(.*?)\n\}\n",
            hooks,
            re.MULTILINE | re.DOTALL,
        ).group(1)
        self.assertTrue(
            ring_begin.startswith(
                "#if REREVVED_DEVELOPER\n"
                "    rerevved::gpu::guest_d3d::ObserveTilingRing(r3.u32, r4.u32);\n"
                "#endif\n"
            )
        )

        stubs = (ROOT / "src/gpu/guest_d3d/ring_stub_hooks.cpp").read_text(
            encoding="utf-8"
        )

        def body(signature: str) -> str:
            start = stubs.index(signature)
            return stubs[start : stubs.index("\n}\n", start)]

        submit = body("REX_HOOK_RAW(sub_8269BD98)")
        scope = "guest::TilingSubmitScope tilingProbe(ctx.r3.u32, ctx.lr, ctx.r7.u32);"
        self.assertEqual(submit.count(scope), 1)
        self.assertLess(submit.index(scope), submit.index("ringStubsActive.load"))
        self.assertLess(
            submit.index("guest::NoteTilingSubmitArm(guest::TilingArm::Direct);"),
            submit.index("RecordDirectSubmit(ctx.r5.u32, ctx.r6.u32)"),
        )
        end_tiling = body("REX_HOOK_RAW(sub_826AC218)")
        scope = "guest::TilingBracketScope tilingProbe(ctx.r3.u32, ctx.r3.u64);"
        self.assertEqual(end_tiling.count(scope), 1)
        self.assertLess(end_tiling.index(scope), end_tiling.index("ObserveTileList"))
        self.assertLess(end_tiling.index(scope), end_tiling.index("endTiling(ctx"))

        # The probe off costs inline flag tests: the scopes test the cached flag in
        # their constructor and destructor and call out of line only when it is set.
        header = (ROOT / "src/gpu/guest_d3d/tiling_probe.h").read_text(encoding="utf-8")
        self.assertRegex(
            header,
            r"inline bool TilingProbeRequested\(\) noexcept\s*\{\s*"
            r"const auto state = tilingProbeState\.load\(std::memory_order_relaxed\);",
        )
        for scope in ("TilingBracketScope", "TilingSubmitScope"):
            match = re.search(
                scope + r"\(std::uint32_t device,[^)]*\) noexcept\s*:(.*?)\n    \}\n"
                r"\s*~" + scope + r"\(\)\s*\{(.*?)\n    \}\n",
                header,
                re.DOTALL,
            )
            self.assertIsNotNone(match, scope)
            self.assertIn("active_(TilingProbeRequested())", match.group(1), scope)
            self.assertRegex(
                match.group(1), r"if \(active_\) \[\[unlikely\]\]\s*Begin\(\);", scope
            )
            self.assertRegex(
                match.group(2), r"if \(active_\) \[\[unlikely\]\]\s*End\(\);", scope
            )

        # An idle observation is sampled before any of the guest words it reads.
        probe = (ROOT / "src/gpu/guest_d3d/tiling_probe.cpp").read_text(
            encoding="utf-8"
        )
        idle = probe[probe.index("void ObserveTilingWorkerIdle(") :]
        idle = idle[: idle.index("\n}\n")]
        self.assertLess(idle.index("probe.Sample()"), idle.index("word(kTilingWorker)"))
        self.assertLess(
            idle.index("word(kTilingWorker)"), idle.index("probe.NoteWorkerIdle(")
        )

        app = (ROOT / "src/app.cpp").read_text(encoding="utf-8")
        self.assertEqual(
            len(
                re.findall(
                    r"if \(gpu::guest_d3d::TilingProbeRequested\(\)\)\s*"
                    r"phases \+= gpu::guest_d3d::FormatTilingProbeCounts\("
                    r"gpu::guest_d3d::PublishedTilingProbe\(\)\.Take\(\)\);",
                    app,
                )
            ),
            1,
        )

    def test_generated_tiling_probe_hook_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        # Each observer runs before the instruction it names, with its registers live.
        for expected in (
            # The interpreter's only return, through its epilogue branch.
            "\t// b 0x827f7cd4\n\tObserveTilingInterpreterExit();\n"
            "\t__restgprlr_27(ctx, base);\n\treturn;\n",
            # The list word read by the interpreter, a branch target.
            "loc_826ABD0C:\n\t// lwz r11,0(r5)\n\tObserveTilingListWord(ctx.r5);\n",
            # The callback call, after mtctr.
            "\t// bctrl \n\tObserveTilingCallback(ctx.r11, ctx.r31);\n"
            "\tctx.lr = 0x826ABD70;\n",
            # The token handler's per-token dispatch, a branch target.
            "loc_826AB770:\n\t// rlwinm r11,r10,0,0,7\n"
            "\tObserveTilingHandlerToken(ctx.r10);\n",
            # A link the handler follows itself.
            "\t// addi r31,r10,4\n\tObserveTilingHandlerLink(ctx.r10);\n"
            "\tctx.r31.s64 = ctx.r10.s64 + 4;\n",
            # The four list-selected stores.
            "\t// stw r9,8(r31)\n\tObserveTilingWrite826AB120(ctx.r31);\n"
            "\tREX_STORE_U32(ctx.r31.u32 + 8, ctx.r9.u32);\n",
            "\t// stw r8,0(r9)\n\tObserveTilingWrite826AB4C8(ctx.r9);\n"
            "\tREX_STORE_U32(ctx.r9.u32 + 0, ctx.r8.u32);\n",
            "\t// stw r7,0(r3)\n\tObserveTilingWrite826AB60C(ctx.r3);\n"
            "\tREX_STORE_U32(ctx.r3.u32 + 0, ctx.r7.u32);\n",
            "\t// stw r6,0(r8)\n\tObserveTilingWrite826AB6FC(ctx.r8);\n"
            "\tREX_STORE_U32(ctx.r8.u32 + 0, ctx.r6.u32);\n",
            # A worker thread parks with its queue empty.
            "\t// addi r28,r26,32\n\tObserveTilingWorkerIdle(ctx.r25);\n",
            # EndTiling's optional-submit guard.
            "\t// cmplwi cr6,r6,0\n\tObserveTilingSubmitGuard(ctx.r6);\n"
            "\tctx.cr6.compare<uint32_t>(ctx.r6.u32, 0, ctx.xer);\n"
            "\t// beq cr6,0x826ac36c\n",
            # The watchdog: the global gate return, the reset store and the trap path.
            "\t// li r3,0\n\tObserveTilingWatchdogGlobal(ctx.r31);\n"
            "\tctx.r3.s64 = 0;\n\t// b 0x826a5bf4\n",
            "\t// stw r9,11008(r31)\n\tObserveTilingWatchdogReset(ctx.r31);\n"
            "\tREX_STORE_U32(ctx.r31.u32 + 11008, ctx.r9.u32);\n",
            "loc_826A5BD4:\n\t// lis r11,-32247\n\tObserveTilingWatchdogZero(ctx.r31);\n",
        ):
            self.assertEqual(generated.count(expected), 1, expected)
        # The entries of the publisher and the watchdog, before the first instruction.
        for function, call in (
            ("sub_8269B900", "ObserveTilingPublisher(ctx.r3);"),
            ("sub_826A5B48", "ObserveTilingWatchdogEnter(ctx.r3);"),
        ):
            expected = (
                f"DEFINE_REX_FUNC({function}) {{\n\tREX_FUNC_PROLOGUE();\n"
                f"\tuint32_t ea{{}};\n\t// mflr r12\n\t{call}\n"
            )
            self.assertEqual(generated.count(expected), 1, call)

    def test_vertex_selection_observer_precedes_restore_when_available(self) -> None:
        paths = chunk_paths()
        if not paths:
            self.skipTest("generated sources are not available")
        expected = (
            "loc_826AE5B8:\n\t// mr r3,r20\n"
            "\tObserveNativeGuestVertexSelection("
            "ctx.r30, ctx.r31, ctx.r15, ctx.r19);\n"
            "\tctx.r3.u64 = ctx.r20.u64;\n"
            "\t// addi r1,r1,256"
        )
        occurrences = count_per_chunk(expected)
        self.assertEqual(occurrences, 1)

    def test_only_verified_hooks_are_configured(self) -> None:
        config = {"midasm_hook": compat_hooks()}

        self.assertCountEqual(config["midasm_hook"], EXPECTED_HOOKS)
        all_hooks = all_configured_hooks()
        addresses = {hook["address"] for hook in all_hooks}
        self.assertEqual(len(addresses), len(all_hooks))

    def test_configured_names_match_compat_hook_functions(self) -> None:
        config = {"midasm_hook": compat_hooks()}
        source = "\n".join(path.read_text(encoding="utf-8") for path in HOOK_SOURCES)
        source_names = set(
            re.findall(r"^void ([A-Z]\w*)[ \t]*\(", source, re.MULTILINE)
        )
        # Hooks with jump_address_on_true return bool and take guest registers.
        source_names.update(
            re.findall(r"^bool ([A-Z]\w*)[ \t]*\(\s*PPCRegister", source, re.MULTILINE)
        )
        draw_hook_source = (ROOT / "src/gpu/guest_d3d/draw_hooks.cpp").read_text(
            encoding="utf-8"
        )
        source_names.update(
            re.findall(
                r"^void (ObserveNativeGuest(?:VertexSelection|GammaTable|PwlGamma))[ \t]*\(",
                draw_hook_source,
                re.MULTILINE,
            )
        )
        hook_names = {hook["name"] for hook in config["midasm_hook"]}

        self.assertEqual(
            source_names - {"CompatNullOptionalDispatch"},
            hook_names,
        )

    def test_combat_identity_uses_record_base_type(self) -> None:
        source = (ROOT / "src" / "unit_combat_rules_hooks.cpp").read_text(
            encoding="ascii"
        )
        identity = source.split("bool tryResolveIdentity", 1)[1].split(
            "bool tryAppendForestCombatLine", 1
        )[0]
        self.assertRegex(
            identity,
            r"tryReadUnitBaseType\(player,\s*unit,\s*baseUnitType\)",
        )
        self.assertRegex(
            identity,
            r"TryResolveUnitIdentity\(\s*"
            r"civilization,\s*baseUnitType,\s*identity\)",
        )
        self.assertIn("unitType = baseUnitType;", identity)
        self.assertNotRegex(
            identity,
            r"TryResolveUnitIdentity\(\s*civilization,\s*unit,\s*identity\)",
        )

    def test_combat_hooks_use_saved_participant_offsets(self) -> None:
        source = (ROOT / "src" / "unit_combat_rules_hooks.cpp").read_text(
            encoding="ascii"
        )
        self.assertIn("kAttackerPlayerOffset = 1572;", source)
        self.assertIn("kAttackerUnitOffset   = 1580;", source)
        self.assertIn("kDefenderPlayerOffset = 1596;", source)
        self.assertIn("kDefenderUnitOffset   = 1604;", source)

    def test_generated_asset_file_override_hook_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        prototype = (
            "extern void ApplyAssetFileOverride(PPCRegister& r1, "
            "PPCRegister& r11, "
            "PPCRegister& r30);"
        )
        if prototype not in generated:
            self.skipTest(
                "generated sources do not include the current hook configuration"
            )
        placement = (
            "\t// lwz r11,84(r1)\n"
            "\tctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 84);\n"
            "\t// cmplwi cr6,r11,0\n"
            "\tApplyAssetFileOverride(ctx.r1, ctx.r11, ctx.r30);\n"
            "\tctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);"
        )
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

    def test_main_menu_logo_hook_preserves_native_fallback(self) -> None:
        source = (ROOT / "src" / "main_menu_logo_hooks.cpp").read_text(encoding="ascii")
        self.assertIn("stackPointer.u32 == 0", source)
        self.assertIn("requestedName.u32 == 0", source)
        self.assertIn("TryGetPayload(payload)", source)
        self.assertIn("kGuestFileName", source)
        self.assertIn("kLogoDdsSize", source)
        self.assertIn("SystemHeapAlloc(allocationSize)", source)
        self.assertIn("std::memcmp(", source)
        self.assertGreaterEqual(source.count("return;"), 4)

    def test_generated_ring_hook_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = (
            "// bl 0x82e923e4\n"
            "\tCompatRingInitializeBegin(ctx.r3, ctx.r4);\n"
            "\tctx.lr = 0x8269CAE4;\n"
            "\t__imp__VdInitializeRingBuffer(ctx, base);\n"
            "\t// rlwinm r11,r25,23,9,31\n"
            "\tCompatRingInitializeEnd();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_generated_replay_hook_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        # Each observer runs before its instruction, with the registers it reads live.
        for expected in (
            "\t// ori r9,r11,22528\n\tRecordGpuTimestamp(ctx.r30, ctx.r31);",
            "loc_8269C4E8:\n\t// oris r10,r30,33024\n"
            "\tRecordTiledSegment(ctx.r29, ctx.r30);",
            "\t// oris r11,r27,33024\n\tRecordDeferredKick(ctx.r28, ctx.r27);",
            "\t// bl 0x8269b900\n\tRecordDirectSubmit(ctx.r28, ctx.r27);",
            "\t// bl 0x8269b900\n\tRecordReplaySubmit826AB890(ctx.r11, ctx.r10);",
            "\t// bl 0x8269b900\n\tRecordReplaySubmit826AB990(ctx.r10, ctx.r11);",
            # Suspend and resume store and clear G+0x2ABE bit 0x2 just before
            # leaving the G+0x3A60 section.
            "\t// bl 0x82e922e4\n\tRecordDeviceSuspend(ctx.r1);\n"
            "\tctx.lr = 0x8268CFE0;",
            "\t// bl 0x82e922e4\n\tRecordDeviceResume();\n\tctx.lr = 0x8268D064;",
            # The worker's deferred swap: begin once the flag test passed, end
            # at the label the swap and the flag-clear path both reach.
            "\t// mr r3,r30\n\tBeginDeferredSwap(ctx.r30);\n"
            "\tctx.r3.u64 = ctx.r30.u64;\n\t// bl 0x826a4630\n",
            "\tsub_826A4638(ctx, base);\nloc_826ABF94:\n\t// mr r3,r29\n"
            "\tEndDeferredSwap(ctx.r30);\n\tctx.r3.u64 = ctx.r29.u64;\n",
            # Device acquire runs after its owner store; release before its own.
            "\tREX_STORE_U32(ctx.r31.u32 + 10888, ctx.r3.u32);\n"
            "\t// addi r1,r1,96\n\tRecordDeviceAcquire();\n",
            "\t// stw r11,10888(r3)\n\tRecordDeviceRelease();\n"
            "\tREX_STORE_U32(ctx.r3.u32 + 10888, ctx.r11.u32);\n",
        ):
            self.assertEqual(generated.count(expected), 1, expected)
        self.assertNotIn("RecordReplayDeferredSwap", generated)
        for function, call in (
            ("sub_826AC0A8", "BeginTiledList();"),
            ("sub_826ABB90", "MarkReplayWorker();"),
        ):
            expected = (
                f"DEFINE_REX_FUNC({function}) {{\n\tREX_FUNC_PROLOGUE();\n"
                f"\tuint32_t ea{{}};\n\t// mflr r12\n\t{call}\n"
            )
            self.assertEqual(generated.count(expected), 1, call)

    def test_generated_display_wait_hook_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        # Each hook runs at its WAIT_REG_MEM header 0xC0043C00, whose body is
        # function 3 (equal) on register 0x1973 = 6515.
        for expected in (
            "\t// li r21,3\n\tctx.r21.s64 = 3;\n"
            "\t// ori r10,r7,15360\n\tRecordDisplayWait826A44E0();\n"
            "\tctx.r10.u64 = ctx.r7.u64 | 15360;\n"
            "\t// li r20,6515\n",
            "\t// lis r8,-16380\n\tctx.r8.s64 = -1073479680;\n"
            "\t// li r7,3\n\tctx.r7.s64 = 3;\n"
            "\t// ori r10,r8,15360\n\tRecordDisplayWait826A45C8();\n"
            "\tctx.r10.u64 = ctx.r8.u64 | 15360;\n"
            "\t// li r8,6515\n",
        ):
            self.assertEqual(generated.count(expected), 1, expected)

    def test_display_waits_record_barriers(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        progress = (
            ROOT / "src" / "gpu" / "completion" / "guest_progress_hooks.cpp"
        ).read_text(encoding="utf-8")
        for site in ("826A44E0", "826A45C8"):
            self.assertRegex(
                source,
                rf"void RecordDisplayWait{site}\(\)\s*\{{\s*"
                rf"rerevved::gpu::completion::detail::RecordDisplayWait\(0x{site}\);\s*\}}",
            )
        body = progress.split("void RecordDisplayWait(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("ledger->Barrier(site);", body)
        self.assertNotIn("Unmodeled", body)

    def test_generated_native_device_observer_preserves_call(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = (
            "\t// stw r31,0(r11)\n"
            "\tREX_STORE_U32(ctx.r11.u32 + 0, ctx.r31.u32);\n"
            "\t// bl 0x82e924c4\n"
            "\tObserveNativeDevicePublication(ctx.r11, ctx.r31);\n"
            "\tctx.lr = 0x826A6464;\n"
            "\t__imp__ExGetXConfigSetting(ctx, base);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_native_device_observer_is_read_only_and_native_gated(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        observer = source.split("void ObserveNativeDevicePublication", 1)[1].split(
            "void RememberGfxRenderConfig", 1
        )[0]
        self.assertIn(
            "SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native",
            observer,
        )
        self.assertIn("isGuestReadableRange", observer)
        self.assertIn("readGuestU32", observer)
        self.assertIn("PublishGuestDevice", observer)
        self.assertNotIn("WriteGuest", observer)

    def test_generated_native_texture_observer_preserves_store(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = (
            "\t// stw r3,0(r19)\n"
            "\tREX_STORE_U32(ctx.r19.u32 + 0, ctx.r3.u32);\n"
            "\t// cmplwi r3,0\n"
            "\tObserveNativeTexturePublication(ctx.r22, ctx.r3);\n"
            "\tctx.cr0.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_native_texture_observer_is_read_only_and_native_gated(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        observer = source.split("void ObserveNativeTexturePublication", 1)[1].split(
            "void RecordGuestClearTarget", 1
        )[0]
        self.assertIn(
            "SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native",
            observer,
        )
        self.assertIn("isGuestReadableRange", observer)
        self.assertIn("readGuestU32", observer)
        self.assertIn("ObserveGuestTexture", observer)
        self.assertNotIn("texture_address, backend_address", observer)
        self.assertNotIn("WriteGuest", observer)

    def test_generated_native_explicit_buffer_observer_placement_when_available(
        self,
    ) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        provider = (
            "loc_8250AE84:\n"
            "\t// stw r27,116(r1)\n"
            "\tObserveNativeResolveProviderIdentity(ctx.r3);\n"
            "\tREX_STORE_U32(ctx.r1.u32 + 116, ctx.r27.u32);"
        )
        factory = (
            "\t// stw r3,0(r29)\n"
            "\tREX_STORE_U32(ctx.r29.u32 + 0, ctx.r3.u32);\n"
            "\t// addi r29,r29,4\n"
            "\tObserveNativeExplicitBufferFactoryStore("
            "ctx.r28, ctx.r29, ctx.r30, ctx.r3);\n"
            "\tctx.r29.s64 = ctx.r29.s64 + 4;"
        )
        for placement in (provider, factory):
            self.assertEqual(generated.count(placement), 1)

    def test_native_explicit_buffer_observers_are_read_only_and_bounded(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        provider = source.split("void ObserveNativeResolveProviderIdentity", 1)[
            1
        ].split("void ObserveNativeExplicitBufferFactoryStore", 1)[0]
        factory = source.split("void ObserveNativeExplicitBufferFactoryStore", 1)[
            1
        ].split("void RecordGuestClearTarget", 1)[0]
        for observer in (provider, factory):
            self.assertIn(
                "SelectedRendererBackend() != rerevved::gpu::RendererBackend::Native",
                observer,
            )
            self.assertIn("isGuestReadableRange", observer)
            self.assertNotIn("WriteGuest", observer)
            self.assertNotIn("TranslateVirtual<uint8_t*>", observer)
        self.assertIn("readGuestU32", source)
        self.assertIn("std::atomic_flag", source)
        self.assertIn("nativeResolveProviderMatchLogCount", source)
        self.assertIn("nativeResolveProviderMismatchLogCount", source)
        self.assertIn("vptr != kExplicitBuffersVtable", provider)
        self.assertIn("vptr_matches", factory)
        self.assertIn("nativeExplicitFactoryLogCount", source)

    def test_generated_swap_observer_preserves_call_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = (
            "\t// bl 0x82e92454\n"
            "\tObserveRendererSwapSource("
            "ctx.r3, ctx.r4, ctx.r30, ctx.r31);\n"
            "\tctx.lr = 0x826A4888;\n"
            "\t__imp__VdSwap(ctx, base);"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_native_present_hook_is_separate_and_cvar_gated(self) -> None:
        grouped = read_hooks(NATIVE_PRESENT_HOOK_CONFIG)
        self.assertEqual(len(grouped), 4)
        hooks = [hook for hook in grouped if hook["name"] == "ReplaceVdSwapCall"]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x826A4880,
                    "name": "ReplaceVdSwapCall",
                    "registers": ["r1", "r3", "r4", "r9", "r10", "r30", "r31"],
                    "after_instruction": True,
                    "jump_address_on_true": 0x826A4888,
                }
            ],
        )
        permanent = compat_hooks()
        self.assertNotIn(0x826A4880, {hook["address"] for hook in permanent})
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/native_present_hooks.toml"'), 1)

        source = NATIVE_PRESENT_SOURCE.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"bool ReplaceVdSwapCall\(PPCRegister& r1,\s*PPCRegister& r3,\s*"
            r"PPCRegister& r4,\s*PPCRegister& r9,\s*PPCRegister& r10,\s*"
            r"PPCRegister& r30,\s*PPCRegister& r31\)",
        )
        gate = source.split("bool ReplaceVdSwapCall", 1)[1]
        # The inactive path is one relaxed load, ahead of any other work.
        self.assertLess(gate.index("ringStubsActive.load"), gate.index("PresentHost"))
        self.assertLess(
            gate.index("ringStubsActive.load"), gate.index("ReplacePresentSwap(")
        )
        # The jump skips the swap observer, so the commit calls it, before
        # OnSwap and only after the preflight.
        self.assertIn("ObserveRendererSwapSource(r3, r4, r30, r31);", source)
        swap = NATIVE_PRESENT_SWAP_SOURCE.read_text(encoding="utf-8")
        commit = swap.split("// The commit:", 1)[1]
        self.assertIn("host.ObserveSource()", commit)
        self.assertIn("delegate->OnSwap", commit)
        self.assertLess(
            commit.index("host.ObserveSource()"), commit.index("delegate->OnSwap")
        )
        self.assertLess(
            swap.index("DecodePresentSwap(*services.memory"),
            swap.index("// The commit:"),
        )

        stubs = "\n".join(
            path.read_text(encoding="utf-8") for path in RING_STUB_SOURCES
        )
        self.assertRegex(stubs, r"REXCVAR_DEFINE_BOOL\(native_ring_stubs,\s*true,")
        self.assertNotIn(
            "REXCVAR_DEFINE_BOOL(native_ring_stubs",
            APP_SOURCE.read_text(encoding="utf-8"),
        )
        # The stubs add raw wrappers only for helpers no other wrapper owns.
        hooked = set(re.findall(r"REX_HOOK_RAW\((sub_[0-9A-F]{8})\)", stubs))
        self.assertEqual(
            hooked,
            {
                "sub_8269C400",
                "sub_8269BD98",
                "sub_826A9BA0",
                "sub_826AC0A8",
                "sub_826AC218",
                "sub_826AB740",
                "sub_8269B118",
            },
        )
        route = ROUTE_TRACE_TABLE.read_text(encoding="utf-8")
        census = GUEST_API_CENSUS_TABLE.read_text(encoding="utf-8")
        for name in hooked:
            address = name.removeprefix("sub_")
            self.assertNotIn(f"ROUTE_ENTRY({address},", route)
            self.assertNotIn(f"GUEST_API_ENTRY({address},", census)
        for address in (
            "8269B1F0",
            "8269B2A0",
            "8269B900",
            "8269BCA8",
            "8269BF40",
            "8269C588",
            "8269C810",
            "826A4150",
            "826AC020",
        ):
            self.assertRegex(route, rf"ROUTE_ENTRY\({address},[^\n]*,\s*New\)")
        # The progress observers record every fence and callback before a stub
        # may replace Original, so a fallback reaches the ledger the same way.
        wrapper = (ROOT / "src/gpu/guest_d3d/route_hooks.cpp").read_text(
            encoding="utf-8"
        )
        wrapper = wrapper.split("#define ROUTE_HOOK_New", 1)[1].split(
            "#define ROUTE_HOOK_Existing", 1
        )[0]
        self.assertEqual(wrapper.count("ObserveGuestProgress<"), 1)
        self.assertLess(
            wrapper.index("ObserveGuestProgress<"), wrapper.index("RunRingStub(")
        )
        # The ledger stays published and is the only completion source.
        service = (ROOT / "src/gpu/guest_gpu_service.cpp").read_text(encoding="utf-8")
        start = service.split("state->progressPublished = true;", 1)[1]
        self.assertLess(
            start.index("completion::PublishGuestProgress(state->ledger);"),
            start.index("PublishRingStubServices("),
        )
        # Only the ledger raises source 1 and writes the scratch block.
        for forbidden in (
            "executeInterrupt",
            "kScratchCallbackOffset",
            "WritePhysical",
        ):
            self.assertNotIn(forbidden, stubs)

    def test_generated_native_present_hook_when_available(self) -> None:
        paths = chunk_paths()
        if not paths:
            self.skipTest("generated sources are not available")

        # Only the absence of generated code skips: the partition that owns
        # 0x826A4884 must exist and carry the replacement.
        owners = [
            text for text in chunk_texts() if "DEFINE_REX_FUNC(sub_826A4638)" in text
        ]
        self.assertEqual(len(owners), 1, "one partition defines sub_826A4638")
        function = owners[0].split("DEFINE_REX_FUNC(sub_826A4638)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertTrue(
            "ReplaceVdSwapCall" in function,
            "sub_826A4638 lacks the ReplaceVdSwapCall replacement",
        )
        replace = (
            "\tctx.r3.s64 = ctx.r30.s64 + 4;\n"
            "\tif (ReplaceVdSwapCall(ctx.r1, ctx.r3, ctx.r4, ctx.r9, ctx.r10, "
            "ctx.r30, ctx.r31)) {\n\t\tgoto loc_826A4888;\n"
        )
        self.assertEqual(function.count(replace), 1)
        swap = function.index("__imp__VdSwap(ctx, base);")
        label = function.index("loc_826A4888:")
        self.assertLess(function.index(replace), swap)
        self.assertLess(swap, label)
        self.assertLess(label, function.index("ctx.r11.s64 = ctx.r30.s64 + 256;"))

    def test_combat_speed_contract(self) -> None:
        source = HOOK_SOURCES[0].read_text(encoding="utf-8")
        app_source = APP_SOURCE.read_text(encoding="utf-8")
        override = source.split("void ApplyCombatPaceOverride", 1)[1].split(
            "void CompatNullOptionalDispatch", 1
        )[0]

        definitions = re.findall(
            r'REXCVAR_DEFINE_STRING\(combat_speed,\s*"normal",\s*'
            r'"ReRevved",\s*"Combat presentation speed"\)\s*'
            r'\.allowed\(\{\s*"normal",\s*"fast"\s*\}\);',
            app_source,
        )
        self.assertEqual(len(definitions), 1)
        self.assertIn('REXCVAR_GET(combat_speed) != "fast"', override)
        self.assertRegex(override, r"constexpr\s+float\s+kNativeStandard\s*=\s*2\.0f;")
        self.assertRegex(override, r"constexpr\s+float\s+kNativeAlternate\s*=\s*1\.5f;")
        self.assertRegex(override, r"constexpr\s+float\s+kNativeFast\s*=\s*0\.5f;")
        self.assertIn(
            "selected != kNativeStandard && selected != kNativeAlternate",
            override,
        )
        self.assertEqual(override.count("writeGuestU32Safely("), 1)
        self.assertNotIn("REXLOG_", override)

    def test_generated_spy_kidnapping_failure_guard_when_available(self) -> None:
        def scan(paths: list[Path]) -> dict[str, str]:
            windows = WindowCollector()
            for lines in chunk_lines(paths):
                for index, line in enumerate(lines):
                    windows.advance(line)
                    if line.strip() == "loc_82CF7090:":
                        windows.start("creation", lines[index - 6 : index + 1], 110)
                    elif line.strip() == "loc_82CF7360:":
                        windows.start("tail", [line], 12)
                windows.end_chunk()
            return windows.windows

        paths, windows = scan_once("spy_kidnapping_failure_guard", scan)
        if not paths:
            self.skipTest("generated sources are not available")
        self.assertEqual(set(windows), {"creation", "tail"})
        creation = windows["creation"]
        self.assertRegex(
            creation,
            r"ctx\.lr = 0x82CF7090;\s+sub_82D11750\(ctx, base\);\s+"
            r"loc_82CF7090:\s+// addi r11,r31,3\s+"
            r"if \(SkipFailedSpyUnitCreation\(ctx\.r3\)\) \{\s+"
            r"goto loc_82CF7360;\s+\}\s+else \{\s+\}\s+"
            r"ctx\.r11\.s64 = ctx\.r31\.s64 \+ 3;",
        )
        order = [
            "SkipFailedSpyUnitCreation(ctx.r3)",
            "ctx.r10.u64 = ctx.r18.u64 + ctx.r3.u64;",
            "REX_STORE_U8(ctx.r29.u32 + ctx.r31.u32, ctx.r23.u8);",
            "REX_STORE_U16(ctx.r30.u32 + ctx.r11.u32, ctx.r8.u16);",
            "sub_82D02390(ctx, base);",
        ]
        positions = [creation.index(token) for token in order]
        self.assertEqual(positions, sorted(positions))
        self.assertRegex(
            windows["tail"],
            r"loc_82CF7360:\s+// lis r11,-32000\s+"
            r"ctx\.r11\.s64 = -2097152000;\s+// lbz r11,-10304\(r11\)\s+"
            r"ctx\.r11\.u64 = REX_LOAD_U8\(ctx\.r11\.u32 \+ -10304\);\s+"
            r"// cmplwi cr6,r11,0\s+"
            r"ctx\.cr6\.compare<uint32_t>\(ctx\.r11\.u32, 0, ctx\.xer\);\s+"
            r"// beq cr6,0x82cf741c\s+if \(ctx\.cr6\.eq\) goto loc_82CF741C;",
        )

    def test_generated_spy_bribery_failure_guard_when_available(self) -> None:
        def scan(paths: list[Path]) -> dict[str, str]:
            windows = WindowCollector()
            for lines in chunk_lines(paths):
                for index, line in enumerate(lines):
                    windows.advance(line)
                    if line.strip() == "loc_82CF7274:":
                        windows.start("creation", lines[index - 6 : index + 1], 42)
                    elif line.strip() == "loc_82CF7354:":
                        windows.start("recount", [line], 12)
                windows.end_chunk()
            return windows.windows

        paths, windows = scan_once("spy_bribery_failure_guard", scan)
        if not paths:
            self.skipTest("generated sources are not available")
        self.assertEqual(set(windows), {"creation", "recount"})
        creation = windows["creation"]
        self.assertRegex(
            creation,
            r"ctx\.lr = 0x82CF7274;\s+sub_82D11750\(ctx, base\);\s+"
            r"loc_82CF7274:\s+// addi r9,r31,3\s+"
            r"if \(SkipFailedSpyUnitCreation\(ctx\.r3\)\) \{\s+"
            r"goto loc_82CF7354;\s+\}\s+else \{\s+\}\s+"
            r"ctx\.r9\.s64 = ctx\.r31\.s64 \+ 3;",
        )
        order = [
            "SkipFailedSpyUnitCreation(ctx.r3)",
            "ctx.r7.u64 = ctx.r18.u64 + ctx.r3.u64;",
            "ctx.r11.s64 = static_cast<int64_t>(ctx.r7.u64 * static_cast<uint64_t>(84));",
            "REX_STORE_U8(ctx.r11.u32 + ctx.r31.u32, ctx.r8.u8);",
            "REX_STORE_U8(ctx.r11.u32 + ctx.r10.u32, ctx.r6.u8);",
        ]
        positions = [creation.index(token) for token in order]
        self.assertEqual(positions, sorted(positions))
        self.assertRegex(
            windows["recount"],
            r"loc_82CF7354:\s+// li r4,1\s+ctx\.r4\.s64 = 1;\s+"
            r"// li r3,1\s+ctx\.r3\.s64 = 1;\s+"
            r"// bl 0x82d11ad8\s+ctx\.lr = 0x82CF7360;\s+"
            r"sub_82D11AD8\(ctx, base\);\s+loc_82CF7360:",
        )

    def test_generated_combat_speed_hook_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = (
            "\tctx.lr = 0x82D7F934;\n"
            "\tsub_82D66B20(ctx, base);\n"
            "\t// lwz r11,-3448(r20)\n"
            "\tApplyCombatPaceOverride();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_live_signin_is_an_application_default(self) -> None:
        app = APP_SOURCE.read_text(encoding="utf-8")
        setup = app[app.index("bool App::SetupEnvironment()") :].split("\n}\n", 1)[0]
        self.assertIn(
            'rex::cvar::SetFlagAsApplicationDefault("live_signin", "true");', setup
        )
        self.assertNotIn('SetFlagByName("live_signin"', app)

    def test_great_general_fix_is_bounded_and_always_on(self) -> None:
        source = GENERAL_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("great_general_attachment_fix", source)
        self.assertNotIn("REXCVAR_", source)
        self.assertNotIn("FixEnabled", source)
        self.assertRegex(
            source,
            r"void FixGreatGeneralBorderCompletion\(\)\s*"
            r"\{\s*repairAllPairs\(\);\s*\}",
        )
        self.assertRegex(
            source,
            r"void FixGreatGeneralPostCombat\(PPCRegister& player,\s*"
            r"PPCRegister& unit\)\s*\{\s*"
            r"repairPairsForCarrier\(player\.s32, unit\.s32\);\s*\}",
        )
        for offset in ["0x00", "0x01", "0x0C", "0x1C", "0x1E", "0x50"]:
            self.assertIn(offset, source)
        self.assertNotIn("REX_STORE_", source)
        self.assertEqual(source.count("TranslateVirtual<uint8_t*>"), 1)
        self.assertEqual(source.count("writeCoordinates("), 2)
        self.assertNotIn("REXLOG_", source)
        self.assertNotIn("0x26", source)
        self.assertNotIn("unload", source.lower())

    def test_generated_great_general_fix_placement_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        placements = [
            (
                "loc_82CBF534:\n"
                "\t// lwz r11,112(r1)\n"
                "\tFixGreatGeneralBorderCompletion();"
            ),
            (
                "\tctx.lr = 0x82CDFA64;\n"
                "\tsub_82CD69B8(ctx, base);\n"
                "\t// b 0x82ce16ac\n"
                "\tFixGreatGeneralPostCombat(ctx.r31, ctx.r15);"
            ),
            (
                "loc_82CDFCC4:\n"
                "\t// lbzx r11,r30,r28\n"
                "\tFixGreatGeneralPostCombat(ctx.r31, ctx.r26);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)
        self.assertNotIn("ReRevvedProbeGreatGeneral", generated)

    def test_generated_rush_cost_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        placements = [
            (
                "\t// cmpwi cr6,r30,29\n"
                "\tFixRushCostDisplay(ctx.r27, ctx.r30, ctx.r31, "
                "ctx.r6, ctx.r7, ctx.r11);"
            ),
            (
                "\t// cmpwi cr6,r25,119\n"
                "\tFixRushCostApply(ctx.r25, ctx.r26, ctx.r28, "
                "ctx.r3, ctx.r6, ctx.r8);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)

    def test_generated_unique_unit_rule_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        placements = [
            (
                "\t// cmpwi cr6,r27,0\n"
                "\tApplyUniqueUnitBaseAttack("
                "ctx.r28, ctx.r29, ctx.r27);\n"
                "\tctx.cr6.compare<int32_t>(ctx.r27.s32, 0, ctx.xer);"
            ),
            (
                "\t// cmpwi cr6,r5,0\n"
                "\tApplyUniqueUnitBaseDefense("
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
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        placements = [
            (
                "\t// bl 0x82cfc0a8\n"
                "\tApplyUnitCombatAttackPercent(ctx.r1, ctx.r16);\n"
                "\tctx.lr = 0x82CDABC0;\n"
                "\tsub_82CFC0A8(ctx, base);"
            ),
            (
                "\t// bl 0x82cfbed0\n"
                "\tApplyUnitCombatDefensePercent(ctx.r1, ctx.r17);\n"
                "\tctx.lr = 0x82CDAC14;\n"
                "\tsub_82CFBED0(ctx, base);"
            ),
        ]
        for placement in placements:
            self.assertEqual(generated.count(placement), 1)
        self.assertNotIn("ApplyUnitCombatAttackPercent(ctx.r1, ctx.r26)", generated)
        self.assertNotIn("ApplyUnitCombatDefensePercent(ctx.r1, ctx.r23)", generated)

    def test_terrain_yield_hook_addresses_are_collision_free(self) -> None:
        hooks = compat_hooks()

        terrain_names = {site["name"] for site in TERRAIN_YIELD_HOOK_SITES}
        terrain_hooks = [hook for hook in hooks if hook["name"] in terrain_names]
        expected = [
            {
                "address": 0x82CF18B4,
                "name": "ApplyTerrainTradeBase",
                "registers": ["r9", "r29"],
            },
            {
                "address": 0x82CF1BAC,
                "name": "ApplyTerrainProductionBase",
                "registers": ["r10", "r31"],
            },
            {
                "address": 0x82CF1D9C,
                "name": "ApplyTerrainFoodBase",
                "registers": ["r10", "r30"],
            },
        ]
        self.assertEqual(terrain_hooks, expected)

    def test_generated_terrain_yield_hook_placements_when_available(self) -> None:
        paths = chunk_paths()
        if not paths:
            self.skipTest("generated sources are not available")

        sources = {}
        for source in chunk_texts():
            for site in TERRAIN_YIELD_HOOK_SITES:
                name = site["function"]
                if f"DEFINE_REX_FUNC({name})" in source:
                    self.assertNotIn(
                        name, sources, f"duplicate generated function: {name}"
                    )
                    sources[name] = source

        for site in TERRAIN_YIELD_HOOK_SITES:
            self.assertIn(site["function"], sources)
            source = sources[site["function"]]
            function_marker = f"DEFINE_REX_FUNC({site['function']})"
            self.assertEqual(source.count(function_marker), 1)
            function = source.split(function_marker, 1)[1].split("DEFINE_REX_FUNC", 1)[
                0
            ]
            # Default-off resource identity observers may sit inside the window.
            function = re.sub(r"\tTrace\w+\([^\n]*\);\n", "", function)
            self.assertEqual(function.count(site["sequence"]), 1)

            registers = ", ".join(
                f"PPCRegister& {register}" for register in site["registers"]
            )
            prototype = f"extern void {site['name']}({registers});"
            self.assertEqual(source.count(prototype), 1)

    def test_generated_unique_era_ability_hook_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        placement = (
            "\t// cmpw cr6,r11,r3\n"
            "\tApplyUniqueEraAbilityCell("
            "ctx.r4, ctx.r9, ctx.r11);\n"
            "\tctx.cr6.compare<int32_t>(ctx.r11.s32, ctx.r3.s32, ctx.xer);"
        )
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF0CB0)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        exact_mode, cumulative_mode = function.split("loc_82CF0D0C:", 1)
        self.assertNotIn("ApplyUniqueEraAbilityCell", exact_mode)
        self.assertEqual(cumulative_mode.count("ApplyUniqueEraAbilityCell"), 1)
        self.assertIn("if (ctx.cr6.eq) goto loc_82CF0D0C;", exact_mode)

    def test_generated_library_gold_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        function = generated.split("DEFINE_REX_FUNC(sub_82D13978)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        # The cache hook follows the label, so every predecessor path reaches it.
        cache = (
            "loc_82D1537C:\n"
            "\t// cmpwi cr6,r15,-1\n"
            "\tApplyChineseLibraryGoldCache("
            "ctx.r1, ctx.r15, ctx.r17, ctx.r27, ctx.r31);\n"
            "\tctx.cr6.compare<int32_t>(ctx.r15.s32, -1, ctx.xer);"
        )
        credit = (
            "\t// stwx r8,r19,r10\n"
            "\tApplyChineseLibraryGoldCredit(ctx.r1, ctx.r8, ctx.r17, ctx.r31);\n"
            "\tREX_STORE_U32(ctx.r19.u32 + ctx.r10.u32, ctx.r8.u32);"
        )
        self.assertEqual(function.count(cache), 1)
        self.assertEqual(function.count(credit), 1)
        self.assertEqual(generated.count("ApplyChineseLibraryGoldCache(ctx."), 1)
        self.assertEqual(generated.count("ApplyChineseLibraryGoldCredit(ctx."), 1)

    def test_generated_stonehenge_religion_hook_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        function = generated.split("DEFINE_REX_FUNC(sub_82D13978)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        # The hook follows the guarded claim-helper call and precedes the
        # first instruction at its return address.
        grant = (
            "\t// bl 0x82d16ba0\n"
            "\tctx.lr = 0x82D16344;\n"
            "\tsub_82D16BA0(ctx, base);\n"
            "\t// lis r11,-31979\n"
            "\tGrantStonehengeReligion(ctx.r23, ctx.r27);\n"
            "\tctx.r11.s64 = -2095775744;"
        )
        self.assertEqual(function.count(grant), 1)
        self.assertEqual(generated.count("GrantStonehengeReligion(ctx."), 1)

    def test_stonehenge_religion_hook_guards_guest_calls(self) -> None:
        source = (ROOT / "src" / "wonder_effect_rules_hooks.cpp").read_text(
            encoding="ascii"
        )
        hook = source.split("void GrantStonehengeReligion", 1)[1]
        self.assertIn("&thread->context()->r23 != &city", hook)
        self.assertRegex(
            hook,
            r"ExecuteTrap\(thread,\s*kTechnologyAvailable,\s*availability,\s*2\)\s*!=\s*0",
        )
        self.assertIn("ExecuteTrap(thread, kTechnologyAcquire, acquisition, 5)", hook)
        self.assertIn("if (granting ||", hook)
        self.assertIn("uint64_t       availability[] = { kReligion, owner };", hook)
        self.assertIn(
            "uint64_t acquisition[] = { owner, kReligion, "
            "static_cast<uint64_t>(query.cityIndex), kWonderRewardReason, "
            "kStateFanOut };",
            hook,
        )
        self.assertIn("constexpr uint64_t kReligion            = 19;", source)
        self.assertIn("constexpr uint64_t kWonderRewardReason  = 5;", source)
        self.assertIn("constexpr uint64_t kStateFanOut         = 1;", source)
        self.assertLess(
            hook.index("kTechnologyAvailable"), hook.index("kTechnologyAcquire")
        )
        self.assertIn("constexpr uint32_t kTechnologyAvailable = 0x82CEF6A8;", source)
        self.assertIn("constexpr uint32_t kTechnologyAcquire   = 0x82D09208;", source)

    def test_era_presentation_buffer_has_native_length_header(self) -> None:
        source = NATION_SELECT_TEXT_SOURCE.read_text(encoding="ascii")
        parser = source.split("bool tryReplaceEraLines", 1)[1].split(
            "} // namespace", 1
        )[0]
        self.assertIn("std::array<const char*, 9>", parser)
        self.assertIn("if (index != 0", parser)
        self.assertNotIn("NATION_SELECT_TEXT_SURFACE_RESERVED_5", parser)
        self.assertIn("NATION_SELECT_TEXT_SURFACE_ERA_HEADING", parser)
        self.assertIn("(index - 1) / 2", parser)
        self.assertIn("(index - 2) / 2", parser)
        publish = source.split("bool tryPublishText", 1)[1].split(
            "bool tryEvaluateEraText", 1
        )[0]
        self.assertIn("includeLengthHeader", publish)
        self.assertIn("writeBigEndianU32", publish)
        self.assertRegex(
            publish,
            r"out\.u64\s*=\s*textAddress;",
        )
        era_hook = source.split("void ApplyEraAbilityNationSelectText", 1)[1].split(
            "void ApplyUniqueUnitNationSelectText", 1
        )[0]
        self.assertRegex(
            era_hook,
            r"tryPublishText\(replacement\.data\(\),\s*"
            r"replacement\.size\(\),\s*true,",
        )

    def test_additional_presentation_hooks_preserve_native_forms(self) -> None:
        source = NATION_SELECT_TEXT_SOURCE.read_text(encoding="ascii")
        for name in [
            "ApplyLeaderNameNationSelectText",
            "ApplyCivilizationNameNationSelectText",
            "ApplyCivilizationTraitNationSelectText",
            "ApplyUniqueUnitSectionHeadingNationSelectText",
        ]:
            self.assertEqual(source.count(f"void {name}"), 1)
        self.assertIn("NATION_SELECT_TEXT_SURFACE_LEADER_NAME", source)
        self.assertIn("NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME", source)
        self.assertIn("NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT", source)
        self.assertIn(
            "NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING",
            source,
        )
        trait_hook = source.split("void ApplyCivilizationTraitNationSelectText", 1)[
            1
        ].split("void ApplyUniqueUnitSectionHeadingNationSelectText", 1)[0]
        self.assertIn("tryReplaceText(traitText", trait_hook)
        self.assertIn("true,\n                   traitTextBuffer", trait_hook)

    def test_generated_presentation_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        unit_placement = (
            "\tctx.lr = 0x82D783B4;\n"
            "\tsub_82E6A430(ctx, base);\n"
            "\t// mr r4,r3\n"
            "\tApplyUniqueUnitNationSelectText("
            "ctx.r3, ctx.r27, ctx.r20);\n"
            "\tctx.r4.u64 = ctx.r3.u64;"
        )
        era_placement = (
            "\tctx.r31.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);\n"
            "\t// addi r29,r25,16\n"
            "\tApplyEraAbilityNationSelectText(ctx.r31, ctx.r20);\n"
            "\tctx.r29.s64 = ctx.r25.s64 + 16;"
        )
        self.assertEqual(generated.count(unit_placement), 1)
        self.assertEqual(generated.count(era_placement), 1)

    def test_generated_unit_movement_hook_preserves_ordinary_return_when_available(
        self,
    ) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        prototype = (
            "extern void ApplyUnitMovementBase(PPCRegister& r30, "
            "PPCRegister& r28, PPCRegister& r3);"
        )
        placement = (
            "\t// add r3,r11,r26\n"
            "\tctx.r3.u64 = ctx.r11.u64 + ctx.r26.u64;\n"
            "\t// addi r1,r1,160\n"
            "\tApplyUnitMovementBase(ctx.r30, ctx.r28, ctx.r3);"
        )
        if prototype not in generated:
            self.skipTest("generated movement hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF1F70)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(function.count("ApplyUnitMovementBase"), 1)
        special_return, ordinary_return = function.split("loc_82CF218C:", 1)
        self.assertNotIn("ApplyUnitMovementBase", special_return)
        self.assertIn("ApplyUnitMovementBase", ordinary_return)

    def test_generated_unit_cost_hook_uses_shared_scalar_when_available(
        self,
    ) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        prototype = (
            "extern void ApplyUnitProductionCostPercent("
            "PPCRegister& r30, PPCRegister& r29, PPCRegister& r28);"
        )
        placement = (
            "loc_82CF1268:\n"
            "\t// mr r3,r28\n"
            "\tApplyUnitProductionCostPercent("
            "ctx.r30, ctx.r29, ctx.r28);\n"
            "\tctx.r3.u64 = ctx.r28.u64;"
        )
        if prototype not in generated:
            self.skipTest("generated unit production cost hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82CF1148)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(function.count("ApplyUnitProductionCostPercent"), 1)
        self.assertIn("ctx.r30.s64 = static_cast<int64_t>", function)
        self.assertIn("ctx.r29.u64 = ctx.r10.u64;", function)
        self.assertIn("ctx.r3.u64 = ctx.r28.u64;", function)

    def test_ai_economy_hooks_follow_native_cost_composition(self) -> None:
        self.assertEqual(
            read_hooks(ROOT / "config" / "ai_economy_hooks.toml"),
            [
                {
                    "address": 0x82CF1058,
                    "name": "ApplyAiResearchCostPercent",
                    "registers": ["r25", "r26", "r3"],
                },
                {
                    "address": 0x82CF126C,
                    "name": "ApplyAiUnitProductionCostPercent",
                    "registers": ["r30", "r29", "r3"],
                },
                {
                    "address": 0x82CF154C,
                    "name": "ApplyAiNonUnitProductionCostPercent",
                    "registers": ["r29", "r27", "r31"],
                },
            ],
        )
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")
        for owner, hook, before, after in (
            (
                "82CF0EE8",
                "ApplyAiResearchCostPercent(ctx.r25, ctx.r26, ctx.r3);",
                "// mullw r3,r11,r30",
                "ctx.r1.s64 = ctx.r1.s64 + 160;",
            ),
            (
                "82CF1148",
                "ApplyAiUnitProductionCostPercent(ctx.r30, ctx.r29, ctx.r3);",
                "ctx.r3.u64 = ctx.r28.u64;",
                "ctx.r1.s64 = ctx.r1.s64 + 128;",
            ),
            (
                "82CF1278",
                "ApplyAiNonUnitProductionCostPercent(ctx.r29, ctx.r27, ctx.r31);",
                "loc_82CF154C:",
                "ctx.r3.u64 = ctx.r31.u64;",
            ),
        ):
            function = generated.split(f"DEFINE_REX_FUNC(sub_{owner})", 1)[1]
            function = function.split("DEFINE_REX_FUNC", 1)[0]
            self.assertEqual(function.count(hook), 1)
            self.assertLess(function.index(before), function.index(hook))
            self.assertIn(hook + "\n\t" + after, function)
            if owner == "82CF1148":
                self.assertLess(
                    function.index("ApplyUnitProductionCostPercent("),
                    function.index(before),
                )
            if owner == "82CF1278":
                self.assertIn("goto loc_82CF1550;", function)

    def test_generated_unit_effect_hook_preserves_native_creation_gate_when_available(
        self,
    ) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        prototype = (
            "extern void ApplyUnitEffectCreationGrants("
            "PPCRegister& r26, PPCRegister& r28, PPCRegister& r30);"
        )
        placement = (
            "loc_82D15B84:\n"
            "\t// lwz r11,116(r1)\n"
            "\tApplyUnitEffectCreationGrants(ctx.r26, ctx.r28, ctx.r30);\n"
            "\tctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 116);"
        )
        if prototype not in generated:
            self.skipTest("generated unit effect hook is not available")
        self.assertEqual(generated.count(prototype), 1)
        self.assertEqual(generated.count(placement), 1)

        function = generated.split("DEFINE_REX_FUNC(sub_82D13978)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        self.assertEqual(function.count("ApplyUnitEffectCreationGrants"), 1)
        self.assertIn("ctx.r3.s64 = 50;", function)
        self.assertIn("cmpwi cr6,r8,0", function)
        self.assertIn("cmpwi cr6,r10,2", function)

    def test_unit_effect_hook_resolves_catalog_identity(self) -> None:
        source = (ROOT / "src" / "unit_effect_rules_hooks.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("unit_catalog::TryResolveUnitIdentity(", source)
        self.assertIn("identity == UNIT_IDENTITY_BASE", source)

    def test_generated_horseback_riding_consumer_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        function = generated.split("DEFINE_REX_FUNC(sub_82D1EAB0)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        placements = [
            (
                "\t// lwz r11,72(r31)\n"
                "\tBeginHorsebackRidingOwnershipCheck(ctx.r31);\n"
                "\tctx.r11.u64 = REX_LOAD_U32(ctx.r31.u32 + 72);\n"
                "\t// slw r10,r10,r26\n"
                "\tEndHorsebackRidingOwnershipCheck(ctx.r31);"
            ),
            (
                "\t// bl 0x82cf0cb0\n"
                "\tSelectHorsebackRidingAbility(ctx.r3);\n"
                "\tctx.lr = 0x82D212A8;\n"
                "\tsub_82CF0CB0(ctx, base);"
            ),
            (
                "\t// mr r3,r26\n"
                "\tSelectHorsebackRidingTechnology(ctx.r4);\n"
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
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        function = generated.split("DEFINE_REX_FUNC(sub_82D1B400)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        placement = (
            "\t// cmpwi cr6,r10,14\n"
            "\tApplyBarbarianVillageCityReplacement(ctx.r10);\n"
            "\tctx.cr6.compare<int32_t>(ctx.r10.s32, 14, ctx.xer);"
        )
        self.assertEqual(function.count(placement), 1)
        self.assertIn("if (!ctx.cr6.eq) goto loc_82D1B8BC;", function)
        fallback = function.split("loc_82D1B8BC:", 1)[1]
        self.assertIn("ctx.r10.s64 = -2096168960;", fallback)

    def test_research_prompt_hooks_are_separate_and_default_off(self) -> None:
        grouped = read_hooks(RESEARCH_PROMPT_HOOK_CONFIG)
        self.assertEqual(len(grouped), 13)
        hooks = [hook for hook in grouped if "ResearchPrompt" in hook["name"]]
        arm = {"name": "ArmAutomaticResearchPrompt", "registers": ["r1"]}
        disarm = {"name": "DisarmAutomaticResearchPrompt"}
        self.assertEqual(
            hooks,
            [
                # Turn-start check, village visit, city change technology reward.
                {"address": 0x82D1D604, **arm},
                {"address": 0x82D1D608, **disarm},
                {"address": 0x82D0F09C, **arm},
                {"address": 0x82D0F0A4, **disarm},
                {"address": 0x82D1C3A8, **arm},
                {"address": 0x82D1C3B0, **disarm},
                {
                    "address": 0x82CED0CC,
                    "name": "HideAutomaticResearchPrompt",
                    "registers": ["r1"],
                },
                {
                    "address": 0x82CE83A4,
                    "name": "SuppressResearchPromptShow",
                    "registers": ["r1"],
                    "jump_address_on_true": 0x82CE87A8,
                },
            ],
        )
        # Special Powers > Research a New Technology (sub_82D259E8, menu action
        # 0x54) is player-invoked: neither its call nor its return is hooked.
        self.assertTrue(
            {0x82D29B94, 0x82D29B98}.isdisjoint({hook["address"] for hook in hooks})
        )
        permanent = compat_hooks()
        self.assertTrue(
            {hook["address"] for hook in permanent}.isdisjoint(
                {hook["address"] for hook in hooks}
            )
        )
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/advisor_hooks.toml"'), 1)

        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertIn("REXCVAR_DEFINE_BOOL(hide_research_prompt, false,", app)
        source = RESEARCH_PROMPT_SOURCE.read_text(encoding="utf-8")
        for hook in hooks:
            self.assertIn(hook["name"] + "(", source)
        self.assertIn(
            "gate.BeginShow(REXCVAR_GET(hide_research_prompt), stack.u32)", source
        )

    def test_generated_research_prompt_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        expected = [
            "\t// bl 0x82ceccd8\n\tArmAutomaticResearchPrompt(ctx.r1);\n"
            "\tctx.lr = 0x82D1D608;\n\tsub_82CECCD8(ctx, base);\n",
            "\t// lhz r11,-3640(r24)\n\tDisarmAutomaticResearchPrompt();\n",
            "\t// bl 0x82ceccd8\n\tArmAutomaticResearchPrompt(ctx.r1);\n"
            "\tctx.lr = 0x82D0F0A0;\n\tsub_82CECCD8(ctx, base);\n",
            "\t// mr r4,r23\n\tDisarmAutomaticResearchPrompt();\n",
            "\t// bl 0x82ceccd8\n\tArmAutomaticResearchPrompt(ctx.r1);\n"
            "\tctx.lr = 0x82D1C3AC;\n\tsub_82CECCD8(ctx, base);\n",
            "\t// li r3,1\n\tDisarmAutomaticResearchPrompt();\n",
            "\t// bl 0x82ce8338\n\tHideAutomaticResearchPrompt(ctx.r1);\n"
            "\tctx.lr = 0x82CED0D0;\n",
            "\t// rlwinm r11,r31,0,1,1\n"
            "\tif (SuppressResearchPromptShow(ctx.r1)) {\n"
            "\t\tgoto loc_82CE87A8;\n",
            "loc_82CE87A8:\n\t// lis r11,-31985\n",
            # The player-invoked caller keeps its native call.
            "\t// bl 0x82ceccd8\n\tctx.lr = 0x82D29B98;\n\tsub_82CECCD8(ctx, base);\n",
        ]
        for sequence in expected:
            self.assertEqual(generated.count(sequence), 1, sequence)

    def test_advisor_popup_hooks_are_separate_and_default_off(self) -> None:
        grouped = read_hooks(ADVISOR_POPUP_HOOK_CONFIG)
        self.assertEqual(len(grouped), 13)
        hooks = [hook for hook in grouped if hook["name"] == "SuppressAdvisorPopupShow"]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x82CE83A8,
                    "name": "SuppressAdvisorPopupShow",
                    "registers": ["r12"],
                    "jump_address_on_true": 0x82CE87A8,
                }
            ],
        )
        others = compat_hooks()
        self.assertNotIn(0x82CE83A8, {hook["address"] for hook in others})
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/advisor_hooks.toml"'), 1)

        table = ADVISOR_POPUP_TABLE.read_text(encoding="utf-8")
        cvars = re.findall(r'"(hide_popup_[a-z_]+)"', table)
        self.assertEqual(len(cvars), 15)
        app = APP_SOURCE.read_text(encoding="utf-8")
        source = ADVISOR_POPUP_SOURCE.read_text(encoding="utf-8")
        self.assertIn("SuppressAdvisorPopupShow(", source)
        for index, cvar in enumerate(cvars):
            self.assertIn(f"REXCVAR_DEFINE_BOOL({cvar}, false,", app)
            self.assertRegex(source, rf"case {index}:\s+return REXCVAR_GET\({cvar}\);")

    def test_generated_advisor_popup_hook_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        function = generated.split("DEFINE_REX_FUNC(sub_82CE8338)", 1)[1]
        function = function.split("DEFINE_REX_FUNC", 1)[0]
        research = (
            "\tif (SuppressResearchPromptShow(ctx.r1)) {\n\t\tgoto loc_82CE87A8;\n"
        )
        advisor = "\tif (SuppressAdvisorPopupShow(ctx.r12)) {\n\t\tgoto loc_82CE87A8;\n"
        self.assertEqual(function.count(research), 1)
        self.assertEqual(function.count(advisor), 1)
        # Second, so the research prompt keeps its own decision first.
        self.assertLess(function.index(research), function.index(advisor))
        self.assertLess(function.index(advisor), function.index("loc_82CE87A8:"))

    def test_barbarian_message_hooks_are_separate_and_default_off(self) -> None:
        grouped = read_hooks(BARBARIAN_MESSAGE_HOOK_CONFIG)
        self.assertEqual(len(grouped), 13)
        hooks = [hook for hook in grouped if "Barbarian" in hook["name"]]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x82CD4498,
                    "name": "SkipBarbarianContactMessage",
                    "jump_address_on_true": 0x82CD4738,
                },
                {
                    "address": 0x82CC85DC,
                    "name": "BeginBarbarianLeaderMessage",
                    "registers": ["r1", "r12", "r3"],
                },
                {
                    "address": 0x82CC8800,
                    "name": "EndBarbarianLeaderMessage",
                    "registers": ["r1"],
                },
                {
                    "address": 0x82CE83B0,
                    "name": "SuppressBarbarianLeaderShow",
                    "registers": ["r1"],
                    "jump_address_on_true": 0x82CE87A8,
                },
            ],
        )
        others = compat_hooks()
        self.assertTrue(
            {hook["address"] for hook in others}.isdisjoint(
                {hook["address"] for hook in hooks}
            )
        )
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/advisor_hooks.toml"'), 1)

        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertIn("REXCVAR_DEFINE_BOOL(hide_barbarian_messages, false,", app)
        self.assertIn(
            "REXCVAR_DEFINE_BOOL(hide_village_barbarian_messages, false,", app
        )
        source = BARBARIAN_MESSAGE_SOURCE.read_text(encoding="utf-8")
        for hook in hooks:
            self.assertIn(hook["name"] + "(", source)
        self.assertIn("return REXCVAR_GET(hide_barbarian_messages);", source)
        self.assertIn("REXCVAR_GET(hide_village_barbarian_messages) &&", source)
        table = BARBARIAN_MESSAGE_TABLE.read_text(encoding="utf-8")
        sites = re.search(r"kVillageSites\[\] = \{(.*?)\};", table, re.S)
        self.assertIsNotNone(sites)
        self.assertEqual(len(re.findall(r"0x[0-9A-F]{8}", sites.group(1))), 9)

    def test_generated_barbarian_message_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        contact = generated.split("DEFINE_REX_FUNC(sub_82CCEFA8)", 1)[1]
        contact = contact.split("DEFINE_REX_FUNC", 1)[0]
        skip = (
            "\t// lwz r3,-4128(r27)\n"
            "\tif (SkipBarbarianContactMessage()) {\n\t\tgoto loc_82CD4738;\n"
        )
        self.assertEqual(contact.count(skip), 1)
        self.assertIn("loc_82CD4738:\n", contact)

        message = generated.split("DEFINE_REX_FUNC(sub_82CC85D0)", 1)[1]
        message = message.split("DEFINE_REX_FUNC", 1)[0]
        begin = "\tBeginBarbarianLeaderMessage(ctx.r1, ctx.r12, ctx.r3);\n"
        end = (
            "\tctx.lr = 0x82CC8800;\n\tsub_82CE8338(ctx, base);\n"
            "\t// addi r1,r1,1216\n\tEndBarbarianLeaderMessage(ctx.r1);\n"
        )
        self.assertEqual(message.count(begin), 1)
        self.assertEqual(message.count(end), 1)
        self.assertLess(message.index(begin), message.index(end))

        show = generated.split("DEFINE_REX_FUNC(sub_82CE8338)", 1)[1]
        show = show.split("DEFINE_REX_FUNC", 1)[0]
        advisor = "\tif (SuppressAdvisorPopupShow(ctx.r12)) {\n\t\tgoto loc_82CE87A8;\n"
        leader = (
            "\tif (SuppressBarbarianLeaderShow(ctx.r1)) {\n\t\tgoto loc_82CE87A8;\n"
        )
        self.assertEqual(show.count(leader), 1)
        # After the other suppression hooks, which keep their decisions first.
        self.assertLess(show.index(advisor), show.index(leader))
        self.assertLess(show.index(leader), show.index("loc_82CE87A8:"))

    def test_ai_civilizations_hooks_are_default_off(self) -> None:
        with AI_CIV_HOOK_CONFIG.open("rb") as stream:
            hooks = tomllib.load(stream)["midasm_hook"]
        self.assertEqual(
            hooks,
            [
                {"address": 0x82D21FF8, "name": "ConfigureExpandedAiSetup"},
                {
                    "address": 0x82D22918,
                    "name": "PreserveExpandedAiSetup",
                    "registers": ["r11", "r10"],
                },
                {"address": 0x82D86674, "name": "ResetAiCivilizationsOnLoad"},
            ],
        )
        others = compat_hooks()
        self.assertNotIn(0x82D86674, {hook["address"] for hook in others})
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/ai_civilizations_hooks.toml"'), 1)

        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertIn('REXCVAR_DEFINE_INT32(ai_civilizations, 4, "ReRevved",', app)
        # A Gameplay row under Combat Speed: a label, choices, one tab source
        # and one Player audience rule.
        settings = OVERLAY_SETTINGS_SOURCE.read_text(encoding="utf-8")
        self.assertIn('{ "ai_civilizations", "AI Civilizations" }', settings)
        self.assertIn(
            '{ "ai_civilizations", { { "0", "None (Multiplayer)" },', settings
        )
        self.assertIn('{ "", { "combat_speed", "ai_civilizations" } }', settings)
        self.assertIn('{ "ai_civilizations", Audience::kPlayer }', settings)
        self.assertEqual(settings.count('"ai_civilizations"'), 5)

        source = AI_CIV_SOURCE.read_text(encoding="utf-8")
        override = source.split("REX_HOOK_RAW(sub_82D11AD8)", 1)[1]
        self.assertIn("ctx.lr == ai_civs::kNewGameRecountReturn", override)
        self.assertIn("gameplay_session::SetupActive()", override)
        # The host snapshot is resolved before setup, never at the late recount.
        self.assertIn("gameplay_session::ForNetworkGame()", override)
        self.assertIn("settings->aiCivilizations", override)
        self.assertIn("REXCVAR_GET(ai_civilizations)", override)
        self.assertNotIn("WaitForHostData", source)
        self.assertIn("ai_civs::CivilizationMask(count, barbarian) != 0", override)
        self.assertIn("maxAi < barbarian - 1", override)
        self.assertIn("ai_civs::ExpandedAiLimit()", override)
        self.assertLess(
            override.index("removeAiCivilizations(ctx, base, maxAi, source)"),
            override.index("__imp__sub_82D11AD8(ctx, base);"),
        )
        self.assertIn(
            "callGuest(ctx, base, __imp__sub_82D02390, player, unit, 0)", source
        )
        self.assertIn("ResetAiCivilizationsOnLoad(", source)

        setup = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="ascii")
        setup = setup.split("void RunNativeNewGameSetup(", 1)[1]
        native_setup = setup.rindex("__imp__sub_82D25170(context, base);")
        load = setup.split("kind == gameplay_session::SetupKind::Load", 1)[1].split(
            "gameplay_session::LockRuleBoundary()", 1
        )[0]
        self.assertIn("gameplay_session::SetupGate gate(true, false)", load)
        self.assertLess(load.index("__imp__sub_82D25170"), load.index("return;"))
        self.assertNotIn("ApplyNewGame", load)
        self.assertLess(
            setup.index("gameplay_session::LockRuleBoundary()"),
            setup.index("gameplay_session::Prepare(network, setup, mode)"),
        )
        self.assertLess(
            setup.index("gameplay_session::Prepare(network, setup, mode)"), native_setup
        )
        self.assertLess(setup.index("gameplay_session::SetupGate gate("), native_setup)
        self.assertLess(
            setup.index("gameplay_session::ApplyNewGame(snapshot, network, mode)"),
            setup.index("SetupScope scope(standardRules, settings.resources)"),
        )
        self.assertLess(
            setup.index(
                "civilizations(settings.aiCivilizations, standardRules, base, mode)"
            ),
            native_setup,
        )
        self.assertLess(
            native_setup,
            setup.index("ai_civilizations::CommitNewGame();"),
        )
        self.assertIn("ai_civs::NoteNewGameRecount();", override)
        self.assertLess(
            override.index("start_resources::OnNewGameSetup(ctx, base);"),
            override.index("__imp__sub_82D11AD8(ctx, base);"),
        )
        reset = source.split("void ResetAiCivilizationsOnLoad()", 1)[1]
        self.assertTrue(
            reset.lstrip().startswith(
                "{\n    rerevved::gameplay_session::ResetOnLoad();"
            )
        )
        gate = setup.split("REX_HOOK_RAW(sub_82D217C8)", 1)[1]
        self.assertIn("gameplay_session::SetupBlocked()", gate)
        self.assertIn("ctx.r3.s64 = -1;", gate)
        self.assertIn("__imp__sub_82D217C8(ctx, base);", gate)

        table = AI_CIV_TABLE.read_text(encoding="utf-8")
        for constant in (
            "kNewGameRecountReturn = 0x82D25310;",
            "kFarewellReturn = 0x82D0C3A4;",
            "kUnitTable        = 0x830F2BF0;",
        ):
            self.assertIn(constant, table)

        barbarian = BARBARIAN_MESSAGE_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("REXCVAR_GET(ai_civilizations)", barbarian)
        self.assertIn("ai_civs::IsRemovedCivilization(player.u32)", barbarian)

    def test_generated_ai_civilizations_hooks_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        def function(address: int) -> str:
            body = generated.split(f"DEFINE_REX_FUNC(sub_{address:08X})", 1)[1]
            return body.split("DEFINE_REX_FUNC", 1)[0]

        # The recount right after setup, whose return address the override tests.
        recount = (
            "\t// li r3,1\n\tctx.r3.s64 = 1;\n"
            "\t// bl 0x82d11ad8\n\tctx.lr = 0x82D25310;\n\tsub_82D11AD8(ctx, base);\n"
        )
        self.assertEqual(function(0x82D25170).count(recount), 1)

        # The elimination's unit kill and its farewell leader message.
        eliminate = function(0x82D0C1E0)
        kill = "\tctx.lr = 0x82D0C280;\n\tsub_82D02390(ctx, base);\n"
        farewell = "\tctx.lr = 0x82D0C3A4;\n\tsub_82CC85D0(ctx, base);\n"
        self.assertEqual(eliminate.count(kill), 1)
        self.assertEqual(eliminate.count(farewell), 1)

        nested = "\tctx.lr = 0x82CC87A4;\n\tsub_82CC85D0(ctx, base);\n"
        self.assertEqual(function(0x82CC85D0).count(nested), 1)

        reset = (
            "\t// bl 0x82cc4040\n\tResetAiCivilizationsOnLoad();\n"
            "\tctx.lr = 0x82D86678;\n\tsub_82CC4040(ctx, base);\n"
        )
        self.assertEqual(function(0x82D86400).count(reset), 1)

    def test_generated_expanded_ai_setup_placement_when_available(self) -> None:
        paths = chunk_paths()
        if not paths:
            self.skipTest("generated sources are not available")
        # Exact hook windows only: native-only setup research does not consume
        # a generated setup body as evidence for its control flow.
        for placement in (
            "\tctx.lr = 0x82D21FF8;\n\tsub_82CD59A8(ctx, base);\n"
            "\t// lwz r9,0(r18)\n\tConfigureExpandedAiSetup();\n",
            "\t// stw r11,0(r18)\n"
            "\tPreserveExpandedAiSetup(ctx.r11, ctx.r10);\n"
            "\tREX_STORE_U32(ctx.r18.u32 + 0, ctx.r11.u32);\n"
            "\t// stw r10,176(r9)\n",
        ):
            self.assertEqual(count_per_chunk(placement), 1)


VIDEO_SCALER_HOOK_CONFIG = ROOT / "config" / "native_video_hooks.toml"
VIDEO_SCALER_HOOK_SOURCE = ROOT / "src" / "gpu" / "guest_d3d" / "video_scaler_hooks.cpp"
VIDEO_SCALER_SOURCE = ROOT / "src" / "gpu" / "guest_d3d" / "video_scaler_stub.cpp"
CMAKE_LISTS = ROOT / "CMakeLists.txt"


VIDEO_SCALER_ADDRESSES = (0x826B042C, 0x826B0434, 0x826B044C)


class VideoScalerHookContractTests(unittest.TestCase):
    def test_hook_config_is_separate_and_skips_only_the_import(self) -> None:
        grouped = read_hooks(VIDEO_SCALER_HOOK_CONFIG)
        self.assertEqual(len(grouped), 14)
        hooks = [hook for hook in grouped if "NativeVideoScaler" in hook["name"]]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x826B042C,
                    "name": "ReplaceNativeVideoScalerCall",
                    "registers": [
                        "r1",
                        "r3",
                        "r4",
                        "r5",
                        "r6",
                        "r7",
                        "r8",
                        "r9",
                        "r10",
                        "r30",
                        "r31",
                    ],
                    "after_instruction": True,
                    "jump_address_on_true": 0x826B0434,
                },
                {
                    "address": 0x826B0434,
                    "name": "ObserveNativeVideoScalerReturn",
                    "registers": ["r1", "r3", "r30", "r31"],
                },
                {
                    "address": 0x826B044C,
                    "name": "ObserveNativeVideoScalerCopy",
                    "registers": ["r1", "r30", "r31"],
                    "after_instruction": True,
                },
            ],
        )
        for hook in hooks:
            # LR is passed by value to generated hooks and cannot be written.
            self.assertNotIn("lr", hook["registers"])
        permanent = compat_hooks()
        self.assertFalse(
            set(VIDEO_SCALER_ADDRESSES) & {hook["address"] for hook in permanent}
        )
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/native_video_hooks.toml"'), 1)
        # No other hook set touches the scaler call site.
        for path in (ROOT / "config").glob("*.toml"):
            if path == VIDEO_SCALER_HOOK_CONFIG:
                continue
            text = path.read_text(encoding="utf-8", errors="replace").lower()
            for address in VIDEO_SCALER_ADDRESSES:
                self.assertNotIn(f"0x{address:08x}", text, path.name)

    def test_cvars_are_default_off_and_init_only(self) -> None:
        source = VIDEO_SCALER_SOURCE.read_text(encoding="utf-8")
        for name in ("native_video_scaler_stub", "native_video_scaler_exit_check"):
            self.assertRegex(
                source,
                r"REXCVAR_DEFINE_BOOL\(" + name + r",\s*false,",
            )
        self.assertEqual(source.count("Lifecycle::kInitOnly"), 2)
        self.assertNotIn("native_video_scaler", APP_SOURCE.read_text(encoding="utf-8"))
        # The stub needs published ring stub services; the check does not.
        self.assertRegex(
            source,
            r"bool VideoScalerStubRequested\(\) noexcept\s*\{\s*"
            r"return RingStubsPublished\(\)\s*&&\s*"
            r"REXCVAR_GET\(native_video_scaler_stub\);",
        )

    def test_hook_adapters_are_cvar_gated(self) -> None:
        source = VIDEO_SCALER_HOOK_SOURCE.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"bool ReplaceNativeVideoScalerCall\(PPCRegister& r1,\s*"
            r"PPCRegister& r3,\s*PPCRegister& r4,\s*PPCRegister& r5,\s*"
            r"PPCRegister& r6,\s*PPCRegister& r7,\s*PPCRegister& r8,\s*"
            r"PPCRegister& r9,\s*PPCRegister& r10,\s*PPCRegister& r30,\s*"
            r"PPCRegister& r31\)",
        )
        replace = source.split("bool ReplaceNativeVideoScalerCall", 1)[1].split(
            "void ObserveNativeVideoScalerReturn", 1
        )[0]
        self.assertIn("VideoScalerActive()", replace)
        self.assertLess(
            replace.index("VideoScalerActive()"),
            replace.index("ReplaceVideoScalerCall("),
        )
        # The replacement writes only r3, through its by-reference argument.
        self.assertEqual(replace.count("r3.u64"), 2)
        for observer in (
            "ObserveNativeVideoScalerReturn",
            "ObserveNativeVideoScalerCopy",
        ):
            body = source.split("void " + observer, 1)[1]
            self.assertIn("VideoScalerStubReportingRequested()", body)
            self.assertLess(
                body.index("VideoScalerStubReportingRequested()"),
                body.index("guest::Observe"),
            )
        self.assertNotIn(".lr", source)

    def test_app_drains_the_counters(self) -> None:
        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(app.count("ReportVideoScalerStub(true);"), 2)
        self.assertEqual(app.count("ReportVideoScalerStub()"), 1)
        before = app.split("ReportVideoScalerStub()", 1)[0]
        self.assertIn("VideoScalerStubReportingRequested()", before[-200:])
        self.assertEqual(app.count('#include "gpu/guest_d3d/video_scaler_stub.h"'), 1)

    def test_generated_call_site_keeps_the_copy_and_the_cursor_store(self) -> None:
        chunks = generated_chunks()
        paths = chunks.paths
        if not paths:
            self.skipTest("generated sources are not available")
        body = None
        for path in paths:
            text = chunks.chunk(path)
            marker = "DEFINE_REX_FUNC(sub_826B02B0)"
            if marker in text:
                body = text.split(marker, 1)[1].split("DEFINE_REX_FUNC(", 1)[0]
                break
        if body is None:
            self.skipTest("sub_826B02B0 is not in the generated sources")
        # The retained sequence, in order, whether or not the hooks are generated.
        retained = [
            "// mr r3,r25",
            "__imp__VdInitializeScalerCommandBuffer(ctx, base);",
            "loc_826B0434:",
            "// rlwinm r29,r3,2,0,29",
            "ctx.lr = 0x826B0448;",
            "sub_827F8550(ctx, base);",
            "// stw r11,48(r31)",
            "REX_STORE_U32(ctx.r31.u32 + 48, ctx.r11.u32);",
        ]
        last = -1
        for needle in retained:
            self.assertEqual(body.count(needle), 1, needle)
            self.assertGreater(body.index(needle), last, needle)
            last = body.index(needle)
        self.assertEqual(body.count("ctx.lr = 0x826B0434;"), 1)
        if "ReplaceNativeVideoScalerCall" not in body:
            self.skipTest("generated sources predate the scaler hooks")
        replace = body.index("if (ReplaceNativeVideoScalerCall(")
        self.assertLess(body.index("// mr r3,r25"), replace)
        # The hook follows the mr and jumps over only the call that sets LR.
        self.assertLess(replace, body.index("ctx.lr = 0x826B0434;"))
        self.assertIn("goto loc_826B0434;", body[replace : replace + 400])
        self.assertLess(body.index("ctx.lr = 0x826B0434;"), body.index("loc_826B0434:"))
        returned = body.index("ObserveNativeVideoScalerReturn(")
        self.assertLess(body.index("loc_826B0434:"), returned)
        self.assertLess(
            returned, body.index("ctx.r29.u64 = __builtin_rotateleft64(ctx.r3.u32")
        )
        copied = body.index("ObserveNativeVideoScalerCopy(")
        self.assertLess(body.index("REX_STORE_U32(ctx.r31.u32 + 48"), copied)
        self.assertLess(copied, body.index("ctx.r1.s64 = ctx.r1.s64 + 2464;"))
        self.assertEqual(body.count("ReplaceNativeVideoScalerCall("), 1)


VIDEO_DISPLAY_MODE_HOOK_CONFIG = ROOT / "config" / "native_video_hooks.toml"
VIDEO_DISPLAY_MODE_HOOK_SOURCE = (
    ROOT / "src" / "gpu" / "guest_d3d" / "video_display_mode_hooks.cpp"
)
VIDEO_DISPLAY_MODE_SOURCE = (
    ROOT / "src" / "gpu" / "guest_d3d" / "video_display_mode_stub.cpp"
)
VIDEO_DISPLAY_MODE_ADDRESSES = (0x826A6260, 0x826A6264, 0x826A626C)


def read_generated_function(text: str, name: str) -> str | None:
    """The generated text of one function in one chunk's text, line by line so
    that only its own window is copied."""
    marker = f"DEFINE_REX_FUNC({name})"
    lines: list[str] = []
    inside = False
    for line in io.StringIO(text):
        if not inside:
            inside = marker in line
        elif "DEFINE_REX_FUNC(" in line:
            break
        if inside:
            lines.append(line)
    return "".join(lines) if inside else None


def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def display_mode_lr_window(body: str) -> str:
    """The generated text where LR is stale after the skipped import call.

    It runs from the label of the continuation to the retained bl that sets LR
    again, so anything LR-reading in it would see the pre-call value."""
    start = body.index("loc_826A6264:")
    end = body.index("ctx.lr = 0x826A626C;", start)
    return body[start:end]


# The only statements the stale-LR window may hold: the label, the retained
# addi and its comments, and one approved observer, which gets no LR.
DISPLAY_MODE_OBSERVER = re.compile(
    r"ObserveNativeVideoDisplayModeReturn\(ctx\.r1, ctx\.r3, ctx\.r5, ctx\.r31\);"
)
DISPLAY_MODE_WINDOW_ALLOWED = (
    re.compile(r"loc_826A6264:"),
    re.compile(r"// addi r3,r1,80"),
    re.compile(r"ctx\.r3\.s64 = ctx\.r1\.s64 \+ 80;"),
    re.compile(r"// bl 0x82e924e4"),
    DISPLAY_MODE_OBSERVER,
)


def display_mode_lr_violations(body: str) -> list[str]:
    """Every LR use or unapproved statement in the stale-LR window."""
    window = display_mode_lr_window(body)
    found = [match.group(0) for match in re.finditer(r"\blr\b|\.lr\b", window)]
    lines = [line.strip() for line in window.splitlines() if line.strip()]
    for line in lines:
        if not any(rule.fullmatch(line) for rule in DISPLAY_MODE_WINDOW_ALLOWED):
            found.append("unapproved: " + line)
    if sum(1 for line in lines if DISPLAY_MODE_OBSERVER.fullmatch(line)) > 1:
        found.append("unapproved: a second observer")
    return found


class VideoDisplayModeHookContractTests(unittest.TestCase):
    def test_hook_config_is_separate_and_skips_only_the_import(self) -> None:
        grouped = read_hooks(VIDEO_DISPLAY_MODE_HOOK_CONFIG)
        self.assertEqual(len(grouped), 14)
        hooks = [hook for hook in grouped if "NativeVideoDisplayMode" in hook["name"]]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x826A6260,
                    "name": "ReplaceNativeVideoDisplayModeCall",
                    "registers": ["r1", "r3", "r31", "lr"],
                    "after_instruction": False,
                    "jump_address_on_true": 0x826A6264,
                },
                {
                    "address": 0x826A6264,
                    "name": "ObserveNativeVideoDisplayModeReturn",
                    "registers": ["r1", "r3", "r5", "r31"],
                    "after_instruction": False,
                },
                {
                    "address": 0x826A626C,
                    "name": "ObserveNativeVideoDisplayModeReconverged",
                    "registers": ["r1", "r3", "r5", "r31", "lr"],
                    "after_instruction": False,
                },
            ],
        )
        # Only the entry jumps, and only to the continuation of the skipped bl.
        self.assertEqual(
            [hook["name"] for hook in hooks if "jump_address_on_true" in hook],
            ["ReplaceNativeVideoDisplayModeCall"],
        )
        # The skipped call leaves LR stale until 0x826A626C: the return
        # observer, which runs in that interval, never receives it.
        by_name = {hook["name"]: hook for hook in hooks}
        self.assertNotIn(
            "lr", by_name["ObserveNativeVideoDisplayModeReturn"]["registers"]
        )
        self.assertEqual(
            [hook["name"] for hook in hooks if "lr" in hook["registers"]],
            [
                "ReplaceNativeVideoDisplayModeCall",
                "ObserveNativeVideoDisplayModeReconverged",
            ],
        )
        for hook in hooks:
            self.assertIs(hook["after_instruction"], False)
        permanent = compat_hooks()
        self.assertFalse(
            set(VIDEO_DISPLAY_MODE_ADDRESSES) & {hook["address"] for hook in permanent}
        )
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/native_video_hooks.toml"'), 1)
        # No other hook set touches the display mode call site.
        for path in (ROOT / "config").glob("*.toml"):
            if path == VIDEO_DISPLAY_MODE_HOOK_CONFIG:
                continue
            text = path.read_text(encoding="utf-8", errors="replace").lower()
            for address in VIDEO_DISPLAY_MODE_ADDRESSES:
                self.assertNotIn(f"0x{address:08x}", text, path.name)

    def test_cvars_are_default_off_and_init_only(self) -> None:
        source = VIDEO_DISPLAY_MODE_SOURCE.read_text(encoding="utf-8")
        for name in (
            "native_video_display_mode_stub",
            "native_video_display_mode_exit_check",
        ):
            self.assertRegex(
                source,
                r"REXCVAR_DEFINE_BOOL\(" + name + r",\s*false,",
            )
        self.assertEqual(source.count("Lifecycle::kInitOnly"), 2)
        self.assertNotIn(
            "native_video_display_mode", APP_SOURCE.read_text(encoding="utf-8")
        )
        # The stub needs published ring stub services; the check does not.
        self.assertRegex(
            source,
            r"bool VideoDisplayModeStubRequested\(\) noexcept\s*\{\s*"
            r"return RingStubsPublished\(\)\s*&&\s*"
            r"REXCVAR_GET\(native_video_display_mode_stub\);",
        )
        self.assertRegex(
            source,
            r"bool VideoDisplayModeExitCheckRequested\(\) noexcept\s*\{\s*"
            r"return REXCVAR_GET\(native_video_display_mode_exit_check\);",
        )
        self.assertRegex(
            source,
            r"bool VideoDisplayModeActive\(\) noexcept\s*\{\s*"
            r"return VideoDisplayModeExitCheckRequested\(\)\s*\|\|\s*"
            r"VideoDisplayModeStubRequested\(\);",
        )
        self.assertRegex(
            source,
            r"bool VideoDisplayModeStubReportingRequested\(\) noexcept\s*\{\s*"
            r"return REXCVAR_GET\(native_video_display_mode_stub\)\s*\|\|\s*"
            r"REXCVAR_GET\(native_video_display_mode_exit_check\);",
        )

    def test_entry_binds_the_services_under_the_lease(self) -> None:
        source = strip_cpp_comments(
            VIDEO_DISPLAY_MODE_SOURCE.read_text(encoding="utf-8")
        )
        body = source.split("bool ReplaceVideoDisplayModeCallWith(", 1)[1].split(
            "void ObserveVideoDisplayModeReturn(", 1
        )[0]
        self.assertEqual(body.count("lease.emplace()"), 1)
        for needle in (
            "static_cast<bool>(*lease)",
            "RingStubsReady(device)",
            "operations.environment()",
            "servicesAdmit(state)",
            "providerAdmits(state.environment)",
            "ApplyVideoDisplayModeStub(plan, r3)",
        ):
            self.assertEqual(body.count(needle), 1, needle)
        # The lease is taken before the identities are read and the
        # replacement is committed while it is still held.
        self.assertLess(
            body.index("lease.emplace()"), body.index("operations.environment()")
        )
        self.assertLess(
            body.index("operations.environment()"), body.index("providerAdmits(")
        )
        self.assertLess(
            body.index("providerAdmits("), body.index("ApplyVideoDisplayModeStub(")
        )
        # The exit check reads the services too: no early exit before the lease.
        self.assertLess(
            body.index("lease.emplace()"), body.index("DeriveVideoDisplayModeStub(")
        )
        # The production probe compares the dispatcher target with the SDK export.
        self.assertIn("target == &__imp__VdSetDisplayMode", source)
        self.assertIn("CurrentRingStubServices()", source)
        self.assertIn("kVideoDisplayModeImportAddress", source)

    def test_hook_adapters_are_cvar_gated(self) -> None:
        source = VIDEO_DISPLAY_MODE_HOOK_SOURCE.read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"bool ReplaceNativeVideoDisplayModeCall\(PPCRegister& r1,\s*"
            r"PPCRegister& r3,\s*PPCRegister& r31,\s*uint64_t lr\)",
        )
        self.assertRegex(
            source,
            r"void ObserveNativeVideoDisplayModeReturn\(PPCRegister& r1,\s*"
            r"PPCRegister& r3,\s*PPCRegister& r5,\s*PPCRegister& r31\)",
        )
        self.assertRegex(
            source,
            r"void ObserveNativeVideoDisplayModeReconverged\(PPCRegister& r1,\s*"
            r"PPCRegister& r3,\s*PPCRegister& r5,\s*PPCRegister& r31,\s*uint64_t lr\)",
        )
        replace = source.split("bool ReplaceNativeVideoDisplayModeCall", 1)[1].split(
            "void ObserveNativeVideoDisplayModeReturn", 1
        )[0]
        self.assertIn("VideoDisplayModeActive()", replace)
        self.assertLess(
            replace.index("VideoDisplayModeActive()"),
            replace.index("ReplaceVideoDisplayModeCall("),
        )
        # The replacement writes only r3, through its by-reference argument.
        self.assertEqual(replace.count("r3.u64"), 2)
        self.assertEqual(len(re.findall(r"\br1\.u64\s*=|\br31\.u64\s*=", replace)), 0)
        for observer, successor in (
            (
                "ObserveNativeVideoDisplayModeReturn",
                "ObserveNativeVideoDisplayModeReconverged",
            ),
            ("ObserveNativeVideoDisplayModeReconverged", None),
        ):
            body = source.split("void " + observer, 1)[1]
            if successor:
                body = body.split("void " + successor, 1)[0]
            self.assertIn("VideoDisplayModeStubReportingRequested()", body)
            self.assertLess(
                body.index("VideoDisplayModeStubReportingRequested()"),
                body.index("guest::Observe"),
            )

    def test_return_observer_never_reads_lr(self) -> None:
        adapter = strip_cpp_comments(
            VIDEO_DISPLAY_MODE_HOOK_SOURCE.read_text(encoding="utf-8")
        )
        adapter_body = adapter.split("void ObserveNativeVideoDisplayModeReturn", 1)[
            1
        ].split("void ObserveNativeVideoDisplayModeReconverged", 1)[0]
        self.assertNotRegex(adapter_body, r"(?i)lr")
        stub = strip_cpp_comments(VIDEO_DISPLAY_MODE_SOURCE.read_text(encoding="utf-8"))
        stub_body = stub.split("void ObserveVideoDisplayModeReturn(", 1)[1].split(
            "void ObserveVideoDisplayModeReconverged(", 1
        )[0]
        self.assertNotRegex(stub_body, r"(?i)lr")
        header = strip_cpp_comments(
            (
                ROOT / "src" / "gpu" / "guest_d3d" / "video_display_mode_stub.h"
            ).read_text(encoding="utf-8")
        )
        declaration = re.search(r"void ObserveVideoDisplayModeReturn\([^)]*\)", header)
        self.assertIsNotNone(declaration)
        self.assertNotRegex(declaration.group(0), r"(?i)lr")

    def test_app_drains_the_counters(self) -> None:
        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(app.count("ReportVideoDisplayModeStub(true);"), 2)
        self.assertEqual(app.count("ReportVideoDisplayModeStub()"), 1)
        before = app.split("ReportVideoDisplayModeStub()", 1)[0]
        self.assertIn("VideoDisplayModeStubReportingRequested()", before[-200:])
        self.assertEqual(
            app.count('#include "gpu/guest_d3d/video_display_mode_stub.h"'), 1
        )
        # Each final report sits next to the scaler's, in its own function, so
        # omitting either one is a failure.
        shutdown = app.split("void App::OnShutdown()", 1)[1].split("\nvoid App::", 1)[0]
        hard_exit = app.split("void App::OnBeforeHardExit()", 1)[1].split(
            "\nvoid App::", 1
        )[0]
        for name, body in (("OnShutdown", shutdown), ("OnBeforeHardExit", hard_exit)):
            self.assertEqual(body.count("ReportVideoDisplayModeStub(true);"), 1, name)
            self.assertEqual(body.count("ReportVideoScalerStub(true);"), 1, name)
            self.assertLess(
                body.index("ReportVideoScalerStub(true);"),
                body.index("ReportVideoDisplayModeStub(true);"),
                name,
            )
            gap = body[
                body.index("ReportVideoScalerStub(true);") : body.index(
                    "ReportVideoDisplayModeStub(true);"
                )
            ]
            self.assertEqual(gap.count("\n"), 1, name)

    def test_window_observer_gets_no_lr_register(self) -> None:
        grouped = read_hooks(VIDEO_DISPLAY_MODE_HOOK_CONFIG)
        self.assertEqual(len(grouped), 14)
        hooks = [hook for hook in grouped if "NativeVideoDisplayMode" in hook["name"]]
        in_window = [
            hook for hook in hooks if 0x826A6264 <= hook["address"] < 0x826A626C
        ]
        self.assertEqual(
            [hook["name"] for hook in in_window],
            ["ObserveNativeVideoDisplayModeReturn"],
        )
        self.assertNotIn("lr", in_window[0]["registers"])

    def test_lr_window_checker_finds_every_lr_use(self) -> None:
        clean = (
            "loc_826A6264:\n"
            "\tObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);\n"
            "\t// addi r3,r1,80\n"
            "\tctx.r3.s64 = ctx.r1.s64 + 80;\n"
            "\t// bl 0x82e924e4\n"
            "\tctx.lr = 0x826A626C;\n"
        )
        self.assertEqual(display_mode_lr_violations(clean), [])
        # A hook given LR, a direct read and a sub-expression all fail.
        with_lr_hook = clean.replace("ctx.r31);", "ctx.r31, ctx.lr);")
        self.assertNotEqual(display_mode_lr_violations(with_lr_hook), [])
        direct = clean.replace("// addi", "ctx.r12.u64 = ctx.lr;\n\t// addi")
        self.assertNotEqual(display_mode_lr_violations(direct), [])
        bare = clean.replace("ctx.r31);", "ctx.r31, lr);")
        self.assertNotEqual(display_mode_lr_violations(bare), [])
        # A use after the retained bl or before the label is outside the window.
        outside = clean + "\tctx.r12.u64 = ctx.lr;\n"
        self.assertEqual(display_mode_lr_violations(outside), [])
        before = "ctx.r12.u64 = ctx.lr;\n" + clean
        self.assertEqual(display_mode_lr_violations(before), [])
        # Another hook or call in the window fails even without an LR token.
        other_hook = clean.replace(
            "\t// addi", "\tObserveNativeVideoScalerReturn(ctx.r1);\n\t// addi"
        )
        self.assertNotEqual(display_mode_lr_violations(other_hook), [])
        extra_call = clean.replace("\t// addi", "\tsub_82000000(ctx, base);\n\t// addi")
        self.assertNotEqual(display_mode_lr_violations(extra_call), [])
        second_observer = clean.replace(
            "\t// addi",
            "\tObserveNativeVideoDisplayModeReturn(ctx.r1, ctx.r3, ctx.r5, ctx.r31);\n\t// addi",
        )
        self.assertNotEqual(display_mode_lr_violations(second_observer), [])
        wrong_registers = clean.replace("ctx.r5, ctx.r31", "ctx.r31")
        self.assertNotEqual(display_mode_lr_violations(wrong_registers), [])

    def test_generated_owner_keeps_the_retained_sequence(self) -> None:
        chunks = generated_chunks()
        paths = chunks.paths
        if not paths:
            self.skipTest("generated sources are not available")
        # The owner is in chunk 221; the others are searched only if it moves.
        paths = sorted(paths, key=lambda path: path.name != "rerevved_recomp.221.cpp")
        body = None
        for path in paths:
            body = read_generated_function(chunks.chunk(path), "sub_826A61F0")
            if body is not None:
                break
        if body is None:
            self.skipTest("sub_826A61F0 is not in the generated sources")
        # The retained sequence, in order, whether or not the hooks are generated.
        retained = [
            "loc_826A6260:",
            "// bl 0x82e924f4",
            "ctx.lr = 0x826A6264;",
            "__imp__VdSetDisplayMode(ctx, base);",
            "loc_826A6264:",
            "// addi r3,r1,80",
            "ctx.r3.s64 = ctx.r1.s64 + 80;",
            "ctx.lr = 0x826A626C;",
            "__imp__VdGetCurrentDisplayInformation(ctx, base);",
            "// lhz r11,152(r1)",
            "ctx.r11.u64 = REX_LOAD_U16(ctx.r1.u32 + 152);",
            "REX_STORE_U32(ctx.r31.u32 + 21544, ctx.r11.u32);",
            "REX_STORE_U32(ctx.r31.u32 + 21548, ctx.r10.u32);",
            "REX_STORE_U32(ctx.r31.u32 + 21552, ctx.r9.u32);",
        ]
        last = -1
        for needle in retained:
            self.assertEqual(body.count(needle), 1, needle)
            self.assertGreater(body.index(needle), last, needle)
            last = body.index(needle)
        if "ReplaceNativeVideoDisplayModeCall" not in body:
            self.skipTest("generated sources predate the display mode hooks")
        # The replacement is before the bl, after its comment, and jumps over
        # only the call and the LR assignment that precedes it.
        replace = body.index("if (ReplaceNativeVideoDisplayModeCall(")
        self.assertLess(body.index("loc_826A6260:"), replace)
        self.assertLess(replace, body.index("ctx.lr = 0x826A6264;"))
        self.assertIn("goto loc_826A6264;", body[replace : replace + 400])
        self.assertEqual(body.count("ReplaceNativeVideoDisplayModeCall("), 1)
        # The return observer is at the continuation, before the addi.
        returned = body.index("ObserveNativeVideoDisplayModeReturn(")
        self.assertLess(body.index("loc_826A6264:"), returned)
        self.assertLess(returned, body.index("ctx.r3.s64 = ctx.r1.s64 + 80;"))
        self.assertEqual(body.count("ObserveNativeVideoDisplayModeReturn("), 1)
        # The reconvergence is after the retained information import, before
        # the first lhz.
        reconverged = body.index("ObserveNativeVideoDisplayModeReconverged(")
        self.assertLess(
            body.index("__imp__VdGetCurrentDisplayInformation(ctx, base);"), reconverged
        )
        self.assertLess(
            reconverged, body.index("ctx.r11.u64 = REX_LOAD_U16(ctx.r1.u32 + 152);")
        )
        self.assertEqual(body.count("ObserveNativeVideoDisplayModeReconverged("), 1)
        # No LR use between the continuation and the retained bl that sets it.
        self.assertEqual(display_mode_lr_violations(body), [])
        self.assertNotIn("ctx.lr", display_mode_lr_window(body))


VIDEO_IMPORT_HOOK_CONFIG = ROOT / "config" / "native_video_hooks.toml"
VIDEO_IMPORT_HOOK_SOURCE = (
    ROOT / "src" / "gpu" / "guest_d3d" / "video_import_stub_hooks.cpp"
)
VIDEO_IMPORT_SOURCE = ROOT / "src" / "gpu" / "guest_d3d" / "video_import_stub_table.cpp"
VIDEO_IMPORT_HEADER = ROOT / "src" / "gpu" / "guest_d3d" / "video_import_stub_table.h"
VIDEO_IMPORT_ADDRESSES = (
    0x826A65A4,
    0x826A65A8,
    0x826A65C4,
    0x826A65CC,
    0x826B053C,
    0x826B0540,
    0x826B0564,
    0x826B0568,
)

# The generated text between the continuation of the skipped import and the
# retained bl that sets LR again. Every non-empty line is listed in order: the
# HSIO interval keeps the diagnostic detour, whose own bl sets LR before the
# effect observer.
VIDEO_HSIO_WINDOW = (
    "loc_826A65A8:",
    "// cntlzw r11,r3",
    "ObserveNativeVideoHsioReturn(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13);",
    "ctx.r11.u64 = ctx.r3.u32 == 0 ? 32 : __builtin_clz(ctx.r3.u32);",
    "// rlwinm. r11,r11,27,31,31",
    "ctx.r11.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 27) & 0x1;",
    "ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);",
    "// stw r11,-20316(r30)",
    "REX_STORE_U32(ctx.r30.u32 + -20316, ctx.r11.u32);",
    "// beq 0x826a65c4",
    "if (ctx.cr0.eq) goto loc_826A65C4;",
    "loc_826A65B8:",
    "// lis r11,-32247",
    "ctx.r11.s64 = -2113339392;",
    "// addi r3,r11,1008",
    "ctx.r3.s64 = ctx.r11.s64 + 1008;",
    "// bl 0x826a5350",
    "ctx.lr = 0x826A65C4;",
    "sub_826A5350(ctx, base);",
    "loc_826A65C4:",
    "// mr r3,r31",
    "ObserveNativeVideoHsioEffect(ctx.r1, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.cr0);",
    "ctx.r3.u64 = ctx.r31.u64;",
    "// bl 0x826b0728",
)
VIDEO_WORKER_WINDOW = (
    "loc_826B0540:",
    "// li r11,0",
    "ObserveNativeVideoEdramWorkerReturn(ctx.r1, ctx.r3, ctx.r29, ctx.r30, ctx.r31, ctx.r13);",
    "ctx.r11.s64 = 0;",
    "// li r8,2048",
    "ctx.r8.s64 = 2048;",
    "// stw r11,84(r1)",
    "REX_STORE_U32(ctx.r1.u32 + 84, ctx.r11.u32);",
    "// addi r7,r1,96",
    "ctx.r7.s64 = ctx.r1.s64 + 96;",
    "// stw r11,80(r1)",
    "REX_STORE_U32(ctx.r1.u32 + 80, ctx.r11.u32);",
    "// addi r6,r1,84",
    "ctx.r6.s64 = ctx.r1.s64 + 84;",
    "// li r5,4096",
    "ctx.r5.s64 = 4096;",
    "// addi r4,r1,80",
    "ctx.r4.s64 = ctx.r1.s64 + 80;",
    "// mr r3,r30",
    "ctx.r3.u64 = ctx.r30.u64;",
    "// bl 0x82e92624",
    "ObserveNativeVideoEdramWorkerPreRetrain(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r29, ctx.r30, ctx.r31, ctx.r13);",
)
VIDEO_HSIO_WINDOW_START = "loc_826A65A8:"
VIDEO_HSIO_WINDOW_END = "ctx.lr = 0x826A65CC;"
VIDEO_WORKER_WINDOW_START = "loc_826B0540:"
VIDEO_WORKER_WINDOW_END = "ctx.lr = 0x826B0568;"


def video_import_window(body: str, start: str, end: str) -> str:
    first = body.index(start)
    return body[first : body.index(end, first)]


def video_import_window_violations(
    body: str, start: str, end: str, expected: tuple[str, ...]
) -> list[str]:
    """Every LR token, unlisted statement or reordering in a stale-LR window."""
    window = video_import_window(body, start, end)
    found = [
        "lr token: " + match.group(0)
        for line in window.splitlines()
        if line.strip() not in {"ctx.lr = 0x826A65C4;"}
        for match in re.finditer(r"\blr\b|\.lr\b", line)
    ]
    lines = [line.strip() for line in window.splitlines() if line.strip()]
    if tuple(lines) != expected:
        for line in lines:
            if line not in expected:
                found.append("unapproved: " + line)
        for line in expected:
            if line not in lines:
                found.append("missing: " + line)
        if not found:
            found.append("reordered or duplicated statements")
    return found


def video_hsio_window_violations(body: str) -> list[str]:
    return video_import_window_violations(
        body, VIDEO_HSIO_WINDOW_START, VIDEO_HSIO_WINDOW_END, VIDEO_HSIO_WINDOW
    )


def video_worker_window_violations(body: str) -> list[str]:
    return video_import_window_violations(
        body, VIDEO_WORKER_WINDOW_START, VIDEO_WORKER_WINDOW_END, VIDEO_WORKER_WINDOW
    )


def assert_text_in_order(test: unittest.TestCase, body: str, needles: list[str]):
    position = -1
    for needle in needles:
        found = body.find(needle, position + 1)
        test.assertGreater(found, position, needle)
        position = found


class VideoImportStubTableHookContractTests(unittest.TestCase):
    HSIO_BODY = (
        "\t// bne cr6,0x826a65b8\n"
        "\tif (!ctx.cr6.eq) goto loc_826A65B8;\n"
        "\t// bl 0x82e92524\n"
        "\tif (ReplaceNativeVideoHsioCall(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.lr)) {\n"
        "\t\tgoto loc_826A65A8;\n"
        "\t}\n"
        "\telse {\n"
        "\t}\n"
        "\tctx.lr = 0x826A65A8;\n"
        "\t__imp__VdIsHSIOTrainingSucceeded(ctx, base);\n"
        + "".join("\t" + line + "\n" for line in VIDEO_HSIO_WINDOW)
        + "\tctx.lr = 0x826A65CC;\n"
        "\tsub_826B0728(ctx, base);\n"
        "\t// cmpwi r3,0\n"
        "\tObserveNativeVideoHsioPostChild(ctx.r1, ctx.r3, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.cr0, ctx.lr);\n"
    )
    WORKER_BODY = (
        "\t// bl 0x82e92634\n"
        "\tif (ReplaceNativeVideoEdramWorkerCall(ctx.r1, ctx.r3, ctx.r29, ctx.r30, ctx.r31, ctx.r13, ctx.lr)) {\n"
        "\t\tgoto loc_826B0540;\n"
        "\t}\n"
        "\tctx.lr = 0x826B0540;\n"
        "\t__imp__VdRetrainEDRAMWorker(ctx, base);\n"
        + "".join("\t" + line + "\n" for line in VIDEO_WORKER_WINDOW)
        + "\tctx.lr = 0x826B0568;\n"
        "\t__imp__VdRetrainEDRAM(ctx, base);\n"
        "\t// cmpwi r3,0\n"
        "\tObserveNativeVideoEdramWorkerPostRetrain(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r29, ctx.r30, ctx.r31, ctx.r13, ctx.lr);\n"
    )

    def test_hook_config_is_separate_and_skips_only_the_imports(self) -> None:
        grouped = read_hooks(VIDEO_IMPORT_HOOK_CONFIG)
        self.assertEqual(len(grouped), 14)
        hooks = [
            hook
            for hook in grouped
            if "NativeVideoHsio" in hook["name"]
            or "NativeVideoEdramWorker" in hook["name"]
        ]
        self.assertEqual(
            hooks,
            [
                {
                    "address": 0x826A65A4,
                    "name": "ReplaceNativeVideoHsioCall",
                    "registers": ["r1", "r3", "r11", "r30", "r31", "r13", "lr"],
                    "after_instruction": False,
                    "jump_address_on_true": 0x826A65A8,
                },
                {
                    "address": 0x826A65A8,
                    "name": "ObserveNativeVideoHsioReturn",
                    "registers": ["r1", "r3", "r11", "r30", "r31", "r13"],
                    "after_instruction": False,
                },
                {
                    "address": 0x826A65C4,
                    "name": "ObserveNativeVideoHsioEffect",
                    "registers": ["r1", "r11", "r30", "r31", "r13", "cr0"],
                    "after_instruction": False,
                },
                {
                    "address": 0x826A65CC,
                    "name": "ObserveNativeVideoHsioPostChild",
                    "registers": [
                        "r1",
                        "r3",
                        "r11",
                        "r30",
                        "r31",
                        "r13",
                        "cr0",
                        "lr",
                    ],
                    "after_instruction": False,
                },
                {
                    "address": 0x826B053C,
                    "name": "ReplaceNativeVideoEdramWorkerCall",
                    "registers": ["r1", "r3", "r29", "r30", "r31", "r13", "lr"],
                    "after_instruction": False,
                    "jump_address_on_true": 0x826B0540,
                },
                {
                    "address": 0x826B0540,
                    "name": "ObserveNativeVideoEdramWorkerReturn",
                    "registers": ["r1", "r3", "r29", "r30", "r31", "r13"],
                    "after_instruction": False,
                },
                {
                    "address": 0x826B0564,
                    "name": "ObserveNativeVideoEdramWorkerPreRetrain",
                    "registers": [
                        "r1",
                        "r3",
                        "r4",
                        "r5",
                        "r6",
                        "r7",
                        "r8",
                        "r29",
                        "r30",
                        "r31",
                        "r13",
                    ],
                    "after_instruction": False,
                },
                {
                    "address": 0x826B0568,
                    "name": "ObserveNativeVideoEdramWorkerPostRetrain",
                    "registers": [
                        "r1",
                        "r3",
                        "r4",
                        "r5",
                        "r6",
                        "r7",
                        "r8",
                        "r29",
                        "r30",
                        "r31",
                        "r13",
                        "lr",
                    ],
                    "after_instruction": False,
                },
            ],
        )
        # Only the two entries jump, each to the continuation of its own bl.
        self.assertEqual(
            {
                hook["name"]: hook["jump_address_on_true"]
                for hook in hooks
                if "jump_address_on_true" in hook
            },
            {
                "ReplaceNativeVideoHsioCall": 0x826A65A8,
                "ReplaceNativeVideoEdramWorkerCall": 0x826B0540,
            },
        )
        # The skipped calls leave LR stale in their intervals: the return, effect
        # and pre-retrain observers, which run there, never receive it.
        self.assertEqual(
            [hook["name"] for hook in hooks if "lr" in hook["registers"]],
            [
                "ReplaceNativeVideoHsioCall",
                "ObserveNativeVideoHsioPostChild",
                "ReplaceNativeVideoEdramWorkerCall",
                "ObserveNativeVideoEdramWorkerPostRetrain",
            ],
        )
        for hook in hooks:
            self.assertIs(hook["after_instruction"], False)
        permanent = compat_hooks()
        self.assertFalse(
            set(VIDEO_IMPORT_ADDRESSES) & {hook["address"] for hook in permanent}
        )
        manifest = MANIFEST.read_text(encoding="ascii")
        self.assertEqual(manifest.count('"config/native_video_hooks.toml"'), 1)
        # No other hook set touches these call sites.
        for path in (ROOT / "config").glob("*.toml"):
            if path == VIDEO_IMPORT_HOOK_CONFIG:
                continue
            text = path.read_text(encoding="utf-8", errors="replace").lower()
            for address in VIDEO_IMPORT_ADDRESSES:
                self.assertNotIn(f"0x{address:08x}", text, path.name)

    def test_no_other_hook_lies_in_a_stale_lr_interval(self) -> None:
        intervals = ((0x826A65A8, 0x826A65CC), (0x826B0540, 0x826B0568))
        ours = set(VIDEO_IMPORT_ADDRESSES)
        for path in sorted((ROOT / "config").glob("*.toml")):
            with path.open("rb") as stream:
                document = tomllib.load(stream)
            for hook in document.get("midasm_hook", []):
                address = hook.get("address")
                for low, high in intervals:
                    if low <= address < high:
                        self.assertIn(address, ours, f"{path.name}: {address:#x}")
                        if path != VIDEO_IMPORT_HOOK_CONFIG:
                            self.fail(f"{path.name} hooks inside an LR interval")

    def test_cvars_are_default_off_and_init_only(self) -> None:
        source = VIDEO_IMPORT_SOURCE.read_text(encoding="utf-8")
        for name in (
            "native_video_hsio_stub",
            "native_video_hsio_exit_check",
            "native_video_edram_worker_stub",
            "native_video_edram_worker_exit_check",
        ):
            self.assertRegex(
                source,
                r"REXCVAR_DEFINE_BOOL\(" + name + r",\s*false,",
            )
        self.assertEqual(source.count("Lifecycle::kInitOnly"), 4)
        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("native_video_hsio", app)
        self.assertNotIn("native_video_edram_worker", app)
        for prefix, name in (
            ("Hsio", "native_video_hsio"),
            ("EdramWorker", "native_video_edram_worker"),
        ):
            # The stub needs published ring stub services; the check does not.
            self.assertRegex(
                source,
                rf"bool Video{prefix}StubRequested\(\) noexcept\s*\{{\s*"
                rf"return RingStubsPublished\(\)\s*&&\s*"
                rf"REXCVAR_GET\({name}_stub\);",
            )
            self.assertRegex(
                source,
                rf"bool Video{prefix}ExitCheckRequested\(\) noexcept\s*\{{\s*"
                rf"return REXCVAR_GET\({name}_exit_check\);",
            )
            self.assertRegex(
                source,
                rf"bool Video{prefix}Active\(\) noexcept\s*\{{\s*"
                rf"return Video{prefix}ExitCheckRequested\(\)\s*\|\|\s*"
                rf"Video{prefix}StubRequested\(\);",
            )
            self.assertRegex(
                source,
                rf"bool Video{prefix}StubReportingRequested\(\) noexcept\s*\{{\s*"
                rf"return REXCVAR_GET\({name}_stub\)\s*\|\|\s*"
                rf"REXCVAR_GET\({name}_exit_check\);",
            )
        self.assertRegex(
            source,
            r"bool VideoImportStubsReportingRequested\(\) noexcept\s*\{\s*"
            r"return VideoHsioStubReportingRequested\(\)\s*\|\|\s*"
            r"VideoEdramWorkerStubReportingRequested\(\);",
        )

    def test_table_is_the_two_immutable_rows(self) -> None:
        header = strip_cpp_comments(VIDEO_IMPORT_HEADER.read_text(encoding="utf-8"))
        rows = header.split("kVideoImportStubRows", 1)[1].split("};", 1)[0]
        self.assertEqual(rows.count("VideoImportId::"), 2)
        self.assertEqual(rows.count("VideoImportReadiness::RingStubsReady"), 2)
        self.assertIn(
            "{ kVideoHsioCallPC, VideoImportId::Hsio, 1, kVideoHsioReturnPC, "
            "&VideoHsioPredicate, kVideoHsioImportAddress, "
            "&__imp__VdIsHSIOTrainingSucceeded, kVideoHsioFrameBytes, "
            "VideoImportReadiness::RingStubsReady }",
            rows,
        )
        self.assertIn(
            "{ kVideoWorkerCallPC, VideoImportId::EdramWorker, 0, "
            "kVideoWorkerReturnPC, &VideoEdramWorkerPredicate, "
            "kVideoWorkerImportAddress, &__imp__VdRetrainEDRAMWorker, "
            "kVideoWorkerFrameBytes, VideoImportReadiness::RingStubsReady }",
            rows,
        )
        self.assertIn("inline constexpr std::array<VideoImportStubRow", header)

    def test_entry_binds_the_services_under_the_lease(self) -> None:
        source = strip_cpp_comments(VIDEO_IMPORT_SOURCE.read_text(encoding="utf-8"))
        body = source.split("bool ReplaceVideoImportStubCallWith(", 1)[1].split(
            "void ObserveVideoImportReturn(", 1
        )[0]
        self.assertEqual(body.count("lease.emplace()"), 1)
        for needle in (
            "static_cast<bool>(*lease)",
            "rowReady(*row, call)",
            "operations.environment(*row)",
            "environmentAdmits(state.environment)",
            "providerAdmits(state.environment, *row)",
            "DeriveVideoImportStub(*row, call, state, operations.memory, plan)",
            "ApplyVideoImportStub(plan, r3)",
        ):
            self.assertEqual(body.count(needle), 1, needle)
        # A wrong PC is refused before the lease, the probe and any read.
        self.assertLess(
            body.index("FindVideoImportStubRow("), body.index("lease.emplace()")
        )
        self.assertLess(body.index("R::Pc"), body.index("lease.emplace()"))
        # The lease is taken before the identities are read and the
        # replacement is committed while it is still held.
        assert_text_in_order(
            self,
            body,
            [
                "lease.emplace()",
                "operations.environment(*row)",
                "rowReady(*row, call)",
                "providerAdmits(state.environment, *row)",
                "DeriveVideoImportStub(",
                "ApplyVideoImportStub(plan, r3)",
            ],
        )
        # The exit check reads the services too: no early exit before the lease.
        self.assertLess(
            body.index("lease.emplace()"), body.index("DeriveVideoImportStub(")
        )
        # Concrete provider checks.
        self.assertRegex(
            source,
            r"bool providerAdmits\(const VideoImportEnvironment& environment, "
            r"const VideoImportStubRow& row\) noexcept\s*\{\s*return "
            r"environment\.dispatcher && environment\.target != nullptr && "
            r"environment\.target == row\.wrapper;",
        )
        self.assertIn("dispatcher->GetFunction(row.importAddress)", source)
        self.assertIn("CurrentRingStubServices()", source)
        self.assertIn("RingStubsReady(low32(call.r31))", source)
        self.assertIn("row.frameBytes", source)
        self.assertIn("row.importAddress", source)
        # Row consistency before anything else in the derivation.
        derive = source.split("DeriveVideoImportStub(const VideoImportStubRow& row", 1)[
            1
        ].split("void ApplyVideoImportStub(", 1)[0]
        assert_text_in_order(
            self,
            derive,
            [
                "rowIsCanonical(row)",
                "R::Services",
                "R::Provider",
                "R::Diagnostics",
                "R::Stack",
                "R::Shape",
                "R::Unreadable",
                "*state.record",
            ],
        )
        self.assertIn("readFrame(row, sp, memory, plan.snapshot)", derive)
        self.assertIn("frameLayoutAllows(row, sp)", derive)

    def test_pending_bound_is_a_compare_exchange_reservation(self) -> None:
        source = strip_cpp_comments(VIDEO_IMPORT_SOURCE.read_text(encoding="utf-8"))
        arm = source.split("std::optional<R> armRecord(", 1)[1].split(
            "Record* openBoundary(", 1
        )[0]
        self.assertEqual(arm.count("compare_exchange_weak"), 1)
        self.assertLess(arm.index("pendingLimit"), arm.index("compare_exchange_weak"))
        self.assertRegex(arm.split("compare_exchange_weak", 1)[1], r"\br\s*=\s*\{\};")
        self.assertNotIn("pending.fetch_add", source)
        self.assertEqual(source.count("c.pending.fetch_sub"), 1)
        header = VIDEO_IMPORT_HEADER.read_text(encoding="utf-8")
        self.assertIn("kVideoImportMaxPending    = 64", header)

    def test_hook_adapters_are_cvar_gated(self) -> None:
        source = VIDEO_IMPORT_HOOK_SOURCE.read_text(encoding="utf-8")
        registers = lambda names: r",\s*".join(  # noqa: E731
            "PPCRegister& " + name for name in names
        )
        signatures = (
            (
                "bool ReplaceNativeVideoHsioCall",
                registers(["r1", "r3", "r11", "r30", "r31", "r13"])
                + r",\s*uint64_t lr",
            ),
            (
                "void ObserveNativeVideoHsioReturn",
                registers(["r1", "r3", "r11", "r30", "r31", "r13"]),
            ),
            (
                "void ObserveNativeVideoHsioEffect",
                registers(["r1", "r11", "r30", "r31", "r13"])
                + r",\s*PPCCRRegister& cr0",
            ),
            (
                "void ObserveNativeVideoHsioPostChild",
                registers(["r1", "r3", "r11", "r30", "r31", "r13"])
                + r",\s*PPCCRRegister& cr0,\s*uint64_t lr",
            ),
            (
                "bool ReplaceNativeVideoEdramWorkerCall",
                registers(["r1", "r3", "r29", "r30", "r31", "r13"])
                + r",\s*uint64_t lr",
            ),
            (
                "void ObserveNativeVideoEdramWorkerReturn",
                registers(["r1", "r3", "r29", "r30", "r31", "r13"]),
            ),
            (
                "void ObserveNativeVideoEdramWorkerPreRetrain",
                registers(
                    [
                        "r1",
                        "r3",
                        "r4",
                        "r5",
                        "r6",
                        "r7",
                        "r8",
                        "r29",
                        "r30",
                        "r31",
                        "r13",
                    ]
                ),
            ),
            (
                "void ObserveNativeVideoEdramWorkerPostRetrain",
                registers(
                    [
                        "r1",
                        "r3",
                        "r4",
                        "r5",
                        "r6",
                        "r7",
                        "r8",
                        "r29",
                        "r30",
                        "r31",
                        "r13",
                    ]
                )
                + r",\s*uint64_t lr",
            ),
        )
        for name, arguments in signatures:
            self.assertRegex(source, re.escape(name) + r"\(" + arguments + r"\)")
        order = [name for name, _ in signatures]
        for index, name in enumerate(order):
            body = source.split(name, 1)[1]
            if index + 1 < len(order):
                body = body.split(order[index + 1], 1)[0]
            if name.startswith("bool Replace"):
                active = (
                    "VideoHsioActive()"
                    if "Hsio" in name
                    else "VideoEdramWorkerActive()"
                )
                self.assertIn(active, body, name)
                self.assertLess(
                    body.index(active), body.index("ReplaceVideoImportStubCall("), name
                )
                # The replacement writes only r3, through its by-reference argument.
                self.assertEqual(body.count("r3.u64"), 2, name)
                self.assertEqual(
                    len(re.findall(r"\br(?:1|13|30|31)\.u64\s*=", body)), 0, name
                )
            else:
                gate = (
                    "VideoHsioStubReportingRequested()"
                    if "Hsio" in name
                    else "VideoEdramWorkerStubReportingRequested()"
                )
                self.assertIn(gate, body, name)
                self.assertLess(body.index(gate), body.index("guest::Observe"), name)
                self.assertEqual(body.count("guest::Observe"), 1, name)

    def test_observers_inside_the_stale_lr_intervals_never_read_lr(self) -> None:
        adapter = strip_cpp_comments(
            VIDEO_IMPORT_HOOK_SOURCE.read_text(encoding="utf-8")
        )
        for name, successor in (
            ("ObserveNativeVideoHsioReturn", "ObserveNativeVideoHsioEffect"),
            ("ObserveNativeVideoHsioEffect", "ObserveNativeVideoHsioPostChild"),
            (
                "ObserveNativeVideoEdramWorkerReturn",
                "ObserveNativeVideoEdramWorkerPreRetrain",
            ),
            (
                "ObserveNativeVideoEdramWorkerPreRetrain",
                "ObserveNativeVideoEdramWorkerPostRetrain",
            ),
        ):
            body = adapter.split("void " + name, 1)[1].split("void " + successor, 1)[0]
            self.assertNotRegex(body, r"(?i)\blr\b", name)
        stub = strip_cpp_comments(VIDEO_IMPORT_SOURCE.read_text(encoding="utf-8"))
        # Every function defined at column zero, keyed by name.
        definitions: dict[str, list[str]] = {}
        for match in re.finditer(
            r"^[A-Za-z][^\n;{}]*?\b(\w+)\([^;{}]*?\)[^;{}]*\n\{\n.*?\n\}\n",
            stub,
            re.MULTILINE | re.DOTALL,
        ):
            definitions.setdefault(match.group(1), []).append(match.group(0))
        roots = ("ObserveVideoImportReturn", "ObserveVideoImportEffect")
        closure = set(roots)
        pending = list(roots)
        while pending:
            current = pending.pop()
            self.assertIn(current, definitions, current)
            for body in definitions[current]:
                for callee in re.findall(r"\b(\w+)\s*\(", body):
                    if callee in definitions and callee not in closure:
                        closure.add(callee)
                        pending.append(callee)
        # The closure must have found the helpers that decide a boundary, so a
        # parser that stops short cannot pass unnoticed.
        for helper in (
            "openBoundary",
            "poisonLive",
            "observerHasWork",
            "bindingStable",
            "servicesStable",
            "sameOwner",
        ):
            self.assertIn(helper, closure, helper)
        # The post observer takes LR by design and is not part of the closure.
        self.assertNotIn("ObserveVideoImportPost", closure)
        for name in sorted(closure):
            for body in definitions[name]:
                self.assertNotRegex(
                    body,
                    r"(?i:\blr\b|\.lr\b)|(?<=[a-z])Lr(?![a-z])|\blr[A-Z_]",
                    name,
                )
        header = strip_cpp_comments(VIDEO_IMPORT_HEADER.read_text(encoding="utf-8"))
        for name in ("ObserveVideoImportReturn", "ObserveVideoImportEffect"):
            declaration = re.search(r"void " + name + r"\([^)]*\)", header)
            self.assertIsNotNone(declaration, name)
            self.assertNotRegex(declaration.group(0), r"(?i)\blr\b")
        observation = re.search(
            r"struct VideoImportObservation\s*\{.*?\};", header, re.DOTALL
        )
        self.assertIsNotNone(observation)
        self.assertNotRegex(observation.group(0), r"(?i)\blr\b")

    def test_app_drains_the_counters(self) -> None:
        app = APP_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(app.count("ReportVideoImportStubs(true);"), 2)
        self.assertEqual(app.count("ReportVideoImportStubs()"), 1)
        self.assertEqual(app.count("VideoImportStubsReportingRequested()"), 1)
        before = app.split("ReportVideoImportStubs()", 1)[0]
        self.assertIn("VideoImportStubsReportingRequested()", before[-200:])
        self.assertEqual(
            app.count('#include "gpu/guest_d3d/video_import_stub_table.h"'), 1
        )
        # The timing drain follows the display mode's.
        self.assertLess(
            app.index("ReportVideoDisplayModeStub()"),
            app.index("ReportVideoImportStubs()"),
        )
        # Each final report sits beside the display mode's, in its own function,
        # so omitting either one is a failure.
        shutdown = app.split("void App::OnShutdown()", 1)[1].split("\nvoid App::", 1)[0]
        hard_exit = app.split("void App::OnBeforeHardExit()", 1)[1].split(
            "\nvoid App::", 1
        )[0]
        for name, body in (("OnShutdown", shutdown), ("OnBeforeHardExit", hard_exit)):
            self.assertEqual(body.count("ReportVideoImportStubs(true);"), 1, name)
            self.assertEqual(body.count("ReportVideoDisplayModeStub(true);"), 1, name)
            self.assertLess(
                body.index("ReportVideoDisplayModeStub(true);"),
                body.index("ReportVideoImportStubs(true);"),
                name,
            )
            gap = body[
                body.index("ReportVideoDisplayModeStub(true);") : body.index(
                    "ReportVideoImportStubs(true);"
                )
            ]
            self.assertEqual(gap.count("\n"), 1, name)

    def test_lr_window_checkers_find_every_lr_use(self) -> None:
        for checker, body, hook, tail in (
            (
                video_hsio_window_violations,
                self.HSIO_BODY,
                "ObserveNativeVideoHsioEffect(ctx.r1, ctx.r11, ctx.r30, ctx.r31, ctx.r13, ctx.cr0);",
                "\t// bl 0x826b0728\n",
            ),
            (
                video_worker_window_violations,
                self.WORKER_BODY,
                "ObserveNativeVideoEdramWorkerPreRetrain(ctx.r1, ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r29, ctx.r30, ctx.r31, ctx.r13);",
                "\t// bl 0x82e92624\n",
            ),
        ):
            self.assertEqual(checker(body), [])
            # A hook given LR, a direct read, a bare name and a sub-expression all fail.
            self.assertNotEqual(
                checker(body.replace(hook, hook.replace(");", ", ctx.lr);"), 1)), []
            )
            self.assertNotEqual(
                checker(body.replace(hook, hook.replace(");", ", lr);"), 1)), []
            )
            self.assertNotEqual(
                checker(body.replace(tail, "\tctx.r12.u64 = ctx.lr;\n" + tail, 1)), []
            )
            self.assertNotEqual(
                checker(body.replace(tail, "\tctx.r12.u64 = ctx.lr + 4;\n" + tail, 1)),
                [],
            )
            # An indirect reader, an unapproved call, another observer and a
            # duplicate of the approved one fail without an LR token.
            self.assertNotEqual(
                checker(
                    body.replace(tail, "\thelper_reads_lr(ctx, base);\n" + tail, 1)
                ),
                [],
            )
            self.assertNotEqual(
                checker(body.replace(tail, "\tsub_82000000(ctx, base);\n" + tail, 1)),
                [],
            )
            self.assertNotEqual(
                checker(
                    body.replace(
                        tail, "\tObserveNativeVideoScalerReturn(ctx.r1);\n" + tail, 1
                    )
                ),
                [],
            )
            self.assertNotEqual(
                checker(body.replace(tail, "\t" + hook + "\n" + tail, 1)), []
            )
            # A moved observer and a hook with the wrong registers fail.
            lead = "\tloc_826A65A8:\n" if "Hsio" in hook else "\tloc_826B0540:\n"
            moved = body.replace("\t" + hook + "\n", "", 1).replace(
                lead, lead + "\t" + hook + "\n", 1
            )
            self.assertNotEqual(checker(moved), [])
            self.assertNotEqual(
                checker(body.replace(hook, hook.replace("ctx.r1, ", "", 1), 1)), []
            )
            # A use after the retained bl that sets LR, or before the label, is
            # outside the window.
            self.assertEqual(checker(body + "\tctx.r12.u64 = ctx.lr;\n"), [])
            self.assertEqual(checker("ctx.r12.u64 = ctx.lr;\n" + body), [])
        # HSIO only: a changed branch ingress or diagnostic is not the retained
        # sequence.
        self.assertNotEqual(
            video_hsio_window_violations(
                self.HSIO_BODY.replace(
                    "if (ctx.cr0.eq) goto loc_826A65C4;",
                    "if (!ctx.cr0.eq) goto loc_826A65C4;",
                    1,
                )
            ),
            [],
        )
        self.assertNotEqual(
            video_hsio_window_violations(
                self.HSIO_BODY.replace("sub_826A5350(ctx, base);", "", 1)
            ),
            [],
        )
        # The worker's retained argument setup is part of the checked sequence.
        self.assertNotEqual(
            video_worker_window_violations(
                self.WORKER_BODY.replace("ctx.r5.s64 = 4096;", "ctx.r5.s64 = 4095;", 1)
            ),
            [],
        )

    def test_generated_owners_keep_the_retained_sequence(self) -> None:
        chunks = generated_chunks()
        paths = chunks.paths
        if not paths:
            self.skipTest("generated sources are not available")
        bodies: dict[str, str | None] = {"sub_826A63E8": None, "sub_826B0508": None}
        for name in bodies:
            for path in paths:
                bodies[name] = read_generated_function(chunks.chunk(path), name)
                if bodies[name] is not None:
                    break
        if any(body is None for body in bodies.values()):
            self.skipTest("an owner is not in the generated sources")
        hsio = bodies["sub_826A63E8"]
        worker = bodies["sub_826B0508"]
        # The retained sequences, in order, whether or not the hooks are generated.
        assert_text_in_order(
            self,
            hsio,
            [
                "REX_LOAD_U32(ctx.r30.u32 + -20316)",
                "if (!ctx.cr6.eq) goto loc_826A65B8;",
                "ctx.lr = 0x826A65A8;",
                "__imp__VdIsHSIOTrainingSucceeded(ctx, base);",
                "loc_826A65A8:",
                "ctx.r11.u64 = ctx.r3.u32 == 0 ? 32 : __builtin_clz(ctx.r3.u32);",
                "ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);",
                "REX_STORE_U32(ctx.r30.u32 + -20316, ctx.r11.u32);",
                "if (ctx.cr0.eq) goto loc_826A65C4;",
                "loc_826A65B8:",
                "ctx.lr = 0x826A65C4;",
                "sub_826A5350(ctx, base);",
                "loc_826A65C4:",
                "ctx.r3.u64 = ctx.r31.u64;",
                "ctx.lr = 0x826A65CC;",
                "sub_826B0728(ctx, base);",
                "ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);",
                "sub_826A5350(ctx, base);",
                "ctx.r3.s64 = 1;",
            ],
        )
        assert_text_in_order(
            self,
            worker,
            [
                "__imp__KeEnterCriticalRegion(ctx, base);",
                "__imp__RtlEnterCriticalSection(ctx, base);",
                "if (ctx.cr6.eq) goto loc_826B0540;",
                "ctx.r3.s64 = 0;",
                "ctx.lr = 0x826B0540;",
                "__imp__VdRetrainEDRAMWorker(ctx, base);",
                "loc_826B0540:",
                "REX_STORE_U32(ctx.r1.u32 + 84, ctx.r11.u32);",
                "ctx.r7.s64 = ctx.r1.s64 + 96;",
                "REX_STORE_U32(ctx.r1.u32 + 80, ctx.r11.u32);",
                "ctx.r6.s64 = ctx.r1.s64 + 84;",
                "ctx.r5.s64 = 4096;",
                "ctx.r4.s64 = ctx.r1.s64 + 80;",
                "ctx.r3.u64 = ctx.r30.u64;",
                "ctx.lr = 0x826B0568;",
                "__imp__VdRetrainEDRAM(ctx, base);",
                "ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);",
                "if (ctx.cr0.eq) goto loc_826B05D4;",
                "loc_826B0580:",
                "sub_8269C7A8(ctx, base);",
                "__imp__RtlLeaveCriticalSection(ctx, base);",
                "__imp__KeLeaveCriticalRegion(ctx, base);",
                "ctx.r3.s64 = 1;",
            ],
        )
        if "ReplaceNativeVideoHsioCall" not in hsio:
            self.skipTest("generated sources predate the HSIO hooks")
        if "ReplaceNativeVideoEdramWorkerCall" not in worker:
            self.skipTest("generated sources predate the worker hooks")
        # Call label -> replacement -> BL LR assignment -> untouched Original.
        assert_text_in_order(
            self,
            hsio,
            [
                "if (!ctx.cr6.eq) goto loc_826A65B8;",
                "if (ReplaceNativeVideoHsioCall(",
                "goto loc_826A65A8;",
                "ctx.lr = 0x826A65A8;",
                "__imp__VdIsHSIOTrainingSucceeded(ctx, base);",
                "loc_826A65A8:",
                "ObserveNativeVideoHsioReturn(",
                "ctx.r11.u64 = ctx.r3.u32 == 0 ? 32 : __builtin_clz(ctx.r3.u32);",
                "loc_826A65C4:",
                "ObserveNativeVideoHsioEffect(",
                "ctx.r3.u64 = ctx.r31.u64;",
                "ctx.lr = 0x826A65CC;",
                "sub_826B0728(ctx, base);",
                "ObserveNativeVideoHsioPostChild(",
                "ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);",
            ],
        )
        assert_text_in_order(
            self,
            worker,
            [
                "ctx.r3.s64 = 0;",
                "if (ReplaceNativeVideoEdramWorkerCall(",
                "goto loc_826B0540;",
                "ctx.lr = 0x826B0540;",
                "__imp__VdRetrainEDRAMWorker(ctx, base);",
                "loc_826B0540:",
                "ObserveNativeVideoEdramWorkerReturn(",
                "ctx.r11.s64 = 0;",
                "ctx.r3.u64 = ctx.r30.u64;",
                "ObserveNativeVideoEdramWorkerPreRetrain(",
                "ctx.lr = 0x826B0568;",
                "__imp__VdRetrainEDRAM(ctx, base);",
                "ObserveNativeVideoEdramWorkerPostRetrain(",
                "ctx.cr0.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);",
            ],
        )
        # Each hook once; the true branch goes to the exact result label.
        for body, names in (
            (
                hsio,
                (
                    "ReplaceNativeVideoHsioCall(",
                    "ObserveNativeVideoHsioReturn(",
                    "ObserveNativeVideoHsioEffect(",
                    "ObserveNativeVideoHsioPostChild(",
                ),
            ),
            (
                worker,
                (
                    "ReplaceNativeVideoEdramWorkerCall(",
                    "ObserveNativeVideoEdramWorkerReturn(",
                    "ObserveNativeVideoEdramWorkerPreRetrain(",
                    "ObserveNativeVideoEdramWorkerPostRetrain(",
                ),
            ),
        ):
            for name in names:
                self.assertEqual(body.count(name), 1, name)
        for body, call, label in (
            (hsio, "if (ReplaceNativeVideoHsioCall(", "goto loc_826A65A8;"),
            (worker, "if (ReplaceNativeVideoEdramWorkerCall(", "goto loc_826B0540;"),
        ):
            start = body.index(call)
            self.assertIn(label, body[start : start + 400])
            self.assertEqual(body.count(label), 1 if "A65A8" in label else 2)
        # The stale-LR intervals hold only the approved statements.
        self.assertEqual(video_hsio_window_violations(hsio), [])
        self.assertEqual(video_worker_window_violations(worker), [])
        # The post observers are the only readers of LR after the retained bl.
        self.assertEqual(hsio.count("ctx.lr)"), 2)
        self.assertEqual(worker.count("ctx.lr)"), 2)


DEVELOPER_SOURCES = (
    "src/gpu/diagnostics/draw_capture_frame_inputs.cpp",
    "src/gpu/diagnostics/label_draw_capture.cpp",
    "src/gpu/diagnostics/route_effect_trace.cpp",
    "src/gpu/diagnostics/route_trace.cpp",
    "src/gpu/diagnostics/scene_payload_store.cpp",
    "src/gpu/diagnostics/scene_resource_capture.cpp",
    "src/gpu/diagnostics/scene_resource_decode.cpp",
    "src/gpu/diagnostics/scene_target_readback.cpp",
    "src/gpu/diagnostics/shader_harvest.cpp",
    "src/gpu/diagnostics/xenos_frame_reference.cpp",
    "src/gpu/guest_d3d/guest_api_census.cpp",
)
DEVELOPER_CVARS = (
    "native_guest_adapter_output",
    "native_route_trace",
    "native_scene_capture",
    "native_scene_capture_frames",
    "native_scene_capture_payloads",
    "native_scene_capture_effects",
    "native_scene_capture_targets",
    "native_xenos_frame_reference",
    "native_shader_harvest",
    "native_shader_harvest_arm",
)
# The completion, original residue, tiling and vertex shader readback probes
# and the vertex shader code probe and watch, with the cvar each one defines.
PROBE_SOURCES = (
    "src/gpu/guest_d3d/completion_probe.cpp",
    "src/gpu/guest_d3d/original_residue_probe.cpp",
    "src/gpu/guest_d3d/tiling_probe.cpp",
    "src/gpu/guest_d3d/vs_code_probe.cpp",
    "src/gpu/guest_d3d/vs_code_watch.cpp",
    "src/gpu/guest_d3d/vs_readback_probe.cpp",
    "src/gpu/guest_d3d/vs_readback_probe_hooks.cpp",
)
PROBE_CVARS = (
    "native_completion_probe",
    "native_original_residue_probe",
    "native_tiling_quiesce_check",
    "native_vs_code_watch",
    "native_vs_readback_probe",
)
PROBE_TEST_SOURCES = (
    "tests/gpu/completion_probe_test.cpp",
    "tests/gpu/original_residue_probe_test.cpp",
    "tests/gpu/tiling_probe_test.cpp",
    "tests/gpu/vs_readback_probe_test.cpp",
    "tests/support/original_residue_probe_variant.cpp",
)
# Hook sources whose shipping branch must reach no D or E recorder.
RUNTIME_HOOK_SOURCES = (
    "src/app.cpp",
    "src/rerevved_hooks.cpp",
    "src/gpu/guest_d3d/draw_hooks.cpp",
    "src/gpu/guest_d3d/guest_api_hooks.cpp",
    "src/gpu/guest_d3d/ring_stub_hooks.cpp",
    "src/gpu/guest_d3d/route_hooks.cpp",
    "src/gpu/guest_d3d/window_writer_hooks.cpp",
)
PROBE_INCLUDE = re.compile(
    r'#include\s+"(?:gpu/guest_d3d/)?'
    r"(?:completion_probe|original_residue_probe|tiling_probe|vs_readback_probe|"
    r'vs_code_probe|vs_code_watch)\.h"'
)
RECORDER_INCLUDE = re.compile(
    r'#include\s+"(?:gpu/diagnostics/|gpu/guest_d3d/)?'
    r"(?:route_trace|route_effect_trace|scene_resource_capture|scene_payload_store|"
    r"scene_target_readback|shader_harvest|shader_harvest_hook|xenos_frame_reference|"
    r'label_draw_capture|draw_capture_frame_inputs|guest_api_census)\.h"'
)


def shipping_text(source: str) -> str:
    """The source as a REREVVED_DEVELOPER=0 translation unit keeps it."""
    kept: list[str] = []
    # Each frame is [is_developer_conditional, active_branch_is_shipping].
    frames: list[list[bool]] = []
    for line in source.splitlines():
        stripped = line.strip()
        if stripped.startswith(("#if ", "#ifdef ", "#ifndef ")):
            developer = stripped == "#if REREVVED_DEVELOPER"
            visible = all(frame[1] for frame in frames)
            frames.append([developer, not developer])
            if visible and not developer:
                kept.append(line)
        elif stripped == "#else" and frames:
            if frames[-1][0]:
                frames[-1][1] = True
            elif all(frame[1] for frame in frames[:-1]):
                kept.append(line)
        elif stripped == "#endif" and frames:
            frame = frames.pop()
            if not frame[0] and all(f[1] for f in frames):
                kept.append(line)
        elif all(frame[1] for frame in frames):
            kept.append(line)
    return "\n".join(kept)


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


class DeveloperBoundaryTests(unittest.TestCase):
    def cmake(self) -> str:
        return (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    def test_option_defaults_off_and_defines_the_numeric_macro(self) -> None:
        cmake = self.cmake()
        self.assertRegex(cmake, r'option\(REREVVED_DEVELOPER "[^"]+" OFF\)')
        self.assertIn("add_compile_definitions(REREVVED_DEVELOPER=1)", cmake)
        self.assertIn("add_compile_definitions(REREVVED_DEVELOPER=0)", cmake)
        header = (ROOT / "src/gpu/guest_d3d/developer_hooks.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("#error", header.split("#if REREVVED_DEVELOPER", 1)[0])

    def test_shipping_sources_hold_no_recorder_and_developer_sources_do(self) -> None:
        cmake = self.cmake()
        listing = cmake.split("set(REREVVED_SOURCES", 1)[1]
        shipping, developer = listing.split(
            "if(REREVVED_DEVELOPER)\n    list(APPEND", 1
        )
        developer = developer.split("endif()", 1)[0]
        for path in DEVELOPER_SOURCES:
            with self.subTest(path=path):
                self.assertNotIn(path, shipping)
                self.assertIn(path, developer)
        # The runtime wrappers, tables and shared bridge compile in both builds.
        for path in (
            "src/gpu/guest_d3d/guest_api_hooks.cpp",
            "src/gpu/guest_d3d/guest_api_entries.inc",
            "src/gpu/guest_d3d/route_hooks.cpp",
            "src/gpu/guest_d3d/route_entries.inc",
            "src/gpu/guest_d3d/draw_hooks.cpp",
        ):
            with self.subTest(path=path):
                self.assertIn(path, shipping)
        self.assertRegex(
            cmake,
            r"if\(REREVVED_DEVELOPER\)\s+add_executable\(native_draw_replay",
        )

    def test_recorder_cvars_exist_only_in_a_developer_build(self) -> None:
        app = (ROOT / "src/app.cpp").read_text(encoding="utf-8")
        shipped = shipping_text(app)
        for name in DEVELOPER_CVARS:
            with self.subTest(cvar=name):
                self.assertRegex(app, rf"REXCVAR_DEFINE_\w+\(\s*{name},")
                self.assertNotRegex(shipped, rf"\b{name}\b")

    def test_shipping_hook_sources_include_no_recorder(self) -> None:
        for name in RUNTIME_HOOK_SOURCES:
            with self.subTest(source=name):
                source = (ROOT / name).read_text(encoding="utf-8")
                shipped = shipping_text(source)
                self.assertEqual(RECORDER_INCLUDE.findall(shipped), [])
                self.assertNotRegex(
                    shipped,
                    r"shaderHarvestObserver|sceneCaptureSession|routeTraceEnabled|"
                    r"diagnostics::(?:Trace|Begin|End|Observe|Start|Stop)",
                )

    def test_probe_sources_and_tests_build_only_in_a_developer_build(self) -> None:
        shipping, developer = split_developer_cmake(self.cmake())
        for path in PROBE_SOURCES:
            with self.subTest(path=path):
                self.assertNotIn(path, shipping)
                self.assertIn(path, developer)
        # The completion bridges compile in both builds as shipping no-ops.
        self.assertIn("src/gpu/guest_d3d/completion_probe_hooks.cpp", shipping)
        tests_shipping, tests_developer = split_developer_cmake(
            (ROOT / "tests/gpu/hook_tests.cmake").read_text(encoding="utf-8")
        )
        for path in PROBE_SOURCES + PROBE_TEST_SOURCES:
            with self.subTest(test_path=path):
                self.assertNotIn(path, tests_shipping)
        for path in PROBE_TEST_SOURCES:
            with self.subTest(test_path=path):
                self.assertIn(path, tests_developer)
        self.assertIn("src/gpu/guest_d3d/completion_probe_hooks.cpp", tests_shipping)

    def test_probe_cvars_exist_only_in_a_developer_build(self) -> None:
        app = (ROOT / "src/app.cpp").read_text(encoding="utf-8")
        shipped_app = shipping_text(app)
        for name in PROBE_CVARS:
            with self.subTest(cvar=name):
                self.assertNotRegex(shipped_app, rf"\b{name}\b")
                owners = [
                    path
                    for path in PROBE_SOURCES
                    if re.search(
                        rf"REXCVAR_DEFINE_\w+\(\s*{name},",
                        (ROOT / path).read_text(encoding="utf-8"),
                    )
                ]
                self.assertEqual(len(owners), 1)
        # No shipping translation unit mentions a probe cvar either.
        probe_stems = {Path(source).stem for source in PROBE_SOURCES}
        for path in (ROOT / "src").rglob("*"):
            if path.suffix not in (".cpp", ".h", ".inc") or path.stem in probe_stems:
                continue
            shipped = shipping_text(path.read_text(encoding="utf-8"))
            for name in PROBE_CVARS:
                self.assertNotRegex(shipped, rf"\b{name}\b", path.name)

    def test_shipping_hook_sources_include_no_probe(self) -> None:
        for name in RUNTIME_HOOK_SOURCES + (
            "src/gpu/guest_d3d/completion_probe_hooks.cpp",
            "src/gpu/guest_d3d/developer_hooks.h",
            "src/gpu/guest_d3d/ring_stubs.cpp",
            "src/gpu/guest_d3d/ring_stubs.h",
        ):
            with self.subTest(source=name):
                shipped = shipping_text((ROOT / name).read_text(encoding="utf-8"))
                self.assertEqual(PROBE_INCLUDE.findall(shipped), [])
                self.assertNotRegex(
                    shipped,
                    r"CompletionProbeRequested|PublishedCompletionProbe|"
                    r"OriginalResidueProbeRequested|NoteOriginalResidueDecision|"
                    r"TilingProbeRequested|PublishedTilingProbe|"
                    r"VsReadbackProbeRequested|VsCodeWatchRequested",
                )

    def test_raw_hooks_exist_in_both_builds(self) -> None:
        shipped = shipping_text(
            (ROOT / "src/gpu/guest_d3d/draw_hooks.cpp").read_text(encoding="utf-8")
        )
        for address in (
            "826A39F8",
            "826A7040",
            "826A7138",
            "826A3568",
            "826AD150",
            "826A3000",
            "826A2FF0",
        ):
            with self.subTest(address=address):
                self.assertEqual(shipped.count(f"REX_HOOK_RAW(sub_{address})"), 1)
                self.assertEqual(shipped.count(f"__imp__sub_{address}(ctx, base)"), 1)
        for name in (
            "ObserveNativeGuestVertexSelection",
            "ObserveNativeGuestGammaTable",
            "ObserveNativeGuestPwlGamma",
        ):
            self.assertIn(f"void {name}(", shipped)
        rerevved_hooks = shipping_text(
            (ROOT / "src/rerevved_hooks.cpp").read_text(encoding="utf-8")
        )
        for name in ("ObserveSceneRegionLocked", "ObserveSceneRegionWritten"):
            self.assertIn(f"void {name}(", rerevved_hooks)

    def test_route_and_census_wrappers_keep_the_shipping_dispatch(self) -> None:
        route = shipping_text(
            (ROOT / "src/gpu/guest_d3d/route_hooks.cpp").read_text(encoding="utf-8")
        )
        census = shipping_text(
            (ROOT / "src/gpu/guest_d3d/guest_api_hooks.cpp").read_text(encoding="utf-8")
        )
        order = [
            "ObserveRingWait<",
            "ObserveCompletionRoute<",
            "ObserveResourceMirror<",
            "ObserveGuestProgress<",
            "RunRingStub(",
            "ConsumeGuestResolve(",
            "ObserveNativeDraw<",
            "ConsumeLowerer(",
            "ObserveLowererExit<",
            "ROUTE_HOOK_RUN(address);",
        ]
        positions = [route.index(token) for token in order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(route.count("__imp__sub_##address(ctx, base)"), 1)
        order = [
            "ObserveSetPredicationCall<",
            "ObserveResourceMirror<",
            "ObserveGuestProgress<",
            "ConsumeGuestClear(",
            "ConsumeSetPredication(",
            "ObserveSetPredicationExit<",
            "setPredicationCall.Original()",
            "__imp__sub_##address(ctx, base)",
        ]
        positions = [census.index(token) for token in order]
        self.assertEqual(positions, sorted(positions))
        # Clear consumption has no recorder blocker in the shipping build: the
        # shared gate is the constexpr-false DeveloperRecorderActive().
        header = (ROOT / "src/gpu/guest_d3d/developer_hooks.h").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            shipping_text(header),
            r"constexpr bool DeveloperRecorderActive\(\) noexcept\s*\{\s*return false;",
        )

    def test_presets_define_developer_configurations(self) -> None:
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        configure = {p["name"]: p for p in presets["configurePresets"]}
        for base in ("windows-base", "linux-base", "mac-arm64-base"):
            self.assertEqual(
                configure[base]["cacheVariables"]["REREVVED_DEVELOPER"], "OFF"
            )
        for platform in ("win-amd64", "linux-amd64", "mac-arm64"):
            developer = configure[f"{platform}-developer"]
            self.assertEqual(developer["inherits"], f"{platform}-release")
            self.assertEqual(developer["cacheVariables"]["REREVVED_DEVELOPER"], "ON")
            self.assertEqual(developer["cacheVariables"]["BUILD_TESTING"], "ON")
        self.assertEqual(
            configure["win-amd64-developer"]["cacheVariables"][
                "REREVVED_PREPARED_TESTS"
            ],
            "ON",
        )
        builds = {p["name"] for p in presets["buildPresets"]}
        tests = {p["name"] for p in presets["testPresets"]}
        for name in (
            "win-amd64-developer",
            "linux-amd64-developer",
            "mac-arm64-developer",
        ):
            self.assertIn(name, builds)
            self.assertIn(name, tests)
        # CI builds and packages only the shipping configuration.
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="ascii")
        self.assertNotIn("-developer", workflow)


class RingAndVideoReplacementBoundaryTests(unittest.TestCase):
    """PT2, B118 restore and the video replacements are developer-only."""

    HOOKS = ROOT / "src/gpu/guest_d3d"
    REPLACEMENT_CVARS = (
        "native_ring_consume_publish",
        "native_consume_tiling_restore",
        "native_tiling_restore_exit_check",
        "native_video_scaler_stub",
        "native_video_scaler_exit_check",
        "native_video_display_mode_stub",
        "native_video_display_mode_exit_check",
        "native_video_hsio_stub",
        "native_video_hsio_exit_check",
        "native_video_edram_worker_stub",
        "native_video_edram_worker_exit_check",
    )
    REPLACEMENT_SOURCES = (
        "src/gpu/guest_d3d/tiling_restore_consume.cpp",
        "src/gpu/guest_d3d/video_display_mode_stub.cpp",
        "src/gpu/guest_d3d/video_import_stub_table.cpp",
        "src/gpu/guest_d3d/video_scaler_stub.cpp",
    )

    def test_replacement_sources_build_only_in_a_developer_build(self) -> None:
        shipping, developer = split_developer_cmake(
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        )
        for path in self.REPLACEMENT_SOURCES:
            with self.subTest(path=path):
                self.assertNotIn(path, shipping)
                self.assertIn(path, developer)
        # The generated hook sites keep their shipping bridges.
        for path in (
            "src/gpu/guest_d3d/video_scaler_hooks.cpp",
            "src/gpu/guest_d3d/video_display_mode_hooks.cpp",
            "src/gpu/guest_d3d/video_import_stub_hooks.cpp",
        ):
            with self.subTest(path=path):
                self.assertIn(path, shipping)

    def test_shipping_text_defines_no_replacement_cvar(self) -> None:
        # The whole-file developer sources are not compiled in a shipping build.
        compiled = [
            path
            for path in sorted(self.HOOKS.glob("*.cpp"))
            if f"src/gpu/guest_d3d/{path.name}" not in self.REPLACEMENT_SOURCES
        ]
        for path in compiled + [ROOT / "src/app.cpp"]:
            text = shipping_text(path.read_text(encoding="utf-8"))
            for cvar in self.REPLACEMENT_CVARS:
                with self.subTest(path=path.name, cvar=cvar):
                    self.assertNotIn(f"({cvar},", text)

    def test_shipping_video_bridges_pass_through(self) -> None:
        for name in (
            "video_scaler_hooks.cpp",
            "video_display_mode_hooks.cpp",
            "video_import_stub_hooks.cpp",
        ):
            text = shipping_text((self.HOOKS / name).read_text(encoding="utf-8"))
            with self.subTest(name=name):
                self.assertRegex(text, r"return false;")
                self.assertNotIn("Report", text)
                self.assertNotIn("Active()", text)

    def test_shipping_b118_forwards_to_original_once(self) -> None:
        text = shipping_text(
            (self.HOOKS / "ring_stub_hooks.cpp").read_text(encoding="utf-8")
        )
        body = text.split("REX_HOOK_RAW(sub_8269B118)", 1)[1].split("REX_HOOK_RAW", 1)[
            0
        ]
        self.assertEqual(body.count("__imp__sub_8269B118(ctx, base)"), 1)
        self.assertNotIn("TilingRestore", body)

    def test_shipping_publish_and_submit_consume_nothing(self) -> None:
        text = shipping_text(
            (self.HOOKS / "ring_stub_hooks.cpp").read_text(encoding="utf-8")
        )
        for symbol in (
            "consumePublish",
            "consumeSubmit",
            "RingConsumePublishRequested",
        ):
            self.assertNotIn(symbol, text)
        publish = text.split("REX_HOOK_RAW(sub_8269C400)", 1)[1].split(
            "REX_HOOK_RAW", 1
        )[0]
        self.assertEqual(publish.count("__imp__sub_8269C400(ctx, base)"), 1)


if __name__ == "__main__":
    unittest.main()
