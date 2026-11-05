#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
BUILD_DIR="$ROOT/build"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$ROOT" -B "$BUILD_DIR"
fi

cmake --build "$BUILD_DIR"
