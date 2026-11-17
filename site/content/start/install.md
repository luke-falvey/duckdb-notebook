# Install

duckdb-notebook is a DuckDB extension. It needs **DuckDB 2.0.0**: notebooks read variables with
`$name`, which DuckDB introduced in 2.0.

The extension is not yet in DuckDB's community repository, so `INSTALL notebook FROM community` does
not work. There are two ways to get it: download the build from CI, or build it yourself.

## Supported platforms

CI builds the extension for every native platform DuckDB distributes for:

| Platform | DuckDB's name | Tested in CI |
|---|---|---|
| Linux x86-64 (glibc) | `linux_amd64` | yes |
| Linux x86-64 (musl, e.g. Alpine) | `linux_amd64_musl` | yes |
| Linux ARM64 (glibc) | `linux_arm64` | built only |
| Linux ARM64 (musl) | `linux_arm64_musl` | yes |
| macOS Apple silicon | `osx_arm64` | yes |
| macOS Intel | `osx_amd64` | built only |
| Windows x86-64 (MSVC) | `windows_amd64` | yes |
| Windows x86-64 (MinGW) | `windows_amd64_mingw` | yes |
| Windows ARM64 | `windows_arm64` | built only |
| DuckDB-WASM 2.0, `base` bundle | `wasm_eh` | yes, loaded into DuckDB-WASM in Chromium |
| DuckDB-WASM 2.0, `threads` bundle | `wasm_threads` | yes, as above; `LOAD` can hang (see [running in the browser](../guides/browser.md)) |
| (no DuckDB-WASM 2.0 bundle loads it) | `wasm_mvp` | built only |

"Tested" means DuckDB's pipeline runs the SQL test suite there (for the WebAssembly builds, a job of
this repository loads them into DuckDB-WASM in headless Chromium and runs the notebook functions);
"built only" platforms compile and link but are not tested. In DuckDB-WASM the editor's server and
PDF export are not available; [running in the browser](../guides/browser.md) has what works there and
how to load it.

A native extension is about 40 MB (15 MB compressed). Almost all of it is DuckDB itself, which every
C++ extension that runs SQL links in; the notebook's own code is under 2 MB. The WebAssembly build
links against the DuckDB already in the page and is 1.2 MB.

## Download the CI build

Every push to `main` builds the extension for each platform above, stamped for DuckDB v2.0.0.

1. Open the latest successful
   [*Build and test* run on `main`](https://github.com/luke-falvey/duckdb-notebook/actions/workflows/MainDistributionPipeline.yml?query=branch%3Amain+is%3Asuccess)
   (you need to be signed in to GitHub to download artifacts).
2. Download the artifact for your platform, `notebook-v2.0.0-extension-<platform>` (for example
   `notebook-v2.0.0-extension-osx_arm64`), and unzip it. It holds `notebook.duckdb_extension`.
   To find your platform's name, run `PRAGMA platform;` in your DuckDB.
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
| PDF export | Chrome, Chromium or Edge, on any platform |
| html-wasm export | Node and npm, once, to build the DuckDB-WASM runtime |
| Map charts drawn from a `geometry` column | DuckDB's `spatial` extension |
| Notebooks on S3 or HTTP | DuckDB's `httpfs` extension: every path argument goes through DuckDB's file system |

Next: [your first notebook](first-notebook.md).
