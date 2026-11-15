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

  // Colour-vision-deficiency-safe categorical palette (Okabe-Ito, then extended).
  NB.palette = ['#0072B2', '#E69F00', '#009E73', '#D55E00', '#CC79A7', '#56B4E9', '#F0E442', '#000000',
    '#999999', '#8C564B', '#17BECF', '#BCBD22'];
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
      out = n.toLocaleString('en-US', { minimumFractionDigits: trim ? 0 : p, maximumFractionDigits: p, useGrouping: !!comma || !spec });
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
  // data: {columns: [{name, type}], rows: [[...]], total}. A profile row under the header shows
  // each column's null share and either a histogram (numbers, dates) or its top values, computed
  // from the rows on hand.

  function profile(rows, c, type) {
    var values = [], nulls = 0;
    rows.forEach(function (r) { if (r[c] == null) nulls++; else values.push(r[c]); });
    var p = { nulls: rows.length ? nulls / rows.length : 0, n: values.length };
    if (NB.isNumericType(type) || NB.isTemporalType(type)) {
      var nums = values.map(function (v) { return NB.isTemporalType(type) ? +NB.toDate(v) : Number(v); }).filter(isFinite);
      if (nums.length) {
        var lo = Math.min.apply(null, nums), hi = Math.max.apply(null, nums), bins = new Array(12).fill(0);
        nums.forEach(function (v) { bins[hi > lo ? Math.min(11, Math.floor((v - lo) / (hi - lo) * 12)) : 0]++; });
        p.hist = bins;
        p.lo = NB.isTemporalType(type) ? NB.formatDate(lo) : NB.formatNumber(lo);
        p.hi = NB.isTemporalType(type) ? NB.formatDate(hi) : NB.formatNumber(hi);
      }
    } else {
      var counts = {};
      values.forEach(function (v) { var k = typeof v === 'object' ? JSON.stringify(v) : String(v); counts[k] = (counts[k] || 0) + 1; });
      p.distinct = Object.keys(counts).length;
      p.top = Object.keys(counts).sort(function (a, b) { return counts[b] - counts[a]; }).slice(0, 3).map(function (k) { return [k, counts[k]]; });
    }
    return p;
  }

  function profileCell(p) {
    var html = '';
    if (p.hist) {
      var max = Math.max.apply(null, p.hist) || 1;
      html += '<svg class="nb-spark" width="72" height="18">' + p.hist.map(function (b, k) {
        var h = Math.round(b / max * 16);
        return '<rect x="' + k * 6 + '" y="' + (17 - h) + '" width="5" height="' + h + '"></rect>';
      }).join('') + '</svg><div class="nb-prof-range">' + NB.esc(p.lo) + ' – ' + NB.esc(p.hi) + '</div>';
    } else if (p.top) {
      html += '<div class="nb-prof-top">' + p.top.map(function (t) {
        return '<span title="' + NB.esc(t[0]) + '">' + NB.esc(t[0].length > 14 ? t[0].slice(0, 13) + '…' : t[0]) + '</span> ' + t[1];
      }).join('<br>') + '</div><div class="nb-prof-range">' + p.distinct + ' distinct</div>';
    }
    if (p.nulls > 0) html += '<div class="nb-prof-null">' + Math.round(p.nulls * 100) + '% null</div>';
    return html;
  }

  // opts: {pageSize, offset, onPage(offset), profile, formats: {col: fmt}, bars: [col], heat: [col]}
  NB.table = function (container, data, opts) {
    opts = opts || {};
    container.innerHTML = '';
    if (!data || !data.columns) return;
    var rows = data.rows || [], cols = data.columns;
    var wrap = NB.el('div', { 'class': 'nb-grid-wrap' });
    var table = NB.el('table', { 'class': 'nb-grid' });
    var thead = '<thead><tr><th class="nb-rownum"></th>' + cols.map(function (c) {
      return '<th><div class="nb-colname">' + NB.esc(c.name) + '</div><div class="nb-coltype">' + NB.esc(c.type) + '</div></th>';
    }).join('') + '</tr>';
    if (opts.profile !== false && rows.length > 1) {
      thead += '<tr class="nb-profile"><th></th>' + cols.map(function (c, k) {
        return '<th>' + profileCell(profile(rows, k, c.type)) + '</th>';
      }).join('') + '</tr>';
    }
    var ranges = {};
    (opts.bars || []).concat(opts.heat || []).forEach(function (name) {
      var k = cols.findIndex(function (c) { return c.name === name; });
      if (k < 0) return;
      var nums = rows.map(function (r) { return Number(r[k]); }).filter(isFinite);
      ranges[name] = [Math.min.apply(null, nums.concat([0])), Math.max.apply(null, nums)];
    });
    var heat = NB.ramp('blues');
    var offset = opts.offset || 0;
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
    var total = data.total != null ? data.total : rows.length, size = opts.pageSize || rows.length;
    var footer = NB.el('div', { 'class': 'nb-grid-footer' });
    footer.appendChild(NB.el('span', { text: total === 1 ? '1 row' : NB.formatNumber(total, ',') + ' rows' }));
    if (opts.onPage && total > size) {
      footer.appendChild(NB.el('button', { text: '‹', disabled: offset <= 0 ? 'disabled' : null, onclick: function () { opts.onPage(Math.max(0, offset - size)); } }));
      footer.appendChild(NB.el('span', { text: (offset + 1) + '–' + Math.min(total, offset + rows.length) }));
      footer.appendChild(NB.el('button', { text: '›', disabled: offset + size >= total ? 'disabled' : null, onclick: function () { opts.onPage(offset + size); } }));
    } else if (data.truncated) {
      footer.appendChild(NB.el('span', { text: ' (first ' + rows.length + ' shown)' }));
    }
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

  return NB;
})();
