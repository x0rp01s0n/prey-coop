#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I "$ROOT/Src" \
    "$ROOT/Src/CoopPerfDiagnostics.cpp" \
    "$ROOT/Tests/CoopPerfDiagnosticsTests.cpp" \
    -o "$BUILD_DIR/CoopPerfDiagnosticsTests"

COOP_PERF_DIAGNOSTICS=0 "$BUILD_DIR/CoopPerfDiagnosticsTests" disabled "$BUILD_DIR/disabled"
COOP_PERF_DIAGNOSTICS=1 "$BUILD_DIR/CoopPerfDiagnosticsTests" enabled "$BUILD_DIR/enabled"
