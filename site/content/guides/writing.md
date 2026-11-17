# Writing a notebook

A notebook is a UTF-8 `.sql` file. Each line that starts with `-- %%` begins a cell, which runs to
the next such line. Everything is SQL or a comment, so the file diffs, lints and runs like any
script. This page explains the parts by task; the [format reference](../reference/format.md) is the
full specification.

## The cell kinds

The first word after `-- %%` decides the kind of cell:

```sql
-- %% [notebook]                    metadata: title, attached databases, extensions (first cell only)
-- %% [markdown]                    prose in one /* ... */ block, with {{ expr }} values
-- %% input <name> <widget> ...     one SET VARIABLE <name> = <default>;
-- %% <name> ...                    SQL; a named query becomes a view called <name>
-- %%                               unnamed SQL
-- %% chart <name> from=<rel> ...   a chart of another cell or of a table
```

After the kind come an optional name and `key=value` options. Quote values that contain spaces:
`label="Minimum order value"`. Text before the first marker is ignored, which makes room for a
licence header.

## SQL cells: queries and effects

A **query cell** is one whose last statement returns rows: `SELECT`, `FROM …`, `VALUES`, `PIVOT`,
`SUMMARIZE`, `DESCRIBE` and so on. If it is named, the runner registers that statement as a temporary
view with the cell's name, and later cells read it by name:

```sql
-- %% filtered
SELECT * FROM orders WHERE region = $region;

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM filtered
GROUP BY ALL;
```

The file keeps the bare `SELECT`; the view is added when it runs. A query cell shows its result as a
table, or as [charts](charts.md) if you add `/* chart */` comments after it.

An **effect cell** contains anything else: `CREATE`, `INSERT`, `COPY`, `ATTACH`, `INSTALL`, `SET`
and so on. It defines whatever its statements create. Effect cells **run only when asked** (**▶ Run
all**, or running the cell itself), never because something above them changed, so a cell that loads
a big file does not reload it every time you edit something:

```sql
-- %% orders run=manual
CREATE OR REPLACE TABLE orders AS FROM 'orders.parquet';
```

| Option | Values | Default |
|---|---|---|
| `run` | `auto`, `manual` | `auto` for query cells, `manual` for effect cells. An `auto` cell runs again when a cell or variable it reads changes |
| `materialize` | `view`, `table` | `view`. `table` stores the result as a temp table: use it for a slow query that many cells read |
| `display` | `table`, `none` | Whether to show the result grid |

## Names

- A name matches `[a-z_][a-z0-9_]*`. `input` and `chart` are reserved.
- Names are unique in a notebook. A duplicate is an error on both cells.
- **The name is the cell's identity.** There are no generated IDs in the file. Name every cell that
  another cell reads, and name cells after what they hold (`monthly`, `top_orders`), not `q1`.

## Dependencies are found for you

DuckDB's parser reads each cell to find what it defines and what it reads; nothing runs. You never
declare dependencies. One rule keeps the file readable top to bottom:

**SQL and input cells may only read cells above them.** A forward reference is an error. Markdown and
chart cells only display things, so they may read any cell; a summary at the top of the page can quote
a result computed further down.

```sql
--8<-- "site/outputs/revenue_deps.sql"
```

```text
--8<-- "site/outputs/revenue_deps.txt"
```

`depends_on` lists the indexes of the cells each cell reads. A name that no cell defines (a table
already in your database, for example) is assumed to exist and needs no cell. The editor's **Graph**
view draws the same thing.

These are **errors**, reported on every cell involved: a name defined by two cells, a forward
reference, a dependency cycle, a query that reads a selection of its own chart, and chart comments on
a cell that does not end in a query. These are **warnings**, which do not stop a run: a variable that
something reads but nothing sets (it reads as `NULL`), a `/* chart */` comment that is not at the end
of its cell, and SQL the parser cannot read.

## Prose with live values

```sql
-- %% [markdown]
/*
# Revenue review

Total revenue in **{{ $region }}**: {{ (SELECT format('${:,.0f}', sum(revenue)) FROM monthly) }}
*/
```

- The body is one block comment holding Markdown: headings, emphasis, code, links, lists, quotes,
  rules, tables and fenced code. Raw HTML is shown as text, and scripts never run.
- `{{ expr }}` is a DuckDB scalar expression, evaluated as `SELECT (expr)` once the cells it reads
  have run. The value is shown inline with a highlighter mark. Format numbers in SQL:
  `format('{:,}', n)`, `format('${:,.0f}', x)`, `strftime(d, '%B %Y')`.
- DuckDB block comments nest, so `/*` and `*/` inside the prose must be balanced. Write a literal
  `*/` as `*\/`.
- Maths (`$…$`), Mermaid diagrams and footnotes are part of the format but not drawn yet: maths shows
  as written, Mermaid as code.

## Metadata

The optional first cell holds YAML:

```sql
-- %% [notebook]
/*
title: Revenue review
format: 1
attach:
  - {path: warehouse.duckdb, as: wh, read_only: true}
extensions: [spatial]
timing: false
cache: false
*/
```

`title` names the notebook in the editor and in exports. `attach` and `extensions` are applied before
the first cell runs (`ATTACH IF NOT EXISTS` and `LOAD`); paths are relative to the notebook file.
`timing: false` hides how long each cell took. `cache: false` stops the editor
[keeping results](cache.md) between sessions. `format` is the format version, `1`.

## Options every cell takes

| Option | Values | Effect |
|---|---|---|
| `hide` | `code`, `output`, `all`, `none` | What to hide in the reading view and in exports. The editor folds the hidden part to a line |
| `span` | `1`–`12` | Width on a 12-column grid in the reading view and exports. Consecutive cells whose spans fit share a row |
| `rows` | `1`–`10000` | Rows per page of the result table. Default: the reader's own setting, 25 unless changed |
| `screen` | `new`, `join`, `skip` | Where the cell goes when the notebook is [presented](reading.md#presenting) |

The editor writes these for you: the code and output switches on a cell write `hide=`, and the
page-size picker under a table writes `rows=`. They are saved with the notebook. See
[reading and presenting](reading.md).

## Running the file without the extension

A notebook always parses as SQL. Run as-is, it stops at the first cell that reads another cell by
name, because the views are added by the runner. Three ways around that:

- `SELECT sql FROM notebook_sql('report.sql')` returns the notebook as a plain script with the views
  spelled out. It runs in any DuckDB 2.0 client with no extension.
- Write cells in "explicit" style, `CREATE OR REPLACE TEMP VIEW monthly AS …`, and the file runs
  unchanged.
- [Run it headless](headless.md) with `notebook_run`.

## Check your work

```sql
SELECT idx, kind, name, errors, warnings FROM notebook_cells('report.sql')
WHERE len(errors) + len(warnings) > 0;

SELECT idx, name, error FROM notebook_run('report.sql') WHERE status = 'error';
```

The first finds problems without running anything; the second runs every cell and lists the ones
that fail.

## Habits that help

- One idea per cell. Filter once (`filtered`) and build everything on it, so one input changes every
  chart.
- Keep loading in one `run=manual` effect cell.
- Put comments explaining intent above a query: they stay above the generated view.
- Aggregate in SQL or in the chart spec. Tables page 25 rows at a time and charts compile to aggregate
  queries, so there is no need to pull raw rows into view.
