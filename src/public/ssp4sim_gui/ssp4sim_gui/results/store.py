"""Discovery and safe opening of result artifacts below configured roots.

The HTTP layer accepts a path from the browser. This module is the trust
boundary: :meth:`ResultStore.resolve` refuses anything that does not resolve to a
file below one of the configured roots, and :meth:`ResultStore.open` picks a
read-only source by file suffix.
"""

from __future__ import annotations

import os
import threading
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple, Union

from .base import ResultSource, RunInfo
from .csv_source import CsvResultSource
from .sqlite_source import SqliteResultError, SqliteResultSource


class StoreError(Exception):
    """Structured error surfaced by the HTTP layer."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


PathLike = Union[str, Path]


class ResultStore:
    """Cache and discover result sources rooted in one or more directories."""

    def __init__(
        self,
        roots: Union[PathLike, Iterable[PathLike]],
        extensions: Sequence[str] = (".csv", ".sqlite", ".db"),
    ) -> None:
        if isinstance(roots, (str, Path)):
            roots = [roots]
        self._roots: List[str] = []
        for root in roots:
            resolved = str(Path(root).resolve())
            if resolved not in self._roots:
                self._roots.append(resolved)
        self._extensions = tuple(
            extension.lower()
            if extension.startswith(".")
            else "." + extension.lower()
            for extension in extensions
        )
        self._lock = threading.RLock()
        self._sources: Dict[str, ResultSource] = {}

    # -- roots and paths --------------------------------------------------

    def roots(self) -> List[str]:
        return list(self._roots)

    def resolve(self, path: PathLike) -> Path:
        candidate = Path(path)
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
        resolved = candidate.resolve()
        if not any(
            resolved == Path(root) or Path(root) in resolved.parents
            for root in self._roots
        ):
            raise StoreError(
                "outside-root",
                "path is outside the configured result roots: {}".format(resolved),
            )
        if not resolved.is_file():
            raise StoreError("not-a-file", "not a file: {}".format(resolved))
        return resolved

    # -- opening ----------------------------------------------------------

    def open(self, path: PathLike) -> RunInfo:
        resolved = self.resolve(path)
        source = self._build_source(resolved)
        try:
            info = source.info()
        except BaseException:
            source.close()
            raise

        with self._lock:
            cached = self._sources.get(source.run_id)
            if cached is not None:
                if cached is not source:
                    source.close()
                try:
                    cached.refresh()
                except (OSError, RuntimeError):
                    pass
                return cached.info()
            self._sources[source.run_id] = source
            return info

    def _build_source(self, resolved: Path) -> ResultSource:
        suffix = resolved.suffix.lower()
        try:
            if suffix == ".csv":
                return CsvResultSource(resolved)
            if suffix in (".sqlite", ".db"):
                return SqliteResultSource(resolved)
        except (OSError, ValueError, SqliteResultError) as exc:
            raise StoreError(
                "unreadable-artifact",
                "cannot read result artifact {}: {}".format(resolved, exc),
            ) from exc
        raise StoreError(
            "unsupported-artifact",
            "unsupported result artifact type: {}".format(resolved.suffix),
        )

    def get(self, run_id: str) -> ResultSource:
        with self._lock:
            return self._sources[run_id]

    def close_run(self, run_id: str) -> None:
        with self._lock:
            source = self._sources.pop(run_id)
        source.close()

    def close(self) -> None:
        with self._lock:
            sources = list(self._sources.values())
            self._sources.clear()
        for source in sources:
            source.close()

    # -- discovery --------------------------------------------------------

    def discover(self) -> List[RunInfo]:
        found: List[Tuple[float, RunInfo]] = []
        seen = set()
        for root_string in self._roots:
            root = Path(root_string)
            if not root.is_dir():
                continue
            for directory, subdirectories, filenames in os.walk(str(root)):
                relative = Path(directory).relative_to(root)
                if len(relative.parts) >= 2:
                    subdirectories[:] = []
                for filename in filenames:
                    lower = filename.lower()
                    if lower.endswith("-wal") or lower.endswith("-shm"):
                        continue
                    candidate = Path(directory) / filename
                    if candidate.suffix.lower() not in self._extensions:
                        continue
                    try:
                        resolved = candidate.resolve()
                    except OSError:
                        continue
                    if resolved in seen:
                        continue
                    seen.add(resolved)
                    try:
                        info = self.open(resolved)
                        modified = resolved.stat().st_mtime
                    except (OSError, StoreError):
                        continue
                    found.append((modified, info))

        found.sort(key=lambda item: item[1].path)
        found.sort(key=lambda item: item[0], reverse=True)
        return [info for _, info in found]
