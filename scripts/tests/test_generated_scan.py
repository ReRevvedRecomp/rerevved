"""Synthetic chunk coverage for the shared generated-code scanner."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

import generated_scan


class GeneratedScanTests(unittest.TestCase):
    def test_cache_reuses_and_invalidates_chunk_signatures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chunk = root / "rerevved_recomp.0.cpp"
            chunk.write_text("old", encoding="ascii")
            scan = Mock(side_effect=lambda paths: [p.read_text() for p in paths])
            with patch.object(generated_scan, "GENERATED", root):
                paths, first = generated_scan.scan_once("synthetic", scan)
                self.assertEqual(paths, [chunk])
                self.assertEqual(first, ["old"])
                self.assertIs(generated_scan.scan_once("synthetic", scan)[1], first)
                scan.assert_called_once_with([chunk])
                stamp = chunk.stat().st_mtime_ns
                chunk.write_text("new", encoding="ascii")
                os.utime(chunk, ns=(stamp + 1_000_000_000, stamp + 1_000_000_000))
                self.assertEqual(
                    generated_scan.scan_once("synthetic", scan)[1], ["new"]
                )
                chunk.write_text("longer", encoding="ascii")
                os.utime(chunk, ns=(stamp + 1_000_000_000, stamp + 1_000_000_000))
                self.assertEqual(
                    generated_scan.scan_once("synthetic", scan)[1], ["longer"]
                )
                self.assertEqual(scan.call_count, 3)

    def test_windows_finish_or_stop_at_chunk_end(self):
        for keep_partial in (False, True):
            with self.subTest(keep_partial=keep_partial):
                windows = generated_scan.WindowCollector()
                windows.start("complete", ["start\n"], 1)
                windows.start("partial", ["start\n"], 2)
                windows.advance("next\n")
                windows.end_chunk(keep_partial=keep_partial)
                windows.advance("other chunk\n")
                expected = {"complete": "start\nnext\n"}
                if keep_partial:
                    expected["partial"] = "start\nnext\n"
                self.assertEqual(windows.windows, expected)

    def test_counts_exclude_matches_across_chunk_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, text in enumerate(("hook ho", "ok hook")):
                (root / f"rerevved_recomp.{index}.cpp").write_text(
                    text, encoding="ascii"
                )
            with patch.object(generated_scan, "GENERATED", root):
                paths, joined = generated_scan.generated_text()
                self.assertEqual(joined, "hook hook hook")
                self.assertEqual(generated_scan.count_per_chunk("hook"), 2)
                self.assertEqual(
                    list(generated_scan.chunk_texts()), ["hook ho", "ok hook"]
                )
                self.assertEqual(
                    generated_scan.generated_chunks().chunk(paths[1]), "ok hook"
                )


if __name__ == "__main__":
    unittest.main()
