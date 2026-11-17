"""Headless visual-vs-collision audit of the generated map (no game window, no audio).

Usage:  python tools/collision_audit.py [out_dir]     (default: ./scratch_w/audit)
Reads src/game/WorldMap.hpp (any map size), renders the map from the sprite sheets and
compares sprite opacity with the SOLID grid.  Writes
  audit_overlay.png, audit_q0..q3.png  (map, solid=red tint, water=blue tint, outlines below)
  audit_report.txt                     (all findings with tile coordinates)
Outline legend: red = invisible wall / placement bug, yellow = visible body but walkable,
cyan = water/shoreline mismatch, magenta = object problem.
"""
import re, sys, os, importlib.util
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET = os.path.join(ROOT, "Ninja Adventure - Asset Pack")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "scratch_w", "audit")
os.makedirs(OUT, exist_ok=True)
T = 16


def load_world():
    src = open(os.path.join(ROOT, "src/game/WorldMap.hpp")).read()
    W = int(re.search(r"constexpr int W = (\d+)", src).group(1))
    H = int(re.search(r"H = (\d+)", src).group(1))
    arrs = {}
    for m in re.finditer(r"constexpr (?:uint16_t|uint8_t) (\w+)\[\d+\] = \{(.*?)\};", src, re.S):
        arrs[m.group(1)] = np.array([int(v) for v in m.group(2).replace("\n", "").split(",") if v.strip()]).reshape(H, W)
    objs = [tuple(map(int, m.groups())) for m in re.finditer(r"\{(\d+),(\d+),(\d+),(\d+),(\d+),(-?\d+),(-?\d+)\}", src.split("OBJECTS[")[1].split("struct Flag")[0])]
    flags = [tuple(map(int, m.groups())) for m in re.finditer(r"\{(\d+),(\d+),(\d+)\}", src.split("FLAGS[")[1].split("struct Base")[0])]
    bases = [tuple(map(int, m.groups())) for m in re.finditer(r"\{(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\}", src.split("BASES[")[1])]
    files = re.findall(r'"(Backgrounds/[^"]+)"', src)
    return W, H, arrs, objs, flags, bases, files


W, H, A, OBJ, FLAGS, BASES, FILES = load_world()
SOLID = A["SOLID"]
sheets = [np.array(Image.open(os.path.join(ASSET, f)).convert("RGBA")) for f in FILES]

spec = importlib.util.spec_from_file_location("wc", os.path.join(ROOT, "tools/world_catalog.py"))
wc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wc)
NAME = {}
for n, (sh, c, r, w, h) in wc.SPR.items():
    NAME.setdefault((wc.SHEETS.index(sh), c, r, w, h), n)


def tile_img(code):
    s, r, c = code >> 12, (code >> 6) & 63, code & 63
    return sheets[s][r * T:(r + 1) * T, c * T:(c + 1) * T]


canvas = np.zeros((H * T, W * T, 4), np.uint8)
canvas[..., 3] = 255


def blit(img, x, y):
    if x < 0 or y < 0 or x + img.shape[1] > canvas.shape[1] or y + img.shape[0] > canvas.shape[0]:
        x0, y0 = max(x, 0), max(y, 0)
        x1 = min(x + img.shape[1], canvas.shape[1])
        y1 = min(y + img.shape[0], canvas.shape[0])
        if x1 <= x0 or y1 <= y0:
            return
        img = img[y0 - y:y1 - y, x0 - x:x1 - x]
        x, y = x0, y0
    a = img[..., 3:4].astype(float) / 255
    reg = canvas[y:y + img.shape[0], x:x + img.shape[1]]
    reg[..., :3] = (img[..., :3] * a + reg[..., :3] * (1 - a)).astype(np.uint8)


for y in range(H):
    for x in range(W):
        for L in ("GROUND", "OVERLAY"):
            c = int(A[L][y, x])
            if c != 0xFFFF:
                blit(tile_img(c), x * T, y * T)

COSMETIC = ("flower", "tuft", "bush", "leaf", "reed", "fern", "tallgrass", "plant", "dry_", "snowbush", "pebble",
            "berry_a", "berry_b", "berry_c", "berry_d", "berry_e", "berry_f", "berry_g", "hay", "banner", "awning",
            "firepit", "grass")


def oname(i):
    return NAME.get((OBJ[i][0],) + OBJ[i][1:5], f"?{OBJ[i][0]}/{OBJ[i][1]},{OBJ[i][2]},{OBJ[i][3]}x{OBJ[i][4]}")


def cosmetic(i):
    return oname(i).startswith(COSMETIC)


opq = np.zeros((H, W))              # max opaque fraction of non-cosmetic object pixels per tile
objtile = -np.ones((H, W), int)
objmask = np.zeros((len(OBJ), H, W))
for i, (s, c, r, w, h, tx, ty) in enumerate(OBJ):
    img = sheets[s][r * T:(r + h) * T, c * T:(c + w) * T]
    blit(img, tx * T, ty * T)
    for dy in range(h):
        for dx in range(w):
            x, y = tx + dx, ty + dy
            if 0 <= x < W and 0 <= y < H:
                f = (img[dy * T:(dy + 1) * T, dx * T:(dx + 1) * T, 3] > 40).mean()
                objmask[i, y, x] = f
                if f > 0 and not cosmetic(i):
                    opq[y, x] = max(opq[y, x], f)
                    objtile[y, x] = i
img_w = canvas.copy()

rep = []
marks = {"red": set(), "yellow": set(), "cyan": set(), "magenta": set()}


def R(s=""):
    rep.append(s)


def sect(t, items, color=None, limit=40):
    R(f"{t}: {len(items)}")
    for it in items[:limit]:
        R("   " + str(it))
    if len(items) > limit:
        R(f"   ... +{len(items) - limit} more")
    if color:
        for it in items:
            if isinstance(it, tuple) and len(it) >= 2 and isinstance(it[0], int):
                marks[color].add((it[0], it[1]))


R(f"map {W}x{H}, objects {len(OBJ)}, flags {len(FLAGS)}")

# ---- A(a) invisible walls
BIT = 0.10
cos_cover = np.zeros((H, W))
for i in range(len(OBJ)):
    if cosmetic(i):
        cos_cover = np.maximum(cos_cover, objmask[i])
inv = []
for y in range(H):
    for x in range(W):
        if SOLID[y, x] == 1 and opq[y, x] < BIT:
            ov = A["OVERLAY"][y, x]
            inv.append((x, y, "cosmetic-only" if cos_cover[y, x] > 0 else ("overlay-only" if ov != 0xFFFF else "nothing")))
sect("A(a) solid=1 tiles with no visible non-cosmetic object (invisible walls)", inv, "red")

# ---- A(b) visible bodies that are walkable (bottom row of each non-cosmetic object)
body = []
for i, (s, c, r, w, h, tx, ty) in enumerate(OBJ):
    if cosmetic(i):
        continue
    y = ty + h - 1
    for dx in range(w):
        x = tx + dx
        if 0 <= x < W and 0 <= y < H and objmask[i, y, x] >= 0.35 and SOLID[y, x] == 0:
            body.append((x, y, oname(i), f"obj#{i}@{tx},{ty} {w}x{h} opaque={objmask[i, y, x]:.2f}"))
sect("A(b) bottom-row opaque tiles of non-cosmetic objects that are WALKABLE", body, "yellow", 60)

# ---- A(c) water vs shoreline


def water_frac(code):
    im = tile_img(code).astype(int)
    op = im[..., 3] > 40
    wat = op & (im[..., 2] > im[..., 0] + 15) & (im[..., 2] >= im[..., 1] - 5)
    return wat.mean()


wrong_land, wrong_wet, partial = [], [], []
nbridge = 0
for y in range(H):
    for x in range(W):
        code = int(A["GROUND"][y, x])
        ov = int(A["OVERLAY"][y, x])
        if (code >> 12) != 1:
            continue
        planks = ov != 0xFFFF and (tile_img(ov)[..., 3] > 40).mean() > 0.3
        wf = water_frac(code)
        if SOLID[y, x] == 0:
            nbridge += 1
        if SOLID[y, x] == 2 and wf < 0.30:
            wrong_land.append((x, y, f"solid water, only {wf:.2f} water pixels"))
        if SOLID[y, x] == 0 and wf > 0.5 and not planks:
            wrong_wet.append((x, y, f"walkable but {wf:.2f} water pixels, no planks"))
        elif SOLID[y, x] == 0 and 0.25 < wf <= 0.5 and not planks:
            partial.append((x, y, f"{wf:.2f}"))
sect("A(c1) solid water tiles that look like land", wrong_land, "cyan")
sect("A(c2) walkable tiles that look like water (no bridge/planks overlay)", wrong_wet, "cyan")
R(f"A(c3) walkable shoreline tiles with 25-50% water pixels (informational): {len(partial)}")
R(f"A(c4) walkable tiles whose ground is from the Water sheet (shore/bridge ends): {nbridge}")

# ---- A(e) border
ring = [(x, y) for y in range(H) for x in range(W) if min(x, y, W - 1 - x, H - 1 - y) == 0 and SOLID[y, x] == 0]
sect("A(e1) walkable tiles in the outermost ring", ring, "red")
thick = min(min(next((k for k in range(0, W // 2) if SOLID[y, k] == 0), 99) for y in range(H)),
            min(next((k for k in range(0, H // 2) if SOLID[k, x] == 0), 99) for x in range(W)))
R(f"A(e2) minimum solid border thickness (tiles): {thick}")
notree = [(x, y) for y in range(H) for x in range(W) if min(x, y, W - 1 - x, H - 1 - y) < 2 and SOLID[y, x] == 1 and opq[y, x] < BIT]
sect("A(e3) border-band solid tiles without a visible object", notree, "red")

# ---- A(f) objects
oob = [(o[5], o[6], o) for o in OBJ if o[5] < 0 or o[6] < 0 or o[5] + o[3] > W or o[6] + o[4] > H]
sect("A(f1) objects out of bounds", oob, "magenta")
over = []
nc = [i for i in range(len(OBJ)) if not cosmetic(i)]
for a in range(len(nc)):
    i = nc[a]
    for j in nc[a + 1:]:
        ov = np.minimum(objmask[i], objmask[j])
        pts = np.argwhere(ov >= 0.35)
        if len(pts):
            y, x = pts[0]
            over.append((int(x), int(y), f"obj#{i} {oname(i)} x obj#{j} {oname(j)} ({len(pts)} tiles)"))
sect("A(f2) non-cosmetic objects with overlapping opaque tiles", over, "magenta")
onwater = []
for i, (s, c, r, w, h, tx, ty) in enumerate(OBJ):
    if oname(i) == "boat":
        continue
    for dy in range(h):
        for dx in range(w):
            x, y = tx + dx, ty + dy
            if 0 <= x < W and 0 <= y < H and objmask[i, y, x] >= 0.35 and SOLID[y, x] == 2:
                onwater.append((x, y, f"{oname(i)} obj#{i} drawn on solid water"))
sect("A(f3) objects drawn over solid water tiles", onwater, "magenta")
cos_on_solid = []
for i, (s, c, r, w, h, tx, ty) in enumerate(OBJ):
    if not cosmetic(i):
        continue
    for dy in range(h):
        for dx in range(w):
            x, y = tx + dx, ty + dy
            if 0 <= x < W and 0 <= y < H and objmask[i, y, x] > 0 and SOLID[y, x] == 1 and opq[y, x] < BIT:
                cos_on_solid.append((x, y, f"cosmetic {oname(i)} on solid tile"))
sect("A(f4) cosmetic objects sitting on otherwise-empty solid tiles", cos_on_solid, "magenta")

# ---- D y-sort
ysort = []
for i in nc:
    s, c, r, w, h, tx, ty = OBJ[i]
    rows = [y for y in range(ty, ty + h) if any(0 <= tx + dx < W and 0 <= y < H and SOLID[y, tx + dx] == 1 and objmask[i, y, tx + dx] > 0 for dx in range(w))]
    if rows and max(rows) < ty + h - 1:
        ysort.append((tx, ty, f"{oname(i)} obj#{i}: lowest solid row {max(rows)} but sprite bottom row {ty + h - 1} (player just south of the solid row is drawn BEHIND the sprite's lower rows)"))
sorted_ok = all(OBJ[k][6] + OBJ[k][4] <= OBJ[k + 1][6] + OBJ[k + 1][4] for k in range(len(OBJ) - 1))
nofoot = [(OBJ[i][5], OBJ[i][6], f"{oname(i)} obj#{i} {OBJ[i][3]}x{OBJ[i][4]}") for i in nc
          if OBJ[i][4] >= 2 and not any(0 <= OBJ[i][5] + dx < W and 0 <= OBJ[i][6] + dy < H and SOLID[OBJ[i][6] + dy, OBJ[i][5] + dx] == 1
                                        for dy in range(OBJ[i][4]) for dx in range(OBJ[i][3]))]
R(f"D1 OBJECTS sorted by bottom-y (client draw order requires it): {sorted_ok}")
sect("D2 y-sort oddities (solid rows end above sprite bottom)", ysort, None, 50)
sect("D3 non-cosmetic objects (h>=2) with no solid tile at all (walk-through props)", nofoot, "magenta", 50)
R("D4 geometry: player is drawn after objects whose bottom <= player.y+16; box spans y+3..y+13. Standing flush below the bottom solid row => in front (correct); flush above => behind (correct).")

# ---- E placement
E = []
for i, (cx, cy, cw, ch, tx, ty, sx, sy) in enumerate(BASES):
    for nm, (x, y) in (("treasure", (tx, ty)), ("spawn", (sx, sy))):
        if not (0 <= x < W and 0 <= y < H) or SOLID[y, x] != 0:
            E.append((x, y, f"base{i} {nm} in solid/oob"))
        elif objtile[y, x] >= 0:
            E.append((x, y, f"base{i} {nm} under non-cosmetic object obj#{objtile[y, x]} {oname(objtile[y, x])}"))
for (x, y, t) in FLAGS:
    if SOLID[y, x] != 0:
        E.append((x, y, f"flag team{t} on solid={SOLID[y, x]}"))
sect("E1 spawn/treasure/flag placement problems", E, "red")
asym = {"x": [], "y": [], "xy": []}
for y in range(H):
    for x in range(W):
        v = SOLID[y, x]
        if v != SOLID[y, W - 1 - x]:
            asym["x"].append((x, y, f"{v}!={SOLID[y, W - 1 - x]}"))
        if v != SOLID[H - 1 - y, x]:
            asym["y"].append((x, y, f"{v}!={SOLID[H - 1 - y, x]}"))
        if v != SOLID[H - 1 - y, W - 1 - x]:
            asym["xy"].append((x, y, f"{v}!={SOLID[H - 1 - y, W - 1 - x]}"))
for k, v in asym.items():
    sect(f"E2 SOLID asymmetry mirror-{k} (each pair listed twice)", v, None, 12)
R(f"E3 bases (cx,cy,cw,ch,tx,ty,sx,sy): {BASES}")

# ---- overlay PNG
S = 2
im = Image.fromarray(img_w).resize((W * T * S, H * T * S), Image.NEAREST).convert("RGBA")
ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
d = ImageDraw.Draw(ov)
for y in range(H):
    for x in range(W):
        v = SOLID[y, x]
        if v == 1:
            d.rectangle([x * T * S, y * T * S, (x + 1) * T * S - 1, (y + 1) * T * S - 1], fill=(255, 0, 0, 70))
        elif v == 2:
            d.rectangle([x * T * S, y * T * S, (x + 1) * T * S - 1, (y + 1) * T * S - 1], fill=(0, 80, 255, 60))
col = {"red": (255, 0, 0, 255), "yellow": (255, 230, 0, 255), "cyan": (0, 255, 255, 255), "magenta": (255, 0, 255, 255)}
for k, pts in marks.items():
    for (x, y) in pts:
        d.rectangle([x * T * S, y * T * S, (x + 1) * T * S - 1, (y + 1) * T * S - 1], outline=col[k], width=2)
full = Image.alpha_composite(im, ov).convert("RGB")
full.save(os.path.join(OUT, "audit_overlay.png"))
for n, (a, b) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
    full.crop((a * W * T * S // 2, b * H * T * S // 2, (a + 1) * W * T * S // 2, (b + 1) * H * T * S // 2)).save(os.path.join(OUT, f"audit_q{n}.png"))
open(os.path.join(OUT, "audit_report.txt"), "w").write("\n".join(rep))
print("\n".join(rep))
