---
id: INFO-019
type: info
title: Interfaces
summary: Public and internal API contracts of SSP4SIM
date: 2026-11-11
status: current
---

# Interfaces

A deliberately small public API, a runtime-control contract shared by the C, C++, and Python surfaces, and a separate local daemon protocol.

- **Public API:** `src/lib/public_include/` holds `simulator.hpp` (C++ `Simulator` lifecycle facade over the internal `SimulationEngine`, with the `Simulator(const std::string& config_path)` convenience constructor and the object-first `Simulator(SharedConfig config, Logger* = nullptr)`, plus `init`, `start` (blocks until running or terminal), `pause`, `resume`, `stop`, `set_speed`, `info`, `wait`, `simulate`), `simulator_c_api.h` (C wrapper mirroring the C++ surface: `ssp4sim_simulator_create`, `init`, `simulate`, `start`, `pause`, `resume`, `stop`, `set_speed`, `get_info`, `wait`, `destroy`, and `ssp4sim_last_error`; control calls take `ssp4sim_simulator_handle *`, `get_info` returns `ssp4sim_run_info` by value (a null handle returns the zero/default record and sets `ssp4sim_last_error()`), and `wait` writes an `ssp4sim_run_result`), `runtime_control.hpp` (public `RunState`/`RunResult` enums and the `RunInfo` type), and `shared_config.hpp` (per-instance `SharedConfig` value with the `from_file`, `from_string`, and `from_json` factories, each taking an optional `Logger*`).
- **Coherent read:** `RunInfo { RunState state; uint64_t sim_time_ns; double speed; }` is the sole public read-back and its fields agree with each other; it is exposed as C++ `Simulator::info()`, C `ssp4sim_simulator_get_info()` (returns `ssp4sim_run_info` by value; a null handle returns the zero/default record and sets `ssp4sim_last_error()`), and Python `info()`.
- **One-shot convenience:** `simulate()` is exactly `start()` followed by `wait()`; its blocking behaviour and exception mapping are unchanged (`REQ-009`).
- **Terminal results:** C++ and Python `wait()` return `Completed`/`completed` or `Stopped`/`stopped` and raise on a failed run; the C ABI `ssp4sim_simulator_wait` returns `SSP4SIM_OK` for Completed or Stopped and writes the `ssp4sim_run_result`, while a failed run returns `SSP4SIM_RUNTIME_ERROR` with `ssp4sim_last_error` set, never a failed result with `SSP4SIM_OK`.
- **Runtime control:** lifecycle, speed, scheduling of lifecycle/speed commands, state, simulation-time/speed read-back, and wait are promised by `REQ-016` and implemented in the control module (`INFO-023`). `start` blocks until the run leaves initialized (running or terminal). Terminal states are monotonic; `stop` is idempotent once stopping or terminal, preserves partial results, and is an error before the run starts, while the teardown path cancels a pre-run state to stopped. A completed run publishes the configured end time after the recorder flush. `pause`, `stop`, and `set_speed` take an optional `at_sim_time_ns` (C ABI: `int has_at_sim_time, uint64_t at_sim_time_ns`) that defers the command to the first macro-step boundary at or after that time; `resume` is not schedulable. Variable overrides (`set_override`, `clear_override`, `list_overrides`), the override target validation, and `y = k*x + m` behavior are planned/deferred and not in current releases.
- **Control daemon:** `src/public/ssp4sim_server/` implements `ssp4simd`, a frontend that relays the same lifecycle and speed control over newline-delimited JSON on a Unix domain socket (`REQ-017`, `INFO-011`, `INFO-010`); it is not part of the engine API. Override commands are planned/deferred.
- **Invocable:** `src/lib/include/execution/invocable.hpp` — abstract init()/invoke(data); implemented by FmuModel and other invocable objects.
- **ExecutionBase:** `src/lib/include/execution/executor.hpp` — abstract executor managing the model vector; the interface for Jacobi/Seidel variants (`INFO-023`).
- **Graph nodes:** `InvocableNode` marker interface for schedulable graph nodes, `src/lib/include/graph/graph.hpp`.
- **Recorder interface:** `DataRecorder::add_storage()` and `add_sink()`; `RecorderSink` abstract `start()`/`on_event()`/`stop()`; implementations `CsvRecorderSink` and `SqliteWALRecorderSink` — signal module (`INFO-024`).
- **Boundary:** these contracts satisfy the container boundaries (`INFO-011`); the config access pattern is the configuration leaf (`INFO-017`).
