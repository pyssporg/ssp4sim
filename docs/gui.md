# GUI
<!-- Layer: 01-product, 02-architecture, 05-operation -->

`ssp4sim-gui` is a separate local web frontend for controlling a simulation and
inspecting results. It is not part of the simulation engine: it talks to the
control daemon for commands and reads the CSV/SQLite result artifacts for data.
The engine keeps no GUI, no network listener, and no result-streaming path.

The boundary is recorded in the breakdown as
[INFO-044](breakdown/02-architecture/gui-frontend-view.md).

The visual language follows
[sequence_extensions](https://github.com/pyssporg/sequence_extensions) (MIT,
Copyright (c) 2024 jkCXf9X4): a compact dark panel grid, cyan accent, a streamed
event console, and dependency-free ES-module JavaScript.

## Architecture

```text
browser ──HTTP + SSE──> ssp4sim_gui (FastAPI, 127.0.0.1)
                           ├── EngineBackend ── NDJSON / AF_UNIX ──> ssp4simd
                           └── ResultStore ──── read-only ────────> *.csv / *.sqlite
```

- **Control** crosses the `EngineBackend` protocol only
  (`status`, `send`, `subscribe`, `unsubscribe`, `close`). The daemon backend
  speaks newline-delimited JSON over an `AF_UNIX` socket; the offline backend
  disables control and leaves result viewing usable.
- **Values** never cross the daemon socket. The GUI server parses result files
  and serves bounded windows to the browser; live mode fetches only samples
  after the last seen time.
- **Swapping engines** means writing another `EngineBackend`. The result readers,
  HTTP API, and frontend do not change.

The daemon is currently on the `external_control` branch; its protocol is
newline-delimited JSON over an `AF_UNIX` stream (see that branch's
`docs/usage.md`). Build it with `SSP4SIM_BUILD_SERVER=ON`.

## Install And Run

The GUI is a standalone Python package; it is not built by CMake and does not
import `pyssp4sim`.

```bash
python3 -m venv .venv-gui
. .venv-gui/bin/activate
pip install -e src/public/ssp4sim_gui
```

Start the daemon, then the GUI:

```bash
./build/src/public/ssp4sim_server/ssp4simd \
  --config ./resources/embrace/embrace.json \
  --socket /tmp/ssp4sim.sock

ssp4sim-gui \
  --daemon-socket /tmp/ssp4sim.sock \
  --results-dir ./wd \
  --port 8765
```

Open <http://127.0.0.1:8765/>. Use `--no-browser` to suppress the automatic
browser launch. `--config ./resources/embrace/embrace.json` additionally adds the
config-relative working directory to the result roots and shows the configured
start/stop/timestep.

View existing artifacts without a daemon:

```bash
ssp4sim-gui --offline --results-dir ./wd
```

`--results-dir` is repeatable. Every path the GUI opens must resolve inside one
of these roots.

## What The UI Does

| Area | Capability |
|---|---|
| Control | Start, pause, resume, stop, reset, set speed, and live state/time read-out |
| Runs | Discover result files under the roots, open a path, keep several runs open, close them |
| Signals | Lazy run → model → storage → signal tree, full-path search, selected-only, expand/collapse; checking a signal adds it to the active plot; string signals are value-only |
| Plots | Canvas time-series with several signals per plot, several plot tabs, fit/zoom/pan, normalize, follow-live |
| Compare | The same signal from two runs is two distinct series; legend labels include the run |
| Values | Latest value and timestamp per selected signal, larger read-out, inline `y = kx + m` override |
| Overrides | Per-value linear transform applied to plots and read-outs; raw value retained; stored locally |
| Console | On-demand dock tab for daemon events, command responses, and fetch errors; unread dot while hidden |

Live plotting polls the active run every second and appends only new samples.
On every state poll, the GUI consumes the daemon's optional `session_id` and
absolute `artifacts`, opens the active CSV/SQLite artifact immediately, and
tails it while the run is `running` or `paused`. It does not wait for `done`,
so a GUI that connects mid-run still lands on the right file. The `done` event
reuses the same snapshot for a final fetch and marks the run finished. Repeated
polls never reopen the same session; a new `session_id` on the same path resets
that run's samples. Older daemons without the snapshot keep the scan/refresh
fallback.

## Layout

The control strip sits under the top bar, the signal tree fills the left column,
and the runs ribbon is on the right. The centre stacks the plot over a dock with
`Values` and `Console` tabs. `Values` is the default tab, so the table gets the
dock's full width and height; `Console` is opened on demand and shows an unread
dot for new events. Per-tick `time` events are not written to the console.

## Variable Selection And Overrides

Signals are grouped as **run → model → storage → signal**. Opening a run fetches
its signal metadata once; the two-second discovery poll omits it, so a large run
list does not resend every signal name. The tree is lazy:
models and storages collapse, a deep group renders at most 200 leaves and offers
"Show more", and the search box filters across the full path, name, label,
group, and kind while auto-expanding matches. **Selected only** narrows the tree
to the current selection. Checking a signal adds it to the active plot
immediately and to the values table; remove it from a plot with the legend ×.

Every numeric value can carry a viewer-side override:

```text
y = k x + m
```

Set `k` and `m` inline in the Values table and tick the enable box. The
transform is applied to plotted series, axis bounds, tooltips, and the value
read-out; the raw sample stays available in the tooltip and cell title, and the
simulation data is never modified. Overrides persist in browser `localStorage`
and survive reloads.

Engine-side overrides are not available yet. The daemon has no override command
on `main`; the planned `set_override`, `clear_override`, and `list_overrides`
commands are tracked in [IMP-054](breakdown/06-evolution/suggested/daemon-api-stability-run-manifest-and-parameter-mutation.md).
The local override records are the intended payload source when that lands; the
engine boundary (`EngineBackend`) and HTTP command whitelist must be extended at
the same time.

## Result Artifacts And Live Data

The GUI reads what the recorder writes; it does not add a result API to the
engine:

- **CSV** (`CsvRecorderSink`): wide table `time,<signal>,...`, comma-space
  separators, empty cell = not recorded. The source tails complete lines only,
  so a partially written final line stays buffered.
- **SQLite** (`SqliteWALRecorderSink`): one table per storage area named
  `I<run_id>_<model>_<storage>`, with `timestamp_ns` and `simulation_time_s`.
  Tables have no indexes; time-window queries scan.

For smooth realtime plots, prefer CSV with a wall-clock flush interval:

```json
{
  "simulation": {
    "recording": {
      "csv": { "enable": true, "interval": 0.0, "flush_interval": 0.2 }
    }
  }
}
```

SQLite is readable while a run is in progress (WAL), but the sink commits every
10,000 inserted rows, so SQLite is the better choice for post-run queries and
run comparison, not for smooth live updates.

The daemon publishes a per-run `session_id` and absolute artifact paths in
`state` and `done`; this is identity for the active run, not a full run
manifest. Use `simulation.working_dir` per run or a `[TIME]` token in
`simulation.recording.csv.file`; otherwise each run overwrites
`working_dir/result.csv` and clients must use the new `session_id` to tell the
runs apart.

## Security

- The server binds to `127.0.0.1` by default. A non-loopback bind requires
  `--allow-remote`; there is no authentication, so do not expose it.
- The daemon socket is owner-only (`0600`) and local.
- Result paths are confined to the configured roots after canonicalisation.
- Commands are whitelisted to `start`, `pause`, `resume`, `stop`, `reset`,
  `set_speed`, `state`, and `shutdown`.

## Tests

```bash
python -m pytest -q tests/python/gui
```

The reader, backend, and static-asset tests use the standard library only. The
HTTP API tests skip unless FastAPI and HTTPX are importable; install the GUI
package (or its dependencies) to run them.

## Limits

- Configuration and parameters are startup-only; the daemon has no set-parameter
  command. Edit the JSON and `reset`, or restart the daemon.
- There is no single-step command over the daemon.
- The GUI does not spawn `ssp4simd`; start it separately.
- One active simulation per daemon.
- Overrides are viewer-side only until the daemon exposes `set_override`,
  `clear_override`, and `list_overrides`.
- CSV does not escape commas, quotes, or newlines in signal names/string values.
