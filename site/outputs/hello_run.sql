SELECT idx, name, status, row_count FROM notebook_run('hello.sql', params := {city: 'Oslo'});
