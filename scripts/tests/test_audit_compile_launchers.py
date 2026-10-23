"""Compile launcher audit tests."""

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
from audit_compile_launchers import audit  # noqa: E402

CLEAN = "CMAKE_CXX_COMPILER_LAUNCHER:STRING=\nCMAKE_C_COMPILER_LAUNCHER:STRING=\n"
GENERATED = "clang++ -c C:/src/generated/default/ppc_recomp.0.cpp -o a.obj\n"
OTHER = "clang++ -c C:/src/src/app.cpp -o b.obj\n"


class AuditTests(unittest.TestCase):
    def test_clean_build_passes(self) -> None:
        self.assertEqual(audit(CLEAN, GENERATED + OTHER), [])

    def test_configured_launcher_fails(self) -> None:
        cache = "CMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\n"
        self.assertEqual(len(audit(cache, GENERATED)), 1)

    def test_launcher_in_command_fails(self) -> None:
        for prefix in ("sccache ", "C:/tools/sccache.exe ", "/usr/bin/ccache "):
            with self.subTest(prefix=prefix):
                problems = audit(CLEAN, prefix + GENERATED)
                self.assertEqual(len(problems), 1)

    def test_vacuous_input_fails(self) -> None:
        self.assertIn("no compile commands found", audit(CLEAN, ""))
        self.assertIn(
            "no generated guest code compile command found", audit(CLEAN, OTHER)
        )

    def test_backslash_launcher_path_fails(self) -> None:
        line = (
            r"C:\tools\sccache.exe clang++ -c generated/default/a.cpp -o a.obj" + "\n"
        )
        self.assertEqual(len(audit(CLEAN, line)), 1)

    def test_non_compile_generated_command_does_not_count(self) -> None:
        touch = "cmake -E touch generated/default/stamp\n"
        self.assertIn(
            "no generated guest code compile command found", audit(CLEAN, touch + OTHER)
        )

    def test_before_codegen_skips_guest_check(self) -> None:
        self.assertEqual(audit(CLEAN, OTHER, require_generated=False), [])

    def test_similar_names_pass(self) -> None:
        self.assertEqual(
            audit(CLEAN, GENERATED + "clang++ -c ccached_widget.cpp\n"), []
        )


if __name__ == "__main__":
    unittest.main()
