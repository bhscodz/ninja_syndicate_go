// Headless console collision/reachability/projectile test using the REAL gcmap code (no SFML, no window).
// Build:  export PATH=/c/msys64/ucrt64/bin:$PATH
//         g++ -std=c++17 -O2 -I src/game tools/collision_test.cpp -o scratch_w/collision_test.exe
// Run:    scratch_w/collision_test.exe [reach.ppm]     (optional PPM of the reachable tiles, 8px/tile)
// Works for any MAP_W/MAP_H (read from Constants.hpp / WorldMap.hpp).  Prints PASS/FAIL counts per check.
#include <cstdio>
#include <cmath>
#include <vector>
#include <queue>
#include <random>
#include <algorithm>
#include <string>
#include "Map.hpp"

using gcmap::solidBox; using gcmap::playerBlocked; using gcmap::solidTile;
static const int T = gc::TILE, W = gc::MAP_W, H = gc::MAP_H;
static int g_fail = 0;
static void check(const char* name, long fails, long total, const std::string& detail = "", bool hypothetical = false) {
    // hypothetical = dt larger than the server's 0.05 s clamp: reported as INFO, not counted as a failure
    std::printf("%s %-58s fails=%ld / %ld %s\n", fails ? (hypothetical ? "INFO" : "FAIL") : "PASS", name, fails, total, detail.c_str());
    if (fails && !hypothetical) ++g_fail;
}

// exact copy of Game::applyMove (src/server/ServerMain.cpp)
static void applyMove(float& px, float& py, float dirX, float dirY, float dt) {
    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len > 0.001f) { dirX /= len; dirY /= len; }
    const float step = gc::PLAYER_SPEED * dt, o = gcmap::boxOffset();
    float nx = px + dirX * step;
    if (!solidBox(nx + o, py + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) px = nx;
    float ny = py + dirY * step;
    if (!solidBox(px + o, ny + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) py = ny;
}
static bool projSolid(float x, float y) {
    return solidBox(x - gc::PROJECTILE_R, y - gc::PROJECTILE_R, gc::PROJECTILE_R * 2, gc::PROJECTILE_R * 2);
}
static const float DVX[4] = {0, 0, -1, 1}, DVY[4] = {1, -1, 0, 0};

int main(int argc, char** argv) {
    gcmap::generate();
    int nsolid = 0, nwater = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { if (world::SOLID[y * W + x] == 1) ++nsolid; if (world::SOLID[y * W + x] == 2) ++nwater; }
    std::printf("map %dx%d tiles, solid=%d water=%d, box=%.0f speed=%.0f\n", W, H, nsolid, nwater, gc::PLAYER_BOX, gc::PLAYER_SPEED);
    check("world::W/H match Constants MAP_W/MAP_H", (world::W != W) + (world::H != H), 2);
    const float o = gcmap::boxOffset();

    // ---- B1 box edge accuracy: approach every free side of every solid tile in 1/8 px steps
    {
        long fails = 0, total = 0; float maxPen = 0, maxGap = 0; int ex = -1, ey = -1; const char* es = "";
        for (int ty = 1; ty < H - 1; ++ty) for (int tx = 1; tx < W - 1; ++tx) {
            if (!solidTile(tx, ty)) continue;
            for (int s = 0; s < 4; ++s) {  // side the player comes from: 0=W,1=E,2=N,3=S
                int nx = tx + (s == 0 ? -1 : s == 1 ? 1 : 0), ny = ty + (s == 2 ? -1 : s == 3 ? 1 : 0);
                if (solidTile(nx, ny)) continue;
                // the centred 10px box only touches the neighbour tile + this tile as long as the neighbour is free
                float py0 = ty * T, px0 = tx * T; // sprite origin aligned to the tile -> box centred on the band
                float found = -1;
                for (float d = 6.f; d >= -6.f; d -= 0.125f) {
                    // d = distance between box edge and tile boundary (positive = gap)
                    float bx, by;  // box top-left
                    if (s == 0) { bx = tx * T - gc::PLAYER_BOX - d; by = ty * T + o; }
                    else if (s == 1) { bx = (tx + 1) * T + d; by = ty * T + o; }
                    else if (s == 2) { bx = tx * T + o; by = ty * T - gc::PLAYER_BOX - d; }
                    else { bx = tx * T + o; by = (ty + 1) * T + d; }
                    if (solidBox(bx, by, gc::PLAYER_BOX, gc::PLAYER_BOX)) { found = d; break; }
                }
                (void)py0; (void)px0;
                ++total;
                // blocked first at d<=0 (overlap).  d should be in (-0.125, 0]
                if (found > 0.0001f) { ++fails; maxGap = std::max(maxGap, found); if (ex < 0) { ex = tx; ey = ty; es = "gap"; } }
                else if (found < -0.13f) { ++fails; maxPen = std::max(maxPen, -found); if (ex < 0) { ex = tx; ey = ty; es = "penetration"; } }
                else if (found == -1) { ++fails; }
            }
        }
        char b[160]; std::snprintf(b, sizeof b, "(max gap %.3fpx, max penetration %.3fpx, first at tile %d,%d %s)", maxGap, maxPen, ex, ey, es);
        check("B1 box blocks exactly at tile boundary (all 4 sides)", fails, total, b);
    }
    // ---- B1b corner approach: overlap only a tile corner
    {
        long fails = 0, total = 0;
        for (int ty = 1; ty < H - 1; ++ty) for (int tx = 1; tx < W - 1; ++tx) {
            if (!solidTile(tx, ty)) continue;
            for (int cx = 0; cx < 2; ++cx) for (int cy = 0; cy < 2; ++cy) {
                int ax = tx + (cx ? 1 : -1), ay = ty + (cy ? 1 : -1);
                if (solidTile(ax, ay) || solidTile(ax, ty) || solidTile(tx, ay)) continue;
                float ov = 1.0f;  // 1px overlap in both axes
                float bx = cx ? (tx + 1) * T - ov : tx * T + ov - gc::PLAYER_BOX;
                float by = cy ? (ty + 1) * T - ov : ty * T + ov - gc::PLAYER_BOX;
                float bx0 = cx ? (tx + 1) * T : tx * T - gc::PLAYER_BOX;  // touching exactly, no overlap
                float by0 = cy ? (ty + 1) * T : ty * T - gc::PLAYER_BOX;
                ++total;
                if (!solidBox(bx, by, gc::PLAYER_BOX, gc::PLAYER_BOX) || solidBox(bx0, by0, gc::PLAYER_BOX, gc::PLAYER_BOX)) ++fails;
            }
        }
        check("B1b corner overlap blocked / corner touch free", fails, total);
    }
    // ---- B2 spawn / treasure positions
    {
        long f = 0;
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& b = gcmap::bases()[i];
            if (playerBlocked(b.spawnX, b.spawnY)) { ++f; std::printf("   base %d spawn %.0f,%.0f blocked\n", i, b.spawnX, b.spawnY); }
            if (playerBlocked(b.treasureX - 8, b.treasureY - 8)) { ++f; std::printf("   base %d treasure %.0f,%.0f blocked for a player standing on it\n", i, b.treasureX, b.treasureY); }
        }
        check("B2 spawn & treasure free for the player box", f, gc::NUM_PLAYERS * 2);
    }
    // ---- B3 no tunnelling / never ends inside a solid, exact applyMove, random walks
    {
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> U(0, 1);
        const float dts[] = {1.f / 120, 1.f / 60, 1.f / 30, 0.05f, 0.1f, 0.2f};
        for (float dt : dts) {
            long bad = 0, total = 0;
            for (int n = 0; n < 4000; ++n) {
                float px, py; int guard = 0;
                do { px = U(rng) * W * T; py = U(rng) * H * T; } while (playerBlocked(px, py) && ++guard < 1000);
                if (playerBlocked(px, py)) continue;
                float dx = U(rng) * 2 - 1, dy = U(rng) * 2 - 1;
                for (int s = 0; s < 120; ++s) {
                    if (s % 30 == 0) { dx = U(rng) * 2 - 1; dy = U(rng) * 2 - 1; }
                    applyMove(px, py, dx, dy, dt); ++total;
                    if (playerBlocked(px, py) || px < 0 || py < 0 || px > W * T || py > H * T) { ++bad; break; }
                }
            }
            char nm[80], det[80]; std::snprintf(nm, sizeof nm, "B3 random walk never inside solid, dt=%.4f", dt);
            std::snprintf(det, sizeof det, "(step %.2fpx; tunnel needs >= %.0fpx)", gc::PLAYER_SPEED * dt, 16 + gc::PLAYER_BOX);
            check(nm, bad, total, det);
        }
        // worst-case stop distance from a wall (no snapping in applyMove)
        std::printf("INFO player can stop up to %.2fpx (dt 0.05) / %.2fpx (dt 1/60) short of a wall (+%.0fpx sprite inset)\n",
                    gc::PLAYER_SPEED * 0.05f, gc::PLAYER_SPEED / 60.f, o);
    }
    // ---- B4 sliding along walls: diagonal input flush against a wall keeps progressing along it
    {
        long fails = 0, total = 0; int ex = -1, ey = -1;
        for (int ty = 2; ty < H - 2; ++ty) for (int tx = 2; tx < W - 2; ++tx) {
            if (!solidTile(tx, ty)) continue;
            // wall on the east of the player (player in tile tx-1), moving diag (+x,+y) along a vertical wall
            if (!solidTile(tx - 1, ty) && !solidTile(tx - 1, ty + 1) && !solidTile(tx - 1, ty - 1) && solidTile(tx, ty + 1) && solidTile(tx, ty - 1)) {
                float px = (tx - 1) * T + (T - gc::PLAYER_BOX) * 0.5f + 0.5f - o + 0.0f, py = ty * T + 2;
                px = tx * T - gc::PLAYER_BOX - o;  // flush
                float y0 = py; for (int s = 0; s < 6; ++s) applyMove(px, py, 1.f, 1.f, 0.05f);
                ++total; if (py - y0 < 6 * gc::PLAYER_SPEED * 0.05f * 0.7f * 0.8f && !playerBlocked(px, py + 4.f)) { ++fails; if (ex < 0) { ex = tx; ey = ty; } }
            }
        }
        char b[80]; std::snprintf(b, sizeof b, "(first at %d,%d)", ex, ey);
        check("B4 slides along vertical walls with diagonal input", fails, total, b);
    }
    // ---- B5 corridors / diagonal gaps statistics
    {
        long c1 = 0, diag = 0;
        for (int y = 1; y < H - 1; ++y) for (int x = 1; x < W - 1; ++x) {
            if (solidTile(x, y)) continue;
            if (solidTile(x - 1, y) && solidTile(x + 1, y)) ++c1;
            if (solidTile(x, y - 1) && solidTile(x, y + 1)) ++c1;
            if (solidTile(x + 1, y) && solidTile(x, y + 1) && !solidTile(x + 1, y + 1)) ++diag;  // diagonal pinch
        }
        std::printf("INFO free tiles squeezed between two solids (1-tile corridor cells, counted per axis): %ld; diagonal pinch points: %ld (box 10px cannot pass a pinch)\n", c1, diag);
    }
    // ---- B6 reachability (1px grid flood fill with the real box), from each spawn
    {
        const int PW = W * T, PH = H * T;
        std::vector<uint8_t> vis(PW * PH, 0);
        std::vector<std::vector<uint8_t>> visBy;
        auto flood = [&](int sx, int sy, std::vector<uint8_t>& v) {
            std::queue<int> q; if (playerBlocked((float)sx, (float)sy)) return;
            v[sy * PW + sx] = 1; q.push(sy * PW + sx);
            const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
            while (!q.empty()) {
                int c = q.front(); q.pop(); int x = c % PW, y = c / PW;
                for (int k = 0; k < 4; ++k) {
                    int nx = x + ddx[k], ny = y + ddy[k];
                    if (nx < 0 || ny < 0 || nx >= PW || ny >= PH || v[ny * PW + nx]) continue;
                    if (playerBlocked((float)nx, (float)ny)) continue;
                    v[ny * PW + nx] = 1; q.push(ny * PW + nx);
                }
            }
        };
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            visBy.emplace_back(PW * PH, 0);
            flood((int)gcmap::bases()[i].spawnX, (int)gcmap::bases()[i].spawnY, visBy[i]);
        }
        vis = visBy[0];
        long spawnFail = 0;
        for (int i = 1; i < gc::NUM_PLAYERS; ++i) {
            auto& b = gcmap::bases()[i];
            if (!vis[(int)b.spawnY * PW + (int)b.spawnX]) { ++spawnFail; std::printf("   spawn %d not reachable from spawn 0\n", i); }
        }
        check("B6a all spawns mutually reachable", spawnFail, gc::NUM_PLAYERS - 1);
        long tfail = 0, thru = 0, tpos = 0;
        for (int k = 0; k < gc::NUM_PLAYERS; ++k) for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (i == k) continue;
            auto& tb = gcmap::bases()[k];
            bool ok = false; long nthru = 0;
            for (int dy = -14; dy <= 14 && true; ++dy) for (int dx = -14; dx <= 14; ++dx) {
                if (dx * dx + dy * dy >= 196) continue;   // server pickup radius 14px, measured from sprite centre
                int px = (int)(tb.treasureX - 8) + dx, py = (int)(tb.treasureY - 8) + dy;
                if (px < 0 || py < 0 || px >= PW || py >= PH || !visBy[i][py * PW + px]) continue;
                ok = true; ++tpos;
                // line of sight from sprite centre to the treasure centre blocked by a solid tile?
                float cx = px + 8, cy = py + 8; bool cross = false;
                for (int s = 0; s <= 16; ++s) { float fx = cx + (tb.treasureX - cx) * s / 16.f, fy = cy + (tb.treasureY - cy) * s / 16.f; if (solidTile((int)(fx / T), (int)(fy / T))) cross = true; }
                if (cross) ++nthru;
            }
            if (!ok) { ++tfail; std::printf("   player %d can never get within pickup range of treasure %d\n", i, k); }
            thru += nthru;
        }
        check("B6b every player can reach every enemy treasure (pickup radius)", tfail, gc::NUM_PLAYERS * (gc::NUM_PLAYERS - 1));
        check("B6c pickup positions whose line to the treasure crosses a solid tile", thru, tpos);
        // own treasure delivery radius 16
        long dfail = 0;
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& b = gcmap::bases()[i]; bool ok = false;
            for (int dy = -16; dy <= 16 && !ok; ++dy) for (int dx = -16; dx <= 16 && !ok; ++dx) {
                if (dx * dx + dy * dy >= 256) continue;
                int px = (int)(b.treasureX - 8) + dx, py = (int)(b.treasureY - 8) + dy;
                if (px >= 0 && py >= 0 && px < PW && py < PH && visBy[i][py * PW + px]) ok = true;
            }
            if (!ok) ++dfail;
        }
        check("B6d every player can reach own delivery spot", dfail, gc::NUM_PLAYERS);
        // unreachable walkable tiles: a tile is reachable when some reachable position has the sprite centre in it
        std::vector<uint8_t> tr(W * H, 0);
        for (int y = 0; y < PH; ++y) for (int x = 0; x < PW; ++x) if (vis[y * PW + x]) { int cx = (x + 8) / T, cy = (y + 8) / T; if (cx < W && cy < H) tr[cy * W + cx] = 1; }
        std::vector<int> comp(W * H, -1); int nc = 0; long unreach = 0;
        struct C { int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; };
        std::vector<C> comps;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (solidTile(x, y) || tr[y * W + x] || comp[y * W + x] >= 0) continue;
            C c; std::queue<std::pair<int, int>> q; q.push({x, y}); comp[y * W + x] = nc;
            while (!q.empty()) { auto [a, b2] = q.front(); q.pop(); ++c.n; c.x0 = std::min(c.x0, a); c.y0 = std::min(c.y0, b2); c.x1 = std::max(c.x1, a); c.y1 = std::max(c.y1, b2);
                const int ax[4] = {1, -1, 0, 0}, ay[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) { int na = a + ax[k], nb = b2 + ay[k]; if (na < 0 || nb < 0 || na >= W || nb >= H || solidTile(na, nb) || tr[nb * W + na] || comp[nb * W + na] >= 0) continue; comp[nb * W + na] = nc; q.push({na, nb}); } }
            comps.push_back(c); ++nc; unreach += c.n;
        }
        std::printf("B6e unreachable walkable tiles: %ld in %d pockets\n", unreach, nc);
        for (size_t i = 0; i < comps.size() && i < 60; ++i) std::printf("   pocket %zu: %d tiles, bbox (%d,%d)-(%d,%d)\n", i, comps[i].n, comps[i].x0, comps[i].y0, comps[i].x1, comps[i].y1);
        if (argc > 1) {
            FILE* f = std::fopen(argv[1], "wb"); const int S = 8;
            std::fprintf(f, "P6\n%d %d\n255\n", W * S, H * S);
            for (int y = 0; y < H * S; ++y) for (int x = 0; x < W * S; ++x) {
                int tx = x / S, ty = y / S; unsigned char c[3];
                if (world::SOLID[ty * W + tx] == 1) { c[0] = 60; c[1] = 60; c[2] = 60; }
                else if (world::SOLID[ty * W + tx] == 2) { c[0] = 40; c[1] = 90; c[2] = 220; }
                else if (tr[ty * W + tx]) { c[0] = 80; c[1] = 200; c[2] = 80; }
                else { c[0] = 255; c[1] = 0; c[2] = 0; }
                std::fwrite(c, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    // ---- C projectiles (exact server logic)
    {
        auto simulate = [&](float x, float y, int dir, float dt, float& dist, bool& hitWater, bool& bounds) {
            float vx = DVX[dir] * gc::PROJECTILE_SPEED, vy = DVY[dir] * gc::PROJECTILE_SPEED; dist = 0; hitWater = false; bounds = false;
            for (int i = 0; i < 100000; ++i) {
                x += vx * dt; y += vy * dt; dist += gc::PROJECTILE_SPEED * dt;
                if (x < 0 || y < 0 || x > W * T || y > H * T) { bounds = true; return; }
                if (projSolid(x, y)) {
                    hitWater = gcmap::tileAt((int)(x / T), (int)(y / T)) == gcmap::Water; return; }
            }
        };
        auto reference = [&](float x, float y, int dir, float& dist, bool& hitWater) {  // 0.25px march, also tests the spawn point
            dist = 0; hitWater = false;
            for (int i = 0; i < 1000000; ++i) {
                if (x < 0 || y < 0 || x > W * T || y > H * T) return;
                if (projSolid(x, y)) { hitWater = gcmap::tileAt((int)(x / T), (int)(y / T)) == gcmap::Water; return; }
                x += DVX[dir] * 0.25f; y += DVY[dir] * 0.25f; dist += 0.25f;
            }
        };
        std::printf("INFO projectile step/tick: %.1fpx @dt .05, %.1fpx @dt .1; wall thickness 16 + diameter %.0f = %.0f needed to tunnel; hit circle %.0f px, player box %.0f\n",
                    gc::PROJECTILE_SPEED * 0.05f, gc::PROJECTILE_SPEED * 0.1f, gc::PROJECTILE_R * 2, 16 + gc::PROJECTILE_R * 2, gc::PLAYER_HIT_R + gc::PROJECTILE_R, gc::PLAYER_BOX);
        const float dts[] = {1.f / 60, 1.f / 30, 0.05f, 0.1f, 0.2f};
        for (float dt : dts) {
            long tunnel = 0, total = 0, water = 0, land = 0, boundsHit = 0; float worst = 0; int wx = -1, wy = -1;
            for (int i = 0; i < gc::NUM_PLAYERS; ++i) for (int d = 0; d < 4; ++d) {
                auto& b = gcmap::bases()[i];
                float sx = b.spawnX + T * 0.5f + DVX[d] * 12.f, sy = b.spawnY + T * 0.5f + DVY[d] * 12.f;
                float dist, rd; bool hw, rw, bo; simulate(sx, sy, d, dt, dist, hw, bo); reference(sx, sy, d, rd, rw);
                ++total; if (bo) ++boundsHit; (rw ? water : land)++;
                if (dist > rd + gc::PROJECTILE_SPEED * dt + 0.5f) { ++tunnel; if (dist - rd > worst) { worst = dist - rd; wx = i; wy = d; } }
            }
            // flush-to-wall shots from every free side of every solid tile at gaps 0/2/4.6px
            for (int ty = 1; ty < H - 1; ++ty) for (int tx = 1; tx < W - 1; ++tx) {
                if (!solidTile(tx, ty)) continue;
                for (int s = 0; s < 4; ++s) {
                    int nx = tx + (s == 0 ? -1 : s == 1 ? 1 : 0), ny = ty + (s == 2 ? -1 : s == 3 ? 1 : 0);
                    if (solidTile(nx, ny)) continue;
                    for (float gap : {0.f, 2.f, 4.6f}) {
                        float bx, by;   // player box top-left, shooting toward the tile (dir index: s==0 -> right(3), 1 -> left(2), 2 -> down(0), 3 -> up(1))
                        int dir = s == 0 ? 3 : s == 1 ? 2 : s == 2 ? 0 : 1;
                        if (s == 0) { bx = tx * T - gc::PLAYER_BOX - gap; by = ty * T + o; }
                        else if (s == 1) { bx = (tx + 1) * T + gap; by = ty * T + o; }
                        else if (s == 2) { bx = tx * T + o; by = ty * T - gc::PLAYER_BOX - gap; }
                        else { bx = tx * T + o; by = (ty + 1) * T + gap; }
                        float px = bx - o, py = by - o;
                        float sx = px + 8 + DVX[dir] * 12.f, sy = py + 8 + DVY[dir] * 12.f;
                        float dist, rd; bool hw, rw, bo; simulate(sx, sy, dir, dt, dist, hw, bo); reference(sx, sy, dir, rd, rw);
                        ++total;
                        // tunnel: the sim projectile went further than one step past where the fine reference stops
                        if (dist > rd + gc::PROJECTILE_SPEED * dt + 0.5f) { ++tunnel; if (dist - rd > worst) { worst = dist - rd; wx = tx; wy = ty; } }
                    }
                }
            }
            char nm[90], det[140]; std::snprintf(nm, sizeof nm, "C1 projectile never passes a solid (dt=%.4f)", dt);
            std::snprintf(det, sizeof det, "(worst overshoot %.1fpx at %d,%d; spawn shots stopped by water=%ld land=%ld)", worst, wx, wy, water, land);
            check(nm, tunnel, total, det, dt > 0.0501f);
            (void)boundsHit;
        }
        // shots from sampled reachable spots over the whole map, dt .05 and .1
        std::mt19937 rng(7); std::uniform_real_distribution<float> U(0, 1);
        for (float dt : {0.05f, 0.1f}) {
            long tun = 0, tot = 0, wat = 0, landh = 0;
            for (int n = 0; n < 30000; ++n) {
                float px = U(rng) * W * T, py = U(rng) * H * T; if (playerBlocked(px, py)) continue;
                int d = n & 3; float sx = px + 8 + DVX[d] * 12.f, sy = py + 8 + DVY[d] * 12.f;
                float dist, rd; bool hw, rw, bo; simulate(sx, sy, d, dt, dist, hw, bo); reference(sx, sy, d, rd, rw);
                ++tot; (rw ? wat : landh)++;
                if (dist > rd + gc::PROJECTILE_SPEED * dt + 0.5f) ++tun;
            }
            char nm[90], det[90]; std::snprintf(nm, sizeof nm, "C2 random shots across map do not tunnel (dt=%.2f)", dt);
            std::snprintf(det, sizeof det, "(shots ending at water=%ld, at trees/objects=%ld)", wat, landh);
            check(nm, tun, tot, det, dt > 0.0501f);
        }
        std::printf("INFO server never uses PROJECTILE_LIFE (projectiles only die on solid/bounds/player): max travel across map = %.0f px\n", std::sqrt((float)W * T * W * T + (float)H * T * H * T));
        std::printf("INFO water IS solid for projectiles (server uses solidBox) -> shots stop at ponds/rivers.  hit circle %.0fpx vs player sprite 16px\n", gc::PLAYER_HIT_R + gc::PROJECTILE_R);
    }
    std::printf("\n%s (%d failing checks)\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
    return 0;
}
