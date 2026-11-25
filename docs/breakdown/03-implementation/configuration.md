---
id: INFO-017
type: info
title: Configuration
summary: Per-instance configuration ownership and the pipeline from parsed config to runtime behavior
date: 2026-11-11
status: current
---

# Configuration

One configuration value drives all runtime behavior; the full key table, type defaults, and validation are runnable reference, not this leaf.

- **Per-instance value:** `SharedConfig` is the parsed configuration for one `Simulator`, built with `SharedConfig::from_file(path)`, `from_string(text)`, or `from_json(json)`, each taking an optional `Logger*`; `Simulator(path)` is the convenience form that loads the file internally. `utils::Config` is the internal per-instance parser behind those factories and holds no process-global state, so simulators in one process can run independent configurations.
- **Required keys:** `simulation.ssp` (SSP path), timing (`start_time`, `stop_time`, `timestep`), `simulation.executor.method` (jacobi/seidel); a missing required key is rejected with a type-checked error via `get*`.
- **Optional keys:** `simulation.executor.thread_pool_workers`, `simulation.recording.csv.enable` (default true), `simulation.log.level_terminal` and `simulation.log.level_file`; a missing optional key takes its default via `getOr(key, default)`.
- **Speed:** `simulation.speed` (finite, `>= 0`) sets wall-clock pace (`0` = as fast as possible, `1` = realtime); it affects wall-clock pacing only, never results, and can be changed at runtime with `set_speed` (`REQ-016`).
- **Daemon:** `ssp4simd` consumes the same JSON file; the daemon adds no config keys and takes its socket path from `--socket` (see `docs/usage.md`).
- **Substitutions:** every string read replaces `[TIME]` with the current timestamp; `simulation.working_dir` anchors default output locations for recorder and log files.
- **Process-global limits:** log sinks (console, file, JSON, cutelog) are configured process-wide, so per-instance configs cannot give each simulator its own logging destination; `ssp4cpp::Ssp` is loaded from the filesystem path in `simulation.ssp`, so SSP bytes cannot be supplied in memory.
- **Lifetime:** a `Simulator` owns a `ThreadedRunner`, which owns `RunControl` and the synchronous `SimulationStepper`; the stepper owns the `SharedConfig` and loaded `Ssp`; destruction joins a live run before tearing the runner and stepper down.
- **Schema reference:** full key table, type defaults, and validation behavior are documented in `docs/configuration.md`.
