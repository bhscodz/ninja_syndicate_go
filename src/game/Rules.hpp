// Gameplay rules shared by server and client (carrier / dropped trophy / leap). Tune the numbers here.
#pragma once
#include <cstdint>

namespace gc {
// --- carrier rules ---
constexpr float CARRIER_SPEED_MUL   = 0.88f;   // a player carrying a trophy moves 12% slower
constexpr float CARRIER_HIT_R       = 8.f;     // carriers are easier to hit (normal PLAYER_HIT_R = 6)
constexpr bool  CARRIER_CAN_SHOOT   = false;   // hands full
constexpr float HUNT_REGEN_MUL      = 0.5f;    // non-carriers regain ammo twice as fast while a trophy is on the loose
// --- dropped trophy ---
constexpr float DROP_RETURN_TIME    = 12.f;    // a dropped trophy flies home by itself after this long
constexpr float DROP_LOCK           = 0.5f;    // nobody can grab a trophy during its first 0.5 s on the ground
constexpr float RETURN_TRAVEL       = 1.0f;    // seconds of the trophy's flight home (untouchable)
constexpr float PICKUP_R            = 14.f;    // steal / pick up radius (px, between player centre and trophy)
constexpr float RECOVER_R           = 16.f;    // owner touching a dropped trophy sends it home instantly
constexpr float DELIVER_R           = 16.f;
// --- leap (jump across water / low walls) ---
constexpr float LEAP_DIST           = 72.f;    // max leap length in px (4.5 tiles)
constexpr float LEAP_DIST_CARRIER   = 56.f;    // carriers jump shorter (3.5 tiles)
constexpr float LEAP_MIN            = 8.f;
constexpr float LEAP_TIME           = 0.35f;   // seconds in the air
constexpr float LEAP_CD             = 3.5f;
constexpr float LEAP_CD_CARRIER     = 5.0f;
constexpr float OVERTIME_SECONDS    = 30.f;    // sudden death when the clock ends in a tie with score > 0
}

// Treasure state machine (server authoritative; TreasureState::state)
enum class TState : uint8_t { AtHome = 0, Carried = 1, Dropped = 2, Returning = 3, Inactive = 4 };
