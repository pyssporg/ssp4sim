---
id: IMP-056
type: imp
title: Synchronous variable get and set for FMU wrapping
summary: SimulatorCore exposes no value accessor; add typed get and set over the per-model input and output signal rings as the FMU-wrapper prerequisite
date: 2026-11-27
status: suggested
---

# Synchronous variable get and set for FMU wrapping

## Purpose

The FMU-wrapper engineer uses this node to fix the core's minimum typed value surface; without it, the wrapper reads private rings and mishandles input-set ordering.

---

**Question:** What typed get/set surface must `SimulatorCore` and its synchronous C ABI expose for an FMU wrapper?

**Evidence:** `SimulatorCore` has no value accessor (`simulator_core.hpp:37-97`) but is the intended FMU/embedded core (`docs/usage.md:150-157`). Values sit in per-model `SignalStorage` rings at a dense raw-byte index (`storage.hpp:114-121`); `FmuModel::pre` copies wired inputs before the FMU write (`model_fmu.cpp:155-168`) and `post` commits outputs (`model_fmu.cpp:188-206`). Planned overrides target `(side, model, name)` with no discovery call (`REQ-016`).

**Change:** Add discovery (model, name, type, size, direction) and typed `get_*`/`set_*` for Real, Integer, Boolean, String, and Enumeration, resolving `(model, name)` to `(storage, index, type)` after `init()`. A set is a pending input-ring value for the next step; decide whether it overrides `pre()`'s wire copy or is rejected, since an FMU wrapper needs boundary inputs settable before routing. `get` reads the latest committed output ring at `sim_time_ns()`. No storage type becomes public; the `Simulator` thread and daemon are out of scope.

**Risk:** Medium — set-versus-resolver precedence changes wired-input semantics and must be explicit; `get` must not read an uncommitted head; the unchecked ring (`storage.hpp:114-121`) needs type and size rejection before writing; the single-threaded core (`simulator_core.hpp:33-35`) leaves serialization to the wrapper and adds no lock.

**Acceptance:** A one-FMU SSP test sets an input, sees it recorded, and gets the matching output; precedence, unknown targets, wrong type or size, and unused get/set are covered; C++ and C ABI tests pass.
