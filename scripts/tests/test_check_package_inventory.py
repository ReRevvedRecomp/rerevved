"""Archive inventory checker tests."""

import io
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
from check_package_inventory import check  # noqa: E402
from package import expected_entries  # noqa: E402


def archive_name(platform):
    return f"rerevved-v1.0.0-{platform}-x64" + (
        ".zip" if platform == "windows" else ".tar.gz"
    )


def write_zip(path, files, extra, directories):
    root = path.name.removesuffix(".zip")
    with zipfile.ZipFile(path, "w") as archive:
        for name in sorted(files) + list(extra):
            archive.writestr(f"{root}/{name}", "x")
        for name in ("game",) if "game" in directories else ():
            archive.writestr(f"{root}/{name}/", "")


def write_tar(path, files, extra, directories, mode=0o755):
    root = path.name.removesuffix(".tar.gz")
    with tarfile.open(path, "w:gz") as archive:
        for name in [""] + sorted(directories):
            info = tarfile.TarInfo(f"{root}/{name}".rstrip("/"))
            info.type = tarfile.DIRTYPE
            archive.addfile(info)
        for name in sorted(files) + list(extra):
            info = tarfile.TarInfo(f"{root}/{name}")
            info.size = 1
            info.mode = mode if name == "rerevved" else 0o644
            archive.addfile(info, io.BytesIO(b"x"))


class InventoryTests(unittest.TestCase):
    def run_case(self, platform, drop=(), extra=(), without_directories=(), mode=0o755):
        directories, files = expected_entries(platform)
        directories = directories - set(without_directories)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name(platform)
            if platform == "windows":
                write_zip(path, files - set(drop), extra, directories)
            else:
                write_tar(path, files - set(drop), extra, directories, mode)
            return check(path, platform)

    def test_exact_inventory_passes(self) -> None:
        self.assertEqual(self.run_case("windows"), [])
        self.assertEqual(self.run_case("linux"), [])

    def test_windows_only_files_are_not_expected_on_linux(self) -> None:
        self.assertEqual(
            self.run_case("linux", extra=["rexglue.exe"]),
            ["unexpected file: rexglue.exe"],
        )

    def test_missing_binary_is_reported(self) -> None:
        self.assertEqual(
            self.run_case("windows", drop=["rexruntime.dll"]),
            ["missing file: rexruntime.dll"],
        )

    def test_forbidden_and_unlisted_files_are_reported(self) -> None:
        problems = self.run_case("windows", extra=["game/default.xex"])
        self.assertIn("forbidden suffix: game/default.xex", problems)
        self.assertIn("unexpected file: game/default.xex", problems)

    def test_missing_game_directory_is_reported(self) -> None:
        for platform in ("windows", "linux"):
            self.assertEqual(
                self.run_case(platform, without_directories=["game"]),
                ["missing directory: game"],
            )

    def test_linux_executable_bit_is_required(self) -> None:
        self.assertEqual(
            self.run_case("linux", mode=0o644), ["rerevved has no executable mode bit"]
        )

    def test_tar_link_entries_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("linux")
            root = path.name.removesuffix(".tar.gz")
            with tarfile.open(path, "w:gz") as archive:
                link = tarfile.TarInfo(f"{root}/rerevved")
                link.type = tarfile.SYMTYPE
                link.linkname = "/etc/passwd"
                archive.addfile(link)
            problems = check(path, "linux")
        self.assertIn(f"link or special entry: {root}/rerevved", problems)

    def test_zip_symlink_entry_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("windows")
            root = path.name.removesuffix(".zip")
            with zipfile.ZipFile(path, "w") as archive:
                info = zipfile.ZipInfo(f"{root}/rerevved.exe")
                info.external_attr = 0o120777 << 16
                archive.writestr(info, "/etc/passwd")
            problems = check(path, "windows")
        self.assertIn(f"link or special entry: {root}/rerevved.exe", problems)

    def test_tar_root_entry_is_required(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("linux")
            root = path.name.removesuffix(".tar.gz")
            with tarfile.open(path, "w:gz") as archive:
                info = tarfile.TarInfo(f"{root}/game")
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            self.assertIn(f"missing root directory entry: {root}", check(path, "linux"))

    def test_regular_file_named_like_a_directory_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("linux")
            root = path.name.removesuffix(".tar.gz")
            with tarfile.open(path, "w:gz") as archive:
                info = tarfile.TarInfo(f"{root}/game/")
                info.size = 1
                archive.addfile(info, io.BytesIO(b"x"))
            problems = check(path, "linux")
        self.assertIn("unexpected file: game", problems)
        self.assertIn("missing directory: game", problems)

    def test_noncanonical_entry_name_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("windows")
            root = path.name.removesuffix(".zip")
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(f"{root}/LICENSE.txt", "x")
                archive.writestr(f"{root}//LICENSE.txt", "x")
            problems = check(path, "windows")
        self.assertIn(f"unsafe entry name: {root}//LICENSE.txt", problems)

    def test_zip_directory_trickery_is_reported(self) -> None:
        cases = {
            "symlinked directory": (
                "{root}/game/",
                0o120777 << 16,
                "link or special entry",
            ),
            "double slash": ("{root}/game//", 0o40755 << 16, "unsafe entry name"),
            "root as file": ("{root}", 0o100644 << 16, "root entry is not a directory"),
        }
        for label, (template, attr, expected) in cases.items():
            with self.subTest(label), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / archive_name("windows")
                root = path.name.removesuffix(".zip")
                with zipfile.ZipFile(path, "w") as archive:
                    info = zipfile.ZipInfo(template.format(root=root))
                    info.external_attr = attr
                    archive.writestr(info, "")
                    archive.writestr(f"{root}/LICENSE.txt", "x")
                problems = check(path, "windows")
                self.assertTrue(any(p.startswith(expected) for p in problems), problems)

    def test_extension_must_match_platform(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            name = "rerevved-v1.0.0-windows-x64.tar.gz"
            path = Path(directory) / name
            write_tar(path, {"rerevved.exe"}, (), {"game"})
            self.assertIn(f"unexpected archive name: {name}", check(path, "windows"))

    def test_wrong_archive_name_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "other.zip"
            write_zip(path, {"rerevved.exe"}, (), {"game"})
            self.assertIn("unexpected archive name: other.zip", check(path, "windows"))

    def test_duplicate_entry_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / archive_name("windows")
            root = path.stem
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(f"{root}/LICENSE.txt", "x")
                with self.assertWarnsRegex(UserWarning, "Duplicate name"):
                    archive.writestr(f"{root}/LICENSE.txt", "x")
            self.assertIn(
                f"duplicate entry: {root}/LICENSE.txt", check(path, "windows")
            )

    def test_wrong_root_is_reported(self) -> None:
        for roots in (("other",), ("{root}", "other")):
            with self.subTest(roots=roots), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / archive_name("windows")
                names = sorted(name.format(root=path.stem) for name in roots)
                with zipfile.ZipFile(path, "w") as archive:
                    for name in names:
                        archive.writestr(f"{name}/LICENSE.txt", "x")
                self.assertEqual(
                    check(path, "windows"),
                    [f"archive must have the single root {path.stem}, found {names}"],
                )


if __name__ == "__main__":
    unittest.main()
