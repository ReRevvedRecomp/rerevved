"""Build a ReRevved release archive from an explicit file allowlist."""

import argparse
import pathlib
import re
import shutil
import sys
import tarfile
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent

BINARIES = {
    "windows": ["rerevved.exe", "rexruntime.dll", "rexgpu-xenos.dll", "TracyClient.dll"],
    "linux": ["rerevved", "librexruntime.so", "librexgpu-xenos.so", "libTracyClient.so"],
}

FORBIDDEN_SUFFIXES = {".xex", ".xexp", ".iso", ".sve", ".log", ".trace"}


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
    parser.add_argument("--platform", choices=("windows", "linux"), default=default_platform)
    parser.add_argument("--arch", default="x64")
    parser.add_argument("--build-dir", type=pathlib.Path, default=None)
    parser.add_argument("--out-dir", type=pathlib.Path, default=REPO / "out")
    args = parser.parse_args()
    if args.build_dir is None:
        args.build_dir = REPO / default_build[args.platform]
    return args


def main():
    args = parse_args()
    version = read_version()
    name = f"rerevved-v{version}-{args.platform}-{args.arch}"

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
    shutil.copy2(REPO / "LICENSE", stage / "LICENSE.txt")

    licenses = stage / "licenses"
    licenses.mkdir()
    shutil.copy2(REPO / "REXGLUE-LICENSE.txt", licenses / "REXGLUE-LICENSE.txt")

    # Preserve the selector's target without staging retail content.
    (stage / "game").mkdir()

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
