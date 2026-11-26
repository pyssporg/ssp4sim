"""Result-source contract.

A result source reads one run's time series from an on-disk artifact: the wide
CSV written by ``CsvRecorderSink`` or the per-storage SQLite tables written by
``SqliteWALRecorderSink``. Data never crosses the engine control channel.

On-disk facts that implementations must honour (from the engine sources):

* CSV: header ``time,<signal>,...``; data rows use ``, `` between fields, so the
  parser needs ``skipinitialspace``; empty field means "not recorded"; time is
  simulation seconds and ascending.
* SQLite: one table per storage area named ``I<run_id>_<model>_<storage>`` with
  ``timestamp_ns`` and ``simulation_time_s`` columns; other columns are the
  variables; no indexes; WAL mode allows concurrent readers.

Sources must be read-only. They never write to the artifact.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional, Protocol, Sequence, runtime_checkable


@dataclass(frozen=True)
class SignalInfo:
    """One value column inside a run."""

    name: str          # stable source-local key, unique within the run
    label: str         # human-readable, e.g. "Consumer.outletTemp"
    kind: str          # "real" | "integer" | "boolean" | "string" | "unknown"
    group: str = ""    # grouping label, e.g. "Consumer.output"
    plottable: bool = True
    path: List[str] = field(default_factory=list)  # run-relative hierarchy, leaf last


@dataclass(frozen=True)
class RunInfo:
    run_id: str
    path: str
    kind: str          # "csv" | "sqlite"
    label: str
    signals: List[SignalInfo]
    time_start: Optional[float]
    time_end: Optional[float]
    row_count: int
    live: bool         # artifact may still be appended to by a running engine


@dataclass(frozen=True)
class Value:
    """Latest value of one signal, for the numeric read-out table."""

    signal: str
    label: str
    value: Any
    time: Optional[float]
    kind: str
    plottable: bool


@dataclass(frozen=True)
class Series:
    """A signal's samples. ``key`` is globally unique across open runs."""

    key: str           # "{run_id}:{signal}"
    run_id: str
    signal: str
    label: str
    t: List[float]
    y: List[Any]


@runtime_checkable
class ResultSource(Protocol):
    path: Path
    run_id: str

    def info(self) -> RunInfo:
        ...

    def signals(self) -> List[SignalInfo]:
        ...

    def read(
        self,
        signals: Sequence[str],
        *,
        after: Optional[float] = None,
        until: Optional[float] = None,
        max_points: Optional[int] = None,
    ) -> List[Series]:
        """Return samples with ``after < t <= until``.

        ``max_points`` is a cap, not a resampling contract: implementations
        stride-decimate and always keep the last sample.
        """
        ...

    def latest(self, signals: Sequence[str]) -> List[Value]:
        ...

    def refresh(self) -> None:
        """Pick up bytes committed since the previous read (live tailing)."""
        ...

    def close(self) -> None:
        ...
