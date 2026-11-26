from __future__ import annotations

import csv
import json
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

pytestmark = pytest.mark.skipif(
    not SSP4SIMD.exists(), reason="ssp4simd build artifact is missing"
)


def build_config(
    workdir: Path,
    *,
    stop_time: float = 3.0,
    speed: float = 1.0,
    timestep: float = 0.05,
) -> Path:
    workdir.mkdir(parents=True, exist_ok=True)
    config = {
        "simulation": {
            "ssp": str(FIXTURE),
            "ssd": "SystemStructure.ssd",
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
                    "file": str(workdir / "result.csv"),
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
    ) -> None:
        config_path = build_config(
            workdir, stop_time=stop_time, speed=speed, timestep=timestep
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


def assert_csv_identity(message: dict, workdir: Path) -> str:
    """Assert the run identity fields and return the session id."""
    session_id = message.get("session_id")
    assert isinstance(session_id, str) and session_id, message

    artifacts = message.get("artifacts")
    assert isinstance(artifacts, list), message
    assert len(artifacts) == 1, message

    artifact = artifacts[0]
    assert artifact["kind"] == "csv", artifact
    assert artifact["run_id"] == -1, artifact

    path = Path(artifact["path"])
    assert path.is_absolute(), artifact
    assert path == workdir / "result.csv", artifact
    assert path.exists(), artifact
    return session_id


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
    live_session = assert_csv_identity(running, daemon.workdir)

    time_event = daemon.wait_event("time")
    assert time_event["sim_time"] >= 0.0

    state = daemon.response({"v": 1, "id": 4, "cmd": "set_speed", "speed": 2.0})
    assert state["ok"], state

    state = daemon.response({"v": 1, "id": 5, "cmd": "state"})
    assert state["ok"] and state["state"] in {"running", "paused", "completed"}
    assert state["speed"] == pytest.approx(2.0)
    assert isinstance(state["sim_time"], float)
    assert state["sim_time"] >= 0.0
    assert assert_csv_identity(state, daemon.workdir) == live_session

    assert daemon.response({"v": 1, "id": 6, "cmd": "pause"})["ok"]
    paused = daemon.wait_event("state")
    while paused["state"] != "paused":
        paused = daemon.wait_event("state")
    assert paused["state"] == "paused"
    assert assert_csv_identity(paused, daemon.workdir) == live_session

    assert daemon.response({"v": 1, "id": 7, "cmd": "resume"})["ok"]
    resumed = daemon.wait_event("state")
    while resumed["state"] != "running":
        resumed = daemon.wait_event("state")
    assert resumed["state"] == "running"
    assert assert_csv_identity(resumed, daemon.workdir) == live_session

    assert daemon.response({"v": 1, "id": 10, "cmd": "stop"})["ok"]
    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done
    assert assert_csv_identity(done, daemon.workdir) == live_session

    assert daemon.response({"v": 1, "id": 11, "cmd": "reset"})["ok"]
    state = daemon.response({"v": 1, "id": 12, "cmd": "state"})
    assert state["state"] == "initialized", state
    assert state["sim_time"] == 0.0, state
    assert state["speed"] == 1.0, state
    reset_session = assert_csv_identity(state, daemon.workdir)
    assert reset_session != live_session

    # A fresh run records the fixture's baseline result.
    assert daemon.response({"v": 1, "id": 14, "cmd": "set_speed", "speed": 0.0})[
        "ok"
    ]
    assert daemon.response({"v": 1, "id": 16, "cmd": "start"})["ok"]

    done = daemon.wait_event("done", timeout=30.0)
    while done["result"] != "completed":
        done = daemon.wait_event("done", timeout=30.0)
    assert done["result"] == "completed"
    assert assert_csv_identity(done, daemon.workdir) == reset_session

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
    live_session = assert_csv_identity(running, daemon.workdir)

    reset = daemon.response({"v": 1, "id": 19, "cmd": "reset"}, timeout=10.0)
    assert reset["ok"], reset

    # The old live run reports its terminal events before the fresh state.
    stopped = daemon.wait_event("state")
    assert stopped["state"] == "stopped", stopped
    assert assert_csv_identity(stopped, daemon.workdir) == live_session
    done = daemon.wait_event("done")
    assert done["result"] == "stopped", done
    assert assert_csv_identity(done, daemon.workdir) == live_session
    initialized = daemon.wait_event("state")
    assert initialized["state"] == "initialized", initialized
    assert assert_csv_identity(initialized, daemon.workdir) != live_session

    # Reset stops and waits for the live run, then rebuilds the runner.
    assert daemon.process.poll() is None
    state = daemon.response({"v": 1, "id": 20, "cmd": "state"})
    assert state["ok"], state
    assert state["state"] == "initialized", state
    assert state["sim_time"] == 0.0, state
    assert state["speed"] == 1.0, state


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
