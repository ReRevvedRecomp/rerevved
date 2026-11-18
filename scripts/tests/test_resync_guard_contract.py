from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "src" / "resync_guard_hooks.cpp"


class ResyncGuardContractTest(unittest.TestCase):
    def test_resync_start_hook_is_unique(self) -> None:
        sources = [
            path.read_text(encoding="utf-8") for path in (ROOT / "src").rglob("*.cpp")
        ]
        self.assertEqual(sum(s.count("REX_HOOK_RAW(sub_82D8FCC8)") for s in sources), 1)
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(cmake.count("src/resync_guard_hooks.cpp"), 1)

    def test_skip_needs_a_failed_session_lookup(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        self.assertRegex(text, r"kNetworkManagerGlobal = 0x8314EFE0;")
        body = text.split("REX_HOOK_RAW(sub_82D8FCC8)", 1)[1]
        player = body.index("ctx.r4.u32")
        gate = body.index("if (manager != 0 && !inSession(ctx, base, manager, player))")
        skip = body.index("return;")
        original = body.index("__imp__sub_82D8FCC8(ctx, base);")
        self.assertLess(player, gate)
        self.assertLess(gate, skip)
        self.assertLess(skip, original)
        self.assertEqual(body.count("__imp__sub_82D8FCC8("), 1)

    def test_lookup_runs_on_a_context_copy(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        helper = text.split("bool inSession", 1)[1].split("} // namespace", 1)[0]
        self.assertIn("PPCContext call = caller;", helper)
        self.assertRegex(helper, r"call\.r3\.u64\s*= manager;")
        self.assertRegex(helper, r"call\.r4\.u64\s*= player;")
        self.assertIn("__imp__sub_82D8AB00(call, base);", helper)
        self.assertIn("caller.fpscr = call.fpscr;", helper)
        self.assertIn("static_cast<uint8_t>(call.r3.u64) != 0", helper)


if __name__ == "__main__":
    unittest.main()
