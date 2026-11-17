# Settings

There are three kinds of settings, kept in three places on purpose: what the notebook says (in the
file, shared with everyone), what a reader prefers (in their browser), and how the extension finds
other programs (environment variables). The extension adds no DuckDB `SET` options.

## In the notebook file

These travel with the notebook and are reviewed like any edit.

**The `[notebook]` cell** (YAML, first cell only):

| Key | Values | Default | Effect |
|---|---|---|---|
| `title` | text | the file name | Shown in the editor's toolbar and as the export's title |
| `format` | `1` | | The format version |
| `attach` | list of `{path, as, read_only}` | | Databases attached before the first cell runs (`ATTACH IF NOT EXISTS`). Paths are relative to the notebook |
| `extensions` | list of names | | Extensions loaded before the first cell runs |
| `timing` | `true`, `false` | `true` | Show each cell's run time in the editor. The ⚙ menu's **timing** switch writes it |
| `cache` | `true`, `false` | `true` | Keep each cell's last result so the notebook reopens showing it ([keeping results](../guides/cache.md)). `false` keeps none and removes what was kept. The ⚙ menu's **keep results** switch writes it |

**Options on a cell's `-- %%` line:**

| Option | Cells | Values | Default |
|---|---|---|---|
| `hide` | all | `code`, `output`, `all`, `none` | `none` |
| `span` | all | `1`–`12` | `12` |
| `rows` | all | `1`–`10000` | the reader's default, 25 |
| `screen` | all | `new`, `join`, `skip` | the reading layout decides ([presenting](../guides/reading.md#presenting)) |
| `run` | SQL | `auto`, `manual` | `auto` for queries, `manual` for effects |
| `materialize` | SQL | `view`, `table` | `view` |
| `width`, `height` | SQL with charts, chart | pixels | `640`, `320` |
| `from` | chart | a cell, table or view | required |
| `label` | input | text | the variable name |
| `placeholder` | `text` input | text | |
| `min`, `max`, `step` | `number`, `slider`; `min`/`max` on `date`, `daterange` | numbers or dates | |
| `options` | `select`, `multiselect` | `a,b,c` | |
| `from` | `select`, `multiselect` | `<cell>.<column>` | |

`hide`, `rows` and `screen` are checked: any other value is an error on the cell. The full rules are in the
[format reference](format.md).

## In the reader's browser

Kept in the browser's local storage, per browser, never in the file:

| Setting | Where | Values | Default |
|---|---|---|---|
| Rows per page | ⚙ → rows per page | 10 to 1,000 | 25 |
| Theme | ⚙ → ◐ | light, dark | light |
| Handwriting | ⚙ → handwriting | `gentle`, `watch`, `off` (a remembered `slow` from older versions reads as `gentle`) | `gentle` |
| Watch speed | the − and + controls in watch mode | 0.25× to 8× | 1× |
| Peek | ⚙ → peek | on, off | on |
| Auto-run | ⚙ → auto-run | on, off | on (not remembered between visits) |

Adding `ink=off` to a page's URL turns the handwriting off for that visit, in the editor and in HTML
exports; `ink=watch` opens it in [watch mode](../guides/reading.md#watch-mode). It is also off when the system asks for reduced motion.

## Environment variables

Read by the extension in the DuckDB process:

| Variable | Used by | Meaning |
|---|---|---|
| `NOTEBOOK_CHROME` | `notebook_export(format := 'pdf')` | The browser that prints PDFs, if `chrome :=` is not given |
| `NOTEBOOK_WASM_RUNTIME` | `notebook_export(format := 'html-wasm')` | The DuckDB-WASM runtime folder, if `wasm_runtime :=` is not given |
| `DISPLAY`, `WAYLAND_DISPLAY` | `notebook_start` on Linux | Without either, the browser is not opened; use the returned URL |
| `TMPDIR` | `html-wasm` exports | Where the data copied into the page is staged |

## Function arguments

Each function's named arguments (ports, output paths, formats) are listed in
[SQL functions](functions.md).
