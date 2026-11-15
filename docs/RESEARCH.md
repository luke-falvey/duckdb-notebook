# Research notes

Survey carried out on 2026-11-15 to inform [DESIGN.md](DESIGN.md).

**How to read the confidence of a claim.** Three kinds of evidence are used and marked:

- **(tested)** checked here against DuckDB 2.0.0 or Chromium.
- **(read)** taken from a page that was fetched and read, mostly on github.com.
- **(search)** taken from search-result summaries only. duckdb.org, motherduck.com, docs.marimo.io,
  observablehq.com and idl.uw.edu could not be fetched from the research environment, so most claims
  about those projects' documentation are of this kind.

§5 lists what remains unverified.

## 1. The DuckDB platform

### duckdb-ui (the `ui` extension)

- A C++ extension, MIT licensed, started with `duckdb -ui` or `CALL start_ui()`. It runs an HTTP server
  inside the DuckDB process, default port 4213. (read)
- Routes include `POST /ddb/run`, `/ddb/interrupt`, `/ddb/tokenize`, and a server-sent-events stream
  for catalog changes. **Every other path is proxied to `https://ui.duckdb.org`**, where the frontend
  bundle (about 8.8 MB) is hosted. The README says most of the UI code is not public. (read)
- Results use DuckDB's own binary serialisation, not Arrow. The repo publishes MIT TypeScript packages
  that decode it (`duckdb-ui-client`, `duckdb-data-reader`, and others). (read)
- Access control is an `Origin` header check; there is no token. (read)
- Notebooks are stored in `~/.duckdb/extension_data/ui/ui.db`, in a `notebook_versions` table with a
  JSON document per version; the current version is the row whose `expires` is null. (read for the
  path and JSON keys; search for the table layout)
- Open issues at the time of the survey: download a whole notebook
  ([#144](https://github.com/duckdb/duckdb-ui/issues/144)), git support and import
  ([#38](https://github.com/duckdb/duckdb-ui/issues/38)), charts
  ([#59](https://github.com/duckdb/duckdb-ui/issues/59)), offline use
  ([#62](https://github.com/duckdb/duckdb-ui/issues/62)), and notebook names reaching remote server
  logs through the proxy ([#258](https://github.com/duckdb/duckdb-ui/issues/258)). (read)

Takeaways: the extension-with-server architecture is proven and its source is a usable reference. The
gaps users report are exactly offline use, openness, charts, export and version control.

### DuckDB features the design relies on

| Feature | Finding | Evidence |
|---|---|---|
| Comment-only cell markers and block-comment bodies | Ignored by the parser, including `;` inside comments | tested |
| Nested block comments | Supported; an unbalanced `*/` is a parse error | tested |
| `SET VARIABLE` / `getvariable()` | Work; unset variable reads as `NULL` | tested |
| `$name` with no bound value | Resolves to the session variable of that name, in queries and inside views | tested (2.0.0 CLI and Python); not found in documentation |
| Views and variables | A view re-reads the variable on each query | tested |
| `STRUCT` and `LIST` variables | `$v.lo`, `x IN $list` work | tested |
| Scope | Variables and temp views are per connection | tested |
| Statement splitting | `extract_statements` returns statements with their types | tested |
| Table names of a query | `get_table_names` works for simple queries but binds, and so fails, for `JOIN … USING` and `INSERT` when tables do not exist | tested |
| `json_serialize_sql` | Gives a full AST with `BASE_TABLE` nodes; `SELECT` statements only | tested |
| `read_text` + `sha256` | Round-trips a notebook file into a table and back unchanged | tested |
| PEG parser default in 2.0, extensible by extensions | | search |
| Community extensions (`INSTALL x FROM community`) | Mechanism exists; submission process not checked | read / unverified |

### DuckDB-WASM

- Single-threaded by default; threads need cross-origin isolation headers. A subset of extensions
  loads on demand; spatial works. `httpfs` needs CORS on every resource. (read)
- Memory is capped at 4 GB by WebAssembly. Databases can persist in the browser's origin-private file
  system. A `.duckdb` or Parquet file on a static host can be attached and read by HTTP range
  requests. (search)
- Download size: third-party figures of roughly 6–12 MB compressed, which conflict with each other.
  (search, unverified)

### Remote access

- **Quack**, DuckDB's own client-server protocol (`quack_serve`, `ATTACH 'quack:host'`), connects one
  DuckDB to another over HTTP. No browser client was found, so it does not replace an HTTP API for a
  web frontend. (read)
- Mosaic maintains DuckDB data servers in Python, Rust and Go that return Arrow over HTTP. (read)

### Architectures compared

| Architecture | For | Against |
|---|---|---|
| **Extension with embedded server** (chosen) | One `INSTALL`; runs in the user's process with their databases, secrets and version; engine internals available | C++; rebuild per DuckDB release |
| Standalone binary linking libduckdb | Easiest to build; full control of HTTP | Second DuckDB version; database file lock conflicts |
| Browser only, DuckDB-WASM | No backend; editor and export are the same thing | 4 GB, one thread, CORS, partial extensions, awkward local files |
| Desktop app | Native engine, file access | Packaging, signing, updates |

## 2. Notebook formats

| Format family | Examples | Diff and merge | Plain-editor editing | SQL tooling | Outputs |
|---|---|---|---|---|---|
| JSON document | Jupyter `.ipynb`, Zeppelin `.zpln` | Poor; outputs in the diff | Poor | None (code is escaped strings) | Inline |
| Markdown with fenced code | Evidence, Quarto, Livebook, Observable Framework | Good | Good | Highlighting inside fences only | Sidecar |
| **Host-language source with comment markers** | marimo `.py`, Pluto `.jl`, Jupytext `# %%`, Databricks `-- COMMAND` | Very good | Excellent | Full: the file is real code | Sidecar or none |
| HTML | Observable Notebook Kit | Good | Fair | Limited | Sidecar |
| YAML | Hex, Deepnote | Good | Fair | Weak | Excluded |
| Directory, one file per cell | Rill, dbt-style projects | Excellent | Excellent | Full | Sidecar |

Points that shaped the design:

- **marimo** stores a notebook as a Python file and never stores outputs in it; a separate session
  snapshot holds them. Users cite this as the reason it works in git. Its main criticism is that the
  one-definition rule and forced reactivity get in the way of messy exploration, which is why this
  design offers a lazy mode and treats effect cells separately. ([format](https://marimo.io/blog/python-not-json),
  [discussion](https://hn.svelte.dev/item/44801392)) (search)
- **Databricks** source format shows the SQL-with-comment-markers idea at scale: cells split by
  `-- COMMAND ----------`, non-SQL cells as `-- MAGIC` lines. ([docs](https://learn.microsoft.com/azure/databricks/notebooks/notebook-format)) (search)
- **Stable cell identity** is handled by an explicit ID field (Jupyter 4.5+, Observable Notebook Kit),
  a UUID in the cell marker (Pluto), or the cell's name or content (marimo, Clerk). Jupytext keeps no
  IDs and loses them on round trip. Using the cell name avoids both generated noise and lost identity.
- **Outputs in a sidecar**, keyed by a content hash, is the common answer to "clean diffs but instant
  reopen": marimo's `__marimo__/session`, Quarto's `_freeze/`, Observable's `.observable/cache`,
  Clerk's `.clerk/`. (search)
- **Agents** edit line-oriented text reliably and JSON notebooks badly; several write-ups recommend
  pairing Jupyter notebooks with a text form for that reason.
  ([example](https://www.zonca.dev/posts/2025-12-11-jupytext-ai-agents)) (search)

## 3. Notebook environments

| Tool | Execution | What to borrow | What to avoid |
|---|---|---|---|
| **marimo** | Reactive graph from static analysis | No hidden state; inputs as graph nodes; lazy mode; script and app from one file | Reactivity with no escape for side effects |
| **Zeppelin** | Paragraph at a time | Chart switcher under every result; dynamic forms bound to query parameters | JSON notes with results inline |
| **duckdb-ui** | Cell at a time | Column profiler; catalog browser; server inside the engine | Closed, remotely hosted frontend |
| **Evidence** | Build-time SQL, then DuckDB-WASM in the browser | SQL plus declarative components; static site | Data only as fresh as the last build; large Parquet bundles |
| **Observable Framework / Notebook Kit** | Reactive JavaScript; SQL cells | Static build with cached query results; open file format | Dynamic queries need a live database or WASM |
| **Livebook** | Linear with stale tracking | Stale markers; diff-friendly file | |
| **Pluto** | Reactive | Static HTML export with precomputed slider states | Cells saved in dependency order, which makes diffs noisy |
| **Quarto** | Render pipeline | Many output formats from one source; frozen results committed for CI | |
| **Hex, Count** | Reactive graph; Count on a canvas | Graph view of cells; chart cells configured by UI | Hosted only |
| **Mosaic** | Chart clients issue SQL through a coordinator | Aggregation pushed into DuckDB; cross-filter selections as SQL predicates | 0.x API with frequent breaking changes |
| **Rill, Malloy** | BI as code | SQL and YAML in git as the product surface | |

Recurring wishes in 2024–2026 discussion: clean git history, reproducibility without giving up quick
exploration, caching so that undoing an edit does not recompute, formats agents can edit, sharing as a
static app or report, and parameterised scheduled runs. (search)

## 4. Visualisation and export

### Libraries

| Library | Licence | Role considered | Notes |
|---|---|---|---|
| D3 | ISC | Layouts and scales inside built-in charts | Too low-level to be the stored spec |
| Observable Plot | ISC | Default renderer for marks on axes | SVG; concise; still 0.x; needs pre-aggregated data for large tables |
| Vega-Lite | BSD-3 | Raw spec escape hatch | Stable JSON schema, widely known; transforms run in the browser |
| Mosaic / vgplot | BSD-3 (unverified) | Source of the pushdown approach; candidate for WASM export | v0.35.0 on 2026-11-03; breaking changes in 0.32 and 0.34 (read) |
| Apache ECharts | Apache-2.0 | Funnel, candlestick, gauge, calendar | 6.x; broad catalogue; SVG renderer |
| Plotly.js | MIT | Not chosen | About 1.4 MB minified |
| Highcharts | Commercial | Excluded | Free only for non-commercial use |
| deck.gl + MapLibre | MIT / BSD-3 | Maps | GeoArrow layers take Arrow from DuckDB spatial directly |
| kepler.gl | MIT | Reference | Uses DuckDB-WASM since 3.1 |
| cosmos.gl | MIT | Networks | Cosmograph, the product on top, is CC-BY-NC |
| Perspective | Apache-2.0 | Pivot alternative | 5.0 can drive DuckDB as a virtual server |
| Graphic Walker | Apache-2.0 | Reference for drag-and-drop chart building | Stores a JSON spec |
| Quak | MIT | Reference for the column profiler | Every view is a SQL query |

**Why the notebook owns its chart spec.** Vega-Lite is the most stable declarative grammar, but its
transforms run in the browser, and the server-side accelerator for it (VegaFusion) targets DataFusion
and measured slower than Mosaic at millions of rows
([Mosaic Selections paper](https://arxiv.org/pdf/2507.19690)). Mosaic pushes work into DuckDB but its
spec is still moving. A small spec owned by the notebook can be compiled to SQL by the extension and
drawn by whichever library fits, and survives a change of library.

**Large data techniques.** M4 downsampling keeps the minimum, maximum, first and last value per pixel
column, so a line chart needs about four rows per pixel of width whatever the table size
([Jugel et al., 2014](https://datavis.cs.columbia.edu/files/papers/m4.pdf)). Scatter and heat marks
bin to the pixel grid with `GROUP BY`. Both are plain SQL.

### Export to HTML without DuckDB

| Approach | Typical size | Interactivity | Works from `file://` |
|---|---|---|---|
| Static snapshot with data inlined | 0.05–0.5 MB plus data | Hover, sort, page | Yes |
| Snapshot plus precomputed input states | 0.3–3 MB | Declared inputs | Yes |
| DuckDB-WASM plus Parquet | 8–15 MB plus data | Full SQL | Not as shipped by existing tools; single-file inlining unproven |

Prior art: Quarto `embed-resources`, marimo HTML export, Pluto static export (snapshot); Pluto slider
server (precomputed states); Evidence, Observable Framework, marimo WASM export, kepler.gl (WASM,
loading it from a CDN or sibling files).

### Export to PDF

| Approach | Fidelity | Dependency |
|---|---|---|
| Browser print with a print stylesheet | Good; SVG stays vector | None |
| Headless Chromium `--print-to-pdf` | Same, scriptable | An installed browser (tested: script-drawn SVG is captured, text stays vector) |
| Typst with SVG charts | Best typography | Typst, plus a browser to make the SVG |
| LaTeX / Pandoc | High | Heavy |

marimo and Jupyter (`nbconvert --to webpdf`) both use Chromium. Evidence and Observable rely on browser
print. Quarto bundles Typst. (search)

## 5. Not verified

- Whether `$name` resolving to a session variable is a documented, committed DuckDB 2.0 feature. It
  was observed in 2.0.0 only.
- DuckDB-WASM download sizes, and whether it can be instantiated from inlined bytes under `file://`.
- The community-extension submission and signing process, and whether extensions that embed a web
  server and a large asset bundle are accepted.
- Whether this extension could be built for DuckDB-WASM.
- Mosaic's licence, and details of its documentation (the site could not be fetched).
- The full schema of duckdb-ui's `ui.db`.
- Licences marked from memory in the survey rather than checked: D3, Plot, deck.gl, MapLibre,
  Perspective, cosmos.gl.
- Details of marimo's and Observable's current documentation, which were seen only through search
  summaries.
- Tools named in the brief but not researched in depth: Datalore, Colab, Mode, Briefer, Curvenote,
  Org-babel, srcbook, Streamlit, SQL Workbench, DBeaver.

## 6. Main sources

- duckdb-ui: [repository](https://github.com/duckdb/duckdb-ui),
  [http_server.cpp](https://github.com/duckdb/duckdb-ui/blob/main/src/http_server.cpp),
  [notebook JSON discussion](https://github.com/duckdb/duckdb-ui/discussions/85)
- DuckDB: [duckdb-wasm](https://github.com/duckdb/duckdb-wasm),
  [duckdb-quack](https://github.com/duckdb/duckdb-quack),
  [community-extensions](https://github.com/duckdb/community-extensions),
  [2.0 announcement](https://duckdb.org/2026/10/21/announcing-duckdb-200)
- marimo: [Python, not JSON](https://marimo.io/blog/python-not-json),
  [dataflow](https://marimo.io/blog/dataflow), [exporting](https://docs.marimo.io/guides/exporting/)
- Observable: [Notebook Kit](https://github.com/observablehq/notebook-kit),
  [Notebooks 2.0 review](https://macwright.com/2025/07/31/observable-notebooks-2),
  [Framework SQL](https://observablehq.com/framework/sql)
- Zeppelin: [display system](https://github.com/apache/zeppelin/blob/master/docs/usage/display_system/basic.md),
  [dynamic forms](https://zeppelin.apache.org/docs/0.9.0/usage/dynamic_form/intro.html)
- Others: [Evidence universal SQL](https://evidence.dev/blog/why-we-built-usql),
  [Quarto execution and freeze](https://quarto.org/docs/projects/code-execution.html),
  [Livebook](https://hexdocs.pm/livebook/readme.md), [Pluto export](https://plutojl.org/en/docs/export/),
  [Jupyter cell IDs](https://jupyter.org/enhancement-proposals/cell-id),
  [Deepnote format](https://deepnote.com/docs/deepnote-format),
  [Hex execution model](https://learn.hex.tech/docs/explore-data/projects/compute-model/execution-model)
- Visualisation: [Mosaic](https://github.com/uwdata/mosaic), [Mosaic paper](https://idl.uw.edu/papers/mosaic),
  [D3](https://d3js.org/), [Quak](https://github.com/manzt/quak), [deck.gl](https://deck.gl/docs/whats-new),
  [anywidget AFM](https://anywidget.dev/en/afm)
