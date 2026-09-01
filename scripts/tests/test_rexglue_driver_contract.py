from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "scripts" / "rexglue.ps1"


class RexGlueDriverContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = DRIVER.read_text(encoding="ascii")

    def test_isolated_path_overrides_keep_existing_defaults(self) -> None:
        for parameter in (
            "UserDataRoot",
            "CacheRoot",
            "LogPath",
            "SdkRepo",
            "SdkInstall",
        ):
            self.assertIn(f"[string]${parameter}", self.source)
        for relative in (
            "out\\rexglue-user",
            "out\\rexglue-cache",
            "out\\rexglue_boot.log",
        ):
            self.assertIn(relative, self.source)

    def test_self_test_stops_before_output_creation(self) -> None:
        self_test = self.source.index("if ($SelfTest)")
        launch_gate = self.source.index("if ($Stage -notin @('Launch', 'All'))")
        output_creation = self.source.index(
            "New-Item -ItemType Directory -Force -Path $userData, $cache"
        )
        self.assertLess(self_test, launch_gate)
        self.assertLess(launch_gate, output_creation)

    def test_launch_arguments_use_resolved_override_paths(self) -> None:
        for expression in (
            "Quote-Cmd $userData",
            "Quote-Cmd $cache",
            "Quote-Cmd $log",
        ):
            self.assertIn(expression, self.source)
        self.assertEqual(
            self.source.count(
                "New-Item -ItemType Directory -Force -Path $userData, $cache"
            ),
            1,
        )

    def test_launch_argument_json_is_scalar_and_strict(self) -> None:
        self.assertIn("[string]$LaunchArgumentJson", self.source)
        self.assertIn(
            "$PSBoundParameters.ContainsKey('LaunchArgumentJson')", self.source
        )
        self.assertIn("LaunchArgumentJson and LaunchArgument are mutually exclusive", self.source)
        self.assertIn("ConvertFrom-Json -InputObject $LaunchArgumentJson -NoEnumerate", self.source)
        self.assertIn("$decodedLaunchArgument -isnot [string]", self.source)

    def test_omitted_path_overrides_keep_their_defaults(self) -> None:
        self.assertIn(
            "$PSBoundParameters.ContainsKey($pathOverride.Name)", self.source
        )
        self.assertNotIn("$null -ne $pathOverride.Value", self.source)

    def test_exact_sdk_paths_can_be_selected_without_mutating_a_sibling(self) -> None:
        self.assertIn("[IO.Path]::GetFullPath($SdkRepo)", self.source)
        self.assertIn("[IO.Path]::GetFullPath($SdkInstall)", self.source)
        self.assertIn("Join-Path $SdkRepo 'out\\install\\win-amd64'", self.source)
        self.assertIn("git -C $SdkRepo rev-parse HEAD", self.source)


if __name__ == "__main__":
    unittest.main()
