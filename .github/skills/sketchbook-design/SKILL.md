---
name: sketchbook-design
description: 'Make a duckdb-notebook sketchbook look good and read well in its hand-drawn, Excalidraw-style look: page structure and story, the 12-column span grid, KPI rows, hiding code and plumbing cells, chart titles, number formats, colours, table page sizes, highlighted {{ }} values, dark mode, and a visual review loop. Use when asked to polish, tidy, lay out, style, beautify, design or review a notebook or sketchbook, make a dashboard or report from one, or prepare it for sharing.'
argument-hint: 'The sketchbook to polish, and who will read it'
---

# Making a sketchbook look good

The look is fixed by the extension (paper, handwriting, hand-drawn boxes and charts, pastel hatching,
highlighter marks). What a notebook controls is **what** is shown, **where**, and **how it is
labelled**. Every choice below is written into the `.sql` file, so it holds in the editor's reading
view and in every export.

Related skills: **sketchbook-authoring** (format), **sketchbook-charts** (specs),
**sketchbook-preview-export** (seeing the result). The worked example is
[template.sql](../sketchbook-authoring/assets/template.sql).

## Procedure

1. **Decide the reader and the one question.** Write it as the first sentence of the opening prose.
2. **Shape the page** (top to bottom): title and lede, inputs, a row of KPIs, the main chart, supporting
   charts, a detail table, notes. Move cells to match; markdown and chart cells may sit above the cells
   they read.
3. **Hide the plumbing**: `hide=all` on data-loading and helper cells, `hide=code` on queries whose
   result is the point. Readers of the reading view and exports then see results only.
4. **Lay out with `span`** (below).
5. **Label everything** (below): chart titles that say the finding, formats on every number.
6. **Review visually** with [review-checklist.md](./references/review-checklist.md): open the reading
   view and an HTML export (sketchbook-preview-export), fix what the checklist flags, repeat.

## Structure that works

```
[markdown]  # Title, one-sentence question, the answer with {{ }} values
input span=6 | input span=6                      (or span=4 x3)
chart kpi span=4 | chart kpi span=4 | chart kpi span=4
query hide=code  + /* chart */ tabs              (the main chart, full width)
[markdown]  ## Section heading + one or two sentences
chart cell span=6 | chart cell span=6            (two views side by side)
query hide=code rows=10                          (detail table)
```

- The `# ` heading gets a hand-drawn red underline; use exactly one, then `## ` for sections.
- `{{ }}` values render as yellow highlighter marks. Put the key numbers in prose, formatted in SQL:
  `format('${:,.0f}', x)`, `format('{:,}', n)`, `format('{:.1f}%', 100 * share)`,
  `strftime(d, '%B %Y')`. Unformatted doubles (`1190000.0000001`) look broken.
- Keep paragraphs to two or three sentences. The handwriting font is large and wide.

## Layout: the span grid

- Reading view and exports use a 12-column grid. `span=N` sets a cell's width; consecutive cells whose
  spans fit in 12 share a row. Default is 12.
- Good rows: `6+6`, `4+4+4`, `3+3+3+3` (KPIs), `8+4` (chart and its note or KPI).
- A query cell's charts are **tabs**, one visible at a time. For two charts of one result side by side,
  keep one as a tab and draw the other with a `-- %% chart x from=<query> span=6` cell, or give each its
  own chart cell.
- Cells in one row stretch to the tallest; pair cells of similar height (two charts, or KPIs).
- Charts scale to their cell. Narrow cells (`span<=4`) suit KPIs, gauges, small bars and pies, not
  time series. `height=` on the cell's head (default 320) makes a chart taller or flatter.

## Charts that look right in the hand-drawn style

- **Fewer marks.** Up to 600 marks per layer are drawn by hand; beyond that they fall back to plain
  shapes. Aggregate (`timeunit: month`, `agg`) so a chart has tens of marks, not thousands.
- **Few colours.** Categories take the Excalidraw palette in order (blue, orange, green, red, violet,
  cyan, ...) with matching pastel hatching. Keep `color` to about 6 values: top-N in SQL, or `facet`.
- **Short labels.** Handwriting is wide: shorten categories in SQL (`left(name, 18)`), prefer
  horizontal bars (`y: {field: category, sort: -x}`) for long names.
- **Titles state the point.** "Software leads every month", not "Revenue by category". In tabs, the
  title is the tab's label, so keep it under about 30 characters.
- **Formats on every measure**: `format: "$,.0f"`, `",d"`, `".1%"`, `".2s"`. Axis `title`s when column
  names are code-like (`rev_usd` -> `Revenue`).
- **Honest baselines.** Bars and areas start at zero. Lines zoom to their range, which exaggerates small
  changes: add `scale: {zero: true}` when the size of a change matters.
- **Sparklines** in KPIs: aggregate them (`sparkline: {x: {field: day, timeunit: week}, y: {field:
  amount, agg: sum}}`). Partial first and last periods dip; filter to complete periods in SQL if that
  misleads.
- **Continuous colour**: `scheme: blues` (default), `greens`, `reds`, `oranges`, `purples`, `greys`,
  `viridis`, `magma`. Sequential data only; do not use them for categories.
- **Pies** only for 2 to 5 parts of one whole; otherwise ranked bars.
- **Reference lines** (`rules: [{y: avg, label: Average}]`) and target `bands` say more than another series.

## Tables

- Select, rename (`amount AS "Order value"`), order and limit columns in SQL; the header shows the
  names as written.
- `rows=10` on detail tables keeps the page short; readers can change it, and the choice is saved.
- For a presentation table use a chart with `type: table` (`columns`, `format`, `bar`, `heat`,
  `limit`) or `type: pivot` with `heat: true`.

## Inputs

- Give every input a `label="..."` that reads as a question or a noun ("Region", "Smallest order").
- Put inputs together at the top in one row; a `slider` needs `min max step`.
- `select from=<cell>.<column>` keeps choices in step with the data; hide that helper cell
  (`hide=all`).
- Static HTML exports freeze inputs (shown, disabled). Use `interactive := 'precomputed'` or
  `html-wasm` when readers must change them (sketchbook-preview-export).

## Timing

Run times show in the bottom right of each cell in the editor. For a notebook you present or record,
set `timing: false` in the `[notebook]` cell (or untick "timing" in the gear menu) to hide them.
Exports never show them.

## Dark mode

The ◐ button in the gear menu inverts the page and turns hues back, as Excalidraw does. Fixed colours in
specs (`color: {value: "#D55E00"}`, gauge `bands`) are inverted too: check them in both themes, and
prefer palette colours (`#1971c2`, `#e8590c`, `#2f9e44`, `#e03131`, `#9c36b5`) for consistency.

## Before sharing

- `hide := 'code'` on `notebook_export` for readers who want results only; the export menu's "code" and
  "output" boxes do the same. Per-cell `hide=` always applies.
- PDF prints the reading view; check page breaks around tall charts.
- Run the checklist once more on the export, not just the editor.
