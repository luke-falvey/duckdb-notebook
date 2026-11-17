// Presenting: a notebook shown one screen at a time (docs/DESIGN.md §18). Shared by the editor
// (app.js) and exported pages (viewer.js), which stay in charge of drawing cells:
//
//   Present.start({ render, enter, leave })   once the page can draw; opens presenting if the URL asks
//   Present.begin(items, grid, scroller)      in render(), before the cells are built: which to build
//   Present.finish(scroller, grid, note)      after they are built: fit the screen to the window
//   Present.reset(grid)                       in render() when not presenting
//
// A screen is a row of the reading layout: a cell, or the consecutive cells whose spans share a row.
// It is laid out at the reading width and then scaled as a whole, so what an author arranged with
// span= is what the audience sees, larger. The current screen lives in the URL hash (#present=3).
var Present = (function () {
  'use strict';
  var P = { on: false, at: 0, shown: 0, count: 0, run: 0, scale: 1, first: null };
  var host = null, mark = null, stage = null, observer = null, lastShown = -1, lastBase = 0, ownFullscreen = false, queued = 0;
  // The reading layout: 12 columns in 1000px with 22px between them (.nb-cells in style.css).
  var COLS = 12, WIDTH = 1000, GAP = 22;
  // A screen is drawn at up to MAX times the size of the reading view. One too tall for the window is
  // drawn smaller, down to FLOOR, and then scrolls.
  var MAX = 2.5, FLOOR = 0.8;
  // Nor is its layout narrower than this, however little it holds.
  var NARROW = 240;
  // Clicks on these act on them; a click anywhere else moves on.
  var ACTS = 'a, button, input, select, textarea, label, option, summary, .nb-chart-body, .nb-input, .nb-tabs, .nb-grid-footer, .nb-present-mark, .nb-banner';

  function span(item) { return Math.max(1, Math.min(COLS, +item.span || COLS)); }

  // items: [{id, span, kind, screen, visible}] in file order. Returns the screens, each a list of
  // indexes into items. A cell starts a new screen unless it fits the row being filled, follows only
  // inputs (an input is shown with what comes after it), or says screen=join; screen=new always
  // starts one and screen=skip leaves the cell out.
  P.screens = function (items) {
    var out = [], cur = null, row = 0, lead = false;
    items.forEach(function (item, i) {
      if (!item.visible || item.screen === 'skip') return;
      var w = span(item), fresh;
      if (!cur || item.screen === 'new') fresh = true;
      else if (item.screen === 'join' || lead) fresh = false;
      else fresh = row + w > COLS;
      if (fresh) { cur = []; out.push(cur); row = 0; lead = true; }
      cur.push(i);
      if (item.kind !== 'input') lead = false;
      row = row + w > COLS ? w : row + w;
    });
    return out;
  };

  // The widest row of a screen, in columns.
  function widest(items, screen) {
    var row = 0, max = 0;
    screen.forEach(function (i) {
      var w = span(items[i]);
      row = row + w > COLS ? w : row + w;
      max = Math.max(max, row);
    });
    return max || COLS;
  }

  function pads(scroller) {
    return { x: Math.max(24, scroller.clientWidth * 0.06), y: Math.max(24, scroller.clientHeight * 0.07) };
  }
  // The width the reading layout is given: 1000px, or what a narrow window has.
  function base(scroller) {
    return Math.min(WIDTH, Math.max(320, scroller.clientWidth - 2 * pads(scroller).x));
  }

  // Chooses the current screen and gives the grid its width. Returns {index: true} for the items to build.
  P.begin = function (items, grid, scroller) {
    var all = P.screens(items), show = {};
    P.count = all.length;
    P.shown = Math.max(0, Math.min(P.at, all.length - 1));
    var screen = all[P.shown] || [], n = widest(items, screen);
    lastBase = base(scroller);
    var col = (lastBase - (COLS - 1) * GAP) / COLS;
    grid.classList.add('nb-screen');
    grid.__nbWidth = n * col + (n - 1) * GAP;
    grid.style.width = grid.__nbWidth + 'px';
    grid.style.gridTemplateColumns = 'repeat(' + n + ', minmax(0, 1fr))';
    screen.forEach(function (i) { show[i] = true; });
    P.first = screen.length ? items[screen[0]].id : P.first;
    return show;
  };

  P.reset = function (grid) {
    grid.classList.remove('nb-screen');
    grid.classList.remove('nb-measure');
    ['width', 'gridTemplateColumns', 'transform', 'marginLeft', 'marginTop', 'marginBottom'].forEach(function (k) { grid.style[k] = ''; });
  };

  // A screen is as wide as what it holds, up to the width of its row in the reading layout: a title, a
  // chart or a row of KPIs is then centred and large, not a wide box with its content at the left.
  // Outputs are size containers (their KPIs are sized by them), which hides their width from the
  // layout, and a table shows only the columns that fit the width it has; .nb-measure (style.css)
  // lifts both while the grid is measured, so the answer does not depend on the width before.
  function shrink(grid) {
    var full = grid.__nbWidth || grid.offsetWidth;
    grid.classList.add('nb-measure');
    grid.style.width = 'max-content';
    var w = Math.ceil(grid.offsetWidth) + 2;
    grid.classList.remove('nb-measure');
    grid.style.width = Math.min(full, Math.max(Math.min(NARROW, full), w)) + 'px';
  }

  // Scales the screen to the window, centred, with generous margins. Larger than the reading view when
  // there is room. A screen taller than the window is drawn smaller so it can be seen whole, but only
  // down to FLOOR: smaller text cannot be read from the back of a room, so past that it scrolls.
  function fit() {
    queued = 0;
    if (!P.on || !stage || !stage.grid.isConnected) return;
    var scroller = stage.scroller, grid = stage.grid, pad = pads(scroller);
    shrink(grid);
    var cw = scroller.clientWidth, ch = scroller.clientHeight, w = grid.offsetWidth, h = grid.offsetHeight;
    if (!w || !h) return;
    var wide = Math.min((cw - 2 * pad.x) / w, MAX);
    var s = Math.max(Math.min(wide, (ch - 2 * pad.y) / h), Math.min(FLOOR, wide));
    P.scale = s;
    grid.style.transform = 'scale(' + s.toFixed(4) + ')';
    grid.style.marginLeft = Math.max(0, (cw - w * s) / 2) + 'px';
    grid.style.marginTop = Math.max(pad.y, (ch - h * s) / 2) + 'px';
    // The layout still counts the grid at its own height; this makes it count the height that shows.
    grid.style.marginBottom = (h * s - h + pad.y) + 'px';
  }

  P.finish = function (scroller, grid, note) {
    if (!P.count) grid.appendChild(NB.el('div', { 'class': 'nb-present-empty', text: note || 'Nothing to show.' }));
    stage = { scroller: scroller, grid: grid };
    fit();
    if (window.ResizeObserver) {
      // Not inside the observer's own callback: fitting changes the size it watches.
      if (!observer) observer = new ResizeObserver(function () { if (!queued) queued = requestAnimationFrame(fit); });
      observer.disconnect();
      observer.observe(grid);
    }
    if (P.shown !== lastShown) { scroller.scrollTop = 0; lastShown = P.shown; }
    indicator();
    writeHash(P.at + 1);
  };

  // The page number, in the corner, with a faint way out for those without a keyboard.
  function indicator() {
    if (!mark) {
      mark = NB.el('div', { 'class': 'nb-present-mark' }, [
        NB.el('button', { 'class': 'nb-present-leave', text: '✕', title: 'Leave (Esc)', 'aria-label': 'Leave presenting', onclick: function () { P.leave(); } }),
        NB.el('span', { 'class': 'nb-present-page', title: '→ or a click: next · ←: back · Home, End · F: full screen · Esc: leave' })
      ]);
      document.body.appendChild(mark);
    }
    var text = P.count ? (P.shown + 1) + ' / ' + P.count : '';
    var page = mark.lastChild;
    if (page.textContent !== text) page.textContent = text;
    if (window.Ink) Ink.present(mark, 'present/mark', P.run + ':' + text);
  }

  // ---- The URL: #present opens the first screen, #present=3 the third. Other flags are kept. ----
  function readHash() {
    var m = /(?:^#|&)present(?:=(\d+))?(?=&|$)/.exec(location.hash);
    return m ? Math.max(1, +(m[1] || 1)) : 0;
  }
  function writeHash(n) {
    if (readHash() === n) return;
    var rest = location.hash.replace(/^#/, '').split('&').filter(function (p) { return p && !/^present(=|$)/.test(p); });
    if (n) rest.unshift(n > 1 ? 'present=' + n : 'present');
    try {
      history.replaceState(history.state, '', location.pathname + location.search + (rest.length ? '#' + rest.join('&') : ''));
    } catch (e) { /* a page that may not rewrite its address presents without it */ }
  }

  // ---- Entering, leaving, moving --------------------------------------------------------------
  P.prefix = function () { return P.on ? 'p' + P.run : ''; };

  P.enter = function (at) {
    if (P.on || !host) return;
    P.on = true;
    // Each presentation is written afresh: its pieces are keyed to this run (P.prefix).
    P.run++;
    P.at = Math.max(0, at || 0);
    lastShown = -1;
    document.body.classList.add('presenting');
    window.scrollTo(0, 0);
    if (window.Ink) Ink.fresh();
    if (host.enter) host.enter();
    host.render();
  };

  P.leave = function () {
    if (!P.on) return;
    P.on = false;
    document.body.classList.remove('presenting');
    if (mark) { mark.remove(); mark = null; }
    if (observer) observer.disconnect();
    stage = null;
    writeHash(0);
    if (ownFullscreen && document.fullscreenElement && document.exitFullscreen) document.exitFullscreen();
    ownFullscreen = false;
    if (host.leave) host.leave();
    host.render();
    // Back in the notebook where the presentation stopped.
    var cell = P.first == null ? null : document.querySelector('.nb-cell[data-idx="' + P.first + '"]');
    if (cell) cell.scrollIntoView({ block: 'center' });
  };

  // A screen arriving for the first time is written at the pen's own pace; one already shown is on
  // the page at once, because Ink leaves a piece it has written alone.
  P.go = function (n) {
    if (!P.on) return;
    n = Math.max(0, Math.min(n, P.count - 1));
    if (n === P.shown) return;
    if (window.Ink && n > P.shown) Ink.fresh();
    P.at = n;
    host.render();
  };
  P.next = function () { P.go(P.shown + 1); };
  P.back = function () { P.go(P.shown - 1); };

  P.fullscreen = function () {
    var el = document.documentElement;
    if (document.fullscreenElement) { if (document.exitFullscreen) document.exitFullscreen(); }
    else if (el.requestFullscreen) { ownFullscreen = true; el.requestFullscreen().catch(function () { ownFullscreen = false; }); }
  };

  P.start = function (h) {
    host = h;
    var n = readHash();
    if (n) P.enter(n - 1);
  };

  document.addEventListener('keydown', function (e) {
    if (!P.on || e.ctrlKey || e.metaKey || e.altKey) return;
    var t = e.target, tag = t && t.tagName, key = e.key;
    // Keys typed into a control belong to it, and Space or Enter on a button presses it.
    if ((tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || (t && t.isContentEditable)) && key !== 'Escape') return;
    if ((tag === 'BUTTON' || tag === 'A') && (key === ' ' || key === 'Enter')) return;
    if (key === 'ArrowRight' || key === 'PageDown' || (key === ' ' && !e.shiftKey)) P.next();
    else if (key === 'ArrowLeft' || key === 'PageUp' || (key === ' ' && e.shiftKey)) P.back();
    else if (key === 'Home') P.go(0);
    else if (key === 'End') P.go(P.count - 1);
    else if (key === 'Escape') P.leave();
    else if (key === 'f' || key === 'F') P.fullscreen();
    else return;
    e.preventDefault();
  });

  document.addEventListener('click', function (e) {
    if (!P.on || e.defaultPrevented || e.button) return;
    var t = e.target;
    if (!t || !t.closest || t.closest(ACTS)) return;
    var picked = window.getSelection && window.getSelection();
    if (picked && !picked.isCollapsed) return;
    P.next();
  });

  // A picked value should not leave the arrow keys with the control that picked it.
  document.addEventListener('change', function (e) {
    var t = e.target;
    if (P.on && t && (t.tagName === 'SELECT' || t.type === 'checkbox' || t.type === 'range') && !t.multiple) t.blur();
  });

  window.addEventListener('resize', function () {
    if (!P.on || !stage) return;
    if (base(stage.scroller) !== lastBase) host.render(); else fit();
  });
  // A screen opened from a link is first measured before the handwriting font has loaded.
  if (document.fonts && document.fonts.addEventListener) document.fonts.addEventListener('loadingdone', fit);
  window.addEventListener('hashchange', function () {
    if (!host) return;
    var n = readHash();
    if (n && !P.on) P.enter(n - 1);
    else if (n) P.go(n - 1);
    else P.leave();
  });
  // Paper gets the whole notebook: printing leaves presenting first.
  window.addEventListener('beforeprint', function () { P.leave(); });

  return P;
})();
