// Landing page, join screen and room lobby for the client (immediate-mode UI
// drawn with the wood Theme on a 1280x960 virtual canvas).
//
// The menu never touches the network itself: Menu::frame() returns an Action
// that ClientMain turns into Net calls, and is fed a View of the net state.
#pragma once
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "../game/Constants.hpp"
#include "../game/Protocol.hpp"
#include "UiTheme.hpp"

namespace menu {

inline const sf::Color TEAM[4] = { { 232, 80, 80 }, { 80, 150, 240 }, { 90, 205, 110 }, { 240, 210, 80 } };
inline const char* TEAM_NAME[4] = { "Red", "Blue", "Green", "Yellow" };
inline const char* CHAR_DIR[4] = { "NinjaRed", "NinjaBlue", "NinjaGreen", "NinjaYellow" };

constexpr float W = 1280.f, H = 960.f;

// ---- single-line text field ------------------------------------------------
struct Field {
    std::string text;
    size_t cur = 0;
    size_t maxLen = 64;
    std::string hint;

    void set(const std::string& s) { text = s; cur = text.size(); }
    void insert(const std::string& in) {
        for (char c : in) {
            if (c < 32 || c > 126) continue;
            if (text.size() >= maxLen) break;
            text.insert(text.begin() + cur, c);
            ++cur;
        }
    }
    void key(sf::Keyboard::Key k, bool ctrl) {
        using K = sf::Keyboard::Key;
        if (ctrl && k == K::V) { insert(sf::Clipboard::getString().toAnsiString()); return; }
        if (ctrl && k == K::C) { sf::Clipboard::setString(text); return; }
        if (ctrl && k == K::X) { sf::Clipboard::setString(text); text.clear(); cur = 0; return; }
        if (k == K::Backspace) { if (cur > 0) { text.erase(cur - 1, 1); --cur; } }
        else if (k == K::Delete) { if (cur < text.size()) text.erase(cur, 1); }
        else if (k == K::Left) { if (cur > 0) --cur; }
        else if (k == K::Right) { if (cur < text.size()) ++cur; }
        else if (k == K::Home) cur = 0;
        else if (k == K::End) cur = text.size();
    }
};

inline void parseHostPort(const std::string& s, std::string& host, unsigned short& port) {
    std::string t;
    for (char c : s) if (c > ' ') t.push_back(c);
    size_t colon = t.rfind(':');
    if (colon != std::string::npos) {
        int pv = std::atoi(t.substr(colon + 1).c_str());
        if (pv > 0 && pv < 65536) port = static_cast<unsigned short>(pv);
        t = t.substr(0, colon);
    }
    if (!t.empty()) host = t;
}

// Signal bars + "NN ms" (green / yellow / red). ms == 0 means unknown.
inline void drawPingWidget(sf::RenderTarget& rt, const sf::Font& font, float x, float y, int ms, bool connected, unsigned sz) {
    sf::Color col = !connected ? sf::Color(150, 150, 150) : (ms < 100 ? sf::Color(110, 230, 120) : (ms < 200 ? sf::Color(245, 210, 80) : sf::Color(240, 90, 80)));
    int bars = !connected || ms == 0 ? 0 : (ms < 60 ? 4 : (ms < 120 ? 3 : (ms < 220 ? 2 : 1)));
    for (int i = 0; i < 4; ++i) {
        float h = 4.f + i * 4.f;
        sf::RectangleShape b({ 4.f, h });
        b.setPosition({ x + i * 6.f, y + 16.f - h });
        b.setFillColor(i < bars ? col : sf::Color(30, 20, 12, 170));
        rt.draw(b);
    }
    std::string label = !connected ? "lost" : (ms == 0 ? "-- ms" : std::to_string(ms) + " ms");
    sf::Text t(font, ui::spaced(label, sz), sz);
    t.setFillColor(col); t.setOutlineColor(sf::Color(20, 10, 5)); t.setOutlineThickness(1.f);
    t.setPosition({ x + 28.f, y + 1.f });
    rt.draw(t);
}

// Tab completion for "/w <partial>", "/msg ...", "/whisper ...", "/pm ...", "/kick ...".
// Cycles through matching names on repeated Tab presses.
inline void tabComplete(std::string& text, const std::vector<std::string>& names) {
    static std::string baseText; static int cycle = 0; static std::string lastResult;
    if (text != lastResult) { baseText = text; cycle = 0; }
    else ++cycle;
    const char* cmds[] = { "/w ", "/msg ", "/whisper ", "/pm ", "/kick ", "/tell " };
    std::string low; for (char c : baseText) low.push_back((char)std::tolower((unsigned char)c));
    for (const char* c : cmds) {
        std::string cs = c;
        if (low.rfind(cs, 0) != 0) continue;
        std::string partial = baseText.substr(cs.size());
        if (partial.find(' ') != std::string::npos) return;   // already past the name
        std::string pl; for (char ch : partial) pl.push_back((char)std::tolower((unsigned char)ch));
        std::vector<std::string> m;
        for (auto& n : names) {
            std::string nl; for (char ch : n) nl.push_back((char)std::tolower((unsigned char)ch));
            if (nl.rfind(pl, 0) == 0) m.push_back(n);
        }
        if (m.empty()) return;
        text = baseText.substr(0, cs.size()) + m[cycle % m.size()] + " ";
        lastResult = text;
        return;
    }
}

struct Action {
    enum Kind { None, Quit, Create, Join, Start, Ready, Leave, Chat, Settings, Kick, CancelStart, Cancel, TrustKey } kind = None;
    std::string host, name, code, text;
    RoomSettings settings;
    unsigned short port = gc::SERVER_PORT;
};

struct View {
    bool connected = false;
    std::string code;
    uint8_t myId = 255;
    LobbyInfo lobby;
};

class Menu {
public:
    enum class Screen { Landing, Join, Lobby };

    Screen screen = Screen::Landing;
    bool busy = false;
    std::string error, status;
    Field name, server, join, chat;
    std::string usedHost = gc::DEFAULT_HOST;
    unsigned short usedPort = gc::SERVER_PORT;
    float screenTime = 0.f;   // seconds since the current screen was entered
    // set when the server's long-term key differs from the pinned one (TOFU warning)
    struct KeyWarn { bool active = false; std::string host; unsigned short port = 0; std::string newFp, oldFp; } keyWarn;
    const std::vector<std::string>& playerNames() const { return names_; }
    const std::string& ownName() const { return myName_; }
    void drawPing(sf::RenderTarget& rt, float x, float y, int ms, bool connected, unsigned sz) { drawPingWidget(rt, *font_, x, y, ms, connected, sz); }

    bool load(const std::string& root, const sf::Font* f) {
        font_ = f;
        const std::string d = root + "Ui/Theme/Theme Wood/";
        bool ok = theme_.load(root);
        ok = btn_[0].loadFromFile(d + "button_normal.png") && ok;
        ok = btn_[1].loadFromFile(d + "button_hover.png") && ok;
        ok = btn_[2].loadFromFile(d + "button_pressed.png") && ok;
        ok = btn_[3].loadFromFile(d + "button_disabled.png") && ok;
        ok = floor_.loadFromFile(root + "Backgrounds/Tilesets/TilesetFloor.png") && ok;
        ok = treasure_.loadFromFile(root + "Items/Treasure/GoldCup.png") && ok;
        const char* fl[4] = { "Red", "Blue", "Green", "Yellow" };
        for (int i = 0; i < 4; ++i) {
            ok = chars_[i].loadFromFile(root + std::string("Actor/Character/") + CHAR_DIR[i] + "/SpriteSheet.png") && ok;
            ok = flags_[i].loadFromFile(root + std::string("Backgrounds/Animated/Flag/Flag") + fl[i] + "16x16.png") && ok;
            chars_[i].setSmooth(false); flags_[i].setSmooth(false);
        }
        for (auto& t : btn_) t.setSmooth(false);
        floor_.setSmooth(false); treasure_.setSmooth(false);
        name.maxLen = gc::NAME_LEN; name.hint = "Your name";
        server.maxLen = 64; server.hint = "host:port";
        join.maxLen = 80; join.hint = "192.168.1.5:7777/K7QX2  or  K7QX2";
        chat.maxLen = gc::CHAT_LEN; chat.hint = "Type a message...";
        focus_ = &name;
        return ok && font_;
    }

    void setScreen(Screen s) {
        screen = s; screenTime = 0.f;
        focus_ = (s == Screen::Landing) ? &name : (s == Screen::Join ? &join : &chat);
        if (s == Screen::Lobby) { chatLog_.clear(); copied_ = 0.f; }
    }
    void fail(const std::string& msg, bool toLanding = false) {
        busy = false; error = msg;
        if (screen == Screen::Lobby || toLanding) setScreen(Screen::Landing);
    }
    void enterLobby() { busy = false; error.clear(); setScreen(Screen::Lobby); }
    std::string joinString(const std::string& code) const {
        return usedHost + ":" + std::to_string(usedPort) + "/" + code;
    }
    // Used by automation: behaves like the user pressing the matching button.
    void request(Action::Kind k, const std::string& code = "") { pending_ = k; pendingCode_ = code; }

    Action frame(sf::RenderWindow& win, float dt, const View& nv, const std::vector<ChatLine>& newChat) {
        Action act;
        t_ += dt; screenTime += dt; copied_ = std::max(0.f, copied_ - dt);
        view_ = sf::View({ W * 0.5f, H * 0.5f }, { W, H });
        win.setView(view_);
        hasClick_ = false;
        while (const std::optional<sf::Event> ev = win.pollEvent()) handleEvent(*ev, act, nv);
        mouse_ = win.mapPixelToCoords(sf::Mouse::getPosition(win), view_);
        for (auto& c : newChat) {
            ChatRow r; r.name = c.name.empty() ? "P" + std::to_string(int(c.fromId)) : c.name;
            r.text = c.text; r.col = c.fromId < 4 ? TEAM[c.fromId] : sf::Color(200, 200, 200);
            r.tcol = sf::Color(235, 230, 220);
            if (c.kind == CHAT_WHISPER_FROM) { r.name = "[whisper from " + r.name + "]"; r.col = r.tcol = sf::Color(214, 150, 255); }
            else if (c.kind == CHAT_WHISPER_TO) { r.name = "[to " + r.name + "]"; r.col = r.tcol = sf::Color(214, 150, 255); }
            else if (c.kind == CHAT_SYSTEM) { r.name = ""; r.col = r.tcol = sf::Color(255, 225, 130); }
            chatLog_.push_back(r);
            while (chatLog_.size() > 60) chatLog_.pop_front();
        }
        if (pending_ != Action::None && act.kind == Action::None) {
            act = makeAction(pending_, pendingCode_, nv);
            pending_ = Action::None;
        }

        win.clear(sf::Color(40, 90, 50));
        drawBackground(win);
        switch (screen) {
            case Screen::Landing: drawLanding(win, act, nv); break;
            case Screen::Join:    drawJoin(win, act, nv); break;
            case Screen::Lobby:   drawLobby(win, act, nv); break;
        }
        if (busy) {
            sf::RectangleShape dim({ W, H }); dim.setFillColor(sf::Color(0, 0, 0, 120)); win.draw(dim);
            panel(win, { { W * 0.5f - 230.f, H * 0.5f - 50.f }, { 460.f, 100.f } });
            int dots = (int)(t_ * 3.f) % 4;
            text(win, "Connecting" + std::string(dots, '.'), 28, sf::Color(255, 240, 180), { W * 0.5f, H * 0.5f - 12.f }, 1, 2.f);
            // spinner
            for (int i = 0; i < 8; ++i) {
                float a = i * 0.785398f + t_ * 5.f;
                sf::CircleShape d(4.f); d.setOrigin({ 4.f, 4.f });
                d.setPosition({ W * 0.5f - 170.f + std::cos(a) * 14.f, H * 0.5f - 12.f + std::sin(a) * 14.f });
                d.setFillColor(sf::Color(255, 230, 140, (uint8_t)(70 + 185 * i / 8)));
                win.draw(d);
            }
            if (button(win, "CANCEL", { { W * 0.5f - 60.f, H * 0.5f + 12.f }, { 120.f, 34.f } }, true, 16, true)) act.kind = Action::Cancel;
        }
        if (keyWarn.active) drawKeyWarning(win, act);
        return act;
    }

    // "Leave match?" overlay drawn on top of the game.
    void drawLeavePrompt(sf::RenderWindow& win) {
        win.setView(sf::View({ W * 0.5f, H * 0.5f }, { W, H }));
        sf::RectangleShape dim({ W, H }); dim.setFillColor(sf::Color(0, 0, 0, 150)); win.draw(dim);
        panel(win, { { W * 0.5f - 280.f, H * 0.5f - 80.f }, { 560.f, 160.f } });
        text(win, "Leave the match?", 32, sf::Color(255, 230, 120), { W * 0.5f, H * 0.5f - 25.f }, 1, 2.f);
        text(win, "Press Y or Enter to leave,  N or Esc to stay", 18, sf::Color(240, 235, 220), { W * 0.5f, H * 0.5f + 30.f }, 1, 1.f);
    }

private:
    struct ChatRow { std::string name, text; sf::Color col, tcol; };
    std::vector<std::string> names_;
    std::string myName_;

    const sf::Font* font_ = nullptr;
    ui::UiTheme theme_;
    sf::Texture btn_[4], floor_, treasure_, chars_[4], flags_[4];
    sf::View view_;
    Field* focus_ = nullptr;
    float t_ = 0.f, copied_ = 0.f;
    bool hasClick_ = false;
    sf::Vector2f click_, mouse_;
    std::deque<ChatRow> chatLog_;
    Action::Kind pending_ = Action::None;
    std::string pendingCode_;

    // ---- input ----
    Action makeAction(Action::Kind k, const std::string& code, const View&) {
        Action a; a.kind = k;
        a.name = name.text.empty() ? std::string("Player") : name.text;
        a.host = gc::DEFAULT_HOST; a.port = gc::SERVER_PORT;
        parseHostPort(server.text, a.host, a.port);
        a.code = code;
        if (k == Action::Join) {
            std::string c = code;
            if (c.empty()) parseJoinString(join.text, a.host, a.port, c);
            a.code = c;
            if (a.code.size() < 3) { error = "Enter a room code or a join string (host:port/CODE)"; a.kind = Action::None; return a; }
        }
        error.clear();
        if (k == Action::TrustKey) keyWarn.active = false;
        usedHost = a.host; usedPort = a.port;
        busy = true;
        return a;
    }

    void cycleFocus(bool back) {
        std::vector<Field*> order;
        if (screen == Screen::Landing) order = { &name, &server };
        else if (screen == Screen::Join) order = { &join, &server, &name };
        else order = { &chat };
        auto it = std::find(order.begin(), order.end(), focus_);
        size_t i = it == order.end() ? 0 : (size_t)(it - order.begin());
        i = back ? (i + order.size() - 1) % order.size() : (i + 1) % order.size();
        focus_ = order[i];
    }

    void handleEvent(const sf::Event& ev, Action& act, const View& nv) {
        using K = sf::Keyboard::Key;
        if (ev.is<sf::Event::Closed>()) { act.kind = Action::Quit; return; }
        if (auto* mp = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mp->button == sf::Mouse::Button::Left) {
                hasClick_ = true;
                click_ = mouseToView(mp->position);
            }
        }
        if (busy) {
            if (auto* k = ev.getIf<sf::Event::KeyPressed>()) if (k->code == K::Escape) act.kind = Action::Cancel;
            return;
        }
        if (keyWarn.active) {
            if (auto* k = ev.getIf<sf::Event::KeyPressed>()) if (k->code == K::Escape) keyWarn.active = false;
            return;
        }
        if (auto* te = ev.getIf<sf::Event::TextEntered>()) {
            if (focus_ && te->unicode >= 32 && te->unicode < 127) focus_->insert(std::string(1, (char)te->unicode));
        }
        if (auto* k = ev.getIf<sf::Event::KeyPressed>()) {
            if (k->code == K::Tab) {
                if (screen == Screen::Lobby && focus_ == &chat) tabComplete(chat.text, names_), chat.cur = chat.text.size();
                else cycleFocus(k->shift);
                return;
            }
            if (k->code == K::Escape) {
                if (screen == Screen::Join) { error.clear(); setScreen(Screen::Landing); }
                else if (screen == Screen::Lobby) act.kind = Action::Leave;
                return;
            }
            if (k->code == K::Enter) {
                if (screen == Screen::Landing) act = makeAction(Action::Create, "", nv);
                else if (screen == Screen::Join) act = makeAction(Action::Join, "", nv);
                else {
                    if (!chat.text.empty()) { act.kind = Action::Chat; act.text = chat.text; chat.set(""); }
                }
                return;
            }
            if (focus_) focus_->key(k->code, k->control);
        }
    }

    sf::Vector2f mouseToView(sf::Vector2i px) const {
        // window is created at 1280x960; the view is fixed so this is only wrong when resized
        return { (float)px.x, (float)px.y };
    }

    // ---- primitives ----
    void text(sf::RenderTarget& rt, const std::string& s, unsigned sz, sf::Color col, sf::Vector2f pos,
              int align = 0, float outline = 0.f, unsigned spaceSz = 0) {
        sf::Text t(*font_, ui::spaced(s, spaceSz ? spaceSz : sz), sz);
        t.setFillColor(col);
        if (outline > 0.f) { t.setOutlineColor(sf::Color(30, 15, 5)); t.setOutlineThickness(outline); }
        auto b = t.getLocalBounds();
        float ox = align == 0 ? b.position.x : (align == 1 ? b.position.x + b.size.x * 0.5f : b.position.x + b.size.x);
        t.setOrigin({ ox, b.position.y + (align == 1 ? b.size.y * 0.5f : 0.f) });
        t.setPosition(pos);
        rt.draw(t);
    }
    void panel(sf::RenderTarget& rt, sf::FloatRect r, int kind = 0) {
        const sf::Texture& tx = kind == 0 ? theme_.panel : (kind == 1 ? theme_.panel2 : theme_.panel3);
        ui::drawNinePatch(rt, tx, r, 5.f, 4.f);
        sf::RectangleShape in({ r.size.x - 28.f, r.size.y - 28.f });   // flat wood fill hides the stretched texture
        in.setPosition({ r.position.x + 14.f, r.position.y + 14.f });
        in.setFillColor(sf::Color(128, 82, 48, 245));
        rt.draw(in);
    }

    bool button(sf::RenderTarget& rt, const std::string& label, sf::FloatRect r, bool enabled = true, unsigned sz = 26, bool allowBusy = false) {
        const bool blocked = (busy && !allowBusy) || (keyWarn.active && !allowBusy);
        bool hover = enabled && !blocked && r.contains(mouse_);
        bool down = hover && sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
        const sf::Texture& tx = !enabled ? btn_[3] : (down ? btn_[2] : (hover ? btn_[1] : btn_[0]));
        ui::drawNinePatch(rt, tx, r, 3.f, 5.f);
        sf::Color col = !enabled ? sf::Color(150, 140, 130) : (hover ? sf::Color(255, 245, 170) : sf::Color(255, 255, 255));
        text(rt, label, sz, col, { r.position.x + r.size.x * 0.5f, r.position.y + r.size.y * 0.5f - 2.f + (down ? 2.f : 0.f) }, 1, 2.f);
        return enabled && !blocked && hasClick_ && r.contains(click_);
    }

    static size_t spacedIdx(const std::string& s, size_t idx, unsigned sz) {
        size_t extra = (size_t)std::max(2, (int)(sz / 4)) - 1, sp = 0;
        for (size_t i = 0; i < idx && i < s.size(); ++i) if (s[i] == ' ') ++sp;
        return idx + sp * extra;
    }

    void field(sf::RenderTarget& rt, Field& f, sf::FloatRect r, unsigned sz = 24) {
        const bool focused = focus_ == &f && !busy;
        const float pad = 14.f, inner = r.size.x - pad * 2.f;
        const bool clicked = hasClick_ && !busy && r.contains(click_);
        if (clicked) focus_ = &f;
        if (focused || clicked) ui::drawNinePatch(rt, theme_.focus, r, 3.f, 4.f);
        else ui::drawNinePatch(rt, theme_.interior, r, 5.f, 2.f);
        // visible window [start, end)
        size_t start = 0;
        while (start < f.cur && ui::textWidth(*font_, sz, f.text.substr(start, f.cur - start)) > inner - 4.f) ++start;
        size_t end = f.text.size();
        while (end > f.cur && end > start && ui::textWidth(*font_, sz, f.text.substr(start, end - start)) > inner) --end;
        std::string shown = f.text.substr(start, end - start);
        sf::Text t(*font_, ui::spaced(shown, sz), sz);
        t.setPosition({ r.position.x + pad, r.position.y + r.size.y * 0.5f - sz * 0.62f });
        if (f.text.empty() && !focused) {
            t.setString(ui::spaced(f.hint, sz)); t.setFillColor(sf::Color(160, 140, 110, 200));
        } else t.setFillColor(sf::Color(255, 245, 170));
        rt.draw(t);
        auto posAt = [&](size_t idx) { return t.findCharacterPos(spacedIdx(shown, idx - start, sz)).x; };
        if (clicked && !shown.empty()) {
            size_t best = start; float bd = 1e9f;
            for (size_t i = start; i <= end; ++i) {
                float d = std::abs(posAt(i) - click_.x);
                if (d < bd) { bd = d; best = i; }
            }
            f.cur = best;
        }
        if (focused && std::fmod(t_, 1.0f) < 0.6f) {
            float cx = (f.text.empty() ? t.getPosition().x : posAt(std::clamp(f.cur, start, end)));
            sf::RectangleShape c({ 2.f, sz * 1.15f });
            c.setPosition({ cx + 1.f, t.getPosition().y + 1.f });
            c.setFillColor(sf::Color(255, 245, 170));
            rt.draw(c);
        }
    }

    // ---- scenes ----
    void drawBackground(sf::RenderTarget& rt) {
        sf::Sprite g(floor_);
        g.setScale({ 4.f, 4.f });
        for (int y = 0; y < 15; ++y)
            for (int x = 0; x < 20; ++x) {
                unsigned h = (unsigned)(x * 73856093u ^ y * 19349663u);
                int c = (h % 100) < 72 ? 0 : 1 + (int)((h / 100) % 4);
                g.setTextureRect({ { c * 16, 12 * 16 }, { 16, 16 } });
                g.setPosition({ x * 64.f, y * 64.f });
                rt.draw(g);
            }
        // flags in the four corners (team colours)
        int ff = (int)(t_ * 6.f) & 3;
        const sf::Vector2f fp[4] = { { 30.f, 20.f }, { W - 30.f - 96.f, 20.f }, { 30.f, H - 120.f }, { W - 126.f, H - 120.f } };
        for (int i = 0; i < 4; ++i) {
            sf::Sprite f(flags_[i]);
            f.setTextureRect({ { ff * 16, 0 }, { 16, 16 } });
            f.setScale({ 6.f, 6.f });
            f.setPosition(fp[i]);
            rt.draw(f);
        }
        // ninjas patrolling the bottom edge
        for (int i = 0; i < 4; ++i) {
            float speed = 55.f + i * 17.f, span = W + 200.f;
            bool right = (i % 2) == 0;
            float p = std::fmod(t_ * speed + i * 310.f, span);
            float x = right ? p - 100.f : W + 100.f - p;
            float y = H - 62.f - (i % 2) * 34.f;
            int face = right ? gc::FACE_RIGHT : gc::FACE_LEFT;
            int fr = ((int)(t_ * 8.f + i)) & 3;
            sf::CircleShape sh(26.f, 16); sh.setFillColor(sf::Color(0, 0, 0, 80)); sh.setOrigin({ 26.f, 26.f });
            sh.setScale({ 1.f, 0.4f }); sh.setPosition({ x + 40.f, y + 76.f }); rt.draw(sh);
            sf::Sprite n(chars_[i]);
            n.setTextureRect({ { face * 16, fr * 16 }, { 16, 16 } });
            n.setScale({ 5.f, 5.f });
            n.setPosition({ x, y });
            rt.draw(n);
        }
    }

    void treasureAt(sf::RenderTarget& rt, sf::Vector2f c, float scale, float phase) {
        sf::Sprite s(treasure_);
        auto sz = treasure_.getSize();
        s.setOrigin({ sz.x * 0.5f, sz.y * 0.5f });
        s.setScale({ scale, scale });
        s.setPosition({ c.x, c.y + std::sin(t_ * 2.5f + phase) * 5.f });
        rt.draw(s);
    }

    void title(sf::RenderTarget& rt, float y, unsigned sz) {
        text(rt, "STEAL THE TREASURE", sz, sf::Color(255, 226, 110), { W * 0.5f, y }, 1, sz >= 60 ? 6.f : 4.f, sz * 2 / 5);
        sf::Text meas(*font_, ui::spaced("STEAL THE TREASURE", sz * 2 / 5), sz);
        float tw = meas.getLocalBounds().size.x * 0.5f + sz * 1.3f;
        float sc = sz / 14.f;
        treasureAt(rt, { W * 0.5f - tw, y }, sc, 0.f);
        treasureAt(rt, { W * 0.5f + tw, y }, sc, 1.5f);
    }

    void errorLine(sf::RenderTarget& rt, float y) {
        if (error.empty()) return;
        auto lines = ui::wrapText(*font_, 18, error, 560.f);
        for (size_t i = 0; i < lines.size(); ++i)
            text(rt, lines[i], 18, sf::Color(255, 120, 100), { W * 0.5f, y + i * 24.f }, 1, 1.5f);
    }

    void drawLanding(sf::RenderTarget& rt, Action& act, const View& nv) {
        title(rt, 150.f, 64);
        text(rt, "4-player capture-the-cup arena", 22, sf::Color(255, 250, 220), { W * 0.5f, 215.f }, 1, 2.f);
        const float pw = 620.f, px = (W - pw) * 0.5f, py = 270.f;
        panel(rt, { { px, py }, { pw, 520.f } });
        float x = px + 50.f, w = pw - 100.f, y = py + 40.f;
        text(rt, "YOUR NAME", 16, sf::Color(255, 220, 140), { x, y }, 0, 1.f);
        field(rt, name, { { x, y + 26.f }, { w, 52.f } });
        y += 100.f;
        text(rt, "SERVER ADDRESS", 16, sf::Color(255, 220, 140), { x, y }, 0, 1.f);
        field(rt, server, { { x, y + 26.f }, { w, 52.f } });
        y += 110.f;
        if (button(rt, "CREATE ROOM", { { x, y }, { w, 62.f } })) act = makeAction(Action::Create, "", nv);
        if (button(rt, "JOIN ROOM", { { x, y + 76.f }, { w, 62.f } })) { error.clear(); setScreen(Screen::Join); }
        if (button(rt, "QUIT", { { x, y + 152.f }, { w, 62.f } })) act.kind = Action::Quit;
        errorLine(rt, py + 540.f);
        text(rt, "Tab: next field   Enter: create room", 15, sf::Color(255, 250, 220), { W * 0.5f, H - 22.f }, 1, 1.5f);
    }

    void drawJoin(sf::RenderTarget& rt, Action& act, const View& nv) {
        title(rt, 150.f, 64);
        const float pw = 760.f, px = (W - pw) * 0.5f, py = 270.f;
        panel(rt, { { px, py }, { pw, 470.f } });
        float x = px + 50.f, w = pw - 100.f, y = py + 36.f;
        text(rt, "JOIN STRING OR ROOM CODE   (Ctrl+V to paste)", 16, sf::Color(255, 220, 140), { x, y }, 0, 1.f);
        field(rt, join, { { x, y + 26.f }, { w, 56.f } }, 26);
        y += 108.f;
        text(rt, "SERVER ADDRESS  (used when only a code is given)", 16, sf::Color(255, 220, 140), { x, y }, 0, 1.f);
        field(rt, server, { { x, y + 26.f }, { w, 52.f } });
        y += 106.f;
        text(rt, "YOUR NAME", 16, sf::Color(255, 220, 140), { x, y }, 0, 1.f);
        field(rt, name, { { x, y + 26.f }, { w, 52.f } });
        y += 100.f;
        float bw = (w - 20.f) * 0.5f;
        if (button(rt, "JOIN", { { x, y }, { bw, 62.f } })) act = makeAction(Action::Join, "", nv);
        if (button(rt, "BACK", { { x + bw + 20.f, y }, { bw, 62.f } })) { error.clear(); setScreen(Screen::Landing); }
        errorLine(rt, py + 490.f);
        text(rt, "Tab: next field   Enter: join   Esc: back", 15, sf::Color(255, 250, 220), { W * 0.5f, H - 22.f }, 1, 1.5f);
    }

    void drawKeyWarning(sf::RenderTarget& rt, Action& act) {
        sf::RectangleShape dim({ W, H }); dim.setFillColor(sf::Color(0, 0, 0, 170)); rt.draw(dim);
        panel(rt, { { W * 0.5f - 400.f, H * 0.5f - 210.f }, { 800.f, 420.f } });
        text(rt, "WARNING: SERVER IDENTITY CHANGED", 30, sf::Color(255, 120, 100), { W * 0.5f, H * 0.5f - 160.f }, 1, 2.f);
        auto lines = ui::wrapText(*font_, 17, "The server " + keyWarn.host + ":" + std::to_string(keyWarn.port) +
            " presented a different key than the one you trusted before. This can mean the server was reinstalled, "
            "or that someone is intercepting your connection. Only continue if you expected this.", 720.f);
        float y = H * 0.5f - 110.f;
        for (auto& l : lines) { text(rt, l, 17, sf::Color(250, 240, 220), { W * 0.5f, y }, 1, 1.f); y += 24.f; }
        text(rt, "Trusted before: " + keyWarn.oldFp, 16, sf::Color(200, 220, 255), { W * 0.5f, y + 14.f }, 1, 1.f);
        text(rt, "Offered now:    " + keyWarn.newFp, 16, sf::Color(255, 200, 160), { W * 0.5f, y + 40.f }, 1, 1.f);
        if (button(rt, "TRUST NEW KEY", { { W * 0.5f - 330.f, H * 0.5f + 120.f }, { 320.f, 56.f } }, true, 22, true) && keyWarn.active) { act.kind = Action::TrustKey; act.host = keyWarn.host; act.port = keyWarn.port; keyWarn.active = false; }
        if (button(rt, "DON'T CONNECT", { { W * 0.5f + 10.f, H * 0.5f + 120.f }, { 320.f, 56.f } }, true, 22, true)) keyWarn.active = false;
    }

public:
    // "Reconnecting..." overlay shown on top of the (frozen) game.
    void drawReconnect(sf::RenderWindow& win, int attempt, float secondsLeft) {
        t_ += 0.016f;
        win.setView(sf::View({ W * 0.5f, H * 0.5f }, { W, H }));
        sf::RectangleShape dim({ W, H }); dim.setFillColor(sf::Color(0, 0, 0, 150)); win.draw(dim);
        panel(win, { { W * 0.5f - 280.f, H * 0.5f - 80.f }, { 560.f, 160.f } });
        text(win, "Connection lost", 28, sf::Color(255, 140, 120), { W * 0.5f, H * 0.5f - 40.f }, 1, 2.f);
        int dots = (int)(t_ * 3.f) % 4;
        text(win, "Reconnecting" + std::string(dots, '.') + "  (attempt " + std::to_string(attempt) + ")", 22, sf::Color(255, 240, 180), { W * 0.5f, H * 0.5f }, 1, 1.5f);
        text(win, "giving up in " + std::to_string((int)std::ceil(secondsLeft)) + " s", 16, sf::Color(230, 220, 200), { W * 0.5f, H * 0.5f + 40.f }, 1, 1.f);
    }

private:
    // ---- lobby ----
    // returns -1 / +1 when the "<" / ">" button was clicked
    int stepper(sf::RenderTarget& rt, const std::string& value, float x, float y, bool enabled) {
        int r = 0;
        if (enabled) {
            if (button(rt, "<", { { x, y }, { 40.f, 32.f } }, true, 20)) r = -1;
            if (button(rt, ">", { { x + 190.f, y }, { 40.f, 32.f } }, true, 20)) r = 1;
        }
        text(rt, value, 20, enabled ? sf::Color(255, 245, 160) : sf::Color(215, 200, 175), { x + 115.f, y + 16.f }, 1, enabled ? 1.5f : 0.f);
        return r;
    }

    void drawLobby(sf::RenderTarget& rt, Action& act, const View& nv) {
        names_.clear();
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) if (nv.lobby.slots[i].active && i != nv.myId) names_.push_back(nv.lobby.slots[i].name);
        myName_ = nv.myId < 4 ? nv.lobby.slots[nv.myId].name : "";
        title(rt, 62.f, 40);
        const bool isHost = nv.myId < 4 && nv.myId == nv.lobby.hostId;
        const bool counting = nv.lobby.countdown >= 0;
        const RoomSettings& st = nv.lobby.settings;
        // left: room + slots + settings
        const float lx = 40.f, ly = 120.f, lw = 700.f, lh = 790.f;
        panel(rt, { { lx, ly }, { lw, lh } });
        text(rt, "ROOM  " + nv.code + (st.name.empty() ? "" : "   -   " + st.name), 20, sf::Color(255, 220, 140), { lx + 36.f, ly + 28.f }, 0, 1.f);
        std::string js = joinString(nv.code);
        sf::FloatRect jr({ lx + 36.f, ly + 54.f }, { lw - 72.f - 150.f, 62.f });
        ui::drawNinePatch(rt, theme_.interior, jr, 5.f, 2.f);
        unsigned jsz = 34;
        while (jsz > 18 && ui::textWidth(*font_, jsz, js) > jr.size.x - 24.f) --jsz;
        text(rt, js, jsz, sf::Color(255, 245, 160), { jr.position.x + jr.size.x * 0.5f, jr.position.y + jr.size.y * 0.5f }, 1, 2.f);
        if (button(rt, copied_ > 0.f ? "COPIED!" : "COPY", { { jr.position.x + jr.size.x + 12.f, jr.position.y }, { 138.f, 62.f } }, true, 22)) {
            sf::Clipboard::setString(js);
            copied_ = 1.6f;
        }

        const float sy0 = ly + 132.f, sh = 80.f;
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& s = nv.lobby.slots[i];
            sf::FloatRect r({ lx + 36.f, sy0 + i * (sh + 6.f) }, { lw - 72.f, sh });
            ui::drawNinePatch(rt, theme_.panel3, r, 5.f, 3.f, s.active ? sf::Color::White : sf::Color(150, 150, 150));
            sf::RectangleShape fill({ r.size.x - 16.f, r.size.y - 16.f });
            fill.setPosition({ r.position.x + 8.f, r.position.y + 8.f });
            fill.setFillColor(s.active ? sf::Color(84, 52, 30) : sf::Color(64, 42, 28));
            rt.draw(fill);
            sf::RectangleShape strip({ 12.f, sh - 28.f });
            strip.setPosition({ r.position.x + 16.f, r.position.y + 14.f });
            strip.setFillColor(s.active ? TEAM[i] : sf::Color(TEAM[i].r / 3, TEAM[i].g / 3, TEAM[i].b / 3));
            rt.draw(strip);
            if (s.active) {
                int fr = ((int)(t_ * 6.f + i)) % 4;
                sf::Sprite n(chars_[i]);
                n.setTextureRect({ { gc::FACE_DOWN * 16, (s.ready ? fr : 0) * 16 }, { 16, 16 } });
                n.setScale({ 4.f, 4.f });
                n.setPosition({ r.position.x + 40.f, r.position.y + (sh - 64.f) * 0.5f });
                if (!s.connected) n.setColor(sf::Color(255, 255, 255, 110));
                rt.draw(n);
                std::string nm = s.name.empty() ? "P" + std::to_string(i) : s.name;
                text(rt, nm + (i == nv.myId ? "  (you)" : ""), 24, TEAM[i], { r.position.x + 116.f, r.position.y + 18.f }, 0, 2.f);
                text(rt, s.connected ? std::string(TEAM_NAME[i]) + " team" : "connection lost...", 15,
                     s.connected ? sf::Color(230, 215, 190) : sf::Color(255, 140, 120), { r.position.x + 116.f, r.position.y + 50.f }, 0, 0.f);
                float bx = r.position.x + r.size.x - 20.f;
                if (i == nv.lobby.hostId) text(rt, "HOST", 20, sf::Color(255, 220, 90), { bx, r.position.y + 14.f }, 2, 2.f);
                else text(rt, s.ready ? "READY" : "not ready", 20, s.ready ? sf::Color(120, 235, 130) : sf::Color(200, 180, 150),
                          { bx, r.position.y + 14.f }, 2, s.ready ? 2.f : 0.f);
                drawPing(rt, bx - 78.f, r.position.y + 50.f, s.ping, s.connected, 14);
                if (isHost && i != nv.myId && !counting &&
                    button(rt, "KICK", { { bx - 190.f, r.position.y + 22.f }, { 84.f, 34.f } }, true, 15)) { act.kind = Action::Kick; act.text = s.name; }
            } else {
                text(rt, "Waiting for player...", 20, sf::Color(170, 150, 120), { r.position.x + 116.f, r.position.y + sh * 0.5f }, 0, 0.f);
            }
        }

        // room settings (host edits, everyone sees)
        float sy = sy0 + 4 * (sh + 6.f) + 8.f;
        text(rt, isHost ? "ROOM SETTINGS" : "ROOM SETTINGS  (host only)", 16, sf::Color(255, 220, 140), { lx + 40.f, sy }, 0, 1.f);
        sy += 24.f;
        const bool canEdit = isHost && !counting;
        const float cx = lx + 300.f;
        RoomSettings ns = st; bool changed = false;
        {
            text(rt, "Match length", 18, sf::Color(240, 230, 210), { lx + 56.f, sy + 8.f }, 0, 0.f);
            const uint8_t opts[5] = { 1, 2, 3, 5, 10 }; int oi = 2;
            for (int k = 0; k < 5; ++k) if (opts[k] == st.minutes) oi = k;
            int d = stepper(rt, std::to_string(st.minutes) + (st.minutes == 1 ? " minute" : " minutes"), cx, sy, canEdit);
            if (d) { ns.minutes = opts[(oi + d + 5) % 5]; changed = true; }
            sy += 36.f;
            text(rt, "Treasures to win", 18, sf::Color(240, 230, 210), { lx + 56.f, sy + 8.f }, 0, 0.f);
            d = stepper(rt, std::to_string(st.winScore), cx, sy, canEdit);
            if (d) { ns.winScore = (uint8_t)std::clamp((int)st.winScore + d, 1, 5); changed = true; }
            sy += 36.f;
            text(rt, "Fog of war", 18, sf::Color(240, 230, 210), { lx + 56.f, sy + 8.f }, 0, 0.f);
            if (canEdit) { if (button(rt, st.fog ? "ON" : "OFF", { { cx + 55.f, sy }, { 120.f, 32.f } }, true, 18)) { ns.fog = !st.fog; changed = true; } }
            else text(rt, st.fog ? "ON" : "OFF", 20, sf::Color(215, 200, 175), { cx + 115.f, sy + 16.f }, 1, 0.f);
            sy += 36.f;
            text(rt, "Start countdown", 18, sf::Color(240, 230, 210), { lx + 56.f, sy + 8.f }, 0, 0.f);
            if (canEdit) { if (button(rt, st.countdown ? "5 SECONDS" : "OFF", { { cx + 55.f, sy }, { 120.f, 32.f } }, true, 16)) { ns.countdown = !st.countdown; changed = true; } }
            else text(rt, st.countdown ? "5 seconds" : "OFF", 20, sf::Color(215, 200, 175), { cx + 115.f, sy + 16.f }, 1, 0.f);
            sy += 36.f;
        }
        if (changed) { act.kind = Action::Settings; act.settings = ns; }

        float by = ly + lh - 100.f, bw = (lw - 72.f - 20.f) * 0.5f;
        if (isHost) {
            if (counting) { if (button(rt, "CANCEL START", { { lx + 36.f, by }, { bw, 66.f } })) act.kind = Action::CancelStart; }
            else if (button(rt, "START MATCH", { { lx + 36.f, by }, { bw, 66.f } })) act.kind = Action::Start;
        } else {
            bool me = nv.myId < 4 && nv.lobby.slots[nv.myId].ready;
            if (button(rt, me ? "NOT READY" : "READY", { { lx + 36.f, by }, { bw, 66.f } })) act.kind = Action::Ready;
        }
        if (button(rt, "LEAVE", { { lx + 56.f + bw, by }, { bw, 66.f } })) act.kind = Action::Leave;

        // right: chat
        const float cxp = 770.f, cy = 120.f, cw = 470.f, ch = 790.f;
        panel(rt, { { cxp, cy }, { cw, ch } });
        sf::RectangleShape inner({ cw - 36.f, ch - 150.f });
        inner.setPosition({ cxp + 18.f, cy + 50.f });
        inner.setFillColor(sf::Color(28, 18, 12, 190));
        rt.draw(inner);
        text(rt, "LOBBY CHAT   (/help, /w name text)", 16, sf::Color(255, 220, 140), { cxp + 26.f, cy + 22.f }, 0, 1.f);
        struct VL { std::string name, text; sf::Color col, tcol; bool first; float nw; };
        std::vector<VL> vls;
        const unsigned fs = 17;
        for (auto& e : chatLog_) {
            std::string nm = e.name + (e.name.empty() ? "" : ":");
            float nw = nm.empty() ? 0.f : ui::textWidth(*font_, fs, nm + " ");
            auto wr = ui::wrapText(*font_, fs, e.text, cw - 70.f, nw);
            for (size_t k = 0; k < wr.size(); ++k) vls.push_back({ k == 0 ? nm : "", wr[k], e.col, e.tcol, k == 0, nw });
        }
        const float lineH = 24.f, top = cy + 60.f, bot = cy + ch - 110.f;
        int maxL = (int)((bot - top) / lineH);
        int stt = std::max(0, (int)vls.size() - maxL);
        float yy = bot - (float)((int)vls.size() - stt) * lineH;
        for (int k = stt; k < (int)vls.size(); ++k, yy += lineH) {
            if (vls[k].first && !vls[k].name.empty()) text(rt, vls[k].name, fs, vls[k].col, { cxp + 30.f, yy }, 0, 1.f);
            text(rt, vls[k].text, fs, vls[k].tcol, { cxp + 30.f + (vls[k].first ? vls[k].nw : 0.f), yy }, 0, 0.f);
        }
        if (vls.empty()) text(rt, "No messages yet", 16, sf::Color(170, 150, 120), { cxp + 30.f, bot - lineH }, 0);
        field(rt, chat, { { cxp + 18.f, cy + ch - 84.f }, { cw - 36.f, 56.f } }, 20);
        text(rt, isHost ? "You are the host" : "Waiting for the host to start...", 16, sf::Color(255, 250, 220), { lx + lw * 0.5f, ly + lh - 18.f }, 1, 1.5f);

        if (counting) {   // start countdown banner (everyone sees it)
            sf::RectangleShape dim({ W, H }); dim.setFillColor(sf::Color(0, 0, 0, 110)); rt.draw(dim);
            panel(rt, { { W * 0.5f - 250.f, H * 0.5f - 90.f }, { 500.f, 180.f } });
            text(rt, "MATCH STARTS IN", 24, sf::Color(255, 240, 180), { W * 0.5f, H * 0.5f - 42.f }, 1, 2.f);
            text(rt, std::to_string((int)nv.lobby.countdown), 72, sf::Color(255, 226, 110), { W * 0.5f, H * 0.5f + 20.f }, 1, 5.f);
            if (isHost && button(rt, "CANCEL", { { W * 0.5f + 140.f, H * 0.5f + 30.f }, { 100.f, 44.f } }, true, 18)) act.kind = Action::CancelStart;
        }
    }
};

} // namespace menu
