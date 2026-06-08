"""Sample the rendered original High Scores (log/orig_s23.png) to extract ground-truth
element positions + colors, to correct port assumptions. Read-only analysis."""
from PIL import Image
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'log/orig_s23.png'
o = Image.open(path).convert('RGB')
W, H = o.size
px = o.load()
print('size', W, H)


def bands(pred, gap=6, minrun=3):
    ys = []
    for y in range(H):
        n = sum(1 for x in range(W) if pred(px[x, y]))
        if n > minrun:
            ys.append(y)
    if not ys:
        return []
    out = []
    s = p = ys[0]
    for y in ys[1:]:
        if y - p > gap:
            out.append((s, p))
            s = y
        p = y
    out.append((s, p))
    return out


def is_yellow(c):
    return c[0] > 150 and c[1] > 120 and c[2] < 90


def is_white(c):
    return c[0] > 190 and c[1] > 190 and c[2] > 190


print('YELLOW bands (title etc):', bands(is_yellow))
print('WHITE bands (text rows):', bands(is_white))

# For the data rows, sample the dominant text color per row band by scanning the
# leftmost bright cluster (the rank+name). Compare row1 vs row2.
wb = bands(is_white, gap=8)
# also bright-any (incl yellow) bands to catch a yellow row1
def bright(c):
    return (c[0] > 150 or c[1] > 150) and max(c) > 150
ab = bands(bright, gap=8)
print('BRIGHT-any bands:', ab)


def row_color(y0, y1):
    # collect bright pixels in this band, average their color
    rs = gs = bs = n = 0
    for y in range(y0, y1 + 1):
        for x in range(W):
            c = px[x, y]
            if max(c) > 150:
                rs += c[0]; gs += c[1]; bs += c[2]; n += 1
    if n == 0:
        return None
    return (rs // n, gs // n, bs // n, n)


for (y0, y1) in ab:
    print('  band y[%d..%d] avg-bright-color=%s' % (y0, y1, row_color(y0, y1)))
