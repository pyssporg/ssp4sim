import { getJSON, postJSON, deleteJSON, connectEvents, ApiError } from './api.js';
import { seriesColor, TimePlot } from './plots.js';

const $ = (id) => document.getElementById(id);

const state = {
  tabs: [],
  activeTabId: null,
  runs: new Map(),
  follow: false,
  normalize: false,
  liveTimer: null,
  stateTimer: null,
  runsTimer: null,
  valuesTimer: null,
  liveInFlight: false,
  liveForcePending: false,
  valuesInFlight: false,
  valuesPending: false,
  stateInFlight: false,
  runsInFlight: false,
  discovered: [],
  selectedRunId: null,
  snapshotHandled: new Set(),
  snapshotClosed: new Set(),
  snapshotErrors: new Set(),
  retiredSessions: new Set(),
  pathSessions: new Map(),
  finishedRunIds: new Set(),
  offline: false,
  simState: 'unknown',
  simTime: 0,
  speed: 1,
  sessionId: null,
  backend: null,
  backendConnected: null,
  sseConnected: null,
  events: null,
  resizeObserver: null,
  toastTimer: null,
  tabSeq: 1,
  treeExpanded: new Set(),
  treeLimits: new Map(),
  selectedOnly: false,
  overrides: new Map(),
  valueRows: new Map(),
  dockTab: 'values',
  consoleUnread: false,
};

let filterTimer = null;

let snapshotChain = Promise.resolve();

let plot = null;

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = String(text);
  return node;
}

function ts() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function fmtTime(value) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return n.toFixed(3);
}

function fmtCount(value) {
  if (value === null || value === undefined) return '—';
  const n = Number(value);
  if (!Number.isFinite(n)) return '—';
  return String(n);
}

function fmtNumber(value, kind) {
  if (value === null || value === undefined) return '—';
  if (kind === 'integer') return String(value);
  if (kind === 'boolean' || typeof value === 'boolean') return String(value);
  if (kind === 'string' || typeof value === 'string') return String(value);
  const n = Number(value);
  if (!Number.isFinite(n)) return String(value);
  if (Number.isInteger(n)) return String(n);
  return String(Number(n.toPrecision(6)));
}

function fmtRange(start, end) {
  const left = start === null || start === undefined ? '?' : fmtTime(start);
  const right = end === null || end === undefined ? '?' : fmtTime(end);
  return `${left}–${right} s`;
}

function query(path, params) {
  const search = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    if (value === null || value === undefined) continue;
    if (Array.isArray(value)) {
      for (const item of value) search.append(key, String(item));
    } else {
      search.append(key, String(value));
    }
  }
  const text = search.toString();
  return text ? `${path}?${text}` : path;
}

function errText(err) {
  if (err === null || err === undefined) return 'unknown error';
  if (typeof ApiError === 'function' && err instanceof ApiError) {
    const code = err.code ? `${err.code}: ` : '';
    return `${code}${err.message || `HTTP ${err.status}`}`;
  }
  if (err.code && err.message) return `${err.code}: ${err.message}`;
  if (err.message) return String(err.message);
  return String(err);
}

function log(message, tone) {
  const box = $('console-lines');
  if (!box) return;
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 4;
  const line = el('span', 'log-line' + (tone === 'err' ? ' err' : tone === 'ok' ? ' ok' : ''));
  line.appendChild(el('span', 'log-time', ts()));
  line.appendChild(document.createTextNode(String(message)));
  box.appendChild(line);
  while (box.childElementCount > 500) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
  markConsoleUnread();
}

function markConsoleUnread() {
  if (state.dockTab === 'console' || state.consoleUnread) return;
  state.consoleUnread = true;
  const tab = $('tab-console');
  if (tab) tab.classList.add('unread');
}

function setDockTab(name) {
  state.dockTab = name === 'console' ? 'console' : 'values';
  const dock = $('dock');
  if (dock) dock.dataset.tab = state.dockTab;
  for (const [id, panelId, tabName] of [
    ['tab-values', 'values', 'values'],
    ['tab-console', 'console', 'console'],
  ]) {
    const tab = $(id);
    const panel = $(panelId);
    const active = state.dockTab === tabName;
    if (tab) {
      tab.classList.toggle('active', active);
      tab.setAttribute('aria-selected', String(active));
    }
    if (panel) panel.classList.toggle('hidden', !active);
  }
  if (state.dockTab === 'console') {
    state.consoleUnread = false;
    const tab = $('tab-console');
    if (tab) tab.classList.remove('unread');
    const box = $('console-lines');
    if (box) box.scrollTop = box.scrollHeight;
  }
}

function showToast(message, isError) {
  const toast = $('toast');
  if (!toast) return;
  toast.textContent = String(message);
  toast.classList.toggle('err', !!isError);
  toast.classList.remove('hidden');
  if (state.toastTimer) clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => {
    toast.classList.add('hidden');
    state.toastTimer = null;
  }, 6000);
  log(message, isError ? 'err' : 'ok');
}

function setDot(kind) {
  const dot = $('backend-dot');
  if (dot) dot.className = `dot ${kind}`;
}

function renderBackendUi() {
  const backend = state.backend || {};
  const name = backend.name || backend.backend || 'unknown';
  const connected = state.backendConnected === true;
  let text = `backend: ${name}`;
  if (!state.offline && !connected) text += ' (disconnected)';
  const detail = backend.detail && backend.detail !== 'connected' ? ` — ${backend.detail}` : '';
  const node = $('backend-text');
  if (node) node.textContent = text + detail;
  setDot(state.offline ? 'err' : connected ? 'ok' : 'warn');
}

function updateTimeUi() {
  const node = $('sim-time');
  if (node) node.textContent = `${fmtTime(state.simTime)} s`;
}

function updateSpeedUi() {
  const node = $('speed-readout');
  if (node) node.textContent = `${Number(state.speed).toFixed(1)}×`;
}

function updateSignalCount() {
  let total = 0;
  for (const entry of state.runs.values()) total += entry.selected.size;
  const node = $('signal-count');
  if (node) node.textContent = String(total);
}

function updateEmptyOverlay() {
  const tab = activeTab();
  const node = $('plot-empty');
  if (!node) return;
  node.classList.toggle('hidden', !!tab && tab.series.size > 0);
}

function resetRunData() {
  for (const entry of state.runs.values()) {
    entry.cursors.clear();
    entry.lastTime = null;
    entry.finished = false;
  }
  state.finishedRunIds.clear();
  for (const tab of state.tabs) {
    for (const series of tab.series.values()) {
      series.t = [];
      series.y = [];
    }
  }
  syncActivePlot();
  void refreshValues();
  log('simulation time reset; cleared plotted samples', 'ok');
}

const SNAPSHOT_FINISHED_STATES = new Set(['completed', 'stopped', 'failed']);
const SNAPSHOT_LIVE_STATES = new Set(['running', 'paused']);

function snapshotFrom(body) {
  if (!body || typeof body !== 'object') return null;
  const sessionId =
    typeof body.session_id === 'string' && body.session_id ? body.session_id : null;
  const artifacts = Array.isArray(body.artifacts) ? body.artifacts : null;
  if (!sessionId || !artifacts) return null;
  const artifact = artifacts.find(
    (item) => item && typeof item.path === 'string' && item.path,
  );
  if (!artifact) return null;
  return { sessionId, path: artifact.path };
}

function findRunByPath(path) {
  return [...state.runs.values()].find((entry) => entry.info.path === path) || null;
}

function resetRunSamples(runId) {
  const entry = state.runs.get(runId);
  if (!entry) return;
  entry.cursors.clear();
  entry.lastTime = null;
  entry.finished = false;
  let activeChanged = false;
  for (const tab of state.tabs) {
    for (const series of tab.series.values()) {
      if (series.runId !== runId) continue;
      series.t = [];
      series.y = [];
      if (tab.id === state.activeTabId) activeChanged = true;
    }
  }
  if (activeChanged) syncActivePlot();
  void refreshValues();
}

function applySnapshotFlags(entry, finished) {
  entry.info.live = !finished;
  entry.finished = finished;
  if (finished) state.finishedRunIds.add(entry.info.run_id);
  else state.finishedRunIds.delete(entry.info.run_id);
}

function selectSnapshotRun(runId) {
  if (state.selectedRunId === null || state.selectedRunId === runId) selectRun(runId);
}

function logSnapshotFailure(key, err) {
  if (state.snapshotErrors.has(key)) return;
  state.snapshotErrors.add(key);
  log(`auto-open run failed: ${errText(err)}`, 'err');
}

async function openArtifact(path) {
  const data = await postJSON('/api/runs/open', { path });
  if (data && data.ok === false) {
    const error = data.error && typeof data.error === 'object' ? data.error : {};
    throw new ApiError(
      400,
      error.code || 'open_failed',
      error.message || 'open failed',
    );
  }
  const info = data && data.run;
  if (!info) throw new Error('server did not return run info');
  return info;
}

async function applySnapshot(data, options = {}) {
  const body = data && data.data && typeof data.data === 'object' ? data.data : data;
  const snapshot = snapshotFrom(body);
  if (!snapshot) return false;
  const stateName = typeof body.state === 'string' ? body.state.toLowerCase() : '';
  const finished = options.finished === true || SNAPSHOT_FINISHED_STATES.has(stateName);
  if (!finished && !SNAPSHOT_LIVE_STATES.has(stateName)) return false;

  const { sessionId, path } = snapshot;
  const key = `${sessionId}\u0000${path}`;
  // A reset retires the previous session; a delayed poll or done from it must
  // never reopen or re-flag the run that the fresh session now owns.
  if (state.retiredSessions.has(sessionId)) return false;
  if (state.snapshotClosed.has(sessionId)) return false;

  const existing = findRunByPath(path);
  if (state.snapshotHandled.has(key)) {
    // Repeated polls of one snapshot only refresh flags; they never reopen.
    if (existing) {
      applySnapshotFlags(existing, finished);
      if (finished) {
        renderRunsList();
        renderOpenRuns();
        ensureLiveTimer();
      }
    }
    return true;
  }

  const previousSession = state.pathSessions.get(path);
  let info;
  try {
    // The store is the trust boundary: a path outside its roots is refused here.
    info = await openArtifact(path);
  } catch (err) {
    logSnapshotFailure(key, err);
    return false;
  }

  const entry = existing || state.runs.get(info.run_id);
  const runId = entry ? entry.info.run_id : info.run_id;
  if (entry) {
    const sessionChanged =
      (previousSession !== undefined && previousSession !== sessionId) ||
      (entry.sessionId && entry.sessionId !== sessionId);
    if (sessionChanged) resetRunSamples(runId);
    entry.info = info;
    entry.sessionId = sessionId;
    entry.autoSession = sessionId;
  } else {
    addRun(info);
    const added = state.runs.get(info.run_id);
    if (added) {
      added.sessionId = sessionId;
      added.autoSession = sessionId;
    }
  }
  state.pathSessions.set(path, sessionId);

  const target = state.runs.get(runId);
  if (target) applySnapshotFlags(target, finished);
  state.snapshotHandled.add(key);

  selectSnapshotRun(runId);
  renderRunsList();
  renderOpenRuns();
  renderSignalTree();
  ensureLiveTimer();
  if (!finished) void pollLive(true);
  return true;
}

function scheduleSnapshot(data, options) {
  snapshotChain = snapshotChain
    .then(() => applySnapshot(data, options))
    .catch((err) => log(`state snapshot failed: ${errText(err)}`, 'err'));
  return snapshotChain;
}

function applyState(data) {
  if (data === null || data === undefined) return;
  if (typeof data === 'string') {
    state.simState = data.toLowerCase();
    updateControlUi();
    return;
  }
  const body = data.data && typeof data.data === 'object' ? data.data : data;
  if (body.ok === false && body.error) {
    log(`state failure: ${errText(body.error)}`, 'err');
    return;
  }
  const sessionId =
    typeof body.session_id === 'string' && body.session_id ? body.session_id : null;
  if (sessionId && state.retiredSessions.has(sessionId)) {
    // A reset has already replaced this session. A delayed poll must not
    // overwrite the fresh state, time, or controls with the old run's.
    log('ignoring state for a stale session', 'err');
    return;
  }
  if (typeof body.state === 'string') state.simState = body.state.toLowerCase();
  if (sessionId) {
    // A new session is authoritative. Never infer a reset from a small
    // sim_time regression: state polls and time events can arrive out of order.
    if (state.sessionId && sessionId !== state.sessionId) {
      state.retiredSessions.add(state.sessionId);
      resetRunData();
    }
    state.sessionId = sessionId;
  }
  if (body.sim_time !== null && body.sim_time !== undefined && Number.isFinite(Number(body.sim_time))) {
    const nextTime = Number(body.sim_time);
    if (!sessionId && Number.isFinite(state.simTime) && nextTime + 0.001 < state.simTime) {
      // Older daemons have no session id; keep the time-regression fallback.
      resetRunData();
    }
    state.simTime = nextTime;
  }
  if (body.speed !== null && body.speed !== undefined && Number.isFinite(Number(body.speed))) {
    state.speed = Number(body.speed);
  }
  void scheduleSnapshot(body);
  updateControlUi();
  updateTimeUi();
  updateSpeedUi();
}

function applyBackendStatus(data) {
  if (!data || typeof data !== 'object') return;
  state.backend = { ...(state.backend || {}), ...data };
  state.backendConnected = data.connected === true;
  if (data.backend === 'offline' || data.name === 'offline') state.offline = true;
  else if (data.connected === true) state.offline = false;
  renderBackendUi();
  if (state.offline) {
    const note = $('control-note');
    if (note) note.textContent = 'Backend offline. Simulation controls are disabled; result viewing still works.';
  }
  updateControlUi();
}

function applyTime(data) {
  if (data === null || data === undefined) return;
  if (typeof data === 'number') {
    state.simTime = data;
    updateTimeUi();
    return;
  }
  const body = data.data && typeof data.data === 'object' ? data.data : data;
  const t = body.sim_time ?? body.time ?? body.t;
  if (t !== null && t !== undefined && Number.isFinite(Number(t))) {
    state.simTime = Number(t);
    updateTimeUi();
  }
  if (body.speed !== null && body.speed !== undefined && Number.isFinite(Number(body.speed))) {
    state.speed = Number(body.speed);
    updateSpeedUi();
  }
}

function updateControlUi() {
  const s = state.simState;
  const offline = state.offline;
  const enable = {
    start: new Set(['created', 'initialized']),
    pause: new Set(['running']),
    resume: new Set(['paused']),
    stop: new Set(['running', 'paused']),
    reset: new Set(['created', 'initialized', 'running', 'paused', 'completed', 'stopped', 'failed']),
  };
  for (const [cmd, button] of Object.entries({
    start: $('btn-start'),
    pause: $('btn-pause'),
    resume: $('btn-resume'),
    stop: $('btn-stop'),
    reset: $('btn-reset'),
  })) {
    if (button) button.disabled = offline || !enable[cmd].has(s);
  }
  const speedButton = $('btn-set-speed');
  if (speedButton) speedButton.disabled = offline;

  const badge = $('state-badge');
  if (badge) {
    badge.classList.remove('running', 'paused', 'done', 'failed', 'completed', 'stopped');
    if (s === 'running') badge.classList.add('running');
    else if (s === 'paused') badge.classList.add('paused');
    else if (s === 'completed' || s === 'stopped') badge.classList.add('done');
    else if (s === 'failed') badge.classList.add('failed');
    badge.textContent = s === 'unknown' ? 'idle' : s;
  }
  updateTimeUi();
  updateSpeedUi();
}

async function refreshState() {
  if (state.offline || state.stateInFlight) return;
  state.stateInFlight = true;
  try {
    const data = await getJSON('/api/state');
    if (data && data.ok === false) log(`state poll failed: ${errText(data.error)}`, 'err');
    else applyState(data);
  } catch (err) {
    log(`state poll failed: ${errText(err)}`, 'err');
  } finally {
    state.stateInFlight = false;
  }
}

async function sendCommand(cmd, extra) {
  if (state.offline) {
    showToast('Simulation controls are disabled in offline mode.', true);
    return;
  }
  const payload = { cmd, ...(extra || {}) };
  try {
    const data = await postJSON('/api/command', payload);
    if (data && data.ok === false) {
      showToast(`${cmd}: ${errText(data.error)}`, true);
      return;
    }
    log(`cmd ${cmd} ok`, 'ok');
    applyState(data);
    if (data && data.data && typeof data.data === 'object') applyState(data.data);
    void refreshState();
  } catch (err) {
    showToast(`${cmd}: ${errText(err)}`, true);
  }
}

function setSpeedFromInput() {
  const raw = ($('speed-input')?.value || '').trim();
  const speed = Number(raw);
  if (raw === '' || !Number.isFinite(speed) || speed < 0) {
    showToast('Speed must be a non-negative number.', true);
    return;
  }
  void sendCommand('set_speed', { speed });
}

function activeTab() {
  return state.tabs.find((tab) => tab.id === state.activeTabId) || null;
}

function tabSeriesArray(tab) {
  return tab ? Array.from(tab.series.values()) : [];
}

function syncActivePlot() {
  if (!plot) return;
  const tab = activeTab();
  plot.setSeries(tabSeriesArray(tab));
  plot.setFollow(state.follow);
  plot.setNormalize(state.normalize);
  plot.render();
  updateEmptyOverlay();
  const status = $('plot-status');
  if (status) {
    const parts = [`${tab ? tab.series.size : 0} series`];
    if (state.follow) parts.push('follow');
    if (state.normalize) parts.push('normalized');
    status.textContent = parts.join(' · ');
  }
}

function createTab(title) {
  const n = state.tabSeq++;
  const tab = {
    id: `plot-tab-${n}`,
    title: title || `Plot ${n}`,
    series: new Map(),
  };
  state.tabs.push(tab);
  state.activeTabId = tab.id;
  renderTabs();
  syncActivePlot();
  return tab;
}

function renderTabs() {
  const wrap = $('plot-tabs');
  if (!wrap) return;
  wrap.textContent = '';
  for (const tab of state.tabs) {
    const item = el('div', 'tab');
    item.setAttribute('role', 'tab');
    item.tabIndex = 0;
    item.setAttribute('aria-selected', String(tab.id === state.activeTabId));
    if (tab.id === state.activeTabId) item.classList.add('active');
    item.appendChild(el('span', null, tab.title));

    const close = el('span', 'tab-close', '×');
    close.setAttribute('role', 'button');
    close.title = `Close ${tab.title}`;
    close.addEventListener('click', (evt) => {
      evt.stopPropagation();
      closeTab(tab.id);
    });
    item.appendChild(close);
    item.addEventListener('click', () => activateTab(tab.id));
    item.addEventListener('keydown', (evt) => {
      if (evt.key === 'Enter' || evt.key === ' ') {
        evt.preventDefault();
        activateTab(tab.id);
      }
    });
    wrap.appendChild(item);
  }

  const add = el('button', 'tab', '+');
  add.type = 'button';
  add.title = 'New plot';
  add.setAttribute('aria-label', 'New plot');
  add.addEventListener('click', () => {
    createTab();
    log('created plot tab', 'ok');
  });
  wrap.appendChild(add);
}

function activateTab(id) {
  if (!state.tabs.some((tab) => tab.id === id)) return;
  state.activeTabId = id;
  renderTabs();
  syncActivePlot();
}

function closeTab(id) {
  if (state.tabs.length <= 1) {
    showToast('Keep at least one plot tab.', true);
    return;
  }
  const index = state.tabs.findIndex((tab) => tab.id === id);
  if (index < 0) return;
  state.tabs.splice(index, 1);
  if (state.activeTabId === id) {
    const next = state.tabs[Math.max(0, index - 1)] || state.tabs[0];
    state.activeTabId = next.id;
  }
  renderTabs();
  syncActivePlot();
}

function handleLegendRemove(seriesOrKey) {
  const key = typeof seriesOrKey === 'string' ? seriesOrKey : seriesOrKey && seriesOrKey.key;
  const tab = activeTab();
  if (!tab || !key) return;
  if (tab.series.delete(key)) {
    if (tab.id === state.activeTabId && plot) plot.removeSeries(key);
    updateEmptyOverlay();
    log(`removed ${key} from ${tab.title}`, 'ok');
  }
}

function setFollow(on) {
  state.follow = !!on;
  const button = $('btn-follow');
  if (button) button.setAttribute('aria-pressed', String(state.follow));
  if (plot) plot.setFollow(state.follow);
  log(`follow ${state.follow ? 'on' : 'off'}`, 'ok');
  ensureLiveTimer();
  if (state.follow) void pollLive();
}

function setNormalize(on) {
  state.normalize = !!on;
  const button = $('btn-normalize');
  if (button) button.setAttribute('aria-pressed', String(state.normalize));
  if (plot) plot.setNormalize(state.normalize);
  log(`normalize ${state.normalize ? 'on' : 'off'}`, 'ok');
}

function clearPlot() {
  const tab = activeTab();
  if (!tab) return;
  tab.series.clear();
  syncActivePlot();
  log(`cleared ${tab.title}`, 'ok');
}

function clearSelection() {
  for (const entry of state.runs.values()) entry.selected.clear();
  renderSignalTree();
  updateSignalCount();
  renderValues([]);
  void refreshValues();
  log('cleared signal selection', 'ok');
}

function plotSelected() {
  addSelectedToActiveTab();
}

function renderRunsList() {
  const wrap = $('runs-list');
  if (!wrap) return;
  wrap.textContent = '';
  const key = (info) => {
    const t = info.time_end ?? info.time_start;
    return Number.isFinite(Number(t)) ? Number(t) : -Infinity;
  };
  const runs = [...state.discovered].sort(
    (a, b) =>
      key(b) - key(a) ||
      String(a.label || a.run_id).localeCompare(String(b.label || b.run_id)),
  );
  for (const info of runs) {
    const row = el('div', 'run');
    if (state.selectedRunId === info.run_id) row.classList.add('selected');

    const left = el('div', 'run-title');
    const head = el('div');
    head.appendChild(document.createTextNode(info.label || info.run_id));
    const opened = state.runs.get(info.run_id);
    // The daemon's done snapshot is authoritative; an open finished run must
    // not keep the mtime-based LIVE marker from the discovery scan.
    if (info.live && !(opened && opened.finished)) {
      const live = el('span', 'badge running', 'LIVE');
      live.style.marginLeft = '6px';
      head.appendChild(live);
    }
    left.appendChild(head);
    left.appendChild(
      el(
        'div',
        'run-meta',
        `${info.kind || 'run'} · ${fmtCount(info.row_count)} rows · ${fmtRange(info.time_start, info.time_end)}`,
      ),
    );

    const open = el('button', 'btn ghost small', 'Open');
    open.type = 'button';
    open.addEventListener('click', () => {
      if (state.runs.has(info.run_id)) selectRun(info.run_id);
      else void openRunPath(info.path, info);
    });

    row.appendChild(left);
    row.appendChild(open);
    wrap.appendChild(row);
  }
}

function renderOpenRuns() {
  const wrap = $('open-runs');
  if (!wrap) return;
  wrap.textContent = '';
  for (const { info } of state.runs.values()) {
    const row = el('div', 'open-run');
    if (state.selectedRunId === info.run_id) row.classList.add('selected');

    const title = el('div', 'open-run-title', `${info.label || info.run_id} — ${info.path}`);
    title.title = info.path;
    const meta = el('div', 'open-run-meta', `${info.kind || 'run'}${info.live ? ' · live' : ''}`);
    const close = el('button', 'btn ghost small', '×');
    close.type = 'button';
    close.title = `Close ${info.label || info.run_id}`;
    close.addEventListener('click', (evt) => {
      evt.stopPropagation();
      void closeRun(info.run_id);
    });

    row.appendChild(title);
    row.appendChild(meta);
    row.appendChild(close);
    row.addEventListener('click', () => selectRun(info.run_id));
    wrap.appendChild(row);
  }
}

function selectRun(runId) {
  if (!state.runs.has(runId)) return;
  state.selectedRunId = runId;
  renderRunsList();
  renderOpenRuns();
}

const OVERRIDE_STORAGE_KEY = 'ssp4sim.overrides.v1';

function overrideKey(runId, signal) {
  return `${runId}\u0000${signal}`;
}

function loadOverrides() {
  let raw = null;
  try {
    raw = localStorage.getItem(OVERRIDE_STORAGE_KEY);
  } catch (err) {
    log(`override load failed: ${errText(err)}`, 'err');
    return;
  }
  if (!raw) return;
  try {
    const parsed = JSON.parse(raw);
    for (const [key, value] of Object.entries(parsed || {})) {
      if (!value || typeof value !== 'object') continue;
      const k = Number(value.k);
      const m = Number(value.m);
      if (!Number.isFinite(k) || !Number.isFinite(m)) continue;
      state.overrides.set(key, { k, m, enabled: value.enabled === true });
    }
  } catch (err) {
    log(`override parse failed: ${errText(err)}`, 'err');
  }
}

function saveOverrides() {
  try {
    const out = {};
    for (const [key, value] of state.overrides.entries()) out[key] = value;
    localStorage.setItem(OVERRIDE_STORAGE_KEY, JSON.stringify(out));
  } catch (err) {
    log(`override save failed: ${errText(err)}`, 'err');
  }
}

function overrideRecord(runId, signal) {
  return state.overrides.get(overrideKey(runId, signal)) || null;
}

function overrideView(runId, signal) {
  const record = overrideRecord(runId, signal);
  if (!record || !record.enabled) return null;
  return { k: record.k, m: record.m, enabled: true };
}

function isOverridden(runId, signal) {
  const record = overrideRecord(runId, signal);
  return Boolean(record && record.enabled);
}

function overrideDisplayValue(runId, signal, value) {
  if (typeof value !== 'number' || !isOverridden(runId, signal)) return value;
  const record = overrideRecord(runId, signal);
  const k = Number(record.k);
  const m = Number(record.m);
  return Number.isFinite(k) && Number.isFinite(m) ? k * value + m : value;
}

function applyOverridesToSeries() {
  for (const tab of state.tabs) {
    for (const series of tab.series.values()) {
      series.override = overrideView(series.runId, series.signal);
    }
  }
  if (plot) {
    // Rebuild the legend marker without resetting the user's zoom/pan.
    plot.refreshLegend();
    plot.render();
  }
}

function signalPath(signal) {
  if (Array.isArray(signal.path) && signal.path.length) {
    return signal.path.map((part) => String(part));
  }
  const full = String(signal.name || signal.label || 'signal');
  const parts = full.split('.').filter(Boolean);
  return parts.length ? parts : [full];
}

function treeKey(...parts) {
  return parts.join('\u0001');
}

function buildTree(entry) {
  const models = new Map();
  for (const signal of entry.info.signals || []) {
    const path = signalPath(signal);
    const leaf = path[path.length - 1] || signal.name;
    const model = path.length > 1 ? path[0] : 'signals';
    const storage =
      path.length > 2
        ? path.slice(1, -1).join('.')
        : path.length === 2
          ? model
          : 'signals';
    if (!models.has(model)) models.set(model, new Map());
    const storages = models.get(model);
    if (!storages.has(storage)) storages.set(storage, []);
    storages.get(storage).push({ signal, leaf, full: path.join('.') });
  }
  return models;
}

function signalMatches(entry, signal, filter) {
  if (state.selectedOnly && !entry.selected.has(signal.name)) return false;
  if (!filter) return true;
  const haystack = [
    signal.name,
    signal.label,
    signal.group,
    signal.kind,
    entry.info.label,
    (signal.path || []).join(' '),
  ]
    .filter(Boolean)
    .join(' ')
    .toLowerCase();
  return haystack.includes(filter);
}

function toggleTree(key) {
  if (state.treeExpanded.has(key)) state.treeExpanded.delete(key);
  else state.treeExpanded.add(key);
  renderSignalTree();
}

function expandAllTree() {
  for (const entry of state.runs.values()) {
    const runId = entry.info.run_id;
    state.treeExpanded.add(treeKey('run', runId));
    for (const [model, storages] of buildTree(entry)) {
      state.treeExpanded.add(treeKey('model', runId, model));
      for (const storage of storages.keys()) {
        state.treeExpanded.add(treeKey('storage', runId, model, storage));
      }
    }
  }
  renderSignalTree();
}

function collapseAllTree() {
  state.treeExpanded.clear();
  for (const entry of state.runs.values()) {
    state.treeExpanded.add(treeKey('run', entry.info.run_id));
  }
  state.treeLimits.clear();
  renderSignalTree();
}

function addSignalToActiveTab(info, signal) {
  const tab = activeTab();
  if (!tab) return false;
  const key = `${info.run_id}:${signal.name}`;
  if (tab.series.has(key)) return false;
  tab.series.set(key, {
    key,
    runId: info.run_id,
    signal: signal.name,
    label: signal.label || signal.name,
    runLabel: info.label || info.run_id,
    color: seriesColor(key),
    t: [],
    y: [],
    visible: true,
    override: overrideView(info.run_id, signal.name),
  });
  return true;
}

function addSelectedToActiveTab() {
  let added = 0;
  for (const entry of state.runs.values()) {
    for (const name of entry.selected) {
      const signal = (entry.info.signals || []).find((item) => item.name === name);
      if (!signal || signal.plottable === false) continue;
      if (addSignalToActiveTab(entry.info, signal)) added += 1;
    }
  }
  if (added > 0) {
    syncActivePlot();
    ensureLiveTimer();
    void pollLive(true);
    log(`plotted ${added} signal(s) in ${activeTab()?.title || 'plot'}`, 'ok');
  } else {
    log('nothing new to plot', 'ok');
  }
}

function renderSignalLeaf(entry, signal, leaf) {
  const row = el('label', 'tree-signal');
  row.title = signal.name;
  if (signal.plottable === false) row.classList.add('value-only');

  const box = document.createElement('input');
  box.type = 'checkbox';
  box.checked = entry.selected.has(signal.name);
  box.addEventListener('change', () => {
    if (box.checked) {
      entry.selected.add(signal.name);
      if (signal.plottable !== false) {
        const added = addSignalToActiveTab(entry.info, signal);
        if (added) {
          syncActivePlot();
          ensureLiveTimer();
          void pollLive(true);
        }
      }
    } else {
      entry.selected.delete(signal.name);
    }
    updateSignalCount();
    void refreshValues();
    if (state.selectedOnly) renderSignalTree();
  });

  row.appendChild(box);
  row.appendChild(el('span', 'tree-signal-name', leaf));
  if (signal.plottable === false) row.appendChild(el('span', 'tree-meta', 'value only'));
  return row;
}

function renderSignalTree() {
  const tree = $('signal-tree');
  if (!tree) return;
  tree.textContent = '';
  const filter = ($('signal-filter')?.value || '').trim().toLowerCase();
  const autoExpand = filter.length > 0;
  let total = 0;

  for (const entry of state.runs.values()) {
    const runId = entry.info.run_id;
    const models = buildTree(entry);
    let runMatches = 0;
    for (const storages of models.values()) {
      for (const items of storages.values()) {
        for (const item of items) {
          if (signalMatches(entry, item.signal, filter)) runMatches += 1;
        }
      }
    }
    if (runMatches === 0) continue;
    total += runMatches;

    const runKey = treeKey('run', runId);
    const runOpen = autoExpand || state.treeExpanded.has(runKey);
    const runEl = el('div', 'tree-run');

    const runHead = document.createElement('button');
    runHead.type = 'button';
    runHead.className = 'tree-head';
    runHead.appendChild(el('span', 'tree-toggle', runOpen ? '▾' : '▸'));
    runHead.appendChild(el('span', 'tree-label', entry.info.label || runId));
    runHead.appendChild(el('span', 'tree-count', String(runMatches)));
    runHead.addEventListener('click', () => toggleTree(runKey));
    runEl.appendChild(runHead);

    if (runOpen) {
      const runChildren = el('div', 'tree-children');
      for (const [model, storages] of models) {
        let modelMatches = 0;
        for (const items of storages.values()) {
          for (const item of items) {
            if (signalMatches(entry, item.signal, filter)) modelMatches += 1;
          }
        }
        if (modelMatches === 0) continue;

        const modelKey = treeKey('model', runId, model);
        const modelOpen = autoExpand || state.treeExpanded.has(modelKey);
        const modelEl = el('div', 'tree-model');
        const modelHead = document.createElement('button');
        modelHead.type = 'button';
        modelHead.className = 'tree-head';
        modelHead.appendChild(el('span', 'tree-toggle', modelOpen ? '▾' : '▸'));
        modelHead.appendChild(el('span', 'tree-label', model));
        modelHead.appendChild(el('span', 'tree-count', String(modelMatches)));
        modelHead.addEventListener('click', () => toggleTree(modelKey));
        modelEl.appendChild(modelHead);

        if (modelOpen) {
          const modelChildren = el('div', 'tree-children');
          for (const [storage, items] of storages) {
            const visible = items.filter((item) =>
              signalMatches(entry, item.signal, filter),
            );
            if (visible.length === 0) continue;

            const storageKey = treeKey('storage', runId, model, storage);
            const storageOpen = autoExpand || state.treeExpanded.has(storageKey);
            const storageEl = el('div', 'tree-storage');
            const storageHead = document.createElement('button');
            storageHead.type = 'button';
            storageHead.className = 'tree-head';
            storageHead.appendChild(
              el('span', 'tree-toggle', storageOpen ? '▾' : '▸'),
            );
            storageHead.appendChild(el('span', 'tree-label', storage));
            storageHead.appendChild(el('span', 'tree-count', String(visible.length)));
            storageHead.addEventListener('click', () => toggleTree(storageKey));
            storageEl.appendChild(storageHead);

            if (storageOpen) {
              const storageChildren = el('div', 'tree-children');
              const limit = state.treeLimits.get(storageKey) || 200;
              for (const item of visible.slice(0, limit)) {
                storageChildren.appendChild(
                  renderSignalLeaf(entry, item.signal, item.leaf),
                );
              }
              if (visible.length > limit) {
                const more = el(
                  'button',
                  'tree-more',
                  `Show ${Math.min(500, visible.length - limit)} more`,
                );
                more.type = 'button';
                more.addEventListener('click', () => {
                  state.treeLimits.set(storageKey, limit + 500);
                  renderSignalTree();
                });
                storageChildren.appendChild(more);
              }
              storageEl.appendChild(storageChildren);
            }
            modelChildren.appendChild(storageEl);
          }
          modelEl.appendChild(modelChildren);
        }
        runChildren.appendChild(modelEl);
      }
      runEl.appendChild(runChildren);
    }
    tree.appendChild(runEl);
  }

  if (total === 0) {
    tree.appendChild(
      el(
        'div',
        'tree-empty',
        state.runs.size ? 'No matching signals.' : 'Open a run to list signals.',
      ),
    );
  }
  updateSignalCount();
}

async function refreshRuns() {
  if (state.runsInFlight) return;
  state.runsInFlight = true;
  try {
    const data = await getJSON('/api/runs?include_signals=false');
    if (data && data.ok === false) {
      log(`runs poll failed: ${errText(data.error)}`, 'err');
      return;
    }
    state.discovered = Array.isArray(data?.runs) ? data.runs : [];
    for (const run of state.discovered) {
      if (state.finishedRunIds.has(run.run_id)) run.live = false;
    }
    const fresh = new Map(state.discovered.map((run) => [run.run_id, run]));
    for (const entry of state.runs.values()) {
      const update = fresh.get(entry.info.run_id);
      if (!update) continue;
      // A finished run stays finished even while its CSV/WAL mtime is recent.
      entry.info.live = entry.finished ? false : Boolean(update.live);
      entry.info.row_count = update.row_count;
      entry.info.time_end = update.time_end;
    }
    renderRunsList();
    renderOpenRuns();
    ensureLiveTimer();
  } catch (err) {
    log(`runs poll failed: ${errText(err)}`, 'err');
  } finally {
    state.runsInFlight = false;
  }
}

async function openRunPath(path, fallbackInfo) {
  const trimmed = String(path || '').trim();
  if (!trimmed) {
    showToast('Enter a results path first.', true);
    return;
  }
  const existing = [...state.runs.values()].find(
    (entry) => entry.info.path === trimmed || (fallbackInfo && entry.info.run_id === fallbackInfo.run_id),
  );
  if (existing) {
    selectRun(existing.info.run_id);
    return;
  }
  try {
    const data = await postJSON('/api/runs/open', { path: trimmed });
    if (data && data.ok === false) {
      showToast(`open: ${errText(data.error)}`, true);
      return;
    }
    const info = data && data.run;
    if (!info) throw new Error('server did not return run info');
    addRun(info);
    selectRun(info.run_id);
    log(`opened run ${info.label || info.run_id}`, 'ok');
    void refreshRuns();
  } catch (err) {
    showToast(`open: ${errText(err)}`, true);
  }
}

function addRun(info) {
  if (state.runs.has(info.run_id)) {
    selectRun(info.run_id);
    return;
  }
  state.runs.set(info.run_id, {
    info,
    selected: new Set(),
    cursors: new Map(),
    finished: false,
  });
  state.treeExpanded.add(treeKey('run', info.run_id));
  const models = buildTree(state.runs.get(info.run_id));
  const firstModel = models.keys().next().value;
  if (firstModel) {
    state.treeExpanded.add(treeKey('model', info.run_id, firstModel));
    const firstStorage = models.get(firstModel).keys().next().value;
    if (firstStorage) {
      state.treeExpanded.add(treeKey('storage', info.run_id, firstModel, firstStorage));
    }
  }
  renderOpenRuns();
  renderRunsList();
  renderSignalTree();
  updateSignalCount();
  ensureLiveTimer();
  void refreshValues();
}

async function closeRun(runId) {
  try {
    const data = await deleteJSON(`/api/runs/${encodeURIComponent(runId)}`);
    if (data && data.ok === false) {
      showToast(`close: ${errText(data.error)}`, true);
      return;
    }
  } catch (err) {
    showToast(`close: ${errText(err)}`, true);
    return;
  }

  const closing = state.runs.get(runId);
  if (closing && closing.autoSession) state.snapshotClosed.add(closing.autoSession);
  state.runs.delete(runId);
  if (state.selectedRunId === runId) state.selectedRunId = null;

  let activeChanged = false;
  for (const tab of state.tabs) {
    for (const [key, series] of Array.from(tab.series.entries())) {
      if (series.runId === runId) {
        tab.series.delete(key);
        if (tab.id === state.activeTabId) activeChanged = true;
      }
    }
  }
  if (activeChanged) syncActivePlot();
  else updateEmptyOverlay();

  renderOpenRuns();
  renderRunsList();
  renderSignalTree();
  updateSignalCount();
  void refreshValues();
  ensureLiveTimer();
  log(`closed run ${runId}`, 'ok');
}

async function refreshValues() {
  if (state.valuesInFlight) {
    state.valuesPending = true;
    return;
  }
  state.valuesInFlight = true;
  const rows = [];
  try {
    for (const [runId, entry] of state.runs.entries()) {
      const signals = Array.from(entry.selected);
      if (signals.length === 0) continue;
      try {
        const data = await getJSON(
          query(`/api/runs/${encodeURIComponent(runId)}/latest`, { signals }),
        );
        if (data && data.ok === false) {
          log(`values ${runId} failed: ${errText(data.error)}`, 'err');
          continue;
        }
        for (const value of data?.values || []) {
          rows.push({
            runId,
            runLabel: entry.info.label || runId,
            signal: value.signal,
            label: value.label || value.signal || '',
            value: value.value,
            kind: value.kind,
            time: value.time,
          });
        }
      } catch (err) {
        log(`values ${runId} failed: ${errText(err)}`, 'err');
      }
    }
    renderValues(rows);
  } catch (err) {
    log(`values refresh failed: ${errText(err)}`, 'err');
  } finally {
    state.valuesInFlight = false;
    if (state.valuesPending) {
      state.valuesPending = false;
      void refreshValues();
    }
  }
}

function createValueRow(row) {
  const tr = document.createElement('tr');
  const key = overrideKey(row.runId, row.signal);
  tr.dataset.key = key;

  const runCell = el('td', 'value-run', row.runLabel);
  const signalCell = el('td', 'value-signal', row.label);
  signalCell.title = row.signal;
  const valueCell = el('td', 'value-cell');
  const timeCell = el('td', 'value-time');
  const overrideCell = el('td', 'override-cell');

  let enabled = null;
  let kInput = null;
  let mInput = null;
  const numeric = row.kind !== 'string';

  if (numeric) {
    enabled = document.createElement('input');
    enabled.type = 'checkbox';
    enabled.className = 'override-enabled';
    enabled.title = 'Enable y = kx + m';

    kInput = document.createElement('input');
    kInput.type = 'number';
    kInput.step = '0.1';
    kInput.className = 'override-input override-k';
    kInput.placeholder = 'k';
    kInput.setAttribute('aria-label', `k for ${row.label || row.signal}`);

    mInput = document.createElement('input');
    mInput.type = 'number';
    mInput.step = '0.1';
    mInput.className = 'override-input override-m';
    mInput.placeholder = 'm';
    mInput.setAttribute('aria-label', `m for ${row.label || row.signal}`);

    const saved = overrideRecord(row.runId, row.signal);
    enabled.checked = saved ? saved.enabled === true : false;
    kInput.value = saved ? String(saved.k) : '1';
    mInput.value = saved ? String(saved.m) : '0';

    const formula = (text) => el('span', 'override-formula', text);
    overrideCell.append(
      enabled,
      formula('y ='),
      kInput,
      formula('×'),
      formula('x'),
      formula('+'),
      mInput,
    );
  } else {
    overrideCell.textContent = '—';
  }

  tr.append(runCell, signalCell, valueCell, overrideCell, timeCell);

  const rowState = {
    tr,
    valueCell,
    timeCell,
    kInput,
    mInput,
    enabled,
    row,
    lastText: null,
  };

  if (numeric) {
    const commit = () => {
      const k = Number(kInput.value);
      const m = Number(mInput.value);
      if (!Number.isFinite(k) || !Number.isFinite(m)) return;
      const overrideKeyValue = overrideKey(rowState.row.runId, rowState.row.signal);
      if (!enabled.checked && k === 1 && m === 0) {
        state.overrides.delete(overrideKeyValue);
      } else {
        // Viewer-side today. When the daemon gains set_override this record is
        // the payload source; the simulation data itself is never mutated here.
        state.overrides.set(overrideKeyValue, { k, m, enabled: enabled.checked });
      }
      saveOverrides();
      applyOverridesToSeries();
      updateValueRow(rowState, rowState.row);
    };
    enabled.addEventListener('change', commit);
    kInput.addEventListener('input', commit);
    mInput.addEventListener('input', commit);
    kInput.addEventListener('blur', () => {
      if (!kInput.value) kInput.value = '1';
      commit();
    });
    mInput.addEventListener('blur', () => {
      if (!mInput.value) mInput.value = '0';
      commit();
    });
  }

  return rowState;
}

function updateValueRow(rowState, row) {
  if (!rowState) return;
  rowState.row = row;
  const overridden = isOverridden(row.runId, row.signal);
  const display = overrideDisplayValue(row.runId, row.signal, row.value);
  const text = fmtNumber(display, row.kind);

  rowState.valueCell.textContent = '';
  rowState.valueCell.appendChild(document.createTextNode(text));
  rowState.valueCell.classList.toggle('overridden', overridden);
  rowState.valueCell.title = overridden
    ? `raw ${fmtNumber(row.value, row.kind)}`
    : '';
  if (overridden) {
    const record = overrideRecord(row.runId, row.signal);
    const badge = el('span', 'override-badge', 'ƒ');
    badge.title = record ? `y = ${record.k}x + ${record.m}` : 'y = kx + m';
    rowState.valueCell.appendChild(document.createTextNode(' '));
    rowState.valueCell.appendChild(badge);
  }

  if (rowState.lastText !== null && rowState.lastText !== text) {
    rowState.valueCell.classList.remove('value-flash');
    void rowState.valueCell.offsetWidth;
    rowState.valueCell.classList.add('value-flash');
    const cell = rowState.valueCell;
    setTimeout(() => cell.classList.remove('value-flash'), 600);
  }
  rowState.lastText = text;
  rowState.timeCell.textContent = fmtTime(row.time);
}

function renderValues(rows) {
  const body = $('values-body');
  if (!body) return;
  const seen = new Set();

  for (const row of rows) {
    const key = overrideKey(row.runId, row.signal);
    seen.add(key);
    let rowState = state.valueRows.get(key);
    if (!rowState) {
      rowState = createValueRow(row);
      state.valueRows.set(key, rowState);
      body.appendChild(rowState.tr);
    }
    updateValueRow(rowState, row);
  }

  for (const [key, rowState] of Array.from(state.valueRows.entries())) {
    if (seen.has(key)) continue;
    rowState.tr.remove();
    state.valueRows.delete(key);
  }

  const count = $('values-count');
  if (count) count.textContent = String(rows.length);
}

function plottedByRun() {
  const map = new Map();
  for (const tab of state.tabs) {
    for (const series of tab.series.values()) {
      if (!map.has(series.runId)) map.set(series.runId, new Set());
      map.get(series.runId).add(series.signal);
    }
  }
  return map;
}

function hasLiveRuns() {
  return [...state.runs.values()].some((entry) => entry.info.live);
}

function hasSelection() {
  return [...state.runs.values()].some((entry) => entry.selected.size > 0);
}

function valuesShouldPoll() {
  return hasSelection() && (state.follow || hasLiveRuns());
}

function shouldRunLive() {
  if (hasLiveRuns()) return true;
  return state.follow && plottedByRun().size > 0;
}

function ensureLiveTimer() {
  if (shouldRunLive()) {
    if (!state.liveTimer) {
      state.liveTimer = setInterval(() => {
        void pollLive().catch((err) => log(`live poll failed: ${errText(err)}`, 'err'));
      }, 1000);
    }
  } else if (state.liveTimer) {
    clearInterval(state.liveTimer);
    state.liveTimer = null;
  }
}

async function pollLive(force) {
  if (state.liveInFlight) {
    if (force) state.liveForcePending = true;
    return;
  }
  state.liveInFlight = true;
  try {
    const plotted = plottedByRun();
    for (const [runId, signals] of plotted.entries()) {
      const entry = state.runs.get(runId);
      if (!entry) continue;
      if (!force && !(entry.info.live || state.follow)) continue;
      if (signals.size === 0) continue;
      try {
        await pollRun(runId, entry, Array.from(signals));
      } catch (err) {
        log(`live poll ${runId} failed: ${errText(err)}`, 'err');
      }
    }
  } catch (err) {
    log(`live poll failed: ${errText(err)}`, 'err');
  } finally {
    state.liveInFlight = false;
    if (state.liveForcePending) {
      state.liveForcePending = false;
      void pollLive(true);
    }
  }
}

async function pollRun(runId, entry, signals) {
  if (!entry.cursors) entry.cursors = new Map();
  const cursors = signals.map((signal) => entry.cursors.get(signal));
  const params = { signals, max_points: 2000 };
  // Only use a lower bound when every requested signal already has one; a newly
  // plotted signal must get its full history once.
  if (
    cursors.length > 0 &&
    cursors.every((value) => typeof value === 'number' && Number.isFinite(value))
  ) {
    params.after = Math.min(...cursors);
  }

  const data = await getJSON(query(`/api/runs/${encodeURIComponent(runId)}/series`, params));
  if (data && data.ok === false) throw new Error(errText(data.error));

  let renderedActive = false;
  for (const series of data?.series || []) {
    const t = Array.isArray(series.t) ? series.t : [];
    const y = Array.isArray(series.y) ? series.y : [];
    const key = series.key || `${runId}:${series.signal}`;
    for (const tab of state.tabs) {
      const target = tab.series.get(key);
      if (!target) continue;
      // The active tab's series objects are shared with TimePlot; mutating them
      // in place plus one render avoids double-appending. Dedupe by time so
      // overlapping requests from mixed cursors stay safe.
      let lastT = target.t.length ? target.t[target.t.length - 1] : -Infinity;
      for (let i = 0; i < t.length; i += 1) {
        if (!(t[i] > lastT)) continue;
        target.t.push(t[i]);
        target.y.push(y[i]);
        lastT = t[i];
        if (tab.id === state.activeTabId) renderedActive = true;
      }
    }
    if (t.length > 0) {
      const last = t[t.length - 1];
      const previous = entry.cursors.get(series.signal);
      if (typeof previous !== 'number' || last > previous) {
        entry.cursors.set(series.signal, last);
      }
    }
  }
  if (renderedActive && plot) plot.render();
}

function normalizeEvent(evt) {
  if (!evt) return {};
  let kind = evt.kind || evt.event || evt.type || null;
  let data = evt.data;
  if (data && typeof data === 'object' && data.kind && (!kind || kind === 'message')) {
    kind = data.kind;
  }
  if (!kind && data && typeof data === 'object') {
    kind = data.kind || null;
    data = data.data ?? data;
  }
  if (
    kind &&
    data &&
    typeof data === 'object' &&
    data.kind === kind &&
    Object.prototype.hasOwnProperty.call(data, 'data')
  ) {
    data = data.data;
  }
  return { kind, data };
}

function summarizeEvent(kind, data) {
  if (data === null || data === undefined) return '';
  if (typeof data !== 'object') return ` ${String(data)}`;
  if (kind === 'time') {
    const t = data.sim_time ?? data.time ?? data.t;
    return t === null || t === undefined ? '' : ` t=${fmtTime(t)}`;
  }
  if (kind === 'state') return data.state ? ` state=${data.state}` : '';
  if (kind === 'done') return data.run_id ? ` run=${data.run_id}` : '';
  if (kind === 'error') return ` ${errText(data.error || data)}`;
  if (kind === 'backend') {
    const name = data.name || data.backend || '?';
    return ` ${name} connected=${data.connected === true}`;
  }
  return '';
}

function onEvent(evt, extra) {
  let kind;
  let data;
  if (extra !== undefined) {
    kind = typeof evt === 'string' ? evt : evt && (evt.kind || evt.event);
    data = extra;
  } else {
    ({ kind, data } = normalizeEvent(evt));
  }
  if (!kind) {
    let text = '';
    try {
      text = JSON.stringify(evt);
    } catch {
      text = String(evt);
    }
    log(`event ${text}`, '');
    return;
  }

  if (kind !== 'time') log(`event ${kind}${summarizeEvent(kind, data)}`, '');
  if (kind === 'backend') applyBackendStatus(data);
  else if (kind === 'state') applyState(data);
  else if (kind === 'time') applyTime(data);
  else if (kind === 'done') void handleDone(data).catch((err) => log(`done handling failed: ${errText(err)}`, 'err'));
  else if (kind === 'error') {
    const message =
      (data && (data.message || (data.error && (data.error.message || data.error.code)))) ||
      'backend error';
    showToast(`Backend error: ${message}`, true);
  }
}

function statusConnected(status) {
  if (status === null || status === undefined) return false;
  if (typeof status === 'boolean') return status;
  if (typeof status === 'string') {
    return ['open', 'connected', 'ok', 'ready'].includes(status.toLowerCase());
  }
  if (typeof status.connected === 'boolean') return status.connected;
  if (typeof status.ok === 'boolean') return status.ok;
  if (status.type === 'open') return true;
  return false;
}

function backendDotClass() {
  if (state.offline) return 'err';
  return state.backendConnected === true ? 'ok' : 'warn';
}

function onSseStatus(status) {
  const connected = statusConnected(status);
  if (state.sseConnected === connected) return;
  state.sseConnected = connected;
  if (connected) {
    setDot(backendDotClass());
    log('SSE connected', 'ok');
  } else {
    setDot(state.offline ? 'err' : 'warn');
    log('SSE disconnected; reconnecting', 'err');
  }
}

async function handleDone(data) {
  const runId = data && (data.run_id || (data.data && data.data.run_id));
  const doneSession =
    data && typeof data.session_id === 'string'
      ? data.session_id
      : data && data.data && typeof data.data.session_id === 'string'
        ? data.data.session_id
        : null;
  if (
    doneSession &&
    (state.retiredSessions.has(doneSession) ||
      (state.sessionId && doneSession !== state.sessionId))
  ) {
    log('ignoring done for a stale session', 'err');
    return;
  }
  log(`simulation done${runId ? ` for ${runId}` : ''}`, 'ok');
  // A late connect or a very short run may only ever see the done snapshot.
  await scheduleSnapshot(data, { finished: true });
  try {
    await pollLive(true);
  } catch (err) {
    log(`final live poll failed: ${errText(err)}`, 'err');
  }
  await refreshValues();

  // The awaits above can cross a reset. Never let an old run's done mark the
  // fresh run (same artifact path, therefore same run id) as finished.
  if (doneSession && state.sessionId && doneSession !== state.sessionId) {
    log('ignoring done for a stale session', 'err');
    return;
  }

  const ids = runId ? (state.runs.has(runId) ? [runId] : []) : Array.from(state.runs.keys());
  for (const id of ids) {
    try {
      const result = await getJSON(`/api/runs/${encodeURIComponent(id)}`);
      if (result && result.run) {
        const entry = state.runs.get(id);
        if (entry) {
          entry.info = result.run;
          // ``done`` is authoritative: a WAL sidecar or a just-touched CSV must
          // not keep the run marked live after the engine reported it finished.
          entry.info.live = false;
          entry.finished = true;
          state.finishedRunIds.add(id);
        }
        log(`run ${id} finished`, 'ok');
      }
    } catch (err) {
      log(`refresh run ${id} failed: ${errText(err)}`, 'err');
    }
  }

  renderOpenRuns();
  renderRunsList();
  renderSignalTree();

  ensureLiveTimer();
  if (!shouldRunLive()) log('no live runs remain; live polling stopped', 'ok');
}

function applyConfig(config, health) {
  const backend = (config && config.backend) || (health && health.backend) || {};
  state.backend = backend;
  state.backendConnected = backend.connected === true;
  state.offline =
    Boolean(config && config.offline) ||
    backend.backend === 'offline' ||
    backend.name === 'offline';

  const version = (config && config.version) ?? (health && health.version);
  const versionNode = $('version-text');
  if (versionNode) {
    versionNode.textContent = version ? (String(version).startsWith('v') ? String(version) : `v${version}`) : 'v—';
  }

  const simulation = config && config.simulation;
  if (
    simulation &&
    simulation.speed !== null &&
    simulation.speed !== undefined &&
    Number.isFinite(Number(simulation.speed))
  ) {
    state.speed = Number(simulation.speed);
    const input = $('speed-input');
    if (input) input.value = String(state.speed);
  }

  renderBackendUi();
  updateControlUi();

  const name = backend.name || backend.backend || 'unknown';
  const note = $('control-note');
  if (note) {
    if (state.offline) {
      note.textContent = 'Backend offline. Simulation controls are disabled; result viewing still works.';
    } else if (state.backendConnected) {
      note.textContent = `Connected to ${name}. Result viewing works independently of the simulation.`;
    } else {
      note.textContent = `Backend ${name} is not connected. Waiting for the daemon; result viewing still works.`;
    }
  }
}

async function loadBootstrap() {
  const results = await Promise.allSettled([
    getJSON('/api/config'),
    getJSON('/api/health'),
  ]);
  const config = results[0].status === 'fulfilled' ? results[0].value : null;
  const health = results[1].status === 'fulfilled' ? results[1].value : null;

  if (!config && !health) {
    state.offline = true;
    state.backendConnected = false;
    renderBackendUi();
    updateControlUi();
    const note = $('control-note');
    if (note) note.textContent = 'Backend unavailable. Simulation controls are disabled; result viewing may still work.';
    showToast('Could not load /api/config or /api/health.', true);
    return;
  }

  applyConfig(config, health);
  if (results[0].status === 'rejected') log(`config failed: ${errText(results[0].reason)}`, 'err');
  if (results[1].status === 'rejected') log(`health failed: ${errText(results[1].reason)}`, 'err');
}

function startEvents() {
  try {
    state.events = connectEvents(onEvent, onSseStatus);
  } catch (err) {
    setDot('warn');
    log(`SSE failed: ${errText(err)}`, 'err');
  }
}

function startTimers() {
  state.stateTimer = setInterval(() => {
    void refreshState();
  }, 2000);
  state.runsTimer = setInterval(() => {
    void refreshRuns();
  }, 2000);
  state.valuesTimer = setInterval(() => {
    if (valuesShouldPoll()) void refreshValues();
  }, 500);
  void refreshState();
}

function observePlot() {
  if (typeof ResizeObserver === 'undefined') return;
  const target = $('plot-wrap');
  if (!target) return;
  state.resizeObserver = new ResizeObserver(() => {
    if (plot) plot.resize();
  });
  state.resizeObserver.observe(target);
}

function teardown() {
  for (const timer of [state.liveTimer, state.valuesTimer, state.stateTimer, state.runsTimer]) {
    if (timer) clearInterval(timer);
  }
  state.liveTimer = null;
  state.valuesTimer = null;
  state.stateTimer = null;
  state.runsTimer = null;
  if (state.toastTimer) clearTimeout(state.toastTimer);
  state.toastTimer = null;
  if (state.resizeObserver) {
    state.resizeObserver.disconnect();
    state.resizeObserver = null;
  }
  if (state.events && typeof state.events.close === 'function') {
    try {
      state.events.close();
    } catch {
      state.events = null;
    }
    state.events = null;
  }
}

function wireUi() {
  $('btn-start')?.addEventListener('click', () => void sendCommand('start'));
  $('btn-pause')?.addEventListener('click', () => void sendCommand('pause'));
  $('btn-resume')?.addEventListener('click', () => void sendCommand('resume'));
  $('btn-stop')?.addEventListener('click', () => void sendCommand('stop'));
  $('btn-reset')?.addEventListener('click', () => void sendCommand('reset'));
  $('btn-set-speed')?.addEventListener('click', setSpeedFromInput);
  $('speed-input')?.addEventListener('keydown', (evt) => {
    if (evt.key === 'Enter') {
      evt.preventDefault();
      setSpeedFromInput();
    }
  });

  $('btn-refresh-runs')?.addEventListener('click', () => void refreshRuns());
  $('btn-open-path')?.addEventListener('click', () => void openRunPath($('path-input')?.value));
  $('path-input')?.addEventListener('keydown', (evt) => {
    if (evt.key === 'Enter') {
      evt.preventDefault();
      void openRunPath($('path-input')?.value);
    }
  });

  $('signal-filter')?.addEventListener('input', () => {
    if (filterTimer) clearTimeout(filterTimer);
    filterTimer = setTimeout(renderSignalTree, 120);
  });
  $('btn-expand-all')?.addEventListener('click', expandAllTree);
  $('btn-collapse-all')?.addEventListener('click', collapseAllTree);
  $('chk-selected-only')?.addEventListener('change', (evt) => {
    state.selectedOnly = evt.target.checked;
    renderSignalTree();
  });
  $('btn-clear-selection')?.addEventListener('click', clearSelection);
  $('btn-plot-selected')?.addEventListener('click', plotSelected);

  $('btn-fit')?.addEventListener('click', () => {
    if (plot) plot.fit();
  });
  $('btn-follow')?.addEventListener('click', () => setFollow(!state.follow));
  $('btn-normalize')?.addEventListener('click', () => setNormalize(!state.normalize));
  $('btn-clear-plot')?.addEventListener('click', clearPlot);
  $('btn-clear-log')?.addEventListener('click', () => {
    const box = $('console-lines');
    if (box) box.textContent = '';
  });

  for (const [id, name] of [
    ['tab-values', 'values'],
    ['tab-console', 'console'],
  ]) {
    const tab = $(id);
    if (!tab) continue;
    tab.addEventListener('click', () => setDockTab(name));
    tab.addEventListener('keydown', (evt) => {
      if (evt.key === 'Enter' || evt.key === ' ') {
        evt.preventDefault();
        setDockTab(name);
      }
    });
  }
  setDockTab('values');
}

async function boot() {
  wireUi();
  loadOverrides();
  updateControlUi();
  plot = new TimePlot($('plot-canvas'), $('plot-tooltip'), $('plot-legend'), {
    onRemove: handleLegendRemove,
    onFollowChange: (on) => {
      state.follow = !!on;
      const button = $('btn-follow');
      if (button) button.setAttribute('aria-pressed', String(state.follow));
      ensureLiveTimer();
    },
  });
  createTab('Plot 1');
  observePlot();
  await loadBootstrap();
  startEvents();
  startTimers();
  void refreshRuns();
  log('GUI ready', 'ok');
}

window.addEventListener('beforeunload', teardown);

boot().catch((err) => {
  log(`startup failed: ${errText(err)}`, 'err');
  showToast(`Startup failed: ${errText(err)}`, true);
});
