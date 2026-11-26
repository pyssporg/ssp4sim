import queue

import pytest

from ssp4sim_gui.backends.base import BackendError
from ssp4sim_gui.backends.offline import OfflineBackend


def test_offline_status_and_name():
    backend = OfflineBackend()
    assert backend.name == "offline"
    assert backend.status() == {
        "backend": "offline",
        "connected": False,
        "socket": None,
        "detail": "offline mode",
    }

    custom = OfflineBackend(detail="no daemon configured")
    assert custom.status()["detail"] == "no daemon configured"


def test_offline_send_raises_offline():
    backend = OfflineBackend(detail="control disabled")
    with pytest.raises(BackendError) as caught:
        backend.send({"cmd": "start"})
    assert caught.value.code == "offline"
    assert caught.value.message == "control disabled"


def test_offline_subscribe_is_empty_and_unsubscribe_is_noop():
    backend = OfflineBackend()
    subscription = backend.subscribe()
    assert subscription.empty()

    backend.unsubscribe(subscription)
    backend.unsubscribe(queue.Queue())


def test_offline_close_is_idempotent():
    backend = OfflineBackend()
    backend.close()
    backend.close()
    assert backend.status()["connected"] is False
