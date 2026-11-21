# DuckDB review quorum, round 2

The second review of `main` (7effbdd, after round 1 was merged as PR #19) by the 15 simulated reviewer lenses in
luke-falvey/duckdb-webhdfs `.github/agents/`. Each lens ran as its own read-only subagent, with the same scope:
find what round 1 missed, what its fixes broke or left half done, and new problems. No lens saw another's
answer. They are lenses built from public review activity, not the people they are named after.

Consensus means raised by 3 or more lenses, or by 2 with high severity traced the same way. Every claim below
was checked against the code, and where the lenses disagreed, settled by running it (see "Disputed and
refuted").

## Verdict tally

All 15 lenses: **request changes**. Nobody approved. The changes asked for are smaller than in round 1:
mostly round-1 fixes that stopped part-way.

| Lens | Verdict | Main points |
|---|---|---|
| mytherin | request changes | PDF `chrome :=` not gated by `lock_configuration`; remote stats on open; only `/api/run` interruptible; shared attaches detached |
| hannes | request changes | Opening an untrusted notebook reaches the network; exports leave attaches; precomputed slider loops forever; `notebook_run` ignores `max_execution_time` |
| lnkuiper | request changes | Session store and read-only mode (refuted, see below); failed copy loses a temp table; CTAS drops constraints; versions taken after the run |
| carlopi | request changes | Remote sources and the `missing` version; html-wasm export leaks its attach; CI jobs skipped when one platform fails |
| tishj | request changes | Store drops constraints; trust-on-open incomplete; html-wasm fixed alias; `@` in names |
| taniabogatsch | request changes | Qualified `_sketchbook.current` view (refuted); `USE` half done; exports don't detach; atomic save and file modes; racy mtimes |
| hawkfish | request changes | TIMESTAMPTZ brush in browser-local time; DST; brush bounds rounded to nearest; racy mtimes |
| samansmink | request changes | Remote versioning on every open and run; `USE` half done; editor save on `db:x@N` forces; atomic save |
| maxxen | request changes | Store drops constraints; ALTER not re-copied; `TEMP VIEW … AS TABLE t` taken for a table; table functions by name list |
| pdet | request changes | `notebook_run` cannot be interrupted; remote file versions; `@` in names; atomic save and symlinks; `notebook_start` not checked at bind |
| tmonster | request changes | File stats on every brush, unbounded; `USE` in the server; name heuristics for table functions; versions after the run |
| dtenwolde | request changes | Session state keyed by cell position; sessions never evicted; unbounded stats before trust; table functions by name list |
| szarnyasg | request changes | Docs say `USE` is honoured everywhere; `@` in names; versions after the run; doc nits |
| kryonix | request changes | Trailing `-- comment` swallows the generated `;` or `)`; cell-wide CTE filter; store loses ALTER and constraints; sample before filter |
| dentiny | request changes | Versions after the run; stats on every input change; `missing` under `file_search_path`; exports detach; watcher polls remote files; `Prune` deletes others' temp files |

## Consensus (implemented unless marked open)

| # | Finding | Lenses | Change |
|---|---|---|---|
| C1 | The cache's file-version check opens every file a notebook names, remote ones included (a `HEAD` per file, a listing plus one per match for a glob, the glob fully listed before the 1000-file cap), on every open, run and input change, before the reader trusts the notebook | 12: mytherin, hannes, lnkuiper, carlopi, tishj, samansmink, maxxen, pdet, tmonster, dtenwolde, szarnyasg, dentiny | Only local paths are looked at (`notebook::IsLocalPath`); a remote file has no known version, so its cells are `unverified` and no request is made. Core test; FORMAT §8 and the cache guide |
| C2 | File versions are taken after the run, so a file changed while the cells read it (or by a later cell) is recorded as the new version and called fresh | 5: lnkuiper, carlopi, tmonster, szarnyasg, dentiny | `/api/run` and `notebook_run` take the versions before the first cell and pass them to `ResultCache::Record`. SQL test: a cell that a later cell's `COPY` overwrites is stale |
| C3 | `/api/variable` (every input change and brush) recomputes every key and file version though nothing ran | 5: lnkuiper, tishj, tmonster, dtenwolde, dentiny | `Record` with no cells that ran stores the variables and looks at no file |
| C4 | Modified times are whole seconds: a same-size rewrite in the same second keeps the key, and the stale result is fresh | 3: lnkuiper, taniabogatsch, hawkfish | Git's racy-clean rule: a file modified in the last two seconds has no known version (`notebook::ModifiedTooRecently`). Core and SQL tests |
| C5 | Only `notebook_run` detaches what the `[notebook]` cell attached; every export leaves it attached. The html-wasm export's `__notebook_export` stays attached, with its temp file, when any copy fails, so every later html-wasm export fails; two exports collide on the fixed name | 8: hannes, lnkuiper, carlopi, tishj, taniabogatsch, pdet, dtenwolde, dentiny | `DetachOnExit` and `PrepareOnce` are shared (`session.hpp`) and used by every export. html-wasm uses a UUID name and a guard that detaches and removes the file however the export ends. SQL tests for both |
| C6 | `notebook_save` accepts a name with `@` (given, or from a file stem such as `report@2.sql`) that `db:<name>` then cannot name: `db:report@2` reads version 2 of `report` | 6: tishj, maxxen, pdet, taniabogatsch, dtenwolde, szarnyasg | `notebook::CheckStoredName` refuses `@` and blank names at bind. Core and SQL tests; FORMAT §9, the storing guide and the function reference |
| C7 | "Honour `USE`" reached the SQL functions only: the editor read and saved `db:` notebooks in the default database, and every `db:` cache went into the default database's `_sketchbook.cache` | 6: samansmink, tishj, taniabogatsch, tmonster, dtenwolde, szarnyasg | `notebook_start` records the current database; sessions read, save, list and cache stored notebooks there. `ResultCache::database` and `ResultCacheStore(…, database)` qualify the cache table; `notebook_run`, `notebook_cache` and `notebook_cache_clear` pass the current database. SQL test (`notebook_storage.test`) and browser test (`test_session_store`) |
| C8 | The session store copies temp tables with `CREATE TABLE … AS`, so PRIMARY KEY, UNIQUE, NOT NULL, CHECK, DEFAULT and generated columns are lost after the first request; the editor and `notebook_run` then disagree | 4: lnkuiper, tishj, maxxen, kryonix | The store records `duckdb_tables().sql` and the non-generated columns of each table; a restore runs that statement, then `INSERT … SELECT`, and falls back to rows only when the statement cannot run. A table whose copy fails keeps its `CREATE TEMP TABLE` in the replay (lnkuiper). `ALTER TABLE` now reads its table, so an altered table is copied again (maxxen, kryonix). Browser test |
| C9 | The cache decides "external data" from a list of function names, so `st_read`, table macros, `duckdb_tables()` and any extension's table function were fresh | 4: maxxen, tmonster, dtenwolde, kryonix | The parse-tree walker records table functions (`References::table_functions`, `Cell::table_functions`); any but a file reader and `range`, `generate_series`, `unnest`, `json_each`, `json_tree`, `repeat_row` makes the cell unverified. Core and SQL tests (a table macro) |
| C10 | The atomic save (temp file, then rename) replaces a symlinked notebook with a regular file, resets its mode and breaks hard links | 8: hannes, mytherin, carlopi, tishj, taniabogatsch, samansmink, pdet, dentiny | **Open**: DuckDB's `FileSystem` cannot read a link or a file mode, so keeping either needs platform code in `src/functions` or a decision to write symlinked files in place. See "Left open" |
| C11 | `IsConflict` retried any error whose message contained "conflict" (a notebook named `conflict-log`), 20 times with no pause | 9: mytherin, lnkuiper, carlopi, taniabogatsch, maxxen, pdet, tmonster, dtenwolde, dentiny | Decided by the error type only (TRANSACTION, or a duplicate-key CONSTRAINT); retries wait a short, growing, jittered time. A version that would pass `INT32_MAX` is an error (tishj, hannes) |
| C12 | `attach:` paths relative to the notebook used only `/`, so `C:\nb\x.sql` resolved them against the working directory | 4: mytherin, hannes, pdet, dentiny | Both separators, and `FileSystem::IsPathAbsolute` |
| C13 | Server sessions (each with an in-memory store) are never evicted, are keyed by the raw path (`a.sql` and `./a.sql` are two sessions) and are created under the global lock while reading files | 6: mytherin, carlopi, taniabogatsch, maxxen, dtenwolde, dentiny | **Open**: needs an eviction policy. See "Left open" |
| C14 | The file watcher (`/api/events`) reads a remote notebook in full twice a second per open tab | 3: hannes, samansmink, dentiny | Remote notebooks are not watched (as stored ones are not) |
| C15 | `notebook_run` runs every cell inside `Init` on its own connection: Ctrl-C and the caller's `max_execution_time` do not reach it | 2, one high: pdet (high), hannes | An interrupt stops it before the next cell; the caller's `max_execution_time` is set on its connection. SQL test |
| C16 | `chrome :=` runs any program SQL names, and `lock_configuration` does not stop it (it stops `notebook_start`) | 2, one high: mytherin (high), hannes | `pdf` is refused under `lock_configuration`. New `notebook_lock.test`, which also tests the lock with external access still on (tishj) |
| C17 | `notebook_start` did not check its path at bind | 2: pdet, szarnyasg | `SourceArgument`, as every other function. SQL test |
| C18 | `loadable-test` and `wasm-test` are skipped whenever one platform's build fails on `main` | 2: carlopi, samansmink | `if: ${{ !cancelled() }}`, as `extension-sizes` already had |

## Unique flags

Implemented, because they were cheap and clearly right:

- **kryonix (high): a trailing `-- comment` swallowed the generated `;` or `)`.** Confirmed: `notebook_sql` wrote
  `SELECT 1 AS x -- by day;`, which merges the cell with the next, and with a store a `materialize=table` cell failed
  in the editor. `SplitStatements` now ends a statement that ends in a line comment with a newline, so everything
  that builds on statements is safe (`WithSemicolon`, `DisplaySql` paging, `ReplaySql`). Core and SQL tests.
- **hannes: the precomputed export's slider loop never advanced** at `min=1e17 step=1`, since adjacent doubles there
  are 16 apart. `notebook::InputSteps` counts the steps first and refuses values it cannot tell apart. Core test.
- **maxxen: `CREATE TEMP VIEW v AS TABLE t` was taken for a temp table** and vanished with a store. `IsCreateTempTable`
  stops at `AS` and `VIEW`. Core test.
- **dentiny: `Prune` deleted another writer's temporary file** (a `notebook_run` beside the editor). It now leaves
  temporaries younger than ten minutes. SQL test.
- **carlopi, hannes: the PDF export** took an older PDF for success when the browser wrote nothing, and wrote
  `<file>.html` over whatever had that name. The old PDF is removed first, and the page gets a UUID name.
- **szarnyasg: FORMAT.md did not state the 256-level nesting limit** of specs and YAML. Added to §5.

Left open, needing a decision or more work than a review fix:

- **hawkfish (high): a brush on a TIMESTAMPTZ axis sends browser-local wall time without an offset**, read in the
  server's time zone; naive TIMESTAMP is drawn in the browser's zone, so DST days are distorted. The fix depends on
  the time-zone model: the renderer has to know each axis's column type (to send UTC with `Z` for TIMESTAMPTZ and
  treat naive values as UTC throughout). Also open from that lens: integer and DECIMAL brush bounds rounded to
  nearest rather than inward, TIME/TIMETZ axes, point selection on wide DECIMALs.
- **dtenwolde: the session's replay and stored tables are keyed by cell index.** Inserting a cell above a
  `materialize=table` cell and running only the new one drops the stored table; deleted cells keep replaying. Keying
  by the stable cell id (`CacheCellIds`) is the fix; it touches every use of `SessionState::replay` and `stored`.
- **samansmink: saving from the editor on `db:x@N` always forces** a new newest version and the session stays pinned
  to `@N`. Whether saving an old version should be refused or re-key the session is a product decision.
- **mytherin, lnkuiper: `notebook_run` detaches an attach another session reuses**, and two runs race on
  check-then-attach. Needs a per-instance reference count of attaches.
- **mytherin: `/api/page`, `/api/options` and `/api/catalog` replay the session with no interrupt or time limit.**
  dtenwolde: those handlers replay before re-attaching.
- **kryonix: the cell-wide CTE filter erases real reads** (`WITH sales AS (FROM sales)`), so a cell can lose its
  dependency; **count(\*) may not evaluate projections** (unverified); **`sample:` samples before the brush filter**
  and without `REPEATABLE`; paging with `LIMIT/OFFSET` without a total order.
- **dentiny: a missing file under `file_search_path`** counts as a known version.
- **mytherin: the browser tests and a ThreadSanitizer run are not in CI.**
- **taniabogatsch: no storage test survives a restart** (WAL replay of saves, the migration and the cache).
- **szarnyasg nits:** the two version error messages are worded differently; error messages could link the
  troubleshooting page.

## Disputed and refuted

Settled by running the release build:

- **The external-access bypass** (tmonster, pdet, carlopi: `FileSystem::GetFileSystem(db)` has no opener).
  samansmink, mytherin, hannes and tishj read DuckDB's source the other way, and they are right: it returns a
  `DatabaseFileSystem`, an `OpenerFileSystem` with the database's opener, so `enable_external_access` and
  `allowed_directories` apply. Not a finding. What remained (remote requests on open and run) is C1.
- **The session store on a read-only database** (lnkuiper, high). maxxen, dtenwolde and hannes said the attach is
  refused and the session falls back to replaying SQL. Run with `duckdb -readonly`: `ATTACH ':memory:'` fails with
  "Cannot launch in-memory database in read-only mode", temp tables still work, and `store_failed` takes over. Not a
  finding.
- **The catalog baked into `_sketchbook.current`** (taniabogatsch, high). DuckDB stores the view as
  `SELECT * FROM _sketchbook.versions …`, without the catalog, and after `DETACH work; ATTACH … AS w2`,
  `FROM w2._sketchbook.current` works. Not a finding.

## Left open (consensus)

- **C10, symlinks and modes on save.** Options: resolve a symlink and write the temporary file next to its target
  (needs `readlink`/`GetFinalPathNameByHandle` behind a platform boundary in `src/functions`); or detect a link and
  write in place (non-atomic for links only); or document that saving replaces links and resets the mode. The
  quorum preferred keeping links; that needs platform code the extension avoids so far.
- **C13, server sessions.** Normalising the session key (absolute path) is cheap, but an eviction policy (idle
  time, a cap, dropping only the store) and building a session outside the global lock change the server's
  lifetime model, which DESIGN §11 describes. Left for a change of its own.

## Testing

Local, against the release build:

- `make core_test`;
- `make test`: all of `test/sql`, 18 files, including the new `notebook_lock.test`;
- `test/browser/test_browser.py` (Playwright, Chromium), including the new `test_session_store`;
- `make frontend_check`;
- `mkdocs build --strict` and the link check;
- `clang-format` 11 on every C++ file touched.

Not run locally: the wasm builds, Windows (the `\` attach path), and the CI jobs themselves.
