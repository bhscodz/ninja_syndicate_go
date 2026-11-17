"""8-neighbour 'blob' autotiling for the Ninja Adventure terrain sheets.
All terrain sets share one layout (3x3 nine-patch + strips + inner-corner / combo tiles); the layout is read once
from the grass/dirt block of TilesetFloor (tile sampling) and re-used for every other set by offset."""
from PIL import Image
R = "Ninja Adventure - Asset Pack/Backgrounds/Tilesets/"
GRASS = (173, 188, 58)
PTS = {'NW': (1, 1), 'N': (8, 1), 'NE': (14, 1), 'W': (1, 8), 'E': (14, 8), 'SW': (1, 14), 'S': (8, 14), 'SE': (14, 14)}

def _sig(im, c, r, thr=70):
    px = im.crop((c * 16, r * 16, c * 16 + 16, r * 16 + 16)).load()
    def out(x, y):
        n = tot = 0
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                p = px[min(15, max(0, x + dx)), min(15, max(0, y + dy))]
                tot += 1
                if sum(abs(p[i] - GRASS[i]) for i in range(3)) < thr: n += 1
        return n * 2 > tot
    return {k: out(*v) for k, v in PTS.items()}

def canon(s):
    """(N,E,S,W,NE,SE,SW,NW) outside flags; a corner only counts when both adjacent edges are inside."""
    n, e, so, w = s['N'], s['E'], s['S'], s['W']
    return (n, e, so, w, (s['NE'] and not n and not e), (s['SE'] and not so and not e),
            (s['SW'] and not so and not w), (s['NW'] and not n and not w))

# dirt-on-grass block of TilesetFloor, origin (0,7): tiles to harvest (relative col,row), nine-patch first so it wins ties
def build_table():
    im = Image.open(R + "TilesetFloor.png").convert("RGBA")
    N, E, S, W = 0, 1, 2, 3
    def k(*edges): return tuple(i in edges for i in range(4)) + (False,) * 4
    table = {}
    hand = {(0, 0): k(N, W), (1, 0): k(N), (2, 0): k(N, E), (0, 1): k(W), (1, 1): k(), (2, 1): k(E), (0, 2): k(S, W), (1, 2): k(S),
            (2, 2): k(S, E), (3, 0): k(N, E, W), (3, 1): k(E, W), (3, 2): k(E, S, W), (0, 3): k(N, S, W), (1, 3): k(N, S),
            (2, 3): k(N, E, S), (3, 3): k(N, E, S, W)}
    for pos, kk in hand.items(): table.setdefault(kk, pos)
    for r in range(0, 5):                                                    # inner corners / combos, harvested by sampling
        for c in range(4, 10):
            if (c, r) == (9, 4): continue
            table.setdefault(canon(_sig(im, c, 7 + r)), (c, r))
    return table
TABLE = build_table()
ORIGINS = {"dirt": ("Floor", 0, 7), "sand": ("Floor", 0, 0), "snow": ("Floor", 0, 14),
           "water": ("Water", 0, 6), "swamp": ("Water", 13, 6), "ice": ("Water", 13, 0)}

def pick(mask, x, y, W, H):
    """relative (col,row) in the set's block for cell (x,y) of bool mask"""
    def m(dx, dy):
        xx, yy = x + dx, y + dy
        return not (0 <= xx < W and 0 <= yy < H) or mask[yy][xx]       # outside the map counts as inside
    n, e, s, w = not m(0, -1), not m(1, 0), not m(0, 1), not m(-1, 0)
    k = (n, e, s, w, (not m(1, -1)) and not n and not e, (not m(1, 1)) and not s and not e,
         (not m(-1, 1)) and not s and not w, (not m(-1, -1)) and not n and not w)
    if k in TABLE: return TABLE[k]
    best, bd = None, 99
    for kk, v in TABLE.items():
        d = sum(4 for a, b in zip(kk[:4], k[:4]) if a != b) + sum(1 for a, b in zip(kk[4:], k[4:]) if a != b)
        if d < bd: bd, best = d, v
    return best

def smooth(mask, W, H, iters=6, keep=None):
    """morphological clean-up so every cell has a sane tile: kill 1-cell spurs/diagonal-only joins, fill 1-cell notches."""
    m = [row[:] for row in mask]
    def g(a, x, y): return 0 <= x < W and 0 <= y < H and a[y][x]
    for _ in range(iters):
        ch = False
        new = [row[:] for row in m]
        for y in range(H):
            for x in range(W):
                n4 = g(m, x, y - 1) + g(m, x + 1, y) + g(m, x, y + 1) + g(m, x - 1, y)
                if m[y][x]:
                    # spur: <=1 orthogonal neighbour; or checkerboard/diagonal-only joint
                    if n4 <= 1 and not (keep and keep[y][x]): new[y][x] = False; ch = True
                else:
                    if n4 >= 3: new[y][x] = True; ch = True
                    elif n4 == 2 and ((g(m, x, y - 1) and g(m, x, y + 1)) or (g(m, x - 1, y) and g(m, x + 1, y))): new[y][x] = True; ch = True
        # diagonal-only touching cells -> fill one orthogonal bridge (symmetric: fill both)
        for y in range(H):
            for x in range(W):
                if new[y][x]:
                    for dx, dy in ((1, 1), (1, -1)):
                        if g(new, x + dx, y + dy) and not g(new, x + dx, y) and not g(new, x, y + dy):
                            new[y][x + dx] = True; new[y + dy][x] = True; ch = True
        m = new
        if not ch: break
    return m
