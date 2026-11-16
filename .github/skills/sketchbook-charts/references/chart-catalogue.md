# Chart catalogue

Every spec here is from [examples/charts.sql](../../../../examples/charts.sql), which the browser tests
draw without errors. Paste a spec into a `/* chart ... */` comment after the query it draws, or into a
`-- %% chart <name> from=<relation>` cell. Column names are the example's; replace them with yours.

Example relations: `daily(day DATE, region, category, revenue, orders)`, `gallery_sales(day, region,
category, product, amount, x, y, lat, lon, stage, id)`, `flows(source, target, value)`,
`prices(day, open, high, low, close)`, `gallery_tasks(task, start_date, end_date, phase)`.

## Marks on axes

Bar, with a click selection other charts can filter on:
```yaml
title: Revenue by region (click a bar to filter)
mark: bar
x: region
y: {field: revenue, agg: sum, format: "$,.0f"}
color: region
select: {as: picked_region, type: point, on: x}
```

Line by month, filtered by that selection:
```yaml
title: Daily revenue by category, filtered by the bar chart
mark: line
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum}
color: category
filter: picked_region
```

Area:
```yaml
title: Monthly orders
mark: area
x: {field: day, timeunit: month}
y: {field: orders, agg: sum}
```

Ranked horizontal bars:
```yaml
title: Category mix
mark: bar
x: {field: revenue, agg: sum}
y: {field: category, sort: -x}
```

Histogram:
```yaml
title: Order sizes
mark: histogram
x: {field: amount, bin: {maxbins: 25}}
```

Scatter (binned to the pixel grid past 5,000 rows):
```yaml
title: 20,000 points, binned to the pixel grid
mark: point
x: x
y: y
```

Box plot:
```yaml
title: Amount by region
mark: boxplot
x: region
y: amount
```

Heatmap:
```yaml
title: Average amount by weekday and category
mark: rect
x: {field: day, timeunit: dayofweek}
y: category
color: {field: amount, agg: avg, scheme: viridis}
```

Pie or donut (keep to a few slices):
```yaml
title: Share by category
mark: arc
theta: {field: amount, agg: sum}
color: category
```

Layers, a value with its average:
```yaml
title: Daily revenue with its average
layers:
  - {mark: area, x: day, y: {field: revenue, agg: sum}, opacity: 0.3}
  - {mark: rule, y: {field: revenue, agg: avg}, color: {value: "#D55E00"}}
```

Small multiples:
```yaml
title: Monthly revenue, one panel per region
mark: bar
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum}
facet: {col: region}
```

Brush a period (interval selection) for cells below to read as `$picked_months`:
```yaml
title: Monthly revenue by category
mark: line
x: month
y: {field: revenue, format: "$,.0f"}
color: category
select: {as: picked_months, type: interval, on: x}
```

## Typed charts

KPI with a sparkline (good at `span=3` or `span=4` in a row of KPIs):
```yaml
type: kpi
label: Revenue
value: {field: amount, agg: sum, format: "$,.0f"}
sparkline: {x: day, y: amount}
```

Gauge:
```yaml
type: gauge
label: Average order
value: {field: amount, agg: avg, format: ",.1f"}
min: 0
max: 150
bands: [{from: 0, to: 50, color: "#D55E00"}, {from: 50, to: 150, color: "#009E73"}]
```

Funnel:
```yaml
type: funnel
stage: stage
value: {field: id, agg: count}
```

Styled table with in-cell bars:
```yaml
type: table
columns: [day, region, revenue]
bar: revenue
limit: 8
```

Pivot with heat shading:
```yaml
type: pivot
rows: [region]
columns: category
values: {field: amount, agg: sum}
heat: true
```

Map (points, `heat`, `hex`, or `arc` with `lat2`/`lon2`; `geometry` draws a choropleth with the
spatial extension; no basemap):
```yaml
type: map
lat: lat
lon: lon
layer: heat
color: {field: amount, agg: avg}
```

Calendar heatmap:
```yaml
type: calendar
date: day
value: revenue
```

Sankey and chord:
```yaml
type: sankey
source: source
target: target
value: value
```
```yaml
type: chord
source: source
target: target
value: value
```

Treemap, sunburst, icicle:
```yaml
type: treemap
path: [category, product]
value: amount
```
```yaml
type: sunburst
path: [category, product]
value: amount
```
```yaml
type: icicle
path: [region, category]
value: amount
```

Network (hand-drawn up to 300 nodes and 400 edges, plain beyond):
```yaml
type: network
source: source
target: target
weight: value
```

Candlestick:
```yaml
type: candlestick
x: day
open: open
high: high
low: low
close: close
```

Gantt:
```yaml
type: gantt
task: task
start: start_date
end: end_date
color: phase
```

## Raw specs

The built-in renderer draws a Vega-Lite spec's `mark` and `encoding`, or an ECharts `series` of type
line, bar, scatter or pie. Rows are passed as they are (up to `limit`, default 5,000), so aggregate in
SQL first. Use these only when the vocabulary above runs out.

```yaml
type: vega-lite
spec:
  mark: point
  encoding:
    x: {field: day, type: temporal}
    y: {field: revenue, type: quantitative}
    color: {field: region, type: nominal}
```
```yaml
type: echarts
spec:
  series:
    - type: pie
      encode: {itemName: target, value: value}
```
