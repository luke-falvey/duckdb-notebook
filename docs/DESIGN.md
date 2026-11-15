# DuckDB Notebook: design

Status: draft for review. Nothing here is implemented yet.

Companion documents: [FORMAT.md](FORMAT.md) specifies the file format; [RESEARCH.md](RESEARCH.md)
records the survey this design draws on and what could not be verified.

## Summary

A notebook is a plain `.sql` file. A DuckDB extension turns your own DuckDB process into the notebook
server and carries the whole user interface inside it, so it works offline. Cells are SQL, prose,
inputs and declarative charts; they form a dependency graph and re-run when something upstream
changes. The same text can be saved, with history, in a table inside any DuckDB database. A notebook
exports to one HTML file that opens anywhere without DuckDB, and to PDF.

```sql
INSTALL notebook FROM community;
LOAD notebook;
CALL notebook_start('reports/revenue.sql');   -- opens the browser
```

| Decision | Choice | Main reason |
|---|---|---|
| File format | `.sql` with `-- %%` cell markers | Valid SQL; diffs, tools and agents handle it natively |
| Runtime | DuckDB extension with embedded HTTP server | Runs in the user's process with their data, secrets and version |
| Frontend | Open source, compiled into the extension | Offline; nothing fetched at run time |
| Cell kinds | SQL, markdown, input, chart | One language to analyse, so reactivity and export stay simple |
| Execution | Reactive graph from parsed SQL | No hidden state; inputs and selections are just variables |
| Charts | Own small YAML spec, compiled to SQL by the extension | Diffable; big data stays in DuckDB; exports need no browser |
| In-database storage | Same text in a versions table | One format, lossless both ways |
| Export | Static single-file HTML by default; PDF via print | Shareable with no DuckDB and no server |

## 1. Goals and non-goals

**Goals**

1. Work entirely offline with every component open source.
2. SQL first: a person who knows DuckDB SQL can do everything, including charts and interactivity.
3. A notebook is a text file that reviews well in a pull request and that an AI agent can edit safely.
4. A notebook can also live inside the database it describes.
5. A wide range of chart types, usable on tables too large to send to a browser.
6. Share a notebook with someone who has neither DuckDB nor this extension.

**Non-goals for the first version**

- Python, JavaScript or R cells, and third-party widget plugins.
- Simultaneous multi-user editing.
- A hosted service, scheduling service or permissions model.

**What prompted these goals.** The official DuckDB UI is the closest existing tool, and its open issues
are the gap this project fills: most of its frontend is closed and fetched from `ui.duckdb.org` on each
load, it does not work offline, it has no charts, and notebooks cannot be exported or put in git.
Elsewhere, the notebooks people praise most (marimo, Livebook, Pluto, Evidence, Observable Framework)
share three traits: a plain-text format, reactive execution, and a static export.

## 2. The notebook file

[FORMAT.md](FORMAT.md) is the full specification. The essentials:

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

- **Markers are comments.** `-- %%` starts a cell; prose and chart specs sit in block comments. DuckDB,
  sqlfluff and any editor see a normal SQL file.
- **A named query cell is a view.** `monthly` above can be queried by any later cell.
- **Parameters are DuckDB variables.** An input cell is a real `SET VARIABLE` statement, and `$region`
  reads it. This needs no templating layer: `$name` is DuckDB 2.0's documented shorthand for
  `getvariable('name')`. Notebooks therefore need DuckDB 2.0 or later.
- **The cell name is its ID.** No generated identifiers appear in the file.
- **No outputs in the file.** They go to a disposable, git-ignored cache directory.

### Why this format

The survey compared five families of notebook format ([RESEARCH.md §2](RESEARCH.md#2-notebook-formats)).
A source file in the host language with comment delimiters (marimo `.py`, Pluto `.jl`, Databricks
`-- COMMAND`, Jupytext `# %%`) gives the best diffs and the best tool support, because the file is
still valid code. For a SQL notebook the host language is SQL.

The cost is that prose and chart specs are comments, so GitHub shows them as comment text rather than
rendered Markdown. A Markdown export covers the case where a rendered view in the repository matters.

### What "runnable" means

The file always parses in DuckDB. It runs start to finish in a plain client only when cells do not
refer to each other by name, because the view wrapper is added by the runner. That limit is deliberate:
keeping the bare `SELECT` in the file is what makes cells pleasant to read and edit. For portability
the extension writes an expanded script with the wrappers spelled out
([example](../examples/revenue.expanded.sql)), which runs in any DuckDB client.

## 3. Execution model

**Graph.** Each cell has a set of names it defines and a set it reads.

| Cell | Defines | Reads |
|---|---|---|
| Query cell | Its own name | Tables, views and variables in its SQL |
| Effect cell | Objects it creates or attaches; variables it sets | Same |
| Input | Its variable | The `from=` cell, if any |
| Chart | Its selection variable, if any | `from`, `filter`, fields |
| Markdown | Nothing | Names inside `{{ }}` |

Reads come from DuckDB's parser, not from regular expressions. The extension walks the parsed
statement for table references that are not CTEs, for `$name` parameters and for `getvariable('…')`
calls. Names that match no cell are assumed to be catalog objects and are left alone.

**Rules**

- One definition per name. A second cell defining the same name is an error on both.
- SQL and input cells read only cells above them, so the graph cannot contain a cycle and the file
  reads top to bottom.
- A cell is `fresh`, `stale`, `running` or `failed`. When a cell's source or anything it reads changes,
  it and everything downstream become stale.

**Query cells re-run automatically.** They are views, so "re-running" means re-reading the view for
the result grid and re-running the data queries of charts that depend on it. With auto-run off, cells
are only marked stale and wait for a click, which is how marimo's lazy mode and Livebook behave.

**Effect cells run only when asked.** Automatically replaying `INSERT`, `COPY … TO` or `ATTACH` would
be surprising and sometimes destructive. Editing or running an effect cell marks its dependents stale.
"Run all" executes every cell in file order, which is also what the headless runner does.

**Inputs and selections** set a variable in the session and trigger the same staleness propagation. To
the graph, a slider and a chart brush are the same thing.

**Sessions.** Each open notebook gets its own DuckDB connection. Temporary views and variables are
private to a connection (checked in 2.0.0), so two notebooks can both define `monthly` without
colliding, while both see the same attached databases.

**Long queries.** Results stream to the grid in pages. Any running cell can be interrupted. A
`materialize=table` option stores a slow cell's result as a temp table so dependents do not recompute
it.

**Trust.** Opening a notebook does not run it. SQL can read local files and reach the network through
`httpfs`, so a notebook from someone else stays inert, showing cached output if any, until the user
chooses to run it.

## 4. Runtime architecture

```
 DuckDB process (CLI, Python, any client)
 ┌─────────────────────────────────────────────────────────────┐
 │  notebook extension                                         │
 │   ├─ format parser, dependency graph, chart-spec compiler   │
 │   ├─ runner (one connection per notebook session)           │
 │   ├─ exporters (HTML, SQL, Markdown, data)                  │
 │   └─ HTTP server on 127.0.0.1 ── serves embedded frontend   │
 └───────────────────────────┬─────────────────────────────────┘
                             │ HTTP + server-sent events, Arrow results
                      Browser: editor, grids, charts
```

**Why an extension.** It runs inside the DuckDB the user already has, so the notebook sees their
attached databases, secrets, settings and extensions, with no second engine version and no file-lock
conflict. Installation is one `INSTALL` statement. Storing notebooks in the database is natural because
the code is already in the database process.

**What it costs.** The extension is C++ and must be rebuilt for each DuckDB release. To limit that,
all logic that does not need engine internals (the UI, chart rendering, export viewer) lives in the
TypeScript frontend, and the C++ side stays small: parser, graph, compiler, runner, server.

### SQL surface

Everything the UI does is also callable from SQL, so notebooks can be run and exported from a script
or CI job with no browser.

| Function | Purpose |
|---|---|
| `notebook_start([path or name], port := …, open := true)` | Start the server, optionally opening a notebook |
| `notebook_stop()` | Stop it |
| `notebook_cells(path)` | Table function: one row per cell with kind, name, options, source, defines, reads |
| `notebook_run(path, params := {…})` | Run every cell in order; returns per-cell status and timings |
| `notebook_export(path, to := 'out.html', format := …, params := {…})` | Export (see §7) |
| `notebook_save(path, name := …, message := …)` | Store a file in the current database as a new version |
| `notebook_checkout(name, to := 'x.sql', version := …)` | Write a stored version to a file |
| `notebook_chart_query(spec, from)` | The SQL a chart spec compiles to, for inspection |

### HTTP server

- Binds to `127.0.0.1` only. A random token is created at start and required on every request. `Host`
  and `Origin` headers are checked as well, which blocks DNS-rebinding and cross-site requests. (The
  official UI relies on an `Origin` check alone.)
- Endpoints: load and save a notebook, parse to cells and graph, run and interrupt a cell, set a
  variable, fetch chart data, catalog listing, export. One server-sent-events stream reports cell
  state changes, catalog changes and edits made to the file by another program.
- **Results are Arrow IPC streams.** The charting and grid libraries consume Arrow directly. The
  official UI uses DuckDB's own binary serialisation with an MIT-licensed TypeScript decoder, which is
  a fallback if producing Arrow inside the extension proves awkward (spike S2).
- The frontend bundle is compressed and compiled into the extension binary. Nothing is fetched from
  the network at run time.

### Frontend

A TypeScript single-page app:

- CodeMirror 6 editor with DuckDB completion from the live catalog and cell names.
- Result grid, virtualised, with a per-column profile (type, null share, histogram or top values),
  the feature people single out in the official UI.
- Catalog browser for attached databases, with drag-to-insert.
- Graph view of cells, showing stale and failed cells.
- Editing view (cells with code) and reading view (code hidden, grid layout via `span`).
- **Two-way file sync.** Saving writes the `.sql` file; an edit made in another editor or by an agent
  appears in the UI through the file watcher. The file is always the source of truth for a file-backed
  notebook.
- Live preview while typing (the idea behind MotherDuck's Instant SQL) is a later addition; the view
  model makes it cheap because a preview is a `LIMIT` over the cell's view.

## 5. Notebooks inside the database

A notebook can be saved into any attached, writable database. The schema is small:

```sql
CREATE SCHEMA IF NOT EXISTS _notebook;

CREATE TABLE _notebook.notebooks (
    name        VARCHAR PRIMARY KEY,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE _notebook.versions (
    name         VARCHAR NOT NULL REFERENCES _notebook.notebooks (name),
    version      INTEGER NOT NULL,
    content      VARCHAR NOT NULL,      -- the .sql file, byte for byte
    content_hash VARCHAR NOT NULL,      -- sha256(content)
    saved_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    message      VARCHAR,
    PRIMARY KEY (name, version)
);

CREATE VIEW _notebook.current AS
SELECT * FROM _notebook.versions
QUALIFY version = max(version) OVER (PARTITION BY name);
```

- **One format.** `content` is exactly the text of the file. Saving to the database and checking out
  to a file are lossless in both directions, so a notebook can move between git and a database freely.
  (Checked: a file read with `read_text`, stored, and read back is identical.)
- **History is rows.** Each save appends a version; a save whose hash equals the current version is
  skipped. Old versions can be listed, diffed and restored with plain SQL.
- **A database carries its own analysis.** Send someone `sales.duckdb` and they get the data and the
  notebooks that explain it. `CALL notebook_start('db:revenue')` opens a stored notebook.
- **Result cache.** For a database-backed notebook the cache goes in `_notebook.cache(name, cell,
  hash, data BLOB)` in place of the sidecar directory.

**File and database together.** A notebook session has one source of truth: the place it was opened
from. `notebook_save` and `notebook_checkout` copy between the two explicitly. There is no automatic
two-way sync in the first version, because silent merges between a git checkout and a database row
are exactly where work gets lost. Both commands compare hashes and refuse to overwrite a newer copy
without `force := true`.

**Alternatives considered.** DuckLake snapshots give time travel for free, but require a DuckLake
catalog and are heavy for a few kilobytes of text. The official UI keeps notebooks as JSON rows in a
private `ui.db`; using the user's own database and the plain-text format is what makes ours portable.

## 6. Visualisation

### Principle: the extension writes the query, the browser draws the result

A chart cell is a short YAML spec ([FORMAT.md §5](FORMAT.md#5-chart-cells)). The extension compiles
it into two things:

1. **A data query**: one aggregate SQL statement over the cell's `from` relation. Grouping, binning,
   time truncation and downsampling all happen in DuckDB.
2. **A render description**: marks, scales and axes for the browser, referring to the query's columns.

The browser receives a few hundred or thousand rows whatever the size of the source table.

This split was chosen over running a query-generating library in the browser for three reasons:

- **Export without a browser.** The extension can compute every chart's data on its own, which is what
  lets `notebook_export` produce a complete HTML file from a script.
- **Small, private exports.** An exported page contains the aggregates behind each chart, not the
  underlying table.
- **One reactive model.** A selection sets a variable and dependents re-query, the same path as an
  input widget.

The techniques come from Mosaic (UW Interactive Data Lab), which demonstrated that pushing aggregation
into DuckDB keeps charts interactive on very large tables: M4 downsampling for lines (at most four
points per pixel column) and binning to the pixel grid for scatter and heat marks. Mosaic itself is
not the chosen dependency for the core path because it compiles queries in the browser and its API is
still changing between 0.x releases. It remains the likely basis for the fully interactive WASM export
(§7).

### Renderers

The spec is ours; the libraries behind it are replaceable. All are permissively licensed.

| Chart family | Library | Notes |
|---|---|---|
| Marks on axes, facets, layers | Observable Plot | SVG output, which is what PDF wants |
| Sankey, chord, treemap, sunburst, icicle | D3 layouts (`d3-sankey`, `d3-chord`, `d3-hierarchy`) | Drawn as SVG |
| Funnel, candlestick, gauge, calendar | Apache ECharts | SVG renderer for export |
| Maps | MapLibre GL + deck.gl GeoArrow layers | Geometry from the `spatial` extension; H3 hex layers |
| Networks | cosmos.gl | GPU layout; Cosmograph, built on it, is non-commercial and is not used |
| Tables, pivots | Virtualised grid | Pivot aggregation is a DuckDB `PIVOT` query |
| KPI tiles, sparklines | Own SVG | |
| Raw escape hatch | Vega-Lite, ECharts | `type: vega-lite` / `type: echarts` with a native spec |

D3 is used as a toolkit inside built-in chart types. Hand-written D3 needs JavaScript cells, which are
out of scope; the raw Vega-Lite and ECharts specs are the declarative escape hatch until then.

### Building charts

- **Chart switcher under every result**, as in Zeppelin: pick a chart type and drag columns to
  channels. The difference is that the result is written into the file as a chart cell, so a chart
  made by clicking is reviewable and editable as text.
- **Suggested chart** from the result's column types: one time column and one number gives a line; one
  category and one number, a bar; two numbers, a scatter or a binned heatmap when large; a single
  value, a KPI tile; a geometry or latitude/longitude pair, a map.
- **Cross-filtering.** A selection on one chart filters others through `filter: <selection>`, or any
  SQL cell through its variable.

### Themes

One light and one dark theme with a categorical palette checked for colour-vision deficiency, applied
across every renderer so charts from different libraries look like one system.

## 7. Export

| Format | Needs DuckDB to view | Produced by | Contents |
|---|---|---|---|
| `html` (default) | No | Extension alone | One file: viewer, prose, chart specs, chart data |
| `html` with `interactive := 'precomputed'` | No | Extension alone | As above plus precomputed results for input values |
| `html-wasm` | No (ships DuckDB-WASM) | Extension alone | Folder: viewer, DuckDB-WASM, Parquet data |
| `pdf` | No | Browser print, or headless Chrome | Paged document, vector charts |
| `sql` | Yes | Extension alone | Expanded script with no notebook dependency |
| `md` | No | Extension; chart images need a browser | Markdown with tables; charts as SVG files |
| `data` | No | Extension alone | One Parquet or CSV file per query cell |

### Single-file HTML

The extension assembles one `.html` file from parts it already has:

- the read-only viewer (a subset of the frontend, stored in the extension),
- the notebook's cells and render descriptions as JSON,
- each chart's data and each visible table's first page as gzip-compressed Arrow, base64 encoded.

The page decompresses with the browser's built-in `DecompressionStream` and draws the charts when
opened. It works from a `file://` path, as an email attachment or on any static host. Tooltips, legend
toggles, table sorting and paging work; nothing needs a server. Expected size is a few hundred
kilobytes for the viewer core plus the data, with heavy renderers (maps, networks, ECharts) included
only when the notebook uses them.

### Interactivity without DuckDB

A static page cannot run SQL, so inputs need another answer. With `interactive := 'precomputed'` the
exporter runs the notebook once per value of each `select`, `checkbox` and stepped `slider` input and
stores each result; the page swaps between them. Combinations multiply, so there is a size budget and
the exporter reports which inputs were frozen at their current value to stay inside it. Pluto's static
slider export works the same way. Free-form inputs (`text`, unbounded `number`) are always frozen.

### Fully interactive export

`html-wasm` writes a folder containing the viewer, DuckDB-WASM and the tables the notebook reads as
Parquet. Inputs and selections then re-run real SQL in the reader's browser. The DuckDB-WASM module
alone is about 8 MB compressed (36 MB uncompressed, measured from the current npm package), and the
folder must be served over HTTP. Whether it can be collapsed into one file that works
from `file://` is unproven (spike S4). If the extension can itself be built for DuckDB-WASM, the same
chart compiler runs in the browser; otherwise the compiler is ported or Mosaic takes that role (S5).

### PDF

Charts are drawn by JavaScript, and the extension has no JavaScript engine, so a browser has to be
involved in making a PDF. Two paths share one print stylesheet (`@page` size and margins, cells never
split across pages, code hidden by default, charts as SVG so they stay vector):

- **From the UI:** "Export PDF" opens the print dialog on the reading view. No dependencies.
- **Headless:** `notebook_export(…, format := 'pdf')` writes the HTML export and runs an installed
  Chrome, Edge or Chromium with `--headless --print-to-pdf`. This was checked with Chromium: content
  drawn by a script after load is captured, and text stays vector.

Maps and GPU network charts are rasterised at print resolution. A Typst backend (text and tables
native, charts as SVG) would give finer typographic control and is noted for later, but it still needs
a browser to produce the SVG.

## 8. Risks and spikes

| ID | Question | Why it matters |
|---|---|---|
| S1 | Can the extension walk a parsed statement for table and parameter references across all statement kinds? | Core of the graph. `duckdb_get_table_names` is not enough: in 2.0.0 it binds, and so fails, on `JOIN … USING` and `INSERT` against tables that do not exist yet. `json_serialize_sql` handles only `SELECT` |
| S2 | Arrow IPC from inside the extension, or DuckDB's own serialisation plus the MIT TypeScript decoder? | Result transport |
| S3 | Size of the extension with the frontend embedded; lazy loading of heavy renderers | Download size and the community-extension build |
| S4 | DuckDB-WASM inlined in a single HTML file, opened from `file://` | Decides whether the fully interactive export can be one file |
| S5 | Does the extension build for DuckDB-WASM with the server parts left out? | Reuse of the chart compiler in `html-wasm`. The community build chain targets Wasm, so this is plausible but untried |
| S6 | Will the community repository accept an extension that embeds an HTTP server and a multi-megabyte asset bundle? | Distribution. The process itself is documented: a pull request with a descriptor file, after which DuckDB's CI builds for Linux, macOS, Windows and Wasm |
| S7 | Chart compiler coverage: how much of the grammar compiles cleanly to one query | Scope of chart work |

Other risks:

- **DuckDB version coupling.** Every DuckDB release needs a rebuilt extension. Keeping the C++ side
  small is the mitigation.
- **DuckDB 2.0 is the minimum version.** The `$name` variable shorthand arrived in 2.0. Supporting
  older versions would mean the runner rewriting `$name` to `getvariable('name')`; that is possible
  but is not planned.
- **Reactivity surprises.** Effect cells are the escape from the pure model. The manual-run default
  and visible stale markers are the guard.
- **Scope of charts.** The list in FORMAT.md is long. The roadmap ships grammar charts first.

## 9. Roadmap

1. **Format and headless runner.** Parser, graph, `notebook_cells`, `notebook_run`, `sql` export.
   Testable with DuckDB's SQL test runner and no UI. Resolves S1.
2. **Server and minimal UI.** Editor, grid, reactive runs, file sync. Resolves S2, S3.
3. **Charts and inputs.** Chart compiler, grammar charts, KPI and table, input widgets, selections.
4. **In-database storage.** `_notebook` schema, save, checkout, history.
5. **Export.** Single-file HTML, PDF, Markdown, precomputed interactivity.
6. **More chart types.** Maps, hierarchies, flows, networks, pivot.
7. **Interactive WASM export.** Resolves S4, S5.

## 10. Open questions

- Should there be a dashboard layout beyond the 12-column `span` grid, such as free placement?
- Should JavaScript cells be added later for bespoke D3, and if so, how do they stay out of the trust
  model's way?
- Should a notebook be able to import cells from another notebook?
- Is the `-- %%` marker the right spelling, or should it follow Databricks' `-- COMMAND ----------`
  for compatibility with that ecosystem?
