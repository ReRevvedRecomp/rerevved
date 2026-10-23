"""Build a ReRevved release archive from an explicit file allowlist."""

import argparse
import pathlib
import re
import shutil
import sys
import tarfile
import tomllib
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent

BINARIES = {
    "windows": ["rerevved.exe", "rexruntime.dll", "rexgpu-xenos.dll"],
    "linux": ["rerevved", "librexruntime.so", "librexgpu-xenos.so"],
}

FORBIDDEN_SUFFIXES = {".xex", ".xexp", ".iso", ".sve", ".log", ".trace"}

BUNDLED_ASSET_FILES = (
    "default_asset_order.txt",
    "rerevved-logo/asset-pack.toml",
    "rerevved-logo/assets/file-data/GFX_MainMenu_logo.dds",
)

# First-party Lua mods shipped disabled under mods/.
BUNDLED_MODS = (
    "cataphracts-defense",
    "chinese-library-gold",
    "founding-militia",
    "hills-production",
    "hoplite-loyalty",
    "jaguar-woodsman",
    "keshik-movement",
    "mongol-horseback",
    "stonehenge-religion",
)
# Bundled mods are declarations only; a mod that needs assets must extend this.
MOD_FILES = ("mod.toml", "main.lua")


def expected_entries(platform):
    """Return the (directories, files) an archive holds, relative to its root."""
    files = set(BINARIES[platform])
    files |= {"README.txt", "LICENSE.txt", "licenses/REXGLUE-LICENSE.txt"}
    if platform == "windows":
        files |= {
            "rexglue.exe",
            "ONLINE.txt",
            "host-online.cmd",
            "host-online.ps1",
            "host-manual.cmd",
        }
    files |= {f"asset-overrides/{name}" for name in BUNDLED_ASSET_FILES}
    for package_id in BUNDLED_MODS:
        files |= {f"mods/{package_id}/{name}" for name in MOD_FILES}
    directories = {"game"}
    for name in files:
        directories |= {str(parent) for parent in pathlib.PurePosixPath(name).parents}
    directories.discard(".")
    return directories, files


def read_version():
    text = (REPO / "CMakeLists.txt").read_text(encoding="ascii")
    match = re.search(r"^project\(rerevved VERSION (\d+\.\d+\.\d+) ", text, re.M)
    if not match:
        raise SystemExit("error: project version not found in CMakeLists.txt")
    return match.group(1)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    default_platform = "windows" if sys.platform == "win32" else "linux"
    default_build = {
        "windows": "out/build/win-amd64-release",
        "linux": "out/build/linux-amd64-release",
    }
    parser.add_argument(
        "--platform", choices=("windows", "linux"), default=default_platform
    )
    parser.add_argument("--arch", default="x64")
    parser.add_argument("--build-dir", type=pathlib.Path, default=None)
    parser.add_argument("--out-dir", type=pathlib.Path, default=REPO / "out")
    parser.add_argument(
        "--sdk-bin", type=pathlib.Path, help="SDK bin directory containing rexglue.exe"
    )
    parser.add_argument(
        "--mods-dir",
        type=pathlib.Path,
        required=True,
        help="assembled bundled mods from scripts/assemble_mods.py",
    )
    args = parser.parse_args()
    if args.build_dir is None:
        args.build_dir = REPO / default_build[args.platform]
    return args


def find_rexglue(build_dir, sdk_bin):
    if sdk_bin is not None:
        source = sdk_bin / "rexglue.exe"
        if source.is_file():
            return source
        raise SystemExit(f"error: --sdk-bin has no rexglue.exe: {sdk_bin}")

    source = build_dir / "rexglue.exe"
    if source.is_file():
        return source
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        match = re.search(
            r"^rexglue_DIR:[^=]+=([^\r\n]+)$",
            cache.read_text(encoding="utf-8"),
            re.M,
        )
        if match:
            # The installed package lives at <sdk>/lib/cmake/rexglue.
            source = pathlib.Path(match.group(1)) / "../../../bin/rexglue.exe"
            if source.is_file():
                return source.resolve()
    raise SystemExit(
        "error: Windows package needs rexglue.exe in the build directory or the "
        "SDK install recorded by CMakeCache.txt (rexglue_DIR); "
        "use --sdk-bin <matching-sdk>/bin"
    )


def reject_developer_build(build_dir):
    """Refuse a build directory configured with REREVVED_DEVELOPER, which holds recorders."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return
    match = re.search(
        r"^REREVVED_DEVELOPER:[A-Z]+=([^\r\n]*)$",
        cache.read_text(encoding="utf-8"),
        re.M,
    )
    # CMake treats every value except its false constants as true.
    value = match.group(1) if match else ""
    if not (
        re.fullmatch(r"(?i)|0|off|no|false|n|ignore", value)
        or re.fullmatch(r"NOTFOUND|.*-NOTFOUND", value)
    ):
        raise SystemExit(
            f"error: {build_dir} is a developer build (REREVVED_DEVELOPER=ON); "
            "package a release build"
        )


def is_link(path):
    # Junctions are not symlinks to pathlib on Python 3.11.
    attributes = getattr(path.lstat(), "st_file_attributes", 0)
    return path.is_symlink() or bool(attributes & 0x400)


def check_mod(source, package_id):
    """Reject anything but a Lua package with the expected identity."""
    if is_link(source) or not source.is_dir():
        raise SystemExit(f"error: bundled mod is not a plain directory: {source}")
    for path in source.iterdir():
        if is_link(path) or path.name not in MOD_FILES or not path.is_file():
            raise SystemExit(f"error: unexpected entry in bundled mod: {path}")
    for name in MOD_FILES:
        if not (source / name).is_file():
            raise SystemExit(f"error: bundled mod {package_id} lacks {name}")
    # The SDK loads source text only; reject precompiled chunks and binaries.
    script = (source / "main.lua").read_bytes()
    if script.startswith(b"\x1bLua") or b"\0" in script:
        raise SystemExit(f"error: bundled mod {package_id} main.lua is not text")
    manifest = tomllib.loads((source / "mod.toml").read_text(encoding="utf-8"))
    mod = manifest.get("mod", {})
    payload = manifest.get("payload", {})
    if (
        manifest.get("manifest_version") != 2
        or mod.get("id") != package_id
        or payload.get("kind") != "lua"
        or payload.get("entry") != "main.lua"
        or payload.get("api") != "rerevved.rules"
    ):
        raise SystemExit(f"error: bundled mod {package_id} is not a Lua rules package")


def check_mods(mods_dir):
    if not mods_dir.exists() or is_link(mods_dir) or not mods_dir.is_dir():
        raise SystemExit(f"error: --mods-dir is not a directory: {mods_dir}")
    found = sorted(path.name for path in mods_dir.iterdir())
    if found != sorted(BUNDLED_MODS):
        raise SystemExit(
            "error: --mods-dir must contain exactly the bundled mods: "
            + ", ".join(BUNDLED_MODS)
        )
    for package_id in BUNDLED_MODS:
        check_mod(mods_dir / package_id, package_id)


def main():
    args = parse_args()
    version = read_version()
    reject_developer_build(args.build_dir)
    name = f"rerevved-v{version}-{args.platform}-{args.arch}"
    # Windows players host the private System Link relay from the package.
    online = args.platform == "windows"
    if online:
        rexglue = find_rexglue(args.build_dir, args.sdk_bin)
    check_mods(args.mods_dir)

    stage = args.out_dir / "pkg" / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    missing = []
    for binary in BINARIES[args.platform]:
        source = args.build_dir / binary
        if not source.is_file():
            missing.append(str(source))
            continue
        shutil.copy2(source, stage / binary)
    if missing:
        raise SystemExit("error: missing release binaries:\n  " + "\n  ".join(missing))

    shutil.copy2(REPO / "scripts" / "packaging" / "README.txt", stage / "README.txt")
    if online:
        shutil.copy2(rexglue, stage / "rexglue.exe")
        for filename in (
            "ONLINE.txt",
            "host-online.cmd",
            "host-online.ps1",
            "host-manual.cmd",
        ):
            shutil.copy2(REPO / "scripts" / "packaging" / filename, stage / filename)
    shutil.copy2(REPO / "LICENSE", stage / "LICENSE.txt")

    licenses = stage / "licenses"
    licenses.mkdir()
    shutil.copy2(REPO / "REXGLUE-LICENSE.txt", licenses / "REXGLUE-LICENSE.txt")

    # Preserve the selector's target without staging retail content.
    (stage / "game").mkdir()

    bundled_assets = stage / "asset-overrides"
    for relative in BUNDLED_ASSET_FILES:
        source = REPO / "assets" / "asset-overrides" / relative
        if not source.is_file():
            raise SystemExit(f"error: missing bundled asset file: {source}")
        destination = bundled_assets / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    # No mod_order.txt ships, so every bundled mod starts disabled.
    for package_id in BUNDLED_MODS:
        shutil.copytree(args.mods_dir / package_id, stage / "mods" / package_id)

    for staged in stage.rglob("*"):
        if staged.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise SystemExit(f"error: forbidden file staged: {staged}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    if args.platform == "windows":
        archive_path = args.out_dir / f"{name}.zip"
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(stage.rglob("*")):
                relative = f"{name}/{path.relative_to(stage).as_posix()}"
                if path.is_dir():
                    if not any(path.iterdir()):
                        archive.writestr(relative + "/", "")
                else:
                    archive.write(path, relative)
    else:
        archive_path = args.out_dir / f"{name}.tar.gz"
        with tarfile.open(archive_path, "w:gz") as archive:
            archive.add(stage, arcname=name)

    print(f"packaged: {archive_path}")


if __name__ == "__main__":
    main()
