from __future__ import annotations

import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


PROBE = ROOT / "src/gpu/guest_d3d/completion_probe.cpp"
HEADER = ROOT / "src/gpu/guest_d3d/completion_probe.h"
HOOKS = ROOT / "src/gpu/guest_d3d/completion_probe_hooks.cpp"
CONFIG = "config/native_completion_probe_hooks.toml"


class CompletionProbeContractTests(unittest.TestCase):
    def test_hook_manifest_and_unique_addresses(self) -> None:
        manifest = tomllib.loads((ROOT / "rerevved_manifest.toml").read_text())
        includes = manifest["entrypoint"]["includes"]
        self.assertEqual(includes.count(CONFIG), 1)
        addresses: dict[int, str] = {}
        for include in includes:
            config = tomllib.loads((ROOT / include).read_text())
            for hook in config.get("midasm_hook", []):
                address = hook["address"]
                self.assertNotIn(address, addresses, f"duplicate {address:08X}")
                addresses[address] = include
        hooks = tomllib.loads((ROOT / CONFIG).read_text())["midasm_hook"]
        expected = {
            0x8268D2AC: ("CompletionConstructed", ["r31", "r30"]),
            0x8269BE1C: ("CompletionDeferredBegin", []),
            0x8269BE20: ("CompletionDeferredEnd", ["r30", "r26", "r11"]),
            0x8269BE4C: ("CompletionDirectBegin", []),
            0x8269BE50: ("CompletionDirectEnd", ["r30", "r26", "r11"]),
            0x826ABB34: ("CompletionRetireBegin", []),
            0x826ABB38: ("CompletionRetireEnd", ["r31", "r11"]),
            0x826A5BCC: ("CompletionWatchdogEnd", []),
            0x8269B450: ("CompletionInterrupt", ["r3", "r4"]),
            0x8269B4A4: ("CompletionInterruptCallback", ["r31", "r30", "r3"]),
            0x8268CB88: ("CompletionOptional347C", ["r9", "r4", "f1"]),
            0x826A3FA0: ("CompletionOptional40A0", ["r31", "r11", "r3"]),
            0x826A40B4: ("CompletionOptional409C", ["r31", "r9", "r3"]),
            0x8269B81C: ("CompletionAllocationBegin", ["r12", "r3", "r4", "r5"]),
            0x8269B8EC: ("CompletionAllocationEnd", ["r3"]),
            0x826A6AB8: ("CompletionManagerBegin", ["r3", "r4", "r5", "r6", "lr"]),
            0x826A6B18: ("CompletionManagerTarget", ["r31", "r11", "r3", "r5"]),
            0x826A6B1C: ("CompletionManagerEnd", ["r3"]),
        }
        actual = {
            hook["address"]: (hook["name"], hook.get("registers", [])) for hook in hooks
        }
        self.assertEqual(actual, expected)
        source = HOOKS.read_text()
        for hook in hooks:
            self.assertNotIn("returns", hook)
            self.assertNotIn("jump", hook)
            self.assertRegex(source, rf"void {hook['name']}\(")

    def test_wrapper_ownership_and_observation_order(self) -> None:
        route = (ROOT / "src/gpu/guest_d3d/route_hooks.cpp").read_text()
        self.assertLess(
            route.index("ObserveCompletionRoute"), route.index("RunRingStub")
        )
        header = HEADER.read_text()
        for address in ("8269C810", "8269C8E0", "8269BCA8", "8269BF40", "826AC020"):
            self.assertIn(f"0x{address}", header)
        stubs = (ROOT / "src/gpu/guest_d3d/ring_stub_hooks.cpp").read_text()
        submit = stubs.split("REX_HOOK_RAW(sub_8269BD98)", 1)[1]
        submit = submit.split("REX_HOOK_RAW", 1)[0]
        self.assertLess(
            submit.index("CompletionScope"), submit.index("ringStubsActive")
        )
        self.assertLess(
            submit.index("ObserveCompletionStubBegin"),
            submit.index("RaiseForDirectSubmit"),
        )
        self.assertGreater(
            submit.index("ObserveCompletionStubEnd"),
            submit.index("RaiseForDirectSubmit"),
        )
        self.assertEqual(submit.count("__imp__sub_8269BD98(ctx, base)"), 1)
        bracket = stubs.split("REX_HOOK_RAW(sub_826AC218)", 1)[1]
        bracket = bracket.split("REX_HOOK_RAW", 1)[0]
        self.assertLess(bracket.index("CompletionScope"), bracket.index("endTiling("))

    def test_observation_only_and_reporting(self) -> None:
        source = PROBE.read_text()
        header = HEADER.read_text()
        self.assertRegex(
            source, r"REXCVAR_DEFINE_BOOL\(native_completion_probe,\s*false,"
        )
        self.assertIn("Lifecycle::kInitOnly", source)
        self.assertIn("std::memory_order_relaxed", header)
        self.assertNotIn("RingStubsPublished", source + header)
        self.assertNotIn("TilingProbeRequested", source + header)
        self.assertIn("CopyGuestGuarded(bytes,", source)
        self.assertNotRegex(
            source + HOOKS.read_text(), r"REX_STORE|Write32\(|Write8\(|REX_HOOK_RAW"
        )
        self.assertNotRegex(source + HOOKS.read_text(), r"\bctx\.\w+\s*=")
        self.assertIn("pendingStore != pc", source)
        self.assertIn("slot_coverage=sampled", source)
        self.assertIn('std::string("unknown")', source)
        app = (ROOT / "src/app.cpp").read_text()
        self.assertIn("phases += gpu::guest_d3d::ReportCompletionProbe()", app)
        self.assertIn("ReportCompletionProbe(true)", app)
        shared = (ROOT / "src/rerevved_hooks.cpp").read_text()
        self.assertIn("ObserveCompletionReset(r31.u32)", shared)
        publication = shared.split("void ObserveNativeDevicePublication(", 1)[1].split(
            "void ObserveNativeTexturePublication(", 1
        )[0]
        self.assertIn("ObserveCompletionSample(0x826A645C", publication)
        self.assertLess(
            publication.index("ObserveCompletionSample"),
            publication.index("SelectedRendererBackend()"),
        )


if __name__ == "__main__":
    unittest.main()
