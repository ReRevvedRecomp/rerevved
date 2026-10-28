"""Assemble the bundled first-party mods from a pinned rerevved-mods checkout."""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

from package import BUNDLED_MODS, check_mods

REPO = pathlib.Path(__file__).resolve().parent.parent
LOCK = REPO / "rerevved-mods.lock.json"


def git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True
    )


def read_commit(path):
    commit = json.loads(path.read_text(encoding="utf-8")).get("commit", "")
    if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
        raise SystemExit(f"error: invalid commit in {path}")
    return commit


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mods-checkout",
        type=pathlib.Path,
        required=True,
        help="rerevved-mods checkout at the commit in rerevved-mods.lock.json",
    )
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=REPO / "out" / "bundled-mods",
        help="output directory that must not exist; pass it to package.py --mods-dir",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    checkout = args.mods_checkout.resolve()
    if args.out.exists():
        raise SystemExit(f"error: output already exists; remove it first: {args.out}")

    expected = read_commit(LOCK)
    head = git(checkout, "rev-parse", "HEAD")
    if head.returncode != 0 or head.stdout.strip() != expected:
        raise SystemExit(
            f"error: {checkout} must be rerevved-mods at {expected} "
            f"(found {head.stdout.strip() or 'no git checkout'})"
        )
    status = git(checkout, "status", "--porcelain")
    if status.returncode != 0 or status.stdout.strip():
        raise SystemExit(f"error: rerevved-mods checkout has local changes: {checkout}")

    # The mods target an earlier title commit; this release must contain it.
    title = read_commit(checkout / "rerevved-api.lock.json")
    if git(REPO, "cat-file", "-e", f"{title}^{{commit}}").returncode != 0:
        raise SystemExit(
            f"error: mods title lock {title} is not in this checkout's history; "
            "fetch full title history"
        )
    if git(REPO, "merge-base", "--is-ancestor", title, "HEAD").returncode != 0:
        raise SystemExit(f"error: mods title lock {title} is not an ancestor of HEAD")

    build = [sys.executable, str(checkout / "tools" / "build_mods.py")]
    for package_id in BUNDLED_MODS:
        build += ["--mod", package_id]
    result = subprocess.run(build, cwd=checkout)
    if result.returncode != 0:
        raise SystemExit("error: rerevved-mods tools/build_mods.py failed")

    args.out.mkdir(parents=True)
    for package_id in BUNDLED_MODS:
        shutil.copytree(checkout / "out" / "mods" / package_id, args.out / package_id)
    check_mods(args.out)
    print(f"assembled: {args.out} ({', '.join(BUNDLED_MODS)})")


if __name__ == "__main__":
    main()
