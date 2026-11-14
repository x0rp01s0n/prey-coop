#!/usr/bin/env python3
"""Compile the production recovery snapshot selector against minimal state values."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = root / "Src/ModMain.cpp"
text = source.read_text()
start = "bool ModMain::ReconcileRecoveryPlayerStateSnapshot("
end = "bool ModMain::PrepareReceivedPlayerStateForHostLoad("
if text.count(start) != 1 or text.count(end) != 1:
    raise RuntimeError("production recovery selector boundary missing or duplicated")
actual = text[text.index(start):text.index(end)]
print("recovery_selector_sha256=" + hashlib.sha256(actual.encode()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix="prey-drop-recovery-selection-") as directory:
    build = Path(directory)
    (build / "SharedDropRecoverySelectionActual.inc").write_text(actual)
    binary = build / "shared-drop-recovery-selection-tests"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), "-I", str(root / "Src"),
        str(root / "Tests/CoopSharedDropRecoverySelectionTests.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
