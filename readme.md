# ninja_syndicate_go — a 4-player SFML arena

A single-map multiplayer game where each of four players guards a treasure in
their corner base and tries to steal the treasures from the other three bases.
Every player has a limited supply of ammo: shooting a foe **teleports them back
to their own base**, so you can stop a thief in their tracks — but only as many
times as your ammo allows.

Built with **SFML 3.1** (fetched via CMake FetchContent), an authoritative
**TCP net core** for coordinates / status / chat, and the *Ninja Adventure*
asset pack that ships in this repo.

## Build

Requirements: CMake ≥ 3.28, Ninja, a C++17 compiler (MSYS2 `ucrt64` g++ works),
and `ninja` on `PATH`.

```bash
./build_script.sh
```

This produces:

- `build/bin/game_server.exe` — the authoritative server
- `build/bin/game_client.exe` — the client

## Run

Open **two** terminals (server first, then one or more clients). Empty slots are
filled by AI bots, so a single client is fully playable.

```bash
# terminal 1 — host
./build/bin/game_server.exe            # optional: port (default 7777)

# terminal 2 — join
./build/bin/game_client.exe 127.0.0.1 7777 You
```

Up to four clients can join. Each takes over one base; the rest are bots.

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` / arrows | Move |
| `Space` | Fire a projectile (uses 1 ammo) |
| `Enter` | Open / send chat |
| `Esc` | Cancel chat |

## Rules

- Each base holds one treasure. Walk onto an **enemy** treasure to pick it up.
- Carry it back to **your** base and step on your base tile to score.
- Hit an enemy with a projectile to **teleport them home** (and drop whatever
  they were carrying). Ammo is limited and regenerates over time.
- First to deliver **3** treasures wins the round; rounds restart automatically.

## Architecture

```
src/game/
  Constants.hpp   gameplay + network constants
  Map.hpp         shared map grid, four base layout, tile collision
  Protocol.hpp    message types, game-state structs, sf::Packet (de)serialise
src/server/
  ServerMain.cpp  authoritative sim: movement, shooting, steal/deliver,
                  teleport-home, AI bots, 30 Hz snapshots, chat
src/client/
  ClientMain.cpp  rendering, input, chat, background net thread,
                  position interpolation, HUD
```

The server is the single source of truth. Clients send input + chat and render
authoritative snapshots. Projectiles, teleport-on-hit, treasure pickup/delivery
and scoring are all resolved server-side and broadcast to everyone.

## Map

The arena is generated from `tools/build_world.py` (Python + Pillow): it lays out
terrain, roads, the central lake with bridges, the four bases and the mirrored
cover, runs a reachability check, and writes `src/game/WorldMap.hpp` (shared by
server collision and client rendering). Re-run `python tools/build_world.py preview.png`
after editing the layout, then rebuild. Sprite rectangles live in `tools/world_catalog.py`.

## Scripts

Windows (`.bat`; `.ps1` wrappers and POSIX `.sh` equivalents also exist):

| Script | Purpose |
| --- | --- |
| `start_server.bat [port]` | Builds if needed, starts the server minimized in a window titled `ninja_syndicate_go Server :<port>` and prints your LAN `ip:port` addresses to share (default port 7777). |
| `stop_server.bat [port]` | Stops only the server started by `start_server.bat` on that port. `stop_server.bat all` kills every `game_server.exe`. |
| `start_client.bat [host] [port] [name]` | Launches the client (defaults `127.0.0.1 7777 Player`). |

`run_server.bat` / `run_client.bat` are thin wrappers. On bash use `./start_server.sh [port]` (pid in `.server.pid`) and `./stop_server.sh [all]`.

## Sound

Sound effects and music come from the pack's `Audio/` folder (SFML Audio). If the audio device or files are missing the game runs silently.
Hotkeys: `M` mute/unmute, `+`/`-` (or `]`/`[`) master volume (shown as a toast). Other players' shots fade with distance.
Set `SOUND_DEBUG=1` to log every played sound to stderr. Menu code can call `Sound::get().play(SfxId::UiClick / UiHover / UiBack)`.

## Rooms

One `game_server` process hosts many independent rooms (up to 4 players each, own lobby, match and chat; empty rooms are destroyed). No bots: empty slots stay empty.

- **Join string:** `host:port/CODE`, e.g. `192.168.1.5:7777/K7QX2`. A plain `CODE` also works and uses the Server address field (default `127.0.0.1:7777`). Codes are 5 chars from `A-Z2-9` without `I O 0 1`, case-insensitive. The string shown in the lobby uses the address you typed to connect, so on a LAN/WAN create the room using the server's reachable address.
- **Flow:** Landing (name, server, Create Room / Join Room / Quit) -> Join (paste string with Ctrl+V) -> Lobby (join string + Copy, 4 coloured slots, host/ready markers, chat, Start for the host, Ready for others, Leave) -> match. `Esc` in a match asks to leave and returns to the landing page; server errors and disconnects return to it with a message.
- **Protocol** (src/game/Protocol.hpp): `C2S_Join{name, JoinMode, code}` -> `S2C_Welcome{id, code}` or `S2C_Error{ErrCode, text}`; lobby updates `S2C_Lobby{LobbyInfo}`; `C2S_Ready` toggles ready, `C2S_Start` (host only) -> `S2C_GameStart` then snapshots/events/chat as before.
- **CLI:** `game_client [host] [port] [name] [--quick]` prefills the fields; `--quick` immediately creates a room.
- **Automation env vars (testing):** `GAME_AUTO=create|join:CODE`, `GAME_AUTO_START=<n>` (host starts once n players are in), `GAME_AUTO_CHAT=<text>` (+ `GAME_AUTO_CHAT_AT=<s>`), `GAME_AUTO_GAMECHAT=<text>` (sent once in the match), `GAME_AUTO_SETTINGS=m=5,w=2,fog=0,cd=1`, `GAME_AUTO_DROP=<s>` (simulate a network drop in a match), `GAME_AUTO_TRUST=1`, `GAME_NAME=<name>`, `GAME_SCREEN=join`, `GAME_JOIN_TEXT=<text>`, `GAME_ERR=<text>`, `GAME_SHOT=<png>` (+ `GAME_SHOT_ON=landing|join|lobby`, `GAME_SHOT_DELAY=<s>`; without `_ON` the shot is taken in-game), `GAME_SHOT_EXIT=1`. Server: `GAME_SERVER_EXIT_AFTER=<s>` (graceful shutdown after N seconds).

### Chat commands
`/w <name> <text>` (aliases `/msg /whisper /pm /tell`) whisper to a player in your room; names are case-insensitive and may be abbreviated (unique prefix). `/r <text>` replies to the last whisper. `/help` lists commands. `/kick <name>` (host, lobby) and `/name <text>` (host, lobby: room name). Tab completes a player name after `/w`, `/msg`, `/pm`, `/kick` (lobby and in-match chat). Whispers are violet (`[whisper from X]` / `[to X]`), server lines yellow.

### Room settings, countdown, rematch
The host sets match length (1/2/3/5/10 min, default 3), treasures to win (1-5, default 3), fog of war and the 5 second start countdown in the lobby; everyone sees the values, only the host can change them, and they are locked once the match starts. Pressing Start begins the countdown (shown to all, the host can cancel it). When a match ends the room returns to the lobby after a few seconds so the host can change settings and start a rematch.

### Ping, timeouts, reconnect
The server pings every client each second (RTT shown in ms with signal bars on the HUD cards, lobby slots, and for yourself). A peer silent for 9 s is dropped. If a player loses connection mid-match the slot stays reserved for 20 s (character idle); the client shows "Reconnecting... (attempt n)" and re-joins with a session token; teammates see "X lost connection" / "X reconnected". Clean leaves (Leave, quitting) free the slot immediately. If the host drops, the next player becomes host. Duplicate names get `#2`, `#3`. The server tells clients when it shuts down. Connecting runs on a worker thread with a Cancel button.

### Encryption (what it does and does not give you)
After TCP connect every packet is encrypted and authenticated: X25519 ephemeral key exchange + HKDF-SHA256 + ChaCha20-Poly1305 (all from SFML's bundled mbedTLS, no home-made crypto), with per-direction counters as nonces so tampering, replay and reordering are rejected. The server has a long-term key (`server_key` next to the exe, created on first start, git-ignored) and prints its fingerprint. The client pins it per `host:port` in `known_hosts` next to the exe (trust on first use) and shows a warning screen if it changes.
- Protects against passive eavesdropping and tampering/replay on the wire, and against impersonation *after* the first connection to a server.
- Does NOT protect against an active man-in-the-middle on the very first connection (TOFU) unless you compare the fingerprint printed by the server. Room codes are not secrets; anyone who knows host and code can join an open lobby. There is no user authentication, and a malicious server/host can still see everything in its rooms.
- Hardening: max packet size 4 KB, 8 connections per IP, 400 connections, 100 rooms, chat rate limit (5 burst, 1/s), room creation limit per IP (3 burst, 1 per 10 s), handshake must finish within 10 s.

## Music

Background music is a shuffled playlist (pack tracks + extra tracks in `Ninja Adventure - Asset Pack/Audio/MusicExtra/`)
that moves on to a different song when one ends. While you carry a trophy the music crossfades to a fast, tense
chase track and fades back to the playlist when you score or get sent home. Extra tracks are by Kevin MacLeod
(incompetech.com, CC BY 4.0) — see `Audio/MusicExtra/CREDITS.txt`. `SOUND_FORCE_CHASE=1` forces the chase music for testing.

## Client controls and HUD (stoppable carriers, leaps, secrets)

- **Shift or J**: leap over water / low walls in the facing direction (cooldown chip bottom-left; works while carrying, shorter and slower to recharge). Not triggered while typing chat.
- A carried or dropped trophy is revealed to everyone: pulsing beacon ring through the fog, blinking dot on the minimap, a screen-edge arrow with the distance in tiles (red "YOUR TREASURE" when yours is stolen, green "DELIVER" toward your base while you carry), a status chip under each player card (`home` / `Bob carries` / `DROPPED 9s`) and a global alarm on every steal. A dropped trophy shows a countdown arc in the owner's colour; touch it as the owner to send it home instantly, or grab it as an enemy once the short lock flash ends.
- Secret hedge corridors: bushes are drawn over players (they fade when you are close), enemies inside are hidden unless within 3 tiles (carriers never), the first time you step inside you get a "Secret passage found!" toast and the passage shows as green dots on the minimap.
- `GAME_BOT=1` autopilot now walks with leap edges and chases dropped trophies.

## Gameplay rules (server, `src/server/GameServer.hpp`, constants in `src/game/Rules.hpp`)
- Treasures are a state machine (`TState`: AtHome, Carried, Dropped, Returning, Inactive); `x,y` are updated every tick and `present == (state == AtHome)`.
- A carrier moves at x0.88 speed, cannot shoot and has hit radius 8 (normal 6). Hitting a carrier drops the trophy where it stands (locked 0.5 s, then anyone not carrying can take it; the owner touching it sends it home with no score). After 12 s it flies home (1 s) by itself. The hit player is still teleported to their base. While any trophy is carried or dropped, non-carriers regain ammo twice as fast.
- Connection loss drops a carried trophy in place; leave, kick or grace expiry sends it home from where it is. Disconnected (grace) players cannot pick up, deliver or be hit. Nothing changes after game over.
- A treasure is live when its slot is active, or when exactly one player is in the room (solo practice: the three neutral treasures are live). Empty bases show nothing in 2-3 player rooms. `GameServer::checkInvariants()` runs every tick, repairs inconsistent state and logs `[rules] invariant repaired`.
- Leap (Shift/J): up to 72 px (carrier 56 px), 0.35 s airborne and immune to shots, cooldown 3.5 s (carrier 5 s), only starts when `gcmap::leapLanding` finds a landing. Input edges (shoot/leap) are latched server-side so a press is never lost.
- Overtime: if the clock ends tied with a score above 0, 30 s sudden death (`Snapshot::overtime`), first delivery wins.
- Headless test: `tools/rules_test.cpp` (build line in its header; no window, no sockets).
