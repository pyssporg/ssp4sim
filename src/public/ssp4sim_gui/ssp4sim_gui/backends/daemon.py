"""Control backend for the ``ssp4simd`` AF_UNIX daemon.

One background thread owns the socket: it connects, reconnects, reads
newline-delimited JSON, and writes queued requests. Public methods only touch
shared state under a condition variable, so constructing the backend or asking
for :meth:`DaemonBackend.status` never blocks on an absent daemon socket.
"""

from __future__ import annotations

import errno
import json
import os
import queue
import select
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Deque, Dict, Optional

from .base import BackendError, Event, publish

MAX_LINE = 1024 * 1024
_READ_TIMEOUT = 0.05
_WAIT_SLICE = 0.05


@dataclass
class _Pending:
    id: int
    payload: Dict[str, Any]
    deadline: float
    ever_connected: bool = False
    sent: bool = False
    response: Optional[Dict[str, Any]] = None
    error: Optional[BackendError] = field(default=None, repr=False)


class DaemonBackend:
    """Command channel to ``ssp4simd`` over newline-delimited JSON."""

    name = "daemon"

    def __init__(
        self,
        socket_path: str,
        *,
        connect_timeout: float = 5.0,
        retry_interval: float = 1.0,
    ) -> None:
        self.socket_path = str(socket_path)
        self._connect_timeout = max(0.0, float(connect_timeout))
        self._retry_interval = max(0.01, float(retry_interval))

        self._condition = threading.Condition()
        self._closed = False
        self._connected = False
        self._detail = "not connected"
        self._next_id = 0
        self._pending: Dict[int, _Pending] = {}
        self._outbox: Deque[_Pending] = deque()
        self._subscriptions: Dict[int, "queue.Queue[Event]"] = {}
        self._sock: Optional[socket.socket] = None
        self._stop = threading.Event()

        self._thread = threading.Thread(
            target=self._run,
            name=f"ssp4sim-daemon[{self.socket_path}]",
            daemon=True,
        )
        self._thread.start()

    # ------------------------------------------------------------------ public

    def status(self) -> Dict[str, Any]:
        with self._condition:
            return {
                "backend": self.name,
                "connected": self._connected,
                "socket": self.socket_path,
                "detail": self._detail,
            }

    def send(
        self, request: Dict[str, Any], timeout: float = 10.0
    ) -> Dict[str, Any]:
        if not isinstance(request, dict):
            raise BackendError("bad_request", "request must be a JSON object")

        timeout = max(0.0, float(timeout))
        deadline = time.monotonic() + timeout
        payload = {
            key: value
            for key, value in request.items()
            if key not in ("v", "id")
        }

        with self._condition:
            if self._closed:
                raise BackendError("closed", "daemon backend is closed")
            request_id = self._next_id
            self._next_id += 1
            pending = _Pending(
                id=request_id,
                payload=payload,
                deadline=deadline,
                ever_connected=self._connected,
            )
            self._pending[request_id] = pending
            self._outbox.append(pending)
            self._condition.notify_all()

        while True:
            with self._condition:
                if pending.response is not None:
                    return pending.response
                if pending.error is not None:
                    raise pending.error

                remaining = pending.deadline - time.monotonic()
                if remaining <= 0:
                    self._pending.pop(request_id, None)
                    self._prune_outbox_locked()
                    if pending.sent or pending.ever_connected:
                        raise BackendError(
                            "timeout",
                            f"request {request_id} timed out after {timeout:.3f}s",
                        )
                    raise BackendError(
                        "not_connected",
                        f"daemon socket is not reachable: {self.socket_path}",
                    )
                self._condition.wait(min(remaining, _WAIT_SLICE))

    def subscribe(self) -> "queue.Queue[Event]":
        subscription: "queue.Queue[Event]" = queue.Queue()
        with self._condition:
            self._subscriptions[id(subscription)] = subscription
        return subscription

    def unsubscribe(self, subscription: "queue.Queue[Event]") -> None:
        with self._condition:
            self._subscriptions.pop(id(subscription), None)

    def close(self) -> None:
        with self._condition:
            if self._closed:
                return
            self._closed = True
            self._stop.set()
            self._fail_all_pending_locked(
                BackendError("closed", "daemon backend is closed")
            )
            self._connected = False
            self._detail = "closed"
            sock = self._sock
            self._condition.notify_all()

        # Closing the socket wakes a worker blocked in select/recv/send.
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

        self._thread.join(timeout=2.0)

    # ------------------------------------------------------------- worker loop

    def _run(self) -> None:
        sock: Optional[socket.socket] = None
        buffer = b""
        next_connect = 0.0

        try:
            while not self._stop.is_set():
                if sock is None:
                    now = time.monotonic()
                    if now >= next_connect:
                        try:
                            sock = self._connect()
                        except OSError as exc:
                            sock = None
                            with self._condition:
                                self._detail = f"cannot connect: {exc}"
                                next_connect = (
                                    time.monotonic() + self._retry_interval
                                )
                                self._condition.notify_all()
                            self._expire_pending()
                        else:
                            buffer = b""
                            next_connect = 0.0
                            with self._condition:
                                closing = self._closed
                                if not closing:
                                    self._sock = sock
                                    self._connected = True
                                    self._detail = "connected"
                                    self._publish_locked(
                                        Event(
                                            "backend",
                                            {
                                                "connected": True,
                                                "detail": "connected",
                                                "socket": self.socket_path,
                                            },
                                        )
                                    )
                                    self._condition.notify_all()
                            if closing:
                                try:
                                    sock.close()
                                except OSError:
                                    pass
                                sock = None
                                break
                    if sock is None:
                        delay = max(
                            0.0,
                            min(next_connect - time.monotonic(), _READ_TIMEOUT),
                        )
                        self._stop.wait(delay)
                        self._expire_pending()
                        continue

                try:
                    readable, _, _ = select.select(
                        [sock], [], [], _READ_TIMEOUT
                    )
                except (OSError, ValueError) as exc:
                    self._disconnect(f"select failed: {exc}")
                    sock, buffer = None, b""
                    next_connect = time.monotonic() + self._retry_interval
                    continue

                if readable:
                    try:
                        chunk = sock.recv(65536)
                    except (BlockingIOError, InterruptedError):
                        chunk = None
                    except OSError as exc:
                        self._disconnect(f"connection lost: {exc}")
                        sock, buffer = None, b""
                        next_connect = time.monotonic() + self._retry_interval
                        continue

                    if chunk == b"":
                        self._disconnect("daemon closed the connection")
                        sock, buffer = None, b""
                        next_connect = time.monotonic() + self._retry_interval
                        continue

                    if chunk:
                        buffer += chunk
                        while sock is not None:
                            newline = buffer.find(b"\n")
                            if newline < 0:
                                break
                            line = buffer[:newline]
                            buffer = buffer[newline + 1 :]
                            if not self._handle_line(line):
                                self._disconnect("malformed response")
                                sock, buffer = None, b""
                                next_connect = (
                                    time.monotonic() + self._retry_interval
                                )
                                break

                        if sock is not None and len(buffer) > MAX_LINE:
                            self._disconnect("response line is too long")
                            sock, buffer = None, b""
                            next_connect = time.monotonic() + self._retry_interval
                            continue

                while sock is not None:
                    with self._condition:
                        pending = self._next_outbox_locked()
                    if pending is None:
                        break

                    envelope: Dict[str, Any] = {"v": 1, "id": pending.id}
                    envelope.update(pending.payload)
                    data = (
                        json.dumps(envelope, separators=(",", ":")) + "\n"
                    ).encode("utf-8")

                    try:
                        self._send_all(sock, data)
                    except OSError as exc:
                        self._disconnect(f"send failed: {exc}")
                        sock, buffer = None, b""
                        next_connect = time.monotonic() + self._retry_interval
                        break

                    with self._condition:
                        pending.sent = True
                        self._condition.notify_all()

                self._expire_pending()
        finally:
            with self._condition:
                self._sock = None
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
            with self._condition:
                self._connected = False
                if not self._closed:
                    self._detail = "backend stopped"
                self._fail_all_pending_locked(
                    BackendError("closed", "daemon backend stopped")
                )
                self._condition.notify_all()

    # ---------------------------------------------------------------- helpers

    def _connect(self) -> socket.socket:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            error = sock.connect_ex(self.socket_path)
            if error not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK):
                raise OSError(error, os.strerror(error))
            if error != 0:
                deadline = time.monotonic() + self._connect_timeout
                while True:
                    if self._stop.is_set():
                        raise OSError("backend is stopping")
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise socket.timeout("connect timed out")
                    _, writable, exceptional = select.select(
                        [],
                        [sock],
                        [sock],
                        min(remaining, _WAIT_SLICE),
                    )
                    if writable or exceptional:
                        break
                error = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                if error:
                    raise OSError(error, os.strerror(error))
        except BaseException:
            sock.close()
            raise
        return sock

    def _send_all(self, sock: socket.socket, data: bytes) -> None:
        view = memoryview(data)
        while view:
            if self._stop.is_set():
                raise OSError("backend is stopping")
            _, writable, exceptional = select.select(
                [], [sock], [sock], _WAIT_SLICE
            )
            if exceptional:
                raise OSError("socket error while writing")
            if not writable:
                continue
            sent = sock.send(view)
            if sent == 0:
                raise OSError("socket closed while writing")
            view = view[sent:]

    def _next_outbox_locked(self) -> Optional[_Pending]:
        while self._outbox:
            pending = self._outbox.popleft()
            if pending.id in self._pending:
                return pending
        return None

    def _prune_outbox_locked(self) -> None:
        if self._outbox:
            self._outbox = deque(
                pending
                for pending in self._outbox
                if pending.id in self._pending
            )

    def _handle_line(self, line: bytes) -> bool:
        if not line.strip():
            return True

        try:
            message = json.loads(line)
        except (UnicodeDecodeError, ValueError):
            self._fail_all_pending(
                BackendError("bad_response", "daemon sent unparsable JSON")
            )
            return False

        if not isinstance(message, dict):
            self._fail_all_pending(
                BackendError("bad_response", "daemon response is not an object")
            )
            return False

        if "event" in message:
            kind = message.get("event")
            if not isinstance(kind, str):
                self._fail_all_pending(
                    BackendError("bad_response", "event name must be a string")
                )
                return False
            with self._condition:
                self._publish_locked(Event(kind, message))
            return True

        if "id" in message:
            request_id = message.get("id")
            with self._condition:
                pending = (
                    self._pending.pop(request_id, None)
                    if isinstance(request_id, int)
                    and not isinstance(request_id, bool)
                    else None
                )
                if pending is not None:
                    pending.response = message
                    self._condition.notify_all()
            return True

        self._fail_all_pending(
            BackendError(
                "bad_response",
                "daemon message has neither \"id\" nor \"event\"",
            )
        )
        return False

    def _publish_locked(self, event: Event) -> None:
        if self._closed:
            return
        publish(self._subscriptions, event)

    def _fail_all_pending(self, error: BackendError) -> None:
        with self._condition:
            self._fail_all_pending_locked(error)
            self._condition.notify_all()

    def _fail_all_pending_locked(self, error: BackendError) -> None:
        for pending in list(self._pending.values()):
            pending.error = BackendError(error.code, error.message)
        self._pending.clear()
        self._outbox.clear()

    def _expire_pending(self) -> None:
        now = time.monotonic()
        with self._condition:
            for pending in list(self._pending.values()):
                if now < pending.deadline:
                    continue
                self._pending.pop(pending.id, None)
                if pending.sent or pending.ever_connected:
                    pending.error = BackendError(
                        "timeout",
                        f"request {pending.id} timed out",
                    )
                else:
                    pending.error = BackendError(
                        "not_connected",
                        f"daemon socket is not reachable: {self.socket_path}",
                    )
            self._prune_outbox_locked()
            self._condition.notify_all()

    def _disconnect(self, detail: str) -> None:
        with self._condition:
            sock = self._sock
            self._sock = None
            was_connected = self._connected
            self._connected = False
            self._detail = detail
            if was_connected:
                self._publish_locked(
                    Event(
                        "backend",
                        {
                            "connected": False,
                            "detail": detail,
                            "socket": self.socket_path,
                        },
                    )
                )
            self._fail_all_pending_locked(
                BackendError("not_connected", detail)
            )
            self._condition.notify_all()

        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
