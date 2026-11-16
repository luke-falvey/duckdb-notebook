// Hand-drawn SVG primitives (the "Option A" look), in the spirit of rough.js, which Excalidraw uses:
// strokes wobble and are drawn twice, corners overshoot, fills are hachures. Drawing is seeded, so a
// chart looks the same every time it is drawn, in the editor, an export and a PDF.
var Sketch = (function () {
  'use strict';
  var NS = 'http://www.w3.org/2000/svg';
  var seed = 7;
  function rnd() {
    seed |= 0; seed = seed + 0x6D2B79F5 | 0;
    var t = Math.imul(seed ^ seed >>> 15, 1 | seed);
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
    return ((t ^ t >>> 14) >>> 0) / 4294967296;
  }
  function off(r) { return (rnd() * 2 - 1) * r; }
  function f(n) { return Math.round(n * 10) / 10; }

  var S = { ink: 'currentColor' };

  // Seeds from a string, so each chart has its own stable wobble.
  S.reseed = function (key) {
    if (typeof key === 'number') { seed = key; return; }
    var h = 2166136261;
    String(key).split('').forEach(function (c) { h = Math.imul(h ^ c.charCodeAt(0), 16777619); });
    seed = h >>> 0;
  };

  S.el = function (tag, attrs, parent) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs || {}) if (attrs[k] != null) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  };

  function strokeD(x1, y1, x2, y2, rough, overshoot) {
    var dx = x2 - x1, dy = y2 - y1, len = Math.sqrt(dx * dx + dy * dy) || 1;
    var o = Math.min(2.4, 0.4 + len * 0.012) * rough;
    var nx = -dy / len, ny = dx / len, bow = off(Math.min(len * 0.015, 3) * rough);
    var ex = overshoot ? dx / len * off(overshoot) : 0, ey = overshoot ? dy / len * off(overshoot) : 0;
    var c1x = x1 + dx * 0.35 + nx * bow + off(o), c1y = y1 + dy * 0.35 + ny * bow + off(o);
    var c2x = x1 + dx * 0.7 + nx * bow + off(o), c2y = y1 + dy * 0.7 + ny * bow + off(o);
    return 'M' + f(x1 + off(o) - ex) + ',' + f(y1 + off(o) - ey) + 'C' + f(c1x) + ',' + f(c1y) + ' ' + f(c2x) + ',' + f(c2y) +
      ' ' + f(x2 + off(o) + ex) + ',' + f(y2 + off(o) + ey);
  }

  function opts(o) {
    o = o || {};
    return {
      stroke: o.stroke || S.ink, width: o.width || 1.5, rough: o.rough == null ? 1 : o.rough, double: o.double !== false,
      fill: o.fill, hachure: o.hachure || 1.3, gap: o.gap || 6, angle: o.angle == null ? -41 : o.angle,
      fillStyle: o.fillStyle || 'hachure', dash: o.dash, opacity: o.opacity, fillOpacity: o.fillOpacity
    };
  }

  // Hatching leaves gaps the pointer falls through; an invisible copy of a filled shape underneath
  // makes all of it answer hover and clicks.
  function hit(g, d) {
    S.el('path', { d: d, fill: 'transparent', stroke: 'none', 'class': 'sk-hit' }, g);
  }

  function path(parent, d, o) {
    return S.el('path', {
      d: d, fill: 'none', stroke: o.stroke, 'stroke-width': o.width, 'stroke-linecap': 'round', 'stroke-linejoin': 'round',
      'stroke-dasharray': o.dash, opacity: o.opacity
    }, parent);
  }

  S.line = function (parent, x1, y1, x2, y2, o) {
    o = opts(o);
    var d = strokeD(x1, y1, x2, y2, o.rough, 2);
    if (o.double) d += strokeD(x1, y1, x2, y2, o.rough, 2);
    return path(parent, d, o);
  };

  var clipId = 0;
  // Fills an arbitrary shape (an SVG path) with hachures, cross-hatching or a solid wash.
  S.fill = function (parent, shapeD, box, o) {
    o = opts(o);
    if (!o.fill || o.fill === 'none') return null;
    if (o.fillStyle === 'solid' || box.w * box.h < 30) {
      return S.el('path', { d: shapeD, fill: o.fill, stroke: 'none', opacity: o.fillOpacity }, parent);
    }
    var g = S.el('g', {}, parent);
    var id = 'skc' + Math.random().toString(36).slice(2, 8) + (++clipId);
    var cp = S.el('clipPath', { id: id }, g);
    S.el('path', { d: shapeD }, cp);
    var lines = S.el('g', { 'clip-path': 'url(#' + id + ')' }, g);
    var angles = o.fillStyle === 'cross-hatch' ? [o.angle, o.angle + 90] : [o.angle];
    angles.forEach(function (deg) {
      var a = deg * Math.PI / 180, cx = box.x + box.w / 2, cy = box.y + box.h / 2;
      var r = Math.sqrt(box.w * box.w + box.h * box.h) / 2 + 4, d = '';
      var ux = Math.cos(a), uy = Math.sin(a), vx = -uy, vy = ux;
      for (var t = -r; t <= r; t += o.gap) {
        var px = cx + vx * t, py = cy + vy * t;
        d += strokeD(px - ux * r, py - uy * r, px + ux * r, py + uy * r, 0.5, 0);
      }
      S.el('path', { d: d, fill: 'none', stroke: o.fill, 'stroke-width': o.hachure, 'stroke-linecap': 'round', opacity: o.fillOpacity }, lines);
    });
    return g;
  };

  S.rect = function (parent, x, y, w, h, o) {
    o = opts(o);
    var g = S.el('g', {}, parent);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (o.fill) {
      var area = 'M' + x + ',' + y + 'h' + w + 'v' + h + 'h' + -w + 'Z';
      hit(g, area);
      S.fill(g, area, { x: x, y: y, w: w, h: h }, o);
    }
    if (o.stroke !== 'none' && w > 0.5 && h > 0.5) {
      var d = '', ov = Math.min(3, w / 6, h / 6);
      for (var pass = 0; pass < (o.double ? 2 : 1); pass++) {
        d += strokeD(x, y, x + w, y, o.rough, ov) + strokeD(x + w, y, x + w, y + h, o.rough, ov) +
          strokeD(x + w, y + h, x, y + h, o.rough, ov) + strokeD(x, y + h, x, y, o.rough, ov);
      }
      path(g, d, o);
    }
    return g;
  };

  // A rectangle with rounded, hand-drawn corners: the outline of cells and cards.
  S.roundRect = function (parent, x, y, w, h, o) {
    o = opts(o);
    var r = Math.min(12, w / 4, h / 4), g = S.el('g', {}, parent);
    var shape = 'M' + (x + r) + ',' + y + 'H' + (x + w - r) + 'Q' + (x + w) + ',' + y + ' ' + (x + w) + ',' + (y + r) + 'V' + (y + h - r) +
      'Q' + (x + w) + ',' + (y + h) + ' ' + (x + w - r) + ',' + (y + h) + 'H' + (x + r) + 'Q' + x + ',' + (y + h) + ' ' + x + ',' + (y + h - r) +
      'V' + (y + r) + 'Q' + x + ',' + y + ' ' + (x + r) + ',' + y + 'Z';
    if (o.fill) S.fill(g, shape, { x: x, y: y, w: w, h: h }, o);
    if (o.stroke === 'none') return g;
    var d = '';
    for (var pass = 0; pass < (o.double ? 2 : 1); pass++) {
      var j = function () { return off(1.1 * o.rough); };
      d += 'M' + f(x + r + j()) + ',' + f(y + j()) + 'L' + f(x + w - r + j()) + ',' + f(y + j()) +
        'Q' + f(x + w + j()) + ',' + f(y + j()) + ' ' + f(x + w + j()) + ',' + f(y + r + j()) +
        'L' + f(x + w + j()) + ',' + f(y + h - r + j()) + 'Q' + f(x + w + j()) + ',' + f(y + h + j()) + ' ' + f(x + w - r + j()) + ',' + f(y + h + j()) +
        'L' + f(x + r + j()) + ',' + f(y + h + j()) + 'Q' + f(x + j()) + ',' + f(y + h + j()) + ' ' + f(x + j()) + ',' + f(y + h - r + j()) +
        'L' + f(x + j()) + ',' + f(y + r + j()) + 'Q' + f(x + j()) + ',' + f(y + j()) + ' ' + f(x + r + j() + 2) + ',' + f(y + j());
    }
    path(g, d, o);
    return g;
  };

  function smooth(pts) {
    if (pts.length < 2) return '';
    var d = 'M' + f(pts[0][0]) + ',' + f(pts[0][1]);
    for (var i = 0; i < pts.length - 1; i++) {
      var p0 = pts[i - 1] || pts[i], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2] || p2;
      d += 'C' + f(p1[0] + (p2[0] - p0[0]) / 6) + ',' + f(p1[1] + (p2[1] - p0[1]) / 6) + ' ' +
        f(p2[0] - (p3[0] - p1[0]) / 6) + ',' + f(p2[1] - (p3[1] - p1[1]) / 6) + ' ' + f(p2[0]) + ',' + f(p2[1]);
    }
    return d;
  }
  S.smooth = smooth;

  S.ellipse = function (parent, cx, cy, rx, ry, o) {
    o = opts(o);
    var g = S.el('g', {}, parent);
    var shape = 'M' + (cx - rx) + ',' + cy + 'a' + rx + ',' + ry + ' 0 1,0 ' + 2 * rx + ',0a' + rx + ',' + ry + ' 0 1,0 ' + -2 * rx + ',0';
    if (o.fill) { hit(g, shape); S.fill(g, shape, { x: cx - rx, y: cy - ry, w: 2 * rx, h: 2 * ry }, o); }
    if (o.stroke === 'none') return g;
    var d = '';
    for (var pass = 0; pass < (o.double ? 2 : 1); pass++) {
      var start = rnd() * Math.PI * 2, pts = [], n = rx < 6 ? 8 : 14;
      for (var i = 0; i <= n + 1; i++) {
        var a = start + i / n * Math.PI * 2 * 1.06, k = 1 + off(0.04 * o.rough);
        pts.push([cx + Math.cos(a) * rx * k, cy + Math.sin(a) * ry * k]);
      }
      d += smooth(pts);
    }
    path(g, d, o);
    return g;
  };

  // A wobbly line through points: line charts, sparklines, squiggles.
  S.polyline = function (parent, pts, o) {
    o = opts(o);
    var d = '';
    for (var pass = 0; pass < (o.double ? 2 : 1); pass++) {
      d += smooth(pts.map(function (p) { return [p[0] + off(0.6 * o.rough), p[1] + off(0.6 * o.rough)]; }));
    }
    return path(parent, d, o);
  };

  // Any shape given as a path: hachure fill plus a stroke drawn twice, slightly apart.
  S.shape = function (parent, d, box, o) {
    o = opts(o);
    var g = S.el('g', {}, parent);
    if (o.fill) { hit(g, d); S.fill(g, d, box, o); }
    if (o.stroke !== 'none') {
      path(g, d, o);
      if (o.double) path(g, d, o).setAttribute('transform', 'translate(' + f(off(0.8)) + ',' + f(off(0.8)) + ')');
    }
    return g;
  };

  // A curved arrow with an open arrowhead, as Excalidraw draws them.
  S.arrow = function (parent, x1, y1, x2, y2, o) {
    var bend = o && o.bend != null ? o.bend : 0.25;
    o = opts(o);
    var mx = (x1 + x2) / 2 - (y2 - y1) * bend, my = (y1 + y2) / 2 + (x2 - x1) * bend;
    var g = S.el('g', {}, parent), d = '';
    for (var pass = 0; pass < 2; pass++) {
      d += 'M' + f(x1 + off(1)) + ',' + f(y1 + off(1)) + 'Q' + f(mx + off(2)) + ',' + f(my + off(2)) + ' ' + f(x2 + off(0.5)) + ',' + f(y2 + off(0.5));
    }
    path(g, d, o);
    var a = Math.atan2(y2 - my, x2 - mx), L = 9;
    [0.45, -0.45].forEach(function (s) {
      S.line(g, x2, y2, x2 - L * Math.cos(a + s), y2 - L * Math.sin(a + s), { stroke: o.stroke, width: o.width, rough: 0.5 });
    });
    return g;
  };

  // Draws a hand-drawn outline behind an HTML element and keeps it the element's size.
  //   kind: round (default), rect, underline, ellipse.
  S.outline = function (el, o) {
    o = o || {};
    el.classList.add('sk-box');
    var draw = function () {
      var old = el.querySelector(':scope > svg.sk-outline');
      var w = el.offsetWidth, h = el.offsetHeight;
      if (!w || !h) return;
      if (old && +old.getAttribute('width') === w && +old.getAttribute('height') === h) return;
      if (old) old.remove();
      S.reseed(o.seed || el.dataset.seed || (w + 'x' + h));
      var svg = S.el('svg', { 'class': 'sk-outline', width: w, height: h, viewBox: '0 0 ' + w + ' ' + h });
      el.insertBefore(svg, el.firstChild);
      var style = { stroke: o.stroke, width: o.width || 1.5, fill: o.fill, fillStyle: o.fillStyle || 'solid', rough: o.rough, dash: o.dash };
      if (o.kind === 'rect') S.rect(svg, 1, 1, w - 2, h - 2, style);
      else if (o.kind === 'underline') S.line(svg, 2, h - 2, w - 2, h - 2, style);
      else if (o.kind === 'ellipse') S.ellipse(svg, w / 2, h / 2, w / 2 + 3, h / 2 + 2, style);
      else S.roundRect(svg, 1, 1, w - 2, h - 2, style);
    };
    draw();
    if (window.ResizeObserver && !el.__skObserver) {
      var timer = null;
      el.__skObserver = new ResizeObserver(function () { clearTimeout(timer); timer = setTimeout(draw, 60); });
      el.__skObserver.observe(el);
    }
    el.__skRedraw = draw;
    return el;
  };

  return S;
})();
