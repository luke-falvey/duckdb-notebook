<!-- Exported from examples/revenue.sql by notebook_export. -->

# Revenue review

Synthetic orders for 2026, generated in the `orders` cell so the notebook has
no external dependencies. Total revenue in **EU** so far:
**4490867.00**.

```sql
-- Effect cell: creates a table, so it only runs when asked.
CREATE OR REPLACE TABLE orders AS
SELECT
    i AS order_id,
    TIMESTAMP '2026-01-01' + INTERVAL (i * 37 % 525600) MINUTE AS ts,
    ['EU', 'US', 'APAC'][1 + i % 3] AS region,
    ['Hardware', 'Software', 'Services'][1 + (i // 3) % 3] AS category,
    round(20 + (i * 7919 % 500) * (1 + (i % 3) * 0.25), 2) AS amount
FROM range(50000) t(i);
```

```sql
SELECT DISTINCT region AS name FROM orders ORDER BY name;
```

| name |
| --- |
| APAC |
| EU |
| US |

**Region**: EU

**Minimum order value**: 0

```sql
SELECT *
FROM orders
WHERE region = $region
  AND amount >= $min_amount;
```

| order_id | ts | region | category | amount |
| --- | --- | --- | --- | --- |
| 0 | 2026-01-01 00:00:00 | EU | Hardware | 20 |
| 3 | 2026-01-01 01:51:00 | EU | Software | 277 |
| 6 | 2026-01-01 03:42:00 | EU | Services | 34 |
| 9 | 2026-01-01 05:33:00 | EU | Hardware | 291 |
| 12 | 2026-01-01 07:24:00 | EU | Software | 48 |
| 15 | 2026-01-01 09:15:00 | EU | Services | 305 |
| 18 | 2026-01-01 11:06:00 | EU | Hardware | 62 |
| 21 | 2026-01-01 12:57:00 | EU | Software | 319 |
| 24 | 2026-01-01 14:48:00 | EU | Services | 76 |
| 27 | 2026-01-01 16:39:00 | EU | Hardware | 333 |
| 30 | 2026-01-01 18:30:00 | EU | Software | 90 |
| 33 | 2026-01-01 20:21:00 | EU | Services | 347 |
| 36 | 2026-01-01 22:12:00 | EU | Hardware | 104 |
| 39 | 2026-01-02 00:03:00 | EU | Software | 361 |
| 42 | 2026-01-02 01:54:00 | EU | Services | 118 |
| 45 | 2026-01-02 03:45:00 | EU | Hardware | 375 |
| 48 | 2026-01-02 05:36:00 | EU | Software | 132 |
| 51 | 2026-01-02 07:27:00 | EU | Services | 389 |
| 54 | 2026-01-02 09:18:00 | EU | Hardware | 146 |
| 57 | 2026-01-02 11:09:00 | EU | Software | 403 |

16667 rows in total.

```sql
SELECT
    date_trunc('month', ts) AS month,
    category,
    sum(amount) AS revenue,
    count(*) AS orders
FROM filtered
GROUP BY ALL
ORDER BY month, category;
```

*Monthly revenue by category* (chart data; charts are drawn in the HTML export)

| x | y | color |
| --- | --- | --- |
| 2026-01-01 00:00:00 | 144786 | Hardware |
| 2026-01-01 00:00:00 | 143828 | Services |
| 2026-01-01 00:00:00 | 144890 | Software |
| 2026-02-01 00:00:00 | 131139 | Hardware |
| 2026-02-01 00:00:00 | 130542 | Services |
| 2026-02-01 00:00:00 | 129445 | Software |
| 2026-03-01 00:00:00 | 144046 | Hardware |
| 2026-03-01 00:00:00 | 144608 | Services |
| 2026-03-01 00:00:00 | 144170 | Software |
| 2026-04-01 00:00:00 | 139741 | Hardware |
| 2026-04-01 00:00:00 | 140000 | Services |
| 2026-04-01 00:00:00 | 140026 | Software |
| 2026-05-01 00:00:00 | 144316 | Hardware |
| 2026-05-01 00:00:00 | 144692 | Services |
| 2026-05-01 00:00:00 | 145318 | Software |
| 2026-06-01 00:00:00 | 140162 | Hardware |
| 2026-06-01 00:00:00 | 139377 | Services |
| 2026-06-01 00:00:00 | 139362 | Software |
| 2026-07-01 00:00:00 | 118510 | Hardware |
| 2026-07-01 00:00:00 | 119103 | Services |

36 rows in total.

*Category mix* (chart data; charts are drawn in the HTML export)

| x | y |
| --- | --- |
| 1497102 | Software |
| 1497055 | Services |
| 1496710 | Hardware |

*Chart* (chart data; charts are drawn in the HTML export)

| value |
| --- |
| 4490867 |

*Order size by weekday and hour* (chart data; charts are drawn in the HTML export)

| x | y | color |
| --- | --- | --- |
| 0 | 0 | 267.50495049504951 |
| 0 | 1 | 249.88888888888889 |
| 0 | 2 | 269.78431372549022 |
| 0 | 3 | 279.59183673469386 |
| 0 | 4 | 280.1782178217822 |
| 0 | 5 | 276.35353535353534 |
| 0 | 6 | 268.18000000000001 |
| 1 | 0 | 286.39175257731961 |
| 1 | 1 | 285.22222222222223 |
| 1 | 2 | 264.75 |
| 1 | 3 | 266.24489795918367 |
| 1 | 4 | 253.60784313725489 |
| 1 | 5 | 261.24489795918367 |
| 1 | 6 | 268.39999999999998 |
| 2 | 0 | 253.24752475247524 |
| 2 | 1 | 258.37373737373736 |
| 2 | 2 | 273.18000000000001 |
| 2 | 3 | 275.22772277227722 |
| 2 | 4 | 276.77999999999997 |
| 2 | 5 | 275.83168316831683 |

168 rows in total.

```sql
-- `picked_months` is set by brushing the trend chart; NULL means no brush.
SELECT order_id, ts, category, amount
FROM filtered
WHERE $picked_months IS NULL
   OR ts BETWEEN $picked_months.lo AND $picked_months.hi
ORDER BY amount DESC
LIMIT 20;
```

| order_id | ts | category | amount |
| --- | --- | --- | --- |
| 321 | 2026-01-09 05:57:00 | Services | 519 |
| 1821 | 2026-02-16 18:57:00 | Software | 519 |
| 3321 | 2026-03-27 07:57:00 | Hardware | 519 |
| 4821 | 2026-05-04 20:57:00 | Services | 519 |
| 6321 | 2026-06-12 09:57:00 | Software | 519 |
| 7821 | 2026-07-20 22:57:00 | Hardware | 519 |
| 9321 | 2026-08-28 11:57:00 | Services | 519 |
| 10821 | 2026-10-06 00:57:00 | Software | 519 |
| 12321 | 2026-11-13 13:57:00 | Hardware | 519 |
| 13821 | 2026-12-22 02:57:00 | Services | 519 |
| 15321 | 2026-01-29 15:57:00 | Software | 519 |
| 16821 | 2026-03-09 04:57:00 | Hardware | 519 |
| 18321 | 2026-04-16 17:57:00 | Services | 519 |
| 19821 | 2026-05-25 06:57:00 | Software | 519 |
| 21321 | 2026-07-02 19:57:00 | Hardware | 519 |
| 22821 | 2026-08-10 08:57:00 | Services | 519 |
| 24321 | 2026-09-17 21:57:00 | Software | 519 |
| 25821 | 2026-10-26 10:57:00 | Hardware | 519 |
| 27321 | 2026-12-03 23:57:00 | Services | 519 |
| 28821 | 2026-01-11 12:57:00 | Software | 519 |

