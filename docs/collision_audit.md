# Collision audit (headless)

Tools (run quietly, no window/audio):
- `python tools/collision_audit.py [out_dir]` renders the generated map from the sprite sheets and compares sprite opacity with the SOLID grid (checks A, D, E). Writes `audit_overlay.png`, `audit_q0..q3.png`, `audit_report.txt`.
- `tools/collision_test.cpp` (build line in its header) runs the real `gcmap` code (checks B, C): box edge accuracy, tunnelling with the exact server `applyMove`, sliding, 1px-grid reachability flood fill, pickup range, projectile simulation with the exact server logic.

State audited: 80x60 map, after the other engineer's last edit (quiet from 23:02 to 23:13).

## Bugs found and fixed
1. **Bottom-half objects had upside-down collision (largest bug).** `mfoot()` in `tools/build_world.py` flipped the footprint for mirrored copies, but sprites are not flipped. In the lower half the solid tiles sat on the canopy/roof and the trunk/base/door row was walkable (204 walkable opaque bottom-row tiles: trees, igloos, towers, boulders, statues, ruins; players walked through trunks). Fix: footprints stay in sprite space. To keep the exact fairness symmetry, `place()` nudges the bottom copy so its solid rows are the mirror image of the top copy (and rejects placements whose mirrored copies overlap each other). Result: A(b) 204 -> 0, mirror-x/y/xy SOLID asymmetry 0 (an interim version that only unflipped footprints had 956 asymmetric cells). Cost: 504 -> 476 objects (some scatter placements no longer fit).
2. **1px sink into east/south walls.** `gcmap::solidBox` used `(px+w-1)/TILE`, so a box overlapping a solid tile by under 1px on its right/bottom side was not blocked (1162 of 2324 side tests). Fixed with an epsilon (`px+w-0.001`) and `floor`; now 0 / 2376 fail, max penetration 0.000px, max gap 0.000px.
3. **`palm_pair` footprint** covered 4 tiles but only 2 trunks are drawn: 2 invisible walls per palm (`DEFAULT_FOOT` in build_world.py, foot `(1,2,2,1)`).
4. **Dead-end planks/stones on sealed pond islands.** Sealing of tiny disconnected pockets made planks/stepping stones solid. Sealed pond tiles are now solid=2 and Water-sheet overlays are removed from sealed tiles.

## Results (final state)
| Check | Result |
|---|---|
| A(a) invisible walls | 44 tiles, all benign: 20 sealed pockets (enclosed on all sides by visible solids, unreachable); see below |
| A(a2) solid under small cosmetic-class props (bush/pebble/awning) | 60, informational (intentional cover; small props are solid) |
| A(b) walkable opaque bottom-row tiles | 0 (was 204) |
| A(c1) solid water tiles that look like land (shoreline corner pieces, 5-18% water pixels) | 192, informational: tile-granular collision makes pond/river corner tiles solid; they read as a 1-tile invisible wall at pond rims |
| A(c2) walkable tiles that look like water without planks | 0; 64 walkable Water-sheet tiles are the bridges/stones/shore ends |
| A(e) border | 0 walkable tiles in outer ring, solid border 2 tiles thick, every border tile has a tree |
| A(f) out of bounds / overlapping opaque objects / objects over water | 0 / 0 / 0 |
| D y-sort | OBJECTS sorted by bottom y: yes; solid rows always include the sprite's bottom row: 0 oddities; 0 walk-through props with no footprint. Player draws in front below the bottom solid row and behind above it (geometry correct). |
| E spawn/treasure/flags | none in solid/water/under objects; SOLID is exactly mirror symmetric in x, y and both |
| B1 box edge accuracy, corners | pass (2376 sides, 790 corners) |
| B2 spawn/treasure free | pass |
| B3 random walks, dt 1/120..0.2 (4000 walks each, exact server applyMove) | 0 stuck/tunnelled; max step 4.6px at the server's 0.05 s clamp vs 26px needed to cross a wall |
| B4 sliding along walls | pass. Note: applyMove does not snap to walls, so a player stops up to 4.6px (0.05 s frame) / 1.5px (60 Hz) short of a wall, plus the 3px sprite inset |
| B5 | 324 one-tile corridor cells (6px slack each side for the 10px box), 25 diagonal pinch points (impassable for the box, as intended) |
| B6 reachability (1px grid, real box) | all 4 spawns mutually reachable; every player reaches every enemy treasure and own delivery spot; 0 unreachable walkable pockets; 0 pickup positions through a wall |
| C projectiles | no tunnelling at dt <= 0.05 (step 11px vs 24px needed). At dt 0.1/0.2 (never happens, server clamps to 0.05) a shot fired flush against a wall starts inside it and can pass: reported as INFO only |

## Observations (not changed)
- Water is solid for projectiles (server uses `solidBox`): shots stop at ponds/rivers (about 2% of random shots end at water). Probably unintended for a river-heavy map; if shots should fly over water, the server projectile check needs a `solidTile`-without-water variant (design decision).
- `gc::PROJECTILE_LIFE` is never used by the server; projectiles only die on solid/bounds/player (the border makes this safe, max travel 1600px).
- Hit circle: PLAYER_HIT_R + PROJECTILE_R = 10px from the sprite centre vs a 10px collision box and 16px sprite: generous but consistent.
- Sealed pockets (20) are unreachable nooks (graveyard enclosure behind a hut etc.); in the lower half the hut sprite no longer covers them so a few 4-tile open-looking enclosed strips show at (11..14,44), (65..68,44), (34,19),(45,19),(34,40),(45,40). Harmless (cannot be entered), could be filled with a prop.
- 24 `WARN blocked/no spot` lines from build_world.py (fence runs etc.) are placement failures of the generator, unchanged in nature.
- Overlay images: `scratch` outputs in the scratchpad (`audit/audit_q*.png`, `reach.png`).

## Gameplay spec v2 map work (Low obstacles, creeks, secret hedges, leap)
Generator: `python tools/build_world.py [preview.png]` (Windows python; it prints a summary and asserts everything below).
Tests: `g++ -std=c++17 -O2 -I src/game tools/leap_test.cpp -o scratch_w/leap_test.exe && scratch_w/leap_test.exe` (also rerun `collision_test.exe`; both PASS).

| Item | Result |
|---|---|
| SOLID==3 (Low) | 44 cells: courtyard walls/posts, village fences, hay bale/tuft, market crates/baskets, `el_crate`. Mirror symmetric (asserted in generator, audit E6, C++ L4). Treated as solid by pruning, reachability, pockets, audit visual checks, projectiles |
| Seam river | 2-wide (carrier-leapable), x=39|40, y 2..19 (+ mirrored south, y 40..57), flows from the north/south border into the lake. Plank bridge (1 tile) at y=3 / y=56, broken bridge = plank stubs only on both banks at y=12 / y=47 (2-tile gap), stepping stones at y=15 and y=17 (visual only, still water). Never touches the 4 lake bridges; central island stays reachable on foot |
| West/east creek | 3-wide (normal leap only), x 2..5 rows 17..19 (mirrored: rows 40..42 and east flank), ends in a pool, 1-tile plank bridge at x=3 (rows 16..20) |
| Secret hedges | 2 sites x 4 mirrors = 8 passages, 6 cells long (48 secret cells). Block = 3 rows x 10; the middle row has a 2-cell open alcove at each end (the `PASSAGES` tiles ax,ay/bx,by are the alcove back cells), the 6 middle cells are `SECRET` id + `SECRET_OBJS` bush. Site A (snow, tile quadrant coords x10..19,y23..25): passages 1-4 `(11,23)-(18,23)`, `(68,23)-(61,23)`, `(11,36)-(18,36)`, `(68,36)-(61,36)`. Site B (desert/grass edge x23..32,y12..14): passages 5-8 `(24,12)-(31,12)`, `(55,12)-(48,12)`, `(24,47)-(31,47)`, `(55,47)-(48,47)`. Detour ratio (walk around with secrets solid / corridor length+1) = 19/7 = 2.7 each; all entrances >= 8 tiles (Chebyshev) from every treasure/spawn |
| Generator asserts | SECRET + SOLID symmetric in x/y/xy; 4 bases connected with secrets treated solid; passage ends in the main region; detour >= 2.5; leap shortening: spawn0->treasure1 walk 74 -> leap 60 tiles = 18.9% (identical for the mirrored pair 2->3); vertical pairs 0% (no creek blocks the west road) |
| leap_test L1 | 262,728 (cell,dir,maxDist) queries from every walkable cell + 60k random sub-tile positions: every landing is non-solid, even px, <= maxDist, no hard tile on the 2px path samples |
| leap_test L2 | x- and y-mirror property exact (<= 0.01px) over 528,960 queries (positions quantised to 1/4 px so mirror images are exact floats) |
| leap_test L3/L5 | useful leaps (path crosses water/Low): normal 652 cell-dir (532 start cells; crossings of 1/2/3 tiles = 184/332/136), carrier 324 (1 and 2 tiles only: 140/184, so 3-tile gaps need a normal leap); 64 cell-dir leaps cross a Low obstacle |
| leap_test L4/S | Low and water: blocked for projectiles, Low also for walking, neither is a hard tile; Tree is hard. SECRET cells walkable and symmetric; passages connected; >= 8 tiles from goals |
| Audit | A(a) 44 / A(c1) 200 (was 192: +8 shoreline corner pieces of the new creek ends at the border); SECRET cells exempt from "walkable opaque body"; audit overlay draws SECRET green and Low orange |

Notes: leapLanding returns the maximum valid distance, so on open ground it returns maxDist (the server decides when a leap is worthwhile, e.g. only when the landing differs from walking, or the player faces a flyOver tile). Secret-site placement is searched deterministically (`SEARCH=1 python tools/build_world.py` lists candidate sites on the empty map) and reserved before the structures are placed; several decoration props (igloos, a desert dome) no longer find a spot ("WARN no spot" lines), which reduces clutter.
