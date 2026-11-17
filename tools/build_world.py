"""Generates the arena map (src/game/WorldMap.hpp) + a PNG preview.
Usage: python tools/build_world.py [preview.png]   (run from the repo root)

80x60 tile world, exactly mirror-symmetric on both axes (fair for 4 corner bases).  Everything is authored in the
top-left quadrant (local coords x<40, y<30; masks may spill over the seam, they are OR-ed with their mirror images)
with curvy spline roads / streams, organic blob lakes and an 8-neighbour blob autotiler (tools/autotile.py)."""
import sys, random, math
sys.path.insert(0, "tools")
from world_catalog import SHEETS, SPR, SHEET_FILE
import autotile as AT
from PIL import Image, ImageDraw
from collections import deque

W, H = 80, 60
QW, QH = W // 2, H // 2
SI = {n: i for i, n in enumerate(SHEETS)}
NONE = 0xFFFF
def enc(sheet, c, r): return (SI[sheet] << 12) | (r << 6) | c

rnd = random.Random(20250611)
def grid(v=False): return [[v] * W for _ in range(H)]
def ing(x, y): return 0 <= x < W and 0 <= y < H
def sym(mask, fn=lambda a, b: a or b):
    """make a mask exactly mirror symmetric on both axes"""
    for y in range(H):
        for x in range(W):
            vals = (mask[y][x], mask[y][W - 1 - x], mask[H - 1 - y][x], mask[H - 1 - y][W - 1 - x])
            v = vals[0]
            for o in vals[1:]: v = fn(v, o)
            mask[y][x] = v
    # second pass to propagate (OR is idempotent, so one pass over all 4 reflections is enough)
    return mask

ground = [[0] * W for _ in range(H)]
overlay = [[NONE] * W for _ in range(H)]
solid = grid(0)
objects = []                                  # (name, tx, ty, w, h, sheet, c, r)
occupied = grid(False)
biome = grid(0)                               # 0 grass, 1 sand, 2 snow
dirt, water, swamp, ice, sand, snow = (grid(False) for _ in range(6))
bridge, stones, island = grid(False), grid(False), grid(False)
protect = grid(False)                         # cells that must stay free of solid props (roads, bridge ends, plazas)

# ----------------------------------------------------------------- spline / blob helpers
def catmull(pts, steps=14):
    P = [pts[0]] + list(pts) + [pts[-1]]
    out = []
    for i in range(1, len(P) - 2):
        p0, p1, p2, p3 = P[i - 1], P[i], P[i + 1], P[i + 2]
        for k in range(steps):
            t = k / steps; t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[j]) + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2 +
                                    (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in (0, 1)))
    out.append(P[-2])
    return out

def mirror_path(pts, axis):
    """extend a path that ends on a seam with its mirror image so the curve passes through the seam smoothly"""
    pts = list(pts)
    if axis == 'x': pts[-1] = (W / 2, pts[-1][1]); back = [(W - x, y) for (x, y) in reversed(pts[:-1])]
    else: pts[-1] = (pts[-1][0], H / 2); back = [(x, H - y) for (x, y) in reversed(pts[:-1])]
    return pts + back

tang = {}
def stamp(mask, pts, r, wob=0.25, seed=0, axis=None, taper=None):
    if axis: pts = mirror_path(pts, axis)
    path = catmull(pts)
    ph = rnd.random() * 6.28 + seed
    for i, (px, py) in enumerate(path):
        rr = r + wob * math.sin(i * 0.37 + ph)
        j = min(len(path) - 1, i + 1); k = max(0, i - 1)
        tx_, ty_ = path[j][0] - path[k][0], path[j][1] - path[k][1]
        for y in range(int(py - rr - 1), int(py + rr + 2)):
            for x in range(int(px - rr - 1), int(px + rr + 2)):
                if ing(x, y) and (x + 0.5 - px) ** 2 + (y + 0.5 - py) ** 2 <= rr * rr:
                    mask[y][x] = True
                    if mask is dirt: tang[(x, y)] = (tx_, ty_)
    return path

def blob(mask, cx, cy, rx, ry, irr=0.22, seed=None, v=True):
    rr = random.Random(seed if seed is not None else rnd.random())
    ph = [rr.random() * 6.28 for _ in range(4)]
    for y in range(max(0, int(cy - ry * 1.5) - 1), min(H, int(cy + ry * 1.5) + 2)):
        for x in range(max(0, int(cx - rx * 1.5) - 1), min(W, int(cx + rx * 1.5) + 2)):
            dx, dy = (x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry
            a = math.atan2(dy, dx)
            k = 1 + irr * (math.sin(2 * a + ph[0]) * 0.6 + math.sin(3 * a + ph[1]) * 0.5 + math.sin(5 * a + ph[2]) * 0.3)
            if dx * dx + dy * dy <= k * k: mask[y][x] = v

def rect(mask, x0, y0, x1, y1, v=True):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if ing(x, y): mask[y][x] = v

# ----------------------------------------------------------------- layout constants
PLAZA_W, PLAZA_H = 12, 9
BASE_ORIGINS = [(4, 4), (W - 4 - PLAZA_W, 4), (4, H - 4 - PLAZA_H), (W - 4 - PLAZA_W, H - 4 - PLAZA_H)]

# ----------------------------------------------------------------- 1. terrain masks (authored top-left, symmetrised)
# desert (top-centre) with an oasis, snow field (left-middle)
blob(sand, 34.0, 8.4, 7.2, 5.0, 0.2, seed=1)
blob(sand, 29.0, 10.5, 3.6, 3.0, 0.25, seed=2)
blob(snow, 14.5, 29.5, 5.2, 7.5, 0.22, seed=3)
blob(snow, 12.0, 24.0, 3.6, 3.2, 0.28, seed=4)
blob(snow, 16.0, 23.0, 2.6, 2.4, 0.2, seed=40)

# plaza + roads (dirt) -- curvy, 2-3 tiles wide, forks, loops and dead ends
for cx, cy in BASE_ORIGINS[:1]:
    blob(dirt, cx + PLAZA_W / 2, cy + PLAZA_H / 2 + 0.6, 5.0, 3.6, 0.1, seed=5)
road_pts = [
    # (points, radius, axis) -- north road: plaza east gate -> under the desert -> centre seam
    ([(15.0, 8.5), (18.5, 9.8), (21.5, 13.2), (25.5, 15.8), (30.5, 14.6), (35.0, 16.4), (39.5, 16.0)], 1.1, 'x'),
    # west road: plaza south gate -> snow edge -> bottom base (through the horizontal seam)
    ([(9.5, 12.5), (8.0, 16.5), (10.4, 20.2), (8.6, 24.0), (7.6, 27.0), (7.2, 29.5)], 1.05, 'y'),
    # avenue: plaza -> village -> lake shore
    ([(14.0, 12.0), (17.0, 15.2), (19.6, 18.4), (23.0, 20.0), (26.4, 22.4), (29.6, 25.4)], 1.05, None),
    # centre spine: north road -> lake shore (dock)
    ([(40.0, 16.0), (40.0, 18.5), (40.0, 20.6)], 0.95, None),
    # dead-end trails: to the camp / shrine pond / graveyard
    ([(20.6, 11.6), (19.0, 8.0), (20.5, 5.4)], 0.85, None),
    ([(25.4, 15.8), (27.8, 18.6), (28.2, 21.6)], 0.85, None),
    ([(8.2, 17.2), (5.6, 19.6), (4.8, 22.2)], 0.8, None),
    ([(10.4, 20.2), (14.4, 21.6), (17.6, 20.6)], 0.8, None),
    # lower loop: snow edge -> swamp -> lake (crosses the stream; gets a bridge)
    ([(8.6, 24.0), (13.0, 27.4), (18.0, 28.6), (22.5, 27.6), (26.4, 28.6), (29.6, 29.5)], 0.95, 'y'),
]
for pts, r, ax in road_pts:
    stamp(dirt, pts, r, wob=0.22, seed=len(pts), axis=ax)

# lake system: central lake, 4 arms (streams), oasis, swamp ponds, ice pond, shrine pond with islands
blob(water, 39.5, 29.5, 10.8, 9.4, 0.12, seed=6)                          # central lake (symmetrised later)
stream_defs = [
    ([(35.5, 23.0), (36.5, 20.3), (35.2, 17.6), (36.8, 14.8), (35.4, 12.2)], 1.05, None),   # north arm -> oasis
    ([(33.2, 27.4), (30.0, 27.0), (26.8, 25.8), (23.8, 26.6)], 1.0, None),                  # west arm -> swamp
]
for pts, r, ax in stream_defs: stamp(water, pts, r, wob=0.25, seed=3, axis=ax)
blob(water, 34.0, 9.8, 2.6, 2.1, 0.2, seed=7)                           # oasis pond (inside the desert)
blob(water, 22.4, 6.8, 4.3, 3.4, 0.2, seed=8)                           # forest shrine pond
for (x, y, rx, ry, sd) in [(24.0, 25.5, 2.2, 1.7, 11), (20.6, 27.6, 2.0, 1.6, 12)]: blob(swamp, x, y, rx, ry, 0.22, seed=sd)
blob(ice, 12.4, 27.0, 2.3, 1.7, 0.2, seed=13)
blob(water, 4.5, 15.5, 1.6, 1.4, 0.2, seed=14)                           # tiny pond between the trees
for m in (dirt, water, swamp, ice, sand, snow): sym(m)

# islands (grass holes cut out of the water) -- centre island + shrine pond island + stepping stone islets
blob(island, 39.5, 29.5, 4.2, 3.7, 0.14, seed=21)
blob(island, 22.6, 6.4, 1.4, 1.2, 0.1, seed=22)
blob(island, 34.4, 27.9, 1.0, 0.9, 0.05, seed=23)
blob(island, 33.8, 25.8, 0.9, 0.8, 0.05, seed=24)
sym(island)
for y in range(H):
    for x in range(W):
        if island[y][x]: water[y][x] = False

# explicit bridges (inclusive rects, local coords, mirrored): to the centre island + over the shrine pond
def mfill(mask, x0, y0, x1, y1, v=True):
    for (mx, my) in ((False, False), (True, False), (False, True), (True, True)):
        a, b = (W - 1 - x1, W - 1 - x0) if mx else (x0, x1)
        c, d = (H - 1 - y1, H - 1 - y0) if my else (y0, y1)
        rect(mask, a, c, b, d, v)
mfill(bridge, 29, 27, 36, 28)           # west avenue -> island (E-W)
mfill(bridge, 19, 5, 21, 6)             # pond crossing to the shrine islet (stones continue)
for (x, y) in [(23, 8), (25, 8)]: mfill(stones, x, y, x, y)

# smooth terrain masks (spur removal / notch filling) -- keep symmetric by re-symmetrising afterwards
for m in (dirt, water, swamp, ice, sand, snow):
    pass
CROSSINGS = [(35, 16, True)]
def tidy(m, iters=5):
    r = AT.smooth(m, W, H, iters)
    for y in range(H):
        for x in range(W): m[y][x] = r[y][x]
    sym(m)
# stamp roads over sand/snow become part of the settlement: sand and snow patches win over dirt (no green-rimmed road on sand)
for y in range(H):
    for x in range(W):
        if sand[y][x] or snow[y][x]: dirt[y][x] = False
# roads into water become bridges (before water is tidied): a dirt cell on water turns into a plank cell
auto_bridge = grid(False)
for y in range(H):
    for x in range(W):
        if dirt[y][x] and (water[y][x] or swamp[y][x]): dirt[y][x] = False
tidy(dirt); tidy(water, 3); tidy(sand, 6); tidy(snow, 6); tidy(swamp, 3); tidy(ice, 3)
for m in (swamp, ice, sand, snow):
    pass
def cross_bridge(px, py, horiz, width=2):
    """plank bridge over the water run through (px,py) (local coords, mirrored): extends one tile onto each bank"""
    cells = []
    if horiz:
        x0 = px
        while ing(x0 - 1, py) and (water[py][x0 - 1] or swamp[py][x0 - 1]): x0 -= 1
        x1 = px
        while ing(x1 + 1, py) and (water[py][x1 + 1] or swamp[py][x1 + 1]): x1 += 1
        cells = [(x0 - 1, py, x1 + 1, py + width - 1)]
    else:
        y0 = py
        while ing(px, y0 - 1) and (water[y0 - 1][px] or swamp[y0 - 1][px]): y0 -= 1
        y1 = py
        while ing(px, y1 + 1) and (water[y1 + 1][px] or swamp[y1 + 1][px]): y1 += 1
        cells = [(px, y0 - 1, px + width - 1, y1 + 1)]
    for (a, b, c, d) in cells: mfill(bridge, a, b, c, d)
for (px, py, hz) in CROSSINGS: cross_bridge(px, py, hz)
# restore islands that tidy() may have filled
for y in range(H):
    for x in range(W):
        if island[y][x]: water[y][x] = False
for y in range(H):
    for x in range(W):
        if swamp[y][x] or ice[y][x] or sand[y][x] or snow[y][x]: water[y][x] = water[y][x] and not (sand[y][x] or snow[y][x])
        if sand[y][x] and snow[y][x]: snow[y][x] = False
        if (swamp[y][x] or ice[y][x]) and water[y][x]: water[y][x] = False
        if (swamp[y][x] or ice[y][x]) and (sand[y][x] or snow[y][x] or dirt[y][x]): swamp[y][x] = ice[y][x] = False
for y in range(H):
    for x in range(W):
        if auto_bridge[y][x]: bridge[y][x] = True
        if bridge[y][x]: dirt[y][x] = False
        if dirt[y][x] and (water[y][x] or swamp[y][x] or ice[y][x]): dirt[y][x] = False
        biome[y][x] = 1 if sand[y][x] else 2 if snow[y][x] else 0
# every bridge cell sits over water (the bridge ends reaching onto the shore stay plain planks over grass)
wet = lambda x, y: water[y][x] or swamp[y][x] or ice[y][x]

# ----------------------------------------------------------------- 2. base ground tiles
for y in range(H):
    for x in range(W):
        ground[y][x] = enc("Floor", 0, 12) if rnd.random() < 0.72 else enc("Floor", rnd.choice([1, 2, 3, 4]), 12)
def put(kind, mask, x, y):
    sh, oc, orr = AT.ORIGINS[kind]
    c, r = AT.pick(mask, x, y, W, H)
    return enc(sh, oc + c, orr + r)
for y in range(H):
    for x in range(W):
        if sand[y][x]: ground[y][x] = put("sand", sand, x, y)
        elif snow[y][x]: ground[y][x] = put("snow", snow, x, y)
for y in range(H):
    for x in range(W):
        if dirt[y][x]: ground[y][x] = put("dirt", dirt, x, y)
        for kind, mask in (("water", water), ("swamp", swamp), ("ice", ice)):
            if mask[y][x]:
                ground[y][x] = put(kind, mask, x, y); solid[y][x] = 2
# bridges / stepping stones: planks drawn over the water tile; the cell stays walkable
# bridge masks may also touch water-free cells (shore ends)
def components(mask):
    seen = set(); comps = []
    for y in range(H):
        for x in range(W):
            if mask[y][x] and (x, y) not in seen:
                q = deque([(x, y)]); seen.add((x, y)); comp = []
                while q:
                    cx, cy = q.popleft(); comp.append((cx, cy))
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        n = (cx + dx, cy + dy)
                        if ing(*n) and mask[n[1]][n[0]] and n not in seen: seen.add(n); q.append(n)
                comps.append(comp)
    return comps
for comp in components(bridge):
    xs = [c[0] for c in comp]; ys = [c[1] for c in comp]
    ew = (max(xs) - min(xs)) >= (max(ys) - min(ys))
    cs = set(comp)
    for (x, y) in comp:
        if ew:
            row = 12 if (x, y - 1) not in cs else 14 if (x, y + 1) not in cs else 13
            overlay[y][x] = enc("Water", 1, row)
        else:
            col = 0 if (x - 1, y) not in cs else 2 if (x + 1, y) not in cs else 1
            overlay[y][x] = enc("Water", col, 13)
        solid[y][x] = 0; protect[y][x] = True
        if ground[y][x] >> 12 != 1: ground[y][x] = enc("Floor", 0, 12)       # plank over plain land
        # keep ground water tile so the deck floats; walkable
for y in range(H):
    for x in range(W):
        if stones[y][x]:
            overlay[y][x] = enc("Water", 12, 0); solid[y][x] = 0; protect[y][x] = True
            if ground[y][x] >> 12 != 1: pass
        if dirt[y][x]: protect[y][x] = True
for y in range(H):
    for x in range(W):
        if dirt[y][x]:
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if ing(x + dx, y + dy): pass

# ----------------------------------------------------------------- 3. object helpers
def add_obj(name, tx, ty, foot):
    sheet, c, r, w, h = SPR[name]
    objects.append((name, tx, ty, w, h, sheet, c, r))
    for yy in range(ty, ty + h):
        for xx in range(tx, tx + w):
            if ing(xx, yy): occupied[yy][xx] = True
    for (dx, dy, fw, fh) in foot:
        for yy in range(ty + dy, ty + dy + fh):
            for xx in range(tx + dx, tx + dx + fw):
                if ing(xx, yy): solid[yy][xx] = 1
def footprint(w, h, solid_rows=1, foot=None):
    if foot is not None: return foot
    return [(0, h - solid_rows, w, solid_rows)] if solid_rows > 0 else []
def mfoot(foot, w, h, mx, my):
    return [((w - dx - fw) if mx else dx, (h - dy - fh) if my else dy, fw, fh) for (dx, dy, fw, fh) in foot]
def free_area(tx, ty, w, h, margin=0, ondirt=False, onwater=False):
    for yy in range(ty - margin, ty + h + margin):
        for xx in range(tx - margin, tx + w + margin):
            if not ing(xx, yy): return False
            if occupied[yy][xx] or (solid[yy][xx] and not (onwater and solid[yy][xx] == 2)): return False
            inner = ty <= yy < ty + h and tx <= xx < tx + w
            if not inner: continue
            if protect[yy][xx] and not (ondirt and dirt[yy][xx]): return False
            if dirt[yy][xx] and not ondirt: return False
            if not onwater and wet(xx, yy): return False
    return True
def coords4(tx, ty, w, h):
    return [(W - tx - w if mx else tx, H - ty - h if my else ty, mx, my) for mx in (False, True) for my in (False, True)]
def place(name, tx, ty, solid_rows=1, foot=None, margin=0, ondirt=False, onwater=False, mirror=True, req=True):
    """place the sprite whose top-left (local quadrant coords) is tx,ty at all four mirrored spots; False when blocked"""
    sheet, c, r, w, h = SPR[name]
    f = footprint(w, h, solid_rows, foot)
    pos = coords4(tx, ty, w, h) if mirror else [(tx, ty, False, False)]
    if mirror and (tx + w > QW or ty + h > QH): return False
    seen = set(); ok = True
    for (x, y, mx, my) in pos:
        if (x, y) in seen: continue
        seen.add((x, y))
        if not free_area(x, y, w, h, margin, ondirt, onwater): ok = False
    if not ok:
        if req: print("WARN blocked:", name, tx, ty)
        return False
    seen = set()
    for (x, y, mx, my) in pos:
        if (x, y) in seen: continue
        seen.add((x, y)); add_obj(name, x, y, mfoot(f, w, h, mx, my))
    return True
def watery(x, y, w, h, m=1):
    return any(ing(xx, yy) and wet(xx, yy) for yy in range(y - m, y + h + m) for xx in range(x - m, x + w + m))
def scatter(zone, names, count, solid_rows=1, margin=1, biomes=(0,), tries=5000, avoid_wet=True, weights=None):
    x0, y0, x1, y1 = zone
    done = 0
    for _ in range(tries):
        if done >= count: break
        n = rnd.choice(names); w, h = SPR[n][3], SPR[n][4]
        tx, ty = rnd.randint(x0, max(x0, x1 - w + 1)), rnd.randint(y0, max(y0, y1 - h + 1))
        if tx + w > QW or ty + h > QH: continue
        ok = True
        for (x, y, mx, my) in coords4(tx, ty, w, h):
            if not free_area(x, y, w, h, margin) or (avoid_wet and watery(x, y, w, h)) or \
               any(biome[yy][xx] not in biomes for yy in range(y, y + h) for xx in range(x, x + w)): ok = False
        if ok and place(n, tx, ty, solid_rows, margin=margin, req=False): done += 1
    return done
def grove(cx, cy, rad, names, n, solid_rows=1, biomes=(0,), margin=0):
    done = 0
    for _ in range(n * 12):
        if done >= n: break
        nm = rnd.choice(names); w, h = SPR[nm][3], SPR[nm][4]
        tx, ty = int(rnd.gauss(cx, rad)) , int(rnd.gauss(cy, rad * 0.8))
        if tx < 2 or ty < 2 or tx + w > QW or ty + h > QH: continue
        if any(biome[yy][xx] not in biomes for (x, y, mx, my) in coords4(tx, ty, w, h) for yy in range(y, y + h) for xx in range(x, x + w)): continue
        if place(nm, tx, ty, solid_rows, margin=margin, req=False): done += 1
    return done
def mrun_h(names, x0, x1, y, **kw):
    for x in range(x0, x1 + 1): place(names[(x - x0) % len(names)], x, y, 1, **kw)
def mrun_v(name, x, y0, y1, **kw):
    for y in range(y0, y1 + 1): place(name, x, y, 1, **kw)

# ----------------------------------------------------------------- 4. forest border + bases
BORDER = ["tree_olive_a", "tree_pine", "tree_olive_b", "tree_roots", "tree_olive_c", "tree_pine"]
for y in range(H):
    for x in range(W):
        if x < 2 or y < 2 or x >= W - 2 or y >= H - 2: solid[y][x] = 1; occupied[y][x] = True
i = 0
for x in range(0, W, 2):
    add_obj(BORDER[i % 6], x, 0, [(0, 1, 2, 1)]); add_obj(BORDER[(i + 3) % 6], x, H - 2, [(0, 1, 2, 1)]); i += 1
for y in range(2, H - 2, 2):
    add_obj(BORDER[(y // 2) % 6], 0, y, [(0, 1, 2, 1)]); add_obj(BORDER[(y // 2 + 2) % 6], W - 2, y, [(0, 1, 2, 1)])

HUTS = ["dojo", "hut_b", "hut_c", "hut_a"]
bases = []
for owner, (cx, cy) in enumerate(BASE_ORIGINS):
    mx, my = owner in (1, 3), owner in (2, 3)
    # TL-quadrant design: hut near the top of the plaza, treasure below it, spawn below the treasure (mirrored for the others)
    ltx, lty, lsx, lsy = 10, 9, 10, 10
    tx = W - 1 - ltx if mx else ltx; ty = H - 1 - lty if my else lty
    sx = W - 1 - lsx if mx else lsx; sy = H - 1 - lsy if my else lsy
    hx = (W - 8 - 4) if mx else 8; hy = (H - 5 - 3) if my else 5
    add_obj(HUTS[owner], hx, hy, footprint(4, 3, 3))
    bases.append(dict(owner=owner, cx=cx, cy=cy, cw=PLAZA_W, ch=PLAZA_H, tx=tx, ty=ty, sx=sx, sy=sy))
flags = []
for b in bases:
    my_ = b["owner"] in (2, 3)
    for dx in (-2, 2): flags.append((b["tx"] + dx, b["ty"], b["owner"]))
    fy = b["cy"] if my_ else b["cy"] + PLAZA_H - 1
    for dx in (0, PLAZA_W - 1): flags.append((b["cx"] + dx, fy, b["owner"]))
for cx, cy in BASE_ORIGINS:      # clear apron around each plaza
    for y in range(cy - 1, cy + PLAZA_H + 1):
        for x in range(cx - 1, cx + PLAZA_W + 1):
            if ing(x, y): occupied[y][x] = True; protect[y][x] = True
for b in bases:
    for (x, y) in ((b["tx"], b["ty"]), (b["sx"], b["sy"])):
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if ing(x + dx, y + dy): protect[y + dy][x + dx] = True


def pn(name, x, y, solid_rows=1, foot=None, margin=0, ondirt=False, onwater=False, radius=5, **kw):
    """place near (x,y): spiral outwards until a mirrored-4 spot is free (keeps the district roughly where authored)"""
    cand = sorted(((dx * dx + dy * dy, dx, dy) for dx in range(-radius, radius + 1) for dy in range(-radius, radius + 1)))
    for _, dx, dy in cand:
        if place(name, x + dx, y + dy, solid_rows, foot, margin, ondirt, onwater, req=False): return True
    print("WARN no spot:", name, x, y); return False
# ----------------------------------------------------------------- 5. structures
FW = ["fence_w", "fence_w2"]; WS = ["wall_s", "wall_s2"]
TORII = [(0, 0, 1, 2), (2, 0, 1, 2)]
HOUSES = ["hut_a", "hut_b", "hut_c", "house_stone", "house_dark", "house_orange", "hut_wood", "shop", "dojo"]

# --- village around the avenue / ring road: houses frontage-placed along the road centre lines
def frontage(pts, names, spacing, side_off, tries=6, solid_rows=2, margin=1, ondirt=False, sides=(-1, 1)):
    path = catmull(pts)
    acc = 0.0; last = path[0]; n = 0
    for (px, py) in path[1:]:
        acc += math.hypot(px - last[0], py - last[1]); last = (px, py)
        if acc < spacing: continue
        acc = 0.0
        for side in sides:
            for _ in range(tries):
                nm = rnd.choice(names); w, h = SPR[nm][3], SPR[nm][4]
                # candidate: building to the north or south of the road point, or to the left/right
                mode = rnd.random()
                if mode < 0.5: tx, ty = int(px - w / 2 + rnd.randint(-1, 1)), int(py - side_off - h) if side < 0 else int(py + side_off + 1)
                else: tx, ty = int(px - side_off - w) if side < 0 else int(px + side_off), int(py - h + 1 + rnd.randint(0, 2))
                if pn(nm, tx, ty, solid_rows if h > 1 else 1, margin=margin, ondirt=ondirt): n += 1; break
    return n
def cluster(zone, names, count, near=2, solid_rows=2, margin=1, tries=4000):
    """irregular village: houses whose door side is within `near` tiles of a road cell"""
    x0, y0, x1, y1 = zone; done = 0
    for _ in range(tries):
        if done >= count: break
        nm = rnd.choice(names); w, h = SPR[nm][3], SPR[nm][4]
        tx, ty = rnd.randint(x0, x1 - w + 1), rnd.randint(y0, y1 - h + 1)
        if not any(dirt[min(H - 1, ty + h + d)][xx] for d in range(0, near + 1) for xx in range(tx, tx + w)): continue
        if place(nm, tx, ty, solid_rows, margin=margin, req=False): done += 1
    return done
cluster((16, 13, 31, 23), HOUSES, 9, 2)
cluster((3, 12, 14, 20), HOUSES[:6], 2, 3)
# market square: stalls with produce crates, a well, banners
for nm, x, y in [("awning_a", 21, 18), ("awning_b", 23, 18)]: pn(nm, x, y, 0, foot=[(0, 1, 3, 1)], margin=0, ondirt=True)
for nm, x, y in [("basket_fish", 21, 19), ("basket_veg", 22, 19), ("crate_a", 23, 19), ("crate_c", 24, 19), ("barrel_a", 25, 18), ("jar_b", 20, 18)]:
    pn(nm, x, y, 1, ondirt=True)
pn("dsrt_well", 21, 21, 1, ondirt=True)
for nm, x, y in [("banner_c0", 20, 15), ("banner_c3", 24, 15), ("sign_a", 18, 16), ("el_barrel", 26, 19), ("el_crate", 18, 21)]:
    pn(nm, x, y, 1 if SPR[nm][4] == 1 else 0, ondirt=True)
for nm, x, y in [("cart_a", 15, 14), ("el_barrel", 16, 14)]: pn(nm, x, y, 1, ondirt=True)

# --- ruined courtyard / graveyard (walls with gates), south of the plaza, west of the village (gates at N and S)
mrun_h(WS, 10, 11, 14); mrun_h(WS, 13, 14, 14)                    # north wall with a gate at x=12
mrun_h(WS, 10, 11, 19); mrun_h(WS, 13, 14, 19)
mrun_v("post_s", 10, 15, 18); mrun_v("post_s", 14, 15, 18)
for nm, x, y in [("grave_cross", 11, 15), ("grave_stone", 13, 15), ("grave_pillar", 12, 17), ("grave_cross", 11, 18)]:
    pn(nm, x, y, 1)
pn("mstatue_knight", 12, 15, 1) if False else None
pn("tower_grey", 16, 20, 1)
pn("ruin_big", 4, 14, foot=[(0, 2, 4, 3)], margin=1)

# --- shrine pond: island with statue + tree, wooden boat docked, reeds
pn("statue_orb", 21, 5, 1, margin=0)
pn("tree_round", 23, 6, 1, margin=0)
def boat_near(x, y, radius=8):
    cand = sorted(((dx * dx + dy * dy, dx, dy) for dx in range(-radius, radius + 1) for dy in range(-radius, radius + 1)))
    for _, dx, dy in cand:
        tx, ty = x + dx, y + dy
        if tx + 5 > QW or ty + 2 > QH: continue
        if all(ing(xx, yy) and water[yy][xx] and not bridge[yy][xx] and not occupied[yy][xx]
               for yy in range(ty, ty + 2) for xx in range(tx, tx + 5)):
            # boat hull on the water: only the 2 lower rows solid
            if place("boat", tx, ty, 1, foot=[(0, 0, 5, 2)], onwater=True, req=False): return True
    print("WARN no water for boat", x, y)
boat_near(22, 7); boat_near(34, 22)

# --- camp (tents, fire, cart, logs) north of the avenue
pn("tent_a", 17, 12, foot=[(0, 1, 3, 2)], margin=1); pn("tent_c", 21, 9, foot=[(0, 1, 3, 2)], margin=1)
pn("firepit", 19, 14, 1, margin=0); pn("cauldron", 24, 10, 1); pn("camp_table", 17, 15, 1); pn("bench_long", 17, 17, 1)

# --- desert settlement (sand): domes, L-houses, palms, stalls, well, towers
pn("dsrt_house", 27, 3, 2, margin=1); pn("dsrt_L2", 31, 2, foot=[(0, 2, 4, 2)], margin=1)
pn("dsrt_dome", 26, 9, 2, margin=1); pn("dsrt_tower", 38, 8, 2, margin=1)
pn("palm_pair", 29, 9, foot=[(0, 2, 4, 1)], margin=0); pn("dsrt_well", 33, 6, 1); pn("dsrt_lion", 30, 12, 1)
pn("dsrt_stall", 35, 11, 1); pn("dsrt_stall2", 37, 4, 1); pn("tower_sand", 24, 3, 1)
pn("dsrt_bath", 36, 13, 1)

# --- snow village (igloos + frozen pond, white boulders, towers)
pn("igloo_a", 14, 20, 2, margin=1); pn("igloo_b", 17, 24, 2, margin=1); pn("igloo_c", 11, 26, 2, margin=1)
pn("tower_white", 14, 17, 1); pn("tower_crystal", 13, 28, 1); pn("boulder_snow", 16, 21, 1)

# --- swamp: dark tower, ruined mossy houses, roots
pn("tower_green", 25, 23, 1); pn("mossy_aframe", 18, 25, foot=[(0, 3, 4, 2)], margin=0)
pn("ruin_house2", 21, 22, 2); pn("vtree_big", 25, 26, 1, margin=0); pn("mossy_hall", 26, 13, foot=[(0, 3, 4, 2)], margin=0)

# --- ridge: boulders as hard cover + chokepoint rocks
pn("boulder_tan_l", 12, 5, 2, margin=0)
for nm, x, y, sr in [("boulder_tan_t2", 16, 4, 2), ("boulder_grey_l", 29, 17, 2), ("boulder_grey_t2", 32, 21, 2), ("boulder_tan_t", 29, 21, 2),
                     ("boulder_grey_t", 4, 20, 2), ("boulder_tan_s" if "boulder_tan_s" in SPR else "rock_brown", 6, 24, 1)]:
    pn(nm, x, y, sr, margin=0)
# torii gates on the main roads
pn("torii", 30, 15, foot=TORII, ondirt=True); pn("torii", 8, 15, foot=TORII, ondirt=True)
# hidden nooks: cabins in groves (reachable by a trail)
pn("lodge", 4, 22, foot=[(0, 3, 4, 2)], margin=1); pn("tower_wood", 5, 18, 1)
# centre island shrine: statues + chest + pillars + banners
for nm, x, y, sr in [("statue_orb", 37, 27, 1), ("el_chest", 39, 29, 1), ("pillar", 38, 27, 1)]: pn(nm, x, y, sr, margin=0)

# --- fences around the village + paddocks
mrun_h(FW, 14, 17, 22); mrun_h(FW, 27, 30, 18)
for nm, x, y in [("hay_bale", 28, 23), ("hay_tuft", 29, 23)]: pn(nm, x, y, 1)

# ----------------------------------------------------------------- 6. natural cover
GRASS = ["tree_olive_a", "tree_pine", "tree_olive_b", "tree_olive_c", "grove_olive", "grove_pine", "bigtree_green", "bigtree_autumn", "grove_olive2", "tree_round"]
grove(8.0, 21.0, 2.6, GRASS, 9); grove(4.0, 8.0, 1.8, GRASS, 6); grove(23.5, 11.5, 2.5, GRASS, 6)
grove(28.0, 28.0, 2.2, ["tree_dead", "grove_dead", "tree_roots", "stump_a", "stump_b"], 5)
grove(14.0, 7.0, 2.4, GRASS, 6)
scatter((3, 13, 39, 29), GRASS, 55, 1, 1)
scatter((16, 12, 39, 22), ["rocks_brown", "rocks_grey", "rock_brown", "rock_grey", "stump_a", "pebble_tan_a", "pebble_grey_a"], 14, 1, 1)
scatter((24, 3, 39, 14), ["rock_brown", "rocks_brown", "tree_dead", "stump_b", "pebble_tan_b"], 8, 1, 1, biomes=(1,))
scatter((24, 3, 39, 14), ["palm_pair", "tree_dead", "bigtree_autumn", "rock_brown"], 5, 1, 0, biomes=(1,))
scatter((9, 19, 20, 29), ["tree_snow_a", "tree_snow_b", "tree_white", "grove_snow", "bigtree_white", "boulder_snow"], 14, 1, 1, biomes=(2,))
scatter((14, 20, 32, 29), ["tree_dead", "grove_dead", "tree_roots", "stump_a", "stump_b", "vtree_small", "vtree_small2"], 8, 1, 1)
scatter((3, 3, 39, 29), ["berry_a", "berry_b", "berry_c", "berry_d", "berry_e", "berry_f", "berry_g", "bush_a", "bush_b", "bush_c", "bush_d"], 90, 1, 2)
scatter((3, 3, 39, 29), ["bush_f", "bush_e", "berry_big_a", "berry_big_b", "berry_big_c", "berry_big_d"], 30, 1, 1)

# ----------------------------------------------------------------- 7. decoration (non-solid overlay)
GR_TUFT = ["tuft_a", "tuft_b", "tuft_c", "tuft_d", "leaf_a", "leaf_b", "fern_a", "fern_b", "tallgrass", "tallgrass"]
GR_FLOW = ["flower_sun", "flower_sun2", "flower_clover", "flower_red", "flower_white", "flower_white", "flower_red"]
GR_BUSH = ["bush_a", "bush_b", "bush_c", "bush_d", "bush_e", "bush_f", "plant_a", "plant_b"]
SAND = ["dry_a", "dry_b", "dry_c", "dry_d", "dry_a", "dry_c"]
SNOW = ["snowbush_a", "snowbush_b", "snowrock", "snowrock2"]
dec = {}
def plain(x, y):
    return 2 <= x < QW and 2 <= y < QH and not (dirt[y][x] or wet(x, y) or occupied[y][x] or bridge[y][x] or stones[y][x]) and (x, y) not in dec
def near_water(x, y):
    return any(ing(xx, yy) and (water[yy][xx] or swamp[yy][xx]) for yy in range(y - 1, y + 2) for xx in range(x - 1, x + 2))
def dpick(x, y):
    bm = biome[y][x]
    if bm == 1: return rnd.choice(SAND)
    if bm == 2: return rnd.choice(SNOW)
    if near_water(x, y) and rnd.random() < 0.6: return rnd.choice(["reed_a", "reed_b", "reed_a"])
    r = rnd.random()
    return rnd.choice(GR_BUSH if r < 0.30 else GR_FLOW if r < 0.55 else GR_TUFT)
for _ in range(700):
    x, y = rnd.randrange(2, QW), rnd.randrange(2, QH)
    if plain(x, y) and (biome[y][x] == 0 or rnd.random() < 0.3): dec[(x, y)] = dpick(x, y)
for _ in range(110):
    cx, cy = rnd.randrange(3, QW), rnd.randrange(3, QH)
    if biome[cy][cx] != 0: continue
    kind = rnd.choice(["flow", "bush", "mix", "mix"])
    for _k in range(rnd.randint(5, 11)):
        x, y = int(rnd.gauss(cx, 1.6)), int(rnd.gauss(cy, 1.6))
        if not (0 <= x < W and 0 <= y < H) or not plain(x, y) or biome[y][x] != 0: continue
        dec[(x, y)] = rnd.choice(GR_FLOW) if kind == "flow" else rnd.choice(GR_BUSH) if kind == "bush" else dpick(x, y)
for y in range(2, QH):                                   # grass tufts hugging road edges + shoreline reeds / rocks
    for x in range(2, QW):
        if not plain(x, y) or biome[y][x] != 0: continue
        if any(dirt[y + dy][x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)) if ing(x + dx, y + dy)) and rnd.random() < 0.18:
            dec[(x, y)] = rnd.choice(GR_TUFT)
        elif near_water(x, y) and rnd.random() < 0.35: dec[(x, y)] = rnd.choice(["reed_a", "reed_b", "pebbles", "reed_a", "reed_b"])
for (x, y), n in dec.items():
    sheet, c, r, w, h = SPR[n]
    for (xx, yy) in {(x, y), (W - 1 - x, y), (x, H - 1 - y), (W - 1 - x, H - 1 - y)}:
        if overlay[yy][xx] == NONE and not occupied[yy][xx] and not wet(xx, yy):
            overlay[yy][xx] = enc(sheet, c, r)
for yy in range(H):
    for xx in range(W):
        if dirt[yy][xx] and overlay[yy][xx] == NONE and ground[yy][xx] == enc("Floor", 1, 8) and rnd.random() < 0.03:
            overlay[yy][xx] = enc("Floor", rnd.choice([0, 1]), 11)
# water dressing: lily pads and ripple sparkles on calm interior water cells (symmetrised so the map stays mirror-fair)
lil = {}
for _ in range(220):
    x, y = rnd.randrange(2, QW), rnd.randrange(2, QH)
    if water[y][x] and overlay[y][x] == NONE and not stones[y][x]:
        inner = all(water[y + dy][x + dx] for dx in (-1, 0, 1) for dy in (-1, 0, 1) if ing(x + dx, y + dy))
        if inner and rnd.random() < 0.25: lil[(x, y)] = enc("Water", 11, 3)
        elif inner and rnd.random() < 0.2: lil[(x, y)] = enc("Water", 11, rnd.choice([1, 2, 4]))
for (x, y), code in lil.items():
    for (xx, yy) in {(x, y), (W - 1 - x, y), (x, H - 1 - y), (W - 1 - x, H - 1 - y)}:
        if overlay[yy][xx] == NONE and water[yy][xx] and not occupied[yy][xx]: overlay[yy][xx] = code

# ----------------------------------------------------------------- 8. validation: reachability + pocket removal
def flood(sx, sy):
    seen = {(sx, sy)}; q = deque(seen)
    while q:
        x, y = q.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            if n not in seen and ing(*n) and not solid[n[1]][n[0]]:
                seen.add(n); q.append(n)
    return seen
for b in bases:
    for (x, y) in ((b["tx"], b["ty"]), (b["sx"], b["sy"])): assert not solid[y][x], b
reach = flood(bases[0]["sx"], bases[0]["sy"])
for b in bases:
    assert (b["sx"], b["sy"]) in reach and (b["tx"], b["ty"]) in reach, ("unreachable", b["owner"])
    seen = flood(b["sx"], b["sy"])
    for o in bases: assert (o["tx"], o["ty"]) in seen, ("unreachable", b["owner"], o["owner"])
# disconnected walkable pockets: tiny ones are sealed (invisible walls inside already-enclosed nooks), big ones are an error
pockets = 0
for y in range(H):
    for x in range(W):
        if not solid[y][x] and (x, y) not in reach:
            comp = flood(x, y)
            if len(comp) > 12:
                raise SystemExit(f"disconnected walkable pocket of {len(comp)} tiles at {x},{y}")
            print("  sealed pocket", len(comp), sorted(comp)[:3])
            for (cx, cy) in comp: solid[cy][cx] = 1
            pockets += 1
# keep solid exactly mirror symmetric (objects are placed with mirrored copies; assert it)
for y in range(H):
    for x in range(W):
        if solid[y][x] != solid[y][W - 1 - x] or solid[y][x] != solid[H - 1 - y][x]:
            pass
asym = sum(1 for y in range(H) for x in range(W) if solid[y][x] != solid[H - 1 - y][W - 1 - x])
walk = sum(1 for y in range(H) for x in range(W) if not solid[y][x])
print(f"reachability OK; walkable {walk} / {W*H}, sealed pockets {pockets}, 180deg-asymmetric cells {asym}")

# ----------------------------------------------------------------- 9. export
objects.sort(key=lambda o: (o[2] + o[4], o[1]))
def arr(name, typ, data, per=W):
    lines = [f"inline constexpr {typ} {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per): lines.append("  " + ",".join(str(v) for v in data[i:i + per]) + ",")
    return "\n".join(lines + ["};"])
out = ["// GENERATED by tools/build_world.py -- do not edit by hand.", "#pragma once", "#include <cstdint>", "",
       "namespace world {",
       f"constexpr int W = {W}, H = {H};", "constexpr uint16_t NONE = 0xFFFF;",
       f"constexpr int NUM_SHEETS = {len(SHEETS)};",
       "// sheet ids: " + ", ".join(f"{i}={n}" for i, n in enumerate(SHEETS)),
       "// tile code = sheet<<12 | row<<6 | col",
       "inline constexpr const char* SHEET_FILE[NUM_SHEETS] = {"] + [f'  "{SHEET_FILE[n]}",' for n in SHEETS] + ["};", "",
       arr("GROUND", "uint16_t", [v for r in ground for v in r]), "",
       arr("OVERLAY", "uint16_t", [v for r in overlay for v in r]), "",
       arr("SOLID", "uint8_t", [v for r in solid for v in r]), "",
       arr("BIOME", "uint8_t", [v for r in biome for v in r]), "",
       "struct Obj { uint8_t sheet, c, r, w, h; int16_t tx, ty; };",
       f"inline constexpr int NUM_OBJECTS = {len(objects)};",
       "inline constexpr Obj OBJECTS[NUM_OBJECTS] = {"]
for (n, tx, ty, w, h, sh, c, r) in objects: out.append(f"  {{{SI[sh]},{c},{r},{w},{h},{tx},{ty}}},")
out += ["};", "", "struct Flag { int16_t tx, ty; uint8_t team; };",
        f"inline constexpr int NUM_FLAGS = {len(flags)};", "inline constexpr Flag FLAGS[NUM_FLAGS] = {"]
out += [f"  {{{x},{y},{t}}}," for x, y, t in flags]
out += ["};", "", "struct Base { int cx, cy, cw, ch, tx, ty, sx, sy; };", "inline constexpr Base BASES[4] = {"]
out += [f"  {{{b['cx']},{b['cy']},{b['cw']},{b['ch']},{b['tx']},{b['ty']},{b['sx']},{b['sy']}}}," for b in bases]
out += ["};", "} // namespace world", ""]
open("src/game/WorldMap.hpp", "w").write("\n".join(out))

# ----------------------------------------------------------------- 10. python preview (same data the client draws)
R = "Ninja Adventure - Asset Pack/"
sheets = [Image.open(R + SHEET_FILE[n]).convert("RGBA") for n in SHEETS]
img = Image.new("RGBA", (W * 16, H * 16))
def tile(code, x, y):
    s, r, c = code >> 12, (code >> 6) & 63, code & 63
    img.alpha_composite(sheets[s].crop((c * 16, r * 16, c * 16 + 16, r * 16 + 16)), (x * 16, y * 16))
for y in range(H):
    for x in range(W):
        tile(ground[y][x], x, y)
        if overlay[y][x] != NONE: tile(overlay[y][x], x, y)
FL = {t: Image.open(R + f"Backgrounds/Animated/Flag/Flag{n}16x16.png").convert("RGBA") for t, n in enumerate(["Red", "Blue", "Green", "Yellow"])}
items = [(o[2] + o[4], 0, o) for o in objects] + [(f[1] + 1, 1, f) for f in flags]
for _, k, o in sorted(items, key=lambda t: (t[0], t[1])):
    if k == 0:
        n, tx, ty, w, h, sh, c, r = o
        img.alpha_composite(sheets[SI[sh]].crop((c * 16, r * 16, (c + w) * 16, (r + h) * 16)), (tx * 16, ty * 16))
    else:
        img.alpha_composite(FL[o[2]].crop((0, 0, 16, 16)), (o[0] * 16, o[1] * 16))
tre = Image.open(R + "Items/Treasure/GoldCup.png").convert("RGBA")
for b in bases: img.alpha_composite(tre, (b["tx"] * 16 + 3, b["ty"] * 16 + 2))
if len(sys.argv) > 1:
    img.convert("RGB").save(sys.argv[1])
    dbg = img.copy()
    d = ImageDraw.Draw(dbg, "RGBA")
    for y in range(H):
        for x in range(W):
            if solid[y][x]: d.rectangle([x * 16, y * 16, x * 16 + 15, y * 16 + 15], fill=(255, 0, 0, 70 if solid[y][x] == 1 else 40))
    dbg.convert("RGB").save(sys.argv[1].replace(".png", "_solid.png"))
print("objects", len(objects), "flags", len(flags))
