// Client for the ninja_syndicate_go multiplayer game.
//
// Connects to the authoritative server, renders the shared map with four
// corner bases, drives the local player from the keyboard, and shows the
// live game state (positions, treasures, projectiles) plus a chat panel.
//
// Run:  game_client.exe [host] [port] [name]
#include "../game/Constants.hpp"
#include "../game/Map.hpp"
#include "../game/Protocol.hpp"
#include "UiTheme.hpp"
#include "Menu.hpp"
#include "NetClient.hpp"
#include "Sound.hpp"
#include "Fog.hpp"
#include "Sky.hpp"
#include "Weather.hpp"
#include "Trophy.hpp"

#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <SFML/System.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <queue>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class GameState { ConnectUI, Connecting, Lobby, Playing, Disconnected };

constexpr float DIRVX[4] = { 0.f, 0.f, -1.f, 1.f };
constexpr float DIRVY[4] = { 1.f, -1.f, 0.f, 0.f };

// Team palette (must stay in sync with the four ninja characters).
const sf::Color TEAM_COLOR[4] = {
    { 232, 80, 80 },   // red
    { 80, 150, 240 },  // blue
    { 90, 205, 110 },  // green
    { 240, 210, 80 }   // yellow
};
const char* CHAR_NAME[4] = { "NinjaRed", "NinjaBlue", "NinjaGreen", "NinjaYellow" };

sf::IntRect cell(int x, int y, int s = gc::TILE) { return {{x * s, y * s}, {s, s}}; }

struct Toast { std::string text; float age = 0.f; sf::Color color; };

struct Assets {
    sf::Texture field, water, nature, flower, treasure, projectile;
    sf::Texture floor, element, desert;  // Added desert for sandy bases
    sf::Texture waterRipple;  // Water decorations
    sf::Texture sheets[world::NUM_SHEETS];  // Floor, Water, Nature, House, Element, Field
    sf::Texture flags[4];                   // animated team flags
    sf::Texture chars[4];
    sf::Font font;
    sf::Font chatFont;                      // plain system font for chat text (readable), pixel font stays for the rest
    bool chatFontOk = false;
    bool fontOk = false;

    bool loadAll(const std::string& root) {
        auto loadTex = [](sf::Texture& t, const std::string& p) {
            return t.loadFromFile(p);
        };
        if (!loadTex(field,    root + "Backgrounds/Tilesets/TilesetField.png")) return false;
        if (!loadTex(water,    root + "Backgrounds/Tilesets/TilesetWater.png")) return false;
        if (!loadTex(nature,   root + "Backgrounds/Tilesets/TilesetNature.png")) return false;
        if (!loadTex(floor,    root + "Backgrounds/Tilesets/TilesetFloor.png")) return false;
        if (!loadTex(element,  root + "Backgrounds/Tilesets/TilesetElement.png")) return false;
        if (!loadTex(desert,   root + "Backgrounds/Tilesets/TilesetDesert.png")) return false;
        {
            for (int i = 0; i < world::NUM_SHEETS; ++i) {
                if (!loadTex(sheets[i], root + world::SHEET_FILE[i])) return false;
                sheets[i].setSmooth(false);
            }
            const char* fl[4] = { "Red", "Blue", "Green", "Yellow" };
            for (int i = 0; i < 4; ++i) {
                if (!loadTex(flags[i], root + std::string("Backgrounds/Animated/Flag/Flag") + fl[i] + "16x16.png")) return false;
                flags[i].setSmooth(false);
            }
        }
        if (!loadTex(waterRipple, root + "Backgrounds/Animated/Water Ripples/SpriteSheet16x16.png")) return false;
        if (!loadTex(flower,   root + "Backgrounds/Animated/Plant/SpriteSheet16x16.png")) return false;
        if (!loadTex(treasure, root + "Items/Treasure/GoldCup.png")) return false;
        if (!loadTex(projectile, root + "FX/Projectile/Fireball.png")) return false;
        for (int i = 0; i < 4; ++i)
            if (!loadTex(chars[i], root + std::string("Actor/Character/") + CHAR_NAME[i] + "/SpriteSheet.png"))
                return false;
        for (auto* t : { &field, &water, &nature, &flower, &treasure, &projectile, &floor, &element, &desert,
                         &waterRipple, &chars[0], &chars[1], &chars[2], &chars[3] })
            t->setSmooth(false);
        fontOk = font.openFromFile(root + "Ui/Font/NormalFont.ttf");
        if (!fontOk) fontOk = font.openFromFile("C:/Windows/Fonts/arial.ttf");
        for (const char* f : { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/verdana.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc" })
            if (!chatFontOk) chatFontOk = chatFont.openFromFile(f);
        return true;
    }
};

// Probe candidate roots so the game runs whether launched from the project
// root or from build/bin/.
std::string findAssetRoot() {
    const char* candidates[] = {
        "Ninja Adventure - Asset Pack/",
        "../Ninja Adventure - Asset Pack/",
        "../../Ninja Adventure - Asset Pack/",
        "../../../Ninja Adventure - Asset Pack/"
    };
    for (const char* c : candidates) {
        sf::Texture probe;
        if (probe.loadFromFile(std::string(c) + "Backgrounds/Tilesets/TilesetField.png"))
            return c;
    }
    return "Ninja Adventure - Asset Pack/";
}

using netc::Net;   // network layer lives in NetClient.hpp (encrypted channel, reconnect, ping)

void cfgText(sf::Text& t, const sf::Font& f, const std::string& s, unsigned sz, sf::Color fill, float thick = 0.f, sf::Color outline = sf::Color::Black) {
    t = sf::Text(f, ui::spaced(s, sz), sz);
    t.setFillColor(fill);
    if (thick > 0.f) { t.setOutlineColor(outline); t.setOutlineThickness(thick); }
}
void centerOrigin(sf::Text& t) {
    auto b = t.getLocalBounds();
    t.setOrigin({ b.position.x + b.size.x * 0.5f, b.position.y + b.size.y * 0.5f });
}

std::string playerName(const Snapshot& s, uint8_t id) {
    if (id < gc::NUM_PLAYERS) return s.players[id].name.empty() ? std::string("P") + std::to_string(int(id)) : s.players[id].name;
    return "?";
}

std::string eventText(const Snapshot& s, const GameEvent& e) {
    auto nm = [&](uint8_t id){ return playerName(s, id); };
    switch (e.type) {
        case EvType::Teleport: return nm(e.a) + " was sent home by " + nm(e.b);
        case EvType::Steal:     return nm(e.a) + " stole " + nm(e.b) + "'s treasure";
        case EvType::Drop:     return nm(e.a) + " dropped " + (e.c < gc::NUM_PLAYERS ? nm(e.c) + "'s" : std::string("a")) + " treasure!";
        case EvType::Recover:  return nm(e.a) + " recovered their treasure";
        case EvType::Returned: return (e.c < gc::NUM_PLAYERS ? nm(e.c) + "'s" : std::string("A")) + " treasure flew home";
        case EvType::Secret:   return {};   // discovery toast is generated locally
        case EvType::Deliver:  return nm(e.a) + " delivered a treasure!";
        case EvType::Join:     return nm(e.a) + " joined the hunt";
        case EvType::Leave:    return nm(e.a) + " left";
        case EvType::Lost:     return nm(e.a) + " lost connection";
        case EvType::Reconnected: return nm(e.a) + " reconnected";
        case EvType::GameOver: return e.a == 255 ? "Draw! Back to the lobby soon..." : (nm(e.a) + " wins! Back to the lobby soon...");
        default: return {};
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string host = gc::DEFAULT_HOST;
    unsigned short port = gc::SERVER_PORT;
    std::string name = "Player";
    bool quick = false;
    {
        int pos = 0;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--quick") { quick = true; continue; }
            if (pos == 0) host = a;
            else if (pos == 1) port = static_cast<unsigned short>(std::atoi(a.c_str()));
            else if (pos == 2) { name = a; if (name.size() > gc::NAME_LEN) name.resize(gc::NAME_LEN); }
            ++pos;
        }
    }

    {   // known_hosts (pinned server keys) lives next to the executable
        std::string a0 = argc > 0 ? argv[0] : "";
        size_t sl = a0.find_last_of("/\\");
        netc::dataDir() = sl == std::string::npos ? std::string() : a0.substr(0, sl + 1);
    }
    std::string root = findAssetRoot();
    Assets assets;
    if (!assets.loadAll(root)) {
        std::cerr << "Failed to load assets from '" << root << "'\n";
        return 1;
    }

    Sound::get().init(root);
    Sound::get().startMusic();

    sf::RenderWindow window(sf::VideoMode({1280, 960}), "ninja_syndicate_go");
    window.setFramerateLimit(60);

    // ---- landing page / lobby (src/client/Menu.hpp) ----
    // Automation env vars: GAME_AUTO=create|join:CODE, GAME_AUTO_START=<players>, GAME_AUTO_CHAT=<text>,
    // GAME_NAME=<name>, GAME_SCREEN=join, GAME_JOIN_TEXT=<text>, GAME_SHOT=<png> [+GAME_SHOT_ON=landing|join|lobby], GAME_SHOT_EXIT=1
    enum class AppMode { Menu, Game };
    AppMode appMode = AppMode::Menu;
    menu::Menu menuUi;
    if (!menuUi.load(root, assets.fontOk ? &assets.font : nullptr)) {
        std::cerr << "Failed to load menu assets\n";
        return 1;
    }
    menuUi.name.set(std::getenv("GAME_NAME") ? std::getenv("GAME_NAME") : name);
    menuUi.server.set(host + ":" + std::to_string(port));
    if (std::getenv("GAME_SCREEN") && std::string(std::getenv("GAME_SCREEN")) == "join") menuUi.setScreen(menu::Menu::Screen::Join);
    if (std::getenv("GAME_JOIN_TEXT")) menuUi.join.set(std::getenv("GAME_JOIN_TEXT"));
    if (std::getenv("GAME_ERR")) menuUi.error = std::getenv("GAME_ERR");
    std::string autoMode = std::getenv("GAME_AUTO") ? std::getenv("GAME_AUTO") : "";
    if (quick) autoMode = "create";
    bool autoDone = false, autoChatDone = false;
    int autoStart = std::getenv("GAME_AUTO_START") ? std::atoi(std::getenv("GAME_AUTO_START")) : 0;
    float menuClock = 0.f, waitClock = 0.f;
    bool pendingConnect = false, menuShotDone = false;
    bool autoTrustDone = false, autoSettingsDone = false, autoDropDone = false, autoGameChatDone = false;
    bool reconnecting = false, reconnAwait = false; int reconnAttempt = 0, reconnWelcomes = 0; float reconnLeft = 0.f, reconnNext = 0.f;
    menu::Action pendingAct;
    bool leavePrompt = false, escWas = false, yWas = false, nWas = false, enterWas = false;

    Net net;
    gcmap::generate();
    
    // 80x60-tile world (1280x960 px); camera shows 426x320 world px, scaled 3x into the 1280x960 window
    sf::View gameView({1280.f / 6.f, 160.f}, {1280.f / 3.f, 320.f});   // 3x zoom into the 1280x960 window (26.7 x 20 tiles)
    sky::Sky skyLayer;                     // drifting clouds + shadows (src/client/Sky.hpp)
    fog::Fog fogOfWar;                     // fog of war + minimap state (src/client/Fog.hpp)
    weather::Weather weatherFx;            // rain / water animation / leaves (src/client/Weather.hpp); F2 toggles, GAME_WEATHER=rain|clear forces
    weatherFx.init(root, &assets.waterRipple);
    float lastMatchTime = 0.f;
    bool fogAllRevealed = false;
    sf::View hudView({640.f, 480.f}, {1280.f, 960.f});

    // interpolation state for remote players
    std::array<sf::Vector2f, gc::NUM_PLAYERS> disp{};
    std::array<bool, gc::NUM_PLAYERS> dispInit{};

    std::deque<ui::ChatEntry> chatLines;   // display history
    ui::UiTheme uiTheme;
    uiTheme.load(root);
    sf::Clock uiClock;
    bool shotDone = false;
    std::vector<Toast> toasts;            // transient event banners (text, age, color)

    bool chatActive = false;
    bool chatSkipText = false;
    std::string chatInput;
    int lastFacing = gc::FACE_DOWN;
    bool spaceDown = false, leapDown = false;
    struct Spark { sf::Vector2f p, v; float age = 0.f; sf::Color col; };
    std::vector<Spark> sparks;                       // secret-discovery sparkle burst (world space)
    std::array<sf::Vector2f, gc::NUM_PLAYERS> lastDropPos{};   // where each trophy last lay on the ground (for the flight home)

    sf::Clock clock;        // per-frame delta (restarted every frame)
    sf::Clock worldClock;   // monotonic animation time: rain, flags, water, shadows (never restarted)
    while (window.isOpen()) {
        float dt = std::min(clock.restart().asSeconds(), 0.05f);
        Sound::get().tick(dt);   // music playlist + chase crossfade (also runs in the menus)

        // ================= menu (landing / join / lobby) =================
        if (appMode == AppMode::Menu) {
            menuClock += dt;
            // connect worker finished?
            auto cres = net.pollConnect();
            if (cres == netc::ConnectResult::Failed) {
                if (net.failKeyChanged) {
                    menuUi.keyWarn.active = true; menuUi.keyWarn.host = pendingAct.host; menuUi.keyWarn.port = pendingAct.port;
                    menuUi.keyWarn.newFp = net.failNewFp; menuUi.keyWarn.oldFp = net.failOldFp;
                    menuUi.fail("");
                } else menuUi.fail(net.failErr);
            }
            std::string rc, err; LobbyInfo li; bool started = false;
            net.roomState(rc, li, err, started);
            menu::View nv; nv.connected = net.connected; nv.code = rc; nv.myId = net.copyId(); nv.lobby = li;
            // server-side results
            if (!err.empty()) { net.disconnect(); menuUi.fail(err); }
            else if (menuUi.busy && !rc.empty()) menuUi.enterLobby();
            else if (menuUi.busy && !net.connecting() && waitClock > 6.f) { net.disconnect(); menuUi.fail("Server did not answer"); }
            else if (menuUi.screen == menu::Menu::Screen::Lobby && !net.connected) { net.disconnect(); menuUi.fail("Disconnected from server"); }
            if (menuUi.busy) waitClock += dt; else waitClock = 0.f;
            if (started && menuUi.screen == menu::Menu::Screen::Lobby) {
                appMode = AppMode::Game;
                uiClock.restart(); shotDone = false; autoDropDone = false; autoGameChatDone = false;
                chatLines.clear();
                continue;
            }
            // automation
            if (std::getenv("GAME_AUTO_TRUST") && menuUi.keyWarn.active && !autoTrustDone && menuUi.screenTime > 2.f) { autoTrustDone = true; menuUi.request(menu::Action::TrustKey); }
            if (!autoDone && menuClock > 0.7f && !menuUi.busy && menuUi.screen != menu::Menu::Screen::Lobby && !autoMode.empty()) {
                autoDone = true;
                if (autoMode == "create") menuUi.request(menu::Action::Create);
                else if (autoMode.rfind("join:", 0) == 0) menuUi.request(menu::Action::Join, autoMode.substr(5));
            }
            std::vector<ChatLine> newChat;
            if (menuUi.screen == menu::Menu::Screen::Lobby) {
                newChat = net.drainChat();
                const uint8_t me = net.copyId();
                for (auto& c : newChat) {   // popup sound in the lobby too
                    if (c.kind == CHAT_WHISPER_FROM) Sound::get().play(SfxId::Whisper, 1.0f);
                    else if (c.kind == CHAT_NORMAL && c.fromId != me) Sound::get().play(SfxId::ChatPop, 1.0f);
                }
            }
            else net.drainChat();
            if (menuUi.screen == menu::Menu::Screen::Lobby) {
                if (!autoChatDone && std::getenv("GAME_AUTO_CHAT") && menuUi.screenTime > (std::getenv("GAME_AUTO_CHAT_AT") ? (float)std::atof(std::getenv("GAME_AUTO_CHAT_AT")) : 1.f)) {
                    autoChatDone = true; net.sendChat(std::getenv("GAME_AUTO_CHAT"));
                }
                if (!autoSettingsDone && std::getenv("GAME_AUTO_SETTINGS") && nv.myId == li.hostId && menuUi.screenTime > 1.5f) {
                    autoSettingsDone = true;   // e.g. "m=5,w=2,fog=0,cd=1"
                    RoomSettings rs = li.settings;
                    std::string spec = std::getenv("GAME_AUTO_SETTINGS");
                    for (size_t pos = 0; pos < spec.size();) {
                        size_t e = spec.find(',', pos); if (e == std::string::npos) e = spec.size();
                        std::string kv = spec.substr(pos, e - pos); pos = e + 1;
                        size_t eq = kv.find('='); if (eq == std::string::npos) continue;
                        std::string k = kv.substr(0, eq); int v = std::atoi(kv.substr(eq + 1).c_str());
                        if (k == "m") rs.minutes = (uint8_t)v; else if (k == "w") rs.winScore = (uint8_t)v;
                        else if (k == "fog") rs.fog = v != 0; else if (k == "cd") rs.countdown = v != 0;
                    }
                    net.sendSettings(rs);
                }
                int n = 0; for (auto& sl : li.slots) if (sl.active) ++n;
                if (autoStart > 0 && n >= autoStart && nv.myId == li.hostId && menuUi.screenTime > 6.f) {
                    autoStart = 0; net.sendSimple(Msg::C2S_Start);
                }
            }
            menu::Action act = menuUi.frame(window, dt, nv, newChat);
            auto startConnect = [&](bool trust) {
                net.disconnect();
                netc::ConnectParams cp;
                cp.host = pendingAct.host; cp.port = pendingAct.port; cp.name = pendingAct.name; cp.code = pendingAct.code;
                cp.mode = pendingAct.kind == menu::Action::Create ? JoinMode::Create : JoinMode::Join;
                cp.trustNewKey = trust;
                menuUi.busy = true;
                net.beginConnect(cp);
            };
            switch (act.kind) {
                case menu::Action::Quit: net.disconnect(); window.close(); break;
                case menu::Action::Create:
                case menu::Action::Join: pendingAct = act; startConnect(false); break;
                case menu::Action::TrustKey: pendingAct.host = act.host; pendingAct.port = act.port; startConnect(true); break;
                case menu::Action::Cancel: net.cancelConnect(); net.disconnect(); menuUi.busy = false; menuUi.error.clear(); break;
                case menu::Action::Start: net.sendSimple(Msg::C2S_Start); break;
                case menu::Action::CancelStart: net.sendSimple(Msg::C2S_StartCancel); break;
                case menu::Action::Ready: net.sendSimple(Msg::C2S_Ready); break;
                case menu::Action::Chat: net.sendChat(act.text); break;
                case menu::Action::Settings: net.sendSettings(act.settings); break;
                case menu::Action::Kick: net.sendKick(act.text); break;
                case menu::Action::Leave: net.disconnect(); menuUi.fail("", true); break;
                default: break;
            }
            if (std::getenv("GAME_SHOT") && std::getenv("GAME_SHOT_ON") && !menuShotDone && !menuUi.busy) {
                std::string on = std::getenv("GAME_SHOT_ON");
                auto sc = menuUi.screen;
                float shotAt = std::getenv("GAME_SHOT_DELAY") ? (float)std::atof(std::getenv("GAME_SHOT_DELAY")) : (on == "lobby" ? 3.5f : 1.5f);
                bool match = menuUi.screenTime > shotAt &&
                             ((on == "landing" && sc == menu::Menu::Screen::Landing) || (on == "join" && sc == menu::Menu::Screen::Join) ||
                              (on == "lobby" && sc == menu::Menu::Screen::Lobby));
                if (match) {
                    sf::Texture shot(window.getSize()); shot.update(window);
                    (void)shot.copyToImage().saveToFile(std::getenv("GAME_SHOT"));
                    menuShotDone = true;
                    if (std::getenv("GAME_SHOT_EXIT")) window.close();
                }
            }
            window.display();
            continue;
        }

        // ---- in-game: server messages, reconnect, Esc asks to leave the match ----
        {
            bool esc = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape);
            bool yk = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Y);
            bool nk = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::N);
            bool en = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
            bool focusOk = window.hasFocus();
            if (focusOk) {
                if (!leavePrompt) { if (esc && !escWas && !chatActive) leavePrompt = true; }
                else {
                    if ((yk && !yWas) || (en && !enterWas)) { leavePrompt = false; reconnecting = false; net.disconnect(); menuUi.fail("", true); appMode = AppMode::Menu; escWas = esc; yWas = yk; enterWas = en; continue; }
                    if ((nk && !nWas) || (esc && !escWas)) leavePrompt = false;
                }
            }
            escWas = esc; yWas = yk; nWas = nk; enterWas = en;

            // automation: simulate a network drop after N seconds in the match
            if (!autoDropDone && std::getenv("GAME_AUTO_DROP") && uiClock.getElapsedTime().asSeconds() > (float)std::atof(std::getenv("GAME_AUTO_DROP"))) {
                autoDropDone = true; net.dropLink();
            }
            if (!autoGameChatDone && std::getenv("GAME_AUTO_GAMECHAT") && uiClock.getElapsedTime().asSeconds() > 1.5f) {
                autoGameChatDone = true; net.sendChat(std::getenv("GAME_AUTO_GAMECHAT"));
            }

            if (net.takeToLobby()) {   // match finished: the room is back in the lobby (rematch / change settings)
                net.clearMatchStarted(); reconnecting = false; leavePrompt = false;
                menuUi.enterLobby(); appMode = AppMode::Menu; menuUi.busy = false;
                continue;
            }
            {
                std::string rc2, err2; LobbyInfo li2; bool st2 = false;
                net.roomState(rc2, li2, err2, st2);
                if (!err2.empty()) {   // kicked / server shut down / session expired: no reconnect
                    reconnecting = false; leavePrompt = false;
                    net.disconnect(); menuUi.fail(err2, true); appMode = AppMode::Menu;
                    continue;
                }
            }
            if (!net.connected && !reconnecting && !net.connecting()) {
                if (!net.canReconnect()) {
                    leavePrompt = false; net.disconnect(); menuUi.fail("Disconnected from server", true); appMode = AppMode::Menu;
                    continue;
                }
                reconnecting = true; reconnAttempt = 0; reconnLeft = 20.f; reconnNext = 0.f; reconnAwait = false;
                reconnWelcomes = net.welcomeCount();
                net.dropLink();
            }
            if (reconnecting) {
                reconnLeft -= dt; reconnNext -= dt;
                auto r = net.pollConnect();
                if (r == netc::ConnectResult::Failed) { reconnNext = 1.5f; if (net.failKeyChanged) reconnLeft = 0.f; }
                if (r == netc::ConnectResult::Ok) reconnAwait = true;
                if (reconnAwait) {
                    if (net.welcomeCount() > reconnWelcomes) { reconnecting = false; reconnAwait = false; toasts.push_back({ "Reconnected", 0.f, sf::Color(140, 235, 150) }); }
                    else if (!net.connected) { reconnAwait = false; reconnNext = 1.5f; }
                }
                if (reconnecting && !reconnAwait && !net.connecting() && reconnNext <= 0.f && reconnLeft > 0.f) {
                    ++reconnAttempt;
                    netc::ConnectParams cp = net.last;
                    std::string rcode; LobbyInfo lx; std::string ex; bool sx = false;
                    net.roomState(rcode, lx, ex, sx);
                    cp.mode = JoinMode::Rejoin; cp.code = rcode; cp.token = net.token; cp.trustNewKey = false;
                    net.beginConnect(cp);
                }
                if (reconnecting && reconnLeft <= 0.f) {
                    reconnecting = false; net.disconnect(); menuUi.fail("Could not reconnect to the server", true); appMode = AppMode::Menu;
                    continue;
                }
            }
        }

        // ---- events (chat typing, close) ----
        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            if (auto* k = ev->getIf<sf::Event::KeyPressed>()) {
                if (!chatActive) { // sound hotkeys: M mute, +/- or ] [ volume
                    using K = sf::Keyboard::Key;
                    float dv = 0.f; bool mt = false;
                    if (k->code == K::F3) { bool on = weatherFx.toggleStorm(); toasts.push_back({ on ? "Storm over the whole map" : "Storms back to local cells", 0.f, sf::Color(200, 230, 255) }); }
                    if (k->code == K::F2) { weatherFx.toggle(); toasts.push_back({ weatherFx.enabled() ? "Weather: on" : "Weather: off", 0.f, sf::Color(200, 230, 255) }); }
                    if (k->code == K::M) mt = true;
                    else if (k->code == K::Equal || k->code == K::Add || k->code == K::RBracket) dv = 0.1f;
                    else if (k->code == K::Hyphen || k->code == K::Subtract || k->code == K::LBracket) dv = -0.1f;
                    if (mt) Sound::get().toggleMute();
                    if (dv != 0.f) Sound::get().adjustVolume(dv);
                    if (mt || dv != 0.f) {
                        toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                            [](const Toast& t){ return t.text.rfind("Volume", 0) == 0 || t.text.rfind("Sound", 0) == 0; }), toasts.end());
                        toasts.push_back({ Sound::get().status(), 0.f, sf::Color(200, 230, 255) });
                    }
                }
                if (k->code == sf::Keyboard::Key::Escape) { chatActive = false; chatInput.clear(); }
                else if (k->code == sf::Keyboard::Key::Enter) {
                    if (!chatActive) { chatActive = true; chatInput.clear(); }
                    else {
                        if (!chatInput.empty()) net.sendChat(chatInput);
                        chatActive = false; chatInput.clear();
                    }
                } else if (!chatActive && k->code == sf::Keyboard::Key::T) {
                    chatActive = true; chatInput.clear(); chatSkipText = true;   // T opens chat (swallow its own character)
                } else if (chatActive && k->code == sf::Keyboard::Key::Backspace) {
                    if (!chatInput.empty()) chatInput.pop_back();
                } else if (chatActive && k->code == sf::Keyboard::Key::Tab) {   // Tab-complete a player name after /w, /msg, /pm, /kick
                    Snapshot ts = net.copySnap(); uint8_t me2 = net.copyId();
                    std::vector<std::string> nms;
                    for (int pi = 0; pi < gc::NUM_PLAYERS; ++pi) if (ts.players[pi].active && pi != me2) nms.push_back(ts.players[pi].name);
                    menu::tabComplete(chatInput, nms);
                }
            }
            if (chatActive) {
                if (auto* te = ev->getIf<sf::Event::TextEntered>()) {
                    char32_t c = te->unicode;
                    if (chatSkipText) chatSkipText = false;
                    else if (c >= 32 && c < 127 && chatInput.size() < gc::CHAT_LEN)
                        chatInput.push_back(static_cast<char>(c));
                }
            }
        }

        // ---- local input + send to server ----
        ClientInput in;
        in.facing = lastFacing;
        if (!chatActive) {
            float dx = 0, dy = 0;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Left))  dx -= 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Right)) dx += 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Up))    dy -= 1;
            if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::S) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Down))  dy += 1;
            in.dirX = dx; in.dirY = dy;
            if (dx || dy) {
                if (std::abs(dx) > std::abs(dy)) in.facing = dx < 0 ? gc::FACE_LEFT : gc::FACE_RIGHT;
                else in.facing = dy < 0 ? gc::FACE_UP : gc::FACE_DOWN;
                lastFacing = in.facing;
            }
            bool space = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Space);
            in.shoot = space && !spaceDown;
            spaceDown = space;
            bool leapKey = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift) ||
                           sf::Keyboard::isKeyPressed(sf::Keyboard::Key::J);
            in.leap = leapKey && !leapDown && window.hasFocus();
            leapDown = leapKey;
        } else leapDown = true;   // typing: swallow the key, no edge when chat closes while it is held
        if (std::getenv("GAME_BOT") && net.hasSnapshot()) {   // QA autopilot: shortest path (walk + leap edges) to the nearest enemy/dropped treasure, shoot now and then
            static float botShoot = 0.f;
            Snapshot bs = net.copySnap(); uint8_t me = net.copyId();
            if (me < gc::NUM_PLAYERS && bs.players[me].active && !bs.gameOver && bs.players[me].leapPhase == 0) {
                const auto& pl = bs.players[me];
                float tx = bs.treasures[me].homeX, ty = bs.treasures[me].homeY;
                if (!pl.carrying) {
                    float best = 1e18f;
                    for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                        const auto& tt = bs.treasures[i];
                        const bool dropped = tt.state == (uint8_t)TState::Dropped;
                        const bool home = tt.state == (uint8_t)TState::AtHome && i != me;
                        if (!dropped && !home) continue;   // dropped trophies (even mine: touching it recovers it) are chased too
                        float d = std::hypot(tt.x - pl.x, tt.y - pl.y) - (dropped ? 60.f : 0.f);
                        if (d < best) { best = d; tx = tt.x; ty = tt.y; }
                    }
                }
                const int W = gc::MAP_W, H = gc::MAP_H;
                int sx = std::clamp((int)((pl.x + 8) / gc::TILE), 0, W - 1), sy = std::clamp((int)((pl.y + 8) / gc::TILE), 0, H - 1);
                int gx = std::clamp((int)(tx / gc::TILE), 0, W - 1), gy = std::clamp((int)(ty / gc::TILE), 0, H - 1);
                // Dijkstra from the goal: walking costs 1, a leap (fly over 1-3 water/low cells, land <= 3 tiles later) costs 3.
                static std::vector<int> prev, dist; static std::vector<int8_t> jdir;   // jdir[c] >= 0: leaving c toward prev[c] is a leap in that direction
                prev.assign(W * H, -2); dist.assign(W * H, 1 << 28); jdir.assign(W * H, -1);
                using QE = std::pair<int, int>;
                std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
                dist[gy * W + gx] = 0; prev[gy * W + gx] = -1; pq.push({ 0, gy * W + gx });
                const int dxs[4] = { 0, 0, -1, 1 }, dys[4] = { 1, -1, 0, 0 };   // gc::FACE_DOWN/UP/LEFT/RIGHT order
                const int maxGap = pl.carrying ? 2 : 3;
                while (!pq.empty()) {
                    auto [dc, c] = pq.top(); pq.pop();
                    if (dc > dist[c]) continue;
                    if (c == sy * W + sx) break;
                    int cx = c % W, cy = c / W;
                    for (int k = 0; k < 4; ++k) {   // walking edges
                        int nx = cx + dxs[k], ny = cy + dys[k];
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H || gcmap::solidTile(nx, ny)) continue;
                        int n = ny * W + nx;
                        if (dc + 1 < dist[n]) { dist[n] = dc + 1; prev[n] = c; jdir[n] = -1; pq.push({ dc + 1, n }); }
                    }
                    if (!gcmap::solidTile(cx, cy)) {   // reverse leap edges: take-off X = c - dir*(gap+1), flying over `gap` flyOver cells
                        for (int k = 0; k < 4; ++k)
                            for (int gap = 1; gap <= maxGap; ++gap) {
                                bool ok = true;
                                for (int j = 1; j <= gap && ok; ++j) ok = gcmap::flyOver(cx - dxs[k] * j, cy - dys[k] * j);
                                int ox = cx - dxs[k] * (gap + 1), oy = cy - dys[k] * (gap + 1);
                                if (!ok || ox < 0 || oy < 0 || ox >= W || oy >= H || gcmap::solidTile(ox, oy)) continue;
                                int n = oy * W + ox;
                                if (dc + 3 < dist[n]) { dist[n] = dc + 3; prev[n] = c; jdir[n] = (int8_t)k; pq.push({ dc + 3, n }); }
                            }
                    }
                }
                int nxt = prev[sy * W + sx];
                if (nxt >= 0) {
                    float wx = (nxt % W) * gc::TILE + 8.f, wy = (nxt / W) * gc::TILE + 8.f;
                    float ddx = wx - (pl.x + 8), ddy = wy - (pl.y + 8);
                    const int jd = jdir[sy * W + sx];
                    if (jd >= 0) {   // leap edge: centre on the take-off tile, face the gap, press leap when the cooldown is ready
                        const float cxp = sx * gc::TILE + 8.f, cyp = sy * gc::TILE + 8.f;
                        const float ex = cxp - (pl.x + 8), ey = cyp - (pl.y + 8);
                        if (std::abs(ex) > 2.f || std::abs(ey) > 2.f) {
                            in.dirX = std::abs(ex) > 1.5f ? (ex > 0 ? 1.f : -1.f) : 0.f;
                            in.dirY = std::abs(ey) > 1.5f ? (ey > 0 ? 1.f : -1.f) : 0.f;
                            if (in.dirX != 0 && in.dirY != 0) { if (std::abs(ex) > std::abs(ey)) in.dirY = 0; else in.dirX = 0; }
                        } else {
                            in.dirX = in.dirY = 0.f;
                            in.facing = jd; lastFacing = jd;
                            if (pl.leapCd == 0) in.leap = true;
                        }
                    } else {
                        in.dirX = std::abs(ddx) > 1.5f ? (ddx > 0 ? 1.f : -1.f) : 0.f;
                        in.dirY = std::abs(ddy) > 1.5f ? (ddy > 0 ? 1.f : -1.f) : 0.f;
                        if (in.dirX != 0 && in.dirY != 0) { if (std::abs(ddx) > std::abs(ddy)) in.dirY = 0; else in.dirX = 0; }
                    }
                    if (in.dirX != 0 || in.dirY != 0) {
                        in.facing = in.dirX < 0 ? gc::FACE_LEFT : in.dirX > 0 ? gc::FACE_RIGHT : in.dirY < 0 ? gc::FACE_UP : gc::FACE_DOWN;
                        lastFacing = in.facing;
                    }
                }
                botShoot -= dt;
                if (botShoot <= 0.f) { in.shoot = !pl.carrying && !std::getenv("GAME_BOT_NOSHOOT"); botShoot = 1.5f; }
            }
        }
        net.sendInput(in);
        { // sound: own footsteps + empty-ammo click
            Snapshot ss = net.hasSnapshot() ? net.copySnap() : Snapshot{};
            uint8_t me = net.copyId();
            bool hasMe = me < gc::NUM_PLAYERS && ss.players[me].active;
            Sound::get().step(dt, hasMe && !chatActive && (in.dirX != 0 || in.dirY != 0));
            bool anyCarried = false;   // tense music while ANYONE runs with a trophy (plus the old local-carrier rule)
            for (auto& tt : ss.treasures) if (tt.state == (uint8_t)TState::Carried) anyCarried = true;
            Sound::get().setChase(hasMe && (ss.players[me].carrying || anyCarried) && !ss.gameOver);
            if (in.shoot && hasMe && ss.players[me].ammo <= 0) Sound::get().play(SfxId::Empty);
        }

        // ---- pull latest state ----
        Snapshot s = net.hasSnapshot() ? net.copySnap() : Snapshot{};
        uint8_t myId = net.copyId();

        // drain new chat into the display history
        for (auto& c : net.drainChat()) {
            ui::ChatEntry ce;
            ce.name = c.name.empty() ? std::string("P") + std::to_string(int(c.fromId)) : c.name;
            ce.text = c.text;
            ce.colorIndex = c.fromId < gc::NUM_PLAYERS ? (int)c.fromId : -1;
            if (c.kind == CHAT_WHISPER_FROM) { ce.name = "[whisper from " + ce.name + "]"; ce.colorIndex = -2; }
            else if (c.kind == CHAT_WHISPER_TO) { ce.name = "[to " + ce.name + "]"; ce.colorIndex = -2; }
            else if (c.kind == CHAT_SYSTEM) { ce.name = "server"; ce.colorIndex = -3; }
            chatLines.push_back(ce);
            if (c.kind == CHAT_WHISPER_FROM) Sound::get().play(SfxId::Whisper, 1.0f);
            else if (c.kind == CHAT_NORMAL && c.fromId != myId) Sound::get().play(SfxId::ChatPop, 1.0f);   // popup sound for teammates' messages
        }
        for (auto& cl : chatLines) cl.age += dt;
        if (std::getenv("GAME_CHAT_OPEN") && !chatActive && chatInput.empty()) { chatActive = true; chatInput = "typing a message..."; }   // screenshot helper
        if (std::getenv("UI_FAKECHAT") && chatLines.empty()) {
            chatLines.push_back({"Ayaka", "hey, who has the red treasure?", 0, 0.f});
            chatLines.push_back({"Bot2", "me! heading home now", 1, 0.f});
            chatLines.push_back({"Mika", "this is a really long message that should definitely wrap around to the next line in the chat panel nicely", 2, 0.f});
            chatLines.push_back({"You", "shooting you all haha", 3, 0.f});
        }
        while (chatLines.size() > gc::CHAT_HISTORY) chatLines.pop_front();

        // drain events into toasts
        for (auto& e : net.drainEvents()) {
            { // sound hooks
                auto& snd = Sound::get();
                float av = 1.f;
                if (e.a < gc::NUM_PLAYERS && myId < gc::NUM_PLAYERS && e.a != myId)
                    av = Sound::distVol(s.players[e.a].x - disp[myId].x, s.players[e.a].y - disp[myId].y);
                switch (e.type) {
                    case EvType::Shoot:    snd.play(SfxId::Shoot, e.a == myId ? 1.f : av); break;
                    case EvType::Teleport: snd.play(SfxId::Hit); snd.play(SfxId::Teleport, 0.8f); break;
                    case EvType::Steal:    snd.play(SfxId::Pickup); snd.play(SfxId::Alarm); break;   // alarm is global
                    case EvType::Drop:     snd.play(SfxId::Drop); break;
                    case EvType::Recover:  snd.play(SfxId::Recover); break;
                    case EvType::Returned: snd.play(SfxId::Return, 0.8f); break;
                    case EvType::Leap:     snd.play(SfxId::Leap, e.a == myId ? 1.f : av); break;
                    case EvType::Land:     snd.play(SfxId::Land, e.a == myId ? 1.f : av); break;
                    case EvType::Secret:   if (e.a == myId) snd.play(SfxId::Secret); break;
                    case EvType::Deliver:  snd.play(SfxId::Deliver); break;
                    case EvType::Join:     if (e.a != myId) snd.play(SfxId::Join); break;
                    case EvType::GameOver: snd.play(e.a == myId ? SfxId::Win : SfxId::Lose); break;
                    default: break;
                }
            }
            std::string t = eventText(s, e);
            if (t.empty()) continue;
            sf::Color col = (e.a < gc::NUM_PLAYERS) ? TEAM_COLOR[e.a] : sf::Color::White;
            if (e.type == EvType::GameOver) col = sf::Color(255, 230, 120);
            if (e.type == EvType::Returned && e.c < gc::NUM_PLAYERS) col = TEAM_COLOR[e.c];
            toasts.push_back({ t, 0.f, col });
        }
        for (auto& t : toasts) t.age += dt;
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(),
                    [](const auto& t){ return t.age > 3.5f; }), toasts.end());

        // interpolation of player display positions toward the snapshot
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (!s.players[i].active) { dispInit[i] = false; continue; }
            sf::Vector2f target{ s.players[i].x, s.players[i].y };
            if (!dispInit[i]) { disp[i] = target; dispInit[i] = true; continue; }
            float jump = std::hypot(target.x - disp[i].x, target.y - disp[i].y);
            if (jump > 40.f) { disp[i] = target; continue; } // teleport -> snap
            float a = std::min(1.f, dt * 15.f);
            disp[i] += (target - disp[i]) * a;
        }

        // trophy bookkeeping: remember where a trophy lay so the flight home can be drawn smoothly
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& t = s.treasures[i];
            if (t.state == (uint8_t)TState::Dropped || t.state == (uint8_t)TState::Carried) lastDropPos[i] = { t.x, t.y };
        }
        // secret passage discovery: my tile carries a passage id I have not found yet (fog reset at match start clears it)
        if (myId < gc::NUM_PLAYERS && s.players[myId].active && net.hasSnapshot()) {
            const sf::Vector2f mc = disp[myId] + sf::Vector2f(8.f, 8.f);
            const int sid = gcmap::secretAt((int)(mc.x / gc::TILE), (int)(mc.y / gc::TILE));
            if (sid > 0 && fogOfWar.discover(sid)) {
                toasts.push_back({ "Secret passage found!", 0.f, sf::Color(130, 255, 160) });
                Sound::get().play(SfxId::Secret);
                for (int k = 0; k < 18; ++k) {
                    const float a = k * 0.3491f, sp = 25.f + (k % 5) * 9.f;
                    sparks.push_back({ mc, { std::cos(a) * sp, std::sin(a) * sp - 10.f }, 0.f,
                                       (k & 1) ? sf::Color(255, 240, 140) : sf::Color(140, 255, 170) });
                }
            }
        }
        for (auto& sp : sparks) { sp.age += dt; sp.p += sp.v * dt; sp.v.y += 30.f * dt; }
        sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& sp){ return sp.age > 0.9f; }), sparks.end());

        // ---- Update camera to follow local player ----
        if (myId != 255 && myId < gc::NUM_PLAYERS) {
            sf::Vector2f playerPos = disp[myId] + sf::Vector2f(8.f, 8.f);   // sprite centre
            // Center camera on player, clamped to map bounds
            float mapW = gc::MAP_W * gc::TILE;
            float mapH = gc::MAP_H * gc::TILE;
            float viewW = gameView.getSize().x;  // 640
            float viewH = gameView.getSize().y;  // 480
            
            // If map is smaller than view, center the map
            float camX, camY;
            if (mapW <= viewW) {
                camX = mapW / 2.f;
            } else {
                camX = std::clamp(playerPos.x, viewW / 2.f, mapW - viewW / 2.f);
            }
            
            if (mapH <= viewH) {
                camY = mapH / 2.f;
            } else {
                camY = std::clamp(playerPos.y, viewH / 2.f, mapH - viewH / 2.f);
            }
            
            // snap the view's top-left to whole world pixels so every sprite pixel maps to exactly 3x3 screen pixels
            gameView.setCenter(sf::Vector2f(std::floor(camX - viewW / 2.f + 0.5f) + viewW / 2.f,
                                            std::floor(camY - viewH / 2.f + 0.5f) + viewH / 2.f));

            // new round (match timer jumped back up) -> forget the explored map; then reveal around the player
            if (s.matchTime > lastMatchTime + 5.f) fogOfWar.reset();
            lastMatchTime = s.matchTime;
            if (net.hasSnapshot()) fogOfWar.reveal(playerPos);
            if (!s.fog && net.hasSnapshot() && !fogAllRevealed) {   // room has fog of war switched off: explore the whole map
                for (float wy = 0.f; wy < gc::MAP_H * gc::TILE; wy += 96.f) for (float wx = 0.f; wx < gc::MAP_W * gc::TILE; wx += 96.f) fogOfWar.reveal({ wx, wy });
                fogOfWar.reveal(playerPos); fogAllRevealed = true;
            }
            if (s.fog) fogAllRevealed = false;
        }

        // ---- render world ----
        window.setView(gameView);
        window.clear(sf::Color(18, 30, 40));
        float time = worldClock.getElapsedTime().asSeconds();

        // ==== WORLD RENDERING (data generated by tools/build_world.py) ====
        // visible tile range (culling) -- ground/overlay tiles are batched per sheet into vertex arrays
        const sf::FloatRect viewRect({ gameView.getCenter().x - gameView.getSize().x * 0.5f,
                                       gameView.getCenter().y - gameView.getSize().y * 0.5f }, gameView.getSize());
        const int tx0 = std::max(0, (int)std::floor(viewRect.position.x / gc::TILE) - 1);
        const int ty0 = std::max(0, (int)std::floor(viewRect.position.y / gc::TILE) - 1);
        const int tx1 = std::min(world::W - 1, (int)std::floor((viewRect.position.x + viewRect.size.x) / gc::TILE) + 1);
        const int ty1 = std::min(world::H - 1, (int)std::floor((viewRect.position.y + viewRect.size.y) / gc::TILE) + 1);
        {
            static std::vector<sf::Vertex> tileVerts[world::NUM_SHEETS];
            for (auto& v : tileVerts) v.clear();
            auto addTile = [&](uint16_t code, int tx, int ty) {
                if (code == world::NONE) return;
                int sheet = code >> 12, row = (code >> 6) & 63, col = code & 63;
                float px = float(tx * gc::TILE), py = float(ty * gc::TILE), s = float(gc::TILE);
                float u = float(col * gc::TILE), v = float(row * gc::TILE);
                auto& a = tileVerts[sheet];
                sf::Vertex q00{ {px, py}, sf::Color::White, {u, v} }, q10{ {px + s, py}, sf::Color::White, {u + s, v} },
                           q01{ {px, py + s}, sf::Color::White, {u, v + s} }, q11{ {px + s, py + s}, sf::Color::White, {u + s, v + s} };
                a.push_back(q00); a.push_back(q10); a.push_back(q01);
                a.push_back(q10); a.push_back(q11); a.push_back(q01);
            };
            for (int y = ty0; y <= ty1; ++y)
                for (int x = tx0; x <= tx1; ++x) {
                    addTile(world::GROUND[y * world::W + x], x, y);
                    addTile(world::OVERLAY[y * world::W + x], x, y);
                }
            // batches go out in sheet order: grounds only use sheets 0 (Floor) / 1 (Water); every overlay either shares its
            // ground's sheet (pushed after it) or uses a later sheet, so layering stays correct.
            for (int sh = 0; sh < world::NUM_SHEETS; ++sh) {
                if (tileVerts[sh].empty()) continue;
                sf::RenderStates rs(&assets.sheets[sh]);
                window.draw(tileVerts[sh].data(), tileVerts[sh].size(), sf::PrimitiveType::Triangles, rs);
            }
        }

        weatherFx.setView(viewRect); weatherFx.update(dt);
        weatherFx.drawGround(window, viewRect, time);   // animated water ripples + rain puddles (ground level)
        skyLayer.setCover(weatherFx.intensity());
        skyLayer.drawShadows(window, viewRect, time);   // cloud shadows on the ground (below props/players)

        // base floor tints
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& b = gcmap::bases()[i];
            sf::RectangleShape floor(sf::Vector2f{ static_cast<float>(b.cw * gc::TILE), static_cast<float>(b.ch * gc::TILE) });
            floor.setPosition({ static_cast<float>(b.cx * gc::TILE), static_cast<float>(b.cy * gc::TILE) });
            sf::Color c = TEAM_COLOR[i];
            floor.setFillColor(sf::Color(c.r, c.g, c.b, 38));
            window.draw(floor);
        }

        // y-sorted props: drawn interleaved with the players (see below)
        int nextObj = 0;
        int nextFlag = 0;
        auto drawPropsUpTo = [&](float footY) {
            while (nextObj < world::NUM_OBJECTS &&
                   (world::OBJECTS[nextObj].ty + world::OBJECTS[nextObj].h) * gc::TILE <= footY) {
                const auto& o = world::OBJECTS[nextObj++];
                if (o.tx + o.w < tx0 || o.tx > tx1 || o.ty + o.h < ty0 || o.ty > ty1) continue;   // culled
                sf::Sprite sp(assets.sheets[o.sheet]);
                sp.setTextureRect({ { o.c * gc::TILE, o.r * gc::TILE }, { o.w * gc::TILE, o.h * gc::TILE } });
                sp.setPosition({ float(o.tx * gc::TILE), float(o.ty * gc::TILE) });
                window.draw(sp);
            }
            (void)nextFlag;
        };
        // flags are small and always sit on the ground: draw with the ground props
        {
            int frame = (int)(time * 6.f) & 3;
            for (int i = 0; i < world::NUM_FLAGS; ++i) {
                const auto& f = world::FLAGS[i];
                sf::Sprite sp(assets.flags[f.team]);
                sp.setTextureRect(cell(frame, 0));
                sp.setPosition({ float(f.tx * gc::TILE), float(f.ty * gc::TILE) });
                window.draw(sp);
            }
        }

        // treasures on the ground, by state: home (bob + owner ring), dropped (bob + countdown arc + pickup-lock flash),
        // returning (flies home with a short trail), carried (drawn with the carrier below + beacon layer after the fog), inactive (not drawn)
        sf::Sprite tr(assets.treasure);
        tr.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& t = s.treasures[i];
            const TState st = (TState)t.state;
            if (st == TState::Carried || st == TState::Inactive) continue;
            const sf::Color oc = TEAM_COLOR[i];
            sf::Vector2f pos{ t.x, t.y };
            float lift = 0.f, flightE = 0.f;
            if (st == TState::Returning) {
                const float f = 1.f - std::clamp(t.timer / gc::RETURN_TRAVEL, 0.f, 1.f);
                flightE = f * f * (3.f - 2.f * f);
                const sf::Vector2f from = lastDropPos[i], home{ t.homeX, t.homeY };
                pos = from + (home - from) * flightE;
                lift = std::sin(trophy::PI * f) * 14.f;
            } else if (st == TState::AtHome) pos = { t.x, t.y };
            if (s.fog && !fogOfWar.visible(pos.x, pos.y)) continue;
            const float bob = (st == TState::Returning) ? -lift : std::sin(time * 2.5f + i) * 1.2f;
            if (st == TState::Returning) {   // short trail of fading ghosts behind the cup
                const sf::Vector2f from = lastDropPos[i], home{ t.homeX, t.homeY };
                for (int k = 4; k >= 1; --k) {
                    const float ef = std::max(0.f, flightE - k * 0.07f);
                    const sf::Vector2f gp = from + (home - from) * ef;
                    tr.setScale({ 1.8f - 0.2f * k, 1.8f - 0.2f * k });
                    tr.setColor(sf::Color(oc.r, oc.g, oc.b, (uint8_t)(150 - k * 30)));
                    tr.setPosition({ gp.x, gp.y - lift * 0.8f });
                    window.draw(tr);
                }
            }
            if (st == TState::Dropped || st == TState::Returning) {   // ground shadow
                sf::CircleShape sh(6.f, 12);
                sh.setOrigin({ 6.f, 6.f }); sh.setScale({ 1.f, 0.45f });
                sh.setFillColor(sf::Color(0, 0, 0, 80));
                sh.setPosition({ pos.x, pos.y + 8.f });
                window.draw(sh);
            }
            bool flash = false;
            if (st == TState::Dropped) flash = t.timer > gc::DROP_RETURN_TIME - gc::DROP_LOCK && std::fmod(time, 0.16f) < 0.08f;
            tr.setScale({ 1.8f, 1.8f });
            tr.setColor(flash ? sf::Color(255, 255, 255, 120) : sf::Color::White);
            tr.setPosition({ pos.x, pos.y + bob });
            window.draw(tr);
            // owner ring (home) / countdown arc (dropped: remaining time / DROP_RETURN_TIME)
            if (st == TState::Dropped) {
                trophy::drawArc(window, { pos.x, pos.y + bob + 1.f }, 10.f, 12.f, 1.f, sf::Color(oc.r, oc.g, oc.b, 60));
                trophy::drawArc(window, { pos.x, pos.y + bob + 1.f }, 10.f, 12.f, t.timer / gc::DROP_RETURN_TIME,
                                flash ? sf::Color(255, 255, 255, 255) : sf::Color(oc.r, oc.g, oc.b, 255));
            } else if (st == TState::AtHome) {
                sf::CircleShape ring(9.f, 18);
                ring.setFillColor(sf::Color::Transparent);
                ring.setOutlineColor(sf::Color(oc.r, oc.g, oc.b, 130));
                ring.setOutlineThickness(1.2f);
                ring.setOrigin({ 9.f, 9.f });
                ring.setPosition({ pos.x, pos.y + bob + 5.f });
                window.draw(ring);
            }
        }

        // projectiles
        sf::Sprite pj(assets.projectile);
        pj.setOrigin({ 8.f, 8.f });
        for (int i = 0; i < s.projectileCount; ++i) {
            auto& pr = s.projectiles[i];
            if (s.fog && !fogOfWar.visible(pr.x, pr.y)) continue;
            int frame = (int)(time * 14.f) & 3;
            pj.setTextureRect(cell(frame, 0));
            sf::Color c = (pr.ownerId < 4) ? TEAM_COLOR[pr.ownerId] : sf::Color::White;
            pj.setColor(c);
            float ang = 0.f;
            if (pr.dir == gc::FACE_DOWN) ang = 90.f;
            else if (pr.dir == gc::FACE_UP) ang = 270.f;
            else if (pr.dir == gc::FACE_LEFT) ang = 180.f;
            pj.setRotation(sf::degrees(ang));
            pj.setPosition({ pr.x, pr.y });
            window.draw(pj);
        }

        // players (y-sorted for nicer overlap), local player last
        int order[gc::NUM_PLAYERS] = {0,1,2,3};
        std::sort(order, order + gc::NUM_PLAYERS, [&](int a, int b){
            return disp[a].y < disp[b].y;
        });
        // draw local last by pushing it to the end while keeping others y-sorted
        int localSlot = -1;
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) if (order[i] == myId) localSlot = i;
        if (localSlot >= 0) {
            int li = order[localSlot];
            for (int i = localSlot; i < gc::NUM_PLAYERS - 1; ++i) order[i] = order[i + 1];
            order[gc::NUM_PLAYERS - 1] = li;
        }

        sf::Sprite pspr(assets.chars[0]);
        sf::CircleShape shadow(7.f, 12);
        shadow.setFillColor(sf::Color(0, 0, 0, 90));
        shadow.setOrigin({ 7.f, 7.f });
        shadow.setScale({ 1.f, 0.45f });
        for (int oi = 0; oi < gc::NUM_PLAYERS; ++oi) {
            int i = order[oi];
            auto& p = s.players[i];
            if (!p.active) continue;
            float px = disp[i].x, py = disp[i].y;
            drawPropsUpTo(py + 16.f);   // everything standing behind this player
            if (s.fog && i != myId && !p.carrying && !fogOfWar.visible(px + 8.f, py + 8.f)) continue;   // hidden in the fog (carriers are always revealed)
            if (i != myId && !p.carrying && myId < gc::NUM_PLAYERS && gcmap::secretAt((int)((px + 8.f) / gc::TILE), (int)((py + 8.f) / gc::TILE)) &&
                std::hypot(px - disp[myId].x, py - disp[myId].y) > 3.f * gc::TILE) continue;   // enemy inside a secret hedge corridor
            const float lift = trophy::leapLift(p.leapPhase);   // leap arc: sprite offset (negative = up)
            const float pyd = py + lift;
            // shadow
            shadow.setScale({ trophy::leapShadow(p.leapPhase), 0.45f * trophy::leapShadow(p.leapPhase) });
            shadow.setPosition({ px + 8.f, py + 13.f });
            window.draw(shadow);
            // local highlight ring
            if (i == myId) {
                sf::CircleShape ring(12.f, 24);
                ring.setFillColor(sf::Color::Transparent);
                ring.setOutlineColor(sf::Color(255, 255, 255, 120 + (int)(std::sin(time * 6.f) * 40 + 40)));
                ring.setOutlineThickness(1.2f);
                ring.setOrigin({ 12.f, 12.f });
                ring.setPosition({ px + 8.f, pyd + 9.f });
                window.draw(ring);
            }
            // carried treasure above head
            if (p.carrying && p.carriedTreasure < gc::NUM_PLAYERS) {
                sf::Sprite ct(assets.treasure);
                ct.setScale({ 1.4f, 1.4f });
                ct.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
                sf::Color src = TEAM_COLOR[s.treasures[p.carriedTreasure].baseOwner];
                ct.setColor(src);
                const sf::Vector2f cp{ px + 8.f, pyd - 4.f + std::sin(time * 6.f) * 1.f };
                sf::CircleShape glow(9.f + std::sin(time * 8.f) * 1.5f, 20);   // soft glow behind the carried cup
                glow.setOrigin({ glow.getRadius(), glow.getRadius() });
                glow.setFillColor(sf::Color(src.r, src.g, src.b, 70));
                glow.setPosition(cp);
                window.draw(glow);
                ct.setPosition(cp);
                window.draw(ct);
            }
            // body
            pspr.setTexture(assets.chars[i]);
            pspr.setTextureRect(cell(p.facing, p.moving ? p.walkFrame : 0));
            pspr.setColor(sf::Color::White);
            pspr.setPosition({ px, pyd });
            window.draw(pspr);
            
            // health bar above player (like example gif)
            sf::RectangleShape hpBg(sf::Vector2f{18.f, 4.f});
            hpBg.setPosition({px - 1.f, pyd - 6.f});
            hpBg.setFillColor(sf::Color(40, 40, 40, 180));
            window.draw(hpBg);
            // Use ammo as "health" proxy (green bar fills based on ammo)
            float hpPercent = static_cast<float>(p.ammo) / gc::MAX_AMMO;
            sf::RectangleShape hpBar(sf::Vector2f{16.f * hpPercent, 2.f});
            hpBar.setPosition({px, pyd - 5.f});
            hpBar.setFillColor(sf::Color(90, 220, 100));
            window.draw(hpBar);
            
            // name
            if (assets.fontOk) {
                sf::Text name(assets.font);
                cfgText(name, assets.font, p.name.empty() ? std::string("P") + std::to_string(i) : p.name,
                        7, sf::Color(255, 255, 255), 1.f, sf::Color::Black);
                centerOrigin(name);
                name.setPosition({ px + 8.f, pyd + 20.f });
                window.draw(name);
            }
        }

        drawPropsUpTo(1e9f);            // remaining props (in front of all players)

        // secret hedge bushes (in front of the players, before the fog) + entrance shimmer + discovery sparkles
        {
            const sf::Vector2f me2 = myId < gc::NUM_PLAYERS ? disp[myId] + sf::Vector2f(8.f, 8.f) : sf::Vector2f(-1e5f, -1e5f);
            for (int i = 0; i < world::NUM_SECRET_OBJS; ++i) {
                const auto& o = world::SECRET_OBJS[i];
                if (o.tx + o.w < tx0 || o.tx > tx1 || o.ty + o.h < ty0 || o.ty > ty1) continue;
                const float d = std::hypot(o.tx * gc::TILE + o.w * 8.f - me2.x, o.ty * gc::TILE + o.h * 8.f - me2.y);
                const float k = std::clamp((d - 1.5f * gc::TILE) / 16.f, 0.f, 1.f);   // 150 within 1.5 tiles, fading back to 255
                sf::Sprite sp(assets.sheets[o.sheet]);
                sp.setTextureRect({ { o.c * gc::TILE, o.r * gc::TILE }, { o.w * gc::TILE, o.h * gc::TILE } });
                sp.setPosition({ float(o.tx * gc::TILE), float(o.ty * gc::TILE) });
                sp.setColor(sf::Color(255, 255, 255, (uint8_t)(150.f + 105.f * k)));
                window.draw(sp);
            }
            for (int i = 0; i < world::NUM_PASSAGES; ++i) {   // subtle shimmer at undiscovered entrances when near
                const auto& pa = world::PASSAGES[i];
                if (fogOfWar.discovered(pa.id)) continue;
                for (int e = 0; e < 2; ++e) {
                    const sf::Vector2f c{ ((e ? pa.bx : pa.ax) + 0.5f) * gc::TILE, ((e ? pa.by : pa.ay) + 0.5f) * gc::TILE };
                    const float d = std::hypot(c.x - me2.x, c.y - me2.y) / gc::TILE;
                    if (d > 8.f) continue;
                    const float near = 1.f - d / 8.f;
                    for (int k = 0; k < 3; ++k) {
                        const float ph = time * 1.3f + k * 2.1f + pa.id;
                        const float a = (0.5f + 0.5f * std::sin(ph * 2.f)) * near * 150.f;
                        sf::CircleShape sp(1.2f, 6);
                        sp.setOrigin({ 1.2f, 1.2f });
                        sp.setPosition({ c.x + std::sin(ph * 1.7f) * 6.f, c.y - 6.f + std::fmod(ph, 1.f) * -8.f + 6.f });
                        sp.setFillColor(sf::Color(255, 250, 170, (uint8_t)a));
                        window.draw(sp);
                    }
                }
            }
            for (auto& sp : sparks) {
                const float f = 1.f - sp.age / 0.9f;
                sf::CircleShape c(1.6f * f + 0.4f, 6);
                c.setOrigin({ c.getRadius(), c.getRadius() });
                c.setPosition(sp.p);
                c.setFillColor(sf::Color(sp.col.r, sp.col.g, sp.col.b, (uint8_t)(255.f * f)));
                window.draw(c);
            }
        }

        // fog of war overlay (world space, after props+players, before the HUD)
        skyLayer.drawClouds(window, viewRect, time);    // clouds above the world, below fog + HUD
        fogOfWar.setTime(time);
        if (s.fog) fogOfWar.draw(window, viewRect);

        // beacon layer (drawn AFTER the fog so carried/dropped trophies show through): pulsing ring + trophy marker above the carrier
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& t = s.treasures[i];
            const TState st = (TState)t.state;
            if (st != TState::Carried && st != TState::Dropped) continue;
            sf::Vector2f bp{ t.x, t.y };
            if (st == TState::Carried && t.carriedBy < gc::NUM_PLAYERS && s.players[t.carriedBy].active) bp = disp[t.carriedBy] + sf::Vector2f(8.f, 6.f);
            const sf::Color oc = TEAM_COLOR[i];
            for (int k = 0; k < 2; ++k) {   // two staggered expanding rings
                const float ph = std::fmod(time * 0.9f + k * 0.5f, 1.f);
                const float r = 8.f + ph * 22.f;
                sf::CircleShape ring(r, 36);
                ring.setOrigin({ r, r });
                ring.setPosition(bp);
                ring.setFillColor(sf::Color::Transparent);
                ring.setOutlineColor(sf::Color(oc.r, oc.g, oc.b, (uint8_t)(230.f * (1.f - ph))));
                ring.setOutlineThickness(1.6f);
                window.draw(ring);
            }
            if (st == TState::Carried && s.fog && !fogOfWar.visible(bp.x, bp.y)) {   // carrier hidden in the fog: keep the cup readable
                sf::Sprite ct(assets.treasure);
                ct.setScale({ 1.4f, 1.4f });
                ct.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
                ct.setColor(oc);
                ct.setPosition({ bp.x, bp.y - 10.f + std::sin(time * 6.f) });
                window.draw(ct);
            }
        }
        weatherFx.drawWorld(window, viewRect, time);    // rain splashes
        weatherFx.drawScreen(window, viewRect, time);   // rain streaks, tint, lightning, leaves

        // ---- HUD ----
        window.setView(hudView);
        if (assets.fontOk) {
            const sf::Font& F = assets.font;
            const float HW = 1280.f, HH = 960.f;
            auto drawText = [&](const std::string& str, unsigned sz, sf::Color col, sf::Vector2f pos, int align = 0, float outline = 0.f) {
                sf::Text t(F);
                cfgText(t, F, str, sz, col, outline);
                auto b = t.getLocalBounds();
                float ox = align == 0 ? b.position.x : (align == 1 ? b.position.x + b.size.x * 0.5f : b.position.x + b.size.x);
                t.setOrigin({ ox, b.position.y + (align == 1 ? b.size.y * 0.5f : 0.f) });
                t.setPosition(pos);
                window.draw(t);
            };
            const float uiTime = uiClock.getElapsedTime().asSeconds();

            // player cards
            const float cardW = 300.f, cardH = 76.f, gap = 12.f;
            const float cx0 = (HW - (4 * cardW + 3 * gap)) * 0.5f;
            for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                auto& p = s.players[i];
                const float x = cx0 + i * (cardW + gap), y = 10.f;
                const sf::Color c = TEAM_COLOR[i];
                const bool me = (i == myId);
                const bool here = p.active;
                ui::drawNinePatch(window, uiTheme.panel, { {x, y}, {cardW, cardH} }, 5.f, 3.f,
                                  here ? sf::Color::White : sf::Color(150, 150, 150));
                // team accent strip
                sf::RectangleShape strip({ 8.f, cardH - 26.f });
                strip.setPosition({ x + 12.f, y + 13.f });
                strip.setFillColor(here ? c : sf::Color(c.r / 3, c.g / 3, c.b / 3));
                window.draw(strip);
                if (me) {
                    sf::RectangleShape hl({ cardW - 4.f, cardH - 4.f });
                    hl.setPosition({ x + 2.f, y + 2.f });
                    hl.setFillColor(sf::Color::Transparent);
                    hl.setOutlineColor(sf::Color(c.r, c.g, c.b, 230));
                    hl.setOutlineThickness(2.f);
                    window.draw(hl);
                }
                std::string label = p.name.empty() ? (here ? "P" + std::to_string(i) : std::string("Waiting...")) : p.name;
                float tx = x + 28.f;
                drawText(label, 18, here ? sf::Color::White : sf::Color(170, 170, 170), { tx, y + 11.f }, 0, 1.f);
                if (me) drawText("YOU", 12, c, { x + cardW - 16.f, y + 13.f }, 2, 1.f);
                else if (p.isBot) drawText("BOT", 12, sf::Color(190, 200, 215), { x + cardW - 16.f, y + 13.f }, 2, 1.f);
                if (here && !p.isBot) menuUi.drawPing(window, x + cardW - 94.f, y + 25.f, p.ping, p.connected, 12);

                // trophy status chip under the card: home / "Bob carries" / "DROPPED 9s" / returning
                if (here) {
                    const auto& tt = s.treasures[i];
                    std::string chip; sf::Color cc(190, 200, 190);
                    switch ((TState)tt.state) {
                        case TState::AtHome:    chip = "home"; cc = sf::Color(150, 220, 160); break;
                        case TState::Carried:   chip = (tt.carriedBy < gc::NUM_PLAYERS ? playerName(s, tt.carriedBy) : std::string("?")) + " carries";
                                                cc = sf::Color(255, 120, 100); break;
                        case TState::Dropped:   chip = "DROPPED " + std::to_string((int)std::ceil(std::max(0.f, tt.timer))) + "s";
                                                cc = std::fmod(uiTime, 0.8f) < 0.5f ? sf::Color(255, 220, 90) : sf::Color(255, 150, 70); break;
                        case TState::Returning: chip = "returning"; cc = sf::Color(170, 210, 255); break;
                        default: break;
                    }
                    if (!chip.empty()) {
                        const float cw = ui::textWidth(F, 12, chip) + 16.f;
                        ui::drawNinePatch(window, uiTheme.panel3, { {x + 6.f, y + cardH + 1.f}, {cw, 20.f} }, 5.f, 2.f, sf::Color(255, 255, 255, 230));
                        drawText(chip, 12, cc, { x + 14.f, y + cardH + 5.f }, 0, 1.f);
                    }
                }
                // treasure pips
                const float icon = 22.f;
                for (int k = 0; k < (int)s.winScore; ++k) {
                    sf::Sprite sp(assets.treasure);
                    auto ts = assets.treasure.getSize();
                    float sc = icon / (float)std::max(ts.x, ts.y);
                    sp.setScale({ sc, sc });
                    sp.setPosition({ tx + k * (icon + 4.f), y + 38.f });
                    sp.setColor(k < p.score ? sf::Color::White : sf::Color(0, 0, 0, 110));
                    window.draw(sp);
                }
                // ammo pips (right aligned)
                const float pw = 9.f, pg = 3.f;
                float ax = x + cardW - 16.f - (gc::MAX_AMMO * (pw + pg) - pg);
                for (int a = 0; a < gc::MAX_AMMO; ++a) {
                    sf::RectangleShape pip({ pw, 12.f });
                    pip.setPosition({ ax + a * (pw + pg), y + 44.f });
                    pip.setFillColor(a < p.ammo ? sf::Color(255, 205, 70) : sf::Color(30, 20, 10, 150));
                    pip.setOutlineColor(sf::Color(40, 25, 10, 200));
                    pip.setOutlineThickness(1.f);
                    window.draw(pip);
                }
            }

            // match timer
            int secs = std::max(0, (int)std::ceil(s.matchTime));
            char tb[16]; std::snprintf(tb, sizeof(tb), "%d:%02d", secs / 60, secs % 60);
            ui::drawNinePatch(window, uiTheme.panel2, { {HW * 0.5f - 60.f, 114.f}, {120.f, 46.f} }, 5.f, 3.f);
            drawText(tb, 28, secs <= 10 ? sf::Color(255, 120, 100) : sf::Color::White, { HW * 0.5f, 136.f }, 1, 2.f);
            drawText("Steal treasures from enemy bases, bring them home. First to " + std::to_string((int)s.winScore) + " wins.",
                     14, sf::Color(255, 240, 180), { HW * 0.5f, 178.f }, 1, 1.5f);
            if (s.gameOver) {   // victory screen: dim the world, confetti, winner panel + standings
                const bool iWon = (s.winner != 255 && s.winner == myId);
                sf::RectangleShape dim({ HW, HH });
                dim.setFillColor(sf::Color(10, 8, 20, 120));
                window.draw(dim);
                const sf::Color conf[6] = { {255, 215, 90}, {232, 80, 80}, {80, 150, 240}, {90, 205, 110}, {255, 150, 220}, {255, 255, 255} };
                const int pieces = (s.winner == 255) ? 0 : (iWon ? 140 : 60);
                for (int i = 0; i < pieces; ++i) {   // deterministic falling confetti, no state needed
                    float fx = std::fmod(i * 0.6180339f, 1.f) * HW + std::sin(uiTime * 1.7f + i) * 18.f;
                    float fy = std::fmod(uiTime * (70.f + (i % 7) * 18.f) + i * 53.f, HH + 40.f) - 20.f;
                    sf::RectangleShape piece({ 6.f + (i % 3) * 2.f, 10.f });
                    piece.setOrigin({ 3.f, 5.f });
                    piece.setRotation(sf::degrees(uiTime * (90.f + i % 5 * 40.f) + i * 20.f));
                    piece.setPosition({ fx, fy });
                    piece.setFillColor(conf[i % 6]);
                    window.draw(piece);
                }
                const float pw = 760.f, ph = 340.f, px = HW * 0.5f - pw * 0.5f, py = 330.f;
                ui::drawNinePatch(window, uiTheme.panel, { {px, py}, {pw, ph} }, 5.f, 3.f);
                sf::RectangleShape inner({ pw - 24.f, ph - 24.f });
                inner.setPosition({ px + 12.f, py + 12.f });
                inner.setFillColor(sf::Color(28, 18, 12, 225));
                window.draw(inner);
                // bobbing trophy beside the title
                sf::Sprite cup(assets.treasure);
                cup.setScale({ 4.f, 4.f });
                cup.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
                for (float side : { -1.f, 1.f }) {
                    cup.setPosition({ HW * 0.5f + side * 300.f, py + 64.f + std::sin(uiTime * 3.f + side) * 5.f });
                    window.draw(cup);
                }
                std::string wname = s.winner == 255 ? "" : playerName(s, s.winner);
                if (s.winner == 255) {
                    drawText("IT'S A DRAW!", 46, sf::Color(255, 230, 120), { HW * 0.5f, py + 62.f }, 1, 3.f);
                    drawText("Nobody stole enough treasure this time.", 18, sf::Color(235, 230, 220), { HW * 0.5f, py + 112.f }, 1, 1.5f);
                } else if (iWon) {
                    drawText("CONGRATULATIONS!", 46, sf::Color(255, 230, 120), { HW * 0.5f, py + 62.f }, 1, 3.f);
                    drawText(wname + ", you stole the most treasure - you win!", 19, sf::Color(255, 245, 190), { HW * 0.5f, py + 112.f }, 1, 1.5f);
                } else {
                    drawText(wname + " WINS!", 46, sf::Color(255, 230, 120), { HW * 0.5f, py + 62.f }, 1, 3.f);
                    drawText("Congratulations to " + wname + "! Better luck next round.", 18, sf::Color(235, 230, 220), { HW * 0.5f, py + 112.f }, 1, 1.5f);
                }
                // final standings, best first
                int rank[gc::NUM_PLAYERS], n = 0;
                for (int i = 0; i < gc::NUM_PLAYERS; ++i) if (s.players[i].active) rank[n++] = i;
                std::sort(rank, rank + n, [&](int a, int b) { return s.players[a].score > s.players[b].score; });
                float ry = py + 160.f;
                for (int k = 0; k < n; ++k) {
                    int i = rank[k];
                    sf::Color tc = TEAM_COLOR[i];
                    sf::RectangleShape bar({ 8.f, 24.f });
                    bar.setPosition({ px + 120.f, ry - 12.f });
                    bar.setFillColor(tc);
                    window.draw(bar);
                    std::string label = std::to_string(k + 1) + ".  " + playerName(s, (uint8_t)i) + (i == myId ? "  (you)" : "");
                    drawText(label, 20, (int)s.winner == i ? sf::Color(255, 230, 120) : sf::Color(235, 230, 220), { px + 142.f, ry }, 0, 1.f);
                    drawText(std::to_string(s.players[i].score) + " / " + std::to_string((int)s.winScore) + " treasures", 18,
                             sf::Color(210, 195, 165), { px + pw - 120.f, ry }, 2, 1.f);
                    ry += 36.f;
                }
            }

            // toasts
            float ty = 270.f;
            for (auto& t : toasts) {
                float f = std::min(1.f, (3.5f - t.age) / 0.6f);
                uint8_t a = (uint8_t)(255.f * std::max(0.f, f));
                float w = ui::textWidth(F, 17, t.text) + 36.f;
                ui::drawNinePatch(window, uiTheme.panel3, { {HW * 0.5f - w * 0.5f, ty - 16.f}, {w, 32.f} }, 5.f, 2.f, sf::Color(255, 255, 255, a));
                drawText(t.text, 17, sf::Color(t.color.r, t.color.g, t.color.b, a), { HW * 0.5f, ty }, 1, 1.5f);
                ty += 38.f;
            }

            // chat panel (top-left, under the player cards)
            const sf::Font& CF = assets.chatFontOk ? assets.chatFont : F;
            const bool realFont = assets.chatFontOk;
            auto chatW = [&](const std::string& str, unsigned sz) {
                sf::Text t(CF, realFont ? str : ui::spaced(str, sz), sz);
                return t.getLocalBounds().size.x + t.getLocalBounds().position.x;
            };
            // chat text: black drop shadow + outline so it stays readable over any terrain
            auto drawChat = [&](const std::string& str, unsigned sz, sf::Color col, sf::Vector2f pos) {
                sf::Text t(CF, realFont ? str : ui::spaced(str, sz), sz);
                t.setPosition(pos + sf::Vector2f(2.f, 2.f));
                t.setFillColor(sf::Color(0, 0, 0, (uint8_t)(col.a * 0.8f)));
                window.draw(t);
                t.setPosition(pos);
                t.setFillColor(col);
                t.setOutlineColor(sf::Color(0, 0, 0, col.a));
                t.setOutlineThickness(2.f);
                window.draw(t);
            };
            const float cpW = 372.f, cpH = 300.f, cpX = 12.f, cpY = 118.f;
            const float pad = 18.f, inH = 38.f, lineH = 24.f, fs = 18.f;
            if (chatActive) {   // the chat window only exists while typing; otherwise messages just float and fade
                ui::drawNinePatch(window, uiTheme.panel, { {cpX, cpY}, {cpW, cpH} }, 5.f, 3.f);
                sf::RectangleShape inner({ cpW - 20.f, cpH - 20.f });
                inner.setPosition({ cpX + 10.f, cpY + 10.f });
                inner.setFillColor(sf::Color(28, 18, 12, 215));
                window.draw(inner);
                drawText("CHAT", 12, sf::Color(255, 220, 140), { cpX + pad, cpY + 9.f }, 0, 1.f);
            }
            const float textW = cpW - pad * 2.f;
            const float logTop = chatActive ? cpY + 28.f : cpY, logBot = chatActive ? cpY + cpH - pad - inH - 8.f : cpY + 6 * lineH;
            // build wrapped visual lines (newest last)
            struct VL { std::string name, text; sf::Color nameCol; sf::Color col; float alpha; bool first; float nameW; };
            std::vector<VL> vls;
            for (auto& e : chatLines) {
                float alpha = 1.f;
                if (!chatActive) alpha = std::clamp((9.f - e.age) / 3.f, 0.f, 1.f);
                if (alpha <= 0.f) continue;
                sf::Color nc = (e.colorIndex >= 0 && e.colorIndex < 4) ? TEAM_COLOR[e.colorIndex] : sf::Color(200, 200, 200);
                sf::Color tc(235, 230, 220);
                if (e.colorIndex == -2) nc = tc = sf::Color(214, 150, 255);        // whispers: violet
                else if (e.colorIndex == -3) nc = tc = sf::Color(255, 225, 130);   // server lines: yellow
                std::string nm = e.name + ":";
                float nw = chatW(nm + " ", (unsigned)fs);
                auto wrapped = ui::wrapText(CF, (unsigned)fs, e.text, textW, nw, !realFont);
                for (size_t k = 0; k < wrapped.size(); ++k)
                    vls.push_back({ k == 0 ? nm : "", wrapped[k], nc, tc, alpha, k == 0, nw });
            }
            int maxLines = (int)((logBot - logTop) / lineH);
            int start = std::max(0, (int)vls.size() - maxLines);
            float ly = chatActive ? logBot - (float)((int)vls.size() - start) * lineH : logTop;   // closed: top-aligned, newest at the bottom
            for (int k = start; k < (int)vls.size(); ++k, ly += lineH) {
                auto& v = vls[k];
                uint8_t a = (uint8_t)(255.f * v.alpha);
                float ox = cpX + pad;
                if (!chatActive) ox = cpX + 6.f;
                if (v.first)
                    drawChat(v.name, (unsigned)fs, sf::Color(v.nameCol.r, v.nameCol.g, v.nameCol.b, a), { ox, ly });
                drawChat(v.text, (unsigned)fs, sf::Color(v.col.r, v.col.g, v.col.b, a), { ox + (v.first ? v.nameW : 0.f), ly });
            }
            if (vls.empty() && chatActive)
                drawChat("No messages yet", 16, sf::Color(190, 170, 140, 200), { cpX + pad, logBot - lineH });

            if (chatActive) {
            // input bar
            sf::FloatRect ib({ cpX + pad - 4.f, cpY + cpH - pad - inH + 4.f }, { cpW - pad * 2.f + 8.f, inH });
            ui::drawNinePatch(window, chatActive ? uiTheme.focus : uiTheme.interior, ib, 5.f, 2.f,
                              sf::Color(255, 255, 255, chatActive ? 255 : 170));
            const float iy = ib.position.y + ib.size.y * 0.5f - 8.f;
            if (chatActive) {
                std::string shown = chatInput;
                while (shown.size() > 1 && chatW(shown, 18) > ib.size.x - 100.f) shown.erase(0, 1);
                drawChat(shown, 18, sf::Color(255, 245, 160), { ib.position.x + 12.f, iy - 3.f });
                if (std::fmod(uiTime, 1.0f) < 0.55f) {
                    float cw = shown.empty() ? 0.f : chatW(shown, 18);
                    sf::RectangleShape cur({ 2.f, 20.f });
                    cur.setPosition({ ib.position.x + 13.f + cw, iy + 1.f });
                    cur.setFillColor(sf::Color(255, 245, 160));
                    window.draw(cur);
                }
                bool nearLimit = (int)chatInput.size() > gc::CHAT_LEN - 20;
                drawText(std::to_string(chatInput.size()) + "/" + std::to_string(gc::CHAT_LEN), 12,
                         nearLimit ? sf::Color(255, 130, 110) : sf::Color(190, 170, 140), { ib.position.x + ib.size.x - 12.f, iy + 2.f }, 2);
            }

            }

            // minimap (bottom-right): explored area only, base markers, trophies, discovered passages, local player
            if (myId < gc::NUM_PLAYERS) {
                std::vector<fog::MapMarker> mk;
                for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                    const auto& t = s.treasures[i];
                    const TState st = (TState)t.state;
                    if (st == TState::Inactive) continue;
                    sf::Vector2f mp{ t.x, t.y };
                    if (st == TState::Carried && t.carriedBy < gc::NUM_PLAYERS) mp = disp[t.carriedBy] + sf::Vector2f(8.f, 8.f);
                    mk.push_back({ mp.x, mp.y, st == TState::Carried ? 1 : (st == TState::Dropped ? 2 : 0), TEAM_COLOR[i] });
                }
                fogOfWar.drawMinimap(window, { HW - 12.f - 3.f - gc::MAP_W * 3.f, HH - 54.f - gc::MAP_H * 3.f }, 3.f, TEAM_COLOR,
                                     disp[myId] + sf::Vector2f(8.f, 8.f), TEAM_COLOR[myId], viewRect, uiTime, &mk);

                // screen-edge arrows toward carried/dropped trophies (and my base when I carry one)
                const sf::Vector2f mc = disp[myId] + sf::Vector2f(8.f, 8.f);
                const float Z = HW / viewRect.size.x;   // world -> HUD scale (3x)
                auto edgeMark = [&](sf::Vector2f wp, sf::Color col, int owner, const std::string& label, bool pulse) {
                    const sf::Vector2f hp{ (wp.x - viewRect.position.x) * Z, (wp.y - viewRect.position.y) * Z };
                    const auto ea = trophy::edgeArrow(hp.x, hp.y, HW * 0.5f, HH * 0.5f, HW * 0.5f - 60.f, HH * 0.5f - 110.f);
                    if (ea.onscreen) return;
                    const float pa = pulse ? 0.65f + 0.35f * std::sin(uiTime * 8.f) : 1.f;
                    sf::ConvexShape ar(3);
                    ar.setPoint(0, { 16.f, 0.f }); ar.setPoint(1, { -10.f, -12.f }); ar.setPoint(2, { -10.f, 12.f });
                    ar.setPosition({ ea.x, ea.y });
                    ar.setRotation(sf::radians(ea.angle));
                    ar.setScale({ pa, pa });
                    ar.setFillColor(sf::Color(col.r, col.g, col.b, 235));
                    ar.setOutlineColor(sf::Color(0, 0, 0, 200));
                    ar.setOutlineThickness(2.f);
                    window.draw(ar);
                    // mini cup + distance, placed inward from the arrow
                    const sf::Vector2f in2{ ea.x - std::cos(ea.angle) * 34.f, ea.y - std::sin(ea.angle) * 34.f };
                    if (owner >= 0) {
                        sf::Sprite cup(assets.treasure);
                        const auto ts = assets.treasure.getSize();
                        const float sc = 20.f / (float)std::max(ts.x, ts.y);
                        cup.setScale({ sc, sc });
                        cup.setOrigin({ ts.x * 0.5f, ts.y * 0.5f });
                        cup.setPosition({ in2.x, in2.y - 8.f });
                        cup.setColor(TEAM_COLOR[owner]);
                        window.draw(cup);
                    }
                    const int tiles = (int)std::lround(std::hypot(wp.x - mc.x, wp.y - mc.y) / gc::TILE);
                    drawText(std::to_string(tiles) + "t", 13, sf::Color(255, 255, 255), { in2.x, in2.y + 8.f }, 1, 1.5f);
                    if (!label.empty()) drawText(label, 12, col, { in2.x, in2.y + 24.f }, 1, 1.5f);
                };
                const bool iCarry = s.players[myId].carrying;
                for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                    const auto& t = s.treasures[i];
                    const TState st = (TState)t.state;
                    if (st != TState::Carried && st != TState::Dropped) continue;
                    if (st == TState::Carried && t.carriedBy == myId) continue;   // I carry it myself
                    sf::Vector2f wp{ t.x, t.y };
                    if (st == TState::Carried && t.carriedBy < gc::NUM_PLAYERS) wp = disp[t.carriedBy] + sf::Vector2f(8.f, 8.f);
                    const bool mine = (i == myId);
                    if (mine && st == TState::Carried) edgeMark(wp, sf::Color(255, 70, 60), i, "YOUR TREASURE", true);
                    else edgeMark(wp, TEAM_COLOR[i], i, st == TState::Dropped ? (mine ? "RECOVER" : "DROPPED") : "", st == TState::Dropped);
                }
                if (iCarry) {
                    const auto& b = gcmap::bases()[myId];
                    edgeMark({ b.treasureX, b.treasureY }, sf::Color(110, 240, 130), -1, "DELIVER", false);
                }

                // leap cooldown chip (bottom-left)
                {
                    const auto& mp = s.players[myId];
                    const float cdMax = (mp.carrying ? gc::LEAP_CD_CARRIER : gc::LEAP_CD) * 10.f;
                    const bool ready = mp.leapCd == 0 && mp.leapPhase == 0;
                    const float bx = 16.f, by = HH - 52.f, bw = 150.f;
                    ui::drawNinePatch(window, uiTheme.panel3, { {bx, by}, {bw, 36.f} }, 5.f, 2.f);
                    sf::RectangleShape fill({ (bw - 16.f) * (ready ? 1.f : 1.f - std::clamp(mp.leapCd / cdMax, 0.f, 1.f)), 5.f });
                    fill.setPosition({ bx + 8.f, by + 25.f });
                    fill.setFillColor(ready ? sf::Color(120, 235, 140) : sf::Color(240, 190, 90));
                    window.draw(fill);
                    char lb[32];
                    if (ready) std::snprintf(lb, sizeof(lb), "LEAP ready");
                    else if (mp.leapPhase) std::snprintf(lb, sizeof(lb), "LEAP...");
                    else std::snprintf(lb, sizeof(lb), "LEAP %.1fs", mp.leapCd / 10.f);
                    drawText(lb, 14, ready ? sf::Color(170, 255, 180) : sf::Color(255, 220, 140), { bx + bw * 0.5f, by + 12.f }, 1, 1.f);
                }
            }

            // controls hint (bottom, right of chat)
            drawText("WASD move  |  Space shoot  |  Shift/J leap  |  T / Enter chat  |  Esc close", 14,
                     sf::Color(230, 225, 210), { HW - 16.f, HH - 28.f }, 2, 1.5f);
        }

        if (reconnecting) menuUi.drawReconnect(window, reconnAttempt, reconnLeft);
        if (!net.connected && !reconnecting) {
            window.setView(hudView);
            sf::RectangleShape bg(sf::Vector2f{ 1280.f, 960.f });
            bg.setFillColor(sf::Color(0, 0, 0, 170));
            window.draw(bg);
            if (assets.fontOk) {
                ui::drawNinePatch(window, uiTheme.panel, { {640.f - 240.f, 480.f - 50.f}, {480.f, 100.f} }, 5.f, 3.f);
                sf::Text m(assets.font);
                cfgText(m, assets.font, "Disconnected from server.", 26, sf::Color(255, 140, 120), 1.5f);
                centerOrigin(m); m.setPosition({ 640.f, 480.f });
                window.draw(m);
            }
        }

        if (!shotDone && std::getenv("GAME_SHOT") && uiClock.getElapsedTime().asSeconds() > (std::getenv("GAME_SHOT_DELAY") ? (float)std::atof(std::getenv("GAME_SHOT_DELAY")) : 3.f)) {
            sf::Texture shot(window.getSize());
            shot.update(window);
            (void)shot.copyToImage().saveToFile(std::getenv("GAME_SHOT"));
            shotDone = true;
            if (std::getenv("GAME_SHOT_EXIT")) window.close();
        }
        if (leavePrompt) menuUi.drawLeavePrompt(window);
        window.display();
    }
    return 0;
}
