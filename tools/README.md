# Development tools

Nothing here is part of the extension build or CI. Outputs are checked in.

## Pen strokes for the handwriting animation

The notebook writes its text onto the page by revealing each real glyph through a fat copy of the pen
strokes that drew it ([docs/HANDWRITING.md](../docs/HANDWRITING.md), `frontend/render-ink.js`). The
fonts do not carry pen strokes, so `font_strokes.py` recovers them: it rasterises each glyph, thins it
to a skeleton, traces the skeleton into strokes, and orders and orients them as a hand would (top-left
first, left to right, dots last). It grew from [spikes/handwriting-strokes](../spikes/handwriting-strokes/README.md).

```bash
pip install fonttools brotli scikit-image scipy pillow numpy
python3 tools/font_strokes.py            # writes frontend/ink-strokes.js (about 30 s)
python3 tools/font_strokes.py --report   # also lists the glyphs that needed a wider reveal
```

Then `GEN=ninja make release`: the frontend is compiled into the extension.

What it covers: every glyph in Excalifont, and printable ASCII plus Latin-1 in Cascadia Code. Anything
else (symbols, emoji, other scripts, ligatures) is wiped in from the left at run time.

**Coverage check.** For each glyph the tool measures how far the furthest inked pixel lies from the
nearest stroke. Where that is wider than the default reveal (2.4 times the font's stroke width), the
glyph gets its own reveal radius (`r` in the data), so a glyph is always fully revealed by the end of its
strokes. The report lists them with the factor; a large factor (more than about 1.5) usually means a
missing stroke, worth a look on the contact sheet.

**Output format** (`frontend/ink-strokes.js`): `InkStrokes[family]` with `ascent`, `descent`, `width`
(median stroke width) and `reveal` (default reveal radius), all in thousandths of an em, and `glyphs`:
per character `s`, a list of strokes as flat `[x, y, x, y, ...]` arrays from the pen origin on the
baseline (y down), and optionally `r`. Families are named as in CSS: `Excalifont`, `Cascadia`.

**Contact sheet.** `tools/contact-sheet.html` plays every glyph with the notebook's own painter. Serve
the repository root (`python3 -m http.server 8790 --bind 127.0.0.1`) and open
`http://127.0.0.1:8790/tools/contact-sheet.html`. "show the strokes" draws each stroke with its number.

**Overrides.** Where the heuristics write a glyph in an odd order or direction, correct it in
`tools/font_strokes_overrides.json` and run the tool again:

```json
{ "Excalifont": { "x": { "order": [1, 0], "reverse": [0] } }, "Cascadia": {} }
```

`order` lists stroke numbers (as shown on the contact sheet) in the order to write them; strokes not
listed follow. `reverse` lists strokes to write from the other end.
