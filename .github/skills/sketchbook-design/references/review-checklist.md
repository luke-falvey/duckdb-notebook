# Sketchbook review checklist

Go through this with the reading view open (gear menu → Reading) and again on the HTML export. Fix in
the `.sql` file, re-run, and look again. Each item names the setting that fixes it.

## Story

- [ ] The first prose cell states the question and the answer, with the key numbers as `{{ }}` values.
- [ ] One `# ` title; sections use `## `.
- [ ] Each chart title says what to notice, in under about 30 characters when it is a tab.
- [ ] Nothing a reader does not need is visible: data loading, helper queries and choice lists have
      `hide=all`; queries shown for their result have `hide=code`.
- [ ] The order reads top to bottom: inputs, headline numbers, main chart, detail, notes.

## Layout

- [ ] Rows add up to 12 with `span` (`6+6`, `4+4+4`, `3+3+3+3`); no lone half-width cell unless intended.
- [ ] Cells sharing a row are of similar height.
- [ ] Narrow cells hold KPIs or small charts, not time series or wide tables.
- [ ] No horizontal scrollbars inside cells (shorten labels, fewer columns, wider span).

## Numbers

- [ ] Every money, percentage and large number has a format: `format:` in specs, `format(...)` in
      `{{ }}`.
- [ ] No long decimals or raw timestamps (`2026-01-01 00:00:00`) where a date or month is meant: cast
      or `strftime` in SQL.
- [ ] `{{ }}` never shows `NULL` (wrap in `coalesce`).

## Charts

- [ ] Tens of marks, not thousands (aggregate with `timeunit`/`agg`); hand-drawn style holds.
- [ ] At most about 6 colours; categories sorted (`sort: -x`) when ranked.
- [ ] Axis titles readable; long category labels not clipped.
- [ ] Lines whose change size matters use `scale: {zero: true}`.
- [ ] No ⚠ on any tab, no red error text.
- [ ] Selections are explained in prose ("drag across the chart to ..."), and the cells they filter
      handle NULL (no selection).

## Tables

- [ ] Columns selected, renamed and ordered for the reader; `rows=10` (or similar) on detail tables.
- [ ] Numbers rounded in SQL where full precision adds nothing.

## Themes and exports

- [ ] Checked in dark mode (◐): fixed colours still distinguishable.
- [ ] HTML export opens offline with the same look; `hide := 'code'` export reads as a report.
- [ ] Inputs in a static export are frozen; use `interactive := 'precomputed'` or `html-wasm` if
      readers must change them.
- [ ] If it will be presented: step through it (gear menu → Present, or the export with `#present`) at
      1366 x 768. Each screen holds one idea and fits without scrolling; inputs sit with what they
      change; working tables are `screen=skip`.
- [ ] `.github/skills/sketchbook-authoring/scripts/check.sh <file>` reports no problems.
