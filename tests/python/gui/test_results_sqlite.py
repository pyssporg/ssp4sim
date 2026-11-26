from __future__ import annotations

import sqlite3
from pathlib import Path

import pytest

from ssp4sim_gui.results.sqlite_source import SqliteResultError, SqliteResultSource


CONSUMER_COLUMNS = (
    '"timestamp_ns" INTEGER NOT NULL, '
    '"simulation_time_s" REAL NOT NULL, '
    '"y" REAL NOT NULL, '
    '"count" INTEGER NOT NULL, '
    '"name" TEXT NOT NULL'
)
AUX_COLUMNS = (
    '"timestamp_ns" INTEGER NOT NULL, '
    '"simulation_time_s" REAL NOT NULL, '
    '"temperature" REAL NOT NULL'
)


def _shared_database(tmp_path: Path) -> Path:
    path = tmp_path / "shared.sqlite"
    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute(
            "CREATE TABLE ssp4sim_run_counter (run_id INTEGER PRIMARY KEY)"
        )
        connection.execute("INSERT INTO ssp4sim_run_counter (run_id) VALUES (1)")
        connection.execute("INSERT INTO ssp4sim_run_counter (run_id) VALUES (2)")
        connection.execute(
            'CREATE TABLE "I1_Consumer_output" ({})'.format(CONSUMER_COLUMNS)
        )
        connection.execute('CREATE TABLE "I1_Aux_output" ({})'.format(AUX_COLUMNS))
        connection.execute(
            'CREATE TABLE "I2_Consumer_output" ({})'.format(CONSUMER_COLUMNS)
        )
        connection.executemany(
            'INSERT INTO "I1_Consumer_output" VALUES (?, ?, ?, ?, ?)',
            [
                (1, 0.0, 1.0, 10, "a"),
                (2, 1.0, 2.0, 11, "b"),
                (3, 2.0, 3.0, 12, "c"),
            ],
        )
        connection.executemany(
            'INSERT INTO "I1_Aux_output" VALUES (?, ?, ?)',
            [
                (1, 0.0, 100.0),
                (2, 1.0, 200.0),
                (3, 2.0, 300.0),
            ],
        )
        connection.executemany(
            'INSERT INTO "I2_Consumer_output" VALUES (?, ?, ?, ?, ?)',
            [
                (4, 0.0, 4.0, 20, "d"),
                (5, 0.5, 5.0, 21, "e"),
            ],
        )
        connection.commit()
    finally:
        connection.close()
    return path


def test_default_run_uses_counter_max(tmp_path: Path) -> None:
    path = _shared_database(tmp_path)
    source = SqliteResultSource(path)

    assert source.db_run_id == 2
    assert source.run_id == source.info().run_id
    assert [signal.name for signal in source.signals()] == [
        "I2_Consumer_output.y",
        "I2_Consumer_output.count",
        "I2_Consumer_output.name",
    ]
    info = source.info()
    assert info.kind == "sqlite"
    assert info.row_count == 2
    assert info.time_start == 0.0
    assert info.time_end == 0.5
    assert info.live is False
    signals = {signal.name: signal for signal in source.signals()}
    assert signals["I2_Consumer_output.y"].path == ["Consumer", "output", "y"]
    assert signals["I2_Consumer_output.name"].path == ["Consumer", "output", "name"]
    source.close()


def test_explicit_run_signal_names_groups_and_kinds(tmp_path: Path) -> None:
    path = _shared_database(tmp_path)
    source = SqliteResultSource(path, run_id=1)

    signals = {signal.name: signal for signal in source.signals()}
    assert set(signals) == {
        "I1_Consumer_output.y",
        "I1_Consumer_output.count",
        "I1_Consumer_output.name",
        "I1_Aux_output.temperature",
    }
    assert signals["I1_Consumer_output.y"].kind == "real"
    assert signals["I1_Consumer_output.count"].kind == "integer"
    assert signals["I1_Consumer_output.name"].kind == "string"
    assert signals["I1_Consumer_output.name"].plottable is False
    assert signals["I1_Consumer_output.y"].group == "I1_Consumer_output"
    assert signals["I1_Consumer_output.y"].label == "I1_Consumer_output.y"

    info = source.info()
    assert info.row_count == 6
    assert info.time_start == 0.0
    assert info.time_end == 2.0
    source.close()


def test_read_groups_by_table_filters_and_keeps_requested_order(tmp_path: Path) -> None:
    path = _shared_database(tmp_path)
    source = SqliteResultSource(path, run_id=1)

    series = source.read(
        ["I1_Aux_output.temperature", "I1_Consumer_output.y"],
        after=0.0,
        until=2.0,
    )
    assert [item.signal for item in series] == [
        "I1_Aux_output.temperature",
        "I1_Consumer_output.y",
    ]
    assert series[0].t == [1.0, 2.0]
    assert series[0].y == [200.0, 300.0]
    assert series[1].t == [1.0, 2.0]
    assert series[1].y == [2.0, 3.0]
    assert series[0].key == "{}:I1_Aux_output.temperature".format(source.run_id)

    decimated = source.read(["I1_Aux_output.temperature"], max_points=2)[0]
    assert decimated.t == [0.0, 2.0]
    assert decimated.y == [100.0, 300.0]
    source.close()


def test_latest_returns_newest_sample_per_signal(tmp_path: Path) -> None:
    path = _shared_database(tmp_path)
    source = SqliteResultSource(path, run_id=1)

    values = {
        value.signal: value
        for value in source.latest(
            ["I1_Aux_output.temperature", "I1_Consumer_output.name"]
        )
    }
    assert values["I1_Aux_output.temperature"].value == 300.0
    assert values["I1_Aux_output.temperature"].time == 2.0
    assert values["I1_Consumer_output.name"].value == "c"
    assert values["I1_Consumer_output.name"].time == 2.0
    source.close()


def test_latest_skips_trailing_nulls(tmp_path: Path) -> None:
    path = tmp_path / "nullable.sqlite"
    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute(
            "CREATE TABLE ssp4sim_run_counter (run_id INTEGER PRIMARY KEY)"
        )
        connection.execute("INSERT INTO ssp4sim_run_counter (run_id) VALUES (1)")
        connection.execute(
            'CREATE TABLE "I1_Model_output" ('
            '"timestamp_ns" INTEGER NOT NULL, '
            '"simulation_time_s" REAL NOT NULL, '
            '"x" REAL)'
        )
        connection.execute(
            'INSERT INTO "I1_Model_output" VALUES (1, 0.0, 1.0), (2, 1.0, NULL)'
        )
        connection.commit()
    finally:
        connection.close()

    source = SqliteResultSource(path)
    value = source.latest(["I1_Model_output.x"])[0]
    assert value.value == 1.0
    assert value.time == 0.0
    source.close()


def test_fallback_run_selection_from_table_prefix(tmp_path: Path) -> None:
    path = tmp_path / "prefix.sqlite"
    connection = sqlite3.connect(path)
    try:
        connection.execute(
            'CREATE TABLE "I3_Model_output" ({})'.format(AUX_COLUMNS)
        )
        connection.execute('INSERT INTO "I3_Model_output" VALUES (1, 0.0, 1.0)')
        connection.commit()
    finally:
        connection.close()

    source = SqliteResultSource(path)
    assert source.db_run_id == 3
    assert [signal.name for signal in source.signals()] == [
        "I3_Model_output.temperature"
    ]
    source.close()


def test_wal_sidecar_marks_run_live(tmp_path: Path) -> None:
    path = tmp_path / "live.sqlite"
    writer = sqlite3.connect(path)
    try:
        writer.execute("PRAGMA journal_mode=WAL")
        writer.execute("PRAGMA wal_autocheckpoint=0")
        writer.execute(
            "CREATE TABLE ssp4sim_run_counter (run_id INTEGER PRIMARY KEY)"
        )
        writer.execute("INSERT INTO ssp4sim_run_counter (run_id) VALUES (1)")
        writer.execute(
            'CREATE TABLE "I1_Model_output" ({})'.format(AUX_COLUMNS)
        )
        writer.execute('INSERT INTO "I1_Model_output" VALUES (1, 0.0, 1.0)')
        writer.commit()

        assert Path(str(path) + "-wal").exists()
        source = SqliteResultSource(path)
        try:
            assert source.info().live is True
            assert source.read(["I1_Model_output.temperature"])[0].y == [1.0]
        finally:
            source.close()
    finally:
        writer.close()


def test_missing_run_raises_clear_error(tmp_path: Path) -> None:
    path = tmp_path / "empty.sqlite"
    connection = sqlite3.connect(path)
    connection.close()

    with pytest.raises(SqliteResultError):
        SqliteResultSource(path)


def test_close_is_idempotent(tmp_path: Path) -> None:
    path = _shared_database(tmp_path)
    source = SqliteResultSource(path)
    source.close()
    source.close()
