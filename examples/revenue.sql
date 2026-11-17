-- %% [notebook]
/*
title: Revenue review
format: 1
*/

-- %% [markdown]
/*
# Revenue review

Synthetic orders for 2026, generated in the `orders` cell so the notebook has
no external dependencies. Total revenue in **{{ $region }}** so far:
**{{ (SELECT sum(revenue) FROM monthly) }}**.
*/

-- %% orders run=manual
-- Effect cell: creates a table, so it only runs when asked.
CREATE OR REPLACE TABLE orders AS
SELECT
    i AS order_id,
    TIMESTAMP '2026-01-01' + INTERVAL (i * 37 % 525600) MINUTE AS ts,
    ['EU', 'US', 'APAC'][1 + i % 3] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i // 3) % 3] AS category,
    round(20 + (i * 7919 % 500) * (1 + (i % 3) * 0.25), 2) AS amount
FROM range(50000) t(i);

-- %% regions
SELECT DISTINCT region AS name FROM orders ORDER BY name;

-- %% input region select from=regions.name label="Region"
SET VARIABLE region = 'EU';

-- %% input min_amount slider min=0 max=500 step=10 label="Minimum order value"
SET VARIABLE min_amount = 0;

-- %% filtered
SELECT *
FROM orders
WHERE region = $region
  AND amount >= $min_amount;

-- %% monthly
SELECT
    date_trunc('month', ts) AS month,
    category,
    sum(amount) AS revenue,
    count(*) AS orders
FROM filtered
GROUP BY ALL
ORDER BY month, category;
/* chart
title: Monthly revenue by category
mark: line
x: month
y: {field: revenue, format: "$,.0f"}
color: category
rules:
  - {y: avg, label: "average {value}"}
notes:
  - {x: [2026-08-01, 2026-12-01], text: fewer orders in the second half}
select: {as: picked_months, type: interval, on: x}
*/
/* chart
title: Category mix
mark: bar
x: {field: revenue, agg: sum}
y: {field: category, sort: -x}
*/

-- %% chart headline from=monthly
/*
type: kpi
value: {field: revenue, agg: sum, format: "$,.0f"}
label: Revenue
sparkline: {x: month, y: revenue}
*/

-- %% chart order_sizes from=filtered
/*
title: Order size by weekday and hour
mark: rect
x: {field: ts, timeunit: hours}
y: {field: ts, timeunit: day}
color: {field: amount, agg: avg, scheme: blues}
*/

-- %% top_orders
-- `picked_months` is set by brushing the trend chart; NULL means no brush.
SELECT order_id, ts, category, amount
FROM filtered
WHERE $picked_months IS NULL
   OR ts BETWEEN $picked_months.lo AND $picked_months.hi
ORDER BY amount DESC
LIMIT 20;
