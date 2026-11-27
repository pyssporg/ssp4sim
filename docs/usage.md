# Usage
<!-- Layer: 05-operation, 01-product -->


This page covers running simulations after SSP4SIM is installed or built. For
installation steps, see [Installation](installation.md). For source builds, see
[Build From Source](build_from_source.md).

## CLI

Run `sim_app` with one JSON configuration file (the example config is in a
source checkout):

```bash
sim_app ./resources/embrace/embrace.json
```

From a source checkout, use the built executable:

```bash
./build/src/public/ssp4sim_app/sim_app ./resources/embrace/embrace.json
```

Show the embedded CLI version:

```bash
sim_app --version
```

Show usage and options:

```bash
sim_app --help
```

Simulation input is controlled by the JSON file passed to `sim_app`. The full
configuration key reference is in [Configuration](configuration.md).

Example configs:

- [`resources/embrace/embrace.json`](../resources/embrace/embrace.json)
- [`resources/generic_config.json`](../resources/generic_config.json)

## Result Artifacts

SSP4SIM treats simulation results as artifacts that should be useful at three
access levels:

| Category | Current support | Main use case |
|---|---|---|
| Local database | Primary result artifact category. Supported today through SQLite WAL | Queryable local storage for viewers, bounded time-window plots, and run-to-run comparison without repeatedly parsing CSV. |
| CSV | Portable export path via `simulation.recording.csv.*` | Lowest-friction access for scripts, spreadsheets, regression fixtures, and quick inspection. |
| Remote database | Not implemented; the remote ingest interface is decided as InfluxDB Line Protocol ([result artifact strategy](breakdown/01-product/result-artifact-strategy.md)) | Central ingestion from multiple simulation sources for shared dashboards, fleet-scale comparison, and long-lived history. |

The local database is the primary result artifact because it gives the viewer a
typed, indexed query layer for recent windows and run-to-run comparison.
CSV remains the lowest-friction export for scripts and archival handoff. For
concurrent-simulation workflows with a live viewer, SQLite WAL with
per-simulation files is the correct choice because each simulation gets an
independent file, enabling parallel writes without lock contention. Remote
database output remains outside the current configuration surface.

## SQLite Naming Convention

When `simulation.recording.sqlite.file` is not set, the SQLite WAL sink
auto-generates one database file per simulation run:

- **Filename**: `{epoch_seconds}_{session_uuid}.sqlite`
  - `epoch_seconds`: Unix epoch timestamp at database open time.
  - `session_uuid`: RFC 4122 UUIDv4 generated once per `Simulator` lifetime.
- **Table names**: `I{run_id}_{model}_{storage_name}`
  - `run_id`: auto-incrementing counter persisted in the `ssp4sim_run_counter` table. Starts at 1 for fresh files, increments for appends to shared files.
  - `model` and `storage_name`: sanitized alphanumeric components from the signal storage name.
- **Example**: `1748739201_a1b2c3d4-e5f6-4789-abcd-ef0123456789.sqlite` containing tables like `I1_Consumer_output`, `I1_Aux_output`.

When `sqlite.file` is set explicitly (shared-file mode), the run counter persists
across runs: run 1 gets `I1_Consumer_output`, run 2 gets `I2_Consumer_output`, etc.
The `ssp4sim_run_counter` table enables table discovery without a metadata side-table.

## Concurrent Execution and Local Database Safety

**No local output format supports safe concurrent multi-writer access except
per-simulation-file SQLite WAL.** This is a fundamental constraint, not a
transient limitation:

| Backend | Concurrent-writer safe? | Reason |
|---|---|---|
| SQLite WAL (per-sim files, default when `sqlite.file` absent) | **Yes** | Each simulation gets an independent `.sqlite` file. No write-lock contention. |
| SQLite WAL (single shared file, opt-in via `sqlite.file`) | **No** | SQLite write lock is per database file, not per table. Writers serialize. Must never be used with concurrent writers. |
| CSV | **No** | File-level append race. Concurrent writes produce corrupted output. |

If you need to run multiple simulations concurrently (e.g., parameter sweeps,
Monte Carlo runs), you **must** use the SQLite WAL sink without setting
`simulation.recording.sqlite.file` — the per-simulation auto-naming is the
safe default. If you set `sqlite.file` to use a shared file, the concurrent
safety guarantee is lost and the runs must be strictly sequential. CSV and
SQLite sinks can run alongside for post-run analysis but must not be
the sole recorder in concurrent workflows.

Recording details live in [Configuration](configuration.md).

## C++ API

`Simulator` accepts either a config path (the unchanged convenience form) or a
prepared `SharedConfig` built by `from_file`, `from_string`, or `from_json`:

```cpp
#include "shared_config.hpp"
#include "cpp_api/simulator.hpp"

ssp4sim::Simulator from_file("./resources/embrace/embrace.json");

auto config = ssp4sim::SharedConfig::from_string(R"({
  "simulation": {
    "ssp": "./resources/embrace/embrace_scen.ssp",
    "start_time": 0.0,
    "stop_time": 2000,
    "timestep": 0.01,
    "tolerance": 1e-4,
    "working_dir": "./wd/from_string",
    "executor": {"method": "jacobi"}
  }
})");
ssp4sim::Simulator from_object(std::move(config));

from_file.init();
from_file.simulate();
from_object.init();
from_object.simulate();
```

Each `Simulator` owns its configuration, so several instances in one process can
run with different configurations. Two limits remain process-wide: logging sinks
are configured globally, and the SSP is still loaded from the path in
`simulation.ssp`, not from in-memory bytes.

### Synchronous C++ API

`cpp_api/simulator_core.hpp` exposes `SimulatorCore` directly. It has no worker
thread, pacing, pause, or speed: callers drive time with `step()` or
`advance_to(target_ns)`, read `sim_time_ns()`/`end_time_ns()`, and call
`finish()` to flush recording. This is the C++ counterpart of the synchronous
C ABI and the intended core for future FMU/embedded wrappers.

```cpp
#include "cpp_api/simulator_core.hpp"

ssp4sim::SimulatorCore core(std::move(config));
core.init();
core.start();
while (core.step() == ssp4sim::StepStatus::Advanced) {
}
core.finish();
```

## Python API

Install the release wheel or build the Python API from source before importing
`pyssp4sim`.

```python
import pyssp4sim

sim = pyssp4sim.Simulator("./resources/embrace/embrace.json")
sim.init()
sim.simulate()
```

The wheel also installs a `pyssp4sim` command that wraps the same flow:

```bash
pyssp4sim ./resources/embrace/embrace.json
```

Show the installed Python API version:

```bash
pyssp4sim --version
```

(`-v` is an alias for `--version`.) Run `pyssp4sim --help` for usage and
options.

## Runtime Control

The C++ `Simulator`, the C API, and the Python `Simulator` expose the same
runtime control in addition to the one-shot `init()` / `simulate()` flow.
`simulate()` is exactly `start()` followed by `wait()`; its blocking behaviour
and exception mapping are unchanged.

| Operation | C++ | C ABI (`c_api/simulator_c_api.h`) | Python |
|---|---|---|---|
| Initialize | `init()` | `ssp4sim_simulator_init` | `init()` |
| Start | `start()` (blocks until running or terminal) | `ssp4sim_simulator_start` | `start()` |
| Pause / resume | `pause(at_sim_time_ns = nullopt)` / `resume()` | `ssp4sim_simulator_pause` / `..._resume` | `pause(at=None)` / `resume()` |
| Stop | `stop(at_sim_time_ns = nullopt)` | `ssp4sim_simulator_stop` | `stop(at=None)` |
| Reset | `reset()` | `ssp4sim_simulator_reset` | `reset()` |
| Set speed | `set_speed(double, at_sim_time_ns = nullopt)` | `ssp4sim_simulator_set_speed` | `set_speed(speed, at=None)` |
| Read coherent state/time/speed | `info()` → `RunInfo` | `ssp4sim_simulator_get_info` → `ssp4sim_run_info` | `info()` → `dict` |
| Wait for finish | `wait()` → `RunResult` | `ssp4sim_simulator_wait(handle, &result)` | `wait()` → `str` |
| One-shot run | `simulate()` | `ssp4sim_simulator_simulate` | `simulate()` |

**States.** The lifecycle is `created`, `initialized`, `running`, `paused`,
`stopping`, `completed`, `stopped`, `failed`. Only one run is allowed per
simulator instance (`Simulator`); a second `start()` throws, and `start()`
blocks until the run is running or terminal. A terminal outcome is monotonic:
no later `stop()` changes it. `stop()` is idempotent once `stopping` or
terminal, preserves results up to the last completed macro step, and is an
error before the run starts; stopping an unstarted run through teardown yields
`stopped`. A completed run reports the configured end time.

**Reset.** `reset()` is blocking: it stops and joins any current run, discards
per-run state, and rebuilds a fresh initialized run from the creation config.
On success the simulator is `initialized` at the configured start time, so a
later `init()` is invalid; a subsequent `start()` runs the config from the
beginning.

**Speed.** `set_speed()` takes a finite factor `>= 0`: `0` runs as fast as
possible, `1` is realtime, values above `1` run faster, and values between `0`
and `1` run slower. Speed changes only wall-clock pacing, never simulation
results.

**Read-back.** `info()` is the sole public read-back and returns one coherent
`RunInfo` with `state`, `sim_time_ns`, and `speed` (C++ `RunInfo`; C
`ssp4sim_run_info` by value; Python `dict` with `state` as a string,
`sim_time_ns` as an int, and `speed` as a float). A null C handle returns the
zero/default record and sets `ssp4sim_last_error()`. `sim_time_ns` is the
current simulation time in nanoseconds; before the first step it is the
configured `simulation.start_time`, which can be non-zero. `speed` is the
current wall-clock pace (`0` = as fast as possible).

**Overrides (planned).** Variable overrides (`set_override`, `clear_override`, `list_overrides`) and `y = k*x + m` behavior are planned/deferred and are not in current releases.

**Scheduling.** `pause`, `stop`, and `set_speed` accept an optional simulation time (`at_sim_time_ns` in C++ and the C ABI, `at` in Python and the daemon). Without it the command applies at the next macro-step boundary; with it the command is deferred to the first macro-step boundary at or after that time, so activation granularity is one macro step. The C ABI adds `has_at_sim_time` and `at_sim_time_ns` to each schedulable call; C++ takes nanoseconds and Python takes seconds. `resume()` has no scheduled form. Scheduling of override commands is planned with the deferred override feature.

**Waiting and results.** `wait()` is idempotent. In C++ it returns `RunResult`
(`Completed` or `Stopped`) and throws on failure. The C ABI
`ssp4sim_simulator_wait` returns `SSP4SIM_OK` for Completed or Stopped and
writes the `ssp4sim_run_result`; a failed run returns
`SSP4SIM_RUNTIME_ERROR` and leaves the message in `ssp4sim_last_error()`, never
a failed result with `SSP4SIM_OK`. Python returns `"completed"` or `"stopped"`
and raises `RuntimeError` on failure.

```python
import pyssp4sim

sim = pyssp4sim.Simulator("./resources/embrace/embrace.json")
sim.init()

sim.set_speed(2.0)

sim.start()  # returns when running or terminal
sim.pause()
sim.set_speed(0.5)
sim.resume()
print(sim.info())  # coherent state, sim_time_ns, speed
print(sim.wait())  # "completed" or "stopped"; raises on failure
```

The C wrapper symbols are declared in
[`c_api/simulator_c_api.h`](../src/lib/public_include/c_api/simulator_c_api.h);
`ssp4sim_simulator_destroy` releases the handle and
`ssp4sim_last_error` returns the last error message.

### Synchronous C API

[`c_api/simulator_core_c_api.h`](../src/lib/public_include/c_api/simulator_core_c_api.h) exposes the
internal `SimulatorCore` directly as `ssp4sim_simulator_core_*`. It has no worker
thread, wall-clock pacing, pause, or speed: callers drive time with `step()`
(one configured macro step) or `advance_to(target_ns)` (an exact target,
clamped to the end time), read `sim_time_ns`/`end_time_ns` with `get_info()`,
and call `finish()` to stop and flush the recorder. `destroy()` does not call
`finish()`, so callers must call `finish()` first to flush recording. Both C
ABIs share `c_api/c_api_common.h` (`ssp4sim_status` and `ssp4sim_last_error`).

```c
#include "c_api/simulator_core_c_api.h"

ssp4sim_simulator_core_handle *core = NULL;
if (ssp4sim_simulator_core_create("./config.json", &core) == SSP4SIM_STATUS_OK) {
    ssp4sim_simulator_core_init(core);
    ssp4sim_simulator_core_start(core);
    ssp4sim_step_status status;
    while (ssp4sim_simulator_core_step(core, &status) == SSP4SIM_STATUS_OK &&
           status == SSP4SIM_STEP_ADVANCED) {
    }
    ssp4sim_simulator_core_finish(core);
    ssp4sim_simulator_core_destroy(core);
}
```

## Control Daemon

`ssp4simd` is a local frontend that drives the simulation runtime
(`Simulator`) directly over a Unix domain socket; external clients use the
C and Python APIs. It is built by default; see
[Build From Source](build_from_source.md#control-daemon).

```bash
ssp4simd --config ./resources/embrace/embrace.json --socket /tmp/ssp4sim.sock
```

`--config` and `--socket` are both required, and only the separated
`--config <path>` / `--socket <path>` forms are accepted. `-h`/`--help` and
`-v`/`--version` are also available. The socket is created with mode `0600`, a
stale socket path is replaced, a live server at the path refuses startup, and
the socket is unlinked on clean shutdown and on SIGTERM/SIGINT. One
single-threaded poll loop serves all clients.

The protocol is newline-delimited JSON (one object per line, maximum 1 MiB).
Every request carries `"v":1`, an optional `id`, and a string `cmd`. A response
echoes the request `id`; a `snapshot` event carries the `id` of the request that
produced it, and other events carry none. Any connected client may issue
commands; the daemon accepts at most eight clients.

**Commands.**

| `cmd` | Request fields | Success response |
|---|---|---|
| `start` | — | `ok:true` |
| `pause` | optional `at` (seconds) | `ok:true` |
| `resume` | — | `ok:true` |
| `stop` | optional `at` (seconds) | `ok:true` |
| `state` | — | `ok:true`, `state`, `sim_time` (seconds), `speed` |
| `set_speed` | `speed` (finite, `>= 0`), optional `at` (seconds) | `ok:true` |
| `reset` | — | `ok:true`, `accepted:true`; completion arrives as a `lifecycle` event |
| `snapshot` | `path` (non-empty string) | `ok:true`, `accepted:true`; completion arrives as a `snapshot` event |
| `restore` | `path` (non-empty string), optional `start` (default `true`) | `ok:true`, `accepted:true`; completion arrives as a `lifecycle` event |
| `shutdown` | — | `ok:true`, then the daemon exits |

`at` and `sim_time` are seconds.
A scheduled command activates at the first macro-step boundary at or after
`at`. Override commands (`set_override`, `clear_override`, `list_overrides`) are
planned/deferred and are not in current releases.

A response is `{"v":1,"id":<id>,"ok":true,...}` on success or
`{"v":1,"id":<id>,"ok":false,"error":{"code":"...","message":"..."}}` on
failure. Typical response error codes include `bad_json`, `bad_version`,
`bad_request`, `bad_speed`, `bad_value`, `bad_state`, `unknown_command`, and
`server_busy`. A lifecycle rebuild failure is reported in the `lifecycle` event
with code `reset_failed` or `restore_failed`.

**Events.** Events are pushed to every client; only `snapshot` events carry an `id`:

| `event` | Fields | Meaning |
|---|---|---|
| `state` | `state`, `sim_time`, `speed` | A lifecycle state change. |
| `time` | `sim_time`, `speed` | Progress while running or paused, throttled to about 10 Hz. |
| `done` | `result`, `sim_time` | Terminal outcome; `result` is `completed`, `stopped`, or `failed`. |
| `error` | `code`, `message` | A simulation failure, emitted before `done`. |
| `snapshot` | `id` (the request id, omitted when absent), `ok`, `code`, `path`, `models_done`, `models_total`, and either `sim_time` (seconds) plus `bytes` (total checkpoint file size), or `error` | A snapshot completed or failed (running or paused). `code` is `ok` on success, otherwise `run_ended` or the failure code. |
| `lifecycle` | `operation` (`reset` or `restore`), `ok`, and either `state` plus `sim_time` (seconds), or `code` plus `error` | A reset or restore finished. On failure `code` is `reset_failed` or `restore_failed` and the previous (stopped) runner is kept. |

`time` events are droppable: a slow client can miss them and may be
disconnected. `state`, `done`, `error`, `snapshot`, and
`lifecycle` events are not droppable.

**`reset` and `shutdown`.** `reset` is asynchronous: it replies `accepted` and
rebuilds on a background thread. While that job runs, `state` and `shutdown`
still work but every mutating command (including `snapshot`) returns
`server_busy`. The background thread stops and joins any live run, emits its
terminal `state` and `done` before the fresh `initialized` state, destroys the
old runner, and installs a freshly constructed, initialized runner built from
the original parsed config, so no in-memory simulation state (FMUs, rings,
resolver frontiers, time, or pending snapshot work) survives. Configured output
destinations are reused rather than deleted: CSV truncates when the new sink
opens, and an explicitly configured SQLite database is retained with a new run
id. Completion is reported by a `lifecycle` event with `operation:"reset"`.
There is one active simulation per daemon. `shutdown` joins any lifecycle job,
stops and waits for a live run, closes clients, unlinks the socket, and exits.

**`snapshot` and `restore`.** `snapshot` requires
`Running` or `Paused` and works identically in both states. The daemon owns an
async snapshot worker and a request queue, so several requests may be queued;
each one immediately replies `accepted`. The worker calls the blocking
`Simulator::snapshot(path)`, which posts one run-thread task on the time-sorted
queue inside `RunControl`; the run worker consumes it at the next
macro-step boundary. Success or failure always arrives as one `snapshot` event
carrying the request `id` and a `code`. There are no snapshot progress events.
A checkpoint is a version-4 `SSP4SNAP` container: a checksummed 28-byte header,
a JSON manifest, and a payload made of self-serialized sections listed in the
manifest (one per live FMU model plus one resolver-frontier section). The
manifest carries the SSP content hash and `sim_time_ns`. The checksum covers
the manifest and payload as a whole. String-valued signal
storage is carried inside the model sections with zeroed binary ranges; invalid
UTF-8 or an embedded NUL is `unsupported`. A save
writes a `<target>.part-<uuid>` temp sibling, fsyncs the temp file strictly,
renames it atomically, and attempts a best-effort parent-directory fsync, so a
partially written file never replaces a good checkpoint. A queued snapshot is
settled as `run_ended` if the run ends before the next step; to discard a
completed checkpoint, delete the file manually.

When `simulation.snapshot.directory` is set, `snapshot` and `restore` reject a
`path` outside that directory with `bad_value`; paths are canonicalized first,
so `..` and symlinks that resolve outside are rejected too. When the key is
absent, any path is accepted.

`restore` is asynchronous like `reset`: it replies `accepted`, stops and joins
any live run on a background thread, rebuilds the runner from the same config,
and loads the snapshot. It requires the same SSP (content hash); experiment
timing (start, timestep, and end) comes from the live config. On completion the
`lifecycle` event with `operation:"restore"` reports `ok:true` and the resulting
`state`/`sim_time`, or `ok:false` with `restore_failed`. With `start:false` the
new run is `Initialized` and remains stopped (`state` confirms it, and `start`
works once the lifecycle event has cleared the busy state); with the default
`start:true` it starts immediately and keeps the prior run's speed (the pace is
carried into the fresh control before start, not reapplied after). All FMUs in
the SSP must declare `canGetAndSetFMUstate` and `canSerializeFMUstate`
(all-or-nothing), or restore fails. Snapshot files are build/version specific
and are not portable across FMU builds. When CSV recording is enabled, restore
writes to a new segment named `<stem>.restored-<uuid><ext>` in the same
directory, so the prior run's recorder output is preserved.

Minimal client (responses and events are read from one persistent connection):

```python
import json
import socket

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect("/tmp/ssp4sim.sock")
stream = sock.makefile("rw")

def call(request):
    stream.write(json.dumps({"v": 1, **request}) + "\n")
    stream.flush()
    while True:
        message = json.loads(stream.readline())
        if message.get("id") == request.get("id"):
            return message
        print("event", message)

print(call({"id": 1, "cmd": "set_speed", "speed": 2.0}))
print(call({"id": 2, "cmd": "start"}))
while True:
    message = json.loads(stream.readline())
    print("event", message)
    if message.get("event") == "done":
        break
call({"id": 3, "cmd": "shutdown"})
```

An illustrative session:

```text
$ ssp4simd --config ./resources/embrace/embrace.json --socket /tmp/ssp4sim.sock &
$ python3 control_client.py
{'v': 1, 'id': 1, 'ok': True}
{'v': 1, 'id': 2, 'ok': True}
event {'v': 1, 'event': 'state', 'state': 'running', 'sim_time': 0.0, 'speed': 2.0}
event {'v': 1, 'event': 'time', 'sim_time': 0.1, 'speed': 2.0}
...
event {'v': 1, 'event': 'done', 'result': 'completed', 'sim_time': 10.0}
```

## Result Artifact Policy


The result artifact policy and supporting tradeoffs are documented in the
[result artifact strategy](breakdown/01-product/result-artifact-strategy.md)
and the [capabilities page](breakdown/01-product/capabilities.md).

## Logging Artifact Strategy

The logging output tiers and their intended uses are documented in the
[logging output artifact strategy](breakdown/01-product/logging-output-artifact-strategy.md).
