// Client-side fog of war + minimap. Header-only; owned by the map/camera work.
//  - explored[] : one bool per tile, revealed in a radius around the local player
//  - draw()     : soft-edged dark overlay (vertex array, tile resolution, world space)
//  - visible()  : is a world position inside the currently-visible radius?
//  - drawMinimap(): explored-only minimap (1 px per tile texture, updated when tiles are revealed)
#pragma once
#include "../game/Constants.hpp"
#include "../game/WorldMap.hpp"

#include <SFML/Graphics.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fog {

constexpr float REVEAL_TILES = 12.f;                // reveal / vision radius in tiles
constexpr float SOFT_TILES   = 4.f;                 // width of the soft visible edge
constexpr uint8_t DIM_ALPHA  = 80;                  // explored but not visible (light white haze)
constexpr uint8_t DARK_ALPHA = 218;                 // never seen (white mist, terrain faintly visible)

// Minimap marker for a trophy (world pixel centre). kind: 0 home cup, 1 carried (blinks, shown even in unexplored area), 2 dropped (ring)
struct MapMarker { float x = 0, y = 0; int kind = 0; sf::Color col; };

class Fog {
public:
    static constexpr int W = gc::MAP_W, H = gc::MAP_H;

    Fog() : explored_(W * H, 0), mmImg_(sf::Vector2u(W, H), sf::Color(0, 0, 0, 0)) {
        (void)mmTex_.resize(sf::Vector2u(W, H));
        mmTex_.setSmooth(false);
        mmTex_.update(mmImg_);
    }

    // Reveal around a world position (pixel centre of the local player).
    void reveal(sf::Vector2f c) {
        cx_ = c.x; cy_ = c.y; have_ = true;
        const int r = (int)std::ceil(REVEAL_TILES) + 1;
        const int tx = (int)(c.x / gc::TILE), ty = (int)(c.y / gc::TILE);
        const float rr = REVEAL_TILES * gc::TILE;
        for (int y = std::max(0, ty - r); y <= std::min(H - 1, ty + r); ++y)
            for (int x = std::max(0, tx - r); x <= std::min(W - 1, tx + r); ++x) {
                float dx = (x + 0.5f) * gc::TILE - c.x, dy = (y + 0.5f) * gc::TILE - c.y;
                if (dx * dx + dy * dy <= rr * rr && !explored_[y * W + x]) {
                    explored_[y * W + x] = 1;
                    mmImg_.setPixel(sf::Vector2u(x, y), tileColor(x, y));
                    dirty_ = true;
                }
            }
    }

    void setTime(float t) { time_ = t; }

    void reset() {
        std::fill(explored_.begin(), explored_.end(), 0);
        mmImg_ = sf::Image(sf::Vector2u(W, H), sf::Color(0, 0, 0, 0));
        mmTex_.update(mmImg_);
        dirty_ = false;
        std::fill(discovered_.begin(), discovered_.end(), 0);
    }

    // Secret passages found by the local player (reset together with the fog at match start).
    bool discovered(int id) const { return id > 0 && id < 256 && discovered_[id]; }
    bool discover(int id) {   // returns true the first time
        if (id <= 0 || id >= 256 || discovered_[id]) return false;
        discovered_[id] = 1; return true;
    }

    // Is a world position inside the currently visible radius?
    bool visible(float x, float y) const {
        if (!have_) return true;
        float dx = x - cx_, dy = y - cy_;
        return dx * dx + dy * dy <= REVEAL_TILES * gc::TILE * REVEAL_TILES * gc::TILE;
    }

    // Draw the fog over the world (call with the game view active, after props/players).
    void draw(sf::RenderTarget& rt, sf::FloatRect view) {
        if (!have_) return;
        const int x0 = std::max(0, (int)std::floor(view.position.x / gc::TILE) - 1);
        const int y0 = std::max(0, (int)std::floor(view.position.y / gc::TILE) - 1);
        const int x1 = std::min(W - 1, (int)std::floor((view.position.x + view.size.x) / gc::TILE) + 1);
        const int y1 = std::min(H - 1, (int)std::floor((view.position.y + view.size.y) / gc::TILE) + 1);
        const int vw = x1 - x0 + 2;                       // vertex grid width
        const int vh = y1 - y0 + 2;
        vert_.assign((size_t)vw * vh, 0);
        for (int vy = 0; vy < vh; ++vy)
            for (int vx = 0; vx < vw; ++vx) vert_[vy * vw + vx] = vertexAlpha(x0 + vx, y0 + vy);

        quads_.clear();
        const sf::Color base(238, 243, 250);
        for (int ty = y0; ty <= y1; ++ty)
            for (int tx = x0; tx <= x1; ++tx) {
                uint8_t a00 = vert_[(ty - y0) * vw + (tx - x0)];
                uint8_t a10 = vert_[(ty - y0) * vw + (tx - x0 + 1)];
                uint8_t a01 = vert_[(ty - y0 + 1) * vw + (tx - x0)];
                uint8_t a11 = vert_[(ty - y0 + 1) * vw + (tx - x0 + 1)];
                if (!(a00 | a10 | a01 | a11)) continue;
                float px = float(tx * gc::TILE), py = float(ty * gc::TILE), s = float(gc::TILE);
                auto V = [&](float x, float y, uint8_t a) { return sf::Vertex{ {x, y}, sf::Color(base.r, base.g, base.b, a), {} }; };
                sf::Vertex v00 = V(px, py, a00), v10 = V(px + s, py, a10), v01 = V(px, py + s, a01), v11 = V(px + s, py + s, a11);
                quads_.push_back(v00); quads_.push_back(v10); quads_.push_back(v01);
                quads_.push_back(v10); quads_.push_back(v11); quads_.push_back(v01);
            }
        if (!quads_.empty()) rt.draw(quads_.data(), quads_.size(), sf::PrimitiveType::Triangles);
    }

    // Minimap in HUD space. pos = top-left, scale = pixels per tile.
    void drawMinimap(sf::RenderTarget& rt, sf::Vector2f pos, float scale, const sf::Color teamCol[4],
                     sf::Vector2f playerPx, sf::Color playerCol, sf::FloatRect camRect, float time,
                     const std::vector<MapMarker>* markers = nullptr) {
        if (dirty_) { mmTex_.update(mmImg_); dirty_ = false; }
        const float w = W * scale, h = H * scale;
        sf::RectangleShape frame({ w + 6.f, h + 6.f });
        frame.setPosition({ pos.x - 3.f, pos.y - 3.f });
        frame.setFillColor(sf::Color(226, 233, 244, 245));
        frame.setOutlineColor(sf::Color(150, 104, 58));
        frame.setOutlineThickness(2.f);
        rt.draw(frame);
        sf::Sprite sp(mmTex_);
        sp.setPosition(pos);
        sp.setScale({ scale, scale });
        rt.draw(sp);
        // camera window
        sf::RectangleShape cam({ camRect.size.x / gc::TILE * scale, camRect.size.y / gc::TILE * scale });
        cam.setPosition({ pos.x + camRect.position.x / gc::TILE * scale, pos.y + camRect.position.y / gc::TILE * scale });
        cam.setFillColor(sf::Color::Transparent);
        cam.setOutlineColor(sf::Color(30, 40, 70, 170));
        cam.setOutlineThickness(1.f);
        rt.draw(cam);
        // base markers
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& b = world::BASES[i];
            sf::RectangleShape m({ b.cw * scale * 0.5f + 4.f, b.ch * scale * 0.5f + 4.f });
            m.setOrigin(m.getSize() * 0.5f);
            m.setPosition({ pos.x + (b.cx + b.cw * 0.5f) * scale, pos.y + (b.cy + b.ch * 0.5f) * scale });
            m.setFillColor(sf::Color(teamCol[i].r, teamCol[i].g, teamCol[i].b, 200));
            m.setOutlineColor(sf::Color::Black);
            m.setOutlineThickness(1.f);
            rt.draw(m);
        }
        // discovered secret passages: entrance dots
        for (int i = 0; i < world::NUM_PASSAGES; ++i) {
            const auto& pa = world::PASSAGES[i];
            if (!discovered(pa.id)) continue;
            for (int e = 0; e < 2; ++e) {
                sf::CircleShape d(2.f, 8);
                d.setOrigin({ 2.f, 2.f });
                d.setPosition({ pos.x + ((e ? pa.bx : pa.ax) + 0.5f) * scale, pos.y + ((e ? pa.by : pa.ay) + 0.5f) * scale });
                d.setFillColor(sf::Color(120, 255, 150));
                d.setOutlineColor(sf::Color(20, 70, 30));
                d.setOutlineThickness(1.f);
                rt.draw(d);
            }
        }
        // trophy markers
        if (markers) for (const auto& m : *markers) {
            const sf::Vector2f c{ pos.x + m.x / gc::TILE * scale, pos.y + m.y / gc::TILE * scale };
            if (m.kind == 0) {   // home cup: small square
                sf::RectangleShape q({ 4.f, 4.f }); q.setOrigin({ 2.f, 2.f }); q.setPosition(c);
                q.setFillColor(sf::Color(m.col.r, m.col.g, m.col.b, 230)); q.setOutlineColor(sf::Color(255, 240, 160)); q.setOutlineThickness(1.f);
                rt.draw(q);
            } else if (m.kind == 1) {   // carried: blinking trophy dot
                if (std::fmod(time, 0.6f) < 0.38f) {
                    sf::CircleShape d(5.f, 14); d.setOrigin({ 5.f, 5.f }); d.setPosition(c);
                    d.setFillColor(m.col); d.setOutlineColor(sf::Color(255, 235, 120)); d.setOutlineThickness(2.f);
                    rt.draw(d);
                }
            } else {   // dropped: ring
                float r = 6.f + 2.f * std::sin(time * 8.f);
                sf::CircleShape d(r, 16); d.setOrigin({ r, r }); d.setPosition(c);
                d.setFillColor(sf::Color(m.col.r, m.col.g, m.col.b, 90)); d.setOutlineColor(m.col); d.setOutlineThickness(2.f);
                rt.draw(d);
            }
        }
        // local player
        float blink = 0.6f + 0.4f * std::sin(time * 6.f);
        sf::CircleShape dot(4.f, 12);
        dot.setOrigin({ 4.f, 4.f });
        dot.setPosition({ pos.x + playerPx.x / gc::TILE * scale, pos.y + playerPx.y / gc::TILE * scale });
        dot.setFillColor(sf::Color(playerCol.r, playerCol.g, playerCol.b, (uint8_t)(255 * blink)));
        dot.setOutlineColor(sf::Color::White);
        dot.setOutlineThickness(1.5f);
        rt.draw(dot);
    }

private:
    std::vector<uint8_t> explored_;
    std::array<uint8_t, 256> discovered_{};
    std::vector<uint8_t> vert_;
    std::vector<sf::Vertex> quads_;
    sf::Image mmImg_;
    sf::Texture mmTex_;
    bool dirty_ = false, have_ = false;
    float time_ = 0.f;
    float cx_ = 0, cy_ = 0;

    float exploredAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= W || y >= H) return 1.f;   // outside the map: treat as explored
        return explored_[y * W + x] ? 1.f : 0.f;
    }
    // alpha at the tile-corner vertex (vx,vy): average explored of the 4 adjacent tiles, softened by distance
    uint8_t vertexAlpha(int vx, int vy) const {
        float e = (exploredAt(vx - 1, vy - 1) + exploredAt(vx, vy - 1) + exploredAt(vx - 1, vy) + exploredAt(vx, vy)) * 0.25f;
        float base = DARK_ALPHA + (DIM_ALPHA - (float)DARK_ALPHA) * e;
        float dx = vx * (float)gc::TILE - cx_, dy = vy * (float)gc::TILE - cy_;
        float d = std::sqrt(dx * dx + dy * dy) / gc::TILE;
        float t = std::clamp((d - (REVEAL_TILES - SOFT_TILES)) / SOFT_TILES, 0.f, 1.f);
        t = t * t * (3.f - 2.f * t);
        // drifting wisps: gentle moving brightness variation in the mist
        float wx = vx * 0.23f + time_ * 0.35f, wy = vy * 0.27f - time_ * 0.2f;
        float n = 0.5f * std::sin(wx * 1.7f + std::sin(wy * 1.3f + time_ * 0.25f)) + 0.5f * std::sin(wy * 2.1f - wx * 0.8f + 1.3f);
        float wisp = 1.f - (1.f - e) * 0.10f * (n + 1.f) + e * 0.12f * n;
        return (uint8_t)std::clamp((int)std::lround(base * t * wisp), 0, 255);
    }

    static sf::Color tileColor(int x, int y) {
        const int i = y * world::W + x;
        const uint16_t g = world::GROUND[i], o = world::OVERLAY[i];
        if (world::SOLID[i] == 2) return { 70, 150, 215 };
        if (o != world::NONE && (o >> 12) == 1 && ((o >> 6) & 63) == 13) return { 205, 140, 70 };   // bridge planks
        if (world::SOLID[i] == 1) return { 44, 78, 50 };
        const int sheet = g >> 12, row = (g >> 6) & 63, col = g & 63;
        (void)col;
        if (sheet == 0 && row >= 7 && row <= 10 && col <= 2) return { 176, 112, 80 };   // dirt
        switch (world::BIOME[i]) {
            case 1: return { 232, 196, 130 };
            case 2: return { 236, 238, 246 };
            default: return { 118, 164, 62 };
        }
    }
};

} // namespace fog
