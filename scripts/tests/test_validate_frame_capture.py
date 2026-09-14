from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "validate-frame-capture.py"
SPEC = importlib.util.spec_from_file_location("frame_capture_validation", SCRIPT)
assert SPEC and SPEC.loader
validation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validation)


class FrameCaptureValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.journal = {
            "schema_version": 1,
            "complete": True,
            "frame": 12,
            "end_frame": 13,
            "start_submission": 30,
            "end_submission": 33,
            "events": [],
        }
        for event_id, kind in enumerate(("draw", "swap"), 1):
            self.journal["events"].append(
                {
                    "id": event_id,
                    "kind": kind,
                    "frame": 12,
                    "submission": 32,
                    "marker": f"REXGLUE_FRAME_{kind.upper()}_{event_id:016X}",
                    "registers": {
                        "file": f"registers_{event_id}.bin",
                        "count": 0x5003,
                        "bytes": 0x5003 * 4,
                        "encoding": "little_endian_dwords",
                    },
                    "outcome": {"kind": "completed", "success": True},
                }
            )
            (self.root / f"registers_{event_id}.bin").write_bytes(bytes(0x5003 * 4))
        self.journal["events"][0].update(
            shaders={
                "vertex": {"hash": "0000000000000001", "file": "shader_vs.bin"},
                "pixel": {"hash": "0000000000000000", "file": None},
            },
            outcome={"kind": "issued", "success": True, "host_issued": True},
            host={
                "vertex_shader_hash": "0000000000000001",
                "pixel_shader_hash": "0000000000000000",
                "used_texture_mask": 0,
            },
        )
        self.journal["events"][1]["frontbuffer"] = {
            "ptr": "0x00001000",
            "width": 1280,
            "height": 720,
        }
        (self.root / "shader_vs.bin").write_bytes(bytes(12))
        # This validator checks the file envelope, not RenderDoc's binary format.
        (self.root / "capture.rdc").write_bytes(b"synthetic capture")
        self.manifest = {
            **{key: value for key, value in self.journal.items() if key != "events"},
            "status": "complete",
            "valid": True,
            "failure_reason": None,
            "event_count": 2,
            "journal_bytes": 2 * (1024 + 0x5003 * 4) + 12,
            "bounds": {"max_events": 4096, "max_bytes": 64 * 1024 * 1024},
            "renderdoc": {"capture_path": str(self.root / "capture.rdc")},
        }
        self.write_capture()

    def write_capture(self) -> None:
        (self.root / "journal.json").write_text(
            json.dumps(self.journal), encoding="ascii"
        )
        names = {
            "journal.json": "journal",
            "capture.rdc": "renderdoc",
            "shader_vs.bin": "shader",
            "registers_1.bin": "registers",
            "registers_2.bin": "registers",
        }
        self.manifest["files"] = [
            {"path": name, "kind": kind, "size": (self.root / name).stat().st_size}
            for name, kind in names.items()
        ]
        (self.root / "manifest.json").write_text(
            json.dumps(self.manifest), encoding="ascii"
        )

    def test_records_provenance_without_claiming_gpu_replay(self) -> None:
        result = validation.validate(self.root)
        self.assertEqual(result["outcomes"], {"draw:issued": 1, "swap:completed": 1})
        self.assertEqual(result["host_shader_pairs"][0]["draw_count"], 1)
        self.assertEqual(len(result["files"]["capture.rdc"]["sha256"]), 64)
        self.assertIn("require RenderDoc inspection", result["scope"])

    def test_rejects_capture_failed_after_journal_write(self) -> None:
        self.manifest.update(status="failed", valid=False, failure_reason="end_failed")
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "no completed"):
            validation.validate(self.root)

    def test_lists_guest_failures_without_claiming_guest_success(self) -> None:
        self.journal["events"][0]["outcome"]["success"] = False
        self.write_capture()
        result = validation.validate(self.root)
        self.assertEqual(result["failed_guest_events"], [{"id": 1, "kind": "draw"}])

    def test_rejects_truncated_registers_even_with_updated_inventory(self) -> None:
        (self.root / "registers_1.bin").write_bytes(bytes(4))
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "snapshot is missing or truncated"):
            validation.validate(self.root)

    def test_rejects_dropped_event_and_missing_swap(self) -> None:
        self.journal["events"].pop()
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "event count mismatch"):
            validation.validate(self.root)
        self.manifest["event_count"] = 1
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "exactly one swap"):
            validation.validate(self.root)

    def test_rejects_stale_journal_boundary(self) -> None:
        self.journal["end_submission"] += 1
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "boundary disagrees"):
            validation.validate(self.root)

    def test_rejects_multiple_guest_frames_in_a_complete_envelope(self) -> None:
        self.journal["end_frame"] = 14
        self.manifest["end_frame"] = 14
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "must span one guest frame"):
            validation.validate(self.root)

    def test_rejects_shader_missing_from_inventory(self) -> None:
        self.journal["events"][0]["shaders"]["vertex"]["file"] = "absent.bin"
        self.write_capture()
        with self.assertRaisesRegex(ValueError, "shader is missing"):
            validation.validate(self.root)


if __name__ == "__main__":
    unittest.main()
