from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path
from types import SimpleNamespace

from generated_scan import WindowCollector, chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]
HOOKS = ROOT / "src/occ_hooks.cpp"
SETTINGS = ROOT / "src/occ_settings.cpp"

EXPECTED_HOOKS = [
    {
        "address": 0x82CEC7A8,
        "name": "ApplyOccCandidateThreshold",
        "registers": ["r11", "r30", "r8"],
    },
    {
        "address": 0x82D15A7C,
        "name": "ApplyOccCompletionThreshold",
        "registers": ["r11", "r25", "r8"],
    },
    {
        "address": 0x82CB5544,
        "name": "GuardOccAiChoice",
        "registers": ["r1", "r16"],
        "jump_address_on_true": 0x82CB6E30,
    },
    {
        "address": 0x82CF5760,
        "name": "ApplyOccGoldReward",
        "registers": ["r22", "r15"],
        "jump_address_on_true": 0x82CF57E8,
    },
    {
        "address": 0x82D0EEB8,
        "name": "GuardOccVillageSettlers",
        "registers": ["r27"],
        "jump_address_on_true": 0x82D0EF30,
    },
    {
        "address": 0x82CBBE58,
        "name": "GuardOccAiExpansion",
        "registers": ["r1", "r24", "r29"],
        "jump_address_on_true": 0x82CBC3D4,
    },
]


HORSEBACK_NAMES = (
    "BeginHorsebackRidingOwnershipCheck",
    "EndHorsebackRidingOwnershipCheck",
    "SelectHorsebackRidingAbility",
    "SelectHorsebackRidingTechnology",
    "ApplyBarbarianVillageCityReplacement",
)


def scan_generated(paths):
    """One pass over the generated chunks for both generated-code tests."""
    hook_names = [hook["name"] for hook in EXPECTED_HOOKS]
    horseback = WindowCollector()
    horseback_counts = dict.fromkeys(HORSEBACK_NAMES, 0)
    hooks = WindowCollector()
    hook_counts = dict.fromkeys(hook_names, 0)
    for lines in chunk_lines(paths):
        previous = deque(maxlen=12)
        current_function = ""
        for line in lines:
            horseback.advance(line)
            hooks.advance(line)
            if line.startswith("DEFINE_REX_FUNC("):
                current_function = line.split("(", 1)[1].split(")", 1)[0]
            if "(ctx." in line:
                stripped = line.strip()
                for name in HORSEBACK_NAMES:
                    if stripped.startswith(f"{name}(ctx."):
                        horseback_counts[name] += 1
                        horseback.start(name, [line], 48)
                for name in hook_names:
                    if re.search(rf"^\s*(?:if \()?{name}\(ctx\.", line):
                        hook_counts[name] += 1
                        hooks.start(name, [*previous, line], 24)
            if "loc_82CB6E30:" in line and line.strip() == "loc_82CB6E30:":
                hooks.start("ai_epilogue", [line], 16)
            if (
                current_function == "sub_82CF5588"
                and line.strip() == "// stw r10,0(r14)"
            ):
                hooks.start("marker", [*previous, line], 4)
            previous.append(line)
        # The horseback windows are only kept when they complete in-chunk.
        horseback.end_chunk(keep_partial=False)
        hooks.end_chunk()
    return SimpleNamespace(
        horseback_counts=horseback_counts,
        horseback_windows=horseback.windows,
        hook_counts=hook_counts,
        hook_windows=hooks.windows,
    )


class OccContractTests(unittest.TestCase):
    def test_native_boundaries_and_registers(self):
        config = tomllib.loads(
            (ROOT / "config/occ_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertEqual(config["midasm_hook"], EXPECTED_HOOKS)

    def test_one_frozen_policy_and_default_off(self):
        source = SETTINGS.read_text(encoding="utf-8")
        self.assertIn(
            'REXCVAR_DEFINE_BOOL(one_city_challenge, false, "ReRevved",', source
        )
        freeze = source.split("void FreezeForGame(bool standardRules)", 1)[1].split(
            "std::shared_ptr<const Policy> ForGame()", 1
        )[0]
        self.assertEqual(freeze.count("REXCVAR_GET(one_city_challenge)"), 1)
        self.assertEqual(source.count("REXCVAR_GET(one_city_challenge)"), 2)
        self.assertIn("std::shared_ptr<const Policy> frozenPolicy", source)
        self.assertEqual(source.count("std::lock_guard lock(policyMutex);"), 3)
        hooks = HOOKS.read_text(encoding="utf-8")
        self.assertNotIn("REXCVAR_GET(one_city_challenge)", hooks)
        self.assertEqual(
            len(re.findall(r"const auto\s+policy\s*= occ::ForGame\(\);", hooks)), 6
        )

    def test_mongol_horseback_uses_consumers_without_publication(self):
        settings = SETTINGS.read_text(encoding="utf-8")
        ability = settings.split("EraAbilityId EffectiveEraAbility(", 1)[1].split(
            "const char* NationSelectEraText(", 1
        )[0]
        self.assertNotIn("REXCVAR_GET", ability)
        text = settings.split("const char* NationSelectEraText(", 1)[1].split(
            "int32_t CandidateThreshold(", 1
        )[0]
        self.assertIn("REXCVAR_GET(one_city_challenge)", text)
        self.assertNotIn("ForGame()", text)
        consumers = (ROOT / "src/unique_era_abilities_hooks.cpp").read_text()
        self.assertEqual(consumers.count("occ::ForGame()"), 2)
        presentation = (ROOT / "src/nation_select_text_hooks.cpp").read_text()
        self.assertIn("occ::NationSelectEraText(civilization, era)", presentation)
        resolver = presentation.split("bool TryEvaluateEraText(", 1)[1]
        self.assertLess(
            resolver.index("occ::NationSelectEraText("),
            resolver.index("unique_era_abilities::TryEvaluate("),
        )
        for source in (settings, HOOKS.read_text(), consumers, presentation):
            self.assertNotIn("RegisterEraAbilityReplacement(", source)
            self.assertNotIn("RegisterNationSelectTextRule(", source)
        self.assertNotIn("0x82D09208", settings)

    def test_existing_horseback_grant_and_camp_gate_when_generated(self):
        names = HORSEBACK_NAMES
        _, scan = scan_once("occ", scan_generated)
        counts = scan.horseback_counts
        windows = scan.horseback_windows
        if not any(counts.values()):
            self.skipTest("Horseback Riding hooks have not been generated")
        self.assertEqual(counts, dict.fromkeys(names, 1))
        ownership = windows["BeginHorsebackRidingOwnershipCheck"]
        self.assertIn("REX_LOAD_U32(ctx.r31.u32 + 72)", ownership)
        self.assertLess(
            ownership.index("if (!ctx.cr6.eq) goto loc_82D212C8;"),
            ownership.index("SelectHorsebackRidingAbility(ctx.r3);"),
        )
        grant = windows["SelectHorsebackRidingTechnology"]
        self.assertEqual(grant.count("sub_82D09208(ctx, base);"), 1)
        camp = windows["ApplyBarbarianVillageCityReplacement"]
        self.assertIn("ctx.cr6.compare<int32_t>(ctx.r10.s32, 14, ctx.xer)", camp)
        self.assertLess(
            camp.index("if (!ctx.cr6.eq) goto loc_82D1B8BC;"),
            camp.index("sub_82D18D88(ctx, base);"),
        )

    def test_title_checkbox_and_ingress(self):
        overlay = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")
        for entry in (
            '{ "", { "one_city_challenge" } }',
            '{ "one_city_challenge", Audience::kPlayer }',
            '{ "one_city_challenge", "One City Challenge" }',
            '{ "", { "resources" } }',
            '{ "", { "raging_barbarians" } }',
        ):
            self.assertIn(entry, overlay)
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        setup = runtime.split("void RunNativeNewGameSetup(", 1)[1].split(
            "} // namespace", 1
        )[0]
        freeze = "gameplay_session::ApplyNewGame(snapshot, network, mode)"
        self.assertEqual(setup.count(freeze), 1)
        self.assertLess(
            setup.index(freeze),
            setup.rindex("__imp__sub_82D25170(context, base)"),
        )
        session = (ROOT / "src/gameplay_session_settings.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "occ::RestoreForGame({ !standardRules && settings.oneCityChallenge", session
        )
        load = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        load = load.split("void ResetAiCivilizationsOnLoad()", 1)[1].split("}", 1)[0]
        self.assertEqual(load.count("FreezeOccForGame();"), 1)
        self.assertIn("FreezeRagingBarbariansForGame();", load)

    def test_guard_precedes_read_and_uses_original_counts(self):
        source = HOOKS.read_text(encoding="utf-8")
        guard = source.split("bool GuardOccAiChoice(", 1)[1].split(
            "bool ApplyOccGoldReward(", 1
        )[0]
        self.assertLess(guard.index("if (!policy->enabled)"), guard.index("readWord("))
        self.assertIn("readWord(uint64_t{ stack.u32 } + 580)", guard)
        self.assertIn("readWord(uint64_t{ stack.u32 } + 588)", guard)
        self.assertIn("ordinal.s64 = -1;", guard)
        self.assertNotIn("0x82F8AD00", source)

    def test_reward_growth_and_marker_contract(self):
        source = HOOKS.read_text(encoding="utf-8")
        settings = SETTINGS.read_text(encoding="utf-8")
        self.assertIn("kGrowthHelper = 0x82D00C78", source)
        self.assertIn(
            "dispatcher_->ExecuteTrap(thread_, kGrowthHelper, args, 1)", source
        )
        self.assertIn("&thread->context()->r22 != &owner", source)
        self.assertIn("&thread->context()->r15 != &applying", source)
        self.assertIn("readWord(markerAddress) != marker", source)
        self.assertLess(
            settings.index("cities.WritePopulation("), settings.index("cities.Refresh(")
        )
        self.assertNotIn("0x82D11750", source)
        self.assertNotIn("0x82D09208", source)
        self.assertIn("REXCVAR_DEFINE_BOOL(occ_trace, false,", source)
        for field in (
            "blocked_settlers",
            "population_before={}",
            "population_after={}",
            "at_cap={}",
            "growth_helper={}",
            "marker_retained=1",
        ):
            self.assertIn(field, source)

    def test_generated_preservation(self):
        names = [hook["name"] for hook in EXPECTED_HOOKS]
        _, scan = scan_once("occ", scan_generated)
        counts = scan.hook_counts
        windows = scan.hook_windows
        if windows:
            self.assertIn("ctx.r3.u64 = ctx.r16.u64;", windows["ai_epilogue"])
            self.assertIn("ctx.r1.s64 = ctx.r1.s64 + 544;", windows["ai_epilogue"])
            self.assertIn("__restgprlr_14(ctx, base);", windows["ai_epilogue"])
            self.assertIn(
                "REX_STORE_U32(ctx.r14.u32 + 0, ctx.r10.u32);", windows["marker"]
            )
        if not any(counts.values()):
            self.skipTest("OCC hooks have not been generated")
        self.assertEqual(counts, dict.fromkeys(names, 1))
        self.assertIn(
            "ApplyOccCandidateThreshold(ctx.r11, ctx.r30, ctx.r8);\n\t"
            "ctx.cr6.compare<int32_t>(ctx.r8.s32, ctx.r11.s32, ctx.xer);",
            windows[names[0]],
        )
        self.assertIn(
            "ApplyOccCompletionThreshold(ctx.r11, ctx.r25, ctx.r8);\n\t"
            "ctx.cr6.compare<int32_t>(ctx.r8.s32, ctx.r11.s32, ctx.xer);",
            windows[names[1]],
        )
        guard = windows[names[2]]
        self.assertIn("goto loc_82CB6E30;", guard)
        self.assertLess(
            guard.index("GuardOccAiChoice(ctx.r1, ctx.r16)"),
            guard.index("ctx.r25.u64 = REX_LOAD_U32(ctx.r9.u32 + ctx.r10.u32);"),
        )
        reward = windows[names[3]]
        self.assertIn("goto loc_82CF57E8;", reward)
        self.assertLess(
            reward.index("ApplyOccGoldReward(ctx.r22, ctx.r15)"),
            reward.index("ctx.cr6.compare<int32_t>(ctx.r15.s32, 0, ctx.xer);"),
        )
        village = windows[names[4]]
        self.assertIn("goto loc_82D0EF30;", village)
        self.assertLess(
            village.index("loc_82D0EEB8:"),
            village.index("GuardOccVillageSettlers(ctx.r27)"),
        )
        self.assertLess(
            village.index("GuardOccVillageSettlers(ctx.r27)"),
            village.index("REX_LOAD_U8(ctx.r11.u32 + -3829)"),
        )
        expansion = windows[names[5]]
        self.assertIn("goto loc_82CBC3D4;", expansion)
        self.assertLess(
            expansion.index("if (!ctx.cr6.eq) goto loc_82CBC07C;"),
            expansion.index("GuardOccAiExpansion(ctx.r1, ctx.r24, ctx.r29)"),
        )
        self.assertLess(
            expansion.index("GuardOccAiExpansion(ctx.r1, ctx.r24, ctx.r29)"),
            expansion.index("ctx.r11.s64 = -2095906816;"),
        )
        self.assertLess(
            expansion.index("ctx.r11.s64 = -2095906816;"),
            expansion.index("REX_LOAD_U32(ctx.r11.u32 + -18212)"),
        )


if __name__ == "__main__":
    unittest.main()
