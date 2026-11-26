"""No-op control backend for viewing result files without a live engine."""

from __future__ import annotations

import queue
from typing import Any, Dict, Optional

from .base import BackendError


class OfflineBackend:
    """A backend that accepts no commands and emits no engine events."""

    name = "offline"

    def __init__(self, detail: str = "offline mode") -> None:
        self.detail = detail

    def status(self) -> Dict[str, Any]:
        return {
            "backend": self.name,
            "connected": False,
            "socket": None,
            "detail": self.detail,
        }

    def send(
        self, request: Dict[str, Any], timeout: float = 10.0
    ) -> Dict[str, Any]:
        raise BackendError("offline", self.detail)

    def subscribe(self) -> "queue.Queue[Event]":
        # No engine events exist offline. The HTTP layer sends the initial
        # backend status, so preloading one here would duplicate it.
        return queue.Queue()

    def unsubscribe(self, subscription: Optional["queue.Queue[Event]"]) -> None:
        return None

    def close(self) -> None:
        return None
