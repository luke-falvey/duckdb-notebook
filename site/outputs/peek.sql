SET VARIABLE q = 'WITH daily AS (SELECT day, sum(amount) AS revenue FROM orders GROUP BY day)
SELECT * FROM daily WHERE revenue > (SELECT avg(revenue) FROM daily)';

-- The cursor (a 0-based byte offset) on "sum": inside the CTE daily.
SELECT kind, name, query FROM notebook_peek(getvariable('q'), strpos(getvariable('q'), 'sum') - 1);
-- The cursor on "avg": inside the sub-query.
SELECT kind, name, query FROM notebook_peek(getvariable('q'), strpos(getvariable('q'), 'avg') - 1);
-- The cursor in the main query: nothing to peek at.
SELECT kind, reason FROM notebook_peek(getvariable('q'), 0);
