const MAX_POINTS = 20000;
const FONT = '10px "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace';

export function seriesColor(key) {
  const text = String(key == null ? '' : key);
  let hash = 2166136261;
  for (let i = 0; i < text.length; i += 1) {
    hash ^= text.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  const hue = (hash >>> 0) % 360;
  return `hsl(${hue}, 65%, 62%)`;
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function isFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function trimZeroes(text) {
  return text.replace(/(\.\d*?[1-9])0+$/, '$1').replace(/\.0*$/, '');
}

function formatSI(value, digits = 4) {
  if (!isFiniteNumber(value)) return '\u2014';
  if (value === 0) return '0';

  const abs = Math.abs(value);
  let scale = 1;
  let suffix = '';

  if (abs >= 1e12) {
    scale = 1e12;
    suffix = 'T';
  } else if (abs >= 1e9) {
    scale = 1e9;
    suffix = 'G';
  } else if (abs >= 1e6) {
    scale = 1e6;
    suffix = 'M';
  } else if (abs >= 1e3) {
    scale = 1e3;
    suffix = 'k';
  } else if (abs >= 1) {
    scale = 1;
  } else if (abs >= 1e-3) {
    scale = 1e-3;
    suffix = 'm';
  } else if (abs >= 1e-6) {
    scale = 1e-6;
    suffix = '\u00b5';
  } else if (abs >= 1e-9) {
    scale = 1e-9;
    suffix = 'n';
  } else if (abs >= 1e-12) {
    scale = 1e-12;
    suffix = 'p';
  } else {
    return value.toExponential(3).replace(/\.?0+e/, 'e');
  }

  return trimZeroes((value / scale).toPrecision(digits)) + suffix;
}

function formatTimeTick(t, useMs) {
  if (!isFiniteNumber(t)) return '';
  if (useMs) {
    const text = trimZeroes((t * 1000).toFixed(3));
    return `${text === '-0' ? '0' : text} ms`;
  }
  let text = t.toFixed(3);
  if (text === '-0.000') text = '0.000';
  return `${text} s`;
}

function formatSeconds(t) {
  if (!isFiniteNumber(t)) return '\u2014';
  let text = t.toFixed(3);
  if (text === '-0.000') text = '0.000';
  return `${text} s`;
}

function niceStep(span, target) {
  if (!isFiniteNumber(span) || span <= 0) return 1;
  const raw = span / Math.max(1, target);
  const power = Math.pow(10, Math.floor(Math.log10(raw)));
  const normalized = raw / power;
  let multiplier = 1;
  if (normalized <= 1) multiplier = 1;
  else if (normalized <= 2) multiplier = 2;
  else if (normalized <= 5) multiplier = 5;
  else multiplier = 10;
  return multiplier * power;
}

export class TimePlot {
  constructor(canvas, tooltipEl, legendEl, options = {}) {
    const { onRemove, onFollowChange } = options || {};

    this.canvas = canvas;
    this.tooltipEl = tooltipEl || null;
    this.legendEl = legendEl || null;
    this.onRemove = typeof onRemove === 'function' ? onRemove : null;
    this.onFollowChange =
      typeof onFollowChange === 'function' ? onFollowChange : null;
    this.ctx =
      canvas && typeof canvas.getContext === 'function'
        ? canvas.getContext('2d')
        : null;

    this.series = [];
    this.byKey = new Map();
    this.follow = false;
    this.normalize = false;
    this.view = null;
    this.hover = null;
    this._drag = null;
    this.width = 0;
    this.height = 0;
    this.dpr = 1;
    this._plot = null;
    this._ro = null;
    this._destroyed = false;

    this._onWheel = this._handleWheel.bind(this);
    this._onPointerDown = this._handlePointerDown.bind(this);
    this._onPointerMove = this._handlePointerMove.bind(this);
    this._onPointerUp = this._handlePointerUp.bind(this);
    this._onPointerLeave = this._handlePointerLeave.bind(this);
    this._onDoubleClick = this._handleDoubleClick.bind(this);

    if (canvas && typeof canvas.addEventListener === 'function') {
      canvas.addEventListener('wheel', this._onWheel, { passive: false });
      canvas.addEventListener('pointerdown', this._onPointerDown);
      canvas.addEventListener('pointermove', this._onPointerMove);
      canvas.addEventListener('pointerup', this._onPointerUp);
      canvas.addEventListener('pointercancel', this._onPointerUp);
      canvas.addEventListener('pointerleave', this._onPointerLeave);
      canvas.addEventListener('dblclick', this._onDoubleClick);
    }

    if (typeof ResizeObserver !== 'undefined' && canvas) {
      this._ro = new ResizeObserver(() => this.resize());
      this._ro.observe(canvas);
    } else if (typeof window !== 'undefined') {
      this._onWindowResize = () => this.resize();
      window.addEventListener('resize', this._onWindowResize);
    }

    this._rebuildLegend();
    this.resize();
  }

  setSeries(list) {
    this.series = [];
    this.byKey = new Map();

    if (Array.isArray(list)) {
      for (const raw of list) {
        if (!raw) continue;
        const series = this._prepare(raw);
        const previous = this.byKey.get(series.key);
        if (previous) {
          const index = this.series.indexOf(previous);
          if (index >= 0) this.series[index] = series;
          else this.series.push(series);
        } else {
          this.series.push(series);
        }
        this.byKey.set(series.key, series);
      }
    }

    this.hover = null;
    this._hideTooltip();
    this._rebuildLegend();
    this.view = null;
    this.fit();
  }

  addSeries(raw) {
    if (!raw) return;
    const series = this._prepare(raw);
    const previous = this.byKey.get(series.key);
    if (previous) {
      const index = this.series.indexOf(previous);
      if (index >= 0) this.series[index] = series;
      else this.series.push(series);
    } else {
      this.series.push(series);
    }
    this.byKey.set(series.key, series);
    this._rebuildLegend();
    if (!this.view) this.fit();
    else this.render();
  }

  removeSeries(key) {
    const series = this.byKey.get(key);
    if (!series) return;
    const index = this.series.indexOf(series);
    if (index >= 0) this.series.splice(index, 1);
    this.byKey.delete(key);
    this.hover = null;
    this._hideTooltip();
    this._rebuildLegend();
    if (!this.series.length) this.view = null;
    this.render();
  }

  append(runId, signal, t, y) {
    const key = `${runId}:${signal}`;
    const series = this.byKey.get(key);
    if (!series || !isFiniteNumber(t)) return;
    this._appendPoint(series, t, y);
    this.render();
  }

  fit() {
    const bounds = this._dataBounds();
    this.view = bounds ? { min: bounds.min, max: bounds.max } : null;
    this.render();
  }

  setFollow(value) {
    this.follow = Boolean(value);
    this.render();
  }

  _userCancelFollow() {
    if (!this.follow) return;
    this.follow = false;
    if (this.onFollowChange) this.onFollowChange(false);
  }

  setNormalize(value) {
    this.normalize = Boolean(value);
    this.render();
  }

  refreshLegend() {
    this._rebuildLegend();
  }

  resize() {
    if (this._destroyed || !this.canvas) return;

    const rect = this.canvas.getBoundingClientRect();
    const width = Math.max(0, rect.width);
    const height = Math.max(0, rect.height);
    const dpr = (typeof window !== 'undefined' && window.devicePixelRatio) || 1;

    this.width = width;
    this.height = height;
    this.dpr = dpr;

    const backingWidth = Math.max(1, Math.round(width * dpr));
    const backingHeight = Math.max(1, Math.round(height * dpr));
    if (this.canvas.width !== backingWidth) this.canvas.width = backingWidth;
    if (this.canvas.height !== backingHeight) this.canvas.height = backingHeight;

    if (this.ctx) this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.render();
  }

  render() {
    if (this._destroyed) return;
    const ctx = this.ctx;
    if (!ctx) return;

    const width = this.width;
    const height = this.height;
    if (!(width > 0) || !(height > 0)) return;

    const dpr = this.dpr || 1;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = '#0d1117';
    ctx.fillRect(0, 0, width, height);

    let left = Math.min(70, Math.max(40, width * 0.09));
    let right = width - 12;
    let top = 12;
    let bottom = height - 26;
    if (right - left < 30) {
      left = 4;
      right = width - 4;
    }
    if (bottom - top < 30) {
      top = 4;
      bottom = height - 4;
    }

    const plotWidth = right - left;
    const plotHeight = bottom - top;
    if (!(plotWidth > 0) || !(plotHeight > 0)) {
      this._plot = null;
      return;
    }
    const plot = { left, right, top, bottom, width: plotWidth, height: plotHeight };
    this._plot = plot;

    const bounds = this._dataBounds();
    if (!bounds) {
      this.view = null;
      this._drawGrid(ctx, plot, { min: 0, max: 1 }, 0, 1, false);
      return;
    }

    if (!this.view || !(this.view.max > this.view.min)) {
      this.view = { min: bounds.min, max: bounds.max };
    }
    let span = this.view.max - this.view.min;
    if (!(span > 0)) span = bounds.span;

    let viewMin = this.view.min;
    let viewMax = this.view.max;
    if (this.follow) {
      const margin = span * 0.05;
      viewMax = bounds.max + margin;
      viewMin = viewMax - span;
    }
    this.view = this._clampView(viewMin, viewMax, bounds);

    const visible = this.series.filter((series) => series.visible);

    let yMin = 0;
    let yMax = 1;
    const normalRanges = new Map();

    if (this.normalize) {
      for (const series of visible) {
        let low = Infinity;
        let high = -Infinity;
        for (let i = 0; i < series.y.length; i += 1) {
          const value = this._seriesValue(series, i);
          if (isFiniteNumber(value)) {
            if (value < low) low = value;
            if (value > high) high = value;
          }
        }
        normalRanges.set(series.key, low <= high ? { min: low, max: high } : null);
      }
    } else {
      let low = Infinity;
      let high = -Infinity;
      for (const series of visible) {
        for (let i = 0; i < series.y.length; i += 1) {
          const value = this._seriesValue(series, i);
          if (isFiniteNumber(value)) {
            if (value < low) low = value;
            if (value > high) high = value;
          }
        }
      }
      if (low <= high) {
        if (!(high > low)) {
          const pad = Math.max(Math.abs(low) * 0.1, 1);
          yMin = low - pad;
          yMax = high + pad;
        } else {
          const pad = (high - low) * 0.05;
          yMin = low - pad;
          yMax = high + pad;
        }
      }
    }
    if (!(yMax > yMin)) {
      yMin = 0;
      yMax = 1;
    }

    this._drawGrid(ctx, plot, this.view, yMin, yMax, true);

    const xSpan = this.view.max - this.view.min;
    const mapX = (t) => left + ((t - this.view.min) / xSpan) * plotWidth;
    const mapY = (value) => bottom - ((value - yMin) / (yMax - yMin)) * plotHeight;

    if (visible.length) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(left, top, plotWidth, plotHeight);
      ctx.clip();

      const maxPoints = Math.max(2, Math.floor(width * 2));

      for (const series of visible) {
        const count = series.t.length;
        if (!count) continue;

        const last = count - 1;
        const stride = Math.max(1, Math.ceil(last / Math.max(1, maxPoints - 1)));
        const range = normalRanges.get(series.key);

        ctx.beginPath();
        let penDown = false;

        const addPoint = (index) => {
          const t = series.t[index];
          const value = this._seriesValue(series, index);
          if (!isFiniteNumber(t) || !isFiniteNumber(value)) {
            penDown = false;
            return;
          }
          let scaled = value;
          if (this.normalize) {
            scaled =
              range && range.max > range.min
                ? (value - range.min) / (range.max - range.min)
                : 0.5;
          }
          const px = mapX(t);
          const py = mapY(scaled);
          if (penDown) ctx.lineTo(px, py);
          else {
            ctx.moveTo(px, py);
            penDown = true;
          }
        };

        for (let i = 0; i < last; i += stride) addPoint(i);
        addPoint(last);

        ctx.strokeStyle = series.color || seriesColor(series.key);
        ctx.lineWidth = 1.4;
        ctx.lineJoin = 'round';
        ctx.lineCap = 'round';
        ctx.stroke();
      }
      ctx.restore();
    }

    if (this.normalize && visible.length) {
      ctx.save();
      ctx.fillStyle = 'rgba(139, 163, 184, 0.9)';
      ctx.font = FONT;
      ctx.textAlign = 'right';
      ctx.textBaseline = 'top';
      ctx.fillText('normalised per series', right - 6, top + 6);
      ctx.restore();
    }

    if (this.hover && visible.length) {
      const hoverX = mapX(this.hover.t);
      if (hoverX >= left - 1 && hoverX <= right + 1) {
        ctx.save();
        ctx.beginPath();
        ctx.rect(left, top, plotWidth, plotHeight);
        ctx.clip();

        ctx.strokeStyle = 'rgba(215, 226, 238, 0.35)';
        ctx.lineWidth = 1;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        ctx.moveTo(hoverX, top);
        ctx.lineTo(hoverX, bottom);
        ctx.stroke();
        ctx.setLineDash([]);

        for (const series of visible) {
          const index = this._nearestIndex(series.t, this.hover.t);
          if (index < 0) continue;
          const value = this._seriesValue(series, index);
          if (!isFiniteNumber(value)) continue;

          const range = normalRanges.get(series.key);
          let scaled = value;
          if (this.normalize) {
            scaled =
              range && range.max > range.min
                ? (value - range.min) / (range.max - range.min)
                : 0.5;
          }

          const x = mapX(series.t[index]);
          const y = mapY(scaled);
          ctx.beginPath();
          ctx.arc(x, y, 2.5, 0, Math.PI * 2);
          ctx.fillStyle = series.color || seriesColor(series.key);
          ctx.fill();
        }
        ctx.restore();
      }
    }
  }

  destroy() {
    if (this._destroyed) return;
    this._destroyed = true;

    const canvas = this.canvas;
    if (canvas) {
      canvas.removeEventListener('wheel', this._onWheel);
      canvas.removeEventListener('pointerdown', this._onPointerDown);
      canvas.removeEventListener('pointermove', this._onPointerMove);
      canvas.removeEventListener('pointerup', this._onPointerUp);
      canvas.removeEventListener('pointercancel', this._onPointerUp);
      canvas.removeEventListener('pointerleave', this._onPointerLeave);
      canvas.removeEventListener('dblclick', this._onDoubleClick);
    }

    if (this._ro) {
      this._ro.disconnect();
      this._ro = null;
    }
    if (this._onWindowResize && typeof window !== 'undefined') {
      window.removeEventListener('resize', this._onWindowResize);
      this._onWindowResize = null;
    }

    this._drag = null;
    this.hover = null;
    this._hideTooltip();
  }

  _prepare(raw) {
    const series = raw;
    if (!series.key) series.key = `${series.runId}:${series.signal}`;
    if (series.label == null) series.label = series.signal;
    if (series.runLabel == null) series.runLabel = series.runId;
    if (!series.color) series.color = seriesColor(series.key);
    series.visible = series.visible !== false;
    if (!Array.isArray(series.t)) series.t = [];
    if (!Array.isArray(series.y)) series.y = [];

    if (series.t.length > 1) {
      let sorted = true;
      for (let i = 1; i < series.t.length; i += 1) {
        if (series.t[i] < series.t[i - 1]) {
          sorted = false;
          break;
        }
      }
      if (!sorted) {
        const order = series.t
          .map((value, index) => index)
          .sort((a, b) => series.t[a] - series.t[b]);
        series.t = order.map((index) => series.t[index]);
        series.y = order.map((index) => series.y[index]);
      }
    }

    return series;
  }

  _appendPoint(series, t, y) {
    const times = series.t;
    const values = series.y;
    const length = times.length;

    if (length === 0 || t >= times[length - 1]) {
      times.push(t);
      values.push(y);
    } else {
      let low = 0;
      let high = length;
      while (low < high) {
        const middle = (low + high) >> 1;
        if (times[middle] <= t) low = middle + 1;
        else high = middle;
      }
      times.splice(low, 0, t);
      values.splice(low, 0, y);
    }

    if (times.length > MAX_POINTS) {
      const drop = times.length - MAX_POINTS;
      times.splice(0, drop);
      values.splice(0, drop);
    }
  }

  _overrideActive(series) {
    const override = series ? series.override : null;
    return Boolean(override && override.enabled);
  }

  _seriesValue(series, index) {
    const x = series && Array.isArray(series.y) ? series.y[index] : undefined;
    const override = series ? series.override : null;
    if (
      this._overrideActive(series) &&
      isFiniteNumber(override.k) &&
      isFiniteNumber(override.m) &&
      isFiniteNumber(x)
    ) {
      return override.k * x + override.m;
    }
    return x;
  }

  _dataBounds() {
    let min = Infinity;
    let max = -Infinity;

    for (const series of this.series) {
      const times = series.t;
      if (!times.length) continue;
      const first = times[0];
      const last = times[times.length - 1];
      if (isFiniteNumber(first) && first < min) min = first;
      if (isFiniteNumber(last) && last > max) max = last;
    }

    if (!isFiniteNumber(min) || !isFiniteNumber(max)) return null;
    if (max < min) {
      const swap = min;
      min = max;
      max = swap;
    }

    let span = max - min;
    if (!(span > 0)) {
      const pad = Math.max(Math.abs(max) * 0.05, 0.5);
      min -= pad;
      max += pad;
      span = max - min;
    }

    return { min, max, span };
  }

  _clampView(min, max, bounds) {
    if (!(max > min)) {
      const center = (min + max) / 2;
      min = center - 0.5;
      max = center + 0.5;
    }

    const span = max - min;
    const low = bounds.min - bounds.span * 0.1;
    const high = bounds.max + bounds.span * 0.1;

    if (span >= high - low) {
      const center = (low + high) / 2;
      return { min: center - span / 2, max: center + span / 2 };
    }
    if (min < low) return { min: low, max: low + span };
    if (max > high) return { min: high - span, max: high };
    return { min, max };
  }

  _drawGrid(ctx, plot, view, yMin, yMax, labels) {
    const { left, right, top, bottom, width, height } = plot;
    const xSpan = view.max - view.min;
    if (!(xSpan > 0) || !(yMax > yMin)) return;

    const xStep = niceStep(xSpan, Math.max(2, Math.floor(width / 90)));
    const yStep = niceStep(yMax - yMin, Math.max(2, Math.floor(height / 44)));
    const xStart = Math.ceil((view.min - xStep * 1e-9) / xStep) * xStep;
    const yStart = Math.ceil((yMin - yStep * 1e-9) / yStep) * yStep;

    const xTicks = [];
    const yTicks = [];

    for (let i = 0; i < 200; i += 1) {
      const value = xStart + i * xStep;
      if (value > view.max + xStep * 1e-6) break;
      xTicks.push(value);
    }
    for (let i = 0; i < 200; i += 1) {
      const value = yStart + i * yStep;
      if (value > yMax + yStep * 1e-6) break;
      yTicks.push(value);
    }

    ctx.save();
    ctx.strokeStyle = 'rgba(139, 163, 184, 0.22)';
    ctx.lineWidth = 1;
    ctx.setLineDash([1, 4]);
    ctx.beginPath();

    for (const value of xTicks) {
      const x = left + ((value - view.min) / xSpan) * width;
      ctx.moveTo(x, top);
      ctx.lineTo(x, bottom);
    }
    for (const value of yTicks) {
      const y = bottom - ((value - yMin) / (yMax - yMin)) * height;
      ctx.moveTo(left, y);
      ctx.lineTo(right, y);
    }

    ctx.stroke();
    ctx.setLineDash([]);

    if (labels) {
      const useMs = xSpan < 1;
      ctx.fillStyle = '#8ba3b8';
      ctx.font = FONT;

      ctx.textBaseline = 'top';
      ctx.textAlign = 'center';
      for (const value of xTicks) {
        const x = left + ((value - view.min) / xSpan) * width;
        const labelX = clamp(x, left + 18, right - 18);
        ctx.fillText(formatTimeTick(value, useMs), labelX, bottom + 7);
      }

      ctx.textBaseline = 'middle';
      ctx.textAlign = 'right';
      for (const value of yTicks) {
        const y = bottom - ((value - yMin) / (yMax - yMin)) * height;
        ctx.fillText(formatSI(value, 4), left - 8, y);
      }
    }

    ctx.restore();
  }

  _nearestIndex(times, target) {
    if (!times.length) return -1;

    let low = 0;
    let high = times.length - 1;
    while (low <= high) {
      const middle = (low + high) >> 1;
      const value = times[middle];
      if (value === target) return middle;
      if (value < target) low = middle + 1;
      else high = middle - 1;
    }

    if (low >= times.length) return times.length - 1;
    if (high < 0) return 0;
    return Math.abs(times[low] - target) <= Math.abs(times[high] - target)
      ? low
      : high;
  }

  _nearestHoverTime(target) {
    let best = null;
    let bestDistance = Infinity;

    for (const series of this.series) {
      if (!series.visible || !series.t.length) continue;
      const index = this._nearestIndex(series.t, target);
      if (index < 0) continue;
      const time = series.t[index];
      const distance = Math.abs(time - target);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = time;
      }
    }

    return best;
  }

  _collectHoverSamples(target) {
    const samples = [];
    for (const series of this.series) {
      if (!series.visible || !series.t.length) continue;
      const index = this._nearestIndex(series.t, target);
      if (index < 0) continue;
      samples.push({
        series,
        t: series.t[index],
        y: this._seriesValue(series, index),
        raw: series.y[index],
      });
    }
    return samples;
  }

  _showTooltip(event, samples) {
    const element = this.tooltipEl;
    if (!element) return;

    const lines = samples.map(({ series, t, y, raw }) => {
      const runLabel = series.runLabel == null ? series.runId : series.runLabel;
      const label = series.label == null ? series.signal : series.label;
      const rawText =
        this._overrideActive(series) && isFiniteNumber(raw)
          ? ` (raw ${formatSI(raw, 4)})`
          : '';
      return `${runLabel} \u2014 ${label}  ${formatSI(y, 4)}${rawText}  (${formatSeconds(t)})`;
    });

    element.textContent = lines.join('\n');
    element.classList.remove('hidden');

    if (!this.canvas) return;
    const canvasRect = this.canvas.getBoundingClientRect();
    const parent = element.offsetParent || element.parentElement;
    if (!parent) return;

    const parentRect = parent.getBoundingClientRect();
    const baseLeft = canvasRect.left - parentRect.left;
    const baseTop = canvasRect.top - parentRect.top;
    const pointerX = event.clientX - parentRect.left;
    const pointerY = event.clientY - parentRect.top;

    const pad = 6;
    const tooltipWidth = element.offsetWidth;
    const tooltipHeight = element.offsetHeight;
    const minLeft = baseLeft + pad;
    const maxLeft = baseLeft + canvasRect.width - tooltipWidth - pad;
    const minTop = baseTop + pad;
    const maxTop = baseTop + canvasRect.height - tooltipHeight - pad;

    let left = pointerX + 14;
    if (left > maxLeft) left = pointerX - tooltipWidth - 14;
    left = clamp(left, minLeft, Math.max(minLeft, maxLeft));

    let top = pointerY + 14;
    if (top > maxTop) top = pointerY - tooltipHeight - 14;
    top = clamp(top, minTop, Math.max(minTop, maxTop));

    element.style.left = `${left}px`;
    element.style.top = `${top}px`;
  }

  _hideTooltip() {
    if (this.tooltipEl && this.tooltipEl.classList) {
      this.tooltipEl.classList.add('hidden');
    }
  }

  _hideHover() {
    const hadHover = this.hover !== null;
    this.hover = null;
    this._hideTooltip();
    if (hadHover) this.render();
  }

  _updateHover(event) {
    const plot = this._plot;
    if (!plot || !this.view || !this.canvas) {
      this._hideHover();
      return;
    }

    const rect = this.canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    if (x < plot.left || x > plot.right || y < plot.top || y > plot.bottom) {
      this._hideHover();
      return;
    }

    const dataX =
      this.view.min + ((x - plot.left) / plot.width) * (this.view.max - this.view.min);
    const nearest = this._nearestHoverTime(dataX);
    if (!isFiniteNumber(nearest)) {
      this._hideHover();
      return;
    }

    const samples = this._collectHoverSamples(nearest);
    if (!samples.length) {
      this._hideHover();
      return;
    }

    const changed = !this.hover || this.hover.t !== nearest;
    this.hover = { t: nearest };
    this._showTooltip(event, samples);
    if (changed) this.render();
  }

  _handleWheel(event) {
    if (this._destroyed || !this.canvas) return;
    const bounds = this._dataBounds();
    if (!bounds || !this._plot || !this.view) return;

    event.preventDefault();

    const plot = this._plot;
    const rect = this.canvas.getBoundingClientRect();
    const localX = event.clientX - rect.left;
    const fraction = clamp((localX - plot.left) / plot.width, 0, 1);
    const span = this.view.max - this.view.min;
    if (!(span > 0)) return;

    const anchor = this.view.min + fraction * span;
    const delta = isFiniteNumber(event.deltaY) ? event.deltaY : 0;
    const scale = Math.exp(delta * 0.0015);
    const nextSpan = clamp(span * scale, bounds.span * 0.2, bounds.span * 20);
    if (!(nextSpan > 0)) return;

    const min = anchor - fraction * nextSpan;
    this.view = this._clampView(min, min + nextSpan, bounds);
    this._userCancelFollow();
    this.render();
  }

  _handlePointerDown(event) {
    if (this._destroyed || !this.view) return;
    if (event.button != null && event.button !== 0) return;

    const bounds = this._dataBounds();
    if (!bounds || !this._plot) return;

    this._drag = {
      pointerId: event.pointerId,
      startX: event.clientX,
      min: this.view.min,
      span: this.view.max - this.view.min,
    };
    this._userCancelFollow();
    this.hover = null;
    this._hideTooltip();

    if (typeof this.canvas.setPointerCapture === 'function') {
      try {
        this.canvas.setPointerCapture(event.pointerId);
      } catch (error) {
        // Pointer capture is best-effort.
      }
    }
    event.preventDefault();
  }

  _handlePointerMove(event) {
    if (this._destroyed) return;

    if (this._drag && this._drag.pointerId === event.pointerId) {
      const plot = this._plot;
      const bounds = this._dataBounds();
      if (!plot || !bounds) return;

      const shift = ((event.clientX - this._drag.startX) / plot.width) * this._drag.span;
      const min = this._drag.min - shift;
      this.view = this._clampView(min, min + this._drag.span, bounds);
      this.hover = null;
      this._hideTooltip();
      this.render();
      return;
    }

    this._updateHover(event);
  }

  _handlePointerUp(event) {
    if (!this._drag || this._drag.pointerId !== event.pointerId) return;

    if (this.canvas && typeof this.canvas.releasePointerCapture === 'function') {
      try {
        this.canvas.releasePointerCapture(event.pointerId);
      } catch (error) {
        // Pointer capture may already have been released.
      }
    }
    this._drag = null;
    this._updateHover(event);
  }

  _handlePointerLeave() {
    if (!this._drag) this._hideHover();
  }

  _handleDoubleClick(event) {
    event.preventDefault();
    this._userCancelFollow();
    this.fit();
  }

  _rebuildLegend() {
    const legend = this.legendEl;
    if (!legend) return;

    legend.textContent = '';

    for (const series of this.series) {
      const runLabel = series.runLabel == null ? series.runId : series.runLabel;
      const label = series.label == null ? series.signal : series.label;
      const text = `${runLabel} \u2014 ${label}`;

      const row = document.createElement('div');
      row.className = 'legend-item';
      row.dataset.key = series.key;
      row.tabIndex = 0;
      row.title = text;
      row.setAttribute('role', 'button');
      row.setAttribute('aria-pressed', String(series.visible));

      const swatch = document.createElement('span');
      swatch.className = 'legend-swatch';
      swatch.style.backgroundColor = series.color || seriesColor(series.key);
      swatch.setAttribute('aria-hidden', 'true');

      const name = document.createElement('span');
      name.textContent = text;
      name.style.maxWidth = '220px';
      name.style.overflow = 'hidden';
      name.style.textOverflow = 'ellipsis';
      name.style.whiteSpace = 'nowrap';

      let overrideMarker = null;
      if (this._overrideActive(series)) {
        const override = series.override;
        const k = isFiniteNumber(override.k) ? override.k : null;
        const m = isFiniteNumber(override.m) ? override.m : null;
        overrideMarker = document.createElement('span');
        overrideMarker.className = 'legend-override';
        overrideMarker.textContent = '\u0192';
        overrideMarker.title =
          k == null || m == null ? 'y = k x + m' : `y = ${k} x + ${m}`;
        overrideMarker.setAttribute('aria-label', overrideMarker.title);
        overrideMarker.style.fontStyle = 'italic';
        overrideMarker.style.opacity = '0.75';
        overrideMarker.style.marginRight = '4px';
      }

      const toggle = document.createElement('input');
      toggle.type = 'checkbox';
      toggle.checked = Boolean(series.visible);
      toggle.title = 'Toggle visibility';
      toggle.setAttribute('aria-label', `Toggle ${text}`);
      toggle.style.width = '11px';
      toggle.style.height = '11px';
      toggle.style.margin = '0';
      toggle.style.accentColor = series.color || seriesColor(series.key);

      const remove = document.createElement('button');
      remove.type = 'button';
      remove.textContent = '\u00d7';
      remove.title = 'Remove';
      remove.setAttribute('aria-label', `Remove ${text}`);
      remove.style.border = '0';
      remove.style.background = 'transparent';
      remove.style.color = '#8ba3b8';
      remove.style.cursor = 'pointer';
      remove.style.padding = '0 2px';
      remove.style.font = 'inherit';
      remove.style.lineHeight = '1';

      const applyVisibility = () => {
        toggle.checked = Boolean(series.visible);
        swatch.style.opacity = series.visible ? '1' : '0.3';
        name.style.opacity = series.visible ? '1' : '0.5';
        row.setAttribute('aria-pressed', String(series.visible));
      };

      const refresh = () => {
        this.hover = null;
        this._hideTooltip();
        this.render();
      };

      row.addEventListener('click', (event) => {
        if (event.target === toggle || event.target === remove) return;
        series.visible = !series.visible;
        applyVisibility();
        refresh();
      });

      row.addEventListener('keydown', (event) => {
        if (event.target !== row) return;
        if (event.key !== 'Enter' && event.key !== ' ') return;
        event.preventDefault();
        series.visible = !series.visible;
        applyVisibility();
        refresh();
      });

      toggle.addEventListener('click', (event) => event.stopPropagation());
      toggle.addEventListener('change', () => {
        series.visible = toggle.checked;
        applyVisibility();
        refresh();
      });

      remove.addEventListener('click', (event) => {
        event.stopPropagation();
        if (this.onRemove) this.onRemove(series.key);
        else this.removeSeries(series.key);
      });

      applyVisibility();
      row.appendChild(swatch);
      row.appendChild(name);
      if (overrideMarker) row.appendChild(overrideMarker);
      row.appendChild(toggle);
      row.appendChild(remove);
      legend.appendChild(row);
    }
  }
}
