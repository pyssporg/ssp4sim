---
id: INFO-030
type: info
title: Test Strategy
summary: Three-layer test pyramid — C++ unit tests, C++ smoke and API-surface tests, Python workflow tests — and the layer-boundary rationale
date: 2026-11-11
status: current
---

# Test Strategy

SSP4SIM tests form a three-layer pyramid; the concrete layout and run commands live in the tests readme.

- **Pyramid:**
  - C++ unit tests cover runtime primitives and the run-control state machine.
  - A C++ integration smoke test drives the public simulator entry point; a focused C API test exercises the implemented runtime control (lifecycle and speed).
  - Python workflow tests cover reference-SSP sweeps, result validation, implemented runtime control, and the daemon end to end.
- **C++ boundary:** C++ tests verify low-level runtime correctness where Python overhead is undesirable or direct library API access is needed.
- **Python boundary:** Python tests handle reference sweeps, result validation, and multi-fixture orchestration where pytest tooling (parameterization, fixtures, xfail markers) adds value.
- **Smoke constraint:** the C++ high-level layer stays limited to the simulator smoke path plus focused API-surface tests; reference sweeps and result comparisons stay in Python.
- **Coverage state:**
  - SQLite recorder has dedicated C++ tests.
  - CSV recorder is tested only through the C++ smoke test and Python workflow tests.
  - FMU adapter and model lifecycle are tested through C++ core tests and indirectly through Python workflows.
  - Implemented runtime control (lifecycle including the blocking start, coherent `info`, speed, scheduling of those commands, wait, and daemon lifecycle/reset) is covered by control unit tests, the C API test, Python control tests, and the daemon end-to-end test. The daemon end-to-end test also asserts the live `session_id` and absolute artifact snapshot; the GUI suite covers its pass-through and auto-open contract. Variable overrides are covered by the daemon end-to-end test: immediate `set_override`/`clear_override` with `list_overrides`, scheduled set and clear (including paused-run listing), the input side, and the `unknown_variable`/`bad_value` paths. The C and Python override APIs are not exposed.
  - Reference fixtures in `resources/reference_ssp/` are the primary workflow test data.
- **Known limits:** model-exchange SSP fixtures are excluded (co-simulation only); the fmi4c mode-bit issue and recommended fixture handling are in the implementation dependency policy (`INFO-018`).
