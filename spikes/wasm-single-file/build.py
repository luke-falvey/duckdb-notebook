"""Build single.html: DuckDB-WASM, its worker, the app and test data in one file."""
import base64, gzip, subprocess, duckdb

DIST = "node_modules/@duckdb/duckdb-wasm/dist"
b64 = lambda data: base64.b64encode(data).decode()
read = lambda path: open(path, "rb").read()

# Test data, written in a storage version the WASM engine (v1.5.x) can read.
subprocess.run(["rm", "-f", "data.duckdb"])
con = duckdb.connect()
con.execute("ATTACH 'data.duckdb' AS o (STORAGE_VERSION 'v1.5.0')")
con.execute("""CREATE TABLE o.orders AS
    SELECT i AS id, ['EU', 'US', 'APAC'][1 + i % 3] AS region, (i * 7919 % 500)::DOUBLE AS amount
    FROM range(200000) t(i)""")
con.execute("COPY o.orders TO 'orders.parquet' (FORMAT parquet, COMPRESSION zstd)")
con.execute("DETACH o")

subprocess.run(["npx", "esbuild", "app.js", "--bundle", "--format=iife", "--minify",
                "--outfile=app.bundle.js", "--log-level=warning"], check=True)
app = open("app.bundle.js").read().replace("</script", "<\\/script")

html = f"""<!doctype html><meta charset="utf-8"><title>DuckDB-WASM single-file spike</title>
<script type="application/octet-stream" id="wasm">{b64(gzip.compress(read(f"{DIST}/duckdb-eh.wasm"), 9))}</script>
<script type="application/octet-stream" id="worker">{b64(read(f"{DIST}/duckdb-browser-eh.worker.js"))}</script>
<script type="application/octet-stream" id="dbfile">{b64(read("data.duckdb"))}</script>
<script type="application/octet-stream" id="parquet">{b64(read("orders.parquet"))}</script>
<body><script>{app}</script>"""
open("single.html", "w").write(html)
print(f"single.html: {len(html) / 1e6:.1f} MB")
