---
id: INFO-037
type: info
title: Runbook
summary: Day-to-day operations — startup, post-run verification, and remediation of common issues
date: 2026-11-11
status: current
---

# Runbook

Common operational procedures for running SSP4SIM simulations: startup, verification, and remediation.

- **Startup:**
  1. Ensure the SSP archive or directory exists and is readable.
  2. Ensure the JSON configuration file is valid per the configuration guide.
  3. Ensure FMU binaries in the SSP have executable permissions (Linux shared libraries).
  4. Run `sim_app /path/to/config.json`.
  5. Alternative: `pyssp4sim /path/to/config.json` (when the wheel is installed).
  6. Within Python: `pyssp4sim.Simulator("/path/to/config.json").init().simulate()`.
  7. Control daemon: run `ssp4simd --config /path/to/config.json --socket /tmp/ssp4sim.sock` and connect a newline-delimited JSON client.
- **Verification after a run:**
  - Simulation completed without ERROR or CRITICAL log messages.
  - Output files exist at the configured working directory.
  - CSV export has the expected columns and row count.
  - Local database output has per-storage tables populated.
  - Daemon: a `{"v":1,"id":1,"cmd":"state"}` request returns `ok:true`, and the run ends with a `done` event whose `result` is `completed` or `stopped`.
- **Common issues:**
  - FMU not found ("file not found" or dlopen error) — check SSP path, FMU binary location, and executable permissions.
  - Config error (parsing failure on startup) — validate JSON syntax and key names against the configuration guide.
  - Simulation hang (no output for an extended period) — check the log for the last successful step, FMU timeout, or recorder backpressure.
  - Wrong results (CSV differs from expected) — check signal connections in the SSD and compare with reference-SSP results.
  - Daemon refuses to start — a live server owns the socket path; stop the old daemon or use another `--socket`. A stale socket is replaced automatically.
  - Daemon client misses progress — `time` events are droppable and throttled to about 10 Hz; slow clients may be disconnected. Use `state`, which returns one coherent state, simulation-time, and speed reading.
  - ctest failure in CI — run the test binary directly (`./build/tests/lib/ssp4sim_tests`).
