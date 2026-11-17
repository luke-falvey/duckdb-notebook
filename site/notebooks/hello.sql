-- %% [markdown]
/*
# Hello, notebook

The busiest hour in **{{ $city }}**: {{ (SELECT hour FROM by_hour ORDER BY trips DESC LIMIT 1) }}:00.
*/

-- %% input city select options=Lisbon,Oslo label="City"
SET VARIABLE city = 'Lisbon';

-- %% by_hour height=240
SELECT hour, count(*) AS trips
FROM (
    SELECT ['Lisbon', 'Oslo'][1 + i % 2] AS city,
           CASE i % 3 WHEN 0 THEN 7 + i % 4 WHEN 1 THEN 16 + i % 5 ELSE i % 24 END AS hour
    FROM range(2000) t(i)
)
WHERE city = $city
GROUP BY hour
ORDER BY hour;
/* chart
title: Trips by hour
mark: bar
x: hour
y: trips
*/
