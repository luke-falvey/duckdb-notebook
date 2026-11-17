SELECT DISTINCT function_name FROM duckdb_functions()
WHERE function_name LIKE 'notebook%' ORDER BY ALL;
