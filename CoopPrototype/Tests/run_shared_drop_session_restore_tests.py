#!/usr/bin/env python3
"""Execute production receipt restore/staging methods with sidecar IO faked."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "Src/CoopPlayerSidecar.cpp").read_text()
start = "bool ModMain::PrepareSharedDropDeliveriesForHostTransfer("
end = "bool ModMain::LoadLocalPlayerSidecar("
assert source.count(start) == 1 and source.count(end) == 1
actual = source[source.index(start):source.index(end)]
commit_start = "bool ModMain::StoreCommittedSharedDropDelivery("
commit_end = "bool ModMain::MergeSharedDropDeliveriesForUpload("
assert source.count(commit_start) == 1 and source.count(commit_end) == 1
commit = source[source.index(commit_start):source.index(commit_end)]
main = (root / "Src/ModMain.cpp").read_text()
stage_start = "bool ModMain::StageHostPlayerCheckpoint()"
stage_end = "bool ModMain::CaptureHostPlayerCheckpoint("
assert main.count(stage_start) == 1 and main.count(stage_end) == 1
stage = main[main.index(stage_start):main.index(stage_end)]
print("selected_session_restore_sha256=" + hashlib.sha256(actual.encode()).hexdigest(), flush=True)
print("selected_checkpoint_stage_sha256=" + hashlib.sha256(stage.encode()).hexdigest(), flush=True)
print("selected_receipt_store_sha256=" + hashlib.sha256(commit.encode()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix="prey-drop-session-restore-") as directory:
    build = Path(directory)
    (build / "SharedDropSessionRestoreActual.inc").write_text(actual)
    (build / "HostCheckpointStageActual.inc").write_text(stage)
    (build / "SharedDropCommitStoreActual.inc").write_text(commit)
    binary = build / "session-restore"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), "-I", str(root / "Src"),
        str(root / "Tests/CoopSharedDropSessionRestoreTests.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
