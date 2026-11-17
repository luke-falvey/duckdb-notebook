-- %% orders run=manual hide=all
CREATE OR REPLACE TABLE orders AS
SELECT
    TIMESTAMP '2026-01-01' + INTERVAL (i * 37 % 525600) MINUTE AS ts,
    ['Hardware', 'Software', 'Services'][1 + (i // 3) % 3] AS category,
    round(20 + (i * 7919 % 500) * (1 + (i % 3) * 0.25), 2) AS amount
FROM range(50000) t(i);

-- %% monthly
SELECT date_trunc('month', ts) AS month, category, sum(amount) AS revenue
FROM orders
GROUP BY ALL
ORDER BY month;
/* chart
title: Revenue by month
mark: line
x: month
y: {field: revenue, format: "$,.0f", title: Revenue}
color: category
*/
/* chart
title: Category mix
mark: bar
x: {field: revenue, agg: sum}
y: {field: category, sort: -x}
*/

-- %% chart revenue_kpi from=monthly span=4
/*
type: kpi
label: Revenue
value: {field: revenue, agg: sum, format: "$,.0f"}
sparkline: {x: month, y: revenue}
*/
