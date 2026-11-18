# Chart spec

Every chart type, each drawn by the cell shown under it. The cells are taken from
[`examples/charts.sql`](https://github.com/luke-falvey/duckdb-sketchbook/blob/main/examples/charts.sql)
when this site is built, and the pictures are screenshots of that notebook's HTML export, which you
can also [open live](../examples/index.md). The grammar itself (every key and what it means) is in
[the format specification, §5](format.md#5-charts); this page is the catalogue and the values each
key accepts.

The examples read these relations, made by the gallery's first cells:

<!-- cell gallery_data -->

<!-- cell daily -->

<!-- cell flows -->

<!-- cell prices -->

## Keys at a glance

A spec is YAML: either at the end of a query cell inside `/* chart … */`, or as the body of a
`-- %% chart <name> from=<relation>` cell.

| Key | Takes |
|---|---|
| `mark` | `line`, `area`, `bar`, `point` (or `circle`, `square`), `rect`, `rule`, `tick`, `text`, `arc`, `boxplot`, `histogram`, `density`, `hexbin` |
| Mark options | Beside `mark`: `opacity` (a number), `strokeWidth` (lines), `point: true` (dots on a line), `innerRadius` (an `arc` as a donut, in pixels). `stack: true` or `zero` on bars (coloured bars are always stacked) and `interpolate: linear` on lines are accepted; any other `stack` or `interpolate`, and `cornerRadius`, are an error |
| `type` | `kpi`, `gauge`, `table`, `pivot`, `map`, `sankey`, `chord`, `treemap`, `sunburst`, `icicle`, `network`, `funnel`, `candlestick`, `calendar`, `gantt`, `vega-lite`, `echarts`. Use instead of `mark` |
| Channels | `x`, `y`, `x2`, `y2`, `color`, `size`, `opacity`, `shape`, `text`, `tooltip`, `order`, `theta`: a column name, or a mapping of the channel keys below |
| `title` | The chart's title; in a query cell's tabs, the tab's label |
| `layers` | A list of specs drawn in one frame, sharing data and scales |
| `rules` | Reference lines: `[{y: avg, label: "average {value}"}, {x: 2026-06-01}]`. See [below](#rules-and-bands) |
| `bands` | Shaded ranges: `[{y: [p25, p75], label: middle half}]` |
| `notes` | Short remarks with an arrow to a data position: `[{x: 2026-02-01, text: short month}]`. See [below](#notes) |
| `facet` | `{row: col}`, `{col: col}` or both: one panel per value |
| `limit` | Rows a chart query may return. Default 10,000 (5,000 for raw specs) |
| `sample` | `<n>`: draw a random sample of n rows instead of downsampling |
| `select` | `{as: <variable>, type: interval \| point, on: x \| y \| [x, y]}`: see [selections](../guides/selections.md) |
| `filter` | A selection's variable name, or a list of them: filter this chart by another chart's selection |

### Channel keys

| Key | Takes |
|---|---|
| `field` | A column of the chart's relation. Writing `{field: name}` instead of a bare name turns a typo into an error rather than a constant |
| `agg` (or `aggregate`) | `sum`, `avg` (or `mean`), `min`, `max`, `count`, `count_distinct` (or `distinct`), `median`, `p05` … `p99`. Channels without `agg` become `GROUP BY` keys when any channel has one |
| `bin` | `true`, `{step: n}` or `{maxbins: n}` |
| `timeunit` | Truncate: `year`, `quarter`, `month`, `week`, `date`, `hour`, `minute`, `second`. Extract a cyclic part: `monthofyear`, `dayofmonth`, `dayofweek` (or `day`; 0 is Sunday), `hours` (or `hourofday`), `minutes` |
| `sort` | `asc`, `desc`, or a channel with an optional `-` (`-x` sorts by x, descending) |
| `type` | `quantitative`, `temporal`, `ordinal`, `nominal`. Inferred from the column type if left out |
| `scale` | `{type: log}`, `{domain: [0, 100]}`, `{zero: true}` or `{zero: false}`. On a categorical `color`, `{domain: [...], range: [...]}` fixes each value's colour |
| `scheme` | A continuous colour scheme (below) |
| `format` | A number format (below) |
| `title` | The axis or legend title, and the name shown on hover |
| `value` | A constant: `color: {value: "#D55E00"}`. A bare name that is not a column is also a constant |

### Number formats

A subset of d3-format: `[$][,][.N][~][f|d|s|e|%]`. Quote them in YAML.

| Format | 1234567.891 | 0.256 |
|---|---|---|
| `"$,.0f"` | $1,234,568 | $0 |
| `",d"` | 1,234,568 | 0 |
| `",.1f"` | 1,234,567.9 | 0.3 |
| `".2f"` | 1234567.89 | 0.26 |
| `".2s"` | 1.23M | 0.26 |
| `".1%"` | 123456789.1% | 25.6% |
| `"~f"` | 1234568 | 0.256 |
| none | 1,234,568 | 0.256 |

`%` multiplies by 100, so use it for ratios. `~` trims trailing zeros. Without `.N`, the number of
decimals follows the size of the number. Values shown on hover use the channel's `format` too.

### Colours

- **Categories** take the Excalidraw palette in order (blue, orange, green, red, violet, cyan, yellow,
  indigo, …), filled with matching pastel hatching. Keep to about six; filter to the top few in SQL,
  or facet.
- **Fixed colours per value**, for statuses or traffic lights:
  `color: {field: status, scale: {domain: [PASS, WARNING, ERROR], range: ["#2f9e44", "#f08c00", "#e03131"]}}`.
- **Continuous schemes**: `blues` (default), `greens`, `reds`, `oranges`, `purples`, `greys`,
  `viridis`, `magma`.

### Sizes

The cell's `width=` and `height=` options size its charts: 640 by 320 by default, scaled to fit the
cell. Charts with more than 600 marks in a layer are drawn with plain shapes instead of hand-drawn
ones, so they stay fast.

## Marks on axes

### Bar, with a click selection

Other charts can `filter` on `picked_region`.

<!-- cell revenue_by_region -->

### Line

Filtered by the bar chart's selection, with days bucketed into months by `timeunit`.

<!-- cell trend -->

### Area

<!-- cell stacked_area -->

### Histogram

<!-- cell histogram -->

### Scatter

Over 5,000 rows, `point` over two numeric axes is binned to a grid of 4-pixel cells, each drawn by
its row count. `sample: 5000` asks for a random sample instead.

<!-- cell scatter -->

### Box plot

<!-- cell boxes -->

### Heatmap

<!-- cell heat -->

### Pie and donut

Keep to two to five slices; otherwise use ranked bars (`y: {field: category, sort: -x}`).

<!-- cell pie -->

### Layers

A value with its average as a reference line.

<!-- cell layered -->

### Small multiples

<!-- cell facets -->

## Rules, bands and notes

### Rules and bands

`rules:` draws reference lines and `bands:` shades ranges, behind the marks in muted ink. Each item
names one axis, `x` or `y`: a rule takes one value, a band two (`[from, to]`).

| A value written as | Means |
|---|---|
| A number, date or category: `150000`, `2026-06-01`, `Hardware` | That position on the axis |
| `avg` (or `mean`), `median`, `min`, `max`, `p05` … `p99` | That aggregate of what the chart draws on the axis, across all series and panels. With `y: {field: revenue, agg: sum}` by month, `avg` is the average monthly sum. Numeric or date axes; dates have no `avg` |
| `{field: budget, agg: max}` | An aggregate of a column of the relation |

Each item may also have a `label` (`{value}` is replaced by the value in the axis's format; in a band,
`{from}` and `{to}`) and a `color`. The axis grows to include a rule or band outside the data. Quote a
category that reads like an aggregate (`x: "max"`). Rules and bands need a chart with axes (not
`arc`); `boxplot` takes values but not aggregates.

<!-- cell references -->

### Notes

`notes:` writes short remarks on the chart, each with a curved arrow to a data position, so it stays
with the data when the chart is resized or the query returns other rows.

| Key | Meaning |
|---|---|
| `x`, `y` | The position, in each axis's values. With only one, the note points at the mark there (the top of the bar or highest line at that `x`). One may be a range, `[from, to]`, drawn as a bracket |
| `text` | A few words; wrapped at about 18 characters and cut after four lines. Required unless the note is a `circle` or a range |
| `circle` | `true` draws a ring round the point |
| `side` | `above`, `below`, `left` or `right`. Without it, the side with most room is chosen |
| `dx`, `dy` | Pixels to move the text from where it was placed |
| `color` | The note's ink colour |

Positions are values, not aggregates. A position outside the axes is drawn at the edge; a category
the data does not have draws nothing. In a faceted chart each note is drawn in every panel.

<!-- cell annotated -->

`examples/revenue.sql` uses both on its trend chart: a rule at the average and a note over the second
half of the year. Any other key or shape in `rules`, `bands` or `notes` is a chart error. The full
rules are in [the format, §5.1](format.md#51-grammar-charts).

## Typed charts

### KPI

`compare` shows a delta against another channel, `sparkline: {x, y}` a small line, and
`bands: [{from, to, color}]` colours the value by the band it falls in (ends inclusive; either may be
left out).

<!-- cell kpi -->

### Gauge

`min`, `max`, `bands` and `label`.

<!-- cell gauge -->

### Funnel

<!-- cell funnel -->

### Styled table

`columns` picks and orders columns, `format: {col: spec}` formats them, `bar: col` draws in-cell bars,
`heat: col` shades cells. `tip: {col: source}` shows the row's `source` value when the reader points at a cell
of `col` (a list, `{col: [a, b]}`, shows several, one per line); those cells are underlined with dots, and the
sources are not shown as columns of their own unless `columns` lists them.

<!-- cell table -->

### Pivot

<!-- cell pivot -->

### Map

`lat` and `lon` (or `geometry`, with the `spatial` extension; polygons draw as a choropleth).
`layer` is `point`, `heat`, `hex` or `arc` (`arc` needs `lat2` and `lon2`). There is no basemap:
tiles are not available offline.

<!-- cell map -->

### Calendar

<!-- cell calendar -->

### Sankey and chord

<!-- cell sankey -->

<!-- cell chord -->

### Treemap, sunburst and icicle

`path` lists the columns from the outermost level in.

<!-- cell treemap -->

<!-- cell sunburst -->

<!-- cell icicle -->

### Network

Hand-drawn up to 300 nodes and 400 edges, plain beyond.

<!-- cell network -->

### Candlestick

<!-- cell candles -->

### Gantt

<!-- cell gantt -->

## Raw specs

When the vocabulary runs out, embed a Vega-Lite or ECharts spec under `spec:`. The relation's rows
are passed as they are (up to `limit`, default 5,000), with no pushdown or downsampling, so aggregate
in SQL first. No Vega or ECharts library is bundled: the built-in renderer draws a Vega-Lite spec's
`mark` and `encoding` (`x`, `y`, `color`, `size`, `text`, `theta`), and an ECharts `series` of type
`line`, `bar`, `scatter` or `pie` with `encode`. Anything else is shown as a table.

<!-- cell raw_vega -->

<!-- cell raw_echarts -->

## See what a spec compiles to

`notebook_chart_query(spec, relation)` returns the SQL a spec compiles to and the render description
sent to the browser. Use it when a chart draws something unexpected:

```sql
--8<-- "site/outputs/chart_query.sql"
```

```text
--8<-- "site/outputs/chart_query.txt"
```
