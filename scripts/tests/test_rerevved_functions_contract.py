"""Static contract for the title function-root configuration."""

from __future__ import annotations

import tomllib
import unittest
from pathlib import Path

from generated_scan import generated_text

ROOT = Path(__file__).resolve().parents[2]
FUNCTION_CONFIG = ROOT / "config" / "rerevved_functions.toml"
GENERATED = ROOT / "generated" / "default"


class RerevvedFunctionsContractTests(unittest.TestCase):
    def test_switch_table_uses_the_guest_index_register(self) -> None:
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(
            config["switch_tables"],
            [
                {
                    "address": 0x82DFF930,
                    "register": 11,
                    "labels": [
                        0x82DFF954,
                        0x82DFF960,
                        0x82DFF96C,
                        0x82DFF978,
                        0x82DFF984,
                        0x82DFF990,
                        0x82DFF99C,
                        0x82DFF9A8,
                    ],
                }
            ],
        )

    def test_generated_dispatch_matches_guest_dataflow_when_available(self) -> None:
        paths, generated = generated_text()
        if not paths:
            self.skipTest("generated sources are not available")

        start = generated.index("DEFINE_REX_FUNC(sub_82DFF5E8)")
        end = generated.index("\nDEFINE_REX_FUNC(", start + 1)
        body = generated[start:end]

        self.assertEqual(body.count("switch (ctx.r11.u32)"), 1)
        self.assertNotIn("switch (ctx.r7.u32)", body)
        self.assertEqual(body.count("__builtin_trap(); // Switch case out of range"), 1)

    def test_runtime_reached_adjusted_pointer_thunk_is_rooted(self) -> None:
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["functions"]["8289E9E0"], {"size": 0x8})

        generated_init = GENERATED / "rerevved_init.cpp"
        if not generated_init.exists():
            self.skipTest("generated initialization source is not available")

        source = generated_init.read_text(encoding="utf-8")
        self.assertEqual(source.count("{ 0x8289E9E0, sub_8289E9E0 },"), 1)

    def test_runtime_reached_float_copy_leaf_is_rooted(self) -> None:
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["functions"]["828F3F10"], {"size": 0x14})

        generated_init = GENERATED / "rerevved_init.cpp"
        if not generated_init.exists():
            self.skipTest("generated initialization source is not available")

        source = generated_init.read_text(encoding="utf-8")
        self.assertEqual(source.count("{ 0x828F3F10, sub_828F3F10 },"), 1)

    def test_runtime_reached_bitfield_leaf_is_rooted(self) -> None:
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["functions"]["828F4950"], {"size": 0x20})

        generated_init = GENERATED / "rerevved_init.cpp"
        if not generated_init.exists():
            self.skipTest("generated initialization source is not available")

        source = generated_init.read_text(encoding="utf-8")
        self.assertEqual(source.count("{ 0x828F4950, sub_828F4950 },"), 1)

    def test_system_link_join_thunks_are_rooted(self) -> None:
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        self.assertEqual(config["functions"]["82C9D678"], {"size": 0x8})
        self.assertEqual(config["functions"]["82C9D680"], {"size": 0x14})

        generated_init = GENERATED / "rerevved_init.cpp"
        if not generated_init.exists():
            self.skipTest("generated initialization source is not available")

        source = generated_init.read_text(encoding="utf-8")
        self.assertEqual(source.count("{ 0x82C9D678, sub_82C9D678 },"), 1)
        self.assertEqual(source.count("{ 0x82C9D680, sub_82C9D680 },"), 1)

    def test_live_mode_vtable_entries_are_rooted(self) -> None:
        expected = {
            "82E4F738": 0x2C,
            "82CA2768": 0x14,
            "82D720E8": 0x10,
            "82D7BB80": 0x4,
        }
        with FUNCTION_CONFIG.open("rb") as stream:
            config = tomllib.load(stream)

        for address, size in expected.items():
            self.assertEqual(config["functions"][address], {"size": size})

        generated_init = GENERATED / "rerevved_init.cpp"
        if not generated_init.exists():
            self.skipTest("generated initialization source is not available")

        source = generated_init.read_text(encoding="utf-8")
        for address in expected:
            self.assertEqual(source.count(f"{{ 0x{address}, sub_{address} }},"), 1)


if __name__ == "__main__":
    unittest.main()
