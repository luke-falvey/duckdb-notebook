// The notebook editor served by notebook_start. The .sql file is the source of truth: the page
// edits cells, rebuilds the file text from them, and asks the server to parse, run and save it.
var App = (function () {
  'use strict';
  var params = new URLSearchParams(location.search);
  var A = {
    token: params.get('token') || '', source: params.get('nb') || '', preamble: '', cells: [],
    dirty: false, autorun: true, reading: false, graph: false, selected: {}, running: false, lastText: '',
    // Rows per page for tables whose cell has no rows= option; remembered in this browser.
    pageSize: +(localStorage.getItem('nb.pageSize') || 25)
  };
  var $ = function (id) { return document.getElementById(id); };

  A.api = function (method, path, body) {
    var url = path + (path.indexOf('?') >= 0 ? '&' : '?') + 'nb=' + encodeURIComponent(A.source);
    return fetch(url, {
      method: method, headers: { 'X-Notebook-Token': A.token, 'Content-Type': 'application/json' },
      body: body ? JSON.stringify(Object.assign({ nb: A.source }, body)) : undefined
    }).then(function (r) {
      return r.json().then(function (j) { if (!r.ok || j.error) throw new Error(j.error || r.statusText); return j; });
    });
  };

  A.status = function (text) { $('status').textContent = text || ''; };

  // ---- Text model ---------------------------------------------------------------------------
  A.text = function () {
    return A.preamble + A.cells.map(function (c) {
      return '-- %%' + (c.head ? ' ' + c.head : '') + '\n' + (c.body ? c.body.replace(/\s+$/, '') + '\n' : '');
    }).join('\n');
  };

  // Merges a server analysis into the cell list. Results stay with cells at the same position.
  A.merge = function (analysis) {
    analysis.forEach(function (a, i) {
      var c = A.cells[i] || (A.cells[i] = { state: 'stale' });
      ['idx', 'kind', 'name', 'widget', 'options', 'is_query', 'defines', 'reads', 'depends_on', 'errors', 'warnings', 'content', 'start_line'].forEach(function (k) { c[k] = a[k]; });
      if (c.head === undefined) c.head = a.head;
      if (c.body === undefined) c.body = a.body;
    });
    A.cells.length = analysis.length;
  };

  A.load = function (data) {
    A.preamble = data.preamble || '';
    A.cells = [];
    A.merge(data.cells);
    A.lastText = data.text;
    A.dirty = false;
    $('title').textContent = data.title || 'Notebook';
    $('source').textContent = A.source;
    document.title = (data.title || A.source) + ' · DuckDB Notebook';
    A.render();
  };

  A.analyse = function () {
    var text = A.text();
    return A.api('POST', '/api/notebook', { text: text }).then(function (r) { A.merge(r.cells); A.render(); });
  };
  var analyseTimer = null;
  A.edited = function (cell) {
    A.dirty = true;
    A.markStale(cell);
    clearTimeout(analyseTimer);
    analyseTimer = setTimeout(function () { A.analyse().catch(function (e) { A.status(e.message); }); }, 500);
    A.status('Unsaved changes');
  };

  // ---- Graph helpers ------------------------------------------------------------------------
  A.downstream = function (indexes) {
    var out = {}, frontier = indexes.slice();
    while (frontier.length) {
      var i = frontier.pop();
      A.cells.forEach(function (c, j) { if (!out[j] && (c.depends_on || []).indexOf(i) >= 0) { out[j] = true; frontier.push(j); } });
    }
    return Object.keys(out).map(Number).sort(function (a, b) { return a - b; });
  };
  A.readers = function (variable) {
    return A.cells.map(function (c, i) { return (c.reads || []).indexOf('$' + variable) >= 0 ? i : -1; }).filter(function (i) { return i >= 0; });
  };
  A.isAuto = function (c) {
    var run = c.options && c.options.run;
    if (run) return run === 'auto';
    return c.kind === 'markdown' || c.kind === 'chart' || (c.kind === 'sql' && c.is_query);
  };
  A.markStale = function (cell) {
    var i = A.cells.indexOf(cell);
    [i].concat(A.downstream([i])).forEach(function (j) { if (A.cells[j].state !== 'running') A.cells[j].state = 'stale'; });
  };

  // ---- Running ------------------------------------------------------------------------------
  A.run = function (indexes) {
    if (A.running) return Promise.resolve();
    A.running = true;
    $('interrupt').hidden = false;
    (indexes || A.cells.map(function (_, i) { return i; })).forEach(function (i) { if (A.cells[i]) A.cells[i].state = 'running'; });
    A.render();
    A.status('Running…');
    var started = performance.now();
    return A.api('POST', '/api/run', { text: A.text(), cells: indexes || null, page_rows: A.pageSize }).then(function (r) {
      A.merge(r.cells);
      r.results.forEach(function (res) {
        var c = A.cells[res.idx];
        if (!c) return;
        c.result = res;
        c.offset = 0;
        if (res.choices) c.choices = res.choices;
        c.state = res.status === 'error' ? 'failed' : 'fresh';
      });
      A.cells.forEach(function (c) { if (c.state === 'running') c.state = 'stale'; });
      A.status('Ran ' + r.results.length + ' cell' + (r.results.length === 1 ? '' : 's') + ' in ' + Math.round(performance.now() - started) + ' ms');
      if (window.AppExtra) AppExtra.catalog();
    }).catch(function (e) {
      A.cells.forEach(function (c) { if (c.state === 'running') c.state = 'failed'; });
      A.status(e.message);
    }).then(function () {
      A.running = false;
      $('interrupt').hidden = true;
      A.render();
    });
  };

  A.runCell = function (cell) {
    var i = A.cells.indexOf(cell), list = [i];
    if (A.autorun) A.downstream([i]).forEach(function (j) { if (A.isAuto(A.cells[j])) list.push(j); });
    return A.run(list);
  };

  A.setVariable = function (name, value, cell) {
    return A.api('POST', '/api/variable', { name: name, value: value, cell: cell.idx }).then(function () {
      var readers = A.readers(name), list = readers.concat(A.downstream(readers));
      list.forEach(function (j) { A.cells[j].state = 'stale'; });
      var auto = list.filter(function (j) { return A.isAuto(A.cells[j]); });
      auto = auto.filter(function (j, k) { return auto.indexOf(j) === k; }).sort(function (a, b) { return a - b; });
      if (A.autorun && auto.length) return A.run(auto);
      A.render();
    }).catch(function (e) { A.status(e.message); });
  };

  A.save = function () {
    return A.api('POST', '/api/notebook', { text: A.text(), save: true }).then(function (r) {
      A.merge(r.cells);
      A.lastText = A.text();
      A.dirty = false;
      A.status(r.note || 'Saved');
      A.render();
    }).catch(function (e) { A.status('Save failed: ' + e.message); });
  };

  // Sets (or with null, removes) a key=value option on a cell's marker line, keeping the rest of the
  // line as written, so a click in the UI is an ordinary edit of the file.
  A.setOption = function (cell, key, value) {
    var tokens = (cell.head || '').match(/(?:[^\s"]+|"[^"]*")+/g) || [];
    tokens = tokens.filter(function (t) { return t.split('=')[0] !== key; });
    if (value != null && value !== '') tokens.push(key + '=' + (/\s/.test(value) ? '"' + value + '"' : value));
    cell.head = tokens.join(' ');
    cell.options = Object.assign({}, cell.options);
    if (value == null || value === '') delete cell.options[key]; else cell.options[key] = String(value);
    A.dirty = true;
    clearTimeout(analyseTimer);
    analyseTimer = setTimeout(function () { A.analyse().catch(function (e) { A.status(e.message); }); }, 300);
    A.status('Unsaved changes');
  };

  // hide= as two switches.
  A.hidden = function (cell) {
    var h = (cell.options && cell.options.hide) || '';
    return { code: h === 'code' || h === 'all', output: h === 'output' || h === 'all' };
  };
  A.setHidden = function (cell, code, output) {
    A.setOption(cell, 'hide', code && output ? 'all' : code ? 'code' : output ? 'output' : null);
    A.render();
  };

  // ---- Rendering ----------------------------------------------------------------------------
  function badge(state) {
    state = state || 'stale';
    return NB.el('span', { 'class': 'nb-badge ' + state, title: state }, [NB.el('i'), state]);
  }

  function addBar(at) {
    var add = function (head, body) {
      return function () {
        A.cells.splice(at, 0, { head: head, body: body, kind: 'sql', state: 'stale', options: {} });
        A.edited(A.cells[at]);
        A.render();
        var ta = document.querySelectorAll('.nb-cell textarea')[at];
        if (ta) ta.focus();
      };
    };
    return NB.el('div', { 'class': 'nb-add' }, [
      NB.el('button', { text: '+ SQL', onclick: add('', '') }),
      NB.el('button', { text: '+ Markdown', onclick: add('[markdown]', '/*\n\n*/') }),
      NB.el('button', { text: '+ Input', onclick: add('input value number', 'SET VARIABLE value = 0;') }),
      NB.el('button', { text: '+ Chart', onclick: add('chart from=', '/*\nmark: bar\nx:\ny:\n*/') })
    ]);
  }

  function codeArea(cell, rows) {
    var ta = NB.el('textarea', { 'class': 'nb-code', spellcheck: 'false', rows: rows || Math.min(24, Math.max(2, (cell.body || '').split('\n').length + 1)) });
    ta.value = cell.body || '';
    ta.addEventListener('input', function () { cell.body = ta.value; A.edited(cell); });
    ta.addEventListener('keydown', function (e) {
      if ((e.shiftKey || e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); A.runCell(cell); }
      else if (e.key === 'Tab' && !e.shiftKey) {
        e.preventDefault();
        var s0 = ta.selectionStart;
        ta.setRangeText('    ', s0, ta.selectionEnd, 'end');
        cell.body = ta.value; A.edited(cell);
      } else if (e.ctrlKey && e.key === ' ' && window.AppExtra) { e.preventDefault(); AppExtra.complete(ta, cell); }
    });
    return ta;
  }

  function messages(cell) {
    var box = NB.el('div', { 'class': 'nb-meta' });
    (cell.errors || []).forEach(function (m) { box.appendChild(NB.el('div', { 'class': 'nb-error', text: m })); });
    (cell.warnings || []).forEach(function (m) { box.appendChild(NB.el('div', { 'class': 'nb-warning', text: '⚠ ' + m })); });
    var r = cell.result;
    if (r && r.status !== 'skipped') {
      var parts = [];
      if (r.row_count != null) parts.push(NB.formatNumber(r.row_count, ',') + ' rows');
      if (r.elapsed_ms != null) parts.push(Math.round(r.elapsed_ms) + ' ms');
      if (cell.defines && cell.defines.length) parts.push('defines ' + cell.defines.join(', '));
      if (parts.length) box.appendChild(NB.el('div', { text: parts.join(' · ') }));
    }
    return box;
  }

  function output(cell) {
    var out = NB.el('div', { 'class': 'nb-output' }), r = cell.result || {};
    if (r.status === 'error') { out.appendChild(NB.el('div', { 'class': 'nb-error', text: r.error })); return out; }
    if (cell.kind === 'markdown') {
      // Before a run, {{ expressions }} show as code rather than as half-rendered markdown.
      var text = r.output != null ? r.output : (cell.content || '').replace(/\{\{([\s\S]*?)\}\}/g, function (_, e) { return '`' + e.trim() + '`'; });
      out.appendChild(NB.el('div', { 'class': 'nb-md', html: NB.markdown(text) }));
    } else if (cell.kind === 'chart' && r.data) {
      NB.chart(out, r.data, { selected: A.selected, onSelect: function (name, value) { A.selected[name] = value; A.setVariable(name, value, cell); } });
    } else if (cell.kind === 'sql' && r.data) {
      var size = +((cell.options && cell.options.rows) || A.pageSize);
      var fetchPage = function (offset, limit) {
        return A.api('GET', '/api/page?cell=' + cell.idx + '&offset=' + offset + '&limit=' + limit).then(function (data) {
          cell.offset = offset; data.total = r.data.total; r.data = data; A.render();
        }).catch(function (e) { A.status(e.message); });
      };
      NB.table(out, r.data, {
        offset: cell.offset || 0, pageSize: size,
        onPage: fetchPage,
        // The page size is part of the notebook: it is written to the cell as rows=N.
        onPageSize: function (next) { A.setOption(cell, 'rows', next); fetchPage(0, next); }
      });
      if (window.AppExtra) out.appendChild(AppExtra.switcher(cell, r.data.columns));
    }
    return out;
  }

  function inputCell(cell) {
    var value = cell.result && cell.result.value !== undefined ? cell.result.value : cell.value;
    var view = Object.assign({}, cell, { value: value, choices: cell.choices });
    var box = NB.el('div');
    var widget = A.inputWidget(view, function (c, v) { cell.value = v; if (cell.result) cell.result.value = v; A.setVariable(cell.name, v, cell); });
    box.appendChild(widget);
    if ((cell.widget === 'select' || cell.widget === 'multiselect') && !cell.choices && cell.name && !cell.fetching) {
      // Choices from a cell's column exist once that cell has run; /api/run refreshes them.
      cell.fetching = true;
      A.api('GET', '/api/options?cell=' + cell.idx).then(function (r) { if (r.values.length) { cell.choices = r.values; A.render(); } }).catch(function () {});
    }
    return box;
  }

  function headButtons(cell, i) {
    var hidden = A.hidden(cell), kids = [];
    kids.push(NB.el('button', { 'class': 'nb-run', text: '▶', title: 'Run (Shift+Enter)', onclick: function () { A.runCell(cell); } }));
    if (cell.kind !== 'notebook') {
      kids.push(NB.el('button', { 'class': 'nb-toggle nb-hide-code' + (hidden.code ? ' on' : ''), text: '</>', title: hidden.code ? 'Code is hidden (hide=code): show it' : 'Hide code in reading view and exports',
        onclick: function () { A.setHidden(cell, !hidden.code, hidden.output); } }));
      kids.push(NB.el('button', { 'class': 'nb-toggle nb-hide-output' + (hidden.output ? ' on' : ''), text: '◫', title: hidden.output ? 'Output is hidden (hide=output): show it' : 'Hide output in reading view and exports',
        onclick: function () { A.setHidden(cell, hidden.code, !hidden.output); } }));
    }
    kids.push(NB.el('button', { text: '↑', title: 'Move up', disabled: i === 0 ? 'disabled' : null, onclick: function () { A.cells.splice(i - 1, 0, A.cells.splice(i, 1)[0]); A.edited(cell); A.render(); } }));
    kids.push(NB.el('button', { text: '↓', title: 'Move down', disabled: i === A.cells.length - 1 ? 'disabled' : null, onclick: function () { A.cells.splice(i + 1, 0, A.cells.splice(i, 1)[0]); A.edited(cell); A.render(); } }));
    kids.push(NB.el('button', { text: '✕', title: 'Delete cell', onclick: function () { if (confirm('Delete this cell?')) { A.cells.splice(i, 1); A.dirty = true; A.analyse(); A.render(); } } }));
    return kids;
  }

  // A hidden part of a cell collapses to a line that shows it while editing, without changing hide=.
  function stub(text, onclick) {
    return NB.el('div', { 'class': 'nb-stub nb-hide-reading', text: text, onclick: onclick });
  }

  A.render = function () {
    var root = $('cells');
    var focused = document.activeElement, focusIndex = -1, caret = 0;
    if (focused && focused.tagName === 'TEXTAREA' && focused.closest('.nb-cell')) {
      focusIndex = Array.prototype.indexOf.call(root.querySelectorAll('textarea'), focused);
      caret = focused.selectionStart;
    }
    root.innerHTML = '';
    document.getElementById('app').classList.toggle('reading', A.reading);
    A.cells.forEach(function (cell, i) {
      if (!A.reading) root.appendChild(addBar(i));
      var hidden = A.hidden(cell);
      var hideCode = hidden.code && !(cell.peekCode && !A.reading);
      var hideOutput = hidden.output && !(cell.peekOutput && !A.reading);
      // Reading view shows results, not code; markdown and inputs have no code to show there.
      if (A.reading) hideCode = true;
      var box = NB.el('div', { 'class': 'nb-cell' + (hideCode ? ' hide-code' : '') + (hideOutput ? ' hide-output' : ''), 'data-kind': cell.kind, 'data-idx': i, 'data-seed': 'cell' + i + (cell.name || '') });
      var span = cell.options && cell.options.span;
      if (span && A.reading) box.style.gridColumn = 'span ' + Math.max(1, Math.min(12, +span));
      var head = NB.el('input', { 'class': 'nb-head', value: cell.head || '', placeholder: 'name and options', title: 'The -- %% marker line' });
      head.addEventListener('change', function () { cell.head = head.value.trim(); A.edited(cell); A.render(); });
      box.appendChild(NB.el('div', { 'class': 'nb-cell-head' }, [
        NB.el('span', { 'class': 'nb-kind', text: cell.kind === 'sql' ? (cell.is_query ? 'query' : 'sql') : cell.kind }),
        head, badge(cell.state)
      ].concat(headButtons(cell, i))));
      var body = NB.el('div', { 'class': 'nb-cell-body' });
      if (cell.kind === 'input') body.appendChild(inputCell(cell));
      // Prose, charts, inputs and metadata show their result; their source opens on demand.
      var folded = cell.kind !== 'sql' && !cell.editing && !(cell.errors && cell.errors.length);
      var codeWrap = NB.el('div', { 'class': 'nb-code-area' }, [codeArea(cell)]);
      if (hidden.code && !A.reading) {
        body.appendChild(stub(cell.peekCode ? '▾ code (hidden when reading)' : '▸ code hidden', function () { cell.peekCode = !cell.peekCode; A.render(); }));
      } else if (cell.kind !== 'sql' && !A.reading) {
        var what = cell.kind === 'markdown' ? 'text' : cell.kind === 'chart' ? 'chart spec' : cell.kind === 'input' ? 'default' : 'settings';
        body.appendChild(stub((folded ? '▸ edit ' : '▾ ') + what, function () { cell.editing = !cell.editing; A.render(); }));
      }
      if (folded && !hidden.code) codeWrap.style.display = 'none';
      body.appendChild(codeWrap);
      body.appendChild(messages(cell));
      if (hidden.output && !A.reading) {
        body.appendChild(stub(cell.peekOutput ? '▾ output (hidden when reading)' : '▸ output hidden', function () { cell.peekOutput = !cell.peekOutput; A.render(); }));
      }
      body.appendChild(output(cell));
      box.appendChild(body);
      root.appendChild(box);
      if (cell.kind === 'markdown') box.addEventListener('dblclick', function (e) { if (!cell.editing && !A.reading && e.target.closest('.nb-output')) { cell.editing = true; A.render(); } });
      box.addEventListener('focusin', function () { box.classList.add('focused'); });
      box.addEventListener('focusout', function () { box.classList.remove('focused'); });
      Sketch.outline(box, { width: 1.5 });
    });
    if (!A.reading) root.appendChild(addBar(A.cells.length));
    if (focusIndex >= 0) {
      var t = root.querySelectorAll('textarea')[focusIndex];
      if (t) { t.focus(); t.selectionStart = t.selectionEnd = caret; }
    }
    if (window.AppExtra) AppExtra.graph();
  };

  // Input widgets, shared with the exported viewer's look.
  A.inputWidget = function (cell, onChange) {
    var o = cell.options || {}, v = cell.value, w = cell.widget, box = NB.el('div', { 'class': 'nb-input' }), control;
    box.appendChild(NB.el('label', { text: o.label || cell.name }));
    var fire = function (value) { onChange(cell, value); };
    if (w === 'select' || w === 'multiselect') {
      var choices = cell.choices || (o.options ? o.options.split(',') : []);
      control = NB.el('select', { multiple: w === 'multiselect' ? 'multiple' : null, onchange: function () {
        var picked = Array.prototype.filter.call(control.options, function (x) { return x.selected; }).map(function (x) { return choices[+x.value]; });
        fire(w === 'multiselect' ? picked : picked[0]);
      } });
      choices.forEach(function (x, i) {
        var on = w === 'multiselect' ? (v || []).some(function (y) { return NB.key(y) === NB.key(x); }) : NB.key(x) === NB.key(v);
        control.appendChild(NB.el('option', { value: i, text: NB.formatValue(x), selected: on ? 'selected' : null }));
      });
    } else if (w === 'checkbox') {
      control = NB.el('input', { type: 'checkbox', checked: v ? 'checked' : null, onchange: function () { fire(control.checked); } });
    } else if (w === 'slider' || w === 'number') {
      var shown = NB.el('span', { 'class': 'nb-input-value', text: v == null ? '' : String(v) });
      var input = NB.el('input', { type: w === 'slider' ? 'range' : 'number', min: o.min, max: o.max, step: o.step || 'any', value: v == null ? '' : v,
        oninput: function () { shown.textContent = input.value; }, onchange: function () { fire(Number(input.value)); } });
      box.appendChild(input);
      box.appendChild(shown);
    } else if (w === 'date') {
      control = NB.el('input', { type: 'date', value: v ? String(v).slice(0, 10) : '', min: o.min, max: o.max, onchange: function () { fire(control.value); } });
    } else if (w === 'daterange') {
      var a = NB.el('input', { type: 'date', value: v && v.lo ? String(v.lo).slice(0, 10) : '' }), b = NB.el('input', { type: 'date', value: v && v.hi ? String(v.hi).slice(0, 10) : '' });
      a.onchange = b.onchange = function () { fire({ lo: a.value, hi: b.value }); };
      box.appendChild(a); box.appendChild(NB.el('span', { text: '–' })); box.appendChild(b);
    } else {
      control = NB.el('input', { type: 'text', value: v == null ? '' : v, placeholder: o.placeholder, onchange: function () { fire(control.value); } });
    }
    if (control) box.appendChild(control);
    return box;
  };

  // ---- File sync ----------------------------------------------------------------------------
  A.watch = function () {
    if (!window.EventSource) return;
    var es = new EventSource('/api/events?nb=' + encodeURIComponent(A.source) + '&token=' + encodeURIComponent(A.token));
    es.addEventListener('changed', function (e) {
      var data = JSON.parse(e.data);
      if (data.text === A.text() || data.text === A.lastText) return;
      var apply = function () { $('banner').innerHTML = ''; A.api('POST', '/api/notebook', { text: data.text, reload: true }).then(function (r) { A.load(r); }); };
      if (!A.dirty) { apply(); A.status('Reloaded: the file changed on disk'); return; }
      $('banner').innerHTML = '';
      $('banner').appendChild(NB.el('div', { 'class': 'nb-banner' }, [
        NB.el('span', { text: 'The file changed on disk, and you have unsaved edits.' }),
        NB.el('button', { text: 'Load the file', onclick: apply }),
        NB.el('button', { text: 'Keep mine', onclick: function () { A.lastText = data.text; $('banner').innerHTML = ''; } })
      ]));
    });
  };

  // ---- Start --------------------------------------------------------------------------------
  A.exportUrl = function () {
    var f = $('export-format').value, code = $('export-code').checked, out = $('export-output').checked;
    var hide = !code && !out ? 'all' : !code ? 'code' : !out ? 'output' : 'none';
    var format = f === 'html-precomputed' ? 'html&interactive=precomputed' : f;
    return '/api/export?nb=' + encodeURIComponent(A.source) + '&token=' + encodeURIComponent(A.token) + '&format=' + format + '&hide=' + hide;
  };

  A.menu = function (open) {
    var menu = $('menu'), gear = $('gear');
    open = open == null ? !menu.classList.contains('open') : open;
    menu.classList.toggle('open', open);
    gear.classList.toggle('open', open);
    gear.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) Sketch.outline(menu, { fill: '#fdfcf8', seed: 'menu' });
  };

  // ---- Start --------------------------------------------------------------------------------
  A.start = function () {
    $('run-all').onclick = function () { A.run(null); };
    $('interrupt').onclick = function () { A.api('POST', '/api/interrupt', {}); };
    $('gear').onclick = function (e) { e.stopPropagation(); A.menu(); };
    $('menu').onclick = function (e) { e.stopPropagation(); };
    document.addEventListener('click', function () { A.menu(false); });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape') A.menu(false); });
    $('autorun').onchange = function (e) { A.autorun = e.target.checked; };
    $('save').onclick = A.save;
    $('view-reading').onclick = function () { A.reading = !A.reading; $('view-reading').classList.toggle('primary', A.reading); A.render(); };
    $('view-graph').onclick = function () { A.graph = !A.graph; $('view-graph').classList.toggle('primary', A.graph); A.render(); };
    $('view-sidebar').onclick = function () { $('app').classList.toggle('with-sidebar'); $('view-sidebar').classList.toggle('primary'); };
    var theme = localStorage.getItem('nb.theme');
    if (theme) document.documentElement.setAttribute('data-theme', theme);
    $('theme').onclick = function () {
      var next = document.documentElement.getAttribute('data-theme') === 'dark' ? 'light' : 'dark';
      document.documentElement.setAttribute('data-theme', next);
      localStorage.setItem('nb.theme', next);
    };
    NB.pageSizes.forEach(function (n) { $('page-size').appendChild(NB.el('option', { value: n, text: n, selected: n === A.pageSize ? 'selected' : null })); });
    $('page-size').onchange = function (e) {
      A.pageSize = +e.target.value;
      localStorage.setItem('nb.pageSize', A.pageSize);
      A.status('Tables without rows= now show ' + A.pageSize + ' rows; run again to refresh them');
    };
    $('export').onclick = function () {
      var f = $('export-format').value;
      A.menu(false);
      if (f === 'pdf') {
        // Print the reading view, with the same code/output choices as the other formats.
        var code = $('export-code').checked, out = $('export-output').checked, wasReading = A.reading;
        A.reading = !code; A.render();
        document.body.classList.toggle('print-no-output', !out);
        setTimeout(function () { window.print(); A.reading = wasReading; A.render(); }, 150);
        return;
      }
      window.open(A.exportUrl(), '_blank');
    };
    document.addEventListener('keydown', function (e) {
      if ((e.ctrlKey || e.metaKey) && e.key === 's') { e.preventDefault(); A.save(); }
      if ((e.ctrlKey || e.metaKey) && e.shiftKey && e.key === 'Enter') { e.preventDefault(); A.run(null); }
    });
    window.addEventListener('beforeunload', function (e) { if (A.dirty) { e.preventDefault(); e.returnValue = ''; } });
    document.fonts.ready.then(function () { Sketch.outline($('toolbar-rule'), { kind: 'underline', stroke: '#ced4da', width: 1.3, seed: 'rule' }); });
    A.api('GET', '/api/notebook').then(function (data) {
      A.load(data);
      A.watch();
      if (window.AppExtra) { AppExtra.catalog(); AppExtra.picker(); }
      A.status('Opened. Nothing has run yet: press Run all.');
    }).catch(function (e) { A.status(e.message); $('cells').appendChild(NB.el('div', { 'class': 'nb-error', text: e.message })); });
  };

  return A;
})();
