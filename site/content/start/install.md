# Install

duckdb-notebook is a DuckDB extension. It needs **DuckDB 2.0.0**: notebooks read variables with
`$name`, which DuckDB introduced in 2.0.

The extension is not yet in DuckDB's community repository, so `INSTALL notebook FROM community` does
not work. There are two ways to get it: download the build from CI, or build it yourself.

## Download the CI build (Linux x86-64)

Every push to `main` builds the extension for Linux x86-64, stamped for DuckDB v2.0.0.

1. Open the latest successful
   [*Build and test* run on `main`](https://github.com/luke-falvey/duckdb-notebook/actions/workflows/MainDistributionPipeline.yml?query=branch%3Amain+is%3Asuccess)
   (you need to be signed in to GitHub to download artifacts).
2. Download the artifact `notebook-v2.0.0-extension-linux_amd64` and unzip it. It holds
   `notebook.duckdb_extension`.
3. Install it into DuckDB 2.0.0. The build is not signed, so DuckDB must be started with `-unsigned`:

```sh
duckdb -unsigned -c "FORCE INSTALL '/path/to/notebook.duckdb_extension'"
```

Then, in any later session:

```sh
duckdb -unsigned
```

```sql
LOAD notebook;
```

`-unsigned` is the CLI's form of the `allow_unsigned_extensions` setting. In Python, pass
`config={"allow_unsigned_extensions": "true"}` to `duckdb.connect`.

## Build from source

You need git, CMake, a C++17 compiler and Ninja (or Make). The first build compiles all of DuckDB and
takes about 15 minutes on four cores; later builds take seconds.

```sh
git clone https://github.com/luke-falvey/duckdb-notebook
cd duckdb-notebook
git submodule update --init --recursive --depth 1
GEN=ninja make release
```

This gives you two things:

- `./build/release/duckdb`, a DuckDB shell with the extension already linked in. Nothing to load.
- `build/release/extension/notebook/notebook.duckdb_extension`, the loadable file.

The loadable file from a local build is stamped with DuckDB's commit hash rather than `v2.0.0`, so it
loads into the shell built alongside it but not into a released DuckDB. To use it in an installed
DuckDB v2.0.0, build a copy stamped `v2.0.0`; the
[contributor notes](https://github.com/luke-falvey/duckdb-notebook/blob/main/CLAUDE.md#build-and-test)
show how.

## Check that it works

```sql
--8<-- "site/outputs/functions.sql"
```

```text
--8<-- "site/outputs/functions.txt"
```

## Optional extras

| For | You need |
|---|---|
| Opening the editor | A web browser on the same machine (the server listens on 127.0.0.1 only) |
| PDF export | Chrome, Chromium or Edge |
| html-wasm export | Node and npm, once, to build the DuckDB-WASM runtime |
| Map charts drawn from a `geometry` column | DuckDB's `spatial` extension |
| Notebooks on S3 or HTTP | DuckDB's `httpfs` extension: every path argument goes through DuckDB's file system |

Next: [your first notebook](first-notebook.md).
