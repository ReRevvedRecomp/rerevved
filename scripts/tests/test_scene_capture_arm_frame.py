"""Source pins for the capture arming options; behavior is covered by the scene_resource_* and xenos_frame_reference_* cases of rerevved_tests."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_CPP = (ROOT / "src" / "app.cpp").read_text(encoding="ascii")

SCENE_TRIGGERS = (
    ("native_scene_capture_arm_ps_digest", "STRING", '""'),
    ("native_scene_capture_arm_delay", "UINT32", "0"),
    ("native_scene_capture_windows", "UINT32", "1"),
)
REFERENCE_TRIGGERS = (
    ("native_xenos_frame_reference_arm_ps_digest", "STRING", '""'),
    ("native_xenos_frame_reference_arm_delay", "UINT32", "0"),
    ("native_xenos_frame_reference_windows", "UINT32", "1"),
)


class SceneCaptureArmFrameTests(unittest.TestCase):
    def test_cvar_is_init_only_and_off_by_default(self) -> None:
        self.assertRegex(
            APP_CPP,
            r'REXCVAR_DEFINE_UINT32\(native_scene_capture_arm_frame, 0, "ReRevved", "[^"]*"\)\s*'
            r"\.lifecycle\(rex::cvar::Lifecycle::kInitOnly\);",
        )

    def test_value_reaches_the_recorder(self) -> None:
        call = re.search(r"StartSceneResourceCapture\((.*?)\)\)\s*\{", APP_CPP, re.S)
        self.assertIsNotNone(call)
        arguments = call.group(1)
        # arm_frame stays the last positional argument; the triggers follow as one aggregate.
        self.assertRegex(
            arguments,
            r"REXCVAR_GET\(native_scene_capture_arm_frame\),\s*\{ "
            r"REXCVAR_GET\(native_scene_capture_arm_ps_digest\), "
            r"REXCVAR_GET\(native_scene_capture_arm_delay\), "
            r"REXCVAR_GET\(native_scene_capture_windows\) \}\s*$",
        )

    def test_trigger_cvars_are_init_only_and_off_by_default(self) -> None:
        for name, kind, default in SCENE_TRIGGERS + REFERENCE_TRIGGERS:
            with self.subTest(name=name):
                self.assertRegex(
                    APP_CPP,
                    rf'REXCVAR_DEFINE_{kind}\({name}, {default}, "ReRevved", "[^"]*"\)\s*'
                    r"\.lifecycle\(rex::cvar::Lifecycle::kInitOnly\);",
                )

    def test_reference_options_reach_the_reference(self) -> None:
        call = re.search(r"XenosFrameReferenceOptions options\{(.*?)\};", APP_CPP, re.S)
        self.assertIsNotNone(call)
        self.assertEqual(
            re.findall(r"REXCVAR_GET\((\w+)\)", call.group(1)),
            [
                "native_xenos_frame_reference_arm_frame",
                "native_xenos_frame_reference_arm_ps_digest",
                "native_xenos_frame_reference_arm_delay",
                "native_xenos_frame_reference_windows",
            ],
        )
        self.assertIn("StartXenosFrameReference(output, options, error)", APP_CPP)


if __name__ == "__main__":
    unittest.main()
