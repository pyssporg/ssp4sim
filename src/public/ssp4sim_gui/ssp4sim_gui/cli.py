"""Launcher: ``ssp4sim-gui`` or ``python -m ssp4sim_gui``."""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import sys
import threading
import time
import webbrowser
from pathlib import Path
from typing import Any, Dict, List, Optional

from .app import create_app
from .backends.daemon import DaemonBackend
from .backends.offline import OfflineBackend
from .results.store import ResultStore

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765


def _is_loopback(host: str) -> bool:
    if host in ("localhost",):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _read_simulation(config: Path) -> Dict[str, Any]:
    try:
        document = json.loads(config.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"ssp4sim-gui: cannot read config {config}: {exc}", file=sys.stderr)
        return {}
    simulation = document.get("simulation", {}) if isinstance(document, dict) else {}
    if not isinstance(simulation, dict):
        return {}
    keys = ("ssp", "start_time", "stop_time", "timestep", "working_dir", "speed")
    return {key: simulation[key] for key in keys if key in simulation}


def _config_roots(config: Path, simulation: Dict[str, Any]) -> List[Path]:
    """Best-effort result directories implied by a simulation config.

    The engine resolves relative paths against its own process cwd, which the GUI
    cannot know. Config-relative is the predictable interpretation for a viewer;
    absolute paths are used as-is.
    """

    roots: List[Path] = []
    base = config.resolve().parent
    working = simulation.get("working_dir")
    if isinstance(working, str) and working:
        candidate = Path(working)
        roots.append(candidate if candidate.is_absolute() else base / candidate)
    recording = simulation.get("recording")
    if isinstance(recording, dict):
        csv = recording.get("csv")
        if isinstance(csv, dict) and isinstance(csv.get("file"), str) and csv["file"]:
            candidate = Path(str(csv["file"]).replace("[TIME]", ""))
            if candidate.suffix.lower() == ".csv":
                roots.append(candidate.parent if candidate.is_absolute() else base / candidate.parent)
    return roots


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ssp4sim-gui",
        description="Local web GUI for ssp4sim daemon control and result files.",
    )
    parser.add_argument(
        "--daemon-socket",
        type=Path,
        default=Path(os.environ["SSP4SIM_SOCKET"]) if os.environ.get("SSP4SIM_SOCKET") else None,
        help="AF_UNIX socket of ssp4simd (default: $SSP4SIM_SOCKET)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        action="append",
        default=[],
        help="directory to scan for .csv/.sqlite result files (repeatable)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        help="simulation JSON config, used to pre-fill result directories and run settings",
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="bind address (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="bind port (default 8765)")
    parser.add_argument(
        "--offline",
        action="store_true",
        help="no control channel; only open and plot existing result files",
    )
    parser.add_argument(
        "--allow-remote",
        action="store_true",
        help="permit binding to a non-loopback address (the GUI has no authentication)",
    )
    parser.add_argument("--no-browser", action="store_true", help="do not open a browser")
    return parser


def main(argv: Optional[List[str]] = None) -> None:
    args = build_parser().parse_args(argv)

    if not _is_loopback(args.host) and not args.allow_remote:
        print(
            "ssp4sim-gui: refusing to bind to a non-loopback address without "
            "--allow-remote; the GUI can command the engine and has no authentication.",
            file=sys.stderr,
        )
        raise SystemExit(2)

    simulation: Dict[str, Any] = {}
    roots: List[Path] = list(args.results_dir)
    if args.config:
        simulation = _read_simulation(args.config)
        roots.extend(_config_roots(args.config, simulation))
    if not roots:
        roots = [Path.cwd()]

    seen: set = set()
    unique_roots: List[Path] = []
    for root in roots:
        resolved = root.resolve()
        if resolved not in seen:
            seen.add(resolved)
            unique_roots.append(resolved)

    store = ResultStore(unique_roots)

    if args.offline or args.daemon_socket is None:
        detail = "started with --offline" if args.offline else "no --daemon-socket; control disabled"
        backend = OfflineBackend(detail=detail)
    else:
        backend = DaemonBackend(args.daemon_socket)

    app = create_app(
        backend,
        store,
        simulation=simulation,
        config_path=str(args.config) if args.config else None,
    )

    url = f"http://{args.host}:{args.port}/"
    print(f"ssp4sim-gui {url}")
    print(f"  backend : {backend.status()}")
    for root in unique_roots:
        print(f"  results : {root}")
    if not args.no_browser:
        threading.Thread(target=lambda: (time.sleep(0.8), webbrowser.open(url)), daemon=True).start()

    import uvicorn

    try:
        uvicorn.run(app, host=args.host, port=args.port, log_level="info")
    except KeyboardInterrupt:
        pass
    finally:
        backend.close()


if __name__ == "__main__":
    main()
