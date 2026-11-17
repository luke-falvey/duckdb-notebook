// SQL syntax highlighting for the editor and exports. The tokenizer follows src/core/sql_lexer.cpp
// (nested block comments, '' escapes, $tag$ strings versus $name variables), and its keyword list
// is DuckDB's own (duckdb_keywords(), sent by the server), so it matches the DuckDB being used.
(function () {
  'use strict';

  // Statement words. DuckDB lists many of them as unreserved keywords, a category that also holds
  // common column names (day, name, value), so only these are taken from it. Also the keywords used
  // until the server's list arrives, and in pages without one.
  var FALLBACK = ('select from where group by order having limit offset with as and or not in is null like ilike between case when then ' +
    'else end join left right inner outer full cross on using union all distinct except intersect create replace table view temp ' +
    'temporary insert into values update set delete drop alter attach detach copy load install pivot unpivot qualify window over ' +
    'partition asc desc nulls first last true false cast exists any asof positional lateral filter within recursive returning ' +
    'describe summarize show variable macro schema if').split(' ');
  var TYPES = ('integer int bigint smallint tinyint hugeint double float real decimal numeric varchar text char boolean bool date ' +
    'time timestamp timestamptz interval uuid blob json struct map list union bit uinteger ubigint usmallint utinyint').split(' ');

  var keywords = {}, types = {};
  function load(list, into) { list.forEach(function (w) { into[String(w).toLowerCase()] = true; }); }
  load(FALLBACK, keywords);
  load(TYPES, types);

  // {keywords: [...], types: [...]} from the server: DuckDB's reserved, column-name and type/function
  // keywords, and its type names. They extend the lists above.
  NB.setSqlWords = function (words) {
    if (!words) return;
    if (words.keywords && words.keywords.length) { keywords = {}; load(FALLBACK, keywords); load(words.keywords, keywords); }
    if (words.types && words.types.length) { types = {}; load(TYPES, types); load(words.types, types); }
  };

  function isIdentStart(c) { return /[A-Za-z_\u0080-\uffff]/.test(c); }
  function isIdentChar(c) { return /[A-Za-z0-9_\u0080-\uffff]/.test(c); }

  // Splits SQL into [{type, text}] covering every character. Types: kw, type, fn, ident, name (a
  // notebook cell), var, str, num, comment, op, ws.
  NB.tokenizeSql = function (text, opts) {
    var names = (opts && opts.names) || {}, out = [], i = 0, n = text.length, lastCode = null;
    function push(type, start) {
      if (i <= start) return;
      var token = { type: type, text: text.slice(start, i) };
      out.push(token);
      if (type !== 'ws' && type !== 'comment') lastCode = token;
    }
    while (i < n) {
      var c = text[i], d = text[i + 1], start = i;
      if (c === '-' && d === '-') {
        while (i < n && text[i] !== '\n') i++;
        push('comment', start);
      } else if (c === '/' && d === '*') {
        var depth = 0;
        while (i < n) {
          if (text[i] === '/' && text[i + 1] === '*') { depth++; i += 2; }
          else if (text[i] === '*' && text[i + 1] === '/') { depth--; i += 2; if (!depth) break; }
          else i++;
        }
        push('comment', start);
      } else if (c === "'") {
        i++;
        while (i < n) { if (text[i] === "'") { if (text[i + 1] === "'") i += 2; else { i++; break; } } else i++; }
        push('str', start);
      } else if (c === '"') {
        i++;
        while (i < n) { if (text[i] === '"') { if (text[i + 1] === '"') i += 2; else { i++; break; } } else i++; }
        push('ident', start);
      } else if (c === '$') {
        var end = i + 1;
        while (end < n && isIdentChar(text[end])) end++;
        var tagOk = end === i + 1 || isIdentStart(text[i + 1]);
        if (end < n && text[end] === '$' && tagOk) {
          var tag = text.slice(i, end + 1), close = text.indexOf(tag, end + 1);
          i = close < 0 ? n : close + tag.length;
          push('str', start);
        } else if (end > i + 1) {
          i = end;
          push('var', start);
        } else if (text[i + 1] === '"') {
          i += 2;
          while (i < n && text[i] !== '"') i++;
          i = Math.min(n, i + 1);
          push('var', start);
        } else { i++; push('op', start); }
      } else if (/\s/.test(c)) {
        while (i < n && /\s/.test(text[i])) i++;
        push('ws', start);
      } else if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(d || ''))) {
        while (i < n && /[0-9._eE]/.test(text[i])) { if ((text[i] === 'e' || text[i] === 'E') && /[+-]/.test(text[i + 1])) i++; i++; }
        push('num', start);
      } else if (isIdentStart(c)) {
        while (i < n && isIdentChar(text[i])) i++;
        var word = text.slice(start, i).toLowerCase(), j = i;
        while (j < n && (text[j] === ' ' || text[j] === '\t')) j++;
        var prev = out.length && out[out.length - 1].type === 'op' && out[out.length - 1].text === '.';
        // A name after AS is an alias, not a reference to a cell.
        var alias = lastCode && lastCode.type === 'kw' && lastCode.text.toLowerCase() === 'as';
        var type = text[j] === '(' && !keywords[word] ? 'fn' : prev ? 'ident' : names[word] && !alias ? 'name' : keywords[word] ? 'kw' : types[word] ? 'type' : 'ident';
        push(type, start);
      } else {
        i++;
        push('op', start);
      }
    }
    return out;
  };

  function esc(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

  // HTML with one <span class="tk-…"> per token; the text is exactly the input.
  NB.highlightSql = function (text, opts) {
    return NB.tokenizeSql(text || '', opts).map(function (t) {
      return t.type === 'ws' || t.type === 'ident' || t.type === 'op' ? esc(t.text) : '<span class="tk-' + t.type + '">' + esc(t.text) + '</span>';
    }).join('');
  };
})();
