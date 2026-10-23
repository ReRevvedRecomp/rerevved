"""Check the SDK and Mods lock files and, given checkouts, ancestry and SDK version."""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SDK_VERSION_SCRIPT = REPO / "scripts" / "sdk_version.cmake"

SDK_REPOSITORY = "https://github.com/ReRevvedRecomp/rerevved-sdk"
MODS_REPOSITORY = "https://github.com/ReRevvedRecomp/rerevved-mods"
COMMIT = re.compile(r"[0-9a-f]{40}")
# A tagged SDK release is a plain version; untagged builds name their commit.
VERSION = re.compile(r"\d+\.\d+\.\d+(\.\d+)?(?:-dev\.g([0-9a-f]{7,40}))?")
# The SDK floor is the project() version in its top-level CMakeLists.txt.
SDK_FLOOR = re.compile(r"project\(\s*ReXGlue\b[^)]*?\bVERSION\s+(\d+)\.(\d+)", re.S)


def validate_sdk_lock(lock):
    errors = []
    if lock.get("repository") != SDK_REPOSITORY:
        errors.append(f"SDK lock repository is not {SDK_REPOSITORY}")
    commit = lock.get("commit", "")
    if not COMMIT.fullmatch(commit):
        errors.append("SDK lock commit is not 40 lowercase hex digits")
    match = VERSION.fullmatch(lock.get("version", ""))
    if not match:
        errors.append("SDK lock version is not a release or development version")
    elif match.group(2) and not commit.startswith(match.group(2)):
        errors.append("SDK lock version does not name the locked commit")
    if lock.get("dirty") != "clean":
        errors.append("SDK lock dirty is not 'clean'")
    return errors


def validate_mods_lock(lock):
    errors = []
    if lock.get("repository") != MODS_REPOSITORY:
        errors.append(f"Mods lock repository is not {MODS_REPOSITORY}")
    if not COMMIT.fullmatch(lock.get("commit", "")):
        errors.append("Mods lock commit is not 40 lowercase hex digits")
    return errors


def git(repo, *args):
    return subprocess.run(
        ["git", "-C", str(repo), *args], capture_output=True, text=True, check=False
    )


def resolve_branch(repo, branch):
    """Prefer the local branch, which holds unpushed landings; a fresh CI clone
    may have only the remote-tracking one. show-ref --verify matches exact ref
    names only, so no tag can stand in for the branch."""
    for ref in (f"refs/heads/{branch}", f"refs/remotes/origin/{branch}"):
        found = git(repo, "show-ref", "--verify", "--hash", ref)
        if found.returncode == 0:
            return ref, found.stdout.strip()
    return None, None


def check_ancestor(repo, label, commit, branch):
    ref, tip = resolve_branch(repo, branch)
    if ref is None:
        return [f"{label}: branch {branch} not found in {repo}"]
    if git(repo, "cat-file", "-e", f"{commit}^{{commit}}").returncode != 0:
        return [f"{label}: commit {commit} is not in {repo}"]
    if git(repo, "merge-base", "--is-ancestor", commit, tip).returncode != 0:
        return [f"{label}: commit {commit} is not on {ref}"]
    return []


def derive_sdk_version(sdk, commit, tree):
    """Run the SDK's version script in a checkout of commit at tree."""
    cmake = shutil.which("cmake")
    if cmake is None:
        return None, "cmake not found"
    added = git(sdk, "worktree", "add", "--detach", "--quiet", str(tree), commit)
    if added.returncode != 0:
        return None, f"cannot check out {commit}: {added.stderr.strip()}"
    try:
        listfile = (tree / "CMakeLists.txt").read_text(encoding="utf-8")
        floor = SDK_FLOOR.search(listfile)
        if floor is None:
            return None, "no project(ReXGlue VERSION x.y.z) in CMakeLists.txt"
        result = subprocess.run(
            [
                cmake,
                f"-DSOURCE_DIR={tree.as_posix()}",
                f"-DFLOOR_MAJOR={floor.group(1)}",
                f"-DFLOOR_MINOR={floor.group(2)}",
                "-P",
                str(SDK_VERSION_SCRIPT),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            return None, f"version script failed: {result.stderr.strip()}"
        return result.stdout.strip(), None
    finally:
        removed = git(sdk, "worktree", "remove", str(tree))
        if removed.returncode != 0:
            print(
                f"warning: could not remove {tree}: {removed.stderr.strip()}; "
                f"run git -C {sdk} worktree prune after deleting it",
                file=sys.stderr,
            )


def check_sdk_version(sdk, lock):
    """The lock version must equal what the SDK derives for the lock commit."""
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        version, problem = derive_sdk_version(
            sdk, lock["commit"], pathlib.Path(directory) / "sdk"
        )
    if problem is not None:
        return [f"SDK version: cannot recompute: {problem}"]
    if version != lock["version"]:
        hint = ""
        if version.endswith("-dev.unknown"):
            hint = " (no v* tag reachable; fetch the SDK tags)"
        return [f"SDK version: lock has {lock['version']}, SDK derives {version}{hint}"]
    print(f"SDK version: {version} derived for {lock['commit']}")
    return []


def check_cycle(mods, mods_commit, title):
    """The Mods checkout at the pinned commit must pin a title ancestor."""
    shown = git(mods, "show", f"{mods_commit}:rerevved-api.lock.json")
    if shown.returncode != 0:
        return [f"Mods lock: no rerevved-api.lock.json at {mods_commit}"]
    title_commit = json.loads(shown.stdout).get("commit", "")
    if not COMMIT.fullmatch(title_commit):
        return ["Mods rerevved-api lock commit is not 40 lowercase hex digits"]
    if git(title, "merge-base", "--is-ancestor", title_commit, "HEAD").returncode != 0:
        return [f"Mods pins title commit {title_commit}, not an ancestor of HEAD"]
    return []


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--title", type=pathlib.Path, default=REPO)
    parser.add_argument("--sdk", type=pathlib.Path, help="rerevved-sdk checkout")
    parser.add_argument("--mods", type=pathlib.Path, help="rerevved-mods checkout")
    parser.add_argument("--branch", default="main", help="maintained branch")
    parser.add_argument(
        "--warn-only",
        action="store_true",
        help="report ancestry problems without failing (topic branches)",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    sdk_lock = json.loads((args.title / "rexglue-sdk.lock.json").read_text())
    mods_lock = json.loads((args.title / "rerevved-mods.lock.json").read_text())

    errors = validate_sdk_lock(sdk_lock) + validate_mods_lock(mods_lock)
    soft = []
    if args.sdk is not None:
        soft += check_ancestor(args.sdk, "SDK lock", sdk_lock["commit"], args.branch)
        if not validate_sdk_lock(sdk_lock):
            soft += check_sdk_version(args.sdk, sdk_lock)
    if args.mods is not None:
        soft += check_ancestor(args.mods, "Mods lock", mods_lock["commit"], args.branch)
        soft += check_cycle(args.mods, mods_lock["commit"], args.title)

    for message in errors:
        print(f"error: {message}", file=sys.stderr)
    for message in soft:
        label = "warning" if args.warn_only else "error"
        print(f"{label}: {message}", file=sys.stderr)
    if errors or (soft and not args.warn_only):
        return 1
    print("pins: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
