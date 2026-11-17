// Shared map model: grid generation, base layout and tile collision.
// No SFML graphics dependency so the server can reuse it.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include "Constants.hpp"
#include "Rules.hpp"
#include "WorldMap.hpp"

namespace gcmap {
enum Tile : uint8_t {
    Grass = 0,
    Tree  = 1,   // solid blocker, drawn as a 2x2 nature composite
    Water = 2,   // solid blocker
    Flower= 3,   // decoration, walkable
    Low   = 4,   // low obstacle (fence / short wall): blocks walking and shots but can be LEAPT over
};

struct BaseInfo {
    int   owner = 0;       // which player owns this base (0..3)
    int   tx = 0, ty = 0;   // treasure tile (home of the item)
    float treasureX = 0, treasureY = 0; // pixel centre of the treasure tile
    int   sx = 0, sy = 0;   // spawn tile for the owning player
    float spawnX = 0, spawnY = 0;       // pixel top-left for a 16x16 sprite
    int   cx = 0, cy = 0, cw = 0, ch = 0; // clearing rect (tiles) for the coloured floor
};

inline std::array<uint8_t, gc::MAP_W * gc::MAP_H>& grid() {
    static std::array<uint8_t, gc::MAP_W * gc::MAP_H> g{};
    return g;
}

inline std::array<BaseInfo, gc::NUM_PLAYERS>& bases() {
    static std::array<BaseInfo, gc::NUM_PLAYERS> b{};
    return b;
}

inline uint8_t tileAt(int x, int y) {
    if (x < 0 || y < 0 || x >= gc::MAP_W || y >= gc::MAP_H) return Tree;
    return grid()[y * gc::MAP_W + x];
}

inline bool solidTile(int x, int y) {
    uint8_t t = tileAt(x, y);
    return t == Tree || t == Water || t == Low;
}
// hard blockers a leap can never cross (trees, buildings, border); water and low obstacles can be flown over
inline bool hardTile(int x, int y) { return tileAt(x, y) == Tree; }
inline bool flyOver(int x, int y) { uint8_t t = tileAt(x, y); return t == Water || t == Low; }
inline uint8_t secretAt(int x, int y) {   // hidden passage id (0 = none); the cell is walkable
    if (x < 0 || y < 0 || x >= gc::MAP_W || y >= gc::MAP_H) return 0;
    return world::SECRET[y * gc::MAP_W + x];
}

// Axis-aligned box test against solid tiles.
inline bool solidBox(float px, float py, float w, float h) {
    // far edges use an epsilon instead of "-1": a box that overlaps a solid tile by even a fraction of a
    // pixel is blocked (with -1 the box could sink up to 1px into the east/south side of every wall)
    int l = static_cast<int>(std::floor(px / gc::TILE));
    int r = static_cast<int>(std::floor((px + w - 0.001f) / gc::TILE));
    int t = static_cast<int>(std::floor(py / gc::TILE));
    int b = static_cast<int>(std::floor((py + h - 0.001f) / gc::TILE));
    for (int y = t; y <= b; ++y)
        for (int x = l; x <= r; ++x)
            if (solidTile(x, y)) return true;
    return false;
}

// Player position is the top-left of a 16x16 sprite; collision box is inset.
inline float boxOffset() { return (gc::TILE - gc::PLAYER_BOX) * 0.5f; }

inline bool playerBlocked(float x, float y) {
    float o = boxOffset();
    return solidBox(x + o, y + o, gc::PLAYER_BOX, gc::PLAYER_BOX);
}

// Leap landing: along facing dir (0 down,1 up,2 left,3 right) find the longest landing distance <= maxDist (px) such that the
// player box fits at the landing point and no HARD tile (tree/building) lies between; water and Low obstacles may be crossed.
// px,py = player SPRITE top-left (like playerBlocked); the box offset is applied here. Returns the landing distance in px
// (landing sprite position = p + dir*d) or 0 when no valid landing exists.
inline bool hardBox(float px, float py, float w, float h) {
    int l = static_cast<int>(std::floor(px / gc::TILE));
    int r = static_cast<int>(std::floor((px + w - 0.001f) / gc::TILE));
    int t = static_cast<int>(std::floor(py / gc::TILE));
    int b = static_cast<int>(std::floor((py + h - 0.001f) / gc::TILE));
    for (int y = t; y <= b; ++y)
        for (int x = l; x <= r; ++x)
            if (hardTile(x, y)) return true;
    return false;
}
inline float leapLanding(float px, float py, int dir, float maxDist) {
    static const int DX[4] = {0, 0, -1, 1}, DY[4] = {1, -1, 0, 0};
    if (dir < 0 || dir > 3) return 0.f;
    const float o = boxOffset();
    const int dmax = static_cast<int>(maxDist / 2.f) * 2;
    const int dmin = static_cast<int>(gc::LEAP_MIN);
    for (int d = dmax; d >= dmin; d -= 2) {
        if (playerBlocked(px + DX[dir] * d, py + DY[dir] * d)) continue;
        bool ok = true;
        for (int s = 0; s <= d && ok; s += 2)
            if (hardBox(px + DX[dir] * s + o, py + DY[dir] * s + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) ok = false;
        if (ok) return static_cast<float>(d);
    }
    return 0.f;
}

// Build the world from the generated arena data (tools/build_world.py -> WorldMap.hpp)
inline void generate() {
    auto& g = grid();
    for (int i = 0; i < gc::MAP_W * gc::MAP_H; ++i)
        g[i] = world::SOLID[i] == 1 ? Tree : (world::SOLID[i] == 2 ? Water : (world::SOLID[i] == 3 ? Low : Grass));
    auto& b = bases();
    for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
        const auto& d = world::BASES[i];
        BaseInfo info;
        info.owner = i;
        info.tx = d.tx; info.ty = d.ty;
        info.treasureX = d.tx * gc::TILE + gc::TILE * 0.5f;
        info.treasureY = d.ty * gc::TILE + gc::TILE * 0.5f;
        info.sx = d.sx; info.sy = d.sy;
        info.spawnX = d.sx * gc::TILE;
        info.spawnY = d.sy * gc::TILE;
        info.cx = d.cx; info.cy = d.cy; info.cw = d.cw; info.ch = d.ch;
        b[i] = info;
    }
}
} // namespace gcmap
