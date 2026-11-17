// Peek under the pen (docs/DESIGN.md section 17). While a query cell is being edited, the first rows
// of the CTE or sub-query the caret is in appear in a small paper note beside the code. The server
// finds the part with DuckDB's parser and runs it alone, read-only (POST /api/peek); this file only
// decides when to ask and where to put the answer. Nothing is asked before the reader has run the
// notebook, and nothing is shown when there is nothing to show.
var AppPeek = (function () {
  'use strict';
  var P = { rows: 5, delay: 320 };
  var $ = function (id) { return document.getElementById(id); };
  function stored(key) { try { return localStorage.getItem(key); } catch (e) { return null; } }
  P.enabled = stored('nb.peek') !== 'off';

  var note = null;       // the note on the page, if any
  var shown = null;      // what it shows: {id, signature, cell}
  var dismissed = null;  // the part the reader closed with Escape: {id, cell}
  var timer = null, seq = 0;

  // The SQL editor of a query cell: not a chart spec, not prose.
  function editor(el) {
    if (!el || el.tagName !== 'TEXTAREA' || !el.classList.contains('nb-code-over') || el.classList.contains('nb-spec')) return null;
    var cell = el.closest('.nb-cell');
    return cell && cell.getAttribute('data-kind') === 'sql' ? el : null;
  }
  function cellOf(ta) { return +ta.closest('.nb-cell').getAttribute('data-idx'); }
  // The server counts bytes; the textarea counts UTF-16 units.
  function bytes(text, chars) { return new TextEncoder().encode(text.slice(0, chars)).length; }
  // Opening a notebook does not run it: there is nothing to peek at until the reader has run something.
  function trusted() {
    return App.cells.some(function (c) { return c.result && c.result.status && c.result.status !== 'skipped'; });
  }

  P.hide = function () {
    clearTimeout(timer);
    seq++;
    if (note) { note.remove(); note = null; }
    shown = null;
    mark(null);
  };

  // The caret moved or the text changed: ask again once it has rested.
  P.moved = function (ta) {
    clearTimeout(timer);
    if (!P.enabled || !editor(ta) || App.running || App.reading) { P.hide(); return; }
    if (ta.selectionStart !== ta.selectionEnd) { P.hide(); return; }
    if (note) place(ta);
    timer = setTimeout(function () { ask(ta); }, P.delay);
  };

  function ask(ta) {
    if (!document.body.contains(ta)) ta = document.activeElement;
    if (!editor(ta) || document.activeElement !== ta || !P.enabled || App.running || !trusted()) { P.hide(); return; }
    var sql = ta.value, caret = ta.selectionStart, cell = cellOf(ta), mine = ++seq;
    if (!sql.trim()) { P.hide(); return; }
    App.api('POST', '/api/peek', { sql: sql, cursor: bytes(sql, caret), rows: P.rows }).then(function (r) {
      if (mine !== seq) return;
      // The answer is for a caret that has since moved, or a cell that has since changed: drop it.
      var now = editor(document.activeElement);
      if (!now || now.value !== sql || now.selectionStart !== caret || cellOf(now) !== cell) return;
      if (!r || r.reason || !r.columns) { dismissed = null; P.hide(); return; }
      var id = r.kind + '/' + r.name + '/' + r.start;
      if (dismissed && dismissed.id === id && dismissed.cell === cell) return;
      dismissed = null;
      show(r, id, cell, now, sql);
    }).catch(function () { if (mine === seq) P.hide(); });
  }

  function label(r) { return r.name || (r.kind === 'cte' ? 'cte' : 'sub-query'); }

  // Characters, from the bytes the server counts in.
  function chars(sql, at) { return new TextDecoder().decode(new TextEncoder().encode(sql).slice(0, at)).length; }

  function show(r, id, cell, ta, sql) {
    var signature = JSON.stringify([id, r.columns, r.rows, r.total, r.more]);
    var part = { sql: sql, from: chars(sql, r.start), to: chars(sql, r.end) };
    if (note && shown && shown.signature === signature && shown.cell === cell) {
      shown.sql = part.sql; shown.from = part.from; shown.to = part.to;
      place(ta);
      return;
    }
    if (note) note.remove();
    var limit = window.innerWidth < 700 ? 250 : 340;
    note = NB.el('div', { 'class': 'nb-peek', role: 'note', 'aria-label': 'First rows of ' + label(r) });
    note.appendChild(NB.el('div', { 'class': 'nb-peek-head' }, [
      NB.el('span', { 'class': 'nb-peek-name', text: label(r) }),
      r.name ? NB.el('span', { 'class': 'nb-peek-kind', text: r.kind === 'cte' ? 'cte' : 'sub-query' }) : null
    ]));
    var count = Math.min(r.columns.length, 6);
    var table = NB.el('table', { 'class': 'nb-peek-grid' });
    var build = function () {
      table.innerHTML = '';
      var head = NB.el('tr');
      r.columns.slice(0, count).forEach(function (c) { head.appendChild(NB.el('th', { text: c.name, title: c.type })); });
      table.appendChild(NB.el('thead', {}, [head]));
      var body = NB.el('tbody');
      r.rows.forEach(function (row) {
        var tr = NB.el('tr');
        row.slice(0, count).forEach(function (v, k) {
          var numeric = NB.isNumericType(r.columns[k].type) && typeof v === 'number';
          tr.appendChild(NB.el('td', { 'class': v == null ? 'nb-null' : numeric ? 'nb-num' : null, text: NB.formatValue(v, r.columns[k].type) }));
        });
        body.appendChild(tr);
      });
      table.appendChild(body);
    };
    build();
    note.appendChild(table);
    var rows = r.rows.length, foot;
    if (!rows) foot = 'no rows';
    else if (r.total != null && r.total > rows) foot = 'first ' + rows + ' of ' + NB.formatNumber(r.total, ',') + ' rows';
    else if (r.more) foot = 'first ' + rows + ' rows';
    else foot = rows + (rows === 1 ? ' row' : ' rows');
    var footer = NB.el('div', { 'class': 'nb-peek-foot', text: foot });
    note.appendChild(footer);
    document.body.appendChild(note);
    // A note is small: columns that do not fit are left out and counted.
    while (count > 1 && note.offsetWidth > limit) { count--; build(); }
    if (count < r.columns.length) footer.textContent = foot + ' · ' + count + ' of ' + r.columns.length + ' columns';
    shown = { id: id, signature: signature, cell: cell, sql: part.sql, from: part.from, to: part.to };
    Sketch.outline(note, { fill: '#fdfcf8', width: 1.3, seed: 'peek' + id });
    place(ta);
    // Written by the pen like the rest of the page; the reader's own caret brought it, so it is quick.
    if (window.Ink) Ink.present(note, 'peek', signature);
  }

  // One box per line of code: where it is and how far its text reaches to the right.
  function lines(pre) {
    var range = document.createRange(), out = [];
    range.selectNodeContents(pre);
    Array.prototype.forEach.call(range.getClientRects(), function (r) {
      if (!r.width && !r.height) return;
      var last = out[out.length - 1];
      if (last && Math.abs(last.top - r.top) < 4) { last.right = Math.max(last.right, r.right); last.bottom = Math.max(last.bottom, r.bottom); }
      else out.push({ top: r.top, bottom: r.bottom, right: r.right });
    });
    return out;
  }

  // The text node and offset of a character of the code, in the highlighted <pre> behind the
  // textarea, which holds the same text with the same metrics.
  function charAt(pre, offset) {
    var walker = document.createTreeWalker(pre, NodeFilter.SHOW_TEXT), node, last = null;
    while ((node = walker.nextNode())) {
      var n = node.nodeValue.length;
      if (offset <= n) return { node: node, offset: offset };
      offset -= n;
      last = node;
    }
    return last ? { node: last, offset: last.nodeValue.length } : null;
  }

  // Where the caret is on screen.
  function caretBox(pre, offset, fallback) {
    var at = charAt(pre, offset);
    if (!at) return fallback;
    var n = at.node.nodeValue.length, from = Math.max(0, Math.min(at.offset, n - 1)), range = document.createRange();
    range.setStart(at.node, from);
    range.setEnd(at.node, Math.min(n, from + 1));
    var rects = range.getClientRects();
    return rects.length ? rects[0] : fallback;
  }

  // The part the note describes is marked in the code with a highlighter, as long as the code is
  // still the text that was asked about. The marks sit behind the text and are no part of it.
  function mark(ta) {
    Array.prototype.forEach.call(document.querySelectorAll('.nb-peek-marks'), function (m) { m.remove(); });
    if (!note || !shown || !ta || ta.value !== shown.sql) return;
    var wrap = ta.closest('.nb-code-wrap'), pre = wrap && wrap.querySelector('.nb-code-hl');
    if (!pre) return;
    var origin = wrap.getBoundingClientRect(), rows = [], text = shown.sql, at = shown.from;
    // Line by line, so the marker runs under the words and not under their indentation.
    text.slice(shown.from, shown.to).split('\n').forEach(function (line) {
      var from = at + (line.length - line.replace(/^\s+/, '').length), to = at + line.replace(/\s+$/, '').length;
      at += line.length + 1;
      var a = charAt(pre, from), b = charAt(pre, to);
      if (to <= from || !a || !b) return;
      var range = document.createRange();
      range.setStart(a.node, a.offset);
      range.setEnd(b.node, b.offset);
      Array.prototype.forEach.call(range.getClientRects(), function (r) {
        if (r.width < 1) return;
        var last = rows[rows.length - 1];
        if (last && Math.abs(last.top - r.top) < 4) { last.left = Math.min(last.left, r.left); last.right = Math.max(last.right, r.right); }
        else rows.push({ top: r.top, bottom: r.bottom, left: r.left, right: r.right });
      });
    });
    var layer = NB.el('div', { 'class': 'nb-peek-marks', 'aria-hidden': 'true' });
    rows.forEach(function (r) {
      layer.appendChild(NB.el('i', { style: 'left:' + (r.left - origin.left - 1) + 'px;top:' + (r.top - origin.top) + 'px;width:' + (r.right - r.left + 2) + 'px;height:' + (r.bottom - r.top) + 'px' }));
    });
    wrap.insertBefore(layer, wrap.firstChild);
  }

  // Puts the note where it covers no code: in the page's right margin when the window is wide enough,
  // else at the right of the cell beside lines short enough to leave it room, as near the caret as
  // possible, else under the code. It is fixed to the window, so the page's layout never changes.
  function place(ta) {
    if (!note) return;
    mark(ta);
    var wrap = ta.closest('.nb-code-wrap'), cell = ta.closest('.nb-cell'), pre = wrap && wrap.querySelector('.nb-code-hl');
    if (!wrap || !cell || !pre) { P.hide(); return; }
    var area = wrap.getBoundingClientRect(), box = cell.getBoundingClientRect(), view = $('main').getBoundingClientRect();
    var caret = caretBox(pre, ta.selectionStart, area);
    // The caret has scrolled out of sight: so has the reason for the note.
    if (caret.bottom < view.top || caret.top > view.bottom) { note.style.visibility = 'hidden'; return; }
    note.style.visibility = '';
    var w = note.offsetWidth, h = note.offsetHeight, gap = 12;
    var lowest = Math.min(view.bottom, window.innerHeight) - h - 6, highest = view.top + 6;
    var left, top = null;
    if (window.innerWidth - box.right - 52 >= w + gap) {
      // Beside the cell, past its state dot.
      left = box.right + 52;
      top = caret.top - 10;
    } else {
      left = box.right - w - 14;
      var rows = lines(pre), step = Math.max(8, (caret.bottom - caret.top) || 20);
      var free = function (t) {
        // The cell's buttons sit in its top right corner.
        if (t < box.top + 36 || t < highest || t > lowest) return false;
        return rows.every(function (r) { return r.bottom < t - 2 || r.top > t + h + 2 || r.right < left - gap; });
      };
      var start = caret.top - 10, reach = Math.ceil((area.height + h) / step) + 2;
      for (var k = 0; k <= reach && top == null; k++) {
        if (free(start + k * step)) top = start + k * step;
        else if (free(start - k * step)) top = start - k * step;
      }
      if (top == null) {
        // No room beside the code: under it, or failing that above the line being typed.
        top = area.bottom + 8;
        if (top > lowest) top = caret.top - h - 10;
      }
    }
    note.style.left = Math.max(6, Math.min(left, window.innerWidth - w - 6)) + 'px';
    note.style.top = Math.max(highest, Math.min(top, Math.max(highest, lowest))) + 'px';
  }

  function refresh() {
    var ta = editor(document.activeElement);
    if (ta) P.moved(ta); else P.hide();
  }

  P.setEnabled = function (on) {
    P.enabled = !!on;
    try { localStorage.setItem('nb.peek', on ? 'on' : 'off'); } catch (e) { /* remembered only if it can be */ }
    dismissed = null;
    if (on) refresh(); else P.hide();
  };

  ['keyup', 'mouseup', 'input', 'focusin'].forEach(function (type) {
    document.addEventListener(type, function (e) {
      if (type === 'keyup' && e.key === 'Escape') return;
      var ta = editor(e.target);
      if (ta) P.moved(ta);
    });
  });
  document.addEventListener('keydown', function (e) {
    if (e.key !== 'Escape' || !note) return;
    var ta = editor(document.activeElement);
    dismissed = shown && ta ? { id: shown.id, cell: shown.cell } : null;
    P.hide();
  });
  // Leaving the cell takes the note away. A render rebuilds the textarea and gives it the focus back
  // at once, so wait a moment before deciding the reader has left.
  document.addEventListener('focusout', function (e) {
    if (editor(e.target)) setTimeout(function () { if (!editor(document.activeElement)) { dismissed = null; P.hide(); } }, 60);
  });
  window.addEventListener('resize', function () { var ta = editor(document.activeElement); if (note && ta) place(ta); });

  P.start = function () {
    var main = $('main'), cells = $('cells'), toggle = $('peek');
    if (toggle) {
      toggle.checked = P.enabled;
      toggle.onchange = function () { P.setEnabled(toggle.checked); };
    }
    if (main) main.addEventListener('scroll', function () { var ta = editor(document.activeElement); if (note && ta) place(ta); }, { passive: true });
    // Cells are rebuilt after every analysis and run: the note stays, and is checked against the new page.
    if (cells && window.MutationObserver) new MutationObserver(function () { refresh(); }).observe(cells, { childList: true });
  };
  P.start();

  return P;
})();
