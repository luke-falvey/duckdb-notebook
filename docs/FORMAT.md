# Notebook format, version 1 (draft)

A notebook is a UTF-8 SQL file. Every byte of it is either a DuckDB statement or a SQL comment, so
editors, formatters, linters and DuckDB itself treat it as ordinary SQL. The notebook structure lives
in comments.

The file holds source only. Results, chart images and UI state are never written to it.

See [`examples/revenue.sql`](../examples/revenue.sql) for a complete notebook.

## 1. Cells

A line that starts with `-- %%` begins a cell. The cell runs to the next marker or the end of the file.
Text before the first marker is ignored by the notebook (use it for a licence header).

```
-- %% <head>
<body>
```

The first token of the head decides the cell kind:

| Head starts with | Kind | Body |
|---|---|---|
| `[notebook]` | Notebook metadata (at most one, first cell) | One block comment containing YAML |
| `[markdown]` | Prose | One block comment containing Markdown |
| `input` | Input widget | One `SET VARIABLE` statement |
| `chart` | Visualisation of another cell or table | One block comment containing a YAML chart spec |
| anything else, or nothing | SQL | DuckDB statements, optionally followed by `/* chart */` comments (§2) |

After the kind comes an optional name and then `key=value` options. Quote values that contain spaces:
`label="Minimum order value"`.

```sql
-- %% monthly                       SQL cell named "monthly"
-- %%                               unnamed SQL cell
-- %% orders run=manual             SQL cell with an option
-- %% input region select from=regions.name
-- %% chart revenue_trend from=monthly
```

### Names

- A name matches `[a-z_][a-z0-9_]*`. `input` and `chart` are reserved.
- Names are unique within a notebook. A duplicate is an error reported on both cells.
- **The name is the cell's identity.** There are no generated IDs in the file. Caches, layout state and
  links between cells all key on the name. Unnamed cells are identified by position and so lose their
  cached output when cells above them are added or removed; name any cell you care about.
- Renaming a cell is an ordinary edit. The UI offers "rename and update references".

### Options common to all cells

| Option | Values | Meaning |
|---|---|---|
| `hide` | `code`, `output`, `all`, `none` | What to hide in reading view and exports. The editor collapses the hidden part to a line that shows it on click |
| `span` | `1`–`12` | Width on a 12-column grid in reading view and exports; consecutive cells whose spans fit share a row. Default `12` |
| `rows` | `1`–`10000` | Rows per page of the cell's result table, in the editor and in exports. Default: the reader's own setting, 25 unless changed |

`hide` and `rows` are checked: any other value is an error on the cell. In the editor, the
code and output switches in a cell's header and the page-size picker under a table write these
options into the marker line, so the choice is saved with the notebook and reviewed like any edit.

## 2. SQL cells

The body is one or more DuckDB statements.

**Query cells.** If the cell is named and its last statement returns rows (`SELECT`, `FROM …`,
`VALUES`, `PIVOT`, `SUMMARIZE`, `DESCRIBE`, …), the runner registers that statement as a temporary view
with the cell's name and displays `SELECT * FROM <name>`. Later cells refer to the result by name:

```sql
-- %% filtered
SELECT * FROM orders WHERE region = $region;

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM filtered
GROUP BY ALL;
```

The file keeps the bare `SELECT`. The view wrapper is added at run time.

**Charts.** A query cell draws its result as a table. To chart it instead, end the cell with one or
more block comments that start with `chart`, each holding a chart spec (§5):

```sql
-- %% monthly
SELECT date_trunc('month', ts) AS month, category, sum(amount) AS revenue
FROM filtered
GROUP BY ALL;
/* chart
title: Monthly revenue
mark: line
x: month
y: revenue
color: category
*/
/* chart
mark: bar
x: {field: revenue, agg: sum}
y: category
*/
```

The query defines the data and the charts are views of it: a reader sees one tab per chart, in file
order, and a last tab with the table. The first chart is shown until another tab is picked. A chart
comment is a comment like any other, so the file still runs as plain SQL, and the expanded script (§7)
leaves the charts out.

- Only comments after the cell's last statement are charts. A `/* chart */` comment before or inside
  the query is an ordinary comment, with a warning.
- Only a cell whose last statement returns rows can have charts. Its name is optional: the charts of an
  unnamed cell read its query directly.
- The spec is the comment's text after `chart`, on the following lines or on the same line
  (`/* chart mark: bar */`). Leading blank lines and trailing whitespace are dropped.
- The cell's `width` and `height` options size all of its charts.
- A chart's `select` variable is defined by its cell (§5.2). The cell's own SQL cannot read it: that
  would make the query depend on a chart of itself. Its charts may `filter` on it, and on any other
  selection; those filters redraw the charts without running the query.
- Changing a spec redraws that cell's charts; the query does not run again.

A chart cell (§5) draws a chart of another cell or of a table, for when one result feeds several
places or a chart reads a table that no cell defines.

**Effect cells.** A cell containing anything other than queries (`CREATE`, `INSERT`, `UPDATE`,
`DELETE`, `ATTACH`, `COPY`, `INSTALL`, `LOAD`, `SET`, …) is an effect cell. It defines whatever its
statements create. Effect cells run only when asked (see [DESIGN.md §3](DESIGN.md#3-execution-model)).

| Option | Values | Meaning |
|---|---|---|
| `run` | `auto`, `manual` | Default `auto` for query cells, `manual` for effect cells |
| `materialize` | `view`, `table` | `view` (default) keeps the result lazy. `table` stores it as a temp table: use for slow queries that many cells read |
| `display` | `table`, `none` | Whether to show the result grid |

### Ordering

SQL and input cells may only refer to cells **above** them. A forward reference is an error. This keeps
the file readable top to bottom and makes the expanded script (§7) a straight translation. Markdown and
chart cells only display things, so they may refer to any cell.

What a cell defines and reads is found by DuckDB's parser, without running anything:

| Cell | Defines | Reads |
|---|---|---|
| SQL | Its name, if it is a named query cell; tables, views, schemas, macros, sequences and types it creates; databases it attaches; variables it sets; its charts' `select: {as: …}` variables | Tables and views in its SQL that are not CTEs, including the targets of `INSERT`, `UPDATE`, `DELETE`, `COPY` and `DROP`; `$name` and `getvariable('name')`; the selections in its charts' `filter`, which add no dependency |
| Input | Its variable | The cell in `from=<cell>.<column>`; anything its default reads |
| Markdown | Nothing | What its `{{ }}` expressions read |
| Chart | Its `select: {as: …}` variable | Its `from`; the selections in its `filter` |

Names compare case-insensitively, and `main.` and `temp.` qualifiers are ignored, so `main.orders`
reads the cell that creates `orders`. A qualified name such as `wh.orders` depends on the cell that
creates it or, failing that, on the cell that attached `wh` or created schema `wh`. A name that no cell
defines is assumed to be a table or view in the database and needs no cell.

These are errors, reported on every cell involved:

- A name defined by more than one cell.
- An SQL or input cell reading something defined further down. A query cell's chart filters are
  exempt: a selection may come from any chart.
- A dependency cycle. With the rule above, a cycle can only pass through a chart, for example a chart
  drawn `from` a cell that reads the chart's own selection.
- A query cell whose SQL reads the selection of one of its own charts.
- Chart comments on a cell whose last statement does not return rows.

These are warnings, which do not stop a notebook from running:

- A variable that some cell reads but no cell sets. It reads as `NULL` (see Parameters).
- A `/* chart */` comment that is not at the end of its cell, which is not drawn.
- SQL the parser cannot read, such as a syntax error. Its dependencies are unknown, and running it
  reports the error on that cell.

Calls to macros, and uses of sequences and types, are not yet counted as reads.

### Parameters

Parameters are DuckDB session variables, read with `$name`:

```sql
WHERE region = $region AND amount >= $min_amount
```

No notebook-specific syntax is involved. `$name` is DuckDB's own shorthand for `getvariable('name')`,
[introduced in DuckDB 2.0](https://duckdb.org/docs/2.0/sql/statements/set_variable.html), so notebooks
need DuckDB 2.0 or later. The two differ when the variable is unset: `getvariable` returns `NULL`,
while `$name` raises a missing-parameter error.

To keep that error away from cells that read a chart selection nobody has made yet, the runner sets
every variable that some cell reads, but no input cell or `SET VARIABLE` statement defines, to `NULL`
before the first cell runs. Views pick up a changed variable on their next
read, so changing an input does not require redefining the cells that use it.

## 3. Input cells

```sql
-- %% input <name> <widget> [options]
SET VARIABLE <name> = <default>;
```

The body is a real statement that sets the default, so the file still works outside the notebook. When
someone moves the widget, the runner sets the variable for that session and re-runs the cells that read
it. The file changes only if they choose "save as default".

| Widget | Variable type | Options |
|---|---|---|
| `text` | `VARCHAR` | `placeholder` |
| `number` | numeric | `min`, `max`, `step` |
| `slider` | numeric | `min`, `max`, `step` |
| `checkbox` | `BOOLEAN` | |
| `date` | `DATE` | `min`, `max` |
| `daterange` | `STRUCT(lo DATE, hi DATE)` | `min`, `max` |
| `select` | scalar | `options=a,b,c` or `from=<cell>.<column>` |
| `multiselect` | `LIST` | `options=…` or `from=<cell>.<column>` |

All widgets accept `label`. `from=` makes the input depend on that cell.

A value set through a widget gets the widget's type: `VARCHAR` for `text`, `DOUBLE` for `number` and
`slider`, `BOOLEAN` for `checkbox`, `DATE` for `date` and for both fields of `daterange`
(`STRUCT(lo DATE, hi DATE)`). A `select` or `multiselect` with `from=` takes the type of that column
(as a list for `multiselect`); with `options=` the values are text.

## 4. Markdown cells

```sql
-- %% [markdown]
/*
# Heading

Total revenue in **{{ $region }}**: {{ (SELECT sum(revenue) FROM monthly) }}
*/
```

- CommonMark plus tables, footnotes, KaTeX maths (`$…$` / `$$…$$` are maths here, not parameters) and
  Mermaid diagrams in ```` ```mermaid ```` fences. The built-in renderer covers headings, emphasis,
  code, links, lists, quotes, rules, tables and fenced code. No KaTeX or Mermaid library is bundled
  yet: maths is shown as written, Mermaid fences as code, and footnotes as plain text.
- `{{ expr }}` is a DuckDB scalar expression. It is evaluated as `SELECT (expr)` and the value is
  formatted inline (`NULL` shows as `NULL`). Dependencies are found the same way as for SQL cells, and
  the expressions are evaluated once the cells they read have run, so a summary may sit above them.
  The closing `}}` is looked for in SQL, so `}}` inside a string or a struct literal does not end the
  expression. An unclosed `{{` or an empty `{{ }}` is an error.
- Raw HTML is escaped and shown as text. Scripts never run.
- DuckDB block comments nest, so `/*` and `*/` inside the prose must be balanced. Write a literal
  `*/` as `*\/`.

## 5. Charts

A chart spec is written in one of two places:

```sql
-- %% monthly
SELECT ...;
/* chart
<YAML spec>
*/

-- %% chart [name] from=<relation> [options]
/*
<YAML spec>
*/
```

At the end of a query cell (§2) the chart draws that cell's result. A chart cell draws `from`, which
names a cell, table or view. The spec says how to turn that relation into a picture. The runner
compiles each spec into one aggregate SQL query over the relation plus a small render description, so
the browser receives only the rows the picture needs. Below, `from` means the relation a chart draws,
whichever way it is written.

### 5.1 Grammar charts

For anything that is marks on axes:

```yaml
title: Monthly revenue by category
mark: line
x: month
y: {field: revenue, agg: sum, format: "$,.0f", title: Revenue}
color: category
```

**`mark`**: `line`, `area`, `bar`, `point`, `rect`, `rule`, `tick`, `text`, `arc`, `boxplot`,
`histogram`, `density`, `hexbin`.

**Channels**: `x`, `y`, `x2`, `y2`, `color`, `size`, `opacity`, `shape`, `text`, `tooltip`, `order`,
and `facet` (`{row: …, col: …}`) for small multiples. A channel is a column name or a mapping:

| Key | Meaning |
|---|---|
| `field` | Column in `from` |
| `agg` | `sum`, `avg`, `min`, `max`, `count`, `count_distinct`, `median`, `p05`…`p99` |
| `bin` | `true` or `{step: …}` / `{maxbins: …}` |
| `timeunit` | Truncates: `year`, `quarter`, `month`, `week`, `date`, `hour`, `minute`, `second`. Extracts a cyclic part: `monthofyear`, `dayofmonth`, `dayofweek` (or `day`, 0 = Sunday), `hours` (or `hourofday`), `minutes` |
| `sort` | `asc`, `desc`, or a channel with optional `-` (`-x`) |
| `type` | `quantitative`, `temporal`, `ordinal`, `nominal`. Inferred from the column type if omitted |
| `scale` | `{type: log}`, `{domain: [0, 100]}`, `{zero: true}` (include zero; bars and areas always do unless `{zero: false}`). On a categorical `color`: `{domain: [PASS, WARNING, ERROR], range: ["#2f9e44", "#f08c00", "#e03131"]}` fixes the order and colour of each value |
| `scheme` | Named colour scheme |
| `format`, `title` | d3-format string; axis or legend title |

Channels without `agg` become `GROUP BY` keys when any channel has one. A bare name that is not a
column of `from` is a constant (`color: red`); `{value: …}` says so explicitly. `arc` (pie and donut)
takes its angle from `theta` (or `y`) and its slices from `color`.

Spec-level keys: `title`; `limit` (rows a query may return, default 10,000); `sample: <n>`;
`facet: {row: …, col: …}`.

**Layers.** `layers:` is a list of specs drawn in one frame, sharing `from` and scales:

```yaml
layers:
  - {mark: area, x: month, y: {field: revenue, agg: sum}, opacity: 0.3}
  - {mark: rule, y: {field: revenue, agg: avg}, color: red}
```

**Large data.** The compiler never ships raw rows for a mark that would overplot: `line` over a
continuous x is reduced to at most four points per horizontal pixel (M4) once `from` has more than four
rows per pixel, and `point`, `rect` and `tick` over two numeric axes are binned to a grid of 4-pixel
cells once `from` has more than 5,000 rows (`hexbin` always is), each cell drawn by its row count.
`histogram` bins `x` and counts; `density` bins `x` and normalises the counts. `sample: 5000` asks for
a row sample instead.

### 5.2 Selections

```yaml
select: {as: picked_months, type: interval, on: x}
```

A selection writes the user's brush or click into the session variable named by `as`. Cells that read
`$picked_months` re-run. With no selection the variable is `NULL`.

| `type` | Variable value |
|---|---|
| `interval` on one channel | `STRUCT(lo, hi)` |
| `interval` on `[x, y]` | `STRUCT(x STRUCT(lo, hi), y STRUCT(lo, hi))` |
| `point` | `LIST` of the selected values of the `on` channel's field |

```sql
WHERE $picked_months IS NULL
   OR ts BETWEEN $picked_months.lo AND $picked_months.hi
```

A chart can take the filter without any SQL: `filter: picked_months` in another chart's spec applies
the selection to that chart's `from` (cross-filtering), matching the selection's fields by name.
`filter` takes one selection or a list, `[a, b]`. `select` and `filter` are read from the top level of
the spec only.

The values have the type of the selected field. A brush on a `DATE` field gives `TIMESTAMP` bounds,
because a brush ends between days. In the UI, dragging on a chart with an `interval` selection
brushes, a click clears it; clicking a mark of a `point` selection selects it (Shift adds to it).

### 5.3 Typed charts

Charts that are not marks on axes use `type:` with their own short vocabulary:

| `type` | Required keys | Notes |
|---|---|---|
| `kpi` | `value` | `label`, `compare` (delta against another channel), `sparkline: {x, y}` |
| `table` | | `columns`, per-column `format: {col: spec}`, `bar` and `heat` column decorations |
| `pivot` | `rows`, `columns`, `values` | `heat: true` shading |
| `map` | `geometry` or `lat` + `lon` | `layer: point \| heat \| hex \| arc` (`arc` needs `lat2`, `lon2`), `color`, `size`. `geometry` needs the `spatial` extension; polygons draw as a choropleth. No basemap: tiles are not available offline |
| `sankey` | `source`, `target`, `value` | |
| `chord` | `source`, `target`, `value` | |
| `treemap`, `sunburst`, `icicle` | `path` (list of columns), `value` | |
| `network` | `source`, `target` | `weight` |
| `funnel` | `stage`, `value` | |
| `candlestick` | `x`, `open`, `high`, `low`, `close` | |
| `calendar` | `date`, `value` | Calendar heatmap |
| `gantt` | `task`, `start`, `end` | |
| `gauge` | `value` | `min`, `max`, `bands: [{from, to, color}]`, `label` |

```yaml
type: kpi
value: {field: revenue, agg: sum, format: "$,.0f"}
label: Revenue
sparkline: {x: month, y: revenue}
```

```yaml
type: map
geometry: geom
layer: choropleth
color: {field: population, scheme: viridis}
```

### 5.4 Raw specs

When the built-in vocabulary runs out, embed a native spec. The rows of `from` are supplied as the
dataset (up to `limit`, default 5,000); no pushdown or downsampling is applied, so aggregate in SQL
first. No Vega or ECharts library is bundled: a Vega-Lite spec's `mark` and `encoding` (`x`, `y`,
`color`, `size`, `text`, `theta`) are drawn by the built-in renderer, as is an ECharts `series` of
type `line`, `bar`, `scatter` or `pie` with `encode`; anything else is shown as a table.

```yaml
type: vega-lite
spec:
  mark: {type: bar, cornerRadius: 4}
  encoding:
    x: {field: category, type: nominal}
    y: {field: revenue, type: quantitative}
```

`type: echarts` works the same way with an ECharts `option` object under `spec`.

## 6. Notebook metadata

```sql
-- %% [notebook]
/*
title: Revenue review
format: 1
attach:
  - {path: warehouse.duckdb, as: wh, read_only: true}
extensions: [spatial]
theme: light
timing: false
*/
```

`format` is the version of this specification. `timing` (`true` by default) shows how long each cell
took to run, in the bottom right corner of its box in the editor; `timing: false` hides it. Any other
value is an error. The editor's gear menu has a switch that writes this line. `attach` and `extensions` are applied before the first
cell runs (`LOAD` for each extension, `ATTACH IF NOT EXISTS` for each database). Paths are relative to
the notebook file.

## 7. Running without the notebook

Three levels, from least to most capable:

1. **The file as-is in any DuckDB client.** It always parses. It runs to completion only if cells do
   not refer to each other by cell name, because the view wrapper is added by the runner. Notebooks
   written in "explicit" style (`CREATE OR REPLACE TEMP VIEW monthly AS …` in the cell body) run
   unchanged.
2. **The expanded script.** `SELECT sql FROM notebook_sql('revenue.sql')` returns the same notebook
   with the wrappers spelled out and undefined variables initialised. It runs anywhere with no
   extension. [`examples/revenue.expanded.sql`](../examples/revenue.expanded.sql) is that output for
   the example.
3. **Headless with the extension.** `FROM notebook_run('revenue.sql')` runs every cell in a session
   of its own and returns one row per cell with its status, in the order the cells ran. Tables the
   notebook creates remain; its views and variables do not reach the caller's connection. Markdown
   cells come back rendered, with each `{{ }}` replaced by its value.
   `notebook_run('revenue.sql', params := {region: 'US'})` runs with other values: a parameter
   replaces an input's default, or sets a variable that nothing in the notebook sets, such as a chart
   selection. A variable set by an SQL cell cannot be a parameter, because the cell would overwrite it.
4. **Exported.** `notebook_export('revenue.sql', file := 'revenue.html')` writes a page that needs
   neither DuckDB nor a server; see [DESIGN.md §7](DESIGN.md#7-export) for the formats.
   `hide := 'code'` (or `'output'`, `'all'`) hides that part of every cell on top of each cell's own
   `hide=`; prose and inputs are always kept. An exported table pages through the rows the export
   includes (`page_rows`, at least each cell's `rows=`).

## 8. Sidecar cache

*Not implemented yet.* The editor shows results from the current session only.

Outputs are cached next to the notebook in `.duckdb-notebook/`, which belongs in `.gitignore`:

```
.duckdb-notebook/
  revenue/
    monthly.<hash>.parquet        result page shown in the grid
    revenue_trend.<hash>.arrow    data behind a chart
    state.json                    last input values, collapsed cells
```

`<hash>` covers the cell's source, the hashes of the cells it reads, and the values of the variables
it reads. A cell whose hash has a cache file opens instantly with no query. The cache is disposable:
deleting it loses nothing but time.

Teams who want reviewable outputs can commit an export (HTML or Markdown) instead of the cache.

## 9. Storage inside a database

The same text can be stored in a DuckDB database: see
[DESIGN.md §5](DESIGN.md#5-notebooks-inside-the-database). The stored `content` is byte-identical to
the file, so this specification is the only format.

## 10. Verified against DuckDB 2.0.0

These properties were checked with the DuckDB 2.0.0 CLI and Python client while writing this draft:

- `-- %%` lines and block-comment bodies (including `;` inside them) are ignored by the parser.
- Block comments nest; an unbalanced `*/` in a body is a parse error.
- `$name` resolves to `SET VARIABLE name` when no value is bound, in a plain query and inside a view,
  and a view reflects a later `SET VARIABLE`. This is documented 2.0 behaviour.
- `STRUCT` and `LIST` variables work as specified: `$brush.lo`, `'EU' IN $regions`.
- Variables and temporary views are private to a connection.
- `examples/revenue.sql` parses; run as-is it stops at the first cross-cell reference, as §7 says.
  `examples/revenue.expanded.sql` runs to completion.
