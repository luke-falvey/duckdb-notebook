-- %% [markdown]
/*
# Numbers

How values travel to a reader: test/sql/notebook_run.test exports this as Markdown.
*/

-- %% numbers
SELECT 268.18::DECIMAL(10, 2) AS d, 268.18::DOUBLE AS f, 12345678901234567.89::DECIMAL(38, 2) AS big,
       0.1::FLOAT AS r, 0.1::DOUBLE + 0.2::DOUBLE AS s, 'inf'::DOUBLE AS pos, '-inf'::DOUBLE AS neg,
       'nan'::DOUBLE AS nan
