from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path
from types import SimpleNamespace

from generated_scan import WindowCollector, chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]

# Pre-instruction sites in the generated PPC owners.
TRACE_SITES = {
    0x82CF1D78: (
        "TraceTerrainInput",
        ["r10", "r8", "r29"],
        "sub_82CF1CE8",
        "beq cr6,0x82cf1d80",
    ),
    0x82CF1D98: (
        "TraceTerrainNative",
        ["r10", "r6", "r29"],
        "sub_82CF1CE8",
        "extsb r30,r6",
    ),
    0x82CF1DA0: (
        "TraceTerrainEffective",
        ["r30", "r10", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r10,2",
    ),
    0x82CF1DC0: (
        "TracePlainsBonus",
        ["r30", "r8", "r29"],
        "sub_82CF1CE8",
        "addi r30,r30,2",
    ),
    0x82CF1DE8: (
        "TraceSeaBonus",
        ["r30", "r8", "r29"],
        "sub_82CF1CE8",
        "addi r30,r30,1",
    ),
    0x82CF1DEC: (
        "TraceTerrainBonuses",
        ["r30", "r10", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r29,-1",
    ),
    0x82CF1E28: (
        "TraceTerrainAbility",
        ["r30", "r3", "r29"],
        "sub_82CF1CE8",
        "bl 0x82cf0cb0",
    ),
    0x82CF1E2C: (
        "TraceTerrainAbilityResult",
        ["r30", "r3", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r3,0",
    ),
    0x82CF1E38: (
        "TraceAbilityTotal",
        ["r30", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r30,0",
    ),
    0x82CF1EB0: (
        "TraceTileBits",
        ["r30", "r9", "r10", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r9,1",
    ),
    0x82CF1EC8: (
        "TraceTileBonusAbility",
        ["r30", "r3", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r3,0",
    ),
    0x82CF1EF4: (
        "TraceTileBonusPlayerMask",
        ["r30", "r4", "r11", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r4,0",
    ),
    0x82CF1EFC: (
        "TraceTileBonus",
        ["r30", "r29"],
        "sub_82CF1CE8",
        "addi r30,r30,1",
    ),
    0x82CF1F10: (
        "TraceFoodIdentity",
        ["r30", "r3", "r29"],
        "sub_82CF1CE8",
        "mr r10,r3",
    ),
    0x82CF1F20: (
        "TraceResourceClass",
        ["r30", "r3", "r10", "r29"],
        "sub_82CF1CE8",
        "cmpwi cr6,r3,0",
    ),
    0x82CF1F30: (
        "TraceResourceFoodCall",
        ["r30", "r3", "r4", "r29"],
        "sub_82CF1CE8",
        "bl 0x82cf0630",
    ),
    0x82CF1F34: (
        "TraceResourceFoodResult",
        ["r30", "r3", "r29"],
        "sub_82CF1CE8",
        "add r30,r3,r30",
    ),
    0x82CF1F54: (
        "TracePlayerModifier",
        ["r30", "r11", "r29"],
        "sub_82CF1CE8",
        "addi r7,r11,1",
    ),
    0x82CF1F5C: (
        "TracePlayerModifierAdd",
        ["r30", "r11", "r7", "r29"],
        "sub_82CF1CE8",
        "add r30,r11,r30",
    ),
    0x82CF1F60: (
        "TraceFoodFinal",
        ["r30", "r29"],
        "sub_82CF1CE8",
        "mr r3,r30",
    ),
    0x82D13F2C: (
        "TraceCityWorkTotals",
        ["r1"],
        "sub_82D13978",
        "cmpwi cr6,r22,0",
    ),
    0x82D931B8: (
        "TraceSettlersFood",
        ["r29", "r30", "r3", "r31"],
        "sub_82D92FA0",
        "mr r5,r3",
    ),
    0x82D931D0: (
        "TraceSettlersFoodCache",
        ["r29", "r30", "r3", "r5", "r31"],
        "sub_82D92FA0",
        "bl 0x82d92590",
    ),
}


IDENTITY_NAMES = ("ApplyResourceIdentityOverride", "SkipBalancedIndiaStartingResource")
YIELD_WRAPPERS = ("sub_82CF1CE8", "sub_82CF1AF0", "sub_82CF17C8", "sub_82D13978")
YIELD_CITY_CALLS = {
    0x82D13E24: ("82CF1CE8", "82D13E28", 144),
    0x82D13E44: ("82CF1AF0", "82D13E48", 128),
    0x82D13E6C: ("82CF17C8", "82D13E70", 136),
}
READER_CALLERS = ("82CF1F10", "82CF1CB0")


def scan_generated(paths):
    """One pass over the generated chunks for all three generated-code tests."""
    identity = WindowCollector()
    identity_counts = dict.fromkeys(IDENTITY_NAMES, 0)
    identity_owners: dict[str, str] = {}

    definitions = dict.fromkeys(YIELD_WRAPPERS, 0)
    yield_counts = dict.fromkeys(TRACE_SITES.keys() | YIELD_CITY_CALLS.keys(), 0)
    yield_sites = WindowCollector()
    yield_locations: dict[int, tuple[str, str]] = {}

    reader_definitions = 0
    reader_lines: list[str] = []
    caller_counts = dict.fromkeys(READER_CALLERS, 0)
    callers = WindowCollector()

    for lines in chunk_lines(paths):
        id_previous: deque[str] = deque(maxlen=112)
        id_owner = ""
        yield_previous: deque[str] = deque(maxlen=36)
        yield_owner = ""
        yield_address = 0
        caller_previous: deque[str] = deque(maxlen=8)
        caller_owner = ""
        for line in lines:
            defines = line.startswith("DEFINE_REX_FUNC(")
            identity.advance(line)
            yield_sites.advance(line)
            callers.advance(line)

            # Identity and India start hooks.
            if defines:
                id_owner = line.split("(", 1)[1].split(")", 1)[0]
            if "(" in line:
                for name in IDENTITY_NAMES:
                    if name in line and re.search(rf"^\s*(?:if \()?{name}\(", line):
                        identity_counts[name] += 1
                        identity_owners[name] = id_owner
                        identity.start(name, [*id_previous, line], 125)
            id_previous.append(line)

            # Yield trace sites.
            if defines:
                definition = re.match(r"DEFINE_REX_FUNC\((sub_([0-9A-F]+))\)", line)
                if definition:
                    yield_owner = definition[1]
                    yield_address = int(definition[2], 16)
                    if yield_owner in definitions:
                        definitions[yield_owner] += 1
                    yield_previous.clear()
            if line.startswith("loc_"):
                label = re.match(r"loc_([0-9A-F]+):", line)
                if label:
                    yield_address = int(label[1], 16)
            if "// " in line:
                instruction = re.match(r"\s*// (.+)", line)
                if instruction:
                    if yield_address in yield_counts:
                        yield_counts[yield_address] += 1
                        yield_locations[yield_address] = (
                            yield_owner,
                            instruction[1].strip(),
                        )
                        yield_sites.start(yield_address, [*yield_previous, line], 48)
                    yield_address += 4
            yield_previous.append(line)

            # Reader wrapper and its yield callers.
            if defines:
                caller_owner = line.split("(", 1)[1].split(")", 1)[0]
                if caller_owner == "sub_82CF1558":
                    reader_definitions += 1
            if caller_owner == "sub_82CF1558":
                reader_lines.append(line)
            if "ctx.lr = 0x" in line:
                for name in READER_CALLERS:
                    if f"ctx.lr = 0x{name};" in line:
                        caller_counts[name] += 1
                        callers.start(name, [*caller_previous, line], 20)
            caller_previous.append(line)
        identity.end_chunk()
        yield_sites.end_chunk()
        callers.end_chunk()
    return SimpleNamespace(
        identity_counts=identity_counts,
        identity_owners=identity_owners,
        identity_windows=identity.windows,
        definitions=definitions,
        yield_counts=yield_counts,
        yield_windows=yield_sites.windows,
        yield_locations=yield_locations,
        reader_definitions=reader_definitions,
        reader="".join(reader_lines),
        caller_counts=caller_counts,
        caller_windows=callers.windows,
    )


class ResourceSettingsContractTests(unittest.TestCase):
    def test_overlay_exposes_only_accepted_modes(self) -> None:
        overlay = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")
        choices = re.search(r'\{ "resources", \{ (.+) \} \}', overlay)
        self.assertIsNotNone(choices)
        assert choices is not None
        self.assertEqual(
            re.findall(r'\{ "(\d+)", "([^"]+)" \}', choices.group(1)),
            [("0", "Normal"), ("1", "Balanced"), ("3", "Legendary")],
        )
        self.assertIn('{ "", { "resources" } }', overlay)
        self.assertIn('{ "resources", Audience::kPlayer }', overlay)
        self.assertIn('{ "resources", "Resources" }', overlay)

    def test_frozen_setup_precedes_india_and_has_one_completion(self) -> None:
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        hooks = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        self.assertEqual(runtime.count("REXCVAR_GET(resources)"), 2)
        self.assertIn("REXCVAR_DEFINE_INT32(resources, 0,", runtime)
        scope = runtime.split("class SetupScope", 1)[1].split("uint16_t loadU16", 1)[0]
        self.assertIn("resource_identity_overrides::Clear();", scope)
        self.assertIn("pendingSetup.emplace(value)", scope)
        self.assertIn("pendingSetup = std::move(previous_)", scope)
        wrapper = runtime.split("void RunNativeNewGameSetup(", 1)[1]
        self.assertLess(
            wrapper.index("SetupScope scope(standardRules);"),
            wrapper.index("__imp__sub_82D25170(context, base)"),
        )
        for freeze in (
            "raging_barbarians::FreezeForGame(standardRules);",
            "occ::FreezeForGame(standardRules);",
        ):
            self.assertEqual(wrapper.count(freeze), 1)
            self.assertLess(
                wrapper.index(freeze), wrapper.index("SetupScope scope(standardRules);")
            )
        self.assertIn("REX_HOOK_RAW(sub_82D25170)", runtime)
        self.assertNotIn("REX_HOOK_RAW(sub_82D11AD8)", runtime)
        completion = hooks.split("REX_HOOK_RAW(sub_82D11AD8)", 1)[1]
        self.assertEqual(completion.count("OnNewGameSetup(ctx, base)"), 1)
        self.assertLess(
            completion.index("ctx.lr == ai_civs::kNewGameRecountReturn"),
            completion.index("OnNewGameSetup(ctx, base)"),
        )
        self.assertLess(
            completion.index("removeAiCivilizations(ctx, base, maxAi, source)"),
            completion.index("OnNewGameSetup(ctx, base)"),
        )
        suppression = runtime.split("bool SuppressIndiaStartingResource()", 1)[1]
        suppression = suppression.split("void ResetOnLoad", 1)[0]
        self.assertIn(
            "pendingSetup && pendingSetup->SuppressIndiaStartingResource()", suppression
        )

    def test_load_clears_before_deserialization(self) -> None:
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        reset = runtime.split("void ResetOnLoad()", 1)[1]
        reset = reset.split("void OnNewGameSetup", 1)[0]
        self.assertIn("resource_identity_overrides::Clear();", reset)
        self.assertIn("pendingSetup.reset();", reset)
        hooks = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        load = hooks.split("void ResetAiCivilizationsOnLoad()", 1)[1]
        self.assertEqual(load.count("rerevved::start_resources::ResetOnLoad();"), 1)
        self.assertNotIn("OnNewGameSetup", load)
        for freeze in ("FreezeRagingBarbariansForGame();", "FreezeOccForGame();"):
            self.assertEqual(load.count(freeze), 1)
        config = tomllib.loads(
            (ROOT / "config/ai_civilizations_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertIn(
            {"address": 0x82D86674, "name": "ResetAiCivilizationsOnLoad"},
            config["midasm_hook"][:2],
        )

    def test_standard_rules_forces_normal_before_selection(self) -> None:
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        scope = runtime.split("class SetupScope", 1)[1].split("uint16_t loadU16", 1)[0]
        self.assertRegex(
            scope,
            r"const int32_t value = standardRules \? 0 : REXCVAR_GET\(resources\)",
        )
        self.assertLess(
            scope.index("standardRules ? 0"), scope.index("pendingSetup.emplace(value)")
        )
        self.assertIn("gotw::FreezeStandardRules(mode)", runtime)

    def test_identity_rows_and_legendary_diagnostics(self) -> None:
        header = (ROOT / "src/resource_identity_overrides.h").read_text(
            encoding="utf-8"
        )
        matrix = header.split("kIdentities[7][4] =", 1)[1].split(";", 1)[0]
        rows = [
            list(map(int, re.findall(r"-?\d+", row)))
            for row in re.findall(r"\{([^{}]+)\}", matrix)
        ]
        self.assertEqual(
            rows,
            [
                [10, 16, 18, -1],
                [0, 20, 19, 11],
                [8, 13, 14, -1],
                [5, 9, 17, -1],
                [1, 3, 6, -1],
                [2, 4, 12, -1],
                [7, 15, 21, -1],
            ],
        )
        logic = (ROOT / "src/start_resources.cpp").read_text(encoding="utf-8")
        pools = logic.split("pools[7][3] =", 1)[1].split(";", 1)[0]
        actual = [
            list(map(int, re.findall(r"-?\d+", row)))
            for row in re.findall(r"\{([^{}]+)\}", pools)
        ]
        wanted = [[18], [0, 19, 20], [8, 13], [9], [1], [12], [15, 21]]
        self.assertEqual(
            [[ordinal for ordinal in row if ordinal >= 0] for row in actual], wanted
        )
        for terrain, pool in enumerate(wanted):
            self.assertTrue(set(pool) <= set(rows[terrain]))
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "Legendary player={} tile=({}, {}) terrain={} ordinal={} override={} skip={} final={} status={}",
            runtime,
        )

    def test_only_flags_are_written_and_snapshot_precedes_presence(self) -> None:
        logic = (ROOT / "src/start_resources.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/start_resources.h").read_text(encoding="utf-8")
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        write = runtime.split("void WriteFlags(", 1)[1].split("bool Refresh", 1)[0]
        self.assertEqual(len(re.findall(r"base_\[[^\n]+\]\s*=", write)), 2)
        self.assertNotIn("kTerrain", write)
        self.assertEqual(
            re.findall(r"(\w+)\.terrain\s*=(?!=)", logic + runtime), ["placement"]
        )
        self.assertNotRegex(logic + runtime, r"\.tiles[^\n]*\.terrain\s*=(?!=)")
        self.assertNotIn("virtual void Write(", header)
        removed_paths = (
            "swapWith",
            "preservesRings",
            "selectTrio(",
            "reserved[",
            "NoTrio",
        )
        for removed in removed_paths:
            self.assertNotIn(removed, header + logic)
        self.assertLess(
            logic.index("overrides::Publish(records)"), logic.index("world.WriteFlags(")
        )
        rollback = logic.split("result.rolledBack = true;", 1)[1]
        self.assertLess(
            rollback.index("overrides::Clear();"), rollback.index("world.WriteFlags(")
        )
        self.assertIn("no eligible existing terrain tile", runtime)
        self.assertNotIn("swap_with", runtime)

    def test_runtime_refresh_binding_and_failure_report(self) -> None:
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        refresh = runtime.split("bool Refresh(Coord coordinate) override", 1)[1]
        refresh = refresh.split("private:", 1)[0]
        self.assertIn("if (!refreshBound())", refresh)
        self.assertRegex(refresh, r"call\.r3\.u64\s*= kReceiver")
        self.assertRegex(
            refresh, r"call\.r4\.u64\s*= static_cast<uint32_t>\(coordinate.a\)"
        )
        self.assertRegex(
            refresh, r"call\.r5\.u64\s*= static_cast<uint32_t>\(coordinate.b\)"
        )
        self.assertIn("__imp__sub_82D7D950(call, base_)", refresh)
        self.assertNotIn("call.r3.u32", refresh)
        self.assertIn("loadU32(base_, kVtable + 0xF4) == kRefresh", runtime)
        self.assertIn("resource_settings_diagnostics, false", runtime)
        setup = runtime.split("void OnNewGameSetup(", 1)[1]
        failure = setup.split("if (result.hardFailure)", 1)[1].split("\n    }", 1)[0]
        self.assertIn("REXLOG_ERROR", failure)
        self.assertIn("return;", failure)

    def test_override_and_india_hook_boundaries(self) -> None:
        config = tomllib.loads(
            (ROOT / "config/terrain_hooks.toml").read_text(encoding="utf-8")
        )
        hooks = config["midasm_hook"]
        # Default-off observers may follow; they must never redirect control flow.
        traces = [hook for hook in hooks if hook["name"].startswith("Trace")]
        for hook in traces:
            self.assertNotIn("jump_address_on_true", hook)
        self.assertEqual(
            [
                hook
                for hook in hooks
                if hook not in traces and not hook["name"].startswith("ApplyTerrain")
            ],
            [
                {
                    "address": 0x82CF15C4,
                    "name": "ApplyResourceIdentityOverride",
                    "registers": ["r11", "r8", "r7"],
                },
                {
                    "address": 0x82D23324,
                    "name": "SkipBalancedIndiaStartingResource",
                    "jump_address_on_true": 0x82D23330,
                },
            ],
        )
        source = (ROOT / "src/resource_identity_hooks.cpp").read_text(encoding="utf-8")
        self.assertEqual(
            re.findall(r"\b(\w+)\.u64\s*=(?!=)", source), ["coordinateSum"]
        )
        self.assertIn("snapshot->empty()", source)
        self.assertIn("tileIndex.u32", source)
        self.assertIn("std::bit_cast<int8_t>", source)
        self.assertNotIn("REX_STORE_", source)
        self.assertNotIn("REXCVAR_GET", source)
        india = source.split("bool SkipBalancedIndiaStartingResource()", 1)[1]
        self.assertIn(
            "return rerevved::start_resources::SuppressIndiaStartingResource();", india
        )

    def test_generated_pre_instruction_hooks_across_all_chunks(self) -> None:
        generated, scan = scan_once("resource_settings", scan_generated)
        if not generated:
            self.skipTest("generated guest code is not present")
        names = IDENTITY_NAMES
        counts = scan.identity_counts
        owners = scan.identity_owners
        windows = scan.identity_windows
        self.assertEqual(counts, dict.fromkeys(names, 1))
        self.assertEqual(
            owners, dict(zip(names, ("sub_82CF1558", "sub_82D217C8"), strict=True))
        )
        identity = windows[names[0]]
        self.assertIn(
            "// lbzx r5,r11,r7\n\tApplyResourceIdentityOverride(ctx.r11, ctx.r8, ctx.r7);\n\t"
            "ctx.r5.u64 = REX_LOAD_U8(ctx.r11.u32 + ctx.r7.u32);",
            identity,
        )
        for native in (
            "ctx.r8.u64 = ctx.r3.u64 + ctx.r4.u64;",
            "ctx.r7.s64 = ctx.r10.s64 + -19832;",
            "ctx.r6.u64 = __builtin_rotateleft64(ctx.r6.u32 | (ctx.r6.u64 << 32), 0) & 0xFFFFFFFFFFFFF001;",
            "if (ctx.cr6.eq) goto loc_82CF1680;",
            "// divw r10,r8,r11",
            "ctx.r27.u64 = ctx.r8.u64 - ctx.r7.u64;",
            "ctx.lr = 0x82CF163C;",
            "sub_82CEF6A8(ctx, base);",
            "ctx.lr = 0x82CF1654;",
            "sub_82CF0CB0(ctx, base);",
            "ctx.cr6.compare<int32_t>(ctx.r10.s32, 10, ctx.xer);",
        ):
            self.assertIn(native, identity)
        india = windows[names[1]]
        self.assertIn(
            "// sthx r6,r11,r15\n\tif (SkipBalancedIndiaStartingResource()) {\n\t\t"
            "goto loc_82D23330;\n\t}\n\telse {\n\t}\n\t"
            "REX_STORE_U16(ctx.r11.u32 + ctx.r15.u32, ctx.r6.u16);",
            india,
        )
        self.assertIn("ctx.f10.f64 = ctx.f11.f64 * ctx.f29.f64;", india)
        for native_draw in (
            "REX_STORE_U32(ctx.r23.u32 + 4, ctx.r9.u32);",
            "REX_STORE_U32(ctx.r23.u32 + 0, ctx.r11.u32);",
            "ctx.r11.u64 = ctx.r11.u64 - ctx.r8.u64;",
        ):
            self.assertLess(india.index(native_draw), india.index(names[1] + "()"))
        self.assertIn(
            "ctx.lr = 0x82D23330;\n\tREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\nloc_82D23330:\n\t// b 0x82d23938",
            india,
        )

    def test_identity_trace_wrapper_is_read_only_and_default_off(self) -> None:
        hooks = (ROOT / "src/resource_identity_hooks.cpp").read_text(encoding="utf-8")
        wrapper = hooks.split("REX_HOOK_RAW(sub_82CF1558)", 1)[1].split(
            "void ApplyResourceIdentityOverride", 1
        )[0]
        self.assertIn("trace(ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64)", wrapper)
        self.assertLess(
            wrapper.index("__imp__sub_82CF1558(ctx, base);"),
            wrapper.index("trace.Returned(ctx.r3.s32);"),
        )
        self.assertNotRegex(wrapper, r"ctx\.\w+(?:\.\w+)?\s*=")
        trace = (ROOT / "src/resource_identity_trace.cpp").read_text(encoding="utf-8")
        self.assertIn("REXCVAR_DEFINE_BOOL(resource_identity_trace, false,", trace)
        self.assertNotIn("REX_STORE_", trace)
        self.assertNotIn("REX_CALL_", trace)

    def test_yield_trace_observers_are_read_only(self) -> None:
        config = tomllib.loads(
            (ROOT / "config/terrain_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertEqual(
            [
                hook
                for hook in config["midasm_hook"]
                if hook["name"].startswith("Trace")
            ],
            [
                {"address": address, "name": name, "registers": registers}
                for address, (name, registers, _, _) in TRACE_SITES.items()
            ],
        )
        hooks = (ROOT / "src/resource_identity_hooks.cpp").read_text(encoding="utf-8")
        for owner in ("82CF1CE8", "82CF1AF0", "82CF17C8", "82D13978"):
            wrapper = re.search(
                rf"REX_HOOK_RAW\(sub_{owner}\)\s*\{{(.*?)\n\}}", hooks, re.S
            )
            self.assertIsNotNone(wrapper)
            assert wrapper is not None
            body = wrapper.group(1)
            self.assertEqual(body.count(f"__imp__sub_{owner}(ctx, base);"), 1)
            self.assertIn("Scope trace(", body)
            self.assertNotRegex(body, r"ctx\.\w+(?:\.\w+)?\s*=")
            if owner != "82D13978":
                self.assertIn("ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64", body)
                self.assertLess(
                    body.index(f"__imp__sub_{owner}"),
                    body.index("trace.Returned(ctx.r3.s32)"),
                )
        for name, _, _, _ in TRACE_SITES.values():
            observer = re.search(
                rf"void {name}\([^)]*\) noexcept\s*\{{(.*?)\n\}}", hooks, re.S
            )
            self.assertIsNotNone(observer, name)
            assert observer is not None
            self.assertNotRegex(observer.group(1), r"\b\w+\.\w+\s*=|REX_STORE_|__imp__")
        trace = (ROOT / "src/resource_identity_trace.cpp").read_text(encoding="utf-8")
        for event in (
            "yield_return",
            "food_step",
            "city_work_totals",
            "overlay_food",
            "overlay_cache",
        ):
            self.assertIn(f"event={event}", trace)
        self.assertIn("readU32(0x82F77364, boundsA)", trace)
        self.assertIn("readU32(0x82F77448, boundsB)", trace)
        self.assertIn("QueryRangeAccess", trace)
        self.assertRegex(
            trace, r"retained\s*= coordinate.exact \|\| coordinate.wrapped"
        )
        self.assertIn("activeCity->matched", trace)
        self.assertNotIn("REX_STORE_", trace)
        self.assertNotIn("REX_CALL_", trace)
        for function in (
            "YieldScope::YieldScope(",
            "void FoodStep(",
            "void CityTotals(",
            "void OverlayFood(",
            "void OverlayCache(",
        ):
            body = trace.split(function, 1)[1].split("\n}", 1)[0]
            self.assertIn("!REXCVAR_GET(resource_identity_trace)", body)
            self.assertIn("return;", body)

    def test_generated_yield_trace_sites_across_all_chunks(self) -> None:
        generated, scan = scan_once("resource_settings", scan_generated)
        if not generated:
            self.skipTest("generated guest code is not present")
        wrappers = set(YIELD_WRAPPERS)
        definitions = scan.definitions
        city_calls = YIELD_CITY_CALLS
        counts = scan.yield_counts
        windows = scan.yield_windows
        locations = scan.yield_locations
        self.assertEqual(definitions, dict.fromkeys(wrappers, 1))
        self.assertEqual(counts, dict.fromkeys(counts, 1))
        for address, (name, registers, owner, instruction) in TRACE_SITES.items():
            self.assertEqual(locations[address], (owner, instruction), name)
        for address, callee, lr in (
            (0x82CF1F30, "82CF0630", "82CF1F34"),
            (0x82CF1E28, "82CF0CB0", "82CF1E2C"),
            (0x82D931D0, "82D92590", "82D931D4"),
        ):
            self.assertIn(
                f"ctx.lr = 0x{lr};\n\tsub_{callee}(ctx, base);", windows[address]
            )
        for address, (callee, lr, stack_offset) in city_calls.items():
            window = windows[address]
            self.assertEqual(
                locations[address], ("sub_82D13978", f"bl 0x{callee.lower()}")
            )
            self.assertIn(f"ctx.lr = 0x{lr};\n\tsub_{callee}(ctx, base);", window)
            self.assertIn("ctx.r3.u64 = ctx.r30.u64;", window)
            self.assertIn("ctx.r4.u64 = ctx.r29.u64;", window)
            self.assertIn("ctx.r5.u64 = REX_LOAD_U32(ctx.r1.u32 + 372);", window)
            self.assertIn("ctx.r6.s64 = -1;", window)
            self.assertRegex(
                window,
                rf"REX_STORE_U32\(ctx.r1.u32 \+ {stack_offset}, ctx.r\d+\.u32\);",
            )
        trace = (ROOT / "src/resource_identity_trace.cpp").read_text(encoding="utf-8")
        for offset, variable in ((144, "food"), (128, "production"), (136, "trade")):
            self.assertIn(f"+ {offset}, {variable})", trace)
        self.assertIn("+ 0x74, cached)", trace)

    def test_regenerated_yield_trace_hooks_are_present_and_bound(self) -> None:
        generated, scan = scan_once("resource_settings", scan_generated)
        if not generated:
            self.skipTest("generated guest code is not present")
        config = ROOT / "config/terrain_hooks.toml"
        generated_mtime = min(path.stat().st_mtime_ns for path in generated)
        if generated_mtime < config.stat().st_mtime_ns:
            self.skipTest(
                "generated guest code predates terrain_hooks.toml; "
                "regenerate to check hook binding"
            )
        for address, (name, registers, _, instruction) in TRACE_SITES.items():
            with self.subTest(hook=name):
                window = scan.yield_windows[address]
                self.assertIn(f"{name}(", window, "regenerated output lacks the hook")
                arguments = ", ".join(f"ctx.{register}" for register in registers)
                self.assertIn(f"{name}({arguments});", window)
                self.assertIn(f"// {instruction}\n\t{name}({arguments});", window)

    def test_generated_reader_wrapper_and_yield_callers_across_all_chunks(self) -> None:
        generated, scan = scan_once("resource_settings", scan_generated)
        if not generated:
            self.skipTest("generated guest code is not present")
        self.assertEqual(scan.reader_definitions, 1)
        self.assertEqual(scan.caller_counts, dict.fromkeys(READER_CALLERS, 1))
        reader = scan.reader
        self.assertIn("ctx.r12.u64 = ctx.lr;", reader)
        self.assertIn("ctx.r11.u64 = ctx.r11.u64 + ctx.r4.u64;", reader)
        self.assertEqual(reader.count("\treturn;"), 2)
        self.assertIn("ctx.r3.s64 = -1;", reader)
        self.assertIn("ctx.r3.u64 = ctx.r29.u64;", reader)
        for name, (a, b, player, resource_class) in {
            "82CF1F10": (28, 27, 29, 0),
            "82CF1CB0": (29, 28, 30, 1),
        }.items():
            window = scan.caller_windows[name]
            for target, source in ((3, a), (4, b), (5, player)):
                self.assertIn(f"ctx.r{target}.u64 = ctx.r{source}.u64;", window)
            self.assertIn("sub_82CF1558(ctx, base);", window)
            self.assertIn(
                f"ctx.cr6.compare<int32_t>(ctx.r3.s32, {resource_class}, ctx.xer);",
                window,
            )


if __name__ == "__main__":
    unittest.main()
