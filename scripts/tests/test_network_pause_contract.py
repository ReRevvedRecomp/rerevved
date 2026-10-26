from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "src" / "network_pause_hooks.cpp"


class NetworkPauseContractTest(unittest.TestCase):
    def test_root_dispatch_hook_is_unique_and_leaf_is_unhooked(self) -> None:
        sources = [
            path.read_text(encoding="utf-8") for path in (ROOT / "src").rglob("*.cpp")
        ]
        self.assertEqual(sum(s.count("REX_HOOK_RAW(sub_82D7CE90)") for s in sources), 1)
        self.assertEqual(sum(s.count("sub_82DE5670") for s in sources), 0)

    def test_hook_gates_on_request_byte_after_native_call(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        body = text.split("REX_HOOK_RAW(sub_82D7CE90)", 1)[1]
        # The native call may clobber r4, so the request byte is read first.
        request = body.index("static_cast<uint8_t>(ctx.r4.u64)")
        original = body.index("__imp__sub_82D7CE90(ctx, base);")
        gate = body.index(
            "ShouldApplyLocalResume(networkByte, requestByte, root, rootFlags)"
        )
        consumer = body.index("callConsumer(ctx, base, root);")
        self.assertLess(request, original)
        # Every early return must follow the native dispatch.
        self.assertLess(original, body.index("return;"))
        self.assertLess(original, gate)
        self.assertLess(gate, consumer)
        self.assertEqual(body.count("REXLOG_INFO"), 1)
        self.assertIn("Multiplayer pause: resume applied locally", body)

    def test_no_host_gate(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        header = (ROOT / "src" / "network_pause.h").read_text(encoding="utf-8")
        for source in (text, header):
            self.assertNotIn("session_data", source)
            self.assertNotIn("isHost", source)
            self.assertNotIn("ShouldApplyHostResume", source)
        self.assertRegex(header, r"requestByte == 0")

    def test_consumer_runs_on_context_copy_with_resume_arguments(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        helper = text.split("void callConsumer", 1)[1].split("REX_HOOK_RAW", 1)[0]
        self.assertIn("PPCContext call = caller;", helper)
        self.assertRegex(helper, r"call\.r3\.u64\s*= root;")
        self.assertRegex(helper, r"call\.r4\.u64\s*= 0;")
        self.assertRegex(helper, r"call\.r5\.u64\s*= pause::kResumeLocalArgument;")
        self.assertIn("__imp__sub_82E2A998(call, base);", helper)
        self.assertIn("caller.fpscr = call.fpscr;", helper)
        self.assertEqual(len(re.findall(r"__imp__sub_82E2A998\(", text)), 1)

    def test_header_constants_and_sources_registered(self) -> None:
        header = (ROOT / "src" / "network_pause.h").read_text(encoding="utf-8")
        self.assertRegex(header, r"kFrontendRootGlobal\s*= 0x82FFD624")
        self.assertRegex(header, r"kPausedFlag\s*= 0x800")
        self.assertRegex(header, r"kResumeLocalArgument\s*= 0xFFFFFFFF")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(cmake.count("src/network_pause_hooks.cpp"), 1)
        self.assertEqual(cmake.count("src/network_pause.h"), 1)
        tests = (ROOT / "tests" / "rerevved_tests.cmake").read_text(encoding="utf-8")
        self.assertEqual(tests.count("tests/network_pause_test.cpp"), 1)


if __name__ == "__main__":
    unittest.main()
