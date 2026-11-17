// The DuckDB-WASM runtime inlined into an html-wasm export (docs/DESIGN.md section 7). Bundled by
// wasm/build.sh into runtime.js, which sets window.NotebookWasm for frontend/viewer.js.
//
// A page opened from file:// has an opaque origin, so its worker cannot fetch a blob: URL the page
// made and DuckDB-WASM's stock loader hangs. The shim below hands the worker the module's bytes by
// message and answers its fetch of a placeholder URL from memory (see spikes/wasm-single-file).
import * as duckdb from '@duckdb/duckdb-wasm';

const MODULE_URL = 'https://inline.invalid/duckdb.wasm';
const SHIM = `
  const WASM_URL = '${MODULE_URL}';
  let wasmBytes = null; const realFetch = self.fetch.bind(self);
  self.addEventListener('message', e => { if (e.data && e.data.__wasm) { wasmBytes = e.data.__wasm; e.stopImmediatePropagation(); } });
  self.fetch = (req, init) => {
    const url = typeof req === 'string' ? req : req.url;
    if (url === WASM_URL) return Promise.resolve(new Response(wasmBytes, { headers: { 'content-type': 'application/wasm', 'content-length': String(wasmBytes.byteLength) } }));
    return realFetch(req, init);
  };
`;

function pad(n) { return (n < 10 ? '0' : '') + n; }

// Values as the extension's JSON shows them, so the same renderer draws both.
function plain(value, typeId, scale) {
  if (value === null || value === undefined) return null;
  if (typeof value === 'bigint') return Number(value);
  if (value instanceof Date || typeId === 'Timestamp' || typeId === 'Date') {
    const d = new Date(typeof value === 'object' && !(value instanceof Date) ? Number(value) : value);
    const date = d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate());
    if (typeId === 'Date') return date;
    return date + ' ' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + ':' + pad(d.getUTCSeconds());
  }
  // Arrow hands decimals over unscaled.
  if (typeId === 'Decimal') return Number(String(value)) / Math.pow(10, scale || 0);
  if (value && typeof value.toArray === 'function') return Array.from(value.toArray(), (v) => plain(v));
  if (value && typeof value.toJSON === 'function') {
    const o = value.toJSON();
    if (o && typeof o === 'object') { for (const k of Object.keys(o)) o[k] = plain(o[k]); return o; }
    return o;
  }
  return value;
}

// Arrow's Type enum: the bundler renames classes, so match on typeId, not constructor names.
function typeIdOf(type) {
  switch (type && type.typeId) {
    case 7: return 'Decimal';
    case 8: return 'Date';
    case 10: return 'Timestamp';
    default: return '';
  }
}

async function boot(moduleBytes, workerSource) {
  const workerUrl = URL.createObjectURL(new Blob([SHIM, workerSource], { type: 'text/javascript' }));
  const worker = new Worker(workerUrl);
  worker.postMessage({ __wasm: moduleBytes.buffer }, [moduleBytes.buffer]);
  const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), worker);
  await db.instantiate(MODULE_URL);
  const conn = await db.connect();
  return {
    version: async () => (await conn.query('SELECT version() AS v')).toArray()[0].v,
    registerFile: (name, bytes) => db.registerFileBuffer(name, bytes),
    query: async (sql) => {
      const table = await conn.query(sql);
      const fields = table.schema.fields;
      const ids = fields.map((f) => typeIdOf(f.type));
      const columns = fields.map((f) => ({ name: f.name, type: String(f.type) }));
      const rows = [];
      for (let r = 0; r < table.numRows; r++) {
        rows.push(fields.map((f, c) => plain(table.getChildAt(c).get(r), ids[c], f.type.scale)));
      }
      return { columns, rows, total: rows.length };
    },
  };
}

window.NotebookWasm = { boot };
