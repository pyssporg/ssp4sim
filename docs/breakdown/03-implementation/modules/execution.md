---
id: INFO-023
type: info
title: execution module
summary: Executor strategies that advance simulation time and coordinate FMU stepping, plus runtime control and pacing
date: 2026-11-11
status: current
---

# execution module

Provides executor strategies that advance simulation time and coordinate FMU stepping, selected by configuration, plus the run-control state machine that starts, pauses, resumes, stops, and paces a run.

- **Role:** `ExecutionBase` defines the executor interface; `ExecutorBuilder` creates the configured executor; strategy pattern, one concrete strategy per algorithm. Executors are pure time-step runners: `SimulationEngine::run()` owns the outer macro-step loop, slices `[start_time, end_time]` by `SharedConfig::fmu.timestep`, calls `RunControl::before_slice()` at each slice top, and invokes the executor on the slice.
- **Engine split:** `Simulator` (`src/lib/public_include/simulator.{hpp,cpp}`) is the public lifecycle facade and owns the internal run thread; `SimulationEngine` (`src/lib/include/simulation/simulation_engine.{hpp,cpp}`) owns SSP import, the graph, executors, recorder, and control, and runs the outer macro-step loop in `run()` on that thread. `src/lib/include/simulation/control/` holds `RunControl` (run-thread state machine: one mutex guards plain state fields, simulation time, speed, pacing anchors, and one ordered command queue; commands are applied by the run thread inside `before_slice()` or at a `publish_boundary()`, and the engine calls `before_slice()` at each slice top); public types are in `runtime_control.hpp`, which is the single enum definition. `simulate()` is `start()` plus `wait()`.
- **Lifecycle:** `start` spawns the single run and blocks until it leaves initialized (running or terminal); `pause` and `resume` gate at macro-step boundaries; `stop` is idempotent once stopping or terminal and preserves partial results, while the teardown `request_stop()` cancels a pre-run state to stopped; terminal states are monotonic. `wait` returns `completed`/`stopped` and rethrows failure. States are created, initialized, running, paused, stopping, completed, stopped, failed (`REQ-016`).
- **Boundary and flush:** `publish_boundary()` publishes a macro boundary without pacing, ignores terminal states, advances simulation time monotonically even while stopping, and applies due commands only when not stopping. `RunControl::finish()` returns the effective result; `SimulationEngine::run()` publishes the last completed boundary before the recorder flush, so a completed run reports its end time. The result is finalized after the flush, so a stop requested during the flush is observed; a flush failure publishes failed and still unblocks `wait`; a failure before the run starts publishes failed.
- **Speed:** `set_speed` sets a finite wall-clock factor (`0` free-running, `1` realtime) used by macro-step pacing. Variable overrides (`y = k*x + m`) are planned/deferred and are not in current releases.
- **Scheduling:** any control operation carrying `at_sim_time_ns` is deferred to the first macro-step boundary at or after that simulation time; activation granularity is one macro step and deferred operations preserve post order. `pause`, `stop`, and `set_speed` expose the option, while `resume` does not; override-command scheduling is planned with the deferred override feature.
- **Jacobi variants:** `jacobi_serial`, `jacobi_parallel_fut` (std::future), `jacobi_parallel_spin` (spin-wait), `jacobi_parallel_tbb` (TBB task group); all FMUs step concurrently on previous-step values.
- **Seidel variants:** `seidel_serial`; `seidel_parallel` is not implemented — selecting it throws at config selection.
- **la2 (loop-aware):** factory-assembled stack, not a class — SCC detection, one `LinearSubstepExecutor` or `GeometricSubstepExecutor` per loop SCC, the condensed component DAG run by an outer `SerialSeidel`, and a mixed-policy `La2DataAccessResolver` (cross-SCC Latest, intra-SCC StartTime). Outer `ParallelSeidel` (`la2.parallel`) is not implemented and throws during stack assembly.
- **Sub-step executors:** `LinearSubstepExecutor` splits the macro step into equal sub-steps (`iterations`); `GeometricSubstepExecutor` shrinks sub-steps, each covering `factor` of the remaining time until the remainder is at or below `threshold`, then takes the rest of the macro step whole.
- **Include boundary:** `src/lib/include/simulation/executor/` — `executor_base.hpp`/`.cpp`, `executor_utils.hpp`, variant subdirs `jacobi/`, `seidel/`, `loop_aware/`, `substep/`, `custom/`; `ExecutorBuilder` sits one level up at `src/lib/include/simulation/executor_builder.hpp`; control lives beside it in `src/lib/include/simulation/control/`.
- **Dependencies:** `utils/` (task thread pool, ring buffer, time conversion), `model/` (FmuModel invocation interface), and `signal/` (typed storage used for recording).
