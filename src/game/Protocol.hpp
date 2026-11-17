// Networking protocol: message types, game-state structs and sf::Packet
// (de)serialisation shared by the authoritative server and all clients.
#pragma once
#include <SFML/Network.hpp>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include "Constants.hpp"
#include "Rules.hpp"
#include "Map.hpp"

enum class Msg : uint32_t {
    C2S_Join,       // client -> server: name (string), JoinMode (uint8), room code (string, join only)
    C2S_Input,      // client -> server: dirX,dirY (float), facing (int32), shoot (bool)
    C2S_Chat,       // client -> server: text (std::string)
    C2S_Ready,      // client -> server: toggle ready flag (lobby only)
    S2C_Welcome,    // server -> client: yourId (uint8), room code (string)
    S2C_Snapshot,   // server -> client: full Snapshot
    S2C_Chat,       // server -> client: ChatLine
    S2C_Event,      // server -> client: GameEvent
    S2C_GameStart,  // server -> client: match is starting
    S2C_Disconnect, // server -> client: server is shutting down
    C2S_Start,      // client -> server: host starts the match (lobby only)
    S2C_Error,      // server -> client: ErrCode (uint8), text (string); connection closed afterwards
    S2C_Lobby,      // server -> client: LobbyInfo (sent on every lobby change)
    C2S_Leave,      // client -> server: clean leave (no reconnect grace)
    C2S_Kick,       // client -> server: host kicks a player (name prefix, string)
    C2S_Settings,   // client -> server: host changes RoomSettings (lobby only)
    C2S_StartCancel,// client -> server: host cancels the start countdown
    S2C_ToLobby,    // server -> client: match over, room is back in the lobby
    S2C_Ping,       // server -> client: timestamp (uint32 ms); client answers C2S_Pong with the same value
    C2S_Pong,       // client -> server: echo of S2C_Ping
    S2C_Secure,     // server -> client: first encrypted packet (proves server holds the pinned key)
};

// --- Rooms ---
enum class JoinMode : uint8_t { Create = 0, Join = 1, Rejoin = 2 };
enum class ErrCode : uint8_t { NotFound, RoomFull, Started, BadRequest, Other, Kicked, Busy, Shutdown };

constexpr int ROOM_CODE_LEN = 5;
// No I, O, 0, 1 (easy to confuse).
constexpr const char* ROOM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

// Host-controlled room settings (locked once the match starts).
struct RoomSettings {
    uint8_t minutes = 3;      // match length: 1, 2, 3, 5 or 10
    uint8_t winScore = 3;     // treasures needed to win: 1..5
    bool    fog = true;       // fog of war
    bool    countdown = true; // 5 second start countdown when the host presses Start
    std::string name;         // friendly room name (host: /name <text>)
};
inline void sanitize(RoomSettings& r) {
    const uint8_t okm[] = { 1, 2, 3, 5, 10 };
    bool ok = false; for (uint8_t m : okm) ok = ok || m == r.minutes;
    if (!ok) r.minutes = 3;
    if (r.winScore < 1 || r.winScore > 5) r.winScore = 3;
    if (r.name.size() > 24) r.name.resize(24);
}

struct LobbySlot { bool active = false; bool ready = false; bool connected = true; uint16_t ping = 0; std::string name; };
struct LobbyInfo {
    uint8_t hostId = 255;
    bool    inMatch = false;
    int8_t  countdown = -1;   // seconds left of the start countdown, -1 = none
    RoomSettings settings;
    std::array<LobbySlot, gc::NUM_PLAYERS> slots;
};

// Uppercase + strip anything that is not a room-code character.
inline std::string normalizeCode(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out.push_back(c);
    }
    return out;
}

// Parses "host:port/CODE", "host/CODE" or plain "CODE". Fields not present stay untouched.
inline void parseJoinString(const std::string& s, std::string& host, unsigned short& port, std::string& code) {
    std::string t;
    for (char c : s) if (c > ' ') t.push_back(c);
    size_t slash = t.rfind('/');
    if (slash == std::string::npos) { code = normalizeCode(t); return; }
    code = normalizeCode(t.substr(slash + 1));
    std::string hp = t.substr(0, slash);
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        int pv = std::atoi(hp.substr(colon + 1).c_str());
        if (pv > 0 && pv < 65536) port = static_cast<unsigned short>(pv);
        hp = hp.substr(0, colon);
    }
    if (!hp.empty()) host = hp;
}

enum class EvType : uint8_t {
    Teleport, // a = victim, b = shooter            "X sent Y home"
    Steal,    // a = thief,  b = victim(base owner)  "X stole from Y"
    Deliver,  // a = player                          "X delivered a treasure"
    Join,     // a = player
    Leave,    // a = player
    Shoot,    // a = shooter
    Start,    // a = round starter
    GameOver, // a = winner (255 = draw)
    Full,     // server full
    Lost,     // a = player lost connection (reserved slot)
    Reconnected, // a = player came back
    Info,     // generic, see text via a/b mapping (unused in payload)
    Drop,     // a = victim (carrier hit), b = shooter, c = treasure id     "X lost Y's treasure"
    Recover,  // a = owner, c = treasure id                                  owner touched the dropped trophy -> home
    Returned, // c = treasure id                                             a dropped trophy flew home by itself
    Leap,     // a = player takes off
    Land,     // a = player lands
    Secret,   // a = player (client-side discovery; the server need not send it)
};

struct PlayerState {
    uint8_t  id = 255;
    bool     active = false;
    bool     isBot  = true;
    std::string name;
    float    x = 0, y = 0;       // top-left of the 16x16 sprite (pixels)
    int32_t  facing = 0;         // gc::FACE_*
    int32_t  ammo = gc::MAX_AMMO;
    int32_t  score = 0;
    bool     carrying = false;
    uint8_t  carriedTreasure = 255;
    bool     moving = false;
    int32_t  walkFrame = 0;
    uint16_t ping = 0;           // server-measured RTT in ms
    uint8_t  leapCd = 0;         // leap cooldown left in tenths of a second (0 = ready)
    uint8_t  leapPhase = 0;      // 0 = on the ground, 1..255 = progress through the leap arc (airborne)
    bool     connected = true;   // false while the slot is reserved for a reconnecting player
    // server-only transient fields (not serialised):
    float    shootCd = 0, ammoTimer = 0, walkTimer = 0;
    float    leapT = 0, leapCdT = 0, leapFromX = 0, leapFromY = 0, leapToX = 0, leapToY = 0;   // leap (server)
    bool     leapQueued = false, shootQueued = false;                                           // latched input edges (server)
    float    botStuck = 0, botShootCd = 0;
    int32_t  botMode = 0;
};

struct TreasureState {
    uint8_t id = 0;
    int32_t baseOwner = 0;
    float   homeX = 0, homeY = 0; // home position (pixel centre)
    float   x = 0, y = 0;         // current position (pixel centre)
    bool    present = true;       // sitting at home
    uint8_t carriedBy = 255;      // player id carrying, else 255
    uint8_t state = 0;            // TState (Rules.hpp): AtHome / Carried / Dropped / Returning / Inactive
    float   timer = 0;            // Dropped: seconds until it returns home; Returning: flight time left
};

struct ProjectileState {
    uint32_t id = 0;
    uint8_t  ownerId = 255;
    float    x = 0, y = 0, vx = 0, vy = 0;
    int32_t  dir = 0;
    int32_t  frame = 0;
};

struct Snapshot {
    float   matchTime = gc::MATCH_TIME;
    bool    gameOver = false;
    uint8_t winner = 255;
    std::array<PlayerState, gc::NUM_PLAYERS> players;
    std::array<TreasureState, gc::NUM_PLAYERS> treasures;
    std::array<ProjectileState, gc::MAX_PROJECTILES> projectiles;
    int32_t projectileCount = 0;
    uint8_t winScore = gc::WIN_SCORE;   // per-room rules (from RoomSettings)
    float   matchLen = gc::MATCH_TIME;
    bool    fog = true;
    bool    overtime = false;           // sudden death: the clock ended in a tie, first delivery wins (matchTime counts the overtime down)
};

enum ChatKind : uint8_t { CHAT_NORMAL = 0, CHAT_WHISPER_FROM = 1, CHAT_WHISPER_TO = 2, CHAT_SYSTEM = 3 };
struct ChatLine {
    uint8_t fromId = 255;
    std::string name;   // sender (whisper from) / recipient (whisper to)
    std::string text;
    uint8_t kind = CHAT_NORMAL;
};

// Movement/firing input sent by a client each tick.
struct ClientInput {
    float   dirX = 0, dirY = 0;   // desired movement (normalised server-side)
    int32_t facing = gc::FACE_DOWN;
    bool    shoot = false;        // edge: only fires on the frames the client sets this
    bool    leap  = false;        // edge: leap over water / low walls (Shift or J)
};

struct GameEvent {
    EvType type;
    uint8_t a = 255, b = 255, c = 255;
};

// --- Packet operators ---
inline sf::Packet& operator<<(sf::Packet& p, const PlayerState& s) {
    return p << s.id << s.active << s.isBot << s.name
             << s.x << s.y << s.facing << s.ammo << s.score
             << s.carrying << s.carriedTreasure << s.moving << s.walkFrame << s.ping << s.connected << s.leapCd << s.leapPhase;
}
inline sf::Packet& operator>>(sf::Packet& p, PlayerState& s) {
    return p >> s.id >> s.active >> s.isBot >> s.name
             >> s.x >> s.y >> s.facing >> s.ammo >> s.score
             >> s.carrying >> s.carriedTreasure >> s.moving >> s.walkFrame >> s.ping >> s.connected >> s.leapCd >> s.leapPhase;
}
inline sf::Packet& operator<<(sf::Packet& p, const TreasureState& t) {
    return p << t.id << t.baseOwner << t.homeX << t.homeY << t.x << t.y << t.present << t.carriedBy << t.state << t.timer;
}
inline sf::Packet& operator>>(sf::Packet& p, TreasureState& t) {
    return p >> t.id >> t.baseOwner >> t.homeX >> t.homeY >> t.x >> t.y >> t.present >> t.carriedBy >> t.state >> t.timer;
}
inline sf::Packet& operator<<(sf::Packet& p, const ProjectileState& pr) {
    return p << pr.id << pr.ownerId << pr.x << pr.y << pr.vx << pr.vy << pr.dir << pr.frame;
}
inline sf::Packet& operator>>(sf::Packet& p, ProjectileState& pr) {
    return p >> pr.id >> pr.ownerId >> pr.x >> pr.y >> pr.vx >> pr.vy >> pr.dir >> pr.frame;
}
inline sf::Packet& operator<<(sf::Packet& p, const Snapshot& s) {
    p << s.matchTime << s.gameOver << s.winner;
    for (const auto& pl : s.players) p << pl;
    for (const auto& t : s.treasures) p << t;
    p << s.projectileCount;
    for (int i = 0; i < s.projectileCount; ++i) p << s.projectiles[i];
    p << s.winScore << s.matchLen << s.fog << s.overtime;
    return p;
}
inline sf::Packet& operator>>(sf::Packet& p, Snapshot& s) {
    p >> s.matchTime >> s.gameOver >> s.winner;
    for (auto& pl : s.players) p >> pl;
    for (auto& t : s.treasures) p >> t;
    p >> s.projectileCount;
    if (s.projectileCount < 0) s.projectileCount = 0;
    if (s.projectileCount > gc::MAX_PROJECTILES) s.projectileCount = gc::MAX_PROJECTILES;
    for (int i = 0; i < s.projectileCount; ++i) p >> s.projectiles[i];
    p >> s.winScore >> s.matchLen >> s.fog >> s.overtime;
    return p;
}
inline sf::Packet& operator<<(sf::Packet& p, const ChatLine& c) {
    return p << c.fromId << c.name << c.text << c.kind;
}
inline sf::Packet& operator>>(sf::Packet& p, ChatLine& c) {
    return p >> c.fromId >> c.name >> c.text >> c.kind;
}
inline sf::Packet& operator<<(sf::Packet& p, const GameEvent& e) {
    return p << static_cast<uint8_t>(e.type) << e.a << e.b << e.c;
}
inline sf::Packet& operator>>(sf::Packet& p, GameEvent& e) {
    uint8_t t; p >> t >> e.a >> e.b >> e.c; e.type = static_cast<EvType>(t); return p;
}
inline sf::Packet& operator<<(sf::Packet& p, const RoomSettings& r) {
    return p << r.minutes << r.winScore << r.fog << r.countdown << r.name;
}
inline sf::Packet& operator>>(sf::Packet& p, RoomSettings& r) {
    p >> r.minutes >> r.winScore >> r.fog >> r.countdown >> r.name;
    sanitize(r);
    return p;
}
inline sf::Packet& operator<<(sf::Packet& p, const LobbyInfo& l) {
    p << l.hostId << l.inMatch << l.countdown << l.settings;
    for (const auto& s : l.slots) p << s.active << s.ready << s.connected << s.ping << s.name;
    return p;
}
inline sf::Packet& operator>>(sf::Packet& p, LobbyInfo& l) {
    p >> l.hostId >> l.inMatch >> l.countdown >> l.settings;
    for (auto& s : l.slots) p >> s.active >> s.ready >> s.connected >> s.ping >> s.name;
    return p;
}
