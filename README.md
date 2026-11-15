# duckdb-notebook

A notebook environment for DuckDB where the notebook is a plain `.sql` file.

**Status: early.** The design is written, and a first pass of the DuckDB extension parses notebooks
and runs them headlessly. There is no user interface yet.

```sql
-- %% [markdown]
/*
# Revenue review
*/

-- %% input region select from=regions.name
SET VARIABLE region = 'EU';

-- %% monthly
SELECT date_trunc('month', ts) AS month, sum(amount) AS revenue
FROM orders
WHERE region = $region
GROUP BY ALL;

-- %% chart from=monthly
/*
mark: line
x: month
y: revenue
*/
```

## The idea

- **A notebook is valid SQL.** Cell boundaries, prose and chart specs live in comments, so the file
  diffs cleanly, works with SQL tooling, and is easy for people and agents to edit.
- **It runs inside your DuckDB.** A DuckDB extension serves the interface from the DuckDB process you
  already have, with the frontend built in. No network access is needed.
- **Cells are reactive.** A named query is a view; inputs and chart selections are DuckDB variables.
  Change one and everything that depends on it updates.
- **Charts are declarative.** A short YAML spec compiles to an aggregate query, so charts stay fast on
  large tables.
- **Notebooks can live in the database.** The same text is stored with version history in a table.
- **Share without DuckDB.** Export to a single self-contained HTML file, or to PDF.

## What works today

Build the extension (see [CLAUDE.md](CLAUDE.md) for details), then in `./build/release/duckdb`:

```sql
-- One row per cell: kind, name, options, body, and any format errors
SELECT idx, kind, name, is_query, errors FROM notebook_cells('examples/revenue.sql');

-- Run every cell in a session of its own; one row per cell with its outcome
SELECT name, status, row_count, error FROM notebook_run('examples/revenue.sql');

-- The notebook as a plain SQL script that any DuckDB 2.0 client can run
SELECT sql FROM notebook_sql('examples/revenue.sql');
```

Charts, inputs, the dependency graph, the server and exports are designed but not built.

## Documents

- [docs/DESIGN.md](docs/DESIGN.md): architecture, execution model, storage, visualisation, export,
  risks and roadmap.
- [docs/FORMAT.md](docs/FORMAT.md): the notebook file format and chart spec.
- [docs/RESEARCH.md](docs/RESEARCH.md): the survey of existing tools behind the design, with sources
  and a list of what is not yet verified.
- [CLAUDE.md](CLAUDE.md): how the code is organised, how to build and test, and conventions.
- [examples/revenue.sql](examples/revenue.sql): an example notebook.
  [examples/revenue.expanded.sql](examples/revenue.expanded.sql) is the same notebook as a plain
  script that runs in any DuckDB 2.0 client.
