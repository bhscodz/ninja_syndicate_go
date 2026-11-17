// Small client helpers for trophy presentation (pure math + a few cheap SFML draw helpers).
#pragma once
#include <SFML/Graphics.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace trophy {

constexpr float PI = 3.14159265358979f;

// Screen-edge arrow: target and centre in the same 2D space (HUD pixels). half = half size of the inset rectangle the
// arrow is clamped to. onscreen is true when the target lies inside that rectangle (no arrow needed).
struct EdgeArrow { float x = 0, y = 0, angle = 0; bool onscreen = true; };

inline EdgeArrow edgeArrow(float tx, float ty, float cx, float cy, float halfW, float halfH) {
    EdgeArrow r;
    const float dx = tx - cx, dy = ty - cy;
    r.angle = std::atan2(dy, dx);
    if (std::abs(dx) <= halfW && std::abs(dy) <= halfH) { r.x = tx; r.y = ty; r.onscreen = true; return r; }
    r.onscreen = false;
    // scale the direction vector so it touches the rectangle border
    const float sx = std::abs(dx) > 1e-4f ? halfW / std::abs(dx) : 1e9f;
    const float sy = std::abs(dy) > 1e-4f ? halfH / std::abs(dy) : 1e9f;
    const float k = std::min(sx, sy);
    r.x = cx + dx * k; r.y = cy + dy * k;
    return r;
}

// Ring/arc made of a triangle strip: draws `frac` (0..1) of a circle clockwise starting at 12 o'clock.
inline void drawArc(sf::RenderTarget& rt, sf::Vector2f c, float r0, float r1, float frac, sf::Color col) {
    frac = std::clamp(frac, 0.f, 1.f);
    if (frac <= 0.f) return;
    const int n = std::max(2, (int)std::ceil(32.f * frac));
    std::vector<sf::Vertex> v;
    v.reserve((size_t)(n + 1) * 2);
    for (int i = 0; i <= n; ++i) {
        const float a = -PI * 0.5f + 2.f * PI * frac * (float)i / (float)n;
        const float ca = std::cos(a), sa = std::sin(a);
        v.push_back({ { c.x + ca * r0, c.y + sa * r0 }, col, {} });
        v.push_back({ { c.x + ca * r1, c.y + sa * r1 }, col, {} });
    }
    rt.draw(v.data(), v.size(), sf::PrimitiveType::TriangleStrip);
}

// Leap arc: vertical sprite offset (negative = up) and shadow scale for leapPhase 0..255
inline float leapLift(int phase) { return phase <= 0 ? 0.f : -std::sin(PI * std::min(255, phase) / 255.f) * 12.f; }
inline float leapShadow(int phase) { return phase <= 0 ? 1.f : 1.f - 0.45f * std::sin(PI * std::min(255, phase) / 255.f); }

} // namespace trophy
