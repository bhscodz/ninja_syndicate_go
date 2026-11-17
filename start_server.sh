#!/usr/bin/env bash
# Usage: ./start_server.sh [port]   (pid kept in .server.pid)
cd "$(dirname "$0")"
PORT=${1:-7777}
BIN=build/bin/game_server.exe; [ -x "$BIN" ] || BIN=build/bin/game_server
[ -x "$BIN" ] || { ./build_script.sh || exit 1; BIN=build/bin/game_server.exe; [ -x "$BIN" ] || BIN=build/bin/game_server; }
if [ -f .server.pid ] && kill -0 "$(cat .server.pid)" 2>/dev/null; then echo "Already running (pid $(cat .server.pid))"; exit 1; fi
nohup "$BIN" "$PORT" > server_log.txt 2>&1 &
echo $! > .server.pid
echo "Server started, pid $!, port $PORT (log: server_log.txt)"
echo "LAN addresses:"
(ipconfig 2>/dev/null | grep -i "IPv4" | sed 's/.*: //' || hostname -I 2>/dev/null | tr ' ' '\n') | tr -d '\r' | while read ip; do [ -n "$ip" ] && echo "  $ip:$PORT"; done
