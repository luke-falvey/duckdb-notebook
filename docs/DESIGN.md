# DuckDB Notebook: design

Status: every roadmap step (§9) has a first implementation: format parser, dependency graph,
headless runner, server and browser UI, chart compiler and renderers, in-database storage, and the
HTML, PDF, Markdown, SQL, data and DuckDB-WASM exports. §11 records what building it taught us and
where the implementation departs from the design below.

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
/* chart
mark: line
x: month
y: revenue
*/
```

- **Markers are comments.** `-- %%` starts a cell; prose and chart specs sit in block comments. DuckDB,
  sqlfluff and any editor see a normal SQL file.
- **A named query cell is a view.** `monthly` above can be queried by any later cell.
- **A query's output is a table until it is given charts.** Chart specs written after the query are
  views of its result, shown as tabs (§14).
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
| Query cell | Its own name; its charts' selection variables | Tables, views and variables in its SQL; its charts' `filter` selections (no dependency) |
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

**Sessions.** Each open notebook gets a session of its own. Temporary views and variables are
private to a connection (checked in 2.0.0), so two notebooks can both define `monthly` without
colliding, while both see the same attached databases. The server does not hold a connection
between requests (see §11): a session is its variables plus the statements that rebuild its
temporary views, replayed onto a fresh connection for each request.

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
 │   ├─ runner (a session replayed onto a connection per run)  │
 │   ├─ exporters (HTML, PDF, SQL, Markdown, data, WASM)       │
 │   └─ HTTP server on 127.0.0.1 ── serves embedded frontend   │
 └───────────────────────────┬─────────────────────────────────┘
                             │ HTTP + server-sent events, JSON results
                      Browser: editor, grids, charts
```

**Why an extension.** It runs inside the DuckDB the user already has, so the notebook sees their
attached databases, secrets, settings and extensions, with no second engine version and no file-lock
conflict. Installation is one `INSTALL` statement. Storing notebooks in the database is natural because
the code is already in the database process.

**What it costs.** The extension is C++ and must be rebuilt for each DuckDB release. To limit that,
all logic that does not need engine internals (the UI, chart rendering, export viewer) lives in the
JavaScript frontend, and the C++ side stays small: parser, graph, compiler, runner, server.

### SQL surface

Everything the UI does is also callable from SQL, so notebooks can be run and exported from a script
or CI job with no browser.

| Function | Purpose | State |
|---|---|---|
| `notebook_cells(path)` | One row per cell: kind, name, options, body, chart specs, variables read, `defines`, `reads`, `depends_on`, errors and warnings | Built |
| `notebook_sql(path)` | The notebook as a plain SQL script (the `sql` export) | Built |
| `notebook_run(path, params := {…})` | Run every cell in order in a session of its own; per-cell status, row count, timing and rendered markdown | Built |
| `notebook_start([path or name], port := …, open := true)` | Start the server, optionally opening a notebook | Built |
| `notebook_stop()` | Stop it | Built |
| `notebook_export(path, file := 'out.html', format := …, params := {…})` | Export (see §7) | Built |
| `notebook_save(path, name := …, message := …, force := …)` | Store a file in the current database as a new version | Built |
| `notebook_checkout(name, file := 'x.sql', version := …, force := …)` | Write a stored version to a file | Built |
| `notebook_chart_query(spec, from)` | The SQL a chart spec compiles to, for inspection | Built |

The output file parameter is `file :=` rather than `to :=`: `to` is a reserved word in DuckDB's
grammar and cannot name a parameter. Every function that takes a path also takes `db:<name>` or
`db:<name>@<version>` for a notebook stored in the database.

All of these are table functions. `notebook_run` executes in its own connection, so tables it creates
persist but its views and variables do not reach the caller. It runs chart cells too (compiling and
querying their data), so a broken chart spec fails a headless run. To run a notebook inside your own
connection, execute the script `notebook_sql` returns.

### HTTP server

- Binds to `127.0.0.1` only. A random token is created at start and required on every request. `Host`
  and `Origin` headers are checked as well, which blocks DNS-rebinding and cross-site requests. (The
  official UI relies on an `Origin` check alone.)
- Endpoints: load and save a notebook, parse to cells and graph, run and interrupt cells, set a
  variable, page through a result, input choices, catalog listing, export. A server-sent-events
  stream reports edits made to the file by another program.
- **Results are JSON** (S2, see §11): a result page or a chart's aggregated rows are small, and JSON
  needs no decoder in the browser.
- The server is cpp-httplib, which DuckDB already vendors. The frontend is compiled into the extension
  binary. Nothing is fetched from the network at run time.

### Frontend

A single-page app in plain JavaScript with no dependencies (`frontend/`). It looks like a sketchbook
drawn in Excalidraw (see Themes, §6): handwriting, paper, and hand-drawn boxes, lines and charts.

- A quiet toolbar: the notebook's title, its status, **Run all**, and a gear. Everything else (open,
  save, auto-run, reading and graph views, catalog, theme, default page size, export) is in the gear's
  menu.

- An editor per cell with Ctrl+Space completion from the live catalog, cell names and variables, and
  Shift+Enter to run. The design called for CodeMirror 6; bundling it needs a JavaScript build step in
  the extension build, which was left for later (§11).
- Result grid, paged from the server, headed by plain column names. An earlier per-column profile
  (type, null share, histogram or top values) was removed to keep tables quiet; the catalog sidebar
  still lists column types. A page-size picker under each table writes `rows=N` to the cell, so the
  choice is saved with the notebook and applies to exports.
- Quiet by default: a cell's kind is a symbol in its top left (☰ settings, ¶ text, ◉ input, ▦ query,
  ✎ SQL that changes the database, ∿ chart; named in its tooltip); the buttons that add a cell appear
  only when the pointer is between cells; a cell's rows, what it defines and its run time sit in the
  bottom right corner of its box (`timing: false` in the `[notebook]` cell hides the time); and a
  table's chart picker stays behind a "+ chart" button until asked for.
- Hover values: pointing at a mark shows its values in a paper note by the pointer, in the editor and
  in exports. On line and area charts the nearest point anywhere along the line answers, marked with a
  dot, even under a brush; a bar answers anywhere in its column (its row, when horizontal), which is
  highlighted. Hatched shapes carry an invisible hit area, so the gaps between hatch lines count.
  Bars on a continuous x (months on a date axis) are as wide as the spacing allows, and the axis is
  padded so the first and last are too. Re-rendering
  keeps the scroll position, so changing an input or brushing a chart does not move the page.
- Code and output switches in every cell header write `hide=` to the cell. While editing, a hidden
  part collapses to a line that reveals it; reading view and exports leave it out.
- Prose, chart, input and metadata cells show their result and keep their source behind an "edit"
  line (or a double-click on prose); SQL cells show their code.
- Catalog browser for attached databases, with click and drag to insert.
- A query's charts as tabs over its result, with the table last. A chart picker under the table (or
  "+ chart") writes a new `/* chart */` comment into the cell; "edit chart spec" opens a chart's spec,
  and Shift+Enter redraws it without running the query.
- Graph view of cells, showing stale and failed cells.
- Editing view (cells with code) and reading view (code hidden, grid layout via `span`).
- **Two-way file sync.** Saving writes the `.sql` file; an edit made in another editor or by an agent
  appears in the UI through the file watcher. The file is always the source of truth for a file-backed
  notebook.
- Live preview while typing is a later addition. MotherDuck's Instant SQL does this by parsing the
  query locally, caching the data it depends on and rewriting it to run fast, and lets the user click
  into any CTE to see its rows. Here a named cell is already a view, so a preview is a `LIMIT` over
  it and stepping through intermediate results is stepping through cells.

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
  hash, data BLOB)` in place of the sidecar directory. (Not built yet: neither cache exists.)

**File and database together.** A notebook session has one source of truth: the place it was opened
from. `notebook_save` and `notebook_checkout` copy between the two explicitly. There is no automatic
two-way sync in the first version, because silent merges between a git checkout and a database row
are exactly where work gets lost. Both commands compare hashes and refuse to overwrite a newer copy
without `force := true`: `notebook_save` refuses text that equals an *older* stored version, and
`notebook_checkout` refuses to overwrite a file whose content matches no stored version. Saving from
the UI of a `db:` notebook appends a version.

**Alternatives considered.** DuckLake snapshots give time travel for free, but require a DuckLake
catalog and are heavy for a few kilobytes of text. The official UI keeps notebooks as JSON rows in a
private `ui.db`; using the user's own database and the plain-text format is what makes ours portable.

## 6. Visualisation

### Principle: the extension writes the query, the browser draws the result

A chart is a short YAML spec ([FORMAT.md §5](FORMAT.md#5-charts)), written at the end of a query cell
or as a chart cell. The extension compiles it into two things:

1. **A data query**: one aggregate SQL statement over the relation the chart draws. Grouping, binning,
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

The spec is ours; the renderers behind it are replaceable. The design planned on libraries:

| Chart family | Planned library | Built |
|---|---|---|
| Marks on axes, facets, layers | Observable Plot | Own SVG (`frontend/render-chart.js`) |
| Sankey, chord, treemap, sunburst, icicle | D3 layouts | Own SVG layouts (`frontend/render-typed.js`) |
| Funnel, candlestick, gauge, calendar | Apache ECharts | Own SVG |
| Maps | MapLibre GL + deck.gl | Own SVG, equirectangular, no basemap |
| Networks | cosmos.gl | Own SVG with a small force layout (up to a few thousand nodes) |
| Tables, pivots | Virtualised grid | Paged grid; pivot is a DuckDB `PIVOT` query |
| KPI tiles, sparklines, gauge | Own SVG | Own SVG |
| Raw escape hatch | Vega-Lite, ECharts | Specs whose marks map onto the built-in renderer are drawn; others show as a table |

The first implementation draws everything itself, in about 1,500 lines of dependency-free
JavaScript, for the reason in §11 ("The frontend has no build step"), and draws it by hand:
`frontend/render-sketch.js` is a small rough.js-style library (wobbly double strokes, overshooting
corners, hachure fills) that every renderer uses. Above 600 marks in a layer it falls back to plain
shapes in the same colours, because the wobble costs time and adds nothing at that density. Drawing
is seeded per chart, so a chart looks the same in the editor, an export and a PDF. All charts are SVG,
so PDFs stay vector. A basemap needs tiles, which are not available offline, so maps draw the data alone. The
libraries remain the upgrade path for richer interaction (zoom, GPU-scale networks, tiled maps).

### Building charts

- **Chart switcher under every result**, as in Zeppelin: pick a chart type and columns for its
  channels. The difference is that the result is written into the file as a `/* chart */` comment in
  the query's cell, so a chart made by clicking is reviewable and editable as text.
- **Suggested chart** from the result's column types: one time column and one number gives a line; one
  category and one number, a bar; two numbers, a scatter or a binned heatmap when large; a single
  value, a KPI tile; a geometry or latitude/longitude pair, a map.
- **Cross-filtering.** A selection on one chart filters others through `filter: <selection>`, or any
  SQL cell through its variable.

### Themes

The notebook looks like a sketchbook drawn in Excalidraw ("Option A" of the design mockups): warm
paper with a faint grain, cells in hand-drawn rounded boxes, Excalifont for text and Cascadia Code for
SQL (both SIL OFL, compiled into the extension; `third_party/fonts/LICENSE.md`), values computed in
prose highlighted with a marker, and charts in Excalidraw's palette: a stroke colour with its pastel
as a hachure fill. Dark mode is Excalidraw's own: the page is inverted and its hues turned back.

## 7. Export

| Format | Needs DuckDB to view | Produced by | Contents |
|---|---|---|---|
| `html` (default) | No | Extension alone | One file: viewer, prose, chart specs, chart data |
| `html` with `interactive := 'precomputed'` | No | Extension alone | As above plus precomputed results for input values |
| `html-wasm` | No (ships DuckDB-WASM) | Extension alone | One file, about 12 MB plus data: viewer, DuckDB-WASM, data as a `.duckdb` file |
| `pdf` | No | Browser print, or headless Chrome | Paged document, vector charts |
| `sql` | Yes | Extension alone | Expanded script with no notebook dependency |
| `md` | No | Extension alone | Markdown with tables; each chart as a table of its data |
| `data` | No | Extension alone | One Parquet or CSV file per query cell |

### Single-file HTML

The extension assembles one `.html` file from parts it already has:

- the read-only viewer (a subset of the frontend, stored in the extension),
- the notebook's cells and render descriptions as JSON,
- each chart's data and each visible table's first page as gzip-compressed JSON, base64 encoded.

The page decompresses with the browser's built-in `DecompressionStream` and draws the charts when
opened. It works from a `file://` path, as an email attachment or on any static host; nothing needs a
server. The fonts travel inside the page as data URIs, so it looks the same offline. Measured: the
viewer and renderers are about 35 KB gzip-compressed and the two fonts 90 KB; the revenue example
exports to 252 KB and the 27-chart gallery to 425 KB.

`hide := 'code'`, `'output'` or `'all'` leaves that part of every cell out of any export, on top of
each cell's own `hide=`; prose and inputs always stay. The gear menu offers the same as two
checkboxes. Tables in an export page through the rows it includes (`page_rows`, default 100, raised
to a cell's `rows=` when larger).

### Interactivity without DuckDB

A static page cannot run SQL, so inputs need another answer. With `interactive := 'precomputed'` the
exporter runs the notebook once per value of each `select`, `checkbox` and stepped `slider` input and
stores each result; the page swaps between them. Combinations multiply, so there is a size budget and
the exporter reports which inputs were frozen at their current value to stay inside it. Pluto's static
slider export works the same way. Free-form inputs (`text`, unbounded `number`) are always frozen.

### Fully interactive export

`html-wasm` embeds DuckDB-WASM and the tables the notebook reads, so inputs and selections re-run
real SQL in the reader's browser. It is also a single file that opens from `file://` with no network.
A [spike](../spikes/wasm-single-file/README.md) confirmed this in Chromium: a 13.6 MB page starts
DuckDB in about 1.4 seconds and queries an inlined 200,000-row database. The fixed cost is about
12 MB, against a few hundred kilobytes for the static export, which is why static stays the default.

The spike set four constraints on this export:

- DuckDB-WASM's stock loader hangs under `file://`; a short shim that passes the module to the worker
  by message is needed.
- Data ships as a `.duckdb` file or Arrow, not Parquet. Parquet, JSON and `spatial` are extensions
  that DuckDB-WASM downloads at run time, so they are unavailable offline unless inlined as well.
- The data file must be written in a storage version the bundled engine reads.
- The DuckDB-WASM on npm is engine v1.5.4, which lacks the `$name` shorthand. Until a 2.0-based
  build is published, the exporter rewrites `$name` to `getvariable('name')`.

Built as designed, with one simplification that removes S5: the browser never compiles a chart. A
chart's SQL depends on its spec and its source's column types, not on input values, because inputs and
selections reach the query through `getvariable()`. So the exporter compiles every chart in advance and
the page re-runs the stored SQL after `SET VARIABLE`. The runtime (DuckDB-WASM plus a small adapter,
`wasm/`) is built once with `sh wasm/build.sh` and passed as `wasm_runtime := 'wasm/dist'`; it is not
compiled into the extension because it is 36 MB uncompressed. The revenue example exports to 12.2 MB
and boots in about a second in headless Chromium with the network blocked.

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
| S1 | ~~Can the extension walk a parsed statement for table and parameter references across all statement kinds?~~ | **Done: yes, with the C++ parser and no binding.** See §11 |
| S2 | ~~Arrow IPC from inside the extension, or DuckDB's own serialisation plus the MIT TypeScript decoder?~~ | **Done: neither; JSON.** Pages and chart data are small after aggregation. See §11 |
| S3 | ~~Size of the extension with the frontend embedded; lazy loading of heavy renderers~~ | **Done: 132 KB of frontend source, 30 KB compressed**, negligible next to the extension itself |
| S4 | ~~DuckDB-WASM inlined in a single HTML file, opened from `file://`~~ | **Done: it works in Chromium.** See [the spike](../spikes/wasm-single-file/README.md). Firefox, Safari and inlined extensions are untested |
| S5 | ~~Does the extension build for DuckDB-WASM with the server parts left out?~~ | **Not needed**: `html-wasm` ships precompiled chart SQL (§7). Still untried |
| S6 | Will the community repository accept an extension that embeds an HTTP server and a multi-megabyte asset bundle? | Distribution. The process itself is documented: a pull request with a descriptor file, after which DuckDB's CI builds for Linux, macOS, Windows and Wasm |
| S7 | ~~Chart compiler coverage: how much of the grammar compiles cleanly to one query~~ | **Done: all of FORMAT.md §5** compiles, with layers and sparklines as one query each. See `examples/charts.sql` |

Other risks:

- **DuckDB version coupling.** Every DuckDB release needs a rebuilt extension. Keeping the C++ side
  small is the mitigation.
- **DuckDB 2.0 is the minimum version.** The `$name` variable shorthand arrived in 2.0. Supporting
  older versions would mean the runner rewriting `$name` to `getvariable('name')`; that is possible
  but is not planned.
- **Reactivity surprises.** Effect cells are the escape from the pure model. The manual-run default
  and visible stale markers are the guard.
- **Scope of charts.** The list in FORMAT.md is long. Every type is drawn, simply; see §6 for what
  the planned libraries would add.

## 9. Roadmap

1. **Format and headless runner.** Parser, `notebook_cells`, `notebook_run`, `sql` export. **Done.**

   1b. **Dependency graph.** `defines` and `reads` per cell from DuckDB's parser, forward-reference
   and cycle errors, `params` for `notebook_run`, `{{ }}` expressions in markdown. Resolves S1.
   **Done.**
2. **Server and minimal UI.** Editor, grid, reactive runs, file sync. Resolves S2, S3. **Done.**
3. **Charts and inputs.** Chart compiler, grammar charts, KPI and table, input widgets, selections.
   **Done.**
4. **In-database storage.** `_notebook` schema, save, checkout, history. **Done.**
5. **Export.** Single-file HTML, PDF, Markdown, precomputed interactivity. **Done.**
6. **More chart types.** Maps, hierarchies, flows, networks, pivot. **Done.**
7. **Interactive WASM export.** Resolves S5. **Done.**

Next, in rough order: the result cache (FORMAT.md §8), a bundled CodeMirror editor, tracking macro
calls in the graph, Firefox and Safari checks for the exports, building every platform in CI, and the
community-repository submission (S6).

## 10. Open questions

- Should there be a dashboard layout beyond the 12-column `span` grid, such as free placement?
- Should JavaScript cells be added later for bespoke D3, and if so, how do they stay out of the trust
  model's way?
- Should a notebook be able to import cells from another notebook?
- Is the `-- %%` marker the right spelling, or should it follow Databricks' `-- COMMAND ----------`
  for compatibility with that ecosystem?

## 11. What the first pass taught us

The first pass built the format parser, `notebook_cells`, `notebook_sql` and `notebook_run`, with
tests. These findings changed or sharpened the design.

### The extension has to use DuckDB's C++ API

DuckDB offers two ways to write an extension. The first pass tried the stable C API first, because it
is attractive: it built in seconds without compiling DuckDB, and one binary loads into every later
DuckDB release. In DuckDB 2.0 it covers table functions, the file system, statement extraction, Arrow
and interrupts. `notebook_cells` and `notebook_sql` worked on it.

It fails on one requirement: **running SQL from inside the extension**. The C API hands an extension
a database handle only while it is loading. The only way to run queries later is to open a connection
at load time and keep it. That was tested, and it pins the database open: after the user closed their
connection, the write-ahead log was not checkpointed and another process could not open the file.
Loading an extension must not do that. The C API has no way to go from a function's context to a
connection, and no weak reference.

The C++ API does. A function can create a connection from its context when it needs one and release
it afterwards, which is what `notebook_run` does. The same check on the C++ build passed: after a
run, the database closed and checkpointed and a second process could read it. So the extension uses
the C++ API, as the official
UI extension does. The costs are the ones §4 already named, now measured:

- The extension is tied to one DuckDB release and is rebuilt for each.
- The first build compiles all of DuckDB: about 15 minutes on 4 cores. Later builds take seconds.

The second cost shapes how the code is organised. Everything that does not need a database lives in
`src/core`, which has no DuckDB dependency and whose tests compile and run in seconds. If DuckDB
later adds a way to obtain a connection through the C API, the thin `src/functions` layer is the only
part that would need porting back.

### Rule: the extension never owns the database

The same measurement gives a rule for everything that follows. A stored `Connection` keeps the
database alive. The server in step 2 must hold a `weak_ptr` to the database instance, create
connections on demand, and shut down when the database goes away. A notebook session's connection
belongs to the session and is released when the session ends.

### Sessions are separate connections, and that is visible

`notebook_run` gives each run its own connection, as §3 specifies for sessions. The consequence is
user-visible: tables a notebook creates remain, but its temp views and variables are gone when the
run returns. `notebook_sql` exists partly for this reason. It returns the script, which a client can
execute in its own connection to keep the views.

### Variables nobody sets start as NULL

`$name` on an unset variable is an error in DuckDB, which breaks any cell that reads a chart
selection before the user has made one. The runner now finds every variable that cells read but no
input cell or `SET VARIABLE` defines, and sets it to `NULL` before the first cell runs. An untyped
`NULL` is enough: `$picked IS NULL OR ts BETWEEN $picked.lo AND $picked.hi` binds and runs. This
needs no knowledge of the chart spec, so it works before the chart compiler exists. FORMAT.md §2 now
states the rule. The cost is that a misspelt variable name reads as `NULL` instead of failing; the
graph step should report unknown variables as warnings.

### Query or effect is decided without a database

A cell is a query cell if its last statement returns rows. The first pass decides this with a small
lexer in `src/core`, not DuckDB's parser: it finds the statement's main keyword, looking past a
`WITH` clause. That keeps the decision available where there is no database (unit tests, and later
the browser). `json_serialize_sql` was considered as a classifier and rejected: it needs a
connection, and it reports `PIVOT` statements as not serialisable. The lexer is deliberately the
only place that scans SQL text; it tracks strings, nested block comments and dollar-quoted strings,
so a `-- %%` line inside a comment or string is not mistaken for a cell marker.

### The dependency graph comes from the parser, not the binder (S1)

`duckdb_get_table_names` binds, so it fails on statements whose tables do not exist yet, which is the
normal state of a notebook that has not run. The graph instead calls DuckDB's `Parser` and walks the
statement tree (`src/functions/references.cpp`). DuckDB 2.0 made this easier than expected: `INSERT`,
`UPDATE`, `DELETE`, `MERGE` and `COPY` are query nodes, and `ParsedExpressionIterator` walks them all,
so the walker only adds what the iterator skips: expression subqueries, the target of an `INSERT`,
`COPY` and `DROP` tables, `CREATE` and `ATTACH` names, `SET VARIABLE`, and `getvariable('…')`.

The split follows the rule above: the extension supplies one function, "what does this SQL define and
read", and everything else (resolving names to cells, ordering, duplicates, cycles, run order) is in
`src/core/graph.cpp` and unit-tested without a database. Findings:

- `PIVOT` parses into a multi-statement that creates an internal `__pivot_enum_…` type; the walker
  ignores it.
- `ATTACH 'x.duckdb'` without `AS` keeps its path as an expression in 2.0; the walker derives the
  alias `x` from it as DuckDB does.
- CTE names are collected per statement rather than per scope. A CTE that shadows a real table hides
  reads of that table elsewhere in the same statement; this has not mattered in practice.
- Markdown cells may refer to cells below them, so the runner evaluates a markdown cell only after
  everything it reads has run. `notebook_run` therefore returns rows in run order, not file order.
- A syntax error is a warning on the cell, not a format error: the notebook still runs and the cell
  fails on its own terms, as before.
- The unknown-variable warning promised above exists. Chart selections are definitions, so a cell
  reading `$picked_months` warns only if no chart selects into it.

### DuckDB 2.0 specifics worth knowing

- `Connection::Query` returns a `QueryResult` that keeps its connection busy until it is consumed or
  destroyed. Release one result before issuing the next query on the same connection.
- A cell with several statements can be sent as one string; the result is the last statement's.
- Running statements on a second connection from inside a table function works, including DDL.

## 12. What building steps 2 to 7 taught us

### A session is replayed, not held

The server must not hold a `Connection` (§11), yet a notebook session has temporary views and
variables that live in one. The first idea, holding the connection and releasing it when the user's
own connections close, does not work: DuckDB calls `OnConnectionClosed` with its connection-manager
lock held, so releasing a connection there deadlocks. Instead a session is data: its variables, read
back from the connection after each request, and per cell the statements that rebuild what a fresh
connection lacks (the temporary view of a query cell, `CREATE TEMP`, `SET`, `USE`). Each request
opens a connection, replays that, runs, and closes it. Replaying views is cheap because they are lazy;
`materialize=table` cells recompute, which is the cost of this model.

The server object lives in the database's object cache as a non-evictable entry. The cache is
destroyed with the database, which stops the server. Checked end to end: a shell running the server
exits, and the database file has no WAL left and opens in another process.

### The frontend has no build step

The design assumed a TypeScript app bundled with npm. That puts Node in the extension build, and
DuckDB's extension CI does not have it. The frontend is plain JavaScript and CSS instead, which CMake
compiles into the binary as byte arrays (`CMakeLists.txt`); editing a file re-runs CMake. The cost is
writing the renderers rather than using libraries (§6). The html-wasm runtime does need Node, so it is
built separately and optionally.

### JSON is enough for results (S2)

Results reach the browser after aggregation or as one page, so they are small. JSON needs nothing in
the browser to decode, keeps the server simple, and is what the exports embed (gzip-compressed). Large
integers beyond 2^53 travel as strings; temporal values as `YYYY-MM-DD HH:MM:SS` text.

### Variables need types from the browser

A value set from the browser is JSON, but a brush on a timestamp axis must compare as a timestamp.
The server derives the variable's SQL type from what produced it: an input's widget or `from=`
column, or the columns behind a chart selection's channels (`STRUCT(lo T, hi T)` for an interval).
Brushes on a `DATE` axis are typed `TIMESTAMP`, because they end between days.

### DuckDB-WASM is a version behind

The npm DuckDB-WASM is engine 1.5, so the html-wasm export rewrites `$name` to `getvariable('name')`
and writes its data file with `STORAGE_VERSION 'v1.5.0'`. One more difference surfaced: 1.5 cannot
read `.lo` from an untyped `NULL`, so selection variables start as `NULL::STRUCT(lo T, hi T)` there.
Arrow hands decimals over unscaled, and bundlers rename Arrow's classes, so the runtime converts by
`typeId` and scale.

### Smaller findings

- `to` is a reserved word, so the output parameter of `notebook_export` and `notebook_checkout` is
  `file :=`.
- Chart cells now run in `notebook_run`: the compiler is cheap to exercise headless, and a broken
  spec should fail CI like a broken query.
- The PDF export finds Chrome, Chromium, Edge or Playwright's `chrome-headless-shell`; the headless
  shell needs far fewer system libraries than full Chrome.

## 13. What the sketchbook look taught us

- **Hand-drawn is a renderer concern, not a format one.** Nothing in the file changed for it. A
  single primitives library (`render-sketch.js`) sits under the chart renderers, the cell outlines and
  the export viewer, so the look is consistent and could be swapped back to plain SVG in one place.
- **Seeded wobble.** Random wobble that changed on every redraw looked like flicker. Each chart and
  each cell outline seeds its own generator from stable inputs (title, data shape, cell position).
- **Outlines follow their content.** A cell's outline is an SVG drawn behind it at its measured size; a
  `ResizeObserver` redraws it when a result, a chart or a font load changes the size.
- **View settings that matter belong in the file.** Page size and hiding code or output change what
  readers of an export see, so the UI writes them as cell options (`rows=`, `hide=`) rather than
  keeping them in the browser. Only the default page size, a personal preference, stays in the
  browser (`localStorage`).
- **Value markers.** To highlight `{{ }}` results in prose, the runner wraps each value in two
  private-use characters (U+E000, U+E001) when asked; the markdown renderer turns them into `<mark>`,
  and the Markdown export strips them. `notebook_run` output is unchanged.
- **SVG attributes do not resolve CSS variables.** Hand-drawn ink is `currentColor`, with the colour
  set in CSS, so themes reach it.

## 14. Charts belong to their query

The first design made every chart a cell of its own, `-- %% chart from=<cell>`. In use, that meant
naming a query only so a chart could find it, and keeping two cells in step for one idea. Now a query
cell can end with `/* chart */` comments: the query says what data is fetched, the charts how it is
shown, and the table remains as the last tab. Chart cells stay for a chart of another cell or of a
table, and every existing notebook still parses.

- **The file is still plain SQL.** A chart is a block comment after the last statement, so DuckDB
  ignores it and `notebook_sql` leaves it out. The parser takes the comments off the body (`Cell::sql`
  is what runs, `Cell::charts` the specs), and the editor rebuilds the body from the two when either
  changes.
- **One relation for every chart.** `ChartFrom` gives the relation a cell's charts read: a chart cell's
  `from`, a named cell's view, or an unnamed cell's query in parentheses. The compiler and the runner
  treat chart cells and query charts alike, so `notebook_chart_query`, exports and DuckDB-WASM needed
  no new chart code.
- **Redraw without re-running.** Editing a spec, or a selection that only a cell's charts filter on,
  redraws that cell's charts (`/api/run` with `redraw`); its query and the cells below it do not run.
- **Selections without cycles.** An overview chart that selects and a detail chart that filters on the
  same query used to be two chart cells reading one view. As charts of one cell, or of cells in either
  order, a filter must not become a dependency, or the pair would form a cycle or a forward reference.
  A query cell's chart filters are therefore listed in `reads` but add no edge to `depends_on`. The
  one thing ruled out is a cell's SQL reading its own chart's selection.
- **A failing chart keeps its result.** The query ran, so its rows and row count stay; the cell is
  reported as an error with the chart's message, which shows in that chart's tab.
