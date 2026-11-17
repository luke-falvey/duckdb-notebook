# Examples

These are real HTML exports of the notebooks in the repository, made with
`notebook_export` by the extension this site documents. Each is a single file that runs in your
browser with no server. Open one, then look at the `.sql` file it came from.

<div class="cards" markdown>
<a class="card" href="revenue.html"><strong>Revenue review</strong><span>Prose with live values, inputs, a brushable trend, a KPI and a heatmap. The Region input works: the export precomputes each region.</span></a>
<a class="card" href="charts.html"><strong>Chart gallery</strong><span>Every chart type the notebook draws, from bars to sankeys, on one page.</span></a>
<a class="card" href="hello.html"><strong>Hello, notebook</strong><span>The three-cell notebook from <em>your first notebook</em>.</span></a>
</div>

| Export | Notebook | Made with |
|---|---|---|
| [revenue.html](revenue.html) | [`examples/revenue.sql`](https://github.com/luke-falvey/duckdb-notebook/blob/main/examples/revenue.sql) | `notebook_export('examples/revenue.sql', file := 'revenue.html', interactive := 'precomputed')` |
| [charts.html](charts.html) | [`examples/charts.sql`](https://github.com/luke-falvey/duckdb-notebook/blob/main/examples/charts.sql) | `notebook_export('examples/charts.sql', file := 'charts.html')` |
| [hello.html](hello.html) | [`site/notebooks/hello.sql`](https://github.com/luke-falvey/duckdb-notebook/blob/main/site/notebooks/hello.sql) | `notebook_export('hello.sql', file := 'hello.html')` |

Press **✎ watch** in an export's header (or add `?ink=watch`) to watch one pen write it, add `?ink=off`
to see it without the handwriting animation, or `#present` to
present it one screen at a time: try [revenue.html#present](revenue.html#present).

## Not published here: a live DuckDB-WASM export

`format := 'html-wasm'` makes a page that runs the notebook's SQL in the browser, so every input and
chart selection works. Each such page carries DuckDB-WASM (the revenue example comes to 12.5 MB),
which is too heavy to keep regenerating in the repository for this site. Make one yourself with the steps in
[exporting](../guides/exporting.md#inputs-that-still-work).

## The notebooks behind the docs

The other notebooks shown in these pages live in
[`site/notebooks/`](https://github.com/luke-falvey/duckdb-notebook/tree/main/site/notebooks).
The repository's tests check that each one parses without errors, so the examples on this site
stay valid as the extension changes.

## As it is written

The same revenue notebook, written onto the page by the pen as it opens:

<img src="../images/handwriting/revenue.gif" alt="The revenue notebook being written onto the page" width="720">

And the chart gallery:

<img src="../images/handwriting/charts.gif" alt="The chart gallery being drawn, chart by chart" width="720">
