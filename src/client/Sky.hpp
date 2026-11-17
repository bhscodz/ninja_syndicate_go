// Drifting pixel-art clouds + their ground shadows. Header-only, procedural textures (no assets).
//  drawShadows(): call with the game view active, right after the ground tiles (below props/players)
//  drawClouds() : call after props/players, before the fog and HUD
#pragma once
#include "../game/Constants.hpp"

#include <SFML/Graphics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sky {

class Sky {
public:
    static constexpr int NTEX = 5, NCLOUDS = 60, TW = 64, TH = 32;

    Sky() {
        uint32_t seed = 90210;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
        for (int k = 0; k < NTEX; ++k) {
            sf::Image cl(sf::Vector2u(TW, TH), sf::Color(0, 0, 0, 0)), sh = cl;
            std::vector<uint8_t> m(TW * TH, 0);
            const int lumps = 5 + (int)(rnd() * 3);
            for (int i = 0; i < lumps; ++i) {
                float t = (i + 0.5f) / lumps;
                float cx = 8.f + t * (TW - 16.f), cy = TH - 9.f - std::sin(t * 3.14159f) * (5.f + rnd() * 5.f);
                float r = 5.f + std::sin(t * 3.14159f) * 4.f + rnd() * 3.f;
                for (int y = 0; y < TH; ++y)
                    for (int x = 0; x < TW; ++x) {
                        float dx = x - cx, dy = (y - cy) * 1.15f;
                        if (dx * dx + dy * dy <= r * r && y <= TH - 5) m[y * TW + x] = 1;
                    }
            }
            for (int y = 0; y < TH; ++y)
                for (int x = 0; x < TW; ++x) {
                    if (!m[y * TW + x]) continue;
                    bool top = y == 0 || !m[(y - 1) * TW + x];
                    bool low = y + 3 >= TH || !m[std::min(TH - 1, y + 3) * TW + x];
                    sf::Color c = top ? sf::Color(255, 255, 255) : low ? sf::Color(206, 220, 240) : sf::Color(238, 244, 252);
                    cl.setPixel(sf::Vector2u(x, y), c);
                    sh.setPixel(sf::Vector2u(x, y), sf::Color::White);
                }
            (void)cloud_[k].loadFromImage(cl); (void)shadow_[k].loadFromImage(sh);
            cloud_[k].setSmooth(false); shadow_[k].setSmooth(false);
        }
        for (auto& c : c_) {
            c.x = rnd() * SPAN; c.y = rnd() * (gc::MAP_H * gc::TILE + 300.f) - 150.f;
            c.tex = (int)(rnd() * NTEX) % NTEX; c.scale = 1.f + std::floor(rnd() * 3.f);
            c.speed = 5.f + rnd() * 6.f; c.alpha = 90.f + rnd() * 40.f;
        }
    }

    void setCover(float c) { cover_ = std::clamp(c, 0.f, 1.f); }   // 0 fair weather .. 1 heavy rain: more, darker, denser clouds
    void drawShadows(sf::RenderTarget& rt, const sf::FloatRect& view, float time) { draw(rt, view, time, true); }
    void drawClouds(sf::RenderTarget& rt, const sf::FloatRect& view, float time)  { draw(rt, view, time, false); }

private:
    struct C { float x, y, scale, speed, alpha; int tex; };
    static constexpr float MARGIN = 320.f;
    static constexpr float SPAN = gc::MAP_W * gc::TILE + 2 * MARGIN;
    sf::Texture cloud_[NTEX], shadow_[NTEX];
    C c_[NCLOUDS];
    float cover_ = 0.f;

    void draw(sf::RenderTarget& rt, const sf::FloatRect& view, float time, bool shadows) {
        const int active = 34 + (int)((NCLOUDS - 34) * cover_);
        for (int ci = 0; ci < active; ++ci) {
            const auto& c = c_[ci];
            float x = std::fmod(c.x + c.speed * time, SPAN);
            if (x < 0) x += SPAN;
            x -= MARGIN;
            float y = c.y + std::sin(time * 0.05f + c.x) * 6.f;
            float w = TW * c.scale, h = TH * c.scale;
            float ox = shadows ? 24.f + c.scale * 6.f : 0.f, oy = shadows ? 36.f + c.scale * 8.f : 0.f;
            float px = std::round(x + ox), py = std::round(y + oy);
            if (px > view.position.x + view.size.x || px + w < view.position.x ||
                py > view.position.y + view.size.y || py + h < view.position.y) continue;
            sf::Sprite sp(shadows ? shadow_[c.tex] : cloud_[c.tex]);
            sp.setScale({ c.scale, c.scale });
            sp.setPosition({ px, py });
            if (shadows) sp.setColor(sf::Color(20, 30, 60, (uint8_t)(52 + 40 * cover_)));
            else { uint8_t g = (uint8_t)(255 - 95 * cover_); sp.setColor(sf::Color(g, (uint8_t)(g + 4), (uint8_t)(g + 14 > 255 ? 255 : g + 14), (uint8_t)std::min(235.f, c.alpha * (1.f + 0.9f * cover_)))); }
            rt.draw(sp);
        }
    }
};

} // namespace sky
