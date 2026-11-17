CREATE TABLE orders AS
SELECT ['EU', 'US', 'APAC'][1 + i % 3] AS region, i % 100 AS amount FROM range(1000) t(i);

FROM notebook_chart_query('mark: bar
x: region
y: {field: amount, agg: sum}', 'orders');
