from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


SOURCE = ROOT / "src/gpu/guest_d3d/vs_readback_probe.cpp"
HEADER = ROOT / "src/gpu/guest_d3d/vs_readback_probe.h"
HOOKS = ROOT / "src/gpu/guest_d3d/vs_readback_probe_hooks.cpp"
ENTRIES = ("82662D28", "82694480")


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def wrapper(address: str) -> str:
    return body(HOOKS.read_text(), f"REX_HOOK_RAW(sub_{address})", "REX_HOOK_RAW")


class VsReadbackProbeContractTests(unittest.TestCase):
    def test_hook_ownership_and_disabled_path(self) -> None:
        sources = [
            path
            for pattern in ("*.cpp", "*.h", "*.inc")
            for path in (ROOT / "src").rglob(pattern)
        ]
        for address in ENTRIES:
            owners = [
                path
                for path in sources
                if f"REX_HOOK_RAW(sub_{address})" in path.read_text()
            ]
            self.assertEqual(owners, [HOOKS], f"unique raw owner of {address}")
            self.assertEqual(
                HOOKS.read_text().count(f"REX_EXTERN(__imp__sub_{address})"), 1
            )
            text = wrapper(address)
            # One Original per path; the cached flag decides before any scope.
            self.assertEqual(text.count(f"__imp__sub_{address}(ctx, base)"), 2)
            flag = text.index("VsReadbackProbeRequested()")
            first = text.index(f"__imp__sub_{address}(ctx, base)")
            scope = text.index("VsReadbackScope")
            self.assertLess(flag, first)
            self.assertLess(first, text.index("return;"))
            self.assertLess(text.index("return;"), scope)
            self.assertLess(scope, text.rindex(f"__imp__sub_{address}(ctx, base)"))
            self.assertLess(
                text.rindex(f"__imp__sub_{address}(ctx, base)"), text.index(".End(ctx)")
            )
            self.assertEqual(text.count(".End(ctx)"), 1)
            self.assertEqual(
                text.split("{", 1)[1].lstrip().split("(", 1)[0].strip(), "if"
            )
        self.assertIn("VsReadbackFunction::Refresh", wrapper(ENTRIES[0]))
        self.assertIn("VsReadbackFunction::Readback", wrapper(ENTRIES[1]))

    def test_no_other_registration_of_the_entries(self) -> None:
        for path in (ROOT / "config").glob("*.toml"):
            text = path.read_text()
            for address in ENTRIES:
                self.assertNotIn(address.lower(), text.lower(), path.name)
        self.assertFalse(list((ROOT / "config").glob("*vs_readback*")))
        manifest = tomllib.loads((ROOT / "rerevved_manifest.toml").read_text())
        for include in manifest["entrypoint"]["includes"]:
            self.assertNotIn("readback", include)
        for table in (
            "src/gpu/guest_d3d/route_entries.inc",
            "src/gpu/guest_d3d/guest_api_entries.inc",
        ):
            text = (ROOT / table).read_text()
            for address in ENTRIES:
                self.assertNotIn(address, text)

    def test_default_off_init_only_and_observation_only(self) -> None:
        source = SOURCE.read_text()
        header = HEADER.read_text()
        hooks = HOOKS.read_text()
        self.assertRegex(
            source, r"REXCVAR_DEFINE_BOOL\(native_vs_readback_probe,\s*false,"
        )
        self.assertIn("Lifecycle::kInitOnly", source)
        self.assertIn("vsReadbackProbeState{ -1 }", source)
        self.assertIn("std::memory_order_relaxed", header)
        for dependency in (
            "native_ring_stubs",
            "native_vs_code_watch",
            "native_consume_dirty_vertex_fetch",
            "native_frame_timing",
            "RingStubsPublished",
            "ringStubsActive",
        ):
            self.assertNotIn(dependency, source + header + hooks)
        everything = source + header + hooks
        self.assertNotRegex(
            everything,
            r"REX_STORE|Write32\(|Write8\(|const_cast|TranslateVirtual<[^c]|Lookup.*Write",
        )
        # No assignment to a context register anywhere in the observer.
        self.assertNotRegex(everything, r"\b(?:ctx|context)\.\w+(?:\.\w+)?\s*=[^=]")
        self.assertEqual(source.count("TranslateVirtual<const std::uint8_t*>"), 1)
        self.assertIn("CopyGuestGuarded(bytes,", source)
        self.assertNotIn("REX_HOOK", source + header)
        self.assertNotRegex(source + header, r"ObserveCompletion|RaiseForDirectSubmit")

    def test_read_whitelist_and_registers(self) -> None:
        source = SOURCE.read_text()
        begin = body(
            source,
            "VsReadbackToken BeginVsReadbackProbe(",
            "void EndVsReadbackProbe(",
        )
        end = body(source, "void EndVsReadbackProbe(", "void AbandonVsReadbackProbe(")
        words = re.findall(r"sampler\.Word\(([^;]+?)\)\)", begin + end)
        self.assertEqual(
            sorted(words),
            sorted(
                [
                    "entry.q, 0x04",
                    "entry.q, 0x08",
                    "entry.q, 0x14",
                    "entry.object, 0x0C",
                    "entry.vs, 0x36C",
                    "entry.vs, 0x370",
                    "entry.vs, 0x20",
                    "entry.q, 0x04",
                    "entry.q, 0x08",
                    "entry.q, 0x14",
                    "entry.object, 0x0C",
                    "entry.sizePointer, 0",
                ]
            ),
        )
        # A copy reads C only when the entry destination is nonzero, and the
        # size word only on a query exit from the saved size pointer.
        self.assertRegex(
            begin, r"if \(entry\.destination != 0\)\s*sampler\.Take\([^;]*0x20"
        )
        self.assertRegex(
            end,
            r"else if \(token\.kind == VsReadbackKind::Query\)\s*sampler\.Take\([^;]*sizePointer",
        )
        self.assertIn("VsReadbackKindOf(entry)", source)
        self.assertIn("entry.destination == 0", source)
        for name in ("r3", "r4", "r5"):
            self.assertIn(f"context.{name}.u64", begin)
        self.assertIn("context.lr", begin)
        self.assertIn("context.r1.u64", begin)
        self.assertIn("exit.result = context.r3.u64;", end)
        self.assertNotIn("context.r4", end)
        self.assertNotIn("context.r5", end)

    def test_caller_buckets_and_pairing_rules(self) -> None:
        header = HEADER.read_text()
        for name, value in (
            ("kVsReadbackRefreshPrepLr", "0x00000000825E1D30ULL"),
            ("kVsReadbackRefreshDescriptorLr", "0x000000008265E270ULL"),
            ("kVsReadbackQueryLr", "0x0000000082662D64ULL"),
            ("kVsReadbackCopyLr", "0x0000000082662DFCULL"),
        ):
            self.assertRegex(header, rf"{name}\s*=\s*{value}")
        self.assertIn("kVsReadbackDetails      = 128", header)
        self.assertIn("kVsReadbackActive       = 64", header)
        self.assertIn("std::chrono::milliseconds(10)", header)
        source = SOURCE.read_text()
        classify = body(
            source,
            "void VsReadbackProbe::Classify(",
            "VsReadbackToken VsReadbackProbe::Begin(",
        )
        self.assertIn("slot.thread != entry.thread", classify)
        self.assertIn("slot.sp > entry.sp", classify)
        self.assertIn("entry.lr != site", classify)
        self.assertIn("nearest->vs != entry.vs", classify)
        self.assertIn("untracked_ != 0", classify)
        full = body(
            source,
            "VsReadbackToken VsReadbackProbe::Begin(",
            "void VsReadbackProbe::Finish(",
        )
        self.assertIn("++counts_.correlationDrop;", full)

    def test_scope_ends_only_on_return(self) -> None:
        source = SOURCE.read_text()
        destructor = body(
            source, "VsReadbackScope::~VsReadbackScope()", "void VsReadbackScope::End("
        )
        self.assertIn("AbandonVsReadbackProbe(*probe_, token_);", destructor)
        self.assertNotIn("EndVsReadbackProbe", destructor)
        self.assertNotIn("catch", wrapper(ENTRIES[0]) + wrapper(ENTRIES[1]))
        finish = body(
            source, "void VsReadbackProbe::Finish(", "void VsReadbackProbe::End("
        )
        self.assertIn("terminal == VsReadbackTerminal::Returned", finish)
        self.assertIn("++counts_.abandon[k];", finish)

    def test_reporting_wiring(self) -> None:
        app = (ROOT / "src/app.cpp").read_text()
        source = SOURCE.read_text()
        self.assertIn('#include "gpu/guest_d3d/vs_readback_probe.h"', app)
        self.assertIn(
            "if (gpu::guest_d3d::VsReadbackProbeRequested())\n"
            "        phases += gpu::guest_d3d::ReportVsReadbackProbe();",
            app,
        )
        self.assertEqual(app.count("ReportVsReadbackProbe(true)"), 2)
        shutdown = body(
            app, "void App::OnShutdown()", "bool App::OnWindowCloseRequested()"
        )
        self.assertIn("ReportVsReadbackProbe(true)", shutdown)
        self.assertLess(
            shutdown.index("ReportVsReadbackProbe(true)"),
            shutdown.index("StopProducers("),
        )
        hard = body(app, "void App::OnBeforeHardExit()", "void App::OnKeyDown(")
        self.assertIn("ReportVsReadbackProbe(true)", hard)
        self.assertLess(
            hard.index("ReportVsReadbackProbe(true)"), hard.index("#if defined(_WIN32)")
        )
        # The final drains run whether or not timing is on.
        self.assertNotRegex(shutdown + hard, r"native_frame_timing|FrameTiming")
        reporter = source.split("std::string ReportVsReadbackProbe(", 1)[1]
        self.assertIn("lock.try_lock_for(kVsReadbackShutdownWait)", reporter)
        self.assertIn(
            "lock.try_lock_for(kVsReadbackShutdownWait)",
            body(
                source,
                "VsReadbackReport VsReadbackProbe::Take(",
                "bool VsReadbackProbe::Retain(",
            ),
        )
        self.assertIn(
            "lock.try_lock_for(kVsReadbackShutdownWait)",
            source.split("bool VsReadbackProbe::Retain(", 1)[1].split(
                "void VsReadbackProbe::NoteReportingFailure", 1
            )[0],
        )
        self.assertIn("probe.Retain(report.counts, shutdown)", reporter)
        self.assertIn("rb_read_final_incomplete=1 rb_read_final_skipped=1", source)
        self.assertIn('"Native VS readback probe: shutdown{}"', source)
        self.assertIn("generation=unknown outer_activation=unknown", source)
        self.assertNotRegex(reporter, r"native_frame_timing|FrameTiming")
        fields = body(
            source,
            "std::string FormatVsReadbackCounts(",
            "std::string ReportVsReadbackProbe(",
        )
        for name in (
            "rb_read_{0}_enter",
            "rb_read_{0}_return",
            "rb_read_{0}_abandon",
            "rb_read_{0}_pending",
            "rb_read_{0}_enter_total",
            "rb_read_refresh_prep",
            "rb_read_refresh_descriptor",
            "rb_read_refresh_other",
            "rb_read_refresh_dirty",
            "rb_read_refresh_clean",
            "rb_read_refresh_q_unknown",
            "rb_read_refresh_negative_result",
            "rb_read_query_in_refresh",
            "rb_read_query_outside_refresh",
            "rb_read_query_parent_unknown",
            "rb_read_copy_in_refresh",
            "rb_read_copy_outside_refresh",
            "rb_read_copy_parent_unknown",
            "rb_read_query_size_valid",
            "rb_read_copy_header_valid",
            "rb_read_requested_meta_bytes",
            "rb_read_requested_code_bytes",
            "rb_read_probe_bytes",
            "rb_read_unreadable",
            "rb_read_range_overflow",
            "rb_read_overlap",
            "rb_read_pair_error",
            "rb_read_correlation_drop",
            "rb_read_event_drop",
            "rb_read_conserved",
        ):
            self.assertIn(name, fields)


if __name__ == "__main__":
    unittest.main()
