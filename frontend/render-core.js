// Shared rendering for the notebook UI and its exports: markdown, number formats, result grids.
// Plain browser JavaScript with no dependencies, so it can be inlined into an exported page.
// render-chart.js and render-typed.js add the charts to the same NB object.
var NB = (function () {
  'use strict';
  var NB = {};

  NB.esc = function (s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  };

  NB.el = function (tag, attrs, children) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      if (k === 'text') e.textContent = attrs[k];
      else if (k === 'html') e.innerHTML = attrs[k];
      else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
      else if (attrs[k] != null && attrs[k] !== false) e.setAttribute(k, attrs[k]);
    }
    (children || []).forEach(function (c) { if (c != null) e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c); });
    return e;
  };

  // Excalidraw's palette: strokes, and the pastel fill that goes with each.
  NB.palette = ['#1971c2', '#e8590c', '#2f9e44', '#e03131', '#9c36b5', '#0c8599', '#f08c00', '#6741d9',
    '#868e96', '#c2255c', '#5c940d', '#1864ab'];
  NB.pastels = ['#a5d8ff', '#ffd8a8', '#b2f2bb', '#ffc9c9', '#eebefa', '#99e9f2', '#ffec99', '#d0bfff',
    '#dee2e6', '#fcc2d7', '#d8f5a2', '#74c0fc'];
  // The pastel fill for a stroke colour from the palette; other colours fill with themselves.
  NB.pastel = function (stroke) {
    var i = NB.palette.indexOf(stroke);
    return i < 0 ? stroke : NB.pastels[i];
  };
  // Sequential ramps from light to dark.
  NB.schemes = {
    blues: ['#f7fbff', '#08306b'], greens: ['#f7fcf5', '#00441b'], reds: ['#fff5f0', '#67000d'],
    viridis: ['#440154', '#21918c', '#fde725'], magma: ['#000004', '#b73779', '#fcfdbf'],
    greys: ['#ffffff', '#000000'], oranges: ['#fff5eb', '#7f2704'], purples: ['#fcfbfd', '#3f007d']
  };

  function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
  NB.ramp = function (scheme) {
    var stops = (NB.schemes[scheme] || NB.schemes.blues).map(hex);
    return function (t) {
      t = Math.max(0, Math.min(1, isFinite(t) ? t : 0));
      var seg = Math.min(stops.length - 2, Math.floor(t * (stops.length - 1)));
      var u = t * (stops.length - 1) - seg, a = stops[seg], b = stops[seg + 1];
      return 'rgb(' + [0, 1, 2].map(function (i) { return Math.round(a[i] + (b[i] - a[i]) * u); }).join(',') + ')';
    };
  };

  // ---- Values and formats -------------------------------------------------------------------

  NB.toDate = function (v) {
    if (v instanceof Date) return v;
    if (typeof v === 'number') return new Date(v);
    var s = String(v);
    if (/^\d{4}-\d{2}-\d{2}$/.test(s)) return new Date(s + 'T00:00:00');
    s = s.replace(' ', 'T').replace(/(\.\d{3})\d+/, '$1').replace(/([+-]\d{2})$/, '$1:00');
    return new Date(s);
  };

  // A subset of d3-format: [$][,][.precision][f|%|s|d|e], with `~` to trim zeros.
  NB.formatNumber = function (v, spec) {
    if (v == null || v === '') return '';
    var n = Number(v);
    if (!isFinite(n)) return String(v);
    var m = /^(\$)?(,)?(?:\.(\d+))?(~)?([fdse%])?$/.exec(spec || '');
    if (!m) m = [null, null, ',', null, null, null];
    var currency = m[1], comma = m[2], prec = m[3] != null ? +m[3] : null, trim = m[4], type = m[5];
    var out;
    if (type === '%') { out = (n * 100).toFixed(prec == null ? 0 : prec) + '%'; }
    else if (type === 's') {
      var units = ['', 'k', 'M', 'G', 'T'], i = 0, a = Math.abs(n);
      while (a >= 1000 && i < units.length - 1) { a /= 1000; n /= 1000; i++; }
      out = (prec == null ? +n.toPrecision(3) : n.toFixed(prec)) + units[i];
    } else if (type === 'e') { out = n.toExponential(prec == null ? 2 : prec); }
    else if (type === 'd') { out = Math.round(n).toLocaleString('en-US', { useGrouping: !!comma }); }
    else {
      var p = prec != null ? prec : (Number.isInteger(n) ? 0 : Math.min(6, Math.max(0, 3 - Math.floor(Math.log10(Math.abs(n) || 1)))));
      out = n.toLocaleString('en-US', { minimumFractionDigits: trim || !spec ? 0 : p, maximumFractionDigits: p, useGrouping: !!comma || !spec });
    }
    return (currency ? (n < 0 ? '-$' + out.replace('-', '') : '$' + out) : out);
  };

  NB.formatDate = function (v, unit) {
    var d = NB.toDate(v);
    if (isNaN(d)) return String(v);
    var p = function (x) { return (x < 10 ? '0' : '') + x; };
    var months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    if (unit === 'year') return String(d.getFullYear());
    if (unit === 'month') return months[d.getMonth()] + ' ' + d.getFullYear();
    if (unit === 'day') return months[d.getMonth()] + ' ' + d.getDate();
    if (unit === 'time') return p(d.getHours()) + ':' + p(d.getMinutes());
    var s = d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
    if (d.getHours() || d.getMinutes() || d.getSeconds()) s += ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
    return s;
  };

  NB.formatValue = function (v, type, fmt) {
    if (v === null || v === undefined) return 'NULL';
    if (typeof v === 'object') return JSON.stringify(v);
    if (typeof v === 'number') return NB.formatNumber(v, fmt);
    return String(v);
  };

  // A DuckDB type's class, as TypeClass in src/core/chart.cpp decides it: numbers by their whole name
  // (so INTERVAL and POINT are not numbers), date and time types by prefix, lists never.
  var NUMERIC_TYPES = ['tinyint', 'smallint', 'integer', 'bigint', 'hugeint', 'utinyint', 'usmallint', 'uinteger', 'ubigint',
    'uhugeint', 'float', 'double', 'decimal', 'real', 'int', 'numeric', 'int1', 'int2', 'int4', 'int8', 'float4', 'float8',
    'varint', 'bignum'];
  NB.typeClass = function (t) {
    t = String(t || '').toLowerCase();
    if (t.indexOf('[') >= 0) return 'nominal';
    if (t.slice(0, 4) === 'date' || t.slice(0, 4) === 'time') return 'temporal';
    return NUMERIC_TYPES.indexOf(t.split('(')[0].trim()) >= 0 ? 'quantitative' : 'nominal';
  };
  NB.isNumericType = function (t) { return NB.typeClass(t) === 'quantitative'; };
  NB.isTemporalType = function (t) { return NB.typeClass(t) === 'temporal'; };

  // ---- Markdown -----------------------------------------------------------------------------
  // CommonMark-ish: headings, paragraphs, emphasis, code, links, lists, quotes, rules, tables and
  // fenced code. Raw HTML is escaped, never interpreted. $maths$ is shown as written; ```mermaid
  // fences are shown as code (no Mermaid or KaTeX library is bundled yet).

  function inline(text) {
    var codes = [];
    text = text.replace(/`([^`]+)`/g, function (_, c) { codes.push(c); return '\u0000' + (codes.length - 1) + '\u0000'; });
    text = NB.esc(text);
    text = text.replace(/!\[([^\]]*)\]\(([^)\s]+)\)/g, function (_, alt, url) {
      return /^(https?:|data:image\/)/.test(url) ? '<img alt="' + alt + '" src="' + url + '">' : alt;
    });
    text = text.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, function (_, label, url) {
      return /^(https?:|mailto:|#)/.test(url) ? '<a href="' + url + '" target="_blank" rel="noopener">' + label + '</a>' : label;
    });
    text = text.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>').replace(/__([^_]+)__/g, '<strong>$1</strong>');
    text = text.replace(/(^|[^*])\*([^*\s][^*]*)\*/g, '$1<em>$2</em>').replace(/(^|\W)_([^_\s][^_]*)_(?=\W|$)/g, '$1<em>$2</em>');
    text = text.replace(/~~([^~]+)~~/g, '<del>$1</del>');
    text = text.replace(/\$([^$\s][^$]*)\$/g, '<span class="nb-math">$1</span>');
    text = text.replace(/\uE000([^\uE001]*)\uE001/g, '<mark class="nb-val">$1</mark>');
    return text.replace(/\u0000(\d+)\u0000/g, function (_, i) { return '<code>' + NB.esc(codes[+i]) + '</code>'; });
  }

  NB.markdown = function (src) {
    var lines = String(src || '').replace(/\r/g, '').split('\n'), out = [], i = 0;
    function isTableSep(l) { return /^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$/.test(l); }
    function cells(l) { return l.trim().replace(/^\||\|$/g, '').split('|').map(function (c) { return c.trim(); }); }
    while (i < lines.length) {
      var line = lines[i];
      var fence = /^\s*(```|~~~)\s*(\S*)/.exec(line);
      if (fence) {
        var body = []; i++;
        while (i < lines.length && lines[i].trim().indexOf(fence[1]) !== 0) body.push(lines[i++]);
        i++;
        var sql = /^(sql|duckdb)$/i.test(fence[2]) && NB.highlightSql;
        out.push('<pre class="nb-code' + (fence[2] ? ' lang-' + NB.esc(fence[2]) : '') + '"><code>' + (sql ? NB.highlightSql(body.join('\n')) : NB.esc(body.join('\n'))) + '</code></pre>');
        continue;
      }
      if (/^\s*$/.test(line)) { i++; continue; }
      var h = /^(#{1,6})\s+(.*?)\s*#*\s*$/.exec(line);
      if (h) { out.push('<h' + h[1].length + '>' + inline(h[2]) + '</h' + h[1].length + '>'); i++; continue; }
      if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) { out.push('<hr>'); i++; continue; }
      if (/^\s*>/.test(line)) {
        var q = [];
        while (i < lines.length && /^\s*>/.test(lines[i])) q.push(lines[i++].replace(/^\s*>\s?/, ''));
        out.push('<blockquote>' + NB.markdown(q.join('\n')) + '</blockquote>');
        continue;
      }
      if (line.indexOf('|') >= 0 && i + 1 < lines.length && isTableSep(lines[i + 1])) {
        var head = cells(line), aligns = cells(lines[i + 1]).map(function (c) {
          return /^:-+:$/.test(c) ? 'center' : /-:$/.test(c) ? 'right' : 'left';
        });
        var t = '<table class="nb-md-table"><thead><tr>' + head.map(function (c, k) {
          return '<th style="text-align:' + aligns[k] + '">' + inline(c) + '</th>';
        }).join('') + '</tr></thead><tbody>';
        i += 2;
        while (i < lines.length && lines[i].indexOf('|') >= 0 && !/^\s*$/.test(lines[i])) {
          t += '<tr>' + cells(lines[i++]).map(function (c, k) {
            return '<td style="text-align:' + (aligns[k] || 'left') + '">' + inline(c) + '</td>';
          }).join('') + '</tr>';
        }
        out.push(t + '</tbody></table>');
        continue;
      }
      var li = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(line);
      if (li) {
        var ordered = /\d/.test(li[2]), items = [];
        while (i < lines.length) {
          var m = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(lines[i]);
          if (m) { items.push(m[3]); i++; }
          else if (/^\s{2,}\S/.test(lines[i]) && items.length) { items[items.length - 1] += ' ' + lines[i++].trim(); }
          else break;
        }
        out.push('<' + (ordered ? 'ol' : 'ul') + '>' + items.map(function (x) {
          var task = /^\[( |x)\]\s+(.*)$/i.exec(x);
          return '<li>' + (task ? '<input type="checkbox" disabled' + (task[1] !== ' ' ? ' checked' : '') + '> ' + inline(task[2]) : inline(x)) + '</li>';
        }).join('') + '</' + (ordered ? 'ol' : 'ul') + '>');
        continue;
      }
      var para = [];
      while (i < lines.length && !/^\s*$/.test(lines[i]) && !/^(#{1,6}\s|\s*```|\s*>|\s*([-*+]|\d+[.)])\s)/.test(lines[i])) para.push(lines[i++]);
      if (!para.length) para.push(lines[i++]);
      out.push('<p>' + inline(para.join('\n')).replace(/ {2,}\n/g, '<br>') + '</p>');
    }
    return out.join('\n');
  };

  // ---- Result grid --------------------------------------------------------------------------
  NB.pageSizes = [10, 25, 50, 100, 250, 1000];

  // How many rows a table shows without scrolling: tables never scroll (a scrollbar is not part of
  // the sketchbook), so a page holds at most what fits its box. Measured once from the stylesheet with
  // a hidden probe; rows do not wrap, so every row has the same height.
  var rowFit = 0;
  NB.tableRows = function () {
    if (rowFit) return rowFit;
    var probe = NB.el('div', { 'class': 'nb-grid-wrap', style: 'position:absolute;visibility:hidden;left:-9999px;top:0', 'aria-hidden': 'true' });
    probe.innerHTML = '<table class="nb-grid"><thead><tr><th class="nb-rownum"></th><th><div class="nb-colname">x</div></th></tr></thead>' +
      '<tbody><tr><td class="nb-rownum">1</td><td>x</td></tr></tbody></table>';
    document.body.appendChild(probe);
    var head = probe.querySelector('thead').offsetHeight, row = probe.querySelector('tbody tr').offsetHeight;
    var max = parseFloat(getComputedStyle(probe).maxHeight);
    probe.remove();
    if (!row || !isFinite(max)) return 25;
    rowFit = Math.max(3, Math.floor((max - head - 4) / row));
    return rowFit;
  };

  // A result grid with a page-size picker. It never scrolls: a page holds what fits its box
  // (NB.tableRows), and columns that do not fit its width are paged with ‹ › instead.
  //   data: {columns, rows, total}
  //   opts.pageSize: rows per page (default 25, at most NB.tableRows())
  //   opts.onPage(offset, size): fetch a page from the server; without it, pages through data.rows
  //   opts.onPageSize(size): the reader picked a page size (the editor writes it to the cell)
  //   opts.offset: first row of data.rows (server paging)
  //   opts.formats {col: fmt}, opts.bars [col], opts.heat [col]
  NB.table = function (container, data, opts) {
    opts = opts || {};
    container.innerHTML = '';
    if (!data || !data.columns) return;
    var fit = NB.tableRows(), size = Math.min(opts.pageSize || 25, fit);
    var serverPaging = !!opts.onPage;
    var all = data.rows || [], cols = data.columns;
    var offset = serverPaging ? (opts.offset || 0) : (container.__nbOffset || 0);
    if (!serverPaging && offset >= all.length) offset = 0;
    var rows = serverPaging ? all.slice(0, size) : all.slice(offset, offset + size);
    var total = data.total != null ? data.total : all.length;
    var available = serverPaging ? total : all.length;

    var wrap = NB.el('div', { 'class': 'nb-grid-wrap' });
    var table = NB.el('table', { 'class': 'nb-grid' });
    var thead = '<thead><tr><th class="nb-rownum"></th>' + cols.map(function (c) {
      return '<th><div class="nb-colname">' + NB.esc(c.name) + '</div></th>';
    }).join('') + '</tr>';
    var ranges = {};
    (opts.bars || []).concat(opts.heat || []).forEach(function (name) {
      var k = cols.findIndex(function (c) { return c.name === name; });
      if (k < 0) return;
      var nums = all.map(function (r) { return Number(r[k]); }).filter(isFinite);
      ranges[name] = [Math.min.apply(null, nums.concat([0])), Math.max.apply(null, nums)];
    });
    var heat = NB.ramp('blues');
    var body = '<tbody>' + rows.map(function (r, i) {
      return '<tr><td class="nb-rownum">' + (offset + i + 1) + '</td>' + r.map(function (v, k) {
        var col = cols[k], numeric = typeof v === 'number', style = '', text = NB.formatValue(v, col.type, (opts.formats || {})[col.name]);
        if (ranges[col.name] && numeric) {
          var range = ranges[col.name], t = (v - range[0]) / ((range[1] - range[0]) || 1);
          if ((opts.heat || []).indexOf(col.name) >= 0) style = 'background:' + heat(t * 0.8);
          else style = 'background:linear-gradient(90deg, var(--nb-bar) ' + Math.round(t * 100) + '%, transparent ' + Math.round(t * 100) + '%)';
        }
        return '<td class="' + (v === null ? 'nb-null' : numeric ? 'nb-num' : '') + '" style="' + style + '">' + NB.esc(text) + '</td>';
      }).join('') + '</tr>';
    }).join('') + '</tbody>';
    table.innerHTML = thead + '</thead>' + body;
    wrap.appendChild(table);
    container.appendChild(wrap);
    // Once laid out, in its final fonts: the columns that fit the width, then the hand-drawn rule
    // under their names.
    var fonts = document.fonts && document.fonts.status !== 'loaded' ? document.fonts.ready : null;
    var laidOut = function (f) { if (fonts) fonts.then(function () { requestAnimationFrame(f); }); else requestAnimationFrame(f); };
    laidOut(function () {
      fitColumns();
      if (!window.Sketch) return;
      var head = table.querySelector('thead tr');
      if (!head || !table.offsetWidth) return;
      var svg = Sketch.el('svg', { 'class': 'nb-head-rule', width: table.offsetWidth, height: 6 });
      svg.style.top = (head.offsetTop + head.offsetHeight - 3) + 'px';
      Sketch.reseed(cols.length * 31 + table.offsetWidth);
      Sketch.line(svg, 2, 3, table.offsetWidth - 2, 3, { width: 1.3 });
      wrap.appendChild(svg);
    });

    var go = function (next) {
      if (serverPaging) opts.onPage(next, size);
      else { container.__nbOffset = next; NB.table(container, data, opts); }
    };

    // Columns from data.__nbColumn on that fit the wrap's width are shown; the rest are hidden
    // and reached with ‹ › in the footer. The row number always shows. The first column shown
    // shows even if it is too wide on its own: it is then cut short with an ellipsis.
    var colControls = NB.el('span', { 'class': 'nb-col-pages' });
    function fitColumns() {
      var avail = wrap.clientWidth, heads = table.querySelectorAll('thead th');
      if (!avail || heads.length < 2) return;
      // Everything fits when the table is no wider than its box. Adding up the columns' rounded widths
      // can say otherwise by a pixel when the table fills its box exactly (a presented screen does).
      if (!data.__nbColumn && table.offsetWidth <= avail) return;
      var first = Math.min(data.__nbColumn || 0, cols.length - 1), used = heads[0].offsetWidth, last = first;
      for (var k = first; k < cols.length; k++) {
        var w = heads[k + 1].offsetWidth;
        if (k > first && used + w > avail) break;
        used += w;
        last = k;
      }
      if (first === 0 && last === cols.length - 1) return;
      Array.prototype.forEach.call(table.rows, function (tr) {
        for (var k = 0; k < cols.length; k++) if (k < first || k > last) tr.cells[k + 1].classList.add('nb-col-off');
      });
      if (used > avail) table.classList.add('nb-grid-narrow');
      var page = function (next) { data.__nbColumn = next; NB.table(container, data, opts); };
      var shown = last - first + 1;
      colControls.appendChild(NB.el('button', { 'class': 'nb-col-page', text: '‹', title: 'Previous columns', disabled: first <= 0 ? 'disabled' : null,
        onclick: function () { page(Math.max(0, first - shown)); } }));
      colControls.appendChild(NB.el('span', { 'class': 'nb-col-range', text: 'columns ' + (first + 1) + (shown > 1 ? '–' + (last + 1) : '') + ' of ' + cols.length }));
      colControls.appendChild(NB.el('button', { 'class': 'nb-col-page', text: '›', title: 'More columns', disabled: last >= cols.length - 1 ? 'disabled' : null,
        onclick: function () { page(last + 1); } }));
    }
    // A table drawn for one width is fitted again when its box changes width (a window resized).
    if (window.ResizeObserver) {
      var width = null;
      new ResizeObserver(function (entries) {
        var w = Math.round(entries[0].contentRect.width);
        if (width !== null && w !== width && wrap.isConnected) { width = w; NB.table(container, data, opts); return; }
        width = w;
      }).observe(wrap);
    }

    var footer = NB.el('div', { 'class': 'nb-grid-footer' });
    footer.appendChild(NB.el('span', { text: total === 1 ? '1 row' : NB.formatNumber(total, ',') + ' rows' }));
    if (available > size || offset > 0) {
      footer.appendChild(NB.el('button', { 'class': 'nb-page', text: '‹', title: 'Previous page', disabled: offset <= 0 ? 'disabled' : null, onclick: function () { go(Math.max(0, offset - size)); } }));
      footer.appendChild(NB.el('span', { 'class': 'nb-page-range', text: (offset + 1) + '–' + Math.min(available, offset + rows.length) }));
      footer.appendChild(NB.el('button', { 'class': 'nb-page', text: '›', title: 'Next page', disabled: offset + size >= available ? 'disabled' : null, onclick: function () { go(offset + size); } }));
    }
    if (!serverPaging && total > all.length) {
      footer.appendChild(NB.el('span', { text: '(' + NB.formatNumber(all.length, ',') + ' included)' }));
    }
    footer.appendChild(colControls);
    // Page sizes up to what fits the table's box.
    var picker = NB.el('select', { 'class': 'nb-page-size', title: 'Rows per page' });
    var sizes = NB.pageSizes.filter(function (n) { return n < fit; }).concat([size, fit]);
    sizes = sizes.filter(function (n, k) { return sizes.indexOf(n) === k; }).sort(function (a, b) { return a - b; });
    sizes.forEach(function (n) { picker.appendChild(NB.el('option', { value: n, text: n + ' per page', selected: n === size ? 'selected' : null })); });
    picker.onchange = function () {
      var next = +picker.value;
      if (opts.onPageSize) opts.onPageSize(next);
      else { opts.pageSize = next; container.__nbOffset = 0; NB.table(container, data, opts); }
    };
    footer.appendChild(NB.el('span', { 'class': 'nb-spacer' }));
    footer.appendChild(picker);
    container.appendChild(footer);
  };

  // A chart a result probably wants, from its column types (DESIGN §6 "Suggested chart").
  NB.suggestChart = function (columns) {
    var time = columns.filter(function (c) { return NB.isTemporalType(c.type); });
    var nums = columns.filter(function (c) { return NB.isNumericType(c.type); });
    var cats = columns.filter(function (c) { return !NB.isTemporalType(c.type) && !NB.isNumericType(c.type); });
    var lat = columns.find(function (c) { return /^lat(itude)?$/i.test(c.name); });
    var lon = columns.find(function (c) { return /^(lon|lng|long|longitude)$/i.test(c.name); });
    if (lat && lon) return 'type: map\nlat: ' + lat.name + '\nlon: ' + lon.name;
    if (columns.length === 1 && nums.length === 1) return 'type: kpi\nvalue: ' + nums[0].name;
    if (time.length && nums.length) {
      return 'mark: line\nx: ' + time[0].name + '\ny: ' + nums[0].name + (cats.length ? '\ncolor: ' + cats[0].name : '');
    }
    if (cats.length && nums.length) return 'mark: bar\nx: ' + cats[0].name + '\ny: ' + nums[0].name;
    if (nums.length >= 2) return 'mark: point\nx: ' + nums[0].name + '\ny: ' + nums[1].name;
    if (nums.length === 1) return 'mark: histogram\nx: ' + nums[0].name;
    if (cats.length) return 'mark: bar\nx: ' + cats[0].name + '\ny: {agg: count}';
    return 'type: table';
  };

  // A short name for a chart: its title, else its mark or type.
  NB.chartLabel = function (chart, k) {
    var r = (chart && chart.render) || {};
    if (r.title) return r.title;
    var layer = (r.layers || [])[0];
    if (r.kind === 'grammar' && layer) return layer.from_mark || layer.mark;
    return r.kind || 'chart ' + (k + 1);
  };

  // An input cell's widget, the same in the editor and an export. onChange(cell, value) fires when the
  // reader changes it. opts.values: the only values offered (a precomputed export's select);
  // opts.enabled === false disables it.
  NB.inputWidget = function (cell, onChange, opts) {
    opts = opts || {};
    var values = opts.values;
    // An option given empty (label=, step=) is not set.
    var option = function (key, fallback) {
      var o = (cell.options || {})[key];
      return o == null || o === '' ? fallback : o;
    };
    var box = NB.el('div', { 'class': 'nb-input' });
    box.appendChild(NB.el('label', { text: option('label', cell.name) }));
    var w = cell.widget, v = cell.value, control;
    var fire = function (value) { onChange(cell, value); };
    if (values) {
      control = NB.el('select', { onchange: function () { fire(values[+control.value]); } });
      values.forEach(function (x, i) { control.appendChild(NB.el('option', { value: i, text: NB.formatValue(x), selected: NB.key(x) === NB.key(v) ? 'selected' : null })); });
    } else if (w === 'select' || w === 'multiselect') {
      var choices = cell.choices || (option('options', '') ? option('options', '').split(',') : []);
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
      var shown = NB.el('span', { 'class': 'nb-input-value', text: NB.formatNumber(v) });
      var input = NB.el('input', { type: w === 'slider' ? 'range' : 'number', min: option('min'), max: option('max'), step: option('step', 'any'),
        value: v, oninput: function () { shown.textContent = NB.formatNumber(input.value); }, onchange: function () { fire(Number(input.value)); } });
      box.appendChild(input);
      box.appendChild(shown);
    } else if (w === 'date') {
      control = NB.el('input', { type: 'date', value: v ? String(v).slice(0, 10) : '', min: option('min'), max: option('max'), onchange: function () { fire(control.value); } });
    } else if (w === 'daterange') {
      var a = NB.el('input', { type: 'date', value: v && v.lo ? String(v.lo).slice(0, 10) : '' }), b = NB.el('input', { type: 'date', value: v && v.hi ? String(v.hi).slice(0, 10) : '' });
      a.onchange = b.onchange = function () { fire({ lo: a.value, hi: b.value }); };
      box.appendChild(a); box.appendChild(NB.el('span', { text: '–' })); box.appendChild(b);
    } else {
      control = NB.el('input', { type: 'text', value: v == null ? '' : v, placeholder: option('placeholder'), onchange: function () { fire(control.value); } });
    }
    if (control) box.appendChild(control);
    if (opts.enabled === false) box.querySelectorAll('input,select').forEach(function (e) { e.disabled = true; });
    return box;
  };

  // ---- Cell framing, shared by the editor and the export viewer ------------------------------
  // `query` for a SQL cell that is a query, `sql` for one that changes the database, else the cell's kind.
  NB.kindName = function (cell) { return cell.kind === 'sql' ? (cell.is_query ? 'query' : 'sql') : cell.kind; };
  // Whether a hide= value hides `what` (code or output).
  NB.hides = function (hide, what) { return hide === 'all' || hide === what; };
  // The columns of 12 a cell spans in reading view (span=).
  NB.span = function (span) { return Math.max(1, Math.min(12, +span)); };
  // The tabs over a query cell's result: one per chart (marked when it failed), then the table.
  NB.queryTabItems = function (charts, count) {
    var items = [];
    for (var k = 0; k < count; k++) {
      var chart = (charts || [])[k];
      items.push({ label: (chart && chart.error ? '⚠ ' : '') + NB.chartLabel(chart, k) });
    }
    return items.concat([{ label: 'table', title: 'The query\'s rows' }]);
  };

  // A row of tabs. items: [{label, title}]; extra: elements appended after the tabs.
  NB.tabs = function (items, active, onPick, extra) {
    var bar = NB.el('div', { 'class': 'nb-tabs', role: 'tablist' });
    items.forEach(function (item, k) {
      bar.appendChild(NB.el('button', { 'class': 'nb-tab' + (k === active ? ' on' : ''), role: 'tab', 'aria-selected': k === active ? 'true' : 'false',
        text: item.label, title: item.title || item.label, onclick: function () { onPick(k); } }));
    });
    (extra || []).forEach(function (e) { bar.appendChild(e); });
    return bar;
  };

  // ---- Hover values ----------------------------------------------------------------------------
  // One tooltip for the page: any element with data-tip, or a chart that answers __nbHover (lines),
  // shows its values next to the pointer. A brush overlay over the marks is looked through.
  NB.tip = (function () {
    var box = null, hoverSvg = null;
    function show(text, e) {
      if (!box) { box = NB.el('div', { 'class': 'nb-tip', role: 'tooltip' }); document.body.appendChild(box); }
      box.innerHTML = '';
      text.split('\n').forEach(function (line) {
        var at = line.indexOf(': ');
        box.appendChild(at > 0 ? NB.el('div', {}, [NB.el('span', { 'class': 'nb-tip-key', text: line.slice(0, at) }), document.createTextNode(' ' + line.slice(at + 2))])
          : NB.el('div', { text: line }));
      });
      box.style.display = 'block';
      var x = e.clientX + 14, y = e.clientY + 14, bw = box.offsetWidth, bh = box.offsetHeight;
      if (x + bw > window.innerWidth - 8) x = e.clientX - bw - 14;
      if (y + bh > window.innerHeight - 8) y = e.clientY - bh - 14;
      box.style.left = Math.max(4, x) + 'px';
      box.style.top = Math.max(4, y) + 'px';
    }
    function hide() {
      if (box) box.style.display = 'none';
      if (hoverSvg && hoverSvg.__nbLeave) hoverSvg.__nbLeave();
      hoverSvg = null;
    }
    function under(e) {
      var el = e.target && e.target.closest ? e.target.closest('[data-tip]') : null;
      if (el || !(e.target instanceof Element) || !e.target.classList.contains('nb-brush-overlay')) return el;
      var stack = document.elementsFromPoint(e.clientX, e.clientY);
      for (var i = 0; i < stack.length; i++) if (stack[i].hasAttribute && stack[i].hasAttribute('data-tip')) return stack[i];
      return null;
    }
    document.addEventListener('mousemove', function (e) {
      var svg = e.target && e.target.closest ? e.target.closest('svg') : null;
      if (hoverSvg && hoverSvg !== svg) hide();
      var lineText = svg && svg.__nbHover ? svg.__nbHover(e.clientX, e.clientY) : null;
      if (lineText) { hoverSvg = svg; show(lineText, e); return; }
      var el = under(e);
      if (el && el.getAttribute('data-tip')) show(el.getAttribute('data-tip'), e);
      else hide();
    }, { passive: true });
    document.addEventListener('mouseleave', hide);
    window.addEventListener('scroll', hide, true);
    return { show: show, hide: hide };
  })();

  return NB;
})();
