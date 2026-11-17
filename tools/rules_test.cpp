// Headless rules test for the server simulation (src/server/GameServer.hpp): treasure state machine, carriers, drop/recover/return,
// live-treasure rules, invariants, input latching and leap. No window, no audio, no sockets.
//
// Build (from the repo root, MSYS2 UCRT64):
//   export PATH=/c/msys64/ucrt64/bin:$PATH
//   g++ -std=c++17 -O1 -DSFML_STATIC -Isrc -Ibuild/_deps/sfml-src/include tools/rules_test.cpp -o <scratch>/rules_test.exe
//       -Lbuild/_deps/sfml-build/lib -lsfml-network-s -lsfml-system-s -lws2_32 -static
// Run:  <scratch>/rules_test.exe        (exit code 0 = all passed)
// leapLanding is replaced by a fake through GameServer::leapLandingFn, so these tests do not depend on the generated map's gaps.
#include "../src/server/GameServer.hpp"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
static void section(const char* n) { std::printf("[%s]\n", n); }

static const float DT = 1.f / 30.f;
static std::vector<GameEvent> g_ev;   // all events since the last clearEv()

static void clearEv() { g_ev.clear(); }
static int countEv(EvType t) { int n = 0; for (auto& e : g_ev) if (e.type == t) ++n; return n; }
static const GameEvent* findEv(EvType t) { for (auto& e : g_ev) if (e.type == t) return &e; return nullptr; }

static void drain(GameServer& g) { std::vector<GameEvent> e; if (g.consumeEvents(e)) g_ev.insert(g_ev.end(), e.begin(), e.end()); }
static void step(GameServer& g, float secs) {
    int n = (int)std::lround(secs / DT);
    for (int i = 0; i < n; ++i) {
        g.update(DT);
        std::vector<GameEvent> e; if (g.consumeEvents(e)) g_ev.insert(g_ev.end(), e.begin(), e.end());
    }
}

static GameServer* make(int players, int winScore = 3, int minutes = 3) {
    auto* g = new GameServer();
    for (int i = 0; i < players; ++i) { uint8_t id = g->assignHuman(); g->setName(id, "P" + std::to_string(i)); }
    RoomSettings rs; rs.winScore = (uint8_t)winScore; rs.minutes = (uint8_t)minutes;
    g->startMatch(rs);
    clearEv();
    return g;
}
static PlayerState& P(GameServer& g, int i) { return g.debugSnapshot().players[i]; }
static TreasureState& T(GameServer& g, int i) { return g.debugSnapshot().treasures[i]; }
static TState st(GameServer& g, int k) { return (TState)g.snapshot().treasures[k].state; }
static void placeCentre(GameServer& g, int i, float cx, float cy) { P(g, i).x = cx - 8; P(g, i).y = cy - 8; }
static void atTreasure(GameServer& g, int i, int k) { placeCentre(g, i, gcmap::bases()[k].treasureX, gcmap::bases()[k].treasureY); }
static void atSpawn(GameServer& g, int i, int base) { P(g, i).x = gcmap::bases()[base].spawnX; P(g, i).y = gcmap::bases()[base].spawnY; }
static void park(GameServer& g) { for (int i = 0; i < 4; ++i) atSpawn(g, i, i); }
static void injectShot(GameServer& g, int owner, float cx, float cy) {
    auto& s = g.debugSnapshot();
    ProjectileState pr; pr.id = 999; pr.ownerId = (uint8_t)owner; pr.x = cx; pr.y = cy; pr.vx = pr.vy = 0;
    s.projectiles[s.projectileCount++] = pr;
}
static float cx(GameServer& g, int i) { return g.snapshot().players[i].x + 8; }
static float cy(GameServer& g, int i) { return g.snapshot().players[i].y + 8; }

// ---- fake leap landing ----
static float g_fakeD = 40.f, g_lastMax = 0.f; static int g_lastDir = -1;
static float fakeLanding(float, float, int dir, float maxD) { g_lastMax = maxD; g_lastDir = dir; return std::min(g_fakeD, maxD); }
static float noLanding(float, float, int, float) { return 0.f; }

// Make player i carry treasure k at the player's current position (test shortcut that keeps state consistent).
static void forceCarry(GameServer& g, int i, int k) {
    auto& p = P(g, i); auto& t = T(g, k);
    p.carrying = true; p.carriedTreasure = (uint8_t)k;
    t.state = (uint8_t)TState::Carried; t.carriedBy = (uint8_t)i; t.present = false;
}

int main() {
    gcmap::generate();

    section("steal + deliver");
    {
        auto* g = make(4); park(*g);
        for (int k = 0; k < 4; ++k) CHECK(st(*g, k) == TState::AtHome && g->snapshot().treasures[k].present);
        atTreasure(*g, 0, 1); step(*g, DT);
        CHECK(st(*g, 1) == TState::Carried && P(*g, 0).carrying && P(*g, 0).carriedTreasure == 1 && T(*g, 1).carriedBy == 0);
        CHECK(!g->snapshot().treasures[1].present);
        const GameEvent* e = findEv(EvType::Steal); CHECK(e && e->a == 0 && e->b == 1 && e->c == 1);
        placeCentre(*g, 0, cx(*g, 0) + 3, cy(*g, 0)); step(*g, DT);
        CHECK(std::fabs(T(*g, 1).x - cx(*g, 0)) < 0.01f && std::fabs(T(*g, 1).y - cy(*g, 0)) < 0.01f);   // x,y follow the carrier
        atTreasure(*g, 0, 0); step(*g, DT);
        CHECK(P(*g, 0).score == 1 && !P(*g, 0).carrying && st(*g, 1) == TState::AtHome && g->snapshot().treasures[1].present);
        CHECK(T(*g, 1).x == T(*g, 1).homeX && countEv(EvType::Deliver) == 1);
        // own treasure at home cannot be "stolen" by its owner
        atTreasure(*g, 1, 1); step(*g, DT);
        CHECK(!P(*g, 1).carrying && st(*g, 1) == TState::AtHome);
        // delivery works even while own treasure is stolen
        park(*g); atTreasure(*g, 2, 0); step(*g, DT);      // p2 steals p0's treasure
        CHECK(st(*g, 0) == TState::Carried);
        atTreasure(*g, 0, 3); step(*g, DT);                // p0 steals 3
        atTreasure(*g, 0, 0); step(*g, DT);                // ... and delivers at own (empty) pedestal
        CHECK(P(*g, 0).score == 2);
        delete g;
    }

    section("hit drops the trophy, lock, pickup, victim teleports");
    {
        auto* g = make(4); park(*g);
        atSpawn(*g, 0, 2); atSpawn(*g, 2, 3);              // victim carries t1 at base2's spawn; p2 parked away
        forceCarry(*g, 0, 1);
        float hx = cx(*g, 0), hy = cy(*g, 0);
        injectShot(*g, 3, hx, hy); clearEv(); step(*g, DT);
        CHECK(st(*g, 1) == TState::Dropped && !P(*g, 0).carrying);
        CHECK(std::fabs(T(*g, 1).x - hx) < 0.01f && std::fabs(T(*g, 1).y - hy) < 0.01f);
        CHECK(P(*g, 0).x == gcmap::bases()[0].spawnX && P(*g, 0).y == gcmap::bases()[0].spawnY);   // still teleported home
        const GameEvent* d = findEv(EvType::Drop); CHECK(d && d->a == 0 && d->b == 3 && d->c == 1);
        CHECK(countEv(EvType::Teleport) == 1 && g->snapshot().projectileCount == 0);
        CHECK(T(*g, 1).timer > gc::DROP_RETURN_TIME - 0.2f);
        // locked: a thief standing on it gets nothing for ~0.5 s
        placeCentre(*g, 3, hx, hy); step(*g, 0.3f);
        CHECK(st(*g, 1) == TState::Dropped && !P(*g, 3).carrying);
        step(*g, 0.4f);
        CHECK(st(*g, 1) == TState::Carried && P(*g, 3).carriedTreasure == 1);
        CHECK(countEv(EvType::Steal) == 1);
        delete g;
    }

    section("owner recovers a dropped trophy (no score)");
    {
        auto* g = make(4); park(*g);
        atSpawn(*g, 0, 2); atSpawn(*g, 2, 3); forceCarry(*g, 0, 1);
        float hx = cx(*g, 0), hy = cy(*g, 0);
        injectShot(*g, 3, hx, hy); step(*g, DT); clearEv();
        placeCentre(*g, 1, hx + 10, hy); step(*g, 0.7f);   // p1 owns treasure 1
        CHECK(st(*g, 1) == TState::AtHome && g->snapshot().treasures[1].present);
        const GameEvent* e = findEv(EvType::Recover); CHECK(e && e->a == 1 && e->c == 1);
        CHECK(P(*g, 1).score == 0 && !P(*g, 1).carrying);
        CHECK(T(*g, 1).x == T(*g, 1).homeX);
        delete g;
    }

    section("dropped trophy auto-returns after 12 s + 1 s flight");
    {
        auto* g = make(4); park(*g);
        atSpawn(*g, 0, 2); atSpawn(*g, 2, 3); forceCarry(*g, 0, 1);
        injectShot(*g, 3, cx(*g, 0), cy(*g, 0)); step(*g, DT); clearEv();
        float sx = T(*g, 1).x;
        step(*g, 11.5f); CHECK(st(*g, 1) == TState::Dropped);
        step(*g, 1.0f);  CHECK(st(*g, 1) == TState::Returning);
        // untouchable while flying
        placeCentre(*g, 3, T(*g, 1).x, T(*g, 1).y); step(*g, DT);
        CHECK(!P(*g, 3).carrying);
        park(*g);
        step(*g, 0.4f);
        CHECK(st(*g, 1) == TState::Returning && T(*g, 1).x != sx);   // moving toward home
        step(*g, 0.8f);
        CHECK(st(*g, 1) == TState::AtHome && g->snapshot().treasures[1].present);
        CHECK(countEv(EvType::Returned) == 1 && findEv(EvType::Returned)->c == 1);
        CHECK(T(*g, 1).x == T(*g, 1).homeX && T(*g, 1).y == T(*g, 1).homeY);
        delete g;
    }

    section("leave / kick: carried -> Returning");
    {
        auto* g = make(4); park(*g);
        atTreasure(*g, 0, 1); step(*g, DT);
        CHECK(P(*g, 0).carrying);
        g->releaseSlot(0);
        CHECK(st(*g, 1) == TState::Returning && !P(*g, 0).active && !P(*g, 0).carrying);
        step(*g, 1.1f);
        CHECK(st(*g, 1) == TState::AtHome && countEv(EvType::Returned) == 1);
        CHECK(st(*g, 0) == TState::Inactive);   // 3 players left: empty base shows nothing
        delete g;
    }

    section("connection loss: Carried -> Dropped in place; ghosts are inert");
    {
        auto* g = make(4); park(*g);
        atSpawn(*g, 0, 2); atSpawn(*g, 2, 3); forceCarry(*g, 0, 1);
        float hx = cx(*g, 0), hy = cy(*g, 0);
        clearEv(); g->setConnected(0, false); drain(*g);
        CHECK(st(*g, 1) == TState::Dropped && !P(*g, 0).carrying);
        CHECK(std::fabs(T(*g, 1).x - hx) < 0.01f && std::fabs(T(*g, 1).y - hy) < 0.01f);
        const GameEvent* d = findEv(EvType::Drop); CHECK(d && d->a == 0 && d->c == 1);
        // ghost cannot pick it back up after the lock
        step(*g, 1.0f);
        CHECK(st(*g, 1) == TState::Dropped && !P(*g, 0).carrying);
        // ghost cannot steal or be hit
        atTreasure(*g, 0, 3); step(*g, 0.2f);
        CHECK(!P(*g, 0).carrying && st(*g, 3) == TState::AtHome);
        clearEv(); injectShot(*g, 1, cx(*g, 0), cy(*g, 0)); step(*g, DT);
        CHECK(countEv(EvType::Teleport) == 0 && g->snapshot().projectileCount == 1);
        // reconnect: normal again
        g->debugSnapshot().projectileCount = 0;
        g->setConnected(0, true); step(*g, DT);
        CHECK(P(*g, 0).carrying);   // standing on treasure 3
        delete g;
        // ghost cannot deliver
        g = make(4); park(*g);
        atTreasure(*g, 0, 1); step(*g, DT); g->setConnected(0, false);   // drops in place
        CHECK(st(*g, 1) == TState::Dropped);
        delete g;
        g = make(4); park(*g); atTreasure(*g, 0, 1); step(*g, DT);
        P(*g, 0).connected = false;                                       // raw flag: carrying ghost at home
        atTreasure(*g, 0, 0); step(*g, DT);
        CHECK(P(*g, 0).score == 0 && P(*g, 0).carrying);
        delete g;
    }

    section("live-treasure rules");
    {
        auto* g = make(1);
        for (int k = 0; k < 4; ++k) CHECK(st(*g, k) == TState::AtHome);     // solo practice: neutrals live
        delete g;
        g = make(2);
        CHECK(st(*g, 0) == TState::AtHome && st(*g, 1) == TState::AtHome && st(*g, 2) == TState::Inactive && st(*g, 3) == TState::Inactive);
        CHECK(!g->snapshot().treasures[2].present);
        // an inactive treasure cannot be stolen
        park(*g); atTreasure(*g, 0, 2); step(*g, 0.2f);
        CHECK(!P(*g, 0).carrying);
        delete g;
        g = make(3);
        CHECK(st(*g, 2) == TState::AtHome && st(*g, 3) == TState::Inactive);
        delete g;
        // solo win reachable: steal a neutral treasure and bring it home
        g = make(1, 1); park(*g);
        atTreasure(*g, 0, 2); step(*g, DT); CHECK(P(*g, 0).carrying);
        atTreasure(*g, 0, 0); step(*g, DT);
        CHECK(g->snapshot().gameOver && g->snapshot().winner == 0 && P(*g, 0).score == 1);
        CHECK(countEv(EvType::GameOver) == 1);
        delete g;
    }

    section("mid-match leave");
    {
        auto* g = make(4); park(*g);
        g->releaseSlot(3);
        CHECK(st(*g, 3) == TState::Inactive && st(*g, 2) == TState::AtHome);
        // carrier of a treasure whose owner leaves: it is cleared
        atTreasure(*g, 0, 2); step(*g, DT); CHECK(P(*g, 0).carrying);
        g->releaseSlot(2);
        CHECK(!P(*g, 0).carrying && st(*g, 2) == TState::Inactive && g->checkInvariants() == 0);
        // down to a single player: neutrals become live
        g->releaseSlot(1);
        CHECK(st(*g, 1) == TState::AtHome && st(*g, 2) == TState::AtHome && st(*g, 3) == TState::AtHome && st(*g, 0) == TState::AtHome);
        delete g;
        // a dropped trophy of a leaving owner is cleared
        g = make(4); park(*g);
        atSpawn(*g, 0, 2); atSpawn(*g, 2, 3); forceCarry(*g, 0, 1);
        g->setConnected(0, false);
        CHECK(st(*g, 1) == TState::Dropped);
        g->releaseSlot(1);
        CHECK(st(*g, 1) == TState::Inactive);
        delete g;
    }

    section("invariants heal and log");
    {
        auto* g = make(4); park(*g);
        // treasure says Carried by nobody
        T(*g, 2).state = (uint8_t)TState::Carried; T(*g, 2).carriedBy = 255;
        int f = g->checkInvariants();
        CHECK(f >= 1 && st(*g, 2) == TState::Returning);
        step(*g, 1.2f); CHECK(st(*g, 2) == TState::AtHome);
        // player carrying a treasure that is at home
        P(*g, 1).carrying = true; P(*g, 1).carriedTreasure = 3;
        CHECK(g->checkInvariants() >= 1 && !P(*g, 1).carrying && P(*g, 1).carriedTreasure == 255);
        // two claims on one treasure: only the real carrier keeps it
        atTreasure(*g, 0, 1); step(*g, DT);
        P(*g, 2).carrying = true; P(*g, 2).carriedTreasure = 1;
        CHECK(g->checkInvariants() >= 1 && P(*g, 0).carrying && !P(*g, 2).carrying);
        // present flag out of sync
        T(*g, 3).present = false;
        g->checkInvariants(); CHECK(g->snapshot().treasures[3].present);
        // carried by an inactive player
        park(*g);
        g->debugSnapshot().players[3].active = false;
        T(*g, 0).state = (uint8_t)TState::Carried; T(*g, 0).carriedBy = 3;
        CHECK(g->checkInvariants() >= 1);
        // stuck Dropped state (timer corrupted) is forced home after 30 s
        auto* h = make(4); park(*h);
        T(*h, 1).state = (uint8_t)TState::Dropped; T(*h, 1).timer = 1e9f; T(*h, 1).x = 400; T(*h, 1).y = 300;
        step(*h, 31.f);
        CHECK(st(*h, 1) == TState::AtHome && h->invariantFixes() >= 1);
        // bogus state value
        T(*h, 2).state = 77;
        CHECK(h->checkInvariants() >= 1 && st(*h, 2) == TState::AtHome);
        delete h; delete g;
    }

    section("nothing changes after gameOver");
    {
        auto* g = make(4, 1); park(*g);
        atTreasure(*g, 0, 1); step(*g, DT); atTreasure(*g, 0, 0); step(*g, DT);
        CHECK(g->snapshot().gameOver && g->snapshot().winner == 0);
        Snapshot before = g->snapshot();
        // try to disturb: steal, shoot, kill clock
        atTreasure(*g, 1, 3);
        ClientInput in; in.shoot = true; in.dirX = 1; in.facing = gc::FACE_RIGHT; g->setInput(2, in);
        clearEv(); step(*g, 3.f);
        const auto& a = g->snapshot();
        CHECK(a.matchTime == before.matchTime && a.winner == before.winner);
        bool same = true;
        for (int i = 0; i < 4; ++i) {
            same = same && a.players[i].score == before.players[i].score && a.players[i].carrying == before.players[i].carrying;
            same = same && a.treasures[i].state == before.treasures[i].state && a.players[i].ammo == before.players[i].ammo;
        }
        CHECK(same);
        CHECK(g_ev.empty());
        delete g;
    }

    section("same-tick double pickup");
    {
        auto* g = make(4); park(*g);
        atTreasure(*g, 0, 2); atTreasure(*g, 1, 2);
        step(*g, DT);
        CHECK(P(*g, 0).carrying && !P(*g, 1).carrying && countEv(EvType::Steal) == 1 && T(*g, 2).carriedBy == 0);
        // and one player gets only one trophy even with two in reach
        park(*g);
        delete g;
    }

    section("input edge latching");
    {
        auto* g = make(4); park(*g);
        ClientInput a; a.shoot = true; a.facing = gc::FACE_RIGHT;
        ClientInput b; b.facing = gc::FACE_RIGHT;
        int ammo0 = P(*g, 0).ammo;
        g->setInput(0, a); g->setInput(0, b);            // two packets in one tick: the press must survive
        clearEv(); step(*g, DT);
        CHECK(countEv(EvType::Shoot) == 1 && P(*g, 0).ammo == ammo0 - 1);
        step(*g, 0.6f); CHECK(countEv(EvType::Shoot) == 1);   // consumed once, not repeated
        g->leapLandingFn = &fakeLanding; g_fakeD = 40;
        ClientInput l; l.leap = true; l.facing = gc::FACE_RIGHT;
        g->setInput(1, l); g->setInput(1, b);
        clearEv(); step(*g, DT);
        CHECK(countEv(EvType::Leap) == 1);
        delete g;
    }

    section("carrier: speed, no shooting, hit radius, hunt regen");
    {
        auto* g = make(4); park(*g);
        ClientInput mv; mv.dirX = 1; mv.facing = gc::FACE_RIGHT;
        // free player vs carrier, same start
        float x0 = P(*g, 3).x; g->setInput(3, mv); step(*g, 0.2f); float dFree = P(*g, 3).x - x0;
        P(*g, 3).x = x0; g->clearInput(3);
        forceCarry(*g, 3, 1); g->setInput(3, mv); step(*g, 0.2f); float dCar = P(*g, 3).x - x0;
        CHECK(dFree > 5.f && std::fabs(dCar / dFree - gc::CARRIER_SPEED_MUL) < 0.03f);
        // no shooting while carrying
        int ammo = P(*g, 3).ammo; ClientInput sh; sh.shoot = true; sh.facing = gc::FACE_RIGHT; g->setInput(3, sh);
        clearEv(); step(*g, 0.2f);
        CHECK(countEv(EvType::Shoot) == 0 && P(*g, 3).ammo == ammo);
        delete g;
        // hit radius: dx = 11 misses a normal player (6+4) but hits a carrier (8+4)
        g = make(4); park(*g); atSpawn(*g, 0, 2); atSpawn(*g, 2, 3);
        injectShot(*g, 1, cx(*g, 0) + 11, cy(*g, 0)); clearEv(); step(*g, DT);
        CHECK(countEv(EvType::Teleport) == 0);
        g->debugSnapshot().projectileCount = 0;
        forceCarry(*g, 0, 1);
        injectShot(*g, 3, cx(*g, 0) + 11, cy(*g, 0)); step(*g, DT);
        CHECK(countEv(EvType::Teleport) == 1 && countEv(EvType::Drop) == 1);
        delete g;
        // hunt ammo regen: twice as fast for non-carriers while a trophy is loose
        g = make(4); park(*g);
        P(*g, 0).ammo = 0; P(*g, 0).ammoTimer = 0; step(*g, 1.6f); CHECK(P(*g, 0).ammo == 0);
        step(*g, 1.5f); CHECK(P(*g, 0).ammo == 1);          // normal: 3 s
        delete g;
        g = make(4); park(*g);
        atSpawn(*g, 1, 2); atSpawn(*g, 2, 3); forceCarry(*g, 1, 0);
        P(*g, 0).ammo = 0; P(*g, 0).ammoTimer = 0; P(*g, 1).ammo = 0; P(*g, 1).ammoTimer = 0;
        step(*g, 1.6f);
        CHECK(P(*g, 0).ammo == 1);                           // hunted: 1.5 s
        CHECK(P(*g, 1).ammo == 0);                           // the carrier regains at the normal rate
        delete g;
    }

    section("leap: start, arc, cooldown, immunity, no landing, carrier");
    {
        auto* g = make(4); park(*g);
        g->leapLandingFn = &fakeLanding; g_fakeD = 40;
        P(*g, 0).facing = gc::FACE_RIGHT;
        float x0 = P(*g, 0).x;
        ClientInput l; l.leap = true; l.facing = gc::FACE_RIGHT;
        clearEv(); g->setInput(0, l); step(*g, DT);
        CHECK(countEv(EvType::Leap) == 1 && P(*g, 0).leapPhase > 0 && g_lastDir == gc::FACE_RIGHT && g_lastMax == gc::LEAP_DIST);
        CHECK(P(*g, 0).leapCd == 35);
        // mid-air: immune to shots, cannot pick up, input ignored
        step(*g, 0.15f);
        CHECK(P(*g, 0).leapPhase > 0 && P(*g, 0).x > x0 + 5 && P(*g, 0).x < x0 + 40);
        injectShot(*g, 1, cx(*g, 0), cy(*g, 0)); step(*g, DT);
        CHECK(countEv(EvType::Teleport) == 0);
        g->debugSnapshot().projectileCount = 0;
        atTreasure(*g, 3, 3);   // unrelated; and put airborne p0 on a treasure
        P(*g, 0).leapToX = P(*g, 0).x; P(*g, 0).leapFromX = P(*g, 0).x;   // keep it hovering over spot (test only)
        placeCentre(*g, 0, gcmap::bases()[2].treasureX, gcmap::bases()[2].treasureY);
        P(*g, 0).leapToX = P(*g, 0).x; P(*g, 0).leapToY = P(*g, 0).y; P(*g, 0).leapFromX = P(*g, 0).x; P(*g, 0).leapFromY = P(*g, 0).y;
        step(*g, 0.1f);
        CHECK(!P(*g, 0).carrying && P(*g, 0).leapPhase > 0);
        step(*g, 0.3f);
        CHECK(P(*g, 0).leapPhase == 0 && countEv(EvType::Land) == 1);
        step(*g, DT); CHECK(P(*g, 0).carrying);   // lands on it -> now it can pick up
        delete g;

        g = make(4); park(*g);
        g->leapLandingFn = &fakeLanding; g_fakeD = 40;
        x0 = P(*g, 0).x; P(*g, 0).facing = gc::FACE_RIGHT;
        g->setInput(0, l); clearEv(); step(*g, 0.4f);
        CHECK(countEv(EvType::Land) == 1 && std::fabs(P(*g, 0).x - (x0 + 40)) < 0.01f && std::fabs(P(*g, 0).y - gcmap::bases()[0].spawnY) < 0.01f);
        // cooldown: a second leap right away does nothing, after 3.5 s it works
        clearEv(); g->setInput(0, l); step(*g, DT); CHECK(countEv(EvType::Leap) == 0);
        step(*g, 3.4f); CHECK(P(*g, 0).leapCd == 0);
        g->setInput(0, l); step(*g, DT); CHECK(countEv(EvType::Leap) == 1);
        // cooldown reported in tenths and decreasing
        step(*g, 0.5f + DT); CHECK(P(*g, 0).leapCd > 0 && P(*g, 0).leapCd < 35);
        delete g;

        // cannot land: no cooldown spent, no event
        g = make(4); park(*g);
        g->leapLandingFn = &noLanding;
        g->setInput(0, l); clearEv(); step(*g, DT);
        CHECK(countEv(EvType::Leap) == 0 && P(*g, 0).leapCd == 0 && P(*g, 0).leapPhase == 0);
        delete g;

        // carrier: shorter jump and longer cooldown; ghost cannot leap
        g = make(4); park(*g);
        g->leapLandingFn = &fakeLanding; g_fakeD = 100;
        forceCarry(*g, 0, 1); g->setInput(0, l); clearEv(); step(*g, DT);
        CHECK(g_lastMax == gc::LEAP_DIST_CARRIER && P(*g, 0).leapCd == 50);
        step(*g, 0.4f);
        CHECK(std::fabs(P(*g, 0).y - gcmap::bases()[0].spawnY - gc::LEAP_DIST_CARRIER) < 0.01f);   // facing down   // fake distance is clamped to the carrier's 56 px
        g->setConnected(1, false); g->setInput(1, l); clearEv(); step(*g, DT);
        CHECK(countEv(EvType::Leap) == 0);
        delete g;
    }

    section("overtime and match end");
    {
        auto* g = make(2, 3, 1); park(*g);
        g->debugSnapshot().players[0].score = 1; g->debugSnapshot().players[1].score = 1;
        g->debugSnapshot().matchTime = 0.05f;
        step(*g, 0.2f);
        CHECK(!g->snapshot().gameOver && g->snapshot().overtime && g->snapshot().matchTime > 20.f);
        atTreasure(*g, 1, 0); step(*g, DT); atTreasure(*g, 1, 1); step(*g, DT);
        CHECK(g->snapshot().gameOver && g->snapshot().winner == 1);
        delete g;
        g = make(2, 3, 1); park(*g);
        g->debugSnapshot().players[0].score = 2;
        g->debugSnapshot().matchTime = 0.05f; step(*g, 0.2f);
        CHECK(g->snapshot().gameOver && g->snapshot().winner == 0 && !g->snapshot().overtime);
        delete g;
        g = make(2, 3, 1); park(*g);                      // 0:0 draw, no overtime
        g->debugSnapshot().matchTime = 0.05f; step(*g, 0.2f);
        CHECK(g->snapshot().gameOver && g->snapshot().winner == 255);
        delete g;
    }

    section("snapshot serialisation roundtrip (leap/treasure/overtime fields)");
    {
        auto* g = make(2); park(*g);
        auto& s = g->debugSnapshot();
        s.overtime = true; s.players[0].leapCd = 17; s.players[0].leapPhase = 99;
        s.treasures[1].state = (uint8_t)TState::Dropped; s.treasures[1].timer = 4.5f;
        sf::Packet pk; pk << s; Snapshot r; pk >> r;
        CHECK(r.overtime && r.players[0].leapCd == 17 && r.players[0].leapPhase == 99 && r.treasures[1].state == (uint8_t)TState::Dropped && r.treasures[1].timer == 4.5f);
        delete g;
    }

    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
