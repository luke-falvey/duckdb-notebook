# Spike: DuckDB-WASM in a single HTML file

Question (S4 in [DESIGN.md](../../docs/DESIGN.md#8-risks-and-spikes)): can a notebook be exported as
**one** HTML file that runs real SQL in the reader's browser, opened by double-click (`file://`), with
no network?

**Answer: yes, with a small shim.** Tested in headless Chromium only.

## Result

`single.html` is 13.6 MB and contains the DuckDB-WASM module (gzip, base64), its worker script, the
app bundle with Apache Arrow, and a 0.8 MB `.duckdb` file. Opened from `file://` with every network
request blocked:

| Step | Time | Outcome |
|---|---|---|
| Decompress and start DuckDB-WASM | 1.4 s | Works |
| `SELECT 42, version()` | 30 ms | Works. The engine reports **v1.5.4** |
| Load an Arrow IPC stream and aggregate it | 53 ms | Works |
| `ATTACH` an inlined `.duckdb` file (200,000 rows) and aggregate | 72 ms | Works, if the file was written with `STORAGE_VERSION 'v1.5.0'` |
| `SET VARIABLE` + `getvariable()` | 3 ms | Works |
| `SET VARIABLE` + `$name` | | Fails: the shorthand needs DuckDB 2.0 |
| `read_parquet` on an inlined Parquet file | | Fails: `parquet` is an extension fetched from `extensions.duckdb.org` |
| `json_extract` | | Fails for the same reason |

## What it took

The stock loader does not work from `file://`. A page opened that way has an opaque origin, and its
worker cannot fetch a `blob:` URL the page created, so `db.instantiate(blobUrl)` hangs. The shim in
[`app.js`](app.js) prepends a few lines to the worker script: it receives the module bytes by
`postMessage` and answers the worker's `fetch` of a placeholder URL from memory. No change to
DuckDB-WASM itself is needed.

## What this means for the export

- A single-file, offline, fully interactive export is feasible. The fixed cost is about 12 MB.
- Ship data as a `.duckdb` file or Arrow IPC, not Parquet, so nothing has to be fetched.
- The exporter must write the data file in a storage version the bundled WASM engine can read.
- Until a 2.0-based DuckDB-WASM is on npm, exported SQL must use `getvariable('name')` in place of
  `$name`.
- JSON functions, Parquet and `spatial` are unavailable offline unless their extension binaries are
  inlined too, which was not attempted.

## Not tested

- Firefox and Safari.
- Inlining extensions.
- Why Parquet and JSON also failed with the network available. In that run the extension download
  was attempted through this environment's proxy and the load failed with a WebAssembly error; the
  cause was not investigated.
- Large data files. Base64 text and the decoded bytes are both held in memory during start-up.

## Reproduce

Needs Node, Python with the `duckdb` package, and Playwright with Chromium.

```bash
npm install
```

```bash
python3 build.py
```

```bash
node run.js
```
