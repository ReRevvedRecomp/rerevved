"""Contracts for the macOS release path."""

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MacOSContractTests(unittest.TestCase):
    def test_ci_builds_the_locked_sdk_and_uncached_title_in_both_publish_modes(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="ascii")
        job = workflow.split("\n  build-macos:\n", 1)[1]
        for text in (
            "runs-on: macos-15",
            "timeout-minutes: 180",
            "/Applications/Xcode_16.4.app/Contents/Developer",
            'test "$(uname -m)" = arm64',
            "brew install cmake ninja vulkan-loader molten-vk",
            "rexglue-sdk.lock.json",
            "steps.sdk-lock-macos.outputs.commit",
            "submodule update --init --recursive",
            "cmake --preset mac-arm64 -DREXGLUE_BUILD_TESTS=OFF",
            "-DCMAKE_OSX_ARCHITECTURES=arm64",
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3",
            "--target install --parallel 2",
            "GH_TOKEN: ${{ secrets.ASSETS_TOKEN }}",
            "GIT_CONFIG_VALUE_0: '!gh auth git-credential'",
            "--include='game/default.xex,game/default.xexp' --exclude=''",
            "if: always()",
            "EXPECTED_XEX_SIZE",
            "EXPECTED_XEXP_SIZE",
            "--before-codegen out/build/mac-arm64-release",
            "--target rerevved_codegen",
            "python scripts/audit_compile_launchers.py out/build/mac-arm64-release",
            "cmake --build --preset mac-arm64-release --parallel 2",
            "ctest --test-dir out/build/mac-arm64-release -C Release -j 2 --output-on-failure",
            "--platform macos --arch arm64",
            '--macos-root "$extract_root/$archive_name"',
            "path: out/rerevved-v*-macos-arm64.tar.gz",
            "inputs.publish_mode == 'release' && 1 || 14",
        ):
            with self.subTest(text=text):
                self.assertIn(text, job)
        self.assertNotIn("actions/cache", job)
        self.assertNotIn("sccache", job)
        self.assertNotIn("if: inputs.publish_mode", job)
        release = (ROOT / ".github/workflows/release.yml").read_text(encoding="ascii")
        self.assertIn("pattern: rerevved-*", release)
        self.assertIn("artifacts/*.tar.gz", release)

    def test_presets_pin_arm64_and_the_deployment_floor(self):
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="ascii"))
        configure = {entry["name"]: entry for entry in presets["configurePresets"]}
        base = configure["mac-arm64-base"]
        self.assertEqual(base["condition"]["rhs"], "Darwin")
        self.assertEqual(base["cacheVariables"]["CMAKE_OSX_ARCHITECTURES"], "arm64")
        self.assertEqual(base["cacheVariables"]["CMAKE_OSX_DEPLOYMENT_TARGET"], "13.3")
        for kind in ("release", "developer"):
            name = f"mac-arm64-{kind}"
            for section in ("buildPresets", "testPresets"):
                entries = {entry["name"]: entry for entry in presets[section]}
                self.assertEqual(entries[name]["configurePreset"], name)


if __name__ == "__main__":
    unittest.main()
