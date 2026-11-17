# Running headless and in CI

A notebook does not need a browser. `notebook_run` runs every cell from SQL and returns a row per
cell, which makes a notebook usable as a scheduled job, a data test, or a report built in CI.

## Run a notebook from SQL

```sql
--8<-- "site/outputs/run_revenue.sql"
```

```text
--8<-- "site/outputs/run_revenue.txt"
```

- Cells run in file order, except that a markdown cell waits for the cells its `{{ }}` values read.
- `status` is `ok`, `error`, or `skipped` for cells with nothing to run (metadata, prose without
  `{{ }}`). An error in one cell does not stop the others; cells that depend on it fail in turn.
- `row_count` is the rows a query cell returned, or the rows of a chart's data query.
- `output` holds a markdown cell's text with each `{{ }}` replaced by its value.
- Effect cells run too, whatever their `run=` option.

The run happens **in a session of its own**. Tables the notebook creates stay in the database; its
temporary views and variables are gone when it returns. To get the views in your own session, run
the expanded script instead: `SELECT sql FROM notebook_sql('report.sql')` and execute the result.

## Parameters

```sql
--8<-- "site/outputs/run_params.sql"
```

```text
--8<-- "site/outputs/run_params.txt"
```

`params :=` replaces input defaults and sets variables nothing else sets, such as chart selections. A
variable that an SQL cell sets cannot be a parameter.

## From the shell

The DuckDB CLI runs one query and exits with `-c`. Start it with `-init /dev/null` so a personal
`~/.duckdbrc` cannot get in the way:

```sh
duckdb -unsigned -init /dev/null -c "LOAD notebook" \
  -c "SELECT name, status, error FROM notebook_run('report.sql') WHERE status = 'error'"
```

With the shell built in the repository (`./build/release/duckdb`) the extension is linked in, so
`-unsigned` and `LOAD` are not needed.

## A check that fails the build

This exits non-zero if any cell has a format error or fails to run:

```sh
--8<-- "site/notebooks/check.sh"
```

The repository's
[`check.sh`](https://github.com/luke-falvey/duckdb-notebook/blob/main/.github/skills/sketchbook-authoring/scripts/check.sh)
does the same with a readable report.

## From Python

The Python package of DuckDB 2.0.0 loads the extension like any other. This script checks notebooks
and exports each one that passes to HTML:

```python
--8<-- "site/notebooks/check_notebooks.py"
```

Table functions take a prepared parameter for the path (`notebook_run(?)`), so file names need no
quoting.

## In GitHub Actions

A workflow that checks every notebook in `notebooks/` and keeps the HTML exports as an artifact. It
expects the extension's Linux build (see [install](../start/install.md)) at
`tools/notebook.duckdb_extension`, and the script above at `tools/check_notebooks.py`:

```yaml
name: Notebooks
on: [push, pull_request]
jobs:
  notebooks:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
      - run: pip install duckdb==2.0.0
      - run: python tools/check_notebooks.py tools/notebook.duckdb_extension out notebooks/*.sql
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: reports
          path: out
```

## Data files for other tools

`format := 'data'` writes each named query cell's result as Parquet (or CSV), for a dashboard tool,
a spreadsheet or another pipeline. See [exporting](exporting.md#data).

## Things to know

- Paths in the notebook (in `read_parquet(...)` or `attach:`) are resolved as DuckDB resolves them:
  relative to DuckDB's working directory, except `attach:` paths, which are relative to the notebook.
- Notebook paths go through DuckDB's file system, so `s3://` and `https://` paths work once `httpfs`
  is loaded.
- `notebook_run` holds no connection after it returns: the database closes normally.
