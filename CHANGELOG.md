# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Runtime control on the C++, C, and Python surfaces: `start` (blocks until
  running or terminal), pause/resume/stop, wall-clock speed, coherent
  `state`/`sim_time_ns`/`speed` read-back via `RunInfo`/`info()`, and wait
  (IMP-037, REQ-016). One run is allowed per simulator instance, and
  `wait()` is idempotent: C++ returns `Completed`/`Stopped` and throws on
  failure; the C ABI returns `SSP4SIM_OK` and writes the `ssp4sim_run_result`
  for completed/stopped, and reports a failed run as
  `SSP4SIM_STATUS_RUNTIME_ERROR`; Python returns `"completed"`/`"stopped"`
  and raises on failure. All surfaces expose `info()` as the sole read-back.
  Pause, stop, and set speed accept an optional simulation time
  (`at_sim_time_ns` / `at`) that defers them to the first macro-step boundary
  at or after that time. Terminal outcomes are monotonic;
  `stop` is idempotent once stopping or terminal and preserves results up to
  the last completed macro step, and a completed run reports the configured end
  time. Variable overrides and override commands are planned/deferred and not
  in this release.
- `ssp4simd`, a local control daemon speaking newline-delimited JSON over a
  Unix domain socket, built by default with `SSP4SIM_BUILD_SERVER=ON`. It
  serves 8 commands and 4 event types from one single-threaded poll loop,
  caps concurrent clients at eight, creates the socket with mode `0600`, and
  unlinks it on clean shutdown and on SIGTERM/SIGINT. Only the
  `--config <path>` / `--socket <path>` forms are accepted. Override commands
  are planned/deferred and not in this release.

### Changed

- Removed the separate public run getters — C++ `Simulator::state`, `Simulator::sim_time_ns`, `Simulator::speed`; C ABI `ssp4sim_simulator_get_state`, `ssp4sim_simulator_get_sim_time_ns`, `ssp4sim_simulator_get_speed`; and Python `state()`, `sim_time_ns()`, `speed()`. `info()` is now the sole public read-back: C++ returns `RunInfo`, the C ABI returns `ssp4sim_run_info` by value (a null handle returns the zero/default record and sets `ssp4sim_last_error()`), and Python returns a `{"state": <string>, "sim_time_ns": <int>, "speed": <float>}` dict. This is a breaking change.
- Removed variable listing from every public surface — C++ `Simulator::list_variables`, the C ABI (`ssp4sim_simulator_list_variables`, `ssp4sim_simulator_free_variables`, `ssp4sim_variable_info`), Python, and the `ssp4simd` `list_variables` command. This is a breaking change.
- Removed the `simulation.realtime` config toggle; `simulation.speed` is now
  the only pacing setting.
- C++ `Simulator` can be constructed from a prepared `SharedConfig` via
  `from_file`, `from_string`, or `from_json`, so configuration is a per-instance
  value rather than process-global and several simulators in one process can run
  independent configurations. `Simulator(path)` is unchanged. Logging sinks
  remain configured process-wide.

## [0.3.6] - 2026-11-24

### Added

- Nested root-override parameter bindings (IMP-042).

### Fixed

- REG-003 root cause.
