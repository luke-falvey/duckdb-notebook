# Handwriting: every part of a notebook is written onto the page

Status: built. The plan below is kept as written; [What was built](#what-was-built) at the end records
where the implementation departs from it and why, with the measurements. The stroke-recovery
experiment it rests on is in [`spikes/handwriting-strokes`](../spikes/handwriting-strokes/README.md).

## Goal

When a notebook appears, nothing pops into place. A pen writes it: headings and prose letter by
letter, tables row by row, SQL as it would be typed by hand, chart axes then marks then labels,
the brackets beside each cell, the underline under the title. It should be smooth enough to enjoy
watching and quick enough never to be in the way.

The finished page must look exactly as it does today. The animation is a way of arriving, not a new
look.

## Summary

| Decision | Choice | Why |
|---|---|---|
| Finished state | Unchanged DOM and pixels; the animation leaves no trace | Exports, PDF, tests, selection and hover keep working with no special cases |
| Shapes (charts, outlines, rules) | Draw each stroke along its length with `stroke-dashoffset` | Every shape already comes out of `Sketch` as SVG paths |
| Text | Real glyphs revealed along recovered pen strokes, on a canvas over the hidden text | True handwriting; exact final glyphs; cheap; proven in the spike |
| Pen strokes for the fonts | Generated offline from the fonts by a tool, checked in as data | No runtime dependency; 212 Excalifont glyphs is a small set |
| Timing | One scheduler, constant pen speed, a hard time budget per screen | Smooth and never slow |
| Re-renders | Animate only what is new or changed, keyed by content | `render()` rebuilds everything on every state change |
| Off switch | Reduced motion, print, PDF export, a gear setting, a URL flag | Accessibility, correctness, tests |

## What exists today

Read before starting: `CLAUDE.md`, `.github/skills/sketchbook-look/SKILL.md`, `docs/DESIGN.md` §13.

- **Shapes** are SVG paths made by `Sketch` in `frontend/render-sketch.js` (`line`, `rect`,
  `roundRect`, `ellipse`, `polyline`, `shape`, `arrow`, `fill`, `outline`). Lines are drawn twice, so
  a single `<path>` usually holds several subpaths. Hachure fills are one path of many lines inside a
  clip path. Above `SKETCH_LIMIT` (600 marks) charts fall back to plain SVG elements.
- **Text** is ordinary HTML in Excalifont (prose, tables, labels) and Cascadia Code (SQL), plus SVG
  `<text>` inside charts. Markdown comes from `NB.markdown`, grids from `NB.table`.
- **Rendering is wholesale.** The editor's `A.render()` and the viewer's `render()` rebuild every
  cell's DOM whenever any state changes: a run finishing, a tab click, a slider moving.
- **Exports** inline `style.css`, the `render-*.js` files and `viewer.js` into one HTML file
  (`src/functions/export.cpp`). PDF export opens that file in headless Chrome with
  `--virtual-time-budget=10000 --print-to-pdf`.
- **Size of a page**, measured on the HTML exports:

  | Notebook | SVG paths | With several subpaths | Characters | Table cells | SVG `<text>` |
  |---|---|---|---|---|---|
  | `examples/revenue.sql` | 561 | 197 | 3,900 | 256 | 45 |
  | `examples/charts.sql` | 1,277 | 508 | 7,200 | 412 | 244 |

## Principles

1. **The finished state is the base state.** Build the DOM exactly as today, then lay the animation
   over it. If the script fails, is switched off, or is interrupted, the page is simply complete.
2. **No trace.** When an element has been written, the DOM is identical to what it would have been
   with the animation off: no extra elements, classes, attributes or inline styles. This is a test.
3. **Text is always in the DOM.** Hidden only by paint, never by `display` or by inserting characters
   over time, so selection, find-in-page, screen readers and layout are unaffected from the first
   frame.
4. **Time, not state.** An element's progress is a function of the clock and its job's start time, so
   a rebuilt element picks up exactly where the old one was.
5. **One pen.** All timing goes through one scheduler with one budget. No component animates itself.

## Architecture

Three new files in `frontend/`, loaded after `render-sketch.js` and before `render-core.js` in
`index.html`, and added to the inlined scripts in `export.cpp`:

| File | Holds |
|---|---|
| `render-ink.js` | `Ink`: the scheduler, the shape animator, the text writer, the switches |
| `ink-strokes.js` | Generated: `Ink.strokes = {Excalifont: {...}, Cascadia: {...}}` |
| (none at run time) `tools/font_strokes.py` | The generator, grown from the spike |

Keep the house rules: plain browser JavaScript, no modules, no libraries, no network, and CMake picks
up new files in `frontend/` by itself.

### The entry point

```js
Ink.present(element, key, signature)
```

Called by the editor and the viewer after they build a piece of the page.

- `key` names the piece stably across re-renders: `cell3/markdown`, `cell5/table`, `cell5/chart/0`,
  `cell5/outline`, `toolbar/rule`.
- `signature` is a cheap hash of what it shows: the text, the data, the size.
- First time a key is seen, or when its signature changes: schedule a job and animate.
- Same key and signature, job finished: do nothing. The element stays as built.
- Same key and signature, job still running: attach this new element to the running job at its
  current progress.

`Ink` keeps the registry (`key → {signature, start, duration}`). Nothing is stored on DOM nodes,
because they are thrown away on every render.

### Shapes

For an SVG subtree, collect its stroked paths in document order and draw each along its length.

- **Split subpaths while animating.** `stroke-dasharray` restarts on every subpath, so a path with
  eight subpaths would draw all eight at once. While a job runs, replace such a path with a group of
  single-subpath copies and animate those in sequence; put the original back when the job ends.
  The two passes of a doubled line should overlap in time, the second starting slightly after the
  first; the four sides of a box follow one another.
- **Lengths.** Read `getTotalLength()` for all paths of a job in one pass before writing any styles.
  Do not use the `pathLength` attribute: WebKit has a rendering bug with it.
- **Hide before start.** A path with round caps shows a dot at zero length. Keep paths that have not
  started `visibility: hidden`.
- **Fills.** Hachure lines are strokes: draw them after the outline, quickly, a few at a time. Solid
  fills and heat-map cells fade in with `opacity`, which the compositor can do without repainting.
- **Dashed strokes** cannot use the dash trick. Fade them.
- **Plain marks** (above `SKETCH_LIMIT`, and network charts past their limits) are not paths. Sweep
  them in as one group with a clip rectangle moving left to right, or fade the group.
- **Order inside a chart:** title, axes and their arrows, grid, marks in data order, fills, legend,
  labels. `render-chart.js` and `render-typed.js` already append in roughly this order; where they do
  not, tag groups with a `data-ink-order` number and remove it at the end.
- **Mechanism.** One `requestAnimationFrame` loop owned by the scheduler sets `stroke-dashoffset` on
  the paths currently being drawn. Only a handful are in motion at once, so per-frame work is small.
  `stroke-dashoffset` is never composited; the cost is repaint, which is why the number of
  simultaneously moving paths is capped (see Budget).

### Text

The spike showed that pen strokes can be recovered from both fonts, and that revealing the real glyph
through a fat copy of its stroke gives exact results. The writer works like this for one block of
text (a heading, a paragraph, a list item, a table cell, a line of code, a label):

1. **Hide the real text** with `-webkit-text-fill-color: transparent` on the block. Layout, selection
   and colours of descendants are untouched; only glyph paint is suppressed.
2. **Lay a canvas over the block**, sized to it, `pointer-events: none`, `aria-hidden`. It must scroll
   with the content (`.nb-main` scrolls in the editor), so it lives inside the block's cell, not on
   `body`.
3. **Find each character's box** from the browser's own layout: walk the block's text nodes and read
   `Range.getClientRects()` per character, when the block's turn comes and not before. This gives
   wrapping, kerning, alignment and inline styling for free. Baseline = box top + ascent, with ascent
   and descent taken from the font's metrics in the stroke data.
4. **Write.** For the glyph under the pen: on a small scratch canvas, stroke the part of its
   centreline written so far at about 2.4 times the font's stroke width, then draw the real character
   with `fillText` using `globalCompositeOperation = 'source-in'`, and copy the result to the block's
   canvas. Finished glyphs are drawn once with plain `fillText` and never touched again.
   Font, size, weight and colour come from the computed style of the character's parent element, so
   syntax colours, bold, links and the muted greys all carry over.
5. **Hand over.** When the block is done, remove the canvas and the hiding style in the same frame.
   The pixels under both are the same glyphs in the same place.

Details that matter:

- **Glyphs without strokes** (emoji, symbols such as `▦ ◉ ∿`, anything outside the two fonts) are
  revealed with a left-to-right wipe of the real character on the same canvas. No character is ever
  left unwritten.
- **Ligatures.** Cascadia Code joins `!=`, `>=`, `=>` into single glyphs, which have no strokes and
  whose boxes are shared. Treat a ligature as one unit and wipe it, or switch ligatures off for code
  (`font-variant-ligatures: none`) if that is judged not to change the look. Decide by looking.
- **SVG text** in charts uses the same glyph painter. Positions come from
  `getStartPositionOfChar(i)` and the transform from `getScreenCTM()`, so rotated axis titles work.
  Its canvas sits over the chart's SVG.
- **Backgrounds and decorations** (the highlighter on `{{ }}` values, inline code chips, table bars
  and heat cells) should not appear before their text. Fade or wipe them in as the text reaches them.
- **Native controls** (selects, sliders, buttons) cannot be written. Their hand-drawn outline is
  drawn, then the control fades in.
- **The code editor** is a highlighted `<pre>` under a transparent `<textarea>`. Write the `<pre>`.
  Typing in the editor never animates.
- **Device pixel ratio and zoom.** Size canvases by `devicePixelRatio`, and re-measure if the block
  resizes mid-job (a `ResizeObserver`, or simply finish the job on resize).
- **Dark mode** is a CSS filter on `body`, so canvases invert with everything else.

### The pen: order, speed and budget

- **Order** is reading order: cells top to bottom; inside a cell, outline bracket, then content top
  to bottom, left to right. Cells overlap: the next starts shortly after the previous one starts, not
  when it ends, so a screen fills in a wave.
- **Speed** is constant along the ink within a job (duration proportional to length), with a gentle
  ease at the start and end of each stroke and a short lift between strokes and between words.
- **Budget.** Everything visible when the page opens finishes within a fixed time, about 2.5 seconds
  at the default setting. Jobs are scaled to fit: the more ink on screen, the faster the pen, as in
  Manim's `Write`, where total time is capped and the stagger shrinks with the count. No block takes
  longer than about 1.2 seconds on its own.
- **Caps.** At most about 150 SVG paths and 40 glyphs in motion at any moment. When a job would
  exceed that, it degrades in steps: per-stroke drawing → per-row or per-series sweep → fade.
  Tables write their header and the first rows cell by cell, then sweep the remaining rows one row
  at a time.
- **Below the fold.** Content off screen is not animated until it scrolls into view
  (`IntersectionObserver` per cell). It is hidden until then by the same paint-only means. A cell
  scrolled past quickly is finished immediately.
- **Skipping.** Escape, or a click or key press anywhere, finishes everything on screen at once.
- **Updates.** A change that follows the reader's own action within the last second (dragging a
  slider, brushing a chart, switching a tab, paging a table) is shown immediately or with a draw of
  at most 200 ms. Only first appearance and fresh run results get the full treatment.
- **Settings.** Off, gentle (default) and slow, in the gear menu, remembered in `localStorage`.
  Consider a `[notebook]` key so an author can switch it off for a notebook; that is a format change
  and needs `docs/FORMAT.md` and the parser to move together.

### When it is off

The animation does not run, and `Ink.present` returns at once, when any of these holds:

- `prefers-reduced-motion: reduce`.
- Printing: finish every job on `beforeprint`, and add `@media print` rules that neutralise any
  hiding style in case one is live.
- PDF export: `export.cpp` marks the page it hands to headless Chrome (an attribute on `<html>` or a
  flag in the embedded data). Do not rely on `--virtual-time-budget` to run animations to the end.
- The reader's setting is off, or the URL carries `ink=off`.
- The browser lacks something the writer needs. Feature-detect and fall through to the plain page.

HTML exports animate when opened. That is the point of them: they are what readers see.

## The stroke data

`tools/font_strokes.py` grows from `spikes/handwriting-strokes/strokes.py`. It is a development
tool: its output is checked in, and neither the extension build nor CI runs it. Needs Python with
`fonttools`, `brotli`, `scikit-image`, `scipy`, `pillow`, `numpy`.

- **Coverage:** every glyph in Excalifont (212), and printable ASCII plus Latin-1 for Cascadia Code.
  Everything else uses the wipe.
- **Output:** `frontend/ink-strokes.js`, per font: ascent, descent, stroke width, and per character a
  list of strokes as flat integer arrays in thousandths of an em. Expect roughly 100 KB before
  compression; keep an eye on export size.
- **A coverage check per glyph:** rasterise the glyph, rasterise its strokes at the reveal width, and
  require that the strokes cover the glyph completely. Where they do not, add a stroke or widen that
  glyph's reveal. Without this a sliver of a letter can stay hidden until the hand-over.
- **Stroke order and direction** come from heuristics (top-left first, left to right, dots and
  crossbars last). Add an overrides file for glyphs that look wrong, and a contact sheet page that
  plays every glyph, so they can be judged by eye.
- **Bold** uses the same strokes with a wider reveal.

## What changes where

| File | Change |
|---|---|
| `frontend/render-ink.js` | New: scheduler, shape animator, text writer, switches |
| `frontend/ink-strokes.js` | New, generated |
| `tools/font_strokes.py`, `tools/README.md` | New: generator, contact sheet, overrides |
| `frontend/index.html`, `frontend/viewer.html` | Load the new scripts; gear menu setting in the editor |
| `src/functions/export.cpp` | Inline the new scripts; mark PDF exports as not animated |
| `frontend/app.js`, `frontend/viewer.js` | Call `Ink.present` with keys and signatures after building each piece; mark reader-initiated updates |
| `frontend/render-core.js`, `render-chart.js`, `render-typed.js` | Only where order tags are needed; drawing code otherwise untouched |
| `frontend/render-sketch.js` | `Sketch.outline` redraws on resize: make sure a redraw does not restart a job |
| `frontend/style.css` | The hiding rule, canvas layer, print rules |
| `test/browser/test_browser.py` | Existing tests run with the animation off; new tests below |
| `docs/DESIGN.md`, `CLAUDE.md`, `.github/skills/sketchbook-look/SKILL.md` | Record the principles and the rules for new components |

No change to the notebook format, the C++ core, or the server, apart from the export flag.

## Tests

Existing browser tests must pass unchanged with the animation off (`ink=off`), and that is how they
should run. New checks, in Chromium through Playwright:

1. **No trace.** Render with the animation on, finish it, and compare `outerHTML` of the notebook
   with a render that had it off. They are equal.
2. **Same pixels.** Screenshots of the two are equal, within anti-aliasing tolerance.
3. **Glyph alignment.** For sample strings in both fonts and several sizes, the writer's final canvas
   frame matches the real text underneath.
4. **Mid-flight.** Part-way through, `innerText` is already complete, a canvas is present, and some
   paths are hidden.
5. **Re-render does not restart.** Trigger a render mid-flight; progress does not go backwards and
   the page still finishes on time.
6. **Second render is still.** Rendering again after everything has finished creates no jobs.
7. **Budget.** `examples/charts.sql` finishes its first screen within the budget, and frame times
   stay smooth (measure with `requestAnimationFrame` timestamps; record the numbers).
8. **Switches.** Reduced motion, `ink=off`, and print each produce a complete page with no jobs.
9. **PDF and Markdown exports** are unchanged.

Add `frontend/render-ink.js` and `ink-strokes.js` to `make frontend_check`.

## Order of work

Each step leaves the tree passing and can be reviewed alone.

1. **Scaffold.** `Ink` with the registry, clock, scheduler and switches, doing nothing visible.
   Scripts loaded and inlined. `ink=off` in the existing tests. Tests 1, 6 and 8.
2. **Shapes.** Subpath splitting, drawing, fills, plain-mark sweeps. Cell outlines, the title
   underline, the table rule, then charts. Tests 2, 4, 5.
3. **Stroke data.** The tool, coverage check, contact sheet, generated file.
4. **Text.** The writer for prose and headings first; test 3. Then tables, code, labels, KPI numbers,
   chart text, decorations and controls.
5. **The pen.** Budget, caps and degradation, scrolling into view, skipping, reader-initiated updates,
   the gear setting. Test 7. Tune by watching `examples/revenue.sql` and `examples/charts.sql` in the
   editor, the reading view, dark mode and an HTML export.
6. **Exports and docs.** PDF flag, print rules, test 9, `DESIGN.md`, `CLAUDE.md`, the look skill.
   Record a short screen capture of both examples for the pull request.

## Risks

| Risk | What to do |
|---|---|
| Repaint cost of many moving strokes, especially in Safari, which is slow at `stroke-dashoffset` | The caps and the degradation ladder; measure early (step 2) on `charts.sql`; a canvas overlay for shapes is the fallback if SVG cannot hold the frame rate |
| The writer's glyphs sit a fraction of a pixel off the real text, so the hand-over twitches | Test 3; take positions from the browser, never from advance widths; a cross-fade of a few frames if a residue remains |
| A glyph's strokes do not cover it | The coverage check in the tool |
| Hidden text leaks into a state where nothing will reveal it (an error mid-job, a print, a detached element) | Hiding is applied only by a running job and removed in a `finally`; print rules; a watchdog that finishes any job older than its duration plus a margin |
| The whole thing is too slow to be pleasant on a long notebook | The budget is enforced in one place; skipping; the setting |
| `render()` storms (a slider drag) restart or stack jobs | The registry and the reader-initiated rule; test 5 |
| Only Chromium is available to test here | Say so in the pull request; keep to widely supported features (`-webkit-text-fill-color`, canvas compositing, `IntersectionObserver`); feature-detect |

## Background

Findings from the survey that shaped this plan:

- Drawing a path with `stroke-dasharray` and `stroke-dashoffset` is the standard technique (Vivus,
  Rough Notation, Motion, anime.js). Rough Notation, which animates rough.js annotations, makes
  duration proportional to path length and sequences strokes with delays; that is the model for
  shapes.
- `stroke-dashoffset` animates on the main thread in every browser, whichever API drives it.
- excalidraw-animate, the closest prior art for this look, grows shape paths segment by segment but
  only reveals text left to right like a typewriter. It does not write letters.
- Hanzi Writer is the model for the text technique: a fat stroke along a character's median line
  reveals the real outline. It does this with SVG clip paths for one large character at a time; for
  thousands of small glyphs a canvas is the cheaper place to do the same compositing.
- Single-line fonts (Hershey, Relief SingleLine) would give pen strokes directly, but they are not
  Excalifont, and browsers cannot render open-path fonts as text.
- No published source for Excalifont's or Virgil's pen strokes was found, hence generating them.

Not verified in the survey, and worth a quick check when it matters: whether dash patterns restart
per subpath in every browser exactly as assumed, Playwright's handling of running animations in
screenshots, and behaviour outside Chromium.

## What was built

Everything in the plan's order of work is in place: `frontend/render-ink.js` (`Ink`),
`frontend/ink-strokes.js` (generated), `tools/font_strokes.py` with its coverage check, overrides and
contact sheet (`tools/README.md`), the calls in `app.js` and `viewer.js`, the export flag, the gear
setting, the print rules and the tests. Where the plan turned out wrong or impractical, this is what was
done instead.

**Glyphs in motion are composited together, once per frame.** The plan revealed each glyph on a scratch
canvas and copied it to the piece's canvas. Measured, that cost about 0.2 ms a glyph, almost all of it a
canvas flush forced by each copy, so 40 glyphs took most of a frame. Now the live layer draws the real
text of every glyph in motion, then applies `destination-in` once with a mask holding every glyph's pen
path so far. Painting fell from about 7 ms to under 2 ms a frame. A glyph's fat mask can in principle
show a few pixels of a neighbour that is also in motion; in practice it is not visible. The last frame
of a glyph is still exactly its `fillText`, which is what the done layer draws: the glyph alignment test
finds no differing pixels in either font at five sizes.

**A cell's bracket is drawn from its end.** A cell's outline is a full rounded box clipped to its left
edge, and the left edge is the end of the path. Drawn forwards, the visible bracket appeared only at the
end of a long invisible draw. `Ink.bracket(box)` draws it backwards, paces it by the part that shows
(`visible`), and finishes the piece once that part is drawn.

**A changed piece writes only what is new.** The plan rewrote a piece whenever its signature changed.
After a run, that meant a paragraph disappearing and being written again only because a `{{ }}` value
was filled in. Text (and decorated elements) that the previous version of the same key already showed
appears at once; only new text is written.

**Waiting below the fold is cheap.** Taking a piece apart (splitting paths, hiding strokes) for every
chart on a long page cost a 200 ms frame at load. A piece below the fold is now hidden whole with
`opacity: 0` on its root (still paint only) and taken apart when it scrolls into view. Being visible is
checked on scroll and resize rather than with an `IntersectionObserver`, which does not report an
element that jumps from below the viewport to above it in one scroll.

**Plain marks.** A run of more than 60 plain marks (points, cells of a heat map) that makes up most of
its group fades in with the group; otherwise it appears in eight chunks. One atom per mark meant 7,782
style writes a frame for one calendar chart, and gathering the run into a temporary group cost 150 ms of
DOM moves.

**Degradation.** Level 0 writes everything; level 1 sweeps table rows after the first three and fades
hatching; level 2 sweeps every text node (code then reads as typed, token by token). The level is chosen
per piece from its actual peak concurrency on the timeline, jumping straight to 2 when far over. The cap
on glyphs is 60, not 40: with batched compositing 60 costs less than 40 did, and 40 pushed ordinary
paragraphs into sweeps.

**Pace.** Pieces of one cell start close together (each 20% into the previous) and each cell 30% into
the one above, so a screen fills as a wave. Pen speed is 2.6 px of ink per ms, a piece takes 260 to
1,200 ms, and a whole batch is scaled to fit the 2.5 s budget.

**Smaller choices.**
- Keys in the editor follow the cell object, not its position, so moving or inserting a cell rewrites
  nothing else. Code is keyed to the notebook's generation (opened or reloaded from disk), so typing
  never writes.
- `Sketch.outline` calls `Ink.replaced(old, svg)` when it redraws at a new size; any other change inside
  a running piece (the table's header rule added a frame later) is caught by a `MutationObserver` and the
  piece is attached again at the same point in time.
- Cascadia Code's ligatures are left on. A run of operator characters in the code font is one unit,
  swept rather than written, and drawn with one `fillText` so the canvas shapes the same ligature.
- A heading's underline (a CSS `::after` background) is wiped in after its text through a
  `--ink-wipe` custom property and a `clip-path`.
- Anything that goes wrong inside a frame finishes every piece at once, so an error can never leave a
  page hidden. Fonts that have not loaded after 3 s release the page.
- `Ink.pause()`, `Ink.advance(ms)` and `window.__INK_PAUSE` stop and move the clock, for tests.
- No `[notebook]` setting: the gear menu's setting (remembered per browser) and `ink=off` were enough,
  and the format stays as it was.

**Measured** in headless Chromium, software rendering, 1300 x 950, from `make browser_test` and by
watching captured frames:

| | `examples/revenue.sql` export | `examples/charts.sql` export |
|---|---|---|
| First screen written | 1.9 s | 2.4 s |
| Frame interval, median / 95th percentile while writing | 16.7 / 16.7 ms | 16.7 / 16.8 ms |
| Ink's work per frame, median / 95th percentile | 1.8 / 4.8 ms | 1.7 / 4.8 ms |
| Worst frame (taking a large chart apart as it scrolls in) | | 60 to 75 ms |

In the editor, opening `revenue.sql` settles in 2.2 s and a Run all in 1.7 s.

**Not done, or not verified.**
- Only Chromium was available. Safari's `stroke-dashoffset` repaint cost, Firefox's canvas text
  rendering, and `-webkit-text-fill-color` with `::marker` outside Chromium are unverified.
- The graph view, the catalog sidebar, the chart picker and the gear menu are tools rather than parts of
  the notebook, and appear as before.
- The few pixels by which a moving glyph's mask can reveal a moving neighbour (above).
- Taking a very large chart apart still costs one long frame when it scrolls into view.

**Presenting scales the page, and the writer follows.** A presented screen (DESIGN §16) is the reading
layout under one CSS transform. A piece's canvases sit inside that transform, so `layers()` measures
the scale of their host (its on-screen width over its layout width) and positions, sizes and draws in
the host's own px, with a backing store denser by the same factor. Characters measured from the
browser's layout are divided by the scale; SVG text already carries it in its screen matrix, which is
divided too. A presentation's pieces have their own keys, so each screen is written when it first
arrives and never again; moving to it calls `Ink.fresh` so the key press does not make it a quick
update. The glyph-alignment test is repeated on a scaled screen.

