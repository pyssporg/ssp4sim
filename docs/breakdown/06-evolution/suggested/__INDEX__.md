---
title: Suggested
summary: Improvement candidates not yet selected for development
---

# Suggested

## Contents

<!-- pb:index:start -->
<!-- GENERATED FILE — do not edit. Regenerate with `pb registers`. -->
- **IMP-021** [Implement a working ParallelSeidel DAG scheduler](IMP-021.md) — Replace the throwing ParallelSeidel stub with a thread-safe parallel DAG scheduler over the serial Seidel algorithm
- **IMP-026** [Direct rerun pipelined Gauss-Seidel executor evaluation](IMP-026.md) — Evaluate pipelining a Gauss-Seidel DAG so node N+1 overlaps node N; FMI analysis shows it is not viable
- **IMP-030** [Remote result ingestion via InfluxDB Line Protocol](IMP-030.md) — Add a remote result sink that emits simulation results over InfluxDB Line Protocol for multi-source ingestion
- **IMP-031** [OpenTelemetry distributed observability](IMP-031.md) — Add OpenTelemetry tracing so cross-process simulations are observable as one correlated trace
- **IMP-035** [XML signal list with per-sink behavior](IMP-035.md) — Let a standalone XML signal list select recorded signals and override per-sink behavior
- **IMP-043** [Unit test coverage for boundary connection resolution](IMP-043.md) — Add focused unit tests for trace_boundary_connectors, which currently has zero direct coverage
- **IMP-045** [Investigate signal_sine_gain_add tree/graph output discrepancy](IMP-045.md) — Re-run signal_sine_gain_add and determine whether the tree_output/graph_output discrepancy is a real output-generation bug
- **IMP-048** [Dependency-ordered FMU initialization for SSPs needing connected inputs at init](IMP-048.md) — Initialize co-simulation FMUs source-first so downstream FMUs see committed upstream outputs before exiting init
- **IMP-052** [Executors take ownership of invocables](IMP-052.md) — Decide whether executors should take unique ownership of invocables against the documented shared-ownership rationale
- **IMP-054** [Daemon API stability, run manifest, and parameter mutation](daemon-api-stability-run-manifest-and-parameter-mutation.md) — Stabilize the daemon contract, complete run discovery beyond the active run identity, and add parameter mutation exposed by the GUI's limits
- **IMP-056** [Synchronous variable get and set for FMU wrapping](synchronous-variable-get-and-set-for-fmu-wrapping.md) — SimulatorCore exposes no value accessor; add typed get and set over the per-model input and output signal rings as the FMU-wrapper prerequisite
<!-- pb:index:end -->
