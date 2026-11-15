-- Header text before the first marker is ignored.
-- %% [notebook]
/*
title: Basic test notebook
format: 1
*/

-- %% [markdown]
/*
# Basic
-- %% this line is inside a comment, so it is not a cell marker
*/

-- %% numbers run=manual
CREATE OR REPLACE TABLE nb_test_numbers AS SELECT i, i % 3 AS grp FROM range(10) t(i);

-- %% input lowest slider min=0 max=9 label="Lowest value"
SET VARIABLE lowest = 4;

-- %% kept
-- rows at or above the input
SELECT * FROM nb_test_numbers WHERE i >= $lowest;

-- %% by_group materialize=table
SELECT grp, count(*) AS n FROM kept GROUP BY grp ORDER BY grp;

-- %% chart from=by_group
/*
mark: bar
x: grp
y: n
select: {as: picked, type: point, on: x}
*/

-- %% picked_rows
SELECT * FROM kept WHERE $picked IS NULL OR list_contains($picked, grp);

-- %%
SELECT count(*) FROM picked_rows;
