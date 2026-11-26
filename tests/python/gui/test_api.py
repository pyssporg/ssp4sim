import queue

import pytest

pytest.importorskip("fastapi")
pytest.importorskip("httpx")

from fastapi.testclient import TestClient  # noqa: E402

from ssp4sim_gui.app import create_app  # noqa: E402
from ssp4sim_gui.results.store import ResultStore  # noqa: E402

CSV_TEXT = """time,gain.y,sine.y
0, 0.000000, 0.000000
0.5, 1.000000, 0.500000
1.0, 2.000000, 1.000000
"""


class StubBackend:
    name = "stub"

    def __init__(self):
        self.requests = []
        self.subscriptions = []
        self.state_response = {
            "v": 1,
            "ok": True,
            "state": "paused",
            "sim_time": 1.0,
            "speed": 2.0,
        }

    def status(self):
        return {"backend": "stub", "connected": True, "detail": "in-memory"}

    def send(self, request, timeout=10.0):
        self.requests.append(dict(request))
        if request.get("cmd") == "state":
            return dict(self.state_response)
        return {"v": 1, "ok": True}

    def subscribe(self):
        subscription = queue.Queue()
        self.subscriptions.append(subscription)
        return subscription

    def unsubscribe(self, subscription):
        self.subscriptions.remove(subscription)

    def close(self):
        pass


@pytest.fixture()
def client(tmp_path):
    (tmp_path / "run.csv").write_text(CSV_TEXT, encoding="utf-8")
    backend = StubBackend()
    app = create_app(backend, ResultStore([tmp_path]))
    with TestClient(app) as test_client:
        test_client.backend = backend
        yield test_client


def _only_run(client):
    runs = client.get("/api/runs").json()["runs"]
    assert len(runs) == 1
    return runs[0]


def test_health_and_config(client):
    health = client.get("/api/health").json()
    assert health["ok"] is True
    assert health["backend"]["backend"] == "stub"
    assert health["results_roots"]

    config = client.get("/api/config").json()
    assert config["offline"] is False
    assert config["simulation"] == {}


def test_discovery_can_omit_signal_metadata(client):
    light = client.get("/api/runs", params={"include_signals": False}).json()["runs"]
    assert len(light) == 1
    assert light[0]["signals"] == []
    assert light[0]["row_count"] == 3


def test_run_open_series_and_latest(client):
    run = _only_run(client)
    assert run["row_count"] == 3
    assert [signal["name"] for signal in run["signals"]] == ["gain.y", "sine.y"]

    opened = client.post("/api/runs/open", json={"path": run["path"]})
    assert opened.status_code == 200
    assert opened.json()["run"]["run_id"] == run["run_id"]

    series = client.get(
        f"/api/runs/{run['run_id']}/series",
        params={"signals": ["gain.y"], "after": 0.0, "max_points": 10},
    ).json()
    assert series["ok"] is True
    assert series["series"][0]["t"] == [0.5, 1.0]
    assert series["last_time"] == 1.0

    latest = client.get(
        f"/api/runs/{run['run_id']}/latest",
        params={"signals": ["gain.y", "sine.y"]},
    ).json()
    values = {value["signal"]: value["value"] for value in latest["values"]}
    assert values == {"gain.y": 2.0, "sine.y": 1.0}


def test_command_proxy_strips_envelope_and_whitelists(client):
    response = client.post("/api/command", json={"cmd": "state", "id": 99, "v": 9})
    assert response.status_code == 200
    assert response.json()["state"] == "paused"
    assert client.backend.requests[-1] == {"cmd": "state"}

    rejected = client.post("/api/command", json={"cmd": "rm"})
    assert rejected.status_code == 400


def test_state_passes_through_optional_session_snapshot(client):
    snapshot = {
        "v": 1,
        "ok": True,
        "state": "running",
        "sim_time": 1.0,
        "speed": 1.0,
        "session_id": "session-1",
        "artifacts": [{"kind": "csv", "path": "/abs/result.csv", "run_id": -1}],
    }
    client.backend.state_response = snapshot

    response = client.get("/api/state")
    assert response.status_code == 200
    assert response.json() == snapshot


def test_path_outside_roots_is_rejected(client):
    response = client.post("/api/runs/open", json={"path": "/etc/passwd"})
    assert response.status_code == 400
    assert response.json()["error"]["code"]


def test_unknown_run_is_404(client):
    assert client.get("/api/runs/nope").status_code == 404
    assert client.get("/api/runs/nope/latest", params={"signals": ["x"]}).status_code == 404

