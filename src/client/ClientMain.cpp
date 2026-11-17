// Client for the steal-the-treasure multiplayer game.
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
#include "Sound.hpp"
#include "Fog.hpp"
#include "Sky.hpp"
#include "Weather.hpp"

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

// ----- Network core --------------------------------------------------------
struct Net {
    sf::TcpSocket socket;
    std::mutex mtx;
    Snapshot snap;
    bool hasSnap = false;
    uint8_t myId = 255;
    std::string roomCode;
    LobbyInfo lobby;
    std::string errText;          // set when the server rejects us (room full / not found / ...)
    bool matchStarted = false;
    std::atomic<bool> running{ false };
    std::atomic<bool> connected{ false };
    std::thread thr;
    std::vector<GameEvent> evQueue;
    std::vector<ChatLine> chatQueue;

    // Connects and sends the Hello (create a room, or join `code`). Welcome/Error arrive asynchronously.
    bool connect(const std::string& host, unsigned short port, const std::string& name,
                 JoinMode mode, const std::string& code) {
        disconnect();
        auto addrOpt = sf::IpAddress::resolve(host);
        if (!addrOpt) return false;
        socket.setBlocking(true);
        auto st = socket.connect(*addrOpt, port, sf::seconds(3));
        if (st != sf::Socket::Status::Done) return false;
        socket.setBlocking(false);
        connected = true;
        running = true;
        {
            std::lock_guard<std::mutex> lk(mtx);
            sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Join) << name << static_cast<uint8_t>(mode) << code;
            socket.send(p);
        }
        thr = std::thread([this] { recvLoop(); });
        return true;
    }

    // Closes the connection and forgets all room/match state.
    void disconnect() {
        running = false;
        if (thr.joinable()) thr.join();
        std::lock_guard<std::mutex> lk(mtx);
        socket.disconnect();
        connected = false;
        snap = Snapshot{}; hasSnap = false; myId = 255; roomCode.clear(); lobby = LobbyInfo{};
        errText.clear(); matchStarted = false; evQueue.clear(); chatQueue.clear();
    }

    void sendInput(const ClientInput& in) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p;
        p << static_cast<uint32_t>(Msg::C2S_Input) << in.dirX << in.dirY << in.facing << in.shoot;
        socket.send(p);
    }

    void sendChat(const std::string& text) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(Msg::C2S_Chat) << text;
        socket.send(p);
    }
    void sendSimple(Msg m) {
        if (!connected) return;
        std::lock_guard<std::mutex> lk(mtx);
        sf::Packet p; p << static_cast<uint32_t>(m);
        socket.send(p);
    }

    Snapshot copySnap() {
        std::lock_guard<std::mutex> lk(mtx);
        return snap;
    }
    uint8_t copyId() { std::lock_guard<std::mutex> lk(mtx); return myId; }
    bool hasSnapshot() { std::lock_guard<std::mutex> lk(mtx); return hasSnap; }
    void roomState(std::string& code, LobbyInfo& li, std::string& err, bool& started) {
        std::lock_guard<std::mutex> lk(mtx);
        code = roomCode; li = lobby; err = errText; started = matchStarted;
    }

    std::vector<GameEvent> drainEvents() {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<GameEvent> out;
        out.swap(evQueue);
        return out;
    }
    std::vector<ChatLine> drainChat() {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<ChatLine> out;
        out.swap(chatQueue);
        return out;
    }

private:
    void recvLoop() {
        while (running) {
            sf::Packet pkt;
            bool got = false, disc = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                auto st = socket.receive(pkt);
                if (st == sf::Socket::Status::Done) { handle(pkt); got = true; }
                else if (st == sf::Socket::Status::Disconnected || st == sf::Socket::Status::Error) { connected = false; disc = true; }
            }
            if (disc) return;
            if (!got) sf::sleep(sf::milliseconds(5));
        }
    }

    void handle(sf::Packet& pkt) {
        uint32_t m; pkt >> m;
        switch (static_cast<Msg>(m)) {
            case Msg::S2C_Welcome: pkt >> myId >> roomCode; break;
            case Msg::S2C_Lobby: pkt >> lobby; break;
            case Msg::S2C_GameStart: matchStarted = true; break;
            case Msg::S2C_Error: { uint8_t c; std::string t; pkt >> c >> t; errText = t.empty() ? "Server error" : t; break; }
            case Msg::S2C_Snapshot: pkt >> snap; hasSnap = true; break;
            case Msg::S2C_Chat: { ChatLine c; pkt >> c; chatQueue.push_back(c); break; }
            case Msg::S2C_Event: { GameEvent e; pkt >> e; evQueue.push_back(e); break; }
            default: break;
        }
    }
public:
    ~Net() { disconnect(); }
};

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
        case EvType::Deliver:  return nm(e.a) + " delivered a treasure!";
        case EvType::Join:     return nm(e.a) + " joined the hunt";
        case EvType::Leave:    return nm(e.a) + " left";
        case EvType::GameOver: return e.a == 255 ? "Draw! Next round soon..." : (nm(e.a) + " wins! Next round soon...");
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

    std::string root = findAssetRoot();
    Assets assets;
    if (!assets.loadAll(root)) {
        std::cerr << "Failed to load assets from '" << root << "'\n";
        return 1;
    }

    Sound::get().init(root);
    Sound::get().startMusic();

    sf::RenderWindow window(sf::VideoMode({1280, 960}), "Steal the Treasure");
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
    bool spaceDown = false;

    sf::Clock clock;
    while (window.isOpen()) {
        float dt = std::min(clock.restart().asSeconds(), 0.05f);
        Sound::get().tick(dt);   // music playlist + chase crossfade (also runs in the menus)

        // ================= menu (landing / join / lobby) =================
        if (appMode == AppMode::Menu) {
            menuClock += dt;
            std::string rc, err; LobbyInfo li; bool started = false;
            net.roomState(rc, li, err, started);
            menu::View nv; nv.connected = net.connected; nv.code = rc; nv.myId = net.copyId(); nv.lobby = li;
            // server-side results
            if (!err.empty()) { net.disconnect(); menuUi.fail(err); }
            else if (menuUi.busy && !rc.empty()) menuUi.enterLobby();
            else if (menuUi.busy && !pendingConnect && waitClock > 4.f) { net.disconnect(); menuUi.fail("Server did not answer"); }
            else if (menuUi.screen == menu::Menu::Screen::Lobby && !net.connected) { net.disconnect(); menuUi.fail("Disconnected from server"); }
            if (menuUi.busy) waitClock += dt; else waitClock = 0.f;
            if (started && menuUi.screen == menu::Menu::Screen::Lobby) {
                appMode = AppMode::Game;
                uiClock.restart(); shotDone = false;
                continue;
            }
            // automation
            if (!autoDone && menuClock > 0.7f && !menuUi.busy && menuUi.screen != menu::Menu::Screen::Lobby && !autoMode.empty()) {
                autoDone = true;
                if (autoMode == "create") menuUi.request(menu::Action::Create);
                else if (autoMode.rfind("join:", 0) == 0) menuUi.request(menu::Action::Join, autoMode.substr(5));
            }
            std::vector<ChatLine> newChat;
            if (menuUi.screen == menu::Menu::Screen::Lobby) newChat = net.drainChat();
            else net.drainChat();
            if (menuUi.screen == menu::Menu::Screen::Lobby) {
                if (!autoChatDone && std::getenv("GAME_AUTO_CHAT") && menuUi.screenTime > 1.f) {
                    autoChatDone = true; net.sendChat(std::getenv("GAME_AUTO_CHAT"));
                }
                int n = 0; for (auto& sl : li.slots) if (sl.active) ++n;
                if (autoStart > 0 && n >= autoStart && nv.myId == li.hostId && menuUi.screenTime > 6.f) {
                    autoStart = 0; net.sendSimple(Msg::C2S_Start);
                }
            }
            menu::Action act = menuUi.frame(window, dt, nv, newChat);
            switch (act.kind) {
                case menu::Action::Quit: window.close(); break;
                case menu::Action::Create:
                case menu::Action::Join: pendingAct = act; pendingConnect = true; break;
                case menu::Action::Start: net.sendSimple(Msg::C2S_Start); break;
                case menu::Action::Ready: net.sendSimple(Msg::C2S_Ready); break;
                case menu::Action::Chat: net.sendChat(act.text); break;
                case menu::Action::Leave: net.disconnect(); menuUi.fail("", true); break;
                default: break;
            }
            if (std::getenv("GAME_SHOT") && std::getenv("GAME_SHOT_ON") && !menuShotDone && menuUi.screenTime > 1.5f) {
                std::string on = std::getenv("GAME_SHOT_ON");
                auto sc = menuUi.screen;
                bool match = (on == "landing" && sc == menu::Menu::Screen::Landing) || (on == "join" && sc == menu::Menu::Screen::Join) ||
                             (on == "lobby" && sc == menu::Menu::Screen::Lobby && menuUi.screenTime > 3.5f);
                if (match) {
                    sf::Texture shot(window.getSize()); shot.update(window);
                    (void)shot.copyToImage().saveToFile(std::getenv("GAME_SHOT"));
                    menuShotDone = true;
                    if (std::getenv("GAME_SHOT_EXIT")) window.close();
                }
            }
            window.display();
            if (pendingConnect && menuUi.busy) {   // connect after the "Connecting..." frame was shown
                pendingConnect = false;
                if (!net.connect(pendingAct.host, pendingAct.port, pendingAct.name,
                                 pendingAct.kind == menu::Action::Create ? JoinMode::Create : JoinMode::Join, pendingAct.code))
                    menuUi.fail("Could not connect to " + pendingAct.host + ":" + std::to_string(pendingAct.port));
            } else pendingConnect = false;
            continue;
        }

        // ---- in-game: lost connection -> back to landing; Esc asks to leave the match ----
        {
            bool esc = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Escape);
            bool yk = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Y);
            bool nk = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::N);
            bool en = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Enter);
            bool focusOk = window.hasFocus();
            if (focusOk) {
                if (!leavePrompt) { if (esc && !escWas && !chatActive) leavePrompt = true; }
                else {
                    if ((yk && !yWas) || (en && !enterWas)) { leavePrompt = false; net.disconnect(); menuUi.fail("", true); appMode = AppMode::Menu; escWas = esc; yWas = yk; enterWas = en; continue; }
                    if ((nk && !nWas) || (esc && !escWas)) leavePrompt = false;
                }
            }
            escWas = esc; yWas = yk; nWas = nk; enterWas = en;
            if (!net.connected) {
                leavePrompt = false;
                net.disconnect();
                menuUi.fail("Disconnected from server", true);
                appMode = AppMode::Menu;
                continue;
            }
        }

        // ---- events (chat typing, close) ----
        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            if (auto* k = ev->getIf<sf::Event::KeyPressed>()) {
                if (!chatActive) { // sound hotkeys: M mute, +/- or ] [ volume
                    using K = sf::Keyboard::Key;
                    float dv = 0.f; bool mt = false;
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
        }
        if (std::getenv("GAME_BOT") && net.hasSnapshot()) {   // QA autopilot: BFS to the nearest enemy treasure, shoot now and then
            static float botShoot = 0.f;
            Snapshot bs = net.copySnap(); uint8_t me = net.copyId();
            if (me < gc::NUM_PLAYERS && bs.players[me].active && !bs.gameOver) {
                const auto& pl = bs.players[me];
                float tx = bs.treasures[me].homeX, ty = bs.treasures[me].homeY;
                if (!pl.carrying) {
                    float best = 1e18f;
                    for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                        if (i == me || !bs.treasures[i].present) continue;
                        float d = std::hypot(bs.treasures[i].x - pl.x, bs.treasures[i].y - pl.y);
                        if (d < best) { best = d; tx = bs.treasures[i].x; ty = bs.treasures[i].y; }
                    }
                }
                int sx = (int)((pl.x + 8) / gc::TILE), sy = (int)((pl.y + 8) / gc::TILE);
                int gx = (int)(tx / gc::TILE), gy = (int)(ty / gc::TILE);
                static std::vector<int> prev; prev.assign(gc::MAP_W * gc::MAP_H, -2);
                std::deque<int> q; prev[gy * gc::MAP_W + gx] = -1; q.push_back(gy * gc::MAP_W + gx);
                while (!q.empty() && prev[sy * gc::MAP_W + sx] == -2) {   // search from the goal so prev[] points toward it
                    int c = q.front(); q.pop_front(); int cx = c % gc::MAP_W, cy = c / gc::MAP_W;
                    const int dxs[4] = {1,-1,0,0}, dys[4] = {0,0,1,-1};
                    for (int k = 0; k < 4; ++k) {
                        int nx = cx + dxs[k], ny = cy + dys[k];
                        if (nx < 0 || ny < 0 || nx >= gc::MAP_W || ny >= gc::MAP_H || gcmap::solidTile(nx, ny)) continue;
                        if (prev[ny * gc::MAP_W + nx] != -2) continue;
                        prev[ny * gc::MAP_W + nx] = c; q.push_back(ny * gc::MAP_W + nx);
                    }
                }
                int nxt = prev[sy * gc::MAP_W + sx];
                if (nxt >= 0) {
                    float wx = (nxt % gc::MAP_W) * gc::TILE + 8.f, wy = (nxt / gc::MAP_W) * gc::TILE + 8.f;
                    float ddx = wx - (pl.x + 8), ddy = wy - (pl.y + 8);
                    in.dirX = std::abs(ddx) > 1.5f ? (ddx > 0 ? 1.f : -1.f) : 0.f;
                    in.dirY = std::abs(ddy) > 1.5f ? (ddy > 0 ? 1.f : -1.f) : 0.f;
                    if (in.dirX != 0 && in.dirY != 0) { if (std::abs(ddx) > std::abs(ddy)) in.dirY = 0; else in.dirX = 0; }
                    in.facing = in.dirX < 0 ? gc::FACE_LEFT : in.dirX > 0 ? gc::FACE_RIGHT : in.dirY < 0 ? gc::FACE_UP : gc::FACE_DOWN;
                    lastFacing = in.facing;
                }
                botShoot -= dt;
                if (botShoot <= 0.f) { in.shoot = !std::getenv("GAME_BOT_NOSHOOT"); botShoot = 1.5f; }
            }
        }
        net.sendInput(in);
        { // sound: own footsteps + empty-ammo click
            Snapshot ss = net.hasSnapshot() ? net.copySnap() : Snapshot{};
            uint8_t me = net.copyId();
            bool hasMe = me < gc::NUM_PLAYERS && ss.players[me].active;
            Sound::get().step(dt, hasMe && !chatActive && (in.dirX != 0 || in.dirY != 0));
            Sound::get().setChase(hasMe && ss.players[me].carrying && !ss.gameOver);   // tense music while running with the trophy
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
            chatLines.push_back(ce);
            if (c.fromId != myId) Sound::get().play(SfxId::Chat, 0.8f);
        }
        for (auto& cl : chatLines) cl.age += dt;
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
                    case EvType::Steal:    snd.play(SfxId::Pickup); break;
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
        }

        // ---- render world ----
        window.setView(gameView);
        window.clear(sf::Color(18, 30, 40));
        float time = clock.getElapsedTime().asSeconds();

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

        // treasures at home
        sf::Sprite tr(assets.treasure);
        tr.setScale({ 1.8f, 1.8f });
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& t = s.treasures[i];
            if (!t.present) continue;
            if (!fogOfWar.visible(t.x, t.y)) continue;
            float bob = std::sin(time * 2.5f + i) * 1.2f;
            tr.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
            tr.setPosition({ t.x, t.y + bob });
            tr.setColor(sf::Color::White);
            window.draw(tr);
            // small owner ring
            sf::CircleShape ring(9.f, 18);
            ring.setFillColor(sf::Color::Transparent);
            ring.setOutlineColor(sf::Color(TEAM_COLOR[i].r, TEAM_COLOR[i].g, TEAM_COLOR[i].b, 130));
            ring.setOutlineThickness(1.2f);
            ring.setOrigin({ 9.f, 9.f });
            ring.setPosition({ t.x, t.y + bob + 5.f });
            window.draw(ring);
        }

        // projectiles
        sf::Sprite pj(assets.projectile);
        pj.setOrigin({ 8.f, 8.f });
        for (int i = 0; i < s.projectileCount; ++i) {
            auto& pr = s.projectiles[i];
            if (!fogOfWar.visible(pr.x, pr.y)) continue;
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
            if (i != myId && !fogOfWar.visible(px + 8.f, py + 8.f)) continue;   // hidden in the fog
            // shadow
            shadow.setPosition({ px + 8.f, py + 13.f });
            window.draw(shadow);
            // local highlight ring
            if (i == myId) {
                sf::CircleShape ring(12.f, 24);
                ring.setFillColor(sf::Color::Transparent);
                ring.setOutlineColor(sf::Color(255, 255, 255, 120 + (int)(std::sin(time * 6.f) * 40 + 40)));
                ring.setOutlineThickness(1.2f);
                ring.setOrigin({ 12.f, 12.f });
                ring.setPosition({ px + 8.f, py + 9.f });
                window.draw(ring);
            }
            // carried treasure above head
            if (p.carrying && p.carriedTreasure < gc::NUM_PLAYERS) {
                sf::Sprite ct(assets.treasure);
                ct.setScale({ 1.4f, 1.4f });
                ct.setOrigin({ assets.treasure.getSize().x * 0.5f, assets.treasure.getSize().y * 0.5f });
                sf::Color src = TEAM_COLOR[s.treasures[p.carriedTreasure].baseOwner];
                ct.setColor(src);
                ct.setPosition({ px + 8.f, py - 4.f + std::sin(time * 6.f) * 1.f });
                window.draw(ct);
            }
            // body
            pspr.setTexture(assets.chars[i]);
            pspr.setTextureRect(cell(p.facing, p.moving ? p.walkFrame : 0));
            pspr.setColor(sf::Color::White);
            pspr.setPosition({ px, py });
            window.draw(pspr);
            
            // health bar above player (like example gif)
            sf::RectangleShape hpBg(sf::Vector2f{18.f, 4.f});
            hpBg.setPosition({px - 1.f, py - 6.f});
            hpBg.setFillColor(sf::Color(40, 40, 40, 180));
            window.draw(hpBg);
            // Use ammo as "health" proxy (green bar fills based on ammo)
            float hpPercent = static_cast<float>(p.ammo) / gc::MAX_AMMO;
            sf::RectangleShape hpBar(sf::Vector2f{16.f * hpPercent, 2.f});
            hpBar.setPosition({px, py - 5.f});
            hpBar.setFillColor(sf::Color(90, 220, 100));
            window.draw(hpBar);
            
            // name
            if (assets.fontOk) {
                sf::Text name(assets.font);
                cfgText(name, assets.font, p.name.empty() ? std::string("P") + std::to_string(i) : p.name,
                        7, sf::Color(255, 255, 255), 1.f, sf::Color::Black);
                centerOrigin(name);
                name.setPosition({ px + 8.f, py + 20.f });
                window.draw(name);
            }
        }

        drawPropsUpTo(1e9f);            // remaining props (in front of all players)

        // fog of war overlay (world space, after props+players, before the HUD)
        skyLayer.drawClouds(window, viewRect, time);    // clouds above the world, below fog + HUD
        fogOfWar.setTime(time);
        fogOfWar.draw(window, viewRect);
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

                // treasure pips
                const float icon = 22.f;
                for (int k = 0; k < gc::WIN_SCORE; ++k) {
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
            ui::drawNinePatch(window, uiTheme.panel2, { {HW * 0.5f - 60.f, 96.f}, {120.f, 46.f} }, 5.f, 3.f);
            drawText(tb, 28, secs <= 10 ? sf::Color(255, 120, 100) : sf::Color::White, { HW * 0.5f, 118.f }, 1, 2.f);
            drawText("Steal treasures from enemy bases, bring them home. First to " + std::to_string(gc::WIN_SCORE) + " wins.",
                     14, sf::Color(255, 240, 180), { HW * 0.5f, 160.f }, 1, 1.5f);
            if (s.gameOver) {
                std::string msg = s.winner == 255 ? "DRAW" : (playerName(s, s.winner) + " WINS!");
                drawText(msg, 44, sf::Color(255, 230, 120), { HW * 0.5f, 220.f }, 1, 3.f);
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

            // chat panel (bottom-left)
            const float cpW = 480.f, cpH = 260.f, cpX = 12.f, cpY = HH - cpH - 12.f;
            const float pad = 18.f, inH = 34.f, lineH = 19.f, fs = 14.f;
            if (chatActive) {   // the chat window only exists while typing; otherwise messages just float and fade
                ui::drawNinePatch(window, uiTheme.panel, { {cpX, cpY}, {cpW, cpH} }, 5.f, 3.f);
                sf::RectangleShape inner({ cpW - 20.f, cpH - 20.f });
                inner.setPosition({ cpX + 10.f, cpY + 10.f });
                inner.setFillColor(sf::Color(28, 18, 12, 215));
                window.draw(inner);
                drawText("CHAT", 12, sf::Color(255, 220, 140), { cpX + pad, cpY + 9.f }, 0, 1.f);
            }
            const float textW = cpW - pad * 2.f;
            const float logTop = chatActive ? cpY + 28.f : HH - 12.f - 6 * lineH, logBot = chatActive ? cpY + cpH - pad - inH - 8.f : HH - 14.f;
            // build wrapped visual lines (newest last)
            struct VL { std::string name, text; sf::Color nameCol; sf::Color col; float alpha; bool first; float nameW; };
            std::vector<VL> vls;
            for (auto& e : chatLines) {
                float alpha = 1.f;
                if (!chatActive) alpha = std::clamp((9.f - e.age) / 3.f, 0.f, 1.f);
                if (alpha <= 0.f) continue;
                sf::Color nc = (e.colorIndex >= 0 && e.colorIndex < 4) ? TEAM_COLOR[e.colorIndex] : sf::Color(200, 200, 200);
                std::string nm = e.name + ":";
                float nw = ui::textWidth(F, (unsigned)fs, nm + " ");
                auto wrapped = ui::wrapText(F, (unsigned)fs, e.text, textW, nw);
                for (size_t k = 0; k < wrapped.size(); ++k)
                    vls.push_back({ k == 0 ? nm : "", wrapped[k], nc, sf::Color(235, 230, 220), alpha, k == 0, nw });
            }
            int maxLines = (int)((logBot - logTop) / lineH);
            int start = std::max(0, (int)vls.size() - maxLines);
            float ly = logBot - (float)((int)vls.size() - start) * lineH;
            for (int k = start; k < (int)vls.size(); ++k, ly += lineH) {
                auto& v = vls[k];
                uint8_t a = (uint8_t)(255.f * v.alpha);
                float ox = cpX + pad;
                const float ol = chatActive ? 0.f : 1.5f;   // floating lines get an outline instead of a panel
                if (v.first)
                    drawText(v.name, (unsigned)fs, sf::Color(v.nameCol.r, v.nameCol.g, v.nameCol.b, a), { ox, ly }, 0, 1.f);
                drawText(v.text, (unsigned)fs, sf::Color(v.col.r, v.col.g, v.col.b, a), { ox + (v.first ? v.nameW : 0.f), ly }, 0, ol);
            }
            if (vls.empty() && chatActive)
                drawText("No messages yet", 13, sf::Color(170, 150, 120, 160), { cpX + pad, logBot - lineH }, 0);

            if (chatActive) {
            // input bar
            sf::FloatRect ib({ cpX + pad - 4.f, cpY + cpH - pad - inH + 4.f }, { cpW - pad * 2.f + 8.f, inH });
            ui::drawNinePatch(window, chatActive ? uiTheme.focus : uiTheme.interior, ib, 5.f, 2.f,
                              sf::Color(255, 255, 255, chatActive ? 255 : 170));
            const float iy = ib.position.y + ib.size.y * 0.5f - 8.f;
            if (chatActive) {
                std::string shown = chatInput;
                while (shown.size() > 1 && ui::textWidth(F, 15, shown) > ib.size.x - 100.f) shown.erase(0, 1);
                drawText(shown, 15, sf::Color(255, 245, 160), { ib.position.x + 12.f, iy }, 0);
                if (std::fmod(uiTime, 1.0f) < 0.55f) {
                    float cw = shown.empty() ? 0.f : ui::textWidth(F, 15, shown);
                    sf::RectangleShape cur({ 2.f, 17.f });
                    cur.setPosition({ ib.position.x + 13.f + cw, iy + 1.f });
                    cur.setFillColor(sf::Color(255, 245, 160));
                    window.draw(cur);
                }
                bool nearLimit = (int)chatInput.size() > gc::CHAT_LEN - 20;
                drawText(std::to_string(chatInput.size()) + "/" + std::to_string(gc::CHAT_LEN), 12,
                         nearLimit ? sf::Color(255, 130, 110) : sf::Color(190, 170, 140), { ib.position.x + ib.size.x - 12.f, iy + 2.f }, 2);
            }

            }

            // minimap (bottom-right): explored area only, base markers, local player
            if (myId < gc::NUM_PLAYERS)
                fogOfWar.drawMinimap(window, { HW - 12.f - 3.f - gc::MAP_W * 3.f, HH - 54.f - gc::MAP_H * 3.f }, 3.f, TEAM_COLOR,
                                     disp[myId] + sf::Vector2f(8.f, 8.f), TEAM_COLOR[myId], viewRect, uiTime);

            // controls hint (bottom, right of chat)
            drawText("WASD move  |  Space shoot (sends foes home)  |  T / Enter chat  |  Esc close", 14,
                     sf::Color(230, 225, 210), { HW - 16.f, HH - 28.f }, 2, 1.5f);
        }

        if (!net.connected) {
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
