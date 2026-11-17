// Handwriting: every part of a notebook is written onto the page by a pen (docs/HANDWRITING.md).
//
// The page is built exactly as it would be without this file; Ink lays an animation over it and
// takes it away again, leaving no trace in the DOM. Callers name the pieces they build:
//
//   Ink.present(element, key, signature)
//
// A key seen for the first time, or with a new signature, is written; the same key and signature
// is left alone, or, if it is still being written, the new element takes over at the same point.
//
//  - Shapes (SVG paths from Sketch) are drawn along their length with stroke-dashoffset. A path with
//    several subpaths is split into single-subpath copies while it is drawn, then put back.
//  - Text stays in the DOM, its glyphs made transparent. A canvas over it writes each real glyph,
//    revealed through a fat copy of the pen strokes recovered from the font (ink-strokes.js).
//  - Everything else (controls, fills, backgrounds, plain marks) fades in when the pen reaches it.
//
// Timing is a function of the clock and a job's start, never of what was drawn before, so a
// rebuilt element continues where the old one was. One scheduler owns every frame.
var Ink = (function () {
  'use strict';
  var I = {};
  var SVGNS = 'http://www.w3.org/2000/svg';

  // ---- Settings -----------------------------------------------------------------------------
  // Times in ms, lengths in CSS px. "slow" stretches every time by SLOW.
  var C = {
    budget: 2500,       // everything on screen when a page opens finishes within this
    blockMax: 1200,     // no single piece takes longer
    blockMin: 260,      // nor shorter, unless the budget says so
    speed: 2.6,         // px of pen travel per ms
    stagger: 0.3,       // the next cell starts when this fraction of the previous one is done
    within: 0.2,        // and the next piece of the same cell after this fraction of the previous piece
    glyphMin: 110,      // a glyph is written in at least this long, so the writing can be seen
    pathMin: 170,       // and a stroke drawn in at least this long
    fadeMin: 200,
    capGlyphs: 60,      // at most this many glyphs in motion at once
    capPaths: 150,      // and this many SVG strokes
    quick: 200,         // an update that follows the reader's own action
    recent: 1000,       // what counts as following it
    lift: 0.3,          // pen travel (in em) for lifting the pen between strokes
    space: 0.45,        // and for moving between words
    tableRows: 3,       // rows written letter by letter before a table sweeps row by row
    plainGroup: 60      // a group with more plain marks than this fades in as one
  };

  // Watch mode (ink=watch, the gear menu's "watch"): one pen writes the page in reading order, at a
  // pace a person can follow, and the page follows the pen. Times are on the pen's clock, which runs
  // at the reader's chosen speed and stops while they pause (docs/HANDWRITING.md, "Watch mode").
  var W = {
    text: 0.4,          // px of a glyph's pen travel per ms
    glyphMin: 60,       // and no glyph quicker than this
    shape: 0.8,         // px per ms along a drawn stroke
    shapeMin: 110,      // a stroke takes at least this long
    shapeMax: 1000,     // and at most this
    hatchLine: 34,      // one line of hatching
    hatch: 900,         // all the hatching of one fill, at most
    hatchAll: 6000,     // and of one piece
    fade: 260,          // a control or a background, faded in
    fill: 150,          // a solid shape in a chart
    dot: 40,            // one plain mark
    chunk: 280,         // a handful of the plain marks after the first ones
    marks: 60,          // strokes (or plain marks) drawn one by one at full pace before the pen hurries
    hurry: 5000,        // the rest of a piece's strokes or marks, together
    rows: 6,            // table rows written letter by letter
    row: 220,           // and each row after them, swept at once
    letter: 15,         // the pen lifted between letters,
    word: 110,          // between words,
    line: 300,          // between lines and blocks of text,
    stroke: 70,         // between the strokes of a drawing,
    kind: 180,          // from writing to drawing,
    piece: 420,         // between the pieces of a cell,
    cell: 800,          // and between cells
    lag: 0.18           // the second pass of a doubled line starts this far into the first
  };
  var SPEEDS = [0.25, 0.35, 0.5, 0.7, 1, 1.4, 2, 3, 5, 8];

  function setting() {
    var s;
    try { s = localStorage.getItem('nb.ink') || 'gentle'; } catch (e) { s = 'gentle'; }
    // "slow" was the gentle pen stretched; watching replaces it.
    return s === 'slow' ? 'gentle' : s;
  }
  I.setting = setting;
  var override = null;     // the mode chosen on this page (watch from an export, leaving watch)
  // gentle, watch or off: this page's choice, then the URL (ink=watch, ink=off), then the setting.
  function mode() {
    if (override) return override;
    var m = /[?&#]ink=(off|watch|gentle)\b/.exec(location.search + location.hash);
    if (m) return m[1];
    return setting();
  }
  I.mode = mode;
  I.setSetting = function (value) {
    var was = mode();
    try { localStorage.setItem('nb.ink', value); } catch (e) { /* private window */ }
    setMode(value, was);
  };
  // Changing mode finishes what is being written; choosing watch writes the page again from the top.
  function setMode(value, was) {
    if (was == null) was = mode();
    override = value;
    if (value === was) return;
    I.finishAll();
    if (value === 'watch') I.rewrite();
    controls();
    try { window.dispatchEvent(new CustomEvent('inkmode', { detail: value })); } catch (e) { /* old browser */ }
  }
  I.watch = function () { setMode('watch'); };
  I.leave = function () {
    var was = mode();
    if (setting() === 'watch') { try { localStorage.setItem('nb.ink', 'gentle'); } catch (e) { /* private window */ } }
    setMode('gentle', was);
  };
  // Forget what has been written and ask the page to build itself again (the editor and the viewer
  // set onrewrite to their render function).
  I.rewrite = function () {
    I.finishAll();
    registry = {};
    pen.free = -1e9; pen.group = null;
    // What follows is the page appearing again, not an answer to the reader's click.
    lastAction = -1e9;
    view.on = true;
    if (typeof I.onrewrite === 'function') I.onrewrite();
  };

  var supported = (function () {
    try {
      var c = document.createElement('canvas').getContext('2d');
      return !!(c && window.requestAnimationFrame && window.CSS && CSS.supports('-webkit-text-fill-color', 'transparent') &&
        document.createRange && document.fonts && window.InkStrokes && typeof c.measureText('x').fontBoundingBoxAscent === 'number');
    } catch (e) { return false; }
  })();

  function media(q) { return !!(window.matchMedia && window.matchMedia(q).matches); }
  // Off for reduced motion, print, a page marked not to animate (PDF export), the reader's setting,
  // ink=off in the URL, and browsers that lack what the writer needs.
  I.enabled = function () {
    if (!I.available()) return false;
    return mode() !== 'off';
  };
  // Whether the page could be written at all, whatever the reader's setting (for a "watch" button).
  I.available = function () {
    return supported && !printing && document.documentElement.getAttribute('data-ink') !== 'off' &&
      !media('(prefers-reduced-motion: reduce)') && !media('print');
  };
  function watching() { return I.enabled() && mode() === 'watch'; }
  I.watching = watching;

  // ---- Registry and scheduler ---------------------------------------------------------------
  var registry = {};       // key -> {sig, job}
  var jobs = [];           // attached and not finished
  var pending = [];        // presented since the last frame
  var raf = 0, printing = false, lastAction = -1e9, fontsReady = false;
  var stats = { jobs: 0, frames: [], ms: {} };
  function prof(name, t0) { stats.ms[name] = (stats.ms[name] || 0) + now() - t0; }
  I.stats = stats;

  // The clock. Tests can stop it (window.__INK_PAUSE before the page loads, or Ink.pause) and move it
  // by hand with Ink.advance, to look at a page part-way through.
  var paused = !!window.__INK_PAUSE, frozen = performance.now();
  function now() { return paused ? frozen : performance.now(); }
  I.pause = function () { frozen = performance.now(); paused = true; };
  I.advance = function (ms) { frozen += ms; schedule(); };
  I.now = now;
  function schedule() { if (!raf) raf = requestAnimationFrame(frame); }

  // The pen's clock, for watch mode: it follows the clock above at the reader's speed, and stands still
  // while they pause. Every time in a watched piece is on this clock, so changing speed or pausing
  // mid-stroke needs nothing re-planned.
  var pen = { t: 0, last: null, held: false, free: -1e9, group: null, speed: 1 };
  try { var saved = parseFloat(localStorage.getItem('nb.ink.speed')); if (saved > 0) pen.speed = saved; } catch (e) { /* private window */ }
  function penClock() {
    var t = now();
    if (pen.last != null && t > pen.last && !pen.held) pen.t += (t - pen.last) * pen.speed;
    pen.last = t;
    return pen.t;
  }
  function clock() { return watching() ? penClock() : now(); }
  I.penNow = function () { return penClock(); };

  // A cheap hash of what an element shows. Clip-path ids from Sketch.fill are random; leave them out.
  function signature(el) {
    var s = (el.outerHTML || '').replace(/skc[a-z0-9]+/g, ''), h = 2166136261;
    for (var i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
    return (h >>> 0).toString(36) + ':' + s.length;
  }
  I.signature = signature;

  //   opts.reverse: draw paths from their end (a cell's bracket is the end of its outline)
  //   opts.visible: the share of the ink that can be seen (the rest is clipped away); it sets the pace
  I.present = function (el, key, sig, opts) {
    if (!el || !key || !I.enabled()) return;
    if (sig == null) sig = signature(el);
    else sig = String(sig);
    var entry = registry[key];
    if (entry && entry.sig === sig) {
      if (entry.job.state === 'done') return;
      pending = pending.filter(function (p) { return p.job !== entry.job; });
      pending.push({ el: el, job: entry.job });
    } else {
      var quick = now() - lastAction < C.recent;
      var job = { key: key, sig: sig, el: null, state: 'new', quick: quick, level: 0, opts: opts || {} };
      if (watching()) {
        // Watching: a reader's change to a piece already written (or being written) shows at once; a
        // piece the pen has not reached yet keeps its place in the queue, whatever changed it.
        // A new piece counts as written when the pen has already passed where it is.
        var before = entry ? entry.job.state : aheadOfPen(el) ? 'parked' : 'done';
        // A piece marked update 'now' (the editor's status line) is never queued behind the pen.
        if ((quick || job.opts.update === 'now') && (before === 'done' || before === 'running')) {
          if (entry) drop(entry.job);
          job.state = 'done';
          registry[key] = { sig: sig, job: job };
          return;
        }
        job.quick = false;
        if (entry && before !== 'done') { drop(entry.job); entry = null; }
      }
      if (entry) drop(entry.job);
      // Text that was already there before the change is not written again.
      if (entry && entry.job.texts) job.prev = entry.job.texts;
      registry[key] = { sig: sig, job: job };
      pending.push({ el: el, job: job });
      stats.jobs++;
    }
    schedule();
  };

  // Whether an element is further on than the pen has got: after the piece it is writing, or after the
  // first piece still waiting when it is between pieces.
  function aheadOfPen(el) {
    // A render replaces every element: what it has presented so far (pending) is the page as it is now.
    var run = null, first = null;
    var look = function (j, e) {
      if (!e || !e.isConnected) return;
      if (j.state === 'running') run = e;
      else if (j.state === 'parked' && (!first || (first.compareDocumentPosition(e) & 2))) first = e;
    };
    jobs.forEach(function (j) { look(j, j.el); });
    pending.forEach(function (p) { look(p.job, p.el); });
    var at = run || first;
    return !!(at && (at.compareDocumentPosition(el) & 4));
  }

  // The reader asked for a run (Run all, a cell's run button): its results are fresh, not an update.
  I.fresh = function () { lastAction = -1e9; };
  I.busy = function () { return jobs.length > 0 || pending.length > 0; };

  // Whatever goes wrong, the page must end up complete: an error finishes everything.
  function frame(ts) {
    raf = 0;
    try { frame1(ts); } catch (e) {
      if (window.console) console.warn('ink:', e);
      try { I.finishAll(); } catch (e2) { /* nothing more to do */ }
    }
  }
  function frame1(ts) {
    var t = now(), t0 = t, watch = watching(), pt = watch ? penClock() : t;
    var list = pending;
    pending = [];
    var h = viewHeight();
    list.forEach(function (p) {
      if (p.job.state === 'done') return;
      if (!p.el.isConnected) { if (!p.job.el) finish(p.job); return; }
      // Below the fold a piece only waits, hidden as a whole; it is taken apart when it comes into view.
      // Watching, every piece waits like that until the pen comes to it.
      if (p.job.state === 'new' || p.job.state === 'parked') {
        var r = p.el.getBoundingClientRect();
        if (!r.width && !r.height) { finish(p.job); return; }
        if (watch) { park(p.job, p.el); return; }
        if (r.top >= h) { park(p.job, p.el); return; }
        if (r.bottom < 0) { finish(p.job); return; }
        unpark(p.job);
        p.job.state = 'new';
      }
      attach(p.job, p.el);
    });
    prof('attach', t0);
    var t1 = now();
    if (fontsReady) { if (watch) planWatch(pt); else planVisible(t); }
    prof('plan', t1);
    jobs.slice().forEach(function (job) {
      if (job.reattach && job.el && job.state !== 'parked') { job.reattach = false; if (job.el.isConnected) attach(job, job.el); else finish(job); }
      if (job.state === 'running') step(job, watch ? pt : t);
    });
    // The pen finished a piece this frame: the next one is planned now, not a frame later.
    // A clock moved far ahead (a test, a slow frame) may finish several pieces in one frame.
    for (var more = 0; watch && fontsReady && more < 20 && !jobs.some(function (j) { return j.state === 'running'; }); more++) {
      var n = jobs.length;
      planWatch(pt);
      jobs.forEach(function (job) { if (job.state === 'running') step(job, pt); });
      if (!jobs.length || (jobs.length === n && jobs.some(function (j) { return j.state === 'running'; }))) break;
    }
    if (watch) follow();
    drain();
    var spent = now() - t0;
    stats.frames.push([ts, spent, jobs.filter(function (j) { return j.state === 'running'; }).length]);
    if (spent > 30) {
      stats.slow = stats.slow || [];
      if (stats.slow.length > 50) stats.slow.shift();
      var snap = {};
      for (var k in stats.ms) snap[k] = Math.round(stats.ms[k] - ((stats.last || {})[k] || 0));
      stats.slow.push([Math.round(spent), JSON.stringify(snap)]);
    }
    stats.last = Object.assign({}, stats.ms);
    if (stats.frames.length > 4000) stats.frames.splice(0, 2000);
    if (watch) controls();
    if (pending.length || (jobs.some(function (j) { return j.state === 'running'; }) && !(watch && pen.held))) schedule();
    else if (watch && !pen.held && jobs.length && !fontsReady) schedule();
  }

  // ---- Jobs ---------------------------------------------------------------------------------
  function attach(job, el) {
    if (job.el) teardown(job);
    job.el = el;
    var t0 = now();
    try {
      collect(job);
      prof('collect', t0);
      var t1 = now();
      hide(job);
      prof('hide', t1);
    } catch (e) {
      if (window.console) console.warn('ink:', e);
      finish(job);
      return;
    }
    if (!job.atoms.length) { finish(job); return; }
    if (job.state === 'new') job.state = 'waiting';
    if (jobs.indexOf(job) < 0) jobs.push(job);
    watch(job);
    if (job.state === 'running') {
      if (job.watch) { timelineWatch(job); pen.free = job.start + job.end; step(job, penClock()); }
      else { timeline(job); step(job, now()); }
    }
  }

  function finish(job) {
    if (job.watch && job.state === 'running') pen.free = Math.min(pen.free, pen.t);
    teardown(job);
    job.state = 'done';
    job.el = null;
    var k = jobs.indexOf(job);
    if (k >= 0) jobs.splice(k, 1);
  }
  function drop(job) {
    if (job.state === 'done') return;
    finish(job);
  }

  I.finishAll = function () {
    pending.forEach(function (p) { p.job.state = 'done'; });
    pending = [];
    jobs.slice().forEach(finish);
    pen.free = Math.min(pen.free, pen.t);
    drain();
    controls();
  };

  function viewHeight() { return window.innerHeight || document.documentElement.clientHeight; }

  // A piece below the fold is hidden whole, by opacity (paint only), until it scrolls into view.
  function park(job, el) {
    if (job.el && job.el !== el) teardown(job);
    if (job.el !== el) {
      job.el = el;
      job.saved = new Map();
      save(job, el);
      el.style.opacity = '0';
    }
    job.state = 'parked';
    if (jobs.indexOf(job) < 0) jobs.push(job);
  }
  function unpark(job) {
    if (job.state !== 'parked' || !job.el) return;
    restore(job, job.el);
    job.el = null;
  }

  // Jobs on screen start; jobs below the fold wait, hidden, until they scroll into view. One that
  // has been scrolled past is simply finished.
  function planVisible(t) {
    var batch = [], h = viewHeight();
    jobs.slice().forEach(function (job) {
      if (job.state === 'parked') {
        var pr = job.el.getBoundingClientRect();
        if (pr.bottom < 0) { finish(job); return; }
        if (pr.top >= h) return;
        var el = job.el;
        unpark(job);
        job.state = 'new';
        attach(job, el);
        if (job.state === 'done') return;
      }
      if (job.state !== 'waiting') return;
      var r = job.el.getBoundingClientRect();
      if (!r.width && !r.height) { finish(job); return; }
      if (r.bottom < 0) { finish(job); return; }
      if (r.top < h) batch.push(job);
    });
    if (!batch.length) return;
    batch.sort(function (a, b) { return a.el.compareDocumentPosition(b.el) & 2 ? 1 : -1; });
    // A wave: the pieces of a cell start close together, and each cell soon after the one above.
    var k = 1, cursor = 0, prev = 0, group = null, groupStart = 0, groupEnd = 0, end = 0;
    batch.forEach(function (job) {
      if (job.quick) { job.dur = job.end = C.quick; job.offset = 0; return; }
      // A piece mostly clipped away is paced by what shows, and done when that is drawn.
      var seen = job.opts.visible || 1;
      var shown = Math.max(C.blockMin * k, Math.min(C.blockMax * k, job.ink * seen / C.speed * k));
      job.dur = shown / seen;
      job.end = Math.min(job.dur, shown * 1.3);
      var g = job.key.split('/')[0];
      if (g !== group) {
        if (group !== null) cursor = groupStart + C.stagger * (groupEnd - groupStart);
        group = g;
        groupStart = groupEnd = cursor;
      } else cursor += C.within * prev;
      job.offset = cursor;
      prev = shown;
      groupEnd = Math.max(groupEnd, cursor + shown);
      end = Math.max(end, cursor + job.end);
    });
    var budget = C.budget * k, f = end > budget ? budget / end : 1;
    batch.forEach(function (job) {
      if (!job.quick) { job.dur *= f; job.end *= f; job.offset *= f; }
      job.start = t + job.offset;
      job.state = 'running';
      degrade(job);
      if (I.plans.length > 500) I.plans.splice(0, 250);
      I.plans.push({ key: job.key, at: Math.round(t), start: Math.round(job.offset), dur: Math.round(job.dur), end: Math.round(job.end), ink: Math.round(job.ink), atoms: job.atoms.length, level: job.level, peaks: job.peaks });
    });
  }

  // Watching: one pen. When it is free it takes the first waiting piece in reading order (document
  // order), wherever that is on the page, so results that arrive for a cell further down wait their
  // turn and an earlier piece that changed is written before anything after it.
  function planWatch(pt) {
    if (jobs.some(function (j) { return j.state === 'running'; })) return;
    for (;;) {
      var next = null;
      jobs.forEach(function (j) {
        if ((j.state !== 'parked' && j.state !== 'waiting') || !j.el || !j.el.isConnected) return;
        if (!next || (next.el.compareDocumentPosition(j.el) & 2)) next = j;
      });
      if (!next) {
        // Anything left is detached from the page: nothing to write.
        jobs.slice().forEach(function (j) { if (j.state === 'parked' || j.state === 'waiting') finish(j); });
        return;
      }
      var el = next.el;
      if (next.state === 'parked') { unpark(next); next.state = 'new'; }
      next.level = 0;
      next.watch = true;
      attach(next, el);
      if (next.state === 'done') continue;
      timelineWatch(next);
      var group = next.key.split('/')[0];
      var gap = pen.group == null ? 0 : group === pen.group ? W.piece : W.cell;
      next.start = Math.max(pt, pen.free + gap);
      next.offset = next.start - pt;
      next.state = 'running';
      pen.group = group;
      pen.free = next.start + next.end;
      if (I.plans.length > 500) I.plans.splice(0, 250);
      I.plans.push({ key: next.key, at: Math.round(now()), pen: Math.round(next.start), start: Math.round(next.offset), dur: Math.round(next.dur), end: Math.round(next.end), ink: Math.round(next.ink), atoms: next.atoms.length, level: 0, watch: true });
      return;
    }
  }

  // How long the pen spends on one atom when watching, on the pen's clock.
  function watchDur(job, a) {
    if (a.type === 'glyph') {
      var travel = a.w - (a.space || 0);
      return Math.max(W.glyphMin, travel / W.text * (a.sweep ? 0.7 : 1));
    }
    if (a.type === 'path') {
      if (a.fast) return W.hatchLine;
      var seen = job.opts.visible || 1;
      // A stroke mostly clipped away is paced by what shows.
      return Math.max(W.shapeMin, Math.min(W.shapeMax, W.shapeMin * 0.4 + a.len * seen / W.shape)) / seen;
    }
    if (a.type === 'wipe') return a.row ? W.row : Math.max(W.glyphMin, a.w / W.text);
    if (a.type === 'appear') return a.nodes.length > 1 ? W.chunk : W.dot;
    if (a.type === 'pseudo') return Math.max(W.shapeMin, Math.min(W.shapeMax, a.w / W.shape));
    return a.el instanceof SVGElement && !a.group ? W.fill : W.fade;
  }

  // The pause before an atom, after the one before it.
  function watchGap(prev, a) {
    var g;
    if (a.hurried && prev.hurried) g = 6;
    else if (prev.type === 'glyph' && a.type === 'glyph') {
      if (prev.unit === a.unit) g = prev.gap === 'line' ? W.line : prev.gap === 'word' ? W.word : W.letter;
      else g = prev.unit.block === a.unit.block && prev.unit.block ? (prev.gap === 'line' ? W.line : W.word) : W.line;
    } else if (prev.type === 'path' && a.type === 'path') g = prev.fast && a.fast ? (a.hgap != null ? a.hgap : 8) : W.stroke;
    else if (prev.type === 'appear' && a.type === 'appear') g = 8;
    else if (prev.type === 'wipe' && a.type === 'wipe') g = W.word;
    else if (prev.type === a.type) g = W.stroke;
    else g = W.kind;
    // The second pass of a doubled line is drawn before the pen moves on.
    if (prev.partner) g = Math.max(g, W.lag * (prev.t1 - prev.t0) + 30);
    return g;
  }

  // One pen: atoms one after another, constant speed along each, a pause between them.
  function timelineWatch(job) {
    var atoms = job.atoms;
    atoms.forEach(function (a) { a.partner = false; a.hurried = false; a.hgap = null; });
    atoms.forEach(function (a) { if (a.with) a.with.partner = true; else if (!a.instant) a.d = watchDur(job, a); });
    // Hatching: each fill is scribbled in quickly, within W.hatch.
    var fills = new Map();
    atoms.forEach(function (a) { if (a.fill && !a.with && !a.instant) { var f = fills.get(a.fill) || []; f.push(a); fills.set(a.fill, f); } });
    var hatching = 0;
    fills.forEach(function (list) {
      var sum = list.reduce(function (n, a) { return n + a.d + 8; }, 0);
      if (sum > W.hatch) list.forEach(function (a) { a.d = Math.max(10, (a.d + 8) * W.hatch / sum - 8); });
      hatching += Math.min(sum, W.hatch);
    });
    // A chart with many hatched shapes: the pen scribbles faster still.
    if (hatching > W.hatchAll) {
      var f = W.hatchAll / hatching;
      fills.forEach(function (list) { list.forEach(function (a) { a.d = Math.max(1, (a.d + 8) * f - 8 * f); a.hgap = 8 * f; }); });
    }
    // A long run of strokes, marks (filled shapes, plain marks) or table rows: the first ones at full pace, then the pen
    // hurries through the rest, which share W.hurry between them.
    ['mark', 'row'].forEach(function (kind) {
      var list = atoms.filter(function (a) {
        if (a.with || a.instant || a.fill) return false;
        return kind === 'row' ? !!a.row : (a.type === 'path' || a.type === 'fade' || (a.type === 'appear' && a.nodes.length === 1));
      });
      if (kind === 'mark' && list.length <= W.marks) return;
      var tail = kind === 'row' ? list : list.slice(W.marks);
      var sum = tail.reduce(function (n, a) { return n + a.d + W.stroke; }, 0);
      if (sum <= W.hurry) return;
      tail.forEach(function (a) { a.d = Math.max(12, (a.d + W.stroke) * W.hurry / sum - 6); a.hurried = true; });
    });
    var t = 0, prev = null, end = 0;
    atoms.forEach(function (a) {
      if (a.instant) { a.t0 = a.t1 = 0; return; }
      if (a.with) {
        var span = a.with.t1 - a.with.t0;
        a.t0 = a.with.t0 + span * W.lag;
        a.t1 = a.t0 + span;
        end = Math.max(end, a.t1);
        return;
      }
      if (prev) t += watchGap(prev, a);
      a.t0 = t;
      a.t1 = t = t + a.d;
      end = Math.max(end, t);
      prev = a;
    });
    job.dur = end;
    // A piece mostly clipped away (a cell's bracket, drawn from its end) is done when what shows is.
    var seen = job.opts.visible || 1;
    job.end = seen < 1 ? Math.min(end, atoms.reduce(function (m, a) { return a.instant ? m : Math.max(m, a.t0 + (a.t1 - a.t0) * seen); }, 0) + 40) : end;
    atoms.sort(function (a, b) { return a.t0 - b.t0 || a.order - b.order; });
    job.next = 0;
    job.active = [];
  }

  // Too much in motion at once is slow to paint: write tables row by row, then everything as
  // sweeps; fade hatching instead of drawing it.
  function degrade(job) {
    var level = job.quick ? 2 : 0;
    for (;;) {
      if (job.level !== level) { job.level = level; attach(job, job.el); if (job.state === 'done') return; }
      timeline(job);
      var peak = concurrency(job);
      (job.peaks = job.peaks || []).push(peak.glyph + '/' + peak.path);
      if (level >= 2 || (peak.glyph <= C.capGlyphs && peak.path <= C.capPaths)) return;
      // Far too much writing at once: straight to sweeps rather than one level at a time.
      level = peak.glyph > 2.5 * C.capGlyphs ? 2 : level + 1;
    }
  }

  function concurrency(job) {
    var ev = { glyph: [], path: [] };
    job.atoms.forEach(function (a) {
      var kind = a.type === 'glyph' ? 'glyph' : a.type === 'path' ? 'path' : null;
      if (kind) { ev[kind].push([a.t0, 1]); ev[kind].push([a.t1, -1]); }
    });
    var out = {};
    Object.keys(ev).forEach(function (k) {
      var n = 0, max = 0;
      ev[k].sort(function (a, b) { return a[0] - b[0] || a[1] - b[1]; }).forEach(function (e) { n += e[1]; max = Math.max(max, n); });
      out[k] = max;
    });
    return out;
  }

  // Places each atom on the job's clock: constant pen speed, each atom at least long enough to be
  // seen, the second pass of a doubled line a little behind the first.
  function timeline(job) {
    var D = job.dur, W = 0, pos = 0, k = job.quick ? 0.35 : 1, lag = 0.22 * (job.opts.visible || 1);
    job.atoms.forEach(function (a) { if (!a.with) W += a.w; });
    W = W || 1;
    job.atoms.forEach(function (a) {
      if (a.instant) { a.t0 = a.t1 = 0; return; }
      if (a.with) {
        var span = a.with.t1 - a.with.t0;
        a.t0 = Math.min(D, a.with.t0 + span * lag);
        a.t1 = Math.min(D, a.t0 + span);
        return;
      }
      a.t0 = D * pos / W;
      pos += a.w;
      var min = (a.type === 'glyph' ? C.glyphMin : a.type === 'path' ? C.pathMin : C.fadeMin) * k;
      a.t1 = Math.min(D, a.t0 + Math.max(D * a.w / W, min));
      if (a.t1 - a.t0 < min) a.t0 = Math.max(0, a.t1 - min);
    });
    job.atoms.sort(function (a, b) { return a.t0 - b.t0 || a.order - b.order; });
    job.next = 0;
    job.active = [];
  }

  function ease(p) { return p <= 0 ? 0 : p >= 1 ? 1 : 0.5 - 0.5 * Math.cos(Math.PI * p); }

  function step(job, t) {
    var rel = t - job.start;
    if (rel < 0) return;
    if (rel >= job.end) { finish(job); return; }
    var atoms = job.atoms;
    while (job.next < atoms.length && atoms[job.next].t0 <= rel) {
      var a = atoms[job.next++];
      startAtom(job, a);
      job.active.push(a);
    }
    var live = false;
    job.active = job.active.filter(function (a) {
      if (rel >= a.t1) { completeAtom(job, a); if (a.text) live = true; return false; }
      a.p = (rel - a.t0) / Math.max(1, a.t1 - a.t0);
      if (a.text) live = true; else updateAtom(job, a);
      return true;
    });
    if (live) { var t1 = now(); paintLive(job); prof('paint', t1); }
  }

  // ---- Collecting what a piece is made of -------------------------------------------------
  // An atom is one thing the pen does: draw a path, write a glyph, sweep a line of text, fade an
  // element. Each has a weight (pen travel in px) that sets its share of the job's time.
  function collect(job) {
    var atoms = [], order = 0, root = job.el;
    job.atoms = atoms;
    job.saved = new Map();
    job.splits = [];
    job.units = [];
    job.svgTexts = [];
    job.textRoot = false;
    job.texts = [];
    var prev = null, instant = false;
    if (job.prev) { prev = {}; job.prev.forEach(function (t) { prev[t] = (prev[t] || 0) + 1; }); }
    // Whether this text was on the page before: then it is shown at once, not written.
    function known(text) {
      job.texts.push(text);
      if (!prev || !prev[text]) return false;
      prev[text]--;
      return true;
    }
    function add(a) { a.order = order++; if (instant) { a.instant = true; a.w = 0; } atoms.push(a); return a; }

    function walkHtml(el, isRoot, alpha) {
      var cs = getComputedStyle(el);
      alpha = (alpha == null ? opacityOf(el.parentElement, null) : alpha) * parseFloat(cs.opacity);
      if (cs.display === 'none' || (!isRoot && el.getClientRects().length === 0 && cs.display !== 'contents')) return;
      if (cs.opacity === '0') return;
      if (el.tagName === 'CANVAS' && el.classList.contains('ink-layer')) return;
      var tag = el.tagName;
      if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || tag === 'IMG' || tag === 'VIDEO' || tag === 'CANVAS') {
        if (!el.classList.contains('nb-code-over') && cs.visibility !== 'hidden') add({ type: 'fade', el: el, to: +cs.opacity, w: 24 });
        return;
      }
      if (!isRoot && cs.visibility !== 'hidden' && decorated(cs) && !el.querySelector('svg, canvas, input, select, textarea')) {
        instant = known(tag + ':' + el.textContent);
        add({ type: 'fade', el: el, to: +cs.opacity, w: 8 });
        instant = false;
      }
      var rows = job.watch ? W.rows : job.level >= 1 ? C.tableRows : Infinity;
      if (tag === 'TR' && el.parentElement && el.parentElement.tagName === 'TBODY' && el.sectionRowIndex >= rows) {
        var row = textUnits(el);
        if (row.length) add({ type: 'wipe', units: row, text: true, row: true, w: Math.max(30, el.offsetWidth * 0.12) });
        return;
      }
      for (var n = el.firstChild; n; n = n.nextSibling) {
        if (n.nodeType === 3) textNode(n, cs, alpha);
        else if (n.nodeType === 1) {
          if (n instanceof SVGElement) { if (n.tagName.toLowerCase() === 'svg') walkSvg(n); }
          else walkHtml(n, false, alpha);
        }
      }
      if (/^H[1-3]$/.test(tag)) {
        var after = getComputedStyle(el, '::after');
        if (after.content !== 'none' && after.backgroundImage !== 'none') add({ type: 'pseudo', el: el, w: el.offsetWidth * 0.5 });
      }
    }

    // Text nodes become glyph atoms, or one sweep when the job writes in sweeps.
    function textNode(node, cs, alpha) {
      if (!/\S/.test(node.data) || cs.visibility === 'hidden' || !alpha) return;
      var unit = style({ node: node, el: node.parentNode, svg: false }, cs, alpha);
      job.units.push(unit);
      job.textRoot = true;
      instant = known(node.data);
      if (job.level >= 2 || instant) add({ type: 'wipe', units: [unit], text: true, w: Math.max(20, node.data.length * fontPx(cs) * 0.25) });
      else glyphs(unit, cs);
      instant = false;
    }

    function glyphs(unit, cs) {
      var fam = family(cs.fontFamily), size = fontPx(cs), data = unit.node.data, i = 0;
      unit.family = fam;
      while (i < data.length) {
        var code = data.codePointAt(i), ch = String.fromCodePoint(code), len = ch.length;
        if (/\s/.test(ch)) {
          var last = atoms[atoms.length - 1];
          if (last) {
            last.w += C.space * size;
            last.space = (last.space || 0) + C.space * size;
            if (last.unit === unit) last.gap = ch === '\n' || last.gap === 'line' ? 'line' : 'word';
          }
          i += len;
          continue;
        }
        // A run of operators in the code font may be one ligature: written as one unit, swept.
        if (fam === 'Cascadia' && /[!=<>\-|&:*\/+.~^%#?;]/.test(ch)) {
          var j = i + 1;
          while (j < data.length && /[!=<>\-|&:*\/+.~^%#?;]/.test(data[j])) j++;
          if (j - i > 1) { add({ type: 'glyph', unit: unit, i: i, n: j - i, text: true, sweep: true, w: (j - i) * 0.6 * size }); i = j; continue; }
        }
        var g = glyph(fam, ch);
        add({ type: 'glyph', unit: unit, i: i, n: len, text: true, g: g, sweep: !g, w: g ? (g.total + C.lift * 1000 * g.s.length) * size / 1000 : 0.6 * size });
        i += len;
      }
    }

    function walkSvg(svg) {
      if (svg.getClientRects().length === 0) return;
      walkGroup(svg);
    }
    var SKIP = { clippath: 1, defs: 1, mask: 1, marker: 1, pattern: 1, lineargradient: 1, radialgradient: 1, title: 1, desc: 1, style: 1, filter: 1, symbol: 1 };
    function walkGroup(g) {
      var fills = [];
      for (var c = g.firstElementChild; c; c = c.nextElementSibling) {
        var tag = c.tagName.toLowerCase();
        // Hundreds of plain marks in a row fade in together, gathered into one group while they do.
        if (isPlain(c)) {
          var run = [c];
          for (var r = c.nextElementSibling; r && isPlain(r); r = r.nextElementSibling) run.push(r);
          if (job.watch && run.length > W.marks) {
            // Watching: the first marks one by one, the rest in a few handfuls.
            run.slice(0, W.marks).forEach(function (n) { add({ type: 'appear', nodes: [n], w: 2 }); });
            var restOf = run.slice(W.marks), hands = Math.min(6, restOf.length);
            for (var q = 0; q < hands; q++) add({ type: 'appear', nodes: restOf.slice(Math.floor(q * restOf.length / hands), Math.floor((q + 1) * restOf.length / hands)), w: 10 });
            c = run[run.length - 1];
            continue;
          }
          if (run.length > C.plainGroup) {
            // Most of a group: the group fades, one element. Otherwise the run appears in chunks.
            if (run.length >= 0.8 * g.childElementCount) { add({ type: 'fade', el: g, to: 1, w: 60, group: true }); fills = []; break; }
            for (var k = 0; k < 8; k++) {
              var chunk = run.slice(Math.floor(k * run.length / 8), Math.floor((k + 1) * run.length / 8));
              if (chunk.length) add({ type: 'appear', nodes: chunk, w: 10 });
            }
            c = run[run.length - 1];
            continue;
          }
        }
        if (SKIP[tag] || c.classList.contains('sk-hit')) continue;
        if (c.getAttribute('display') === 'none') continue;
        if (tag === 'g' && c.hasAttribute('clip-path')) { fills.push(c); continue; }
        if (tag === 'g' || tag === 'svg' || tag === 'a') { walkGroup(c); continue; }
        if (tag === 'text') { svgText(c); continue; }
        if (tag === 'path' || tag === 'line' || tag === 'polyline' || tag === 'polygon' || tag === 'rect' || tag === 'circle' || tag === 'ellipse') {
          if (drawable(c)) shape(c);
          else if (c.getAttribute('stroke') === 'none' && c.getAttribute('fill') && c.getAttribute('fill') !== 'none') fills.push(c);
          else add({ type: 'fade', el: c, to: 1, w: 16 });
          continue;
        }
        add({ type: 'fade', el: c, to: 1, w: 16 });
      }
      // Fills come after the outlines they fill.
      fills.forEach(function (f) {
        if (f.tagName.toLowerCase() === 'g') hatching(f);
        else add({ type: 'fade', el: f, to: 1, w: 12 });
      });
    }
    function isPlain(n) {
      var tag = n.tagName.toLowerCase();
      return (tag === 'circle' || tag === 'rect' || tag === 'line') && !n.classList.contains('sk-hit');
    }
    function drawable(el) {
      var fill = el.getAttribute('fill'), stroke = el.getAttribute('stroke');
      if (el.getAttribute('stroke-dasharray') || el.style.strokeDasharray) return false;
      if (el.tagName.toLowerCase() === 'line') return stroke !== 'none';
      return fill === 'none' && !!stroke && stroke !== 'none';
    }

    // A stroked path: one atom per subpath. A doubled line's second pass runs just behind its first.
    var lastShape = null;
    function shape(el, fast, fill) {
      var pieces = subpaths(el), list = [];
      if (pieces.length > 1) {
        var group = document.createElementNS(SVGNS, 'g');
        pieces.forEach(function (d) {
          var copy = el.cloneNode(false);
          copy.setAttribute('d', d);
          group.appendChild(copy);
          list.push(copy);
        });
        job.splits.push({ orig: el, group: group, left: list.length });
      } else list.push(el);
      var split = job.splits.length && pieces.length > 1 ? job.splits[job.splits.length - 1] : null;
      var half = list.length % 2 === 0 && !fast ? list.length / 2 : list.length;
      var made = [];
      list.forEach(function (p, k) {
        var a = { type: 'path', el: p, split: split, len: 0, w: 0, fast: fast, fill: fill };
        if (k >= half) a.with = made[k - half];
        made.push(add(a));
      });
      // S.shape draws a doubled shape as two identical paths, the second shifted.
      if (!fast && lastShape && list.length === 1 && lastShape.el.getAttribute('d') === el.getAttribute('d') && lastShape.el.parentNode === el.parentNode) {
        made[0].with = lastShape.atom;
      }
      lastShape = list.length === 1 ? { el: el, atom: made[0] } : null;
    }
    function hatching(g) {
      if (job.level >= 1) { add({ type: 'fade', el: g, to: 1, w: 20 }); return; }
      var lines = g.querySelectorAll('g > path');
      Array.prototype.forEach.call(lines, function (p) { shape(p, true, g); });
    }

    function svgText(el) {
      if (!/\S/.test(el.textContent)) return;
      var cs = getComputedStyle(el);
      var unit = style({ node: el, el: el, svg: true }, cs, opacityOf(el, null) * parseFloat(cs.fillOpacity || '1'));
      job.units.push(unit);
      job.svgTexts.push(el);
      job.textRoot = true;
      instant = known(el.textContent);
      if (job.level >= 2 || instant) { add({ type: 'wipe', units: [unit], text: true, w: Math.max(20, el.textContent.length * fontPx(cs) * 0.25) }); instant = false; return; }
      instant = false;
      // An SVG text's characters are addressed by index in its whole text content.
      unit.node = { data: el.textContent };
      glyphs(unit, cs);
      unit.node = el;
    }

    // Text inside a row, for a sweep.
    function textUnits(el) {
      var out = [], walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
      for (var n = walker.nextNode(); n; n = walker.nextNode()) {
        if (!/\S/.test(n.data)) continue;
        var pcs = getComputedStyle(n.parentNode);
        var u = style({ node: n, el: n.parentNode, svg: false }, pcs, opacityOf(n.parentNode, null));
        out.push(u);
        job.units.push(u);
      }
      job.textRoot = job.textRoot || out.length > 0;
      // Backgrounds in the row (bars, heat) fade with it.
      Array.prototype.forEach.call(el.querySelectorAll('td, th'), function (td) {
        if (decorated(getComputedStyle(td))) add({ type: 'fade', el: td, to: 1, w: 2 });
      });
      return out;
    }

    if (root instanceof SVGElement) {
      if (root.tagName.toLowerCase() === 'svg') walkSvg(root); else walkGroup(root);
    } else walkHtml(root, true);

    // Lengths: read every path's length before writing any style.
    var ink = 0;
    atoms.forEach(function (a) {
      if (a.type === 'path') {
        try { a.len = a.el.getTotalLength(); } catch (e) { a.len = 0; }
        a.w = a.with || a.instant ? 0 : a.fast ? a.len * 0.25 : a.len;
      }
      if (!a.with) ink += a.w;
    });
    job.ink = ink;
  }

  // How a text unit is painted, read before anything is hidden.
  var INLINE = { SPAN: 1, B: 1, STRONG: 1, EM: 1, I: 1, CODE: 1, A: 1, MARK: 1, SMALL: 1, SUB: 1, SUP: 1, U: 1, S: 1, KBD: 1, LABEL: 1 };
  function style(u, cs, alpha) {
    // The block a text unit is part of: the pen moves on to the next word, not the next line, within it.
    var b = u.el;
    while (b && b.nodeType === 1 && INLINE[b.tagName]) b = b.parentNode;
    u.block = b;
    u.font = cs.fontStyle + ' ' + cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
    u.size = fontPx(cs);
    u.color = u.svg ? cs.fill : cs.color;
    u.alpha = alpha;
    u.bold = (parseInt(cs.fontWeight, 10) || 400) >= 600;
    return u;
  }

  function decorated(cs) {
    if (cs.backgroundImage && cs.backgroundImage !== 'none') return true;
    if (visibleColor(cs.backgroundColor)) return true;
    var sides = ['Top', 'Right', 'Bottom', 'Left'];
    for (var i = 0; i < 4; i++) {
      if (parseFloat(cs['border' + sides[i] + 'Width']) > 0 && cs['border' + sides[i] + 'Style'] !== 'none' && visibleColor(cs['border' + sides[i] + 'Color'])) return true;
    }
    return false;
  }
  function visibleColor(c) { return !!c && c !== 'transparent' && !/rgba\([^)]*,\s*0\)$/.test(c); }

  // Splits a path's d into subpaths at each absolute moveto. Relative movetos depend on where the
  // previous subpath ended, so such a path is left whole.
  function subpaths(el) {
    if (el.tagName.toLowerCase() !== 'path') return [null];
    var d = el.getAttribute('d') || '';
    if (/m/.test(d)) return [d];
    var parts = d.split(/(?=M)/).filter(function (s) { return /\S/.test(s); });
    return parts.length > 1 ? parts : [d];
  }

  function family(css) {
    if (/Cascadia/i.test(css)) return 'Cascadia';
    if (/Excalifont/i.test(css)) return 'Excalifont';
    return null;
  }
  function fontPx(cs) { return parseFloat(cs.fontSize) || 16; }

  // Stroke data for a glyph, unpacked once: points, stroke lengths, total length (em/1000).
  var glyphCache = {};
  function glyph(fam, ch) {
    if (!fam || !window.InkStrokes) return null;
    var key = fam + ch;
    if (key in glyphCache) return glyphCache[key];
    var f = InkStrokes[fam], raw = f && f.glyphs[ch], g = null;
    if (raw) {
      g = { s: [], lens: [], total: 0, reveal: Math.max(f.reveal, raw.r || 0) * 2, box: [1e9, 1e9, -1e9, -1e9] };
      raw.s.forEach(function (s) {
        var L = 0;
        for (var i = 0; i < s.length; i += 2) {
          if (i) L += Math.hypot(s[i] - s[i - 2], s[i + 1] - s[i - 1]);
          g.box[0] = Math.min(g.box[0], s[i]); g.box[1] = Math.min(g.box[1], s[i + 1]);
          g.box[2] = Math.max(g.box[2], s[i]); g.box[3] = Math.max(g.box[3], s[i + 1]);
        }
        g.s.push(s);
        g.lens.push(L);
        g.total += L;
      });
      // The pen never reaches past its strokes plus the reveal's half width (and bold's extra).
      var m = g.reveal * 0.75 + 30;
      g.box = [g.box[0] - m, g.box[1] - m, g.box[2] + m, g.box[3] + m];
    }
    glyphCache[key] = g;
    return g;
  }
  I.glyph = glyph;

  // ---- Hiding and restoring ---------------------------------------------------------------------
  // Every attribute Ink touches is saved first and put back exactly, so a finished element is the
  // element as it was built.
  function save(job, el) {
    if (!job.saved.has(el)) job.saved.set(el, { style: el.getAttribute('style'), cls: el.getAttribute('class') });
  }
  function restore(job, el) {
    var s = job.saved.get(el);
    if (!s) return;
    if (s.style == null) el.removeAttribute('style'); else el.setAttribute('style', s.style);
    if (s.cls == null) el.removeAttribute('class'); else el.setAttribute('class', s.cls);
    job.saved.delete(el);
  }

  function hide(job) {
    job.splits.forEach(function (s) { if (s.orig.parentNode) s.orig.parentNode.replaceChild(s.group, s.orig); });

    job.atoms.forEach(function (a) {
      if (a.type === 'path') { save(job, a.el); a.el.style.visibility = 'hidden'; }
      else if (a.type === 'fade') { save(job, a.el); a.el.style.opacity = '0'; }
      else if (a.type === 'pseudo') { save(job, a.el); a.el.style.setProperty('--ink-wipe', '0%'); }
      else if (a.type === 'appear') a.nodes.forEach(function (n) { save(job, n); n.style.opacity = '0'; });
    });
    if (job.textRoot && !(job.el instanceof SVGElement)) { save(job, job.el); job.el.classList.add('ink-hide'); }
    job.svgTexts.forEach(function (t) { save(job, t); t.style.opacity = '0'; });
  }

  function teardown(job) {
    if (!job.el) return;
    try {
      if (job.layers) job.layers.forEach(function (c) { if (c.parentNode) c.parentNode.removeChild(c); });
      job.layers = null;
      if (job.splits) job.splits.forEach(function (s) { if (s.group.parentNode) s.group.parentNode.replaceChild(s.orig, s.group); });

      if (job.saved) job.saved.forEach(function (_, el) { restore(job, el); });
    } finally {
      job.splits = [];
      job.atoms = [];
      job.active = [];
      unwatch(job);
    }
  }

  // ---- Atoms ------------------------------------------------------------------------------------
  function startAtom(job, a) {
    if (a.type === 'path') {
      a.el.style.strokeDasharray = a.len + ' ' + (a.len + 2);
      a.el.style.strokeDashoffset = (job.opts.reverse ? -a.len : a.len);
    } else if (a.text) {
      a.units ? a.units.forEach(function (u) { measure(job, u); }) : measure(job, a.unit);
    }
  }
  function updateAtom(job, a) {
    if (a.type === 'path') {
      if (a.p > 0) a.el.style.visibility = 'visible';
      a.el.style.strokeDashoffset = a.len * (1 - ease(a.p)) * (job.opts.reverse ? -1 : 1);
    } else if (a.type === 'fade') {
      a.el.style.opacity = String(a.to * a.p);
    } else if (a.type === 'pseudo') {
      a.el.style.setProperty('--ink-wipe', (ease(a.p) * 100).toFixed(1) + '%');
    }
  }
  function completeAtom(job, a) {
    if (a.type === 'path') {
      if (a.split) {
        a.el.style.visibility = 'visible';
        a.el.style.strokeDasharray = '';
        a.el.style.strokeDashoffset = '';
        if (--a.split.left === 0 && a.split.group.parentNode) {
          a.split.group.parentNode.replaceChild(a.split.orig, a.split.group);
          job.splits.splice(job.splits.indexOf(a.split), 1);
        }
      } else restore(job, a.el);
    } else if (a.type === 'appear') {
      a.nodes.forEach(function (n) { restore(job, n); });
    } else if (a.type === 'fade' || a.type === 'pseudo') {
      restore(job, a.el);

    } else if (a.text) {
      a.p = 1;
      drawDone(job, a);
    }
  }

  // ---- The text writer ------------------------------------------------------------------------
  // Two canvases over the piece: "done" holds finished glyphs, drawn once with fillText; "live" is
  // cleared and redrawn each frame with the glyphs in motion. They sit in the nearest positioned
  // ancestor, so they scroll with the page and the editor's scrolling cells.
  var dpr = function () { return window.devicePixelRatio || 1; };
  var PAD = 40;

  function layers(job) {
    if (job.layers) return job.layers;
    var host = job.el.parentElement;
    while (host && host !== document.body && getComputedStyle(host).position === 'static') host = host.parentElement;
    host = host || document.body;
    var r = job.el.getBoundingClientRect(), hr = host.getBoundingClientRect();
    var positioned = host !== document.body || getComputedStyle(host).position !== 'static';
    var left = positioned ? r.left - hr.left - host.clientLeft + host.scrollLeft : r.left + window.scrollX;
    var top = positioned ? r.top - hr.top - host.clientTop + host.scrollTop : r.top + window.scrollY;
    var w = Math.ceil(r.width + 2 * PAD), h = Math.ceil(r.height + 2 * PAD), k = dpr();
    job.origin = { x: r.left - PAD, y: r.top - PAD };
    job.layers = [0, 1].map(function () {
      var c = document.createElement('canvas');
      c.className = 'ink-layer';
      c.setAttribute('aria-hidden', 'true');
      c.width = Math.ceil(w * k);
      c.height = Math.ceil(h * k);
      c.style.cssText = 'left:' + (left - PAD) + 'px;top:' + (top - PAD) + 'px;width:' + w + 'px;height:' + h + 'px';
      host.appendChild(c);
      return c;
    });
    job.done = job.layers[0].getContext('2d');
    job.live = job.layers[1].getContext('2d');
    return job.layers;
  }

  var ascents = {}, measureCtx = null;
  function ascent(font) {
    if (!(font in ascents)) {
      measureCtx = measureCtx || document.createElement('canvas').getContext('2d');
      measureCtx.font = font;
      ascents[font] = measureCtx.measureText('Hg').fontBoundingBoxAscent;
    }
    return ascents[font];
  }

  // Positions every character of a text unit from the browser's own layout, when its turn comes.
  function measure(job, u) {
    if (u.chars) return;
    var t0 = now();
    try { measure1(job, u); } finally { prof('measure', t0); }
  }
  function measure1(job, u) {
    layers(job);
    // The page may have scrolled since the canvases were laid (watch mode follows the pen): measure
    // from where they are now.
    var lr = job.layers[0].getBoundingClientRect();
    job.origin = { x: lr.left, y: lr.top };
    u.chars = {};
    var ox = job.origin.x, oy = job.origin.y, data = u.svg ? u.node.textContent : u.node.data;
    if (u.svg) {
      var ctm = u.node.getScreenCTM(), n = u.node.getNumberOfChars();
      if (!ctm) return;
      var pt = (u.node.ownerSVGElement || u.node).createSVGPoint ? (u.node.ownerSVGElement || u.node).createSVGPoint() : null;
      for (var i = 0; i < n && i < data.length; i++) {
        if (/\s/.test(data[i])) continue;
        try {
          var p = u.node.getStartPositionOfChar(i);
          pt.x = p.x; pt.y = p.y;
          var q = pt.matrixTransform(ctm);
          u.chars[i] = { m: [ctm.a, ctm.b, ctm.c, ctm.d, q.x - ox, q.y - oy], w: u.node.getSubStringLength(i, 1) };
        } catch (e) { /* hidden by an ellipsis or out of range */ }
      }
      return;
    }
    var clip = clipRect(u.node.parentNode, job.el), range = document.createRange(), asc = ascent(u.font);
    for (var j = 0; j < data.length; j++) {
      var code = data.charCodeAt(j);
      if (code === 32 || code === 10 || code === 9) continue;
      range.setStart(u.node, j);
      range.setEnd(u.node, j + (code >= 0xD800 && code < 0xDC00 ? 2 : 1));
      var r = range.getBoundingClientRect();
      if (!r.width || r.right <= clip.left || r.left >= clip.right || r.bottom <= clip.top || r.top >= clip.bottom) continue;
      u.chars[j] = { m: [1, 0, 0, 1, r.left - ox, r.top + asc - oy], w: r.width, top: r.top - oy, h: r.height };
    }
    u.clip = clip;
  }

  // Text inside an element that clips (a scrolling table, an ellipsis) is drawn only where it shows.
  function clipRect(el, root) {
    var c = { left: -1e9, top: -1e9, right: 1e9, bottom: 1e9 };
    for (var e = el; e && e !== document.body; e = e.parentElement) {
      var o = getComputedStyle(e);
      if (o.overflowX !== 'visible' || o.overflowY !== 'visible') {
        var r = e.getBoundingClientRect();
        if (o.overflowX !== 'visible') { c.left = Math.max(c.left, r.left); c.right = Math.min(c.right, r.right); }
        if (o.overflowY !== 'visible') { c.top = Math.max(c.top, r.top); c.bottom = Math.min(c.bottom, r.bottom); }
      }
      if (e === root) break;
    }
    return c;
  }
  function opacityOf(el, root) {
    var a = 1;
    for (var e = el; e && e !== document.body; e = e.parentElement || e.parentNode) {
      if (e.nodeType !== 1) break;
      a *= parseFloat(getComputedStyle(e).opacity);
      if (e === root) break;
    }
    return a;
  }
  I.plans = [];

  // Glyphs in motion are painted together each frame on the live layer: their real text, then
  // destination-in with one mask holding every glyph's pen path so far, drawn fat. One composite per
  // frame, whatever the number of glyphs. A glyph at the end of its strokes is exactly its fillText,
  // which is what the done layer draws when it finishes, at the same transform.
  function textOf(u, i, n) { return (u.svg ? u.node.textContent : u.node.data).substr(i, n); }

  // Setting a canvas font parses CSS; do it only when it changes.
  function font(ctx, f, color) {
    if (ctx.__inkFont !== f) { ctx.font = f; ctx.__inkFont = f; }
    if (ctx.__inkColor !== color) { ctx.fillStyle = color; ctx.__inkColor = color; }
  }

  var maskCanvas = null;
  function paintLive(job) {
    var items = [];
    job.active.forEach(function (a) {
      if (!a.text) return;
      if (a.type === 'wipe') { a.units.forEach(function (u) { items.push({ wipe: u, p: a.p }); }); return; }
      var u = a.unit, c = u.chars[a.i];
      if (!c) return;
      items.push({ u: u, c: c, g: a.sweep ? null : a.g, ch: textOf(u, a.i, a.n), i: a.i, n: a.n, p: a.p });
    });
    job.box = paintSet(job.live, items, dpr(), job.box);
  }

  // items: {u, c, g, ch, p} for a glyph (g null: swept in), or {wipe: unit, p}. prev: the device box
  // painted last time, cleared first. Returns the device box painted now.
  function paintSet(ctx, items, k, prev) {
    var W = ctx.canvas.width, H = ctx.canvas.height;
    maskCanvas = maskCanvas || document.createElement('canvas');
    if (maskCanvas.width < W || maskCanvas.height < H) { maskCanvas.width = Math.max(maskCanvas.width, W); maskCanvas.height = Math.max(maskCanvas.height, H); }
    var mx = maskCanvas.getContext('2d'), box = [1e9, 1e9, -1e9, -1e9];
    var grow = function (m, x0, y0, x1, y1) {
      [[x0, y0], [x1, y0], [x0, y1], [x1, y1]].forEach(function (q) {
        var X = (m[0] * q[0] + m[2] * q[1] + m[4]) * k, Y = (m[1] * q[0] + m[3] * q[1] + m[5]) * k;
        if (X < box[0]) box[0] = X; if (Y < box[1]) box[1] = Y; if (X > box[2]) box[2] = X; if (Y > box[3]) box[3] = Y;
      });
    };
    var at = function (c2, m) { c2.setTransform(m[0] * k, m[1] * k, m[2] * k, m[3] * k, m[4] * k, m[5] * k); };
    // Where each item can reach.
    items.forEach(function (it) {
      if (it.wipe) { linesOf(it.wipe).forEach(function (l) { l.chars.forEach(function (ci) { var c = it.wipe.chars[ci]; grow(c.m, -0.3 * it.wipe.size, -1.4 * it.wipe.size, c.w + 0.3 * it.wipe.size, 0.8 * it.wipe.size); }); }); return; }
      var e = it.u.size / 1000;
      if (it.g) grow(it.c.m, it.g.box[0] * e, it.g.box[1] * e, it.g.box[2] * e, it.g.box[3] * e);
      else grow(it.c.m, -0.3 * it.u.size, -1.4 * it.u.size, sweepWidth(it) + 0.3 * it.u.size, 0.8 * it.u.size);
    });
    box = [Math.max(0, Math.floor(box[0]) - 1), Math.max(0, Math.floor(box[1]) - 1), Math.min(W, Math.ceil(box[2]) + 1), Math.min(H, Math.ceil(box[3]) + 1)];
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    if (prev) ctx.clearRect(prev[0], prev[1], prev[2] - prev[0], prev[3] - prev[1]);
    if (!items.length || box[2] <= box[0] || box[3] <= box[1]) return null;
    ctx.clearRect(box[0], box[1], box[2] - box[0], box[3] - box[1]);
    mx.setTransform(1, 0, 0, 1, 0, 0);
    mx.globalCompositeOperation = 'source-over';
    mx.clearRect(box[0], box[1], box[2] - box[0], box[3] - box[1]);
    mx.fillStyle = mx.strokeStyle = '#000';
    mx.lineCap = 'round';
    mx.lineJoin = 'round';
    items.forEach(function (it) {
      if (it.wipe) { wipeUnit(ctx, mx, it.wipe, it.p, at); return; }
      var u = it.u, m = it.c.m, size = u.size;
      at(ctx, m);
      font(ctx, u.font, u.color);
      ctx.globalAlpha = u.alpha;
      ctx.fillText(it.ch, 0, 0);
      at(mx, m);
      if (it.g) {
        mx.lineWidth = it.g.reveal * size / 1000 * (u.bold ? 1.4 : 1) + 1.5 / (Math.hypot(m[0], m[1]) * k);
        strokePartial(mx, it.g, it.p, size / 1000);
      } else {
        mx.fillRect(-0.3 * size, -1.4 * size, 0.3 * size + sweepWidth(it) * ease(it.p), 2.2 * size);
      }
    });
    ctx.globalAlpha = 1;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.globalCompositeOperation = 'destination-in';
    ctx.drawImage(maskCanvas, box[0], box[1], box[2] - box[0], box[3] - box[1], box[0], box[1], box[2] - box[0], box[3] - box[1]);
    ctx.globalCompositeOperation = 'source-over';
    return box;
  }

  // One glyph, for the contact sheet and tests.
  I.paintGlyph = function (ctx, g, ch, m, u, p, k) {
    paintSet(ctx, [{ u: u, c: { m: m, w: 0 }, g: g, ch: ch, p: p }], k || 1, null);
  };

  // The pen's path so far: strokes in order, a pause for each lift.
  function strokePartial(ctx, g, p, unit) {
    var lift = C.lift * 1000, budget = p * (g.total + lift * g.s.length);
    ctx.beginPath();
    for (var i = 0; i < g.s.length && budget > 0; i++) {
      var s = g.s[i], L = g.lens[i];
      var use = Math.min(L, budget);
      budget -= L + lift;
      ctx.moveTo(s[0] * unit, s[1] * unit);
      if (s.length === 2) { ctx.lineTo(s[0] * unit + 0.01, s[1] * unit); continue; }
      for (var j = 2; j < s.length; j += 2) {
        var seg = Math.hypot(s[j] - s[j - 2], s[j + 1] - s[j - 1]);
        if (seg >= use) {
          var f = seg ? use / seg : 1;
          ctx.lineTo((s[j - 2] + (s[j] - s[j - 2]) * f) * unit, (s[j - 1] + (s[j + 1] - s[j - 1]) * f) * unit);
          break;
        }
        use -= seg;
        ctx.lineTo(s[j] * unit, s[j + 1] * unit);
      }
    }
    ctx.stroke();
  }

  // Characters with no pen strokes (symbols, emoji, ligatures) are wiped in from the left.
  function sweepWidth(it) {
    var w = it.c.w;
    if (it.n > 1) { var last = it.u.chars[it.i + it.n - 1]; if (last) w = last.m[4] + last.w - it.c.m[4]; }
    return w;
  }

  // A whole text node (or SVG text) swept left to right, line by line.
  function wipeUnit(ctx, mx, u, p, at) {
    var lines = linesOf(u), total = 0;
    lines.forEach(function (l) { total += l.w; });
    var reach = ease(p) * total;
    font(ctx, u.font, u.color);
    ctx.globalAlpha = u.alpha;
    lines.forEach(function (l) {
      if (reach <= 0) return;
      var front = l.x + Math.min(l.w, reach);
      reach -= l.w;
      l.chars.forEach(function (ci) {
        var c = u.chars[ci];
        if (c.m[4] >= front) return;
        at(ctx, c.m);
        ctx.fillText(textOf(u, ci, charLen(u, ci)), 0, 0);
        at(mx, c.m);
        mx.fillRect(-0.3 * u.size, -1.4 * u.size, 0.3 * u.size + Math.min(c.w, front - c.m[4]), 2.2 * u.size);
      });
    });
  }
  function charLen(u, i) {
    var code = (u.svg ? u.node.textContent : u.node.data).charCodeAt(i);
    return code >= 0xD800 && code < 0xDC00 ? 2 : 1;
  }
  function linesOf(u) {
    if (u.lines) return u.lines;
    var lines = [], cur = null;
    Object.keys(u.chars).map(Number).sort(function (a, b) { return a - b; }).forEach(function (i) {
      var c = u.chars[i], y = Math.round(c.m[5]);
      if (!cur || Math.abs(cur.y - y) > 2 || c.m[4] < cur.x + cur.w - 2) { cur = { y: y, x: c.m[4], w: 0, chars: [] }; lines.push(cur); }
      cur.w = Math.max(cur.w, c.m[4] + c.w - cur.x);
      cur.chars.push(i);
    });
    u.lines = lines;
    return lines;
  }

  // A finished glyph is drawn once on the done layer, with the same transform as its last frame.
  function drawDone(job, a) {
    if (!job.done) return;
    var ctx = job.done, k = dpr();
    var draw = function (u, i, n) {
      var c = u.chars[i];
      if (!c) return;
      ctx.setTransform(c.m[0] * k, c.m[1] * k, c.m[2] * k, c.m[3] * k, c.m[4] * k, c.m[5] * k);
      font(ctx, u.font, u.color);
      ctx.globalAlpha = u.alpha;
      ctx.fillText(textOf(u, i, n), 0, 0);
    };
    if (a.type === 'wipe') {
      a.units.forEach(function (u) { Object.keys(u.chars).forEach(function (i) { draw(u, +i, charLen(u, +i)); }); });
    } else draw(a.unit, a.i, a.n);
    ctx.globalAlpha = 1;
  }

  // ---- Watching pieces while they are written ------------------------------------------------
  // Something else changed a piece mid-job (an outline redrawn at a new size, a table's header rule
  // added a frame later, a resize): take it apart and attach again at the same point in time.
  var mo = window.MutationObserver ? new MutationObserver(function (records) {
    records.forEach(function (r) {
      jobs.forEach(function (job) { if (job.el && (job.el === r.target || job.el.contains(r.target))) job.reattach = true; });
    });
    schedule();
  }) : null;
  var ro = window.ResizeObserver ? new ResizeObserver(function (entries) {
    entries.forEach(function (e) {
      jobs.forEach(function (job) {
        if (job.el !== e.target) return;
        var size = e.contentRect.width + 'x' + e.contentRect.height;
        if (job.size && job.size !== size) job.reattach = true;
        job.size = size;
      });
    });
    schedule();
  }) : null;
  function watch(job) {
    if (mo) mo.observe(job.el, { childList: true, subtree: true, characterData: true });
    if (ro && !(job.el instanceof SVGElement)) { job.size = null; ro.observe(job.el); }
  }
  function unwatch(job) {
    if (ro && job.el && !(job.el instanceof SVGElement)) ro.unobserve(job.el);
    // A MutationObserver cannot forget one target; re-observe the jobs still running.
    if (mo) {
      mo.disconnect();
      jobs.forEach(function (j) { if (j !== job && j.el && j.state !== 'done') mo.observe(j.el, { childList: true, subtree: true, characterData: true }); });
    }
  }
  // Ink's own changes to the DOM are not news.
  function drain() { if (mo) mo.takeRecords(); }

  // Sketch.outline draws a new SVG when its element changes size: it continues the old one's job.
  I.replaced = function (old, svg) {
    jobs.forEach(function (job) {
      if (job.el !== old) return;
      // A piece still waiting keeps waiting, hidden: the new drawing is hidden in its place.
      if (job.state === 'parked') { park(job, svg); return; }
      job.el = svg; job.reattach = true; schedule();
    });
    pending.forEach(function (p) { if (p.el === old) p.el = svg; });
  };

  // ---- The reader ---------------------------------------------------------------------------------
  // A click or a key finishes everything on screen at once. What follows the reader's own action
  // within a second is drawn quickly; see I.fresh for runs.
  function act(e) {
    if (e.type === 'keydown' && (e.key === 'Shift' || e.key === 'Control' || e.key === 'Alt' || e.key === 'Meta')) return;
    if (watching()) { watchInput(e); return; }
    lastAction = now();
    if (jobs.length || pending.length) I.finishAll();
  }
  ['pointerdown', 'keydown'].forEach(function (type) { window.addEventListener(type, act, true); });
  ['input', 'change'].forEach(function (type) { window.addEventListener(type, function () { lastAction = now(); }, true); });
  // Scrolling the page brings waiting pieces into view; scrolling inside a piece moves its text.
  window.addEventListener('scroll', function (e) {
    if (!jobs.length) return;
    var t = e.target;
    jobs.forEach(function (job) { if (job.state === 'running' && t.nodeType === 1 && job.el.contains(t) && job.textRoot) job.reattach = true; });
    schedule();
  }, true);
  window.addEventListener('resize', function () { jobs.forEach(function (job) { job.reattach = true; }); schedule(); });
  window.addEventListener('beforeprint', function () { printing = true; I.finishAll(); });
  window.addEventListener('afterprint', function () { printing = false; });
  if (document.fonts) {
    document.fonts.ready.then(function () { fontsReady = true; schedule(); });
    // Fonts that never arrive must not leave a page hidden.
    setTimeout(function () { if (!fontsReady) { fontsReady = true; schedule(); } }, 3000);
  } else fontsReady = true;

  // ---- Watching ---------------------------------------------------------------------------------
  // Where the pen is now, in client coordinates: the end of the stroke being drawn.
  function penPoint() {
    var job = null;
    for (var i = 0; i < jobs.length; i++) if (jobs[i].state === 'running' && jobs[i].watch) { job = jobs[i]; break; }
    if (!job || !job.el) return null;
    var a = null;
    for (var k = job.active.length - 1; k >= 0; k--) if (!job.active[k].with) { a = job.active[k]; break; }
    if (!a) return pen.at || null;
    var p = Math.max(0, Math.min(1, a.p || 0)), r;
    try {
      if (a.type === 'path') {
        var L = a.len * ease(p), pt = a.el.getPointAtLength(job.opts.reverse ? a.len - L : L), m = a.el.getScreenCTM();
        if (m) return (pen.at = { x: m.a * pt.x + m.c * pt.y + m.e, y: m.b * pt.x + m.d * pt.y + m.f });
      } else if (a.type === 'glyph' && job.layers) {
        var c = a.unit.chars[a.i];
        if (!c) return pen.at || null;
        var q = a.g ? strokePoint(a.g, p) : [0.6 * p * 1000, -300], e = a.unit.size / 1000;
        var x = c.m[0] * q[0] * e + c.m[2] * q[1] * e + c.m[4], y = c.m[1] * q[0] * e + c.m[3] * q[1] * e + c.m[5];
        var lr = job.layers[1].getBoundingClientRect(), z = lr.width / (job.layers[1].offsetWidth || lr.width || 1);
        return (pen.at = { x: lr.left + x * z, y: lr.top + y * z });
      } else {
        var el = a.el || (a.nodes && a.nodes[0]) || (a.units && a.units[0] && a.units[0].el);
        if (!el) return pen.at || null;
        r = el.getBoundingClientRect();
        return (pen.at = { x: r.left + r.width * p, y: r.top + r.height / 2 });
      }
    } catch (err) { /* a node that went away mid-frame */ }
    return pen.at || null;
  }
  // The point a glyph's pen has reached, in its em/1000 units (the end of strokePartial's path).
  function strokePoint(g, p) {
    var lift = C.lift * 1000, budget = p * (g.total + lift * g.s.length), at = [g.s[0][0], g.s[0][1]];
    for (var i = 0; i < g.s.length && budget > 0; i++) {
      var s = g.s[i], use = Math.min(g.lens[i], budget);
      budget -= g.lens[i] + lift;
      at = [s[0], s[1]];
      for (var j = 2; j < s.length; j += 2) {
        var seg = Math.hypot(s[j] - s[j - 2], s[j + 1] - s[j - 1]);
        if (seg >= use) { var f = seg ? use / seg : 1; at = [s[j - 2] + (s[j] - s[j - 2]) * f, s[j - 1] + (s[j + 1] - s[j - 1]) * f]; break; }
        use -= seg;
        at = [s[j], s[j + 1]];
      }
    }
    return at;
  }

  // The page follows the pen, keeping it between a fifth and two thirds of the way down. A reader who
  // scrolls stops it; it starts again when they leave the pen on screen for a moment, or press a key.
  var view = { on: true, target: null, last: 0, quiet: 0, scroller: null };
  function scrollerOf(el) {
    for (var e = el && el.parentElement; e && e !== document.body && e !== document.documentElement; e = e.parentElement) {
      var o = getComputedStyle(e).overflowY;
      if ((o === 'auto' || o === 'scroll') && e.scrollHeight > e.clientHeight + 4) return e;
    }
    return document.scrollingElement || document.documentElement;
  }
  function follow() {
    var t = now(), dt = Math.min(100, t - (view.last || t));
    view.last = t;
    var at = penPoint();
    nib(at);
    if (!at) return;
    var job = jobs.filter(function (j) { return j.state === 'running' && j.watch; })[0];
    var sc = job ? scrollerOf(job.el) : view.scroller;
    if (!sc) return;
    if (sc !== view.scroller) { view.scroller = sc; view.target = null; }
    var page = sc === document.scrollingElement || sc === document.documentElement;
    var top = page ? 0 : sc.getBoundingClientRect().top + sc.clientTop, h = page ? viewHeight() : sc.clientHeight;
    var y = at.y - top;
    if (!view.on) {
      // Back on when the reader has stopped scrolling with the pen in view.
      if (t - view.quiet > 1500 && y > 0.1 * h && y < 0.8 * h) view.on = true;
      else return;
    }
    var max = sc.scrollHeight - sc.clientHeight;
    if (view.target == null && (y > 0.66 * h || y < 0.15 * h)) view.target = Math.max(0, Math.min(max, sc.scrollTop + y - 0.36 * h));
    if (view.target == null) return;
    var d = view.target - sc.scrollTop;
    if (Math.abs(d) < 1 || (d > 0 && sc.scrollTop >= max - 1) || (d < 0 && sc.scrollTop <= 0)) { view.target = null; return; }
    // Eased, and never slower than a little per frame, so a long way is covered smoothly.
    var move = d * (1 - Math.exp(-dt / 280));
    if (Math.abs(move) < 1.5) move = Math.sign(d) * Math.min(Math.abs(d), 1.5);
    view.expect = sc.scrollTop + move;
    sc.scrollTop = view.expect;
  }
  function stopFollowing() { view.on = false; view.target = null; view.quiet = now(); }
  ['wheel', 'touchmove'].forEach(function (type) {
    window.addEventListener(type, function () { if (watching() && I.busy()) stopFollowing(); }, { passive: true, capture: true });
  });
  // A press on a scrollbar lands on the scrolling element itself.
  window.addEventListener('pointerdown', function (e) {
    if (watching() && I.busy() && (e.target === document.documentElement || e.target === view.scroller)) stopFollowing();
  }, true);

  // A small dot at the end of the stroke being drawn.
  var tip = null;
  function nib(at) {
    if (!at || pen.hideNib) { if (tip) { tip.remove(); tip = null; } return; }
    if (!tip) {
      tip = document.createElement('div');
      tip.className = 'ink-nib';
      tip.setAttribute('aria-hidden', 'true');
      document.documentElement.appendChild(tip);
    }
    tip.style.transform = 'translate(' + at.x.toFixed(1) + 'px,' + at.y.toFixed(1) + 'px)';
  }

  // The controls: a small mark in the corner while the pen has work, gone when it has none.
  var mark = null;
  function penWork() { return pending.length > 0 || jobs.some(function (j) { return j.watch || j.state === 'parked'; }); }
  function speedLabel() { var v = pen.speed; return (v < 1 ? String(v).replace(/^0/, '') : String(v)) + '×'; }
  function controls() {
    var show = watching() && penWork();
    if (!show) {
      if (mark) { mark.remove(); mark = null; }
      nib(null);
      return;
    }
    if (!mark) {
      var b = function (act, text, title) {
        var x = document.createElement('button');
        x.type = 'button';
        x.setAttribute('data-ink-act', act);
        x.textContent = text;
        x.title = title;
        return x;
      };
      mark = document.createElement('div');
      mark.className = 'ink-watch';
      mark.setAttribute('role', 'toolbar');
      mark.setAttribute('aria-label', 'Watching the page being written');
      [b('pause', '', 'Pause or go on (space, p)'), b('slower', '−', 'Slower (-)'), document.createElement('span'), b('faster', '+', 'Faster (+)'),
        b('piece', '⇥', 'Finish this piece (n)'), b('all', '⇥⇥', 'Write everything now (e)'), b('follow', '↧', 'Follow the pen again (any key)'),
        b('leave', '×', 'Stop watching (Esc)')].forEach(function (x) { mark.appendChild(x); });
      mark.children[2].className = 'ink-speed';
      mark.addEventListener('click', function (e) {
        var x = e.target.closest && e.target.closest('[data-ink-act]');
        if (x) command(x.getAttribute('data-ink-act'));
      });
      document.documentElement.appendChild(mark);
    }
    var pause = mark.children[0], label = pen.held ? '▸' : '‖';
    if (pause.textContent !== label) pause.textContent = label;
    if (mark.children[2].textContent !== speedLabel()) mark.children[2].textContent = speedLabel();
    mark.classList.toggle('ink-held', pen.held);
    mark.classList.toggle('ink-lost', !view.on);
  }

  function command(what) {
    penClock();
    if (what === 'pause') { pen.held = !pen.held; view.on = true; }
    else if (what === 'faster' || what === 'slower') {
      var i = 0;
      while (i < SPEEDS.length - 1 && SPEEDS[i] < pen.speed - 1e-9) i++;
      pen.speed = SPEEDS[Math.max(0, Math.min(SPEEDS.length - 1, i + (what === 'faster' ? 1 : -1)))];
      try { localStorage.setItem('nb.ink.speed', String(pen.speed)); } catch (e) { /* private window */ }
    } else if (what === 'piece') {
      jobs.slice().forEach(function (j) { if (j.state === 'running' && j.watch) finish(j); });
      pen.free = pen.t;
    } else if (what === 'all') I.finishAll();
    else if (what === 'follow') view.on = true;
    else if (what === 'leave') { I.leave(); return; }
    controls();
    schedule();
  }
  I.command = command;
  I.speed = function () { return pen.speed; };
  I.held = function () { return pen.held; };
  I.following = function () { return view.on; };

  // Keys while watching. Keys typed into a control belong to it; the pen's keys apply only while it
  // has work. Presenting (render-present.js) keeps Space and Escape for itself.
  var SCROLLKEYS = { PageUp: 1, PageDown: 1, ArrowUp: 1, ArrowDown: 1, Home: 1, End: 1 };
  function watchInput(e) {
    if (e.type !== 'keydown') {
      // A press on the page is the reader's own action; on the controls it is not.
      if (!(e.target && e.target.closest && e.target.closest('.ink-watch'))) lastAction = now();
      return;
    }
    lastAction = now();
    var t = e.target, tag = t && t.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || (t && t.isContentEditable)) return;
    if (e.ctrlKey || e.metaKey || e.altKey || !penWork()) return;
    var key = e.key, presenting = document.body.classList.contains('presenting');
    if (SCROLLKEYS[key] && !presenting) { stopFollowing(); return; }
    view.on = true;
    var what = null;
    if ((key === ' ' && !presenting && tag !== 'BUTTON') || key === 'p' || key === 'P') what = 'pause';
    else if (key === '+' || key === '=' || key === ']') what = 'faster';
    else if (key === '-' || key === '_' || key === '[') what = 'slower';
    else if (key === 'n' || key === 'N') what = 'piece';
    else if (key === 'e' || key === 'E') what = 'all';
    // Escape closes an open menu first.
    else if (key === 'Escape' && !presenting && !document.querySelector('.open')) what = 'leave';
    if (!what) { schedule(); return; }
    e.preventDefault();
    e.stopPropagation();
    command(what);
  }

  // A cell's outline is clipped to its left edge, which is the end of the path: draw it backwards,
  // and pace it by the part that shows.
  I.bracket = function (box) {
    var w = box.offsetWidth, h = box.offsetHeight;
    return { reverse: true, visible: Math.min(1, (h + 16) / (2 * (w + h) || 1)) };
  };

  // For tests and the contact sheet.
  I.jobs = function () { return jobs.slice(); };
  I.config = C;

  return I;
})();
