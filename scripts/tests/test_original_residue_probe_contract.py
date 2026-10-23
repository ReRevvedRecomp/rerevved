from __future__ import annotations

import re
import tomllib
import unittest
from pathlib import Path

from cmake_sources import rerevved_tests_sources

ROOT = Path(__file__).resolve().parents[2]


SOURCE = ROOT / "src/gpu/guest_d3d/original_residue_probe.cpp"
HEADER = ROOT / "src/gpu/guest_d3d/original_residue_probe.h"
HOOKS = ROOT / "src/gpu/guest_d3d/ring_stub_hooks.cpp"
STUBS = ROOT / "src/gpu/guest_d3d/ring_stubs.cpp"


def body(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class OriginalResidueProbeContractTests(unittest.TestCase):
    def test_default_off_observation_only(self) -> None:
        source = SOURCE.read_text()
        header = HEADER.read_text()
        self.assertRegex(
            source, r"REXCVAR_DEFINE_BOOL\(native_original_residue_probe,\s*false,"
        )
        self.assertIn("Lifecycle::kInitOnly", source)
        self.assertIn("originalResidueProbeState{ -1 }", source)
        self.assertIn("RefreshOriginalResidueProbeRequested", header)
        self.assertIn("std::memory_order_relaxed", header)
        self.assertNotIn("RingStubsPublished", source + header)
        self.assertNotIn("ringStubsActive", source + header)
        self.assertNotRegex(
            source + header,
            r"PPCContext|TranslateVirtual|Read32\(|Read8\(|Write32\(|Write8\(|REX_HOOK|ObserveCompletion|RecordDirectSubmit|RecordDeferredKick",
        )
        note = body(
            source,
            "void NoteOriginalResidueDecision(",
            "void CloseOriginalResidueProbe()",
        )
        self.assertNotRegex(note, r"REXLOG|std::format|new |mutex|lock|push_back")
        # Late, two error cases, or one dispatch cell.
        self.assertEqual(note.count("fetch_add(1,"), 4)
        self.assertIn("!validReason(function, reason)", note)
        self.assertRegex(source, r"\.exchange\(0, std::memory_order_relaxed\)")

    def test_existing_wrapper_ownership_and_order(self) -> None:
        hooks = HOOKS.read_text()
        for name in ("8269C400", "8269BD98"):
            wrapper = body(hooks, f"REX_HOOK_RAW(sub_{name})", "REX_HOOK_RAW")
            self.assertEqual(wrapper.count("residue(ctx.lr)"), 1)
            self.assertEqual(wrapper.count("guest::ready("), 1)
            self.assertEqual(wrapper.count(f"__imp__sub_{name}(ctx, base)"), 1)
            self.assertLess(
                wrapper.index("residue(ctx.lr)"), wrapper.index("guest::ready(")
            )
            self.assertRegex(
                wrapper,
                rf"#if REREVVED_DEVELOPER\s+(?:[^#]*?\s)?if \(residue\.active\)\s+guest::NoteOriginalResidueDecision\([^;]+;\s*#endif\s+__imp__sub_{name}\(ctx, base\);\s*}}",
            )
            self.assertNotRegex(
                wrapper.split(f"__imp__sub_{name}(ctx, base);", 1)[1], r"ctx\."
            )
        # The request is read once, where the trace for the call begins.
        self.assertEqual(hooks.count("OriginalResidueProbeRequested()"), 1)
        self.assertRegex(
            hooks,
            r"explicit ResidueTrace\(std::uint64_t callerLr\) noexcept\s*"
            r": active\(OriginalResidueProbeRequested\(\)\)\s*"
            r", lr\(active \? callerLr : 0\)",
        )
        publish = body(hooks, "REX_HOOK_RAW(sub_8269C400)", "REX_HOOK_RAW")
        self.assertIn("OriginalResidueDispatch::Original", publish)
        self.assertIn("OriginalResidueDispatch::Classified", publish)
        self.assertNotIn("ExitScope", publish)
        self.assertEqual(publish.count("guest::ClassifyPublish("), 1)
        self.assertEqual(publish.count("return;"), 1)
        self.assertRegex(
            publish,
            r"consumePublish\([^;]+\) == guest::ConsumeOutcome::Consumed\)\s+return;",
        )
        self.assertLess(publish.index("return;"), publish.index("__imp__sub_8269C400"))
        submit = body(hooks, "REX_HOOK_RAW(sub_8269BD98)", "REX_HOOK_RAW")
        self.assertEqual(submit.count("guest::ClassifyPublisher("), 1)
        self.assertEqual(submit.count("guest::RaiseForDirectSubmit("), 1)
        self.assertLess(
            submit.index("residue(ctx.lr)"), submit.index("CompletionScope")
        )
        self.assertLess(
            submit.index("CompletionScope"), submit.index("TilingSubmitScope")
        )
        self.assertLess(
            submit.index("TilingSubmitScope"), submit.index("ringStubsActive")
        )
        self.assertRegex(
            submit,
            r"if \(path != guest::PublisherPath::Original\)\s*\{\s*guest::ObserveCompletionStubBegin\(\);\s*raised = guest::RaiseForDirectSubmit",
        )
        self.assertEqual(submit.count("RecordDirectSubmit("), 1)
        self.assertLess(
            submit.index("RecordDirectSubmit("), submit.index("guest::kick(g)")
        )
        self.assertIn("ctx.r3.u64 = ctx.r4.u32;", submit)
        self.assertIn('"G+0x541C or G+0x5B0C set"', submit)
        self.assertIn('"worker owns the ring: deferred chain"', submit)
        ready = body(hooks, "bool ready(", "// Runs a guest function")
        self.assertEqual(ready.count("CheckRingLayout("), 1)
        self.assertEqual(ready.count("DisableRingStubs("), 1)
        cached = ready.split("const auto check", 1)[0]
        self.assertIn("validatedDevice.load(std::memory_order_acquire) == g", cached)
        self.assertIn("return true;", cached)
        self.assertNotRegex(cached, r"CheckRingLayout|Read32|Read8|g != 0")

    def test_diagnostics_reuse_existing_operations(self) -> None:
        source = STUBS.read_text()
        publish = body(
            source, "PublishBranch ClassifyPublish(", "std::optional<Coherency>"
        )
        self.assertEqual(publish.count("memory.Read8("), 2)
        self.assertEqual(publish.count("memory.Read32("), 1)
        self.assertLess(publish.index("kModeHigh"), publish.index("kModeLow"))
        self.assertLess(publish.index("kModeLow"), publish.index("kTiling"))
        self.assertLess(
            publish.index("!high || !low || !tiling"),
            publish.index("*high & kModeRingStarved"),
        )
        self.assertLess(
            publish.index("*high & kModeRingStarved"),
            publish.index("*low & kModeLowRecording"),
        )
        publisher = body(
            source, "PublisherPath ClassifyPublisher(", "bool RaiseForDirectSubmit("
        )
        self.assertEqual(publisher.count("memory.Read32("), 2)
        self.assertEqual(publisher.count("memory.Read8("), 1)
        self.assertLess(
            publisher.index("kCpuFenceSink"), publisher.index("kRingCallback")
        )
        self.assertLess(publisher.index("kRingCallback"), publisher.index("kModeHigh"))
        raise_body = body(
            source, "bool RaiseForDirectSubmit(", "bool DirectWriterIsStubbed("
        )
        self.assertEqual(raise_body.count("memory.Read32("), 1)
        self.assertEqual(raise_body.count("memory.Write32("), 1)
        self.assertRegex(raise_body, r"if \(raise == 0\)\s*return true;")
        self.assertIn("diagnostic->writeSucceeded = written;", raise_body)
        self.assertIn("return written;", raise_body)

    def test_reporting_and_final_legacy_drain(self) -> None:
        source = SOURCE.read_text()
        final = body(
            source,
            "OriginalResidueShutdownCounts TakeOriginalResidueShutdownCounts()",
            "std::string ReportOriginalResidueProbe(",
        )
        self.assertEqual(final.count("TakeOriginalResidueCounts()"), 1)
        self.assertEqual(final.count("TakeRingStubCounts()"), 1)
        self.assertNotRegex(final, r"timing|Timing|shutdown|Shutdown\s*&&")
        reporter = source.split("std::string ReportOriginalResidueProbe(", 1)[1]
        self.assertRegex(reporter.split("lock.lock();", 1)[1], r"if \(shutdown\)\s*\{")
        self.assertIn("TakeOriginalResidueShutdownCounts()", reporter)
        self.assertIn('"Native ring stubs: shutdown{}"', source)
        self.assertIn('"Native original residue probe: shutdown{}"', source)
        self.assertNotRegex(reporter, r"native_frame_timing|FrameTiming")
        app = (ROOT / "src/app.cpp").read_text()
        self.assertEqual(app.count("ReportOriginalResidueProbe(true)"), 2)
        shutdown = body(
            app, "void App::OnShutdown()", "bool App::OnWindowCloseRequested()"
        )
        self.assertLess(
            shutdown.index("StopProducers("),
            shutdown.index("ReportOriginalResidueProbe(true)"),
        )
        self.assertRegex(
            shutdown,
            r"if \(guestVideoStopped\)\s*\{\s*#if REREVVED_DEVELOPER\s*rerevved::gpu::guest_d3d::CloseOriginalResidueProbe\(\);\s*rerevved::gpu::guest_d3d::ReportOriginalResidueProbe\(true\);\s*#endif",
        )
        hard = body(app, "void App::OnBeforeHardExit()", "void App::OnKeyDown(")
        self.assertLess(
            hard.index("#if defined(_WIN32)"),
            hard.index("ReportOriginalResidueProbe(true)"),
        )
        self.assertNotRegex(shutdown + hard, r"native_frame_timing|FrameTiming")
        self.assertIn("gpu::guest_d3d::ReportOriginalResidueProbe();", app)
        self.assertNotIn("phases += gpu::guest_d3d::ReportOriginalResidueProbe();", app)
        self.assertEqual(
            app.count("FormatRingStubCounts(gpu::guest_d3d::TakeRingStubCounts())"), 1
        )

    def test_close_before_final_reports(self) -> None:
        app = (ROOT / "src/app.cpp").read_text()
        for start, end in (
            ("void App::OnShutdown()", "bool App::OnWindowCloseRequested()"),
            ("void App::OnBeforeHardExit()", "void App::OnKeyDown("),
        ):
            final = body(app, start, end)
            self.assertEqual(final.count("CloseOriginalResidueProbe()"), 1)
            self.assertLess(
                final.index("CloseOriginalResidueProbe()"),
                final.index("ReportOriginalResidueProbe(true)"),
            )
        source = SOURCE.read_text()
        close = body(
            source,
            "void CloseOriginalResidueProbe()",
            "OriginalResidueCounts TakeOriginalResidueCounts()",
        )
        self.assertIn("originalResidueAdmissions.fetch_or(kClosed", close)
        self.assertRegex(
            close,
            r"while \(\(originalResidueAdmissions.load\([^\n]+& ~kClosed\) != 0\s*&& std::chrono::steady_clock::now\(\) < deadline\)",
        )
        self.assertIn("std::memory_order_acquire", close)
        self.assertIn("detail::kOriginalResidueShutdownWait", close)
        self.assertIn("std::chrono::milliseconds(10)", HEADER.read_text())

    def test_final_report_lock_is_bounded(self) -> None:
        source = SOURCE.read_text()
        reporter = source.split("std::string ReportOriginalResidueProbe(", 1)[1]
        lock_failure = body(
            reporter,
            "if (!lock.try_lock_for(detail::kOriginalResidueShutdownWait))",
            "else",
        )
        self.assertIn("std::timed_mutex", source)
        self.assertIn("skippedFinalReports.fetch_add(1,", lock_failure)
        self.assertIn("reportingFailures.fetch_add(1,", lock_failure)
        self.assertIn("or_final_incomplete=1 or_final_skipped=1", lock_failure)
        self.assertNotRegex(lock_failure, r"logReport|TakeOriginalResidue|operations\.")
        drain = body(
            source,
            "OriginalResidueCounts TakeOriginalResidueCounts()",
            "std::string FormatOriginalResidueCounts(",
        )
        self.assertLess(
            drain.index("originalResidueAdmissions.load("), drain.index(".exchange(")
        )

    def test_reporting_retains_failed_batches(self) -> None:
        source = SOURCE.read_text()
        reporter = source.split("std::string ReportOriginalResidueProbe(", 1)[1]
        final = body(reporter, "if (shutdown)", "retain(TakeOriginalResidueCounts());")
        self.assertLess(
            final.index("retain(counts.residue)"),
            final.index("operations.formatResidue("),
        )
        self.assertLess(
            final.index("retain(counts.legacy)"),
            final.index("operations.formatLegacy("),
        )
        self.assertLess(
            final.index("operations.formatLegacy("),
            final.index("logReport(operations, false,"),
        )
        for family, legacy in (("residue", "false"), ("legacy", "true")):
            self.assertLess(
                final.index(f"logReport(operations, {legacy},"),
                final.index(f"pendingReport.{family} = {{}};"),
            )
        failure = body(reporter, "catch (...)", "return {};")
        self.assertIn("reportingFailures.fetch_add(1,", failure)
        self.assertNotIn("pendingReport", failure)

    def test_periodic_report_logs_before_commit(self) -> None:
        source = SOURCE.read_text()
        periodic = body(source, "retain(TakeOriginalResidueCounts());", "catch (...)")
        self.assertLess(
            periodic.index("operations.formatResidue("),
            periodic.index("logReport(operations, false, fields, false);"),
        )
        self.assertLess(
            periodic.index("logReport(operations, false, fields, false);"),
            periodic.index("pendingReport.residue = {};"),
        )
        self.assertIn('"Native original residue probe: periodic{}"', source)
        app = (ROOT / "src/app.cpp").read_text()
        self.assertNotIn("phases += gpu::guest_d3d::ReportOriginalResidueProbe();", app)

    def test_wrapper_vocabulary_and_build_registration(self) -> None:
        source = SOURCE.read_text()
        formatter = body(
            source,
            "std::string FormatOriginalResidueCounts(",
            "OriginalResidueShutdownCounts TakeOriginalResidueShutdownCounts()",
        )
        self.assertIn("or_scope=wrapper_decision", formatter)
        self.assertNotRegex(formatter, r"cp_|cef_|deferred_arm|native_arm|body_entry")
        for name in (
            "publish_calls",
            "publish_original_dispatch",
            "submit_calls",
            "submit_original_dispatch",
            "submit_served_kick",
            "submit_served_silent",
            "internal_error",
            "writers_admitted",
            "final_incomplete",
            "final_skipped",
        ):
            self.assertIn("or_" + name + "={}", formatter)
        reason_names = body(source, "reasonNames{", "};")
        self.assertEqual(
            re.findall(r'"([a-z_]+)"', reason_names),
            [
                "inactive",
                "not_ready",
                "layout_mismatch",
                "unreadable",
                "starved",
                "recording",
                "tiling",
                "submit",
                "publisher_unreadable",
                "publisher_sink",
                "publisher_callback",
                "pending_unreadable",
                "pending_nonzero",
                "raise_write_failed",
                "served_kick",
                "served_silent",
                "lock_storage",
                "token_storage",
                "token_wrap",
                "list_storage",
            ],
        )
        manifest = tomllib.loads((ROOT / "rerevved_manifest.toml").read_text())
        for config in manifest["entrypoint"]["includes"]:
            document = tomllib.loads((ROOT / config).read_text())
            self.assertFalse(
                any(
                    "OriginalResidue" in hook["name"]
                    for hook in document.get("midasm_hook", [])
                )
            )
        developer = (
            (ROOT / "CMakeLists.txt")
            .read_text()
            .split("if(REREVVED_DEVELOPER)\n    list(APPEND REREVVED_SOURCES", 1)[1]
            .split("endif()", 1)[0]
        )
        self.assertIn("src/gpu/guest_d3d/original_residue_probe.cpp", developer)
        test_sources = rerevved_tests_sources()
        for path in (
            "src/gpu/guest_d3d/original_residue_probe.cpp",
            "src/gpu/guest_d3d/ring_stub_hooks.cpp",
            "tests/gpu/original_residue_probe_test.cpp",
            "tests/gpu/ring_stub_route_test.cpp",
        ):
            self.assertIn(path, test_sources)
        self.assertRegex(
            (ROOT / "tests/gpu/original_residue_probe_test.cpp").read_text(),
            r'TEST_CASE\("original_residue_probe", "\[ctest\]',
        )


if __name__ == "__main__":
    unittest.main()
