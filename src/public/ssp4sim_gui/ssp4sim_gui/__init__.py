"""Separate local web GUI for the ssp4sim engine.

The GUI owns no simulation logic. It talks to an :class:`EngineBackend` for
control and to :class:`ResultSource` implementations for time-series data read
straight from the CSV/SQLite result artifacts.
"""

__version__ = "0.1.0"
