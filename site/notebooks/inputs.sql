-- %% [notebook]
/*
title: Every input widget
format: 1
*/

-- %% sales run=manual hide=all
CREATE OR REPLACE TABLE sales AS
SELECT
    i AS id,
    DATE '2026-01-01' + (i % 365)::INTEGER AS day,
    ['North', 'South', 'East', 'West'][1 + i % 4] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i // 4) % 3] AS category,
    round(20 + (i * 7919 % 480), 2) AS amount,
    i % 7 = 0 AS refunded
FROM range(5000) t(i);

-- %% regions hide=all
SELECT DISTINCT region FROM sales ORDER BY region;

-- %% input search text placeholder="part of a region name" label="Search" span=4
SET VARIABLE search = '';

-- %% input min_amount number min=0 max=500 step=10 label="Smallest amount" span=4
SET VARIABLE min_amount = 0;

-- %% input max_amount slider min=0 max=500 step=10 label="Largest amount" span=4
SET VARIABLE max_amount = 500;

-- %% input include_refunds checkbox label="Include refunds" span=4
SET VARIABLE include_refunds = true;

-- %% input since date min=2026-01-01 max=2026-12-31 label="From" span=4
SET VARIABLE since = DATE '2026-01-01';

-- %% input period daterange min=2026-01-01 max=2026-12-31 label="Period" span=4
SET VARIABLE period = {lo: DATE '2026-01-01', hi: DATE '2026-12-31'};

-- %% input region select from=regions.region label="Region" span=6
SET VARIABLE region = 'North';

-- %% input categories multiselect options=Hardware,Software,Services label="Categories" span=6
SET VARIABLE categories = ['Hardware', 'Software', 'Services'];

-- %% picked
SELECT category, count(*) AS orders, sum(amount) AS revenue
FROM sales
WHERE region ILIKE '%' || $search || '%'
  AND region = $region
  AND amount BETWEEN $min_amount AND $max_amount
  AND ($include_refunds OR NOT refunded)
  AND day >= $since
  AND day BETWEEN $period.lo AND $period.hi
  AND list_contains($categories, category)
GROUP BY ALL
ORDER BY category;
