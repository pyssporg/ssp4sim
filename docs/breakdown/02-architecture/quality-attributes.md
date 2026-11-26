---
id: INFO-014
type: info
title: Quality attributes
summary: Non-functional characteristics that shape architecture and design decisions
date: 2026-11-11
status: current
---

# Quality attributes

Experimental-project quality: performance, scalability, and reliability are best-effort within the chosen patterns, not formal targets.

- **Performance:** no formal targets; parallel executor variants (future, spin-wait, TBB) exist for experimentation, not production benchmarks; hot-path logging is compile-time optional; recorder uses bounded buffers (50 per storage, 4096-event queue) with optional backpressure. The control monitor samples state every 50 ms and emits `time` events at most every 100 ms (`INFO-023`).
- **Scalability:** single-node only, no distributed or multi-node simulation; thread count configurable via `executor.thread_pool_workers` (`INFO-017`); FMU count bounded by available memory and pool size; recorder backpressure limits throughput when recording cannot keep up. The daemon holds one active simulation and at most eight clients (`INFO-010`).
- **Reliability:** experimental, no formal targets; FMU step failures clean up without fmi2Terminate; recorder buffer overflow drops events with a rate-limited warning; CTest is the canonical test runner (`INFO-028`). The daemon serialises commands, emits one `done` per run, replaces a stale socket, refuses to start over a live server, and disconnects slow clients after dropping their `time` events.
- **Security:** no security model; FMU vendor code runs with the caller's privileges (dlopen, no sandboxing) — trust boundary (`INFO-010`); config files and SSP archives are trusted input. The engine itself has no network exposure; the optional `ssp4simd` frontend listens only on a Unix domain socket with mode 0600, and any connected local client may command.
- **Maintainability:** compact codebase with a narrow runtime surface (project goal); module areas with clear boundaries (`INFO-012`); no enforced formatter — match surrounding style; root-cause fixes preferred over compatibility shims.
