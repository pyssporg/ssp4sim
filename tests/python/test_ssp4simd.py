from __future__ import annotations

import csv
import json
import re
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import pytest


def find_upwards(start: Path, target: str) -> Path:
    current = start.resolve()

    for parent in [current] + list(current.parents):
        if (parent / target).exists():
            return parent

    raise FileNotFoundError(f"Could not find '{target}' in any parent directory")


SSP4SIM_ROOT = find_upwards(Path(__file__), "__SSP4SIM_ROOT__")
SSP4SIMD = SSP4SIM_ROOT / "build" / "src" / "public" / "ssp4sim_server" / "ssp4simd"
FIXTURE = (
    SSP4SIM_ROOT
    / "resources"
    / "reference_ssp"
    / "artifacts"
    / "models"
    / "signal_step_gain"
    / "baseline"
    / "model"
)

DAHLQUIST = (
    SSP4SIM_ROOT
    / "resources"
    / "reference_ssp"
    / "artifacts"
    / "models"
    / "Dahlquist"
    / "baseline"
    / "model"
)

pytestmark = pytest.mark.skipif(
    not SSP4SIMD.exists(), reason="ssp4simd build artifact is missing"
)


def build_config(
    workdir: Path,
    *,
    stop_time: float = 3.0,
    speed: float = 1.0,
    timestep: float = 0.05,
    fixture: Path | None = None,
    ssd: str = "SystemStructure.ssd",
    csv_file: Path | None = None,
    snapshot_dir: Path | None = None,
) -> Path:
    workdir.mkdir(parents=True, exist_ok=True)
    config = {
        "simulation": {
            "ssp": str(fixture if fixture is not None else FIXTURE),
            "ssd": ssd,
            "start_time": 0.0,
            "stop_time": stop_time,
            "timestep": timestep,
            "tolerance": 1e-4,
            "speed": speed,
            "working_dir": str(workdir),
            "executor": {
                "method": "jacobi",
                "thread_pool_workers": 5,
                "forward_derivatives": False,
                "jacobi": {"parallel": True, "method": 1},
                "seidel": {"parallel": False},
            },
            "recording": {
                "csv": {
                    "enable": True,
                    "file": str(
                        csv_file if csv_file is not None else workdir / "result.csv"
                    ),
                    "interval": 0.0,
                },
                "wait_for": True,
                "sqlite": {"enable": False},
            },
            "log": {
                "fmu": False,
                "level_terminal": "disable",
                "level_file": "disable",
                "level_json": "disable",
                "level_cutelog": "disable",
            },
        }
    }
    if snapshot_dir is not None:
        config["simulation"]["snapshot"] = {"directory": str(snapshot_dir)}
    path = workdir / "config.json"
    path.write_text(json.dumps(config, indent=2))
    return path


class Daemon:
    def __init__(
        self,
        workdir: Path,
        socket_path: Path,
        *,
        stop_time: float = 3.0,
        speed: float = 1.0,
        timestep: float = 0.05,
        fixture: Path | None = None,
        ssd: str = "SystemStructure.ssd",
        csv_file: Path | None = None,
        snapshot_dir: Path | None = None,
    ) -> None:
        config_path = build_config(
            workdir,
            stop_time=stop_time,
            speed=speed,
            timestep=timestep,
            fixture=fixture,
            ssd=ssd,
            csv_file=csv_file,
            snapshot_dir=snapshot_dir,
        )
        self.workdir = workdir
        self.socket_path = socket_path
        self.process = subprocess.Popen(
            [
                str(SSP4SIMD),
                "--config",
                str(config_path),
                "--socket",
                str(socket_path),
            ],
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.socket: socket.socket | None = None
        self.buffer = b""
        self.events: list[dict] = []
        self._connect()

    def _connect(self) -> None:
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                stdout, stderr = self.process.communicate()
                pytest.fail(
                    "ssp4simd exited before accepting connections\n"
                    f"stdout:\n{stdout}\nstderr:\n{stderr}"
                )
            if self.socket_path.exists():
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(5.0)
                try:
                    sock.connect(str(self.socket_path))
                except OSError:
                    sock.close()
                else:
                    self.socket = sock
                    self.buffer = b""
                    return
            time.sleep(0.05)
        pytest.fail("timed out waiting for ssp4simd socket")

    def sendall(self, payload: bytes) -> None:
        assert self.socket is not None
        self.socket.sendall(payload)

    def send(self, request: dict) -> None:
        self.sendall(json.dumps(request).encode() + b"\n")

    def _read_message(self, timeout: float) -> dict:
        assert self.socket is not None
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out reading a protocol line")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(64 * 1024)
            if not chunk:
                raise ConnectionError("ssp4simd closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def response(self, request: dict, timeout: float = 10.0) -> dict:
        self.send(request)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self._read_message(max(0.01, deadline - time.monotonic()))
            if "event" in message:
                self.events.append(message)
                continue
            if message.get("id") == request.get("id"):
                return message
            self.events.append(message)
        raise TimeoutError(f"no response for request {request!r}")

    def wait_event(self, name: str, timeout: float = 15.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, event in enumerate(self.events):
                if event.get("event") == name:
                    return self.events.pop(index)
            message = self._read_message(max(0.01, deadline - time.monotonic()))
            if message.get("event") == name:
                return message
            self.events.append(message)
        raise TimeoutError(f"timed out waiting for {name!r} event")

    def wait_lifecycle(
        self, operation: str, *, ok: bool = True, timeout: float = 30.0
    ) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for index, event in enumerate(self.events):
                if (
                    event.get("event") == "lifecycle"
                    and event.get("operation") == operation
                    and event.get("ok") is ok
                ):
                    return self.events.pop(index)
            message = self._read_message(max(0.01, deadline - time.monotonic()))
            if (
                message.get("event") == "lifecycle"
                and message.get("operation") == operation
                and message.get("ok") is ok
            ):
                return message
            self.events.append(message)
        raise TimeoutError(f"timed out waiting for {operation!r} lifecycle event")

    def close(self) -> None:
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def shutdown(self) -> None:
        self.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


@pytest.fixture()
def daemon(tmp_path: Path):
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")
    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        instance = Daemon(tmp_path / "work", Path(socket_dir) / "s.sock")
        try:
            yield instance
        finally:
            instance.shutdown()


def read_csv_value(path: Path, column: str, at_time: float) -> float:
    assert path.exists(), f"missing result file: {path}"
    with path.open(newline="") as input_file:
        rows = list(csv.DictReader(input_file))
    row = next(row for row in rows if abs(float(row["time"]) - at_time) < 1e-6)
    return float(row[column])


def test_lifecycle_and_shutdown(daemon: Daemon) -> None:
    # Known real output target in the fixture: model "gain", variable "gain.y".
    target = "gain.y"

    assert daemon.response({"v": 1, "id": 2, "cmd": "set_speed", "speed": 1.0})[
        "ok"
    ]
    assert daemon.response({"v": 1, "id": 3, "cmd": "start"})["ok"]

    running = daemon.wait_event("state")
    while running["state"] != "running":
        running = daemon.wait_event("state")
    assert running["state"] == "running", running
    assert running["sim_time"] >= 0.0

    time_event = daemon.wait_event("time")
    assert time_event["sim_time"] >= 0.0

    state = daemon.response({"v": 1, "id": 4, "cmd": "set_speed", "speed": 2.0})
    assert state["ok"], state

    state = daemon.response({"v": 1, "id": 5, "cmd": "state"})
    assert state["ok"] and state["state"] in {"running", "paused", "completed"}
    assert state["speed"] == pytest.approx(2.0)
    assert isinstance(state["sim_time"], float)
    assert state["sim_time"] >= 0.0

    assert daemon.response({"v": 1, "id": 6, "cmd": "pause"})["ok"]
    paused = daemon.wait_event("state")
    while paused["state"] != "paused":
        paused = daemon.wait_event("state")
    assert paused["state"] == "paused"

    assert daemon.response({"v": 1, "id": 7, "cmd": "resume"})["ok"]
    resumed = daemon.wait_event("state")
    while resumed["state"] != "running":
        resumed = daemon.wait_event("state")
    assert resumed["state"] == "running"

    assert daemon.response({"v": 1, "id": 10, "cmd": "stop"})["ok"]
    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done

    reset = daemon.response({"v": 1, "id": 11, "cmd": "reset"})
    assert reset["ok"] and reset["accepted"], reset
    lifecycle = daemon.wait_lifecycle("reset")
    assert lifecycle["state"] == "initialized", lifecycle
    assert lifecycle["sim_time"] == 0.0, lifecycle
    state = daemon.response({"v": 1, "id": 12, "cmd": "state"})
    assert state["state"] == "initialized", state
    assert state["sim_time"] == 0.0, state
    assert state["speed"] == 1.0, state

    # A fresh run records the fixture's baseline result.
    assert daemon.response({"v": 1, "id": 14, "cmd": "set_speed", "speed": 0.0})[
        "ok"
    ]
    assert daemon.response({"v": 1, "id": 16, "cmd": "start"})["ok"]

    done = daemon.wait_event("done", timeout=30.0)
    while done["result"] != "completed":
        done = daemon.wait_event("done", timeout=30.0)
    assert done["result"] == "completed"

    result_file = daemon.workdir / "result.csv"
    assert read_csv_value(result_file, target, 0.2) == pytest.approx(3.0)

    assert daemon.response({"v": 1, "id": 17, "cmd": "shutdown"})["ok"]
    socket_path = daemon.socket_path
    assert daemon.process.wait(timeout=20) == 0
    assert not socket_path.exists()


def test_reset_while_running(daemon: Daemon) -> None:
    assert daemon.response({"v": 1, "id": 18, "cmd": "start"})["ok"]

    running = daemon.wait_event("state")
    while running["state"] != "running":
        running = daemon.wait_event("state")

    reset = daemon.response({"v": 1, "id": 19, "cmd": "reset"}, timeout=10.0)
    assert reset["ok"] and reset["accepted"], reset
    lifecycle = daemon.wait_lifecycle("reset")
    assert lifecycle["state"] == "initialized", lifecycle

    # The old live run reports its terminal events before the fresh state.
    stopped = daemon.wait_event("state")
    assert stopped["state"] == "stopped", stopped
    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done
    initialized = daemon.wait_event("state")
    assert initialized["state"] == "initialized", initialized

    # Reset stops and waits for the live run, then rebuilds the runner.
    assert daemon.process.poll() is None
    state = daemon.response({"v": 1, "id": 20, "cmd": "state"})
    assert state["ok"], state
    assert state["state"] == "initialized", state
    assert state["sim_time"] == 0.0, state
    assert state["speed"] == 1.0, state


def test_reset_is_async_and_reports_lifecycle(daemon: Daemon) -> None:
    assert daemon.response({"v": 1, "id": 30, "cmd": "start"})["ok"]

    running = daemon.wait_event("state")
    while running["state"] != "running":
        running = daemon.wait_event("state")

    reset = daemon.response({"v": 1, "id": 31, "cmd": "reset"}, timeout=10.0)
    assert reset["ok"] and reset["accepted"], reset

    lifecycle = daemon.wait_lifecycle("reset")
    assert lifecycle["state"] == "initialized", lifecycle
    assert lifecycle["sim_time"] == 0.0, lifecycle

    state = daemon.response({"v": 1, "id": 32, "cmd": "state"})
    assert state["ok"], state
    assert state["state"] == "initialized", state
    assert state["sim_time"] == 0.0, state


def test_sigterm_unlinks_socket(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")
    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(tmp_path / "work", socket_path)
        try:
            instance.close()
            instance.process.send_signal(signal.SIGTERM)
            assert instance.process.wait(timeout=10) == 0
            assert not socket_path.exists()
        finally:
            instance.shutdown()


def test_scheduled_at_commands(daemon: Daemon) -> None:
    # A scheduled speed change is accepted before the run starts.
    scheduled_speed = daemon.response(
        {"v": 1, "id": 30, "cmd": "set_speed", "speed": 0.0, "at": 1.0}
    )
    assert scheduled_speed["ok"], scheduled_speed

    # Invalid "at" values are rejected with bad_value on every command.
    invalid_at = [
        ("pause", {"at": -1.0}),
        ("stop", {"at": "later"}),
        ("set_speed", {"speed": 1.0, "at": 1.0e300}),
    ]
    for index, (command, fields) in enumerate(invalid_at):
        response = daemon.response(
            {"v": 1, "id": 31 + index, "cmd": command, **fields}
        )
        assert response["ok"] is False, response
        assert response["error"]["code"] == "bad_value", response

    assert daemon.response({"v": 1, "id": 46, "cmd": "set_speed", "speed": 0.0})[
        "ok"
    ]
    assert daemon.response({"v": 1, "id": 47, "cmd": "start"})["ok"]
    done = daemon.wait_event("done", timeout=30.0)
    while done["result"] != "completed":
        done = daemon.wait_event("done", timeout=30.0)
    assert done["result"] == "completed", done


def test_scheduled_pause_and_stop(daemon: Daemon) -> None:
    assert daemon.response({"v": 1, "id": 50, "cmd": "set_speed", "speed": 0.2})[
        "ok"
    ]
    assert daemon.response({"v": 1, "id": 51, "cmd": "start"})["ok"]

    running = daemon.wait_event("state")
    while running["state"] != "running":
        running = daemon.wait_event("state")

    state = daemon.response({"v": 1, "id": 52, "cmd": "state"})
    paused = daemon.response(
        {"v": 1, "id": 53, "cmd": "pause", "at": state["sim_time"] + 0.05}
    )
    assert paused["ok"], paused

    event = daemon.wait_event("state")
    while event["state"] != "paused":
        event = daemon.wait_event("state")
    assert event["state"] == "paused"

    assert daemon.response({"v": 1, "id": 54, "cmd": "resume"})["ok"]
    event = daemon.wait_event("state")
    while event["state"] != "running":
        event = daemon.wait_event("state")

    state = daemon.response({"v": 1, "id": 55, "cmd": "state"})
    stopped = daemon.response(
        {"v": 1, "id": 56, "cmd": "stop", "at": state["sim_time"] + 0.05}
    )
    assert stopped["ok"], stopped

    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done


def test_protocol_errors_keep_daemon_alive(daemon: Daemon) -> None:
    daemon.sendall(b'{"v":1,"id":20,"cmd":\n')
    malformed = daemon._read_message(5.0)
    assert malformed["ok"] is False
    assert malformed["error"]["code"] == "bad_json"

    unknown = daemon.response({"v": 1, "id": 21, "cmd": "does_not_exist"})
    assert unknown["ok"] is False
    assert unknown["error"]["code"] == "unknown_command"

    try:
        daemon.sendall(b'{"v":1,"id":23,"cmd":"' + b"a" * (1024 * 1024) + b"\n")
    except OSError:
        # The daemon may close the read side as soon as the line limit is hit.
        pass
    too_long = daemon._read_message(10.0)
    assert too_long["ok"] is False
    assert too_long["error"]["code"] == "line_too_long"

    daemon.close()
    daemon._connect()
    state = daemon.response({"v": 1, "id": 24, "cmd": "state"})
    assert state["ok"] and state["state"] == "initialized"

    assert daemon.response({"v": 1, "id": 25, "cmd": "shutdown"})["ok"]
    socket_path = daemon.socket_path
    assert daemon.process.wait(timeout=20) == 0
    assert not socket_path.exists()


def test_stop_immediately_after_start(daemon: Daemon) -> None:
    assert daemon.response({"v": 1, "id": 60, "cmd": "start"})["ok"]
    assert daemon.response({"v": 1, "id": 61, "cmd": "stop"})["ok"]

    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done

    state = daemon.response({"v": 1, "id": 62, "cmd": "state"})
    assert state["state"] == "stopped", state
    assert state["sim_time"] >= 0.0


def test_stop_after_done_is_noop(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "done", socket_path, stop_time=0.3, speed=0.0
        )
        try:
            assert instance.response({"v": 1, "id": 70, "cmd": "start"})["ok"]
            done = instance.wait_event("done", timeout=30.0)
            while done["result"] != "completed":
                done = instance.wait_event("done", timeout=30.0)
            assert done["result"] == "completed", done

            assert instance.response({"v": 1, "id": 71, "cmd": "stop"})["ok"]
            state = instance.response({"v": 1, "id": 72, "cmd": "state"})
            assert state["state"] == "completed", state

            assert instance.response({"v": 1, "id": 73, "cmd": "stop"})["ok"]
            state = instance.response({"v": 1, "id": 74, "cmd": "state"})
            assert state["state"] == "completed", state
        finally:
            instance.shutdown()


def test_failed_run_emits_error_then_done(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "failed",
            socket_path,
            stop_time=1.0,
            speed=0.0,
            timestep=0.0,
        )
        try:
            assert instance.response({"v": 1, "id": 90, "cmd": "start"})["ok"]

            events = list(instance.events)
            instance.events.clear()
            while True:
                message = instance._read_message(15.0)
                events.append(message)
                if message.get("event") == "done":
                    break

            errors = [e for e in events if e.get("event") == "error"]
            dones = [e for e in events if e.get("event") == "done"]
            assert len(errors) == 1, events
            assert errors[0]["code"] == "simulation_failed", errors[0]
            assert len(dones) == 1, events
            assert dones[0]["result"] == "failed", dones[0]
            assert events.index(errors[0]) < events.index(dones[0])

            state = instance.response({"v": 1, "id": 91, "cmd": "state"})
            assert state["state"] == "failed", state
            assert not [
                e for e in instance.events if e.get("event") in {"error", "done"}
            ]
        finally:
            instance.shutdown()


def test_shutdown_immediately_after_start(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "shutdown", socket_path, stop_time=60.0, speed=1.0
        )
        try:
            assert instance.response({"v": 1, "id": 80, "cmd": "start"})["ok"]
            assert instance.response({"v": 1, "id": 81, "cmd": "shutdown"})["ok"]

            # Read the terminal sequence to EOF without skipping duplicates.
            messages = list(instance.events)
            instance.events.clear()
            while True:
                try:
                    messages.append(instance._read_message(10.0))
                except ConnectionError:
                    break

            # A 60 s run must not delay shutdown once stop is requested.
            assert instance.process.wait(timeout=15) == 0
            assert not socket_path.exists()

            done_events = [m for m in messages if m.get("event") == "done"]
            assert len(done_events) == 1, done_events
            assert done_events[0]["result"] == "stopped", done_events[0]

            done_index = messages.index(done_events[0])
            terminal_states = [
                m
                for m in messages[:done_index]
                if m.get("event") == "state" and m.get("state") == "stopped"
            ]
            assert terminal_states, messages
        finally:
            instance.shutdown()


def test_shutdown_during_reset(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "shutdown_during_reset",
            socket_path,
            stop_time=60.0,
            speed=1.0,
        )
        try:
            assert instance.response({"v": 1, "id": 82, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")
            assert running["state"] == "running", running

            reset = instance.response({"v": 1, "id": 83, "cmd": "reset"})
            assert reset["ok"] and reset["accepted"], reset

            assert instance.response({"v": 1, "id": 84, "cmd": "shutdown"})["ok"]

            # A reset in flight must not prevent the lifecycle thread join.
            assert instance.process.wait(timeout=15) == 0
            assert not socket_path.exists()
        finally:
            instance.shutdown()


def test_state_during_reset(tmp_path: Path) -> None:
    if not FIXTURE.exists():
        pytest.skip(f"missing reference fixture: {FIXTURE}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "state_during_reset",
            socket_path,
            stop_time=60.0,
            speed=1.0,
        )
        try:
            assert instance.response({"v": 1, "id": 85, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")
            assert running["state"] == "running", running

            reset = instance.response({"v": 1, "id": 86, "cmd": "reset"})
            assert reset["ok"] and reset["accepted"], reset

            state = instance.response({"v": 1, "id": 87, "cmd": "state"})
            assert state["ok"] is True, state
            assert "error" not in state, state
            assert state["state"] in {
                "running",
                "paused",
                "stopped",
                "initialized",
            }, state

            lifecycle = instance.wait_lifecycle("reset")
            assert lifecycle["state"] == "initialized", lifecycle

            assert instance.response({"v": 1, "id": 88, "cmd": "shutdown"})["ok"]
        finally:
            instance.shutdown()


def test_snapshot_and_restore(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        snapshot_path = tmp_path / "run.snap"
        instance = Daemon(
            tmp_path / "snapshot",
            socket_path,
            stop_time=2.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "dahlquist.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 100, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            response = instance.response(
                {"v": 1, "id": 101, "cmd": "snapshot", "path": str(snapshot_path)}
            )
            assert response["ok"] and response["accepted"], response

            event = instance.wait_event("snapshot", timeout=15.0)
            assert event["ok"], event
            assert event["bytes"] > 0, event
            assert snapshot_path.exists()

            assert instance.response({"v": 1, "id": 102, "cmd": "stop"})["ok"]
            done = instance.wait_event("done")
            while done["result"] != "stopped":
                done = instance.wait_event("done")

            response = instance.response(
                {
                    "v": 1,
                    "id": 103,
                    "cmd": "restore",
                    "path": str(snapshot_path),
                    "start": True,
                },
                timeout=30.0,
            )
            assert response["ok"] and response["accepted"], response
            lifecycle = instance.wait_lifecycle("restore")
            assert lifecycle["state"] == "running", lifecycle

            resumed = instance.wait_event("state")
            while resumed["state"] != "running":
                resumed = instance.wait_event("state")

            assert instance.response({"v": 1, "id": 104, "cmd": "shutdown"})["ok"]
            assert instance.process.wait(timeout=20) == 0
        finally:
            instance.shutdown()


def test_restore_start_false_is_async(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        snapshot_path = tmp_path / "start-false.snap"
        instance = Daemon(
            tmp_path / "start-false",
            socket_path,
            stop_time=5.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "start-false.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 105, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            response = instance.response(
                {"v": 1, "id": 106, "cmd": "snapshot", "path": str(snapshot_path)}
            )
            assert response["ok"] and response["accepted"], response
            event = instance.wait_event("snapshot", timeout=15.0)
            assert event["ok"], event

            response = instance.response(
                {
                    "v": 1,
                    "id": 107,
                    "cmd": "restore",
                    "path": str(snapshot_path),
                    "start": False,
                },
                timeout=30.0,
            )
            assert response["ok"] and response["accepted"], response
            lifecycle = instance.wait_lifecycle("restore")
            assert lifecycle["state"] == "initialized", lifecycle

            # The lifecycle event also marks the handoff, so start is accepted
            # again once it arrives.
            state = instance.response({"v": 1, "id": 108, "cmd": "state"})
            assert state["state"] == "initialized", state

            assert instance.response({"v": 1, "id": 109, "cmd": "start"})["ok"]
            resumed = instance.wait_event("state")
            while resumed["state"] != "running":
                resumed = instance.wait_event("state")
            assert resumed["state"] == "running"
        finally:
            instance.shutdown()


def test_snapshot_while_paused(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        snapshot_path = tmp_path / "paused.snap"
        instance = Daemon(
            tmp_path / "snapshot-paused",
            socket_path,
            stop_time=2.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "dahlquist-paused.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 110, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response({"v": 1, "id": 111, "cmd": "pause"})["ok"]
            paused = instance.wait_event("state")
            while paused["state"] != "paused":
                paused = instance.wait_event("state")
            assert paused["state"] == "paused"

            response = instance.response(
                {"v": 1, "id": 112, "cmd": "snapshot", "path": str(snapshot_path)}
            )
            assert response["ok"] and response["accepted"], response

            # The response only accepts the request; completion arrives as an
            # event once the worker drains the event queue.
            event = instance.wait_event("snapshot", timeout=15.0)
            assert event["ok"], event
            assert event["path"] == str(snapshot_path)
            assert snapshot_path.exists(), event
        finally:
            instance.shutdown()



def test_snapshot_events_carry_request_ids(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        first = tmp_path / "first.snap"
        second = tmp_path / "second.snap"
        instance = Daemon(
            tmp_path / "correlated",
            socket_path,
            stop_time=5.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "correlated.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 200, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response({"v": 1, "id": 201, "cmd": "pause"})["ok"]
            paused = instance.wait_event("state")
            while paused["state"] != "paused":
                paused = instance.wait_event("state")

            for request_id, path in ((201, first), (202, second)):
                response = instance.response(
                    {"v": 1, "id": request_id, "cmd": "snapshot", "path": str(path)}
                )
                assert response["ok"] and response["accepted"], response

            events = [instance.wait_event("snapshot") for _ in range(2)]
            by_id = {event.get("id"): event for event in events}
            assert set(by_id) == {201, 202}, events
            assert by_id[201]["path"] == str(first), events
            assert by_id[202]["path"] == str(second), events
            assert all(event["ok"] and event["code"] == "ok" for event in events), events
            assert first.exists() and second.exists()
        finally:
            instance.shutdown()


def test_snapshot_path_policy(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    snaps = tmp_path / "snaps"
    snaps.mkdir()
    outside = tmp_path / "outside.snap"
    inside = snaps / "inside.snap"
    external = tmp_path / "external"
    external.mkdir()
    escaping_link = snaps / "escape"
    escaping_link.symlink_to(external, target_is_directory=True)

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "policy",
            socket_path,
            stop_time=5.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "policy.csv",
            snapshot_dir=snaps,
        )
        try:
            assert instance.response({"v": 1, "id": 205, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            rejected = instance.response(
                {"v": 1, "id": 206, "cmd": "snapshot", "path": str(outside)}
            )
            assert rejected["ok"] is False, rejected
            assert rejected["error"]["code"] == "bad_value", rejected

            symlinked = instance.response(
                {
                    "v": 1,
                    "id": 209,
                    "cmd": "snapshot",
                    "path": str(escaping_link / "escape.snap"),
                }
            )
            assert symlinked["ok"] is False, symlinked
            assert symlinked["error"]["code"] == "bad_value", symlinked

            accepted = instance.response(
                {"v": 1, "id": 207, "cmd": "snapshot", "path": str(inside)}
            )
            assert accepted["ok"] and accepted["accepted"], accepted
            event = instance.wait_event("snapshot")
            assert event["ok"] and event["id"] == 207, event
            assert inside.exists()

            restore = instance.response(
                {
                    "v": 1,
                    "id": 208,
                    "cmd": "restore",
                    "path": str(outside),
                    "start": False,
                }
            )
            assert restore["ok"] is False, restore
            assert restore["error"]["code"] == "bad_value", restore
        finally:
            instance.shutdown()


def test_restore_writes_uuid_csv_segment(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    original = tmp_path / "segment.csv"
    snapshot_path = tmp_path / "segment.snap"

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "segment",
            socket_path,
            stop_time=2.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=original,
        )
        try:
            assert instance.response({"v": 1, "id": 210, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response(
                {"v": 1, "id": 211, "cmd": "snapshot", "path": str(snapshot_path)}
            )["ok"]
            event = instance.wait_event("snapshot")
            assert event["ok"], event

            response = instance.response(
                {
                    "v": 1,
                    "id": 212,
                    "cmd": "restore",
                    "path": str(snapshot_path),
                    "start": True,
                },
                timeout=30.0,
            )
            assert response["ok"] and response["accepted"], response
            lifecycle = instance.wait_lifecycle("restore")
            assert lifecycle["ok"], lifecycle

            assert original.exists(), "the prior run's CSV must be preserved"
            segments = list(tmp_path.glob("segment.restored-*.csv"))
            assert len(segments) == 1, segments
            uuid = re.fullmatch(
                r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-"
                r"[89ab][0-9a-f]{3}-[0-9a-f]{12}",
                segments[0].stem.removeprefix("segment.restored-"),
            )
            assert uuid, segments[0].name
        finally:
            instance.shutdown()


def test_restore_preserves_prior_speed(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    snapshot_path = tmp_path / "speed.snap"

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "speed",
            socket_path,
            stop_time=5.0,
            speed=2.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "speed.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 220, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response(
                {"v": 1, "id": 221, "cmd": "snapshot", "path": str(snapshot_path)}
            )["ok"]
            event = instance.wait_event("snapshot")
            assert event["ok"], event

            response = instance.response(
                {
                    "v": 1,
                    "id": 222,
                    "cmd": "restore",
                    "path": str(snapshot_path),
                    "start": True,
                },
                timeout=30.0,
            )
            assert response["ok"] and response["accepted"], response
            lifecycle = instance.wait_lifecycle("restore")
            assert lifecycle["state"] == "running", lifecycle

            state = instance.response({"v": 1, "id": 223, "cmd": "state"})
            assert state["ok"], state
            assert state["speed"] == pytest.approx(2.0), state
        finally:
            instance.shutdown()


def test_reset_installs_fresh_runner(tmp_path: Path) -> None:
    if not DAHLQUIST.exists():
        pytest.skip(f"missing reference fixture: {DAHLQUIST}")

    before = tmp_path / "before.snap"
    after = tmp_path / "after.snap"

    with tempfile.TemporaryDirectory(prefix="ssp4simd-") as socket_dir:
        socket_path = Path(socket_dir) / "s.sock"
        instance = Daemon(
            tmp_path / "reset-fresh",
            socket_path,
            stop_time=5.0,
            speed=1.0,
            timestep=0.01,
            fixture=DAHLQUIST,
            csv_file=tmp_path / "reset-fresh.csv",
        )
        try:
            assert instance.response({"v": 1, "id": 240, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response(
                {"v": 1, "id": 241, "cmd": "snapshot", "path": str(before)}
            )["ok"]
            event = instance.wait_event("snapshot", timeout=15.0)
            assert event["code"] == "ok", event

            reset = instance.response({"v": 1, "id": 242, "cmd": "reset"})
            assert reset["ok"] and reset["accepted"], reset
            lifecycle = instance.wait_lifecycle("reset")
            assert lifecycle["state"] == "initialized", lifecycle
            assert lifecycle["sim_time"] == 0.0, lifecycle

            assert instance.response({"v": 1, "id": 243, "cmd": "start"})["ok"]
            running = instance.wait_event("state")
            while running["state"] != "running":
                running = instance.wait_event("state")

            assert instance.response(
                {"v": 1, "id": 244, "cmd": "snapshot", "path": str(after)}
            )["ok"]
            event = instance.wait_event("snapshot", timeout=15.0)
            assert event["code"] == "ok", event
            assert event["bytes"] > 0, event
            assert after.exists()
        finally:
            instance.shutdown()
