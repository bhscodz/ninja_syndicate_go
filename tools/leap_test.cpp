// Headless leap / secret-passage / low-obstacle test using the REAL gcmap code (no SFML, no window).
// Build:  export PATH=/c/msys64/ucrt64/bin:$PATH
//         g++ -std=c++17 -O2 -I src/game tools/leap_test.cpp -o scratch_w/leap_test.exe
// Run:    scratch_w/leap_test.exe
// px,py passed to leapLanding are the player SPRITE top-left (same convention as playerBlocked and the server).
#include <cstdio>
#include <cmath>
#include <vector>
#include <queue>
#include <random>
#include <map>
#include <string>
#include "Map.hpp"

using namespace gcmap;
static const int T = gc::TILE, W = gc::MAP_W, H = gc::MAP_H;
static int g_fail = 0;
static void check(const char* name, long fails, long total, const std::string& detail = "") {
    std::printf("%s %-62s fails=%ld / %ld %s\n", fails ? "FAIL" : "PASS", name, fails, total, detail.c_str());
    if (fails) ++g_fail;
}
static const int DX[4] = {0, 0, -1, 1}, DY[4] = {1, -1, 0, 0};   // gc::FACE_* order: down, up, left, right
static const int MIRX[4] = {0, 1, 3, 2};                          // dir after mirroring x
static bool pathHasHard(float px, float py, int dir, float d) {
    const float o = boxOffset();
    for (float s = 0; s <= d + 0.001f; s += 2.f)
        if (hardBox(px + DX[dir] * s + o, py + DY[dir] * s + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) return true;
    return false;
}
// does the straight path cross any flyOver tile (i.e. is the leap actually useful vs walking)?  returns tiles crossed (0 = none)
static int crossedWidth(float px, float py, int dir, float d) {
    const float o = boxOffset();
    int w = 0, last = -1;
    for (float s = 0; s <= d + 0.001f; s += 2.f) {
        float cx = px + o + gc::PLAYER_BOX * 0.5f + DX[dir] * s, cy = py + o + gc::PLAYER_BOX * 0.5f + DY[dir] * s;
        int tx = (int)std::floor(cx / T), ty = (int)std::floor(cy / T);
        int id = tx * 1000 + ty;
        if (flyOver(tx, ty) && id != last) { ++w; last = id; }
    }
    return w;
}

int main() {
    generate();
    int nlow = 0, nsec = 0;
    for (int i = 0; i < W * H; ++i) { if (world::SOLID[i] == 3) ++nlow; if (world::SECRET[i]) ++nsec; }
    std::printf("map %dx%d: Low cells %d, secret cells %d, passages %d, LEAP_DIST %.0f carrier %.0f min %.0f\n", W, H, nlow, nsec,
                world::NUM_PASSAGES, gc::LEAP_DIST, gc::LEAP_DIST_CARRIER, gc::LEAP_MIN);
    check("Low tiles exist", nlow == 0, 1);

    // ---- L1 validity of every landing from every walkable cell centre and a random sub-tile sample
    std::mt19937 rng(7);
    std::vector<std::pair<float, float>> pts;
    for (int ty = 0; ty < H; ++ty) for (int tx = 0; tx < W; ++tx) if (!solidTile(tx, ty)) pts.push_back({(float)tx * T, (float)ty * T});
    size_t ncell = pts.size();
    for (int i = 0; i < 60000; ++i) {
        float x = std::uniform_real_distribution<float>(0, W * T - T)(rng), y = std::uniform_real_distribution<float>(0, H * T - T)(rng);
        x = std::floor(x * 4.f) / 4.f; y = std::floor(y * 4.f) / 4.f;   // exact binary fractions: mirror images are exact
        if (!playerBlocked(x, y)) pts.push_back({x, y});
    }
    long bad = 0, total = 0, nonzero[2] = {0, 0};
    std::map<int, long> byWidth[2];
    const float maxD[2] = {gc::LEAP_DIST, gc::LEAP_DIST_CARRIER};
    for (int v = 0; v < 2; ++v)
        for (auto& p : pts) for (int d = 0; d < 4; ++d) {
            float L = leapLanding(p.first, p.second, d, maxD[v]);
            ++total;
            if (L == 0.f) continue;
            if (L < gc::LEAP_MIN || L > maxD[v] + 0.001f || std::fmod(L, 2.f) != 0.f) { ++bad; continue; }
            float lx = p.first + DX[d] * L, ly = p.second + DY[d] * L;
            if (playerBlocked(lx, ly)) { ++bad; continue; }
            if (pathHasHard(p.first, p.second, d, L)) { ++bad; continue; }
            ++nonzero[v];
            if (v == 0 || true) byWidth[v][crossedWidth(p.first, p.second, d, L)]++;
        }
    check("L1 every landing is non-solid, <= maxDist, even px, no hard tile crossed", bad, total,
          "(non-zero: normal " + std::to_string(nonzero[0]) + ", carrier " + std::to_string(nonzero[1]) + ")");

    // ---- L2 mirror property (x and y), exact to 0.01 px
    long mbad = 0, mtot = 0;
    for (int v = 0; v < 2; ++v)
        for (auto& p : pts) for (int d = 0; d < 4; ++d) {
            float a = leapLanding(p.first, p.second, d, maxD[v]);
            float mx = W * T - T - p.first, my = H * T - T - p.second;
            float bx = leapLanding(mx, p.second, MIRX[d], maxD[v]);
            float by = leapLanding(p.first, my, d < 2 ? 1 - d : d, maxD[v]);
            mtot += 2;
            if (std::fabs(a - bx) > 0.01f) { ++mbad; if (mbad < 6) std::printf("  mx fail p=(%.3f,%.3f) d=%d a=%.1f b=%.1f v=%d\n", p.first, p.second, d, a, bx, v); }
            if (std::fabs(a - by) > 0.01f) ++mbad;
        }
    check("L2 mirror property (x / y mirrored position + direction -> same distance)", mbad, mtot);

    // ---- L3 useful leaps: from walkable cells, the landing crosses water / Low (cannot simply be walked)
    for (int v = 0; v < 2; ++v) {
        long useful = 0; std::map<int, long> wd;
        std::vector<char> hasLeap(W * H, 0);
        for (size_t i = 0; i < ncell; ++i) for (int d = 0; d < 4; ++d) {
            float L = leapLanding(pts[i].first, pts[i].second, d, maxD[v]);
            if (L == 0.f) continue;
            int cw = crossedWidth(pts[i].first, pts[i].second, d, L);
            if (cw > 0) { ++useful; wd[cw]++; hasLeap[(int)(pts[i].second / T) * W + (int)(pts[i].first / T)] = 1; }
        }
        long cells = 0; for (char c : hasLeap) cells += c;
        std::string det;
        for (auto& kv : wd) det += " crossing-" + std::to_string(kv.first) + "-tiles:" + std::to_string(kv.second);
        std::printf("INFO L3 %s: useful cell-dir leaps %ld, start cells with a useful leap %ld,%s\n", v ? "carrier" : "normal", useful, cells, det.c_str());
        check(v ? "L3 carriers have useful leaps" : "L3 normal player has useful leaps", useful == 0, 1);
    }

    // ---- L4 specific features: low obstacles block walking and shots but are not hard; water same
    {
        long f = 0, t = 0;
        for (int ty = 0; ty < H; ++ty) for (int tx = 0; tx < W; ++tx) {
            uint8_t s = world::SOLID[ty * W + tx];
            ++t;
            bool blockWalk = playerBlocked(tx * T, ty * T), shot = solidBox(tx * T + T / 2 - gc::PROJECTILE_R, ty * T + T / 2 - gc::PROJECTILE_R, gc::PROJECTILE_R * 2, gc::PROJECTILE_R * 2);
            if (s == 3 && !(solidTile(tx, ty) && flyOver(tx, ty) && !hardTile(tx, ty) && blockWalk && shot)) ++f;
            if (s == 2 && !(solidTile(tx, ty) && flyOver(tx, ty) && !hardTile(tx, ty) && shot)) ++f;
            if (s == 1 && !(solidTile(tx, ty) && !flyOver(tx, ty) && hardTile(tx, ty) && shot)) ++f;
            if (s == 0 && (solidTile(tx, ty) || hardTile(tx, ty))) ++f;
        }
        check("L4 tile classes: Low/water blocked+flyable, Tree hard, projectile blocked by all solids", f, t);
    }

    // ---- L5 leap never starts a landing inside a SECRET-less solid and leaps over Low fence really exist
    {
        long lowLeaps = 0;
        for (size_t i = 0; i < ncell; ++i) for (int d = 0; d < 4; ++d) {
            float L = leapLanding(pts[i].first, pts[i].second, d, gc::LEAP_DIST);
            if (L == 0.f) continue;
            const float o = boxOffset();
            for (float s = 0; s <= L; s += 2.f) {
                int tx = (int)std::floor((pts[i].first + o + 5 + DX[d] * s) / T), ty = (int)std::floor((pts[i].second + o + 5 + DY[d] * s) / T);
                if (tileAt(tx, ty) == Low) { ++lowLeaps; break; }
            }
        }
        std::printf("INFO L5 cell-dir leaps crossing a Low obstacle: %ld\n", lowLeaps);
        check("L5 Low obstacles can be leapt", lowLeaps == 0, 1);
    }

    // ---- S1 secret passages: symmetric, walkable, bases connected with secret cells treated as solid, ends connected
    {
        long f = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            bool s = secretAt(x, y) > 0;
            if (s != (secretAt(W - 1 - x, y) > 0) || s != (secretAt(x, H - 1 - y) > 0)) ++f;
            if (s && solidTile(x, y)) ++f;
        }
        check("S1 SECRET symmetric and walkable", f, W * H);
        std::vector<char> seen(W * H, 0); std::queue<std::pair<int, int>> q;
        auto& b0 = bases()[0]; q.push({b0.sx, b0.sy}); seen[b0.sy * W + b0.sx] = 1;
        while (!q.empty()) {
            auto [x, y] = q.front(); q.pop();
            for (int k = 0; k < 4; ++k) {
                int nx = x + DX[k], ny = y + DY[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H || seen[ny * W + nx] || solidTile(nx, ny) || secretAt(nx, ny)) continue;
                seen[ny * W + nx] = 1; q.push({nx, ny});
            }
        }
        long unreach = 0;
        for (auto& b : bases()) { if (!seen[b.sy * W + b.sx]) ++unreach; if (!seen[b.ty * W + b.tx]) ++unreach; }
        for (int i = 0; i < world::NUM_PASSAGES; ++i) {
            auto& p = world::PASSAGES[i];
            if (!seen[p.ay * W + p.ax] || !seen[p.by * W + p.bx]) ++unreach;
        }
        check("S2 bases + passage entrances connected with secret cells solid", unreach, 8 + world::NUM_PASSAGES * 2);
        long tooClose = 0;
        for (int i = 0; i < world::NUM_PASSAGES; ++i) for (auto& b : bases()) {
            auto& p = world::PASSAGES[i];
            for (auto c : {std::make_pair((int)p.ax, (int)p.ay), std::make_pair((int)p.bx, (int)p.by)}) {
                if (std::max(std::abs(c.first - b.tx), std::abs(c.second - b.ty)) < 8) ++tooClose;
                if (std::max(std::abs(c.first - b.sx), std::abs(c.second - b.sy)) < 8) ++tooClose;
            }
        }
        check("S3 passage entrances >= 8 tiles from every treasure/spawn", tooClose, world::NUM_PASSAGES * 16);
    }
    std::printf(g_fail ? "\n%d FAILING CHECKS\n" : "\nALL LEAP CHECKS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
