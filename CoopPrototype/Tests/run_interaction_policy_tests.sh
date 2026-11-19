#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I "$ROOT/Src" "$ROOT/Tests/CoopInteractionPolicyTests.cpp" \
    "$ROOT/Src/CoopEnemyStateWire.cpp" -o "$BUILD_DIR/interaction_tests"
"$BUILD_DIR/interaction_tests"
