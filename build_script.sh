#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

for tool in cmake ninja g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool is not installed or not in PATH."
        exit 1
    fi
done

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

echo
echo "Build complete."
echo "  Server: build/bin/game_server.exe [port]"
echo "  Client: build/bin/game_client.exe [host] [port] [name]"
echo
echo "Quick start (one terminal each):"
echo "  ./build/bin/game_server.exe"
echo "  ./build/bin/game_client.exe 127.0.0.1 7777 You"
echo
echo "Empty player slots are filled by bots, so a single client is playable."
