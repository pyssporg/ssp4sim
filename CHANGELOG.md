# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Variable overrides (`REQ-016`, `REQ-017`): C++
  `Simulator::set_override`/`clear_override` apply `y = k*x + m` to the
  configured `input` or `output` side of a Real, Integer, or Boolean variable
  before it is recorded or propagated, with optional `at` scheduling.
  `ssp4simd` adds `set_override`, `clear_override`, and `list_overrides`;
  `list_overrides` is daemon-owned and derived from the daemon's scheduled
  event timeline filtered by the current `info().sim_time_ns`. Targets are
  validated against the initialized graph (`unknown_variable`,
  `unsupported_type`), and the timeline is discarded by reset, restore,
  terminal state, and shutdown.
- Per-run identity on the control surface: `RunInfo` carries the run's
  `session_id` (stable per run, new after reset/restore) and absolute CSV/SQLite
  `artifacts` (SQLite `run_id`; empty before init or when recording is off),
  exposed through `Simulator::info()` and `SimulatorCore::session_id()` /
  `artifacts()`. `ssp4simd` includes them in the `state` response, `state` event,
  and `done` event. The GUI consumes the optional snapshot to open and tail the
  active artifact immediately on connect, including mid-run, and to distinguish
  a reset that reuses the same output path.
- Synchronous C ABI (`c_api/simulator_core_c_api.h`,
  `ssp4sim_simulator_core_*`) over the internal `SimulatorCore`: `create`,
  `init`, `start`, `step`, `advance_to`,
  `get_info`, `finish`, and `destroy`. `advance_to` consumes an exact target,
  clamped to the end time, with no thread, pacing, pause, or speed. Callers
  must call `finish` before `destroy` to flush recording. `ssp4sim_status`,
  `SSP4SIM_STATUS_*`, and `ssp4sim_last_error` now live in the shared
  `c_api/c_api_common.h`, included by both the threaded
  (`c_api/simulator_c_api.h`) and synchronous headers; existing includes and
  symbols stay source-compatible.
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
  time.
- `ssp4simd`, a local control daemon speaking newline-delimited JSON over a
  Unix domain socket, built by default with `SSP4SIM_BUILD_SERVER=ON`. It
  serves 13 commands and 6 event types from one single-threaded poll loop,
  caps concurrent clients at eight, creates the socket with mode `0600`, and
  unlinks it on clean shutdown and on SIGTERM/SIGINT. Only the
  `--config <path>` / `--socket <path>` forms are accepted.

### Changed

- Run-control posting now lives in `Simulator`; `SimulationRuntime` only
  consumes and applies actions. `Simulator::rebuild`/`RebuildResult` moved to
  the internal `detail::RebuildAccess`, and `SimulatorCore::step`/`advance_to`
  return `bool` (true = completed). `SnapshotError` remains for load-time error
  codes.
- Replaced the split run-control queue (`RunControl`, `RunThreadTask`,
  `LoopAction`, and snapshot work) with a single `runtime::ControlChannel`:
  one mutex, one condition variable, one time-ordered `Action` queue, and one
  published `ChannelStatus`. Immediate control calls block until applied while
  scheduled calls remain fire-and-forget. `Simulator::snapshot` and
  `SimulatorCore::save_snapshot` return `bool` and `SnapshotSummary` is gone;
  the daemon `snapshot` event carries `id?`, `ok`, `code`, `path`, and `bytes`
  on success or `error` on failure, without `sim_time`, `models_done`, or
  `models_total`. The C ABI maps a `false` snapshot result to
  `SSP4SIM_STATUS_RUNTIME_ERROR`; Python raises `RuntimeError`.
- The `ssp4simd` daemon now drives `reset`/`restore` through the public
  `Simulator` API instead of the internal rebuild header; the `Simulator`
  identity is stable across lifecycle jobs, and the lifecycle/shutdown race is
  fixed. `reset` reports the old live run's terminal `state` and `done` before
  the fresh `initialized` state, and `shutdown` finalizes its own terminal
  event.
- Removed the separate public run getters — C++ `Simulator::state`, `Simulator::sim_time_ns`, `Simulator::speed`; C ABI `ssp4sim_simulator_get_state`, `ssp4sim_simulator_get_sim_time_ns`, `ssp4sim_simulator_get_speed`; and Python `state()`, `sim_time_ns()`, `speed()`. `info()` is now the sole public read-back: C++ returns `RunInfo`, the C ABI returns `ssp4sim_run_info` by value (a null handle returns the zero/default record and sets `ssp4sim_last_error()`), and Python returns a `{"state": <string>, "sim_time_ns": <int>, "speed": <float>}` dict. This is a breaking change.
- Removed variable listing from every public surface — C++ `Simulator::list_variables`, the C ABI (`ssp4sim_simulator_list_variables`, `ssp4sim_simulator_free_variables`, `ssp4sim_variable_info`), Python, and the `ssp4simd` `list_variables` command. This is a breaking change.
- Public API headers are grouped by surface: `cpp_api/simulator.hpp` (threaded
  C++), `cpp_api/simulator_core.hpp` (synchronous C++), `c_api/simulator_c_api.h`
  (threaded C), `c_api/simulator_core_c_api.h` (synchronous C), and
  `c_api/c_api_common.h`. Shared C++ types remain `runtime_control.hpp` and
  `shared_config.hpp` at the public include root. Include paths for the entry
  point headers changed; the C and C++ symbols themselves are unchanged.
- Removed the `simulation.realtime` config toggle; `simulation.speed` is now
  the only pacing setting.
- Split the internal runtime into a synchronous `SimulatorCore` (setup,
  graph, recorder, one macro step, exact `advance_to`) and a `SimulationRuntime`
  (run thread, `ControlChannel` queue/status, pacing, `RunState`, `wait`). The
  public `Simulator`, C ABI, Python, and daemon surfaces are unchanged; the core
  is intended for future synchronous/FMU wrappers.
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
