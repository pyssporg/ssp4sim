from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from ssp4sim_gui.results.csv_source import CsvFormatError, CsvResultSource


HEADER = "time,gain.y,Consumer.consumerFeed.X[1],count,label\n"


def _append(path: Path, text: str) -> None:
    with path.open("a", encoding="utf-8", newline="") as handle:
        handle.write(text)


def _write(path: Path, text: str) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def _make_source(tmp_path: Path) -> CsvResultSource:
    path = tmp_path / "run.csv"
    _write(
        path,
        HEADER
        + "0.000000, 1.000000, , 3, alpha\n"
        + "0.100000, -2.500000, 7.000000, 4, beta\n",
    )
    return CsvResultSource(path)


def test_header_signals_kinds_and_skipinitialspace(tmp_path: Path) -> None:
    source = _make_source(tmp_path)
    signals = {signal.name: signal for signal in source.signals()}

    assert list(signals) == [
        "gain.y",
        "Consumer.consumerFeed.X[1]",
        "count",
        "label",
    ]
    assert signals["gain.y"].kind == "real"
    assert signals["Consumer.consumerFeed.X[1]"].kind == "real"
    assert signals["count"].kind == "integer"
    assert signals["label"].kind == "string"
    assert signals["label"].plottable is False
    assert signals["gain.y"].plottable is True

    series = source.read(
        ["gain.y", "Consumer.consumerFeed.X[1]", "count", "label"]
    )
    assert series[0].t == [0.0, 0.1]
    assert series[0].y == [1.0, -2.5]
    assert series[1].y == [None, 7.0]
    assert series[2].y == [3, 4]
    assert series[3].y == ["alpha", "beta"]


def test_run_id_and_info(tmp_path: Path) -> None:
    source = _make_source(tmp_path)
    expected = hashlib.sha256(str(source.path.resolve()).encode("utf-8")).hexdigest()[:10]

    assert source.run_id == expected
    info = source.info()
    assert info.run_id == expected
    assert info.kind == "csv"
    assert info.row_count == 2
    assert info.time_start == 0.0
    assert info.time_end == 0.1


def test_missing_time_column_is_a_value_error(tmp_path: Path) -> None:
    path = tmp_path / "start_values.csv"
    _write(path, "name,value\ngain,1\n")

    assert issubclass(CsvFormatError, ValueError)
    with pytest.raises(CsvFormatError):
        CsvResultSource(path)


def test_incremental_refresh_keeps_partial_tail(tmp_path: Path) -> None:
    path = tmp_path / "live.csv"
    _write(path, HEADER)
    source = CsvResultSource(path)
    assert source.info().row_count == 0

    # Second row is deliberately partial: no trailing newline yet.
    _append(path, "0.0, 1.0, 5.0, 1, a\n0.1, 2.0, 6.0, 2, b")
    source.refresh()
    assert source.info().row_count == 1
    assert source.read(["gain.y"])[0].y == [1.0]

    _append(path, "\n0.2, 3.0, 7.0, 3, c\n")
    source.refresh()
    assert source.info().row_count == 3
    assert source.read(["Consumer.consumerFeed.X[1]"])[0].y == [5.0, 6.0, 7.0]

    _append(path, "0.3, 4.0")
    source.refresh()
    assert source.info().row_count == 3

    _append(path, ", 8.0, 4, d\n")
    source.refresh()
    assert source.info().row_count == 4
    assert source.read(["gain.y"], after=0.2)[0].y == [4.0]


def test_after_is_exclusive_and_until_is_inclusive(tmp_path: Path) -> None:
    source = _make_source(tmp_path)

    series = source.read(["gain.y"], after=0.0, until=0.1)
    assert series[0].t == [0.1]
    assert series[0].y == [-2.5]

    series = source.read(["gain.y"], after=0.1)
    assert series[0].t == []


def test_decimation_keeps_last_sample(tmp_path: Path) -> None:
    path = tmp_path / "many.csv"
    lines = ["time,gain.y\n"]
    for index in range(10):
        lines.append("{:.1f}, {}\n".format(index / 10.0, index))
    _write(path, "".join(lines))

    source = CsvResultSource(path)
    series = source.read(["gain.y"], max_points=3)[0]
    assert len(series.t) == 3
    assert series.t[0] == 0.0
    assert series.t[-1] == 0.9
    assert series.t == sorted(series.t)

    last_only = source.read(["gain.y"], max_points=1)[0]
    assert last_only.t == [0.9]
    assert last_only.y == [9]


def test_latest_uses_newest_non_null_sample(tmp_path: Path) -> None:
    path = tmp_path / "latest.csv"
    _write(
        path,
        HEADER
        + "0.0, 1.0, 5.0, 1, a\n"
        + "0.1, 2.0, , 2, b\n"
        + "0.2, , 7.0, 3, c\n"
        + "0.3, , , 4, d\n",
    )
    source = CsvResultSource(path)

    latest = {value.signal: value for value in source.latest(["gain.y", "Consumer.consumerFeed.X[1]"])}
    assert latest["gain.y"].value == 2.0
    assert latest["gain.y"].time == 0.1
    assert latest["Consumer.consumerFeed.X[1]"].value == 7.0
    assert latest["Consumer.consumerFeed.X[1]"].time == 0.2


def test_close_is_idempotent(tmp_path: Path) -> None:
    source = _make_source(tmp_path)
    source.close()
    source.close()


def test_truncated_file_restarts_from_header(tmp_path: Path) -> None:
    path = tmp_path / "reset.csv"
    _write(path, HEADER + "0.0, 1.0, 5.0, 1, a\n0.1, 2.0, 6.0, 2, b\n")
    source = CsvResultSource(path)
    assert source.info().row_count == 2

    path.write_text(HEADER + "0.0, 9.0, 8.0, 7, z\n", encoding="utf-8")
    source.refresh()

    info = source.info()
    assert info.row_count == 1
    assert info.time_end == 0.0
    assert source.read(["gain.y"])[0].y == [9.0]


def test_signal_paths_are_hierarchical(tmp_path: Path) -> None:
    source = _make_source(tmp_path)
    signals = {signal.name: signal for signal in source.signals()}
    assert signals["gain.y"].path == ["gain", "y"]
    assert signals["Consumer.consumerFeed.X[1]"].path == [
        "Consumer",
        "consumerFeed",
        "X[1]",
    ]
