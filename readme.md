# Steal the Treasure — a 4-player SFML arena

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
| `start_server.bat [port]` | Builds if needed, starts the server minimized in a window titled `StealTreasure Server :<port>` and prints your LAN `ip:port` addresses to share (default port 7777). |
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
- **Automation env vars (testing):** `GAME_AUTO=create|join:CODE`, `GAME_AUTO_START=<n>` (host starts once n players are in), `GAME_AUTO_CHAT=<text>`, `GAME_NAME=<name>`, `GAME_SCREEN=join`, `GAME_JOIN_TEXT=<text>`, `GAME_ERR=<text>`, `GAME_SHOT=<png>` (+ `GAME_SHOT_ON=landing|join|lobby` for menu screens, otherwise in-game after 3s), `GAME_SHOT_EXIT=1`.

## Music

Background music is a shuffled playlist (pack tracks + extra tracks in `Ninja Adventure - Asset Pack/Audio/MusicExtra/`)
that moves on to a different song when one ends. While you carry a trophy the music crossfades to a fast, tense
chase track and fades back to the playlist when you score or get sent home. Extra tracks are by Kevin MacLeod
(incompetech.com, CC BY 4.0) — see `Audio/MusicExtra/CREDITS.txt`. `SOUND_FORCE_CHASE=1` forces the chase music for testing.
