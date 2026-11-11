"""Release archive inventory checks."""

import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import package  # noqa: E402
from check_package_inventory import check  # noqa: E402
from package import (  # noqa: E402
    BUNDLED_MAPS,
    BUNDLED_MODS,
    KEYBOARD_ICON_DIR,
    KEYBOARD_ICON_FILES,
    reject_developer_build,
)

MOD_NAMES = {
    f"mods/{package_id}/{name}"
    for package_id in BUNDLED_MODS
    for name in ("mod.toml", "main.lua")
}
MAP_NAMES = {
    f"maps/{map_id}/{name}"
    for map_id in BUNDLED_MAPS
    for name in ("asset-pack.toml", "assets/map-pack/dlc.fpk")
}
NORMAL_FILES = (
    {
        "rerevved.exe",
        "rexglue.exe",
        "rexruntime.dll",
        "rexgpu-xenos.dll",
        "README.txt",
        "ONLINE.txt",
        "host-online.cmd",
        "host-online.ps1",
        "host-manual.cmd",
        "LICENSE.txt",
        "licenses/REXGLUE-LICENSE.txt",
        "game/",
        "asset-overrides/default_asset_order.txt",
        "asset-overrides/rerevved-logo/asset-pack.toml",
        "asset-overrides/rerevved-logo/assets/file-data/GFX_MainMenu_logo.dds",
        "licenses/xelu-prompts-Readme.txt",
        "licenses/xelu-prompts-PROVENANCE.md",
        "gamecontrollerdb.txt",
        "licenses/sdl-gamecontrollerdb-LICENSE",
    }
    | MOD_NAMES
    | MAP_NAMES
    | {f"{KEYBOARD_ICON_DIR}/{name}" for name in KEYBOARD_ICON_FILES}
)


def write_mods(root, kind="lua"):
    """Write the bundled Lua packages as scripts/assemble_mods.py produces them."""
    for package_id in BUNDLED_MODS:
        package = root / package_id
        package.mkdir(parents=True)
        (package / "mod.toml").write_text(
            "manifest_version = 2\n\n[mod]\n"
            f'id = "{package_id}"\nname = "{package_id}"\nversion = "1.0"\n\n'
            f'[payload]\nkind = "{kind}"\nentry = "main.lua"\n'
            'api = "rerevved.rules"\napi_version = 1\n',
            encoding="ascii",
        )
        (package / "main.lua").write_text("return {}\n", encoding="ascii")
    return root


def write_maps(root):
    """Write the bundled map packs as scripts/assemble_mods.py produces them."""
    for map_id in BUNDLED_MAPS:
        archive = root / map_id / "assets" / "map-pack"
        archive.mkdir(parents=True)
        (root / map_id / "asset-pack.toml").write_text(
            f'id = "{map_id}"\n', encoding="ascii"
        )
        (archive / "dlc.fpk").write_bytes(b"fpk")
    return root


class PackageTests(unittest.TestCase):
    def test_windows_archive_includes_default_logo_pack(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            output = root / "output"
            build.mkdir()
            for binary in (
                "rerevved.exe",
                "rexglue.exe",
                "rexruntime.dll",
                "rexgpu-xenos.dll",
            ):
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
                    "--mods-dir",
                    str(write_mods(root / "mods")),
                    "--maps-dir",
                    str(write_maps(root / "maps")),
                ],
                cwd=REPO,
                check=True,
                capture_output=True,
                text=True,
            )

            archive_path = next(output.glob("rerevved-v*-windows-x64.zip"))
            archive_root = archive_path.stem
            self.assertEqual(check(archive_path, "windows"), [])
            prefix = f"{archive_root}/asset-overrides/"
            with zipfile.ZipFile(archive_path) as archive:
                names = set(archive.namelist())
                self.assertEqual(
                    names, {f"{archive_root}/{name}" for name in NORMAL_FILES}
                )
                self.assertEqual(
                    archive.read(prefix + "default_asset_order.txt")
                    .decode("ascii")
                    .strip(),
                    "rerevved-logo",
                )
            keyboard = f"{archive_root}/{KEYBOARD_ICON_DIR}/"
            self.assertEqual(
                {n for n in names if n.startswith(keyboard)},
                {keyboard + name for name in KEYBOARD_ICON_FILES},
            )
            with zipfile.ZipFile(archive_path) as archive:
                self.assertEqual(
                    archive.read(keyboard + "Semicolon.dds"),
                    (
                        REPO / "assets" / KEYBOARD_ICON_DIR / "Semicolon.dds"
                    ).read_bytes(),
                )
                self.assertIn(
                    b"CC0",
                    archive.read(f"{archive_root}/licenses/xelu-prompts-Readme.txt"),
                )
                self.assertEqual(
                    archive.read(f"{archive_root}/gamecontrollerdb.txt"),
                    (
                        REPO
                        / "thirdparty"
                        / "sdl-gamecontrollerdb"
                        / "gamecontrollerdb.txt"
                    ).read_bytes(),
                )
            self.assertIn(prefix + "rerevved-logo/asset-pack.toml", names)
            self.assertIn(
                prefix + "rerevved-logo/assets/file-data/GFX_MainMenu_logo.dds",
                names,
            )

    def run_package(
        self, build, output, *extra, mods=None, maps=None, platform="windows"
    ):
        if mods is None:
            mods = output.parent / "bundled-mods"
            if not mods.exists():
                write_mods(mods)
        if maps is None:
            maps = output.parent / "bundled-maps"
            if not maps.exists():
                write_maps(maps)
        return subprocess.run(
            [
                sys.executable,
                str(REPO / "scripts/package.py"),
                "--platform",
                platform,
                "--build-dir",
                str(build),
                "--out-dir",
                str(output),
                "--mods-dir",
                str(mods),
                "--maps-dir",
                str(maps),
                *extra,
            ],
            cwd=REPO,
            capture_output=True,
            text=True,
        )

    def test_windows_relay_cli_sources(self):
        for location in ("build", "cache", "override"):
            with (
                self.subTest(location=location),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                build = root / "build"
                output = root / "output"
                sdk = root / "SDK with spaces"
                build.mkdir()
                (sdk / "bin").mkdir(parents=True)
                config = sdk / "lib/cmake/rexglue"
                config.mkdir(parents=True)
                for binary in ("rerevved.exe", "rexruntime.dll", "rexgpu-xenos.dll"):
                    (build / binary).write_bytes(binary.encode("ascii"))
                (build / "private.log").write_text("must not ship", encoding="ascii")
                extra = []
                if location == "build":
                    cli = build / "rexglue.exe"
                else:
                    cli = sdk / "bin/rexglue.exe"
                    if location == "cache":
                        (build / "CMakeCache.txt").write_text(
                            f"rexglue_DIR:PATH={config.as_posix()}\n", encoding="utf-8"
                        )
                    else:
                        extra = ["--sdk-bin", str(sdk / "bin")]
                        (build / "rexglue.exe").write_bytes(b"not the override")
                cli.write_bytes(b"selected relay CLI")
                result = self.run_package(build, output, *extra)
                self.assertEqual(result.returncode, 0, result.stderr)
                archive_path = next(output.glob("rerevved-v*-windows-x64.zip"))
                prefix = archive_path.stem + "/"
                with zipfile.ZipFile(archive_path) as archive:
                    self.assertEqual(
                        set(archive.namelist()),
                        {prefix + name for name in NORMAL_FILES},
                    )
                    self.assertEqual(
                        archive.read(prefix + "rexglue.exe"), cli.read_bytes()
                    )
                    for name in (
                        "host-online.cmd",
                        "host-online.ps1",
                        "host-manual.cmd",
                        "ONLINE.txt",
                    ):
                        self.assertEqual(
                            archive.read(prefix + name),
                            (REPO / "scripts/packaging" / name).read_bytes(),
                        )
                self.assertTrue((output / "pkg" / archive_path.stem).is_dir())

    def test_windows_package_reports_missing_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_package(root, root / "output")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Windows package needs rexglue.exe", result.stderr)
            result = self.run_package(
                root, root / "output", "--sdk-bin", str(root / "missing")
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--sdk-bin has no rexglue.exe", result.stderr)
            self.assertFalse((root / "output").exists())

    def make_build(self, root, platform="windows"):
        build = root / "build"
        build.mkdir()
        names = {
            "windows": (
                "rerevved.exe",
                "rexglue.exe",
                "rexruntime.dll",
                "rexgpu-xenos.dll",
            ),
            "linux": ("rerevved", "librexruntime.so", "librexgpu-xenos.so"),
        }[platform]
        for binary in names:
            (build / binary).write_bytes(binary.encode("ascii"))
        return build

    def test_developer_build_directory_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = self.make_build(root, "linux")
            (build / "CMakeCache.txt").write_text(
                "REREVVED_DEVELOPER:BOOL=ON\n", encoding="ascii"
            )
            result = self.run_package(build, root / "output", platform="linux")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("developer build", result.stderr)
            self.assertFalse((root / "output").exists())

    def test_developer_cache_values_are_rejected(self):
        for value in ("ON", "TRUE", "1", "-1", "1.0", "00", "notfound", " OFF"):
            with (
                self.subTest(value=value),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                (root / "CMakeCache.txt").write_text(
                    f"REREVVED_DEVELOPER:BOOL={value}\n", encoding="ascii"
                )
                with self.assertRaisesRegex(SystemExit, "developer build"):
                    reject_developer_build(root)

    def test_release_cache_values_are_accepted(self):
        for value in ("OFF", "0", "FALSE", ""):
            with (
                self.subTest(value=value),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                (root / "CMakeCache.txt").write_text(
                    f"REREVVED_DEVELOPER:BOOL={value}\n", encoding="ascii"
                )
                reject_developer_build(root)

    def test_bundled_mods_ship_disabled_in_both_archives(self):
        for platform in ("windows", "linux"):
            with (
                self.subTest(platform=platform),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = Path(directory)
                build = self.make_build(root, platform)
                result = self.run_package(build, root / "output", platform=platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                if platform == "windows":
                    archive_path = next((root / "output").glob("*.zip"))
                    with zipfile.ZipFile(archive_path) as archive:
                        names = set(archive.namelist())
                    prefix = archive_path.stem + "/"
                else:
                    archive_path = next((root / "output").glob("*.tar.gz"))
                    with tarfile.open(archive_path) as archive:
                        names = {m.name for m in archive.getmembers() if m.isfile()}
                    prefix = archive_path.name.removesuffix(".tar.gz") + "/"
                for name in MOD_NAMES:
                    self.assertIn(prefix + name, names)
                self.assertFalse(any(name.endswith("mod_order.txt") for name in names))

    def test_bundled_mods_are_validated(self):
        first = BUNDLED_MODS[0]
        exact = "must contain exactly the bundled mods"
        unexpected = "unexpected entry in bundled mod"
        cases = {
            "missing": (lambda mods: shutil.rmtree(mods / first), exact),
            "unknown": (lambda mods: (mods / "unbundled-mod").mkdir(), exact),
            "code": (lambda mods: (mods / first / "code").mkdir(), unexpected),
            "assets": (lambda mods: (mods / first / "assets").mkdir(), unexpected),
            "binary": (
                lambda mods: (mods / first / "mod.dll").write_bytes(b"MZ"),
                unexpected,
            ),
            "bytecode": (
                lambda mods: (mods / first / "main.lua").write_bytes(b"\x1bLuaU"),
                "main.lua is not text",
            ),
            "no-script": (
                lambda mods: (mods / first / "main.lua").unlink(),
                "lacks main.lua",
            ),
            "identity": (
                lambda mods: (mods / first / "mod.toml").write_text(
                    (mods / BUNDLED_MODS[1] / "mod.toml").read_text(encoding="ascii"),
                    encoding="ascii",
                ),
                "is not a Lua rules package",
            ),
        }
        for name, (damage, message) in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = self.make_build(root)
                mods = write_mods(root / "mods")
                damage(mods)
                result = self.run_package(build, root / "output", mods=mods)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertFalse((root / "output").exists())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = self.make_build(root)
            mods = write_mods(root / "mods", kind="native")
            result = self.run_package(build, root / "output", mods=mods)
            self.assertIn("is not a Lua rules package", result.stderr)

    def test_bundled_maps_are_validated(self):
        first = BUNDLED_MAPS[0]
        cases = {
            "missing": lambda maps: (maps / first / "asset-pack.toml").unlink(),
            "extra": lambda maps: (maps / first / "extra.txt").write_text("x"),
            "empty": lambda maps: (
                maps / first / "assets/map-pack/dlc.fpk"
            ).write_bytes(b""),
        }
        for name, damage in cases.items():
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = self.make_build(root)
                maps = write_maps(root / "maps")
                damage(maps)
                result = self.run_package(build, root / "output", maps=maps)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"bundled map {first}", result.stderr)
                self.assertNotIn("Traceback", result.stderr)
                self.assertFalse((root / "output").exists())


class MacOSPackageTests(unittest.TestCase):
    def make_runtime(self, root):
        for name in package.BINARIES["macos"]:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"Mach-O fixture")
        icd = root / package.MACOS_ICD
        icd.parent.mkdir(parents=True, exist_ok=True)
        icd.write_text(
            '{"ICD":{"library_path":"../../../lib/libMoltenVK.dylib"}}',
            encoding="ascii",
        )

    def test_macos_package_keeps_only_allowlisted_runtime_and_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            output = root / "output"
            notices = root / "notices"
            self.make_runtime(build)
            (build / "private.log").write_text("excluded", encoding="ascii")
            notices.mkdir()
            for filename in package.MACOS_LICENSES:
                (notices / filename).write_text(filename, encoding="ascii")
            argv = [
                "package.py",
                "--platform",
                "macos",
                "--build-dir",
                str(build),
                "--out-dir",
                str(output),
                "--mods-dir",
                str(write_mods(root / "mods")),
                "--maps-dir",
                str(write_maps(root / "maps")),
                "--macos-licenses",
                str(notices),
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(package, "prepare_macos_runtime") as prepare,
            ):
                package.main()
            archive_path = next(output.glob("rerevved-v*-macos-arm64.tar.gz"))
            stage = output / "pkg" / archive_path.name.removesuffix(".tar.gz")
            prepare.assert_called_once_with(stage)
            with tarfile.open(archive_path) as archive:
                prefix = archive_path.name.removesuffix(".tar.gz") + "/"
                files = {
                    member.name.removeprefix(prefix)
                    for member in archive.getmembers()
                    if member.isfile()
                }
                self.assertEqual(files, package.expected_entries("macos")[1])
                self.assertIn(
                    b"not notarized", archive.extractfile(prefix + "README.txt").read()
                )
                for filename in package.MACOS_LICENSES:
                    self.assertEqual(
                        archive.extractfile(prefix + "licenses/" + filename).read(),
                        filename.encode("ascii"),
                    )

    def test_relocation_precedes_signing_and_executable_is_signed_last(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_runtime(root)
            with (
                mock.patch.object(
                    package,
                    "macos_dependencies",
                    return_value=["/sdk/lib/librexruntime.dylib"],
                ),
                mock.patch.object(
                    package, "macos_rpaths", return_value=["/opt/homebrew/lib"]
                ),
                mock.patch.object(package, "macos_tool") as tool,
                mock.patch.object(package, "check_macos_linkage") as validate,
            ):
                package.prepare_macos_runtime(root)
            calls = [call.args for call in tool.call_args_list]
            signatures = [i for i, call in enumerate(calls) if call[0] == "codesign"]
            edits = [
                i for i, call in enumerate(calls) if call[0] == "install_name_tool"
            ]
            self.assertLess(max(edits), min(signatures))
            self.assertEqual(len(signatures), len(package.BINARIES["macos"]))
            self.assertEqual(calls[-1][-1], str(root / "rerevved"))
            self.assertEqual(
                calls[-1][1:-1], ("--force", "--sign", "-", "--timestamp=none")
            )
            relative = "@loader_path/../../librexruntime.dylib"
            self.assertTrue(any(relative in call for call in calls))
            validate.assert_called_once_with(root)

    def test_macos_linkage_rejects_external_paths_wrong_arch_and_newer_os(self):
        cases = (
            (
                "valid",
                "@loader_path/librexruntime.dylib",
                "arm64",
                "13.3",
                "@loader_path",
            ),
            (
                "external",
                "/opt/homebrew/lib/librexruntime.dylib",
                "arm64",
                "13.3",
                "@loader_path",
            ),
            (
                "arch",
                "@loader_path/librexruntime.dylib",
                "x86_64",
                "13.3",
                "@loader_path",
            ),
            ("os", "@loader_path/librexruntime.dylib", "arm64", "14.0", "@loader_path"),
            ("rpath", "@loader_path/librexruntime.dylib", "arm64", "13.3", "/sdk/lib"),
        )
        for label, dependency, arch, minimum, rpath in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.make_runtime(root)

                def tool(*command):
                    if command[0] == "lipo":
                        return arch + "\n"
                    if command[:2] == ("otool", "-l"):
                        return (
                            "cmd LC_BUILD_VERSION\n"
                            f"    minos {minimum}\n"
                            "cmd LC_RPATH\n    cmdsize 32\n"
                            f"    path {rpath} (offset 12)\n"
                        )
                    if command[:2] == ("otool", "-L"):
                        path = Path(command[-1])
                        identity = (
                            f"\t@rpath/{path.name} (compatibility version 1.0.0)\n"
                            if path.suffix == ".dylib"
                            else ""
                        )
                        linked = (
                            dependency
                            if path.name == "rerevved"
                            else "/usr/lib/libSystem.B.dylib"
                        )
                        return (
                            f"{path}:\n{identity}\t{linked} "
                            "(compatibility version 1.0.0)\n"
                        )
                    return ""

                with mock.patch.object(package, "macos_tool", side_effect=tool):
                    if label == "valid":
                        package.check_macos_linkage(root)
                        icd = root / package.MACOS_ICD
                        icd.write_text(
                            json.dumps({"ICD": {"library_path": "/external/MoltenVK"}}),
                            encoding="ascii",
                        )
                        with self.assertRaisesRegex(SystemExit, "ICD"):
                            package.check_macos_linkage(root)
                    else:
                        with self.assertRaises(SystemExit):
                            package.check_macos_linkage(root)


class AssembleModsTests(unittest.TestCase):
    """Rejections that keep a release from bundling the wrong mods."""

    def git(self, root, *args):
        subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)

    def make_checkout(self, root, title_commit):
        checkout = root / "mods-src"
        checkout.mkdir()
        self.git(checkout, "init", "-q")
        (checkout / "rerevved-api.lock.json").write_text(
            f'{{"commit": "{title_commit}"}}\n', encoding="ascii"
        )
        self.git(checkout, "add", "rerevved-api.lock.json")
        self.git(
            checkout,
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            "commit",
            "-q",
            "-m",
            "lock",
        )
        commit = subprocess.run(
            ["git", "-C", str(checkout), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        return checkout, commit

    def run_assemble(self, root, checkout, locked):
        lock = root / "rerevved-mods.lock.json"
        lock.write_text(f'{{"commit": "{locked}"}}\n', encoding="ascii")
        script = (
            "import sys, pathlib; sys.path.insert(0, sys.argv[1]); "
            "import assemble_mods; assemble_mods.LOCK = pathlib.Path(sys.argv[2]); "
            "sys.argv = ['assemble_mods', '--mods-checkout', sys.argv[3], "
            "'--out', sys.argv[4], '--maps-out', sys.argv[5]]; assemble_mods.main()"
        )
        return subprocess.run(
            [
                sys.executable,
                "-c",
                script,
                str(REPO / "scripts"),
                str(lock),
                str(checkout),
                str(root / "out"),
                str(root / "maps-out"),
            ],
            capture_output=True,
            text=True,
        )

    def title_head(self):
        return subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def test_rejects_other_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout, _ = self.make_checkout(root, self.title_head())
            result = self.run_assemble(root, checkout, "0" * 40)
            self.assertIn("must be rerevved-mods at " + "0" * 40, result.stderr)
            self.assertFalse((root / "out").exists())

    def test_rejects_untracked_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout, commit = self.make_checkout(root, self.title_head())
            (checkout / "extra.dds").write_bytes(b"x")
            result = self.run_assemble(root, checkout, commit)
            self.assertIn("checkout has local changes", result.stderr)

    def test_rejects_unknown_title_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout, commit = self.make_checkout(root, "f" * 40)
            result = self.run_assemble(root, checkout, commit)
            self.assertIn("is not in this checkout's history", result.stderr)

    def test_rejects_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout, commit = self.make_checkout(root, self.title_head())
            (root / "out").mkdir()
            result = self.run_assemble(root, checkout, commit)
            self.assertIn("output already exists", result.stderr)

    def test_host_launchers_are_ascii_crlf(self):
        for name in ("host-online.cmd", "host-manual.cmd"):
            data = (REPO / "scripts/packaging" / name).read_bytes()
            data.decode("ascii")
            self.assertIn(b"\r\n", data)
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""))
            self.assertNotIn(b"\r", data.replace(b"\r\n", b""))


if __name__ == "__main__":
    unittest.main()
