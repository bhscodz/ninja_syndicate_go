"""Generates the arena map (src/game/WorldMap.hpp) + a PNG preview.
Usage: python tools/build_world.py [preview.png]   (run from the repo root)
128x96 tile world, mirror-symmetric on both axes. Everything is authored in the
top-left quadrant (local coords x<64, y<48) and mirrored by mfill()/mp()."""
import sys, random
sys.path.insert(0, "tools")
from world_catalog import SHEETS, SPR, SHEET_FILE
from PIL import Image, ImageDraw
from collections import deque

W, H = 128, 96
SI = {n: i for i, n in enumerate(SHEETS)}
NONE = 0xFFFF
def enc(sheet, c, r): return (SI[sheet] << 12) | (r << 6) | c

ground = [[0] * W for _ in range(H)]
overlay = [[NONE] * W for _ in range(H)]
solid = [[0] * W for _ in range(H)]      # 1 = tree/rock/building, 2 = water
objects = []                               # (name, tx, ty, w, h, sheet, c, r)
rnd = random.Random(11)

def mirrors(x0, y0, x1, y1):
    """all 4 mirrored copies of an inclusive tile rect"""
    out = []
    for mx in (False, True):
        for my in (False, True):
            a, b = (W - 1 - x1, W - 1 - x0) if mx else (x0, x1)
            c, d = (H - 1 - y1, H - 1 - y0) if my else (y0, y1)
            out.append((a, c, b, d))
    return out
def fill(mask, x0, y0, x1, y1, v=True):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < W and 0 <= y < H: mask[y][x] = v
def mfill(mask, x0, y0, x1, y1, v=True):
    for r in mirrors(x0, y0, x1, y1): fill(mask, *r, v)

biome = [[0] * W for _ in range(H)]       # 0 grass, 1 sand, 2 snow
dirt = [[False] * W for _ in range(H)]
water = [[False] * W for _ in range(H)]   # blue lake
swamp = [[False] * W for _ in range(H)]   # purple ponds
ice = [[False] * W for _ in range(H)]     # frozen ponds
island = [[False] * W for _ in range(H)]
for y in range(H):
    for x in range(W):
        ground[y][x] = enc("Floor", 0, 12) if rnd.random() < 0.72 else enc("Floor", rnd.choice([1, 2, 3, 4]), 12)

PLAZA_W, PLAZA_H = 14, 10
BASE_ORIGINS = [(4, 4), (W - 4 - PLAZA_W, 4), (4, H - 4 - PLAZA_H), (W - 4 - PLAZA_W, H - 4 - PLAZA_H)]
for cx, cy in BASE_ORIGINS: fill(dirt, cx, cy, cx + PLAZA_W - 1, cy + PLAZA_H - 1)

# ---- biomes (local quadrant coordinates) --------------------------------
mfill(biome, 48, 12, 61, 28, 1)           # desert (joins its mirror image at the top/bottom centre)
mfill(biome, 47, 16, 47, 24, 1)
mfill(biome, 14, 35, 32, 47, 2)           # snow field
mfill(biome, 15, 33, 31, 34, 2)
# ---- roads ----------------------------------------------------------------
mfill(dirt, 18, 8, 63, 10)                # top road
mfill(dirt, 10, 14, 12, 47)               # left road
mfill(dirt, 62, 11, 63, 33)               # north approach to the lake (cuts the desert)
mfill(dirt, 13, 46, 45, 47)               # west approach to the lake (cuts snow + swamp)
mfill(dirt, 13, 28, 61, 29)               # village street
mfill(dirt, 14, 11, 17, 13)               # links to the plaza
mfill(dirt, 30, 11, 31, 23)               # north-south trail through the ruins
# ---- lake + island --------------------------------------------------------
mfill(water, 46, 34, 63, 47)
mfill(island, 58, 42, 63, 47)
for y in range(H):
    for x in range(W):
        if island[y][x]: water[y][x] = False; dirt[y][x] = True
bridge = [[False] * W for _ in range(H)]
mfill(bridge, 46, 46, 57, 47); mfill(bridge, 62, 34, 63, 41)
for y in range(H):
    for x in range(W):
        if bridge[y][x]: water[y][x] = False
for (x0, y0, w, h) in [(34, 36, 6, 4), (41, 38, 4, 4), (35, 42, 5, 3), (41, 43, 3, 2)]:
    mfill(swamp, x0, y0, x0 + w - 1, y0 + h - 1)
for (x0, y0, w, h) in [(25, 40, 5, 4)]:
    mfill(ice, x0, y0, x0 + w - 1, y0 + h - 1)
for (x0, y0, w, h) in [(4, 37, 4, 3), (4, 24, 3, 3)]:
    mfill(water, x0, y0, x0 + w - 1, y0 + h - 1)

def autotile9(mask, x, y, origin):
    def m(dx, dy):
        xx, yy = x + dx, y + dy
        return 0 <= xx < W and 0 <= yy < H and mask[yy][xx]
    n, s, w, e = not m(0, -1), not m(0, 1), not m(-1, 0), not m(1, 0)
    col = 0 if w else (2 if e else 1)
    row = 0 if n else (2 if s else 1)
    return origin[0] + col, origin[1] + row

sandm = [[biome[y][x] == 1 for x in range(W)] for y in range(H)]
snowm = [[biome[y][x] == 2 for x in range(W)] for y in range(H)]
for y in range(H):
    for x in range(W):
        if sandm[y][x]:
            c, r = autotile9(sandm, x, y, (0, 0)); ground[y][x] = enc("Floor", c, r)
        elif snowm[y][x]:
            c, r = autotile9(snowm, x, y, (0, 14)); ground[y][x] = enc("Floor", c, r)
for y in range(H):
    for x in range(W):
        if dirt[y][x]:
            c, r = autotile9(dirt, x, y, (0, 7)); ground[y][x] = enc("Floor", c, r)
        for mask, origin in ((water, (0, 6)), (swamp, (13, 6)), (ice, (13, 0))):
            if mask[y][x]:
                c, r = autotile9(mask, x, y, origin)
                ground[y][x] = enc("Water", c, r); solid[y][x] = 2
for y in range(H):
    for x in range(W):
        if bridge[y][x]:
            horiz = (46 <= y <= 49)
            overlay[y][x] = enc("Water", 5 + ((x if horiz else y) % 2), 13)

# ---- objects ----------------------------------------------------------------
occupied = [[False] * W for _ in range(H)]
def add_obj(name, tx, ty, foot):
    sheet, c, r, w, h = SPR[name]
    objects.append((name, tx, ty, w, h, sheet, c, r))
    for yy in range(ty, ty + h):
        for xx in range(tx, tx + w):
            if 0 <= xx < W and 0 <= yy < H: occupied[yy][xx] = True
    for (dx, dy, fw, fh) in foot:
        for yy in range(ty + dy, ty + dy + fh):
            for xx in range(tx + dx, tx + dx + fw):
                if 0 <= xx < W and 0 <= yy < H: solid[yy][xx] = 1
def footprint(w, h, solid_rows=1, foot=None):
    """default footprint = bottom `solid_rows` rows; or explicit list of (dx,dy,w,h) rects"""
    if foot is not None: return foot
    return [(0, h - solid_rows, w, solid_rows)] if solid_rows > 0 else []
def mfoot(foot, w, h, mx, my):
    return [((w - dx - fw) if mx else dx, (h - dy - fh) if my else dy, fw, fh) for (dx, dy, fw, fh) in foot]

def free_area(tx, ty, w, h, margin=0):
    for yy in range(ty - margin, ty + h + margin):
        for xx in range(tx - margin, tx + w + margin):
            if not (0 <= xx < W and 0 <= yy < H): return False
            if occupied[yy][xx] or solid[yy][xx] or dirt[yy][xx] or bridge[yy][xx]: return False
    return True
def mp(name, tx, ty, solid_rows=1, foot=None):
    """place at all four mirrored positions (tx,ty = local top-left)"""
    sheet, c, r, w, h = SPR[name]
    assert tx + w <= W // 2 and ty + h <= H // 2, (name, tx, ty)
    f = footprint(w, h, solid_rows, foot)
    for mx in (False, True):
        for my in (False, True):
            x = W - tx - w if mx else tx
            y = H - ty - h if my else ty
            if not free_area(x, y, w, h): print("WARN overlap", name, x, y)
            add_obj(name, x, y, mfoot(f, w, h, mx, my))
def watery(x, y, w, h):
    return any(water[yy][xx] or swamp[yy][xx] or ice[yy][xx] for yy in range(y - 1, y + h + 1)
               for xx in range(x - 1, x + w + 1) if 0 <= xx < W and 0 <= yy < H)
def scatter(zone, names, count, solid_rows=1, margin=1, biomes=(0,), tries=4000):
    x0, y0, x1, y1 = zone
    done = 0
    for _ in range(tries):
        if done >= count: break
        n = rnd.choice(names); w, h = SPR[n][3], SPR[n][4]
        tx, ty = rnd.randint(x0, x1 - w + 1), rnd.randint(y0, y1 - h + 1)
        if tx + w > W // 2 or ty + h > H // 2: continue
        ok = True
        for mx in (False, True):
            for my in (False, True):
                x = W - tx - w if mx else tx; y = H - ty - h if my else ty
                if not free_area(x, y, w, h, margin) or watery(x, y, w, h) or \
                   any(biome[yy][xx] not in biomes for yy in range(y, y + h) for xx in range(x, x + w)):
                    ok = False
        if ok:
            mp(n, tx, ty, solid_rows); done += 1
    return done
def mrun_h(names, x0, x1, y):
    for x in range(x0, x1 + 1): mp(names[(x - x0) % len(names)], x, y, 1)
def mrun_v(name, x, y0, y1):
    for y in range(y0, y1 + 1): mp(name, x, y, 1)

# border forest: two tile thick wall of trees all around
BORDER = ["tree_olive_a", "tree_pine", "tree_olive_b", "tree_roots", "tree_olive_c", "tree_pine"]
for y in range(H):
    for x in range(W):
        if x < 2 or y < 2 or x >= W - 2 or y >= H - 2: solid[y][x] = 1
i = 0
for x in range(0, W, 2):
    add_obj(BORDER[i % 6], x, 0, [(0, 1, 2, 1)]); add_obj(BORDER[(i + 3) % 6], x, H - 2, [(0, 1, 2, 1)]); i += 1
for y in range(2, H - 2, 2):
    add_obj(BORDER[(y // 2) % 6], 0, y, [(0, 1, 2, 1)]); add_obj(BORDER[(y // 2 + 2) % 6], W - 2, y, [(0, 1, 2, 1)])

# four bases: hut at the top of the plaza, flags around the treasure
HUTS = ["dojo", "hut_b", "hut_c", "hut_a"]
bases = []
for owner, (cx, cy) in enumerate(BASE_ORIGINS):
    add_obj(HUTS[owner], cx + 5, cy + 1, footprint(4, 3, 3))
    tx, ty = cx + 6, cy + 6
    bases.append(dict(owner=owner, cx=cx, cy=cy, cw=PLAZA_W, ch=PLAZA_H, tx=tx, ty=ty, sx=cx + 6, sy=cy + 8))
flags = []
for b in bases:
    for dx in (-2, 3): flags.append((b["tx"] + dx, b["ty"], b["owner"]))
    for dx in (0, 13): flags.append((b["cx"] + dx, b["cy"] + 9, b["owner"]))
for cx, cy in BASE_ORIGINS:      # keep a clear apron around each plaza
    for y in range(cy - 1, cy + PLAZA_H + 1):
        for x in range(cx - 1, cx + PLAZA_W + 1):
            if 0 <= x < W and 0 <= y < H: occupied[y][x] = True

# ---- hand-built structures (local coordinates) -----------------------------
FW = ["fence_w", "fence_w2"]; WS = ["wall_s", "wall_s2"]
mp("statue_orb", 60, 44, 1)                                   # island shrine
mp("pillar", 58, 43, 1)
TORII = [(0, 0, 1, 2), (2, 0, 1, 2)]                          # only the two pillars are solid
mp("torii", 19, 28, foot=TORII); mp("torii", 44, 28, foot=TORII)
for name, x in [("hut_a", 15), ("shop", 20), ("house_stone", 24), ("house_dark", 28), ("hut_c", 33), ("house_orange", 38), ("hut_wood", 42)]:
    mp(name, x, 24, 3)
for name, x in [("hut_b", 15), ("hut_wood", 20), ("house_orange", 25), ("shop", 29)]:
    mp(name, x, 31, 3)
mp("dsrt_well", 34, 31, 1); mp("dsrt_stall", 37, 31, 1); mp("dsrt_stall2", 40, 31, 1)
mrun_h(FW, 14, 17, 22); mrun_h(FW, 21, 29, 22); mrun_h(FW, 33, 43, 22)
# ruined courtyard / graveyard (gates on the south and north)
mrun_h(WS, 34, 36, 13); mrun_h(WS, 40, 43, 13); mrun_h(WS, 34, 36, 19); mrun_h(WS, 40, 43, 19)
mrun_v("post_s", 34, 14, 18); mrun_v("post_s", 43, 14, 18)
mp("mstatue_knight", 37, 15, 1); mp("mstatue_monk", 40, 15, 1); mp("mstatue_orb", 38, 17, 1)
mp("ruin_big", 22, 14, foot=[(0, 2, 4, 3)])
mp("ruin_a", 26, 17, 3); mp("ruin_b", 23, 19, 2); mp("ruin_c", 27, 12, 1)
mp("tower_grey", 18, 15, 1); mp("tower_grey", 46, 14, 1)
# desert settlement
mp("dsrt_dome", 50, 13, 2); mp("dsrt_long", 55, 13, 2); mp("dsrt_tower", 58, 19, 2)
mp("dsrt_well", 52, 18, 1); mp("dsrt_lion", 55, 22, 1); mp("dsrt_stall", 50, 24, 1); mp("dsrt_stall2", 54, 25, 1)
mp("tower_sand", 59, 17, 1)
# forest camp (tents, fire)
mp("tent_a", 14, 17, foot=[(0, 1, 3, 2)]); mp("tent_c", 19, 19, foot=[(0, 1, 3, 2)]); mp("firepit", 17, 20, 1)
# snow village + tower, swamp tower
mp("igloo_a", 16, 37, 2); mp("igloo_b", 21, 37, 2); mp("igloo_c", 16, 42, 2); mp("tower_white", 29, 36, 1)
mp("tower_green", 32, 36, 1)

# ---- natural cover ---------------------------------------------------------
GRASS = ["tree_olive_a", "tree_pine", "tree_olive_b", "tree_olive_c", "grove_olive", "grove_pine", "bigtree_green", "bigtree_autumn", "grove_olive2"]
scatter((3, 14, 9, 47), GRASS, 40, 1, 1)
scatter((14, 12, 28, 23), GRASS + ["rock_brown"], 16, 1, 1)
scatter((20, 12, 46, 22), ["rocks_brown", "rocks_grey", "rock_brown", "rock_grey", "stump_a"], 12, 2, 1)
scatter((14, 30, 45, 34), GRASS, 14, 1, 1)
scatter((44, 11, 62, 23), ["rock_brown", "rocks_brown", "tree_dead", "stump_b"], 10, 1, 1, biomes=(0, 1))
scatter((47, 12, 61, 28), ["tree_dead", "rock_brown", "rocks_brown", "stump_b", "bigtree_autumn"], 14, 1, 1, biomes=(1,))
scatter((14, 35, 32, 47), ["tree_snow_a", "tree_snow_b", "tree_white", "grove_snow", "bigtree_white"], 26, 1, 1, biomes=(2,))
scatter((33, 35, 45, 45), ["tree_dead", "grove_dead", "tree_roots", "stump_a", "stump_b"], 9, 1, 1)
scatter((46, 11, 62, 33), GRASS, 8, 1, 1, biomes=(0,))
scatter((3, 12, 63, 47), GRASS, 90, 1, 2)

# solid bush clumps as light cover (1x1, never in dirt/lanes: free_area excludes dirt)
scatter((3, 12, 63, 47), ["bush_a", "bush_b", "bush_c", "bush_d"], 70, 1, 2)
scatter((3, 12, 63, 47), ["bush_f", "bush_e"], 40, 1, 1)
# ---- decoration (non-solid) -------------------------------------------------
GR_TUFT = ["tuft_a", "tuft_b", "tuft_c", "tuft_d", "leaf_a", "leaf_b", "fern_a", "fern_b", "tallgrass", "tallgrass"]
GR_FLOW = ["flower_sun", "flower_sun2", "flower_clover", "flower_red", "flower_white", "flower_white", "flower_red"]
GR_BUSH = ["bush_a", "bush_b", "bush_c", "bush_d", "bush_e", "bush_f", "plant_a", "plant_b"]
SAND = ["dry_a", "dry_b", "dry_c", "dry_d", "dry_a", "dry_c"]
SNOW = ["snowbush_a", "snowbush_b", "snowrock", "snowrock2"]
dec = {}
def plain(x, y):
    return 2 <= x < W // 2 and 2 <= y < H // 2 and not (dirt[y][x] or bridge[y][x] or water[y][x] or swamp[y][x] or ice[y][x] or occupied[y][x]) and (x, y) not in dec
def near_water(x, y):
    return any(0 <= xx < W and 0 <= yy < H and (water[yy][xx] or swamp[yy][xx]) for yy in range(y - 1, y + 2) for xx in range(x - 1, x + 2))
def dpick(x, y):
    bm = biome[y][x]
    if bm == 1: return rnd.choice(SAND)
    if bm == 2: return rnd.choice(SNOW)
    if near_water(x, y) and rnd.random() < 0.6: return rnd.choice(["reed_a", "reed_b"])
    r = rnd.random()
    return rnd.choice(GR_BUSH if r < 0.30 else GR_FLOW if r < 0.55 else GR_TUFT)
# scattered singles
for _ in range(1500):
    x, y = rnd.randrange(2, W // 2), rnd.randrange(2, H // 2)
    if plain(x, y) and (biome[y][x] == 0 or rnd.random() < 0.3): dec[(x, y)] = dpick(x, y)
# lush clusters (flower meadows, bush patches)
for _ in range(220):
    cx, cy = rnd.randrange(3, W // 2), rnd.randrange(3, H // 2)
    if biome[cy][cx] != 0: continue
    kind = rnd.choice(["flow", "bush", "mix", "mix"])
    for _k in range(rnd.randint(5, 11)):
        x, y = int(rnd.gauss(cx, 1.6)), int(rnd.gauss(cy, 1.6))
        if not (0 <= x < W and 0 <= y < H) or not plain(x, y) or biome[y][x] != 0: continue
        dec[(x, y)] = rnd.choice(GR_FLOW) if kind == "flow" else rnd.choice(GR_BUSH) if kind == "bush" else dpick(x, y)
# dirt road edges: grass tufts hugging the path
for y in range(2, H // 2):
    for x in range(2, W // 2):
        if plain(x, y) and biome[y][x] == 0 and any(dirt[y + dy][x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))) and rnd.random() < 0.18:
            dec[(x, y)] = rnd.choice(GR_TUFT)
for (x, y), n in dec.items():
    sheet, c, r, w, h = SPR[n]
    for (xx, yy) in {(x, y), (W - 1 - x, y), (x, H - 1 - y), (W - 1 - x, H - 1 - y)}:
        if overlay[yy][xx] == NONE and not occupied[yy][xx]:
            overlay[yy][xx] = enc(sheet, c, r); occupied[yy][xx] = True
for yy, row in enumerate(dirt):
    for xx, v in enumerate(row):
        if v and rnd.random() < 0.03 and overlay[yy][xx] == NONE and ground[yy][xx] == enc("Floor", 1, 8):
            overlay[yy][xx] = enc("Floor", rnd.choice([0, 1]), 11)
for _ in range(60):
    x, y = rnd.randrange(47, W // 2), rnd.randrange(35, H // 2)
    for (xx, yy) in {(x, y), (W - 1 - x, y), (x, H - 1 - y), (W - 1 - x, H - 1 - y)}:
        if water[yy][xx] and overlay[yy][xx] == NONE:
            overlay[yy][xx] = enc("Water", 11, rnd.choice([3, 3, 4, 1]))

for b in bases:
    for (x, y) in ((b["tx"], b["ty"]), (b["sx"], b["sy"])):
        assert not solid[y][x], b

def flood(sx, sy):
    seen = {(sx, sy)}; q = deque(seen)
    while q:
        x, y = q.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, y + dy)
            if n not in seen and 0 <= n[0] < W and 0 <= n[1] < H and not solid[n[1]][n[0]]:
                seen.add(n); q.append(n)
    return seen
for b in bases:
    seen = flood(b["sx"], b["sy"])
    for o in bases: assert (o["tx"], o["ty"]) in seen, ("unreachable", b["owner"], o["owner"])
walk = sum(1 for y in range(H) for x in range(W) if not solid[y][x])
print("reachability OK; reachable walkable tiles:", len(flood(bases[0]["sx"], bases[0]["sy"])), "/", walk)

# ---- export -----------------------------------------------------------------
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
for (n, tx, ty, w, h, sh, c, r) in objects:
    out.append(f"  {{{SI[sh]},{c},{r},{w},{h},{tx},{ty}}},")
out += ["};", "", "struct Flag { int16_t tx, ty; uint8_t team; };",
        f"inline constexpr int NUM_FLAGS = {len(flags)};", "inline constexpr Flag FLAGS[NUM_FLAGS] = {"]
out += [f"  {{{x},{y},{t}}}," for x, y, t in flags]
out += ["};", "", "struct Base { int cx, cy, cw, ch, tx, ty, sx, sy; };",
        "inline constexpr Base BASES[4] = {"]
out += [f"  {{{b['cx']},{b['cy']},{b['cw']},{b['ch']},{b['tx']},{b['ty']},{b['sx']},{b['sy']}}}," for b in bases]
out += ["};", "} // namespace world", ""]
open("src/game/WorldMap.hpp", "w").write("\n".join(out))

# ---- python preview (same data the client draws) -----------------------------
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
