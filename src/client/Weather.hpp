// Client-side weather + water animation. Header-only, no server sync: the weather cycle is a pure function of the
// wall clock (unix time), so every client shows the same sky.
//
//   Weather w;  w.init(assetRoot, &rippleTexture);
//   per frame:  w.update(dt);
//               w.drawGround(window, viewRect, time);   // game view, right after the ground tiles: water ripples + puddles
//               w.drawWorld(window, viewRect, time);    // game view, after clouds: rain splashes
//               w.drawScreen(window, viewRect, time);   // after the fog, before the HUD: streaks, tint, lightning, leaves
//   F2 -> w.toggle().  GAME_WEATHER=rain|clear forces the weather (testing).
#pragma once
#include "../game/Constants.hpp"
#include "../game/WorldMap.hpp"
#include "Sound.hpp"

#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace weather {

class Weather {
public:
    Weather() {
        if (const char* e = std::getenv("GAME_WEATHER")) {
            std::string s = e;
            if (s == "rain") forced_ = 1; else if (s == "clear" || s == "off") forced_ = 0;
        }
        uint32_t seed = 4242;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
        for (auto& d : far_)  d = { rnd(), rnd(), 0.75f + rnd() * 0.5f, 0.6f + rnd() * 0.8f };
        for (auto& d : near_) d = { rnd(), rnd(), 0.75f + rnd() * 0.5f, 0.6f + rnd() * 0.8f };
        for (auto& l : leaves_) l = { rnd(), rnd(), rnd() * 6.28f, 0.7f + rnd() * 0.6f, (int)(rnd() * 3.f) };
        // tiles covered by a prop sprite: no puddles / ripples there
        covered_.assign(world::W * world::H, 0);
        for (int i = 0; i < world::NUM_OBJECTS; ++i) {
            const auto& o = world::OBJECTS[i];
            for (int y = o.ty; y < o.ty + o.h; ++y)
                for (int x = o.tx; x < o.tx + o.w; ++x)
                    if (x >= 0 && y >= 0 && x < world::W && y < world::H) covered_[y * world::W + x] = 1;
        }
    }

    void init(const std::string& root, const sf::Texture* ripple) {
        ripple_ = ripple;
        std::string dir = root;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        dir += "Audio/Sounds/Ambient/";
        rainOk_ = rain_.openFromFile(dir + "Rain2.wav");
        stormOk_ = storm_.openFromFile(dir + "Storm.wav");
        for (auto* m : { &rain_, &storm_ }) { m->setLooping(true); m->setVolume(0.f); }
    }

    void toggle() { enabled_ = !enabled_; }
    bool enabled() const { return enabled_; }
    float intensity() const { return cur_; }      // 0 clear .. 1 heavy rain (smoothed)

    void update(float dt) {
        float target = 0.f;
        if (enabled_) target = forced_ >= 0 ? (float)forced_ : cycle(unixNow());
        cur_ += (target - cur_) * std::min(1.f, dt * (forced_ >= 0 ? 0.9f : 1.5f));
        if (cur_ < 0.002f) cur_ = 0.f;
        // wet ground builds up under rain and dries slowly
        wet_ += (cur_ > 0.12f ? cur_ * 0.045f : -0.012f) * dt;
        wet_ = std::clamp(wet_, 0.f, 1.f);
        updateSplashes(dt);
        updateAudio();
        // lightning (heavy rain only): deterministic from the clock
        flash_ = 0.f;
        if (cur_ > 0.8f) {
            double t = unixNow();
            long slot = (long)std::floor(t / 13.0);
            uint32_t h = hash((uint32_t)slot, 77u);
            if (h % 5u < 2u) {
                double dtl = t - (slot * 13.0 + 2.0 + (h >> 8) % 8u);
                if (dtl >= 0.0 && dtl < 0.9)
                    flash_ = (float)(std::exp(-dtl * 7.0) * 0.8 + (dtl > 0.28 ? std::exp(-(dtl - 0.28) * 9.0) * 0.55 : 0.0)) * (cur_ - 0.7f) * 3.3f;
            }
        }
    }

    // ---- ground level: animated water + puddles (call right after the ground tile batches) ----
    void drawGround(sf::RenderTarget& rt, const sf::FloatRect& view, float time) {
        const int tx0 = std::max(0, (int)std::floor(view.position.x / gc::TILE) - 1);
        const int ty0 = std::max(0, (int)std::floor(view.position.y / gc::TILE) - 1);
        const int tx1 = std::min(world::W - 1, (int)std::floor((view.position.x + view.size.x) / gc::TILE) + 1);
        const int ty1 = std::min(world::H - 1, (int)std::floor((view.position.y + view.size.y) / gc::TILE) + 1);
        sf::Sprite sp(*ripple_);
        sf::RectangleShape glint({ 2.f, 1.f });
        sf::CircleShape pd(1.f, 14);
        for (int y = ty0; y <= ty1; ++y)
            for (int x = tx0; x <= tx1; ++x) {
                const int i = y * world::W + x;
                const uint32_t h = hash((uint32_t)x, (uint32_t)y);
                const uint16_t g = world::GROUND[i];
                const float px = float(x * gc::TILE), py = float(y * gc::TILE);
                if (world::SOLID[i] == 2) {
                    if (ripple_ == nullptr || (g >> 12) != 1 || (g & 63) > 10) continue;      // blue water tiles only (not swamp / ice)
                    if (world::OVERLAY[i] != world::NONE || covered_[i]) continue;
                    if (!inner(x, y)) continue;
                    const float ph = (h & 255) / 255.f;
                    if (h % 6u == 0u) {          // 4-frame ripple sprite at a random phase, fading in and out
                        int frame = (int)(time * 2.4f + ph * 4.f) & 3;
                        sp.setTextureRect({ { frame * 16, 0 }, { 16, 16 } });
                        sp.setPosition({ px, py });
                        sp.setColor(sf::Color(255, 255, 255, (uint8_t)(120 + 60 * std::sin(time * 1.3f + ph * 6.28f))));
                        rt.draw(sp);
                    }
                    if ((h >> 5) % 5u == 0u) {   // shimmer glints
                        float s = std::sin(time * 2.7f + ph * 19.f);
                        if (s > 0.55f) {
                            glint.setPosition({ px + 2.f + float((h >> 9) % 11), py + 2.f + float((h >> 13) % 11) });
                            glint.setFillColor(sf::Color(255, 255, 255, (uint8_t)(210 * (s - 0.55f) / 0.45f)));
                            rt.draw(glint);
                        }
                    }
                } else if (wet_ > 0.05f && world::SOLID[i] == 0 && world::OVERLAY[i] == world::NONE && !covered_[i]) {
                    const bool dirtTile = (g >> 12) == 0 && ((g >> 6) & 63) >= 7 && ((g >> 6) & 63) <= 10 && (g & 63) < 10;
                    const bool grassTile = (g >> 12) == 0 && ((g >> 6) & 63) == 12;
                    if (!(dirtTile ? (h % 9u == 0u) : grassTile ? (h % 31u == 0u) : false)) continue;
                    const float rx = 5.f + float((h >> 6) % 4u), ry = 2.5f + float((h >> 10) % 2u);
                    const uint8_t a = (uint8_t)std::min(255.f, 150.f * wet_);
                    pd.setRadius(1.f); pd.setScale({ rx, ry });
                    pd.setPosition({ px + 8.f - rx, py + 9.f - ry });
                    pd.setFillColor(sf::Color(96, 130, 170, (uint8_t)(a * 0.62f)));
                    pd.setOutlineThickness(0.18f); pd.setOutlineColor(sf::Color(190, 215, 240, (uint8_t)(a * 0.7f)));
                    rt.draw(pd);
                }
            }
    }

    // ---- world space: raindrop splashes / rings (game view active) ----
    void drawWorld(sf::RenderTarget& rt, const sf::FloatRect&, float) {
        if (splashes_.empty()) return;
        sf::CircleShape ring(1.f, 18);
        ring.setFillColor(sf::Color::Transparent);
        sf::RectangleShape dot({ 1.f, 1.f });
        for (const auto& s : splashes_) {
            float k = s.age / s.life;
            uint8_t a = (uint8_t)(190.f * (1.f - k) * std::min(1.f, cur_ * 1.6f + 0.2f));
            if (s.water) {
                float r = 1.5f + k * (s.big ? 6.5f : 4.5f);
                ring.setRadius(r); ring.setScale({ 1.f, 0.5f });
                ring.setOrigin({ r, r });
                ring.setPosition({ s.x, s.y });
                ring.setOutlineThickness(0.7f);
                ring.setOutlineColor(sf::Color(225, 240, 255, a));
                rt.draw(ring);
                if (s.big && k < 0.4f) {
                    ring.setRadius(r * 0.55f); ring.setOrigin({ r * 0.55f, r * 0.55f });
                    rt.draw(ring);
                }
            } else {
                dot.setFillColor(sf::Color(215, 232, 250, a));
                float h = k * 3.f;
                dot.setPosition({ std::round(s.x - 2.f - k * 2.f), std::round(s.y - h) }); rt.draw(dot);
                dot.setPosition({ std::round(s.x + 2.f + k * 2.f), std::round(s.y - h) }); rt.draw(dot);
                dot.setPosition({ std::round(s.x), std::round(s.y - h - 2.f) });           rt.draw(dot);
            }
        }
    }

    // ---- screen space: rain streaks, tint, lightning, drifting leaves (call after the fog, before the HUD) ----
    void drawScreen(sf::RenderTarget& rt, const sf::FloatRect& view, float time) {
        sf::View old = rt.getView();
        rt.setView(sf::View(sf::FloatRect({ 0.f, 0.f }, { SW, SH })));
        const float cx = view.position.x * 3.f, cy = view.position.y * 3.f;     // world -> screen px (3x zoom) for parallax
        if (cur_ > 0.01f) {
            // darken + blue-grey cast (multiply) and a touch of desaturation (grey wash)
            float k = std::min(1.f, cur_);
            sf::RectangleShape tint({ SW, SH });
            tint.setFillColor(sf::Color((uint8_t)(255 - 62 * k), (uint8_t)(255 - 50 * k), (uint8_t)(255 - 28 * k)));
            rt.draw(tint, sf::RenderStates(sf::BlendMultiply));
            tint.setFillColor(sf::Color(120, 135, 155, (uint8_t)(34 * k)));
            rt.draw(tint);
            verts_.clear();
            streaks(far_, (int)(NFAR * k), 0.22f, 22.f, 1.0f, 820.f, cx, cy, time, sf::Color(175, 195, 230, (uint8_t)(70 + 25 * k)));
            streaks(near_, (int)(NNEAR * k), 0.55f, 46.f, 1.7f, 1500.f, cx, cy, time, sf::Color(215, 228, 250, (uint8_t)(100 + 40 * k)));
            if (!verts_.empty()) rt.draw(verts_.data(), verts_.size(), sf::PrimitiveType::Triangles);
        }
        if (flash_ > 0.01f) {
            sf::RectangleShape f({ SW, SH });
            f.setFillColor(sf::Color(235, 240, 255, (uint8_t)std::min(120.f, 120.f * flash_)));
            rt.draw(f);
        }
        if (cur_ < 0.6f) {   // a few leaves drifting on the breeze (fewer / gone in the rain)
            const int n = (int)(NLEAF * (1.f - cur_ / 0.6f));
            static const sf::Color LC[3] = { { 120, 170, 60 }, { 210, 130, 50 }, { 160, 190, 80 } };
            sf::RectangleShape lf({ 5.f, 3.f });
            for (int i = 0; i < n; ++i) {
                const auto& l = leaves_[i];
                float x = std::fmod(l.x * SW + time * 38.f * l.sp - cx * 0.35f, SW + 80.f); if (x < 0) x += SW + 80.f;
                float y = std::fmod(l.y * SH + time * 16.f * l.sp + std::sin(time * 1.1f + l.ph) * 16.f - cy * 0.35f, SH + 60.f); if (y < 0) y += SH + 60.f;
                lf.setOrigin({ 2.5f, 1.5f });
                lf.setPosition({ x - 40.f, y - 30.f });
                lf.setRotation(sf::radians(l.ph + time * 1.5f * l.sp));
                lf.setFillColor(LC[l.kind]);
                rt.draw(lf);
            }
        }
        rt.setView(old);
    }

private:
    static constexpr float SW = 1280.f, SH = 960.f;
    static constexpr int NFAR = 230, NNEAR = 120, NLEAF = 14;
    struct Drop { float x, y, len, spd; };
    struct Leaf { float x, y, ph, sp; int kind; };
    struct Splash { float x, y, age, life; bool water, big; };

    Drop far_[NFAR], near_[NNEAR];
    Leaf leaves_[NLEAF];
    std::vector<sf::Vertex> verts_;
    std::vector<Splash> splashes_;
    std::vector<uint8_t> covered_;
    const sf::Texture* ripple_ = nullptr;
    sf::Music rain_, storm_;
    bool rainOk_ = false, stormOk_ = false, rainPlaying_ = false, stormPlaying_ = false;
    bool enabled_ = true;
    int forced_ = -1;
    float cur_ = 0.f, wet_ = 0.f, flash_ = 0.f;
    uint32_t rng_ = 1234567u;

    static double unixNow() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    static uint32_t hash(uint32_t a, uint32_t b) {
        uint32_t h = a * 374761393u + b * 668265263u + 0x9e3779b9u;
        h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        return h;
    }
    float frand() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / float(1 << 24); }

    // 10-minute cycle: clear -> light rain -> heavy rain -> clear
    static float cycle(double t) {
        const double u = std::fmod(t, 600.0);
        auto sstep = [](double a, double b, double x) { double k = std::clamp((x - a) / (b - a), 0.0, 1.0); return k * k * (3 - 2 * k); };
        if (u < 240.0) return 0.f;
        if (u < 330.0) return (float)(0.42 * sstep(240.0, 280.0, u));                            // light rain
        if (u < 450.0) return (float)(0.42 + 0.58 * sstep(330.0, 365.0, u));                     // heavy rain
        if (u < 510.0) return (float)(1.0 - sstep(450.0, 510.0, u));                             // clearing
        return 0.f;
    }

    // water tile whose 8 neighbours are water too (no shoreline art)
    static bool inner(int x, int y) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= world::W || yy >= world::H || world::SOLID[yy * world::W + xx] != 2) return false;
            }
        return true;
    }

    void streaks(const Drop* d, int n, float par, float lenBase, float w, float speed, float cx, float cy, float time, sf::Color c) {
        const float WW = SW + 240.f, HH = SH + 160.f;
        for (int i = 0; i < n; ++i) {
            float x = std::fmod(d[i].x * WW - cx * par - time * speed * 0.26f * d[i].spd, WW); if (x < 0) x += WW;
            float y = std::fmod(d[i].y * HH - cy * par + time * speed * d[i].spd, HH); if (y < 0) y += HH;
            x -= 120.f; y -= 80.f;
            float len = lenBase * d[i].len;
            sf::Vector2f a(x, y), b(x - len * 0.26f, y + len);
            sf::Vector2f nrm(0.97f * w * 0.5f, 0.25f * w * 0.5f);
            sf::Color ce = c; ce.a = (uint8_t)(c.a * 0.15f);
            verts_.push_back({ a - nrm, ce }); verts_.push_back({ a + nrm, ce }); verts_.push_back({ b - nrm, c });
            verts_.push_back({ a + nrm, ce }); verts_.push_back({ b + nrm, c }); verts_.push_back({ b - nrm, c });
        }
    }

    void updateSplashes(float dt) {
        for (auto& s : splashes_) s.age += dt;
        splashes_.erase(std::remove_if(splashes_.begin(), splashes_.end(), [](const Splash& s) { return s.age >= s.life; }), splashes_.end());
        if (cur_ < 0.03f || have_view_ == false) return;
        spawnAcc_ += dt * (30.f + 150.f * cur_);
        int n = (int)spawnAcc_; spawnAcc_ -= n;
        for (int k = 0; k < n && splashes_.size() < 260; ++k) {
            float x = viewR_.position.x + frand() * viewR_.size.x, y = viewR_.position.y + frand() * viewR_.size.y;
            int tx = (int)(x / gc::TILE), ty = (int)(y / gc::TILE);
            if (tx < 0 || ty < 0 || tx >= world::W || ty >= world::H) continue;
            uint8_t so = world::SOLID[ty * world::W + tx];
            if (so == 1 && frand() < 0.7f) continue;                  // mostly skip tree / building tiles
            bool water = so == 2;
            if (!water && frand() > 0.45f) continue;                  // fewer visible hits on land
            splashes_.push_back({ x, y, 0.f, water ? 0.55f + frand() * 0.3f : 0.22f + frand() * 0.1f, water, water && frand() < 0.35f });
        }
    }

    void updateAudio() {
        if (!rainOk_ && !stormOk_) return;
        const float master = Sound::get().muted() ? 0.f : Sound::get().volume();
        const float rv = std::min(1.f, cur_ * 1.5f) * 0.55f * master * 100.f;          // low-volume ambience
        const float sv = std::max(0.f, (cur_ - 0.55f) / 0.45f) * 0.35f * master * 100.f;
        if (rainOk_) {
            rain_.setVolume(rv);
            if (rv > 0.5f && !rainPlaying_) { rain_.play(); rainPlaying_ = true; }
            if (rv <= 0.5f && rainPlaying_) { rain_.pause(); rainPlaying_ = false; }
        }
        if (stormOk_) {
            storm_.setVolume(sv);
            if (sv > 0.5f && !stormPlaying_) { storm_.play(); stormPlaying_ = true; }
            if (sv <= 0.5f && stormPlaying_) { storm_.pause(); stormPlaying_ = false; }
        }
    }

public:
    // remember the camera rect (spawn area for splashes); call once per frame before update()
    void setView(const sf::FloatRect& v) { viewR_ = v; have_view_ = true; }

private:
    sf::FloatRect viewR_;
    bool have_view_ = false;
    float spawnAcc_ = 0.f;
};

} // namespace weather
