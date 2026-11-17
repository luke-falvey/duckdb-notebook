-- %% [markdown]
/*
# Query charts

Charts written after a query, in the same cell.
*/

-- %% sales
SELECT range AS day, range % 3 AS grp, range * 10 AS amount FROM range(9)
/* chart
mark: bar
x: grp
y: {field: amount, agg: sum}
title: Amount by group
select: {as: picked_groups, type: point, on: x}
*/
/* chart
type: kpi
value: {field: amount, agg: sum}
*/

-- %%
SELECT grp, count(*) AS n FROM sales GROUP BY grp ORDER BY grp
/* chart
mark: bar
x: grp
y: n
filter: picked_groups
*/

-- %% broken
SELECT 1 AS a
/* chart
mark: line
x: {field: no_such_column}
y: a
*/

-- %% picked
SELECT * FROM sales WHERE $picked_groups IS NULL OR list_contains($picked_groups, grp)
