from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from cmake_sources import check_probe_registration

ROOT = Path(__file__).resolve().parents[2]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="ascii")
TESTS_CMAKE = (ROOT / "tests/rerevved_tests.cmake").read_text(encoding="ascii")
RENDER_CMAKE = (ROOT / "tests/gpu/render_tests.cmake").read_text(encoding="ascii")
PREPARED_CMAKE = (ROOT / "tests/gpu/prepared_tests.cmake").read_text(encoding="ascii")
SMOKE_CMAKE = (ROOT / "tests/gpu/native_app_smoke.cmake").read_text(encoding="ascii")
WORKFLOW = (ROOT / ".github/workflows/build.yml").read_text(encoding="ascii")
PRESETS = json.loads((ROOT / "CMakePresets.json").read_text(encoding="ascii"))

# The explicit CPU-only, GPU-free tier. A case joins it by carrying the
# [local-fast] tag in its source, and by being listed here in the same change.
LOCAL_FAST_CASES = frozenset(
    (
        "resource_settings",
        "save_sidecar",
        "start_resources",
        "occ_settings",
        "raging_barbarians_settings",
        "gotw_settings",
        "city_founding_unit_rules",
        "host_settings",
        "discord_presence_model",
        "discord_invites_model",
        "building_wonder_effect_rules_contract",
        "building_wonder_effect_rules_c_header",
        "unit_catalog_api_contract",
        "unit_catalog_api_c_header",
        "nation_select_text_contract",
        "nation_select_text_c_header",
        "unique_unit_rules_contract",
        "unique_unit_rules_c_header",
        "unit_movement_rules_contract",
        "unit_movement_rules_c_header",
        "unit_production_cost_rules_contract",
        "unit_production_cost_rules_c_header",
        "unit_combat_rules_contract",
        "unit_combat_rules_c_header",
        "unit_effect_rules_contract",
        "unit_effect_rules_c_header",
        "terrain_yield_rules_contract",
        "terrain_yield_rules_c_header",
        "unique_era_abilities_contract",
        "unique_era_abilities_c_header",
        "custom aspects parse strictly and reduce",
        "requests at or below 16:9 and unknown surfaces stay original",
        "horizontal scale follows the reduced target aspect",
        "the frustum widens about its own horizontal center",
        "invalid frusta and scales are refused unchanged",
        "frustum words change only left and right and never accumulate",
        "the GFx affine row is compressed about the guest center",
        "the scissor rectangle shrinks outward-rounded and stays inside the output",
        "a compressed viewport reproduces the affine correction without double compression",
        "frame policy returns the first failing reason",
        "probe output stays bounded",
        "the feature is inert by default and no frame is wide without owner acceptance",
        "the renderer cache is rebuilt only when the scale differs from the last build",
        "a changed frustum without a setter is flagged once",
        "a frustum with top at or below bottom is refused",
        "the hypothetical scale follows a wide custom value in every mode",
        "a full bounded stack refuses a push and keeps its contents",
        "the request ledger separates mismatch from eviction",
        "a wide build between Original sweeps restarts them",
        "the census-accepted camera vtables, frame caller and hooks",
        "the GFx scope is accepted only inside the world frame",
        "a camera is widened only for a symmetric 16:9 frustum",
        "movie state is derived from gameplay and recent movie hooks",
        "entries not seen for a bounded time are retired conservatively",
        "an untracked wide build keeps every camera rebuilding until Original sweeps",
        "original mode stays inert and the latch camera acceptance matches the census",
        "every wide camera is tracked and an untouched table is inert",
        "a settings change racing a snapshot keeps the feature active",
    )
)
PYTHON_CONTRACTS = (
    "save_sidecar_contract",
    "resource_settings_contract",
    "raging_barbarians_contract",
    "occ_contract",
    "gotw_contract",
    "city_founding_unit_contract",
)
SCHEDULING_TAG = re.compile(r"^(native_gpu|run_serial|timeout-[0-9]+)$")
CASE = re.compile(r'TEST_CASE\(\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\)')


def prepared_research_sources() -> set[Path]:
    """Sources compiled only into rerevved_prepared_tests."""
    stems = PREPARED_CMAKE.split("set(REREVVED_PREPARED_STEMS", 1)[1].split(")", 1)[0]
    return {
        ROOT / "tests/gpu" / f"{stem}{suffix}.cpp"
        for stem in re.findall(r"native_\w+", stems)
        for suffix in ("_test", "_original")
    }


def rerevved_tests_sources() -> set[Path]:
    """Test sources named by the rerevved_tests build files.

    The fixture builders are compiled into both executables; they carry no
    cases and are not part of either exclusive set.
    """
    shared = set(
        re.findall(
            r"tests/gpu/\w+\.cpp",
            RENDER_CMAKE.split("set(REREVVED_PREPARED_FIXTURE_SOURCES", 1)[1].split(
                ")", 1
            )[0],
        )
    )
    named = set()
    for path in (ROOT / "tests").rglob("*.cmake"):
        if path.name != "prepared_tests.cmake":
            named.update(re.findall(r"tests/[\w/]+\.cpp", path.read_text("ascii")))
    named.update(re.findall(r"tests/[\w/]+\.cpp", CMAKE))
    return {ROOT / name for name in named - shared}


def source_cases() -> list[tuple[str, tuple[str, ...], Path]]:
    cases = []
    for path in sorted((ROOT / "tests").rglob("*.cpp")):
        text = path.read_text(encoding="ascii")
        for name, tags in CASE.findall(text):
            cases.append((name, tuple(re.findall(r"\[([^\]]+)\]", tags)), path))
    return cases


def other_registrations() -> set[str]:
    """Names registered with add_test outside the Catch2 discovery."""
    names = set()
    for path in [ROOT / "CMakeLists.txt", *(ROOT / "tests").rglob("*.cmake")]:
        names.update(
            re.findall(r"add_test\(NAME\s+([A-Za-z0-9_]+)", path.read_text("ascii"))
        )
    return names


class LocalFastTierContractTests(unittest.TestCase):
    def test_case_registration_and_fast_tier(self) -> None:
        check_probe_registration(self)
        cases = source_cases()
        names = [name for name, _, _ in cases]
        # The two executables register under different prefixes, so a name
        # must be unique within its own executable only.
        research = prepared_research_sources()
        main = rerevved_tests_sources()
        self.assertFalse(main & research, "a source is in both test executables")
        for path in {path for _, _, path in cases}:
            self.assertIn(path, main | research, f"{path.name} is in no executable")
        for owner in (main, research):
            owned = [name for name, _, path in cases if path in owner]
            self.assertEqual(len(owned), len(set(owned)))
        self.assertFalse(set(names) & other_registrations())
        for name, tags, path in cases:
            with self.subTest(case=name):
                if "renderer-research" in tags:
                    # Research cases are hidden and registered only on request.
                    self.assertIn(".", tags, f"{path.name} research case is not hidden")
                    self.assertNotIn("ctest", tags)
                    self.assertNotIn("local-fast", tags)
                elif "manual" in tags:
                    # Manual cases are hidden and run only through a helper command.
                    self.assertIn(".", tags, f"{path.name} manual case is not hidden")
                    self.assertNotIn("ctest", tags)
                    self.assertFalse(
                        [tag for tag in tags if SCHEDULING_TAG.match(tag)]
                        + [tag for tag in tags if tag == "local-fast"]
                    )
                else:
                    self.assertIn("ctest", tags, f"{path.name} case is not discovered")
                suites = [
                    tag
                    for tag in tags
                    if tag
                    not in (
                        "ctest",
                        "local-fast",
                        ".",
                        "manual",
                        "renderer-research",
                        "release-smoke",
                    )
                    and not SCHEDULING_TAG.match(tag)
                ]
                self.assertEqual(len(suites), 1, "one suite tag is required")
                self.assertRegex(suites[0], r"^[a-z0-9]+(-[a-z0-9]+)*$")
                self.assertEqual(len(tags), len(set(tags)))
                self.assertLessEqual(
                    len([t for t in tags if t.startswith("timeout")]), 1
                )

        tagged = {name for name, tags, _ in cases if "local-fast" in tags}
        self.assertEqual(tagged, set(LOCAL_FAST_CASES))

        for name, tags, _ in cases:
            if "local-fast" not in tags:
                continue
            with self.subTest(case=name):
                self.assertNotRegex(name, r"capture|harvest|d3d12|fixture|bench")
                self.assertNotIn(name, PYTHON_CONTRACTS)
                self.assertFalse(
                    [tag for tag in tags if SCHEDULING_TAG.match(tag)],
                    "a fast case holds no device lock, serial slot or timeout",
                )

    def test_render_tests_declare_no_standalone_executable(self) -> None:
        self.assertNotIn("add_executable(", RENDER_CMAKE)
        self.assertNotIn("add_test(", RENDER_CMAKE)
        self.assertIn("target_sources(rerevved_tests", RENDER_CMAKE)
        self.assertIn("include(tests/gpu/native_app_smoke.cmake)", RENDER_CMAKE)

    def test_app_smoke_is_the_only_lifecycle_executable(self) -> None:
        self.assertEqual(SMOKE_CMAKE.count("add_executable("), 1)
        self.assertIn("add_executable(rerevved_native_app_smoke", SMOKE_CMAKE)
        self.assertEqual(
            re.findall(r"add_test\(NAME (\w+)", SMOKE_CMAKE),
            ["native_app_hidden", "native_app_hidden_trace"],
        )
        self.assertRegex(
            SMOKE_CMAKE,
            r"if\(REREVVED_DEVELOPER\)\s+add_test\(NAME native_app_hidden_trace",
            "the full-app trace check is developer-only",
        )

    def test_ticket_helper_needs_no_renderer_rename_or_windows_header(self) -> None:
        # Tests never rename the real renderer class by compile definition.
        self.assertNotIn("NativeRendererD3D12=", RENDER_CMAKE)
        # The ticket helper header compiles on every platform: the D3D12 and
        # presenter includes and the renderer accessors sit under _WIN32, and
        # the ticket Export a non-Windows build lacks comes from a test source.
        access = (ROOT / "tests/support/native_render_frame_test_access.h").read_text(
            encoding="ascii"
        )
        guarded = access.split("#ifdef _WIN32", 1)[1]
        self.assertNotIn("#include <d3d12.h>", access.split("#ifdef _WIN32", 1)[0])
        self.assertNotIn("#include <rex/ui/d3d12/", access.split("#ifdef _WIN32", 1)[0])
        self.assertIn("#include <d3d12.h>", guarded)
        self.assertIn("static std::shared_ptr<NativeRenderFrameTicket> Ticket", access)
        self.assertLess(
            access.index("static void PublishExports"),
            access.index("static ID3D12CommandQueue* Queue"),
        )
        non_windows = RENDER_CMAKE.split("if(NOT WIN32)", 1)[1].split("endif()", 1)[0]
        self.assertIn(
            "tests/support/native_render_frame_ticket_export.cpp", non_windows
        )

    def test_prepared_research_executable_is_optional_and_prefixed(self) -> None:
        self.assertIn("include(tests/gpu/prepared_tests.cmake)", RENDER_CMAKE)
        self.assertRegex(
            PREPARED_CMAKE, r'option\(REREVVED_PREPARED_TESTS\s+"[^"]+"\s+OFF\)'
        )
        self.assertIn(
            "if(BUILD_TESTING AND WIN32 AND REREVVED_PREPARED_TESTS)", PREPARED_CMAKE
        )
        self.assertEqual(PREPARED_CMAKE.count("add_executable("), 1)
        self.assertIn("add_executable(rerevved_prepared_tests", PREPARED_CMAKE)
        self.assertIn('TEST_PREFIX "prepared."', PREPARED_CMAKE)
        self.assertIn("LABELS prepared-research", PREPARED_CMAKE)
        # The representative cases keep their names in the unified executable.
        unified = {
            name
            for name, _, path in source_cases()
            if path not in prepared_research_sources()
        }
        for name in (
            "native_prepared_bc1_fixture",
            "native_prepared_rgba8_matrix_fixture",
            "native_prepared_float_matrix_fixture",
            "native_prepared_cube_matrix_fixture",
            "native_pixel_lod_fixture",
        ):
            self.assertIn(name, unified)
        self.assertNotIn("native_prepared_bc2_fixture", unified)

    def test_fast_target_depends_on_only_the_unified_executable(self) -> None:
        self.assertIn("add_custom_target(rerevved_fast_tests)", TESTS_CMAKE)
        self.assertIn(
            "add_dependencies(rerevved_fast_tests rerevved_tests)", TESTS_CMAKE
        )
        self.assertNotRegex(
            TESTS_CMAKE,
            r"add_dependencies\(rerevved_fast_tests[^)]*\brerevved\b\s*[)\s]",
        )
        self.assertEqual(TESTS_CMAKE.count("rerevved_fast_tests"), 2)

    def test_catch2_is_discovered_after_the_build_with_tags_as_labels(self) -> None:
        self.assertIn("add_subdirectory(thirdparty/catch2 EXCLUDE_FROM_ALL)", CMAKE)
        self.assertIn(
            'include("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/catch2/extras/Catch.cmake")',
            CMAKE,
        )
        for include in (
            "include(tests/rerevved_tests.cmake)",
            "include(tests/gpu/render_tests.cmake)",
            "include(tests/gpu/hook_tests.cmake)",
        ):
            self.assertIn(include, CMAKE)
        self.assertNotIn("add_subdirectory(tests", CMAKE)
        discover = re.search(
            r"catch_discover_tests\((?P<body>.*?)\)\n", TESTS_CMAKE, re.DOTALL
        )
        self.assertIsNotNone(discover)
        body = discover.group("body")
        self.assertTrue(body.startswith("rerevved_tests"))
        self.assertIn('TEST_SPEC "${REREVVED_TEST_SPEC}"', body)
        self.assertIn('set(REREVVED_TEST_SPEC "[ctest]")', TESTS_CMAKE)
        self.assertIn(
            'set(REREVVED_TEST_SPEC "[ctest],[renderer-research]")', TESTS_CMAKE
        )
        self.assertEqual(TESTS_CMAKE.count("catch_discover_tests("), 1)
        self.assertIn("ADD_TAGS_AS_LABELS", body)
        self.assertIn("DISCOVERY_MODE POST_BUILD", body)
        self.assertIn("Catch2::Catch2\n", TESTS_CMAKE)
        self.assertNotIn("Catch2WithMain", TESTS_CMAKE)
        self.assertIn("TEST_INCLUDE_FILES", TESTS_CMAKE)

    def discover_scheduling(self, tags):
        ctest = shutil.which("ctest")
        if not ctest:
            self.skipTest("CTest is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            discovery = root / "discovery.cmake"
            discovery.write_text(
                f"set_tests_properties( synthetic PROPERTIES LABELS [==[{tags}]==])\n",
                encoding="ascii",
            )
            (root / "CTestTestfile.cmake").write_text(
                'add_test(synthetic "unused")\n'
                f'include("{discovery.as_posix()}")\n'
                f'set(REREVVED_TEST_DISCOVERY_FILE "{discovery.as_posix()}")\n'
                f'include("{ROOT.as_posix()}/tests/support/'
                'catch_apply_test_properties.cmake")\n',
                encoding="ascii",
            )
            return subprocess.run(
                [ctest, "--test-dir", str(root), "--show-only=json-v1"],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_scheduling_tags_become_ctest_properties(self) -> None:
        for tags, expected in (
            ("ctest;synthetic", {}),
            (
                "ctest;synthetic;native_gpu;run_serial;timeout-60",
                {"RESOURCE_LOCK": ["native_gpu"], "RUN_SERIAL": True, "TIMEOUT": 60.0},
            ),
        ):
            with self.subTest(tags=tags):
                result = self.discover_scheduling(tags)
                self.assertEqual(result.returncode, 0, result.stderr)
                properties = {
                    entry["name"]: entry["value"]
                    for entry in json.loads(result.stdout)["tests"][0]["properties"]
                    if entry["name"] in ("RESOURCE_LOCK", "RUN_SERIAL", "TIMEOUT")
                }
                self.assertEqual(properties, expected)

    def test_invalid_scheduling_tags_fail(self) -> None:
        for tags, message in (
            ("native-gpu", "Unknown scheduling tag"),
            ("run-serial", "Unknown scheduling tag"),
            ("timeout-", "Unknown scheduling tag"),
            ("timeout-abc", "Unknown scheduling tag"),
            ("timeout-60;timeout-60", "more than one timeout tag"),
        ):
            with self.subTest(tags=tags):
                result = self.discover_scheduling(f"ctest;synthetic;{tags}")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_python_contract_label_covers_the_six_verify_discovered_tests(
        self,
    ) -> None:
        match = re.search(
            r"foreach\(_python_contract IN ITEMS(?P<names>.*?)\)\n",
            TESTS_CMAKE,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        self.assertEqual(tuple(match.group("names").split()), PYTHON_CONTRACTS)
        self.assertIn("APPEND PROPERTY LABELS python-contract", TESTS_CMAKE)
        for name in PYTHON_CONTRACTS:
            with self.subTest(test=name):
                self.assertRegex(
                    TESTS_CMAKE,
                    rf"add_test\(NAME\s+{name}\s+COMMAND\s+\$\{{Python3_EXECUTABLE\}}"
                    r"\s+-B\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/scripts/tests/test_\w+\.py",
                )

    def test_presets_reuse_the_release_configuration(self) -> None:
        build = {p["name"]: p for p in PRESETS["buildPresets"]}
        test = {p["name"]: p for p in PRESETS["testPresets"]}
        configure = {p["name"] for p in PRESETS["configurePresets"]}
        fast_build = build["win-amd64-local-fast"]
        fast_test = test["win-amd64-local-fast"]
        self.assertEqual(fast_build["configurePreset"], "win-amd64-release")
        self.assertEqual(fast_build["targets"], ["rerevved_fast_tests"])
        self.assertEqual(fast_test["configurePreset"], "win-amd64-release")
        self.assertEqual(fast_test["filter"]["include"]["label"], "local-fast")
        self.assertEqual(fast_test["execution"]["jobs"], 2)
        self.assertNotIn("win-amd64-local-fast", configure)
        self.assertNotIn("targets", build["win-amd64-release"])

    def test_ci_runs_ctest_with_an_explicit_configuration_and_two_jobs(self) -> None:
        commands = re.findall(r"run: (ctest .*)", WORKFLOW)
        self.assertEqual(len(commands), 4)
        for command in commands:
            with self.subTest(command=command):
                self.assertRegex(command, r"\s-C Release\b")
                self.assertRegex(command, r"\s-j 2\b")
                self.assertIn("--output-on-failure", command)


if __name__ == "__main__":
    unittest.main()
