"""Read a ``SqliteWALRecorderSink`` database without ever writing to it.

The recorder stores one table per storage area. Every table carries
``timestamp_ns`` and ``simulation_time_s`` followed by the variables. Tables for
a run are named ``I<run_id>_<sanitized_model>_<sanitized_storage>``; the run id
is recorded in ``ssp4sim_run_counter``. Signal keys are exposed as
``"<table>.<column>"`` because sanitisation removed the original dots from the
table name.
"""

from __future__ import annotations

import hashlib
import re
import sqlite3
import threading
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from .base import RunInfo, Series, SignalInfo, Value


class SqliteResultError(RuntimeError):
    """A SQLite result artifact could not be opened or queried."""


@dataclass(frozen=True)
class _Column:
    name: str
    kind: str


_RUN_TABLE = re.compile(r"^I(\d+)_")


def _quote_identifier(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def _declared_kind(declared: object) -> str:
    text = str(declared or "").upper()
    if "INT" in text:
        return "integer"
    if "REAL" in text or "FLOA" in text or "DOUB" in text:
        return "real"
    if "TEXT" in text or "CHAR" in text or "CLOB" in text or "BLOB" in text:
        return "string"
    return "unknown"


def _coerce(value: object, kind: str) -> object:
    if value is None:
        return None
    if kind == "integer":
        if isinstance(value, int) and not isinstance(value, bool):
            return value
        try:
            return int(value)  # type: ignore[arg-type]
        except (TypeError, ValueError):
            try:
                return float(value)  # type: ignore[arg-type]
            except (TypeError, ValueError):
                return value
    if kind == "real":
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            return float(value)
        try:
            return float(value)  # type: ignore[arg-type]
        except (TypeError, ValueError):
            return value
    if kind == "string":
        return value if isinstance(value, str) else str(value)
    return value


def _decimate_indices(count: int, max_points: Optional[int]) -> Optional[List[int]]:
    if max_points is None or max_points <= 0 or count <= max_points:
        return None
    if max_points == 1:
        return [count - 1]
    step = -(-(count - 1) // (max_points - 1))
    indices = list(range(0, count - 1, step))
    indices.append(count - 1)
    return indices


class SqliteResultSource:
    """Read-only view of one run inside a SQLite result database."""

    def __init__(
        self,
        path: Path,
        run_id: Optional[object] = None,
        label: Optional[str] = None,
    ) -> None:
        self.path = Path(path)
        self._resolved = self.path.resolve()
        self.label = self.path.stem if label is None else label
        self._lock = threading.RLock()
        self._connection: Optional[sqlite3.Connection] = None
        self._tables: Dict[str, List[_Column]] = {}
        self._db_run_id: Optional[int] = None
        self.db_run_id: Optional[int] = None
        self.run_id = ""
        self._signals_cache: Optional[List[SignalInfo]] = None
        self._info_cache: Optional[RunInfo] = None
        self._info_key: Optional[object] = None
        self._info_at = 0.0
        # A read-only WAL connection may create an empty ``-wal`` sidecar. Only
        # that pre-existing sidecar (or one carrying data) means a live writer.
        self._wal_was_present = Path(str(self._resolved) + "-wal").exists()

        self._connection = self._open_connection()
        try:
            self._db_run_id = self._select_run(run_id)
            self.db_run_id = self._db_run_id
            self.run_id = self._make_run_id(self._db_run_id)
            self._discover_tables()
        except BaseException:
            self.close()
            raise

    # -- protocol ---------------------------------------------------------

    def _artifact_key(self):
        key = []
        for candidate in (self._resolved, Path(str(self._resolved) + "-wal")):
            try:
                stat = candidate.stat()
                key.append((stat.st_mtime_ns, stat.st_size))
            except OSError:
                key.append(None)
        return tuple(key)

    def info(self) -> RunInfo:
        with self._lock:
            now = time.monotonic()
            key = self._artifact_key()
            if (
                self._info_cache is not None
                and key == self._info_key
                and now - self._info_at < 2.0
            ):
                return replace(self._info_cache, live=self._is_live())

            row_count = 0
            time_start: Optional[float] = None
            time_end: Optional[float] = None
            for table in sorted(self._tables):
                sql = (
                    "SELECT COUNT(*), MIN({time}), MAX({time}) FROM {table}".format(
                        time=_quote_identifier("simulation_time_s"),
                        table=_quote_identifier(table),
                    )
                )
                row = self._query_one(sql)
                row_count += int(row[0] or 0)
                if row[1] is not None:
                    value = float(row[1])
                    time_start = value if time_start is None else min(time_start, value)
                if row[2] is not None:
                    value = float(row[2])
                    time_end = value if time_end is None else max(time_end, value)
            self._info_cache = RunInfo(
                run_id=self.run_id,
                path=str(self.path),
                kind="sqlite",
                label=self.label,
                signals=self.signals(),
                time_start=time_start,
                time_end=time_end,
                row_count=row_count,
                live=False,
            )
            self._info_key = key
            self._info_at = now
            return replace(self._info_cache, live=self._is_live())

    def signals(self) -> List[SignalInfo]:
        with self._lock:
            if self._signals_cache is not None:
                return list(self._signals_cache)
            result: List[SignalInfo] = []
            for table in sorted(self._tables):
                for column in self._tables[table]:
                    name = "{}.{}".format(table, column.name)
                    suffix = table
                    prefix = (
                        "I{}_".format(self.db_run_id)
                        if self.db_run_id is not None
                        else ""
                    )
                    if prefix and suffix.startswith(prefix):
                        suffix = suffix[len(prefix) :]
                    parts = suffix.split("_")
                    model = parts[0] or "signals"
                    storage = "_".join(parts[1:]) or model
                    result.append(
                        SignalInfo(
                            name=name,
                            label=name,
                            kind=column.kind,
                            group=table,
                            plottable=column.kind != "string",
                            path=[model, storage, column.name],
                        )
                    )
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
        with self._lock:
            lookup = self._signal_lookup()
            ordered: List[str] = []
            groups: Dict[str, List[Tuple[str, _Column]]] = {}
            for name in signals:
                if name not in lookup:
                    raise KeyError(name)
                if name in ordered:
                    continue
                ordered.append(name)
                table, column = lookup[name]
                groups.setdefault(table, []).append((name, column))

            by_name: Dict[str, Series] = {}
            for table, items in groups.items():
                columns = [column.name for _, column in items]
                sql = "SELECT {}, {} FROM {}".format(
                    _quote_identifier("simulation_time_s"),
                    ", ".join(_quote_identifier(column) for column in columns),
                    _quote_identifier(table),
                )
                clauses: List[str] = []
                params: List[float] = []
                if after is not None:
                    clauses.append("{} > ?".format(_quote_identifier("simulation_time_s")))
                    params.append(float(after))
                if until is not None:
                    clauses.append("{} <= ?".format(_quote_identifier("simulation_time_s")))
                    params.append(float(until))
                if clauses:
                    sql += " WHERE " + " AND ".join(clauses)
                sql += " ORDER BY {}".format(_quote_identifier("simulation_time_s"))

                rows = self._query(sql, params)
                indices = _decimate_indices(len(rows), max_points)
                if indices is not None:
                    rows = [rows[index] for index in indices]
                times = [float(row[0]) for row in rows]

                for offset, (name, column) in enumerate(items, start=1):
                    values = [_coerce(row[offset], column.kind) for row in rows]
                    by_name[name] = Series(
                        key="{}:{}".format(self.run_id, name),
                        run_id=self.run_id,
                        signal=name,
                        label=name,
                        t=list(times),
                        y=values,
                    )
            return [by_name[name] for name in ordered]

    def latest(self, signals: Sequence[str]) -> List[Value]:
        with self._lock:
            lookup = self._signal_lookup()
            ordered: List[str] = []
            groups: Dict[str, List[Tuple[str, _Column]]] = {}
            for name in signals:
                if name not in lookup:
                    raise KeyError(name)
                if name in ordered:
                    continue
                ordered.append(name)
                table, column = lookup[name]
                groups.setdefault(table, []).append((name, column))

            by_name: Dict[str, Value] = {}
            for table, items in groups.items():
                quoted_table = _quote_identifier(table)
                quoted_time = _quote_identifier("simulation_time_s")
                selects: List[str] = []
                for _, column in items:
                    quoted_column = _quote_identifier(column.name)
                    selects.append(
                        "(SELECT {time} FROM {table} WHERE {column} IS NOT NULL "
                        "ORDER BY {time} DESC LIMIT 1)".format(
                            time=quoted_time,
                            table=quoted_table,
                            column=quoted_column,
                        )
                    )
                    selects.append(
                        "(SELECT {column} FROM {table} WHERE {column} IS NOT NULL "
                        "ORDER BY {time} DESC LIMIT 1)".format(
                            time=quoted_time,
                            table=quoted_table,
                            column=quoted_column,
                        )
                    )
                rows = self._query("SELECT " + ", ".join(selects), [])
                row = rows[0] if rows else tuple(None for _ in selects)
                for index, (name, column) in enumerate(items):
                    time_value = row[index * 2]
                    raw_value = row[index * 2 + 1]
                    by_name[name] = Value(
                        signal=name,
                        label=name,
                        value=_coerce(raw_value, column.kind),
                        time=float(time_value) if time_value is not None else None,
                        kind=column.kind,
                        plottable=column.kind != "string",
                    )
            return [by_name[name] for name in ordered]

    def refresh(self) -> None:
        # Queries run in autocommit mode, so every read already sees the newest
        # committed WAL transaction. There is no incremental parse state.
        return

    def close(self) -> None:
        with self._lock:
            connection = self._connection
            self._connection = None
            if connection is not None:
                try:
                    connection.close()
                except sqlite3.Error:
                    pass

    # -- opening and discovery -------------------------------------------

    def _open_connection(self) -> sqlite3.Connection:
        uri = self._resolved.as_uri() + "?mode=ro"
        try:
            connection = sqlite3.connect(
                uri,
                uri=True,
                check_same_thread=False,
                isolation_level=None,
                timeout=5.0,
            )
            connection.execute("PRAGMA query_only = ON")
            return connection
        except sqlite3.Error as exc:
            raise SqliteResultError(
                "cannot open SQLite result file {}: {}".format(self._resolved, exc)
            ) from exc

    def _is_live(self) -> bool:
        # WAL presence alone is sticky: a closed writer can leave the sidecar
        # behind. Recency of the database or WAL keeps a finished run from
        # polling forever while still following an actively committing writer.
        wal = Path(str(self._resolved) + "-wal")
        if not wal.exists():
            return False
        try:
            if wal.stat().st_size == 0:
                # A read-only open can create an empty sidecar; that is not a
                # writer and must not make a finished run look live.
                return False
            newest = max(self._resolved.stat().st_mtime, wal.stat().st_mtime)
        except OSError:
            return True
        return time.time() - newest < 15.0

    def _make_run_id(self, db_run_id: int) -> str:
        seed = "{}\0{}".format(self._resolved, db_run_id).encode("utf-8")
        return hashlib.sha256(seed).hexdigest()[:10]

    def _table_names(self) -> List[str]:
        rows = self._query(
            "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name",
            [],
        )
        return [str(row[0]) for row in rows]

    def _select_run(self, requested: Optional[object]) -> int:
        if requested is not None:
            try:
                return int(requested)  # type: ignore[arg-type]
            except (TypeError, ValueError) as exc:
                raise SqliteResultError(
                    "invalid SQLite run id {!r}".format(requested)
                ) from exc

        names = self._table_names()
        if "ssp4sim_run_counter" in names:
            row = self._query_one(
                "SELECT MAX(run_id) FROM {}".format(
                    _quote_identifier("ssp4sim_run_counter")
                )
            )
            if row[0] is not None:
                return int(row[0])

        run_ids = []
        for name in names:
            match = _RUN_TABLE.match(name)
            if match:
                run_ids.append(int(match.group(1)))
        if run_ids:
            return max(run_ids)
        raise SqliteResultError(
            "no recorded runs found in SQLite result file {}".format(self._resolved)
        )

    def _discover_tables(self) -> None:
        prefix = "I{}_".format(self._db_run_id)
        for table in self._table_names():
            if not table.startswith(prefix):
                continue
            rows = self._query(
                "PRAGMA table_info({})".format(_quote_identifier(table)), []
            )
            names = [str(row[1]) for row in rows]
            if "simulation_time_s" not in names:
                continue
            columns: List[_Column] = []
            for row in rows:
                name = str(row[1])
                if name in ("timestamp_ns", "simulation_time_s"):
                    continue
                columns.append(_Column(name=name, kind=_declared_kind(row[2])))
            self._tables[table] = columns

    def _signal_lookup(self) -> Dict[str, Tuple[str, _Column]]:
        lookup: Dict[str, Tuple[str, _Column]] = {}
        for table in sorted(self._tables):
            for column in self._tables[table]:
                lookup["{}.{}".format(table, column.name)] = (table, column)
        return lookup

    # -- query helpers ----------------------------------------------------

    def _connection_or_raise(self) -> sqlite3.Connection:
        if self._connection is None:
            raise SqliteResultError("SQLite result source is closed")
        return self._connection

    def _query(
        self, sql: str, params: Sequence[object]
    ) -> List[Tuple[object, ...]]:
        connection = self._connection_or_raise()
        try:
            cursor = connection.execute(sql, tuple(params))
            return [tuple(row) for row in cursor.fetchall()]
        except sqlite3.Error as exc:
            raise SqliteResultError(
                "query failed on SQLite result file {}: {}".format(self._resolved, exc)
            ) from exc

    def _query_one(self, sql: str) -> Tuple[object, ...]:
        rows = self._query(sql, [])
        if not rows:
            return (None,)
        return rows[0]
