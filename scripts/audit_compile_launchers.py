"""Fail if a build directory compiles through a cache launcher.

Generated guest code is derived from the private retail binary, so its objects
must never reach a shared compiler cache. Checks the configured launcher
variables and the compiler command lines Ninja would run.
"""

import argparse
import pathlib
import re
import subprocess
import sys

LAUNCHER_VARIABLE = re.compile(r"^CMAKE_(?:C|CXX)_COMPILER_LAUNCHER:[^=]*=(.+)$", re.M)
LAUNCHERS = {"sccache", "ccache"}
GUEST_COMPILE = re.compile(r"generated[\\/]default[\\/][^\s\"']+\.(?:cpp|cc|cxx|c)\b")
COMPILE_FLAG = re.compile(r"(?:^|\s)[-/]c(?:\s|$)")


def launcher_tokens(line):
    """Return command tokens whose file stem names a compiler cache."""
    found = []
    for token in re.split(r"[\s\"']+", line):
        stem = re.split(r"[\\/]", token)[-1].lower().removesuffix(".exe")
        if stem in LAUNCHERS:
            found.append(token)
    return found


def audit(cache_text, commands, require_generated=True):
    """Return problems found in a CMakeCache.txt text and Ninja command lines."""
    problems = [
        f"compiler launcher configured: {match.group(0)}"
        for match in LAUNCHER_VARIABLE.finditer(cache_text)
        if match.group(1).strip()
    ]
    lines = [line for line in commands.splitlines() if line.strip()]
    if not lines:
        problems.append("no compile commands found")
    guest = [
        line
        for line in lines
        if GUEST_COMPILE.search(line) and COMPILE_FLAG.search(line)
    ]
    if require_generated and not guest:
        problems.append("no generated guest code compile command found")
    problems += [
        f"cache launcher in compile command: {line[:200]}"
        for line in lines
        if launcher_tokens(line)
    ]
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument(
        "--before-codegen",
        action="store_true",
        help="generated sources do not exist yet; skip the guest compile check",
    )
    args = parser.parse_args(argv)
    cache = (args.build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    result = subprocess.run(
        ["ninja", "-C", str(args.build_dir), "-t", "commands"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        return result.returncode
    problems = audit(cache, result.stdout, require_generated=not args.before_codegen)
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"compile audit: ok {args.build_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
