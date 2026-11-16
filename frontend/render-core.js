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

  NB.isNumericType = function (t) { return /INT|FLOAT|DOUBLE|DECIMAL|REAL|NUMERIC/i.test(t || ''); };
  NB.isTemporalType = function (t) { return /^(DATE|TIME|TIMESTAMP)/i.test(t || ''); };

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
        out.push('<pre class="nb-code' + (fence[2] ? ' lang-' + NB.esc(fence[2]) : '') + '"><code>' + NB.esc(body.join('\n')) + '</code></pre>');
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

  // A result grid with a page-size picker.
  //   data: {columns, rows, total}
  //   opts.pageSize: rows per page (default 25)
  //   opts.onPage(offset, size): fetch a page from the server; without it, pages through data.rows
  //   opts.onPageSize(size): the reader picked a page size (the editor writes it to the cell)
  //   opts.offset: first row of data.rows (server paging)
  //   opts.formats {col: fmt}, opts.bars [col], opts.heat [col]
  NB.table = function (container, data, opts) {
    opts = opts || {};
    container.innerHTML = '';
    if (!data || !data.columns) return;
    var size = opts.pageSize || 25;
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
    // The header rule is a hand-drawn line under the column names.
    if (window.Sketch) requestAnimationFrame(function () {
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
    var picker = NB.el('select', { 'class': 'nb-page-size', title: 'Rows per page' });
    var sizes = NB.pageSizes.indexOf(size) < 0 ? NB.pageSizes.concat([size]).sort(function (a, b) { return a - b; }) : NB.pageSizes;
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

  return NB;
})();
