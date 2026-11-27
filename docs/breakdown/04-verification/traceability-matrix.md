---
id: TR-001
type: trace
title: Traceability Matrix
summary: Requirement-to-test coverage map — two covered, seven partial, one missing, six untraced
date: 2026-11-11
status: current
pb_exempt: true
---

# Traceability Matrix

Maps each product requirement to its verification coverage: test locations and known gaps.

**Coverage map**

| REQ ID | Description | Test File(s) | Coverage | Notes |
|---|---|---|---|---|
| REQ-001 | SSP 1.0 archive loading | `tests/lib/high_level/` (indirect), `tests/python/high_level/test_reference_ssps.py` | Partial | No dedicated SSP-loading unit test; verified through smoke and workflow tests |
| REQ-002 | FMI 2.0 co-simulation lifecycle | `tests/lib/core/test_fmi4c_adapter.cpp`, `tests/lib/high_level/` | Covered | FMU adapter has dedicated unit tests; lifecycle verified through workflow |
| REQ-003 | Configurable execution strategy | `tests/lib/utils/test_parallel.cpp`, `tests/lib/high_level/` | Partial | Executor strategies tested indirectly; no strategy-specific unit tests |
| REQ-004 | CSV output with sample interval | `tests/lib/core/test_data_recorder.cpp`, `tests/lib/high_level/` | Partial | Recorder unit test exists; CSV format not tested in isolation |
| REQ-005 | Local database output with per-storage tables | `tests/lib/core/test_sqlite_recorder.cpp`, `tests/lib/high_level/` | Partial | DuckDB recorder removed; acceptance is all SQLite (per-simulation WAL files, optional shared-file mode); WAL concurrency covered in the recorder unit tests |
| REQ-006 | Local logging with levels and sinks | (none) | Partial | Logging implemented (ssp4cpp::utils::log); dedicated log tests missing — TEST gap, not code gap |
| REQ-007 | Distributed observability with OpenTelemetry | (none) | Missing | Feature not implemented in code — PRODUCT gap (no opentelemetry references in src/lib/) |
| REQ-009 | Multiple embedding API surfaces | `tests/lib/high_level/test_c_api_control.cpp`, `tests/lib/high_level/test_simulator_core_c_api.cpp`, `tests/python/high_level/test_runtime_control.py` | Partial | Non-override control methods covered through the threaded C and Python surfaces; the synchronous SimulatorCore C surface is covered; the CLI lifecycle remains a smoke path |
| REQ-016 | Runtime control and overrides | `tests/lib/control/`, `tests/lib/simulation/test_simulation_lifecycle.cpp`, `tests/lib/high_level/test_c_api_control.cpp`, `tests/python/high_level/test_runtime_control.py` | Partial | Lifecycle, blocking `start`, speed, terminal `wait` result, coherent `info` read-back, scheduled pause/stop/speed, terminal monotonicity, pre-run teardown, and the final published time are covered at the run-control, C, and Python levels. Variable overrides and override commands are planned/deferred and not in current releases; phase-shift (`UC-014`) is not promised |
| REQ-017 | External runtime access over a local socket | `tests/lib/simulation/test_snapshot.cpp`, `tests/python/test_ssp4simd.py`, `src/public/ssp4sim_server/test_ndjson.cpp` | Covered | Daemon framing, implemented command dispatch, events, reset/shutdown, end-to-end lifecycle/speed control, and the version-4 sectioned `SSP4SNAP` checkpoint container with its apply-failure path are covered; override commands are planned/deferred. Transport choice is `AD-007` |

**Untraced:** REQ-010 through REQ-015 exist in the requirements folder but have no matrix rows yet.

**Summary:**
- **Covered:** 2 of 16 requirements (REQ-002, REQ-017).
- **Partial:** 7 of 16 requirements (REQ-001, REQ-003, REQ-004, REQ-005, REQ-006, REQ-009, REQ-016).
- **Missing:** 1 of 16 requirements (REQ-007).
- **Untraced:** 6 of 16 requirements (REQ-010 through REQ-015).
