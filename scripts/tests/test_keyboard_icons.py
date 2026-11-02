"""Keyboard button-icon art: generator, runtime key list and provenance."""

import hashlib
import importlib.util
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import build_keyboard_icons as generator  # noqa: E402

CPP = REPO / "src" / "keyboard_icons.cpp"
PROVENANCE = REPO / "thirdparty" / "xelu-prompts" / "PROVENANCE.md"


def runtime_keys():
    """The SDK key names listed in src/keyboard_icons.cpp, in order."""
    text = CPP.read_text(encoding="ascii")
    match = re.search(
        r"std::array<std::string_view, (\d+)> kKeys\{ \{(.*?)\} \};", text, re.S
    )
    if not match:
        raise AssertionError("kKeys table not found in src/keyboard_icons.cpp")
    keys = re.findall(r'"([^"]+)"', match.group(2))
    if len(keys) != int(match.group(1)):
        raise AssertionError("kKeys length differs from its declared size")
    return keys


def expected_header():
    return struct.pack(
        "<4s31I",
        b"DDS ",
        124,
        0x100F,
        64,
        64,
        256,
        0,
        0,
        *([0] * 11),
        32,
        0x41,
        0,
        32,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        0x1000,
        0,
        0,
        0,
        0,
    )


class KeyboardIconTests(unittest.TestCase):
    def test_runtime_key_list_matches_generator_and_shipped_files(self) -> None:
        keys = runtime_keys()
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(set(keys), set(generator.KEYS))
        shipped = {path.stem for path in generator.OUTPUT_DIR.iterdir()}
        self.assertEqual(set(keys), shipped)
        self.assertTrue(all(p.suffix == ".dds" for p in generator.OUTPUT_DIR.iterdir()))

    def test_shipped_files_have_the_exact_legacy_header(self) -> None:
        header = expected_header()
        self.assertEqual(len(header), 128)
        for path in sorted(generator.OUTPUT_DIR.iterdir()):
            with self.subTest(path.name):
                data = path.read_bytes()
                self.assertEqual(len(data), 16512)
                self.assertEqual(data[:128], header)

    def test_generator_reproduces_every_file(self) -> None:
        if importlib.util.find_spec("PIL") is None:
            self.skipTest("Pillow is not installed")
        result = subprocess.run(
            [sys.executable, "-B", str(REPO / "scripts" / "build_keyboard_icons.py")]
            + ["--check"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_provenance_hashes_match_the_imported_art(self) -> None:
        text = PROVENANCE.read_text(encoding="ascii")
        recorded = dict(re.findall(r"^\| (\S+) \| ([0-9a-f]{64}) \|$", text, re.M))
        art = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in generator.SOURCE_DIR.iterdir()
        }
        expected = {f"{stem}_Key_Light.png": None for stem in generator.KEYS.values()}
        self.assertEqual(set(art), set(expected))
        self.assertEqual(
            {name: recorded.get(name) for name in art},
            art,
        )
        self.assertEqual(set(recorded) - set(art), {"Readme.txt"})
        self.assertIn(
            "2848fd4259437a6f594f81ff8d8d7a1f922397c7e9da163b2535aa82ae1df037", text
        )
        self.assertIn("6560174", text)
        self.assertIn("CC0", (PROVENANCE.parent / "Readme.txt").read_text("ascii"))


if __name__ == "__main__":
    unittest.main()
