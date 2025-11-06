#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
GENERATOR="${CMAKE_GENERATOR:-Unix Makefiles}"

echo "Configuring CMake project (generator: ${GENERATOR})..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$GENERATOR"

echo "Building DearSQL application..."
cmake --build "$BUILD_DIR" --target dear-sql

echo "Build complete. Binary located at: $BUILD_DIR/DearSQL.app"
