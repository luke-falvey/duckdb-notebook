-- %% orders run=manual hide=all
CREATE OR REPLACE TABLE orders AS
SELECT
    i AS order_id,
    DATE '2026-01-01' + (i % 365)::INTEGER AS day,
    ['North', 'South', 'East', 'West'][1 + i % 4] AS region,
    round(20 + (i * 7919 % 480), 2) AS amount
FROM range(20000) t(i);

-- %% busy_days
WITH daily AS (
    SELECT day, region, sum(amount) AS revenue
    FROM orders
    GROUP BY ALL
),
average AS (
    SELECT region, avg(revenue) AS typical
    FROM daily
    GROUP BY region
)
SELECT daily.*, round(revenue / typical, 2) AS times_typical
FROM daily JOIN average USING (region)
WHERE revenue > 1.2 * typical
ORDER BY times_typical DESC;
