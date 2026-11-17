# Gameplay spec v2: stoppable carriers, dropped trophies, secret hedges, leaps

Shared contract for the three workstreams (WS1 server rules, WS2 map, WS3 client). Constants live in `src/game/Rules.hpp`,
protocol fields are already added to `src/game/Protocol.hpp`, map API stubs are in `src/game/Map.hpp` + generated `WorldMap.hpp`.
Protocol magic is bumped to `STT2`. The server is authoritative; snapshots go out at 30 Hz.

## A. Trophy bugs found in the old logic (fix all)
* The server never updated `t.x/t.y` while carried and the client only drew `present==true` treasures -> the owner saw an empty pedestal
  and no trophy anywhere ("vanished"). Fix: treasure is a state machine and `x,y` are written every tick (carrier centre while carried).
* An enemy carrier in fog was invisible (client skipped drawing before the carried-trophy draw) and the minimap never showed treasures.
* Hit/leave returned the trophy home silently (no event/animation). Reconnect-grace carriers stayed frozen and invisible.
* Treasures of empty/inactive slots were live and stealable (solo/duo = farm the empty base).
* Scores/projectiles kept running after `gameOver`; grace (disconnected) players were hittable and could pick up; `setInput` overwrote
  edges (a one-frame `shoot` could be lost when two input packets arrived in one tick) -> latch `shootQueued`/`leapQueued`.
* `GameServer::checkInvariants()` every tick heals corrupted state (see WS1) and logs a warning.
* Live-treasure rule: treasure k is live iff slot k is active, or exactly 1 player is active (solo practice: the 3 neutral treasures are
  live). 2-3 players: empty bases show no trophy (`TState::Inactive`). A mid-match leave makes that treasure Inactive; a carried/dropped one
  is cleared (goes home then Inactive). When only 1 player remains the neutrals become live at home.
* Optional overtime: timer ends in a tie with best score > 0 -> `OVERTIME_SECONDS` sudden death (first delivery wins); needs `Snapshot::overtime`.

## B. Carrier rules (constants in Rules.hpp)
* Carrier speed x0.88, cannot shoot, hit radius 8 px (normal 6). Non-carriers regain ammo x2 faster while any trophy is Carried/Dropped.
* Carrier is revealed to everyone: pulsing beacon ring in the world drawn THROUGH the fog, blinking trophy dot on the minimap (even in
  unexplored area), screen-edge arrow with distance in tiles, global alarm sound on every steal.
* Hit on a carrier: trophy is DROPPED at that spot (`Dropped`, timer = DROP_RETURN_TIME), victim still teleports home. Locked for
  DROP_LOCK (0.5 s) then anyone (not airborne, not a ghost, not already carrying) can pick it up. Owner touching it (RECOVER_R) ->
  instant AtHome (event Recover, no score). After 12 s it flies home (`Returning`, RETURN_TRAVEL 1 s, untouchable) then AtHome (event Returned).
* Delivery unchanged (works even if your own treasure is stolen). Leave/kick/grace-expiry: Carried -> Returning from where it is.
  Connection loss (grace starts): Carried -> Dropped in place. Any non-Home/non-Carried state older than 30 s is forced home by the invariant check.
* State machine per treasure: AtHome -> Carried (enemy within PICKUP_R; players in id order; first wins), Dropped -> Carried (after lock),
  Dropped -> AtHome (owner), Dropped -> Returning (timer<=0), Returning -> AtHome, Carried -> AtHome (delivery, score+1, winScore check),
  Carried -> Dropped (hit / connection loss), Carried -> Returning (leave/kick/invariant), any -> Inactive via `refreshLive()`.
* Events: Steal(a=thief,b=owner,c=id), Drop(a=victim,b=shooter,c=id), Recover(a=owner,c=id), Returned(c=id), Deliver, Leap(a), Land(a).
  Keep `TreasureState::present = (state==AtHome)` for compatibility.

## C. Secret paths (hedge corridors) — WS2 generates, WS3 renders
* 1-tile wide, 6-10 tile long walkable corridors flanked by solid bush walls (ordinary placed solid objects). The corridor cells are
  `SOLID==0` but carry `SECRET[y*W+x] = passage id (1..N)` and a 1x1 bush sprite in `SECRET_OBJS` (NOT in `OBJECTS`) that the client
  draws IN FRONT of players so a player walking inside is partly hidden. Each passage is mirrored into all 4 quadrants. Never required for
  connectivity (flood fill with secrets treated solid must still connect all 4 bases), >= 8 tiles from any treasure/spawn, detour ratio >= 2.5.
  Placement is searched deterministically in the generator (free 3xN rectangle with free approach tiles at both ends).
* `PASSAGES[]` lists the two entrance tiles of each passage (id, ax,ay,bx,by). `PORTALS` stays empty (stretch goal only).
* Client: bush alpha 255, ~150 when the local player is within 1.5 tiles; enemies standing on SECRET cells are hidden unless within 3 tiles
  of the local player (carriers never hidden); discovery when my tile has SECRET id not yet discovered: toast "Secret passage found!",
  sound `Secret`, sparkle burst, minimap dot; discovered[] reset with the fog at match start; entrance shimmer when within 8 tiles of an
  undiscovered one. Collision needs no server code (SOLID==0).

## D. Leap (jump across)
* Input: Shift or J, `ClientInput.leap` edge. Server latches `leapQueued`. Cooldown LEAP_CD (carrier LEAP_CD_CARRIER), exposed as
  `PlayerState::leapCd` (tenths of a second left) and `leapPhase` (0 grounded, 1..255 progress through LEAP_TIME).
* New SOLID code 3 = low obstacle (fence, short wall, hay bale, crates) -> `Tile::Low`: blocks walking and shots like a wall but can be leapt over.
* `gcmap::leapLanding(px,py,dir,maxDist)`: dir 0 down,1 up,2 left,3 right (gc::FACE_*). For d from maxDist down to LEAP_MIN step 2 px:
  landing box (player box at p + dir*d) must be `!solidBox`, and every 2px sample from 0..d must contain no `hardTile` under the box
  (trees/buildings/border block; water and Low may be crossed). Returns the first valid d or 0. maxDist = carrier ? LEAP_DIST_CARRIER : LEAP_DIST.
* Server: leap starts only if grounded, connected, cooldown ready, landing>0 (no cooldown spent when it cannot land); during the arc ignore input,
  lerp x,y from->to (no collision checks, pre-validated), airborne players are immune to projectiles and cannot pick up/deliver; event Leap/Land.
  Gaps: type A 2 tiles (carriers can cross), type B 3 tiles (72px only, carriers cannot).
* Map features (mirrored): creeks 2-3 tiles wide through open grass each with a one-tile plank bridge so walking connectivity never needs a leap,
  fences/walls as Low, stepping stones, maybe one broken bridge segment (never all four lake bridges).
* Client: arc animation (sprite y offset -sin(pi*t)*12, shrinking shadow), sounds Leap/Land, HUD cooldown chip. GAME_BOT autopilot: BFS with jump edges.

## E. Sounds to add to Sound.hpp (files exist in Audio/Sounds)
Alarm `Alert/Alert3.wav` (every steal, global), Drop `Alert/Alert5.wav`, Recover `Bonus/PowerUp2.wav`, Return `Magic & Skill/Heal.wav`,
Leap `Jump & Bounce/Jump.wav`, Land `Jump & Bounce/Bounce3.wav`, Secret `Bonus/Bonus3.wav` (verify each file exists; pick alternatives if not).
Chase music (`setChase`) should also trigger when ANY trophy is carried near/at all (tense), keep the existing local-carrier behaviour.

## F. Tests (headless, no manual navigation; humans test feel)
tools/rules_test.cpp (treasure state machine on real GameServer logic), tools/leap_test.cpp or collision_test.cpp extension (leap validity + mirror
symmetry on the real map), generator asserts (SECRET/SOLID==3 symmetry, connectivity with secrets solid, distance from treasures, gap widths).
