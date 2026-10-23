"""Shared access to the generated guest code for the contract suites.

A suite builds its hook-window index with one `scan_once` call per process.
Chunks are decoded once per scan; the cache is keyed by the generated directory
and the size and mtime of every chunk, so a regenerated tree is rescanned.
"""

from __future__ import annotations

from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
# Read at call time so a caller can retarget the directory.
GENERATED = ROOT / "generated" / "default"
CHUNK_PATTERN = "rerevved_recomp.*.cpp"

_CACHE: dict[tuple[Any, ...], Any] = {}


def chunk_paths() -> list[Path]:
    return sorted(GENERATED.glob(CHUNK_PATTERN))


def chunk_lines(paths: list[Path]) -> Iterator[list[str]]:
    """Yield the lines of one chunk at a time to bound memory."""
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            yield stream.readlines()


def scan_once(name: str, scan: Callable[[list[Path]], Any]) -> tuple[list[Path], Any]:
    """Return the chunk list and the cached `scan(paths)` result for `name`."""
    paths = chunk_paths()
    signature = tuple(
        (path.name, stat.st_size, stat.st_mtime_ns)
        for path, stat in ((path, path.stat()) for path in paths)
    )
    key = (name, str(GENERATED), signature)
    if key not in _CACHE:
        _CACHE[key] = scan(paths)
    return paths, _CACHE[key]


class WindowCollector:
    """Collect fixed-length line windows that open at matched lines."""

    def __init__(self) -> None:
        self.windows: dict[Any, str] = {}
        self._active: list[tuple[Any, list[str], int]] = []

    def advance(self, line: str) -> None:
        active = self._active
        for index in range(len(active) - 1, -1, -1):
            name, lines, remaining = active[index]
            lines.append(line)
            if remaining == 1:
                self.windows[name] = "".join(lines)
                active.pop(index)
            else:
                active[index] = (name, lines, remaining - 1)

    def start(self, name: Any, lines: list[str], after: int) -> None:
        self._active.append((name, lines, after))

    def end_chunk(self, keep_partial: bool = True) -> None:
        if keep_partial:
            for name, lines, _ in self._active:
                self.windows[name] = "".join(lines)
        self._active = []


class GeneratedText:
    """Every chunk decoded once and joined, with the chunk boundaries kept."""

    def __init__(self, paths: list[Path]) -> None:
        texts = [path.read_text(encoding="utf-8") for path in paths]
        self.bounds: list[tuple[int, int]] = []
        offset = 0
        for text in texts:
            self.bounds.append((offset, offset + len(text)))
            offset += len(text)
        self.names = {path: index for index, path in enumerate(paths)}
        self.paths = paths
        self.text = "".join(texts)

    def chunk(self, path: Path) -> str:
        """Return one chunk of the joined text."""
        start, end = self.bounds[self.names[path]]
        return self.text[start:end]


def generated_text() -> tuple[list[Path], str]:
    """Return the chunk list and all chunks joined, read once per process."""
    paths, cached = scan_once("generated_text", GeneratedText)
    return paths, cached.text


def count_per_chunk(needle: str) -> int:
    """Sum the occurrences of `needle` inside each chunk, none across a seam."""
    _, cached = scan_once("generated_text", GeneratedText)
    return sum(cached.text.count(needle, start, end) for start, end in cached.bounds)


def generated_chunks() -> GeneratedText:
    """Return the cached text, for per-chunk access without rescanning."""
    return scan_once("generated_text", GeneratedText)[1]


def chunk_texts() -> Iterator[str]:
    """Yield the chunks of the cached text one at a time."""
    _, cached = scan_once("generated_text", GeneratedText)
    for start, end in cached.bounds:
        yield cached.text[start:end]
