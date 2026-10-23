"""Lock file checker tests."""

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_pins  # noqa: E402

SDK_COMMIT = "2b5efb732e44ef6f1cd790751da0574e9a0f72f8"


def git(path, *args):
    result = subprocess.run(
        ["git", "-C", str(path), *args], capture_output=True, text=True, check=True
    )
    return result.stdout.strip()


def make_repo(path, files=None):
    """Create a repository with one commit on main and return its commit."""
    path.mkdir(parents=True)
    git(path, "init", "-q", "-b", "main")
    git(path, "config", "user.name", "test")
    git(path, "config", "user.email", "test@example.invalid")
    for name, text in (files or {"file.txt": "x"}).items():
        (path / name).parent.mkdir(parents=True, exist_ok=True)
        (path / name).write_text(text)
    git(path, "add", "-A")
    git(path, "commit", "-q", "-m", "test: add files")
    return git(path, "rev-parse", "HEAD")


def sdk_lock(**changes):
    lock = {
        "repository": check_pins.SDK_REPOSITORY,
        "commit": SDK_COMMIT,
        "version": "0.36.0-dev.g2b5efb7",
        "dirty": "clean",
    }
    lock.update(changes)
    return lock


class LockFormatTests(unittest.TestCase):
    def test_valid_sdk_lock(self) -> None:
        self.assertEqual(check_pins.validate_sdk_lock(sdk_lock()), [])

    def test_tagged_release_sdk_lock(self) -> None:
        self.assertEqual(check_pins.validate_sdk_lock(sdk_lock(version="0.11.0")), [])

    def test_sdk_lock_problems(self) -> None:
        for change in (
            {"repository": "https://github.com/someone/rerevved-sdk"},
            {"commit": "2B5EFB7"},
            {"commit": SDK_COMMIT + "\n"},
            {"version": "0.36.0-dev.gdeadbee"},
            {"version": "v0.11.0"},
            {"version": "0.11"},
            {"version": "0.11.0-rc.g2b5efb7"},
            {"dirty": "dirty"},
        ):
            with self.subTest(change=change):
                self.assertTrue(check_pins.validate_sdk_lock(sdk_lock(**change)))

    def test_mods_lock_problems(self) -> None:
        good = {"repository": check_pins.MODS_REPOSITORY, "commit": "a" * 40}
        self.assertEqual(check_pins.validate_mods_lock(good), [])
        self.assertTrue(check_pins.validate_mods_lock({**good, "commit": "main"}))
        self.assertTrue(check_pins.validate_mods_lock({**good, "repository": "x"}))


class AncestryTests(unittest.TestCase):
    def test_ancestor_of_branch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "repo"
            first = make_repo(repo)
            (repo / "file.txt").write_text("y")
            git(repo, "commit", "-q", "-am", "test: change")
            self.assertEqual(check_pins.check_ancestor(repo, "lock", first, "main"), [])
            git(repo, "checkout", "-q", "-b", "topic", first)
            (repo / "file.txt").write_text("z")
            git(repo, "commit", "-q", "-am", "test: topic")
            topic = git(repo, "rev-parse", "HEAD")
            self.assertTrue(check_pins.check_ancestor(repo, "lock", topic, "main"))
            self.assertTrue(check_pins.check_ancestor(repo, "lock", "f" * 40, "main"))
            self.assertTrue(check_pins.check_ancestor(repo, "lock", first, "absent"))
            # A tag never stands in for the branch of the same name.
            git(repo, "tag", "main", topic)
            git(repo, "tag", "absent", topic)
            git(repo, "tag", "refs/heads/absent", topic)
            git(repo, "tag", "refs/remotes/origin/absent", topic)
            self.assertTrue(check_pins.check_ancestor(repo, "lock", topic, "main"))
            self.assertTrue(check_pins.check_ancestor(repo, "lock", topic, "absent"))

    def test_mods_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            title = root / "title"
            title_commit = make_repo(title)
            mods = root / "mods"
            api = json.dumps({"commit": title_commit})
            mods_commit = make_repo(mods, {"rerevved-api.lock.json": api})
            self.assertEqual(check_pins.check_cycle(mods, mods_commit, title), [])
            other = root / "other"
            make_repo(other, {"other.txt": "different"})
            self.assertTrue(check_pins.check_cycle(mods, mods_commit, other))
            self.assertTrue(check_pins.check_cycle(mods, title_commit, title))


# Test stub, not the SDK's cmake/rex_version.cmake: it reports an exact v* tag
# like the real resolver, else the floor and the short HEAD of SOURCE_DIR, so a
# result proves which commit and floor were used.
STUB_VERSION_SCRIPT = """function(rex_resolve_version out_var)
    cmake_parse_arguments(ARG "" "FLOOR_MAJOR;FLOOR_MINOR;SOURCE_DIR" "" ${ARGN})
    execute_process(COMMAND git describe --tags --exact-match --match "v*"
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE tag OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE tag_rc ERROR_QUIET)
    if(tag_rc EQUAL 0 AND tag MATCHES "^v(.+)$")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        return()
    endif()
    execute_process(COMMAND git rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out_var} "${ARG_FLOOR_MAJOR}.${ARG_FLOOR_MINOR}.0-dev.g${sha}" PARENT_SCOPE)
endfunction()
"""


def make_sdk(path, version="0.37.0"):
    """Create a stub SDK repository and return its commit."""
    listfile = f"project(ReXGlue\n    VERSION {version}\n    LANGUAGES CXX)\n"
    return make_repo(
        path,
        {"CMakeLists.txt": listfile, "cmake/rex_version.cmake": STUB_VERSION_SCRIPT},
    )


def write_title(path, lock):
    path.mkdir()
    (path / "rexglue-sdk.lock.json").write_text(json.dumps(lock))
    mods = {"repository": check_pins.MODS_REPOSITORY, "commit": "a" * 40}
    (path / "rerevved-mods.lock.json").write_text(json.dumps(mods))


@unittest.skipIf(
    shutil.which("cmake") is None, "cmake not found; SDK version recompute not tested"
)
class SdkVersionTests(unittest.TestCase):
    def test_matching_and_mismatching_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdk"
            commit = make_sdk(sdk)
            good = sdk_lock(commit=commit, version=f"0.37.0-dev.g{commit[:7]}")
            self.assertEqual(check_pins.check_sdk_version(sdk, good), [])
            bad = sdk_lock(commit=commit, version=f"0.36.0-dev.g{commit[:7]}")
            problems = check_pins.check_sdk_version(sdk, bad)
            self.assertEqual(len(problems), 1)
            self.assertIn(f"SDK derives 0.37.0-dev.g{commit[:7]}", problems[0])
            worktrees = git(sdk, "worktree", "list", "--porcelain")
            self.assertEqual(worktrees.count("worktree "), 1)

    def test_tagged_release_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdk"
            commit = make_sdk(sdk, version="0.11.0")
            release = sdk_lock(commit=commit, version="0.11.0")
            # Without the tag the SDK derives a dev version, so the lock fails.
            problems = check_pins.check_sdk_version(sdk, release)
            self.assertEqual(len(problems), 1)
            self.assertIn(f"SDK derives 0.11.0-dev.g{commit[:7]}", problems[0])
            git(sdk, "tag", "v0.11.0", commit)
            self.assertEqual(check_pins.check_sdk_version(sdk, release), [])

    def test_uses_files_at_lock_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory) / "sdk"
            commit = make_sdk(sdk)
            listfile = (sdk / "CMakeLists.txt").read_text()
            (sdk / "CMakeLists.txt").write_text(listfile.replace("0.37.0", "0.38.0"))
            git(sdk, "commit", "-q", "-am", "test: bump floor")
            lock = sdk_lock(commit=commit, version=f"0.37.0-dev.g{commit[:7]}")
            self.assertEqual(check_pins.check_sdk_version(sdk, lock), [])

    def test_lock_commit_off_branch(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            base = make_sdk(sdk)
            git(sdk, "checkout", "-q", "-b", "topic", base)
            (sdk / "topic.txt").write_text("topic")
            git(sdk, "add", "topic.txt")
            git(sdk, "commit", "-q", "-m", "test: topic")
            topic = git(sdk, "rev-parse", "HEAD")
            git(sdk, "checkout", "-q", "main")
            title = root / "title"
            write_title(
                title, sdk_lock(commit=topic, version=f"0.37.0-dev.g{topic[:7]}")
            )
            args = ["--title", str(title), "--sdk", str(sdk), "--branch", "main"]
            self.assertEqual(check_pins.main(args), 1)
            self.assertEqual(check_pins.main([*args, "--warn-only"]), 0)


class MissingCmakeTests(unittest.TestCase):
    def test_missing_cmake_is_reported(self) -> None:
        with mock.patch.object(check_pins.shutil, "which", return_value=None):
            problems = check_pins.check_sdk_version(Path("unused"), sdk_lock())
        self.assertEqual(problems, ["SDK version: cannot recompute: cmake not found"])


if __name__ == "__main__":
    unittest.main()
