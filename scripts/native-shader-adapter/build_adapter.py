"""Reconstruct and build the title-owned raw shader adapter."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

UPSTREAM_REVISION = "339af41df2c23dbe3256c1c377716b81a0e0fe6b"
EXPECTED_RAW_ADAPTER_SHA256 = (
    "f145fdd0e4d0b61c343c36b7d37a5bd8337192bd2db27109085950312f627c97"
)
SUBMODULE_REVISIONS = {
    "thirdparty/dxc-bin": "737ac9f51c3d06f0cd1ee7e14dc065bafd52310d",
    "thirdparty/fmt": "873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f",
    "thirdparty/smol-v": "9dd54c379ac29fa148cb1b829bb939ba7381d8f4",
    "thirdparty/xxHash": "2bf8313b934633b2a5b7e8fd239645b85e10c852",
    "thirdparty/zstd": "f7a8bb1263448e5028aceeba606a08fe3809550f",
}
RECONSTRUCTION_FILES = (
    "XenosRecomp/constant_table.h",
    "XenosRecomp/dxc_compiler.cpp",
    "XenosRecomp/dxc_compiler.h",
    "XenosRecomp/pch.h",
    "XenosRecomp/shader.h",
    "XenosRecomp/shader_code.h",
    "XenosRecomp/shader_common.h",
    "XenosRecomp/shader_recompiler.cpp",
    "XenosRecomp/shader_recompiler.h",
)
THIRDPARTY_DIRECTORIES = (
    "thirdparty/dxc-bin",
    "thirdparty/fmt",
    "thirdparty/smol-v",
    "thirdparty/xxHash",
    "thirdparty/zstd",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_git(repo: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def git_blob(repo: Path, revision: str, relative: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"{revision}:{relative}"],
        check=True,
        capture_output=True,
    )
    return result.stdout


def write_blob(root: Path, relative: str, content: bytes) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return path


def archive_tree(source: Path, revision: str, destination: Path) -> None:
    archive = subprocess.run(
        ["git", "-C", str(source), "archive", "--format=tar", revision],
        check=True,
        capture_output=True,
    ).stdout
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tar:
        tar.extractall(destination, filter="data")


def apply_unified_patch(root: Path, patch_path: Path) -> None:
    """Apply a bounded unified diff without changing any Git repository."""
    lines = patch_path.read_text(encoding="utf-8").splitlines(keepends=True)
    index = 0
    while index < len(lines):
        if not lines[index].startswith("diff --git "):
            index += 1
            continue
        index += 1
        target = None
        while index < len(lines) and not lines[index].startswith("diff --git "):
            line = lines[index]
            if line.startswith("+++ b/"):
                target = line[6:].rstrip("\r\n")
                break
            index += 1
        if target is None:
            raise ValueError(f"patch has no target: {patch_path}")
        path = root / target
        original = path.read_text(encoding="utf-8").splitlines(keepends=True)
        result: list[str] = []
        source_index = 0
        while index < len(lines) and not lines[index].startswith("diff --git "):
            line = lines[index]
            if not line.startswith("@@ "):
                index += 1
                continue
            match = re.match(r"@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
            if match is None:
                raise ValueError(f"invalid patch hunk: {line.rstrip()}")
            old_start = int(match.group(1)) - 1
            result.extend(original[source_index:old_start])
            source_index = old_start
            index += 1
            while index < len(lines):
                hunk_line = lines[index]
                if hunk_line.startswith("@@ ") or hunk_line.startswith("diff --git "):
                    break
                index += 1
                if hunk_line == "\n":
                    continue
                if hunk_line.startswith("\\"):
                    continue
                if not hunk_line:
                    raise ValueError(f"invalid empty patch line: {patch_path}")
                marker = hunk_line[0]
                payload = hunk_line[1:]
                if marker == " ":
                    if (
                        source_index >= len(original)
                        or original[source_index] != payload
                    ):
                        raise ValueError(f"patch context mismatch in {path}")
                    result.append(original[source_index])
                    source_index += 1
                elif marker == "-":
                    if (
                        source_index >= len(original)
                        or original[source_index] != payload
                    ):
                        raise ValueError(f"patch removal mismatch in {path}")
                    source_index += 1
                elif marker == "+":
                    result.append(payload)
                else:
                    raise ValueError(f"invalid patch marker in {path}: {marker!r}")
        result.extend(original[source_index:])
        path.write_text("".join(result), encoding="utf-8", newline="")
    if index == 0:
        raise ValueError(f"patch has no diff entries: {patch_path}")


def reconstruct(source_repo: Path, build_root: Path) -> dict:
    source_root = build_root / "source"
    if build_root.exists():
        raise ValueError(f"build root must be fresh: {build_root}")
    build_root.mkdir(parents=True)
    if run_git(source_repo, "rev-parse", "HEAD") != UPSTREAM_REVISION:
        raise ValueError(
            "source repository revision does not match the pinned upstream"
        )
    source_root.mkdir()
    for relative in RECONSTRUCTION_FILES:
        write_blob(
            source_root, relative, git_blob(source_repo, UPSTREAM_REVISION, relative)
        )
    write_blob(
        source_root,
        "LICENSE.md",
        git_blob(source_repo, UPSTREAM_REVISION, "LICENSE.md"),
    )
    for relative in THIRDPARTY_DIRECTORIES:
        source = source_repo / relative
        expected = SUBMODULE_REVISIONS[
            relative.split("/")[0] + "/" + relative.split("/")[1]
        ]
        if run_git(source, "rev-parse", "HEAD") != expected:
            raise ValueError(f"submodule revision mismatch: {relative}")
        archive_tree(source, expected, source_root / relative)
    owned = Path(__file__).resolve().parent
    shutil.copy2(owned / "CMakeLists.txt", source_root / "CMakeLists.txt")
    shutil.copy2(owned / "raw_adapter.cpp", source_root / "XenosRecomp/raw_adapter.cpp")
    shutil.copy2(owned / "shader_common.h", build_root / "shader_common.h")

    apply_unified_patch(source_root, owned / "raw-interface.patch")
    reconstruction = {
        "upstream_revision": UPSTREAM_REVISION,
        "submodule_revisions": SUBMODULE_REVISIONS,
        "raw_adapter_sha256": digest(source_root / "XenosRecomp/raw_adapter.cpp"),
        "reconstructed_emitter_sha256": digest(
            source_root / "XenosRecomp/shader_recompiler.cpp"
        ),
    }
    if reconstruction["reconstructed_emitter_sha256"] != (
        "05a99249ab612a02584b1cdc2942bbf0b19272eaba356d2cde86e7412955a171"
    ):
        raise ValueError("reconstruction emitter does not match the pinned source")
    if reconstruction["raw_adapter_sha256"] != EXPECTED_RAW_ADAPTER_SHA256:
        raise ValueError("raw adapter input does not match the pinned source")
    for name in (
        "pixel-register-lod.patch",
        "fetch-write-mask.patch",
        "exec-clause-gate.patch",
        "register-lod-flow.patch",
        "loop-constants.patch",
    ):
        apply_unified_patch(source_root, owned / name)
    return reconstruction


def build(source_root: Path, build_root: Path) -> Path:
    cmake_root = build_root / "cmake"
    subprocess.run(
        [
            "cmake",
            "-S",
            str(source_root),
            "-B",
            str(cmake_root),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        check=True,
    )
    subprocess.run(
        ["cmake", "--build", str(cmake_root), "--config", "Release"],
        check=True,
    )
    executable = cmake_root / "raw_adapter.exe"
    if not executable.is_file():
        raise ValueError(f"adapter build did not produce {executable}")
    return executable


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-repo", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--diagnostics-root", type=Path)
    parser.add_argument("--build", action="store_true")
    args = parser.parse_args()

    source_repo = args.source_repo.resolve(strict=True)
    build_root = args.build_root.resolve()
    diagnostics = args.diagnostics_root.resolve() if args.diagnostics_root else None
    if diagnostics is not None and diagnostics.exists():
        raise ValueError(f"diagnostics root must be fresh: {diagnostics}")
    record = reconstruct(source_repo, build_root)
    executable = build(build_root / "source", build_root) if args.build else None
    if executable is not None:
        record["adapter_sha256"] = digest(executable)
        record["adapter"] = str(executable)
    record_path = build_root / "reconstruction.json"
    record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="ascii")
    if diagnostics is not None:
        diagnostics.mkdir(parents=True)
        diagnostics_record = diagnostics / "reconstruction.json"
        shutil.copy2(record_path, diagnostics_record)
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
