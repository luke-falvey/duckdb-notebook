-- %% [markdown]
/*
Total {{ (SELECT sum(n) FROM nb_values) }} at level {{ $level }}; braces in {{ '}}' }} are fine.
*/

-- %% input level slider min=0 max=10
SET VARIABLE level = 3;

-- %% nb_values
SELECT range AS n FROM range($level);

-- %% [markdown]
/*
No expressions here.
*/

-- %% [markdown]
/*
Broken: {{ (SELECT * FROM no_such_table_here) }}
*/
