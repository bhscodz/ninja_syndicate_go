// Per-room match simulation (no sockets, so tools/rules_test.cpp can drive it headlessly).
// Rules are specified in docs/gameplay_spec.md; constants live in game/Rules.hpp.
//
// Treasure state machine (TState):
//   AtHome -> Carried (enemy within PICKUP_R), Dropped -> Carried (after DROP_LOCK), Dropped -> AtHome (owner touch, Recover),
//   Dropped -> Returning (timer), Returning -> AtHome (Returned), Carried -> AtHome (delivery), Carried -> Dropped (hit / connection
//   loss), Carried -> Returning (leave/kick/expiry/invariant), any -> Inactive via refreshLive().
#pragma once
#include "../game/Constants.hpp"
#include "../game/Map.hpp"
#include "../game/Protocol.hpp"
#include "../game/Rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace gcserver {
constexpr float DIRVX[4] = { 0.f, 0.f, -1.f, 1.f };
constexpr float DIRVY[4] = { 1.f, -1.f, 0.f, 0.f };
inline std::string botName(int i) {
    const char* n[] = { "Rogue", "Blue Fox", "Greenling", "Sunny" };
    return n[i & 3];
}
constexpr float STALE_TREASURE_SECONDS = 30.f;   // Dropped/Returning older than this is forced home by checkInvariants
}

class GameServer {
public:
    using LeapFn = float (*)(float, float, int, float);
    LeapFn leapLandingFn = &gcmap::leapLanding;   // test hook (the real map function by default)

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

    // false = reconnect grace (ghost). A carrier that loses connection drops the trophy where it stands.
    void setConnected(uint8_t id, bool c) {
        if (id >= gc::NUM_PLAYERS) return;
        auto& p = snap_.players[id];
        bool was = p.connected;
        p.connected = c;
        if (was && !c && p.active) {
            p.shootQueued = p.leapQueued = false;
            if (p.carrying && !snap_.gameOver) dropTreasure(p, 255);
        }
    }
    void clearInput(uint8_t id) {
        if (id >= gc::NUM_PLAYERS) return;
        inputs_[id] = ClientInput{};
        snap_.players[id].shootQueued = snap_.players[id].leapQueued = false;
    }
    bool isGameOver() const { return snap_.gameOver; }
    const std::string& nameOf(uint8_t id) const { return snap_.players[id].name; }
    int playerCount() const {
        int n = 0; for (auto& p : snap_.players) if (p.active) ++n; return n;
    }

    // Convert the first available empty slot to a human player. Returns 255 if full.
    uint8_t assignHuman() {
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            if (!snap_.players[i].active) {
                auto& p = snap_.players[i];
                p.active = true;
                p.connected = true;
                p.ping = 0;
                p.isBot = false;
                p.score = 0;
                p.carrying = false;
                p.carriedTreasure = 255;
                p.x = gcmap::bases()[i].spawnX;
                p.y = gcmap::bases()[i].spawnY;
                p.ammo = gc::MAX_AMMO;
                resetLeap(p);
                inputs_[i] = ClientInput{};
                refreshLive();
                return i;
            }
        }
        return 255; // all slots full
    }

    void releaseSlot(uint8_t id) {
        if (id >= gc::NUM_PLAYERS) return;
        auto& p = snap_.players[id];
        // a trophy in this player's hands flies home from where it is
        if (p.carrying && p.carriedTreasure < gc::NUM_PLAYERS) {
            auto& t = snap_.treasures[p.carriedTreasure];
            if (t.state == (uint8_t)TState::Carried) { t.x = p.x + 8; t.y = p.y + 8; startReturn(p.carriedTreasure); }
        }
        p.active = false;
        p.isBot = false;
        p.connected = true;
        p.carrying = false; p.carriedTreasure = 255;
        p.shootQueued = false; p.leapQueued = false;
        resetLeap(p);
        inputs_[id] = ClientInput{};
        refreshLive();
        pushEvent({ EvType::Leave, id, 255, 255 });
    }

    void setName(uint8_t id, const std::string& n) {
        if (id < gc::NUM_PLAYERS) snap_.players[id].name = n.empty() ? gcserver::botName(id) : n;
    }

    // Latches the shoot/leap edges so two input packets in one tick cannot lose a press (consumed in update()).
    void setInput(uint8_t id, const ClientInput& in) {
        if (id >= gc::NUM_PLAYERS || snap_.players[id].isBot) return;
        auto& p = snap_.players[id];
        p.shootQueued = p.shootQueued || in.shoot;
        p.leapQueued  = p.leapQueued  || in.leap;
        inputs_[id] = in;
        inputs_[id].shoot = false; inputs_[id].leap = false;
    }

    void pushEvent(const GameEvent& e) { events_.push_back(e); }
    void clearEvents() { events_.clear(); }

    void broadcastChat(uint8_t id, const std::string& text) {
        if (id >= gc::NUM_PLAYERS || text.empty()) return;
        ChatLine c{ id, snap_.players[id].name, text };
        chatQueue_.push_back(c);
    }

    const Snapshot& snapshot() const { return snap_; }
    Snapshot& debugSnapshot() { return snap_; }          // test hook
    int invariantFixes() const { return invariantFixes_; }

    void update(float dt) {
        if (snap_.gameOver) return;   // result screen: nothing changes (the room returns to the lobby in Room::tick)
        refreshLive();
        const bool hunt = anyLoose();
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& p = snap_.players[i];
            if (!p.active) continue;
            p.shootCd -= dt; if (p.shootCd < 0) p.shootCd = 0;
            p.leapCdT -= dt; if (p.leapCdT < 0) p.leapCdT = 0;
            p.ammoTimer += dt * ((hunt && !p.carrying) ? 1.f / gc::HUNT_REGEN_MUL : 1.f);
            if (p.ammoTimer >= gc::AMMO_REGEN_TIME && p.ammo < gc::MAX_AMMO) {
                p.ammo++; p.ammoTimer = 0;
            } else if (p.ammo >= gc::MAX_AMMO) p.ammoTimer = 0;

            const bool wantShoot = p.shootQueued, wantLeap = p.leapQueued;
            p.shootQueued = p.leapQueued = false;

            if (p.leapPhase > 0) {
                advanceLeap(p, dt);                 // airborne: input ignored
            } else if (!p.connected) {
                p.moving = false; p.walkFrame = 0; p.walkTimer = 0;   // ghost: idle
            } else {
                if (wantLeap) tryLeap(p);
                if (p.leapPhase == 0) {
                    if (wantShoot) tryShoot(p);
                    applyMove(p, inputs_[i], dt);
                }
            }
            p.leapCd = (uint8_t)std::min(255.f, std::ceil(p.leapCdT * 10.f - 0.001f));
        }

        updateProjectiles(dt);
        updateTreasures(dt);
        checkInvariants(dt);
        updateMatchClock(dt);
    }

    // Heals corrupted state and logs; returns the number of repairs.
    int checkInvariants(float dt = 0.f) {
        int fixes = 0;
        auto fix = [&](const char* what, int idx) {
            ++fixes; ++invariantFixes_;
            std::cerr << "[rules] invariant repaired: " << what << " (#" << idx << ")\n";
        };
        const uint8_t N = gc::NUM_PLAYERS;
        for (uint8_t i = 0; i < N; ++i) {
            auto& p = snap_.players[i];
            if (p.carrying) {
                bool ok = p.active && p.carriedTreasure < N &&
                          snap_.treasures[p.carriedTreasure].state == (uint8_t)TState::Carried &&
                          snap_.treasures[p.carriedTreasure].carriedBy == i;
                if (!ok) { p.carrying = false; p.carriedTreasure = 255; fix("player carrying nothing valid", i); }
            } else if (p.carriedTreasure != 255) p.carriedTreasure = 255;
            if (p.facing < 0 || p.facing > 3) { p.facing = gc::FACE_DOWN; fix("bad facing", i); }
            if (p.ammo < 0 || p.ammo > gc::MAX_AMMO) { p.ammo = std::clamp(p.ammo, 0, (int32_t)gc::MAX_AMMO); fix("ammo range", i); }
        }
        for (uint8_t k = 0; k < N; ++k) {
            auto& t = snap_.treasures[k];
            t.id = k;
            if (t.state > (uint8_t)TState::Inactive) { setState(k, TState::AtHome); t.carriedBy = 255; t.x = t.homeX; t.y = t.homeY; fix("bad treasure state", k); }
            if (t.state == (uint8_t)TState::Carried) {
                bool ok = t.carriedBy < N && snap_.players[t.carriedBy].active && snap_.players[t.carriedBy].carrying &&
                          snap_.players[t.carriedBy].carriedTreasure == k;
                if (!ok) { t.carriedBy = 255; startReturn(k); fix("carried treasure without carrier", k); }
            } else if (t.carriedBy != 255) { t.carriedBy = 255; fix("carriedBy on non-carried treasure", k); }
            // age of the current state
            if (t.state != lastState_[k]) { lastState_[k] = t.state; stateAge_[k] = 0.f; }
            else stateAge_[k] += dt;
            if ((t.state == (uint8_t)TState::Dropped || t.state == (uint8_t)TState::Returning) && stateAge_[k] > gcserver::STALE_TREASURE_SECONDS) {
                setState(k, TState::AtHome); t.x = t.homeX; t.y = t.homeY; fix("stale treasure forced home", k);
            }
            if (t.state == (uint8_t)TState::AtHome || t.state == (uint8_t)TState::Inactive) {
                if (t.x != t.homeX || t.y != t.homeY) { t.x = t.homeX; t.y = t.homeY; fix("home treasure off its pedestal", k); }
            }
            t.present = t.state == (uint8_t)TState::AtHome;
        }
        refreshLive();
        return fixes;
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
    RoomSettings settings_;
    std::array<float, gc::NUM_PLAYERS>   retFromX_{}, retFromY_{}, stateAge_{};
    std::array<uint8_t, gc::NUM_PLAYERS> lastState_{};
    int invariantFixes_ = 0;

    static bool airborne(const PlayerState& p) { return p.leapPhase > 0; }
    static bool ghost(const PlayerState& p) { return !p.connected; }
    static void resetLeap(PlayerState& p) {
        p.leapT = 0; p.leapCdT = 0; p.leapCd = 0; p.leapPhase = 0;
    }

    void setState(uint8_t k, TState s) {
        snap_.treasures[k].state = (uint8_t)s;
        snap_.treasures[k].present = s == TState::AtHome;
        lastState_[k] = (uint8_t)s; stateAge_[k] = 0.f;
    }
    bool anyLoose() const {
        for (auto& t : snap_.treasures)
            if (t.state == (uint8_t)TState::Carried || t.state == (uint8_t)TState::Dropped) return true;
        return false;
    }

    // Live rule: treasure k is live iff slot k is active, or exactly one player is active (solo practice).
    void refreshLive() {
        const bool solo = playerCount() == 1;
        for (uint8_t k = 0; k < gc::NUM_PLAYERS; ++k) {
            auto& t = snap_.treasures[k];
            bool live = snap_.players[k].active || solo;
            if (live && t.state == (uint8_t)TState::Inactive) {
                setState(k, TState::AtHome); t.x = t.homeX; t.y = t.homeY; t.carriedBy = 255;
            } else if (!live && t.state != (uint8_t)TState::Inactive) {
                if (t.carriedBy < gc::NUM_PLAYERS) {
                    auto& c = snap_.players[t.carriedBy];
                    if (c.carrying && c.carriedTreasure == k) { c.carrying = false; c.carriedTreasure = 255; }
                }
                setState(k, TState::Inactive); t.carriedBy = 255; t.x = t.homeX; t.y = t.homeY;
            }
        }
    }

    void resetMatch(bool firstTime) {
        snap_.matchTime = settings_.minutes * 60.f;
        snap_.matchLen = snap_.matchTime;
        snap_.winScore = settings_.winScore;
        snap_.fog = settings_.fog;
        snap_.gameOver = false;
        snap_.overtime = false;
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
            p.moving = false; p.walkFrame = 0; p.walkTimer = 0;
            p.shootQueued = p.leapQueued = false;
            resetLeap(p);
            inputs_[i] = ClientInput{};
        }
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            auto& t = snap_.treasures[i];
            t.id = i; t.baseOwner = i;
            t.homeX = gcmap::bases()[i].treasureX;
            t.homeY = gcmap::bases()[i].treasureY;
            t.x = t.homeX; t.y = t.homeY;
            t.carriedBy = 255; t.timer = 0;
            setState(i, TState::AtHome);
        }
        refreshLive();
        snap_.projectileCount = 0;
    }

    void applyMove(PlayerState& p, const ClientInput& in, float dt) {
        float dx = in.dirX, dy = in.dirY;
        float len = std::sqrt(dx * dx + dy * dy);
        bool moving = len > 0.001f;
        if (moving) { dx /= len; dy /= len; p.facing = in.facing; }
        p.moving = moving;
        const float step = gc::PLAYER_SPEED * dt * (p.carrying ? gc::CARRIER_SPEED_MUL : 1.f);
        const float o = gcmap::boxOffset();
        float nx = p.x + dx * step;
        if (!gcmap::solidBox(nx + o, p.y + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) p.x = nx;
        float ny = p.y + dy * step;
        if (!gcmap::solidBox(p.x + o, ny + o, gc::PLAYER_BOX, gc::PLAYER_BOX)) p.y = ny;
        if (moving) { p.walkTimer += dt; p.walkFrame = static_cast<int>(p.walkTimer * 9.f) % 4; }
        else { p.walkTimer = 0; p.walkFrame = 0; }
    }

    void tryShoot(PlayerState& p) {
        if (p.carrying && !gc::CARRIER_CAN_SHOOT) return;   // hands full
        if (p.shootCd > 0 || p.ammo <= 0 || snap_.projectileCount >= gc::MAX_PROJECTILES) return;
        p.shootCd = gc::SHOOT_COOLDOWN;
        p.ammo--;
        ProjectileState pr;
        pr.id = nextProjId_++;
        pr.ownerId = p.id;
        pr.x = p.x + gc::TILE * 0.5f + gcserver::DIRVX[p.facing] * 12.f;
        pr.y = p.y + gc::TILE * 0.5f + gcserver::DIRVY[p.facing] * 12.f;
        pr.vx = gcserver::DIRVX[p.facing] * gc::PROJECTILE_SPEED;
        pr.vy = gcserver::DIRVY[p.facing] * gc::PROJECTILE_SPEED;
        pr.dir = p.facing; pr.frame = 0;
        snap_.projectiles[snap_.projectileCount++] = pr;
        pushEvent({ EvType::Shoot, p.id, 255, 255 });
    }

    // ---- leap ----
    void tryLeap(PlayerState& p) {
        if (!p.connected || p.leapPhase > 0 || p.leapCdT > 0) return;
        const float maxD = p.carrying ? gc::LEAP_DIST_CARRIER : gc::LEAP_DIST;
        float d = leapLandingFn(p.x, p.y, p.facing, maxD);
        if (!(d > 0.f)) return;   // cannot land: no cooldown spent
        p.leapFromX = p.x; p.leapFromY = p.y;
        p.leapToX = p.x + gcserver::DIRVX[p.facing] * d;
        p.leapToY = p.y + gcserver::DIRVY[p.facing] * d;
        p.leapT = 0; p.leapPhase = 1;
        p.leapCdT = p.carrying ? gc::LEAP_CD_CARRIER : gc::LEAP_CD;
        p.moving = false; p.walkFrame = 0;
        pushEvent({ EvType::Leap, p.id, 255, 255 });
    }
    void advanceLeap(PlayerState& p, float dt) {
        p.leapT += dt;
        float u = p.leapT / gc::LEAP_TIME;
        p.moving = false;
        if (u >= 1.f) {
            p.x = p.leapToX; p.y = p.leapToY;
            p.leapT = 0; p.leapPhase = 0;
            pushEvent({ EvType::Land, p.id, 255, 255 });
        } else {
            p.x = p.leapFromX + (p.leapToX - p.leapFromX) * u;
            p.y = p.leapFromY + (p.leapToY - p.leapFromY) * u;
            p.leapPhase = (uint8_t)std::clamp(1 + (int)(u * 254.f), 1, 255);
        }
    }

    // ---- projectiles ----
    void updateProjectiles(float dt) {
        int& n = snap_.projectileCount;
        for (int i = 0; i < n; ) {
            auto& pr = snap_.projectiles[i];
            pr.x += pr.vx * dt;
            pr.y += pr.vy * dt;
            pr.frame = (pr.frame + 1) & 3;
            bool remove = false;
            if (pr.x < 0 || pr.y < 0 || pr.x > gc::MAP_W * gc::TILE || pr.y > gc::MAP_H * gc::TILE)
                remove = true;
            if (!remove && gcmap::solidBox(pr.x - gc::PROJECTILE_R, pr.y - gc::PROJECTILE_R,
                                           gc::PROJECTILE_R * 2, gc::PROJECTILE_R * 2))
                remove = true;
            if (!remove) {
                for (uint8_t j = 0; j < gc::NUM_PLAYERS; ++j) {
                    auto& pl = snap_.players[j];
                    if (!pl.active || j == pr.ownerId || ghost(pl) || airborne(pl)) continue;
                    float dx = (pl.x + 8) - pr.x;
                    float dy = (pl.y + 8) - pr.y;
                    float r = (pl.carrying ? gc::CARRIER_HIT_R : gc::PLAYER_HIT_R) + gc::PROJECTILE_R;
                    if (dx * dx + dy * dy < r * r) {
                        if (pl.carrying) dropTreasure(pl, pr.ownerId);
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

    // The carrier's trophy falls where the carrier stands.
    void dropTreasure(PlayerState& p, uint8_t shooter) {
        if (!p.carrying || p.carriedTreasure >= gc::NUM_PLAYERS) { p.carrying = false; p.carriedTreasure = 255; return; }
        uint8_t k = p.carriedTreasure;
        auto& t = snap_.treasures[k];
        setState(k, TState::Dropped);
        t.x = p.x + 8; t.y = p.y + 8; t.carriedBy = 255; t.timer = gc::DROP_RETURN_TIME;
        p.carrying = false; p.carriedTreasure = 255;
        pushEvent({ EvType::Drop, p.id, shooter, k });
    }

    void startReturn(uint8_t k) {
        auto& t = snap_.treasures[k];
        retFromX_[k] = t.x; retFromY_[k] = t.y;
        setState(k, TState::Returning);
        t.carriedBy = 255; t.timer = gc::RETURN_TRAVEL;
        // detach any carrier that still points at it
        for (auto& p : snap_.players) if (p.carrying && p.carriedTreasure == k) { p.carrying = false; p.carriedTreasure = 255; }
    }

    void teleportHome(PlayerState& p) {
        p.x = gcmap::bases()[p.id].spawnX;
        p.y = gcmap::bases()[p.id].spawnY;
        p.moving = false; p.walkFrame = 0;
    }

    void updateTreasures(float dt) {
        // 1. timers and positions
        for (uint8_t k = 0; k < gc::NUM_PLAYERS; ++k) {
            auto& t = snap_.treasures[k];
            switch ((TState)t.state) {
                case TState::Carried:
                    if (t.carriedBy < gc::NUM_PLAYERS) { t.x = snap_.players[t.carriedBy].x + 8; t.y = snap_.players[t.carriedBy].y + 8; }
                    break;
                case TState::Dropped:
                    t.timer -= dt;
                    if (t.timer <= 0) startReturn(k);
                    break;
                case TState::Returning: {
                    t.timer -= dt;
                    if (t.timer <= 0) {
                        setState(k, TState::AtHome); t.x = t.homeX; t.y = t.homeY; t.timer = 0;
                        pushEvent({ EvType::Returned, 255, 255, k });
                    } else {
                        float u = 1.f - t.timer / gc::RETURN_TRAVEL;
                        t.x = retFromX_[k] + (t.homeX - retFromX_[k]) * u;
                        t.y = retFromY_[k] + (t.homeY - retFromY_[k]) * u;
                    }
                    break;
                }
                default:
                    t.x = t.homeX; t.y = t.homeY; break;
            }
        }
        // 2. players in id order (first one in range wins)
        for (uint8_t i = 0; i < gc::NUM_PLAYERS && !snap_.gameOver; ++i) {
            auto& p = snap_.players[i];
            if (!p.active || ghost(p) || airborne(p)) continue;
            float pcx = p.x + 8, pcy = p.y + 8;
            if (p.carrying) {
                auto& home = gcmap::bases()[i];
                float dx = pcx - home.treasureX, dy = pcy - home.treasureY;
                if (dx * dx + dy * dy < gc::DELIVER_R * gc::DELIVER_R && p.carriedTreasure < gc::NUM_PLAYERS) {
                    uint8_t k = p.carriedTreasure;
                    auto& t = snap_.treasures[k];
                    p.score++;
                    setState(k, TState::AtHome); t.x = t.homeX; t.y = t.homeY; t.carriedBy = 255;
                    p.carrying = false; p.carriedTreasure = 255;
                    pushEvent({ EvType::Deliver, p.id, 255, k });
                    if (p.score >= settings_.winScore || snap_.overtime) {
                        snap_.gameOver = true;
                        snap_.winner = p.id;
                        snap_.matchTime = 0;
                        pushEvent({ EvType::GameOver, p.id, 255, 255 });
                    }
                }
                continue;
            }
            for (uint8_t k = 0; k < gc::NUM_PLAYERS; ++k) {
                auto& t = snap_.treasures[k];
                float dx = pcx - t.x, dy = pcy - t.y;
                float d2 = dx * dx + dy * dy;
                bool mine = t.baseOwner == static_cast<int>(p.id);
                if (t.state == (uint8_t)TState::AtHome) {
                    if (mine || d2 >= gc::PICKUP_R * gc::PICKUP_R) continue;
                    grab(p, k);
                    break;
                }
                if (t.state == (uint8_t)TState::Dropped && t.timer <= gc::DROP_RETURN_TIME - gc::DROP_LOCK) {
                    if (mine) {
                        if (d2 < gc::RECOVER_R * gc::RECOVER_R) {
                            setState(k, TState::AtHome); t.x = t.homeX; t.y = t.homeY; t.timer = 0;
                            pushEvent({ EvType::Recover, p.id, 255, k });
                            break;
                        }
                    } else if (d2 < gc::PICKUP_R * gc::PICKUP_R) {
                        grab(p, k);
                        break;
                    }
                }
            }
        }
    }

    void grab(PlayerState& p, uint8_t k) {
        auto& t = snap_.treasures[k];
        setState(k, TState::Carried);
        t.carriedBy = p.id; t.timer = 0; t.x = p.x + 8; t.y = p.y + 8;
        p.carrying = true; p.carriedTreasure = k;
        pushEvent({ EvType::Steal, p.id, static_cast<uint8_t>(t.baseOwner), k });
    }

    void updateMatchClock(float dt) {
        if (snap_.gameOver) return;
        snap_.matchTime -= dt;
        if (snap_.matchTime > 0) return;
        snap_.matchTime = 0;
        int best = 0, bestId = 255, nBest = 0;
        for (uint8_t i = 0; i < gc::NUM_PLAYERS; ++i) {
            int s = snap_.players[i].score;
            if (s > best) { best = s; bestId = i; nBest = 1; }
            else if (s == best && s > 0) ++nBest;
        }
        if (nBest > 1 && !snap_.overtime && gc::OVERTIME_SECONDS > 0) {
            snap_.overtime = true;                 // sudden death: the first delivery wins
            snap_.matchTime = gc::OVERTIME_SECONDS;
            return;
        }
        snap_.gameOver = true;
        snap_.winner = nBest > 1 ? 255 : static_cast<uint8_t>(bestId);
        pushEvent({ EvType::GameOver, snap_.winner, 255, 255 });
    }
};
