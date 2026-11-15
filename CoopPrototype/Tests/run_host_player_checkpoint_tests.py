#!/usr/bin/env python3
"""Compile the production checkpoint store and exercise real temporary files."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = root / "Src/CoopPlayerStateStore.cpp"
header = root / "Src/CoopPlayerStateStore.h"
for path in (source, header):
    print(path.name + "_sha256=" + hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
sidecar = (root / "Src/CoopPlayerSidecar.cpp").read_text()
start = "struct RecoveryJournalMetadata\n"
end = "uint64_t GetRecoveryJournalDiskMaxRevision("
if sidecar.count(start) != 1 or sidecar.count(end) != 1:
    raise RuntimeError("production recovery metadata boundary missing or duplicated")
metadata = sidecar[sidecar.index(start):sidecar.index(end)]
print("recovery_metadata_sha256=" + hashlib.sha256(metadata.encode()).hexdigest(), flush=True)

with tempfile.TemporaryDirectory(prefix="prey-host-checkpoint-") as directory:
    build = Path(directory)
    (build / "HostCheckpointRecoveryActual.inc").write_text(metadata)
    binary = build / "host-player-checkpoint-tests"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")),
        "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(root / "Src"), "-I", str(build),
        str(root / "Tests/CoopHostPlayerCheckpointTests.cpp"),
        str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(build / "fixtures")], check=True)
