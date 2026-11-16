---
name: sketchbook-look
description: 'Change the sketchbook look itself in duckdb-notebook: the Excalidraw-style hand-drawn renderer, paper and handwriting styles, colours, palette, chart drawing, cell outlines, tabs, tables, dark mode and the export viewer, in frontend/ (style.css, render-sketch.js, render-core.js, render-chart.js, render-typed.js, app.js, viewer.js). Use when asked to restyle, theme, tweak fonts or colours, fix a rendering or layout bug, add a chart renderer, or make charts or cells look better for every notebook rather than one.'
argument-hint: 'What should look different, and where (editor, exports, charts)'
---

# Changing the sketchbook look

The look is a renderer concern: nothing in the notebook format describes it. Plain browser JavaScript
and CSS in `frontend/`, no build step and no modules, compiled into the extension by CMake and inlined
into exports. Background: [docs/DESIGN.md §13](../../../docs/DESIGN.md) ("What the sketchbook look taught
us") and §14 (charts as tabs). For a single notebook's appearance use **sketchbook-design** instead.

## Where things are

| File | Holds |
|---|---|
| `frontend/style.css` | All styling: CSS variables (`--nb-bg`, `--nb-ink`, `--nb-accent`, `--nb-mark`, `--nb-hand`, `--nb-mono`, `--nb-wobbly`), paper grain, dark mode, gear menu, cells, tabs, tables, charts, print rules |
| `frontend/render-sketch.js` | `Sketch`: seeded hand-drawn SVG primitives: `reseed`, `el`, `line`, `fill` (hachure, cross-hatch, solid), `rect`, `roundRect`, `ellipse`, `polyline`, `smooth`, `shape`, `arrow`, `outline(el, {kind, width, fill})` |
| `frontend/render-core.js` | `NB`: palette and pastels, colour `schemes`/`ramp`, number and date formats, markdown (with `{{ }}` highlight markers), `NB.table` (paging, page sizes), `NB.tabs`, `NB.chartLabel`, `suggestChart` |
| `frontend/render-sql.js` | SQL highlighting: `NB.tokenizeSql`, `NB.highlightSql`, `NB.setSqlWords` (DuckDB's keywords and types from the server). The editor lays the highlighted `<pre>` behind a transparent, self-sizing textarea (`.nb-code-wrap` in `app.js`); both must share every text metric. Colours are the `.tk-*` classes in `style.css` |
| `frontend/render-chart.js` | Grammar charts: scales, axes (wobbly arrows), marks, legends, brushing; `SKETCH_LIMIT` (600) above which marks are plain SVG |
| `frontend/render-typed.js` | Typed charts: KPI, gauge, sankey, chord, treemap, sunburst, icicle, network, funnel, candlestick, gantt, calendar, map, table, pivot, raw specs |
| `frontend/app.js`, `app-extra.js`, `index.html` | The editor: toolbar and gear menu, cells, query tabs and chart spec editor, stubs for hidden parts, catalog, graph, chart picker |
| `frontend/viewer.js` | The export viewer (html, precomputed, html-wasm, PDF) |
| `frontend/*.woff2`, `third_party/fonts/LICENSE.md` | Excalifont and Cascadia Code (SIL OFL), served at `/assets/` and inlined into exports by `src/functions/export.cpp` |

Both the editor and the viewer use the shared `render-*.js`, so a change there shows in both. Check
both anyway.

## Procedure

1. Make the change in `frontend/`. Reuse `Sketch` primitives and the palette rather than new drawing
   code or colours.
2. `make frontend_check` (needs Node) to catch syntax errors.
3. `GEN=ninja make release`: the frontend is compiled in, so nothing changes without a rebuild. New
   files in `frontend/` are picked up by CMake's glob automatically.
4. Restart any running server (sketchbook-preview-export), reload, and look at: the editor, the reading
   view, dark mode (◐), an HTML export, and `examples/charts.sql` exported (every chart type).
5. `make browser_test` (Playwright, Chromium) and `make test`. Add a browser check for visible
   behaviour you introduce; browser tests locate elements by the classes and IDs in `index.html` and
   the renderers, so keep those stable or update the tests.
6. Record a lasting principle in DESIGN.md §13 if you add one.

## Rules that keep the look consistent

- **Seed every drawing.** Call `Sketch.reseed(key)` with a stable key (chart title and data shape, cell
  position) before drawing, so a chart wobbles the same in the editor, an export and a PDF. Unseeded
  randomness flickers on every re-render.
- **Ink is `currentColor`.** SVG presentation attributes do not resolve `var(--...)`; set colour in CSS
  and draw with `currentColor`, so themes and dark mode reach it.
- **Dark mode is an inversion** (`filter: invert(93%) hue-rotate(180deg)` on `body`), as in Excalidraw.
  Do not add a second colour set; pick colours that survive inversion.
- **Pastel fills go with palette strokes**: `NB.pastel(stroke)`; categorical colours come from
  `NB.palette` in order.
- **Performance**: hand-drawn paths are heavier than plain ones. Keep the plain fallback above
  `SKETCH_LIMIT` (and the network limits, 300 nodes and 400 edges) for anything that can be large.
- **Outlines** (`Sketch.outline`) draw an SVG behind an element at its measured size and redraw on
  resize; the element needs `isolation: isolate` (class `sk-box`) and the SVG sits at `z-index: -1`.
- **Hover needs area, not ink.** Hatching leaves gaps; filled `Sketch` shapes add an invisible
  `.sk-hit` path underneath so the whole shape answers. Measure shapes by `.sk-hit`, not the group:
  a group's bounding box includes hatch lines the clip path hides.
- **`getBBox` fails on detached SVG**: compute geometry from data, or hatch across the whole viewBox as
  `render-typed.js` does.
- **`NB.table` and `NB.chart` clear their container.** Give each its own holder element when other
  content (tabs, pickers) shares the parent.
- **Narrow containers**: SVGs with a `viewBox` should have `max-width: 100%; height: auto` so they shrink
  in `span=3/4` cells instead of scrolling. `.nb-output` is a size container: use `cqw` units for type
  that must fit (the KPI value does).
- **Handwriting is wide**: leave margin for labels (the y axis grows its left margin from the longest
  tick label) and truncate with an ellipsis rather than overlap.
- **No libraries, no network.** Exports must work offline; do not add CDN fonts, scripts or images. A
  new font needs an OFL-compatible licence recorded in `third_party/fonts/LICENSE.md`, and the inlining
  in `export.cpp` (`url('/assets/<file>.woff2')` is replaced by a data URI).
- **Print**: `@media print` rules hide chrome; PDF export prints the reading view. Check a PDF after
  layout changes.

## Things readers notice first

The page title and `# ` heading underline, cell outlines, the highlighter on `{{ }}` values and active
tabs (`--nb-mark`), KPI numbers, axis arrows and legends. Small changes there have the largest effect;
compare before and after screenshots of the same notebook.
