from __future__ import annotations

import re
import tomllib
import unittest
from collections import deque
from pathlib import Path

from generated_scan import WindowCollector, chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_HOOKS = [
    {"address": 0x82E76ED4, "name": "ApplyGotwSeed", "registers": ["r3", "r4", "r5"]},
    {"address": 0x82E76ED8, "name": "LogGotwSeed", "registers": ["r30", "r29"]},
    {
        "address": 0x82D7DC44,
        "name": "GuardGotwOutcome",
        "registers": ["r28"],
        "jump_address_on_true": 0x82D7DC90,
    },
    {
        "address": 0x82DBF138,
        "name": "GuardGotwScoreCache",
        "jump_address_on_true": 0x82DBF13C,
    },
    {
        "address": 0x82DBF14C,
        "name": "GuardGotwWeekCache",
        "jump_address_on_true": 0x82DBF150,
    },
    {
        "address": 0x82DBE69C,
        "name": "GuardGotwSubmission",
        "jump_address_on_true": 0x82DBE7AC,
    },
]
NEW_HOOKS = [
    {"address": 0x82D91A94, "name": "ReapplyGotwSeed", "registers": ["r3", "r4", "r5"]},
    {
        "address": 0x82DADC88,
        "name": "TraceGotwWriterEntry",
        "registers": ["lr", "r3", "r4", "r5"],
    },
    {
        "address": 0x82DADD1C,
        "name": "TraceGotwWriterExit",
        "registers": ["lr", "r3", "r4", "r5", "r31"],
    },
    {
        "address": 0x82D0F13C,
        "name": "TraceGotwGeneratorSeed",
        "registers": ["r9"],
    },
    {"address": 0x82D0F168, "name": "TraceGotwPrng", "registers": ["r27"]},
    {"address": 0x82D0F100, "name": "TraceGotwProcedural"},
    {"address": 0x82D0F0F4, "name": "TraceGotwNamedLoad", "registers": ["r3"]},
]
EXPECTED_HOOKS += NEW_HOOKS

CPP_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\w+|[^\s]')


def token_pattern(text):
    return r"\s*".join(re.escape(token) for token in CPP_TOKEN.findall(text))


def function_body(source, name):
    start = re.search(rf"\b{name}\s*\([^)]*\)[^;{{]*\{{", source)
    if not start:
        raise AssertionError(f"function definition missing: {name}")
    depth = 1
    for token in CPP_TOKEN.finditer(source, start.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                following = CPP_TOKEN.search(source, token.end())
                if following and following.group() == "catch":
                    continue
                return source[start.start() : token.end()]
    raise AssertionError(f"unterminated function: {name}")


class FunctionBodyTests(unittest.TestCase):
    def test_function_try_block_includes_all_handlers(self):
        function = (
            'void record() noexcept try { nested("}"); } '
            "catch (const Error& error) { recover(); } "
            "catch (...) { fail(); }"
        )
        self.assertEqual(
            function_body(function + " void next() {}", "record"),
            function.removeprefix("void "),
        )

    def test_ordinary_function_keeps_nested_handlers(self):
        function = "void record() { try { nested(); } catch (...) { fail(); } }"
        self.assertEqual(
            function_body(function + " void next() {}", "record"),
            function.removeprefix("void "),
        )

    def test_missing_and_unterminated_functions_fail(self):
        for source in (
            "void record();",
            "void record() {",
            "void record() try {} catch (...) {",
        ):
            with self.subTest(source=source), self.assertRaises(AssertionError):
                function_body(source, "record")


def scan_generated(paths):
    """Scan every generated chunk; retain only bounded contract windows."""
    names = [hook["name"] for hook in EXPECTED_HOOKS]
    hook_call = re.compile(r"^\s*(?:if \()?(" + "|".join(names) + r")\(")
    counts = dict.fromkeys(names, 0)
    origins = {}
    prng_anchor_seen = False
    anchors = {
        ("sub_82E76E90", "ctx.lr = 0x82E76ED8;"): "seed_call",
        ("sub_82D91A78", "ctx.lr = 0x82D91A98;"): "late_seed_call",
        ("sub_82DADC88", "ctx.r12.u64 = ctx.lr;"): "writer_entry",
        ("sub_82DADC88", "ctx.lr = 0x82DADD20;"): "writer_exit",
        ("sub_82D0F0D0", "ctx.lr = 0x82D0F0F4;"): "named_load",
        ("sub_82D0F0D0", "loc_82D0F100:"): "procedural",
        (
            "sub_82D0F0D0",
            "ctx.r27.u64 = REX_LOAD_U32(ctx.r9.u32 + 312);",
        ): "generator_seed",
        ("sub_82D0F0D0", "REX_STORE_U32(ctx.r8.u32 + 0, ctx.r27.u32);"): "prng_seed",
        ("sub_82D7DAB0", "loc_82D7DC10:"): "outcome",
        ("sub_82D7DAB0", "ctx.r24.s64 = -1;"): "result_markers",
        ("sub_82D7DAB0", "loc_82D7DC90:"): "unclassified_result",
        ("sub_82D7DAB0", "loc_82D7DCBC:"): "result_cleanup",
        ("sub_82DBF4A8", "ctx.lr = 0x82DBF590;"): "child_gate",
        ("sub_82DBF090", "REX_STORE_U32(ctx.r30.u32 + 36, ctx.r3.u32);"): "cache",
        ("sub_82DBE688", "loc_82DBE7AC:"): "submission_return",
    }
    wanted = {
        "sub_82E76FC0",
        "sub_82E76E90",
        "sub_82E60F70",
        "sub_82D25170",
    }
    collector = WindowCollector()
    native = {}
    for lines in chunk_lines(paths):
        previous = deque(maxlen=48)
        owner = ""
        native_owner = ""
        for line in lines:
            if line.startswith("DEFINE_REX_FUNC("):
                owner = line.split("(", 1)[1].split(")", 1)[0]
                native_owner = owner.removeprefix("__imp__")
                previous.clear()
            collector.advance(line)
            if native_owner in wanted:
                native.setdefault(native_owner, []).append(line)
            if "Gotw" in line:
                match = hook_call.search(line)
                if match:
                    name = match[1]
                    counts[name] += 1
                    origins[name] = owner
                    collector.start(name, [*previous, line], 48)
            key = anchors.get((owner, line.strip()))
            if key and (key != "prng_seed" or not prng_anchor_seen):
                collector.start(key, [*previous, line], 48)
                prng_anchor_seen |= key == "prng_seed"
            previous.append(line)
        collector.end_chunk()
    windows = collector.windows
    return counts, origins, windows, native


def generated_windows():
    return scan_once("gotw", scan_generated)[1]


class GotwContractTests(unittest.TestCase):
    def assert_cpp(self, expected, source):
        self.assertRegex(source, token_pattern(expected))

    @classmethod
    def setUpClass(cls):
        cls.counts, cls.origins, cls.windows, cls.native = generated_windows()

    def test_hook_boundaries(self):
        config = tomllib.loads(
            (ROOT / "config/gotw_hooks.toml").read_text(encoding="utf-8")
        )
        self.assertEqual(config["midasm_hook"], EXPECTED_HOOKS)
        header = (ROOT / "src/gotw_hooks.h").read_text(encoding="utf-8")
        for hook in ("ApplyGotwSeed", "ReapplyGotwSeed"):
            self.assert_cpp(
                f"void {hook}(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept;",
                header,
            )
        self.assertNotIn(0x82D8685C, [hook["address"] for hook in EXPECTED_HOOKS])

    def test_argument_only_writes_and_unconditional_info(self):
        hooks = (ROOT / "src/gotw_hooks.cpp").read_text(encoding="utf-8")
        apply = function_body(hooks, "ApplyGotwSeed")
        self.assertIn("LatchForGame(week.u32, setup.u32)", apply)
        self.assert_cpp("if (policy.customWeek) { week.u64 = policy.week; }", apply)
        self.assert_cpp("if (policy.IsCustom())", apply)
        self.assert_cpp("seed.u64 = policy.seed;", apply)
        self.assertEqual(
            re.findall(r"\b(\w+)\.u64\s*=(?!=)", hooks),
            ["week", "seed", "week", "seed"],
        )
        self.assertNotIn("REX_STORE_", hooks)
        self.assertNotIn("TranslateVirtual<uint8_t*>", hooks)
        self.assertNotIn("REXCVAR_GET", hooks)
        logger = function_body(hooks, "LogGotwSeed")
        self.assertIn("REXLOG_INFO", logger)
        self.assertIn("week={} seed={}", logger)
        self.assertIn("RecordAutomaticSeed(seed)", logger)
        self.assertNotIn("trace", logger)
        self.assertIn("native_week={}", logger)

    def test_title_settings_and_origin(self):
        source = (ROOT / "src/gotw_settings.cpp").read_text(encoding="utf-8")
        self.assert_cpp(
            'REXCVAR_DEFINE_BOOL(gotw_custom_week, false, "ReRevved",', source
        )
        self.assert_cpp('REXCVAR_DEFINE_STRING(gotw_week, "1", "ReRevved",', source)
        self.assert_cpp(
            'REXCVAR_DEFINE_BOOL(gotw_standard_rules, true, "ReRevved",', source
        )
        for removed in ("gotw_custom_seed", "gotw_seed", "ParseSeed", "IsShippedSeed"):
            self.assertNotIn(removed, source)
        self.assertIn(".validator(", source)
        self.assertIn("std::from_chars", source)
        self.assert_cpp("result.ptr != text.data() + text.size()", source)
        latch = function_body(source, "LatchForGame")
        self.assert_cpp(
            "next.customWeek = ParseWeek(REXCVAR_GET(gotw_week), next.week);", latch
        )
        self.assert_cpp(
            "if (next.customWeek) { next.seed = SeedForWeek(next.week); }", latch
        )
        self.assertIn('logRejected("week",', latch)
        self.assertNotIn("REXCVAR_GET", function_body(source, "ForGame"))
        self.assert_cpp("mode == 2 && ForGame().IsCustom()", source)
        header = (ROOT / "src/gotw_settings.h").read_text(encoding="utf-8")
        self.assert_cpp("return customWeek;", function_body(header, "IsCustom"))
        self.assertNotIn("customSeed", header)
        overlay = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")
        for text in (
            '{ "Game of the Week", { "gotw_custom_week", "gotw_week", "gotw_standard_rules" } }',
            '{ "gotw_custom_week", Audience::kPlayer }',
            '{ "gotw_week", Audience::kPlayer }',
            '{ "gotw_standard_rules", Audience::kPlayer }',
            '{ "gotw_custom_week", "Week" }',
            '{ "gotw_week", "Week" }',
            '{ "gotw_standard_rules", "Standard Rules" }',
            'layout.inline_toggles = { { "gotw_custom_week", "gotw_week", true } };',
            """{ "gotw_custom_week", "Enable to play a different week's game instead of this week's." }""",
            '{ "gotw_standard_rules", "Enable to play Game of the Week without gameplay changes." }',
        ):
            self.assert_cpp(text, overlay)
        for removed in ("gotw_custom_seed", "gotw_seed", "gotw_weekly_seed"):
            self.assertNotIn(removed, overlay)

    def test_weekly_field_store_back(self):
        source = (ROOT / "src/gotw_settings.cpp").read_text(encoding="utf-8")
        record = function_body(source, "RecordAutomaticSeed")
        self.assert_cpp("if (!selection.customWeek) { selection.seed = seed; }", record)
        self.assertNotIn("dispatchAutomatic", record)
        week = function_body(source, "RecordAutomaticWeek")
        self.assert_cpp(
            "!active.customWeek && active.nativeWeek == week && IsValidWeek(week)", week
        )
        self.assert_cpp("dispatchAutomatic(week, active.launch);", week)
        dispatcher = function_body(source, "dispatchAutomatic")
        self.assertIn("alive.lock()", dispatcher)
        self.assert_cpp("active.launch != launch", dispatcher)
        self.assertIn("seedDispatcher([", dispatcher)
        self.assert_cpp("!REXCVAR_GET(gotw_custom_week)", dispatcher)
        self.assert_cpp(
            'rex::cvar::SetFlagByName("gotw_week", std::to_string(value));', dispatcher
        )
        self.assertGreater(
            dispatcher.index("REXCVAR_GET"), dispatcher.index("seedDispatcher([")
        )

    def test_standard_rules_freeze_and_consumers(self):
        settings = (ROOT / "src/gotw_settings.cpp").read_text(encoding="utf-8")
        freeze = function_body(settings, "FreezeStandardRules")
        self.assert_cpp("mode == 2 && REXCVAR_GET(gotw_standard_rules)", freeze)
        self.assert_cpp(
            "standardRules.store(enabled, std::memory_order_relaxed);", freeze
        )
        self.assertNotIn("REXCVAR", function_body(settings, "StandardRulesForGame"))
        runtime = (ROOT / "src/start_resources_runtime.cpp").read_text(encoding="utf-8")
        wrapper = function_body(runtime, "RunNativeNewGameSetup")
        for read in ("loadU32(base, kSetupObject)", "loadU32(base, setup + 4)"):
            self.assertIn(read, wrapper)
        self.assert_cpp("constexpr uint32_t kSetupObject = 0x8314EFC8;", runtime)
        gate = wrapper.index("gotw::FreezeStandardRules(mode)")
        native = wrapper.index("__imp__sub_82D25170(context, base)")
        for consumer in (
            "raging_barbarians::FreezeForGame(standardRules)",
            "occ::FreezeForGame(standardRules)",
            "SetupScope scope(standardRules)",
        ):
            self.assertLess(gate, wrapper.index(consumer))
            self.assertLess(wrapper.index(consumer), native)
        self.assert_cpp(
            "const int32_t value = standardRules ? 0 : REXCVAR_GET(resources);", runtime
        )
        for name, config in (
            ("raging_barbarians", "raging_barbarians"),
            ("occ", "one_city_challenge"),
        ):
            source = (ROOT / f"src/{name}_settings.cpp").read_text(encoding="utf-8")
            self.assert_cpp(
                f"!standardRules && REXCVAR_GET({config})",
                function_body(source, "FreezeForGame"),
            )
        ai = (ROOT / "src/ai_civilizations_hooks.cpp").read_text(encoding="utf-8")
        self.assert_cpp(
            "const bool standardRules = rerevved::gotw::StandardRulesForGame();", ai
        )
        self.assert_cpp(
            "standardRules ? ai_civs::kAllAiCivilizations : resolveLimit(network, source)",
            ai,
        )
        load = function_body(ai, "ResetAiCivilizationsOnLoad")
        self.assertNotIn("StandardRules", load)
        self.assertLess(
            load.index("FreezeOccForGame()"), load.index("RestoreAfterLoadReset()")
        )
        sidecar = (ROOT / "src/save_sidecar_hooks.cpp").read_text(encoding="utf-8")
        capture = function_body(sidecar, "captureState")
        for frozen in (
            "raging_barbarians::Evaluate(0).enabled",
            "occ::ForGame()",
            "start_resources::ForGame()",
            "resource_identity_overrides::Current()",
        ):
            self.assertIn(frozen, capture)
        self.assertNotIn("REXCVAR", capture)

    def test_shipped_map_list_order_and_week_mapping(self):
        source = (ROOT / "src/gotw_settings.cpp").read_text(encoding="utf-8")
        values = re.search(r"kMapList\s*=\s*\{([^}]+)\}", source)[1]
        actual = [int(value) for value in re.findall(r"\d+", values)]
        excluded = {
            5,
            7,
            13,
            19,
            28,
            34,
            54,
            66,
            82,
            88,
            94,
            115,
            124,
            130,
            137,
            148,
            160,
            161,
            212,
            226,
            255,
            265,
            296,
        }
        self.assertEqual(
            actual, [value for value in range(1, 301) if value not in excluded]
        )
        self.assertEqual(len(actual), 277)
        self.assertEqual((actual[1084 % 277], actual[1085 % 277]), (276, 277))
        for missing in (*excluded, 0, 301, 500, 0xFFFFFFFF):
            self.assertNotIn(missing, actual)
        self.assert_cpp(
            "return kMapList[week % kMapList.size()];",
            function_body(source, "SeedForWeek"),
        )
        self.assert_cpp(
            "return week >= 1 && week <= 0x7FFFFFFFu;",
            function_body(source, "IsValidWeek"),
        )
        self.assertIn("Pregame.FPK ccglobaldefines.xml:228", source)
        for digest in (
            "14E50B0382542B345F29B129175CB1B1B9A4538547E8B5567CAA5A864531A2DE",
            "C68C7BEE458CD4C5B6A3773AFD2EA07D8CC60E7A606DB5D1B82F3626D44445E3",
        ):
            self.assertIn(digest, source)

    def test_native_freeze_order_when_generated(self):
        wanted = {
            "sub_82E76FC0",
            "sub_82E76E90",
            "sub_82E60F70",
            "sub_82D25170",
        }
        windows = self.native
        if not windows:
            self.skipTest("native code has not been generated")
        self.assertEqual(set(windows), wanted)
        menu = "".join(windows["sub_82E76FC0"])
        self.assertLess(
            menu.index("REX_STORE_U32(ctx.r11.u32 + 4, ctx.r10.u32);"),
            menu.index("sub_82E76E90(ctx, base);"),
        )
        self.assertIn("ctx.r10.s64 = 2;", menu)
        early = "".join(windows["sub_82E76E90"])
        self.assertLess(early.index("ApplyGotwSeed("), early.index("sub_82DADC88("))
        caller = "".join(windows["sub_82E60F70"])
        self.assertIn("ctx.lr = 0x82E610A4;", caller)
        self.assertIn("sub_82D25170(ctx, base);", caller)
        setup = "".join(windows["sub_82D25170"])
        self.assertLess(setup.index("sub_82D217C8("), setup.index("sub_82D11AD8("))
        self.assertIn("ctx.lr = 0x82D25258;", setup)
        self.assertIn("ctx.lr = 0x82D25310;", setup)
        ai = (ROOT / "src/ai_civilizations.h").read_text(encoding="utf-8")
        self.assertIn("kNewGameRecountReturn = 0x82D25310", ai)

    def test_controls_layout(self):
        overlay = (ROOT / "src/overlay_settings.cpp").read_text(encoding="utf-8")
        tabs = re.split(r"layout\.tabs\s*=\s*\{", overlay, maxsplit=1)[1].split(
            "\n    };", 1
        )[0]
        self.assertEqual(
            re.findall(r'^        \{\s*"(\w+)"', tabs, re.MULTILINE),
            [
                "Gameplay",
                "Notifications",
                "Online",
                "Controls",
                "Display",
                "Audio",
                "Developer",
            ],
        )
        controls = re.split(r'\{\s*"Controls"\s*,', tabs, maxsplit=1)[1]
        controls = re.split(r'\{\s*"Display"\s*,', controls, maxsplit=1)[0]
        for text in (
            '{ "", { "input_gamepad_in_background", "input_vibration", "mnk_mode", "mnk_mouse", "mnk_sensitivity" } }',
            '{ "Keybinds", { "@Input/Keybinds/Controller" }, true }',
            '{ "Overlay Hotkeys", { "@Input/Keybinds" }, true }',
        ):
            self.assert_cpp(text, controls)
        self.assert_cpp('{ "input_vibration", Audience::kPlayer }', overlay)
        self.assert_cpp('{ "input_vibration", "Vibration" }', overlay)
        self.assert_cpp(
            'layout.float_decimals = { { "mnk_sensitivity", 2 } };', overlay
        )

    def test_late_correction_requires_readable_live_gotw_mode(self):
        source = (ROOT / "src/gotw_hooks.cpp").read_text(encoding="utf-8")
        late = function_body(source, "ReapplyGotwSeed")
        self.assert_cpp(
            "ReapplyGotwSeed(PPCRegister& setup, PPCRegister& week, PPCRegister& seed) noexcept",
            late,
        )
        self.assert_cpp(
            "rerevved::gotw::detail::ReapplySeed(setup, week, seed, readWord);", late
        )
        reapply = function_body(source, "ReapplySeed")
        self.assert_cpp("const auto policy = rerevved::gotw::ForGame();", reapply)
        self.assert_cpp("if (!policy.IsCustom()) { return; }", reapply)
        self.assert_cpp("constexpr uint32_t kSetupGlobal = 0x8314EFC8;", source)
        self.assert_cpp(
            "if (read && read(kSetupGlobal, setup) && setup != 0 && setup == policy.setup && "
            "receiver.u32 == setup && week.u32 == policy.week && "
            "read(uint64_t{ setup } + 4, mode) && mode == 2) "
            "{ if (policy.customWeek) { week.u64 = policy.week; } seed.u64 = policy.seed; }",
            reapply,
        )
        self.assertNotIn("LatchForGame", late)
        self.assertNotIn("REXCVAR", late)
        self.assertNotIn("LatchForGame", reapply)
        self.assertNotIn("REXCVAR", reapply)

    def test_observers_are_default_off_bounded_and_read_only(self):
        settings = (ROOT / "src/gotw_settings.cpp").read_text(encoding="utf-8")
        source = (ROOT / "src/gotw_hooks.cpp").read_text(encoding="utf-8")
        self.assert_cpp('REXCVAR_DEFINE_BOOL(gotw_trace, false, "ReRevved",', settings)
        trace = function_body(source, "traceEvent")
        self.assert_cpp("if (!rerevved::gotw::TraceEnabled()) { return; }", trace)
        self.assertIn("++traceSequence", trace)
        self.assertIn("kTraceEventCap", trace)
        self.assertIn("truncated cap={}", trace)
        for hook in NEW_HOOKS[1:]:
            body = function_body(source, hook["name"])
            self.assertIn("noexcept", body)
            self.assertIn("traceEvent(", body)
            self.assertNotRegex(body, r"\.\w+\s*=(?!=)")
            self.assertNotIn("REX_STORE", body)
        for address in ("0x134", "0x138", "0x13C", "0x82F7AD00", "0x82F7AD04"):
            self.assertIn(address, source)

    def test_native_seed_origin_when_generated(self):
        if "seed_call" not in self.windows:
            self.skipTest("GOTW native code has not been generated")
        window = self.windows["seed_call"]
        for native in (
            "ctx.cr6.compare<int32_t>(ctx.r3.s32, 1, ctx.xer);",
            "if (!ctx.cr6.eq) goto loc_82E76F24;",
            "ctx.r5.s64 = 0;",
            "ctx.r4.u64 = ctx.r29.u64;",
            "ctx.r3.u64 = REX_LOAD_U32(ctx.r30.u32 + -4152);",
            "ctx.lr = 0x82E76ED8;",
            "sub_82DADC88(ctx, base);",
        ):
            self.assertIn(native, window)
        self.assertLess(window.index("ctx.r5.s64 = 0;"), window.index("sub_82DADC88("))

    def test_new_native_boundaries_across_all_chunks(self):
        if "late_seed_call" not in self.windows:
            self.skipTest("GOTW native code has not been generated")
        late = self.windows["late_seed_call"]
        for native in (
            "ctx.r4.u64 = REX_LOAD_U32(ctx.r3.u32 + 52);",
            "ctx.r5.s64 = 0;",
            "ctx.r3.u64 = REX_LOAD_U32(ctx.r11.u32 + -4152);",
            "ctx.lr = 0x82D91A98;",
            "sub_82DADC88(ctx, base);",
        ):
            self.assertIn(native, late)
        entry = self.windows["writer_entry"]
        self.assertIn("ctx.r12.u64 = ctx.lr;", entry)
        self.assertIn("__savegprlr_29(ctx, base);", entry)
        end = self.windows["writer_exit"]
        self.assertIn("REX_STORE_U32(ctx.r31.u32 + 316, ctx.r10.u32);", end)
        self.assertIn("ctx.lr = 0x82DADD20;", end)
        self.assertIn("sub_821B15C0(ctx, base);", end)
        named = self.windows["named_load"]
        self.assertIn("sub_82D0ADE0(ctx, base);", named)
        self.assertIn("ctx.r11.u64 = ctx.r3.u32 & 0xFF;", named)
        self.assertIn("if (!ctx.cr6.eq) goto loc_82D10FC4;", named)
        procedural = self.windows["procedural"]
        self.assertIn("loc_82D0F100:", procedural)
        self.assertIn("ctx.r9.u64 = REX_LOAD_U32(ctx.r10.u32 + -4152);", procedural)
        generator = self.windows["generator_seed"]
        self.assertIn("ctx.r27.u64 = REX_LOAD_U32(ctx.r9.u32 + 312);", generator)
        prng = self.windows["prng_seed"]
        self.assertIn("REX_STORE_U32(ctx.r8.u32 + 4, ctx.r24.u32);", prng)
        self.assertIn("REX_STORE_U32(ctx.r8.u32 + 0, ctx.r27.u32);", prng)
        self.assertIn("ctx.r11.s64 = -2096234496;", prng)

    def test_generated_hooks_and_cleanup(self):
        if "seed_call" not in self.windows:
            self.skipTest("GOTW native code has not been generated")
        self.assertEqual(self.counts, dict.fromkeys(self.counts, 1))
        self.assertEqual(
            self.origins,
            {
                "ApplyGotwSeed": "sub_82E76E90",
                "LogGotwSeed": "sub_82E76E90",
                "GuardGotwOutcome": "sub_82D7DAB0",
                "GuardGotwScoreCache": "sub_82DBF090",
                "GuardGotwWeekCache": "sub_82DBF090",
                "GuardGotwSubmission": "sub_82DBE688",
                "ReapplyGotwSeed": "sub_82D91A78",
                "TraceGotwWriterEntry": "sub_82DADC88",
                "TraceGotwWriterExit": "sub_82DADC88",
                "TraceGotwGeneratorSeed": "sub_82D0F0D0",
                "TraceGotwPrng": "sub_82D0F0D0",
                "TraceGotwProcedural": "sub_82D0F0D0",
                "TraceGotwNamedLoad": "sub_82D0F0D0",
            },
        )
        seed = self.windows["seed_call"]
        self.assertLess(
            seed.index("ApplyGotwSeed(ctx.r3, ctx.r4, ctx.r5);"),
            seed.index("sub_82DADC88("),
        )
        self.assertLess(
            seed.index("sub_82DADC88("), seed.index("LogGotwSeed(ctx.r30, ctx.r29);")
        )
        late = self.windows["late_seed_call"]
        self.assertLess(
            late.index("ctx.r5.s64 = 0;"),
            late.index("ReapplyGotwSeed(ctx.r3, ctx.r4, ctx.r5);"),
        )
        self.assertLess(
            late.index("ReapplyGotwSeed(ctx.r3, ctx.r4, ctx.r5);"),
            late.index("sub_82DADC88("),
        )
        entry = self.windows["writer_entry"]
        self.assertLess(
            entry.index("TraceGotwWriterEntry(ctx.lr, ctx.r3, ctx.r4, ctx.r5);"),
            entry.index("ctx.r12.u64 = ctx.lr;"),
        )
        end = self.windows["writer_exit"]
        self.assertLess(
            end.index("REX_STORE_U32(ctx.r31.u32 + 316, ctx.r10.u32);"),
            end.index("TraceGotwWriterExit(ctx.lr, ctx.r3, ctx.r4, ctx.r5, ctx.r31);"),
        )
        self.assertLess(end.index("TraceGotwWriterExit("), end.index("sub_821B15C0("))
        named = self.windows["named_load"]
        self.assertLess(
            named.index("sub_82D0ADE0("), named.index("TraceGotwNamedLoad(ctx.r3);")
        )
        self.assertLess(
            named.index("TraceGotwNamedLoad("),
            named.index("ctx.r11.u64 = ctx.r3.u32 & 0xFF;"),
        )
        procedural = self.windows["procedural"]
        self.assertLess(
            procedural.index("loc_82D0F100:"),
            procedural.index("TraceGotwProcedural();"),
        )
        self.assertLess(
            procedural.index("TraceGotwProcedural();"),
            procedural.index("ctx.r11.s64 = -2097676288;"),
        )
        generator = self.windows["generator_seed"]
        self.assertLess(
            generator.index("TraceGotwGeneratorSeed(ctx.r9);"),
            generator.index("ctx.r27.u64 = REX_LOAD_U32(ctx.r9.u32 + 312);"),
        )
        prng = self.windows["prng_seed"]
        self.assertLess(
            prng.index("REX_STORE_U32(ctx.r8.u32 + 0, ctx.r27.u32);"),
            prng.index("TraceGotwPrng(ctx.r27);"),
        )
        self.assertLess(
            prng.index("TraceGotwPrng(ctx.r27);"),
            prng.index("ctx.r11.s64 = -2096234496;"),
        )
        outcome = self.windows["outcome"]
        self.assertLess(
            outcome.index("if (!ctx.cr6.eq) goto loc_82D7DCBC;"),
            outcome.index("GuardGotwOutcome(ctx.r28)"),
        )
        guard = self.windows["GuardGotwOutcome"]
        self.assertLess(
            guard.index("if (ctx.cr6.eq) goto loc_82D7DCA8;"),
            guard.index("GuardGotwOutcome(ctx.r28)"),
        )
        self.assertLess(
            guard.index("if (ctx.cr6.lt) goto loc_82D7DC9C;"),
            guard.index("GuardGotwOutcome(ctx.r28)"),
        )
        self.assertIn("goto loc_82D7DC90;", guard)
        unclassified = self.windows["unclassified_result"].split("loc_82D7DC90:", 1)[1]
        self.assertIn("ctx.r24.s64 = -1;", self.windows["result_markers"])
        self.assertIn("ctx.r22.s64 = 1;", self.windows["result_markers"])
        self.assertIn("REX_STORE_U32(ctx.r31.u32 + 24, ctx.r24.u32);", unclassified)
        self.assertIn("REX_STORE_U8(ctx.r31.u32 + 12, ctx.r22.u8);", unclassified)
        child = self.windows["child_gate"]
        self.assertIn("ctx.r11.s64 = ctx.r11.s64 + -17;", child)
        self.assertIn("if (ctx.cr6.gt) goto loc_82DBF634;", child)
        cache = self.windows["cache"]
        self.assertLess(
            cache.index("GuardGotwScoreCache()"),
            cache.index("REX_STORE_U32(ctx.r30.u32 + 36, ctx.r3.u32);"),
        )
        self.assertIn("goto loc_82DBF13C;", cache)
        self.assertLess(
            cache.index("GuardGotwWeekCache()"),
            cache.index("REX_STORE_U32(ctx.r30.u32 + 40, ctx.r3.u32);"),
        )
        self.assertIn("goto loc_82DBF150;", cache)
        self.assertIn("REX_STORE_U32(ctx.r30.u32 + 4, ctx.r11.u32);", cache)
        submission = self.windows["GuardGotwSubmission"]
        self.assertIn("goto loc_82DBE7AC;", submission)
        self.assertLess(
            submission.index("GuardGotwSubmission()"),
            submission.index("ctx.cr6.compare<int32_t>(ctx.r31.s32, -1, ctx.xer);"),
        )
        native_return = self.windows["submission_return"].split("loc_82DBE7AC:", 1)[1]
        self.assertIn("ctx.r3.s64 = 1;", native_return)
        self.assertIn("__restgprlr_26(ctx, base);", native_return)
        cleanup = self.windows["result_cleanup"].split("loc_82D7DCBC:", 1)[1]
        self.assertIn("sub_82DBEF18(ctx, base);", cleanup)


if __name__ == "__main__":
    unittest.main()
