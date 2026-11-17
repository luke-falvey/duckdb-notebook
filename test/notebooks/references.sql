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
  - {y: [2000, 2700], label: Target}
notes:
  - {x: b, text: the bigger group}
*/
/* chart
title: Notes
mark: line
x: day
y: n
notes:
  - {x: 2026-03-01, y: 59, text: "A note that is long enough to wrap", circle: true}
  - {x: 2026-01-20, text: start, side: below}
  - {x: [2026-03-15, 2026-04-05], text: spring}
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
