// Wood-theme UI helpers (nine-patch panels, text wrapping) for the HUD.
#pragma once
#include <SFML/Graphics.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace ui {

struct ChatEntry {
    std::string name, text;
    int colorIndex = -1;  // -1 = neutral
    float age = 0.f;
};

struct UiTheme {
    sf::Texture panel, panel2, panel3, bg, interior, focus;
    bool ok = false;

    bool load(const std::string& root) {
        const std::string d = root + "Ui/Theme/Theme Wood/";
        ok = panel.loadFromFile(d + "nine_path_panel.png")
          && panel2.loadFromFile(d + "nine_path_panel_2.png")
          && panel3.loadFromFile(d + "nine_path_panel_3.png")
          && bg.loadFromFile(d + "nine_path_bg.png")
          && interior.loadFromFile(d + "nine_path_panel_interior.png")
          && focus.loadFromFile(d + "nine_path_focus.png");
        for (auto* t : { &panel, &panel2, &panel3, &bg, &interior, &focus }) t->setSmooth(false);
        return ok;
    }
};

// Draw a 16x16-style nine-patch texture into rect. margin is in source pixels,
// scale multiplies the border thickness on screen. tint modulates the colour.
inline void drawNinePatch(sf::RenderTarget& rt, const sf::Texture& tex, sf::FloatRect r,
                          float margin, float scale, sf::Color tint = sf::Color::White) {
    const float tw = (float)tex.getSize().x, th = (float)tex.getSize().y;
    float m = margin * scale;
    m = std::min(m, std::min(r.size.x, r.size.y) * 0.5f);
    const float xs[4] = { r.position.x, r.position.x + m, r.position.x + r.size.x - m, r.position.x + r.size.x };
    const float ys[4] = { r.position.y, r.position.y + m, r.position.y + r.size.y - m, r.position.y + r.size.y };
    const float us[4] = { 0.f, margin, tw - margin, tw };
    const float vs[4] = { 0.f, margin, th - margin, th };
    sf::Vertex v[54];
    int n = 0;
    auto put = [&](float x, float y, float u, float w) { v[n++] = sf::Vertex{ {x, y}, tint, {u, w} }; };
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            put(xs[i], ys[j], us[i], vs[j]);         put(xs[i + 1], ys[j], us[i + 1], vs[j]);
            put(xs[i], ys[j + 1], us[i], vs[j + 1]);
            put(xs[i + 1], ys[j], us[i + 1], vs[j]); put(xs[i + 1], ys[j + 1], us[i + 1], vs[j + 1]);
            put(xs[i], ys[j + 1], us[i], vs[j + 1]);
        }
    sf::RenderStates rs;
    rs.texture = &tex;
    rt.draw(v, 54, sf::PrimitiveType::Triangles, rs);
}

// NormalFont.ttf has a ~1px space glyph, so words run together: widen every space.
inline std::string spaced(const std::string& s, unsigned size) {
    std::string out; out.reserve(s.size() + 8);
    const int n = std::max(2, (int)(size / 4));
    for (char c : s) { if (c == ' ') out.append(n, ' '); else out.push_back(c); }
    return out;
}

inline float textWidth(const sf::Font& f, unsigned size, const std::string& s) {
    sf::Text t(f, spaced(s, size), size);
    return t.getLocalBounds().size.x + t.getLocalBounds().position.x;
}

// Greedy word wrap; the first line is allowed `firstIndent` fewer pixels.
inline std::vector<std::string> wrapText(const sf::Font& f, unsigned size, const std::string& s,
                                         float maxW, float firstIndent = 0.f) {
    std::vector<std::string> lines;
    std::string cur;
    float avail = maxW - firstIndent;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(' ', i);
        std::string word = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = (j == std::string::npos) ? s.size() : j + 1;
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (textWidth(f, size, trial) <= avail) { cur = trial; continue; }
        if (!cur.empty()) { lines.push_back(cur); cur.clear(); avail = maxW; }
        // break very long words by characters
        while (textWidth(f, size, word) > avail && word.size() > 1) {
            size_t k = word.size() - 1;
            while (k > 1 && textWidth(f, size, word.substr(0, k)) > avail) --k;
            lines.push_back(word.substr(0, k));
            word = word.substr(k);
            avail = maxW;
        }
        cur = word;
    }
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    return lines;
}

} // namespace ui
