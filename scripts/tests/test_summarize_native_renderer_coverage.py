from __future__ import annotations

import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "summarize-native-renderer-coverage.py"
SPEC = importlib.util.spec_from_file_location("native_coverage_summary", SCRIPT)
assert SPEC and SPEC.loader
summary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summary)

DIGEST = "2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303"
BASE_XEX_DIGEST = "b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd"
TITLE_UPDATE_DIGEST = "c1fc6149a63550987d991efdbb80e3697845a9a49d3f2ec180ea9817db8d12d4"
TITLE_COMMIT = "b" * 40
SDK_COMMIT = "37dd3f38d2fa6501ca114578c8176a0633e070fc"
SDK_VERSION = "0.11.0-dev.g37dd3f3"
CONFIG_TEXT = (
    "xenos_enabled=true\nrov_enabled=true\nguest_width=1280\nguest_height=720\n"
    "output_width=1920\noutput_height=1080\nresolution_scale=1\n"
    "window_mode=windowed\ncombat_speed=normal\n"
)
CONFIG_DIGEST = hashlib.sha256(CONFIG_TEXT.encode()).hexdigest()


def _log_line(message: str, level: str = "info") -> str:
    return f"[2026-08-29 12:00:00.000] [{level}] [rexglue] [t1] {message}"


def _snapshot(index: int, *, checkpoint: bool = False, accepted: bool = False, segment: int = 0) -> dict:
    row = {
        "frame_sequence": segment if accepted else 0,
        "valid_fields": 0,
        "gameplay_active": False,
        "interface_update": False,
        "active_player": -1,
        "human_player_mask": 0,
        "turn_owner_known": False,
        "human_turn": False,
        "available": False,
        "civilization": 0,
        "era": 0,
        "year": 0,
        "turn": 0,
        "accepted": accepted,
    }
    if checkpoint:
        row.update(mark=index, segment=segment)
    else:
        row["index"] = index
    return row


def _coverage(
    *,
    run_id: str = "NRD-RUN-20260829-0001",
    transition: str = "NRD-TRANS-0001",
    digest: str = DIGEST,
    marks: int = 0,
    exit_class: str = "guest_complete",
) -> dict:
    counters = [
        {
            "segment": segment,
            "operation": 0,
            "operation_id": "NRD-OP-0002",
            "runtime_join_key": "d3d:0x826A3568",
            "contract_id": "NRD-CONTRACT-0001",
            "domain": domain,
            "domain_id": "primitive-4" if domain == 0 else "unknown",
            "site": site,
            "site_address": 0x82303E3C if site == 0 else 0x82303E8C,
            "count": 0,
        }
        for segment in range(8)
        for domain in range(2)
        for site in range(2)
    ]
    accepted_segments = {0, 7, *(range(1, marks + 1))}
    return {
        "schema": "rerevved.native_renderer.coverage.v1",
        "run_id": run_id,
        "transition_id": transition,
        "input_digest": digest,
        "xenos_enabled": True,
        "rov_enabled": True,
        "observer_byte_budget": 3456,
        "operation_metadata": {
            "operation_id": "NRD-OP-0002",
            "runtime_join_key": "d3d:0x826A3568",
            "roles": ["wrapper", "lowering-boundary"],
            "contract_ids": ["NRD-CONTRACT-0001"],
            "hook_sites": [
                {"address": 0x82303E3C, "phase": "value", "discriminator": "primitive-4"},
                {"address": 0x82303E8C, "phase": "value", "discriminator": "primitive-4"},
            ],
            "registers": [],
            "value_domains": [
                {"id": "primitive-4", "value": 4, "selection": "site-fixed"},
                {"id": "unknown", "value": None, "selection": "unmapped-input"},
            ],
        },
        "transition_attribution_valid": True,
        "exit_class": exit_class,
        "lifetime_evaluation": "evaluated" if exit_class == "guest_complete" else "not-evaluated",
        "complete": True,
        "incomplete": False,
        "recovered_incomplete": False,
        "counters": counters,
        "counter_failures": {"saturated": 0, "rejected_in_flight": 0},
        "segments": [_snapshot(i, accepted=i in accepted_segments, segment=i) for i in range(8)],
        "checkpoints": [
            _snapshot(i, checkpoint=True, accepted=i < marks, segment=i + 1 if i < marks else 0)
            for i in range(6)
        ],
        "anomalies": [],
    }


def _write_bundle(
    root: Path,
    *,
    run_id: str = "NRD-RUN-20260829-0001",
    repeat: int = 1,
    marks: list[str] | None = None,
    exit_class: str = "guest_complete",
    fixture_sha256: str = DIGEST,
    cache_class: str = "cold",
) -> None:
    marks = marks or []
    for directory in ("observer", "screenshots", "shaders", "user-data", f"cache/{cache_class}"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    log = root / "coverage.log"
    log.write_text(
        _log_line("NRD-COVERAGE-BEGIN") + "\n" + _log_line("NRD-COVERAGE-END") + "\n",
        encoding="ascii",
    )
    screenshot = root / "screenshots" / "stable.png"
    screenshot.write_bytes(summary.PNG_SIGNATURE)
    coverage = _coverage(run_id=run_id, marks=len(marks), exit_class=exit_class)
    coverage_path = root / "observer" / "coverage.json"
    coverage_path.write_text(json.dumps(coverage, sort_keys=True), encoding="ascii")
    run = {
        "schema": "rerevved.native_renderer.run.v2",
        "run_id": run_id,
        "fixture_id": "NRD-FIX-0001",
        "fixture": "config/native_renderer_fixture_0001.toml",
        "fixture_sha256": fixture_sha256,
        "fixture_staged_path": None,
        "transition_id": "NRD-TRANS-0001",
        "input_digest": DIGEST,
        "expected_marks": marks,
        "repeat": repeat,
        "cache_class": cache_class,
        "cache_seed_sha256": None if cache_class == "cold" else "c" * 64,
        "title_commit": TITLE_COMMIT,
        "title_dirty": False,
        "sdk_commit": SDK_COMMIT,
        "sdk_dirty": False,
        "sdk_version": SDK_VERSION,
        "executable": "out/build/win-amd64-release/rerevved.exe",
        "executable_sha256": DIGEST,
        "base_xex": "game/default.xex",
        "base_xex_sha256": BASE_XEX_DIGEST,
        "title_update": "game/default.xexp",
        "title_update_sha256": TITLE_UPDATE_DIGEST,
        "xenos_enabled": True,
        "rov_enabled": True,
        "renderer_config": {
            "guest_width": 1280,
            "guest_height": 720,
            "output_width": 1920,
            "output_height": 1080,
            "resolution_scale": 1,
            "window_mode": "windowed",
            "combat_speed": "normal",
            "xenos_enabled": True,
            "rov_enabled": True,
            "configuration_digest": CONFIG_DIGEST,
        },
        "host_graphics": {
            "os_build": "Windows 11 10.0.26100",
            "gpu_name": "Synthetic GPU",
            "gpu_vendor_id": "0x1234",
            "gpu_device_id": "0x5678",
            "driver_version": "1.2.3",
            "d3d_feature_level": "12_1",
        },
        "readiness": {
            "owner_ready": True,
            "overlay_policy": "closed",
            "overlays_closed_before_launch": True,
            "start_invariant": "fixture ready",
            "authorized_skip_boundary": "intro only",
            "expected_screenshots": ["stable.png"],
            "stop_conditions": ["natural completion"],
        },
        "output_root": ".",
        "output_directory": "observer",
        "screenshot_directory": "screenshots",
        "shader_directory": "shaders",
        "user_data_directory": "user-data",
        "cache_directory": f"cache/{cache_class}",
        "save_directory": "user-data",
        "checkpoint": "complete",
        "timing": {"started_utc": "2026-08-29T12:00:00Z", "ended_utc": "2026-08-29T12:10:00Z"},
        "command_result": {"exit_code": 0, "classification": "accepted"},
        "operator_review": {"overlays_remained_closed": True, "reached_marks": marks, "unexpected_errors": []},
        "log": {"path": "coverage.log", "sha256": hashlib.sha256(log.read_bytes()).hexdigest()},
        "artifacts": [{"path": "observer/coverage.json", "sha256": hashlib.sha256(coverage_path.read_bytes()).hexdigest()}],
        "screenshots": [{"path": "screenshots/stable.png", "sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest()}],
        "saves": [],
    }
    (root / "run.json").write_text(json.dumps(run, sort_keys=True), encoding="ascii")


def _rewrite_coverage(root: Path, coverage: dict) -> None:
    path = root / "observer" / "coverage.json"
    path.write_text(json.dumps(coverage, sort_keys=True), encoding="ascii")
    run_path = root / "run.json"
    run = json.loads(run_path.read_text(encoding="ascii"))
    run["artifacts"][0]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")


def _rewrite_log(root: Path, text: str) -> None:
    lines = text.splitlines()
    rendered = []
    for line in lines:
        level = "info" if line in {"NRD-COVERAGE-BEGIN", "NRD-COVERAGE-END"} else "error"
        rendered.append(_log_line(line, level))
    _rewrite_raw_log(root, "\n".join(rendered) + "\n")


def _rewrite_raw_log(root: Path, text: str) -> None:
    log = root / "coverage.log"
    log.write_text(text, encoding="ascii")
    run_path = root / "run.json"
    run = json.loads(run_path.read_text(encoding="ascii"))
    run["log"]["sha256"] = hashlib.sha256(log.read_bytes()).hexdigest()
    run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")


def _rewrite_new_game_bundle(root: Path, transition: str) -> None:
    route = summary.LOCKED_NEW_GAME_ROUTES[transition]
    run_path = root / "run.json"
    run = json.loads(run_path.read_text(encoding="ascii"))
    run.update(
        fixture_id="NRD-FIX-0002",
        fixture="config/native_renderer_fixture_0002.json",
        fixture_sha256=summary.ACCEPTED_FIXTURE_SHA256["NRD-FIX-0002"],
        transition_id=transition,
        expected_marks=route["expected_marks"],
    )
    run["readiness"].update(
        start_invariant="process absent",
        authorized_skip_boundary=(
            "boot intro movie only after owner readiness; do not skip any setup panel"
        ),
        expected_screenshots=route["expected_screenshots"],
        stop_conditions=route["stop_conditions"],
    )
    run["operator_review"]["reached_marks"] = route["expected_marks"]
    for path in (root / "screenshots").iterdir():
        path.unlink()
    run["screenshots"] = []
    for name in route["expected_screenshots"]:
        screenshot = root / "screenshots" / name
        screenshot.write_bytes(summary.PNG_SIGNATURE)
        run["screenshots"].append(
            {
                "path": f"screenshots/{name}",
                "sha256": hashlib.sha256(screenshot.read_bytes()).hexdigest(),
            }
        )
    run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")

    coverage = _coverage(
        transition=transition,
        marks=1,
        exit_class="window_close",
    )
    expected_state = {
        "NRD-TRANS-0002": {
            "valid_fields": 5,
            "gameplay_active": False,
            "interface_update": True,
            "active_player": -1,
            "human_player_mask": 0,
            "turn_owner_known": False,
            "human_turn": False,
            "available": False,
            "civilization": -1,
            "era": -1,
            "year": -(1 << 31),
            "turn": -1,
        },
        "NRD-TRANS-0003": {
            "valid_fields": 127,
            "gameplay_active": True,
            "interface_update": True,
            "active_player": 0,
            "human_player_mask": 1,
            "turn_owner_known": True,
            "human_turn": True,
            "available": True,
            "civilization": 0,
            "era": 0,
            "year": -4000,
            "turn": 0,
        },
    }[transition]
    coverage["checkpoints"][0].update(expected_state)
    coverage["segments"][1].update(expected_state)
    coverage["segments"][7].update(expected_state)
    _rewrite_coverage(root, coverage)


class SummaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        _write_bundle(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_deterministic_zero_summary_has_exact_contract_rows(self) -> None:
        first = summary.summarize_run(self.root)
        second = summary.summarize_run(self.root)
        self.assertEqual(summary.deterministic_json(first), summary.deterministic_json(second))
        self.assertEqual(first["count"], 0)
        self.assertEqual(first["matrix"], {})
        self.assertEqual(first["zero_absence_claim"], "blocked")
        self.assertEqual(first["lifetime_claim"], "not-applicable")
        self.assertEqual([row["outcome"] for row in first["operations"]], ["blocked", "blocked"])
        self.assertEqual(first["operations"][0]["discriminator"], "primitive-4")

    def test_matrix_contains_only_observed_rows_and_names_checkpoints(self) -> None:
        coverage = json.loads((self.root / "observer" / "coverage.json").read_text())
        coverage["counters"][0]["count"] = 3
        _rewrite_coverage(self.root, coverage)
        result = summary.summarize_run(self.root)
        self.assertEqual(result["matrix"]["0"]["0"]["0"], [0])
        self.assertNotIn("1", result["matrix"]["0"])
        self.assertEqual(result["operations"][0]["outcome"], "observed")
        self.assertEqual(result["operations"][0]["first_checkpoint"], "start")

    def test_unknown_discriminator_is_blocked_but_preserves_evidence(self) -> None:
        coverage = json.loads((self.root / "observer" / "coverage.json").read_text())
        coverage["counters"][2]["count"] = 1
        _rewrite_coverage(self.root, coverage)
        result = summary.summarize_run(self.root)
        self.assertEqual(result["operations"][0]["outcome"], "blocked")
        self.assertEqual(
            result["operations"][0]["qualification"],
            "site-local partial snapshot; no global absence claim",
        )
        self.assertEqual(result["operations"][1]["outcome"], "blocked")
        self.assertEqual(
            result["operations"][1]["qualification"],
            "unmapped-input discriminator is unsupported",
        )
        self.assertEqual(result["operations"][1]["count"], 1)
        self.assertEqual(result["operations"][1]["first_checkpoint"], "start")

    def test_counter_failure_flags_block(self) -> None:
        for field in ("saturated", "rejected_in_flight"):
            root = self.root / field
            _write_bundle(root)
            coverage = json.loads((root / "observer" / "coverage.json").read_text())
            coverage["counter_failures"][field] = 1
            _rewrite_coverage(root, coverage)
            with self.assertRaises(summary.CoverageError):
                summary.summarize_run(root)

    def test_zero_absence_requires_start_final_and_ordered_marks(self) -> None:
        for segment in (0, 7):
            root = self.root / f"segment-{segment}"
            _write_bundle(root)
            coverage = json.loads((root / "observer" / "coverage.json").read_text())
            coverage["segments"][segment]["accepted"] = False
            _rewrite_coverage(root, coverage)
            with self.assertRaises(summary.CoverageError):
                summary.summarize_run(root)
        root = self.root / "mark-order"
        _write_bundle(root, marks=["one", "two"])
        coverage = json.loads((root / "observer" / "coverage.json").read_text())
        coverage["checkpoints"][1]["segment"] = 1
        _rewrite_coverage(root, coverage)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(root)

    def test_anomaly_ids_and_fields_are_exact(self) -> None:
        coverage = json.loads((self.root / "observer" / "coverage.json").read_text())
        coverage["anomalies"] = [{"id": 9, "name": "invalid_segment", "count": 1}]
        _rewrite_coverage(self.root, coverage)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        coverage["anomalies"][0]["name"] = "wrong"
        _rewrite_coverage(self.root, coverage)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        coverage = _coverage()
        coverage["resources"] = {"created": 1, "used": 0, "released": 0, "active": 0}
        _rewrite_coverage(self.root, coverage)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_checkpoint_anomaly_is_not_a_valid_transition(self) -> None:
        coverage = json.loads((self.root / "observer" / "coverage.json").read_text())
        coverage["anomalies"] = [{"id": 15, "name": "checkpoint_sequence", "count": 1}]
        _rewrite_coverage(self.root, coverage)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_fixed_artifact_allowlist_and_runtime_state_boundary(self) -> None:
        (self.root / "cache" / "cold" / "opaque.bin").write_bytes(b"private")
        summary.summarize_run(self.root)
        (self.root / "unexpected.txt").write_text("x", encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        (self.root / "unexpected.txt").unlink()
        (self.root / "user-data" / "unrecorded.sve").write_bytes(b"save")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        (self.root / "user-data" / "unrecorded.sve").unlink()
        (self.root / "shaders" / "unexpected.dxil").write_bytes(b"shader")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_save_copy_fixture_is_staged_immutable_and_not_an_output(self) -> None:
        staged = self.root / "user-data" / "save5.sve"
        staged.write_bytes(b"immutable fixture")
        fixture_sha = hashlib.sha256(staged.read_bytes()).hexdigest()
        original_sha = summary.ACCEPTED_FIXTURE_SHA256["NRD-FIX-0003"]
        summary.ACCEPTED_FIXTURE_SHA256["NRD-FIX-0003"] = fixture_sha
        self.addCleanup(
            summary.ACCEPTED_FIXTURE_SHA256.__setitem__,
            "NRD-FIX-0003",
            original_sha,
        )
        run_path = self.root / "run.json"
        run = json.loads(run_path.read_text(encoding="ascii"))
        run.update(
            fixture_id="NRD-FIX-0003",
            fixture="out/evidence/fixture/save5.sve",
            fixture_sha256=fixture_sha,
            fixture_staged_path="user-data/save5.sve",
            transition_id="NRD-TRANS-0004",
        )
        run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")
        coverage = json.loads(
            (self.root / "observer" / "coverage.json").read_text(encoding="ascii")
        )
        coverage["transition_id"] = "NRD-TRANS-0004"
        _rewrite_coverage(self.root, coverage)
        summary.summarize_run(self.root)

        run = json.loads(run_path.read_text(encoding="ascii"))
        run["saves"] = [
            {"path": "user-data/save5.sve", "sha256": fixture_sha}
        ]
        run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        run["saves"] = []
        run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")

        staged.write_bytes(b"mutated fixture")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_locked_new_game_routes_and_states_are_exact(self) -> None:
        for transition in ("NRD-TRANS-0002", "NRD-TRANS-0003"):
            root = self.root / transition
            _write_bundle(root)
            _rewrite_new_game_bundle(root, transition)
            self.assertEqual(summary.summarize_run(root)["transition_id"], transition)

            run_path = root / "run.json"
            run = json.loads(run_path.read_text(encoding="ascii"))
            run["readiness"]["stop_conditions"][0] += " altered"
            run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")
            with self.assertRaisesRegex(summary.CoverageError, "owner-locked route"):
                summary.summarize_run(root)

            _rewrite_new_game_bundle(root, transition)
            coverage = json.loads(
                (root / "observer" / "coverage.json").read_text(encoding="ascii")
            )
            coverage["checkpoints"][0]["civilization"] = 7
            coverage["segments"][1]["civilization"] = 7
            _rewrite_coverage(root, coverage)
            with self.assertRaisesRegex(summary.CoverageError, "owner-locked invariant"):
                summary.summarize_run(root)

    def test_locked_new_game_fixture_prohibits_saves(self) -> None:
        _rewrite_new_game_bundle(self.root, "NRD-TRANS-0002")
        save = self.root / "user-data" / "unexpected.sve"
        save.write_bytes(b"save")
        run_path = self.root / "run.json"
        run = json.loads(run_path.read_text(encoding="ascii"))
        run["saves"] = [
            {
                "path": "user-data/unexpected.sve",
                "sha256": hashlib.sha256(save.read_bytes()).hexdigest(),
            }
        ]
        run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")
        with self.assertRaisesRegex(summary.CoverageError, "prohibits save output"):
            summary.summarize_run(self.root)

    def test_new_game_setup_filesystem_diagnostic_bound_is_exact(self) -> None:
        _rewrite_new_game_bundle(self.root, "NRD-TRANS-0002")
        messages = [
            r"ResolvePath(\Device) failed - device not found",
            *(["ResolvePath(UPDATE:\\) failed - device not found"] * 4),
        ]
        _rewrite_log(
            self.root,
            "NRD-COVERAGE-BEGIN\n" + "\n".join(messages) + "\nNRD-COVERAGE-END\n",
        )
        result = summary.summarize_run(self.root)
        self.assertEqual(
            [(row["id"], row["count"]) for row in result["diagnostics"]],
            [("NRD-SDK-FAIL-0010", 1), ("NRD-SDK-FAIL-0011", 4)],
        )

        run = json.loads((self.root / "run.json").read_text(encoding="ascii"))
        for overflow in (
            [messages[0], messages[0]],
            [messages[1]] * 5,
        ):
            _rewrite_log(
                self.root,
                "NRD-COVERAGE-BEGIN\n" + "\n".join(overflow) + "\nNRD-COVERAGE-END\n",
            )
            with self.subTest(overflow=overflow), self.assertRaisesRegex(
                summary.CoverageError, "accepted fixture bound"
            ):
                summary.summarize_run(self.root)

        _rewrite_log(
            self.root,
            "NRD-COVERAGE-BEGIN\n" + messages[0] + "\nNRD-COVERAGE-END\n",
        )
        run = json.loads((self.root / "run.json").read_text(encoding="ascii"))
        with self.assertRaisesRegex(summary.CoverageError, "accepted fixture bound"):
            summary._check_log(
                self.root,
                run["log"],
                {"fixture_id": "NRD-FIX-0002", "transition_id": "NRD-TRANS-0003"},
            )

    def test_screenshot_plan_path_and_hash_are_exact(self) -> None:
        screenshot = self.root / "screenshots" / "stable.png"
        screenshot.write_bytes(summary.PNG_SIGNATURE)
        run_path = self.root / "run.json"
        run = json.loads(run_path.read_text())
        run["readiness"]["expected_screenshots"] = ["stable.png"]
        run["screenshots"] = [{"path": "screenshots/stable.png", "sha256": hashlib.sha256(summary.PNG_SIGNATURE).hexdigest()}]
        run_path.write_text(json.dumps(run), encoding="ascii")
        self.assertEqual(len(summary.summarize_run(self.root)["inventory"]), 4)
        run["screenshots"][0]["path"] = "screenshots/other.png"
        run_path.write_text(json.dumps(run), encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_screenshot_png_signature_is_required(self) -> None:
        screenshot = self.root / "screenshots" / "stable.png"
        screenshot.write_bytes(b"BM\x00\x00\x00\x00\x00\x00")
        run_path = self.root / "run.json"
        run = json.loads(run_path.read_text(encoding="ascii"))
        run["screenshots"][0]["sha256"] = hashlib.sha256(
            screenshot.read_bytes()
        ).hexdigest()
        run_path.write_text(json.dumps(run, sort_keys=True), encoding="ascii")
        with self.assertRaisesRegex(summary.CoverageError, "PNG signature"):
            summary.summarize_run(self.root)

    def test_sdk_diagnostics_use_pinned_registry_and_emit_counts(self) -> None:
        _rewrite_log(
            self.root,
            "NRD-COVERAGE-BEGIN\n**** PRIMARY RINGBUFFER: Failed to execute packet.\n"
            "**** ExecutePacket: Failed to execute packet.\nNRD-COVERAGE-END\n",
        )
        result = summary.summarize_run(self.root)
        self.assertEqual(result["diagnostics"][0]["id"], "NRD-SDK-FAIL-0001")
        self.assertEqual(result["diagnostics"][0]["count"], 2)
        self.assertEqual(
            result["diagnostics"][0]["evidence"]["source_sha256"],
            "543898c0f86969fef256c5c267503ff935a74b1f9ec1bd460e949e43f412e3fe",
        )
        _rewrite_log(self.root, "NRD-COVERAGE-BEGIN\nSDK error literal\nNRD-COVERAGE-END\n")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_sdk_diagnostic_templates_are_bounded(self) -> None:
        messages = [
            "**** INDIRECT RINGBUFFER: Failed to execute packet.",
            "Shader 00000000000000AF translation failed; marking as ignored",
            "Failed to create graphics pipeline with VS 00000000000000AF, PS 00000000000000BE",
            "IssueSwap: RequestSwapTexture failed - fetch0: 00000000 00000001 00000002 00000003 00000004 00000005",
            "Out-of-submission queue operation fence wait failed with HRESULT 0x80004005",
            "D3D12 device removed: HRESULT 0x887A0005 - DEVICE_HUNG (TDR - GPU command took too long)",
            "DRED breadcrumb: completed 3 of 4 ops",
            "  [0] op type 1",
            "  [1] op type 2",
            "  [2] op type 3",
            "  [3] op type 4 <-- FAULT",
            "DRED page fault at VA 0x00000000DEADBEEF",
            "Failed to write the swap source capture",
            r"ResolvePath(\Device) failed - device not found",
            "ResolvePath(UPDATE:\\) failed - device not found",
        ]
        _rewrite_log(self.root, "NRD-COVERAGE-BEGIN\n" + "\n".join(messages) + "\nNRD-COVERAGE-END\n")
        result = summary.summarize_run(self.root)
        self.assertEqual([row["id"] for row in result["diagnostics"]], [f"NRD-SDK-FAIL-{index:04d}" for index in range(1, 12)])
        for message in (
            "Failed to execute packet.",
            "Failed to execute packet",
            "Shader 00000000000000AF translation failed; marking as ignored extra",
            "IssueSwap: RequestSwapTexture failed - fetch0: 00",
            "Submission tracker signal failed with HRESULT 0x80004005",
        ):
            _rewrite_log(self.root, f"NRD-COVERAGE-BEGIN\n{message}\nNRD-COVERAGE-END\n")
            with self.assertRaises(summary.CoverageError):
                summary.summarize_run(self.root)

    def test_expected_filesystem_probe_diagnostics_are_exact(self) -> None:
        messages = [
            r"ResolvePath(\Device) failed - device not found",
            "ResolvePath(UPDATE:\\) failed - device not found",
            "ResolvePath(UPDATE:\\) failed - device not found",
        ]
        _rewrite_log(
            self.root,
            "NRD-COVERAGE-BEGIN\n" + "\n".join(messages) + "\nNRD-COVERAGE-END\n",
        )
        result = summary.summarize_run(self.root)
        self.assertEqual(
            [(row["id"], row["count"]) for row in result["diagnostics"]],
            [("NRD-SDK-FAIL-0010", 1), ("NRD-SDK-FAIL-0011", 2)],
        )
        self.assertEqual(
            result["diagnostics"][0]["evidence"]["source_sha256"],
            "461fd8f5d9b0077847ac477a6f106556bae4c4af0a3c7f70c4467946966a2d51",
        )
        run = json.loads((self.root / "run.json").read_text(encoding="ascii"))
        _, intro_diagnostics = summary._check_log(
            self.root,
            run["log"],
            {"fixture_id": "NRD-FIX-0001", "transition_id": "NRD-TRANS-0010"},
        )
        self.assertEqual(
            [(row["id"], row["count"]) for row in intro_diagnostics],
            [("NRD-SDK-FAIL-0010", 1), ("NRD-SDK-FAIL-0011", 2)],
        )
        for state in (
            {"fixture_id": "NRD-FIX-0001", "transition_id": "NRD-TRANS-0004"},
            {"fixture_id": "NRD-FIX-0003", "transition_id": "NRD-TRANS-0010"},
        ):
            with self.subTest(state=state), self.assertRaisesRegex(
                summary.CoverageError, "accepted fixture bound"
            ):
                summary._check_log(self.root, run["log"], state)
        for message in (
            r"ResolvePath(\Device\Image) failed - device not found",
            "ResolvePath(update:\\) failed - device not found",
            "ResolvePath(UPDATE:\\) failed - entry not found",
            "ResolvePath(UPDATE:) failed - device not found",
        ):
            _rewrite_log(
                self.root,
                f"NRD-COVERAGE-BEGIN\n{message}\nNRD-COVERAGE-END\n",
            )
            with self.subTest(message=message), self.assertRaises(summary.CoverageError):
                summary.summarize_run(self.root)
        for name, bounded_messages in (
            ("device-count", [messages[0], messages[0]]),
            ("update-count", [messages[1]] * 5),
        ):
            _rewrite_log(
                self.root,
                "NRD-COVERAGE-BEGIN\n"
                + "\n".join(bounded_messages)
                + "\nNRD-COVERAGE-END\n",
            )
            with self.subTest(name=name), self.assertRaisesRegex(
                summary.CoverageError, "accepted fixture bound"
            ):
                summary.summarize_run(self.root)
            bounded_run = json.loads((self.root / "run.json").read_text(encoding="ascii"))
            with self.subTest(name=name, transition="full-intro"), self.assertRaisesRegex(
                summary.CoverageError, "accepted fixture bound"
            ):
                summary._check_log(
                    self.root,
                    bounded_run["log"],
                    {"fixture_id": "NRD-FIX-0001", "transition_id": "NRD-TRANS-0010"},
                )

    def test_log_envelope_and_dred_details_fail_closed(self) -> None:
        log = self.root / "coverage.log"
        log.write_text("NRD-COVERAGE-BEGIN\nNRD-COVERAGE-END\n", encoding="ascii")
        run_path = self.root / "run.json"
        run = json.loads(run_path.read_text())
        run["log"]["sha256"] = hashlib.sha256(log.read_bytes()).hexdigest()
        run_path.write_text(json.dumps(run), encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        _rewrite_log(
            self.root,
            "NRD-COVERAGE-BEGIN\nDRED breadcrumb: completed 3 of 4 ops\n"
            "  [0] op type 1\n  [1] op type 2\nNRD-COVERAGE-END\n",
        )
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)

    def test_locked_sdk_device_feature_continuations_are_exact(self) -> None:
        feature_lines = [
            "* Max GPU virtual address bits per resource: 40",
            "* Non-zeroed heap creation: yes",
            "* Pixel-shader-specified stencil reference: no",
            "* Programmable sample positions: tier 2",
            "* Rasterizer-ordered views: yes",
            "* Resource binding: tier 3",
            "* Tiled resources: tier 4",
            "* Unaligned block-compressed textures: yes",
        ]

        def render(
            parent: str = "Direct3D 12 device and OS features:",
            level: str = "info",
            lines: list[str] | None = None,
        ) -> str:
            return "\n".join([
                _log_line("NRD-COVERAGE-BEGIN"),
                _log_line(parent, level),
                *(feature_lines if lines is None else lines),
                _log_line("NRD-COVERAGE-END"),
            ]) + "\n"

        _rewrite_raw_log(self.root, render())
        summary.summarize_run(self.root)
        _rewrite_raw_log(self.root, render(lines=[
            "* Max GPU virtual address bits per resource: 4294967295",
            *feature_lines[1:],
        ]))
        summary.summarize_run(self.root)

        mutations = {
            "stray": "\n".join([
                _log_line("NRD-COVERAGE-BEGIN"),
                feature_lines[0],
                _log_line("NRD-COVERAGE-END"),
            ]) + "\n",
            "missing": render(lines=feature_lines[:-1]),
            "extra": render(lines=feature_lines + ["* Extra feature: yes"]),
            "reordered": render(lines=[feature_lines[1], feature_lines[0], *feature_lines[2:]]),
            "malformed": render(lines=[
                *feature_lines[:4],
                "* Rasterizer-ordered views: enabled",
                *feature_lines[5:],
            ]),
            "overflow": render(lines=[
                "* Max GPU virtual address bits per resource: 4294967296",
                *feature_lines[1:],
            ]),
            "leading-zero": render(lines=[
                "* Max GPU virtual address bits per resource: 040",
                *feature_lines[1:],
            ]),
            "signed": render(lines=[
                "* Max GPU virtual address bits per resource: +40",
                *feature_lines[1:],
            ]),
            "negative": render(lines=[
                "* Max GPU virtual address bits per resource: -1",
                *feature_lines[1:],
            ]),
            "enveloped-continuation": render(lines=[
                _log_line(feature_lines[0]),
                *feature_lines[1:],
            ]),
            "different-parent": render(parent="Direct3D 12 device features:"),
            "different-level": render(level="debug"),
        }
        for name, text in mutations.items():
            with self.subTest(name=name):
                _rewrite_raw_log(self.root, text)
                with self.assertRaises(summary.CoverageError):
                    summary.summarize_run(self.root)

    def test_unknown_diagnostic_and_wrong_sdk_are_blocked(self) -> None:
        _rewrite_log(self.root, "NRD-COVERAGE-BEGIN\nunknown fatal condition\nNRD-COVERAGE-END\n")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)
        _write_bundle(self.root / "sdk")
        run_path = self.root / "sdk" / "run.json"
        run = json.loads(run_path.read_text())
        run["sdk_version"] = "wrong"
        run_path.write_text(json.dumps(run), encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root / "sdk")

    def test_window_close_and_shutdown_block_lifetime_claims(self) -> None:
        for exit_class in ("window_close", "shutdown"):
            root = self.root / exit_class
            _write_bundle(root, exit_class=exit_class)
            result = summary.summarize_run(root)
            self.assertEqual(result["lifetime_claim"], "not-applicable")
            self.assertEqual(result["zero_absence_claim"], "blocked")

    def test_duplicate_truncated_and_fresh_output_rules(self) -> None:
        second = self.root / "output"
        _write_bundle(second)
        output = second / "summary.json"
        self.assertEqual(summary.main([str(second), "--output", str(output)]), 0)
        self.assertEqual(summary.main([str(second), "--output", str(output)]), 1)
        nested = self.root / "nested"
        _write_bundle(nested)
        nested_output = nested / "reports" / "summary.json"
        nested_output.parent.mkdir()
        self.assertEqual(summary.main([str(nested), "--output", str(nested_output)]), 0)
        self.assertEqual(summary.main([str(nested), "--output", str(nested / "observer" / "summary.json")]), 1)
        run_path = self.root / "run.json"
        run_path.write_text('{"schema":"x","schema":"y"}', encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_run(self.root)


class SeriesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.first = self.root / "first"
        self.second = self.root / "second"
        _write_bundle(self.first, run_id="NRD-RUN-20260829-0001", repeat=1)
        _write_bundle(self.second, run_id="NRD-RUN-20260829-0002", repeat=2)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _observe(self, root: Path, counter: int) -> None:
        coverage = json.loads((root / "observer" / "coverage.json").read_text())
        coverage["counters"][counter]["count"] = 1
        _rewrite_coverage(root, coverage)

    def test_matching_series_computes_real_union_and_intersection(self) -> None:
        self._observe(self.first, 0)
        self._observe(self.second, 0)
        self._observe(self.second, 2)
        result = summary.summarize_series([self.second, self.first])
        self.assertEqual(result["runs"][0]["repeat"], 1)
        self.assertEqual(result["matrix"]["intersection"]["0"]["0"]["0"], [0])
        self.assertEqual(result["matrix"]["union"]["0"]["1"]["0"], [0])
        self.assertEqual(result["discriminators"]["stable_observed"], ["primitive-4"])
        self.assertEqual(result["discriminators"]["incidental"], [])

    def test_series_identity_and_repeat_mismatches_block(self) -> None:
        run_path = self.second / "run.json"
        run = json.loads(run_path.read_text())
        run["fixture_sha256"] = "d" * 64
        run_path.write_text(json.dumps(run), encoding="ascii")
        with self.assertRaises(summary.CoverageError):
            summary.summarize_series([self.first, self.second])
        _write_bundle(self.second, run_id="NRD-RUN-20260829-0002", repeat=1)
        with self.assertRaises(summary.CoverageError):
            summary.summarize_series([self.first, self.second])


if __name__ == "__main__":
    unittest.main()
