import * as duckdb from '@duckdb/duckdb-wasm';
import * as arrow from 'apache-arrow';
const R = { steps: {}, t: {} }; window.__R = R;
const b64bytes = id => Uint8Array.from(atob(document.getElementById(id).textContent), c => c.charCodeAt(0));
const gunzip = async u8 => new Uint8Array(await new Response(new Blob([u8]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
const step = async (name, fn) => { const t0 = performance.now(); try { R.steps[name] = await fn(); } catch (e) { R.steps[name] = 'ERR ' + String(e.message || e).slice(0, 300); } R.t[name] = Math.round(performance.now() - t0); };
const rows = t => t.toArray().map(r => Object.fromEntries(Object.entries(r.toJSON()).map(([k, v]) => [k, typeof v === 'bigint' ? Number(v) : v])));
(async () => {
  R.protocol = location.protocol; R.origin = self.origin; R.coi = self.crossOriginIsolated;
  let db, conn;
  await step('boot', async () => {
    const wasm = await gunzip(b64bytes('wasm'));
    // Shim: the worker cannot fetch a page-created blob: URL under file:// (opaque origins differ),
    // so hand it the bytes by message and answer its fetch of a placeholder URL from memory.
    const SHIM = `
      const WASM_URL = 'https://inline.invalid/duckdb.wasm';
      let wasmBytes = null; const realFetch = self.fetch.bind(self);
      self.addEventListener('message', e => { if (e.data && e.data.__wasm) { wasmBytes = e.data.__wasm; e.stopImmediatePropagation(); } });
      self.fetch = (req, init) => {
        const url = typeof req === 'string' ? req : req.url;
        if (url === WASM_URL) return Promise.resolve(new Response(wasmBytes, { headers: { 'content-type': 'application/wasm', 'content-length': String(wasmBytes.byteLength) } }));
        return realFetch(req, init);
      };\n`;
    const workerUrl = URL.createObjectURL(new Blob([SHIM, b64bytes('worker')], { type: 'text/javascript' }));
    const worker = new Worker(workerUrl);
    worker.postMessage({ __wasm: wasm.buffer }, [wasm.buffer]);
    db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), worker);
    await db.instantiate('https://inline.invalid/duckdb.wasm');
    conn = await db.connect();
    return 'ok';
  });
  await step('select', async () => rows(await conn.query("SELECT 42 AS answer, version() AS v")));
  await step('arrow_ipc', async () => {
    const t = arrow.tableFromArrays({ k: ['a', 'b', 'a'], n: Float64Array.from([1, 2, 3]) });
    await conn.insertArrowFromIPCStream(arrow.tableToIPC(t, 'stream'), { name: 'ipc_t' });
    return rows(await conn.query("SELECT k, sum(n) AS s FROM ipc_t GROUP BY k ORDER BY k"));
  });
  await step('duckdb_file', async () => {
    await db.registerFileBuffer('data.duckdb', b64bytes('dbfile'));
    await conn.query("ATTACH 'data.duckdb' AS d (READ_ONLY)");
    return rows(await conn.query("SELECT region, count(*) AS n, round(avg(amount),2) AS a FROM d.orders GROUP BY region ORDER BY region"));
  });
  await step('parquet', async () => {
    await db.registerFileBuffer('orders.parquet', b64bytes('parquet'));
    return rows(await conn.query("SELECT region, count(*) AS n FROM read_parquet('orders.parquet') GROUP BY region ORDER BY region"));
  });
  await step('json_fn', async () => rows(await conn.query("SELECT json_extract('{\"a\":1}', '$.a')::VARCHAR AS j")));
  await step('dollar_var', async () => { await conn.query("SET VARIABLE region='EU'"); return rows(await conn.query("SELECT $region AS r")); });
  await step('pivot_etc', async () => rows(await conn.query("SELECT region, count(*) FILTER (amount > 250) AS big, median(amount) AS med FROM d.orders GROUP BY ALL ORDER BY region")));
  await step('variable', async () => { await conn.query("SET VARIABLE region='EU'"); return rows(await conn.query("SELECT getvariable('region') AS r")); });
  R.done = true;
})();
