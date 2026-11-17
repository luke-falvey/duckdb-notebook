// Loads the notebook extension's WebAssembly build into DuckDB-WASM in headless Chromium and runs
// notebook_cells, notebook_sql, notebook_chart_query and notebook_run on examples/revenue.sql.
// See test/wasm/README.md. No server is started: Playwright answers every request of the page
// (and its workers) from local files, under a made-up origin.
//
//   node test/wasm/run.cjs [wasm_eh] [wasm_threads]
//
// Each architecture needs `make wasm_eh` / `make wasm_threads` first. With no argument, every
// architecture that has been built is tested.
'use strict';
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');
const esbuild = require('esbuild');

const ROOT = path.resolve(__dirname, '..', '..');
const DIST = path.join(__dirname, 'node_modules', '@duckdb', 'duckdb-wasm', 'dist');
const ORIGIN = 'http://notebook.test';

// DuckDB-WASM 2.0 has two bundles: base (platform wasm_eh) and threads (wasm_threads). There is no
// bundle for wasm_mvp any more, so a wasm_mvp extension has nothing to load into.
const ARCHS = {
  wasm_eh: { bundle: 'base' },
  wasm_threads: { bundle: 'threads' },
};

function extensionPath(arch) {
  return path.join(ROOT, 'build', arch, 'extension', 'notebook', 'notebook.duckdb_extension.wasm');
}

const CONTENT_TYPES = { '.js': 'text/javascript', '.wasm': 'application/wasm', '.html': 'text/html', '.sql': 'text/plain' };

async function testArch(browser, arch, pageScript) {
  const failures = [];
  const check = (name, ok, detail) => {
    console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${ok || detail === undefined ? '' : ': ' + JSON.stringify(detail)}`);
    if (!ok) failures.push(name);
  };
  const files = {
    '/index.html': '<!doctype html><meta charset="utf-8"><title>notebook wasm test</title><script src="page.js"></script>',
    '/page.js': pageScript,
    '/notebook.duckdb_extension.wasm': extensionPath(arch),
  };
  for (const name of fs.readdirSync(DIST)) {
    if (/\.(wasm|worker\.js)$/.test(name)) files['/' + name] = path.join(DIST, name);
  }
  const context = await browser.newContext();
  await context.route(ORIGIN + '/**', (route) => {
    const url = new URL(route.request().url());
    const file = files[url.pathname];
    if (file === undefined) return route.fulfill({ status: 404, body: 'not found' });
    const body = file.startsWith('/') && fs.existsSync(file) ? fs.readFileSync(file) : file;
    // Cross-origin isolation, which the threads bundle needs for SharedArrayBuffer.
    route.fulfill({
      status: 200,
      body,
      headers: {
        'content-type': CONTENT_TYPES[path.extname(url.pathname)] || 'application/octet-stream',
        'cross-origin-opener-policy': 'same-origin',
        'cross-origin-embedder-policy': 'require-corp',
      },
    });
  });
  const page = await context.newPage();
  page.on('pageerror', (e) => console.log('  page error:', e.message));
  await page.goto(ORIGIN + '/index.html');
  const t0 = Date.now();
  await page.evaluate((bundle) => window.notebookTest.boot(bundle), ARCHS[arch].bundle);
  console.log(`  DuckDB-WASM started in ${Date.now() - t0} ms`);
  const sql = (text) => page.evaluate((t) => window.notebookTest.sql(t), text);

  const info = await sql("SELECT version() AS version, (SELECT platform FROM pragma_platform()) AS platform");
  const engine = info.rows && info.rows[0];
  check('engine is DuckDB v2.0.0', engine && engine.version === 'v2.0.0', info);
  check(`engine platform is ${arch}`, engine && engine.platform === arch, info);

  const t1 = Date.now();
  const load = await sql(`LOAD '${ORIGIN}/notebook.duckdb_extension.wasm'`);
  check('LOAD notebook', !load.error, load.error);
  if (load.error) {
    await context.close();
    return failures;
  }
  console.log(`  extension loaded in ${Date.now() - t1} ms`);

  await page.evaluate(
    ([name, text]) => window.notebookTest.registerText(name, text),
    ['revenue.sql', fs.readFileSync(path.join(ROOT, 'examples', 'revenue.sql'), 'utf8')],
  );

  const cells = await sql(
    'SELECT idx, kind, name, len(errors) AS errors, len(charts) AS charts, depends_on FROM notebook_cells(\'revenue.sql\') ORDER BY idx',
  );
  check('notebook_cells', cells.rows && cells.rows.length > 5 && cells.rows.every((r) => r.errors === 0), cells);
  const monthly = (cells.rows || []).find((r) => r.name === 'monthly');
  check('notebook_cells: monthly has charts and depends on filtered', monthly && monthly.charts > 0 &&
    (monthly.depends_on || []).includes('filtered'), monthly);

  const script = await sql("SELECT sql FROM notebook_sql('revenue.sql')");
  check('notebook_sql', script.rows && /CREATE OR REPLACE TABLE orders/.test(script.rows[0].sql) &&
    /CREATE OR REPLACE TEMP VIEW monthly|monthly/.test(script.rows[0].sql), script.error);

  await sql("CREATE TABLE nb_chart_t AS SELECT range AS i, range % 3 AS g FROM range(100)");
  const chart = await sql("SELECT len(queries) AS n, queries[1] AS q, render FROM notebook_chart_query('mark: bar\nx: g\ny: {field: i, agg: sum}', 'nb_chart_t')");
  const compiled = chart.rows && chart.rows[0];
  check('notebook_chart_query compiles', compiled && compiled.n === 1 && /"mark":"bar"/.test(compiled.render), chart);
  if (compiled) {
    const data = await sql(compiled.q);
    check('compiled chart query runs', data.rows && data.rows.length === 3, data);
  }

  const run = await sql(
    "SELECT idx, kind, name, status, error, row_count FROM notebook_run('revenue.sql', params := {region: 'US'}) ORDER BY idx",
  );
  check('notebook_run: every cell ok or skipped', run.rows && run.rows.length > 5 &&
    run.rows.every((r) => r.status === 'ok' || r.status === 'skipped'), run);
  check('notebook_run: tables it creates remain', !(await sql('SELECT count(*) AS n FROM orders')).error);

  const md = await sql("SELECT bytes FROM notebook_export('revenue.sql', file := 'revenue.md', format := 'md')");
  check('notebook_export md', md.rows && md.rows[0].bytes > 0, md);
  if (md.rows) {
    const text = await page.evaluate(() => window.notebookTest.readFile('revenue.md'));
    check('notebook_export md: written to the virtual file system', /Revenue review/.test(text), text.slice(0, 200));
  }

  for (const [name, query] of [
    ['notebook_start', 'SELECT * FROM notebook_start()'],
    ['notebook_stop', 'SELECT * FROM notebook_stop()'],
    ['notebook_export pdf', "SELECT * FROM notebook_export('revenue.sql', file := 'r.pdf', format := 'pdf')"],
  ]) {
    const result = await sql(query);
    check(`${name} fails with a clear message`, result.error && /not available in this build|cannot do/.test(result.error), result);
  }

  await context.close();
  return failures;
}

(async () => {
  let archs = process.argv.slice(2);
  if (archs.length === 0) archs = Object.keys(ARCHS).filter((a) => fs.existsSync(extensionPath(a)));
  if (archs.length === 0) {
    console.error('No WebAssembly build found. Run `make wasm_eh` first (see test/wasm/README.md).');
    process.exit(1);
  }
  for (const arch of archs) {
    if (!ARCHS[arch]) {
      console.error(`Unknown architecture ${arch}; expected one of ${Object.keys(ARCHS).join(', ')}`);
      process.exit(1);
    }
    if (!fs.existsSync(extensionPath(arch))) {
      console.error(`${extensionPath(arch)} is missing; run \`make ${arch}\` first`);
      process.exit(1);
    }
  }
  const bundled = await esbuild.build({
    entryPoints: [path.join(__dirname, 'page.js')],
    bundle: true,
    format: 'iife',
    write: false,
    logLevel: 'warning',
  });
  const pageScript = bundled.outputFiles[0].text;
  const browser = await chromium.launch();
  let failed = 0;
  try {
    for (const arch of archs) {
      console.log(`${arch}:`);
      const failures = await testArch(browser, arch, pageScript);
      failed += failures.length;
    }
  } finally {
    await browser.close();
  }
  console.log(failed ? `${failed} check(s) failed` : 'all checks passed');
  process.exit(failed ? 1 : 0);
})();
