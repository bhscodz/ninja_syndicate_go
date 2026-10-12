#!/usr/bin/env bash

set -euo pipefail

# Go to the project root.
cd "$(dirname "${BASH_SOURCE[0]}")"

# Check required tools.
for tool in cmake ninja g++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool is not installed or not in PATH."
        exit 1
    fi
done

# Configure the project.
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug

# Compile and link the project.
cmake --build build --parallel

echo "Build completed successfully."
echo "Executable: build/bin/sfml_app.exe"
echo "use run the compiled code from build/bin/sfml_app.exe"