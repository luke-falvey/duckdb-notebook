SELECT count(*) AS cells_run FROM notebook_run('hello.sql', cache := true);
FROM notebook_cache('hello.sql');
FROM notebook_cache_clear('hello.sql');
