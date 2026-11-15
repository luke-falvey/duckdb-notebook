# duckdb-notebook

A notebook environment for DuckDB where the notebook is a plain `.sql` file.

**Status: design phase.** This repository currently holds the design, not an implementation.

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

## Documents

- [docs/DESIGN.md](docs/DESIGN.md): architecture, execution model, storage, visualisation, export,
  risks and roadmap.
- [docs/FORMAT.md](docs/FORMAT.md): the notebook file format and chart spec.
- [docs/RESEARCH.md](docs/RESEARCH.md): the survey of existing tools behind the design, with sources
  and a list of what is not yet verified.
- [examples/revenue.sql](examples/revenue.sql): an example notebook.
  [examples/revenue.expanded.sql](examples/revenue.expanded.sql) is the same notebook as a plain
  script that runs in any DuckDB 2.0 client.
