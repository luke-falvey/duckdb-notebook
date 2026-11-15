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

  function makeScale(layers, channel, range, zero) {
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
    var nums = values.map(function (v) { return numeric(v, enc.type); }).filter(isFinite);
    var lo = nums.length ? Math.min.apply(null, nums) : 0, hi = nums.length ? Math.max.apply(null, nums) : 1;
    var scaleOpt = enc.scale || {};
    if (scaleOpt.domain && scaleOpt.domain.length === 2) { lo = numeric(scaleOpt.domain[0], enc.type); hi = numeric(scaleOpt.domain[1], enc.type); }
    else if (zero && scaleOpt.zero !== false && enc.type !== 'temporal') { lo = Math.min(0, lo); hi = Math.max(0, hi); }
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
      var domain = distinct(values), idx = {};
      domain.forEach(function (d, i) { idx[key(d)] = i; });
      f = function (v) { return NB.palette[(idx[key(v)] || 0) % NB.palette.length]; };
      f.domain = domain;
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

  function axis(g, scale, orient, w, h, title) {
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
        s('line', { x1: p, x2: p, y1: h, y2: h + 4 }, a);
        var tx = s('text', { x: p, y: h + 16, 'text-anchor': 'middle' }, a);
        var label = tickLabel(scale, t);
        tx.textContent = label.length > 16 ? label.slice(0, 15) + '…' : label;
      } else {
        if (scale.kind !== 'band') s('line', { x1: 0, x2: w, y1: p, y2: p, 'class': 'nb-grid-line' }, a);
        var ty = s('text', { x: -6, y: p + 4, 'text-anchor': 'end' }, a);
        var l2 = tickLabel(scale, t);
        ty.textContent = l2.length > 10 ? l2.slice(0, 9) + '…' : l2;
      }
    });
    if (orient === 'bottom') s('line', { x1: 0, x2: w, y1: h, y2: h, 'class': 'nb-domain' }, a);
    else s('line', { x1: 0, x2: 0, y1: 0, y2: h, 'class': 'nb-domain' }, a);
    if (title) {
      var tt = orient === 'bottom' ? s('text', { x: w / 2, y: h + 34, 'text-anchor': 'middle', 'class': 'nb-axis-title' }, a)
        : s('text', { transform: 'translate(' + (-M.left + 12) + ',' + h / 2 + ') rotate(-90)', 'text-anchor': 'middle', 'class': 'nb-axis-title' }, a);
      tt.textContent = title;
    }
  }

  function legend(container, color) {
    if (!color.enc || color.enc.value !== undefined) return;
    var box = NB.el('div', { 'class': 'nb-legend' });
    if (color.domain) {
      color.domain.slice(0, 20).forEach(function (d) {
        box.appendChild(NB.el('span', { 'class': 'nb-legend-item' }, [NB.el('i', { style: 'background:' + color(d) }), d === null ? 'NULL' : String(d)]));
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

  function tooltip(el, layer, row) {
    var lines = [];
    ['x', 'y', 'color', 'size', 'text'].forEach(function (c) {
      var e = layer.encoding[c];
      if (e && e.column && row[e.column] !== undefined) lines.push((e.title || c) + ': ' + (e.type === 'temporal' ? NB.formatDate(row[e.column]) : NB.formatValue(row[e.column], '', e.format)));
    });
    (layer.tooltip || []).forEach(function (e) { lines.push((e.title || e.column) + ': ' + NB.formatValue(row[e.column], '', e.format)); });
    if (layer.binned && row.count != null) lines.push('rows: ' + NB.formatNumber(row.count, ','));
    s('title', {}, el).textContent = lines.join('\n');
  }

  // ---- Marks ----------------------------------------------------------------------------------

  function groupBy(rows, column) {
    var groups = {}, order = [];
    rows.forEach(function (r) { var k = column ? key(r[column]) : ''; if (!groups[k]) { groups[k] = []; order.push(k); } groups[k].push(r); });
    return order.map(function (k) { return groups[k]; });
  }

  function pos(scale, v, type) { return scale.kind === 'band' ? scale(v) : scale(numeric(v, type)); }

  function drawLayer(g, layer, x, y, color, w, h, size) {
    var enc = layer.encoding, ex = enc.x, ey = enc.y, ec = enc.color, rows = layer.rows;
    var colorOf = function (r) { return ec && ec.column ? color(r[ec.column]) : (ec && ec.value) || NB.palette[0]; };
    var opacity = layer.mark_opacity != null ? layer.mark_opacity : null;
    var mark = layer.mark;
    if (mark === 'line' || mark === 'area') {
      groupBy(rows, ec && ec.column).forEach(function (group) {
        var pts = group.map(function (r) {
          var px = x.kind === 'band' ? x.center(r[ex.column]) : x(numeric(r[ex.column], ex.type));
          return [px, y(numeric(r[ey.column], ey.type)), r];
        }).filter(function (p) { return isFinite(p[0]) && isFinite(p[1]); }).sort(function (a, b) { return a[0] - b[0]; });
        if (!pts.length) return;
        var d = 'M' + pts.map(function (p) { return p[0].toFixed(1) + ',' + p[1].toFixed(1); }).join('L');
        var c = colorOf(group[0]);
        if (mark === 'area') {
          var base = y(Math.max(y.domain ? y.domain[0] : 0, 0));
          s('path', { d: d + 'L' + pts[pts.length - 1][0].toFixed(1) + ',' + base + 'L' + pts[0][0].toFixed(1) + ',' + base + 'Z', fill: c, 'fill-opacity': opacity != null ? opacity : 0.35, stroke: 'none' }, g);
        }
        s('path', { d: d, fill: 'none', stroke: c, 'stroke-width': layer.mark_strokeWidth || 2, 'class': 'nb-line' }, g);
        if (pts.length <= 60 || layer.mark_point) pts.forEach(function (p) { tooltip(s('circle', { cx: p[0], cy: p[1], r: 3, fill: c, 'class': 'nb-pt' }, g), layer, p[2]); });
      });
    } else if (mark === 'bar') {
      var horizontal = y.kind === 'band' && x.kind !== 'band';
      var stack = {};
      rows.forEach(function (r) {
        var c = colorOf(r), el;
        if (horizontal) {
          var yb = y(r[ey.column]), v = numeric(r[ex.column], ex.type), k = key(r[ey.column]);
          var start = stack[k] || 0; stack[k] = start + v;
          var x0 = x(start), x1 = x(start + v);
          el = s('rect', { x: Math.min(x0, x1), y: yb, width: Math.abs(x1 - x0), height: y.bandwidth, fill: c, 'class': 'nb-mark' }, g);
        } else if (x.kind === 'band') {
          var xb = x(r[ex.column]), vy = numeric(r[ey.column], ey.type), kx = key(r[ex.column]);
          var st = stack[kx] || 0; stack[kx] = st + vy;
          var y0 = y(st), y1 = y(st + vy);
          el = s('rect', { x: xb, y: Math.min(y0, y1), width: x.bandwidth, height: Math.abs(y1 - y0), fill: c, 'class': 'nb-mark' }, g);
        } else {
          var a = x(numeric(r[ex.column], ex.type)), b = ex.end ? x(numeric(r[ex.end], ex.type)) : a + 4;
          var yy = y(numeric(r[ey.column], ey.type)), zero = y(0);
          el = s('rect', { x: Math.min(a, b) + 0.5, y: Math.min(yy, zero), width: Math.max(1, Math.abs(b - a) - 1), height: Math.abs(zero - yy), fill: c, 'class': 'nb-mark' }, g);
        }
        if (layer.mark_cornerRadius) el.setAttribute('rx', layer.mark_cornerRadius);
        el.__row = r;
        tooltip(el, layer, r);
      });
    } else if (mark === 'point' || mark === 'tick' || mark === 'text') {
      var maxCount = 1;
      if (layer.binned) rows.forEach(function (r) { maxCount = Math.max(maxCount, r.count); });
      rows.forEach(function (r) {
        var px = x.kind === 'band' ? x.center(r[ex.column]) : x(numeric(r[ex.column], ex.type));
        var py = y ? (y.kind === 'band' ? y.center(r[ey.column]) : y(numeric(r[ey.column], ey.type))) : h / 2;
        if (!isFinite(px) || !isFinite(py)) return;
        var el;
        if (mark === 'text') {
          el = s('text', { x: px, y: py, 'text-anchor': 'middle', 'class': 'nb-mark-text', fill: colorOf(r) }, g);
          el.textContent = NB.formatValue(r[enc.text && enc.text.column], '', enc.text && enc.text.format);
        } else if (mark === 'tick') {
          el = s('line', { x1: px - 6, x2: px + 6, y1: py, y2: py, stroke: colorOf(r), 'stroke-width': 2, 'class': 'nb-mark' }, g);
        } else {
          var radius = layer.binned ? 1.5 + 4 * Math.sqrt(r.count / maxCount) : enc.size && enc.size.column ? size(r[enc.size.column]) : 3.5;
          var fill = layer.binned && !(ec && ec.column) ? NB.ramp('blues')(0.3 + 0.7 * Math.sqrt(r.count / maxCount)) : colorOf(r);
          el = s('circle', { cx: px, cy: py, r: radius, fill: fill, 'fill-opacity': opacity != null ? opacity : 0.75, 'class': 'nb-mark' }, g);
        }
        el.__row = r;
        tooltip(el, layer, r);
      });
    } else if (mark === 'rect') {
      rows.forEach(function (r) {
        var x0 = pos(x, r[ex.column], ex.type), y0 = pos(y, r[ey.column], ey.type);
        var wd = x.kind === 'band' ? x.bandwidth : Math.abs(x(numeric(r[ex.end || ex.column], ex.type)) - x0) || 4;
        var ht = y.kind === 'band' ? y.bandwidth : Math.abs(y(numeric(r[ey.end || ey.column], ey.type)) - y0) || 4;
        var el = s('rect', { x: x0, y: y.kind === 'band' ? y0 : y0 - ht, width: wd, height: ht, fill: colorOf(r), 'class': 'nb-mark' }, g);
        el.__row = r;
        tooltip(el, layer, r);
      });
    } else if (mark === 'rule') {
      rows.forEach(function (r) {
        var c = colorOf(r), el;
        if (ex && ex.column && !(ey && ey.column)) { var px = pos(x, r[ex.column], ex.type); el = s('line', { x1: px, x2: px, y1: 0, y2: h, stroke: c, 'stroke-width': 1.5 }, g); }
        else if (ey && ey.column && !(ex && ex.column)) { var py = pos(y, r[ey.column], ey.type); el = s('line', { x1: 0, x2: w, y1: py, y2: py, stroke: c, 'stroke-width': 1.5, 'stroke-dasharray': '4 3' }, g); }
        else if (ex && ey) {
          var e2 = enc.x2 || enc.y2;
          var X1 = pos(x, r[ex.column], ex.type), Y1 = pos(y, r[ey.column], ey.type);
          var X2 = enc.x2 && enc.x2.column ? pos(x, r[enc.x2.column], ex.type) : X1, Y2 = enc.y2 && enc.y2.column ? pos(y, r[enc.y2.column], ey.type) : Y1;
          if (e2) el = s('line', { x1: X1, x2: X2, y1: Y1, y2: Y2, stroke: c, 'stroke-width': 1.5 }, g);
        }
        if (el) tooltip(el, layer, r);
      });
    } else if (mark === 'boxplot') {
      rows.forEach(function (r) {
        var cx = x ? x.center(r.x) : w / 2, bw = x ? Math.min(40, x.bandwidth) : 40, c = NB.palette[0];
        var q1 = y(r.y_q1), q3 = y(r.y_q3), md = y(r.y_median), lo = y(r.y_min), hi = y(r.y_max);
        s('line', { x1: cx, x2: cx, y1: lo, y2: hi, stroke: c }, g);
        var box = s('rect', { x: cx - bw / 2, y: Math.min(q1, q3), width: bw, height: Math.abs(q1 - q3), fill: c, 'fill-opacity': 0.3, stroke: c }, g);
        s('line', { x1: cx - bw / 2, x2: cx + bw / 2, y1: md, y2: md, stroke: c, 'stroke-width': 2 }, g);
        s('title', {}, box).textContent = 'min ' + NB.formatNumber(r.y_min) + '\nq1 ' + NB.formatNumber(r.y_q1) + '\nmedian ' + NB.formatNumber(r.y_median) + '\nq3 ' + NB.formatNumber(r.y_q3) + '\nmax ' + NB.formatNumber(r.y_max);
      });
    }
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
      var el = s('path', { d: d, fill: enc.color && enc.color.column ? color(r[enc.color.column]) : NB.palette[0], stroke: 'var(--nb-bg)', 'class': 'nb-mark' }, svgEl);
      el.__row = r;
      tooltip(el, layer, r);
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
    var w = panelW - M.left - M.right, h = panelH - M.top - M.bottom;
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
    var x = makeScale(allLayers, 'x', [0, w], hasBars && horizontalBars);
    var y = makeScale(allLayers, 'y', [h, 0], hasBars && !horizontalBars);
    if (y && y.kind === 'linear' && x && x.kind === 'band' && hasBars) {
      var sums = stackTotals('y', 'x');
      if (sums.length) { var hi = Math.max.apply(null, sums.concat([y.domain[1]])); y = linear([Math.min(0, y.domain[0]), hi], [h, 0], true); y.enc = allLayers[0].encoding.y; }
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
        var g = s('g', { transform: 'translate(' + M.left + ',' + M.top + ')' }, svgEl);
        axis(g, x, 'bottom', w, h, x.enc && x.enc.title);
        axis(g, y, 'left', w, h, y && y.enc && y.enc.title);
        var plot = s('g', { 'class': 'nb-plot' }, g);
        allLayers.forEach(function (l) {
          var rows = l.rows.filter(function (r) {
            return (fr === null || key(r.facet_row) === key(fr)) && (fc === null || key(r.facet_col) === key(fc));
          });
          drawLayer(plot, Object.assign({}, l, { rows: rows }), x, y, color, w, h, size);
        });
        var sel = render.selection;
        if (sel && sel.type === 'interval') brush(g, sel, x, y, w, h, opts);
        if (sel && sel.type === 'point') pointSelect(svgEl, sel, allLayers, opts);
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
    if (render.title) container.appendChild(NB.el('div', { 'class': 'nb-chart-title', text: render.title }));
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
