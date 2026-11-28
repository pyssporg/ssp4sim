"""HTTP/SSE surface of the GUI.

The browser never talks to the engine. It sends commands to this app, which
forwards them through the selected :class:`EngineBackend`, and it reads
time-series data from this app, which serves rows parsed from result artifacts on
disk. Only command responses, state events, and bounded series windows cross the
wire.
"""

from __future__ import annotations

import asyncio
import queue
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import Body, FastAPI, HTTPException, Query, Request
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import __version__
from .backends.base import BackendError, EngineBackend, Event
from .results.base import ResultSource, RunInfo, Series
from .results.store import ResultStore, StoreError

# Commands the GUI is allowed to forward. The daemon is the final validator;
# this list prevents the browser from turning the proxy into an arbitrary
# envelope injector.
ALLOWED_COMMANDS = {
    "start",
    "pause",
    "resume",
    "stop",
    "reset",
    "set_speed",
    "set_override",
    "clear_override",
    "list_overrides",
    "state",
    "shutdown",
}
LONG_COMMANDS = {"start", "stop", "reset"}


def _run_dict(run: RunInfo, *, include_signals: bool = True) -> Dict[str, Any]:
    return {
        "run_id": run.run_id,
        "path": run.path,
        "kind": run.kind,
        "label": run.label,
        "signals": [asdict(signal) for signal in run.signals] if include_signals else [],
        "time_start": run.time_start,
        "time_end": run.time_end,
        "row_count": run.row_count,
        "live": run.live,
    }


def _error(status: int, code: str, message: str) -> JSONResponse:
    return JSONResponse(
        status_code=status,
        content={"ok": False, "error": {"code": code, "message": message}},
    )


def _sse(kind: str, payload: Dict[str, Any]) -> str:
    import json

    return f"event: {kind}\ndata: {json.dumps(payload, default=str)}\n\n"


def create_app(
    backend: EngineBackend,
    store: ResultStore,
    *,
    simulation: Optional[Dict[str, Any]] = None,
    config_path: Optional[str] = None,
) -> FastAPI:
    app = FastAPI(title="ssp4sim lab", version=__version__, docs_url="/api/docs", redoc_url=None)
    app.state.backend = backend
    app.state.store = store
    app.state.simulation = simulation or {}
    app.state.config_path = config_path

    static_dir = Path(__file__).with_name("static")
    app.mount("/static", StaticFiles(directory=str(static_dir), check_dir=False), name="static")

    def source(run_id: str) -> ResultSource:
        try:
            return store.get(run_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"unknown run: {run_id}")

    @app.exception_handler(BackendError)
    def _backend_failure(_request: Request, exc: BackendError) -> JSONResponse:
        return _error(502, exc.code, exc.message)

    @app.exception_handler(StoreError)
    def _store_failure(_request: Request, exc: StoreError) -> JSONResponse:
        return _error(400, exc.code, exc.message)

    @app.get("/")
    def index() -> Any:
        page = static_dir / "index.html"
        if not page.is_file():
            raise HTTPException(status_code=503, detail="GUI assets are not installed")
        return FileResponse(page)

    @app.get("/api/health")
    def health() -> Dict[str, Any]:
        return {
            "ok": True,
            "version": __version__,
            "backend": backend.status(),
            "results_roots": store.roots(),
        }

    @app.get("/api/config")
    def config() -> Dict[str, Any]:
        status = backend.status()
        return {
            "ok": True,
            "version": __version__,
            "backend": status,
            "daemon_socket": status.get("socket"),
            "offline": status.get("backend") == "offline",
            "results_roots": store.roots(),
            "config_path": app.state.config_path,
            "simulation": app.state.simulation,
        }

    @app.get("/api/state")
    def state() -> Any:
        try:
            return backend.send({"cmd": "state"}, timeout=5.0)
        except BackendError as exc:
            return _error(502, exc.code, exc.message)

    @app.post("/api/command")
    def command(payload: Dict[str, Any] = Body(...)) -> Any:
        name = payload.get("cmd")
        if name not in ALLOWED_COMMANDS:
            raise HTTPException(status_code=400, detail=f"unsupported command: {name!r}")
        request = {key: value for key, value in payload.items() if key not in ("v", "id")}
        timeout = 120.0 if name in LONG_COMMANDS else 10.0
        try:
            return backend.send(request, timeout=timeout)
        except BackendError as exc:
            return _error(502, exc.code, exc.message)

    @app.get("/api/events")
    async def events(request: Request) -> Any:
        subscription = backend.subscribe()

        async def stream():
            try:
                yield _sse(
                    "backend",
                    {"kind": "backend", "data": backend.status(), "at": time.time()},
                )
                while True:
                    if await request.is_disconnected():
                        break
                    try:
                        event: Event = await asyncio.get_running_loop().run_in_executor(
                            None, subscription.get, True, 5.0
                        )
                    except queue.Empty:
                        yield ": ping\n\n"
                        continue
                    yield _sse(
                        event.kind,
                        {"kind": event.kind, "data": event.data, "at": event.at},
                    )
            finally:
                backend.unsubscribe(subscription)

        return StreamingResponse(
            stream(),
            media_type="text/event-stream",
            headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
        )

    @app.get("/api/runs")
    def runs(include_signals: bool = True) -> Dict[str, Any]:
        return {
            "ok": True,
            "runs": [
                _run_dict(run, include_signals=include_signals)
                for run in store.discover()
            ],
        }

    @app.post("/api/runs/open")
    def open_run(payload: Dict[str, Any] = Body(...)) -> Dict[str, Any]:
        path = payload.get("path")
        if not path:
            raise HTTPException(status_code=400, detail="path is required")
        return {"ok": True, "run": _run_dict(store.open(path))}

    @app.get("/api/runs/{run_id}")
    def get_run(run_id: str) -> Dict[str, Any]:
        try:
            run = store.get(run_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"unknown run: {run_id}")
        run.refresh()
        return {"ok": True, "run": _run_dict(run.info())}

    @app.delete("/api/runs/{run_id}")
    def close_run(run_id: str) -> Dict[str, Any]:
        try:
            store.close_run(run_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"unknown run: {run_id}")
        return {"ok": True}

    @app.get("/api/runs/{run_id}/series")
    def series(
        run_id: str,
        signals: List[str] = Query(default=[]),
        after: Optional[float] = None,
        until: Optional[float] = None,
        max_points: int = 2000,
    ) -> Dict[str, Any]:
        if not signals:
            raise HTTPException(status_code=400, detail="signals is required")
        run = source(run_id)
        limit = max(2, min(max_points, 20000))
        run.refresh()
        try:
            data: List[Series] = run.read(
                signals, after=after, until=until, max_points=limit
            )
        except KeyError as exc:
            raise HTTPException(status_code=400, detail=f"unknown signal: {exc}")
        last_time = max((item.t[-1] for item in data if item.t), default=after)
        return {
            "ok": True,
            "run_id": run_id,
            "series": [asdict(item) for item in data],
            "last_time": last_time,
        }

    @app.get("/api/runs/{run_id}/latest")
    def latest(
        run_id: str,
        signals: List[str] = Query(default=[]),
    ) -> Dict[str, Any]:
        if not signals:
            raise HTTPException(status_code=400, detail="signals is required")
        run = source(run_id)
        run.refresh()
        try:
            values = run.latest(signals)
        except KeyError as exc:
            raise HTTPException(status_code=400, detail=f"unknown signal: {exc}")
        return {"ok": True, "values": [asdict(value) for value in values]}

    return app
