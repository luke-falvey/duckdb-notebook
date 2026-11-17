---
name: sketchbook-authoring
description: 'Write, edit or fix a duckdb-notebook "sketchbook": a plain .sql file with -- %% cells (SQL, query charts, markdown with {{ }} values, inputs, chart cells, [notebook] metadata). Use when asked to create a notebook or sketchbook, add cells, wire inputs to queries, add prose or {{ }} values, organise cells, fix notebook errors or warnings from notebook_cells/notebook_run, or start from a template. Covers the cell format, naming, dependency rules, run options and validation.'
argument-hint: 'What the sketchbook should show, and where its data comes from'
---

# Writing a sketchbook

A sketchbook is a duckdb-notebook: a UTF-8 `.sql` file whose cells start with `-- %%`. Every byte is
DuckDB SQL or a SQL comment, so the file runs, diffs and lints like any script. The spec is
[docs/FORMAT.md](../../../docs/FORMAT.md); this skill is the working summary.

Related skills: **sketchbook-charts** (chart specs), **sketchbook-design** (making it look good),
**sketchbook-preview-export** (opening, checking visually, exporting).

## Procedure

1. **Start from the template.** Copy [template.sql](./assets/template.sql) to the target path (user
   notebooks usually live in `notebooks/`, which is not committed; examples for the repository go in
   `examples/` and are tested). It already follows every rule below.
2. **Point the data cell at the real source.** One effect cell (`run=manual`) that creates a table from
   files or an attached database, or no cell at all if the data is already a table. For other databases
   and extensions use the `[notebook]` cell's `attach:` and `extensions:` (paths are relative to the
   notebook).
3. **Write the questions as query cells**, top to bottom, each building on cells above it. Name every
   cell another cell reads, or that you want to keep a cached result for.
4. **Add charts to query cells** with `/* chart */` comments (see sketchbook-charts). The table stays as
   the last tab.
5. **Add inputs** for what a reader should vary, and **prose** with `{{ }}` values that state the
   findings.
6. **Validate**: `.github/skills/sketchbook-authoring/scripts/check.sh <file>` lists errors, warnings and
   cells that fail when run, and exits 1 on any error. Fix everything it reports.
7. **Look at it** (sketchbook-preview-export) and polish it (sketchbook-design).

## The cells

```sql
-- %% [notebook]                    metadata, first cell only: title, format, attach, extensions, timing
-- %% [markdown]                    prose in one /* ... */ block, with {{ expr }} values
-- %% input <name> <widget> opts    one SET VARIABLE <name> = <default>;
-- %% <name> opts                   SQL; a named query becomes a temp view called <name>
-- %%                               unnamed SQL
-- %% chart <name> from=<rel> opts  a chart of another cell or of a table
```

- **Names** match `[a-z_][a-z0-9_]*`, are unique, and `input`/`chart` are reserved. The name is the
  cell's identity; there are no generated IDs.
- **Query cells**: if the last statement returns rows (SELECT, FROM-first, VALUES, PIVOT, SUMMARIZE,
  DESCRIBE, ...) and the cell is named, it becomes `CREATE OR REPLACE TEMP VIEW <name>` at run time.
  The file keeps the bare query. `materialize=table` stores it as a temp table instead: use it for slow
  queries many cells read.
- **Effect cells** (CREATE, INSERT, ATTACH, COPY, ...) run only when asked; `run=manual|auto`
  overrides the default.
- **Query charts**: `/* chart` + YAML + `*/` after the query's last statement. See sketchbook-charts.
- **Inputs**: `text`, `number`, `slider` (`min max step`), `checkbox`, `date`, `daterange`
  (`STRUCT(lo, hi)`), `select`/`multiselect` (`options=a,b,c` or `from=<cell>.<column>`). All take
  `label="..."`. The body is a real `SET VARIABLE` with the default, so the file runs without the UI.
- **Markdown**: CommonMark headings, emphasis, lists, quotes, tables, fenced code, links. Raw HTML is
  shown as text. `{{ expr }}` is evaluated as `SELECT (expr)` once the cells it reads have run, so a
  summary may sit above the cells it quotes. Block comments nest: keep `/*` and `*/` balanced, write a
  literal `*/` as `*\/`.

Options any cell takes: `hide=code|output|all|none`, `span=1..12` (reading-view grid width),
`rows=1..10000` (table page size). Values are checked; anything else is an error on the cell.

## Parameters and variables

- Read inputs with `$name` (DuckDB 2.0 shorthand for `getvariable('name')`).
- `$name` on an unset variable is an error, so the runner sets variables that no input or `SET
  VARIABLE` defines (chart selections) to NULL first. Always guard selections:
  `WHERE $picked IS NULL OR day BETWEEN $picked.lo AND $picked.hi`.
- `notebook_run(path, params := {region: 'US'})` overrides input defaults and selections. A variable an
  SQL cell sets cannot be a parameter: make it an input.

## Rules that produce errors

| Problem | Fix |
|---|---|
| A SQL or input cell reads something defined further down | Move the definition up. Markdown and chart cells may read anything |
| A name defined by two cells, or a duplicate cell name | Rename one |
| A dependency cycle | Usually a cell reading a selection of a chart drawn from it: read the selection in another cell |
| A query cell's SQL reads its own chart's selection | Move the filtering query into a new cell below |
| Chart comments on a cell that does not end in a query | Make the last statement a SELECT, or use a chart cell |
| `chart cell needs from=<cell or table>` | Add `from=` |
| `[markdown]` / `chart` body is not a single block comment | Remove the `--` comment lines inside such cells; put notes in the prose |
| `hide=` / `rows=` value not valid | `hide=code|output|all|none`; `rows` 1 to 10000 |
| Input body is not `SET VARIABLE <same name> = ...;` | Make the statement and the head name match |

Warnings do not stop a run but are worth fixing: a variable no cell sets (reads as NULL), a
`/* chart */` comment that is not at the end of its cell (not drawn), SQL the parser cannot read.

## Good habits

- One idea per cell, and name cells after what they hold (`monthly`, `top_orders`), not `q1`.
- Do filtering once (`filtered`) and build everything else on it, so an input changes every chart.
- Aggregate in SQL or in the chart spec, never ship raw rows to look at: the table pages 25 rows at a
  time and charts compile to aggregate queries.
- Keep heavy loading in one `run=manual` effect cell so editing does not reload data.
- Format numbers inside `{{ }}` with DuckDB: `format('{:,}', n)`, `format('{:,.1f}', x)`,
  `format('${:,.0f}', x)`, `strftime(d, '%B %Y')`, and `coalesce(..., 'none')` for possible NULLs.
- Comments above a query stay above the generated `CREATE VIEW`, so explain intent there.
- Never hold state outside the file: page size and hide switches in the UI write `rows=` and `hide=`.

## Useful commands (in `./build/release/duckdb`)

```sql
SELECT idx, kind, name, defines, reads, depends_on, errors, warnings FROM notebook_cells('nb.sql');
SELECT idx, name, status, row_count, error FROM notebook_run('nb.sql');
SELECT sql FROM notebook_sql('nb.sql');   -- the same notebook as a plain script
```
