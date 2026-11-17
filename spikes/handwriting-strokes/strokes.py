"""Spike: recover pen strokes (centerlines) from an outline font by skeletonising each glyph.
Output: JSON {font: {upm, ascent, descent, width, glyphs: {char: {adv, s: [[x,y,...], ...]}}}} in em units x1000."""
import json, math, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
from skimage.morphology import skeletonize
from scipy import ndimage as ndi

SIZE = 320          # raster em size in px
PAD = 80

def neighbours(p, pix):
    y, x = p
    return [(y+dy, x+dx) for dy in (-1,0,1) for dx in (-1,0,1) if (dy or dx) and (y+dy, x+dx) in pix]

def trace(skel):
    pix = set(map(tuple, np.argwhere(skel)))
    deg = {p: len(neighbours(p, pix)) for p in pix}
    # junction clusters: pixels with degree >= 3
    nodes = {p for p in pix if deg[p] != 2}
    paths, seen = [], set()
    def walk(start, nxt):
        path = [start, nxt]; prev, cur = start, nxt
        while cur not in nodes:
            seen.add(cur)
            cand = [q for q in neighbours(cur, pix) if q != prev and q not in path[-3:]]
            if not cand: break
            # prefer 4-connected continuation
            cand.sort(key=lambda q: abs(q[0]-cur[0]) + abs(q[1]-cur[1]))
            prev, cur = cur, cand[0]; path.append(cur)
        return path
    for n in nodes:
        for q in neighbours(n, pix):
            if q in nodes:
                if (q, n) not in seen and (n, q) not in seen:
                    seen.add((n, q)); paths.append([n, q])
                continue
            if q in seen: continue
            paths.append(walk(n, q))
    # closed loops with no nodes (o, 0)
    rest = [p for p in pix if p not in seen and p not in nodes]
    while rest:
        s = rest[0]; nb = neighbours(s, pix)
        if not nb: seen.add(s); rest = [p for p in rest if p not in seen]; continue
        loop = [s]; prev, cur = s, nb[0]; seen.add(s)
        while cur != s and cur not in seen:
            seen.add(cur); loop.append(cur)
            cand = [q for q in neighbours(cur, pix) if q != prev and (q not in seen or q == s)]
            if not cand: break
            cand.sort(key=lambda q: abs(q[0]-cur[0]) + abs(q[1]-cur[1]))
            prev, cur = cur, cand[0]
        loop.append(s); paths.append(loop)
        rest = [p for p in rest if p not in seen]
    return paths, nodes, deg

def length(p):
    return sum(math.hypot(p[i+1][0]-p[i][0], p[i+1][1]-p[i][1]) for i in range(len(p)-1))

def direction(p, at_end, k=10):
    a, b = (p[-1], p[max(0, len(p)-1-k)]) if at_end else (p[0], p[min(len(p)-1, k)])
    d = (a[0]-b[0], a[1]-b[1]); n = math.hypot(*d) or 1
    return (d[0]/n, d[1]/n)

def merge(paths, width):
    """Join strokes that meet at a junction and continue in the same direction (pen did not lift)."""
    paths = [list(p) for p in paths]
    changed = True
    while changed:
        changed = False
        best = None
        for i in range(len(paths)):
            for j in range(len(paths)):
                if i == j: continue
                for ei in (0, 1):
                    for ej in (0, 1):
                        a = paths[i][-1] if ei else paths[i][0]
                        b = paths[j][-1] if ej else paths[j][0]
                        gap = math.hypot(a[0]-b[0], a[1]-b[1])
                        if gap > width * 0.9: continue
                        da, db = direction(paths[i], ei), direction(paths[j], ej)
                        dot = -(da[0]*db[0] + da[1]*db[1])      # 1 = straight continuation
                        if dot < 0.45: continue
                        score = dot - gap / (width * 3)
                        if best is None or score > best[0]: best = (score, i, j, ei, ej)
        if best:
            _, i, j, ei, ej = best
            a = paths[i] if ei else paths[i][::-1]
            b = paths[j][::-1] if ej else paths[j]
            merged = a + b
            paths = [p for k, p in enumerate(paths) if k not in (i, j)] + [merged]
            changed = True
    return paths

def rdp(pts, eps):
    if len(pts) < 3: return pts
    a, b = np.array(pts[0], float), np.array(pts[-1], float)
    ab = b - a; n = np.hypot(*ab)
    def off(p):
        v = np.array(p, float) - a
        return abs(ab[0] * v[1] - ab[1] * v[0]) / n if n else float(np.hypot(*v))
    d = [off(p) for p in pts[1:-1]]
    i = int(np.argmax(d)) + 1
    if d[i-1] > eps: return rdp(pts[:i+1], eps)[:-1] + rdp(pts[i:], eps)
    return [pts[0], pts[-1]]

def glyph_strokes(font, ch, upm_scale):
    img = Image.new('L', (SIZE * 2 + PAD * 2, SIZE * 2 + PAD * 2), 0)
    ImageDraw.Draw(img).text((PAD, PAD), ch, font=font, fill=255)
    a = np.array(img) > 127
    if not a.any(): return [], 0
    dist = ndi.distance_transform_edt(a)
    skel = skeletonize(a)
    width = float(np.median(dist[skel])) * 2
    paths, nodes, deg = trace(skel)
    # prune spurs: short branches ending in a free end at a junction
    ends = {p for p in nodes if deg[p] <= 1}
    keep = []
    for p in paths:
        free = (p[0] in ends) + (p[-1] in ends)
        if free == 1 and length(p) < width * 0.9 and len(paths) > 1: continue
        keep.append(p)
    paths = merge(keep, width)
    # a dot (i, j, ., :) skeletonises to a point or tiny path: keep as a zero-length stroke
    out = []
    for p in paths:
        # pen direction: start at the top-left-most end, unless the stroke is a closed loop
        closed = p[0] == p[-1]
        if not closed and (p[0][0] + p[0][1] * 0.35) > (p[-1][0] + p[-1][1] * 0.35): p = p[::-1]
        pts = rdp(p, 1.1)
        out.append([(x - PAD, y - PAD) for (y, x) in pts])
    # order: left to right by start x, dots and short strokes last
    out.sort(key=lambda s: (length(s) < width * 1.2, min(q[0] for q in s) + 0.25 * s[0][1]))
    return out, width

def run(name, chars):
    ttf = f'{name}.ttf'
    tt = TTFont(ttf); upm = tt['head'].unitsPerEm; cmap = tt.getBestCmap(); hmtx = tt['hmtx']
    font = ImageFont.truetype(ttf, SIZE)
    asc, desc = font.getmetrics()
    glyphs, widths = {}, []
    for ch in chars:
        if ord(ch) not in cmap: continue
        strokes, w = glyph_strokes(font, ch, upm)
        if w: widths.append(w)
        k = 1000.0 / SIZE
        glyphs[ch] = {'adv': round(hmtx[cmap[ord(ch)]][0] * 1000 / upm),
                      's': [[round(v * k) for q in s for v in (q[0], q[1] - asc)] for s in strokes]}
    return {'ascent': round(asc * 1000 / SIZE), 'descent': round(desc * 1000 / SIZE),
            'width': round(float(np.median(widths)) * 1000 / SIZE), 'glyphs': glyphs}

if __name__ == '__main__':
    chars = [chr(c) for c in range(33, 127)]
    # The repository ships woff2; PIL needs TrueType, so unpack next to this script first.
    here = os.path.dirname(os.path.abspath(__file__))
    for n in ('Excalifont', 'CascadiaCode'):
        f = TTFont(os.path.join(here, '..', '..', 'frontend', n + '.woff2')); f.flavor = None; f.save(n + '.ttf')
    data = {n: run(n, chars) for n in ('Excalifont', 'CascadiaCode')}
    json.dump(data, open('strokes.json', 'w'), separators=(',', ':'))
    for n, d in data.items():
        ns = [len(g['s']) for g in d['glyphs'].values()]
        print(n, 'glyphs', len(d['glyphs']), 'stroke width (em/1000)', d['width'], 'strokes per glyph avg %.2f max %d' % (sum(ns)/len(ns), max(ns)),
              'points', sum(len(s)//2 for g in d['glyphs'].values() for s in g['s']))
    print('json bytes', os.path.getsize('strokes.json'))
    page = open(os.path.join(here, 'compare.tpl.html')).read().replace('__DATA__', open('strokes.json').read())
    open('compare.html', 'w').write(page)
    print('wrote compare.html: open it in a browser from this directory')
