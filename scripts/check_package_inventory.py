"""Check a release archive against the allowlisted inventory."""

import argparse
import pathlib
import re
import stat
import sys
import tarfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from package import BINARIES, FORBIDDEN_SUFFIXES, expected_entries  # noqa: E402

NAME = re.compile(r"rerevved-v\d+\.\d+\.\d+-(windows|linux)-x64\.(zip|tar\.gz)")
SUFFIX = {"windows": ".zip", "linux": ".tar.gz"}


def archive_entries(path):
    """Return (name, kind, executable) per entry; kind is dir, file or other."""
    entries = []
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                mode = info.external_attr >> 16
                if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                    kind = "other"
                elif info.filename.endswith("/"):
                    kind = "dir"
                else:
                    kind = "file"
                entries.append((info.filename, kind, False))
        return entries
    with tarfile.open(path, "r:gz") as archive:
        for member in archive.getmembers():
            if member.isdir():
                kind = "dir"
            elif member.isfile():
                kind = "file"
            else:
                kind = "other"
            entries.append((member.name, kind, bool(member.mode & 0o111)))
    return entries


def check(path, platform):
    """Return a list of problems with the archive at path."""
    problems = []
    match = NAME.fullmatch(path.name)
    if (
        not match
        or match.group(1) != platform
        or path.name[-len(SUFFIX[platform]) :] != SUFFIX[platform]
    ):
        problems.append(f"unexpected archive name: {path.name}")
    entries = archive_entries(path)
    root = path.name.removesuffix(".zip").removesuffix(".tar.gz")
    directories, files = expected_entries(platform)
    seen = {}
    executables = set()
    for name, kind, executable in entries:
        parts = (name[:-1] if name.endswith("/") else name).split("/")
        if (
            name.startswith("/")
            or "\\" in name
            or any(p in ("", ".", "..") for p in parts)
        ):
            problems.append(f"unsafe entry name: {name}")
            continue
        if kind == "other":
            problems.append(f"link or special entry: {name}")
            continue
        normalized = "/".join(parts)
        if normalized in seen:
            problems.append(f"duplicate entry: {normalized}")
        seen[normalized] = kind
        if executable:
            executables.add(normalized)
    roots = {n.split("/", 1)[0] for n in seen}
    if roots and roots != {root}:
        return problems + [
            f"archive must have the single root {root}, found {sorted(roots)}"
        ]
    if seen.get(root, "dir") != "dir":
        problems.append(f"root entry is not a directory: {root}")
    is_tar = path.name.endswith(".tar.gz")
    if is_tar and seen.get(root) != "dir":
        problems.append(f"missing root directory entry: {root}")
    seen_files = {
        n[len(root) + 1 :] for n, k in seen.items() if k == "file" and n != root
    }
    seen_directories = {
        n[len(root) + 1 :] for n, k in seen.items() if k == "dir" and n != root
    }
    for relative in sorted(seen_files | seen_directories):
        if pathlib.PurePosixPath(relative).suffix.lower() in FORBIDDEN_SUFFIXES:
            problems.append(f"forbidden suffix: {relative}")
    problems += [f"unexpected file: {n}" for n in sorted(seen_files - files)]
    problems += [f"missing file: {n}" for n in sorted(files - seen_files)]
    problems += [
        f"unexpected directory: {n}" for n in sorted(seen_directories - directories)
    ]
    # A tar holds every directory; a zip holds only the empty ones.
    required = directories if is_tar else {"game"}
    problems += [f"missing directory: {n}" for n in sorted(required - seen_directories)]
    executable = BINARIES[platform][0]
    if platform == "linux" and f"{root}/{executable}" not in executables:
        problems.append(f"{executable} has no executable mode bit")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path)
    parser.add_argument("--platform", choices=("windows", "linux"), required=True)
    args = parser.parse_args(argv)
    problems = check(args.archive, args.platform)
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"inventory: ok {args.archive.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
