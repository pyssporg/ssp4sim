"""Read a ``CsvRecorderSink`` result file without ever writing to it.

The recorder writes a wide table: a ``time`` column followed by one column per
signal. Values are separated by ``", "`` and an empty field means the signal was
not recorded at that timestamp. Rows are appended; a live writer may leave a
partially written final line, so parsing stops at the last newline and the tail
stays buffered until the next :meth:`CsvResultSource.refresh`.
"""

from __future__ import annotations

import csv
import hashlib
import io
import os
import threading
import time
from dataclasses import replace
from pathlib import Path
from typing import List, Optional, Sequence

from .base import RunInfo, Series, SignalInfo, Value


class CsvFormatError(ValueError):
    """Raised when a file is not a CSV result artifact (no ``time`` column)."""


def _run_id_for(path: Path) -> str:
    resolved = str(path.resolve()).encode("utf-8")
    return hashlib.sha256(resolved).hexdigest()[:10]


def _sample_column(rows: Sequence[Sequence[Optional[str]]], column: int, limit: int = 1000) -> List[str]:
    samples: List[str] = []
    for row in rows:
        if column >= len(row):
            continue
        value = row[column]
        if value is None:
            continue
        samples.append(value)
        if len(samples) >= limit:
            break
    return samples


def _infer_kind(samples: Sequence[str]) -> str:
    if not samples:
        return "unknown"
    int_like = True
    for text in samples:
        try:
            int(text.strip(), 10)
        except ValueError:
            int_like = False
            break
    if int_like:
        return "integer"
    for text in samples:
        try:
            float(text.strip())
        except ValueError:
            return "string"
    return "real"


def _coerce(value: Optional[str], kind: str) -> Optional[object]:
    if value is None:
        return None
    if kind == "integer":
        try:
            return int(value, 10)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return value
    if kind == "real":
        try:
            return float(value)
        except ValueError:
            return value
    return value


def _decimate_indices(count: int, max_points: Optional[int]) -> Optional[List[int]]:
    """Return indices to keep, or ``None`` when no decimation is needed."""
    if max_points is None or max_points <= 0 or count <= max_points:
        return None
    if max_points == 1:
        return [count - 1]
    step = -(-(count - 1) // (max_points - 1))
    indices = list(range(0, count - 1, step))
    indices.append(count - 1)
    return indices


class CsvResultSource:
    """Incremental reader for one wide CSV result artifact."""

    def __init__(self, path: Path, label: Optional[str] = None) -> None:
        self.path = Path(path)
        self.run_id = _run_id_for(self.path)
        self.label = self.path.stem if label is None else label
        self._lock = threading.RLock()
        self._header: Optional[List[str]] = None
        self._time_index: Optional[int] = None
        self._rows: List[List[Optional[str]]] = []
        self._times: List[float] = []
        self._pending = b""
        self._offset = 0
        self._inode: Optional[int] = None
        self._closed = False
        self._signals_cache: Optional[List[SignalInfo]] = None
        self._info_cache: Optional[RunInfo] = None
        self.refresh()

    # -- protocol ---------------------------------------------------------

    def info(self) -> RunInfo:
        with self._lock:
            if self._info_cache is None:
                times = self._times
                self._info_cache = RunInfo(
                    run_id=self.run_id,
                    path=str(self.path),
                    kind="csv",
                    label=self.label,
                    signals=self.signals(),
                    time_start=min(times) if times else None,
                    time_end=max(times) if times else None,
                    row_count=len(self._rows),
                    live=False,
                )
            return replace(self._info_cache, live=self._is_live())

    def _is_live(self) -> bool:
        # A partial trailing line proves a writer was interrupted mid-row.
        # Otherwise use recency: the recorder flushes in batches, so a run
        # that finished long ago must not look live forever.
        if self._pending:
            return True
        try:
            return time.time() - self.path.stat().st_mtime < 10.0
        except OSError:
            return False

    def signals(self) -> List[SignalInfo]:
        with self._lock:
            if self._header is None:
                return []
            if self._signals_cache is not None:
                return list(self._signals_cache)
            result: List[SignalInfo] = []
            for index, name in enumerate(self._header):
                if index == self._time_index:
                    continue
                kind = _infer_kind(_sample_column(self._rows, index))
                path = [segment for segment in name.split(".") if segment] or [name]
                group = ".".join(path[:-1])
                result.append(
                    SignalInfo(
                        name=name,
                        label=name,
                        kind=kind,
                        group=group,
                        plottable=kind != "string",
                        path=path,
                    )
                )
            # Do not cache a header-only inference: kinds become known once
            # samples exist, and an "unknown" cache would coerce numbers to text.
            if all(signal.kind != "unknown" for signal in result):
                self._signals_cache = result
            return list(result)

    def read(
        self,
        signals: Sequence[str],
        *,
        after: Optional[float] = None,
        until: Optional[float] = None,
        max_points: Optional[int] = None,
    ) -> List[Series]:
        self.refresh()
        with self._lock:
            infos = {signal.name: signal for signal in self.signals()}
            columns = {}
            for name in signals:
                if name not in infos:
                    raise KeyError(name)
                try:
                    columns[name] = self._header.index(name)  # type: ignore[union-attr]
                except (AttributeError, ValueError):
                    raise KeyError(name)

            positions: List[int] = []
            for index, time_value in enumerate(self._times):
                if after is not None and not time_value > after:
                    continue
                if until is not None and not time_value <= until:
                    continue
                positions.append(index)

            indices = _decimate_indices(len(positions), max_points)
            if indices is not None:
                positions = [positions[index] for index in indices]

            times = [self._times[index] for index in positions]
            result: List[Series] = []
            for name in signals:
                info = infos[name]
                column = columns[name]
                values = []
                for row_index in positions:
                    row = self._rows[row_index]
                    raw = row[column] if column < len(row) else None
                    values.append(_coerce(raw, info.kind))
                result.append(
                    Series(
                        key=f"{self.run_id}:{name}",
                        run_id=self.run_id,
                        signal=name,
                        label=info.label,
                        t=list(times),
                        y=values,
                    )
                )
            return result

    def latest(self, signals: Sequence[str]) -> List[Value]:
        self.refresh()
        with self._lock:
            infos = {signal.name: signal for signal in self.signals()}
            columns = {}
            for name in signals:
                if name not in infos:
                    raise KeyError(name)
                try:
                    columns[name] = self._header.index(name)  # type: ignore[union-attr]
                except (AttributeError, ValueError):
                    raise KeyError(name)

            result: List[Value] = []
            for name in signals:
                info = infos[name]
                column = columns[name]
                value: Optional[object] = None
                value_time: Optional[float] = None
                for row_index in range(len(self._rows) - 1, -1, -1):
                    row = self._rows[row_index]
                    raw = row[column] if column < len(row) else None
                    if raw is None:
                        continue
                    value = _coerce(raw, info.kind)
                    value_time = self._times[row_index]
                    break
                result.append(
                    Value(
                        signal=name,
                        label=info.label,
                        value=value,
                        time=value_time,
                        kind=info.kind,
                        plottable=info.plottable,
                    )
                )
            return result

    def _reset_stream(self) -> None:
        self._header = None
        self._time_index = None
        self._rows = []
        self._times = []
        self._pending = b""
        self._offset = 0
        self._signals_cache = None
        self._info_cache = None

    def refresh(self) -> None:
        """Read bytes appended since the previous call, ignoring the tail."""
        with self._lock:
            if self._closed:
                raise RuntimeError("result source is closed")
            with self.path.open("rb") as handle:
                stat = os.fstat(handle.fileno())
                # The recorder truncates its CSV on every run. Restart from the
                # header when the file was replaced or shrank under our offset.
                if self._inode is not None and (
                    stat.st_ino != self._inode or stat.st_size < self._offset
                ):
                    self._reset_stream()
                self._inode = stat.st_ino
                handle.seek(self._offset)
                chunk = handle.read()
            if not chunk:
                return
            self._offset += len(chunk)

            data = self._pending + chunk
            boundary = data.rfind(b"\n")
            if boundary < 0:
                self._pending = data
                return
            complete = data[: boundary + 1]
            self._pending = data[boundary + 1 :]
            self._consume(complete.decode("utf-8", errors="replace"))
            self._info_cache = None

    def close(self) -> None:
        with self._lock:
            self._closed = True

    # -- parsing ----------------------------------------------------------

    def _consume(self, text: str) -> None:
        reader = csv.reader(io.StringIO(text, newline=""), skipinitialspace=True)
        for fields in reader:
            if not fields:
                continue
            if self._header is None:
                header = [field.strip() for field in fields]
                if header:
                    header[0] = header[0].lstrip("\ufeff")
                if "time" not in header:
                    raise CsvFormatError(
                        "{}: CSV result has no 'time' column".format(self.path)
                    )
                self._header = header
                self._time_index = header.index("time")
                continue

            if all(field == "" for field in fields):
                continue
            while len(fields) < len(self._header):
                fields.append("")
            time_index = self._time_index if self._time_index is not None else 0
            try:
                time_value = float(fields[time_index])
            except (TypeError, ValueError):
                continue
            row = [
                None if field == "" else field
                for field in fields[: len(self._header)]
            ]
            self._rows.append(row)
            self._times.append(time_value)
