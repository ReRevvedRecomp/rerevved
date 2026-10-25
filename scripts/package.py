"""Build a ReRevved release archive from an explicit file allowlist."""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tomllib
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent

MACOS_LIBRARIES = {
    "librexruntime.dylib": "librexruntime.dylib",
    "librexgpu-xenos.dylib": "librexgpu-xenos.dylib",
    "libTracyClient.dylib": "libTracyClient.dylib",
    "libvulkan.1.dylib": "vulkan/lib/libvulkan.1.dylib",
    "libMoltenVK.dylib": "vulkan/lib/libMoltenVK.dylib",
}
MACOS_ICD = "vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
MACOS_LICENSES = (
    "MOLTENVK-LICENSE.txt",
    "VULKAN-LOADER-LICENSE.txt",
    "TRACY-LICENSE.txt",
    "SPIRV-CROSS-LICENSE.txt",
    "CEREAL-LICENSE.txt",
)

BINARIES = {
    "windows": ["rerevved.exe", "rexruntime.dll", "rexgpu-xenos.dll"],
    "linux": ["rerevved", "librexruntime.so", "librexgpu-xenos.so"],
    "macos": ["rerevved", *MACOS_LIBRARIES.values()],
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
    if platform == "macos":
        files |= {MACOS_ICD, *(f"licenses/{name}" for name in MACOS_LICENSES)}
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
    default_platform = {
        "win32": "windows",
        "darwin": "macos",
    }.get(sys.platform, "linux")
    default_build = {
        "windows": "out/build/win-amd64-release",
        "linux": "out/build/linux-amd64-release",
        "macos": "out/build/mac-arm64-release",
    }
    parser.add_argument(
        "--platform", choices=("windows", "linux", "macos"), default=default_platform
    )
    parser.add_argument("--arch", default=None)
    parser.add_argument(
        "--macos-licenses", type=pathlib.Path, help="pinned macOS dependency notices"
    )
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
    args.arch = args.arch or ("arm64" if args.platform == "macos" else "x64")
    if args.platform == "macos" and args.arch != "arm64":
        parser.error("macOS packages support arm64 only")
    if args.platform == "macos" and args.macos_licenses is None:
        parser.error("macOS packages require --macos-licenses")
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


def macos_tool(*command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"error: {command[0]} failed:\n{error.output}") from error


def macos_dependencies(path):
    # A dylib's first entry is its own LC_ID_DYLIB, not a dependency.
    lines = macos_tool("otool", "-L", str(path)).splitlines()
    return [
        line.strip().split(" (", 1)[0]
        for line in lines[2 if path.suffix == ".dylib" else 1 :]
    ]


def macos_rpaths(path):
    commands = macos_tool("otool", "-l", str(path))
    return re.findall(
        r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)", commands
    )


def macos_system_library(name):
    return name.startswith(("/usr/lib/", "/System/Library/"))


def check_macos_linkage(root):
    """Check every packaged Mach-O, including the runtime-loaded Vulkan stack."""
    root = root.resolve()
    allowed = {root / name for name in MACOS_LIBRARIES.values()}
    for relative in BINARIES["macos"]:
        path = root / relative
        if macos_tool("lipo", "-archs", str(path)).strip() != "arm64":
            raise SystemExit(f"error: expected arm64 Mach-O: {relative}")
        commands = macos_tool("otool", "-l", str(path))
        minimums = re.findall(r"^\s*minos (\d+(?:\.\d+)+)", commands, re.M)
        minimums += re.findall(
            r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version (\d+(?:\.\d+)+)",
            commands,
        )
        if not minimums or any(
            tuple(map(int, version.split("."))) > (13, 3, 0) for version in minimums
        ):
            raise SystemExit(f"error: macOS deployment target exceeds 13.3: {relative}")
        if macos_rpaths(path) != ["@loader_path"]:
            raise SystemExit(f"error: nonportable RPATH in {relative}")
        for dependency in macos_dependencies(path):
            if macos_system_library(dependency):
                continue
            if not dependency.startswith("@loader_path/"):
                raise SystemExit(
                    f"error: external dependency in {relative}: {dependency}"
                )
            resolved = (
                path.parent / dependency.removeprefix("@loader_path/")
            ).resolve()
            if resolved not in allowed or not resolved.is_file():
                raise SystemExit(f"error: missing package dependency: {dependency}")
        macos_tool("codesign", "--verify", "--strict", str(path))
    icd = json.loads((root / MACOS_ICD).read_text(encoding="utf-8"))
    if icd.get("ICD", {}).get("library_path") != "../../../lib/libMoltenVK.dylib":
        raise SystemExit("error: MoltenVK ICD must use the packaged relative library")


def prepare_macos_runtime(stage):
    """Relocate before signing so signatures cover the final bytes."""
    for relative in BINARIES["macos"]:
        path = stage / relative
        path.chmod(0o755)
        for dependency in macos_dependencies(path):
            if macos_system_library(dependency):
                continue
            target = MACOS_LIBRARIES.get(pathlib.PurePosixPath(dependency).name)
            if target is None:
                raise SystemExit(f"error: unbundled macOS dependency: {dependency}")
            loader_relative = pathlib.Path(
                os.path.relpath(stage / target, path.parent)
            ).as_posix()
            macos_tool(
                "install_name_tool",
                "-change",
                dependency,
                f"@loader_path/{loader_relative}",
                str(path),
            )
        if path.suffix == ".dylib":
            macos_tool("install_name_tool", "-id", f"@rpath/{path.name}", str(path))
        for rpath in dict.fromkeys(macos_rpaths(path)):
            macos_tool("install_name_tool", "-delete_rpath", rpath, str(path))
        macos_tool("install_name_tool", "-add_rpath", "@loader_path", str(path))
    for relative in [*MACOS_LIBRARIES.values(), "rerevved"]:
        macos_tool(
            "codesign",
            "--force",
            "--sign",
            "-",
            "--timestamp=none",
            str(stage / relative),
        )
    check_macos_linkage(stage)


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
        if args.platform == "macos":
            (stage / binary).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, stage / binary)
    if missing:
        raise SystemExit("error: missing release binaries:\n  " + "\n  ".join(missing))

    readme = "README-macos.txt" if args.platform == "macos" else "README.txt"
    shutil.copy2(REPO / "scripts" / "packaging" / readme, stage / "README.txt")
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

    if args.platform == "macos":
        for filename in MACOS_LICENSES:
            source = args.macos_licenses / filename
            if not source.is_file():
                raise SystemExit(f"error: missing macOS license: {source}")
            shutil.copy2(source, licenses / filename)
        source = args.build_dir / MACOS_ICD
        destination = stage / MACOS_ICD
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        prepare_macos_runtime(stage)

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
