-- %% [notebook]
/*
title: Cache test
*/

-- %% nb_cache_numbers run=manual
CREATE OR REPLACE TABLE nb_cache_numbers AS SELECT i, i % 3 AS grp FROM range(10) t(i);

-- %% input lowest slider min=0 max=9
SET VARIABLE lowest = 4;

-- %% kept
SELECT * FROM nb_cache_numbers WHERE i >= $lowest;

/* chart
mark: bar
x: grp
y: {agg: count}
*/

-- %% by_group
SELECT grp, count(*) AS n FROM kept GROUP BY grp ORDER BY grp;

-- %% [markdown]
/*
There are {{ (SELECT count(*) FROM kept) }} rows.
*/

-- %% [markdown]
/*
Plain prose.
*/

-- %%
SELECT 42 AS answer;
