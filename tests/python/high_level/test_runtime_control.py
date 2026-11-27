from __future__ import annotations

import json
import threading
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

FIXTURE_NAME = "signal_step_gain"


def fixture_ssp_root() -> Path:
    model_dir = REFERENCE_SSP_ROOT / FIXTURE_NAME
    if not model_dir.exists():
        pytest.skip(f"missing reference fixture: {model_dir}")
    ssp_root = find_runnable_ssp(model_dir)
    if ssp_root is None:
        pytest.skip(f"no runnable SSP under {model_dir}")
    return ssp_root


def write_config(
    ssp_root: Path,
    workdir: Path,
    *,
    stop_time: float,
    timestep: float = 0.05,
    speed: float = 0.0,
    recording: bool = False,
) -> Path:
    config: dict[str, Any] = json.loads(GENERIC_CONFIG_PATH.read_text())
    simulation = config["simulation"]

    simulation["ssp"] = str(ssp_root)
    simulation["ssd"] = "SystemStructure.ssd"
    simulation["start_time"] = 0.0
    simulation["stop_time"] = stop_time
    simulation["timestep"] = timestep
    simulation["tolerance"] = 1e-4
    simulation["speed"] = speed
    simulation["working_dir"] = str(workdir)

    executor = simulation["executor"]
    executor["method"] = "jacobi"
    executor["forward_derivatives"] = False

    recording_config = simulation["recording"]
    recording_config.pop("enable", None)
    recording_config.pop("result_file", None)
    recording_config["csv"] = {
        "enable": recording,
        "file": str(workdir / "result.csv"),
        "interval": 0.0,
    }
    recording_config["wait_for"] = True
    recording_config["sqlite"] = {"enable": False}

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


def test_control_round_trip(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "control",
        stop_time=10.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    assert simulator.info()["state"] == "created"

    simulator.init()
    assert simulator.info()["state"] == "initialized"

    simulator.start()
    wait_for_state(simulator, "running")

    simulator.pause()
    wait_for_state(simulator, "paused")
    assert simulator.info()["state"] == "paused"

    simulator.resume()
    wait_for_state(simulator, "running")

    simulator.stop()
    assert simulator.wait() == "stopped"
    assert simulator.info()["state"] == "stopped"


def test_scheduled_control_lifecycle(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "scheduled",
        stop_time=10.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()

    simulator.start()
    wait_for_state(simulator, "running")

    # Argument plumbing only; the change is scheduled far enough ahead to be safe.
    simulator.set_speed(1.0, at=0.5)

    # At speed 1.0 the pause is about one wall-clock second away, so the run is
    # still observable as running immediately after the scheduling call.
    simulator.pause(at=1.0)
    assert simulator.info()["state"] == "running"
    wait_for_state(simulator, "paused")

    simulator.resume()
    wait_for_state(simulator, "running")

    simulator.stop(at=2.0)
    assert simulator.info()["state"] == "running"

    assert simulator.wait() == "stopped"
    assert simulator.info()["state"] == "stopped"


def test_simulate_releases_gil_for_pause(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "gil",
        stop_time=1.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()

    errors: list[BaseException] = []

    def run_simulation() -> None:
        try:
            simulator.simulate()
        except BaseException as error:
            errors.append(error)

    worker = threading.Thread(target=run_simulation)
    worker.start()
    try:
        wait_for_state(simulator, "running")
        simulator.pause()
        wait_for_state(simulator, "paused")
        assert simulator.info()["state"] == "paused"
        simulator.resume()
    finally:
        worker.join(timeout=15.0)

    assert not worker.is_alive(), "simulate() did not finish after resume"
    assert not errors, f"simulate() raised: {errors[0] if errors else ''}"
    assert simulator.info()["state"] == "completed"


def test_reset_rebuilds_initialized_run(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "reset",
        stop_time=0.2,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()
    simulator.start()
    wait_for_state(simulator, "running")

    simulator.reset()
    assert simulator.info()["state"] == "initialized"
    assert simulator.info()["sim_time_ns"] == 0

    simulator.start()
    assert simulator.wait() == "completed"


def test_info_tracks_the_run(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "info_tracking",
        stop_time=10.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    assert simulator.info()["speed"] == 1.0
    assert simulator.info()["sim_time_ns"] == 0

    simulator.init()
    assert simulator.info()["speed"] == 1.0
    assert simulator.info()["sim_time_ns"] == 0

    simulator.start()
    wait_for_state(simulator, "running")

    deadline = time.monotonic() + 5.0
    while simulator.info()["sim_time_ns"] == 0 and time.monotonic() < deadline:
        time.sleep(0.005)

    assert simulator.info()["sim_time_ns"] > 0
    assert simulator.info()["speed"] == 1.0

    simulator.stop()
    assert simulator.wait() == "stopped"
    assert simulator.info()["state"] == "stopped"

    # Partial results remain readable after a stopped run.
    assert simulator.info()["sim_time_ns"] > 0


def test_failed_run_surfaces_an_error(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "failed",
        stop_time=10.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    with pytest.raises(RuntimeError):
        simulator.wait()

    simulator.init()
    with pytest.raises(RuntimeError):
        simulator.wait()

    uninitialized = pyssp4sim.Simulator(str(config_path))
    with pytest.raises(RuntimeError):
        uninitialized.simulate()


def test_invalid_transitions_and_speed(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "invalid",
        stop_time=10.0,
        timestep=0.05,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()

    with pytest.raises(RuntimeError):
        simulator.pause()
    with pytest.raises(RuntimeError):
        simulator.resume()
    with pytest.raises(RuntimeError):
        simulator.stop()

    with pytest.raises(ValueError):
        simulator.set_speed(-1.0)
    with pytest.raises(ValueError):
        simulator.set_speed(float("inf"))


def test_create_with_missing_config_raises(tmp_path: Path) -> None:
    missing = tmp_path / "missing_config.json"
    with pytest.raises(RuntimeError):
        pyssp4sim.Simulator(str(missing))



def test_info_is_the_readback(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "info",
        stop_time=10.0,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()

    info = simulator.info()
    assert set(info) == {"state", "sim_time_ns", "speed"}
    assert isinstance(info["state"], str)
    assert isinstance(info["sim_time_ns"], int)
    assert isinstance(info["speed"], float)
    assert info["state"] == "initialized"
    assert info["sim_time_ns"] == 0
    assert info["speed"] == 1.0

    simulator.start()
    info = simulator.info()
    assert set(info) == {"state", "sim_time_ns", "speed"}
    assert info["state"] == "running"
    assert info["speed"] == 1.0
    assert info["sim_time_ns"] >= 0

    simulator.stop()
    assert simulator.wait() == "stopped"
    info = simulator.info()
    assert set(info) == {"state", "sim_time_ns", "speed"}
    assert info["state"] == "stopped"


def test_stop_after_terminal_is_noop(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "terminal",
        stop_time=0.3,
        speed=0.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()
    simulator.start()
    assert simulator.wait() == "completed"
    assert simulator.info()["state"] == "completed"

    simulator.stop()
    assert simulator.info()["state"] == "completed"
    simulator.stop()
    assert simulator.info()["state"] == "completed"
    assert simulator.info()["state"] == "completed"


def test_stop_immediately_after_start(tmp_path: Path) -> None:
    ssp_root = fixture_ssp_root()
    config_path = write_config(
        ssp_root,
        tmp_path / "immediate",
        stop_time=10.0,
        speed=1.0,
    )

    simulator = pyssp4sim.Simulator(str(config_path))
    simulator.init()
    simulator.start()
    simulator.stop()

    assert simulator.wait() == "stopped"
    assert simulator.info()["state"] == "stopped"
