# Spike: pen strokes for the notebook's fonts

Question: can text in the notebook be animated as real handwriting, stroke by stroke, without a
single-line font and without changing how the finished page looks?

**Answer: yes.** Pen strokes can be recovered from Excalifont and Cascadia Code automatically, and
revealing the real glyph through a fat copy of its pen stroke gives a partial glyph that is always a
true piece of the letter, and a finished glyph that is pixel-for-pixel the real font.

This is evidence for [docs/HANDWRITING.md](../../docs/HANDWRITING.md), which is the plan built on it.

## What was tried

`strokes.py` rasterises each glyph at 320 px, thins it to a one-pixel skeleton (scikit-image),
traces the skeleton into paths, prunes spurs, joins paths that continue through a junction, orders
and orients them (top-left first, left to right, dots last) and writes them as JSON in thousandths of
an em.

| | Excalifont | Cascadia Code |
|---|---|---|
| Glyphs processed (printable ASCII) | 94 | 94 |
| Median stroke width | 0.090 em | 0.097 em |
| Strokes per glyph, average / most | 2.1 / 8 | 2.2 / 9 |
| Points | 2,225 | 1,716 |

Both fonts together: 39 KB of JSON, in about 12 seconds.

## Two ways to draw with the strokes

**1. Stroke the centreline itself** ([strokes-vs-font.png](strokes-vs-font.png)). For Excalifont,
which is drawn with a pen of constant width, the stroked centreline is nearly indistinguishable from
the font. For Cascadia Code it is not: smoothing rounds the sharp corners, so `E`, `F` and `L` come
out wrong.

**2. Reveal the real glyph through a fat stroke** ([masked-reveal.png](masked-reveal.png)). Draw the
partial centreline 2.4 times as wide as the font's stroke, then paint the real glyph with
`globalCompositeOperation = 'source-in'`, so only the part of the glyph under the pen shows. At 100%
the result is exactly the real glyph, in either font; part-way it is a true fragment of the letter.
The centreline only has to cover the glyph, so its flaws do not show.

The plan uses the second. It is done on a 2D canvas, where it costs two draw calls per glyph in
progress, not with SVG masks, which re-rasterise their whole subtree every frame.

## What the spike did not do

- Position glyphs from the browser's own layout. The test page advances by the font's advance width
  and ignores kerning, which is why its lines drift from the real text. The real thing reads each
  character's box from the laid-out page.
- Check that a fat stroke covers every glyph completely. Thin serifs or a missed spur would leave a
  sliver hidden until the glyph is swapped for the real text. The production tool needs this check.
- Glyphs beyond printable ASCII, stroke order by hand, bold weights, or timing.
- Any browser but Chromium.

## Reproduce

Needs Python with `fonttools`, `brotli`, `scikit-image`, `scipy`, `pillow` and `numpy`.

```bash
python3 strokes.py
```

It unpacks the fonts from `frontend/`, writes `strokes.json`, and builds `compare.html`. Open that
page in a browser from this directory.
