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
| `chart` | Visualisation | One block comment containing a YAML chart spec |
| anything else, or nothing | SQL | DuckDB statements |

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
| `hide` | `code`, `output`, `all` | What to hide in reading view and exports |
| `span` | `1`–`12` | Width on a 12-column grid in reading view and exports; consecutive cells whose spans fit share a row. Default `12` |

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

## 4. Markdown cells

```sql
-- %% [markdown]
/*
# Heading

Total revenue in **{{ $region }}**: {{ (SELECT sum(revenue) FROM monthly) }}
*/
```

- CommonMark plus tables, footnotes, KaTeX maths (`$…$` / `$$…$$` are maths here, not parameters) and
  Mermaid diagrams in ```` ```mermaid ```` fences.
- `{{ expr }}` is a DuckDB scalar expression. It is evaluated as `SELECT (expr)` and the value is
  formatted inline. Dependencies are found the same way as for SQL cells.
- Raw HTML is sanitised. Scripts never run.
- DuckDB block comments nest, so `/*` and `*/` inside the prose must be balanced. Write a literal
  `*/` as `*\/`.

## 5. Chart cells

```sql
-- %% chart [name] from=<relation> [options]
/*
<YAML spec>
*/
```

`from` names a cell, table or view. The spec says how to turn that relation into a picture. The runner
compiles each spec into one aggregate SQL query over `from` plus a small render description, so the
browser receives only the rows the picture needs.

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
| `timeunit` | `year`, `quarter`, `month`, `week`, `day`, `hours`, … Truncates, or extracts a cyclic part |
| `sort` | `asc`, `desc`, or a channel with optional `-` (`-x`) |
| `type` | `quantitative`, `temporal`, `ordinal`, `nominal`. Inferred from the column type if omitted |
| `scale` | `{type: log}`, `{domain: [0, 100]}`, `{zero: false}` |
| `scheme` | Named colour scheme |
| `format`, `title` | d3-format string; axis or legend title |

Channels without `agg` become `GROUP BY` keys when any channel has one.

**Layers.** `layers:` is a list of specs drawn in one frame, sharing `from` and scales:

```yaml
layers:
  - {mark: area, x: month, y: {field: revenue, agg: sum}, opacity: 0.3}
  - {mark: rule, y: {field: revenue, agg: avg}, color: red}
```

**Large data.** The compiler never ships raw rows for a mark that would overplot: `line` over a
continuous x is reduced to at most four points per horizontal pixel (M4), and `point`, `rect`, `hexbin`
and `density` are binned to the plot's pixel grid. `sample: 5000` asks for a row sample instead.

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
the selection to that chart's `from` (cross-filtering).

### 5.3 Typed charts

Charts that are not marks on axes use `type:` with their own short vocabulary:

| `type` | Required keys | Notes |
|---|---|---|
| `kpi` | `value` | `label`, `compare` (delta against another channel), `sparkline: {x, y}` |
| `table` | | `columns`, per-column `format`, `bar` and `heat` cell decorations |
| `pivot` | `rows`, `columns`, `values` | Subtotals, heat shading |
| `map` | `geometry` or `lat` + `lon` | `layer: point \| heat \| hex \| choropleth \| arc \| path`, `color`, `size`, `basemap` |
| `sankey` | `source`, `target`, `value` | |
| `chord` | `source`, `target`, `value` | |
| `treemap`, `sunburst`, `icicle` | `path` (list of columns), `value` | |
| `network` | `source`, `target` | `weight`, `node_color`, `node_size` |
| `funnel` | `stage`, `value` | |
| `candlestick` | `x`, `open`, `high`, `low`, `close` | |
| `calendar` | `date`, `value` | Calendar heatmap |
| `gantt` | `task`, `start`, `end` | |
| `gauge` | `value` | `min`, `max`, `bands` |

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
dataset; no pushdown or downsampling is applied, so aggregate in SQL first.

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
*/
```

`format` is the version of this specification. `attach` and `extensions` are applied before the first
cell runs. Paths are relative to the notebook file.

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
   of its own and returns one row per cell with its status. Tables the notebook creates remain; its
   views and variables do not reach the caller's connection.

## 8. Sidecar cache

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
