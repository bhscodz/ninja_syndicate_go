// Shared map model: grid generation, base layout and tile collision.
// No SFML graphics dependency so the server can reuse it.
#pragma once
#include <array>
#include <cstdint>
#include "Constants.hpp"
#include "WorldMap.hpp"

namespace gcmap {
enum Tile : uint8_t {
    Grass = 0,
    Tree  = 1,   // solid blocker, drawn as a 2x2 nature composite
    Water = 2,   // solid blocker
    Flower= 3,   // decoration, walkable
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
    return t == Tree || t == Water;
}

// Axis-aligned box test against solid tiles.
inline bool solidBox(float px, float py, float w, float h) {
    int l = static_cast<int>(px / gc::TILE);
    int r = static_cast<int>((px + w - 1) / gc::TILE);
    int t = static_cast<int>(py / gc::TILE);
    int b = static_cast<int>((py + h - 1) / gc::TILE);
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

// Build the world from the generated arena data (tools/build_world.py -> WorldMap.hpp)
inline void generate() {
    auto& g = grid();
    for (int i = 0; i < gc::MAP_W * gc::MAP_H; ++i)
        g[i] = world::SOLID[i] == 1 ? Tree : (world::SOLID[i] == 2 ? Water : Grass);
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
