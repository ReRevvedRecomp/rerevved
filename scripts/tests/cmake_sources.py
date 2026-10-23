"""Shared source-list inspection for repository contract checks."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def cmake_text() -> str:
    return "\n".join(
        (ROOT / name).read_text()
        for name in (
            "CMakeLists.txt",
            "tests/gpu/render_tests.cmake",
            "tests/gpu/hook_tests.cmake",
        )
    )


def rerevved_tests_sources() -> set[str]:
    """Windows sources compiled into rerevved_tests: its target_sources() lists
    and the OBJECT libraries it links. A unity wrapper stands for the production
    sources it includes."""
    text = "\n".join(
        (ROOT / name).read_text()
        for name in ("tests/gpu/render_tests.cmake", "tests/gpu/hook_tests.cmake")
    )
    text = re.sub(r"if\(NOT WIN32\).*?\n\s*endif\(\)", "", text, flags=re.S)
    sources: set[str] = set()
    for block in re.findall(
        r"target_sources\(rerevved_tests PRIVATE(.*?)\n\s*\)", text, re.S
    ):
        sources.update(re.findall(r"[\w./-]+\.cpp", block))
    for name, block in re.findall(r"add_library\((\w+) OBJECT(.*?)\n\s*\)", text, re.S):
        if re.search(
            rf"target_link_libraries\(rerevved_tests PRIVATE[^)]*\b{name}\b", text
        ):
            sources.update(re.findall(r"[\w./-]+\.cpp", block))
    for source in list(sources):
        if source.endswith("_variant.cpp"):
            wrapper = (ROOT / source).read_text()
            sources.update(
                "src/" + path
                for path in re.findall(r'#include "(gpu/[\w/]+\.cpp)"', wrapper)
            )
    return sources


def check_probe_registration(test):
    """Repository wiring checked alongside the complete Catch2 case inventory."""
    sources = rerevved_tests_sources()
    for path in (
        "src/gpu/guest_d3d/completion_probe.cpp",
        "src/gpu/guest_d3d/completion_probe_hooks.cpp",
        "tests/gpu/completion_probe_test.cpp",
        "src/gpu/guest_d3d/vs_readback_probe.cpp",
        "src/gpu/guest_d3d/vs_readback_probe_hooks.cpp",
        "tests/gpu/vs_readback_probe_test.cpp",
        "src/gpu/guest_d3d/tiling_probe.cpp",
        "src/gpu/guest_d3d/video_scaler_stub.cpp",
        "src/gpu/guest_d3d/video_scaler_hooks.cpp",
        "tests/gpu/video_scaler_stub_test.cpp",
        "src/gpu/guest_d3d/video_display_mode_stub.cpp",
        "src/gpu/guest_d3d/video_display_mode_hooks.cpp",
        "src/gpu/guest_d3d/ring_stubs.cpp",
        "tests/gpu/video_display_mode_stub_test.cpp",
        "src/gpu/guest_d3d/video_import_stub_table.cpp",
        "src/gpu/guest_d3d/video_import_stub_hooks.cpp",
        "tests/gpu/video_import_stub_table_test.cpp",
    ):
        test.assertIn(path, sources)
    cmake = cmake_text()
    for path, count in (
        ("src/gpu/guest_d3d/vs_readback_probe.cpp", 2),
        ("src/gpu/guest_d3d/vs_readback_probe_hooks.cpp", 2),
        ("src/gpu/guest_d3d/vs_readback_probe.h", 1),
        ("src/gpu/guest_d3d/video_display_mode_stub.cpp", 2),
        ("src/gpu/guest_d3d/video_display_mode_hooks.cpp", 2),
        ("src/gpu/guest_d3d/video_import_stub_table.cpp", 2),
        ("src/gpu/guest_d3d/video_import_stub_hooks.cpp", 2),
        ("src/gpu/guest_d3d/video_import_stub_table.h", 1),
    ):
        test.assertEqual(cmake.count(path), count, path)
    test.assertIn("target_sources(rerevved PRIVATE", cmake)
    builds = [
        target.split("\n    )", 1)[0]
        for target in re.split(r"add_executable\(", cmake)[1:]
    ]
    builds.append("\n".join(sorted(sources)))
    test.assertIn("src/gpu/guest_d3d/route_hooks.cpp", builds[-1])
    for sources in builds:
        if "src/gpu/guest_d3d/route_hooks.cpp" in sources:
            test.assertIn("src/gpu/guest_d3d/completion_probe.cpp", sources)
            test.assertIn("src/gpu/guest_d3d/tiling_probe.cpp", sources)
