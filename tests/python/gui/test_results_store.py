from __future__ import annotations

import os
import sqlite3
from pathlib import Path

import pytest

from ssp4sim_gui.results.store import ResultStore, StoreError


def _write_csv(path: Path, rows: str = "0.0, 1.0\n") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        handle.write("time,gain.y\n" + rows)


def _write_sqlite(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(path)
    try:
        connection.execute(
            "CREATE TABLE ssp4sim_run_counter (run_id INTEGER PRIMARY KEY)"
        )
        connection.execute("INSERT INTO ssp4sim_run_counter (run_id) VALUES (1)")
        connection.execute(
            'CREATE TABLE "I1_Model_output" ('
            '"timestamp_ns" INTEGER NOT NULL, '
            '"simulation_time_s" REAL NOT NULL, '
            '"x" REAL NOT NULL)'
        )
        connection.execute('INSERT INTO "I1_Model_output" VALUES (1, 0.0, 1.0)')
        connection.commit()
    finally:
        connection.close()


def _touch(path: Path, seconds: int) -> None:
    os.utime(str(path), (seconds, seconds))


def test_store_error_exposes_code_and_message() -> None:
    error = StoreError("code", "message")
    assert error.code == "code"
    assert error.message == "message"
    assert str(error) == "message"


def test_resolve_rejects_paths_outside_roots(tmp_path: Path) -> None:
    root = tmp_path / "results"
    root.mkdir()
    outside = tmp_path / "outside.csv"
    _write_csv(outside)
    store = ResultStore([root])

    with pytest.raises(StoreError) as excinfo:
        store.resolve(outside)
    assert excinfo.value.code == "outside-root"

    with pytest.raises(StoreError) as excinfo:
        store.resolve(root / ".." / "outside.csv")
    assert excinfo.value.code == "outside-root"

    with pytest.raises(StoreError) as excinfo:
        store.resolve(root)
    assert excinfo.value.code == "not-a-file"


def test_resolve_accepts_files_below_a_root(tmp_path: Path) -> None:
    root = tmp_path / "results"
    nested = root / "day" / "run.csv"
    _write_csv(nested)
    store = ResultStore([root])

    assert store.resolve(nested) == nested.resolve()
    assert store.roots() == [str(root.resolve())]


def test_discover_newest_first_skips_bad_and_sidecars(tmp_path: Path) -> None:
    root = tmp_path / "results"
    old = root / "old.csv"
    new = root / "new.csv"
    shallow = root / "sub" / "shallow.csv"
    deep = root / "sub" / "deep" / "deep.csv"
    too_deep = root / "sub" / "deep" / "deeper" / "too_deep.csv"
    bad = root / "start_values.csv"
    sqlite_file = root / "run.sqlite"

    _write_csv(old)
    _write_csv(new)
    _write_csv(shallow)
    _write_csv(deep)
    _write_csv(too_deep)
    bad.write_text("name,value\ngain,1\n", encoding="utf-8")
    _write_sqlite(sqlite_file)
    (root / "run.sqlite-wal").write_text("sidecar", encoding="utf-8")
    (root / "notes.txt").write_text("ignore", encoding="utf-8")

    _touch(old, 1_000)
    _touch(shallow, 1_500)
    _touch(new, 2_000)
    _touch(deep, 2_500)
    _touch(too_deep, 4_000)
    _touch(bad, 3_000)
    _touch(sqlite_file, 3_500)

    store = ResultStore([root])
    runs = store.discover()
    paths = [run.path for run in runs]

    assert paths == [
        str(sqlite_file.resolve()),
        str(deep.resolve()),
        str(new.resolve()),
        str(shallow.resolve()),
        str(old.resolve()),
    ]
    assert all("start_values" not in path for path in paths)
    assert all("too_deep" not in path for path in paths)
    assert {run.kind for run in runs if run.path.endswith(".csv")} == {"csv"}
    assert next(run for run in runs if run.kind == "sqlite").run_id

    for run in runs:
        assert store.get(run.run_id).info().run_id == run.run_id


def test_open_is_cached_by_stable_run_id(tmp_path: Path) -> None:
    root = tmp_path / "results"
    path = root / "run.csv"
    _write_csv(path)
    store = ResultStore([root])

    first = store.open(path)
    second = store.open(path)
    assert first.run_id == second.run_id
    assert store.get(first.run_id).info().run_id == first.run_id


def test_open_get_and_close(tmp_path: Path) -> None:
    root = tmp_path / "results"
    path = root / "run.csv"
    _write_csv(path)
    store = ResultStore([root])

    info = store.open(path)
    source = store.get(info.run_id)
    assert source.path == path.resolve()

    store.close_run(info.run_id)
    with pytest.raises(KeyError):
        store.get(info.run_id)
    with pytest.raises(KeyError):
        store.close_run(info.run_id)

    store.close()
    with pytest.raises(KeyError):
        store.get(info.run_id)


def test_open_rejects_unsupported_and_unreadable(tmp_path: Path) -> None:
    root = tmp_path / "results"
    root.mkdir()
    unsupported = root / "notes.txt"
    unsupported.write_text("ignore", encoding="utf-8")
    unreadable = root / "start_values.csv"
    unreadable.write_text("name,value\ngain,1\n", encoding="utf-8")
    store = ResultStore([root])

    with pytest.raises(StoreError) as excinfo:
        store.open(unsupported)
    assert excinfo.value.code == "unsupported-artifact"

    with pytest.raises(StoreError) as excinfo:
        store.open(unreadable)
    assert excinfo.value.code == "unreadable-artifact"


def test_closing_twice_is_safe(tmp_path: Path) -> None:
    root = tmp_path / "results"
    _write_csv(root / "run.csv")
    store = ResultStore([root])
    store.open(root / "run.csv")
    store.close()
    store.close()


def test_extensions_are_configurable(tmp_path: Path) -> None:
    root = tmp_path / "results"
    _write_csv(root / "run.csv")
    _write_sqlite(root / "run.sqlite")
    store = ResultStore([root], extensions=(".sqlite",))

    runs = store.discover()
    assert [run.path for run in runs] == [str((root / "run.sqlite").resolve())]
    assert runs[0].kind == "sqlite"
