-- Expanded from examples/revenue.sql: runs in any DuckDB client, no extension needed.
-- Illustrative output of notebook_export(..., format := 'sql'); do not edit.

-- selection variables start empty
SET VARIABLE picked_months = NULL::STRUCT(lo TIMESTAMP, hi TIMESTAMP);

-- cell orders
-- Effect cell: creates a table, so it only runs when asked.
CREATE OR REPLACE TABLE orders AS
SELECT
    i AS order_id,
    TIMESTAMP '2026-01-01' + INTERVAL (i * 37 % 525600) MINUTE AS ts,
    ['EU', 'US', 'APAC'][1 + i % 3] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i * 7) % 3] AS category,
    round(20 + (i * 7919 % 500) * (1 + (i % 3) * 0.25), 2) AS amount
FROM range(50000) t(i);

-- cell regions
CREATE OR REPLACE TEMP VIEW regions AS
SELECT DISTINCT region AS name FROM orders ORDER BY name;

-- input region
SET VARIABLE region = 'EU';

-- input min_amount
SET VARIABLE min_amount = 0;

-- cell filtered
CREATE OR REPLACE TEMP VIEW filtered AS
SELECT *
FROM orders
WHERE region = $region
  AND amount >= $min_amount;

-- cell monthly
CREATE OR REPLACE TEMP VIEW monthly AS
SELECT
    date_trunc('month', ts) AS month,
    category,
    sum(amount) AS revenue,
    count(*) AS orders
FROM filtered
GROUP BY ALL
ORDER BY month, category;

-- cell top_orders
CREATE OR REPLACE TEMP VIEW top_orders AS
SELECT order_id, ts, category, amount
FROM filtered
WHERE $picked_months IS NULL
   OR ts BETWEEN $picked_months.lo AND $picked_months.hi
ORDER BY amount DESC
LIMIT 20;

