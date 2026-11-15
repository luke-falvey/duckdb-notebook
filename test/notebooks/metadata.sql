-- %% [notebook]
/*
title: Metadata
attach:
  - {path: ":memory:", as: nb_meta_db}
*/

-- %%
CREATE OR REPLACE TABLE nb_meta_db.t AS SELECT 7 AS v;

-- %% from_attached
SELECT v FROM nb_meta_db.t;
