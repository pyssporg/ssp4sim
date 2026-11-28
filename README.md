# SSP4SIM
<!-- Layer: 00-intent, 01-product, 05-operation, 03-implementation -->


SSP4SIM is a C++23 simulation engine for SSP archives with local database result artifacts, CSV export, runtime control (pause/resume/stop, reset, speed, and variable overrides), mid-run snapshot/restore, and a local control daemon (`ssp4simd`). See [breakdown/00-intent/purpose.md](docs/breakdown/00-intent/purpose.md) for the full project purpose.

See [breakdown/01-product/capabilities.md](docs/breakdown/01-product/capabilities.md) for the full capabilities list.

Builds upon the [SSP4CPP](https://github.com/pyssporg/ssp4cpp) XML deserializer
and uses [Quill](https://github.com/odygrd/quill) for logging.

## Prerequisites

- Linux `x86_64` for the prebuilt release artifacts. The Python module is an
  `abi3` build and supports Python >= 3.9.
- Source builds: CMake with presets support, Ninja, a C++23 compiler, and a
  vcpkg checkout with `VCPKG_ROOT` set (see [vcpkg.md](vcpkg.md)).

## Install

Release tarballs and wheels are described in
[Installation](docs/installation.md). A source checkout is not installed from
the repository root: build first, then install the generated Python package
from `build/src/public/python_api`:

```bash
cmake --preset=vcpkg -DCMAKE_BUILD_TYPE=Release -DSSP4SIM_BUILD_PYTHON_API=ON
cmake --build build
pip install -e ./build/src/public/python_api
```

Full source-build instructions, including the container workflow and test
commands, are in [Build From Source](docs/build_from_source.md).

## Quick Start

From a source checkout, run the built CLI against the bundled example config
(the example references files under `resources/`):

```bash
./build/src/public/ssp4sim_app/sim_app ./resources/embrace/embrace.json
```

After installing a release tarball, add its `bin/` directory to `PATH` and pass
a config file of your own; the tarball does not install `resources/`:

```bash
export PATH="$HOME/.local/opt/ssp4sim/bin:$PATH"
sim_app /path/to/config.json
```

Or use the Python API:

```python
import pyssp4sim
sim = pyssp4sim.Simulator("./resources/embrace/embrace.json")
sim.init()
sim.simulate()
```

`Simulator` also exposes `start()`, `pause()`, `resume()`, `reset()`, `stop()`,
`set_speed()`, `set_override()`, `clear_override()`, `snapshot()`, `restore()`,
`info()`, and `wait()`; `start()` blocks until the run is running or terminal,
and `simulate()` remains `start()` followed by `wait()`. `info()` is the sole
read-back and returns one coherent `state`/`sim_time_ns`/`speed` reading.
Overrides apply `y = k*x + m` to an input or output side and accept optional
`at` scheduling. The daemon adds `list_overrides`; the same lifecycle and speed
control is available through the C API (see
[Usage](docs/usage.md#runtime-control)).

## Tests And Lint

```bash
ctest --test-dir build --output-on-failure -j   # canonical C++ runner
pytest -q tests/python                          # Python API (after the build above)
tests/run_unit.sh --tier T1                     # focused C++ unit tier
```

Test layout and expected failures are in [tests/README.md](tests/README.md).
No Python linter or formatter is configured in this repository (there is no
`ruff.toml` or lint section in a Python project file); match the surrounding
C++ style described in [docs/development.md](docs/development.md).

## Documentation

For structured per-layer documentation, see the [breakdown](docs/breakdown/) directory.

- [Documentation](docs/README.md): installation, usage, configuration, and development guides.
- [Installation](docs/installation.md): release tarball and wheel consumption.
- [Build from source](docs/build_from_source.md): local and container builds.
- [Usage](docs/usage.md): CLI, Python API, runtime control, the control daemon, examples, and result artifacts.
- [GUI](docs/gui.md): separate local web control panel and result viewer.
- [Configuration](docs/configuration.md): JSON keys, defaults, and supported values.
- [Testing](tests/README.md): C++ and Python test layout and commands.
- [Development](docs/development.md): contributor workflow and repository conventions.
- [Release pipeline](docs/linux_binary_distribution.md): Linux binary and wheel packaging.
- [Profiling](docs/profiling.md): build and runtime profiling commands.
- [Logging](docs/logging_guidlines.md): logging tiers, hot-path logging, and sink notes.
- [AGENTS.md](AGENTS.md): agent-specific repository guidelines and quick commands.

## Contributing

Contributions are welcome. Development details are in [docs/development.md](docs/development.md).

## License

This project is released under the MIT license. See [LICENSE](LICENSE) for details.
