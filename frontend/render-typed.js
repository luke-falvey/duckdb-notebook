// Typed charts (FORMAT.md section 5.3): kpi, table, pivot, map, sankey, chord, treemap, sunburst,
// icicle, network, funnel, candlestick, calendar, gantt, gauge, and raw vega-lite / echarts specs.
// All drawn as SVG or HTML with no library, so exports stay small and print as vectors.
(function (NB) {
  'use strict';
  var s = NB.svg, T = {};

  // A hand-drawn box in a palette colour with its pastel hachure (solid when asked).
  function box(parent, x, y, w, h, stroke, solid) {
    return Sketch.rect(parent, x, y, w, h, { stroke: stroke, fill: solid ? stroke : NB.pastel(stroke), fillStyle: solid ? 'solid' : 'hachure', width: 1.3, gap: Math.max(4, Math.min(7, Math.min(w, h) / 3)) });
  }
  // A hand-drawn version of a filled SVG path given as attributes ({d: ...}).
  function shape(parent, stroke, attrs) {
    // Charts are drawn before they are attached, so getBBox is unavailable: hatch the whole chart
    // area and let the shape's clip path cut it out.
    var root = parent.ownerSVGElement || parent, vb = (root.getAttribute('viewBox') || '0 0 640 360').split(' ').map(Number);
    return Sketch.shape(parent, attrs.d, { x: vb[0], y: vb[1], w: vb[2], h: vb[3] }, { stroke: stroke, fill: NB.pastel(stroke), width: 1.3, gap: 6 });
  }
  NB.typed = T;

  function svgBox(container, w, h) {
    return s('svg', { viewBox: '0 0 ' + w + ' ' + h, width: w, 'class': 'nb-chart-svg', preserveAspectRatio: 'xMidYMid meet' }, container);
  }
  function title(el, text) { el.setAttribute('data-tip', text); return el; }
  function fmt(v, enc) { return NB.formatValue(v, '', enc && enc.format); }
  function colorFor(values) {
    var idx = {};
    NB.scales.distinct(values).forEach(function (v, i) { idx[NB.key(v)] = i; });
    return function (v) { return NB.palette[(idx[NB.key(v)] || 0) % NB.palette.length]; };
  }

  T.kpi = function (c, render, data) {
    var spec = render.spec || {}, enc = render.encoding || {}, row = (data[0] || [])[0] || {};
    var box = NB.el('div', { 'class': 'nb-kpi' });
    box.appendChild(NB.el('div', { 'class': 'nb-kpi-label', text: spec.label || (enc.value && enc.value.title) || '' }));
    box.appendChild(NB.el('div', { 'class': 'nb-kpi-value', text: fmt(row.value, enc.value) }));
    if (row.compare != null) {
      var delta = row.value - row.compare, pct = row.compare ? delta / Math.abs(row.compare) : null;
      box.appendChild(NB.el('div', { 'class': 'nb-kpi-delta ' + (delta >= 0 ? 'up' : 'down'), text: (delta >= 0 ? '▲ ' : '▼ ') + NB.formatNumber(Math.abs(delta), (enc.value || {}).format) + (pct != null ? ' (' + NB.formatNumber(pct, '.1%') + ')' : '') }));
    }
    c.appendChild(box);
    if (data[1] && data[1].length) {
      var rows = data[1], w = 240, h = 48;
      var xs = rows.map(function (r) { return NB.scales.numeric(r.x, enc.sparkline && enc.sparkline.x.type); }), ys = rows.map(function (r) { return Number(r.y); });
      var x = NB.scales.linear([Math.min.apply(null, xs), Math.max.apply(null, xs)], [2, w - 2]), y = NB.scales.linear([Math.min.apply(null, ys), Math.max.apply(null, ys)], [h - 2, 2]);
      var svg = s('svg', { viewBox: '0 0 ' + w + ' ' + h, width: w, height: h, 'class': 'nb-sparkline' }, box);
      Sketch.polyline(svg, xs.map(function (v, i) { return [x(v), y(ys[i])]; }), { stroke: NB.palette[0], width: 2 });
    }
  };

  T.gauge = function (c, render, data) {
    var spec = render.spec || {}, enc = render.encoding || {}, v = Number(((data[0] || [])[0] || {}).value);
    var lo = +(spec.min != null ? spec.min : 0), hi = +(spec.max != null ? spec.max : Math.max(1, v)), w = 240, h = 150, cx = 120, cy = 130, R = 100;
    var svg = svgBox(c, w, h);
    var at = function (t) { var a = Math.PI * (1 - t); return [cx + R * Math.cos(a), cy - R * Math.sin(a)]; };
    var arc = function (t0, t1, color, width) {
      var p0 = at(t0), p1 = at(t1);
      Sketch.shape(svg, 'M' + p0[0] + ',' + p0[1] + 'A' + R + ',' + R + ' 0 0 1 ' + p1[0] + ',' + p1[1], { x: 0, y: 0, w: w, h: h }, { stroke: color, width: width, fill: null });
    };
    arc(0, 1, 'var(--nb-muted-bg)', 18);
    (spec.bands || []).forEach(function (b, i) {
      arc((b.from - lo) / (hi - lo), (b.to - lo) / (hi - lo), b.color || NB.palette[i], 6);
    });
    var t = Math.max(0, Math.min(1, (v - lo) / ((hi - lo) || 1)));
    arc(0, t || 0.0001, NB.palette[0], 18);
    var label = s('text', { x: cx, y: cy - 10, 'text-anchor': 'middle', 'class': 'nb-gauge-value' }, svg);
    label.textContent = fmt(v, enc.value);
    var cap = s('text', { x: cx, y: cy + 14, 'text-anchor': 'middle', 'class': 'nb-axis' }, svg);
    cap.textContent = spec.label || '';
  };

  T.table = function (c, render, data, opts, chart) {
    var spec = render.spec || {}, formats = {}, bars = [], heat = [];
    var columns = spec.columns || [];
    (Array.isArray(columns) ? [] : []).forEach(function () {});
    Object.keys(spec.format || {}).forEach(function (k) { formats[k] = spec.format[k]; });
    if (spec.bar) bars = [].concat(spec.bar);
    if (spec.heat) heat = [].concat(spec.heat);
    NB.table(c, chart.datasets[0], { formats: formats, bars: bars, heat: heat });
  };

  T.pivot = function (c, render, data, opts, chart) {
    var spec = render.spec || {};
    NB.table(c, chart.datasets[0], { heat: spec.heat ? (chart.datasets[0].columns || []).slice((render.rows || []).length).map(function (col) { return col.name; }) : [] });
  };

  // ---- Map: an equirectangular projection fitted to the data; no basemap (nothing is fetched).
  T.map = function (c, render, data) {
    var rows = data[0] || [], w = render.width || 640, h = render.height || 360, spec = render.spec || {};
    var svg = svgBox(c, w, h), points = [];
    var geos = rows.filter(function (r) { return r.geojson; }).map(function (r) { try { return { geo: JSON.parse(r.geojson), row: r }; } catch (e) { return null; } }).filter(Boolean);
    function walk(coords, f) { if (typeof coords[0] === 'number') f(coords); else coords.forEach(function (x) { walk(x, f); }); }
    geos.forEach(function (g) { walk(g.geo.coordinates, function (p) { points.push([p[1], p[0]]); }); });
    rows.forEach(function (r) {
      if (r.lat != null) points.push([r.lat, r.lon]);
      if (r.lat2 != null) points.push([r.lat2, r.lon2]);
    });
    if (!points.length) { c.appendChild(NB.el('div', { 'class': 'nb-empty', text: 'No locations' })); return; }
    var la = points.map(function (p) { return p[0]; }), lo = points.map(function (p) { return p[1]; });
    var lat0 = Math.min.apply(null, la), lat1 = Math.max.apply(null, la), lon0 = Math.min.apply(null, lo), lon1 = Math.max.apply(null, lo);
    var k = Math.cos((lat0 + lat1) / 2 * Math.PI / 180) || 1;
    var sx = (w - 20) / ((lon1 - lon0) * k || 1), sy = (h - 20) / ((lat1 - lat0) || 1), sc = Math.min(sx, sy);
    var proj = function (lat, lon) { return [10 + ((lon - lon0) * k) * sc + ((w - 20) - (lon1 - lon0) * k * sc) / 2, h - 10 - (lat - lat0) * sc - ((h - 20) - (lat1 - lat0) * sc) / 2]; };
    s('rect', { x: 0, y: 0, width: w, height: h, 'class': 'nb-map-bg' }, svg);
    var colorValues = rows.map(function (r) { return r.color; }).filter(function (v) { return v != null; });
    var numericColor = colorValues.length && colorValues.every(function (v) { return typeof v === 'number'; });
    var cmin = Math.min.apply(null, colorValues), cmax = Math.max.apply(null, colorValues), ramp = NB.ramp((render.encoding && render.encoding.color && render.encoding.color.scheme) || (spec.color && spec.color.scheme) || 'viridis');
    var cat = colorFor(colorValues);
    var colorOf = function (r) { return r.color == null ? NB.palette[0] : numericColor ? ramp((r.color - cmin) / ((cmax - cmin) || 1)) : cat(r.color); };
    geos.forEach(function (g) {
      var d = '';
      var ring = function (coords) { d += 'M' + coords.map(function (p) { var q = proj(p[1], p[0]); return q[0].toFixed(1) + ',' + q[1].toFixed(1); }).join('L') + (g.geo.type.indexOf('Polygon') >= 0 ? 'Z' : ''); };
      var t = g.geo.type, cs = g.geo.coordinates;
      if (t === 'Polygon' || t === 'MultiLineString') cs.forEach(ring);
      else if (t === 'MultiPolygon') cs.forEach(function (poly) { poly.forEach(ring); });
      else if (t === 'LineString') ring(cs);
      else if (t === 'Point') { var q = proj(cs[1], cs[0]); title(s('circle', { cx: q[0], cy: q[1], r: 4, fill: colorOf(g.row) }, svg), String(g.row.color)); return; }
      title(s('path', { d: d, fill: t.indexOf('Polygon') >= 0 ? colorOf(g.row) : 'none', stroke: t.indexOf('Polygon') >= 0 ? 'var(--nb-bg)' : colorOf(g.row), 'stroke-width': 1, 'class': 'nb-mark' }, svg), g.row.color != null ? String(g.row.color) : '');
    });
    var maxCount = Math.max.apply(null, rows.map(function (r) { return r.count || 1; }));
    var smax = Math.max.apply(null, rows.map(function (r) { return Number(r.size) || 1; }));
    rows.forEach(function (r) {
      if (r.lat == null) return;
      var p = proj(r.lat, r.lon);
      if (r.lat2 != null) {
        var q = proj(r.lat2, r.lon2), mx = (p[0] + q[0]) / 2, my = (p[1] + q[1]) / 2 - Math.hypot(q[0] - p[0], q[1] - p[1]) / 4;
        s('path', { d: 'M' + p.join(',') + 'Q' + mx + ',' + my + ' ' + q.join(','), fill: 'none', stroke: colorOf(r), 'stroke-opacity': 0.6 }, svg);
        return;
      }
      if (render.binned) {
        var cell = Math.max(3, Math.min(w, h) / 200 * sc / sc * 3);
        title(s('rect', { x: p[0] - cell / 2, y: p[1] - cell / 2, width: cell, height: cell, fill: ramp(Math.sqrt((r.color != null ? r.color - cmin : r.count) / ((r.color != null ? cmax - cmin : maxCount) || 1))) }, svg), 'rows: ' + r.count + (r.color != null ? '\nvalue: ' + NB.formatNumber(r.color) : ''));
      } else {
        title(s('circle', { cx: p[0], cy: p[1], r: r.size != null ? 2 + 8 * Math.sqrt(r.size / smax) : 3, fill: colorOf(r), 'fill-opacity': 0.75, 'class': 'nb-mark' }, svg), r.lat + ', ' + r.lon + (r.color != null ? '\n' + r.color : ''));
      }
    });
  };

  // ---- Flows ------------------------------------------------------------------------------------
  T.sankey = function (c, render, data) {
    var links = data[0] || [], w = render.width || 640, h = render.height || 360, nodes = {}, order = [];
    links.forEach(function (l) {
      [l.source, l.target].forEach(function (n) { if (!nodes[n]) { nodes[n] = { name: n, inV: 0, outV: 0, layer: 0 }; order.push(n); } });
      nodes[l.source].outV += +l.value; nodes[l.target].inV += +l.value;
    });
    // Layers by longest path from a source (links that would loop are ignored for layering).
    for (var pass = 0; pass < order.length; pass++) links.forEach(function (l) { if (l.source !== l.target && nodes[l.target].layer < nodes[l.source].layer + 1 && nodes[l.source].layer < order.length) nodes[l.target].layer = nodes[l.source].layer + 1; });
    var layers = [];
    order.forEach(function (n) { var L = Math.min(nodes[n].layer, order.length); (layers[L] = layers[L] || []).push(nodes[n]); });
    layers = layers.filter(Boolean);
    var nodeW = 12, gap = 8, svg = svgBox(c, w, h);
    var maxTotal = Math.max.apply(null, layers.map(function (col) { return col.reduce(function (a, n) { return a + Math.max(n.inV, n.outV); }, 0); }));
    var k = (h - gap * (Math.max.apply(null, layers.map(function (l) { return l.length; })) + 1)) / (maxTotal || 1);
    var color = colorFor(order);
    layers.forEach(function (col, li) {
      var y = gap, x = layers.length > 1 ? 10 + li * (w - 20 - nodeW - 90) / (layers.length - 1) : w / 2;
      col.forEach(function (n) {
        n.x = x; n.y = y; n.h = Math.max(1, Math.max(n.inV, n.outV) * k); n.outY = y; n.inY = y; y += n.h + gap;
        title(box(svg, x, n.y, nodeW, n.h, color(n.name)), n.name + ': ' + NB.formatNumber(Math.max(n.inV, n.outV)));
        var t = s('text', { x: x + nodeW + 4, y: n.y + n.h / 2 + 4, 'class': 'nb-node-label' }, svg); t.textContent = n.name;
      });
    });
    links.forEach(function (l) {
      var a = nodes[l.source], b = nodes[l.target], th = Math.max(1, l.value * k);
      var x0 = a.x + nodeW, x1 = b.x, y0 = a.outY + th / 2, y1 = b.inY + th / 2, mx = (x0 + x1) / 2;
      a.outY += th; b.inY += th;
      title(s('path', { d: 'M' + x0 + ',' + y0 + 'C' + mx + ',' + y0 + ' ' + mx + ',' + y1 + ' ' + x1 + ',' + y1, fill: 'none', stroke: color(l.source), 'stroke-opacity': 0.35, 'stroke-width': th }, svg), l.source + ' → ' + l.target + ': ' + NB.formatNumber(l.value));
    });
  };

  T.chord = function (c, render, data) {
    var links = data[0] || [], size = Math.min(render.width || 480, render.height || 480, 480), R = size / 2 - 40, cx = size / 2, cy = size / 2;
    var names = NB.scales.distinct([].concat.apply([], links.map(function (l) { return [l.source, l.target]; }))), totals = {};
    names.forEach(function (n) { totals[n] = 0; });
    links.forEach(function (l) { totals[l.source] += +l.value; totals[l.target] += +l.value; });
    var sum = names.reduce(function (a, n) { return a + totals[n]; }, 0) || 1, pad = 0.02, angle = 0, arcs = {}, color = colorFor(names);
    var svg = svgBox(c, size, size), at = function (r, a) { return [cx + r * Math.cos(a - Math.PI / 2), cy + r * Math.sin(a - Math.PI / 2)]; };
    names.forEach(function (n) {
      var span = totals[n] / sum * (2 * Math.PI - pad * names.length);
      arcs[n] = { a0: angle, a1: angle + span, cur: angle };
      var p0 = at(R, angle), p1 = at(R, angle + span), q1 = at(R + 10, angle + span), q0 = at(R + 10, angle);
      title(shape(svg, color(n), { d: 'M' + p0 + 'A' + R + ',' + R + ' 0 ' + (span > Math.PI ? 1 : 0) + ' 1 ' + p1 + 'L' + q1 + 'A' + (R + 10) + ',' + (R + 10) + ' 0 ' + (span > Math.PI ? 1 : 0) + ' 0 ' + q0 + 'Z', fill: color(n) }, svg), n);
      var lp = at(R + 24, angle + span / 2), t = s('text', { x: lp[0], y: lp[1], 'text-anchor': 'middle', 'class': 'nb-node-label' }, svg); t.textContent = n;
      angle += span + pad;
    });
    links.forEach(function (l) {
      var A = arcs[l.source], B = arcs[l.target], sa = l.value / sum * (2 * Math.PI - pad * names.length);
      var a0 = A.cur, a1 = A.cur + sa; A.cur = a1;
      var b0 = B.cur, b1 = B.cur + sa; B.cur = b1;
      var p = function (a) { return at(R, a).join(','); };
      title(s('path', { d: 'M' + p(a0) + 'A' + R + ',' + R + ' 0 0 1 ' + p(a1) + 'Q' + cx + ',' + cy + ' ' + p(b0) + 'A' + R + ',' + R + ' 0 0 1 ' + p(b1) + 'Q' + cx + ',' + cy + ' ' + p(a0) + 'Z', fill: color(l.source), 'fill-opacity': 0.5 }, svg), l.source + ' → ' + l.target + ': ' + NB.formatNumber(l.value));
    });
  };

  // ---- Hierarchies --------------------------------------------------------------------------------
  function hierarchy(rows, depth) {
    var root = { name: '', children: {}, value: 0 };
    rows.forEach(function (r) {
      var node = root;
      node.value += +r.value || 0;
      for (var i = 0; i < depth; i++) {
        var k = r['path_' + i] == null ? '(none)' : String(r['path_' + i]);
        node = node.children[k] = node.children[k] || { name: k, children: {}, value: 0, parent: node, depth: i + 1 };
        node.value += +r.value || 0;
      }
    });
    (function list(n) { n.kids = Object.keys(n.children).map(function (k) { return n.children[k]; }).sort(function (a, b) { return b.value - a.value; }); n.kids.forEach(list); })(root);
    return root;
  }
  function pathOf(n) { var p = []; while (n && n.depth) { p.unshift(n.name); n = n.parent; } return p.join(' / '); }

  T.treemap = function (c, render, data) {
    var w = render.width || 640, h = render.height || 360, root = hierarchy(data[0] || [], render.depth || 1), svg = svgBox(c, w, h);
    var color = colorFor(root.kids.map(function (k) { return k.name; }));
    (function layout(node, x, y, ww, hh, top) {
      var kids = node.kids, total = node.value || 1, horizontal = ww >= hh, off = 0;
      kids.forEach(function (k) {
        var share = k.value / total, kx = horizontal ? x + off * ww : x, ky = horizontal ? y : y + off * hh;
        var kw = horizontal ? share * ww : ww, kh = horizontal ? hh : share * hh;
        off += share;
        var top2 = top || k;
        if (!k.kids.length) {
          title(box(svg, kx + 1.5, ky + 1.5, Math.max(0, kw - 3), Math.max(0, kh - 3), color(top2.name)), pathOf(k) + ': ' + NB.formatNumber(k.value));
          if (kw > 40 && kh > 14) { var t = s('text', { x: kx + 4, y: ky + 13, 'class': 'nb-node-label nb-on-color' }, svg); t.textContent = k.name; }
        } else layout(k, kx, ky, kw, kh, top2);
      });
    })(root, 0, 0, w, h, null);
  };

  T.icicle = function (c, render, data) {
    var w = render.width || 640, depth = render.depth || 1, rowH = 32, h = rowH * depth, root = hierarchy(data[0] || [], depth), svg = svgBox(c, w, h);
    var color = colorFor(root.kids.map(function (k) { return k.name; }));
    (function layout(node, x, ww, top) {
      var off = x;
      node.kids.forEach(function (k) {
        var kw = k.value / (node.value || 1) * ww, top2 = top || k;
        title(box(svg, off + 1.5, (k.depth - 1) * rowH + 1.5, Math.max(0, kw - 3), rowH - 3, color(top2.name)), pathOf(k) + ': ' + NB.formatNumber(k.value));
        if (kw > 40) { var t = s('text', { x: off + 4, y: (k.depth - 1) * rowH + 20, 'class': 'nb-node-label nb-on-color' }, svg); t.textContent = k.name; }
        layout(k, off, kw, top2);
        off += kw;
      });
    })(root, 0, w, null);
  };

  T.sunburst = function (c, render, data) {
    var size = Math.min(render.width || 480, render.height || 480, 480), depth = render.depth || 1, ring = (size / 2 - 10) / (depth + 0.5), cx = size / 2, cy = size / 2;
    var root = hierarchy(data[0] || [], depth), svg = svgBox(c, size, size), color = colorFor(root.kids.map(function (k) { return k.name; }));
    var at = function (r, a) { return (cx + r * Math.cos(a - Math.PI / 2)).toFixed(1) + ',' + (cy + r * Math.sin(a - Math.PI / 2)).toFixed(1); };
    (function layout(node, a0, a1, top) {
      var a = a0;
      node.kids.forEach(function (k) {
        var span = k.value / (node.value || 1) * (a1 - a0), r0 = ring * (k.depth - 0.5), r1 = r0 + ring, top2 = top || k, large = span > Math.PI ? 1 : 0;
        title(shape(svg, color(top2.name), { d: 'M' + at(r0, a) + 'L' + at(r1, a) + 'A' + r1 + ',' + r1 + ' 0 ' + large + ' 1 ' + at(r1, a + span) + 'L' + at(r0, a + span) + 'A' + r0 + ',' + r0 + ' 0 ' + large + ' 0 ' + at(r0, a) + 'Z', fill: color(top2.name), 'fill-opacity': 1 - 0.15 * (k.depth - 1), stroke: 'var(--nb-bg)' }, svg), pathOf(k) + ': ' + NB.formatNumber(k.value));
        layout(k, a, a + span, top2);
        a += span;
      });
    })(root, 0, Math.PI * 2, null);
  };

  // ---- Network: a few hundred iterations of a simple force layout. ------------------------------
  T.network = function (c, render, data) {
    var links = data[0] || [], w = render.width || 640, h = render.height || 420, svg = svgBox(c, w, h);
    var names = NB.scales.distinct([].concat.apply([], links.map(function (l) { return [l.source, l.target]; }))).slice(0, 2000);
    var nodes = names.map(function (n, i) { var a = i / names.length * 2 * Math.PI; return { name: n, x: w / 2 + Math.cos(a) * w / 4, y: h / 2 + Math.sin(a) * h / 4, vx: 0, vy: 0, deg: 0 }; });
    var byName = {};
    nodes.forEach(function (n) { byName[n.name] = n; });
    var edges = links.filter(function (l) { return byName[l.source] && byName[l.target]; }).map(function (l) { byName[l.source].deg++; byName[l.target].deg++; return { a: byName[l.source], b: byName[l.target], v: +l.value }; });
    var ideal = Math.sqrt(w * h / Math.max(1, nodes.length)) * 0.6;
    for (var it = 0; it < 250; it++) {
      var cool = 1 - it / 250;
      for (var i = 0; i < nodes.length; i++) for (var j = i + 1; j < nodes.length; j++) {
        var a = nodes[i], b = nodes[j], dx = a.x - b.x, dy = a.y - b.y, d2 = dx * dx + dy * dy + 0.01, f = ideal * ideal / d2;
        a.vx += dx * f * 0.05; a.vy += dy * f * 0.05; b.vx -= dx * f * 0.05; b.vy -= dy * f * 0.05;
      }
      edges.forEach(function (e) { var dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, d = Math.sqrt(dx * dx + dy * dy) + 0.01, f = (d - ideal) / d * 0.1; e.a.vx += dx * f; e.a.vy += dy * f; e.b.vx -= dx * f; e.b.vy -= dy * f; });
      nodes.forEach(function (n) { n.vx += (w / 2 - n.x) * 0.01; n.vy += (h / 2 - n.y) * 0.01; n.x += Math.max(-10, Math.min(10, n.vx)) * cool; n.y += Math.max(-10, Math.min(10, n.vy)) * cool; n.vx *= 0.5; n.vy *= 0.5; n.x = Math.max(8, Math.min(w - 8, n.x)); n.y = Math.max(8, Math.min(h - 8, n.y)); });
    }
    var vmax = Math.max.apply(null, edges.map(function (e) { return e.v; }).concat([1]));
    edges.forEach(function (e) { title(edges.length > 400 ? s('line', { x1: e.a.x, y1: e.a.y, x2: e.b.x, y2: e.b.y, stroke: '#adb5bd', 'stroke-width': 0.5 + 2.5 * e.v / vmax }, svg) : Sketch.line(svg, e.a.x, e.a.y, e.b.x, e.b.y, { stroke: '#adb5bd', width: 0.5 + 2.5 * e.v / vmax, double: false }), e.a.name + ' — ' + e.b.name + ': ' + NB.formatNumber(e.v)); });
    var dmax = Math.max.apply(null, nodes.map(function (n) { return n.deg; }).concat([1]));
    nodes.forEach(function (n) {
      title(nodes.length > 300 ? s('circle', { cx: n.x, cy: n.y, r: 3 + 6 * Math.sqrt(n.deg / dmax), fill: NB.palette[0] }, svg) : Sketch.ellipse(svg, n.x, n.y, 3 + 6 * Math.sqrt(n.deg / dmax), 3 + 6 * Math.sqrt(n.deg / dmax), { stroke: NB.palette[0], fill: NB.pastels[0], fillStyle: 'solid', width: 1.2 }), n.name + ' (' + n.deg + ' links)');
      if (nodes.length <= 60) { var t = s('text', { x: n.x + 8, y: n.y + 4, 'class': 'nb-node-label' }, svg); t.textContent = n.name; }
    });
  };

  T.funnel = function (c, render, data) {
    var rows = data[0] || [], w = render.width || 640, rowH = 36, h = rows.length * rowH, svg = svgBox(c, w, h);
    var max = Math.max.apply(null, rows.map(function (r) { return +r.value; }).concat([1]));
    rows.forEach(function (r, i) {
      var bw = r.value / max * (w - 200), next = rows[i + 1] ? rows[i + 1].value / max * (w - 200) : bw, x = 120 + (w - 200 - bw) / 2, y = i * rowH;
      title(shape(svg, NB.palette[0], { d: 'M' + x + ',' + y + 'L' + (x + bw) + ',' + y + 'L' + (120 + (w - 200 + next) / 2) + ',' + (y + rowH - 2) + 'L' + (120 + (w - 200 - next) / 2) + ',' + (y + rowH - 2) + 'Z', fill: NB.palette[0], 'fill-opacity': 1 - i * 0.6 / rows.length }, svg), r.stage + ': ' + NB.formatNumber(r.value));
      var t = s('text', { x: 112, y: y + rowH / 2 + 4, 'text-anchor': 'end', 'class': 'nb-node-label' }, svg); t.textContent = r.stage;
      var v = s('text', { x: w - 72, y: y + rowH / 2 + 4, 'class': 'nb-node-label' }, svg);
      v.textContent = NB.formatNumber(r.value) + (i ? ' (' + NB.formatNumber(r.value / rows[0].value, '.0%') + ')' : '');
    });
  };

  function timeAxes(c, render, rows, getX, yDomain) {
    var M = NB.scales.margin, w = (render.width || 640) - M.left - M.right, h = (render.height || 320) - M.top - M.bottom;
    var svg = svgBox(c, w + M.left + M.right, h + M.top + M.bottom), g = s('g', { transform: 'translate(' + M.left + ',' + M.top + ')' }, svg);
    var xs = rows.map(getX).filter(isFinite);
    var x = NB.scales.time([Math.min.apply(null, xs), Math.max.apply(null, xs)], [0, w]);
    x.enc = { type: 'temporal' };
    var y = yDomain.band ? NB.scales.band(yDomain.band, [0, h]) : NB.scales.linear(yDomain, [h, 0], true);
    y.enc = { type: yDomain.band ? 'nominal' : 'quantitative' };
    NB.scales.axis(g, x, 'bottom', w, h);
    NB.scales.axis(g, y, 'left', w, h);
    return { g: g, x: x, y: y, w: w, h: h };
  }

  T.candlestick = function (c, render, data) {
    var rows = data[0] || [];
    if (!rows.length) return;
    var lows = rows.map(function (r) { return +r.low; }), highs = rows.map(function (r) { return +r.high; });
    var a = timeAxes(c, render, rows, function (r) { return +NB.toDate(r.x); }, [Math.min.apply(null, lows), Math.max.apply(null, highs)]);
    var bw = Math.max(1, a.w / rows.length * 0.6);
    rows.forEach(function (r) {
      var x = a.x(+NB.toDate(r.x)), up = +r.close >= +r.open, col = up ? '#009E73' : '#D55E00';
      Sketch.line(a.g, x, a.y(+r.high), x, a.y(+r.low), { stroke: col, width: 1.1, double: false });
      title(box(a.g, x - bw / 2, a.y(Math.max(+r.open, +r.close)), bw, Math.max(1, Math.abs(a.y(+r.open) - a.y(+r.close))), col, true),
        NB.formatDate(r.x) + '\nopen ' + r.open + '\nhigh ' + r.high + '\nlow ' + r.low + '\nclose ' + r.close);
    });
  };

  T.gantt = function (c, render, data) {
    var rows = data[0] || [];
    if (!rows.length) return;
    var tasks = NB.scales.distinct(rows.map(function (r) { return r.task; }));
    var ends = rows.map(function (r) { return +NB.toDate(r.end); });
    var a = timeAxes(c, render, rows.concat(rows.map(function (r) { return { start: r.end }; })), function (r) { return +NB.toDate(r.start); }, { band: tasks });
    var color = colorFor(rows.map(function (r) { return r.color != null ? r.color : r.task; }));
    rows.forEach(function (r, i) {
      var x0 = a.x(+NB.toDate(r.start)), x1 = a.x(ends[i]);
      title(box(a.g, x0, a.y(r.task), Math.max(1, x1 - x0), a.y.bandwidth, color(r.color != null ? r.color : r.task)), r.task + '\n' + NB.formatDate(r.start) + ' → ' + NB.formatDate(r.end));
    });
  };

  T.calendar = function (c, render, data) {
    var rows = data[0] || [], cell = 12, values = {};
    if (!rows.length) return;
    rows.forEach(function (r) { values[String(r.date).slice(0, 10)] = +r.value; });
    var dates = rows.map(function (r) { return NB.toDate(String(r.date).slice(0, 10)); });
    var first = new Date(Math.min.apply(null, dates)), last = new Date(Math.max.apply(null, dates));
    first.setDate(first.getDate() - first.getDay());
    var weeks = Math.ceil((last - first) / 6.048e8) + 1, w = weeks * cell + 40, h = 7 * cell + 30, svg = svgBox(c, w, h);
    var nums = rows.map(function (r) { return +r.value; }), lo = Math.min.apply(null, nums), hi = Math.max.apply(null, nums), ramp = NB.ramp((render.spec && render.spec.scheme) || 'greens');
    var d = new Date(first), lastMonth = -1;
    for (var i = 0; i < weeks * 7; i++) {
      var k = d.getFullYear() + '-' + ('0' + (d.getMonth() + 1)).slice(-2) + '-' + ('0' + d.getDate()).slice(-2), v = values[k];
      var x = 30 + Math.floor(i / 7) * cell, y = 18 + d.getDay() * cell;
      title(s('rect', { x: x, y: y, width: cell - 2, height: cell - 2, rx: 2, fill: v == null ? 'var(--nb-muted-bg)' : ramp(0.1 + 0.9 * (v - lo) / ((hi - lo) || 1)) }, svg), k + (v == null ? '' : ': ' + NB.formatNumber(v)));
      if (d.getDay() === 0 && d.getMonth() !== lastMonth) { lastMonth = d.getMonth(); var t = s('text', { x: x, y: 12, 'class': 'nb-axis' }, svg); t.textContent = NB.formatDate(d, 'month').slice(0, 3); }
      d.setDate(d.getDate() + 1);
    }
    ['S', 'M', 'T', 'W', 'T', 'F', 'S'].forEach(function (l, i) { if (i % 2) { var t = s('text', { x: 0, y: 18 + i * cell + 9, 'class': 'nb-axis' }, svg); t.textContent = l; } });
  };

  // Raw Vega-Lite: marks and encodings that map onto the grammar renderer are drawn with it. No
  // Vega library is bundled, so transforms, params and signals are not supported.
  T['vega-lite'] = function (c, render, data, opts, chart) {
    var spec = (render.spec && render.spec.spec) || {}, enc = spec.encoding || {}, mark = typeof spec.mark === 'string' ? spec.mark : (spec.mark || {}).type;
    var layer = { mark: mark === 'circle' || mark === 'square' ? 'point' : mark, data: 0, encoding: {} };
    var columns = (chart.datasets[0] || {}).columns || [];
    ['x', 'y', 'color', 'size', 'text', 'theta'].forEach(function (ch) {
      var e = enc[ch];
      if (!e) return;
      if (e.value !== undefined) { layer.encoding[ch] = { value: e.value }; return; }
      var col = columns.find(function (cc) { return cc.name === e.field; });
      var type = { quantitative: 'quantitative', temporal: 'temporal', ordinal: 'ordinal', nominal: 'nominal' }[e.type] || (col && NB.isNumericType(col.type) ? 'quantitative' : col && NB.isTemporalType(col.type) ? 'temporal' : 'nominal');
      layer.encoding[ch] = { column: e.field, field: e.field, type: type, title: e.title || e.field, format: e.format };
    });
    if (!layer.mark) { c.appendChild(NB.el('div', { 'class': 'nb-error', text: 'This Vega-Lite spec has no mark the built-in renderer can draw' })); return; }
    NB.grammar(c, { kind: 'grammar', width: spec.width || render.width, height: spec.height || render.height, layers: [layer] }, data, opts);
  };

  // Raw ECharts: line, bar, scatter and pie series with `encode` are drawn through the grammar
  // renderer; anything else is shown as a table. No ECharts library is bundled.
  T.echarts = function (c, render, data, opts, chart) {
    var option = (render.spec && render.spec.spec) || {}, series = [].concat(option.series || [])[0] || {};
    var map = { line: 'line', bar: 'bar', scatter: 'point', pie: 'arc' }, mark = map[series.type], e = series.encode || {};
    if (!mark || (!e.x && !e.value)) { NB.table(c, chart.datasets[0]); return; }
    var columns = (chart.datasets[0] || {}).columns || [];
    var typeOf = function (f) { var col = columns.find(function (cc) { return cc.name === f; }); return col && NB.isNumericType(col.type) ? 'quantitative' : col && NB.isTemporalType(col.type) ? 'temporal' : 'nominal'; };
    var layer = { mark: mark, data: 0, encoding: {} };
    if (mark === 'arc') { layer.encoding.theta = { column: e.value, type: 'quantitative', title: e.value }; layer.encoding.color = { column: e.itemName, type: 'nominal', title: e.itemName }; }
    else { layer.encoding.x = { column: e.x, type: typeOf(e.x), title: e.x }; layer.encoding.y = { column: e.y, type: typeOf(e.y), title: e.y }; }
    NB.grammar(c, { kind: 'grammar', width: render.width, height: render.height, layers: [layer] }, data, opts);
  };
})(NB);
