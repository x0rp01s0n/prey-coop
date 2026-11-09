#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-$ROOT/Tests}/door-evidence.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pedantic \
    "$ROOT/Tests/CoopDoorEventEvidenceTests.cpp" "$ROOT/Src/CoopAreaObjectJournal.cpp" \
    -o "$BUILD_DIR/door_evidence_tests"
"$BUILD_DIR/door_evidence_tests" "$BUILD_DIR"
