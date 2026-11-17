// The reader for an exported notebook (notebook_export, format html or html-wasm). It needs no
// server and no network: the cells, results and chart data are in the page, gzip-compressed.
//  - Static: what the run produced, drawn once.
//  - interactive := 'precomputed': inputs switch between results computed for each value.
//  - html-wasm: DuckDB-WASM runs the notebook's queries in the page, so inputs and selections
//    re-run real SQL.
(function () {
  'use strict';
  var root = document.getElementById('nb-root');
  var payload, current, wasm = null, variables = {}, selected = {};

  function bytes(id) {
    return b64bytes(document.getElementById(id).textContent.trim());
  }
  function gunzip(u8) {
    return new Response(new Blob([u8]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer().then(function (b) { return new Uint8Array(b); });
  }
  function b64bytes(text) {
    var bin = atob(text), out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  // A SQL literal for a value set by a widget or a chart selection.
  function literal(v, type) {
    if (v === null || v === undefined) return 'NULL';
    if (Array.isArray(v)) return '[' + v.map(function (x) { return literal(x, type); }).join(', ') + ']';
    if (typeof v === 'object') return '{' + Object.keys(v).map(function (k) { return "'" + k.replace(/'/g, "''") + "': " + literal(v[k], type); }).join(', ') + '}';
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    if (typeof v === 'number') return String(v);
    var s = "'" + String(v).replace(/'/g, "''") + "'";
    return type ? s + '::' + type : s;
  }

  function option(cell, key, fallback) { return cell.options && cell.options[key] != null ? cell.options[key] : fallback; }

  // A query cell's charts as tabs, with its table last. The first chart shows until another is picked.
  function queryTabs(cell, out) {
    var charts = cell.charts, tab = Math.min(cell.tab || 0, charts.length);
    var items = NB.queryTabItems(charts, charts.length);
    out.appendChild(NB.tabs(items, tab, function (k) { cell.tab = k; render(); }));
    var holder = NB.el('div', { 'class': 'nb-tab-body' });
    out.appendChild(holder);
    if (tab < charts.length) {
      NB.chart(holder, charts[tab], { selected: selected, onSelect: wasm ? onSelect : null, seed: 'cell' + cell.idx + '/' + tab, inTab: true });
    } else if (cell.table) {
      NB.table(holder, cell.table, { pageSize: +option(cell, 'rows', 25) });
    } else if (cell.error) {
      holder.appendChild(NB.el('div', { 'class': 'nb-error', text: cell.error }));
    }
  }

  function widgetType(cell) {
    return { date: 'DATE', daterange: 'DATE', number: 'DOUBLE', slider: 'DOUBLE' }[cell.widget] || '';
  }

  // What a cell shows: its code (never while presenting, as in the editor's reading view) and its
  // output, when it has any. The export's hide= applies to code and results; prose and inputs are what
  // a reader came for.
  function parts(cell) {
    var own = option(cell, 'hide', ''), global = payload.hide || '';
    var code = !Present.on && cell.kind === 'sql' && !!cell.sql && !NB.hides(own, 'code') && !NB.hides(global, 'code');
    var output = cell.kind !== 'notebook' && !NB.hides(own, 'output') && (cell.kind === 'markdown' || cell.kind === 'input' || !NB.hides(global, 'output'));
    if (output && cell.kind === 'sql') output = !!((cell.charts && cell.charts.length) || cell.error || cell.table);
    return { code: code, output: output };
  }

  function render() {
    root.innerHTML = '';
    var head = NB.el('div', { 'class': 'nb-viewer-header' }, [
      NB.el('span', { text: payload.source || '' }),
      NB.el('span', {}, [
        NB.el('span', { id: 'nb-mode', text: wasm ? 'Live: queries run in this page with DuckDB-WASM' : payload.precomputed ? 'Inputs switch between precomputed results' : 'Static export' }),
        NB.el('button', { 'class': 'nb-tab nb-present-start', text: 'present', title: 'Present: one screen at a time. Arrows, Space or a click move; Esc leaves; F for full screen',
          onclick: function () { Present.enter(0); } })
      ])
    ]);
    // Watch the page being written again, slowly, by one pen (render-ink.js, watch mode).
    if (window.Ink) {
      head.lastChild.appendChild(NB.el('button', { 'class': 'ink-watch-start', type: 'button', text: '✎ watch', title: 'Watch this page being written by hand (or open it with ?ink=watch)', onclick: function () { Ink.watch(); } }));
    }
    root.appendChild(head);
    if (payload.frozen && payload.frozen.length) root.appendChild(NB.el('div', { 'class': 'nb-frozen', text: 'Fixed at their current value in this export: ' + payload.frozen.join(', ') }));
    var grid = NB.el('div', { 'class': 'nb-cells' });
    var pre = {};
    if (payload.precomputed && !wasm) payload.precomputed.inputs.forEach(function (i) { pre[i.name] = i.values; });
    var shows = current.map(parts), screen = null;
    if (Present.on) {
      screen = Present.begin(current.map(function (cell, i) {
        return { id: cell.idx, span: option(cell, 'span'), kind: cell.kind, screen: option(cell, 'screen'), visible: shows[i].code || shows[i].output };
      }), grid, root);
    }
    current.forEach(function (cell, i) {
      var showCode = shows[i].code, showOutput = shows[i].output;
      if ((!showCode && !showOutput) || (screen && !screen[i])) return;
      var box = NB.el('div', { 'class': 'nb-cell', 'data-kind': cell.kind, 'data-idx': cell.idx, 'data-seed': 'cell' + cell.idx + (cell.name || ''), style: 'grid-column: span ' + NB.span(option(cell, 'span', 12)) });
      var out = NB.el('div', { 'class': 'nb-output' }), shown = 0;
      if (showCode) {
        box.appendChild(NB.el('div', { 'class': 'nb-code-label', text: NB.kindName(cell) + (cell.name ? ' · ' + cell.name : '') }));
        box.appendChild(NB.el('pre', { 'class': 'nb-source', html: NB.highlightSql(cell.sql) }));
        shown++;
      }
      if (showOutput) {
        if (cell.kind === 'markdown') {
          if (cell.error) out.appendChild(NB.el('div', { 'class': 'nb-error', text: cell.error }));
          else out.appendChild(NB.el('div', { 'class': 'nb-md', html: NB.markdown(cell.output != null ? cell.output : cell.content) }));
          shown++;
        } else if (cell.kind === 'sql' && cell.charts && cell.charts.length) {
          queryTabs(cell, out);
          shown++;
        } else if (cell.kind === 'sql') {
          if (cell.error) out.appendChild(NB.el('div', { 'class': 'nb-error', text: cell.error }));
          else NB.table(out, cell.table, { pageSize: +option(cell, 'rows', 25) });
          shown++;
        } else if (cell.kind === 'input') {
          var enabled = !!wasm || !!pre[cell.name];
          box.appendChild(NB.inputWidget(cell, onInput, { exported: true, enabled: enabled, values: wasm ? null : pre[cell.name] }));
          shown++;
        } else if (cell.kind === 'chart') {
          if (cell.error && !cell.chart) out.appendChild(NB.el('div', { 'class': 'nb-error', text: cell.error }));
          else NB.chart(out, cell.chart, { selected: selected, onSelect: wasm ? onSelect : null, seed: 'chart' + cell.idx });
          shown++;
        }
      }
      if (!shown) return;
      box.appendChild(out);
      grid.appendChild(box);
    });
    root.appendChild(grid);
    // Outlines are drawn once the cells are laid out, and follow their size from then on.
    Array.prototype.forEach.call(grid.children, function (box) { Sketch.outline(box, { width: 1.5 }); });
    if (window.Ink) { Ink.onrewrite = render; write(head, grid); }
    if (Present.on) Present.finish(root, grid);
    window.__NB_READY = true;
  }

  // Hands each piece of the page to the pen (render-ink.js). A piece already written, with the same
  // content, is left as it is; one that changed is written again.
  function write(head, grid) {
    Ink.present(head, 'header');
    if (head.nextSibling && head.nextSibling.classList.contains('nb-frozen')) Ink.present(head.nextSibling, 'frozen');
    Array.prototype.forEach.call(grid.children, function (box) {
      var id = Present.prefix() + 'cell' + box.getAttribute('data-idx') + '/';
      Array.prototype.forEach.call(box.children, function (part, k) {
        if (part.classList.contains('sk-outline')) Ink.present(part, id + 'outline', box.getAttribute('data-seed'), Ink.bracket(box));
        else Ink.present(part, id + k + (part.getAttribute('class') || part.tagName));
      });
    });
  }

  // ---- precomputed ----
  function onInput(cell, value) {
    if (wasm) {
      cell.value = value;
      wasm.query('SET VARIABLE "' + cell.name.replace(/"/g, '""') + '" = ' + literal(value, widgetType(cell))).then(refresh);
      return;
    }
    var p = payload.precomputed;
    if (!p) return;
    variables[cell.name] = value;
    var key = p.inputs.map(function (i) {
      var v = variables[i.name] !== undefined ? variables[i.name] : (payload.cells[0] && current.find(function (c) { return c.name === i.name && c.kind === 'input'; }) || {}).value;
      var idx = i.values.findIndex(function (x) { return NB.key(x) === NB.key(v); });
      return Math.max(0, idx);
    }).join(',');
    if (p.states[key]) { current = p.states[key]; render(); }
  }

  // ---- html-wasm ----
  function onSelect(name, value) {
    var type = '';
    current.forEach(function (c) {
      (c.chart ? [c.chart] : []).concat(c.charts || []).forEach(function (chart) {
        var r = chart && chart.render;
        if (r && r.selection && r.selection.as === name) {
          (r.layers || []).forEach(function (l) {
            var e = l.encoding[(r.selection.channels || ['x'])[0]];
            if (e && e.type === 'temporal') type = 'TIMESTAMP';
          });
        }
      });
    });
    selected[name] = value;
    wasm.query('SET VARIABLE "' + name.replace(/"/g, '""') + '" = ' + literal(value, type)).then(refresh);
  }

  function refresh() {
    var jobs = current.map(function (cell) {
      if (cell.kind === 'sql' && cell.display_sql) {
        var table = wasm.query('SELECT * FROM (' + cell.display_sql + ') LIMIT ' + Math.max(100, +option(cell, 'rows', 0))).then(function (t) { cell.table = t; cell.error = null; }, function (e) { cell.error = String(e.message || e); });
        return Promise.all([table].concat((cell.charts || []).map(redrawChart)));
      }
      if (cell.kind === 'markdown' && cell.expressions && cell.expressions.length) {
        return Promise.all(cell.expressions.map(function (x) { return wasm.query('SELECT (' + x.sql + ')'); })).then(function (results) {
          var text = '', at = 0;
          cell.expressions.forEach(function (x, i) {
            var v = results[i].rows[0] ? results[i].rows[0][0] : null;
            text += cell.content.slice(at, x.begin) + '\uE000' + (v === null ? 'NULL' : typeof v === 'object' ? JSON.stringify(v) : String(v)) + '\uE001';
            at = x.end;
          });
          cell.output = text + cell.content.slice(at);
          cell.error = null;
        }, function (e) { cell.error = String(e.message || e); });
      }
      if (cell.kind === 'chart' && cell.chart) return redrawChart(cell.chart);
      return null;
    });
    return Promise.all(jobs).then(render);
  }

  function redrawChart(chart) {
    if (!chart || !chart.queries) return null;
    return Promise.all(chart.queries.map(function (q) { return wasm.query(q); })).then(function (sets) { chart.datasets = sets; chart.error = null; }, function (e) { chart.error = String(e.message || e); });
  }

  function bootWasm() {
    var status = document.getElementById('nb-mode');
    if (status) status.textContent = 'Starting DuckDB-WASM…';
    return gunzip(bytes('nb-wasm-module')).then(function (module) {
      var worker = new TextDecoder().decode(bytes('nb-wasm-worker'));
      return window.NotebookWasm.boot(module, worker);
    }).then(function (engine) {
      wasm = engine;
      return gunzip(b64bytes(payload.wasm.db));
    }).then(function (dbfile) {
      return wasm.registerFile('notebook.duckdb', dbfile);
    }).then(function () {
      var setup = ["ATTACH 'notebook.duckdb' AS nbdata (READ_ONLY)", 'USE nbdata'].concat(payload.wasm.setup || []);
      return setup.reduce(function (p, sql) { return p.then(function () { return wasm.query(sql); }); }, Promise.resolve());
    }).then(refresh).then(function () { window.__NB_WASM = true; }).catch(function (e) {
      wasm = null;
      render();
      root.insertBefore(NB.el('div', { 'class': 'nb-error', text: 'DuckDB-WASM could not start; showing the exported results. ' + (e.message || e) }), root.firstChild);
    });
  }

  gunzip(bytes('nb-data')).then(function (raw) {
    payload = JSON.parse(new TextDecoder().decode(raw));
    NB.setSqlWords(payload.sql_words);
    window.__NB_PAYLOAD = payload;
    current = payload.cells;
    document.title = payload.title || document.title;
    render();
    Present.start({ render: render });
    if (payload.wasm && window.NotebookWasm) return bootWasm();
  }).catch(function (e) {
    root.appendChild(NB.el('div', { 'class': 'nb-error', text: 'Could not read this notebook: ' + (e.message || e) }));
  });
})();
