import * as duckdb from '@duckdb/duckdb-wasm';

// Served by your site: DuckDB-WASM 2.0's base bundle (from node_modules/@duckdb/duckdb-wasm/dist)
// and the extension built for wasm_eh.
const DUCKDB_MODULE = '/duckdb/duckdb-base.wasm';
const DUCKDB_WORKER = '/duckdb/duckdb-browser-base.worker.js';
const NOTEBOOK_EXTENSION = new URL('/extensions/notebook.duckdb_extension.wasm', location.href).href;

export async function openNotebookDuckDB() {
  const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), new Worker(DUCKDB_WORKER));
  await db.instantiate(DUCKDB_MODULE);
  await db.open({ allowUnsignedExtensions: true });
  const conn = await db.connect();
  await conn.query(`LOAD '${NOTEBOOK_EXTENSION}'`);
  return { db, conn };
}
