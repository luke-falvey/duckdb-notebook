-- %% good
SELECT 42 AS answer;

-- %% broken
SELECT * FROM table_that_does_not_exist;

-- %% after
SELECT answer + 1 AS next FROM good;
