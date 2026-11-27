from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

import pytest

from ._helpers import (
    GENERIC_CONFIG_PATH,
    REFERENCE_SSP_ROOT,
    find_runnable_ssp,
    pyssp4sim,
)

FIXTURE_NAME = "Dahlquist"


def fixture_ssp_root() -> Path:
    model_dir = REFERENCE_SSP_ROOT / FIXTURE_NAME
    if not model_dir.exists():
        pytest.skip(f"missing reference fixture: {model_dir}")
    ssp_root = find_runnable_ssp(model_dir)
    if ssp_root is None:
        pytest.skip(f"no runnable SSP under {model_dir}")
    return ssp_root


def write_config(ssp_root: Path, workdir: Path) -> Path:
    config: dict[str, Any] = json.loads(GENERIC_CONFIG_PATH.read_text())
    simulation = config["simulation"]

    simulation["ssp"] = str(ssp_root)
    simulation["ssd"] = "SystemStructure.ssd"
    simulation["start_time"] = 0.0
    simulation["stop_time"] = 2.0
    simulation["timestep"] = 0.01
    simulation["tolerance"] = 1e-6
    simulation["speed"] = 0.1
    simulation["working_dir"] = str(workdir)

    executor = simulation["executor"]
    executor["method"] = "jacobi"
    executor["forward_derivatives"] = False

    recording = simulation["recording"]
    recording["csv"] = {"enable": False}
    recording["sqlite"] = {"enable": False}
    recording["wait_for"] = False

    log_config = simulation["log"]
    log_config["fmu"] = False
    log_config["level_terminal"] = "disable"
    log_config["level_file"] = "disable"
    log_config["level_json"] = "disable"
    log_config["level_cutelog"] = "disable"

    workdir.mkdir(parents=True, exist_ok=True)
    config_path = workdir / "config.json"
    config_path.write_text(json.dumps(config, indent=2))
    return config_path


def wait_for_state(simulator: Any, expected: str, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if simulator.info()["state"] == expected:
            return
        time.sleep(0.005)
    assert simulator.info()["state"] == expected


def test_snapshot_restore_round_trip(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(ssp_root, tmp_path / "snapshot")

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()
    simulator.start()
    wait_for_state(simulator, "running")

    # Pause first so the published time and the snapshot agree; a running
    # snapshot can complete while the control plane has already advanced.
    simulator.pause()
    wait_for_state(simulator, "paused")
    snapshot_time_ns = simulator.info()["sim_time_ns"]

    # A path-like object exercises PyUnicode_FSConverter on the snapshot side.
    snapshot_path = tmp_path / "snapshot" / "state.snap"
    assert simulator.snapshot(snapshot_path) is None
    assert snapshot_path.exists()

    simulator.stop()
    assert simulator.wait() == "stopped"

    # Bytes exercise the same path parser on the restore side.
    assert simulator.restore(os.fsencode(snapshot_path), True) is None
    info = simulator.info()
    assert info["state"] == "running"
    assert info["sim_time_ns"] >= snapshot_time_ns

    simulator.stop()
    assert simulator.wait() == "stopped"
