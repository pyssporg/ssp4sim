import json
import os
import queue
import shutil
import socket
import tempfile
import threading
import time

import pytest

from ssp4sim_gui.backends.base import BackendError
from ssp4sim_gui.backends.daemon import DaemonBackend


def _make_socket_dir():
    directory = tempfile.mkdtemp(prefix="ssp4sim-gui-", dir="/tmp")
    return directory, os.path.join(directory, "daemon.sock")


class FakeDaemon:
    """Tiny AF_UNIX NDJSON server used only by these tests."""

    def __init__(self, path):
        self.path = path
        self.events = []
        self.respond = True
        self.raw_line = None
        self.last_request = None
        self._listener = None
        self._thread = None
        self._stop = threading.Event()

    def start(self):
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._run, name="fake-ssp4simd", daemon=True
        )
        self._thread.start()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if self._listener is not None:
                return
            time.sleep(0.005)
        raise AssertionError("fake daemon did not start")

    def stop(self):
        self._stop.set()
        listener = self._listener
        if listener is not None:
            try:
                listener.close()
            except OSError:
                pass
        thread = self._thread
        if thread is not None:
            thread.join(timeout=2.0)
        self._thread = None
        self._listener = None
        self._remove_socket()

    def _remove_socket(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def _run(self):
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            listener.bind(self.path)
            listener.listen(4)
            listener.settimeout(0.05)
            self._listener = listener
            while not self._stop.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break

                connection.settimeout(0.05)
                with connection:
                    buffer = b""
                    while not self._stop.is_set():
                        try:
                            chunk = connection.recv(65536)
                        except socket.timeout:
                            continue
                        except OSError:
                            break
                        if not chunk:
                            break
                        buffer += chunk
                        while b"\n" in buffer:
                            line, buffer = buffer.split(b"\n", 1)
                            if line.strip():
                                self._handle(connection, line)
        finally:
            try:
                listener.close()
            except OSError:
                pass
            self._remove_socket()

    def _handle(self, connection, line):
        try:
            message = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return

        self.last_request = message

        try:
            if self.raw_line is not None:
                connection.sendall(self.raw_line + b"\n")

            for event in list(self.events):
                connection.sendall(
                    json.dumps(event).encode("utf-8") + b"\n"
                )

            if not self.respond:
                return

            response = {"v": 1, "id": message.get("id"), "ok": True}
            if isinstance(message.get("cmd"), str):
                response["cmd"] = message["cmd"]
            connection.sendall(json.dumps(response).encode("utf-8") + b"\n")
        except OSError:
            return


def _wait_for(predicate, timeout=2.0, message="condition was not met"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError(message)


def _wait_event(subscription, kind, timeout=2.0):
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"no {kind!r} event within {timeout}s")
        try:
            event = subscription.get(timeout=remaining)
        except queue.Empty:
            raise AssertionError(f"no {kind!r} event within {timeout}s")
        if event.kind == kind:
            return event


def test_send_matches_response_while_event_reaches_subscriber():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        subscription = backend.subscribe()
        event = {
            "v": 1,
            "event": "state",
            "state": "running",
            "sim_time": 1.5,
            "speed": 2.0,
        }
        server.events = [event]

        response = backend.send(
            {"cmd": "state", "v": 99, "id": 123}, timeout=2.0
        )

        assert response["ok"] is True
        assert response["v"] == 1
        assert response["cmd"] == "state"
        assert isinstance(response["id"], int)
        assert response["id"] != 123
        assert server.last_request is not None
        assert server.last_request["v"] == 1
        assert server.last_request["id"] == response["id"]

        received = _wait_event(subscription, "state")
        assert received.kind == "state"
        assert received.data == event
        assert backend.status()["connected"] is True
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_events_fan_out_to_two_subscribers():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        first = backend.subscribe()
        second = backend.subscribe()
        event = {
            "v": 1,
            "event": "time",
            "sim_time": 2.5,
            "speed": 1.0,
        }
        server.events = [event]

        backend.send({"cmd": "state"}, timeout=2.0)

        for subscription in (first, second):
            received = _wait_event(subscription, "time")
            assert received.data == event
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_event_kinds_and_payloads_are_preserved():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        subscription = backend.subscribe()
        events = [
            {
                "v": 1,
                "event": "state",
                "state": "running",
                "sim_time": 0.5,
                "speed": 1.0,
            },
            {"v": 1, "event": "time", "sim_time": 1.0, "speed": 1.0},
            {"v": 1, "event": "done", "result": "success", "sim_time": 2.0},
            {
                "v": 1,
                "event": "error",
                "code": "simulation_failed",
                "message": "boom",
            },
        ]
        server.events = events

        backend.send({"cmd": "state"}, timeout=2.0)

        for expected in events:
            received = _wait_event(subscription, expected["event"])
            assert received.kind == expected["event"]
            assert received.data == expected
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_state_and_done_events_preserve_session_snapshot():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        subscription = backend.subscribe()
        artifacts = [{"kind": "csv", "path": "/abs/result.csv", "run_id": -1}]
        events = [
            {
                "v": 1,
                "event": "state",
                "state": "running",
                "session_id": "session-1",
                "artifacts": artifacts,
            },
            {
                "v": 1,
                "event": "done",
                "session_id": "session-1",
                "artifacts": artifacts,
            },
        ]
        server.events = events

        backend.send({"cmd": "state"}, timeout=2.0)

        for expected in events:
            received = _wait_event(subscription, expected["event"])
            assert received.data == expected
            assert received.data["session_id"] == "session-1"
            assert received.data["artifacts"] == artifacts
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_send_times_out_when_daemon_never_responds():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        _wait_for(
            lambda: backend.status()["connected"] is True,
            message="backend did not connect",
        )
        server.respond = False

        with pytest.raises(BackendError) as caught:
            backend.send({"cmd": "start"}, timeout=0.3)

        assert caught.value.code == "timeout"
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_send_without_server_raises_not_connected():
    directory, path = _make_socket_dir()
    backend = DaemonBackend(path, connect_timeout=0.2, retry_interval=0.05)
    try:
        status = backend.status()
        assert status["backend"] == "daemon"
        assert status["connected"] is False
        assert status["socket"] == path
        assert isinstance(status["detail"], str)

        with pytest.raises(BackendError) as caught:
            backend.send({"cmd": "state"}, timeout=0.3)

        assert caught.value.code == "not_connected"
    finally:
        backend.close()
        shutil.rmtree(directory, ignore_errors=True)


def test_backend_reconnects_after_server_restart():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=0.5, retry_interval=0.05
        )
        _wait_for(
            lambda: backend.status()["connected"] is True,
            message="backend did not connect",
        )
        assert backend.send({"cmd": "state"}, timeout=2.0)["ok"] is True
        subscription = backend.subscribe()

        server.stop()
        _wait_for(
            lambda: backend.status()["connected"] is False,
            message="backend did not notice the dropped connection",
        )
        dropped = _wait_event(subscription, "backend")
        assert dropped.data["connected"] is False
        assert dropped.data["socket"] == path
        assert isinstance(dropped.data["detail"], str)

        server.start()
        _wait_for(
            lambda: backend.status()["connected"] is True,
            message="backend did not reconnect",
        )
        restored = _wait_event(subscription, "backend")
        assert restored.data["connected"] is True
        assert restored.data["socket"] == path
        assert backend.send({"cmd": "state"}, timeout=2.0)["ok"] is True
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_unparsable_line_raises_bad_response():
    directory, path = _make_socket_dir()
    server = FakeDaemon(path)
    backend = None
    try:
        server.start()
        backend = DaemonBackend(
            path, connect_timeout=1.0, retry_interval=0.05
        )
        _wait_for(
            lambda: backend.status()["connected"] is True,
            message="backend did not connect",
        )
        server.raw_line = b"this is not json"

        with pytest.raises(BackendError) as caught:
            backend.send({"cmd": "state"}, timeout=2.0)

        assert caught.value.code == "bad_response"
    finally:
        if backend is not None:
            backend.close()
        server.stop()
        shutil.rmtree(directory, ignore_errors=True)


def test_close_is_idempotent_and_send_after_close_raises_closed():
    directory, path = _make_socket_dir()
    backend = DaemonBackend(path, connect_timeout=0.2, retry_interval=0.05)
    try:
        backend.close()
        backend.close()

        with pytest.raises(BackendError) as caught:
            backend.send({"cmd": "state"}, timeout=0.3)

        assert caught.value.code == "closed"
        assert backend.status()["connected"] is False
    finally:
        backend.close()
        shutil.rmtree(directory, ignore_errors=True)
