#!/usr/bin/env python3
"""Run the actual native save resolver against temporary filesystem fixtures."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "Src/ModMain.cpp").read_text()


def region(start, end):
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError("save resolver extraction boundary missing or duplicated: " + start)
    return source[source.index(start):source.index(end)]


actual = region("std::string NormalizeNativeWindowsPath(",
                "bool TryNormalizeCoopClientQuarantineSaveSlot(")
actual += region("bool TryReadableSaveCandidate(", "void RemoveResolvedSaveSlot(")
print("selected_native_save_resolver_sha256=" + hashlib.sha256(actual.encode()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix="prey-native-save-resolver-") as directory:
    build = Path(directory)
    (build / "NativeSaveResolverActual.inc").write_text(actual)
    binary = build / "native-save-resolver-tests"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), str(root / "Tests/CoopNativeSaveResolverTests.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(build / "fixture")], check=True)
