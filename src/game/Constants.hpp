// Shared gameplay constants for the steal-the-treasure multiplayer game.
#pragma once
#include <cstdint>

namespace gc {
// --- Map / world ---
constexpr int  TILE    = 16;     // tile size in pixels (assets are 16x16)
constexpr int  MAP_W   = 80;     // arena width (tiles)
constexpr int  MAP_H   = 60;     // arena height (tiles)
constexpr int  NUM_PLAYERS = 4;  // one base in each corner

// --- Player movement & collision ---
constexpr float PLAYER_SPEED   = 92.f;   // pixels per second
constexpr float PLAYER_BOX      = 10.f;  // collision box side (was 12, tighter hitbox)
constexpr float PLAYER_HIT_R     = 6.f;   // radius used by projectile-hit tests (was 7)

// --- Ammo / shooting / projectiles ---
constexpr int   MAX_AMMO         = 6;
constexpr float AMMO_REGEN_TIME  = 3.0f;  // seconds to recover one shot
constexpr float SHOOT_COOLDOWN   = 0.32f; // seconds between shots
constexpr float PROJECTILE_SPEED = 220.f; // pixels per second
constexpr float PROJECTILE_LIFE  = 1.6f;  // seconds before it expires
constexpr float PROJECTILE_R     = 4.f;   // projectile hit radius (was 5, tighter)

// --- Match rules ---
constexpr int   WIN_SCORE  = 3;     // treasures delivered to win
constexpr float MATCH_TIME = 180.f; // seconds per round

// --- Network ---
constexpr unsigned short SERVER_PORT = 7777;
constexpr const char*     DEFAULT_HOST = "127.0.0.1";
constexpr float SNAP_INTERVAL = 1.f / 30.f; // server snapshot rate

// --- Character sheet layout (matches Ninja Adventure sprite sheets) ---
// Columns are directions; rows are animation frames.
constexpr int FACE_DOWN  = 0;
constexpr int FACE_UP    = 1;
constexpr int FACE_LEFT  = 2;
constexpr int FACE_RIGHT = 3;

constexpr int NAME_LEN    = 16;
constexpr int CHAT_LEN    = 128;
constexpr int CHAT_HISTORY = 8;
constexpr int MAX_PROJECTILES = 32;
} // namespace gc
