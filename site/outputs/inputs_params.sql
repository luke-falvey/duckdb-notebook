SELECT name, status, row_count
FROM notebook_run('inputs.sql', params := {region: 'South', include_refunds: false})
WHERE kind = 'sql';
