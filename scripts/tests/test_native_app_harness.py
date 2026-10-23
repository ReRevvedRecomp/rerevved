"""Exercise optimized harness failure paths without launching the app."""

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests/gpu/native_app_harness.py"


class NativeAppHarnessTests(unittest.TestCase):
    def run_harness(
        self,
        events=None,
        code=0,
        timeout=False,
        catalog="catalog",
        mode="app",
        verdict=None,
        trace_text=None,
    ):
        harness = types.ModuleType("optimized_native_app_harness")
        exec(
            compile(HARNESS.read_text(), str(HARNESS), "exec", optimize=2),
            harness.__dict__,
        )
        child = Mock()
        child.wait.side_effect = (
            [subprocess.TimeoutExpired("app", 45), -1] if timeout else [code]
        )
        with tempfile.TemporaryDirectory() as directory:

            def launch(command, **kwargs):
                if verdict is not None:
                    kwargs["stdout"].write(verdict)
                if events is not None or trace_text is not None:
                    trace = Path(
                        next(
                            arg.split("=", 1)[1]
                            for arg in command
                            if arg.startswith("--native_present_trace=")
                        )
                    )
                    trace.write_text(
                        trace_text
                        if trace_text is not None
                        else "".join(
                            f"[[event]]\nname = '{name}'\nframe = {frame}\nvalue = {value}\n"
                            for name, frame, value in events
                        )
                    )
                return child

            output = io.StringIO()
            with (
                patch.dict(os.environ, {"REREVVED_NATIVE_PRESENT_CATALOG": catalog}),
                patch.object(sys, "argv", [str(HARNESS), mode, "app.exe", directory]),
                patch.object(harness.subprocess, "Popen", side_effect=launch),
                contextlib.redirect_stdout(output),
            ):
                result = harness.main()
        return result, output.getvalue(), child

    def test_unknown_mode_is_a_usage_error(self):
        result, output, child = self.run_harness(mode="other")
        self.assertEqual(result, 2)
        self.assertIn("usage:", output)
        child.wait.assert_not_called()

    def test_smoke_mode_reads_the_verdict(self):
        result, output, child = self.run_harness(
            mode="smoke", verdict="noise\nPASS: 12 native fixture frames\n"
        )
        self.assertEqual(result, 0)
        self.assertIn("PASS: 12 native fixture frames", output)
        command = child.method_calls
        self.assertEqual([call[0] for call in command], ["wait"])

    def test_smoke_mode_fails_on_a_failed_verdict(self):
        result, output, _ = self.run_harness(
            mode="smoke", code=1, verdict="FAIL: missing suspended pump\n"
        )
        self.assertEqual(result, 1)
        self.assertIn("exit 1; FAIL: missing suspended pump", output)

    def test_smoke_mode_fails_a_clean_exit_without_a_pass(self):
        result, output, _ = self.run_harness(mode="smoke", verdict="")
        self.assertEqual(result, 1)
        self.assertIn("no verdict", output)

    def test_smoke_mode_skips_when_the_gpu_is_unavailable(self):
        result, output, _ = self.run_harness(
            mode="smoke", code=77, verdict="SKIP: GPU/window unavailable\n"
        )
        self.assertEqual(result, 77)
        self.assertIn("GPU/window unavailable", output)

    def test_smoke_mode_timeout_kills_and_reaps_own_child(self):
        result, output, child = self.run_harness(mode="smoke", timeout=True)
        self.assertEqual(result, 1)
        self.assertIn("did not finish", output)
        self.assertEqual(
            [call[0] for call in child.method_calls], ["wait", "kill", "wait"]
        )

    def test_missing_catalog_skips_and_names_input(self):
        result, output, child = self.run_harness(catalog="")
        self.assertEqual(result, 77)
        self.assertIn("REREVVED_NATIVE_PRESENT_CATALOG", output)
        child.wait.assert_not_called()

    def test_unavailable_skips(self):
        result, output, _ = self.run_harness([("unavailable", 0, 0)], code=1)
        self.assertEqual(result, 77)
        self.assertIn("GPU/window unavailable", output)

    def test_timeout_kills_and_reaps_own_child(self):
        result, output, child = self.run_harness(timeout=True)
        self.assertEqual(result, 1)
        self.assertIn("FAIL:", output)
        self.assertEqual(
            [call[0] for call in child.method_calls], ["wait", "kill", "wait"]
        )
        child.wait.assert_called_with()

    def test_nonzero_exit_fails_with_optimization(self):
        result, output, _ = self.run_harness([("startup", 0, 0)], code=1)
        self.assertEqual(result, 1)
        self.assertIn("FAIL: exit 1", output)

    def test_incomplete_event_order_fails_with_optimization(self):
        result, output, _ = self.run_harness([("startup", 0, 0)])
        self.assertEqual(result, 1)
        self.assertIn("missing ordered event detached", output)

    def test_missing_trace_fails(self):
        result, output, _ = self.run_harness()
        self.assertEqual(result, 1)
        self.assertIn("missing trace", output)

    def test_invalid_trace_fails(self):
        for trace_text in ("[[event", "other = 1"):
            with self.subTest(trace_text=trace_text):
                result, output, _ = self.run_harness(trace_text=trace_text)
                self.assertEqual(result, 1)
                self.assertIn("invalid trace", output)

    @staticmethod
    def complete_events():
        events = [("startup", 0, 0), ("detached", 0, 0)]
        for frame in range(1, 5):
            events.extend(
                [("published", frame, 1), ("present", frame, 1), ("gamma", frame, 1)]
            )
            if frame == 1:
                events.extend([("surface", 1280, 720), ("script_resize", 0, 0)])
            if frame == 2:
                events.extend(
                    [
                        ("surface", 960, 640),
                        ("script_minimize", 0, 0),
                        ("suspend", 0, 0),
                        ("present", 0, 3),
                        ("script_restore", 0, 0),
                        ("restore", 0, 0),
                    ]
                )
        events.extend(
            [
                ("script_overlay_open", 0, 0),
                ("published", 5, 1),
                ("present", 5, 1),
                ("overlay_composed", 5, 1),
                ("published", 6, 1),
                ("present", 6, 1),
                ("overlay_composed", 6, 2),
                ("script_overlay_input", 0, 3),
                ("overlay_input_capture", 0, 1),
                ("script_overlay_close", 0, 0),
                ("published", 7, 1),
                ("present", 7, 1),
                ("overlay_cleared", 7, 3),
                ("published", 8, 1),
                ("present", 8, 1),
                ("published", 9, 1),
                ("present", 9, 1),
            ]
        )
        events.extend(
            (name, 0, int(name == "shutdown"))
            for name in (
                "script_close",
                "close_veto",
                "close_admission",
                "close_veto",
                "producer_stopped",
                "shutdown",
                "overlays_shutdown",
                "close_accept",
            )
        )
        return events

    def test_failed_drain_fails(self):
        events = [
            (name, frame, 0 if name == "shutdown" else value)
            for name, frame, value in self.complete_events()
        ]
        result, output, _ = self.run_harness(events)
        self.assertEqual(result, 1)
        self.assertIn("shutdown did not drain successfully", output)

    def test_missing_gamma_fails(self):
        events = [event for event in self.complete_events() if event[0] != "gamma"]
        result, output, _ = self.run_harness(events)
        self.assertEqual(result, 1)
        self.assertIn("incorrect gamma generation", output)

    def test_complete_occluded_trace_passes(self):
        events = self.complete_events()
        result, output, _ = self.run_harness(events)
        self.assertEqual(result, 0)
        self.assertIn("occluded or presented", output)
        frozen = [
            (name, 4 if name == "overlay_composed" else frame, value)
            for name, frame, value in events
        ]
        result, output, _ = self.run_harness(frozen)
        self.assertEqual(result, 1)
        self.assertIn("at least two advancing published frames", output)
        first_frame_only = [
            event for event in events if event != ("overlay_composed", 6, 2)
        ]
        result, output, _ = self.run_harness(first_frame_only)
        self.assertEqual(result, 1)
        self.assertIn("at least two advancing published frames", output)
        repeated_frame = [
            (name, 5 if name == "overlay_composed" else frame, value)
            for name, frame, value in events
        ]
        result, output, _ = self.run_harness(repeated_frame)
        self.assertEqual(result, 1)
        self.assertIn("at least two advancing published frames", output)
        repeated_clear = events.copy()
        repeated_clear.insert(-1, ("overlay_cleared", 7, 3))
        result, output, _ = self.run_harness(repeated_clear)
        self.assertEqual(result, 1)
        self.assertIn("clear was not deduplicated", output)
        result, output, _ = self.run_harness(
            [event for event in events if event[0] != "overlay_composed"]
        )
        self.assertEqual(result, 1)
        self.assertIn("missing ordered event overlay_composed", output)


if __name__ == "__main__":
    unittest.main()
