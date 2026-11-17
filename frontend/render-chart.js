// Grammar charts (marks on axes) for NB.chart. The extension has already aggregated and downsampled
// the data; this file maps the rows it sends to SVG. Typed charts live in render-typed.js.
(function (NB) {
  'use strict';
  var SVG = 'http://www.w3.org/2000/svg';
  var M = { top: 12, right: 16, bottom: 42, left: 58 };

  function s(tag, attrs, parent) {
    var e = document.createElementNS(SVG, tag);
    for (var k in attrs || {}) if (attrs[k] != null) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  }
  NB.svg = s;

  NB.objects = function (dataset) {
    if (!dataset || !dataset.columns) return [];
    var names = dataset.columns.map(function (c) { return c.name; });
    return (dataset.rows || []).map(function (r) {
      var o = {};
      names.forEach(function (n, k) { o[n] = r[k]; });
      return o;
    });
  };

  // ---- Scales -------------------------------------------------------------------------------

  function niceStep(span, count) {
    var raw = span / Math.max(1, count), p = Math.pow(10, Math.floor(Math.log10(raw || 1))), m = raw / p;
    return p * (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10);
  }

  function linear(domain, range, nice) {
    var d0 = domain[0], d1 = domain[1];
    if (d0 === d1) { d0 -= 1; d1 += 1; }
    if (nice) { var st = niceStep(d1 - d0, 6); d0 = Math.floor(d0 / st) * st; d1 = Math.ceil(d1 / st) * st; }
    var f = function (v) { return range[0] + (v - d0) / (d1 - d0) * (range[1] - range[0]); };
    f.invert = function (p) { return d0 + (p - range[0]) / (range[1] - range[0]) * (d1 - d0); };
    f.ticks = function (n) {
      var st = niceStep(d1 - d0, n || 6), out = [];
      for (var v = Math.ceil(d0 / st) * st; v <= d1 + st / 1e6; v += st) out.push(+v.toPrecision(12));
      return out;
    };
    f.domain = [d0, d1];
    f.kind = 'linear';
    return f;
  }

  var TIME_STEPS = [[1e3, 'time'], [6e4, 'time'], [3.6e6, 'time'], [8.64e7, 'day'], [6.048e8, 'day'],
    [2.592e9, 'month'], [7.776e9, 'month'], [3.1536e10, 'year']];

  function time(domain, range) {
    var f = linear(domain, range, false);
    var ticks = f.ticks;
    f.kind = 'time';
    f.ticks = function (n) {
      var span = f.domain[1] - f.domain[0], unit = 'year';
      for (var i = 0; i < TIME_STEPS.length; i++) if (span / TIME_STEPS[i][0] <= (n || 6) * 1.5) { unit = TIME_STEPS[i][1]; break; }
      f.unit = unit;
      if (unit === 'month' || unit === 'year') {
        var start = new Date(f.domain[0]), out = [], stepMonths = unit === 'year' ? 12 * Math.max(1, Math.ceil(span / 3.1536e10 / (n || 6))) : Math.max(1, Math.ceil(span / 2.592e9 / (n || 6)));
        var d = new Date(start.getFullYear(), unit === 'year' ? 0 : start.getMonth(), 1);
        while (+d <= f.domain[1]) { if (+d >= f.domain[0]) out.push(+d); d = new Date(d.getFullYear(), d.getMonth() + stepMonths, 1); }
        return out;
      }
      return ticks(n);
    };
    f.format = function (v) { return NB.formatDate(v, f.unit); };
    return f;
  }

  function band(domain, range, padding) {
    // Bands always run from the smaller pixel to the larger; y callers reverse the domain instead.
    range = [Math.min(range[0], range[1]), Math.max(range[0], range[1])];
    var n = Math.max(1, domain.length), step = (range[1] - range[0]) / n, pad = padding == null ? 0.15 : padding;
    var index = {};
    domain.forEach(function (d, i) { index[key(d)] = i; });
    var f = function (v) { var i = index[key(v)]; return i == null ? NaN : range[0] + i * step + step * pad / 2; };
    f.bandwidth = step * (1 - pad);
    f.step = step;
    f.domain = domain;
    f.kind = 'band';
    f.ticks = function () { return domain; };
    f.center = function (v) { return f(v) + f.bandwidth / 2; };
    f.invertRange = function (a, b) {
      var lo = Math.min(a, b), hi = Math.max(a, b);
      return domain.filter(function (d) { var c = f(d) + f.bandwidth / 2; return c >= lo && c <= hi; });
    };
    return f;
  }

  function key(v) { return v === null ? '\u0000null' : typeof v === 'object' ? JSON.stringify(v) : String(v); }
  NB.key = key;

  function numeric(v, type) { return type === 'temporal' ? +NB.toDate(v) : Number(v); }

  function distinct(values) {
    var seen = {}, out = [];
    values.forEach(function (v) { var k = key(v); if (!seen[k]) { seen[k] = 1; out.push(v); } });
    return out;
  }

  // ---- Scale construction over all layers -----------------------------------------------------

  function channelValues(layers, channel) {
    var out = [];
    layers.forEach(function (l) {
      var enc = l.encoding[channel];
      if (!enc || enc.value !== undefined) return;
      l.rows.forEach(function (r) {
        out.push(r[enc.column]);
        if (enc.end) out.push(r[enc.end]);
        if (l.mark === 'boxplot' && channel === 'y') ['y_min', 'y_max'].forEach(function (c) { out.push(r[c]); });
      });
      var other = l.encoding[channel + '2'];
      if (other && other.column) l.rows.forEach(function (r) { out.push(r[other.column]); });
    });
    return out;
  }

  function isBand(layers, channel) {
    var enc = null, bars = false;
    layers.forEach(function (l) {
      if (l.encoding[channel] && l.encoding[channel].column) enc = enc || l.encoding[channel];
      if (l.mark === 'bar' || l.mark === 'boxplot' || l.mark === 'rect') bars = true;
    });
    if (!enc) return false;
    if (enc.type === 'nominal' || enc.type === 'ordinal') return true;
    // Bars over an unbinned number or date read better on a band scale.
    return bars && !enc.bin && enc.type !== 'temporal' && channel === 'x' && !enc.agg;
  }

  function makeScale(layers, channel, range, zero, extra) {
    var enc = null;
    layers.forEach(function (l) { var e = l.encoding[channel]; if (e && e.column && !enc) enc = e; });
    if (!enc) return null;
    var values = channelValues(layers, channel).filter(function (v) { return v !== null && v !== undefined; });
    if (isBand(layers, channel)) {
      var domain = distinct(values);
      if (enc.type === 'ordinal' && domain.every(function (v) { return typeof v === 'number'; })) domain.sort(function (a, b) { return a - b; });
      if (channel === 'y') domain.reverse();
      var b = band(domain, range);
      b.enc = enc;
      return b;
    }
    // Rules and bands are part of the picture: a target above every bar still has to fit.
    var nums = values.concat(extra || []).map(function (v) { return numeric(v, enc.type); }).filter(isFinite);
    var lo = nums.length ? Math.min.apply(null, nums) : 0, hi = nums.length ? Math.max.apply(null, nums) : 1;
    var scaleOpt = enc.scale || {};
    if (scaleOpt.domain && scaleOpt.domain.length === 2) { lo = numeric(scaleOpt.domain[0], enc.type); hi = numeric(scaleOpt.domain[1], enc.type); }
    // Bars and areas start at zero unless zero: false; other marks only with zero: true.
    else if ((zero || scaleOpt.zero === true) && scaleOpt.zero !== false && enc.type !== 'temporal') { lo = Math.min(0, lo); hi = Math.max(0, hi); }
    var f;
    if (enc.type === 'temporal') f = time([lo, hi], range);
    else if (scaleOpt.type === 'log' && lo > 0) {
      var lg = linear([Math.log10(lo), Math.log10(hi)], range, false);
      f = function (v) { return lg(Math.log10(v)); };
      f.invert = function (p) { return Math.pow(10, lg.invert(p)); };
      f.ticks = function () { var out = []; for (var e = Math.floor(lg.domain[0]); e <= Math.ceil(lg.domain[1]); e++) out.push(Math.pow(10, e)); return out; };
      f.kind = 'log';
    } else f = linear([lo, hi], range, !scaleOpt.domain);
    f.enc = enc;
    return f;
  }

  function colorScale(layers, legendFormat) {
    var enc = null;
    layers.forEach(function (l) { var e = l.encoding.color; if (e && !enc) enc = e; });
    if (!enc) return function () { return NB.palette[0]; };
    if (enc.value !== undefined) return function () { return enc.value; };
    var values = channelValues(layers, 'color').filter(function (v) { return v != null; });
    if (enc.type === 'quantitative' && !enc.scheme && distinct(values).length <= 10 && values.every(Number.isInteger)) enc = Object.assign({}, enc, { type: 'nominal' });
    var f;
    if (enc.type === 'quantitative' || enc.type === 'temporal') {
      var nums = values.map(function (v) { return numeric(v, enc.type); });
      var lo = Math.min.apply(null, nums), hi = Math.max.apply(null, nums), ramp = NB.ramp(enc.scheme || 'blues');
      f = function (v) { return ramp(0.15 + 0.85 * (numeric(v, enc.type) - lo) / ((hi - lo) || 1)); };
      f.continuous = [lo, hi];
      f.ramp = ramp;
    } else {
      // scale: {domain: [...], range: [...]} fixes the order of the values and their colours, so a status
      // is always the same colour (PASS green, ERROR red) whatever the data holds.
      var sc = enc.scale || {}, domain = Array.isArray(sc.domain) ? sc.domain.slice() : [], idx = {};
      distinct(values).forEach(function (v) { if (!domain.some(function (d) { return key(d) === key(v); })) domain.push(v); });
      var range = Array.isArray(sc.range) && sc.range.length ? sc.range : NB.palette;
      domain.forEach(function (d, i) { idx[key(d)] = i; });
      f = function (v) { return range[(idx[key(v)] || 0) % range.length]; };
      f.domain = domain.filter(function (d) { return values.some(function (v) { return key(v) === key(d); }); });
    }
    f.enc = enc;
    return f;
  }

  // ---- Axes, legend, tooltips -----------------------------------------------------------------

  function tickLabel(scale, v) {
    if (scale.kind === 'time') return scale.format(v);
    if (scale.kind === 'band') return v === null ? 'NULL' : typeof v === 'number' ? NB.formatNumber(v, scale.enc.format) : String(v);
    return NB.formatNumber(v, scale.enc.format || (Math.abs(v) >= 1e5 ? '~s' : ''));
  }

  function axis(g, scale, orient, w, h, title, left) {
    if (!scale) return;
    var ticks = scale.ticks(orient === 'bottom' ? Math.max(2, Math.floor(w / 90)) : Math.max(2, Math.floor(h / 40)));
    if (scale.kind === 'band' && ticks.length > (orient === 'bottom' ? w / 14 : h / 12)) {
      var every = Math.ceil(ticks.length / (orient === 'bottom' ? w / 14 : h / 12));
      ticks = ticks.filter(function (_, i) { return i % every === 0; });
    }
    var a = s('g', { 'class': 'nb-axis' }, g);
    ticks.forEach(function (t) {
      var p = scale.kind === 'band' ? scale.center(t) : scale(numeric(t, 'x') === numeric(t, 'x') ? t : t);
      if (!isFinite(p)) return;
      if (orient === 'bottom') {
        if (scale.kind !== 'band') s('line', { x1: p, x2: p, y1: 0, y2: h, 'class': 'nb-grid-line' }, a);
        Sketch.line(a, p, h - 2, p, h + 4, { width: 1.2, rough: 0.4, double: false, stroke: 'currentColor' });
        var tx = s('text', { x: p, y: h + 16, 'text-anchor': 'middle' }, a);
        var label = tickLabel(scale, t);
        tx.textContent = label.length > 16 ? label.slice(0, 15) + '…' : label;
      } else {
        if (scale.kind !== 'band') s('line', { x1: 0, x2: w, y1: p, y2: p, 'class': 'nb-grid-line' }, a);
        var ty = s('text', { x: -6, y: p + 4, 'text-anchor': 'end' }, a);
        var l2 = tickLabel(scale, t);
        ty.textContent = l2.length > 20 ? l2.slice(0, 19) + '…' : l2;
      }
    });
    // Excalidraw-style axes: a wobbly stroke with an arrowhead at the open end.
    if (orient === 'bottom') Sketch.arrow(a, 0, h, w + 8, h, { bend: 0, width: 1.4, stroke: 'currentColor' }).setAttribute('class', 'nb-domain');
    else Sketch.arrow(a, 0, h, 0, -8, { bend: 0, width: 1.4, stroke: 'currentColor' }).setAttribute('class', 'nb-domain');
    if (title) {
      var tt = orient === 'bottom' ? s('text', { x: w / 2, y: h + 34, 'text-anchor': 'middle', 'class': 'nb-axis-title' }, a)
        : s('text', { transform: 'translate(' + (-(left || M.left) + 12) + ',' + h / 2 + ') rotate(-90)', 'text-anchor': 'middle', 'class': 'nb-axis-title' }, a);
      tt.textContent = title;
    }
  }

  function legend(container, color) {
    if (!color.enc || color.enc.value !== undefined) return;
    var box = NB.el('div', { 'class': 'nb-legend' });
    if (color.domain) {
      color.domain.slice(0, 20).forEach(function (d) {
        var c = color(d);
        box.appendChild(NB.el('span', { 'class': 'nb-legend-item' }, [NB.el('i', { style: 'background:' + NB.pastel(c) + ';border-color:' + c }), d === null ? 'NULL' : String(d)]));
      });
    } else if (color.continuous) {
      var stops = [0, 0.25, 0.5, 0.75, 1].map(function (t) { return color.ramp(0.15 + 0.85 * t) + ' ' + t * 100 + '%'; }).join(',');
      box.appendChild(NB.el('span', { text: tickLabel({ kind: color.enc.type === 'temporal' ? 'time' : 'linear', enc: color.enc, format: NB.formatDate }, color.continuous[0]) }));
      box.appendChild(NB.el('i', { 'class': 'nb-legend-ramp', style: 'background:linear-gradient(90deg,' + stops + ')' }));
      box.appendChild(NB.el('span', { text: NB.formatNumber(color.continuous[1]) }));
    }
    box.insertBefore(NB.el('b', { text: color.enc.title || '' }), box.firstChild);
    container.appendChild(box);
  }

  // The values of a mark, one 'label: value' per line, shown by NB.tip on hover.
  function tipText(layer, row) {
    var lines = [];
    ['x', 'y', 'color', 'size', 'text'].forEach(function (c) {
      var e = layer.encoding[c];
      // Truncated dates read as their unit: a month bar says "Jun 2026", not its first day.
      var unit = { year: 'year', quarter: 'month', month: 'month', week: 'day', date: 'day' }[e && e.timeunit];
      if (e && e.column && row[e.column] !== undefined) lines.push((e.title || e.column || c) + ': ' + (e.type === 'temporal' ? NB.formatDate(row[e.column], unit) : NB.formatValue(row[e.column], '', e.format)));
    });
    (layer.tooltip || []).forEach(function (e) { lines.push((e.title || e.column) + ': ' + NB.formatValue(row[e.column], '', e.format)); });
    if (layer.binned && row.count != null) lines.push('rows: ' + NB.formatNumber(row.count, ','));
    return lines.join('\n');
  }
  function tooltip(el, layer, row) { el.setAttribute('data-tip', tipText(layer, row)); }

  // ---- Marks ----------------------------------------------------------------------------------

  function groupBy(rows, column) {
    var groups = {}, order = [];
    rows.forEach(function (r) { var k = column ? key(r[column]) : ''; if (!groups[k]) { groups[k] = []; order.push(k); } groups[k].push(r); });
    return order.map(function (k) { return groups[k]; });
  }

  function pos(scale, v, type) { return scale.kind === 'band' ? scale(v) : scale(numeric(v, type)); }

  // Marks are hand-drawn (see render-sketch.js). Above a few hundred marks the wobble stops paying
  // for itself, so dense layers fall back to plain shapes in the same colours.
  var SKETCH_LIMIT = 600;

  function mark(el, layer, r) {
    el.setAttribute('class', 'nb-mark');
    el.__row = r;
    tooltip(el, layer, r);
    return el;
  }

  function drawLayer(g, layer, x, y, color, w, h, size) {
    var enc = layer.encoding, ex = enc.x, ey = enc.y, ec = enc.color, rows = layer.rows;
    var colorOf = function (r) { return ec && ec.column ? color(r[ec.column]) : (ec && ec.value) || NB.palette[0]; };
    var fillOf = function (c) { return color.continuous ? c : NB.pastel(c); };
    var opacity = layer.mark_opacity != null ? layer.mark_opacity : null;
    var kind = layer.mark, plain = rows.length > SKETCH_LIMIT;
    if (kind === 'line' || kind === 'area') {
      groupBy(rows, ec && ec.column).forEach(function (group) {
        var pts = group.map(function (r) {
          var px = x.kind === 'band' ? x.center(r[ex.column]) : x(numeric(r[ex.column], ex.type));
          return [px, y(numeric(r[ey.column], ey.type)), r];
        }).filter(function (p) { return isFinite(p[0]) && isFinite(p[1]); }).sort(function (a, b) { return a[0] - b[0]; });
        if (!pts.length) return;
        var c = colorOf(group[0]);
        // Every point of a line answers hover, even where no dot is drawn (see hoverPoints).
        if (g.__points) { g.__series = (g.__series || 0) + 1; pts.forEach(function (p) { g.__points.push({ x: p[0], y: p[1], text: tipText(layer, p[2]), color: c, series: g.__series }); }); }
        if (kind === 'area') {
          var base = y(Math.max(y.domain ? y.domain[0] : 0, 0));
          var area = Sketch.smooth(pts) + 'L' + pts[pts.length - 1][0].toFixed(1) + ',' + base + 'L' + pts[0][0].toFixed(1) + ',' + base + 'Z';
          Sketch.fill(g, area, { x: 0, y: 0, w: w, h: h }, { fill: NB.pastel(c), gap: 7, fillOpacity: opacity != null ? Math.min(1, opacity * 2.5) : null });
        }
        // Thousands of points are already downsampled (M4); a sketch line through them stays legible.
        var line = Sketch.polyline(g, pts, { stroke: c, width: layer.mark_strokeWidth || 2, double: pts.length < 400 });
        line.setAttribute('class', 'nb-line');
        if (pts.length <= 60 || layer.mark_point) pts.forEach(function (p) { mark(s('circle', { cx: p[0], cy: p[1], r: 3, fill: c, 'class': 'nb-pt' }, g), layer, p[2]); });
      });
    } else if (kind === 'bar') {
      var horizontal = y.kind === 'band' && x.kind !== 'band';
      var stack = {};
      // Bars on a continuous x (months on a date axis) are as wide as the gap to their neighbours allows.
      var slot = 4;
      if (!horizontal && x.kind !== 'band' && !ex.end) {
        var xs = rows.map(function (r) { return x(numeric(r[ex.column], ex.type)); }).filter(isFinite).sort(function (a, b) { return a - b; });
        var gap = Infinity;
        for (var i = 1; i < xs.length; i++) if (xs[i] - xs[i - 1] > 0.5) gap = Math.min(gap, xs[i] - xs[i - 1]);
        slot = isFinite(gap) ? Math.max(3, Math.min(64, gap * 0.75)) : 24;
      }
      rows.forEach(function (r) {
        var c = colorOf(r), bx, by, bw, bh;
        if (horizontal) {
          var v = numeric(r[ex.column], ex.type), k = key(r[ey.column]), start = stack[k] || 0;
          stack[k] = start + v;
          var x0 = x(start), x1 = x(start + v);
          bx = Math.min(x0, x1); by = y(r[ey.column]); bw = Math.abs(x1 - x0); bh = y.bandwidth;
        } else if (x.kind === 'band') {
          var vy = numeric(r[ey.column], ey.type), kx = key(r[ex.column]), st = stack[kx] || 0;
          stack[kx] = st + vy;
          var y0 = y(st), y1 = y(st + vy);
          bx = x(r[ex.column]); by = Math.min(y0, y1); bw = x.bandwidth; bh = Math.abs(y1 - y0);
        } else {
          var a = x(numeric(r[ex.column], ex.type)), b = ex.end ? x(numeric(r[ex.end], ex.type)) : a + slot / 2;
          if (!ex.end) a -= slot / 2;
          var yy = y(numeric(r[ey.column], ey.type)), zero = y(0);
          bx = Math.min(a, b) + 0.5; by = Math.min(yy, zero); bw = Math.max(1, Math.abs(b - a) - 1); bh = Math.abs(zero - yy);
          if (!ex.end) { var right = Math.min(w, bx + bw); bx = Math.max(0, bx); bw = Math.max(1, right - bx); }
        }
        if (!isFinite(bx) || !isFinite(by)) return;
        // A bar answers hover anywhere in its column (its row, for horizontal bars), so short bars
        // need no aim. See hoverPoints.
        if (g.__bars) g.__bars.push({ x0: bx, x1: bx + bw, y0: by, y1: by + bh, horizontal: horizontal, text: tipText(layer, r), color: c });
        var el = plain ? s('rect', { x: bx, y: by, width: bw, height: bh, fill: fillOf(c), stroke: c }, g)
          : Sketch.rect(g, bx, by, bw, bh, { stroke: c, fill: fillOf(c), width: 1.3, gap: Math.max(4, Math.min(7, bw / 4)),
            // Narrow bars scribble when drawn twice with full wobble: one calmer stroke.
            rough: bw < 14 ? 0.4 : 1, double: bw >= 14 });
        mark(el, layer, r);
      });
    } else if (kind === 'point' || kind === 'tick' || kind === 'text') {
      var maxCount = 1;
      if (layer.binned) rows.forEach(function (r) { maxCount = Math.max(maxCount, r.count); });
      rows.forEach(function (r) {
        var px = x.kind === 'band' ? x.center(r[ex.column]) : x(numeric(r[ex.column], ex.type));
        var py = y ? (y.kind === 'band' ? y.center(r[ey.column]) : y(numeric(r[ey.column], ey.type))) : h / 2;
        if (!isFinite(px) || !isFinite(py)) return;
        if (g.__dots) g.__dots.push({ x: px, y: py });
        var el, c = colorOf(r);
        if (kind === 'text') {
          el = s('text', { x: px, y: py, 'text-anchor': 'middle', 'class': 'nb-mark-text', fill: c }, g);
          el.textContent = NB.formatValue(r[enc.text && enc.text.column], '', enc.text && enc.text.format);
        } else if (kind === 'tick') {
          el = Sketch.line(g, px - 6, py, px + 6, py, { stroke: c, width: 2, rough: 0.5, double: !plain });
        } else {
          var radius = layer.binned ? 1.5 + 4 * Math.sqrt(r.count / maxCount) : enc.size && enc.size.column ? size(r[enc.size.column]) : 3.5;
          var fill = layer.binned && !(ec && ec.column) ? NB.ramp('blues')(0.3 + 0.7 * Math.sqrt(r.count / maxCount)) : fillOf(c);
          el = plain || radius < 3 ? s('circle', { cx: px, cy: py, r: radius, fill: layer.binned ? fill : c, 'fill-opacity': opacity != null ? opacity : 0.75 }, g)
            : Sketch.ellipse(g, px, py, radius, radius, { stroke: c, fill: fill, fillStyle: 'solid', width: 1.1, rough: 0.6, double: false });
        }
        mark(el, layer, r);
      });
    } else if (kind === 'rect') {
      rows.forEach(function (r) {
        var x0 = pos(x, r[ex.column], ex.type), y0 = pos(y, r[ey.column], ey.type);
        var wd = x.kind === 'band' ? x.bandwidth : Math.abs(x(numeric(r[ex.end || ex.column], ex.type)) - x0) || 4;
        var ht = y.kind === 'band' ? y.bandwidth : Math.abs(y(numeric(r[ey.end || ey.column], ey.type)) - y0) || 4;
        var top = y.kind === 'band' ? y0 : y0 - ht, c = colorOf(r);
        // Heatmap cells keep a solid colour (hachures would hide the gradient) inside a hand-drawn edge.
        var el = plain ? s('rect', { x: x0, y: top, width: wd, height: ht, fill: c }, g)
          : Sketch.rect(g, x0 + 1, top + 1, wd - 2, ht - 2, { stroke: 'currentColor', width: 0.9, fill: color.continuous ? c : fillOf(c), fillStyle: 'solid', rough: 0.6, double: false });
        mark(el, layer, r);
      });
    } else if (kind === 'rule') {
      rows.forEach(function (r) {
        var c = colorOf(r), el;
        if (ex && ex.column && !(ey && ey.column)) { var px = pos(x, r[ex.column], ex.type); el = Sketch.line(g, px, 0, px, h, { stroke: c, width: 1.5, dash: '7 6' }); }
        else if (ey && ey.column && !(ex && ex.column)) { var py = pos(y, r[ey.column], ey.type); el = Sketch.line(g, 0, py, w, py, { stroke: c, width: 1.5, dash: '7 6' }); }
        else if (ex && ey) {
          var e2 = enc.x2 || enc.y2;
          var X1 = pos(x, r[ex.column], ex.type), Y1 = pos(y, r[ey.column], ey.type);
          var X2 = enc.x2 && enc.x2.column ? pos(x, r[enc.x2.column], ex.type) : X1, Y2 = enc.y2 && enc.y2.column ? pos(y, r[enc.y2.column], ey.type) : Y1;
          if (e2) el = Sketch.line(g, X1, Y1, X2, Y2, { stroke: c, width: 1.5 });
        }
        if (el) tooltip(el, layer, r);
      });
    } else if (kind === 'boxplot') {
      rows.forEach(function (r) {
        var cx = x ? x.center(r.x) : w / 2, bw = x ? Math.min(40, x.bandwidth) : 40, c = NB.palette[0];
        var q1 = y(r.y_q1), q3 = y(r.y_q3), md = y(r.y_median), lo = y(r.y_min), hi = y(r.y_max);
        var box = s('g', {}, g);
        Sketch.line(box, cx, lo, cx, hi, { stroke: c, width: 1.2 });
        Sketch.rect(box, cx - bw / 2, Math.min(q1, q3), bw, Math.abs(q1 - q3), { stroke: c, fill: NB.pastel(c), width: 1.3 });
        Sketch.line(box, cx - bw / 2, md, cx + bw / 2, md, { stroke: c, width: 2.2, rough: 0.5 });
        box.setAttribute('data-tip', (ex && ex.column ? (ex.title || 'x') + ': ' + NB.formatValue(r[ex.column]) + '\n' : '') + 'max: ' + NB.formatNumber(r.y_max) + '\nq3: ' + NB.formatNumber(r.y_q3) + '\nmedian: ' + NB.formatNumber(r.y_median) + '\nq1: ' + NB.formatNumber(r.y_q1) + '\nmin: ' + NB.formatNumber(r.y_min));
      });
    }
  }

  // Lines are thin targets: hovering anywhere over the plot shows the values of the nearest line point,
  // nearest in x first, marked with a dot. NB.tip calls svg.__nbHover.
  function hoverPoints(svgEl, g, points, bars, w, h) {
    if (!points.length && !bars.length) return;
    var band = s('rect', { 'class': 'nb-hover-band', visibility: 'hidden' }, g);
    g.insertBefore(band, g.firstChild);
    var dot = s('circle', { r: 5, 'class': 'nb-hover-dot', fill: 'none', 'stroke-width': 2, visibility: 'hidden' }, g);
    // The bar whose column (or row) holds the pointer: the one under it, else the nearest.
    function barAt(p) {
      var best = null, bestD = Infinity;
      bars.forEach(function (b) {
        var inBand = b.horizontal ? p.y >= b.y0 - 1 && p.y <= b.y1 + 1 : p.x >= b.x0 - 1 && p.x <= b.x1 + 1;
        if (!inBand) return;
        var d = b.horizontal ? (p.x < b.x0 ? b.x0 - p.x : p.x > b.x1 ? p.x - b.x1 : 0) : (p.y < b.y0 ? b.y0 - p.y : p.y > b.y1 ? p.y - b.y1 : 0);
        if (d < bestD) { bestD = d; best = b; }
      });
      if (best) return best;
      // Between bars: the nearest one across, within reach.
      bars.forEach(function (b) {
        var d = b.horizontal ? Math.max(b.y0 - p.y, p.y - b.y1) : Math.max(b.x0 - p.x, p.x - b.x1);
        if (d < bestD && d < 40) { bestD = d; best = b; }
      });
      return best;
    }
    function off() { dot.setAttribute('visibility', 'hidden'); band.setAttribute('visibility', 'hidden'); return null; }
    svgEl.__nbHover = function (clientX, clientY) {
      var m = g.getScreenCTM();
      if (!m) return null;
      var pt = svgEl.createSVGPoint();
      pt.x = clientX; pt.y = clientY;
      var p = pt.matrixTransform(m.inverse()), best = null, bestD = Infinity;
      if (p.x < -4 || p.x > w + 4 || p.y < -4 || p.y > h + 4) return off();
      var bar = bars.length ? barAt(p) : null;
      if (bar) {
        dot.setAttribute('visibility', 'hidden');
        if (bar.horizontal) { band.setAttribute('x', 0); band.setAttribute('width', w); band.setAttribute('y', bar.y0); band.setAttribute('height', bar.y1 - bar.y0); }
        else { band.setAttribute('x', bar.x0); band.setAttribute('width', bar.x1 - bar.x0); band.setAttribute('y', 0); band.setAttribute('height', h); }
        band.setAttribute('visibility', 'visible');
        return bar.text;
      }
      band.setAttribute('visibility', 'hidden');
      points.forEach(function (q) {
        var d = Math.abs(q.x - p.x) * 4 + Math.abs(q.y - p.y);
        if (d < bestD) { bestD = d; best = q; }
      });
      if (!best) return off();
      dot.setAttribute('cx', best.x); dot.setAttribute('cy', best.y); dot.setAttribute('stroke', best.color);
      dot.setAttribute('visibility', 'visible');
      return best.text;
    };
    svgEl.__nbLeave = off;
  }

  function arcChart(svgEl, layer, color, w, h) {
    var enc = layer.encoding, value = enc.theta || enc.y || enc.size;
    var total = layer.rows.reduce(function (a, r) { return a + Math.max(0, Number(r[value.column]) || 0); }, 0) || 1;
    var cx = w / 2, cy = h / 2, R = Math.min(w, h) / 2 - 10, inner = layer.mark_innerRadius || 0, angle = -Math.PI / 2;
    layer.rows.forEach(function (r) {
      var v = Math.max(0, Number(r[value.column]) || 0), a2 = angle + v / total * Math.PI * 2;
      var large = a2 - angle > Math.PI ? 1 : 0;
      var p = function (rad, a) { return (cx + rad * Math.cos(a)).toFixed(1) + ',' + (cy + rad * Math.sin(a)).toFixed(1); };
      var d = 'M' + p(R, angle) + 'A' + R + ',' + R + ' 0 ' + large + ' 1 ' + p(R, a2) + (inner ? 'L' + p(inner, a2) + 'A' + inner + ',' + inner + ' 0 ' + large + ' 0 ' + p(inner, angle) : 'L' + cx + ',' + cy) + 'Z';
      var c = enc.color && enc.color.column ? color(r[enc.color.column]) : NB.palette[0];
      mark(Sketch.shape(svgEl, d, { x: cx - R, y: cy - R, w: 2 * R, h: 2 * R }, { stroke: c, fill: NB.pastel(c), width: 1.4, gap: 7 }), layer, r);
      angle = a2;
    });
  }

  // ---- Selections -----------------------------------------------------------------------------

  function isoLocal(ms) {
    var d = new Date(ms), p = function (x) { return (x < 10 ? '0' : '') + x; };
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  }

  function invertRange(scale, a, b) {
    if (scale.kind === 'band') { var cats = scale.invertRange(a, b); return cats.length ? { lo: cats[0], hi: cats[cats.length - 1] } : null; }
    var v1 = scale.invert(Math.min(a, b)), v2 = scale.invert(Math.max(a, b));
    if (v1 > v2) { var t = v1; v1 = v2; v2 = t; }
    return scale.kind === 'time' ? { lo: isoLocal(v1), hi: isoLocal(v2) } : { lo: v1, hi: v2 };
  }

  function brush(g, sel, x, y, w, h, opts) {
    var channels = sel.channels || ['x'], onX = channels.indexOf('x') >= 0, onY = channels.indexOf('y') >= 0;
    var overlay = s('rect', { x: 0, y: 0, width: w, height: h, fill: 'transparent', 'class': 'nb-brush-overlay' }, g);
    var rect = s('rect', { 'class': 'nb-brush', x: 0, y: 0, width: 0, height: 0 }, g);
    var start = null;
    function local(e) { var b = overlay.getBoundingClientRect(); return [(e.clientX - b.left) / b.width * w, (e.clientY - b.top) / b.height * h]; }
    function draw(a, b) {
      var x0 = onX ? Math.min(a[0], b[0]) : 0, x1 = onX ? Math.max(a[0], b[0]) : w;
      var y0 = onY ? Math.min(a[1], b[1]) : 0, y1 = onY ? Math.max(a[1], b[1]) : h;
      rect.setAttribute('x', x0); rect.setAttribute('y', y0); rect.setAttribute('width', x1 - x0); rect.setAttribute('height', y1 - y0);
    }
    overlay.addEventListener('mousedown', function (e) { start = local(e); draw(start, start); e.preventDefault(); });
    window.addEventListener('mousemove', function (e) { if (start) draw(start, local(e)); });
    window.addEventListener('mouseup', function (e) {
      if (!start) return;
      var end = local(e), a = start;
      start = null;
      if (Math.abs(end[0] - a[0]) < 3 && Math.abs(end[1] - a[1]) < 3) { draw([0, 0], [0, 0]); rect.setAttribute('width', 0); opts.onSelect && opts.onSelect(sel.as, null); return; }
      var value;
      if (onX && onY) value = { x: invertRange(x, a[0], end[0]), y: invertRange(y, a[1], end[1]) };
      else if (onY) value = invertRange(y, a[1], end[1]);
      else value = invertRange(x, a[0], end[0]);
      opts.onSelect && opts.onSelect(sel.as, value);
    });
    // Show the current selection after a redraw.
    var cur = opts.selected && opts.selected[sel.as];
    if (cur && onX && !onY && x.kind !== 'band' && cur.lo != null) {
      var lo = x(numeric(cur.lo, x.enc.type)), hi = x(numeric(cur.hi, x.enc.type));
      if (isFinite(lo) && isFinite(hi)) draw([lo, 0], [hi, h]);
    }
  }

  function pointSelect(svgEl, sel, layers, opts) {
    var channel = (sel.channels || ['x'])[0], current = (opts.selected && opts.selected[sel.as]) || [];
    var keys = current.map(key);
    layers.forEach(function (l) {
      var enc = l.encoding[channel];
      if (!enc || !enc.column) return;
      svgEl.querySelectorAll('.nb-mark').forEach(function (el) {
        if (!el.__row) return;
        var v = el.__row[enc.column];
        if (keys.length && keys.indexOf(key(v)) < 0) el.classList.add('nb-dim');
        el.style.cursor = 'pointer';
        el.addEventListener('click', function (e) {
          var k = keys.indexOf(key(v)), next = e.shiftKey ? current.slice() : [];
          if (k >= 0 && e.shiftKey) next.splice(k, 1); else if (k < 0) next.push(v);
          opts.onSelect && opts.onSelect(sel.as, next.length ? next : null);
        });
      });
    });
  }

  // ---- Rules and bands ------------------------------------------------------------------------

  // The value of one end of a rule or band: written in the spec, or computed by DuckDB (render.reference_data).
  function refValue(ref, row) { return ref && ref.column ? (row ? row[ref.column] : null) : ref ? ref.value : null; }

  function references(render, datasets) {
    var row = render.reference_data != null ? (datasets[render.reference_data] || [])[0] : null;
    var rules = (render.rules || []).map(function (r) { return Object.assign({}, r, { at: refValue(r, row) }); });
    var bands = (render.bands || []).map(function (b) { return Object.assign({}, b, { lo: refValue(b.from, row), hi: refValue(b.to, row) }); });
    var extra = { x: [], y: [] };
    rules.forEach(function (r) { if (r.at != null) extra[r.axis].push(r.at); });
    bands.forEach(function (b) { [b.lo, b.hi].forEach(function (v) { if (v != null) extra[b.axis].push(v); }); });
    return { rules: rules, bands: bands, extra: extra };
  }

  // Where a value sits on an axis, as [start, end] pixels: a point on a continuous axis, the width of
  // its band on a category axis.
  function span(scale, v) {
    if (!scale || v == null) return null;
    if (scale.kind === 'band') { var p = scale(v); return isFinite(p) ? [p, p + scale.bandwidth] : null; }
    var q = scale(numeric(v, scale.enc.type));
    return isFinite(q) ? [q, q] : null;
  }

  function refLabel(text, scale, values) {
    return String(text || '').replace(/\{(value|from|to)\}/g, function (_, k) { var v = values[k]; return v == null ? '' : tickLabel(scale, scale.kind === 'band' ? v : numeric(v, scale.enc.type)); });
  }

  // Drawn after the axes and before the marks, in muted ink, so they sit behind the data. Their labels
  // are short handwriting that hatching would hide, so the function returned writes them over the marks.
  function drawReferences(g, refs, x, y, w, h) {
    if (!refs.rules.length && !refs.bands.length) return function () {};
    var layer = s('g', { 'class': 'nb-refs' }, g), labels = [];
    // What notes should keep clear of: the lines and their labels.
    refs.boxes = [];
    var clamp = function (p, max) { return Math.max(0, Math.min(max, p)); };
    var label = function (parent, text, px, py, anchor) {
      if (!text) return;
      text = text.length > 40 ? text.slice(0, 39) + '…' : text;
      labels.push({ color: parent.style.color, text: text, x: f1(px), y: f1(py), anchor: anchor });
      var wide = text.length * 7 + 4;
      refs.boxes.push([anchor === 'end' ? px - wide : px, py - 12, anchor === 'end' ? px : px + wide, py + 4, 6]);
    };
    refs.bands.forEach(function (b) {
      var scale = b.axis === 'x' ? x : y, max = b.axis === 'x' ? w : h, a = span(scale, b.lo), c = span(scale, b.hi);
      if (!a || !c) return;
      var p0 = clamp(Math.min(a[0], c[0]), max), p1 = clamp(Math.max(a[1], c[1]), max);
      if (p1 - p0 < 0.5) return;
      var e = s('g', { 'class': 'nb-ref nb-ref-band' }, layer), text = refLabel(b.label, scale, { from: b.lo, to: b.hi });
      if (b.color) e.style.color = b.color;
      var style = { stroke: 'none', fill: 'currentColor', gap: 9, hachure: 1.1, fillOpacity: 0.45 };
      if (b.axis === 'y') {
        Sketch.rect(e, 0, p0, w, p1 - p0, style);
        label(e, text, 6, p1 - p0 >= 18 ? p0 + 14 : p0 - 4, 'start');
      } else {
        Sketch.rect(e, p0, 0, p1 - p0, h, style);
        var wide = text.length * 7 + 8;
        if (p0 + wide <= w) label(e, text, p0 + 5, 12, 'start'); else label(e, text, p1 - 5, 12, 'end');
      }
      if (text) e.setAttribute('data-ref', text);
    });
    refs.rules.forEach(function (r) {
      var scale = r.axis === 'x' ? x : y, max = r.axis === 'x' ? w : h, at = span(scale, r.at);
      if (!at) return;
      var p = (at[0] + at[1]) / 2;
      if (p < -0.5 || p > max + 0.5) return;
      var e = s('g', { 'class': 'nb-ref nb-ref-rule' }, layer), text = refLabel(r.label, scale, { value: r.at });
      if (r.color) e.style.color = r.color;
      var style = { stroke: 'currentColor', width: 1.4, dash: '7 6', rough: 0.6, double: false };
      if (r.axis === 'y') {
        Sketch.line(e, 0, p, w, p, style);
        refs.boxes.push([0, p - 3, w, p + 3, 2]);
        label(e, text, w - 4, p < 16 ? p + 15 : p - 5, 'end');
      } else {
        Sketch.line(e, p, 0, p, h, style);
        refs.boxes.push([p - 3, 0, p + 3, h, 2]);
        if (p + text.length * 7 + 8 <= w) label(e, text, p + 5, 12, 'start'); else label(e, text, p - 5, 12, 'end');
      }
      if (text) e.setAttribute('data-ref', text);
    });
    return function () {
      if (!labels.length) return;
      var over = s('g', { 'class': 'nb-ref-labels' }, g);
      labels.forEach(function (l) {
        var t = s('text', { x: l.x, y: l.y, 'text-anchor': l.anchor, 'class': 'nb-ref nb-ref-label' }, over);
        if (l.color) t.style.color = l.color;
        t.textContent = l.text;
      });
    };
  }
  // ---- Notes ----------------------------------------------------------------------------------

  // Handwriting is wide: a note is a few short lines.
  function wrapNote(text) {
    var lines = [], line = '';
    String(text).split(/\s+/).filter(Boolean).forEach(function (word) {
      if (line && (line + ' ' + word).length > 18) { lines.push(line); line = word; } else line = line ? line + ' ' + word : word;
    });
    if (line) lines.push(line);
    if (lines.length > 4) { lines = lines.slice(0, 4); lines[3] = lines[3].slice(0, 17) + '…'; }
    return lines;
  }

  // Notes (render.notes) sit in data coordinates and are drawn after the marks: text where there is
  // room, a curved arrow to the point, optionally a ring round it; a range gets a bracket. What is in
  // the way is what the marks left behind for hover (points, bars) plus the notes already placed.
  function drawNotes(g, notes, x, y, w, h, plot, taken) {
    if (!notes || !notes.length) return;
    var layer = s('g', { 'class': 'nb-notes' }, g), CH = 7.4, LH = 17;
    var obstacles = taken.slice();
    plot.__bars.forEach(function (b) { obstacles.push([b.x0, b.y0, b.x1, b.y1, 1]); });
    plot.__dots.forEach(function (d) { obstacles.push([d.x - 5, d.y - 5, d.x + 5, d.y + 5, 3]); });
    plot.__points.forEach(function (p, i) {
      obstacles.push([p.x - 5, p.y - 5, p.x + 5, p.y + 5, 3]);
      var q = plot.__points[i + 1];
      if (!q || q.series !== p.series) return;
      var n = Math.ceil(Math.max(Math.abs(q.x - p.x), Math.abs(q.y - p.y)) / 10);
      for (var k = 1; k < n; k++) { var cx = p.x + (q.x - p.x) * k / n, cy = p.y + (q.y - p.y) * k / n; obstacles.push([cx - 4, cy - 4, cx + 4, cy + 4, 3]); }
    });
    var mid = function (scale, v) { var p = span(scale, v); return p ? (p[0] + p[1]) / 2 : NaN; };
    // The marks at a position on one axis: their extreme on the other (the top of a bar, the highest
    // line), in pixels.
    var reach = function (axis, lo, hi) {
      var best = NaN, take = function (v) { if (isFinite(v)) best = isFinite(best) ? (axis === 'x' ? Math.min(best, v) : Math.max(best, v)) : v; };
      plot.__bars.forEach(function (b) {
        if (axis === 'x' ? b.x1 >= lo - 1 && b.x0 <= hi + 1 : b.y1 >= lo - 1 && b.y0 <= hi + 1) take(axis === 'x' ? b.y0 : b.x1);
      });
      var pts = plot.__points.concat(plot.__dots), near = Infinity;
      pts.forEach(function (p) { var v = axis === 'x' ? p.x : p.y; near = Math.min(near, v < lo ? lo - v : v > hi ? v - hi : 0); });
      pts.forEach(function (p) { var v = axis === 'x' ? p.x : p.y, d = v < lo ? lo - v : v > hi ? v - hi : 0; if (d <= near + 2) take(axis === 'x' ? p.y : p.x); });
      return best;
    };
    var overlap = function (box) {
      var total = 0;
      obstacles.forEach(function (o) {
        var ox = Math.min(box[2], o[2]) - Math.max(box[0], o[0]), oy = Math.min(box[3], o[3]) - Math.max(box[1], o[1]);
        if (ox > 0 && oy > 0) total += ox * oy * o[4];
      });
      // Outside the plot costs most of all: the note would be cut off or sit on the axes.
      var inside = Math.max(0, Math.min(box[2], w) - Math.max(box[0], 0)) * Math.max(0, Math.min(box[3], h - 2) - Math.max(box[1], -M.top + 2));
      return total + ((box[2] - box[0]) * (box[3] - box[1]) - inside) * 60;
    };
    var write = function (parent, lines, bx, by, anchor) {
      lines.forEach(function (line, i) {
        s('text', { x: f1(bx), y: f1(by + 13 + i * LH), 'text-anchor': anchor || 'start', 'class': 'nb-note-text' }, parent).textContent = line;
      });
    };
    var DIRS = { 'above-right': [0.7, -0.7], 'above-left': [-0.7, -0.7], right: [1, 0], left: [-1, 0], above: [0, -1], 'below-right': [0.7, 0.7], 'below-left': [-0.7, 0.7], below: [0, 1] };
    var ORDER = ['above-right', 'above-left', 'right', 'left', 'above', 'below-right', 'below-left', 'below'];
    notes.forEach(function (note) {
      var lines = wrapNote(note.text || ''), tw = lines.reduce(function (m, l) { return Math.max(m, l.length); }, 0) * CH + 4, th = lines.length * LH;
      var e = s('g', { 'class': 'nb-note' }, layer);
      if (note.color) e.style.color = note.color;
      var rangeAxis = Array.isArray(note.x) ? 'x' : Array.isArray(note.y) ? 'y' : null;
      if (rangeAxis) {
        // A bracket over a range, just clear of the marks under it, with the text beyond it.
        var scale = rangeAxis === 'x' ? x : y, r = note[rangeAxis], a = span(scale, r[0]), b = span(scale, r[1]);
        if (!a || !b) return;
        var max = rangeAxis === 'x' ? w : h, p0 = Math.max(0, Math.min(a[0], b[0])), p1 = Math.min(max, Math.max(a[1], b[1]));
        var other = rangeAxis === 'x' ? note.y : note.x, at = other != null ? mid(rangeAxis === 'x' ? y : x, other) : reach(rangeAxis, p0, p1);
        var style = { stroke: 'currentColor', width: 1.4, rough: 0.7 }, T = 6;
        if (rangeAxis === 'x') {
          var by = Math.max(th + 8 - M.top, (isFinite(at) ? at : 30) - 12) + (note.dy || 0);
          Sketch.line(e, p0, by + T, p0, by, style); Sketch.line(e, p0, by, p1, by, style); Sketch.line(e, p1, by, p1, by + T, style);
          write(e, lines, (p0 + p1) / 2 + (note.dx || 0), by - th - 4, 'middle');
          obstacles.push([p0, by - th - 6, p1, by + T, 6]);
        } else {
          var bx = Math.min(w - tw - 8, (isFinite(at) ? at : w - 60) + 12) + (note.dx || 0);
          Sketch.line(e, bx - T, p0, bx, p0, style); Sketch.line(e, bx, p0, bx, p1, style); Sketch.line(e, bx, p1, bx - T, p1, style);
          write(e, lines, bx + 6, (p0 + p1) / 2 - th / 2 + (note.dy || 0), 'start');
          obstacles.push([bx - T, p0, bx + tw + 8, p1, 6]);
        }
        return;
      }
      var ax = note.x != null ? mid(x, note.x) : NaN, ay = note.y != null ? mid(y, note.y) : NaN;
      if (note.x != null && !isFinite(ax) || note.y != null && !isFinite(ay)) return;
      if (!isFinite(ay)) { ay = reach('x', ax, ax); if (!isFinite(ay)) ay = h / 2; }
      if (!isFinite(ax)) { ax = reach('y', ay, ay); if (!isFinite(ax)) ax = w / 2; }
      ax = Math.max(0, Math.min(w, ax)); ay = Math.max(0, Math.min(h, ay));
      var ring = note.circle ? 11 : 0;
      if (ring) { Sketch.ellipse(e, ax, ay, ring + 1, ring - 1, { stroke: 'currentColor', width: 1.5, rough: 1.2 }); obstacles.push([ax - ring - 2, ay - ring - 2, ax + ring + 2, ay + ring + 2, 6]); }
      if (!lines.length) return;
      var names = ORDER.filter(function (n) { return !note.side || n.indexOf(note.side) >= 0; });
      if (note.side) names.sort(function (a2, b2) { return (a2 === note.side ? 0 : 1) - (b2 === note.side ? 0 : 1); });
      var best = null, bestScore = Infinity;
      [30, 56, 84].forEach(function (dist, di) {
        names.forEach(function (name, ni) {
          var u = DIRS[name], reachOut = dist + ring;
          var bx = ax + u[0] * reachOut + (u[0] > 0.3 ? 0 : u[0] < -0.3 ? -tw : -tw / 2);
          var by = ay + u[1] * reachOut + (u[1] > 0.3 ? 0 : u[1] < -0.3 ? -th : -th / 2);
          var score = overlap([bx - 3, by - 3, bx + tw + 3, by + th + 3]) + ni * 25 + di * 60;
          if (score < bestScore) { bestScore = score; best = [bx, by]; }
        });
      });
      var tx = best[0] + (note.dx || 0), ty = best[1] + (note.dy || 0);
      write(e, lines, tx, ty);
      obstacles.push([tx - 2, ty - 2, tx + tw + 2, ty + th + 2, 6]);
      // The arrow leaves the edge of the text nearest the point and stops just short of it.
      var sx = Math.max(tx - 3, Math.min(tx + tw + 3, ax)), sy = Math.max(ty - 1, Math.min(ty + th + 3, ay));
      var len = Math.sqrt((ax - sx) * (ax - sx) + (ay - sy) * (ay - sy)), gap = ring ? ring + 3 : 6;
      if (len < gap + 10) return;
      var ex = ax - (ax - sx) / len * gap, ey = ay - (ay - sy) / len * gap;
      Sketch.arrow(e, sx, sy, ex, ey, { bend: sx <= ax ? 0.16 : -0.16, width: 1.3, stroke: 'currentColor' });
    });
  }
  function f1(n) { return Math.round(n * 10) / 10; }

  // ---- Grammar entry point --------------------------------------------------------------------

  NB.grammar = function (container, render, datasets, opts) {
    var allLayers = (render.layers || []).filter(Boolean).map(function (l) {
      return Object.assign({}, l, { rows: datasets[l.data] || [] });
    });
    if (allLayers.length === 1 && allLayers[0].mark === 'arc') {
      var color0 = colorScale(allLayers);
      var w0 = Math.min(render.width || 640, 360), h0 = Math.min(render.height || 320, 320);
      var svg0 = s('svg', { viewBox: '0 0 ' + w0 + ' ' + h0, width: w0, height: h0, 'class': 'nb-chart-svg' }, container);
      arcChart(svg0, allLayers[0], color0, w0, h0);
      legend(container, color0);
      if (render.selection && render.selection.type === 'point') pointSelect(svg0, Object.assign({}, render.selection, { channels: ['color'] }), allLayers, opts || {});
      return;
    }
    opts = opts || {};
    var facetRows = [null], facetCols = [null];
    if (render.facet) {
      var rowsAll = [].concat.apply([], allLayers.map(function (l) { return l.rows; }));
      if (render.facet.row) facetRows = distinct(rowsAll.map(function (r) { return r.facet_row; }));
      if (render.facet.col) facetCols = distinct(rowsAll.map(function (r) { return r.facet_col; }));
    }
    var totalW = render.width || 640, totalH = render.height || 320;
    var panelW = Math.max(120, Math.floor(totalW / facetCols.length)), panelH = Math.max(100, Math.floor(totalH / facetRows.length));
    // Category labels on the y axis are handwritten and wide: make room for the longest one.
    var left = M.left;
    allLayers.forEach(function (l) {
      var e = l.encoding.y;
      if (!e || !e.column || (e.type !== 'nominal' && e.type !== 'ordinal')) return;
      l.rows.forEach(function (r) { var v = r[e.column]; left = Math.max(left, Math.min(180, 30 + 8.2 * String(v == null ? 'NULL' : v).slice(0, 20).length)); });
    });
    var w = panelW - left - M.right, h = panelH - M.top - M.bottom;
    var horizontalBars = allLayers.some(function (l) { return l.mark === 'bar' && l.encoding.y && (l.encoding.y.type === 'nominal' || l.encoding.y.type === 'ordinal'); });
    var hasBars = allLayers.some(function (l) { return l.mark === 'bar' || l.mark === 'area'; });
    var stackTotals = function (channel, by) {
      // Stacked bars need the scale to cover the sums, not the parts.
      var sums = {}, out = [];
      allLayers.forEach(function (l) {
        if (l.mark !== 'bar' || !l.encoding[by] || !l.encoding[channel]) return;
        l.rows.forEach(function (r) { var k = key(r[l.encoding[by].column]); sums[k] = (sums[k] || 0) + Number(r[l.encoding[channel].column]); });
      });
      for (var k in sums) out.push(sums[k]);
      return out;
    };
    // Bars on a continuous x are centred on their value: pad the axis by half the smallest gap so the
    // first and last bars are as wide as the rest.
    var refs = references(render, datasets);
    var padForBars = function (sc, width) {
      if (!sc || sc.kind === 'band' || horizontalBars) return sc;
      var vals = [];
      allLayers.forEach(function (l) {
        var e = l.encoding.x;
        if (l.mark === 'bar' && e && e.column && !e.end) l.rows.forEach(function (r) { vals.push(numeric(r[e.column], e.type)); });
      });
      vals = vals.filter(isFinite).sort(function (a, b) { return a - b; });
      var gap = Infinity;
      for (var i = 1; i < vals.length; i++) if (vals[i] > vals[i - 1]) gap = Math.min(gap, vals[i] - vals[i - 1]);
      if (!vals.length || !isFinite(gap)) return sc;
      var lo = vals[0] - gap / 2, hi = vals[vals.length - 1] + gap / 2;
      refs.extra.x.forEach(function (v) { v = numeric(v, sc.enc.type); if (isFinite(v)) { lo = Math.min(lo, v); hi = Math.max(hi, v); } });
      var padded = sc.enc && sc.enc.type === 'temporal' ? time([lo, hi], [0, width]) : linear([lo, hi], [0, width], false);
      padded.enc = sc.enc;
      return padded;
    };
    var x = padForBars(makeScale(allLayers, 'x', [0, w], hasBars && horizontalBars, refs.extra.x), w);
    var y = makeScale(allLayers, 'y', [h, 0], hasBars && !horizontalBars, refs.extra.y);
    if (y && y.kind === 'linear' && x && x.kind === 'band' && hasBars) {
      var sums = stackTotals('y', 'x');
      if (sums.length) { var hi = Math.max.apply(null, sums.concat([y.domain[1]])); y = linear([Math.min(0, y.domain[0]), hi], [h, 0], true); y.enc = allLayers[0].encoding.y; }
    }
    // Formatted numbers on the y axis ($150,000) need room too; the x scale is then rebuilt narrower.
    if (y && y.kind !== 'band') {
      var longest = 0;
      y.ticks(Math.max(2, Math.floor(h / 40))).forEach(function (t) { longest = Math.max(longest, tickLabel(y, t).length); });
      var need = Math.min(170, 30 + 7.6 * longest);
      if (need > left) {
        left = need;
        w = panelW - left - M.right;
        x = padForBars(makeScale(allLayers, 'x', [0, w], hasBars && horizontalBars, refs.extra.x), w);
      }
    }
    if (x && x.kind === 'linear' && y && y.kind === 'band' && hasBars) {
      var sx = stackTotals('x', 'y');
      if (sx.length) { var hx = Math.max.apply(null, sx.concat([x.domain[1]])); x = linear([Math.min(0, x.domain[0]), hx], [0, w], true); x.enc = allLayers[0].encoding.x; }
    }
    if (!x) x = band([''], [0, w]);
    var sizeEnc = null;
    allLayers.forEach(function (l) { if (l.encoding.size && l.encoding.size.column) sizeEnc = l.encoding.size; });
    var size = function () { return 3.5; };
    if (sizeEnc) {
      var sv = channelValues(allLayers, 'size').map(Number).filter(isFinite), smax = Math.max.apply(null, sv.concat([1]));
      size = function (v) { return 2 + 10 * Math.sqrt(Math.max(0, Number(v)) / smax); };
    }
    var color = colorScale(allLayers);
    var grid = NB.el('div', { 'class': 'nb-facets', style: 'grid-template-columns:repeat(' + facetCols.length + ',1fr)' });
    facetRows.forEach(function (fr) {
      facetCols.forEach(function (fc) {
        var panel = NB.el('div', { 'class': 'nb-facet' });
        if (render.facet) panel.appendChild(NB.el('div', { 'class': 'nb-facet-title', text: [fr, fc].filter(function (v) { return v !== null; }).join(' · ') }));
        var svgEl = s('svg', { viewBox: '0 0 ' + panelW + ' ' + panelH, width: panelW, 'class': 'nb-chart-svg', preserveAspectRatio: 'xMidYMid meet' });
        panel.appendChild(svgEl);
        var g = s('g', { transform: 'translate(' + left + ',' + M.top + ')' }, svgEl);
        axis(g, x, 'bottom', w, h, x.enc && x.enc.title);
        axis(g, y, 'left', w, h, y && y.enc && y.enc.title, left);
        var refLabels = drawReferences(g, refs, x, y, w, h);
        var plot = s('g', { 'class': 'nb-plot' }, g);
        plot.__points = [];
        plot.__bars = [];
        plot.__dots = [];
        allLayers.forEach(function (l) {
          var rows = l.rows.filter(function (r) {
            return (fr === null || key(r.facet_row) === key(fr)) && (fc === null || key(r.facet_col) === key(fc));
          });
          drawLayer(plot, Object.assign({}, l, { rows: rows }), x, y, color, w, h, size);
        });
        refLabels();
        drawNotes(g, render.notes, x, y, w, h, plot, refs.boxes || []);
        var sel = render.selection;
        if (sel && sel.type === 'interval') brush(g, sel, x, y, w, h, opts);
        if (sel && sel.type === 'point') pointSelect(svgEl, sel, allLayers, opts);
        hoverPoints(svgEl, g, plot.__points, plot.__bars, w, h);
        grid.appendChild(panel);
      });
    });
    container.appendChild(grid);
    legend(container, color);
  };

  NB.chart = function (container, chart, opts) {
    container.innerHTML = '';
    if (!chart) return;
    if (chart.error) { container.appendChild(NB.el('div', { 'class': 'nb-error', text: chart.error })); return; }
    var render = chart.render || {}, datasets = (chart.datasets || []).map(NB.objects);
    // A stable wobble per chart: the same chart is drawn the same way every time.
    Sketch.reseed((opts && opts.seed) || ((render.title || '') + JSON.stringify(render.layers || render.kind).length + '/' + datasets.map(function (d) { return d.length; }).join(',')));
    // In a query cell's tabs the title is already the tab's label.
    if (render.title && !(opts && opts.inTab)) container.appendChild(NB.el('div', { 'class': 'nb-chart-title', text: render.title }));
    var body = NB.el('div', { 'class': 'nb-chart-body' });
    container.appendChild(body);
    try {
      if (render.kind === 'grammar') NB.grammar(body, render, datasets, opts || {});
      else if (NB.typed && NB.typed[render.kind]) NB.typed[render.kind](body, render, datasets, opts || {}, chart);
      else body.appendChild(NB.el('div', { 'class': 'nb-error', text: 'Cannot draw chart kind ' + render.kind }));
    } catch (e) {
      body.appendChild(NB.el('div', { 'class': 'nb-error', text: 'Chart failed to draw: ' + e.message }));
    }
  };

  NB.scales = { linear: linear, band: band, time: time, color: colorScale, axis: axis, numeric: numeric, distinct: distinct, tooltip: tooltip, margin: M };
})(NB);
