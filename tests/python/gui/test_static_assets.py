"""Static-asset contract for the ssp4sim web GUI shell."""

from __future__ import annotations

import re
from html.parser import HTMLParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STATIC = ROOT / "src" / "public" / "ssp4sim_gui" / "ssp4sim_gui" / "static"
INDEX = STATIC / "index.html"
CSS = STATIC / "styles.css"

ATTRIBUTION = "Visual language adapted from sequence_extensions (MIT, © 2024 jkCXf9X4)."

EXPECTED_IDS = {
    "topbar",
    "backend-dot",
    "backend-text",
    "version-text",
    "control-ribbon",
    "control-card",
    "state-badge",
    "sim-time",
    "speed-readout",
    "speed-input",
    "btn-set-speed",
    "btn-start",
    "btn-pause",
    "btn-resume",
    "btn-stop",
    "btn-reset",
    "control-note",
    "runs-ribbon",
    "runs-card",
    "btn-refresh-runs",
    "path-input",
    "btn-open-path",
    "runs-list",
    "open-runs",
    "signals-card",
    "signal-count",
    "signal-filter",
    "btn-expand-all",
    "btn-collapse-all",
    "chk-selected-only",
    "btn-plot-selected",
    "btn-clear-selection",
    "signal-tree",
    "plot-tabs",
    "btn-fit",
    "btn-follow",
    "btn-normalize",
    "btn-clear-plot",
    "plot-status",
    "plot-canvas",
    "plot-empty",
    "plot-tooltip",
    "plot-legend",
    "dock",
    "console",
    "btn-clear-log",
    "console-lines",
    "dock-tabs",
    "tab-values",
    "tab-console",
    "values-hint",
    "values",
    "values-count",
    "values-body",
    "toast",
    "footer",
    "layout",
    "sidebar",
    "content",
    "plot-wrap",
}

EXPECTED_BUTTONS = {
    "btn-set-speed",
    "btn-start",
    "btn-pause",
    "btn-resume",
    "btn-stop",
    "btn-reset",
    "btn-refresh-runs",
    "btn-open-path",
    "btn-expand-all",
    "btn-collapse-all",
    "btn-plot-selected",
    "btn-clear-selection",
    "btn-fit",
    "btn-follow",
    "btn-normalize",
    "btn-clear-plot",
    "btn-clear-log",
}

PALETTE = {
    "bg": "#0d1117",
    "panel": "#111820",
    "panel-2": "#161f2a",
    "line": "#22303f",
    "text": "#d7e2ee",
    "muted": "#8ba3b8",
    "accent": "#4fc1ff",
    "ok": "#3fb950",
    "warn": "#d29922",
    "err": "#f85149",
}


class _PageParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.tags: list[tuple[str, dict[str, str | None]]] = []
        self.ids: list[str] = []

    def _record(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        self.tags.append((tag, attributes))
        if "id" in attributes:
            self.ids.append(attributes["id"] or "")

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self._record(tag, attrs)

    def handle_startendtag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self._record(tag, attrs)


def _index_text() -> str:
    return INDEX.read_text(encoding="utf-8")


def _parse_index() -> _PageParser:
    parser = _PageParser()
    parser.feed(_index_text())
    parser.close()
    return parser


def test_static_files_exist() -> None:
    assert INDEX.is_file(), f"missing {INDEX}"
    assert CSS.is_file(), f"missing {CSS}"


def test_index_references_static_assets() -> None:
    text = _index_text()
    assert '<link rel="stylesheet" href="/static/styles.css">' in text
    assert re.search(
        r'<script type="module" src="/static/app\.js"></script>\s*</body>',
        text,
    ), "app.js module script must be the last element before </body>"


def test_index_emits_the_exact_id_contract() -> None:
    parser = _parse_index()
    assert len(parser.ids) == len(set(parser.ids)), "duplicate ids in index.html"
    assert set(parser.ids) == EXPECTED_IDS


def test_buttons_use_the_button_classes() -> None:
    parser = _parse_index()
    buttons = {
        attrs.get("id"): attrs for tag, attrs in parser.tags if tag == "button"
    }
    assert set(buttons) == EXPECTED_BUTTONS

    allowed = {"btn", "primary", "ghost", "warn", "small"}
    for button_id, attrs in buttons.items():
        classes = set((attrs.get("class") or "").split())
        assert "btn" in classes, f"{button_id} is missing the btn class"
        assert classes <= allowed, f"{button_id} has unexpected classes: {classes}"

    assert "primary" in buttons["btn-start"]["class"].split()
    assert "warn" in buttons["btn-stop"]["class"].split()


def test_toggle_buttons_start_unpressed() -> None:
    parser = _parse_index()
    buttons = {
        attrs.get("id"): attrs for tag, attrs in parser.tags if tag == "button"
    }
    for button_id in ("btn-follow", "btn-normalize"):
        assert buttons[button_id].get("aria-pressed") == "false"


def test_values_table_contract() -> None:
    text = _index_text()
    assert (
        "<thead><tr><th>Run</th><th>Signal</th><th>Value</th><th>Override</th><th>Time</th></tr></thead>"
        in text
    )
    parser = _parse_index()
    assert any(
        tag == "tbody" and attrs.get("id") == "values-body"
        for tag, attrs in parser.tags
    )


def test_no_external_assets_or_inline_handlers() -> None:
    text = _index_text()
    for scheme in ("http://", "https://"):
        assert scheme not in text
    assert "http://" not in CSS.read_text(encoding="utf-8")
    assert "https://" not in CSS.read_text(encoding="utf-8")

    for _tag, attrs in _parse_index().tags:
        assert not any(name.lower().startswith("on") for name in attrs)


def test_attribution_is_repeated_in_markup_and_css() -> None:
    assert ATTRIBUTION in _index_text()
    assert ATTRIBUTION in CSS.read_text(encoding="utf-8")


def test_css_defines_the_palette() -> None:
    css = CSS.read_text(encoding="utf-8")
    for name, value in PALETTE.items():
        assert re.search(rf"--{re.escape(name)}:\s*{re.escape(value)};", css), (
            f"missing palette property --{name}: {value}"
        )
