"""Recover pen strokes from the notebook's fonts and write them to frontend/ink-strokes.js.

The handwriting animation (frontend/render-ink.js, docs/HANDWRITING.md) reveals each real glyph
through a fat copy of its pen stroke. This tool finds those strokes: it rasterises each glyph, thins it
to a one-pixel skeleton, traces the skeleton into paths, prunes spurs, joins paths that continue
through a junction, and orders and orients them as a hand would (top-left first, left to right, dots
last). Grown from spikes/handwriting-strokes/strokes.py.

It also checks coverage: for every glyph it measures how far the furthest inked pixel is from the
nearest stroke, and records that as the glyph's reveal radius when it is wider than the default, so a
glyph is always fully revealed by the time its strokes are done.

This is a development tool. Its output is checked in; neither the extension build nor CI runs it.
Needs Python with fonttools, brotli, scikit-image, scipy, pillow and numpy.

    python3 tools/font_strokes.py            # writes frontend/ink-strokes.js
    python3 tools/font_strokes.py --report   # also lists the glyphs that needed a wider reveal

Hand corrections go in tools/font_strokes_overrides.json (see tools/README.md).
"""
import json
import math
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from scipy import ndimage as ndi
from skimage.morphology import skeletonize

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIZE = 320          # raster em size in px
PAD = 80
# The reveal stroke is this many times the font's median stroke width (the spike's finding).
REVEAL = 2.4

# CSS family name -> file, and which characters to process. Excalifont is small enough to take whole;
# Cascadia Code is limited to printable ASCII and Latin-1. Anything else is wiped in at run time.
FONTS = {
    'Excalifont': ('Excalifont.woff2', None),
    'Cascadia': ('CascadiaCode.woff2', [c for c in range(0x21, 0x7F)] + [c for c in range(0xA1, 0x100)]),
}


def neighbours(p, pix):
    y, x = p
    return [(y + dy, x + dx) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy or dx) and (y + dy, x + dx) in pix]


def trace(skel):
    pix = set(map(tuple, np.argwhere(skel)))
    deg = {p: len(neighbours(p, pix)) for p in pix}
    nodes = {p for p in pix if deg[p] != 2}
    paths, seen = [], set()

    def walk(start, nxt):
        path = [start, nxt]
        prev, cur = start, nxt
        while cur not in nodes:
            seen.add(cur)
            cand = [q for q in neighbours(cur, pix) if q != prev and q not in path[-3:]]
            if not cand:
                break
            cand.sort(key=lambda q: abs(q[0] - cur[0]) + abs(q[1] - cur[1]))
            prev, cur = cur, cand[0]
            path.append(cur)
        return path

    for n in sorted(nodes):
        for q in neighbours(n, pix):
            if q in nodes:
                if (q, n) not in seen and (n, q) not in seen:
                    seen.add((n, q))
                    paths.append([n, q])
                continue
            if q in seen:
                continue
            paths.append(walk(n, q))
    # Closed loops with no junction (o, 0).
    rest = sorted(p for p in pix if p not in seen and p not in nodes)
    while rest:
        s = rest[0]
        nb = neighbours(s, pix)
        if not nb:
            seen.add(s)
            rest = [p for p in rest if p not in seen]
            continue
        loop = [s]
        prev, cur = s, nb[0]
        seen.add(s)
        while cur != s and cur not in seen:
            seen.add(cur)
            loop.append(cur)
            cand = [q for q in neighbours(cur, pix) if q != prev and (q not in seen or q == s)]
            if not cand:
                break
            cand.sort(key=lambda q: abs(q[0] - cur[0]) + abs(q[1] - cur[1]))
            prev, cur = cur, cand[0]
        loop.append(s)
        paths.append(loop)
        rest = [p for p in rest if p not in seen]
    return paths, nodes, deg


def length(p):
    return sum(math.hypot(p[i + 1][0] - p[i][0], p[i + 1][1] - p[i][1]) for i in range(len(p) - 1))


def direction(p, at_end, k=10):
    a, b = (p[-1], p[max(0, len(p) - 1 - k)]) if at_end else (p[0], p[min(len(p) - 1, k)])
    d = (a[0] - b[0], a[1] - b[1])
    n = math.hypot(*d) or 1
    return (d[0] / n, d[1] / n)


def merge(paths, width):
    """Joins strokes that meet at a junction and continue in the same direction (the pen did not lift)."""
    paths = [list(p) for p in paths]
    changed = True
    while changed:
        changed = False
        best = None
        for i in range(len(paths)):
            for j in range(len(paths)):
                if i == j:
                    continue
                for ei in (0, 1):
                    for ej in (0, 1):
                        a = paths[i][-1] if ei else paths[i][0]
                        b = paths[j][-1] if ej else paths[j][0]
                        gap = math.hypot(a[0] - b[0], a[1] - b[1])
                        if gap > width * 0.9:
                            continue
                        da, db = direction(paths[i], ei), direction(paths[j], ej)
                        dot = -(da[0] * db[0] + da[1] * db[1])
                        if dot < 0.45:
                            continue
                        score = dot - gap / (width * 3)
                        if best is None or score > best[0]:
                            best = (score, i, j, ei, ej)
        if best:
            _, i, j, ei, ej = best
            a = paths[i] if ei else paths[i][::-1]
            b = paths[j][::-1] if ej else paths[j]
            paths = [p for k, p in enumerate(paths) if k not in (i, j)] + [a + b]
            changed = True
    return paths


def rdp(pts, eps):
    if len(pts) < 3:
        return pts
    a, b = np.array(pts[0], float), np.array(pts[-1], float)
    ab = b - a
    n = np.hypot(*ab)

    def off(p):
        v = np.array(p, float) - a
        return abs(ab[0] * v[1] - ab[1] * v[0]) / n if n else float(np.hypot(*v))

    d = [off(p) for p in pts[1:-1]]
    i = int(np.argmax(d)) + 1
    if d[i - 1] > eps:
        return rdp(pts[:i + 1], eps)[:-1] + rdp(pts[i:], eps)
    return [pts[0], pts[-1]]


def raster(font, ch):
    img = Image.new('L', (SIZE * 2 + PAD * 2, SIZE * 2 + PAD * 2), 0)
    ImageDraw.Draw(img).text((PAD, PAD), ch, font=font, fill=255)
    return np.array(img) > 127


def glyph_strokes(font, ch, fix):
    a = raster(font, ch)
    if not a.any():
        return [], 0, a
    dist = ndi.distance_transform_edt(a)
    skel = skeletonize(a)
    width = float(np.median(dist[skel])) * 2
    paths, nodes, deg = trace(skel)
    ends = {p for p in nodes if deg[p] <= 1}
    keep = []
    for p in paths:
        free = (p[0] in ends) + (p[-1] in ends)
        if free == 1 and length(p) < width * 0.9 and len(paths) > 1:
            continue
        keep.append(p)
    paths = merge(keep, width)
    out = []
    for p in paths:
        # Pen direction: start at the top-left end, unless the stroke is a closed loop.
        closed = p[0] == p[-1]
        if not closed and (p[0][0] + p[0][1] * 0.35) > (p[-1][0] + p[-1][1] * 0.35):
            p = p[::-1]
        out.append([(x - PAD, y - PAD) for (y, x) in rdp(p, 1.1)])
    # Order: left to right by start, dots and short strokes last.
    out.sort(key=lambda s: (length(s) < width * 1.2, min(q[0] for q in s) + 0.25 * s[0][1]))
    if fix:
        if 'order' in fix:
            out = [out[i] for i in fix['order'] if i < len(out)] + [s for i, s in enumerate(out) if i not in fix['order']]
        for i in fix.get('reverse', []):
            if i < len(out):
                out[i] = out[i][::-1]
    return out, width, a


def needed_radius(a, strokes):
    """The distance (raster px) from the furthest inked pixel of the glyph to the nearest stroke."""
    img = Image.new('L', a.shape[::-1], 0)
    d = ImageDraw.Draw(img)
    for s in strokes:
        pts = [(x + PAD, y + PAD) for (x, y) in s]
        if len(pts) == 1:
            d.point(pts[0], fill=255)
        else:
            d.line(pts, fill=255, width=1)
    lines = np.array(img) > 0
    if not lines.any():
        return float('inf')
    dist = ndi.distance_transform_edt(~lines)
    return float(dist[a].max())


def run(name, filename, codes, overrides):
    tt = TTFont(os.path.join(ROOT, 'frontend', filename))
    tt.flavor = None
    with tempfile.NamedTemporaryFile(suffix='.ttf', delete=False) as f:
        tt.save(f.name)
        ttf = f.name
    cmap = tt.getBestCmap()
    upm = tt['head'].unitsPerEm
    font = ImageFont.truetype(ttf, SIZE)
    asc, desc = font.getmetrics()
    codes = sorted(c for c in (codes or cmap.keys()) if c in cmap and c > 0x20 and not 0x300 <= c < 0x370 and c != 0xAD)
    k = 1000.0 / SIZE
    results, widths = {}, []
    for c in codes:
        ch = chr(c)
        strokes, w, a = glyph_strokes(font, ch, overrides.get(ch))
        if not strokes:
            continue
        widths.append(w)
        results[ch] = (strokes, a)
    width = float(np.median(widths))
    reveal = width * REVEAL / 2   # default reveal radius in raster px
    glyphs, wide = {}, []
    for ch, (strokes, a) in results.items():
        r = needed_radius(a, strokes) + 1.0
        g = {'s': [[round(v * k) for q in s for v in (q[0], q[1] - asc)] for s in strokes]}
        if r > reveal:
            g['r'] = round(r * k)
            wide.append((ch, round(r / reveal, 2)))
        glyphs[ch] = g
    os.unlink(ttf)
    return {'ascent': round(asc * 1000 / SIZE), 'descent': round(desc * 1000 / SIZE), 'width': round(width * k),
            'reveal': round(reveal * k), 'glyphs': glyphs}, wide


def main():
    report = '--report' in sys.argv
    overrides = json.load(open(os.path.join(HERE, 'font_strokes_overrides.json')))
    data = {}
    for name, (filename, codes) in FONTS.items():
        d, wide = run(name, filename, codes, overrides.get(name, {}))
        data[name] = d
        ns = [len(g['s']) for g in d['glyphs'].values()]
        print('%s: %d glyphs, stroke width %d/1000 em, %.2f strokes per glyph (max %d), %d need a wider reveal'
              % (name, len(ns), d['width'], sum(ns) / len(ns), max(ns), len(wide)))
        if report:
            for ch, f in sorted(wide, key=lambda x: -x[1]):
                print('   %r x%.2f' % (ch, f))
    text = json.dumps(data, separators=(',', ':'), ensure_ascii=False, sort_keys=True)
    out = os.path.join(ROOT, 'frontend', 'ink-strokes.js')
    with open(out, 'w', encoding='utf-8') as f:
        f.write('// Generated by tools/font_strokes.py from the fonts in this folder: do not edit.\n'
                '// Pen strokes per glyph, in thousandths of an em from the pen origin on the baseline (y down).\n'
                '// s: strokes as flat [x, y, x, y, ...] lists; r: a reveal radius wider than the font\'s default.\n'
                'var InkStrokes = ' + text + ';\n')
    print('wrote %s (%d bytes)' % (os.path.relpath(out, ROOT), os.path.getsize(out)))


if __name__ == '__main__':
    main()
