from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run-native-renderer-coverage.ps1"
DIGEST = "2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303"
FIXTURE_0002_SHA = "a1bcefa50427ec719fe4d5721cb9438ee3f44ec7c09db48fdc73c3d326e9d684"


@unittest.skipUnless(shutil.which("pwsh"), "PowerShell is required")
class RunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.title = self.root / "title"
        self.sdk = self.root / "sdk"
        self.title.mkdir()
        self.sdk.mkdir()
        self._git_init(self.title)
        self._git_init(self.sdk)

        fixture = self.title / "config" / "native_renderer_fixture_0001.toml"
        fixture.parent.mkdir()
        fixture.write_bytes((ROOT / "config" / "native_renderer_fixture_0001.toml").read_bytes())
        (self.title / "config" / "native_renderer_fixture_0002.json").write_bytes(
            (ROOT / "config" / "native_renderer_fixture_0002.json").read_bytes()
        )
        exe = self.title / "out" / "build" / "win-amd64-release" / "rerevved.exe"
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"synthetic executable")
        game = self.title / "game"
        game.mkdir()
        (game / "default.xex").write_bytes((ROOT / "game" / "default.xex").read_bytes())
        (game / "default.xexp").write_bytes((ROOT / "game" / "default.xexp").read_bytes())
        launcher = self.title / "scripts" / "rexglue.ps1"
        launcher.parent.mkdir()
        launcher.write_text(self._mock_launcher(), encoding="ascii")
        (self.title / ".gitignore").write_text("/out/evidence/\n", encoding="ascii")

        (self.sdk / ".gitignore").write_text("/out/\n", encoding="ascii")
        (self.sdk / "README.md").write_text("synthetic SDK\n", encoding="ascii")
        self._git_add_commit(self.sdk)
        sdk_commit = self._git(self.sdk, "rev-parse", "HEAD")
        self._git(self.sdk, "config", "remote.origin.url", "https://example.invalid/synthetic-sdk.git")
        install = self.sdk / "out" / "install" / "win-amd64" / "lib" / "cmake" / "rexglue"
        install.mkdir(parents=True)
        (install / "rexglueConfig.cmake").write_text(
            'set(REXGLUE_VERSION_STRING "0.11.0-dev.g37dd3f3")\n'
            f'set(REXGLUE_GIT_REVISION "{sdk_commit}")\n'
            'set(REXGLUE_GIT_DIRTY "clean")\n'
            'set(REXGLUE_BUILD_PLATFORM "win-amd64")\n',
            encoding="ascii",
        )
        (self.title / "rexglue-sdk.lock.json").write_text(
            '{"repository":"https://example.invalid/synthetic-sdk","commit":"%s",'
            '"version":"0.11.0-dev.g37dd3f3","dirty":"clean","platform":"win-amd64"}\n'
            % sdk_commit,
            encoding="ascii",
        )
        self._git_add_commit(self.title)
        self.title_commit = self._git(self.title, "rev-parse", "HEAD")
        self.fixture = fixture
        self.fixture_sha = hashlib.sha256(fixture.read_bytes()).hexdigest()
        self.exe_sha = hashlib.sha256(exe.read_bytes()).hexdigest()
        self.base = [
            "-RunId", "NRD-RUN-20260829-0001",
            "-FixtureId", "NRD-FIX-0001", "-TransitionId", "NRD-TRANS-0001",
            "-Fixture", "config/native_renderer_fixture_0001.toml",
            "-FixtureSha256", self.fixture_sha,
            "-InputDigest", DIGEST, "-TitleCommit", self.title_commit,
            "-ExecutableSha256", self.exe_sha,
            "-TitleRepo", str(self.title), "-SdkRepo", str(self.sdk),
            "-SdkInstall", str(self.sdk / "out" / "install" / "win-amd64"),
            "-ExpectedMark", "settled-menu", "-ExpectedScreenshot", "menu.png",
            "-StopCondition", "settled main menu", "-StartInvariant", "process absent",
            "-AuthorizedSkipBoundary", "intro movie only after owner readiness",
            "-OutputWidth", "1920", "-OutputHeight", "1080", "-ResolutionScale", "1",
            "-WindowMode", "windowed", "-CombatSpeed", "normal",
            "-OsBuild", "10.0.26100.0", "-GpuName", "Synthetic GPU",
            "-GpuVendorId", "0x10de", "-GpuDeviceId", "0x1234",
            "-DriverVersion", "1.2.3", "-D3DFeatureLevel", "12_1",
        ]

    @staticmethod
    def _mock_launcher() -> str:
        return r'''param(
  [string]$Stage, [switch]$Interactive, [string]$UserDataRoot,
  [string]$CacheRoot, [string]$LogPath, [string]$SdkRepo,
  [string]$SdkInstall, [string]$LaunchArgumentJson
)
$LaunchArgument = ConvertFrom-Json -InputObject $LaunchArgumentJson -NoEnumerate
if (($LaunchArgument -join " ") -match 'NRD-RUN-20260829-0002') { exit 7 }
$runRoot = Split-Path -Parent $LogPath
[IO.File]::WriteAllText($LogPath, "NRD-COVERAGE-BEGIN`nmock`nNRD-COVERAGE-END`n", [Text.Encoding]::ASCII)
$observer = Join-Path $runRoot 'observer'
$payload = [ordered]@{
  sdk_repo = $SdkRepo
  sdk_install_root = $SdkInstall
  launch_arguments = @($LaunchArgument)
}
[IO.File]::WriteAllText((Join-Path $observer 'coverage.json'), ($payload | ConvertTo-Json -Compress), [Text.Encoding]::ASCII)
$screenshots = Join-Path $runRoot 'screenshots'
$pngBytes = [byte[]](0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
if (($LaunchArgument -join " ") -match 'NRD-RUN-20260829-0003') {
  $pngBytes = [byte[]](0x42, 0x4d, 0, 0, 0, 0, 0, 0)
}
[IO.File]::WriteAllBytes((Join-Path $screenshots 'menu.png'), $pngBytes)
exit 0
'''

    @staticmethod
    def _git(repo: Path, *args: str) -> str:
        return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()

    def _git_init(self, repo: Path) -> None:
        subprocess.check_call(["git", "init", "--quiet", str(repo)])
        self._git(repo, "config", "user.email", "tests@example.invalid")
        self._git(repo, "config", "user.name", "Synthetic Tests")

    @staticmethod
    def _git_add_commit(repo: Path) -> None:
        subprocess.check_call(["git", "-C", str(repo), "add", "."])
        subprocess.check_call(["git", "-C", str(repo), "commit", "--quiet", "-m", "fixture"])

    def tearDown(self) -> None:
        self.temp.cleanup()

    def invoke(self, *extra: str, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["pwsh", "-NoProfile", "-NonInteractive", "-File", str(RUNNER), *self.base, *extra],
            text=True,
            input=input_text,
            capture_output=True,
        )

    def invoke_override(self, name: str, value: str) -> subprocess.CompletedProcess[str]:
        args = list(self.base)
        index = args.index(name)
        args[index + 1] = value
        return subprocess.run(
            ["pwsh", "-NoProfile", "-NonInteractive", "-File", str(RUNNER), *args],
            text=True,
            capture_output=True,
        )

    def invoke_parameters(
        self, overrides: dict[str, str | list[str]]
    ) -> subprocess.CompletedProcess[str]:
        parameters: dict[str, str | list[str]] = {
            self.base[index].removeprefix("-"): self.base[index + 1]
            for index in range(0, len(self.base), 2)
        }
        parameters.update(overrides)

        def literal(value: str) -> str:
            return "'" + value.replace("'", "''") + "'"

        entries = []
        for name, value in parameters.items():
            rendered = (
                "@(" + ",".join(literal(item) for item in value) + ")"
                if isinstance(value, list)
                else literal(value)
            )
            entries.append(f"{name}={rendered}")
        command = (
            "$parameters=@{" + ";".join(entries) + "}; & "
            + literal(str(RUNNER))
            + " @parameters"
        )
        return subprocess.run(
            ["pwsh", "-NoProfile", "-NonInteractive", "-Command", command],
            text=True,
            capture_output=True,
        )

    def invoke_run(
        self,
        *extra: str,
        review: tuple[str, str, str] | None = ("yes", "settled-menu", "none"),
        respond_prelaunch: bool = True,
        stop_confirmation: str = "exact",
    ) -> subprocess.CompletedProcess[str]:
        command = [
            "pwsh", "-NoProfile", "-NonInteractive", "-File", str(RUNNER),
            *self.base, *extra, "-Run", "-OwnerReady", "-OverlaysClosed",
        ]
        process = subprocess.Popen(
            command,
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
        assert process.stdin is not None
        assert process.stdout is not None
        output: list[str] = []
        for line in process.stdout:
            output.append(line)
            prefix = "review-channel: echo "
            if not line.startswith(prefix):
                continue
            challenge = line.removeprefix(prefix).strip()
            if "-pre-launch-" in challenge:
                if respond_prelaunch:
                    process.stdin.write(challenge + "\n")
                    process.stdin.flush()
                else:
                    process.stdin.close()
            elif "-post-exit-" in challenge:
                if review is None:
                    process.stdin.close()
                else:
                    run_id = command[command.index("-RunId") + 1]
                    stop_answer = stop_confirmation
                    if stop_answer == "exact":
                        stop_answer = (
                            f"{run_id}:closed-immediately-after-planned-capture-"
                            "without-entering-another-transition"
                        )
                    process.stdin.write(challenge + "\n")
                    process.stdin.write(stop_answer + "\n")
                    process.stdin.write("\n".join(review) + "\n")
                    process.stdin.flush()
        returncode = process.wait()
        if not process.stdin.closed:
            process.stdin.close()
        process.stdout.close()
        return subprocess.CompletedProcess(command, returncode, "".join(output), "")

    def test_plan_is_default_and_creates_nothing(self) -> None:
        before = set(self.root.rglob("*"))
        result = subprocess.run(
            ["pwsh", "-NoProfile", "-NonInteractive", "-File", str(RUNNER), *self.base],
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("plan: valid", result.stdout)
        self.assertEqual(before, set(self.root.rglob("*")))
        plan = json.loads(result.stdout[result.stdout.index("{"):])
        self.assertEqual(plan["schema"], "rerevved.native_renderer.plan.v2")
        self.assertEqual(plan["run_root"], "out/evidence/native-renderer-d3d/NRD-RUN-20260829-0001")
        self.assertEqual(plan["launcher"], "scripts/rexglue.ps1")
        self.assertEqual(plan["metadata"]["renderer_config"]["guest_width"], 1280)
        self.assertIn("-LaunchArgumentJson", plan["child_arguments"])
        self.assertNotIn(str(self.root), result.stdout)

    def test_plan_reports_complete_identity(self) -> None:
        result = self.invoke("-Repeat", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fixture=NRD-FIX-0001", result.stdout)
        self.assertIn("renderer=xenos/rov", result.stdout)
        self.assertIn("marks=settled-menu", result.stdout)

    def test_locked_new_game_plans_are_exact_and_read_only(self) -> None:
        routes = {
            "NRD-TRANS-0002": {
                "ExpectedMark": ["new-game-setup-romans"],
                "ExpectedScreenshot": [
                    "main-menu.png", "single-player-menu.png",
                    "difficulty-warlord.png", "civilization-romans.png",
                ],
                "StopCondition": [
                    "choose Single Player, New Game, Warlord, and highlight Romans without confirming",
                    "close immediately after civilization-romans.png capture without confirming Romans, entering gameplay, or saving",
                ],
            },
            "NRD-TRANS-0003": {
                "ExpectedMark": ["first-settled-human-turn-map"],
                "ExpectedScreenshot": [
                    "main-menu.png", "single-player-menu.png",
                    "difficulty-warlord.png", "civilization-romans.png",
                    "first-settled-human-turn-map.png",
                ],
                "StopCondition": [
                    "choose Single Player, New Game, Warlord, and confirm Romans",
                    "close immediately after first-settled-human-turn-map.png capture before gameplay input or saving",
                ],
            },
        }
        for transition, route in routes.items():
            with self.subTest(transition=transition):
                before = set(self.root.rglob("*"))
                result = self.invoke_parameters(
                    {
                        "FixtureId": "NRD-FIX-0002",
                        "TransitionId": transition,
                        "Fixture": "config/native_renderer_fixture_0002.json",
                        "FixtureSha256": FIXTURE_0002_SHA,
                        "ExpectedMark": route["ExpectedMark"],
                        "ExpectedScreenshot": route["ExpectedScreenshot"],
                        "StopCondition": route["StopCondition"],
                        "StartInvariant": "process absent",
                        "AuthorizedSkipBoundary": (
                            "boot intro movie only after owner readiness; "
                            "do not skip any setup panel"
                        ),
                    }
                )
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertEqual(before, set(self.root.rglob("*")))
                plan = json.loads(result.stdout[result.stdout.index("{"):])
                self.assertEqual(plan["metadata"]["fixture_id"], "NRD-FIX-0002")
                self.assertEqual(plan["metadata"]["transition_id"], transition)
                self.assertEqual(
                    plan["metadata"]["readiness"]["expected_screenshots"],
                    route["ExpectedScreenshot"],
                )

        altered = dict(routes["NRD-TRANS-0002"])
        altered["StopCondition"] = ["altered route"]
        result = self.invoke_parameters(
            {
                "FixtureId": "NRD-FIX-0002",
                "TransitionId": "NRD-TRANS-0002",
                "Fixture": "config/native_renderer_fixture_0002.json",
                "FixtureSha256": FIXTURE_0002_SHA,
                "ExpectedMark": altered["ExpectedMark"],
                "ExpectedScreenshot": altered["ExpectedScreenshot"],
                "StopCondition": altered["StopCondition"],
                "StartInvariant": "process absent",
                "AuthorizedSkipBoundary": (
                    "boot intro movie only after owner readiness; do not skip any setup panel"
                ),
            }
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("owner-locked route", result.stderr + result.stdout)

    def test_child_exit_cannot_bypass_complete_manifest(self) -> None:
        result = self.invoke_run()
        self.assertEqual(result.returncode, 0, result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        manifest = json.loads((run_root / "run.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["checkpoint"], "complete")
        self.assertEqual(manifest["command_result"], {"exit_code": 0, "classification": "accepted"})
        self.assertEqual(manifest["operator_review"]["reached_marks"], ["settled-menu"])
        self.assertTrue(manifest["operator_review"]["overlays_remained_closed"])
        self.assertFalse(manifest["title_dirty"])
        self.assertFalse(manifest["sdk_dirty"])
        self.assertEqual(manifest["artifacts"][0]["path"], "observer/coverage.json")
        self.assertEqual(manifest["screenshots"][0]["path"], "screenshots/menu.png")
        required = {
            "fixture_id", "fixture_staged_path", "base_xex_sha256",
            "title_update_sha256", "renderer_config",
            "host_graphics", "readiness", "timing", "operator_review", "save_directory",
            "user_data_directory", "cache_directory", "screenshots", "saves",
            "title_dirty", "sdk_dirty",
        }
        self.assertTrue(required.issubset(manifest))
        self.assertIsNone(manifest["fixture_staged_path"])

        observed = json.loads((run_root / "observer" / "coverage.json").read_text(encoding="ascii"))
        self.assertEqual(Path(observed["sdk_repo"]).resolve(), self.sdk.resolve())
        self.assertEqual(Path(observed["sdk_install_root"]).resolve(), (self.sdk / "out" / "install" / "win-amd64").resolve())
        args = set(observed["launch_arguments"])
        self.assertIn("--gpu_plugin=xenos", args)
        self.assertIn("--render_target_path_d3d12=rov", args)
        self.assertIn("--video_mode_width=1280", args)
        self.assertIn("--video_mode_height=720", args)
        self.assertIn("--window_width=1920", args)
        self.assertIn("--window_height=1080", args)
        self.assertIn("--resolution_scale=1", args)
        self.assertIn("--fullscreen=false", args)
        self.assertIn("--combat_speed=normal", args)
        renderer = manifest["renderer_config"]
        self.assertEqual(renderer["guest_width"], 1280)
        self.assertEqual(renderer["guest_height"], 720)
        self.assertEqual(renderer["output_width"], 1920)
        self.assertEqual(renderer["output_height"], 1080)
        self.assertEqual(renderer["resolution_scale"], 1)
        self.assertEqual(renderer["window_mode"], "windowed")
        self.assertEqual(renderer["combat_speed"], "normal")
        stop_instruction = "operator-stop: planned stop condition: settled main menu"
        self.assertIn(stop_instruction, result.stdout)
        self.assertIn(
            "operator-stop: after the planned capture, close immediately without "
            "entering another transition",
            result.stdout,
        )
        self.assertLess(
            result.stdout.index(stop_instruction),
            result.stdout.index("review-channel: echo NRD-REVIEW-pre-launch-"),
        )

    def test_nonzero_child_exit_is_recorded_then_rejected(self) -> None:
        args = list(self.base)
        index = args.index("NRD-RUN-20260829-0001")
        args[index] = "NRD-RUN-20260829-0002"
        original = self.base
        self.base = args
        try:
            result = self.invoke_run()
        finally:
            self.base = original
        self.assertNotEqual(result.returncode, 0)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0002"
        manifest = json.loads((run_root / "run.json").read_text())
        self.assertEqual(manifest["checkpoint"], "rejected")
        self.assertEqual(manifest["command_result"]["exit_code"], 7)
        self.assertEqual(manifest["command_result"]["classification"], "launch-failed")

    def test_non_png_screenshot_is_recorded_then_rejected(self) -> None:
        args = list(self.base)
        index = args.index("NRD-RUN-20260829-0001")
        args[index] = "NRD-RUN-20260829-0003"
        original = self.base
        self.base = args
        try:
            result = self.invoke_run()
        finally:
            self.base = original
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected screenshot menu.png is not a PNG file", result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0003"
        manifest = json.loads((run_root / "run.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["checkpoint"], "rejected")

    def test_warm_cache_requires_and_copies_exact_seed(self) -> None:
        result = self.invoke("-CacheClass", "warm")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("WarmCacheSeed", result.stderr + result.stdout)
        seed = self.root / "warm-seed"
        seed.mkdir()
        (seed / "pipeline.bin").write_bytes(b"seed")
        record = "pipeline.bin\n" + hashlib.sha256(b"seed").hexdigest() + "\n"
        digest = hashlib.sha256(record.encode()).hexdigest()
        result = self.invoke(
            "-CacheClass", "warm", "-WarmCacheSeed", str(seed),
            "-WarmCacheSeedSha256", "0" * 64,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("warm cache seed SHA-256", result.stderr + result.stdout)
        result = self.invoke_run(
            "-CacheClass", "warm", "-WarmCacheSeed", str(seed),
            "-WarmCacheSeedSha256", digest,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        manifest = json.loads((run_root / "run.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["cache_seed_sha256"], digest)
        self.assertEqual((run_root / "cache" / "warm" / "pipeline.bin").read_bytes(), b"seed")

    def test_review_channel_must_be_live_before_launch(self) -> None:
        result = self.invoke_run(respond_prelaunch=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pre-launch challenge failed", result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        self.assertFalse(run_root.exists())

    def test_review_channel_requires_post_exit_freshness(self) -> None:
        result = self.invoke_run(review=None)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("post-exit challenge failed", result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        manifest = json.loads((run_root / "run.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["checkpoint"], "rejected")
        self.assertEqual(manifest["command_result"]["classification"], "process-exited-zero")

    def test_stale_prequeued_review_cannot_launch(self) -> None:
        result = self.invoke(
            "-Run", "-OwnerReady", "-OverlaysClosed",
            input_text="yes\nsettled-menu\nnone\n",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pre-launch challenge failed", result.stderr + result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        self.assertFalse(run_root.exists())

    def test_blank_review_answers_are_rejected(self) -> None:
        result = self.invoke_run(review=("yes", "", ""))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checkpoint review answer was blank", result.stdout)
        self.assertIn("unexpected-error review answer was blank", result.stdout)
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        manifest = json.loads((run_root / "run.json").read_text(encoding="ascii"))
        self.assertEqual(manifest["checkpoint"], "rejected")

    def test_planned_stop_confirmation_rejects_no(self) -> None:
        result = self.invoke_run(stop_confirmation="no")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("operator did not close immediately after the planned capture", result.stdout)

    def test_planned_stop_confirmation_rejects_blank(self) -> None:
        result = self.invoke_run(stop_confirmation="")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("planned-stop confirmation was blank", result.stdout)

    def test_planned_stop_confirmation_rejects_stale_input(self) -> None:
        result = self.invoke_run(
            stop_confirmation=(
                "NRD-RUN-20260829-0000:closed-immediately-after-planned-capture-"
                "without-entering-another-transition"
            )
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("planned-stop confirmation was stale, malformed, or mismatched", result.stdout)

    def test_planned_stop_confirmation_rejects_malformed_input(self) -> None:
        result = self.invoke_run(stop_confirmation="yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("planned-stop confirmation was stale, malformed, or mismatched", result.stdout)

    def test_planned_stop_confirmation_rejects_mismatch(self) -> None:
        result = self.invoke_run(
            stop_confirmation=(
                "NRD-RUN-20260829-0001:closed-immediately-after-planned-capture-"
                "without-entering-another-transition-extra"
            )
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("planned-stop confirmation was stale, malformed, or mismatched", result.stdout)

    def test_rejection_gates(self) -> None:
        cases = [
            (("-RunId", "NRD-RUN-bad"), "RunId"),
            (("-FixtureId", "NRD-FIX-0007"), "FixtureId"),
            (("-TransitionId", "NRD-TRANS-0012"), "TransitionId"),
            (("-InputDigest", "0" * 64), "InputDigest"),
            (("-FixtureSha256", "0" * 64), "fixture SHA"),
            (("-TitleCommit", "0" * 40), "title commit"),
            (("-ExecutableSha256", "0" * 64), "executable SHA"),
            (("-ExpectedMark", "settled,menu"), "ExpectedMark"),
            (("-ExpectedScreenshot", "menu.bmp"), "screenshot extension"),
            (["-OwnerReady"], "OwnerReady"),
        ]
        for extra, label in cases:
            result = self.invoke_override(*extra) if isinstance(extra, tuple) else self.invoke(*extra)
            self.assertNotEqual(result.returncode, 0, label)
            self.assertIn("coverage blocked", result.stderr + result.stdout, label)

    def test_existing_run_path_escape_and_dirty_tree_are_rejected(self) -> None:
        run_root = self.title / "out" / "evidence" / "native-renderer-d3d" / "NRD-RUN-20260829-0001"
        run_root.mkdir(parents=True)
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already exists", result.stderr + result.stdout)
        shutil.rmtree(run_root)
        outside = self.root / "outside.toml"
        outside.write_text("outside\n", encoding="ascii")
        result = self.invoke_override("-Fixture", str(outside))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("contained", result.stderr + result.stdout)
        result = self.invoke_override("-SdkInstall", str(outside))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("contained", result.stderr + result.stdout)
        (self.title / "dirty.txt").write_text("dirty", encoding="ascii")
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not clean", result.stderr + result.stdout)

    def test_launcher_is_exact_tracked_child_only(self) -> None:
        source = RUNNER.read_text(encoding="ascii")
        self.assertNotRegex(source, r"\[switch\]\$Plan")
        self.assertIn("[string]$TitleCommit, [string]$ExecutableSha256", source)
        self.assertNotIn("RexglueScript", source)
        self.assertIn("Join-Path $TitleRepo 'scripts\\rexglue.ps1'", source)
        self.assertIn("'ls-files', '--error-unmatch', 'scripts/rexglue.ps1'", source)
        self.assertIn("'-NoProfile', '-NonInteractive', '-File', $rexglueScript", source)
        self.assertIn("$launchOutput = @(& $pwshPath @childArguments", source)
        self.assertIn("'-Stage', 'Launch', '-Interactive', '-SdkRepo', $SdkRepo", source)
        self.assertIn("'-SdkInstall', $SdkInstall", source)
        self.assertIn("'-LaunchArgumentJson', $launchArgumentJson", source)
        self.assertNotRegex(source, r"\s-Command(?:\s|$)")
        self.assertNotIn("REREVVED_NRD_", source)
        self.assertNotIn("[string]$CacheRoot", source)
        self.assertNotIn("[string[]]$LaunchArgument", source)
        self.assertNotIn("& $rexglueScript -Stage Launch", source)
        self.assertEqual(source.count("$launchOutput = @(& $pwshPath @childArguments"), 1)
        self.assertIn("'user-data/save5.sve'", source)
        self.assertIn("NRD-FIX-0002 prohibits save output", source)
        self.assertIn("Copy-Item -LiteralPath $fixturePath -Destination $fixtureStagedPath", source)
        self.assertIn("'staged fixture changed during the run'", source)
        self.assertIn("$relativeSave -cne $fixtureStagedRelative", source)
        self.assertEqual(source.count("Confirm-ReviewChannel 'pre-launch' $RunId"), 1)
        self.assertEqual(source.count("Confirm-ReviewChannel 'post-exit' $RunId"), 1)
        self.assertEqual(source.count("$expectedStopConfirmation ="), 1)
        self.assertIn("$stopAnswer -cne $expectedStopConfirmation", source)
        self.assertLess(
            source.index("Confirm-ReviewChannel 'pre-launch' $RunId"),
            source.index("New-Item -ItemType Directory -Path $runRoot"),
        )
        self.assertLess(
            source.index("operator-stop: planned stop condition:"),
            source.index("Confirm-ReviewChannel 'pre-launch' $RunId"),
        )
        self.assertGreater(
            source.index("Confirm-ReviewChannel 'post-exit' $RunId"),
            source.index("$manifest['command_result']['exit_code'] = $launchExitCode"),
        )
        for flag in (
            "--gpu_plugin=", "--render_target_path_d3d12=",
            "native_renderer_coverage_run=", "native_renderer_coverage_transition=",
            "native_renderer_coverage_input_digest", "native_renderer_coverage_output=observer",
            "--video_mode_width=1280", "--video_mode_height=720", "--window_width=",
            "--window_height=", "--resolution_scale=", "--fullscreen=",
        ):
            self.assertIn(flag, source)


if __name__ == "__main__":
    unittest.main()
