-- %% [notebook]
/*
title: Rules, bands and notes on charts
format: 1
*/

-- %% days
SELECT DATE '2026-01-01' + i::INTEGER AS day, ['a', 'b'][1 + i % 2] AS grp, i AS n, 100 - i AS budget
FROM range(100) t(i);
/* chart
title: Lines at aggregates and values
mark: line
x: day
y: n
rules:
  - {y: avg, label: "average {value}"}
  - {y: p90, label: p90}
  - {y: 120, label: Target}
  - {x: 2026-02-01, label: Launch}
  - {x: median}
  - {y: {field: budget, agg: max}, label: "budget {value}", color: "#e03131"}
bands:
  - {y: [p25, p75], label: "{from} to {to}"}
  - {x: [min, 2026-01-15], label: Warm-up}
*/
/* chart
title: Bars by group
mark: bar
x: grp
y: {field: n, agg: sum, format: ",d"}
rules:
  - {y: avg, label: "average {value}"}
  - {x: b}
bands:
  - {y: [2000, 2400], label: Target}
*/

-- %% bad
SELECT * FROM days;
/* chart
title: A bad rule
mark: bar
x: grp
y: {field: n, agg: sum}
rules:
  - {x: avg}
*/
