---
id: INFO-010
type: info
title: Context view
summary: SSP4SIM system boundary, external actors, external systems, and trust boundaries
date: 2026-11-11
status: current
---

# Context view

SSP4SIM is a simulation engine: consumes SSP archives, loads FMI 2.0 FMUs, produces simulation results. Direct path from SSP input to executable run, not a full simulation environment. A local control daemon frontend drives the same engine.

- **Actors:** CLI invokes `sim_app` with a JSON config file (exit codes, stdout/stderr); Python caller imports `pyssp4sim` and calls the simulator API; C API caller links the shared library (error code returns); a local control client sends newline-delimited JSON commands to `ssp4simd` over a Unix domain socket. Concrete entry points are code (`INFO-016`).
- **Input systems:** SSP 1.0 archive or unpacked directory (filesystem); FMI 2.0 co-simulation FMU binaries (dlopen/dlsym via the fmi4c adapter, Linux shared libraries); NDJSON control requests on the daemon socket.
- **Output systems:** local result database (SQLite WAL) and CSV export (filesystem); log sinks terminal, file, JSON, cutelog; NDJSON responses and `state`/`time`/`done`/`error` events on the daemon socket. Remote result ingestion via InfluxDB Line Protocol is future output, out of scope for current releases.
- **Trust boundaries:** FMU vendor libraries run unsandboxed with the caller's privileges; config files and SSP archives treated as trusted input — structural validity checked, no security validation. The daemon listens only on a local Unix domain socket (mode 0600), accepts at most eight clients, and lets any connected client command; the engine itself opens no network listener (`INFO-014`).
