# ssp4sim-gui

Standalone local web GUI for the SSP4SIM daemon and result artifacts. It
controls a simulation through an `EngineBackend` and visualises CSV/SQLite
results read from disk; it does not link the engine.

```bash
pip install -e .
ssp4sim-gui --daemon-socket /tmp/ssp4sim.sock --results-dir ./wd
ssp4sim-gui --offline --results-dir ./wd          # view existing results
```

See [docs/gui.md](../../../docs/gui.md) for architecture, live-data caveats,
security, and tests.
