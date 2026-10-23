"""Synthetic tests for route census extraction and frontier reporting."""

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "route_analysis", REPO / "scripts/gpu/analyze-native-route-trace.py"
)
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


class RouteAnalysisTest(unittest.TestCase):
    def setUp(self):
        self.entries = {
            0x826A3608: {"draw_entry"},
            0x8268CC88: {"template"},
            0x8269B158: {"result_reader", "wait_reader"},
            0x826ABEA8: {"event"},
        }
        self.calls = [(0x826A3608, 0x8250C7B4, 0x8250C498)]

    def snapshot(self, segment, rows=(), overflow=(), count_only=()):
        return {
            "schema": 1,
            "segment": segment,
            "title_revision": "synthetic-title",
            "sdk_revision": "synthetic-sdk",
            "entry_count": len(self.entries),
            "begin_frame": 0,
            "frame_count": 12,
            "callers": list(rows),
            "overflow": list(overflow),
            "count_only": list(count_only),
        }

    def row(self, entry, lr, count=2):
        return {
            "function": entry,
            "group": "|".join(sorted(self.entries[entry])),
            "caller_lr": lr,
            "count": count,
            "segment_count": count,
            "first_frame": 1,
            "segment_first_frame": 1,
        }

    def test_known_and_new_draw_returns(self):
        result = ANALYSIS.analyze(
            [self.snapshot("menu", [self.row(0x826A3608, 0x8250C7B4)])],
            self.entries,
            self.calls,
        )
        self.assertNotIn("DRAW FRONTIER", result)
        self.assertIn("automatic-index 0x826A3608: ran", result)
        self.assertIn("draw templates 0x8268CC88: not observed", result)
        self.assertIn("Incomplete marker/shutdown", result)
        result = ANALYSIS.analyze(
            [self.snapshot("menu", [self.row(0x826A3608, 0xDEADBEEF)])],
            self.entries,
            self.calls,
        )
        self.assertIn("DRAW FRONTIER: 0xDEADBEEF", result)
        self.assertIn("replacement surface remains OPEN", result)

    def test_readers_events_templates_and_overflow(self):
        rows = [self.row(entry, 0x1234) for entry in self.entries]
        report = ANALYSIS.analyze(
            [
                self.snapshot(
                    name,
                    rows,
                    [{"function": 0x8269B158, "count": 3, "segment_count": 1}],
                )
                for name in ("menu", "gameplay", "shutdown")
            ],
            self.entries,
            self.calls,
        )
        self.assertIn("Result-reader owners ran: 0x8269B158", report)
        self.assertIn("Event/wait owners ran: 0x8269B158, 0x826ABEA8", report)
        self.assertIn("draw templates 0x8268CC88: ran", report)
        self.assertIn("capacity overflow at 0x8269B158", report)
        self.assertNotIn("Incomplete marker/shutdown", report)
        self.assertIn("owning function is unclassified", report)

    def test_corrupt_snapshot_is_rejected(self):
        snapshot = self.snapshot("menu", [self.row(0x826A3608, 0x8250C7B4)])
        snapshot["callers"][0]["group"] = "event"
        with self.assertRaisesRegex(ValueError, "differs from census"):
            ANALYSIS.analyze([snapshot], self.entries, self.calls)

    def test_overflow_only_is_executed_in_segment_and_session(self):
        overflow = [{"function": 0x8269B158, "count": 7, "segment_count": 3}]
        report = ANALYSIS.analyze(
            [self.snapshot("shutdown", overflow=overflow)], self.entries, self.calls
        )
        self.assertIn("Result-reader owners ran: 0x8269B158", report)
        self.assertIn("Entries executed in the whole session: 0x8269B158", report)
        self.assertIn("Overflow 0x8269B158: segment=3, cumulative=7", report)
        for line in report.splitlines():
            if "never executed" in line:
                self.assertNotIn("0x8269B158", line)
        overflow[0]["segment_count"] = 0
        report = ANALYSIS.analyze(
            [self.snapshot("shutdown", overflow=overflow)], self.entries, self.calls
        )
        self.assertIn("Result-reader owners ran: \n", report)
        self.assertIn("Entries executed in the whole session: 0x8269B158", report)

    def test_tail_returns_are_not_exact_unknown_callers(self):
        report = ANALYSIS.analyze(
            [self.snapshot("menu", [self.row(0x8269B158, 0x1234)])],
            self.entries,
            self.calls,
            {0x8269B158: 2},
            1,
        )
        self.assertIn("nearest bl return (tail-reachable)", report)
        self.assertNotIn("owning function is unclassified", report)
        self.assertIn("Tail-reachable entry 0x8269B158", report)
        self.assertIn("1 unresolved indirect bctr tail sites", report)
        self.assertIn("Draw entries directly tail-reachable: none", report)

    def test_scan_tail_branches_distinguishes_lr_writes_and_indirect(self):
        with tempfile.TemporaryDirectory() as directory:
            generated = Path(directory)
            (generated / "fixture.cpp").write_text(
                "// b 0x8269b158\nsub_8269B158(ctx, base);\nreturn;\n"
                "// bl 0x826a3608\nctx.lr = 0x1234;\nsub_826A3608(ctx, base);\nreturn;\n"
                "// b 0x826a3608\nctx.lr = 0x1234;\nsub_826A3608(ctx, base);\nreturn;\n"
                "// bctr\nsub_8268CC88(ctx, base);\nreturn;\n"
                "// bctr\nREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\nreturn;\n"
                "// bctrl\nctx.lr = 0x1234;\nREX_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n",
                encoding="ascii",
            )
            tails, indirect = ANALYSIS.scan_tail_entries(generated, self.entries)
            self.assertEqual(tails, {0x8269B158: 1, 0x8268CC88: 1})
            self.assertEqual(indirect, 1)

    def test_count_only_is_executed_without_callers(self):
        self.entries[0x827F8550] = {"resource_update"}
        counts = [{"function": 0x827F8550, "count": 9, "segment_count": 4}]
        report = ANALYSIS.analyze(
            [self.snapshot("shutdown", count_only=counts)], self.entries, self.calls
        )
        self.assertIn("Count-only 0x827F8550: segment=4, cumulative=9", report)
        self.assertIn("Entries executed in the whole session: 0x827F8550", report)
        self.assertIn("per-segment counts use sampled boundaries", report)
        for line in report.splitlines():
            if "never executed" in line:
                self.assertNotIn("0x827F8550", line)
        with self.assertRaisesRegex(ValueError, "count-only function has caller rows"):
            ANALYSIS.analyze(
                [self.snapshot("menu", [self.row(0x827F8550, 0x1234)])],
                self.entries,
                self.calls,
            )

    def test_output_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "analysis.md"
            ANALYSIS.write_report(output, "preserved evidence\n")
            with self.assertRaises(FileExistsError):
                ANALYSIS.write_report(output, "replacement\n")
            with (
                patch(
                    "sys.argv",
                    [
                        "analysis",
                        "--census",
                        "unused",
                        "--capture",
                        "unused",
                        "--output",
                        str(output),
                    ],
                ),
                self.assertRaisesRegex(FileExistsError, "refusing to overwrite"),
            ):
                ANALYSIS.main()
            self.assertEqual(output.read_text(encoding="ascii"), "preserved evidence\n")

    def test_full_lr_is_preserved(self):
        row = self.row(0x826A3608, "0xffffffff8250c7b4")
        result = ANALYSIS.analyze(
            [self.snapshot("menu", [row])], self.entries, self.calls
        )
        self.assertIn("0xFFFFFFFF8250C7B4", result)

    def test_table_check_detects_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            table = Path(directory) / "entries.inc"
            calls = Path(directory) / "calls.inc"
            for path, text in zip(
                (table, calls),
                ANALYSIS.render_tables(self.entries, self.calls),
                strict=True,
            ):
                path.write_text(text, encoding="ascii")
            ANALYSIS.check_tables(self.entries, self.calls, table, calls)
            calls.write_text("stale", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                ANALYSIS.check_tables(self.entries, self.calls, table, calls)

    def test_census_selects_owners_not_instruction_sites(self):
        draw_rows = "\n".join(
            f"| 0x826A3608 | 0x{0x8250C700 + 4 * i:08X} | 0x8250C498 | scene | U | GEN 0x11111111 |"
            for i in range(17)
        )
        reader_rows = "\n".join(
            f"| 0x{0x82690000 + 4 * i:08X} | 1 | 0x2A90 | GEN 0x22222222 |"
            for i in range(33)
        )
        census = "\n".join(
            [
                "## 1. Draw",
                draw_rows,
                "| 0x8268CC88 | template | U | GEN 0x33333333 |",
                "## 2. Producers",
                "| 0x82302758 | producer | 3 | GEN 0x44444444 | U |",
                "bulk 0x826A4CD0, 0x826A50A0 and 0x8281ABB8",
                "## 3. Resources",
                "| GFx decoded texture CPU upload, base and mips | 0x82304CF0 -> 0x82578E00; callsites 0x82305240 | U | source |",
                "## Appendix A. Producers",
                "| 0x8268D690 | 1 | 0x2948 | GEN 0x55555555 |",
                "## Appendix B. Readers",
                reader_rows,
            ]
        )
        entries, calls = ANALYSIS.census_tables(census)
        self.assertEqual(len(calls), 17)
        self.assertEqual(calls[0][1], 0x8250C704)
        self.assertIn("fixed_state_producer", entries[0x8268D690])
        self.assertIn("resource_update", entries[0x82578E00])
        for instruction in (
            0x11111111,
            0x22222222,
            0x33333333,
            0x44444444,
            0x55555555,
            0x82305240,
        ):
            self.assertNotIn(instruction, entries)


if __name__ == "__main__":
    unittest.main()
