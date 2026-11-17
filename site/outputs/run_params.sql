SELECT name, output FROM notebook_run('examples/revenue.sql', params := {region: 'US'})
WHERE kind = 'markdown';
