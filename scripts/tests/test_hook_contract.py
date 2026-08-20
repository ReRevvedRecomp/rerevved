from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOOK_CONFIG = ROOT / "rerevved_hooks.toml"
HOOK_SOURCE = ROOT / "src" / "compat_hooks.cpp"
GENERATED = ROOT / "generated" / "default"

EXPECTED_HOOKS = [
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
        "address": 0x82253D2C,
        "name": "ReRevvedCompatExpandGfxVectorGlyphCache",
        "registers": ["r31"],
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
        source = HOOK_SOURCE.read_text(encoding="utf-8")
        source_names = set(
            re.findall(r"^void (ReRevved\w+)\s*\(", source, re.MULTILINE)
        )
        hook_names = {hook["name"] for hook in config["midasm_hook"]}

        self.assertEqual(
            source_names - {"ReRevvedCompatNullOptionalDispatch"},
            hook_names,
        )

    def test_generated_ring_hook_placement_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "// bl 0x82e923e4\n"
            "\tReRevvedCompatRingInitializeBegin(ctx.r3, ctx.r4);\n"
            "\tctx.lr = 0x8269CAE4;\n"
            "\t__imp__VdInitializeRingBuffer(ctx, base);\n"
            "\t// rlwinm r11,r25,23,9,31\n"
            "\tReRevvedCompatRingInitializeEnd();"
        )
        self.assertEqual(generated.count(expected), 1)

    def test_generated_vector_glyph_cache_hook_when_available(self) -> None:
        paths = sorted(GENERATED.glob("rerevved_recomp.*.cpp"))
        if not paths:
            self.skipTest("generated sources are not available")

        generated = "".join(path.read_text(encoding="utf-8") for path in paths)
        expected = (
            "\tctx.lr = 0x82253D2C;\n"
            "\tsub_82253DC8(ctx, base);\n"
            "\t// mr r3,r31\n"
            "\tReRevvedCompatExpandGfxVectorGlyphCache(ctx.r31);"
        )
        self.assertEqual(generated.count(expected), 1)


if __name__ == "__main__":
    unittest.main()
