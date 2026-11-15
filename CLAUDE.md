# Working on duckdb-notebook

A DuckDB extension that makes a plain `.sql` file a notebook. Read `docs/DESIGN.md` for what is being
built and why, and `docs/FORMAT.md` for the file format. This file is about how to work in the code.

## Where things stand

Roadmap step 1 of `docs/DESIGN.md` §9 is done: the format parser and a headless runner.

| SQL function | What it does | File |
|---|---|---|
| `notebook_cells(path)` | One row per cell: kind, name, options, body, errors | `src/functions/notebook_cells.cpp` |
| `notebook_sql(path)` | The notebook as a plain SQL script | `src/functions/notebook_sql.cpp` |
| `notebook_run(path)` | Runs every cell in its own session; one row per cell with status | `src/functions/notebook_run.cpp` |

Not started: dependency graph, HTTP server, frontend, chart compiler, in-database storage, export.
`docs/DESIGN.md` §9 lists what comes next and §11 records what the first pass taught us.

## Layout

```
src/core/          Plain C++17, no DuckDB headers. Lexer, format parser, script generation.
src/functions/     The SQL surface. One file per function, DuckDB C++ API.
src/include/       Headers: notebook/*.hpp for core, notebook/functions.hpp for the SQL surface.
test/core/         Unit tests for src/core. No DuckDB needed.
test/sql/          sqllogictest files: end-to-end tests through SQL.
test/notebooks/    Notebook files the SQL tests read.
examples/          Notebooks for people to read. Tests check they stay valid and runnable.
duckdb/            Submodule, pinned to the DuckDB release we build against (v2.0.0).
extension-ci-tools/ Submodule: DuckDB's shared build and CI scripts.
spikes/            Throwaway experiments kept as evidence. Not built or tested by CI.
```

## Build and test

```bash
git submodule update --init --recursive --depth 1
```

```bash
make core_test
```

Compiles and runs the `src/core` tests in a few seconds. Use this loop for anything in `src/core`.

```bash
GEN=ninja make release
```

Builds DuckDB and the extension. **The first build compiles all of DuckDB and takes about an hour on
4 cores**; later builds only recompile the extension and take seconds. Start it in the background as
soon as a session begins.

```bash
make test
```

Runs `test/sql/*.test` against the release build.

```bash
./build/release/duckdb
```

A DuckDB shell with the extension already loaded, for trying things by hand.

Run one test file with `./build/release/test/unittest "test/sql/notebook_run.test"`.

## Conventions

**Put logic in `src/core`.** Anything that does not need a database (parsing, validation, generating
SQL text) goes there, takes strings in and returns plain structs, and gets a unit test in
`test/core/core_test.cpp`. This keeps the fast test loop useful and the code portable. `src/functions`
should stay thin: read the file, call core, shape the result.

**Format errors are data, not exceptions.** `ParseNotebook` never throws. Problems are attached to the
cell in `Cell::errors` so a UI can show all of them. Functions that cannot proceed on an invalid
notebook call `ThrowIfInvalid`, which reports the first error and points at `notebook_cells`.

**Adding a SQL function.** Declare `RegisterX` in `src/include/notebook/functions.hpp`, implement it in
`src/functions/x.cpp` as a table function (bind reads and parses, scan emits rows), add the file to
`CMakeLists.txt`, call it from `src/notebook_extension.cpp`, and add `test/sql/x.test`. Copy
`notebook_sql.cpp` as the smallest example.

**Never keep a `Connection` alive past a query.** A stored `Connection` owns the database instance.
If the extension holds one, the database is not closed when the user closes it: the file stays locked
and is not checkpointed. This was measured, not assumed (see DESIGN §11). Create connections where
they are used and let them go, as `notebook_run` does. The future server must hold a
`weak_ptr<DatabaseInstance>` and create connections on demand.

**Release a `QueryResult` before the next query on the same connection.** In DuckDB 2.0 a result keeps
its connection busy until it is consumed or destroyed.

**Reading files** goes through `ReadNotebook`, which uses DuckDB's file system, so remote paths work
when `httpfs` is loaded. Do not use `std::ifstream`.

**The format spec and the parser move together.** A change to `src/core/format.cpp` that changes what
is accepted needs the matching change in `docs/FORMAT.md`, and the reverse.

**Style.** DuckDB's: tabs, 120 columns, `PascalCase` functions, `snake_case` variables.
`.clang-format` is DuckDB's own. Run `clang-format -i` on files you touch.

**Tests.** Every behaviour change gets a test. Prefer a core unit test; add a SQL test when the
behaviour involves DuckDB (execution, file access, result shape). SQL tests must clean up tables they
create, because all test files share one database.

## Things that are easy to get wrong

- A `-- %%` line inside a block comment or string literal is content, not a marker. All text scanning
  goes through `SqlScanner` in `src/core/sql_lexer.cpp`, which tracks that state. Do not use regular
  expressions or `find` on SQL text.
- DuckDB block comments nest. `$$…$$` and `$tag$…$tag$` are string literals, while `$name` is a
  variable reference.
- Whether a cell is a query is decided by the lexer (`IsQueryStatement`), not by DuckDB's parser, so
  it works without a database. If you replace it with the real parser, keep the core tests passing.
- `notebook_run` executes in a session of its own. Tables it creates persist; its temp views and
  variables are gone when it returns. To run a notebook in your own connection, execute the output of
  `notebook_sql`.
- The version of DuckDB in the `duckdb` submodule, the `duckdb_version` in
  `.github/workflows/MainDistributionPipeline.yml`, and the `extension-ci-tools` branch must all name
  the same release.

## Git

Develop on a feature branch and open a pull request against `main`. CI runs the core tests and
DuckDB's extension pipeline (currently `linux_amd64` only).
