-- A sketchbook template: copy it, replace the data cell with your own source, and keep the shape.
-- Validate with .github/skills/sketchbook-authoring/scripts/check.sh <file>.

-- %% [notebook]
/*
title: Orders at a glance
format: 1
*/

-- %% [markdown]
/*
# Orders at a glance

One sentence on the question this sketchbook answers. The numbers below follow the inputs:
**{{ $region }}** took **{{ (SELECT format('${:,.0f}', sum(revenue)) FROM monthly) }}** across
**{{ (SELECT format('{:,}', sum(orders)) FROM monthly) }}** orders.
*/

-- %% orders run=manual hide=all
-- The data. Replace with read_parquet(...), an attached table or similar. Effect cells (CREATE ...)
-- only run when asked, so the source is read once.
CREATE OR REPLACE TABLE orders AS
SELECT
    i AS order_id,
    DATE '2026-01-01' + (i % 365)::INTEGER AS day,
    ['North', 'South', 'East', 'West'][1 + i % 4] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i // 4) % 3] AS category,
    round(40 + (i * 7919 % 400) * (1 + (i % 4) * 0.2), 2) AS amount
FROM range(20000) t(i);

-- %% regions hide=all
SELECT DISTINCT region FROM orders ORDER BY region;

-- %% input region select from=regions.region label="Region" span=6
SET VARIABLE region = 'North';

-- %% input min_amount slider min=0 max=500 step=25 label="Smallest order" span=6
SET VARIABLE min_amount = 0;

-- %% filtered hide=all
SELECT * FROM orders WHERE region = $region AND amount >= $min_amount;

-- %% chart revenue_kpi from=filtered span=4
/*
type: kpi
label: Revenue
value: {field: amount, agg: sum, format: "$,.0f"}
sparkline: {x: {field: day, timeunit: week}, y: {field: amount, agg: sum}}
*/

-- %% chart orders_kpi from=filtered span=4
/*
type: kpi
label: Orders
value: {field: order_id, agg: count, format: ",d"}
*/

-- %% chart average_kpi from=filtered span=4
/*
type: kpi
label: Average order
value: {field: amount, agg: avg, format: "$,.0f"}
*/

-- %% monthly hide=code
SELECT date_trunc('month', day) AS month, category, sum(amount) AS revenue, count(*) AS orders
FROM filtered
GROUP BY ALL
ORDER BY month, category;
/* chart
title: Revenue by month
mark: line
x: month
y: {field: revenue, format: "$,.0f", title: Revenue}
color: category
select: {as: picked_months, type: interval, on: x}
*/
/* chart
title: Category mix
mark: bar
x: {field: revenue, agg: sum, format: "$,.0f", title: Revenue}
y: {field: category, sort: -x}
color: category
*/

-- %% [markdown]
/*
## Largest orders

Drag across the line chart to narrow this list to a period.
*/

-- %% top_orders hide=code rows=10
SELECT order_id, day, category, amount
FROM filtered
WHERE $picked_months IS NULL OR day BETWEEN $picked_months.lo AND $picked_months.hi
ORDER BY amount DESC
LIMIT 50;
