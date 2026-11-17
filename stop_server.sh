#!/usr/bin/env bash
# Usage: ./stop_server.sh [all]
cd "$(dirname "$0")"
if [ "$1" = all ]; then taskkill //F //IM game_server.exe 2>/dev/null || pkill -x game_server || echo "none running"; rm -f .server.pid; exit 0; fi
if [ -f .server.pid ] && kill -0 "$(cat .server.pid)" 2>/dev/null; then
  kill "$(cat .server.pid)"; rm -f .server.pid; echo "Stopped."
else echo "No server started by start_server.sh is running."; rm -f .server.pid; fi
