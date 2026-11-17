-- %% [markdown]
/*
# Peek under the pen

A notebook with CTEs and sub-queries, for the editor's peek: with the caret inside one, its first
rows show in a note beside the code.
*/

-- %% nb_peek_sales run=manual
CREATE OR REPLACE TABLE nb_peek_sales AS
SELECT
    i AS id,
    ['north', 'south', 'east', 'west'][1 + i % 4] AS region,
    DATE '2026-01-01' + CAST(i % 90 AS INTEGER) AS day,
    10 + (i * 37) % 200 AS amount
FROM range(400) t(i);

-- %% input floor number label="Smallest sale"
SET VARIABLE floor = 50;

-- %% by_region
WITH big AS (
    SELECT * FROM nb_peek_sales WHERE amount >= $floor
),
totals AS (
    SELECT region, count(*) AS sales, sum(amount) AS total
    FROM big
    GROUP BY region
)
SELECT t.region, t.sales, t.total, t.total / s.everything AS share
FROM totals t,
     (SELECT sum(total) AS everything FROM totals) s
ORDER BY t.region;

-- %% best_days
SELECT day, total
FROM (
    SELECT day, sum(amount) AS total
    FROM nb_peek_sales
    WHERE region IN (SELECT region FROM by_region WHERE share > 0.2)
    GROUP BY day
) d
ORDER BY total DESC
LIMIT 5;
