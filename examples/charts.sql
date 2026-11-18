-- %% [notebook]
/*
title: Chart gallery
format: 1
*/

-- %% [markdown]
/*
# Chart gallery

Every chart type the notebook draws, on synthetic data made in the first cell. Each chart cell is
a short YAML spec; the extension turns it into one or more aggregate queries, so the browser only
ever receives the rows the picture needs.
*/

-- %% gallery_data run=manual
CREATE OR REPLACE TABLE gallery_sales AS
SELECT
    i AS id,
    DATE '2026-01-01' + (i % 365)::INTEGER AS day,
    ['North', 'South', 'East', 'West'][1 + i % 4] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i // 4) % 3] AS category,
    ['Laptops', 'Phones', 'Licences', 'Support', 'Training'][1 + (i // 12) % 5] AS product,
    CASE WHEN i * 7919 % 100 < 50 THEN 'visit' WHEN i * 7919 % 100 < 75 THEN 'signup'
        WHEN i * 7919 % 100 < 90 THEN 'trial' ELSE 'purchase' END AS stage,
    round(10 + (i * 7919 % 1000) / 10.0 + (i % 4) * 15, 2)::DECIMAL(10, 2) AS amount,
    (i * 31 % 997) / 10.0 AS x,
    (i * 31 % 997) / 10.0 * 0.8 + (i * 17 % 211) / 5.0 AS y,
    -36.85 + (i * 7919 % 400) / 1000.0 * ((i % 7) / 7.0) AS lat,
    174.76 + (i * 104729 % 500) / 1000.0 * ((i % 5) / 5.0) AS lon
FROM range(20000) t(i);
CREATE OR REPLACE TABLE gallery_tasks AS
SELECT * FROM (VALUES
    ('Design', DATE '2026-01-05', DATE '2026-02-10', 'Plan'),
    ('Build', DATE '2026-02-01', DATE '2026-04-20', 'Make'),
    ('Test', DATE '2026-04-01', DATE '2026-05-15', 'Make'),
    ('Launch', DATE '2026-05-15', DATE '2026-05-30', 'Ship')
) t(task, start_date, end_date, phase);

-- %% daily
SELECT day, region, category, sum(amount) AS revenue, count(*) AS orders
FROM gallery_sales
GROUP BY ALL
ORDER BY ALL;

-- %% flows
SELECT category AS source, product AS target, sum(amount) AS value
FROM gallery_sales
GROUP BY ALL
ORDER BY ALL;

-- %% prices
SELECT
    day,
    first(amount ORDER BY id) AS open,
    max(amount) AS high,
    min(amount) AS low,
    last(amount ORDER BY id) AS close
FROM gallery_sales
WHERE day < DATE '2026-03-01'
GROUP BY day;

-- %% chart revenue_by_region from=daily span=6
/*
title: Revenue by region (click a bar to filter)
mark: bar
x: region
y: {field: revenue, agg: sum, format: "$,.0f"}
color: {field: region, scale: {domain: [North, South, East, West], range: ["#1971c2", "#2f9e44", "#e8590c", "#9c36b5"]}}
select: {as: picked_region, type: point, on: x}
*/

-- %% chart trend from=daily span=6
/*
title: Daily revenue by category, filtered by the bar chart
mark: line
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum}
color: category
filter: picked_region
*/

-- %% chart stacked_area from=daily span=6
/*
title: Monthly orders
mark: area
x: {field: day, timeunit: month}
y: {field: orders, agg: sum}
*/

-- %% chart histogram from=gallery_sales span=6
/*
title: Order sizes
mark: histogram
x: {field: amount, bin: {maxbins: 25}}
*/

-- %% chart scatter from=gallery_sales span=6
/*
title: 20,000 points, binned to the pixel grid
mark: point
x: x
y: y
*/

-- %% chart boxes from=gallery_sales span=6
/*
title: Amount by region
mark: boxplot
x: region
y: amount
*/

-- %% chart heat from=gallery_sales span=6
/*
title: Average amount by weekday and category
mark: rect
x: {field: day, timeunit: dayofweek}
y: category
color: {field: amount, agg: avg, scheme: viridis}
*/

-- %% chart pie from=gallery_sales span=6
/*
title: Share by category
mark: arc
theta: {field: amount, agg: sum}
color: category
*/

-- %% chart layered from=daily
/*
title: Daily revenue with its average
layers:
  - {mark: area, x: day, y: {field: revenue, agg: sum}, opacity: 0.3}
  - {mark: rule, y: {field: revenue, agg: avg}, color: {value: "#D55E00"}}
*/

-- %% chart facets from=daily
/*
title: Monthly revenue, one panel per region
mark: bar
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum}
facet: {col: region}
*/

-- %% chart kpi from=gallery_sales span=4
/*
type: kpi
label: Revenue
value: {field: amount, agg: sum, format: "$,.0f"}
sparkline: {x: day, y: amount}
bands: [{to: 1000000, color: "#e03131"}, {from: 1000000, color: "#2f9e44"}]
*/

-- %% chart gauge from=gallery_sales span=4
/*
type: gauge
label: Average order
value: {field: amount, agg: avg, format: ",.1f"}
min: 0
max: 150
bands: [{from: 0, to: 50, color: "#D55E00"}, {from: 50, to: 150, color: "#009E73"}]
*/

-- %% chart funnel from=gallery_sales span=4
/*
type: funnel
stage: stage
value: {field: id, agg: count}
*/

-- %% chart table from=daily span=6
/*
type: table
columns: [day, region, revenue]
tip: {region: [category, orders]}
link: {region: "https://example.com/regions/{region}"}
bar: revenue
limit: 8
*/

-- %% chart pivot from=gallery_sales span=6
/*
type: pivot
rows: [region]
columns: category
values: {field: amount, agg: sum}
heat: true
*/

-- %% chart map from=gallery_sales span=6
/*
type: map
lat: lat
lon: lon
layer: heat
color: {field: amount, agg: avg}
*/

-- %% chart calendar from=daily span=6
/*
type: calendar
date: day
value: revenue
*/

-- %% chart sankey from=flows span=6
/*
type: sankey
source: source
target: target
value: value
*/

-- %% chart chord from=flows span=6
/*
type: chord
source: source
target: target
value: value
*/

-- %% chart treemap from=gallery_sales span=4
/*
type: treemap
path: [category, product]
value: amount
*/

-- %% chart sunburst from=gallery_sales span=4
/*
type: sunburst
path: [category, product]
value: amount
*/

-- %% chart icicle from=gallery_sales span=4
/*
type: icicle
path: [region, category]
value: amount
*/

-- %% chart network from=flows span=6
/*
type: network
source: source
target: target
weight: value
*/

-- %% chart candles from=prices span=6
/*
type: candlestick
x: day
open: open
high: high
low: low
close: close
*/

-- %% chart gantt from=gallery_tasks span=6
/*
type: gantt
task: task
start: start_date
end: end_date
color: phase
*/

-- %% chart raw_vega from=daily span=6
/*
type: vega-lite
spec:
  mark: point
  encoding:
    x: {field: day, type: temporal}
    y: {field: revenue, type: quantitative}
    color: {field: region, type: nominal}
*/

-- %% chart raw_echarts from=flows span=6
/*
type: echarts
spec:
  series:
    - type: pie
      encode: {itemName: target, value: value}
*/

-- %% chart references from=daily
/*
title: Monthly revenue against its average and a target range
mark: line
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum, format: "$,.0f"}
rules:
  - {y: avg, label: "average {value}"}
  - {x: 2026-07-01, label: price change}
bands:
  - {y: [136000, 141000], label: target}
*/

-- %% chart annotated from=daily
/*
title: Notes point at what matters
mark: line
x: {field: day, timeunit: month}
y: {field: revenue, agg: sum, format: "$,.0f"}
notes:
  - {x: 2026-02-01, text: "February is a short month", circle: true}
  - {x: [2026-05-01, 2026-08-01], text: steady summer}
  - {x: 2026-11-01, text: "stock ran out", side: below}
*/
