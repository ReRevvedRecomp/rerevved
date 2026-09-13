from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "summarize-draw-capture.py"
SPEC = importlib.util.spec_from_file_location("draw_capture_summary", SCRIPT)
assert SPEC and SPEC.loader
summary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary)


class DrawCaptureSummaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.parts = {
            "registers.bin": bytes(16),
            "vertex.ucode.bin": bytes(12),
            "pixel.ucode.bin": bytes(12),
            "edram_before.bin": bytes(8),
            "edram_after.bin": b"\x00\x01\x00\x03\x00\x00\x00\x00",
            "vertex_0.bin": b"vertex bytes",
        }
        for name, contents in self.parts.items():
            (self.root / name).write_bytes(contents)
        self.manifest = {
            "schema_version": 1,
            "status": "complete",
            "files": [
                {"name": name, "size_bytes": len(contents)}
                for name, contents in self.parts.items()
            ],
        }
        self.write_manifest()

    def write_manifest(self) -> None:
        (self.root / "manifest.json").write_text(
            json.dumps(self.manifest), encoding="ascii"
        )

    def test_records_all_file_digests_and_edram_difference(self) -> None:
        result = summary.summarize(self.root)
        self.assertEqual(result["edram_changed_bytes"], 2)
        self.assertEqual(set(result["files"]), set(self.parts))
        for name, contents in self.parts.items():
            self.assertEqual(
                result["files"][name]["sha256"], hashlib.sha256(contents).hexdigest()
            )

    def test_rejects_failed_capture_even_when_files_exist(self) -> None:
        self.manifest["status"] = "failed"
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "no completed"):
            summary.summarize(self.root)

    def test_rejects_short_or_missing_gpu_readback(self) -> None:
        (self.root / "edram_after.bin").write_bytes(bytes(4))
        with self.assertRaisesRegex(ValueError, "size mismatch"):
            summary.summarize(self.root)
        (self.root / "edram_after.bin").unlink()
        with self.assertRaises(FileNotFoundError):
            summary.summarize(self.root)


if __name__ == "__main__":
    unittest.main()
