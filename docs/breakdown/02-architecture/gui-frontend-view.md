---
id: INFO-044
type: info
title: GUI frontend view
summary: Boundary, components, and protocols of the separate ssp4sim-gui frontend consuming the daemon and result artifacts
date: 2026-11-26
status: current
---

# GUI frontend view

## Purpose

The architect or operator deciding whether a GUI belongs to the engine uses this node to place the separate frontend and its interfaces; without it the breakdown wrongly implies no frontend exists or that it lives inside libssp4sim.

---

`ssp4sim-gui` is a Python package (`src/public/ssp4sim_gui/`, distribution `ssp4sim-gui`) outside `libssp4sim`: not CMake-built, no `pyssp4sim` import, no C++ linkage; the engine opens no network listener (`CON-001`).

Control: browser --HTTP+SSE--> FastAPI app (loopback only, no auth) --`EngineBackend`--> `ssp4simd` over NDJSON/AF_UNIX; backends are `daemon` and `offline`; the daemon lives on `origin/external_control`, not `main`.

Data: browser <-- FastAPI <-- `ResultStore` (read-only) reading wide CSV (`CsvRecorderSink`) and SQLite WAL (`SqliteWALRecorderSink`); values never cross the control channel. `EngineBackend` is the protocol seam; `ResultStore` serves bounded windows; FastAPI/SSE serves HTTP and events. UI: top control ribbon, full-height signal tree, right runs ribbon, multi-signal canvas plots and plot tabs, live follow, tabbed values/console dock, and run comparison.

Live: CSV needs `simulation.recording.csv.flush_interval > 0`; SQLite commits every 10 000 rows; no run manifest, so live detection uses artifact recency, cleared by `done`. Selection is a lazy run → model → storage → signal tree with full-path search and selected-only; checking a signal adds it to the active plot. Numeric values support a viewer-side `y = kx + m` override (`k`, `m`, enable) applied to plots and read-outs and persisted in browser storage; engine-side override awaits `IMP-054`. Parameters are startup-only; no single step; GUI does not spawn the daemon; one active run per daemon. Tests: 49 passed, 1 skipped (stdlib); 55 passed with FastAPI/HTTPX. Detail: `docs/gui.md`; placement: `INFO-011`; open work: `IMP-054`.
