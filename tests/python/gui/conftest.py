import sys
from pathlib import Path

# The GUI is a standalone package that is not built by CMake. Make it importable
# from the source tree so its tests run in the repository venv without an install.
_PACKAGE_ROOT = Path(__file__).resolve().parents[3] / "src" / "public" / "ssp4sim_gui"
if str(_PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(_PACKAGE_ROOT))
