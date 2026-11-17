-- %% sql
CREATE OR REPLACE TABLE w_days AS SELECT DATE '2026-01-01' + i::INTEGER AS d FROM range(3) t(i);

-- %% input n number
SET VARIABLE n = 5;

-- %% input day select from=w_days.d
SET VARIABLE day = DATE '2026-01-01';

-- %% [markdown]
/*
values {{ typeof($n) }} {{ $n }} {{ typeof($day) }} {{ [1, 2] }} {{ 1.50::DECIMAL(4,2) }}
*/
