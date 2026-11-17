-- %% early
-- Reads a cell further down: an error.
SELECT * FROM later;

-- %% later
SELECT 1 AS n;

-- %%
CREATE OR REPLACE TABLE nb_test_twice AS SELECT 1 AS n;

-- %%
CREATE OR REPLACE TABLE nb_test_twice AS SELECT 2 AS n;

-- %% chart trend from=detail
/*
mark: line
select: {as: brush, type: interval, on: x}
*/

-- %% detail
-- Reads the selection of a chart that draws this cell: a cycle.
SELECT * FROM later WHERE $brush IS NULL;

-- %% loose
-- Warnings do not stop a notebook from running.
SELECT $nobody_sets_this AS v;
