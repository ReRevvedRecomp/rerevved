import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "build_native_shaders",
    Path(__file__).resolve().parents[1] / "build-native-shaders.py",
)
SHADERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SHADERS)


class NativeShaderInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

        def asset(name, data):
            (self.root / name).write_bytes(data)
            return {"file": name, "sha256": hashlib.sha256(data).hexdigest()}

        registers = [0] * 0x5003
        registers[0x2000] = 0x0A010280
        self.recipe = {
            "schema_version": 1,
            "output": "generated",
            "texture_fetches": [2, 0, 1],
            "registers": asset("registers.bin", struct.pack("<20483I", *registers)),
            "shared_header": asset("common.h", b"header"),
            "tools": {
                name: asset(name + suffix, b"pinned tool")
                for name, suffix in [
                    ("adapter", ".exe"),
                    ("vertex_compiler", ".exe"),
                    ("pixel_compiler", ".exe"),
                    ("dxcompiler", ".dll"),
                    ("dxil", ".dll"),
                ]
            },
            "shaders": [
                {
                    "stage": stage,
                    "hash": "0123456789ABCDEF",
                    "byte_order": "little",
                    "microcode": asset(stage + ".bin", bytes(12)),
                }
                for stage in ("vs", "ps")
            ],
        }
        self.path = self.root / "recipe.json"
        self.path.write_text(json.dumps(self.recipe), encoding="ascii")

    def test_check_preserves_inputs_and_runs_no_tools(self):
        with patch.object(SHADERS.subprocess, "run") as run:
            SHADERS.generate(self.path, check=True)
        run.assert_not_called()
        self.assertFalse((self.root / "generated").exists())

    def test_changed_tool_rejected_before_output_creation(self):
        (self.root / "adapter.exe").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            SHADERS.generate(self.path)
        self.assertFalse((self.root / "generated").exists())

    def test_existing_output_preserved(self):
        output = self.root / "generated"
        output.mkdir()
        sentinel = output / "vs.dxil"
        sentinel.write_bytes(b"accepted")
        with self.assertRaisesRegex(ValueError, "fresh directory"):
            SHADERS.generate(self.path)
        self.assertEqual(sentinel.read_bytes(), b"accepted")
