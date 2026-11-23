#!/bin/sh
set -eu

cd -- "$(dirname -- "$0")"
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_CONFIG="${BUILD_CONFIG:-Release}"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_CONFIG"
fi
exec cmake --build "$BUILD_DIR" --config "$BUILD_CONFIG" --target run --parallel
