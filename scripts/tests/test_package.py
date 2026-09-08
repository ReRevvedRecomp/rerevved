"""Release archive inventory checks."""

import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


class PackageTests(unittest.TestCase):
    def test_windows_archive_includes_default_logo_pack(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            output = root / "output"
            build.mkdir()
            for binary in ("rerevved.exe", "rexruntime.dll", "rexgpu-xenos.dll"):
                (build / binary).write_bytes(binary.encode("ascii"))

            subprocess.run(
                [
                    sys.executable,
                    str(REPO / "scripts" / "package.py"),
                    "--platform",
                    "windows",
                    "--arch",
                    "x64",
                    "--build-dir",
                    str(build),
                    "--out-dir",
                    str(output),
                ],
                cwd=REPO,
                check=True,
                capture_output=True,
                text=True,
            )

            archive_path = next(output.glob("rerevved-v*-windows-x64.zip"))
            archive_root = archive_path.stem
            prefix = f"{archive_root}/asset-overrides/"
            with zipfile.ZipFile(archive_path) as archive:
                names = set(archive.namelist())
                self.assertEqual(
                    archive.read(prefix + "default_asset_order.txt")
                    .decode("ascii")
                    .strip(),
                    "rerevved-logo",
                )
            self.assertIn(prefix + "rerevved-logo/asset-pack.toml", names)
            self.assertIn(
                prefix + "rerevved-logo/assets/file-data/GFX_MainMenu_logo.dds",
                names,
            )


if __name__ == "__main__":
    unittest.main()
