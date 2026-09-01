"""Read-only, root-confined access to Kilix help documents."""

from __future__ import annotations

from collections import OrderedDict
import os
from pathlib import Path, PurePosixPath
import stat
from typing import Any


MAX_HELP_FILE_BYTES = 2 * 1024 * 1024
MAX_SEARCH_RESULTS = 8
MAX_SEARCH_QUERY = 80
MAX_READ_LINES = 80
MAX_READ_CHARACTERS = 6000
MAX_CACHED_DOCUMENTS = 8
SEARCH_STOP_WORDS = frozenset(
    {"a", "an", "and", "do", "for", "how", "i", "in", "is", "of", "the", "to"}
)
SKIP_DIRECTORIES = frozenset(
    {
        ".cache",
        ".git",
        ".release-work",
        ".venv",
        ".worktrees",
        "build",
        "dist",
        "node_modules",
        "__pycache__",
        "venv",
    }
)


class HelpError(RuntimeError):
    """A documentation request fell outside the trusted read-only contract."""


def _is_help_name(name: str) -> bool:
    lowered = name.lower()
    return (
        lowered.endswith((".md", ".markdown", ".txt", ".rst"))
        or lowered in {"readme", "help"}
        or lowered.startswith(("readme.", "help."))
    )


def _printable(value: object, *, field: str, maximum: int) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= maximum:
        raise HelpError(f"{field} is outside its length bound")
    if not all(character.isascii() and 32 <= ord(character) <= 126
               for character in value):
        raise HelpError(f"{field} must be printable ASCII")
    return value


class HelpLibrary:
    """Index and retrieve only documentation beneath one detected Kilix root."""

    def __init__(self, root: Path, *, root_kind: str = "explicit"):
        try:
            resolved = root.expanduser().resolve(strict=True)
        except OSError as error:
            raise HelpError("the Kilix help root does not exist") from error
        if not resolved.is_dir():
            raise HelpError("the Kilix help root is not a directory")
        self.root = resolved
        self.root_kind = root_kind
        self._files = self._index()
        self._cache: OrderedDict[str, str] = OrderedDict()
        if not self._files:
            raise HelpError("the Kilix help root contains no help documents")

    @classmethod
    def discover(cls) -> "HelpLibrary":
        home = Path.home()
        candidates = (
            (home / "gpu_terminal", "development"),
            (home / ".local" / "gpu_terminal", "installed"),
        )
        for candidate, kind in candidates:
            if candidate.is_dir():
                return cls(candidate, root_kind=kind)
        raise HelpError("no development or installed Kilix help root was found")

    def _index(self) -> dict[str, Path]:
        indexed: dict[str, Path] = {}
        for directory, names, files in os.walk(self.root, followlinks=False):
            names[:] = sorted(
                name for name in names
                if name not in SKIP_DIRECTORIES
                and not Path(directory, name).is_symlink()
            )
            for name in sorted(files):
                if not _is_help_name(name):
                    continue
                path = Path(directory, name)
                try:
                    metadata = path.lstat()
                except OSError:
                    continue
                if (path.is_symlink() or not stat.S_ISREG(metadata.st_mode) or
                        metadata.st_size > MAX_HELP_FILE_BYTES):
                    continue
                relative = path.relative_to(self.root).as_posix()
                if (len(relative) > 512 or not all(
                    character.isascii() and 32 <= ord(character) <= 126
                    for character in relative
                )):
                    continue
                indexed[relative] = path
        return dict(sorted(indexed.items()))

    @property
    def document_count(self) -> int:
        return len(self._files)

    @property
    def cached_document_count(self) -> int:
        return len(self._cache)

    def _read_bytes(self, relative: str) -> bytes:
        file_flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
        directory_flags = file_flags | os.O_DIRECTORY
        descriptors: list[int] = []
        total = 0
        chunks: list[bytes] = []
        try:
            current = os.open(self.root, directory_flags)
            descriptors.append(current)
            parts = PurePosixPath(relative).parts
            for component in parts[:-1]:
                current = os.open(
                    component, directory_flags, dir_fd=current
                )
                descriptors.append(current)
            document = os.open(parts[-1], file_flags, dir_fd=current)
            descriptors.append(document)
            metadata = os.fstat(document)
            if (not stat.S_ISREG(metadata.st_mode) or
                    metadata.st_size > MAX_HELP_FILE_BYTES):
                raise HelpError("the help document is not bounded text")
            while total <= MAX_HELP_FILE_BYTES:
                chunk = os.read(
                    document,
                    min(65536, MAX_HELP_FILE_BYTES + 1 - total),
                )
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
            if total > MAX_HELP_FILE_BYTES:
                raise HelpError("the help document is not bounded text")
            return b"".join(chunks)
        except HelpError:
            raise
        except OSError as error:
            raise HelpError(
                "the help document is no longer a confined regular file"
            ) from error
        finally:
            for descriptor in reversed(descriptors):
                try:
                    os.close(descriptor)
                except OSError:
                    pass

    def _text(self, relative: str, *, cache: bool = True) -> str:
        cached = self._cache.get(relative)
        if cached is not None:
            self._cache.move_to_end(relative)
            return cached
        data = self._read_bytes(relative)
        if b"\0" in data:
            raise HelpError("the help document is not bounded text")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as error:
            raise HelpError("the help document is not UTF-8") from error
        if cache:
            self._cache[relative] = text
            while len(self._cache) > MAX_CACHED_DOCUMENTS:
                self._cache.popitem(last=False)
        return text

    def _relative(self, value: object) -> str:
        path_text = _printable(value, field="path", maximum=512)
        candidate = PurePosixPath(path_text)
        if candidate.is_absolute() or any(
            part in ("", ".", "..") for part in candidate.parts
        ) or candidate.as_posix() != path_text:
            raise HelpError("help paths must be normalized and relative")
        relative = candidate.as_posix()
        if relative not in self._files:
            raise HelpError("the requested path is not an indexed help document")
        return relative

    def search(self, query_value: object) -> dict[str, Any]:
        query = _printable(query_value, field="query", maximum=MAX_SEARCH_QUERY)
        if not query.strip():
            raise HelpError("the help query cannot be blank")
        raw_terms = tuple(term.casefold() for term in query.split() if term)
        terms = tuple(term for term in raw_terms if term not in SEARCH_STOP_WORDS)
        if not terms:
            terms = raw_terms
        candidates: list[tuple[int, int, str, int, str]] = []
        for relative in self._files:
            try:
                text = self._text(relative, cache=False)
            except HelpError:
                continue
            path_haystack = relative.casefold()
            path_score = sum(term in path_haystack for term in terms)
            best_score = 0
            best_line = 0
            best_snippet = ""
            for line_number, line in enumerate(text.splitlines(), start=1):
                haystack = f"{relative} {line}".casefold()
                score = sum(term in haystack for term in terms)
                if score > best_score:
                    best_score = score
                    best_line = line_number
                    best_snippet = " ".join(line.strip().split())[:240]
                    if score == len(terms):
                        break
            if best_score > 0:
                candidates.append(
                    (best_score, path_score, relative, best_line, best_snippet)
                )
        candidates.sort(key=lambda item: (-item[0], -item[1], item[2], item[3]))
        results = [
            {"path": relative, "line": line, "snippet": snippet}
            for _score, _path_score, relative, line, snippet
            in candidates[:MAX_SEARCH_RESULTS]
        ]
        return {
            "protocol": "kilix.help.search/v1",
            "root": self.root_kind,
            "query": query,
            "results": results,
            "truncated": len(candidates) > MAX_SEARCH_RESULTS,
        }

    def read(self, path_value: object, line_start_value: object) -> dict[str, Any]:
        relative = self._relative(path_value)
        if (not isinstance(line_start_value, int) or
                isinstance(line_start_value, bool) or
                not 1 <= line_start_value <= 1_000_000):
            raise HelpError("line_start must be a positive integer")
        text = self._text(relative)
        lines = text.splitlines()
        if line_start_value > max(1, len(lines)):
            raise HelpError("line_start is beyond the help document")
        start_index = line_start_value - 1
        selected: list[str] = []
        characters = 0
        for line in lines[start_index:start_index + MAX_READ_LINES]:
            addition = len(line) + (1 if selected else 0)
            if characters + addition > MAX_READ_CHARACTERS:
                break
            selected.append(line)
            characters += addition
        line_end = (line_start_value + len(selected) - 1
                    if selected else 0)
        return {
            "protocol": "kilix.help.read/v1",
            "root": self.root_kind,
            "path": relative,
            "line_start": line_start_value,
            "line_end": line_end,
            "total_lines": len(lines),
            "content": "\n".join(selected),
            "truncated": line_end < len(lines),
        }
