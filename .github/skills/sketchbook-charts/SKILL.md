---
name: sketchbook-charts
description: 'Write chart specs for duckdb-notebook sketchbooks: /* chart */ comments on query cells (shown as tabs) and -- %% chart cells. Use when asked to chart, plot, visualise or graph a result; pick a chart type (line, bar, area, scatter, histogram, heatmap, pie, boxplot, KPI, gauge, pivot, map, sankey, treemap, gantt, candlestick and more); add brushing, click selection or cross-filtering; fix chart errors; or tune axes, formats, colours, layers and facets.'
argument-hint: 'The result to chart and the question the chart should answer'
---

# Charts in a sketchbook

A chart is a short YAML spec. The extension compiles it to one aggregate SQL query (grouping,
binning, time truncation and downsampling all happen in DuckDB) plus a render description the browser
draws by hand, Excalidraw-style. Spec reference: [docs/FORMAT.md §5](../../../docs/FORMAT.md#5-charts).
Every chart type with a working spec: [chart-catalogue.md](./references/chart-catalogue.md), taken
from [examples/charts.sql](../../../examples/charts.sql).

## Where a chart goes

**At the end of the query it draws** (the normal case). Each `/* chart */` comment after the last
statement is a tab; the result table is always the last tab, and the first chart opens by default.

```sql
-- %% monthly
SELECT date_trunc('month', day) AS month, category, sum(amount) AS revenue
FROM filtered GROUP BY ALL ORDER BY month;
/* chart
title: Revenue by month
mark: line
x: month
y: {field: revenue, format: "$,.0f"}
color: category
*/
```

- Only comments after the last statement count; elsewhere they are plain comments (with a warning).
- The cell must end in a query. It need not be named.
- `width=` and `height=` on the cell's head size its charts (default 640 by 320, scaled to fit).

**As a chart cell** when the chart reads another cell or a table no cell defines, or when several
charts of one result should sit side by side in reading view (tabs show one at a time):

```sql
-- %% chart revenue_kpi from=monthly span=4
/*
type: kpi
value: {field: revenue, agg: sum, format: "$,.0f"}
label: Revenue
*/
```

A chart cell's body must be exactly one block comment: no `--` lines.

## Procedure

1. Decide what the reader should see first; that chart goes first. Prefer one clear chart per question.
2. Pick the chart from the table below, then copy its spec from the catalogue.
3. Name columns exactly as the query outputs them. A bare name that is not a column becomes a
   constant (`color: red`), so a typo silently draws nothing useful: use `{field: name}` to get an error
   instead.
4. Give it a `title` that states the point ("Revenue fell after June"), axis `title`s when column names
   are cryptic, and a `format` for every money, percentage or large number.
5. Run the cell (Shift+Enter) or `check.sh` from sketchbook-authoring. In the editor, "edit chart spec"
   under a chart tab edits it; Shift+Enter there redraws without re-running the query.

## Choosing a chart

| Question | Spec |
|---|---|
| Change over time | `mark: line`, `x:` a date (add `timeunit: month` to bucket), `color:` for series |
| Volume over time | `mark: area` or `mark: bar` with `timeunit` |
| Compare categories | `mark: bar`, category on `y` with `sort: -x` for ranked horizontal bars |
| Part of a whole | `mark: bar` stacked by `color`; `mark: arc` only for 2 to 5 slices |
| Distribution | `mark: histogram` (`bin: {maxbins: 25}`), `mark: boxplot`, `mark: density` |
| Two measures | `mark: point` (binned automatically past 5,000 rows) or `mark: hexbin` |
| Two categories and a measure | `mark: rect` with `color: {field, agg, scheme}`, or `type: pivot` with `heat: true` |
| One headline number | `type: kpi` (with `sparkline`), `type: gauge` with `bands` |
| Flows | `type: sankey`, `type: chord`, `type: network` |
| Hierarchy | `type: treemap`, `sunburst`, `icicle` with `path: [a, b]` |
| Steps, schedules, prices | `type: funnel`, `type: gantt`, `type: candlestick` |
| Days of the year | `type: calendar` |
| Places | `type: map` with `lat`/`lon` (or `geometry` with the spatial extension); no basemap offline |
| A styled table | `type: table` with `columns`, `format`, `bar`, `heat`, `limit` |
| Same chart per group | add `facet: {col: region}` (or `row`) |
| A reference line or band | `layers:` with a `rule` (`agg: avg`) over an `area` or `line` |

## Channels in one look

`x y x2 y2 color size opacity shape text tooltip order theta`, each a column name or a mapping:
`field`, `agg` (`sum avg min max count count_distinct median p05..p99`), `bin`, `timeunit` (`year
quarter month week date hour minute second`, or cyclic `monthofyear dayofmonth dayofweek hours
minutes`), `sort` (`asc`, `desc`, `-x`), `type` (`quantitative temporal ordinal nominal`), `scale`
(`{type: log}`, `{domain: [0, 100]}`, `{zero: false}`), `scheme`, `format`, `title`, `value`.
Channels without `agg` become GROUP BY keys when another channel aggregates.

Spec keys: `title`, `limit` (rows a query may return, default 10,000), `sample: <n>`, `facet`,
`layers`, `select`, `filter`.

**Formats** (d3 subset the renderer understands): `[$][,][.N][~][f|d|s|e|%]`, for example `"$,.0f"`,
`",d"`, `".1%"` (multiplies by 100), `".2s"` (12.3k), `"~f"` (trim zeros). Quote them in YAML.

**Colour schemes** for continuous colour: `blues` (default), `greens`, `reds`, `oranges`, `purples`,
`greys`, `viridis`, `magma`. Categories use the Excalidraw palette in order (blue, orange, green, red,
violet, cyan, yellow, indigo, ...), filled with matching pastels. To give categories fixed colours
(statuses, traffic lights), set the order and colours on the channel:
`color: {field: status, scale: {domain: [PASS, WARNING, ERROR], range: ["#2f9e44", "#f08c00", "#e03131"]}}`.

## Selections and cross-filtering

```yaml
select: {as: picked_months, type: interval, on: x}     # drag to brush; click clears
select: {as: picked_region, type: point, on: x}        # click a mark; Shift adds
```

- The variable is `STRUCT(lo, hi)` for an interval (`TIMESTAMP` bounds on a date axis), a `LIST` for
  points, NULL with nothing selected. Cells that read `$picked_months` re-run.
- `filter: picked_region` (or a list) on another chart filters that chart without SQL, matching fields
  by name. A query cell's charts may filter on any selection, including one from the same cell or a
  cell below; that only redraws the chart, the query does not re-run.
- A cell's SQL must not read its own chart's selection (error). Put the filtered query in a new cell:

```sql
-- %% detail
SELECT * FROM filtered
WHERE $picked_months IS NULL OR day BETWEEN $picked_months.lo AND $picked_months.hi;
```

## Hover values

Pointing at a mark shows its channels' values (formatted with each channel's `format` and named by its
`title`), plus any `tooltip:` fields. On lines and areas the nearest point answers, and a bar answers
anywhere in its column, so give channels readable `title`s and `format`s: they are what the reader
sees there. Dates truncated with `timeunit` read in that unit ("Jun 2026").

## Large data

You do not need to pre-aggregate for size: `line` is reduced to four points per pixel (M4), and
`point`, `rect`, `tick` over two numeric axes are binned past 5,000 rows. Pre-aggregate for meaning
instead. Charts with more than 600 marks are drawn with plain (not hand-drawn) shapes so they stay fast.

## Checking what a spec compiles to

```sql
FROM notebook_chart_query('mark: bar
x: region
y: {field: amount, agg: sum}', 'orders');
```

Returns the SQL and render description; use it to debug a chart that draws the wrong thing.

## Common mistakes

- `y: revenue` on a bar chart with many rows per x: add `agg: sum`, or bars overdraw each other.
- Dates stored as text: cast in SQL (`day::DATE`) so the axis is temporal.
- `mark: arc` with 15 slices: use ranked bars.
- More than about 6 series in `color`: filter to the top few in SQL, or facet.
- A chart error marks the cell failed but keeps its table; the chart's tab shows ⚠ and the message.
