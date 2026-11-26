---
id: INFO-016
type: info
title: Code structure
summary: Repository layout and the module dependency graph
date: 2026-11-11
status: current
---

# Code structure

Repository layout and module dependency direction; responsibilities and interfaces are owned by the component view.

- **Top level:** `src/lib/` core engine, `src/public/` entry points, `tests/`, `3rdParty/` vendored dependencies, `containers/` build containers.
- **Module dependency graph:** handler FMU loading feeds the analysis graph, which feeds the execution graph; model, execution, and signal depend on utils, and the engine drives both execution and control (it calls control at each macro-step slice top), so execution does not call into control for a per-macro-step hook.

```
handler/ (FMU load) --> analysis graph --> execution graph
                                          |
model/  execution/  signal/ <-------------+
 (FMU wrap) (strategy) (recording)
          |
          +--> utils/ (shared)

runner/ (ThreadedRunner::run)
   +--> stepper/    (SimulationStepper::step, one slice)
   +--> control/    (before_slice pacing, finalization)
```

- **Entry points:** CLI `src/public/ssp4sim_app/`, Python `src/public/python_api/`, control daemon `src/public/ssp4sim_server/`, public headers `src/lib/public_include/` (`cpp_api/`, `c_api/`, and shared C++ types); build-target mapping is the build environment (`INFO-015`).
- **Module areas:** responsibilities and public interfaces are architected in the component view (`INFO-012`); this leaf holds layout and dependency direction only.
