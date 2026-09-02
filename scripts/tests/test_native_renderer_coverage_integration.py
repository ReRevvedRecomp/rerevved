"""Static contract tests for the title-owned native-renderer observer integration."""

from __future__ import annotations

import json
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
APP_CPP = (ROOT / "src" / "rerevved_app.cpp").read_text(encoding="ascii")
CAPTURE = (ROOT / "scripts" / "capture-window.ps1").read_text(encoding="ascii")
MANIFEST = (ROOT / "rerevved_manifest.toml").read_text(encoding="ascii")
LOCK = json.loads((ROOT / "rexglue-sdk.lock.json").read_text(encoding="ascii"))


class CoverageIntegrationTests(unittest.TestCase):
    def test_observer_is_default_off_and_admitted_before_guest_launch(self) -> None:
        self.assertIn(
            'REXCVAR_DEFINE_STRING(native_renderer_coverage_run, ""', APP_CPP
        )
        setup = APP_CPP.index("bool ReRevvedApp::SetupPresentation()")
        post_setup = APP_CPP.index("void ReRevvedApp::OnPostSetup()")
        start = APP_CPP.index("native_renderer::Start(options)")
        self.assertLess(setup, start)
        self.assertLess(start, post_setup)
        self.assertIn('output_name != "observer"', APP_CPP)
        self.assertIn('user_data_root().filename() != "user-data"', APP_CPP)

    def test_f10_is_host_only_and_all_exit_paths_finalize(self) -> None:
        self.assertRegex(
            APP_CPP,
            r"constexpr\s+std::size_t\s+kCoverageInputZOrder\s*=\s*1000;",
        )
        self.assertIn(
            "window()->AddInputListener(this, kCoverageInputZOrder);", APP_CPP
        )
        self.assertIn('"bind_native_renderer_coverage_checkpoint"', APP_CPP)
        self.assertIn('"F10"', APP_CPP)
        self.assertIn(
            'REXLOG_INFO("NRD-COVERAGE-CHECKPOINT accepted={}"', APP_CPP
        )
        self.assertIn("rex::ui::ProcessKeyEvent(event);", APP_CPP)
        repeat_filter = APP_CPP.index("void ReRevvedApp::OnKeyDown")
        handled = APP_CPP.index("event.set_handled(true);", repeat_filter)
        bind_dispatch = APP_CPP.index("rex::ui::ProcessKeyEvent(event);", repeat_filter)
        handler = APP_CPP[repeat_filter:bind_dispatch]
        self.assertLess(repeat_filter, handled)
        self.assertLess(handled, bind_dispatch)
        self.assertIn("coverage_bind_registered_ &&", handler)
        self.assertIn(
            "event.virtual_key() == rex::ui::VirtualKey::kF10 &&",
            handler,
        )
        self.assertIn("event.prev_state()", handler)
        self.assertEqual(handler.count("event.set_handled(true);"), 1)
        self.assertIn("ExitClass::GuestComplete", APP_CPP)
        self.assertIn("ExitClass::WindowClose", APP_CPP)
        self.assertIn("ExitClass::Shutdown", APP_CPP)
        self.assertIn("native-renderer coverage observer start segment failed", APP_CPP)
        self.assertIn("final_segment_recorded", APP_CPP)

    def test_capture_helper_encodes_png_bytes(self) -> None:
        self.assertIn("window_topmost_capture.png", CAPTURE)
        self.assertIn("[System.Drawing.Imaging.ImageFormat]::Png", CAPTURE)
        self.assertNotIn("[System.Drawing.Imaging.ImageFormat]::Bmp", CAPTURE)

    def test_only_generated_coverage_hooks_are_included(self) -> None:
        self.assertEqual(MANIFEST.count("native_renderer_coverage_hooks.toml"), 1)
        self.assertEqual(
            LOCK["commit"], "37dd3f38d2fa6501ca114578c8176a0633e070fc"
        )

    def test_locked_sdk_dispatches_title_before_guest_mnk(self) -> None:
        sdk = os.environ.get("REREVVED_SDK_SOURCE")
        if not sdk:
            sibling = ROOT.parent / "rerevved-rexglue-sdk"
            if sibling.is_dir():
                sdk = str(sibling)
        if not sdk:
            self.skipTest("locked SDK source is not available")

        commit = LOCK["commit"]

        def show(path: str) -> str:
            return subprocess.run(
                ["git", "-C", sdk, "show", f"{commit}:{path}"],
                check=True,
                capture_output=True,
                text=True,
                encoding="utf-8",
            ).stdout

        input_system = show("src/input/input_system.cpp")
        window = show("src/ui/window.cpp")
        keybinds = show("src/ui/keybinds.cpp")
        self.assertIn("MnkInputDriver>(nullptr, 0)", input_system)
        self.assertIn("input_listeners_.crbegin()", window)
        self.assertIn("if (event_handled)", window)
        self.assertIn("e.set_handled(true);", keybinds)
        self.assertIn("return true;", keybinds)


if __name__ == "__main__":
    unittest.main()
