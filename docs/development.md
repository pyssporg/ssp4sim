# Development Guide
<!-- Layer: 03-implementation, 02-architecture, 05-operation -->


This page collects contributor conventions. Build commands live in
[Build From Source](build_from_source.md), test-suite details live in
[tests/README.md](../tests/README.md), and runtime configuration lives in
[Configuration](configuration.md).

## Repository Layout

For the complete repository structure, see the [code structure overview](breakdown/03-implementation/code-structure.md).
For detailed architecture information, see the [architecture layer](breakdown/02-architecture/).

## Control Modules

Runtime control has three roles across two folders: the caller-facing facade,
the single locked queue/status object, and the internal simulation side.

- **Caller facade** — `src/lib/public_include/cpp_api/simulator.hpp`:
  `Simulator` is the caller-facing facade and the sole action producer. It owns
  only the creation config, posts `Start`/`Pause`/`Resume`/`Stop`/`SetSpeed`/
  `Snapshot` through its internal runtime's `ControlChannel`, reads status
  through it, and delegates the run thread and outer loop to that runtime.
  `restore` and `reset` use the internal `detail::RebuildAccess::rebuild`
  helper, which builds replacements through the private
  `create_initialized`/`create_restored` factories. The daemon is a pure
  public-`Simulator` client and never includes the internal rebuild header.
- **Control channel** —
  `src/lib/include/simulation/runtime/control_channel.{hpp,cpp}`:
  `ControlChannel` is the only shared object between the caller and the run
  thread. One mutex and one condition variable guard `RunState`, simulation
  time, speed, the applied sequence, the last error, and one
  `std::multimap<due_ns, Action>` of `Start`, `Pause`, `Resume`, `Stop`,
  `SetSpeed`, and `Snapshot` actions, plus a closed flag. Only the runtime
  writes `RunState`. `post()` is fire-and-forget; `post_and_wait()` enqueues
  and blocks for the applied result. `PacingClock` is a separate collaborator
  with speed, wall/simulation anchors, and pure deadline math; it stores no
  simulation time.
- **Simulation side** — same folder:
  `SimulationRuntime` owns the synchronous `SimulatorCore`, the run thread,
  the future, a second `lifecycle_mutex_` for that handle only (never nested
  with the channel lock), and the outer macro-step loop. At each loop top it
  takes a due action, else steps the core while Running, else waits on the
  channel condition variable for an action or the pacing deadline. One epilogue
  closes the channel, publishes the final time, flushes the recorder, then
  publishes the terminal state; it implements `run()`, `start()`, `wait()`,
  `shutdown()`, `snapshot()`, and `load_snapshot()`.
- `src/lib/public_include/cpp_api/simulator_core.hpp`: synchronous C++ core
  (`SimulatorCore`), which owns `save_snapshot`/`load_snapshot`.
- `src/lib/public_include/c_api/`: threaded and synchronous C ABIs plus the
  shared status/error header.
- `src/lib/public_include/runtime_control.hpp`: public state/result enums,
  shared by the C++, C, and Python surfaces.
- `src/public/ssp4sim_server/`: the `ssp4simd` frontend (`daemon`, Unix-domain
  socket `server`, NDJSON framing and `protocol`). It links the engine and adds
  no listening network port to the engine.
- `tests/lib/control/`: unit tests for `ControlChannel` (queue, state, pacing)
  and `PacingClock`.

Variable overrides are planned/deferred and are not in current releases.

## Core Development Loop

1. Build with the smallest relevant source-build workflow.
2. Run the focused test that proves the change.
3. Run one example simulation when behavior changes user-visible runtime output.
4. Update the relevant documentation when behavior, commands, architecture, or
   workflow assumptions change.

Use [Build From Source](build_from_source.md) for exact build commands and
[tests/README.md](../tests/README.md) for test selection.

## C++ Test Build Isolation

Kernel, parser, and analysis/graph tests under `tests/lib/` are standalone
per-test executables (`test_<name>`) whose sources are computed at configure
time as the transitive include cone of the test file. The cone rule relies on
the invariant that implementation `.cpp` files sit adjacent to their `.hpp`
under `src/lib/include/**` in normalized layout; keep that invariant when adding
implementation files. Do not hand-maintain cone lists — new test files are
picked up automatically. Integration tests (full-pipeline dependencies) are
listed explicitly in `SSP4SIM_INTEGRATION_TESTS` in `tests/lib/CMakeLists.txt`
and remain in the retained `ssp4sim_tests` binary. A bug in one `src/lib/` file
therefore blocks only the tests whose cones reach it.

## Coding Style

- C++ formatting follows a 4-space indent with braces on the next line.
- Class names use `PascalCase`; functions and files generally use
  `snake_case`.
- Keep includes ordered and local headers grouped consistently with nearby
  files under `src/lib/include/**`.
- No enforced formatter is checked in; match surrounding style closely.
- Keep changes direct and explicit. Minimize duplication, but do not introduce
  abstractions that hide important simulation or memory-layout details.
- Prefer repository-owned code in `src/lib/`, `src/public/`, `tests/`, `resources/`,
  and `resources/scripts/` over patching vendored dependencies in `3rdParty/`, unless
  the task is explicitly about vendored behavior.
- Treat this as active experimental software: clear root-cause fixes are
  preferred over compatibility shims and broad defensive workarounds.

## Dependencies And Generated Data

- CMake presets (`CMakePresets.json`) expect vcpkg; see
  [vcpkg.md](../vcpkg.md) for setup.
- Common optional build flags: `SSP4SIM_BUILD_TEST`,
  `SSP4SIM_BUILD_PYTHON_API`, `SSP4SIM_BUILD_SERVER`, and
  `SSP4SIM_LOG_HOT_PATH`.
- Use the repo-local `venv` for Python commands when it exists. Prefer
  `. venv/bin/activate && <command>` or `venv/bin/python <command>` over the
  system Python for workflow scripts and pytest.
See [Dependency Policy](breakdown/03-implementation/dependency-policy.md) for the authoritative policy on generated and fixture data, including Python environment guidelines.

## Configuration Work

Update the configuration reference when adding, removing, renaming, or changing
defaults for keys read from `utils::Config`.

## Executor Stacks And The Single-Resolver Rule

Executors nest: `ExecutorBase` is an `Invocable` whose children may themselves be
executors (e.g. `SerialSeidel -> LinearSubstepExecutor -> FmuModel`). `Simulator`
owns the outer macro-step loop. When composing a stack:

- **Configuration is parsed once, centrally; options are nested.** The
  `SharedConfig::from_file/from_string/from_json` factories and
  `ssp4sim::ExecutorOptions::from_json()`
  (`src/lib/public_include/shared_config.hpp`) are the only places that parse
  configuration; `utils::Config` is an internal per-instance value, never a
  process-global. Option structs are *nested* so each layer passes its
  slice straight through — no local re-aggregation:
  `SharedConfig::executor` is the `ssp4sim::ExecutorOptions`
  (the sole `ExecutorBuilder` input), `ExecutorOptions::la2` *is* the
  `ssp4sim::La2Options` consumed by `make_la2_stack`, and
  `SharedConfig::fmu` *is* the `ssp4sim::FmuModelConfig` handed to the model
  layer. Executors and models are constructed from these typed values and
  never parse configuration themselves.
- **Dispatch is a variant registry over the typed options.** `ExecutorBuilder`
  is constructed with only `ssp4sim::ExecutorOptions` and holds a flat list of
  registered leaf variants
  (`ExecutorBuilder::variants_`). Each `Variant` is one concrete executor:
  a `selects` predicate over the options plus a uniform `(nodes) -> executor`
  factory reading only the builder's typed options. Every family registers its
  leaf forms as separate variants (jacobi: serial + TBB / spin / futures;
  seidel: serial / parallel stub; custom delay x2; la2 with both `la2` and
  `loop_aware` accepted by one selector; `parallel_seidel` /
  `parallel-seidel` as a first-class throwing variant). `ExecutorBuilder::build`
  scans the registry and requires *exactly one* matching variant — no
  per-family branching: none match is a precise no-match error naming the
  method, more than one is an ambiguity error. The resolved factory builds the
  specialized executor, which `build` returns directly.
  `SimulationRuntime::run()` owns the outer macro-step loop: it takes a due
  `Action` from `ControlChannel` at each loop top (pacing and deferred
  commands), executes it while it owns the core, calls `SimulatorCore::step()`
  for one configured step when Running, publishes the completed step time, and
  then flushes and finalizes through the channel epilogue. Every exit path
  (pre-init failure, pre-run cancellation, normal or error exit) closes the
  channel before the recorder flush. The synchronous `SimulatorCore`
  owns stepping, graph invocation, recorder, and the final partial step.
- **la2 is a factory, not a class.** There is no `La2Scheduler` type;
  `make_la2_stack(nodes, La2Options)` (`executor/loop_aware/la2_builder.hpp`)
  is a config-free, pure construction function that SCC-partitions the graph,
  wraps each loop SCC in a `LinearSubstepExecutor` /
  `GeometricSubstepExecutor`, condenses the component DAG
  (`utils/graph/rewire.hpp`), and returns the outer `SerialSeidel` over the
  condensed graph. `ExecutorBuilder`'s `la2` / `loop_aware` strategies pass
  `options.la2` (the nested `ssp4sim::La2Options`) straight through.
  `options.parallel` requests `ParallelSeidel` as the outer executor; while
  that stub is unimplemented it throws during assembly.
- **Models receive the nested `SharedConfig::fmu`.** `FmuModel` is constructed
  with the `ssp4sim::FmuModelConfig` (experiment times in ns, tolerance,
  forward derivatives, FMU logging), which is *owned* by `SharedConfig`
  (`SharedConfig::fmu` is the single source of truth for
  `simulation.timestep` / `start_time` / `stop_time`), passed untouched
  through `GraphBuilder` by `pre::build_simulation_graph`, and forwarded to
  every model.
- **The read policy is broken out of every executor.** Resolver setup was
  moved out of the executor constructors entirely — `SeidelBase`,
  `JacobiBase`, the three `JacobiParallel*` variants and
  `DelayExecutorBase` no longer derive or install a resolver (and
  `ExecutorBase::set_resolver` / `raw_nodes` are gone). Assembly owns it:
  `make_la2_stack` derives the single `La2DataAccessResolver` from the SCC
  partition and installs it over every model; the `ExecutorBuilder` factories
  (jacobi / custom delay -> StartTime, seidel -> EndTime) derive a flat
  `DataAccessResolver` stamped with the family's default mode and install it.
  Both funnel through one canonical install path, `install_resolver()` /
  `install_flat_resolver()` (`resolver/data_access_resolver.hpp`), which hands
  the shared resolver to every `FmuModel::access_resolver` — nested or not,
  so the assembler's resolver is authoritative. No executor constructor has
  referential knowledge of the read policy; inner executors are
  resolver-neutral (the `LinearSubstepExecutor` /
  `GeometricSubstepExecutor` classes never touched it).

Reusable building blocks that serve la2 but live outside it:

- `utils/graph/graph.hpp` — `ssp4sim::utils::graph::Graph` is the common
  graph-analysis interface (SCC detection, per-SCC loop classification,
  component DAG, topological sort, `verify_placement()`), composed from the
  building blocks below; `component_dag()` exposes the SCC DAG as
  `scc index -> successor scc indices`. Results are a snapshot of the
  adjacency at `analyze()` time — composition changes go through
  `utils/graph/rewire.hpp`, then `analyze()` is run again.
- `utils/graph/rewire.hpp` — connection-mutation building block:
  `condense_component_dag()` rewires a component DAG onto representative nodes
  (only representative adjacency is rebuilt; model data edges
  (`model->connections`) are untouched for resolver lookups), plus the
  `disconnect()` / `redirect()` primitives used to change graph composition.
- `utils/graph/topological_sort.hpp` — `topological_sort()` Kahn-orders the
  component DAG (shared `scc index -> successor scc indices` representation)
  and throws on a cycle.
- `LinearSubstepExecutor` / `GeometricSubstepExecutor` — self-contained executors
  (`executor/substep/`) that relax any group of nodes over a linear or shrinking
  sub-step schedule. `LinearSubstepExecutor` builds its equal sub-step schedule
  inline in `invoke` (base sub-step + remainder spread over the first
  sub-steps); `GeometricSubstepExecutor` owns its schedule construction
  (`build_schedule` static). Both sweep the group in parallel per sub-step; use
  them outside la2 to sub-step groups on equal or shrinking schedules. They
  take no pacing parameter: the runner's outer loop owns pacing via
  `ControlChannel::wait_for_work()` and the `PacingClock` deadline.
  `LinearSubstepExecutor` requires `iterations` >= 1 (its constructor throws on
  0); `GeometricSubstepExecutor` requires a factor in (0, 1) and shrinks each
  sub-step by `factor` until the remaining time is at or below its absolute
  `threshold` (ns), then takes the remaining step whole.

When reusing `SeidelBase` with executors as nodes, remember that its node array
is positional (`index_of_id` maps the process-wide `Node` id -> array index), so executors can
sit alongside models in one outer graph.

## Profiling And Logging

- Build profiling notes: [docs/profiling.md](profiling.md)
- Logging conventions: [docs/logging_guidlines.md](logging_guidlines.md)

## Release Process

The release pipeline and packaging details are documented in
[docs/linux_binary_distribution.md](linux_binary_distribution.md).

Tagging example:

```bash
git tag v0.1.1
git push origin v0.1.1
```

## Contributing

Welcome to the project!
Open an issue or pull request with:

- As clear as possible problem/solution description
- Tests run
- Before/after output when simulation behavior changes

Commit messages should be short, imperative, and sentence case, for example
`Add substeps` or `Split jacobi implementations`.
