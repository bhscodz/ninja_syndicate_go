// Authoritative game server for the steal-the-treasure multiplayer game.
//
// Responsibilities:
//   - Host many independent ROOMS (up to 4 players each, own lobby/match/chat).
//     A client's Hello either creates a room or joins one by its short code.
//   - Simulate all players (human + AI bots that fill empty slots).
//   - Resolve movement/collision, shooting, treasure stealing/delivery, and
//     the "teleport-home" hit mechanic.
//   - Broadcast snapshots at 30 Hz and push events/chat immediately.
//
// Run:  game_server.exe [port]
#include "../game/Constants.hpp"
#include "../game/Map.hpp"
#include "../game/Protocol.hpp"
#include "../game/SecureChannel.hpp"

#include <SFML/Network.hpp>
#include <SFML/System.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <csignal>
#include <memory>
#include <set>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float DIRVX[4] = { 0.f, 0.f, -1.f, 1.f };
constexpr float DIRVY[4] = { 1.f, -1.f, 0.f, 0.f };

struct ClientConn {
    std::unique_ptr<sf::TcpSocket> socket;
    uint8_t id = 255;
    bool dead = false;
};

std::string botName(int i) {
    const char* n[] = { "Rogue", "Blue Fox", "Greenling", "Sunny" };
    return n[i & 3];
}

class GameServer {
public:
    GameServer() { resetMatch(true); }   // gcmap::generate() is called once in main()

    // Lobby -> match: reset scores/positions/treasures, keep the seated players.
    void startMatch(const RoomSettings& rs) {
        settings_ = rs; sanitize(settings_);
        resetMatch(false);
    }
    // Match over -> lobby: scores/positions reset, same players.
    void resetToLobby() { resetMatch(false); }
    const RoomSettings& settings() const { return settings_; }
    void setPing(uint8_t id, int ms) { if (id < gc::NUM_PLAYERS) snap_.players[id].ping = (uint16_t)std::clamp(ms, 0, 9999); }
    void setConnected(uint8_t id, bool c) { if (id < gc::NUM_PLAYERS) snap_.players[id].connected = c; }
    void clearInput(uint8_t id) { if (id < gc::NUM_PLAYERS) inputs_[id] = ClientInput{}; }
    bool isGameOver() const { return snap_.gameOver; }
    const std::string& nameOf(uint8_t id) const { return snap_.players[id].name; }
    int playerCount() const {
        int n = 0; for (auto& p : snap_.players) if (p.active) ++n; return n;
    }

    // Convert the first available empty slot to a human player. Returns 255 if full.
    uint8_t assignHuman() {
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (!snap_.players[i].active) {
                snap_.players[i].active = true;
                snap_.players[i].connected = true;
                snap_.players[i].ping = 0;
                snap_.players[i].isBot = false;
                snap_.players[i].score = 0;
                snap_.players[i].carrying = false;
                snap_.players[i].carriedTreasure = 255;
                snap_.players[i].x = gcmap::bases()[i].spawnX;
                snap_.players[i].y = gcmap::bases()[i].spawnY;
                snap_.players[i].ammo = gc::MAX_AMMO;
                return i;
            }
        }
        return 255; // all slots full
    }

    void releaseSlot(uint8_t id) {
        if (id >= gc::NUM_PLAYERS) return;
        auto& p = snap_.players[id];
        // drop any carried treasure back home
        if (p.carrying && p.carriedTreasure < gc::NUM_PLAYERS) {
            auto& t = snap_.treasures[p.carriedTreasure];
            t.present = true; t.x = t.homeX; t.y = t.homeY; t.carriedBy = 255;
        }
        p.active = false;
        p.isBot = false;
        p.carrying = false; p.carriedTreasure = 255;
        pushEvent({ EvType::Leave, id, 255, 255 });
    }

    void setName(uint8_t id, const std::string& n) {
        if (id < gc::NUM_PLAYERS) snap_.players[id].name = n.empty() ? botName(id) : n;
    }

    void setInput(uint8_t id, const ClientInput& in) {
        if (id < gc::NUM_PLAYERS && !snap_.players[id].isBot)
            inputs_[id] = in;
    }

    void pushEvent(const GameEvent& e) { events_.push_back(e); }
    void clearEvents() { events_.clear(); }

    void broadcastChat(uint8_t id, const std::string& text) {
        if (id >= gc::NUM_PLAYERS || text.empty()) return;
        ChatLine c{ id, snap_.players[id].name, text };
        chatQueue_.push_back(c);
    }

    const Snapshot& snapshot() const { return snap_; }

    void update(float dt) {
        // per-player timers + input resolution (no AI)
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& p = snap_.players[i];
            if (!p.active) continue;
            p.shootCd -= dt; if (p.shootCd < 0) p.shootCd = 0;
            p.ammoTimer += dt;
            if (p.ammoTimer >= gc::AMMO_REGEN_TIME && p.ammo < gc::MAX_AMMO) {
                p.ammo++; p.ammoTimer = 0;
            } else if (p.ammo >= gc::MAX_AMMO) p.ammoTimer = 0;

            ClientInput in = inputs_[i];
            if (in.shoot) tryShoot(p);
            applyMove(p, in, dt);
        }

        updateProjectiles(dt);
        updateTreasures();
        updateMatchClock(dt);
    }

    bool consumeEvents(std::vector<GameEvent>& out) {
        if (events_.empty()) return false;
        out.swap(events_);
        return true;
    }
    bool consumeChat(std::vector<ChatLine>& out) {
        if (chatQueue_.empty()) return false;
        out.swap(chatQueue_);
        return true;
    }

private:
    Snapshot snap_;
    std::array<ClientInput, gc::NUM_PLAYERS> inputs_{};
    std::vector<GameEvent> events_;
    std::vector<ChatLine>  chatQueue_;
    uint32_t nextProjId_ = 1;
    float restartTimer_ = 0;
    RoomSettings settings_;

    void resetMatch(bool firstTime) {
        if (!firstTime) {
            // soft reset: keep slots/humans, reset positions & scores
        }
        snap_.matchTime = settings_.minutes * 60.f;
        snap_.matchLen = snap_.matchTime;
        snap_.winScore = settings_.winScore;
        snap_.fog = settings_.fog;
        snap_.gameOver = false;
        snap_.winner = 255;
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& p = snap_.players[i];
            p.id = i;
            if (firstTime) { p.active = false; p.isBot = false; p.name = ""; }
            p.x = gcmap::bases()[i].spawnX;
            p.y = gcmap::bases()[i].spawnY;
            p.facing = gc::FACE_DOWN;
            p.ammo = gc::MAX_AMMO; p.ammoTimer = 0; p.shootCd = 0;
            p.score = 0; p.carrying = false; p.carriedTreasure = 255;
            p.moving = false; p.walkFrame = 0;
            p.botStuck = 0; p.botShootCd = 0; p.botMode = 0;
        }
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& t = snap_.treasures[i];
            t.id = i; t.baseOwner = i;
            t.homeX = gcmap::bases()[i].treasureX;
            t.homeY = gcmap::bases()[i].treasureY;
            t.x = t.homeX; t.y = t.homeY;
            t.present = true; t.carriedBy = 255;
        }
        snap_.projectileCount = 0;
    }

    static void applyMove(PlayerState& p, const ClientInput& in, float dt) {
        float dx = in.dirX, dy = in.dirY;
        float len = std::sqrt(dx * dx + dy * dy);
        bool moving = len > 0.001f;
        if (moving) { dx /= len; dy /= len; p.facing = in.facing; }
        p.moving = moving;
        const float step = gc::PLAYER_SPEED * dt;
        const float o = gcmap::boxOffset();
        float nx = p.x + dx * step;
        if (!gcmap::solidBox(nx + o, p.y + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) p.x = nx;
        float ny = p.y + dy * step;
        if (!gcmap::solidBox(p.x + o, ny + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) p.y = ny;
        if (moving) { p.walkTimer += dt; p.walkFrame = static_cast<int>(p.walkTimer * 9.f) % 4; }
        else { p.walkTimer = 0; p.walkFrame = 0; }
    }

    void tryShoot(PlayerState& p) {
        if (p.shootCd > 0 || p.ammo <= 0 || snap_.projectileCount >= gc::MAX_PROJECTILES) return;
        p.shootCd = gc::SHOOT_COOLDOWN;
        p.ammo--;
        ProjectileState pr;
        pr.id = nextProjId_++;
        pr.ownerId = p.id;
        pr.x = p.x + gc::TILE * 0.5f + DIRVX[p.facing] * 12.f;
        pr.y = p.y + gc::TILE * 0.5f + DIRVY[p.facing] * 12.f;
        pr.vx = DIRVX[p.facing] * gc::PROJECTILE_SPEED;
        pr.vy = DIRVY[p.facing] * gc::PROJECTILE_SPEED;
        pr.dir = p.facing; pr.frame = 0;
        snap_.projectiles[snap_.projectileCount++] = pr;
        pushEvent({ EvType::Shoot, p.id, 255, 255 });
    }

    void updateProjectiles(float dt) {
        int& n = snap_.projectileCount;
        for (int i = 0; i < n; ) {
            auto& pr = snap_.projectiles[i];
            pr.x += pr.vx * dt;
            pr.y += pr.vy * dt;
            pr.frame = (pr.frame + 1) & 3;
            bool remove = false;
            // expire / out of bounds
            if (pr.x < 0 || pr.y < 0 || pr.x > gc::MAP_W * gc::TILE || pr.y > gc::MAP_H * gc::TILE)
                remove = true;
            // hit a solid tile
            if (!remove && gcmap::solidBox(pr.x - gc::PROJECTILE_R, pr.y - gc::PROJECTILE_R,
                                           gc::PROJECTILE_R * 2, gc::PROJECTILE_R * 2))
                remove = true;
            // hit a player (not the owner)
            if (!remove) {
                for (uint8_t j = 0; j < gc::NUM_PLAYERS; ++j) {
                    auto& pl = snap_.players[j];
                    if (!pl.active || j == pr.ownerId) continue;
                    float dx = (pl.x + 8) - pr.x;
                    float dy = (pl.y + 8) - pr.y;
                    float r = gc::PLAYER_HIT_R + gc::PROJECTILE_R;
                    if (dx * dx + dy * dy < r * r) {
                        teleportHome(pl);
                        pushEvent({ EvType::Teleport, pl.id, pr.ownerId, 255 });
                        remove = true;
                        break;
                    }
                }
            }
            if (remove) {
                pr = snap_.projectiles[n - 1]; // swap-remove
                --n;
            } else {
                ++i;
            }
        }
    }

    void teleportHome(PlayerState& p) {
        if (p.carrying && p.carriedTreasure < gc::NUM_PLAYERS) {
            auto& t = snap_.treasures[p.carriedTreasure];
            t.present = true; t.x = t.homeX; t.y = t.homeY; t.carriedBy = 255;
            p.carrying = false; p.carriedTreasure = 255;
        }
        p.x = gcmap::bases()[p.id].spawnX;
        p.y = gcmap::bases()[p.id].spawnY;
        p.moving = false; p.walkFrame = 0;
    }

    void updateTreasures() {
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& p = snap_.players[i];
            if (!p.active) continue;
            float pcx = p.x + 8, pcy = p.y + 8;
            if (!p.carrying) {
                for (uint8_t k = 0; k < gc::NUM_PLAYERS; ++k) {
                    auto& t = snap_.treasures[k];
                    if (!t.present || t.baseOwner == static_cast<int>(p.id)) continue;
                    float dx = pcx - t.x, dy = pcy - t.y;
                    if (dx * dx + dy * dy < 196.f) { // 14px
                        t.present = false; t.carriedBy = p.id;
                        p.carrying = true; p.carriedTreasure = k;
                        pushEvent({ EvType::Steal, p.id, static_cast<uint8_t>(t.baseOwner), k });
                        break;
                    }
                }
            } else {
                auto& home = gcmap::bases()[p.id];
                float dx = pcx - home.treasureX, dy = pcy - home.treasureY;
                if (dx * dx + dy * dy < 256.f) { // 16px
                    p.score++;
                    auto& t = snap_.treasures[p.carriedTreasure];
                    t.present = true; t.x = t.homeX; t.y = t.homeY; t.carriedBy = 255;
                    p.carrying = false; p.carriedTreasure = 255;
                    pushEvent({ EvType::Deliver, p.id, 255, 255 });
                    if (p.score >= settings_.winScore && !snap_.gameOver) {
                        snap_.gameOver = true;
                        snap_.winner = p.id;
                        snap_.matchTime = 0;
                        restartTimer_ = 5.f;
                        pushEvent({ EvType::GameOver, p.id, 255, 255 });
                    }
                }
            }
        }
    }

    void updateMatchClock(float dt) {
        if (snap_.gameOver) return;   // the room returns to the lobby (see Room::tick)
        snap_.matchTime -= dt;
        if (snap_.matchTime <= 0) {
            snap_.matchTime = 0;
            snap_.gameOver = true;
            restartTimer_ = 5.f;
            // winner = highest score
            int best = 0, bestId = 255; bool tie = false;
            for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
                int s = snap_.players[i].score;
                if (s > best) { best = s; bestId = i; tie = false; }
                else if (s == best && s > 0) tie = true;
            }
            snap_.winner = tie ? 255 : static_cast<uint8_t>(bestId);
            pushEvent({ EvType::GameOver, snap_.winner, 255, 255 });
        }
    }

    // ---- Minimal but functional AI: greedy axis pursuit + reactive shots ----
    ClientInput botThink(PlayerState& bot, float dt) {
        ClientInput in;
        in.facing = bot.facing;

        // choose a target: carry home, else nearest present enemy treasure
        float tx, ty;
        if (bot.carrying) {
            auto& h = gcmap::bases()[bot.id];
            tx = h.treasureX; ty = h.treasureY;
        } else {
            int best = -1; float bd = 1e9f;
            for (uint8_t k = 0; k < gc::NUM_PLAYERS; ++k) {
                auto& t = snap_.treasures[k];
                if (!t.present || t.baseOwner == static_cast<int>(bot.id)) continue;
                float d = (t.x - (bot.x + 8)) * (t.x - (bot.x + 8))
                        + (t.y - (bot.y + 8)) * (t.y - (bot.y + 8));
                if (d < bd) { bd = d; best = k; }
            }
            if (best < 0) { tx = gcmap::bases()[bot.id].treasureX; ty = gcmap::bases()[bot.id].treasureY; }
            else { tx = snap_.treasures[best].x; ty = snap_.treasures[best].y; }
        }

        float ddx = tx - (bot.x + 8), ddy = ty - (bot.y + 8);
        const float o = gcmap::boxOffset();
        const float probe = gc::PLAYER_SPEED * dt + 0.5f;

        bot.botStuck -= dt;
        if (bot.botStuck > 0) {
            // keep last facing, nudge perpendicular (set during stuck entry)
            in.dirX = DIRVX[bot.facing] * 0.6f;
            in.dirY = DIRVY[bot.facing] * 0.6f;
        } else if (std::abs(ddx) > std::abs(ddy) + 1.f) {
            int d = ddx > 0 ? gc::FACE_RIGHT : gc::FACE_LEFT;
            if (gcmap::solidBox(bot.x + o + DIRVX[d] * probe, bot.y + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) {
                // blocked: try vertical
                int vd = ddy > 0 ? gc::FACE_DOWN : gc::FACE_UP;
                if (!gcmap::solidBox(bot.x + o, bot.y + o + DIRVY[vd] * probe, gc::PLAYER_BOX, gc::PLAYER_BOX)) { d = vd; }
                else { bot.botStuck = 0.25f; }
            }
            in.facing = d; in.dirX = DIRVX[d]; in.dirY = DIRVY[d];
        } else if (std::abs(ddy) > 1.f) {
            int d = ddy > 0 ? gc::FACE_DOWN : gc::FACE_UP;
            if (gcmap::solidBox(bot.x + o, bot.y + o + DIRVY[d] * probe, gc::PLAYER_BOX, gc::PLAYER_BOX)) {
                int hd = ddx > 0 ? gc::FACE_RIGHT : gc::FACE_LEFT;
                if (!gcmap::solidBox(bot.x + o + DIRVX[hd] * probe, bot.y + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) { d = hd; }
                else { bot.botStuck = 0.25f; }
            }
            in.facing = d; in.dirX = DIRVX[d]; in.dirY = DIRVY[d];
        }

        // shooting: fire if an enemy is roughly in front, on a short cooldown
        bot.botShootCd -= dt;
        if (bot.ammo > 0 && bot.shootCd <= 0 && bot.botShootCd <= 0) {
            float ax = bot.x + 8, ay = bot.y + 8;
            float fx = DIRVX[bot.facing], fy = DIRVY[bot.facing];
            for (uint8_t j = 0; j < gc::NUM_PLAYERS; ++j) {
                if (j == bot.id || !snap_.players[j].active) continue;
                float ex = (snap_.players[j].x + 8) - ax;
                float ey = (snap_.players[j].y + 8) - ay;
                float fwd = ex * fx + ey * fy;
                float perp = std::abs(ex * (-fy) + ey * fx);
                if (fwd > 10.f && fwd < 130.f && perp < 16.f) {
                    in.shoot = true;
                    bot.botShootCd = 0.9f;
                    break;
                }
            }
        }
        return in;
    }
};

// ===================== rooms, connections, hardening =====================

constexpr int   MAX_ROOMS = 100, MAX_CONNS = 400, MAX_CONN_PER_IP = 8;
constexpr float GRACE_SECONDS = 20.f;      // reconnect window for a dropped player mid-match
constexpr float TIMEOUT_SECONDS = 9.f;     // no packet at all for this long -> drop
constexpr float HELLO_TIMEOUT = 10.f;      // handshake + Join must complete within this
constexpr float COUNTDOWN_SECONDS = 5.f;
constexpr float RESULT_SECONDS = 7.f;      // game-over screen time before the room returns to the lobby

sf::Clock g_clock;
uint32_t nowMs() { return static_cast<uint32_t>(g_clock.getElapsedTime().asMilliseconds()); }
volatile std::sig_atomic_t g_stop = 0;
extern "C" void onSignal(int) { g_stop = 1; }
sec::KeyPair g_identity;

struct Bucket {
    float tokens, cap, rate;   // rate = tokens per second
    Bucket(float c = 5.f, float r = 1.f) : tokens(c), cap(c), rate(r) {}
    void refill(float dt) { tokens = std::min(cap, tokens + rate * dt); }
    bool take() { if (tokens >= 1.f) { tokens -= 1.f; return true; } return false; }
};

struct Room;

struct Conn {
    std::unique_ptr<sf::TcpSocket> socket;
    sec::Channel chan;
    int stage = 0;               // 0 = awaiting handshake, 1 = secure (awaiting Join), 2 = in a room
    uint8_t id = 255;
    bool dead = false, cleanLeave = false;
    Room* room = nullptr;
    std::string ip;
    float age = 0.f, sinceRecv = 0.f, pingTimer = 0.f, pingSmooth = -1.f;
    Bucket chat{ 5.f, 1.f };

    // encrypts + sends; marks the connection dead on failure
    bool send(sf::Packet& plain) {
        if (dead || !chan.on()) return false;
        sf::Packet w;
        if (!chan.seal(plain, w)) { dead = true; return false; }
        auto st = sec::sendPacket(*socket, w);
        if (st == sf::Socket::Status::Disconnected || st == sf::Socket::Status::Error) { dead = true; return false; }
        return true;
    }
};

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

struct Slot {
    uint64_t token = 0;
    Conn* conn = nullptr;
    bool ready = false;
    float graceLeft = 0.f;       // > 0 while the slot is reserved for a reconnecting player
    uint8_t lastWhisper = 255;
};

enum class RState { Lobby, Countdown, Match };

struct Room {
    std::string code;
    GameServer game;
    RState state = RState::Lobby;
    RoomSettings settings;
    uint8_t host = 255;
    std::array<Slot, gc::NUM_PLAYERS> slots;
    std::vector<Conn*> members;
    float snapAcc = 0.f, countdownLeft = 0.f, resultTimer = 0.f, lobbyTimer = 0.f;
    bool lobbyDirty = true;

    bool active(int i) const { return game.snapshot().players[i].active; }
    void send(sf::Packet& pkt) { for (auto* c : members) c->send(pkt); }
    void sendLine(Conn* c, const ChatLine& l) { if (!c) return; sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Chat) << l; c->send(p); }
    void sendAllLine(const ChatLine& l) { sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Chat) << l; send(p); }
    void system(Conn* c, const std::string& text) { ChatLine l; l.fromId = 255; l.name = "server"; l.text = text; l.kind = CHAT_SYSTEM; sendLine(c, l); }

    void pickHost() {
        host = 255;
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) if (active(i) && slots[i].conn) { host = i; return; }
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) if (active(i)) { host = i; return; }
    }
    LobbyInfo lobbyInfo() const {
        LobbyInfo li; li.hostId = host; li.inMatch = state == RState::Match; li.settings = settings;
        li.countdown = state == RState::Countdown ? static_cast<int8_t>(std::ceil(countdownLeft)) : static_cast<int8_t>(-1);
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            const auto& p = game.snapshot().players[i];
            li.slots[i].active = p.active;
            li.slots[i].ready = slots[i].ready;
            li.slots[i].connected = slots[i].conn != nullptr;
            li.slots[i].ping = p.ping;
            li.slots[i].name = p.name;
        }
        return li;
    }
    std::string uniqueName(std::string name) const {
        auto taken = [&](const std::string& n) {
            for (int i = 0; i < gc::NUM_PLAYERS; ++i) if (active(i) && lower(game.nameOf(i)) == lower(n)) return true;
            return false;
        };
        if (!taken(name)) return name;
        for (int n = 2; n < 10; ++n) {
            std::string suffix = " #" + std::to_string(n);
            std::string base = name; if (base.size() + suffix.size() > static_cast<size_t>(gc::NAME_LEN)) base.resize(gc::NAME_LEN - suffix.size());
            if (!taken(base + suffix)) return base + suffix;
        }
        return name;
    }

    // Permanently remove a player (clean leave, kick, expired reservation).
    void removePlayer(uint8_t id) {
        if (id >= gc::NUM_PLAYERS || !active(id)) return;
        game.releaseSlot(id);
        slots[id] = Slot{};
        game.setConnected(id, true);
        if (host == id || host == 255) pickHost();
        lobbyDirty = true;
        if (state == RState::Countdown && game.playerCount() == 0) state = RState::Lobby;
    }

    void beginMatch() {
        state = RState::Match;
        game.startMatch(settings);
        snapAcc = 0.f; resultTimer = 0.f;
        for (auto& s : slots) s.ready = false;
        sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_GameStart);
        send(p);
        lobbyDirty = true;
        std::cout << "Room " << code << ": match started (" << game.playerCount() << " players, "
                  << int(settings.minutes) << " min, first to " << int(settings.winScore) << ")\n";
    }
    void toLobby() {
        state = RState::Lobby;
        game.resetToLobby();
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            slots[i].ready = false;
            if (active(i) && !slots[i].conn) removePlayer(i);   // anyone still reconnecting is gone now
        }
        sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_ToLobby);
        send(p);
        lobbyDirty = true;
        std::cout << "Room " << code << ": back to lobby\n";
    }

    void tick(float dt) {
        // reserved slots expire
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (active(i) && !slots[i].conn && slots[i].graceLeft > 0.f) {
                slots[i].graceLeft -= dt;
                if (slots[i].graceLeft <= 0.f) { std::cout << "Room " << code << ": player " << int(i) << " did not come back\n"; removePlayer(i); }
            }
        }
        if (state == RState::Countdown) {
            int before = static_cast<int>(std::ceil(countdownLeft));
            countdownLeft -= dt;
            if (countdownLeft <= 0.f) beginMatch();
            else if (static_cast<int>(std::ceil(countdownLeft)) != before) lobbyDirty = true;
        }
        if (state == RState::Match) {
            game.update(dt);
            std::vector<GameEvent> ev;
            if (game.consumeEvents(ev))
                for (const auto& e : ev) { sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Event) << e; send(p); }
            snapAcc += dt;
            if (snapAcc >= gc::SNAP_INTERVAL) {
                snapAcc = 0.f;
                sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Snapshot) << game.snapshot();
                send(p);
            }
            if (game.isGameOver()) {
                resultTimer += dt;
                if (resultTimer > RESULT_SECONDS) toLobby();
            }
        } else {
            game.clearEvents();
        }
        lobbyTimer += dt;
        if (state != RState::Match && (lobbyDirty || lobbyTimer > 1.f)) {
            lobbyDirty = false; lobbyTimer = 0.f;
            sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Lobby) << lobbyInfo();
            send(p);
        } else if (lobbyTimer > 1.f) lobbyTimer = 0.f;
    }

    // ---- chat / commands ----
    // Finds a player by exact name or unique prefix. Returns slot, -1 not found, -2 ambiguous.
    int findPlayer(const std::string& q, uint8_t except, std::string* ambig = nullptr) const {
        std::string ql = lower(q);
        std::vector<int> pre;
        for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (!active(i) || i == except) continue;
            std::string n = lower(game.nameOf(i));
            if (n == ql) return i;
            if (n.rfind(ql, 0) == 0) pre.push_back(i);
        }
        if (pre.size() == 1) return pre[0];
        if (pre.empty()) return -1;
        if (ambig) { for (int i : pre) { if (!ambig->empty()) *ambig += ", "; *ambig += game.nameOf(i); } }
        return -2;
    }

    void whisper(Conn& from, int to, const std::string& text) {
        if (to < 0 || to >= gc::NUM_PLAYERS || !active(to)) { system(&from, "That player is not here."); return; }
        if (to == from.id) { system(&from, "You cannot whisper to yourself."); return; }
        if (!slots[to].conn) { system(&from, game.nameOf(to) + " is offline (reconnecting)."); return; }
        if (text.empty()) { system(&from, "Usage: /w <name> <message>"); return; }
        ChatLine in; in.fromId = from.id; in.name = game.nameOf(from.id); in.text = text; in.kind = CHAT_WHISPER_FROM;
        ChatLine out; out.fromId = static_cast<uint8_t>(to); out.name = game.nameOf(to); out.text = text; out.kind = CHAT_WHISPER_TO;
        sendLine(slots[to].conn, in);
        sendLine(&from, out);
        slots[to].lastWhisper = from.id;
        slots[from.id].lastWhisper = static_cast<uint8_t>(to);
    }

    void chat(Conn& c, std::string text) {
        std::string t;
        for (char ch : text) if (ch >= 32 && ch < 127) t.push_back(ch);
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.empty()) return;
        if (!c.chat.take()) { system(&c, "Slow down - you are sending messages too fast."); return; }
        if (t[0] != '/') {
            ChatLine l; l.fromId = c.id; l.name = game.nameOf(c.id); l.text = t; l.kind = CHAT_NORMAL;
            sendAllLine(l);
            return;
        }
        size_t sp = t.find(' ');
        std::string cmd = lower(t.substr(1, sp == std::string::npos ? std::string::npos : sp - 1));
        std::string args = sp == std::string::npos ? "" : t.substr(sp + 1);
        while (!args.empty() && args.front() == ' ') args.erase(args.begin());
        if (cmd == "w" || cmd == "msg" || cmd == "whisper" || cmd == "pm" || cmd == "tell") {
            if (args.empty()) { system(&c, "Usage: /w <name> <message>"); return; }
            // 1) longest full player name followed by a space (handles names with spaces)
            int best = -1; size_t bestLen = 0; std::string al = lower(args);
            for (int i = 0; i < gc::NUM_PLAYERS; ++i) {
                if (!active(i) || i == c.id) continue;
                std::string n = lower(game.nameOf(i));
                if (n.size() > bestLen && al.size() > n.size() && al.rfind(n, 0) == 0 && al[n.size()] == ' ') { best = i; bestLen = n.size(); }
            }
            if (best >= 0) { std::string msg = args.substr(bestLen + 1); while (!msg.empty() && msg.front() == ' ') msg.erase(msg.begin()); whisper(c, best, msg); return; }
            // 2) first word as name / unique prefix
            size_t s2 = args.find(' ');
            std::string tok = args.substr(0, s2);
            std::string msg = s2 == std::string::npos ? "" : args.substr(s2 + 1);
            std::string amb;
            int idx = findPlayer(tok, c.id, &amb);
            if (idx == -1) { system(&c, "No player named '" + tok + "' in this room."); return; }
            if (idx == -2) { system(&c, "'" + tok + "' is ambiguous: " + amb); return; }
            whisper(c, idx, msg);
        } else if (cmd == "r" || cmd == "reply") {
            uint8_t lw = slots[c.id].lastWhisper;
            if (lw >= gc::NUM_PLAYERS || !active(lw)) { system(&c, "Nobody to reply to."); return; }
            whisper(c, lw, args);
        } else if (cmd == "help" || cmd == "?") {
            system(&c, "/w <name> <text>  whisper (aliases /msg /whisper /pm), names can be abbreviated");
            system(&c, "/r <text>  reply to the last whisper");
            system(&c, "/kick <name>  host only, lobby only");
            system(&c, "/name <text>  host only: room name (lobby)");
        } else if (cmd == "kick") {
            kick(c, args);
        } else if (cmd == "name") {
            if (c.id != host || state != RState::Lobby) { system(&c, "Only the host can rename the room in the lobby."); return; }
            settings.name = args; sanitize(settings); lobbyDirty = true;
        } else {
            system(&c, "Unknown command. Try /help");
        }
    }

    void kick(Conn& c, const std::string& who) {
        if (c.id != host) { system(&c, "Only the host can kick."); return; }
        if (state == RState::Match) { system(&c, "You can only kick in the lobby."); return; }
        if (who.empty()) { system(&c, "Usage: /kick <name>"); return; }
        std::string amb; int idx = findPlayer(who, c.id, &amb);
        if (idx == -1) { system(&c, "No player named '" + who + "'."); return; }
        if (idx == -2) { system(&c, "'" + who + "' is ambiguous: " + amb); return; }
        kickSlot(static_cast<uint8_t>(idx));
    }
    void kickSlot(uint8_t idx) {
        Conn* t = slots[idx].conn;
        std::cout << "Room " << code << ": player " << int(idx) << " kicked\n";
        if (t) {
            sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Error) << static_cast<uint8_t>(ErrCode::Kicked) << std::string("You were kicked by the host");
            t->send(p);
            t->dead = true; t->cleanLeave = true;
        } else removePlayer(idx);
    }
};

void sendError(Conn& c, ErrCode code, const std::string& text) {
    sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Error) << static_cast<uint8_t>(code) << text;
    c.send(p);
    c.dead = true; c.cleanLeave = true;
}

std::map<std::string, std::unique_ptr<Room>> g_rooms;
std::map<std::string, int> g_ipCount;
std::map<std::string, Bucket> g_ipCreate;
std::mt19937 g_rng{ std::random_device{}() ^ static_cast<unsigned>(nowMs()) };

std::string newRoomCode() {
    const int n = static_cast<int>(std::strlen(ROOM_ALPHABET));
    for (;;) {
        std::string c;
        uint8_t rb[ROOM_CODE_LEN]; sec::randomBytes(rb, sizeof(rb));
        for (int i = 0; i < ROOM_CODE_LEN; ++i) c.push_back(ROOM_ALPHABET[rb[i] % n]);
        if (!g_rooms.count(c)) return c;
    }
}

void handleHello(Conn& c, sf::Packet& pkt) {
    std::string name, code; uint8_t mode = 0; uint64_t token = 0;
    pkt >> name >> mode >> code >> token;
    if (!pkt) { sendError(c, ErrCode::BadRequest, "Bad request"); return; }
    { std::string n; for (char ch : name) if (ch >= 32 && ch < 127) n.push_back(ch);
      while (!n.empty() && n.front() == ' ') n.erase(n.begin());
      while (!n.empty() && n.back() == ' ') n.pop_back();
      if (n.size() > static_cast<size_t>(gc::NAME_LEN)) n.resize(gc::NAME_LEN);
      name = n.empty() ? "Player" : n; }
    Room* room = nullptr;
    if (mode == static_cast<uint8_t>(JoinMode::Rejoin)) {
        auto it = g_rooms.find(normalizeCode(code));
        if (it == g_rooms.end()) { sendError(c, ErrCode::NotFound, "Session expired (room is gone)"); return; }
        room = it->second.get();
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (room->active(i) && !room->slots[i].conn && room->slots[i].token == token && token != 0) {
                c.id = i; c.room = room; c.stage = 2;
                room->slots[i].conn = &c; room->slots[i].graceLeft = 0.f;
                room->members.push_back(&c);
                room->game.setConnected(i, true);
                room->game.pushEvent({ EvType::Reconnected, i, 255, 255 });
                if (room->host == 255) room->pickHost();
                sf::Packet w; w << static_cast<uint32_t>(Msg::S2C_Welcome) << i << room->code << token;
                c.send(w);
                if (room->state == RState::Match) { sf::Packet gs; gs << static_cast<uint32_t>(Msg::S2C_GameStart); c.send(gs); }
                room->lobbyDirty = true;
                std::cout << "Room " << room->code << ": player " << int(i) << " reconnected\n";
                return;
            }
        }
        sendError(c, ErrCode::NotFound, "Session expired");
        return;
    }
    if (mode == static_cast<uint8_t>(JoinMode::Create)) {
        if (static_cast<int>(g_rooms.size()) >= MAX_ROOMS) { sendError(c, ErrCode::Busy, "Server is full (room limit reached)"); return; }
        auto bit = g_ipCreate.emplace(c.ip, Bucket(3.f, 1.f / 10.f)).first;
        if (!bit->second.take()) { sendError(c, ErrCode::Busy, "Too many rooms created, please wait a few seconds"); return; }
        auto r = std::make_unique<Room>();
        r->code = newRoomCode();
        room = r.get();
        g_rooms[r->code] = std::move(r);
        std::cout << "Room " << room->code << " created\n";
    } else {
        auto it = g_rooms.find(normalizeCode(code));
        if (it == g_rooms.end()) { sendError(c, ErrCode::NotFound, "Room not found"); return; }
        room = it->second.get();
        if (room->state != RState::Lobby) { sendError(c, ErrCode::Started, "Match already started"); return; }
    }
    uint8_t id = room->game.assignHuman();
    if (id == 255) { sendError(c, ErrCode::RoomFull, "Room is full"); return; }
    room->game.setName(id, room->uniqueName(name));
    Slot& sl = room->slots[id];
    sl = Slot{}; sl.conn = &c; sl.token = sec::randomToken();
    c.id = id; c.room = room; c.stage = 2;
    room->members.push_back(&c);
    if (room->host == 255) room->pickHost();
    sf::Packet w; w << static_cast<uint32_t>(Msg::S2C_Welcome) << id << room->code << sl.token;
    c.send(w);
    room->lobbyDirty = true;
    std::cout << "Room " << room->code << ": player " << int(id) << " (" << room->game.nameOf(id) << ") joined\n";
}

// plaintext handshake: client sends (magic, Qc); we answer (magic, Qs, S) and switch to AEAD.
void handleHandshake(Conn& c, sf::Packet& pkt) {
    uint32_t magic = 0; std::string qc;
    pkt >> magic >> qc;
    if (!pkt || magic != sec::MAGIC || qc.size() != 32) { c.dead = true; c.cleanLeave = true; return; }
    sec::KeyPair e = sec::generateKeyPair();
    uint8_t dh1[32], dh2[32];
    if (!e.ok || !sec::dh(e.priv, (const uint8_t*)qc.data(), dh1) || !sec::dh(g_identity.priv, (const uint8_t*)qc.data(), dh2)) { c.dead = true; return; }
    sec::SessionKeys k;
    if (!sec::deriveKeys(dh1, dh2, (const uint8_t*)qc.data(), e.pub, g_identity.pub, k)) { c.dead = true; return; }
    sf::Packet reply;
    reply << sec::MAGIC << std::string((const char*)e.pub, 32) << std::string((const char*)g_identity.pub, 32);
    if (sec::sendPacket(*c.socket, reply) != sf::Socket::Status::Done) { c.dead = true; return; }
    c.chan.start(k.s2c, k.c2s);
    c.stage = 1;
    sf::Packet ok; ok << static_cast<uint32_t>(Msg::S2C_Secure);
    c.send(ok);   // first AEAD packet: proves we hold the long-term key
}

void handlePacket(Conn& c, sf::Packet& pkt) {
    uint32_t m = 0; pkt >> m;
    Msg msg = static_cast<Msg>(m);
    if (msg == Msg::C2S_Pong) {
        uint32_t ts = 0; pkt >> ts;
        float rtt = static_cast<float>(static_cast<int32_t>(nowMs() - ts));
        if (rtt >= 0.f && rtt < 20000.f) {
            c.pingSmooth = c.pingSmooth < 0.f ? rtt : c.pingSmooth * 0.6f + rtt * 0.4f;
            if (c.room) c.room->game.setPing(c.id, static_cast<int>(c.pingSmooth + 0.5f));
        }
        return;
    }
    if (c.stage == 1) { if (msg == Msg::C2S_Join) handleHello(c, pkt); return; }
    if (!c.room) return;
    Room& r = *c.room;
    switch (msg) {
        case Msg::C2S_Input: {
            if (r.state != RState::Match) break;
            ClientInput in;
            pkt >> in.dirX >> in.dirY >> in.facing >> in.shoot;
            if (!(in.dirX >= -1.5f && in.dirX <= 1.5f)) in.dirX = 0;     // also rejects NaN
            if (!(in.dirY >= -1.5f && in.dirY <= 1.5f)) in.dirY = 0;
            if (in.facing < 0 || in.facing > 3) in.facing = gc::FACE_DOWN;
            r.game.setInput(c.id, in);
            break;
        }
        case Msg::C2S_Chat: {
            std::string text; pkt >> text;
            if (text.size() > static_cast<size_t>(gc::CHAT_LEN)) text.resize(gc::CHAT_LEN);
            r.chat(c, text);
            break;
        }
        case Msg::C2S_Ready:
            if (r.state == RState::Lobby) { r.slots[c.id].ready = !r.slots[c.id].ready; r.lobbyDirty = true; }
            break;
        case Msg::C2S_Settings: {
            RoomSettings rs; pkt >> rs;
            if (c.id == r.host && r.state == RState::Lobby && pkt) { r.settings = rs; r.lobbyDirty = true; }
            break;
        }
        case Msg::C2S_Kick: {
            std::string who; pkt >> who;
            r.kick(c, who);
            break;
        }
        case Msg::C2S_Start:
            if (r.state == RState::Lobby && c.id == r.host && r.game.playerCount() >= 1) {
                if (r.settings.countdown) { r.state = RState::Countdown; r.countdownLeft = COUNTDOWN_SECONDS; r.lobbyDirty = true; }
                else r.beginMatch();
            }
            break;
        case Msg::C2S_StartCancel:
            if (r.state == RState::Countdown && c.id == r.host) { r.state = RState::Lobby; r.lobbyDirty = true; }
            break;
        case Msg::C2S_Leave:
            c.dead = true; c.cleanLeave = true;
            break;
        default: break;
    }
}

std::string exeDir(const char* argv0) {
    std::string s = argv0 ? argv0 : "";
    size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? std::string() : s.substr(0, p + 1);
}

} // namespace

int main(int argc, char** argv) {
    unsigned short port = gc::SERVER_PORT;
    if (argc >= 2) port = static_cast<unsigned short>(std::atoi(argv[1]));
    std::cout << std::unitbuf;
    gcmap::generate();

    bool created = false;
    g_identity = sec::loadOrCreateIdentity(exeDir(argv[0]) + "server_key", created);
    if (!g_identity.ok) { std::cerr << "Server: could not create identity key\n"; return 1; }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    sf::TcpListener listener;
    if (listener.listen(port) != sf::Socket::Status::Done) {
        std::cerr << "Server: could not bind port " << port << "\n";
        return 1;
    }
    listener.setBlocking(false);
    sf::SocketSelector selector;
    selector.add(listener);

    std::vector<std::unique_ptr<Conn>> conns;

    std::cout << "Steal-the-Treasure server running on port " << port
              << " (rooms of up to " << gc::NUM_PLAYERS << " players).\n"
              << "Server key fingerprint: " << sec::fingerprint(g_identity.pub)
              << (created ? "  (new key generated)" : "") << "\n"
              << "Press Ctrl+C to stop.\n";

    sf::Clock clock;
    float housekeeping = 0.f;

    while (!g_stop) {
        if (selector.wait(sf::milliseconds(16))) {
            if (selector.isReady(listener)) {
                for (int guard = 0; guard < 8; ++guard) {
                    auto sock = std::make_unique<sf::TcpSocket>();
                    if (listener.accept(*sock) != sf::Socket::Status::Done) break;
                    std::string ip = sock->getRemoteAddress() ? sock->getRemoteAddress()->toString() : "?";
                    if (static_cast<int>(conns.size()) >= MAX_CONNS || g_ipCount[ip] >= MAX_CONN_PER_IP) { sock->disconnect(); continue; }
                    sock->setBlocking(false);
                    auto cc = std::make_unique<Conn>();
                    cc->socket = std::move(sock); cc->ip = ip; g_ipCount[ip]++;
                    selector.add(*cc->socket);
                    conns.push_back(std::move(cc));
                }
            }
            for (auto& c : conns) {
                if (c->dead || !selector.isReady(*c->socket)) continue;
                for (int guard = 0; guard < 64 && !c->dead; ++guard) {
                    sf::Packet pkt;
                    auto st = c->socket->receive(pkt);
                    if (st == sf::Socket::Status::Done) {
                        c->sinceRecv = 0.f;
                        if (c->stage == 0) {
                            if (pkt.getDataSize() > 256) { c->dead = true; c->cleanLeave = true; }
                            else handleHandshake(*c, pkt);
                        } else {
                            sf::Packet plain;
                            if (pkt.getDataSize() > sec::MAX_WIRE || !c->chan.open(pkt, plain)) {
                                std::cout << "Dropping " << c->ip << ": bad/oversized/replayed packet\n";
                                c->dead = true; c->cleanLeave = true;
                            } else handlePacket(*c, plain);
                        }
                    } else if (st == sf::Socket::Status::Disconnected || st == sf::Socket::Status::Error) c->dead = true;
                    else break;
                }
            }
        }

        float dt = std::min(clock.restart().asSeconds(), 0.05f);
        for (auto& kv : g_rooms) kv.second->tick(dt);

        // per-connection timers: pings, rate limits, timeouts
        for (auto& cp : conns) {
            Conn& c = *cp;
            if (c.dead) continue;
            c.age += dt; c.sinceRecv += dt; c.chat.refill(dt);
            if (c.stage >= 1) {
                c.pingTimer += dt;
                if (c.pingTimer >= 1.f) { c.pingTimer = 0.f; sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Ping) << nowMs(); c.send(p); }
            }
            if (c.stage < 2 && c.age > HELLO_TIMEOUT) { c.dead = true; c.cleanLeave = true; }
            if (c.sinceRecv > TIMEOUT_SECONDS) { std::cout << "Timeout: " << c.ip << "\n"; c.dead = true; }
        }
        housekeeping += dt;
        if (housekeeping > 30.f) {
            housekeeping = 0.f;
            for (auto it = g_ipCreate.begin(); it != g_ipCreate.end();) { it->second.refill(30.f); it = it->second.tokens >= it->second.cap ? g_ipCreate.erase(it) : std::next(it); }
        }

        // drop dead connections (reserve the slot if it was an unclean loss mid-match)
        for (auto it = conns.begin(); it != conns.end();) {
            Conn& c = **it;
            if (!c.dead) { ++it; continue; }
            selector.remove(*c.socket);
            if (--g_ipCount[c.ip] <= 0) g_ipCount.erase(c.ip);
            if (Room* r = c.room) {
                r->members.erase(std::remove(r->members.begin(), r->members.end(), &c), r->members.end());
                r->slots[c.id].conn = nullptr;
                if (!c.cleanLeave && r->state == RState::Match && r->active(c.id)) {
                    r->slots[c.id].graceLeft = GRACE_SECONDS;
                    r->game.setConnected(c.id, false);
                    r->game.clearInput(c.id);
                    r->game.pushEvent({ EvType::Lost, c.id, 255, 255 });
                    if (r->host == c.id) r->pickHost();
                    r->lobbyDirty = true;
                    std::cout << "Room " << r->code << ": player " << int(c.id) << " lost connection (slot reserved)\n";
                } else {
                    r->removePlayer(c.id);
                    std::cout << "Room " << r->code << ": player " << int(c.id) << " left\n";
                }
            }
            it = conns.erase(it);
        }
        for (auto it = g_rooms.begin(); it != g_rooms.end();) {
            if (it->second->game.playerCount() == 0) { std::cout << "Room " << it->first << " destroyed\n"; it = g_rooms.erase(it); }
            else ++it;
        }
    }

    std::cout << "Shutting down, notifying clients...\n";
    for (auto& cp : conns) {
        if (cp->stage >= 1 && !cp->dead) {
            sf::Packet p; p << static_cast<uint32_t>(Msg::S2C_Disconnect);
            cp->send(p);
        }
    }
    sf::sleep(sf::milliseconds(200));
    return 0;
}
