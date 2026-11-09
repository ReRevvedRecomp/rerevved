from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/network_admission_hooks.toml"
HOOKS = ROOT / "src/network_admission_hooks.cpp"
SESSION = ROOT / "src/gameplay_session_settings.cpp"
RUNTIME = ROOT / "src/start_resources_runtime.cpp"

HOOK = (
    0x82E2B16C,
    "AdmitNetworkGameStart",
    ["r1", "r13", "r30", "r31", "fpscr"],
)

OWNER_WINDOW = (
    (0x82E2B160, "mtctr r10"),
    (0x82E2B164, "bctrl"),
    (0x82E2B168, "mr r30,r3"),
    (0x82E2B16C, "clrlwi r11,r30,24"),
    (0x82E2B170, "li r29,0"),
    (0x82E2B174, "cmplwi cr6,r11,0"),
    (0x82E2B178, "beq cr6,0x82e2b26c"),
    (0x82E2B17C, "lwz r3,112(r31)"),
    (0x82E2B180, "cmplwi cr6,r3,0"),
    (0x82E2B184, "beq cr6,0x82e2b1c4"),
    (0x82E2B188, "lwz r11,4(r3)"),
    (0x82E2B18C, "lwz r10,120(r31)"),
    (0x82E2B190, "cmpw cr6,r11,r10"),
    (0x82E2B194, "bne cr6,0x82e2b1a4"),
    (0x82E2B198, "lwz r11,116(r31)"),
    (0x82E2B19C, "cmplwi cr6,r11,0"),
    (0x82E2B1A0, "beq cr6,0x82e2b238"),
)
REQUEST_WINDOW = (
    (0x82E54FD8, "lis r11,-32000"),
    (0x82E54FDC, "li r10,1"),
    (0x82E54FE0, "lwz r11,-10716(r11)"),
    (0x82E54FE4, "stw r10,120(r11)"),
    (0x82E54FE8, "blr"),
)
RETURN_WINDOW = (
    (0x82E6AFF8, "mflr r12"),
    (0x82E6B018, "bl 0x82d8b6c8"),
    (0x82E6B020, "bl 0x82d8e798"),
    (0x82E6B028, "li r4,10"),
    (0x82E6B030, "lwz r10,28(r11)"),
    (0x82E6B034, "mtctr r10"),
    (0x82E6B038, "bctrl"),
)

JOIN_CONFIG = ROOT / "config/join_refusal_hooks.toml"
JOIN_HOOKS = ROOT / "src/join_refusal_hooks.cpp"
JOIN_HOOK = (0x82D89740, "RefuseJoinInProgress", ["r3", "r9"], 0x82D897B8)
POLICY_START = 0x82D89678
POLICY_WORDS = 82
POLICY_WINDOW = {
    0x82D89678: "mflr r12",
    0x82D89720: "bl 0x82d8aa60",
    0x82D89724: "clrlwi r11,r3,24",
    0x82D8972C: "beq cr6,0x82d897b4",
    0x82D89734: "lwz r11,-10716(r11)",
    0x82D89738: "lwz r10,112(r11)",
    0x82D8973C: "lwz r9,4(r10)",
    0x82D89740: "cmpwi cr6,r9,2",
    0x82D89744: "bne cr6,0x82d897b4",
    0x82D89748: "mr r4,r29",
    0x82D89750: "bl 0x82d8b400",
    0x82D89764: "xori r3,r8,1",
    0x82D89770: "beq cr6,0x82d897b8",
    0x82D897B4: "mr r3,r31",
    0x82D897B8: "addi r1,r1,112",
    0x82D897BC: "b 0x827f7cdc",
}

MUTATIONS = (
    "ApplyNewGame",
    "ResetProgress",
    "ResetCombatObservers",
    "SuspendModRulesForGame",
    "StoreNetworkSettings",
    "Publish(",
    "RecordSetupOutcome",
    "SetupScope",
    "__imp__sub_82D25170",
    "sub_82D25170",
    "sub_82D217C8",
    "sub_82E60F70",
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def encode(address: int, text: str) -> int:
    """Encode the few PowerPC forms the windows use."""
    parts = text.replace(",", " ").split()
    mnemonic, args = parts[0], parts[1:]

    def reg(name: str) -> int:
        return int(name[1:])

    def displaced(name: str) -> tuple[int, int]:
        match = re.fullmatch(r"(-?\d+)\((r\d+)\)", name)
        assert match, name
        return int(match[1]), reg(match[2])

    if mnemonic in ("lwz", "stw"):
        offset, base = displaced(args[1])
        opcode = 32 if mnemonic == "lwz" else 36
        return opcode << 26 | reg(args[0]) << 21 | base << 16 | offset & 0xFFFF
    if mnemonic == "li":
        return 14 << 26 | reg(args[0]) << 21 | int(args[1]) & 0xFFFF
    if mnemonic == "lis":
        return 15 << 26 | reg(args[0]) << 21 | int(args[1]) & 0xFFFF
    if mnemonic == "mr":
        return (
            31 << 26
            | reg(args[1]) << 21
            | reg(args[0]) << 16
            | reg(args[1]) << 11
            | 444 << 1
        )
    if mnemonic == "clrlwi":
        return (
            21 << 26
            | reg(args[1]) << 21
            | reg(args[0]) << 16
            | int(args[2]) << 6
            | 31 << 1
        )
    if mnemonic == "cmplwi":
        return (
            10 << 26
            | int(args[0][2]) << 23
            | reg(args[1]) << 16
            | int(args[2]) & 0xFFFF
        )
    if mnemonic == "cmpw":
        return (
            31 << 26 | int(args[0][2]) << 23 | reg(args[1]) << 16 | reg(args[2]) << 11
        )
    if mnemonic in ("beq", "bne"):
        condition = 12 if mnemonic == "beq" else 4
        bit = 4 * int(args[0][2]) + 2
        return (
            16 << 26
            | condition << 21
            | bit << 16
            | (int(args[1], 16) - address) & 0xFFFC
        )
    if mnemonic == "bl":
        return 18 << 26 | (int(args[0], 16) - address) & 0x03FFFFFC | 1
    if mnemonic == "mtctr":
        return 31 << 26 | reg(args[0]) << 21 | 9 << 16 | 467 << 1
    if mnemonic == "mflr":
        return 31 << 26 | reg(args[0]) << 21 | 8 << 16 | 339 << 1
    if mnemonic == "bctrl":
        return 0x4E800421
    if mnemonic == "blr":
        return 0x4E800020
    raise AssertionError(text)


def profile(source: str, name: str) -> list[tuple[int, int]]:
    body = re.search(r"constexpr NativeWord %s\[\] = \{(.*?)\n\};" % name, source, re.S)
    assert body, name
    return [
        (int(address, 16), int(value, 16))
        for address, value in re.findall(
            r"\{ (0x[0-9A-F]{8}), (0x[0-9A-F]{8}) \}", body[1]
        )
    ]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("\n{\n", start)
    end = source.index("\n}\n", brace)
    return source[brace:end]


class NetworkAdmissionContractTests(unittest.TestCase):
    def test_encoder_matches_known_native_words(self):
        known = (
            (0x82CDDF34, "bl 0x82cd4cf0", 0x4BFF6DBD),
            (0x82CE16AC, "lis r10,-31979", 0x3D408315),
            (0x82CE16B4, "lwz r11,25412(r10)", 0x816A6344),
            (0x82CE16BC, "stw r11,25412(r10)", 0x916A6344),
            (0x82CE1748, "clrlwi r10,r11,27", 0x556A06FE),
            (0x82D1B43C, "cmpw cr6,r19,r18", 0x7F139000),
        )
        for address, text, word in known:
            self.assertEqual(encode(address, text), word, text)

    def test_hook_is_the_single_early_seam(self):
        hooks = tomllib.loads(read(CONFIG))["midasm_hook"]
        address, name, registers = HOOK
        self.assertEqual(
            hooks, [{"address": address, "name": name, "registers": registers}]
        )
        self.assertNotIn("jump_address_on_true", hooks[0])
        manifest = read(ROOT / "rerevved_manifest.toml")
        self.assertIn('"config/network_admission_hooks.toml"', manifest)
        taken = []
        for path in (ROOT / "config").glob("*.toml"):
            if path != CONFIG:
                taken += [
                    row["address"]
                    for row in tomllib.loads(read(path)).get("midasm_hook", [])
                ]
        self.assertNotIn(address, taken)
        source = read(HOOKS)
        header = read(ROOT / "src/network_admission_hooks.h")
        signature = (
            "void AdmitNetworkGameStart(PPCRegister& stack, PPCRegister& threadPointer, "
            "PPCRegister& updateResult, PPCRegister& app, PPCFPSCRRegister& fpscr)"
        )
        self.assertIn(signature, header)
        self.assertEqual(source.count(signature), 1)
        self.assertEqual(signature.count("PPCRegister&"), len(registers) - 1)

    def test_native_windows_are_guarded_by_independent_encodings(self):
        source = read(HOOKS)
        owner = profile(source, "kOwnerProfile")
        expected = [
            (address, encode(address, text))
            for address, text in OWNER_WINDOW + REQUEST_WINDOW
        ]
        self.assertEqual(owner, expected)
        self.assertEqual(
            profile(source, "kReturnProfile"),
            [(address, encode(address, text)) for address, text in RETURN_WINDOW],
        )
        body = function_body(source, "void admitStart(")
        self.assertLess(body.index("ownerProfile"), body.index("ReadTransition"))
        self.assertIn("returnProfile", body)

    def test_refusal_reaches_no_setup_mutation(self):
        source = read(HOOKS)
        for name in MUTATIONS:
            self.assertNotIn(name, source, name)
        session = read(SESSION)
        admit = function_body(session, "Admission detail::AdmitNetworkGame(")
        for name in MUTATIONS + ("weeklyAttempt", "acceptedWeeklyWeek"):
            self.assertNotIn(name, admit, name)
        body = function_body(source, "void admitStart(")
        order = [
            "IsNetworkGameStart",
            "AdmitNetworkGame(",
            "ReadTransition(memory, app.u32, updateResult.u32);\n        const auto action",
            "DecideRecheck(",
            "if (result.admitted)",
            "PostNotification",
            "cancelAttempt(memory, stack, threadPointer, fpscr, completeRoute)",
        ]
        positions = [body.index(fragment) for fragment in order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("catch (...)", body)
        cancel = function_body(source, "void cancelAttempt(")
        self.assertLess(
            cancel.index("sub_82E54FD8"), cancel.index("MatchFrontendReturn")
        )
        self.assertLess(
            cancel.index("MatchFrontendReturn"), cancel.index("sub_82E6AFF8")
        )

    def test_accept_path_keeps_the_setup_wrapper_order(self):
        runtime = read(RUNTIME)
        wrapper = function_body(runtime, "void RunNativeNewGameSetup(")
        order = [
            "SuspendModRulesForGame(false);",
            "custom_achievements::ResetProgress();",
            "custom_achievement_combat::ResetCombatObservers();",
            "gameplay_session::Prepare(network, setup, mode)",
            "gameplay_session::ApplyNewGame(snapshot, network, mode)",
            "SetupScope scope(standardRules, settings.resources);",
            "ai_civilizations::SetupScope civilizations(",
            "__imp__sub_82D25170(context, base);\n    ai_civilizations::CommitNewGame();",
            "custom_achievements::RecordSetupOutcome(",
        ]
        positions = [wrapper.index(fragment) for fragment in order]
        self.assertEqual(positions, sorted(positions))
        # Late rejection stays as the fallback when the early seam did not stage a snapshot.
        self.assertIn(
            "gameplay_session::SetupGate gate(snapshot.settings.has_value());", wrapper
        )
        self.assertIn('rex::ui::PostNotification("Can\'t start this game"', wrapper)

    def test_prepare_consumes_a_staged_admission_for_networks_only(self):
        session = read(SESSION)
        prepare = function_body(session, "host_settings::GameSnapshot detail::Prepare(")
        self.assertLess(
            prepare.index("takeStagedAdmission(setup, mode, stamp)"),
            prepare.index("return capture(networkGame);"),
        )
        self.assertLess(
            prepare.index("if (networkGame)"), prepare.index("takeStagedAdmission")
        )
        self.assertIn(
            "stagedAdmission    = {};", function_body(session, "void ResetOnLoad(")
        )

    def test_files_are_registered_and_ascii(self):
        cmake = read(ROOT / "CMakeLists.txt")
        tests = read(ROOT / "tests/rerevved_tests.cmake")
        for name in (
            "network_admission.cpp",
            "network_admission.h",
            "network_admission_hooks.cpp",
            "network_admission_hooks.h",
            "lobby_content_watch.cpp",
            "lobby_content_watch.h",
        ):
            self.assertIn(f"src/{name}", cmake)
        for name in ("src/network_admission.cpp", "src/lobby_content_watch.cpp"):
            self.assertIn(name, tests)
        self.assertNotIn("network_admission_hooks.cpp", tests)
        for name in ("network_admission_test.cpp", "lobby_content_watch_test.cpp"):
            self.assertIn(f"tests/{name}", tests)
        for path in (
            CONFIG,
            HOOKS,
            ROOT / "src/network_admission.cpp",
            ROOT / "src/network_admission.h",
            ROOT / "src/network_admission_hooks.h",
            ROOT / "src/lobby_content_watch.cpp",
            ROOT / "src/lobby_content_watch.h",
            ROOT / "tests/network_admission_test.cpp",
            ROOT / "tests/lobby_content_watch_test.cpp",
            Path(__file__),
        ):
            read(path).encode("ascii")

    def test_lobby_watch_is_started_and_stopped_with_presence(self):
        app = read(ROOT / "src/app.cpp")
        self.assertEqual(app.count("rerevved::lobby_content_watch::Start();"), 1)
        self.assertEqual(
            app.count("rerevved::lobby_content_watch::Stop();"),
            app.count("rerevved::StopPresence();"),
        )

    def test_generated_hook_site(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        name = HOOK[1]
        if name not in text:
            self.skipTest("network admission hook requires owner codegen")
        call = f"{name}(ctx.r1, ctx.r13, ctx.r30, ctx.r31, ctx.fpscr);"
        site = re.compile(
            r"loc_82E2B16C:\n\t// clrlwi r11,r30,24\n\t%s\n\tctx\.r11\.u64 = ctx\.r30\.u32 & 0xFF;"
            % re.escape(call)
        )
        matches = list(site.finditer(text))
        self.assertEqual(len(matches), 1)
        self.assertEqual(text.count(call), 1)

        start = text.rindex("DEFINE_REX_FUNC(sub_82E2B138)", 0, matches[0].start())
        end = text.index("\n}\n", matches[0].end())
        owner = text[start:end]
        hook = owner.index(call)
        for needle in (
            "ctx.lr = 0x82E2B168;",
            "// lwz r11,0(r3)\n\tctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 0);",
        ):
            self.assertIn(needle, owner)
        self.assertLess(owner.index("ctx.lr = 0x82E2B168;"), hook)
        for later in (
            "REX_LOAD_U32(ctx.r31.u32 + 120)",
            "REX_LOAD_U32(ctx.r31.u32 + 116)",
            "ctx.lr = 0x82E2B1C0;",
            "ctx.lr = 0x82E2B228;",
        ):
            self.assertGreater(owner.index(later), hook, later)
        before = owner[:hook]
        self.assertNotIn("REX_STORE_U32(ctx.r31.u32", before)
        stores = re.findall(r"REX_STORE_U\d+\(([^,]+),", before)
        self.assertTrue(
            all(re.fullmatch(r"ea|ctx\.r1\.u32 \+ -?\d+", s) for s in stores), stores
        )

    def test_join_refusal_hook_is_one_registered_seam(self):
        address, name, registers, target = JOIN_HOOK
        hooks = tomllib.loads(read(JOIN_CONFIG))["midasm_hook"]
        self.assertEqual(
            hooks,
            [
                {
                    "address": address,
                    "name": name,
                    "registers": registers,
                    "jump_address_on_true": target,
                }
            ],
        )
        self.assertIn(
            '"config/join_refusal_hooks.toml"', read(ROOT / "rerevved_manifest.toml")
        )
        taken = []
        for path in (ROOT / "config").glob("*.toml"):
            if path != JOIN_CONFIG:
                taken += [
                    row["address"]
                    for row in tomllib.loads(read(path)).get("midasm_hook", [])
                ]
        self.assertNotIn(address, taken)
        signature = (
            "bool RefuseJoinInProgress(PPCRegister& result, PPCRegister& stateKey)"
        )
        self.assertIn(signature, read(ROOT / "src/join_refusal_hooks.h"))
        source = read(JOIN_HOOKS)
        self.assertEqual(source.count(signature), 1)
        self.assertEqual(signature.count("PPCRegister&"), len(registers))
        self.assertIn("src/join_refusal_hooks.cpp", read(ROOT / "CMakeLists.txt"))
        self.assertIn("src/join_refusal_hooks.h", read(ROOT / "CMakeLists.txt"))
        self.assertIn(
            "src/join_refusal_hooks.cpp", read(ROOT / "tests/rerevved_tests.cmake")
        )
        for path in (
            JOIN_CONFIG,
            JOIN_HOOKS,
            ROOT / "src/join_refusal_hooks.h",
        ):
            read(path).encode("ascii")

    def test_join_refusal_decides_on_the_key_and_cannot_throw(self):
        body = function_body(read(JOIN_HOOKS), "bool RefuseJoinInProgress(")
        order = [
            "IsGameInProgress(stateKey.u32)",
            "result.u64 = admission::kRefusedResult;",
            "REXLOG_WARN(",
            "return true;",
        ]
        positions = [body.index(fragment) for fragment in order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("catch (...)", body)
        for needle in ("REX_KERNEL_MEMORY", "Word(", "Byte(", "ReadTransition"):
            self.assertNotIn(needle, body)
        self.assertEqual(body.count("REXLOG_"), 1)
        self.assertEqual(body.count("{}"), 1)
        self.assertIn('(refusal {})", ++refusals);', body)

    def test_generated_join_refusal_site(self):
        paths, text = generated_text()
        if not paths:
            self.skipTest("generated guest code is not present")
        address, name, _, target = JOIN_HOOK
        if name not in text:
            self.skipTest("join refusal hook requires owner codegen")
        call = (
            f"if ({name}(ctx.r3, ctx.r9)) {{\n"
            f"\t\tgoto loc_{target:08X};\n"
            "\t}\n"
            "\telse {\n"
            "\t}"
        )
        self.assertEqual(text.count(call), 1)
        position = text.index(call)
        start = text.rindex("DEFINE_REX_FUNC(sub_82D89678)", 0, position)
        end = text.index("\n}\n", position)
        policy = text[start:end]
        hook = policy.index(call)

        # Every instruction of the policy has one comment, so a comment's index gives its address.
        words = [line.strip() for line in re.findall(r"^\t// (.*)$", policy, re.M)]
        self.assertEqual(len(words), POLICY_WORDS)
        for word_address, expected in POLICY_WINDOW.items():
            self.assertEqual(
                words[(word_address - POLICY_START) // 4], expected, hex(word_address)
            )
        self.assertEqual(words[(address - POLICY_START) // 4], "cmpwi cr6,r9,2")

        site = re.compile(
            r"\t// cmpwi cr6,r9,2\n\t%s\n\tctx\.cr6\.compare<int32_t>\(ctx\.r9\.s32, 2, ctx\.xer\);"
            % re.escape(call)
        )
        self.assertEqual(len(site.findall(policy)), 1)
        self.assertIn(
            f"loc_{target:08X}:\n\t// addi r1,r1,112\n\tctx.r1.s64 = ctx.r1.s64 + 112;",
            policy,
        )
        self.assertEqual(policy.count(f"goto loc_{target:08X};"), 2)
        self.assertLess(policy.index("sub_82D8AA60(ctx, base);"), hook)
        self.assertLess(policy.index("REX_LOAD_U32(ctx.r10.u32 + 4)"), hook)
        self.assertGreater(policy.index("sub_82D8B400(ctx, base);"), hook)


if __name__ == "__main__":
    unittest.main()
