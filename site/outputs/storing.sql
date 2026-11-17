FROM notebook_save('examples/revenue.sql', message := 'first draft');
FROM notebook_save('examples/revenue.sql');
SELECT name, version, message FROM _notebook.versions;
