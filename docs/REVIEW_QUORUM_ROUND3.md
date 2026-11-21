# DuckDB review quorum, round 3

The third review of `main` (58c6017, after round 2 was merged as PR #20) by the 15 simulated reviewer lenses in
luke-falvey/duckdb-webhdfs `.github/agents/`. Each lens ran as its own read-only subagent with the same scope: what
rounds 1 and 2 missed, what round 2's fixes broke or left half done, new problems, and a view on every item round 2
left open. No lens saw another's answer. They are lenses built from public review activity, not the people they are
named after.

Consensus means raised by 3 or more lenses, or by 2 with high severity traced the same way. Every claim below was
checked against the code before it was acted on, and where it could be, settled by running it.

## Verdict tally

14 lenses: **request changes**; 1 (carlopi): **approve with nits**. Nobody found a crash or a security hole on the
scale of round 1. Most findings are round-2 fixes that stopped part-way: C7 (`USE`) and C15 (time limit) reached
only some entry points, and C2 (versions before the run) fixed one order of write and read but broke the other.

| Lens | Verdict | Main points |
|---|---|---|
| mytherin | request changes | Sessions keyed by source, not database; the limit is per cell, not per call, and exports have none; attach race now in every export; `ResultCache::database` as a field contract |
| hannes | request changes | UNC paths count as local (SMB before trust); C7 key; save widens 0600 to 0644; limit dropped under `lock_configuration`; export detach race |
| lnkuiper | request changes | Every request copies every temp table, and C8 made it dearer; C7 key; exports unbounded; limit set after Prepare; failed-copy replay clashes |
| carlopi | approve with nits | Limit fails open under `lock_configuration`; C7 key; old `db:` caches orphaned under `USE`; html-wasm test checks less than it says |
| tishj | request changes | Write-then-read stale forever; limit per cell; C7 key; orphaned caches; store restores tables alphabetically (foreign keys) |
| taniabogatsch | request changes | C7 key; failed-copy fallback fails itself; no restart test; no directory fsync; `CanonicalizePath` exists, so C10 needs no platform code |
| hawkfish | request changes | Write-then-read stale forever; untrusted brush stored as VARCHAR; TIMESTAMPTZ brushes (proposed answer); integer bounds rounded outward; FAT's 2 s mtimes |
| samansmink | request changes | Session settings (TimeZone, s3_region, file_search_path) do not reach `notebook_run`; C7 key; two "is local" predicates; remote `WriteFile` uploads twice |
| maxxen | request changes | Write-then-read stale forever; C10 and C13 left open on a false premise (`CanonicalizePath`); alphabetical restore; `CacheLocation` struct |
| pdet | request changes | C7 key; exports ignore interrupt and limit; interrupt path untested; a failed PDF export deletes the previous PDF |
| tmonster | request changes | Per-request restore rebuilds indexes; `duckdb_tables()`/`duckdb_columns()` scan every attached catalog per cell; write-then-read stale; timing-dependent tests |
| dtenwolde | request changes | C8 fallback erases a table another cell made; C7 key as hidden global state; a temp macro shadowing `range`; listing failure treated as copy failure |
| szarnyasg | request changes | Write-then-read stale (undocumented); C7 key contradicts the docs; C16 docs half done; limit wording; version messages |
| kryonix | request changes | Write-then-read stale; CTE scoping (reuse peek's); `sample:` before the brush and unseeded; paging without order; temp sequences restart |
| dentiny | request changes | Write-then-read stale, and `""` meaning two things; C7 key; `missing` under `file_search_path`; two "is local" predicates |

## Consensus (implemented unless marked open)

| # | Finding | Lenses | Change |
|---|---|---|---|
| R1 | Server sessions are keyed by `source` alone, but carry the database of whichever `notebook_start` created them: `USE a; notebook_start('db:x'); USE b; notebook_start('db:x')` opened and saved `a`'s `x`, while the picker listed `b`'s. `Start` then `SetDatabase` left a window with no database. `a.sql` and `./a.sql` were two sessions | 15 (every lens; 11 on the database, the rest on the key) | Sessions are keyed by (database, `db:` name) for stored notebooks and by `FileSystem::CanonicalizePath` for files. `notebook_start` puts its current database in the URL (`&db=`), the page sends it with every request, and the picker keeps it; `Start(port, database)` sets it before the server answers. A session is built outside the server's lock and inserted only if no other request got there first (C13, in part). Browser test `test_session_keys` |
| R2 | C2 takes every file version once, before the first cell: a cell that reads a file an earlier cell writes (`COPY … TO 'x'`, then `FROM 'x'`) is stale after every run, forever. Before round 2 it was fresh | 8: szarnyasg, tmonster, dentiny, hawkfish, tishj, maxxen, kryonix, taniabogatsch | Versions are taken just before each cell runs (`RunFileVersions`, `CacheKeys(…, cell_files)`), so a cell is keyed with the files as it read them. A later change still makes it stale |
| R2a | …and per-cell versions alone would not help: the file was written milliseconds before, so the two-second rule gives it no version, which recorded a key that can never match: `stale` again (`""` meant both "unknown" and, once recorded, "certainly different") | 2: dentiny, tishj | A version within the two seconds is kept with a `?` mark: the key covers it but is not verified, and the entry is stored as such (`CacheEntryKey`, a `?` before the key). It reads `unverified` while the file is unchanged, `stale` once it changes, and never `fresh`. No layout change; `CACHE_FORMAT` stays 3, since old entries compare as before. Core and SQL tests |
| R3 | C15 is half done: the limit is set after `Prepare` (the `[notebook]` cell's ATTACH is unbounded); the `SET` is refused under `lock_configuration` and the failure ignored, so the limit fails open (`Runner::Peek` fails closed); it bounds each cell, not the call (N cells take N × limit); a cell's own `SET max_execution_time = 0` lifts it; exports (and the precomputed export's up to `budget` runs) have no interrupt check and no limit at all; nothing tests the interrupt path | 10: mytherin, carlopi, hannes, tishj, pdet, lnkuiper, tmonster, dtenwolde, szarnyasg, samansmink | `Runner` takes the caller's `ClientContext`. Before the replay, before `Prepare` and before every cell it throws if the caller was interrupted or its deadline passed, and writes the time left into the runner connection's `max_execution_time` directly (no `SET`, so the lock does not apply; a cell's own `SET` lasts until the next cell). `notebook_run` and every export pass it. SQL tests: two slow cells fail the call; a cell's `SET max_execution_time = 0` does not lift it; an export is bounded; the limit holds under `lock_configuration` |
| R4 | C8's fallback for a failed store copy fails itself: a cell that made A (copied) and B (not) replays `CREATE TEMP TABLE A` over the A the store restored, which stops the replay before B; a table an earlier cell made whose re-copy fails (after an `INSERT`) is erased with nothing to make it again; a listing failure got the same treatment | 3: lnkuiper, taniabogatsch, dtenwolde | When a table the cell made cannot be copied, none of the cell's tables comes from the store and its `CREATE TEMP TABLE` statements are replayed; a table an earlier cell made keeps its older copy. **No test**: nothing short of a fault hook makes the in-memory store's `CREATE TABLE … AS` fail on demand |
| R5 | Two "is local" predicates (`notebook::IsLocalPath`, `IsLocalFile`) that agree only by accident, and the core one counts UNC paths (`\\host\share\x.csv`) as local: opening an untrusted notebook with a cache folder would make Windows connect to the host over SMB before the reader trusted anything | 5: samansmink, dentiny, maxxen, mytherin, hannes | `IsLocalPath` refuses `\\…` and `//…`; `IsLocalFile` and the atomic save use it. Core test |
| R6 | `WriteFile` on an object store uploaded a temporary file, then copied and deleted it (or, without `MoveFile`, uploaded everything twice), and could leave the temporary object behind | 3: samansmink, pdet, lnkuiper | A remote target is written directly: its upload is already all or nothing |
| R7 | C10 (links and modes on save) was left open on a false premise: DuckDB 2.0's `FileSystem::CanonicalizePath` resolves links (`realpath`, `GetFinalPathNameByHandleW`) through the opener, and works for a file that does not exist yet | 2 found it (taniabogatsch, maxxen); 7 wanted links kept, 7 would document only (to avoid platform code) | `WriteFile` writes the temporary file beside the canonical target and moves it there: a link stays a link, with no platform code, and the target is checked against `allowed_directories` (a link out of them is refused, not replaced). Cache entries are written `FILE_FLAGS_PRIVATE` (0600), since they hold query results (hannes, dentiny). Browser test. **Open**: the notebook file's own mode is reset and hard links broken (no `FileSystem` primitive); documented |
| R8 | Saving from the editor on `db:x@N` quietly forced a new newest version | 9: mytherin, samansmink, lnkuiper, taniabogatsch, dtenwolde, maxxen, hannes, tishj (refuse); pdet (save and re-key) | Refused, with what to do instead. Browser test |
| R9 | Old `db:` caches: before C7 every stored notebook's cache was in the default database; under `USE` it was neither read nor cleared | 2: carlopi, tishj (low, traced the same way) | Read from the default database when the current one has nothing, cleared with it, never written. SQL test |
| R10 | The store restores temp tables in name order, so a table with a foreign key to one later in the alphabet fails its DDL and comes back without its constraints | 2: tishj, maxxen | Checked: DuckDB allows foreign keys between temp tables. Tables whose statement fails are tried again while any succeeds. Browser test |
| R11 | Replay and stored tables keyed by cell index (open from round 2) | 9: dtenwolde, mytherin, pdet, lnkuiper, taniabogatsch, maxxen, tishj, hannes, tmonster | **Open**. Everyone agrees on `CacheCellIds`. But `Restore` runs without the notebook and replays in index order, which is cell order; keyed by id it needs an order of its own, plus pruning of ids no longer in the notebook. A change of its own |
| R12 | Shared attaches: a one-off run detaches an alias another session found attached and uses; two runs race on check-then-attach. Round 2's C5 made every export detach, so it happens more | 7: mytherin, hannes, carlopi, pdet, lnkuiper, maxxen, taniabogatsch | **Open**. Agreed answer: a per-`DatabaseInstance` registry in the object cache, a reference count per alias, check-and-attach under its lock, detach at zero. It changes who owns an attach (DESIGN §11), so a change of its own |
| R13 | Each server request restores every temp table from the store (now through its DDL, rebuilding keys), and `KeepTempTables` lists `duckdb_tables()`/`duckdb_columns()`, which scan every attached catalog, after every writing cell | 2, one high: tmonster (high), lnkuiper | **Open**: needs a benchmark first (both said so). Directions: restore only what a request reads; look entries up through the catalog API instead of the table functions |

## Unique flags

Implemented, because they were cheap and clearly right:

- **dentiny, hannes: `file_search_path`.** DuckDB applies it when reading, not when opening, so a relative path was
  recorded as `missing`, a known version, and stayed fresh however the file it read changed. With
  `file_search_path` set, a relative path has no known version (`unverified`). SQL test.
- **kryonix: a `{{ }}` expression ending in a line comment** (`{{ total -- EUR` then `}}` on the next line) was a
  syntax error: `SELECT (` + expression + `)` put the `)` in the comment. Now `+ "\n)"`, in the runner and in the
  graph. SQL test. (A comment on the same line as `}}` swallows the `}}`; that is the lexer working as specified.)
- **pdet: a failed PDF export deleted the previous PDF.** It now prints to a file of its own and moves it over the
  target once printed.
- **hannes: `combinations * values.size()`** in the precomputed export could overflow with an absurd `budget`; now
  compared by division.
- **szarnyasg: the two version errors.** One rule for both: "a version is a whole number from 1 to 2147483647; leave it
  out for the newest" (`notebook_checkout(…, version := 3000000000)` used to say "versions start at 1").
- **szarnyasg: docs.** C16 reached the reference only: the exporting guide and DESIGN §3 now say `pdf` is refused
  under `lock_configuration`; troubleshooting has rows for that, the `@` name, the time limit and the old-version
  save; the headless guide explains the limit; FORMAT §8 says opening makes no request "beyond reading the notebook
  itself"; FORMAT §9 says names saved with `@` before the rule are still read (`db:report@2@1`); DESIGN §17 records
  how files are checked.

Left open, needing a decision or more work than a review fix:

- **samansmink (high): session settings do not reach a run.** `SET TimeZone`, an extension option (`s3_region`) or
  `file_search_path` at SESSION scope stays on the caller's connection; `notebook_run` and the exports run on a
  fresh one. Copying the caller's `ClientConfig::user_settings` is the general answer (and replaces the
  `max_execution_time` special case), but which settings a notebook should inherit, and the server's equivalent
  (a snapshot at `notebook_start`), are a design decision DESIGN §3 has to make.
- **mytherin: an interrupt does not stop a cell already running** in `notebook_run` or an export, only the next one.
  Forwarding it means running through `PendingQuery` and interrupting the runner's connection; the deadline now
  bounds the running cell, which covers the case that matters most.
- **mytherin: `/api/page`, `/api/options` and `/api/catalog` replay with no limit** (still open from round 2).
- **C13 eviction.** Keys and building outside the lock are done (R1). The lenses split on what to evict: most would
  drop only the in-memory store after an idle time, but dtenwolde showed that replay rows name the store, so dropping
  it alone leaves views that dangle; evicting whole sessions resets trust. Sessions live until the server stops
  (now stated in DESIGN §11).
- **hawkfish: brushes.** A brush made before the reader trusts the notebook is stored as `VARCHAR` and survives
  reopening; integer and DECIMAL bounds are cast to the column type, rounding outward; TIMESTAMPTZ bounds are browser
  wall time without an offset. hawkfish's proposed rule (send TIMESTAMPTZ as UTC with `Z`, treat naive values as UTC
  in the browser) needs no type plumbing, but changes every temporal axis; it deserves a change and tests of its own.
  Also: FAT keeps 2-second mtimes, which the 2-second margin only just covers.
- **kryonix:** the references walker's cell-wide CTE filter (reuse peek's CTE scoping); `sample:` applied before the
  brush filter and without `REPEATABLE` (and `sample: 10%` read as 10 rows); paging with `LIMIT/OFFSET` and no total
  order; temp sequences replayed from the start while their tables come back with rows.
- **dtenwolde:** a temp macro shadowing an allow-listed table function (`range`) counts as pure.
- **taniabogatsch:** no storage test survives a restart (open since round 2); a save syncs the file but not its
  directory; a crash between write and move leaves `<file>.<uuid>.tmp` beside the notebook.
- **pdet, tmonster:** `notebook_cache_sources.test` sleeps over 2 s several times, and its "unverified right after
  writing" checks assume the run starts within two seconds of the write. An injectable clock would remove both.
- **carlopi:** the html-wasm failure test does not check that the export's temporary database file is gone (where
  it lives depends on `TMPDIR`, which a portable test cannot glob); `TempDirectory()` falls back to `/tmp` and `.`.
- **maxxen:** `ResultCache::database` is a field callers must set before `Load` (mytherin too); `IsConflict` still
  matches "duplicate key" for CONSTRAINT errors; two mechanisms classify table functions.
- **samansmink:** nothing is logged through DuckDB's logger, so a missing cache or an unverified remote file leaves
  no trace.
- **tishj:** `notebook_lock.test` locks the configuration for the rest of its file, which CLAUDE.md's "all test
  files share one database" would make unsafe. It passes because the sqllogictest runner opens a fresh in-memory
  database for each file; what files do share is what they write to disk (`__TEST_DIR__`, cache folders). The
  CLAUDE.md sentence should be corrected; left to whoever owns that file.

## Disagreements

- **C10: keep links, or document.** Half the lenses would only document that saving replaces a link, to keep
  platform code out of `src/functions`. `CanonicalizePath` removes that objection, so links are kept. hannes worried
  that resolving a link lets a save write outside `allowed_directories`; it is the other way round, since the opener
  checks the resolved target and refuses it.
- **`db:x@N` save: refuse or re-key.** pdet would save it as the newest and re-key the session so no edit is lost; the
  other eight would refuse. Refusing won: the page keeps the text, and the message says how to keep it.
- **Eviction: store only, or whole sessions** (above).

## Testing

Local, against the release build:

- `make core_test`;
- `make test`: all of `test/sql`, 18 files;
- `test/browser/test_browser.py` (Playwright, Chromium): 353 checks, including the new `test_session_keys`
  (database in the URL, one session per file, a save through a symlink, private cache entries, an old version not
  saved, a foreign key through the store). The html-wasm export was skipped (no runtime built);
- `make frontend_check`;
- `mkdocs build --strict` and the link check;
- `clang-format` (18) on the C++ files touched, except `server.cpp`, whose handler lambdas it re-indents
  (CLAUDE.md).

Not run locally: the wasm builds, Windows (UNC paths, `CanonicalizePath` there), macOS, and the CI jobs themselves.
