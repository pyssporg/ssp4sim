---
id: INFO-032
type: info
title: Deployment Topology
summary: Release tarball layout, install paths, container image, and runtime environment for SSP4SIM
date: 2026-11-11
status: current
---

# Deployment Topology

The product ships as a Linux tarball plus a Python wheel and runs from a filesystem tree.

- **Release tarball layout** (`ssp4sim-linux-x86_64-<version>/`):
  - `bin/sim_app` — CLI binary.
  - `bin/ssp4simd` — local control daemon.
  - `lib/libssp4sim.a` — static library.
  - `include/ssp4sim/` — shared public types (runtime_control.hpp, shared_config.hpp), `include/ssp4sim/cpp_api/` (simulator.hpp, stepper.hpp), and `include/ssp4sim/c_api/` (simulator_c_api.h, stepper_c_api.h, c_api_common.h).
  - `python/pyssp4sim-*.whl` — Python wheel.
  - `resources/` — embrace.json under embrace/, generic_config.json.
  - `README.md`, `LICENSE`, `RELEASE.txt`.
- **Install paths:**
  - System-wide: extract tarball to `/opt/ssp4sim/` or `/usr/local/`.
  - User-local: extract to `~/.local/ssp4sim/`.
  - Python: install the wheel with pip.
  - From source: build with CMake presets and use the build output directly.
- **Container image:** built from `containers/ubuntu22-gcc13/Containerfile`; container build commands are in the build docs.
- **Runtime environment:**
  - Linux x86_64, Ubuntu 22.04 primary.
  - No environment variables required; all configuration via JSON file.
  - The `working_dir` config key anchors output locations.
  - `ssp4simd` binds a Unix domain socket path supplied with `--socket` (mode 0600); it opens no listening network port (`INFO-010`).
