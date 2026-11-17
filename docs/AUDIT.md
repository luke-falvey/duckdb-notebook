# Design audit

An audit of the codebase for bad design and repeated structure, made before a simplification pass.
Audited at `94eb001` (main, 2026-11-17), then extended to the four features merged by `e49f765`. Line
numbers in the first part refer to `94eb001`, in the features section to `e49f765`.

Each finding has an id, where it is, why it is a problem, the proposed change, its **risk**, a rough
**size**, and a **class**:

- **Safe**: behaviour-preserving. No change to the file format, SQL functions (names, arguments,
  columns), the HTTP API, export output or pixels. These are the scope of the simplification PR.
- **Fix**: a bug or inconsistency whose fix changes behaviour. Listed so they are not lost; they belong
  in their own small PRs, not in a refactor that promises identical behaviour.
- **Docs**: documentation only.

Status (filled in during phase 2): *todo*, *done*, *deferred (why)*, *rejected (why)*.

## Ranking

Ranked by value for effort. The first group is the proposed scope of the simplification PR.

| # | Finding | Class | Risk | Size | Status |
|---|---|---|---|---|---|
| R1 | "Prepare a session and run every cell" written six times | Safe | low | −80 | done: `Runner::Prepare` and `Runner::RunAll`; callers that handle each outcome keep their loops; the extra html-wasm and precomputed runs stay (behaviour) |
| R2 | Per-cell result JSON built twice (server, export) | Safe | low | −40 | done in part: `RunForEditor` moved to `session.cpp`, one `RanFromResult`; the export keeps its own JSON (it reads inputs after the whole run and orders keys differently, so sharing would change exported files) |
| C1 | Core string helpers copied across files | Safe | low | −45 | done: `notebook/text.hpp` |
| F1 | Input widget written twice (editor, viewer) | Safe | low | −35 | done: `NB.inputWidget`; its `exported` flag keeps two pages' differences, logged as B31 |
| R3 | Table-function boilerplate (once-state, columns, named params) | Safe | low | −60 | done: `OnceState`/`OnceInit`, `NamedString`/`NamedBool` in `common.cpp`. Not done: the loop-and-compare parameter readers (their NULL handling differs) and the `column` lambdas (no gain) |
| R4 | Server handler boilerplate (session lookup, runner, notebook) | Safe | low-med | −80 | done in part: `Source(req, body)`. Deferred: `WithRunner` and moving `/api/run`'s loop, which re-indent every handler lambda for little gain after R1 and R2 |
| C2 | `from=<rel>.<col>` parsed three times | Safe | low | −10 | done: `Cell::InputFrom`, with a test |
| R5 | `DESCRIBE` into columns written three times | Safe | low | −25 | done: `DescribeColumns` |
| F2 | Query tabs, hide rules, span/seed, kind label re-derived in editor and viewer | Safe | low | −30 | done: `NB.queryTabItems`, `NB.hides`, `NB.span`, `NB.kindName`. The Markdown export's hide rule is B11 |
| F3 | `app-extra.js` guarded as optional although it always loads | Safe | low | −10 | done |
| F4 | Repeated "edited, re-analyse later" sequence in `app.js` | Safe | low | −12 | done: `unsaved(ms)` |
| D1 | Dead code (C++ and JS) | Safe | low | −60 | done, except `Json::Raw` and `Runner::Save`, which the features started using (see the correction below); a map's double `color` is B30 |
| C3 | Pure logic in `src/functions` that belongs in `src/core` | Safe | low | ~300 moved | done in part: Markdown export and title (`document.cpp`), `ReplaySql` and `DisplaySql` (`script.cpp`), all with tests. Deferred: `VariableSqlType`'s type building, the `{{ }}` splice, the ATTACH SQL and the precomputed input space, which each need a callback threaded through for a few lines |
| C4 | Chart YAML parsed repeatedly; selection map rebuilt per chart | Safe | low | small | done: one parse in `CompileChart`, `NotebookSelections` (tested). Still built once per chart: caching it would tie a Runner to one notebook |
| T1 | Browser test start/stop repeated, shells leak on failure | Safe | low | −40 | done: `serve()`/`stop()`, same 280 checks |
| C5 | `chart.cpp` does too much (now ~1370 lines) | Safe | med | ±0 | todo |
| C6 | `[notebook]` metadata parsed in three places | Safe* | low-med | small | deferred: one `NotebookMeta` touches the parser, the runner, the cache, the title and a regular expression in `app.js`; more than a structural change should, so left for its own PR |
| R6 | Path / `db:name@version` / stem / extension logic in six variants | Safe* | low | ~60 moved | deferred: unifying odd paths changes behaviour |
| B1–B31 | Bugs found on the way (B15–B19 from the docs-site PR #11, B20–B29 in the merged features) | Fix | low | small each | deferred: second PR after the refactor, one commit and failing-first test per bug |
| DOC1–DOC11 | Docs that disagree with the code | Docs | none | small | done, except DOC1 (`display=`), which is B15 |
| N1 | Chart rules, bands and notes repeat their checks | Safe | low | −30 | with C5 |
| N2 | Editor result handling repeated | Safe | low | −20 | done: `show()`, `dropKept()` |
| N3 | Smaller server and cache items | Safe | low | small | rejected: `ParseOneQuery` would narrow what peek's `try` covers; the Value-to-text pair in `result_cache.cpp` is two short lines; moving `cache.Record` out of `NotebookJson` changes when the store is written |
| T2 | Small test findings | Safe | low | small | done: `revenue.expanded.sql` regenerated (one line had drifted) and tested; the authoring template runs in `notebook_run.test`. The duplicate `CHECK` is gone since the feature merges |
| F5 | Ink keys depend on child position | Fix | low | small | deferred: changes animation timing |

\* Safe only if the single rule chosen is the one the current callers agree on; edge cases are listed.

---

## Runner and server (`src/functions`)

### R1. "Prepare a session and run every cell" is written six times
- **Where:** `export.cpp:205-222` (`BuildPayload`), `:272-282` (`WasmPayload`), `:590-600`
  (precomputed), `:805-815` (data export); `notebook_run.cpp:250-253`; `server.cpp:313-317`. Each
  repeats `SessionState` + `Runner` + `AnalyzeText` + (sometimes) `ThrowIfInvalid` + `ApplyMetadata` +
  `InitializeVariables` + a loop over `RunOrder`.
- **Why:** the copies have drifted (see B3: four server endpoints never call `ApplyMetadata`), and
  every new feature (the result cache in flight) has to be threaded through six places.
- **Change:** `Runner::Prepare(notebook, source, params)` and `Runner::RunAll(notebook, options,
  callback)` in `session.cpp`; callers keep their own per-cell handling.
- **Not in scope:** removing the extra runs (html-wasm runs the notebook twice, precomputed 2+N times).
  That changes timing and what effect cells do, so it is a separate change.
- Safe, low risk, about −80 lines.

### R2. Per-cell result JSON is built twice
- **Where:** `server.cpp:344-390`, `export.cpp:223-253`. Both copy status, error, row_count, output,
  charts; both read an input's value with `SELECT getvariable(...)` and its `choices` with
  `InputChoices(..., 1000)`.
- **Why:** the two drift apart; the shapes already differ only in key names (`data`/`elapsed_ms` from
  the server; `table`/`chart` in exports).
- **Change:** one `OutcomeJson(runner, cell, outcome)` in `session.cpp` building the common fields;
  each caller adds its own keys. Key names stay exactly as they are (HTTP API and payload unchanged).
- Safe, low risk, about −40 lines.

### R3. Table-function boilerplate
- **Where:** a one-row "done" global state is defined five times (`SqlState`, `ChartState`,
  `OnceState` in `storage.cpp:80` and `server.cpp:719`, `ExportState`). A `column(name, type)` lambda is
  copied in `notebook_cells.cpp:85` and `notebook_run.cpp:228`. Named parameters are read three ways:
  `NamedString`/`NamedBool` (`storage.cpp:64-73`), loop-and-compare (`export.cpp:721`,
  `server.cpp:729`, `notebook_chart_query.cpp:322`), `find` (`notebook_run.cpp:210`). `StringOrNull`
  (`notebook_cells.cpp:75`) is re-inlined at `notebook_run.cpp:276`.
- **Change:** `OnceState`/`OnceInit`, `AddColumn`, `NamedString`/`NamedBool`/`NamedInt`,
  `StringOrNull` in `common.cpp`, declared in `functions.hpp`.
- Safe, low risk, about −60 lines.

### R4. Server handler boilerplate
- **Where:** `body.Get("nb") ? … : Param(req, "nb")` at `server.cpp:275,305,398,527`;
  `GetSession` + lock + database + `Runner` + `AnalyzeText` in five handlers
  (`:266-268, 305-315, 398-406, 426-430, 448-452, 462-465`). `/api/run`'s loop (`:322-392`) is
  runner logic living in the transport.
- **Change:** `SessionFor(state, req, body)` and a `WithRunner(...)` helper; move `/api/run`'s loop
  into `session.cpp` as `RunCells(...)` next to R1. `server.cpp` keeps routing, auth and SSE.
- Safe if `WithRunner` does exactly what each handler does today (it must *not* add `ApplyMetadata`
  where it is missing; that is B3). Low-medium risk (clang-format re-indents lambdas here). About −80.

### R5. "DESCRIBE a relation into columns" is written three times
- **Where:** `session.cpp:261-269`, `session.cpp:445-458` (`ColumnType`),
  `notebook_chart_query.cpp:333-342`.
- **Change:** one `DescribeColumns(Connection&, relation)` returning `vector<ChartColumn>`;
  `ColumnType` looks up its result.
- Safe, low risk, about −25 lines.

### R6. Path, `db:` and output-name logic in six variants
- **Where:** file stem in `storage.cpp:58` (`/\`), `export.cpp:78` (`/\:`), inline at
  `export.cpp:762-767` (strips `db:`), `server.cpp:550-551` (`/:`, no leading-dot guard). The `db:`
  prefix is tested in `common.cpp:36,52`, `session.cpp:201`, `server.cpp:140,283,580`,
  `export.cpp:762`; `name@version` parsed in `common.cpp:38-43` (with `atoi`) and `server.cpp:285-286`.
  Format → extension/MIME tables at `export.cpp:768` and `server.cpp:552-555`.
- **Change:** core `ParseSource(string) -> {stored, name, version, stem, dir}`,
  `ExportFileExtension(format)`, `ExportMimeType(format)`, with unit tests.
- Safe for ordinary paths. Odd paths (`a:b.sql`, `.hidden.sql`, `db:x@abc`) currently get different
  stems in different places; the unified rule must be chosen and the change noted. Low risk, ~60 moved.

## Core (`src/core`)

### C1. String helpers copied across core files
- **Where:** `SqlString` (`chart.cpp:26-35`) is identical to `QuoteString` (`script.cpp:28-37`).
  `Lower` in `chart.cpp:19` and `graph.cpp:16`. `Trim` in `sql_lexer.cpp:20`, `yaml.cpp:16`, and
  `format.cpp:29` (named `TrimBlankLines` but trims all whitespace). "Add if absent" in `graph.cpp:23`,
  `sql_lexer.cpp:282`, `format.cpp:236-238` and `:424-428`.
- **Change:** one internal header `src/core/text.hpp` (`Trim`, `Lower`, `AddUnique`); `chart.cpp`
  uses `QuoteString`.
- Safe, low risk, about −45 lines.

### C2. An input's `from=<relation>.<column>` is split with `rfind('.')` three times
- **Where:** `graph.cpp:117-121`, `export.cpp:151-156`, `session.cpp:476-479`.
- **Change:** core `InputSource(const Cell&, string &relation, string &column)`.
- Safe, low risk, about −10 lines.

### C3. Pure logic in `src/functions` that belongs in `src/core` (house rule)
- **Where:** `export.cpp:442-539` (`Markdown`, `MarkdownTable`, `MarkdownChart`, `Hides`), its
  `HtmlEscape`/`ReplaceAll`/`ScriptSafe`, `CellJson` (`:176-202`), `NotebookTitle` (`:116-133`), the
  precomputed input-space enumeration (`:604-654`), `InputChoices`' `options=` split (`:139-149`);
  `session.cpp:113-137` `ReplaySql` and `:232-242` `DisplaySql` (both re-derive the last statement that
  `ChartFrom` already finds, `script.cpp:164-166`); the type-building half of `VariableSqlType`
  (`:460-513`); the `{{ }}` splice (`:341-366`).
- **Why:** none of it has a unit test, and it is half of `export.cpp`'s length.
- **Change:** move it into `src/core` (`export_markdown.cpp`, `script.cpp`) with core tests; pass a
  callback where a DuckDB lookup is needed, as `AnalyzeNotebook` already does.
- Safe, low risk, about 300 lines moved, plus tests.

### C4. Chart YAML is parsed many times
- **Where:** `ChartSelection` + `ChartFilters` parse each spec twice (`graph.cpp:138,159`);
  `CompileChart` parses, then `ChartSelectionInfo(spec_text)` parses again (`chart.cpp:953`);
  `Runner::Chart` (`session.cpp:279-285`) rebuilds the notebook-wide selection map, parsing every spec,
  for each chart it draws (quadratic per run); `export.cpp:330-338` loops all specs per variable;
  `chart.cpp:169` duplicates `ChartFilters`.
- **Change:** `Yaml&` overloads and one core `NotebookSelections(const Notebook&)`, computed once per run.
- Safe, low risk, small.

### C5. `chart.cpp` does too much
- **Where:** `Compiler` spans `:57-838`; `GrammarLayer` ~260 lines (`:339-598`); `Typed` ~200 lines
  (`:634-837`), a 14-branch if-chain with two styles of "required" check.
- **Change:** split into `chart.cpp` (channels, aggregates, `CompileChart`), `chart_grammar.cpp`,
  `chart_typed.cpp` (a table of type → handler and required keys) and `chart_selection.cpp` (C4), with
  a private `chart_internal.hpp`.
- Safe only if error messages stay word-for-word; medium risk (a large move; the feature PR adding
  reference lines and notes touches the same code, so do it last). Net ±0 lines.

### C6. `[notebook]` metadata is parsed in three places
- **Where:** `format.cpp:249-255` (timing), `export.cpp:117-124` (title), `session.cpp:170-216`
  (extensions, attach). Only `timing` is validated.
- **Change:** one core `NotebookMeta` parsed in `ParseNotebook`; readers use its fields.
- Safe if no new validation is added (that would be a Fix). Low-medium risk.

## Frontend (`frontend/`)

### F1. The input widget is written twice
- **Where:** `app.js:612-645` (`A.inputWidget`; its comment wrongly says it is shared with the viewer)
  and `viewer.js:40-71`. The viewer adds precomputed `values` and `enabled`; the slider label is
  `NB.formatNumber(v)` in the viewer and `String(v)` in the editor.
- **Change:** `NB.inputWidget(cell, onChange, {values, enabled, label})` in `render-core.js`; each
  caller passes its current label formatter, so pixels are unchanged.
- Safe, low risk, about −35 lines.

### F2. Cell framing re-derived in editor and viewer
- **Where:** query tab labels `app.js:402-405` and `viewer.js:84`; hide rules `app.js:216-219` and
  `viewer.js:110-117` (and `export.cpp:496-508`); span clamp and outline seed `app.js:517,523`,
  `viewer.js:120`; kind label three ways (`app.js:355`, `viewer.js:122`, `app-extra.js:82`).
- **Change:** `NB.queryTabs(charts)`, `NB.hidden(cell, hide)`, `NB.span(cell)`, `NB.kindName(cell)`.
- Safe, low risk, about −30 lines. (Aligning the Markdown export's hide rule with the viewer is B11.)

### F3. `app-extra.js` is guarded as optional although it always loads
- **Where:** `if (window.AppExtra)` at `app.js:146,275,446,447,583,739`; `index.html:81` always loads
  it and exports never do. Duplicate `// ---- Start ----` header (`app.js:665,682`).
- **Change:** drop the guards; keep the file as a size split with a header saying so.
- Safe, low risk, about −10 lines.

### F4. "Edited, re-analyse later" repeated
- **Where:** `app.js:88-94`, `209-212`, `295-301`, and `addChart` at `:309`.
- **Change:** one `scheduleAnalyse(ms)`.
- Safe, low risk, about −12 lines.

### F5. Ink keys depend on child position (deferred)
- **Where:** `app.js:602-605`, `viewer.js:224-228` key pieces by `id + childIndex + className`;
  HANDWRITING.md:86 documents `cell5/table`. Toggling `hide=output` inserts a stub and re-writes the
  output for no reason.
- **Change:** key by class and share one `Ink.presentCell`. Pixels unchanged but animation timing
  changes, so it is a Fix, not Safe.

## The four merged features (audited at e49f765)

Charts' reference lines, bands and notes (#5), the result cache (#7), presenting (#8) and peek (#9). None
added another copy of R1's run-every-cell loop. Earlier findings grew:

- **R2** is half done: the server's per-cell JSON moved into `RunForEditor` (`result_cache.cpp:488-521`),
  runner code in the cache file; the export still builds its own, and `RanCell` is built twice
  (`server.cpp:409-418`, `notebook_run.cpp:131-139`). R2 now moves `RunForEditor` into `session.cpp`.
- **R3** grows: `PeekState` (`peek.cpp:612`) and `CacheScanState` reused as a once-state
  (`result_cache.cpp:604-611`); another `column` lambda (`result_cache.cpp:558`); a fourth way of reading
  named parameters (`notebook_run.cpp:76-77`).
- **R6** grows: another `db:`/`@` parse (`result_cache.cpp:82-86`) and file-stem rule (`:95-98`). Still deferred.
- **C6** grows: `[notebook]` metadata is also read by `CacheEnabled` (`cache.cpp:303`) and with a regular
  expression in JS (`app.js:389`).
- **C5** is unblocked (the reference-lines feature has merged), but `chart.cpp` is now ~1300 lines.

### N1. Chart rules, bands and notes repeat their checks (Safe part)
- **Where:** "first layer with this axis" twice (`chart.cpp` `ReferenceValue` and `AxisTypeOf`); "arc has
  no axes" twice; unknown keys checked two ways (an if-chain and a set); `IsReferenceAgg`'s `pNN` test
  repeats `Agg`'s.
- **Change:** one `LayerOn(axis)`, one `NoAxes(what)`, one key set, one `IsPercentile`. Error messages
  word for word unchanged. Safe, low risk, about −30. Folded into C5.

### N2. Frontend result handling repeated (Safe)
- **Where:** "apply a result to a cell" in both `A.kept` and `A.run` (`app.js`); "drop kept results" three
  times (`A.forget`, `A.setCaching`, the render `catch`).
- **Change:** `applyResult(cell, r)` and `dropKept()`. Safe, low risk, about −20.

### N3. Smaller server and cache items (Safe)
- `SameQuery`/`SameRecursiveQuery` (`peek.cpp:278-314`) repeat the parse-one-SELECT boilerplate → one
  `ParseOneQuery`.
- Value → `(type, text, null)` written twice in `result_cache.cpp:370-386` → one helper.
- The redraw merge (`result_cache.cpp:419-431`) and the cache directory derived from a source
  (`:95-100`) need no database → core (C3), with tests.
- `NotebookJson` calls `cache.Record` while building JSON; `/api/variable` records with no cells and so
  prunes (lists a directory) on every input change. Moving the call is Safe; skipping the prune is a
  (harmless) behaviour change, deferred with the bugs.

## Dead code (grep-confirmed, all Safe)

### D1
- C++: `Statement::line` (read only by a test);
  unused `value`/`value_enc` in the pivot (`chart.cpp:685`, kept for its validation side effect); two identical branches at `export.cpp:345-354`; stale comment
  `notebook_run.cpp:174-177`.
- JS: `render-typed.js:81-82` (`columns` unused, `[].forEach(function(){})`), the copy loop at `:83`;
  `render-chart.js:210` (`numeric(...) === numeric(...) ? t : t`); unused `type` parameter of
  `NB.formatValue`; `NB.scales.color`, `NB.scales.tooltip`, `Ink.busy`; `viewer.js:20-24` `b64bytes`
  duplicates `bytes()`; `Sketch.el` identical to `NB.svg`; `.nb-btn` in `style.css:28` matches nothing.
- **Not removed** without a decision: JSON fields nothing reads (`truncated` in results,
  `payload.wasm.tables`, `open` in `/api/notebooks`, `sql` from `/api/variable`). They are part of the
  HTTP API and payload; the brief forbids changing those. `Ink.pause` is documented API, kept.

- **Corrected after the feature merge (e49f765):** `Json::Raw` now has three callers (`cache.cpp:349`,
  `result_cache.cpp:470`, `server.cpp:416`) and `Runner::Save` is called from `server.cpp` and
  `notebook_run.cpp`, so neither is dead any more. (`JsonToSqlLiteral`'s RAW case still mis-quotes; see B20.)

## Tests

### T1. Browser test server start/stop is repeated, and shells leak when a test fails
- **Where:** `test/browser/test_browser.py` starts and stops a server shell at `56-59`/`300-302`,
  `314-317`/`327-329`, `638-641`/`698-700`; only `test_ink_editor` stops it in a `finally`.
- **Change:** one `serve(...)` context manager that always stops the shell (by PID as a last resort),
  plus `ran(page)`/`saved(page)` waits. Tests and assertions unchanged.
- Safe, low risk, about −40 lines.

### T2. Small test findings
- `core_test.cpp:53-55`: a `CHECK` subsumed by the next one (merge).
- `examples/revenue.expanded.sql` is stale (still says `top_orders (line 87)`) and nothing checks it,
  although FORMAT.md:485 says it runs. Regenerate it and add a test comparing it to `notebook_sql`.
- The authoring skill's `template.sql` is never run by a test; add it to `notebook_run.test`.
- `TestParseErrors` checks only "one error per cell", not which; check the message.

## Bugs found on the way (Fix: not in the refactor)

Agreed with the parent session: after the refactor PR merges, a second PR fixes these, one commit per
bug, each with a test that fails before the fix. Severity: **medium** = wrong results, a leak, or an
accepted invalid notebook; **low** = an edge case, a wrong message, or an ignored option.

| # | Severity | Bug | Where |
|---|---|---|---|
| B1 | medium | `notebook_export(format := 'sql')` and html-wasm skip graph analysis, so a cycle `notebook_sql` rejects exports fine | `export.cpp:552,576` |
| B2 | medium | `TypeClass('INTERVAL')` is quantitative (`int` prefix); JS `isNumericType` also matches INTERVAL/POINT | `chart.cpp:855-861`, `render-core.js:109` |
| B3 | medium | `/api/variable`, `/api/page`, `/api/options`, `/api/catalog` never call `ApplyMetadata`; `attach:` missing if first | `server.cpp` |
| B4 | medium | Brush adds `window` listeners on every render and never removes them | `render-chart.js:507-508` |
| B5 | low | `RewriteVariables` turns `$"a""b"` into `getvariable('a')"b"`; the lexer handles it | `chart.cpp:992-997` |
| B6 | low | `SET VARIABLE "lim"` (quoted): graph sees a setter, `ParameterError` does not | `sql_lexer.cpp:354`, `script.cpp:169-219` |
| B7 | medium | `chart from=main.monthly` gets no edge; FORMAT says `main.` is ignored | `graph.cpp:120,132` vs `references.cpp:30-44` |
| B8 | low | Sparkline field error has an empty chart type | `chart.cpp:623,664` |
| B9 | medium | `/api/catalog` runs a query while a result is live on the same connection | `server.cpp:467-479` |
| B10 | low | Export: unknown `format` rejected only after the notebook has run; `data_format`/`interactive` unchecked; `params :=` names unchecked (run checks them); server `hide` unchecked | `export.cpp`, `server.cpp:547` |
| B11 | low | Markdown export does not apply a prose cell's own `hide=output`; the viewer does | `export.cpp:496-508` |
| B12 | low | Catalog columns are `draggable` with no `dragstart`; tooltip says click, code uses double-click | `app-extra.js:28-33` |
| B13 | low | `mark_innerRadius` read by JS but never forwarded; `interpolate`, `stack`, `cornerRadius` forwarded but never drawn | `render-chart.js:469`, `chart.cpp:589` |
| B14 | medium | html-wasm viewer re-implements `JsonToSqlLiteral`/`VariableSqlType` partially and formats `{{ }}` differently | `viewer.js:26-34,92-94,166-204` |
| B15 | low | `display=table\|none` is in the FORMAT §2 option table but nothing reads it. **Change the spec:** drop it; `hide=output` already does this | `docs/FORMAT.md` §2 |
| B16 | low | FORMAT §3 says an input's file changes only on "save as default", a control that does not exist. **Change the spec:** say moving a widget never changes the file and the default is `default=` | `docs/FORMAT.md:201` |
| B17 | low | `theme: light` appears in the FORMAT §6 metadata example; nothing reads it. **Change the spec:** remove it from the example (the theme is a viewer setting) | `docs/FORMAT.md` §6 |
| B18 | low | The `hide=` error lists `code`, `output`, `all` but not `none`, which is valid. **Change the code:** list `none` too | `format.cpp:133` |
| B19 | low | Graph view: a long-range edge's curve bends left by `depth * 2` px from x=200 and is clipped at the left edge. **Change the code:** cap the bend or widen the view box to fit the deepest edge | `app-extra.js:86` |
| B20 | low | `JsonToSqlLiteral` quotes a RAW JSON value as a string; RAW values now exist (cache) | `script.cpp:96` |
| B21 | medium | Opening the editor at `#present` runs the whole notebook, effect cells included, with no reader action; FORMAT §8 and DESIGN §17 say nothing runs until the reader asks | `app.js` present enter, `render-present.js:221-225` |
| B22 | low | `screen=` is not a layout option for the cache key, so editing it makes the cell and everything below stale | `cache.cpp:34-36` |
| B23 | low | Peek's refused-function list misses `notebook_cache_clear` (deletes files outside any transaction). Refuse every `notebook_*` but the read-only ones | `peek.cpp:419-422` |
| B24 | low | A server session loads its cache once; results written later by `notebook_run(cache := true)` never appear and cleared entries keep showing | `server.cpp:158-183` |
| B25 | low | Typed charts silently ignore `rules`, `bands`, `notes` (kpi/gauge `bands` have another shape) | `chart.cpp` typed branch |
| B26 | low | A redraw marks a stale cache entry fresh (merges charts and takes the current key) | `result_cache.cpp:419-443` |
| B27 | low | `trusted()` in peek counts cached results as runs, so a reopened page sends peeks the server refuses; `Session::ran` never clears on reload while `trusted` does | `app-peek.js:28`, `server.cpp` |
| B28 | low | Reference values: `IsNumber` accepts `inf`, `nan`, `0x10` (YAML does not); `inf` dumps as null and the rule vanishes. A note's `color` is not checked to be a scalar | `chart.cpp` |
| B29 | low | Presenting writes the requested screen to the hash, not the one shown (`#present=99` stays 99) | `render-present.js:136` |
| B30 | low | A map chart measures `color` twice (`Measure` with and without an encoding); a binned colour adds its `__bin_color` CTE twice | `chart.cpp` map branch |
| B31 | low | An input looks different in the editor and an export: a slider shows `1000` in one and `1,000` in the other, and an empty `label=` or `step=` counts as unset in one and set in the other (`NB.inputWidget`'s `exported` flag keeps both until fixed) | `render-core.js` `NB.inputWidget` |

Also: graph de-duplicates the "defined twice" error by matching the parser's message text
(`graph.cpp:209-213` against `format.cpp:421-423`); hidden code is not drawn in HTML exports but is
still in the payload (`export.cpp:176-201`), against DESIGN:434; a chart cell wobbles differently in
the editor and an export (`app.js:389` vs `viewer.js:140`); `wasm/runtime.js` rounds BIGINTs beyond
2^53 that C++ sends as text.

## Docs that disagree with the code

| # | Where | What |
|---|---|---|
| DOC1 | FORMAT §2 | Documents `display=table\|none`; nothing implements it |
| DOC2 | FORMAT §5.1 | Missing marks `circle`, `square`; aggregates `mean`, `distinct`, `aggregate:`; channel `theta`; `layer: choropleth` used but not listed |
| DOC3 | FORMAT §7 | "Three levels", lists four |
| DOC4 | DESIGN §3 | Says the graph has no cycles and Run all is file order; both changed (§11, §14) |
| DOC5 | DESIGN §4 | `notebook_save`/`checkout` also take `database :=`; export options `budget`, `page_rows`, `chrome`, `data_format`, `hide`, `interactive`, `wasm_runtime` listed only in a skill |
| DOC6 | DESIGN §8 | Frontend size (132 KB) is now ~344 KB plus fonts |
| DOC7 | HANDWRITING | "Add render-ink.js to frontend_check" already done; chart picker said excluded from the pen but is written |
| DOC8 | CLAUDE.md | `src/include/notebook/*.hpp` "for core" but `session.hpp` includes DuckDB; `notebook_cells.cpp:84` says keep in step with the README, which has no column list |
| DOC9 | DESIGN §18 | "In the editor a reload starts a new session": sessions persist per source and `ran` survives a reload |
| DOC10 | FORMAT §8 | Display-layer options list `rows`, `width`, `height`; the code also has `label`, `placeholder` |
| DOC11 | DESIGN §19 | Says every `notebook_*` function is refused by peek; not true (B23) |

Unreferenced: `docs/handwriting/*.gif` (1.2 MB) are linked from nowhere; link them from HANDWRITING.

## Rejected

- **Merge `app-extra.js` into `app.js`.** The split keeps `app.js` readable; F3 removes the pretence
  that it is optional, which is the real problem.
- **Move Vega-Lite/ECharts translation into `chart.cpp`.** It would change `render.kind` and so tab
  labels; not pixel-identical.
- **Graph view through `Sketch`.** Changes pixels; HANDWRITING records it as deliberate.
- **One list of frontend scripts for `index.html` and `export.cpp:435-437`.** Two short lists; generating
  them needs CMake work on files the CI session is changing.

## Scope of the simplification PR (agreed)

R1, R2, R3, R4, R5, C1, C2, C3, C4, F1, F2, F3, F4, D1, T1 and the T2 test additions, and docs
DOC1–DOC8. C6 only if it stays purely structural. C5 last, after the chart reference-lines feature has
merged. R6 deferred (a behaviour change for odd paths). B1–B31 in a second PR after this one. N1–N3 are in scope (N1 with C5).
