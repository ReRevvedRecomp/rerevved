from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import chunk_lines, scan_once

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config/capital_grace_hooks.toml"

# The native comment at each seam is checked independently of the hook source.
INSTRUCTIONS = {
    0x82CDDF34: "bl 0x82cd4cf0",
    0x82CE16AC: "lis r10,-31979",
    0x82CE16B0: "li r3,1",
    0x82CE16B4: "lwz r11,25412(r10)",
    0x82CE16B8: "addi r11,r11,-1",
    0x82CE16BC: "stw r11,25412(r10)",
    0x82CE16C0: "addi r1,r1,368",
    0x82CE16E8: "lwz r8,0(r9)",
    0x82CE16F4: "lwz r11,-4(r31)",
    0x82CE1730: "b 0x82ce1738",
    0x82CE1748: "clrlwi r10,r11,27",
    0x82CE1784: "lwz r11,-4128(r29)",
    0x82CE17EC: "addi r1,r1,144",
    0x82CE1828: "addi r1,r1,144",
    0x82D2C9BC: "bl 0x82ce1950",
    0x82D2C9C0: "bl 0x82cd58b8",
    0x82D2CE00: "bl 0x82ce1950",
    0x82D2CE04: "lis r29,-31981",
    0x82D2C170: "bl 0x82ce1950",
    0x82D2C174: "bl 0x82cd58b8",
    0x82D2BBA0: "ori r8,r9,1",
    0x82D2BBA4: "stwx r8,r30,r5",
    0x82D2BD84: "bl 0x82ce1950",
    0x82D2BD88: "lis r11,-31979",
    0x82D2BDE0: "lwz r11,-18200(r23)",
    0x82D2BDF8: "rlwinm r7,r8,0,0,30",
    0x82D2BDFC: "stwx r7,r11,r10",
    0x82D2BE00: "b 0x82d2a70c",
    0x82D2D2E0: "li r3,1",
    0x82D1B43C: "cmpw cr6,r19,r18",
    0x82D1D1B4: "addi r1,r1,320",
}


NUCLEAR_INSTRUCTIONS = {
    0x82CF7498: "addi r16,r6,-23032",
    0x82CF7938: "bl 0x82cfadf0",
    0x82CF76A4: "stbx r9,r31,r10",
    0x82CF77F0: "lwz r10,276(r1)",
    0x82CF7BF0: "stbx r7,r31,r11",
    0x82CF7D08: "li r5,0",
    0x82CF78CC: "addi r1,r1,256",
    0x82CF78D0: "b 0x827f7ca0",
    0x82CF7D34: "lis r10,-31985",
    0x82CF7D68: "addi r1,r1,256",
    0x82CF7B7C: "addi r1,r1,256",
    0x82CF75D0: "addi r1,r1,256",
    0x82CDF1C8: "cmpwi cr6,r20,-1",
    0x82CDF26C: "lwz r11,0(r25)",
    0x82CDF1C4: "bl 0x82d02390",
}
EXPECTED_HOOKS = (
    (0x82CDDF34, "GuardCapitalGraceAdmission", "r3 r30 r31 r15 r29", 0x82CE16AC),
    (0x82D1B43C, "GuardCapitalGraceCapture", "r3 r4", 0x82D1D1B4),
    (0x82CE16E8, "BeginCapitalGraceSubmit", "r1 r31 r30", None),
    (0x82CE16F4, "AppendCapitalGraceToken", "r1 r31 r9", None),
    (0x82CE1730, "CompleteCapitalGraceAppend", "r1", None),
    (0x82CE1748, "ObserveCapitalGraceDrainSerial", "r1 r31 r11", None),
    (0x82CE1784, "BindCapitalGraceDrain", "r1", None),
    (0x82CE17EC, "EndCapitalGraceSubmit", "r1", None),
    (0x82CE1828, "EndCapitalGraceSubmit", "r1", None),
    (0x82D2C9BC, "BeginCapitalGraceBlitz", "r1 r3 r4 r5 r6 r7", None),
    (0x82D2C9C0, "EndCapitalGraceBlitz", "r1", 0x82D2D2E0),
    (0x82D2CE00, "BeginCapitalGraceOrdinary", "r1 r3 r4 r5 r6 r7", None),
    (0x82D2CE04, "EndCapitalGraceOrdinary", "r1", 0x82D2D2E0),
    (0x82D2C170, "BeginCapitalGracePacked", "r1 r3 r4 r5 r6 r7", None),
    (0x82D2C174, "EndCapitalGracePacked", "r1", 0x82D2D2E0),
    (0x82D2BBA0, "PrepareCapitalGraceParent", "r1 r15 r23 r25 r31 r9", None),
    (0x82D2BD84, "BeginCapitalGraceLinked", "r1 r3 r4 r5 r6 r7", None),
    (0x82D2BD88, "EndCapitalGraceLinked", "r1 r15 r23 r25 r31", 0x82D2BDE0),
    (0x82D2BE00, "FinishCapitalGraceParent", "r1", 0x82D2D2E0),
)

EXPECTED_HOOKS += (
    (0x82CF7498, "BeginCapitalGraceNuclearZero", "r1 r22 r4", 0x82CF78CC),
    (0x82CF7938, "BeginCapitalGraceNuclearAlternate", "r1 r16 r4", 0x82CF7D34),
    (
        0x82CF76A4,
        "GuardCapitalGraceNuclearZeroPopulation",
        "r1 r31 r10 r27 r26",
        0x82CF78CC,
    ),
    (
        0x82CF7BF0,
        "GuardCapitalGraceNuclearAlternatePopulation",
        "r1 r31 r11 r28 r27",
        0x82CF7D34,
    ),
    (
        0x82CF77F0,
        "GuardCapitalGraceNuclearZeroVictim",
        "r1 r28 r30 r31 r27 r26",
        0x82CF78CC,
    ),
    (
        0x82CF7D08,
        "GuardCapitalGraceNuclearAlternateVictim",
        "r1 r29 r30 r31 r28 r27",
        0x82CF7D34,
    ),
    (0x82CF78CC, "EndCapitalGraceNuclearZero", "r1", None),
    (0x82CF75D0, "EndCapitalGraceNuclearEarly", "r1", None),
    (0x82CF7B7C, "EndCapitalGraceNuclearEarly", "r1", None),
    (0x82CF7D68, "EndCapitalGraceNuclearEarly", "r1", None),
    (0x82CDF1C8, "FinishCapitalGraceNuclearZero", "r1 r31 r15", 0x82CDF26C),
    (0x82CE16AC, "EndCapitalGraceNuclearDispatch", "r1", None),
)

OWNERS = {
    "82CDDF08",
    "82CE16C8",
    "82CE1950",
    "82D259E8",
    "82D1B400",
    "82CF7428",
    "82CF78D8",
}


def encode_instruction(address, instruction):
    operation, operands = instruction.split(maxsplit=1)
    if operation in {"b", "bl"}:
        return (
            0x48000000
            | ((int(operands, 16) - address) & 0x03FFFFFC)
            | (operation == "bl")
        )
    args = [int(value) for value in re.findall(r"-?\d+", operands)]
    if operation in {"lis", "li"}:
        opcode = 15 if operation == "lis" else 14
        return (opcode << 26) | (args[0] << 21) | (args[1] & 0xFFFF)
    if operation == "addi":
        return (14 << 26) | (args[0] << 21) | (args[1] << 16) | (args[2] & 0xFFFF)
    if operation in {"lwz", "stw"}:
        opcode = 32 if operation == "lwz" else 36
        return (opcode << 26) | (args[0] << 21) | (args[2] << 16) | (args[1] & 0xFFFF)
    if operation == "ori":
        return (24 << 26) | (args[1] << 21) | (args[0] << 16) | args[2]
    if operation == "clrlwi":
        args = [args[0], args[1], 0, args[2], 31]
        operation = "rlwinm"
    if operation == "rlwinm":
        return (
            (21 << 26)
            | (args[1] << 21)
            | (args[0] << 16)
            | (args[2] << 11)
            | (args[3] << 6)
            | (args[4] << 1)
        )
    if operation == "cmpw":
        return (31 << 26) | (args[0] << 23) | (args[1] << 16) | (args[2] << 11)
    if operation == "cmpwi":
        return (11 << 26) | (args[0] << 23) | (args[1] << 16) | (args[2] & 0xFFFF)
    if operation == "stbx":
        return (31 << 26) | (args[0] << 21) | (args[1] << 16) | (args[2] << 11) | 430
    if operation == "stwx":
        return (31 << 26) | (args[0] << 21) | (args[1] << 16) | (args[2] << 11) | 302
    raise ValueError(f"unsupported profile instruction {instruction}")


def scan_generated(paths):
    instructions = {}
    calls = {}
    owner = None
    address = 0
    branch = None
    for lines in chunk_lines(paths):
        for line in lines:
            # Conditional hooks emit their goto on the line after "if (...) {".
            if branch is not None:
                calls[branch].append(calls[branch].pop() + " " + line.strip())
                branch = None
            function = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)", line)
            if function:
                owner = function[1] if function[1] in OWNERS else None
                address = int(function[1], 16)
            if owner is None:
                continue
            label = re.match(r"loc_([0-9A-F]+):", line)
            if label:
                address = int(label[1], 16)
            instruction = re.match(r"\s*// (.+)", line)
            if instruction:
                if address in INSTRUCTIONS or address in NUCLEAR_INSTRUCTIONS:
                    instructions[address] = instruction[1].strip()
                address += 4
            if "CapitalGrace" in line and "(ctx." in line:
                calls.setdefault(owner, []).append(line.strip())
                if line.rstrip().endswith("{"):
                    branch = owner
    return instructions, calls


class CapitalGraceContractTests(unittest.TestCase):
    def test_capability_words_encode_the_native_instructions(self):
        source = (ROOT / "src/capital_grace_hooks.cpp").read_text(encoding="utf-8")
        profile = source.split("constexpr NativeWord kProfile[] = {", 1)[1].split(
            "};", 1
        )[0]
        words = {
            int(address, 16): int(value, 16)
            for address, value in re.findall(
                r"\{ (0x[0-9A-F]+), (0x[0-9A-F]+) \}", profile
            )
        }
        self.assertEqual(set(words), set(INSTRUCTIONS))
        for address, instruction in INSTRUCTIONS.items():
            self.assertEqual(words[address], encode_instruction(address, instruction))

    def test_nuclear_capability_words_encode_native_seams(self):
        source = (ROOT / "src/capital_grace_nuclear.cpp").read_text(encoding="utf-8")
        profile = source.split("profile[] = {", 1)[1].split("};", 1)[0]
        words = {
            int(address, 16): int(value, 16)
            for address, value in re.findall(
                r"\{ (0x[0-9A-F]+), (0x[0-9A-F]+) \}", profile
            )
        }
        self.assertEqual(set(words), set(NUCLEAR_INSTRUCTIONS))
        for address, instruction in NUCLEAR_INSTRUCTIONS.items():
            self.assertEqual(words[address], encode_instruction(address, instruction))

    def test_hook_registers_and_native_destinations(self):
        hooks = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["midasm_hook"]
        expected = []
        for address, name, registers, target in EXPECTED_HOOKS:
            row = {"address": address, "name": name, "registers": registers.split()}
            if target is not None:
                row["jump_address_on_true"] = target
            expected.append(row)
        self.assertEqual(hooks, expected)
        manifest = tomllib.loads(
            (ROOT / "rerevved_manifest.toml").read_text(encoding="utf-8")
        )
        self.assertIn("config/capital_grace_hooks.toml", str(manifest))

    def test_generated_native_seams_and_consumed_tail(self):
        paths, (instructions, _) = scan_once("capital_grace", scan_generated)
        if not paths:
            self.skipTest("generated guest code is not present")
        self.assertEqual(instructions, INSTRUCTIONS | NUCLEAR_INSTRUCTIONS)

    def test_generated_hook_installation(self):
        paths, (_, calls) = scan_once("capital_grace", scan_generated)
        if not paths:
            self.skipTest("generated guest code is not present")
        if not calls:
            self.skipTest("capital-grace hooks require owner codegen")
        actual = [call for owner_calls in calls.values() for call in owner_calls]
        for _, name, registers, target in EXPECTED_HOOKS:
            args = ", ".join(f"ctx.{register}" for register in registers.split())
            call = f"{name}({args})"
            if target is None:
                call += ";"
            else:
                call = f"if ({call}) {{ goto loc_{target:08X};"
            self.assertEqual(
                actual.count(call), sum(row[1] == name for row in EXPECTED_HOOKS)
            )
        self.assertEqual(len(actual), len(EXPECTED_HOOKS))


if __name__ == "__main__":
    unittest.main()
