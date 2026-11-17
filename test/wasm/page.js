// Runs in the test page (bundled by run.cjs with esbuild). Starts DuckDB-WASM from the files run.cjs
// serves and exposes a small API to the test: window.notebookTest.boot(bundle) and .sql(text).
import * as duckdb from '@duckdb/duckdb-wasm';

const BASE = location.origin + '/';

// Arrow values to plain JSON: BigInt to Number (or string beyond 2^53), lists and structs unpacked.
function plain(value) {
  if (value === null || value === undefined) return null;
  if (typeof value === 'bigint') {
    return value <= BigInt(Number.MAX_SAFE_INTEGER) && value >= BigInt(Number.MIN_SAFE_INTEGER) ? Number(value) : String(value);
  }
  if (value && typeof value.toArray === 'function' && !(value instanceof Uint8Array)) return Array.from(value.toArray(), plain);
  if (value && typeof value.toJSON === 'function') {
    const o = value.toJSON();
    if (o && typeof o === 'object') {
      for (const k of Object.keys(o)) o[k] = plain(o[k]);
    }
    return o;
  }
  return value;
}

let db = null;
let conn = null;

async function boot(bundleName) {
  const bundles = {
    base: { mainModule: BASE + 'duckdb-base.wasm', mainWorker: BASE + 'duckdb-browser-base.worker.js' },
    threads: {
      mainModule: BASE + 'duckdb-threads.wasm',
      mainWorker: BASE + 'duckdb-browser-threads.worker.js',
      pthreadWorker: BASE + 'duckdb-browser-threads.pthread.worker.js',
    },
  };
  const bundle = bundles[bundleName];
  const worker = new Worker(bundle.mainWorker);
  db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), worker);
  await db.instantiate(bundle.mainModule, bundle.pthreadWorker || null);
  // A local build is unsigned; DuckDB-WASM refuses unsigned extensions unless told otherwise.
  await db.open({ allowUnsignedExtensions: true });
  conn = await db.connect();
}

async function sql(text) {
  try {
    const table = await conn.query(text);
    const names = table.schema.fields.map((f) => f.name);
    const rows = [];
    for (let r = 0; r < table.numRows; r++) {
      const row = {};
      names.forEach((name, c) => (row[name] = plain(table.getChildAt(c).get(r))));
      rows.push(row);
    }
    return { rows };
  } catch (e) {
    return { error: String(e && e.message ? e.message : e) };
  }
}

async function registerText(name, text) {
  await db.registerFileText(name, text);
}

async function readFile(name) {
  const bytes = await db.copyFileToBuffer(name);
  return new TextDecoder().decode(bytes);
}

window.notebookTest = { boot, sql, registerText, readFile, isolated: () => self.crossOriginIsolated };
