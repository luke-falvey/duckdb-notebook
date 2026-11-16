# duckdb-notebook

A notebook environment for DuckDB where the notebook is a plain `.sql` file.

**Status: early, but every part of the design has a first implementation.** A DuckDB extension
parses notebooks, works out the dependencies between cells, runs them, serves an editor in your
browser, draws charts, stores notebooks in the database, and exports them to HTML, PDF, Markdown,
SQL, data files and a live DuckDB-WASM page.

```sql
-- %% [markdown]
/*
# Revenue review
*/

-- %% input region select from=regions.name
SET VARIABLE region = 'EU';

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM orders
WHERE region = $region
GROUP BY ALL;

-- %% chart from=monthly
/*
mark: line
x: month
y: revenue
*/
```

## The idea

- **A notebook is valid SQL.** Cell boundaries, prose and chart specs live in comments, so the file
  diffs cleanly, works with SQL tooling, and is easy for people and agents to edit.
- **It runs inside your DuckDB.** A DuckDB extension serves the interface from the DuckDB process you
  already have, with the frontend built in. No network access is needed.
- **Cells are reactive.** A named query is a view; inputs and chart selections are DuckDB variables.
  Change one and everything that depends on it updates.
- **Charts are declarative.** A short YAML spec compiles to an aggregate query, so charts stay fast on
  large tables.
- **Notebooks can live in the database.** The same text is stored with version history in a table.
- **Share without DuckDB.** Export to a single self-contained HTML file, or to PDF.
- **Looks like a sketchbook.** Paper, handwriting and hand-drawn charts, after Excalidraw, in the
  editor and in every export.

## What works today

Build the extension (see [CLAUDE.md](CLAUDE.md)), then in `./build/release/duckdb`:

```sql
-- The editor: opens in your browser, served from this DuckDB process on 127.0.0.1
CALL notebook_start('examples/revenue.sql');

-- One row per cell: kind, name, what it defines and reads, and any errors
SELECT idx, name, defines, reads, depends_on, errors FROM notebook_cells('examples/revenue.sql');

-- Run every cell in a session of its own, charts included; params override inputs
SELECT name, status, row_count, error FROM notebook_run('examples/revenue.sql', params := {region: 'US'});

-- Exports: a single HTML file that needs no DuckDB, and the other formats
FROM notebook_export('examples/revenue.sql', file := 'revenue.html');
FROM notebook_export('examples/revenue.sql', file := 'results.html', hide := 'code');  -- results only
FROM notebook_export('examples/revenue.sql', file := 'revenue.html', interactive := 'precomputed');
FROM notebook_export('examples/revenue.sql', format := 'pdf');      -- needs Chrome or Chromium
FROM notebook_export('examples/revenue.sql', format := 'md');
FROM notebook_export('examples/revenue.sql', format := 'data');     -- one Parquet file per query cell

-- Notebooks stored in the database, with history; db:<name> works wherever a path does
FROM notebook_save('examples/revenue.sql', message := 'first draft');
SELECT name, version, saved_at FROM _notebook.versions;
FROM notebook_checkout('revenue', file := 'revenue_copy.sql');

-- What a chart spec compiles to
FROM notebook_chart_query('mark: bar
x: region
y: {field: amount, agg: sum}', 'orders');
```

For an export that runs real SQL in the reader's browser, build the DuckDB-WASM runtime once (needs
Node) and pass it in:

```sh
sh wasm/build.sh
```

```sql
FROM notebook_export('examples/revenue.sql', format := 'html-wasm', wasm_runtime := 'wasm/dist');
```

[examples/charts.sql](examples/charts.sql) shows every chart type.

Not yet built: the result cache, a CodeMirror editor (the editor is a plain text area with
completion), and charting libraries for zoomable maps and GPU-scale networks (the built-in SVG
renderers draw every chart type, simply). [docs/DESIGN.md](docs/DESIGN.md) §9 and §12 have the
details.

## Documents

- [docs/DESIGN.md](docs/DESIGN.md): architecture, execution model, storage, visualisation, export,
  risks and roadmap.
- [docs/FORMAT.md](docs/FORMAT.md): the notebook file format and chart spec.
- [docs/RESEARCH.md](docs/RESEARCH.md): the survey of existing tools behind the design, with sources
  and a list of what is not yet verified.
- [CLAUDE.md](CLAUDE.md): how the code is organised, how to build and test, and conventions.
- [examples/revenue.sql](examples/revenue.sql): an example notebook, and
  [examples/charts.sql](examples/charts.sql), every chart type.
  [examples/revenue.expanded.sql](examples/revenue.expanded.sql) is the same notebook as a plain
  script that runs in any DuckDB 2.0 client.
