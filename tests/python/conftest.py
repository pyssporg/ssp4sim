from __future__ import annotations

import sys
from pathlib import Path


def find_upwards(start: Path, target: str) -> Path:
    current = start.resolve()

    for parent in [current] + list(current.parents):
        if (parent / target).exists():
            return parent

    raise FileNotFoundError(f"Could not find '{target}' in any parent directory")


SSP4SIM_ROOT = find_upwards(Path(__file__), "__SSP4SIM_ROOT__")
PYTHON_API_BUILD = SSP4SIM_ROOT / "build" / "src" / "public" / "python_api"

if PYTHON_API_BUILD.exists():
    sys.path.insert(0, str(PYTHON_API_BUILD))


def pytest_collection_modifyitems(session, config, items):
    config._ssp4sim_multi_item = len(items) > 1
