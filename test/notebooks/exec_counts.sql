-- %% [markdown]
/*
# Execution counts

Each query below adds one to a sequence of its own every time DuckDB executes it (the scalar
subquery runs once per execution), so test/sql/notebook_exec_counts.test can say how many times a
run executes each cell's query. The sequences are created by the test.
*/

-- %% plain_view
SELECT x FROM range(3) t(x) WHERE x < (SELECT nextval('nb_exec_plain') + 100);

-- %% charted_view
SELECT x, x % 2 AS g FROM range(3) t(x) WHERE x < (SELECT nextval('nb_exec_charted') + 100)
/* chart
mark: bar
x: g
y: {field: x, agg: sum}
*/
/* chart
mark: bar
x: g
y: {field: x, agg: max}
*/

-- %% charted_table materialize=table
SELECT x, x % 2 AS g FROM range(3) t(x) WHERE x < (SELECT nextval('nb_exec_table') + 100)
/* chart
mark: bar
x: g
y: {field: x, agg: sum}
*/
/* chart
mark: bar
x: g
y: {field: x, agg: max}
*/

-- %%
SELECT x, x % 2 AS g FROM range(3) t(x) WHERE x < (SELECT nextval('nb_exec_unnamed') + 100)
/* chart
mark: bar
x: g
y: {field: x, agg: sum}
*/

-- %% from_table
SELECT count(*) AS n FROM charted_table;
