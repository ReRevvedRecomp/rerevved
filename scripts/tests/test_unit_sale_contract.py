from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from generated_scan import scan_once

ROOT = Path(__file__).resolve().parents[2]
SITES = (
    (0x82D2B398, "BeginUnitSalePreview", "r1 r3 r7 r10 r11 r15 r17 r19 r21 r26 r31"),
    (0x82D2B45C, "ApplyUnitSalePreview", "r1 r3 r5 r6 r9 r15 r17 r19 r21 r26 r31"),
    (0x82D2B4BC, "BeginUnitSaleCredit", "r1 r15 r17 r19 r21 r26 r31"),
    (0x82D2B5DC, "ApplyUnitSaleCredit", "r1 r5 r8 r15 r17 r19 r21 r29 r30 r31"),
)


def sale_windows(paths: list[Path]) -> dict[int, str]:
    """Index only the two selected label-bounded instruction windows."""
    rows: dict[int, str] = {}
    for path in paths:
        address = None
        current = None
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if line.strip() == "loc_82D2ABEC:":
                    address = 0x82D2ABEC
                    current = None
                elif line.strip() == "loc_82D2B398:":
                    address = 0x82D2B398
                    current = None
                elif line.strip() == "loc_82D2B690:":
                    address = current = None
                if address is None:
                    continue
                if line.startswith("\t// "):
                    if address == 0x82D2AC10:
                        address = current = None
                        continue
                    current = address
                    rows[current] = line
                    address += 4
                elif current is not None:
                    rows[current] += line
    return rows


class UnitSaleContract(unittest.TestCase):
    def generated(self) -> dict[int, str]:
        paths, rows = scan_once("unit_sale_windows", sale_windows)
        if not paths:
            self.skipTest("generated sources are not available")
        self.assertIn(0x82D2B398, rows, "generated sale input window is missing")
        return rows

    def test_configured_register_seams_and_integration(self) -> None:
        config = tomllib.loads((ROOT / "config/unit_sale_hooks.toml").read_text())
        self.assertEqual(
            config["midasm_hook"],
            [
                {"address": address, "name": name, "registers": registers.split()}
                for address, name, registers in SITES
            ],
        )
        manifest = tomllib.loads((ROOT / "rerevved_manifest.toml").read_text())
        self.assertEqual(
            manifest["entrypoint"]["includes"].count("config/unit_sale_hooks.toml"), 1
        )
        source = (ROOT / "src/unit_sale_hooks.cpp").read_text()
        for _, name, registers in SITES:
            declaration = re.search(rf"void {name}\((.*?)\)\s*\{{", source, re.S)
            self.assertIsNotNone(declaration)
            self.assertEqual(
                declaration.group(1).count("PPCRegister&"), len(registers.split())
            )
        self.assertNotIn("REX_HOOK", source)
        self.assertNotIn("REX_STORE", source)
        self.assertNotRegex(source, r"\bsub_[0-9A-Fa-f]+\(")
        self.assertIn("CopyGuestGuarded", source)
        self.assertIn("QueryRangeAccess", source)
        self.assertIn("#if defined(_WIN32)", source)
        self.assertIn("REXCVAR_DEFINE_BOOL(unit_sale_trace, false,", source)
        self.assertIn("count < 256", source)

    def test_native_collection_arithmetic_and_unchanged_calls(self) -> None:
        rows = self.generated()
        instructions = {
            0x82D2B398: "addi r9,r21,68",
            0x82D2B3A8: "bne cr6,0x82d2b3b0",
            0x82D2B3B0: "rlwinm r11,r10,0,27,27",
            0x82D2B3D0: "lbz r10,-12(r11)",
            0x82D2B3E4: "cmplw cr6,r6,r19",
            0x82D2B3EC: "lhz r10,26(r11)",
            0x82D2B3F4: "cmpw cr6,r6,r31",
            0x82D2B404: "cmpwi cr6,r10,7",
            0x82D2B450: "mullw r11,r5,r9",
            0x82D2B454: "rlwinm r10,r11,2,0,29",
            0x82D2B458: "add r6,r11,r10",
            0x82D2B45C: "bl 0x82cef160",
            0x82D2B48C: "bl 0x82e6a430",
            0x82D2B4A8: "bl 0x82ce8338",
            0x82D2B4BC: "lwz r11,-18200(r26)",
            0x82D2B51C: "lbz r10,-12(r11)",
            0x82D2B580: "bl 0x82cd5a70",
            0x82D2B5CC: "mullw r11,r8,r5",
            0x82D2B5D0: "mullw r11,r11,r30",
            0x82D2B5D4: "rlwinm r10,r11,2,0,29",
            0x82D2B5D8: "add r5,r11,r10",
            0x82D2B5DC: "bl 0x82d038e8",
            0x82D2B5EC: "li r3,41",
            0x82D2B5F4: "bl 0x82ce1950",
            0x82D2B678: "lwz r5,144(r1)",
            0x82D2B67C: "li r3,16",
            0x82D2B680: "bl 0x82ce1950",
        }
        for address, instruction in instructions.items():
            with self.subTest(address=f"{address:08X}"):
                self.assertTrue(rows[address].startswith(f"\t// {instruction}\n"))
        self.assertIn("ctx.r6.u64 = ctx.r11.u64 + ctx.r10.u64;", rows[0x82D2B458])
        self.assertIn("ctx.r5.u64 = ctx.r11.u64 + ctx.r10.u64;", rows[0x82D2B5D8])
        for address, function in (
            (0x82D2B45C, "82CEF160"),
            (0x82D2B580, "82CD5A70"),
            (0x82D2B5DC, "82D038E8"),
            (0x82D2B5F4, "82CE1950"),
            (0x82D2B680, "82CE1950"),
        ):
            self.assertIn(f"sub_{function}(ctx, base);", rows[address])

    def test_native_decline_and_direct_ingress_are_preserved(self) -> None:
        rows = self.generated()
        expected = {
            0x82D2ABEC: "clrlwi r11,r11,31",
            0x82D2ABF0: "cmpwi cr6,r11,0",
            0x82D2ABF4: "beq cr6,0x82d2b4bc",
            0x82D2ABF8: "lwz r11,-18200(r26)",
            0x82D2ABFC: "lwz r10,212(r1)",
            0x82D2AC00: "rlwinm r9,r11,7,0,24",
            0x82D2AC04: "lwzx r8,r9,r10",
            0x82D2AC08: "cmpwi cr6,r8,0",
            0x82D2AC0C: "bne cr6,0x82d2b4bc",
            0x82D2B4B0: "cmpwi cr6,r3,0",
            0x82D2B4B4: "lwz r31,-18196(r11)",
            0x82D2B4B8: "bne cr6,0x82d2a70c",
        }
        for address, instruction in expected.items():
            self.assertTrue(rows[address].startswith(f"\t// {instruction}\n"))
        self.assertIn("if (ctx.cr6.eq) goto loc_82D2B4BC;", rows[0x82D2ABF4])
        self.assertIn("if (!ctx.cr6.eq) goto loc_82D2B4BC;", rows[0x82D2AC0C])
        self.assertIn("if (!ctx.cr6.eq) goto loc_82D2A70C;", rows[0x82D2B4B8])
        self.assertNotIn("UnitSale", rows[0x82D2B4B8])

    def test_generated_hooks_precede_original_instruction_effects(self) -> None:
        rows = self.generated()
        for address, name, registers in SITES:
            call = f"{name}({', '.join('ctx.' + register for register in registers.split())});"
            with self.subTest(name=name):
                self.assertEqual(
                    rows[address].count(call),
                    1,
                    "regenerate guest sources for the configured sale seams",
                )
                self.assertLess(
                    rows[address].index(call),
                    rows[address].index("ctx.", rows[address].index(call) + len(call)),
                )


if __name__ == "__main__":
    unittest.main()
