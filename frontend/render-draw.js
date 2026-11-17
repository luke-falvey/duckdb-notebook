// The draw-in animation: a notebook appears as if sketched in front of you. Hand-drawn strokes draw
// themselves (stroke-dashoffset over a normalised pathLength, so nothing is measured), text is written
// in with a left-to-right wipe, and plain shapes and fills fade in. Cells go top to bottom, each one
// outline first, then its text, then its charts' axes and marks.
//
// Draw.page(cells, opts) animates cell elements that are on screen (or just below it); it runs on
// opening a notebook and on explicit runs, never on ordinary redraws. Any click or key press finishes
// it at once. It is skipped for prefers-reduced-motion and finished before printing.
var Draw = (function () {
  'use strict';
  var ID = 'nb-draw';
  var D = {};
  var pending = typeof WeakMap === 'function' ? new WeakMap() : null;
  var passStart = 0;

  D.reduced = function () {
    return !!(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  };

  D.finishAll = function () {
    if (!document.getAnimations) return;
    document.getAnimations().forEach(function (a) { if (a.id === ID) a.finish(); });
  };
  document.addEventListener('pointerdown', D.finishAll, true);
  document.addEventListener('keydown', D.finishAll, true);
  window.addEventListener('beforeprint', D.finishAll);

  D.running = function () {
    return document.getAnimations ? document.getAnimations().filter(function (a) { return a.id === ID && a.playState === 'running'; }).length : 0;
  };

  function play(el, frames, delay, duration, easing, done) {
    if (!el.animate) return;
    var a = el.animate(frames, { duration: duration, delay: Math.max(0, delay), fill: 'backwards', easing: easing || 'ease-out' });
    a.id = ID;
    if (done) a.onfinish = done;
  }

  // One stroke draws itself. Dashes restart on each subpath, so a hatch path's lines all draw at once.
  function stroke(path, delay, duration) {
    path.setAttribute('pathLength', '1');
    path.style.strokeDasharray = '1';
    play(path, [{ strokeDashoffset: 1 }, { strokeDashoffset: 0 }], delay, duration, 'ease-in-out', function () {
      path.removeAttribute('pathLength');
      path.style.strokeDasharray = '';
    });
  }
  function fade(el, delay, duration) { play(el, [{ opacity: 0 }, { opacity: 1 }], delay, duration); }
  function wipe(el, delay, duration, down) {
    play(el, [{ clipPath: down ? 'inset(0 0 100% 0)' : 'inset(0 100% 0 0)' }, { clipPath: 'inset(0 0 0 0)' }], delay, duration);
  }

  function drawable(path) {
    if (path.classList.contains('sk-hit')) return false;
    var s = path.getAttribute('stroke');
    return s && s !== 'none' && !path.getAttribute('stroke-dasharray') && path.getAttribute('d');
  }

  // The marks of one SVG, in document order: strokes draw, everything else fades. Large charts fade as a whole.
  var SKIP = 'nb-brush-overlay nb-brush nb-hover-band nb-hover-dot sk-hit'.split(' ');
  function svg(el, start, span) {
    var parts = Array.prototype.filter.call(el.querySelectorAll('path, rect, circle, ellipse, line, polyline, polygon, text'), function (p) {
      return !SKIP.some(function (c) { return p.classList.contains(c); }) && !p.closest('clipPath');
    });
    if (parts.length > 400) { fade(el, start, 500); return; }
    var step = parts.length ? span / parts.length : 0;
    parts.forEach(function (p, k) {
      var at = start + k * step;
      if (p.tagName === 'path' && drawable(p)) stroke(p, at, 420);
      else fade(p, at, 280);
    });
  }

  function outline(box, delay) {
    var o = box.querySelector(':scope > svg.sk-outline');
    if (o) {
      Array.prototype.forEach.call(o.querySelectorAll('path'), function (p) { if (drawable(p)) stroke(p, delay, 520); });
      return true;
    }
    return false;
  }

  // Text blocks and controls, written in; tables are revealed from the top down.
  var TEXT = '.nb-margin, .nb-cell-head, .nb-md > *, .nb-input, .nb-code-wrap, pre.nb-source, .nb-code-label, .nb-tabs, .nb-chart-title, ' +
    '.nb-kpi-label, .nb-kpi-value, .nb-grid-wrap, .nb-grid-footer, .nb-legend, .nb-foot, .nb-stub, .nb-switcher, .nb-error';
  var SVGS = 'svg:not(.sk-outline):not(.nb-head-rule)';

  function cell(box, start, part) {
    var scope = part === 'output' ? box.querySelectorAll('.nb-output') : [box];
    var items = [];
    Array.prototype.forEach.call(scope, function (root) {
      Array.prototype.forEach.call(root.querySelectorAll(TEXT), function (el) { if (!el.parentElement.closest('.nb-md > *, .nb-grid-wrap')) items.push(el); });
      Array.prototype.forEach.call(root.querySelectorAll(SVGS), function (el) { if (!el.parentElement.closest('svg')) items.push(el); });
    });
    items.sort(function (a, b) { return a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1; });
    var t = start;
    if (part !== 'output') {
      // The outline exists once the cell has been laid out; if it is drawn later, draw it in then.
      if (!outline(box, t) && pending) pending.set(box, t);
      t += 160;
    }
    items.forEach(function (el) {
      if (el.tagName === 'svg') { svg(el, t, 520); t += 380; }
      else { wipe(el, t, el.classList.contains('nb-grid-wrap') ? 520 : 380, el.classList.contains('nb-grid-wrap')); t += 110; }
    });
    return t - start;
  }

  // Sketch.outline calls this when it first draws a cell's outline after the pass started.
  Sketch.onOutline = function (box) {
    if (!pending || !pending.has(box)) return;
    var at = pending.get(box);
    pending.delete(box);
    outline(box, at - (performance.now() - passStart));
  };

  // Animates the given cell elements. opts.part: 'cell' (default; outline and contents) or 'output'.
  // Cells more than a screen below the fold are left as they are.
  D.page = function (cells, opts) {
    opts = opts || {};
    if (D.reduced() || !document.body.animate) return;
    D.finishAll();
    passStart = performance.now();
    var limit = window.innerHeight * 2, t = 0;
    Array.prototype.forEach.call(cells, function (box) {
      var r = box.getBoundingClientRect();
      if (r.bottom < 0 || r.top > limit) return;
      var used = cell(box, t, opts.part);
      t += Math.min(used, 260);
    });
  };

  return D;
})();
