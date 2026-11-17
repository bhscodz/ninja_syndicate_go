// Client-side weather + water animation. Header-only, no server sync: the weather is a pure function of the
// wall clock (unix time), so every client shows the same sky.
//
// Look (from the pack's Example 3 / Example 4 gifs): sparse crisp white rain streaks, small white ripple rings on the
// ground, small green leaves drifting on the breeze. Rain does not fall everywhere: a few storm cells drift across the
// map and it only rains inside them (soft edges), so you walk in and out of the rain.
//
//   Weather w;  w.init(assetRoot, &rippleTexture);
//   per frame:  w.setView(viewRect); w.update(dt);
//               w.drawGround(window, viewRect, time);   // game view, right after the ground tiles: water ripples + puddles
//               w.drawWorld(window, viewRect, time);    // game view: ripple rings where drops hit
//               w.drawScreen(window, viewRect, time);   // after the fog, before the HUD: streaks, tint, lightning, leaves
//   F2 -> w.toggle().  GAME_WEATHER=rain|clear forces the weather (testing; rain still falls in the storm cells).
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
            if (s == "rain") forced_ = 1; else if (s == "storm") { forced_ = 1; everywhere_ = true; } else if (s == "clear" || s == "off") forced_ = 0;
            envForced_ = forced_;
        }
        uint32_t seed = 4242;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
        for (auto& d : far_)  d = { rnd(), rnd(), 0.8f + rnd() * 0.5f, 0.7f + rnd() * 0.6f, rnd() };
        for (auto& d : near_) d = { rnd(), rnd(), 0.8f + rnd() * 0.5f, 0.7f + rnd() * 0.6f, rnd() };
        // leaf streams: a leaf is blown off a tree canopy along a fixed path (a pure function of the clock, so it is the
        // same for every client), travels with the wind in WORLD space and fades out; then the cycle repeats
        for (int i = 0; i < world::NUM_OBJECTS; ++i) {
            const auto& o = world::OBJECTS[i];
            const bool tree = o.sheet == 2 && o.w >= 2 && o.h >= 2 && (o.r <= 4 || o.r >= 18);
            if (!tree) continue;
            emitters_.push_back({ (o.tx + o.w * 0.5f) * gc::TILE + (rnd() - 0.5f) * 14.f, (o.ty + o.h * 0.35f) * gc::TILE + rnd() * 8.f,
                                  rnd() * 6.28f, 8.f + rnd() * 7.f, (int)(rnd() * 3.f) % 3 });
        }
        // storm cells that drift across the map (world px)
        for (auto& c : cells_) c = { rnd() * MAPW, rnd() * MAPH, (rnd() - 0.5f) * 16.f, (rnd() - 0.5f) * 12.f, 300.f + rnd() * 140.f };
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
        makeLeafTextures();
        std::string dir = root;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        dir += "Audio/Sounds/Ambient/";
        rainOk_ = rain_.openFromFile(dir + "Rain2.wav");
        stormOk_ = storm_.openFromFile(dir + "Storm.wav");
        for (auto* m : { &rain_, &storm_ }) { m->setLooping(true); m->setVolume(0.f); }
    }

    void toggle() { enabled_ = !enabled_; }
    // F3: demo switch - storm over the whole map until toggled off again
    bool toggleStorm() { everywhere_ = !everywhere_; if (everywhere_) { forced_ = 1; enabled_ = true; } else forced_ = envForced_; return everywhere_; }
    bool enabled() const { return enabled_; }
    float intensity() const { return local_; }    // 0 clear .. 1 heavy rain at the camera (smoothed)

    void update(float dt) {
        float target = 0.f;
        if (enabled_) target = forced_ >= 0 ? (float)forced_ : cycle(unixNow());
        cur_ += (target - cur_) * std::min(1.f, dt * (forced_ >= 0 ? 0.9f : 1.5f));
        if (cur_ < 0.002f) cur_ = 0.f;
        placeCells(unixNow());
        const float camX = viewR_.position.x + viewR_.size.x * 0.5f, camY = viewR_.position.y + viewR_.size.y * 0.5f;
        local_ = cur_ * coverage(camX, camY);
        // wet ground builds up where it rains and dries slowly
        wet_ += (local_ > 0.12f ? local_ * 0.06f : -0.012f) * dt;
        wet_ = std::clamp(wet_, 0.f, 1.f);
        updateSplashes(dt);
        updateAudio();
        // lightning (heavy rain only): deterministic from the clock
        flash_ = 0.f;
        if (local_ > 0.8f) {
            double t = unixNow();
            long slot = (long)std::floor(t / 13.0);
            uint32_t h = hash((uint32_t)slot, 77u);
            if (h % 5u < 2u) {
                double dtl = t - (slot * 13.0 + 2.0 + (h >> 8) % 8u);
                if (dtl >= 0.0 && dtl < 0.9)
                    flash_ = (float)(std::exp(-dtl * 7.0) * 0.8 + (dtl > 0.28 ? std::exp(-(dtl - 0.28) * 9.0) * 0.55 : 0.0)) * (local_ - 0.7f) * 3.3f;
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
                    const float wl = cur_ > 0.f ? coverage(px + 8.f, py + 8.f) : 0.f;       // puddles only where it rains
                    if (wl < 0.25f) continue;
                    const float rx = 5.f + float((h >> 6) % 4u), ry = 2.5f + float((h >> 10) % 2u);
                    const uint8_t a = (uint8_t)std::min(255.f, 150.f * wet_ * wl);
                    pd.setRadius(1.f); pd.setScale({ rx, ry });
                    pd.setPosition({ px + 8.f - rx, py + 9.f - ry });
                    pd.setFillColor(sf::Color(96, 130, 170, (uint8_t)(a * 0.55f)));
                    pd.setOutlineThickness(0.18f); pd.setOutlineColor(sf::Color(200, 220, 240, (uint8_t)(a * 0.6f)));
                    rt.draw(pd);
                }
            }
    }

    // ---- world space: ripple rings where drops land (game view active) ----
    void drawWorld(sf::RenderTarget& rt, const sf::FloatRect& view, float time) {
        drawLeaves(rt, view, time);
        if (splashes_.empty()) return;
        sf::CircleShape ring(1.f, 20);
        ring.setFillColor(sf::Color::Transparent);
        for (const auto& s : splashes_) {
            float k = s.age / s.life;
            uint8_t a = (uint8_t)(235.f * (1.f - k));
            float r = (s.water ? 2.f : 1.5f) + k * (s.water ? (s.big ? 7.f : 5.f) : 4.5f);
            ring.setRadius(r); ring.setScale({ 1.f, 0.46f });
            ring.setOrigin({ r, r });
            ring.setPosition({ std::round(s.x), std::round(s.y) });
            ring.setOutlineThickness(0.8f);                 // ~1 game pixel, crisp white like the example
            ring.setOutlineColor(sf::Color(255, 255, 255, a));
            rt.draw(ring);
            if (k > 0.3f) {                                 // short-lived inner ring following the first
                float r2 = r * 0.55f;
                ring.setRadius(r2); ring.setOrigin({ r2, r2 });
                ring.setOutlineColor(sf::Color(255, 255, 255, (uint8_t)(a * 0.7f)));
                rt.draw(ring);
            }
        }
    }

    // ---- screen space: rain streaks, tint, lightning, drifting leaves (call after the fog, before the HUD) ----
    void drawScreen(sf::RenderTarget& rt, const sf::FloatRect& view, float time) {
        sf::View old = rt.getView();
        rt.setView(sf::View(sf::FloatRect({ 0.f, 0.f }, { SW, SH })));
        const float cx = view.position.x * 3.f, cy = view.position.y * 3.f;     // world -> screen px (3x zoom) for parallax
        if (cur_ > 0.01f && local_ > 0.003f) {
            // very light cool cast only (the example has no darkening)
            float k = std::min(1.f, local_);
            sf::RectangleShape tint({ SW, SH });
            tint.setFillColor(sf::Color((uint8_t)(255 - 26 * k), (uint8_t)(255 - 20 * k), (uint8_t)(255 - 10 * k)));
            rt.draw(tint, sf::RenderStates(sf::BlendMultiply));
        }
        if (cur_ > 0.01f) {
            verts_.clear();
            streaks(far_, NFAR, 0.30f, 21.f, 3.f, 560.f, view, cx, cy, time, sf::Color(230, 240, 255, 190));
            streaks(near_, NNEAR, 0.60f, 33.f, 3.f, 950.f, view, cx, cy, time, sf::Color(255, 255, 255, 245));
            if (!verts_.empty()) rt.draw(verts_.data(), verts_.size(), sf::PrimitiveType::Triangles);
        }
        if (flash_ > 0.01f) {
            sf::RectangleShape f({ SW, SH });
            f.setFillColor(sf::Color(235, 240, 255, (uint8_t)std::min(120.f, 120.f * flash_)));
            rt.draw(f);
        }
        rt.setView(old);
    }

    // remember the camera rect (spawn area for splashes); call once per frame before update()
    void setView(const sf::FloatRect& v) { viewR_ = v; have_view_ = true; }

private:
    static constexpr float SW = 1280.f, SH = 960.f;
    static constexpr float MAPW = world::W * gc::TILE, MAPH = world::H * gc::TILE;
    static constexpr int NFAR = 150, NNEAR = 90;
    struct Drop { float x, y, len, spd, th; };
    struct Splash { float x, y, age, life; bool water, big; };
    struct Cell { float x0, y0, vx, vy, r; float x = 0, y = 0; };

    Drop far_[NFAR], near_[NNEAR];
    struct Emitter { float x, y, ph, T; int kind; };
    std::vector<Emitter> emitters_;      // one leaf stream per (some) tree canopy
    Cell cells_[3];
    sf::Texture leafTex_[3];
    bool leafOk_ = false;
    std::vector<sf::Vertex> verts_;
    std::vector<Splash> splashes_;
    std::vector<uint8_t> covered_;
    const sf::Texture* ripple_ = nullptr;
    sf::Music rain_, storm_;
    bool rainOk_ = false, stormOk_ = false, rainPlaying_ = false, stormPlaying_ = false;
    bool enabled_ = true;
    int forced_ = -1, envForced_ = -1;
    bool everywhere_ = false;   // GAME_WEATHER=storm: rain over the whole map (testing)
    float cur_ = 0.f, local_ = 0.f, wet_ = 0.f, flash_ = 0.f;
    uint32_t rng_ = 1234567u;
    sf::FloatRect viewR_;
    bool have_view_ = false;
    float spawnAcc_ = 0.f;

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
        if (u < 45.0) return 0.f;
        if (u < 120.0) return (float)(0.6 * sstep(45.0, 70.0, u));                               // light rain
        if (u < 420.0) return (float)(0.6 + 0.4 * sstep(120.0, 150.0, u));                       // heavy rain
        if (u < 480.0) return (float)(1.0 - sstep(420.0, 480.0, u));                             // clearing
        return 0.f;
    }

    // storm cells drift slowly and wrap around the map (deterministic from the clock => same for every client)
    void placeCells(double t) {
        for (auto& c : cells_) {
            const double wx = MAPW + 2.0 * c.r, wy = MAPH + 2.0 * c.r;
            double x = std::fmod((double)c.x0 + (double)c.vx * t, wx); if (x < 0) x += wx;
            double y = std::fmod((double)c.y0 + (double)c.vy * t, wy); if (y < 0) y += wy;
            c.x = (float)x - c.r; c.y = (float)y - c.r;
        }
    }
    // 0..1: how much of the storm covers a world position (soft edges)
    float coverage(float wx, float wy) const {
        if (everywhere_) return 1.f;
        float best = 0.f;
        for (const auto& c : cells_) {
            float d = std::hypot(wx - c.x, (wy - c.y) * 1.15f);
            float k = std::clamp((c.r - d) / (c.r * 0.4f), 0.f, 1.f);
            best = std::max(best, k * k * (3.f - 2.f * k));
        }
        return best;
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

    // crisp, thin, slightly slanted white streaks like the example; a drop is visible when the local rain
    // intensity at its world position exceeds its personal threshold (so rain thins out toward a storm's edge)
    void streaks(const Drop* d, int n, float par, float lenBase, float w, float speed, const sf::FloatRect& view,
                 float cx, float cy, float time, sf::Color c) {
        const float WW = SW + 240.f, HH = SH + 160.f;
        for (int i = 0; i < n; ++i) {
            float x = std::fmod(d[i].x * WW - cx * par - time * speed * 0.30f * d[i].spd, WW); if (x < 0) x += WW;
            float y = std::fmod(d[i].y * HH - cy * par + time * speed * d[i].spd, HH); if (y < 0) y += HH;
            x -= 120.f; y -= 80.f;
            const float inten = cur_ * coverage(view.position.x + x / 3.f, view.position.y + y / 3.f);
            if (inten <= d[i].th * 0.95f) continue;
            x = std::round(x / 3.f) * 3.f; y = std::round(y / 3.f) * 3.f;
            float len = std::round(lenBase * d[i].len / 3.f) * 3.f;
            sf::Vector2f a(x, y), b(x - len * 0.30f, y + len);
            sf::Vector2f nrm(w * 0.5f, 0.f);
            verts_.push_back({ a - nrm, c }); verts_.push_back({ a + nrm, c }); verts_.push_back({ b - nrm, c });
            verts_.push_back({ a + nrm, c }); verts_.push_back({ b + nrm, c }); verts_.push_back({ b - nrm, c });
        }
    }

    // leaves drifting away from trees; positions depend only on time, never on the screen or the camera
    void drawLeaves(sf::RenderTarget& rt, const sf::FloatRect& view, float time) {
        if (!leafOk_ || local_ > 0.75f) return;
        const float calm = 1.f - local_ / 0.75f;
        const double clock = unixNow();
        for (const auto& e : emitters_) {
            const float u = (float)std::fmod(clock / e.T + e.ph * 0.159, 1.0);      // 0..1 progress along its life
            if (u > 0.85f) continue;                                                  // rest period between leaves
            const float tau = u * e.T;                                // seconds since it left the tree
            const float gust = 1.f + 0.25f * std::sin((float)clock * 0.21f + e.ph);   // wind strength breathes slowly
            float x = e.x + (30.f * gust) * tau + std::sin(1.9f * tau + e.ph) * 7.f;
            float y = e.y + 8.f * tau + 0.28f * tau * tau + std::sin(2.7f * tau + e.ph * 1.3f) * 5.f;
            if (x < view.position.x - 20.f || x > view.position.x + view.size.x + 20.f ||
                y < view.position.y - 20.f || y > view.position.y + view.size.y + 20.f) continue;
            const float k = u / 0.85f;
            float a = std::min(1.f, k * 8.f) * std::min(1.f, (1.f - k) * 5.f) * calm;    // fade in / out
            sf::Sprite lf(leafTex_[e.kind]);
            lf.setOrigin({ 5.f, 3.f });
            lf.setPosition({ std::round(x), std::round(y) });
            lf.setScale({ std::sin(tau * 1.4f + e.ph) > 0.f ? 1.f : -1.f, 1.f });
            lf.setRotation(sf::radians(0.4f + tau * 0.5f + std::sin(2.3f * tau + e.ph) * 0.7f));
            lf.setColor(sf::Color(255, 255, 255, (uint8_t)(255.f * a)));
            rt.draw(lf);
        }
        (void)time;
    }

    void updateSplashes(float dt) {
        for (auto& s : splashes_) s.age += dt;
        splashes_.erase(std::remove_if(splashes_.begin(), splashes_.end(), [](const Splash& s) { return s.age >= s.life; }), splashes_.end());
        if (local_ < 0.03f || have_view_ == false) return;
        spawnAcc_ += dt * (40.f + 130.f * local_);
        int n = (int)spawnAcc_; spawnAcc_ -= n;
        for (int k = 0; k < n && splashes_.size() < 320; ++k) {
            float x = viewR_.position.x + frand() * viewR_.size.x, y = viewR_.position.y + frand() * viewR_.size.y;
            int tx = (int)(x / gc::TILE), ty = (int)(y / gc::TILE);
            if (tx < 0 || ty < 0 || tx >= world::W || ty >= world::H) continue;
            uint8_t so = world::SOLID[ty * world::W + tx];
            if (so == 1) continue;                                    // no rings on trees / buildings
            if (frand() > cur_ * coverage(x, y) + 0.05f) continue;    // rings only inside the storm
            bool water = so == 2;
            splashes_.push_back({ x, y, 0.f, water ? 0.6f + frand() * 0.3f : 0.5f + frand() * 0.25f, water, water && frand() < 0.35f });
        }
    }

    void updateAudio() {
        if (!rainOk_ && !stormOk_) return;
        const float master = Sound::get().muted() ? 0.f : Sound::get().volume();
        const float rv = std::min(1.f, local_ * 1.5f) * 0.55f * master * 100.f;          // low-volume ambience
        const float sv = std::max(0.f, (local_ - 0.55f) / 0.45f) * 0.35f * master * 100.f;
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

    // 10x6 pixel-art leaf (dark outline, two greens, light midrib + stem) in three shades
    void makeLeafTextures() {
        static const sf::Color body[3][4] = {
            { { 40, 92, 44 }, { 92, 156, 56 }, { 146, 200, 78 }, { 62, 120, 48 } },     // green
            { { 52, 104, 40 }, { 122, 172, 52 }, { 184, 214, 86 }, { 84, 136, 44 } },   // yellow-green
            { { 74, 92, 36 }, { 150, 154, 54 }, { 208, 196, 86 }, { 108, 120, 44 } },   // olive / autumn
        };
        leafOk_ = true;
        for (int k = 0; k < 3; ++k) {
            sf::Image im(sf::Vector2u(10, 6), sf::Color(0, 0, 0, 0));
            auto in = [](float x, float y) {   // pointed ellipse, tip on the right
                float dx = (x - 5.f) / 4.4f, dy = (y - 2.5f) / 2.7f;
                return dx * dx + dy * dy * (1.f + std::max(0.f, dx) * 0.6f) <= 1.f;
            };
            for (int y = 0; y < 6; ++y)
                for (int x = 0; x < 10; ++x) {
                    if (!in((float)x, (float)y)) continue;
                    bool edge = !in(x - 1.f, (float)y) || !in(x + 1.f, (float)y) || !in((float)x, y - 1.f) || !in((float)x, y + 1.f);
                    sf::Color c = edge ? body[k][0] : (y <= 2 ? body[k][2] : body[k][1]);
                    if (!edge && y == 3) c = body[k][3];                          // midrib
                    im.setPixel(sf::Vector2u(x, y), c);
                }
            im.setPixel(sf::Vector2u(0, 3), body[k][0]);                            // stem
            im.setPixel(sf::Vector2u(1, 3), body[k][3]);
            if (!leafTex_[k].loadFromImage(im)) leafOk_ = false;
            leafTex_[k].setSmooth(false);
        }
    }
};

} // namespace weather
