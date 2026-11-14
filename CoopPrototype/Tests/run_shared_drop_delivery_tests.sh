#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I "$ROOT/Src" \
    "$ROOT/Tests/CoopSharedDropDeliveryMetadataTests.cpp" \
    -o "$BUILD_DIR/CoopSharedDropDeliveryMetadataTests"
"$BUILD_DIR/CoopSharedDropDeliveryMetadataTests"
python3 "$ROOT/Tests/run_shared_drop_recovery_selection_tests.py"
