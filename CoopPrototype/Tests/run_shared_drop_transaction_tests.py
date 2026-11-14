#!/usr/bin/env python3
"""Compile selected, unmodified production methods against engine/transport fakes."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def section(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError(f"production source boundary changed: {start!r}")
    return text[text.index(start):text.index(end)]


root = Path(__file__).resolve().parents[1]
source = root / "Src/CoopSharedDropSync.cpp"
header = root / "Src/ModMain.h"
cpp = source.read_text()
methods = [
    ("bool ModMain::IsSharedDropWorldReady()", "void ModMain::DetachSharedDropEntities()"),
    ("void ModMain::DetachSharedDropEntities()", "uint64_t ModMain::BuildSharedDropStableId("),
    ("uint64_t ModMain::BuildSharedDropStableId(", "void ModMain::OnNativeRecyclerIngredientSpawned("),
    ("bool ModMain::ShouldDeferNativeSharedItemPickup(", "bool ModMain::MaterializeSharedDrop("),
    ("bool ModMain::RemoveSharedDropLocal(", "void ModMain::RestoreCommittedSharedDropDeliveries("),
    ("void ModMain::TickSharedDropPickupRetries(", "void ModMain::RecoverSavedSharedDrops("),
    ("void ModMain::RecoverSavedSharedDrops()", "void ModMain::PruneUnboundSharedDrops()"),
    ("void ModMain::ResetSharedDropState(", "bool ModMain::DebugSharedDropCommand("),
]
start = cpp.index("bool IsFiniteTransform(")
actual = cpp[start:cpp.index("\n}\n", start) + 3]
reservation = re.search(r"^constexpr auto kSharedDropReservationTimeout = [^;]+;", cpp, re.M)
if reservation is None:
    raise RuntimeError("production reservation timeout missing")
actual = reservation.group(0) + "\n" + actual
actual += section(cpp, "bool ParsePositiveInteger(", "int ParseAttemptCount(")
actual += "\n".join(section(cpp, start, end) for start, end in methods)
main_cpp = (root / "Src/ModMain.cpp").read_text()
actual += section(main_cpp, "bool IsSharedDropReplayPayload(", "bool IsAreaScopedReliablePayload(")
actual += section(main_cpp, "bool IsAreaScopedReliablePayload(", "bool IsReliableTransferStartPayload(")
record = section(header.read_text(), "    struct SharedDropRecord\n", "    uint64_t BuildSharedDropStableId(")
if len(re.findall(r"\bModMain::HandleSharedDrop\(", actual)) != 1:
    raise RuntimeError("actual HandleSharedDrop body missing or duplicated")
print("source_sha256=" + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
print("selected_methods_sha256=" + hashlib.sha256(actual.encode()).hexdigest(), flush=True)
with tempfile.TemporaryDirectory(prefix="prey-shared-drop-tests-") as directory:
    build = Path(directory)
    (build / "SharedDropActual.inc").write_text(actual)
    (build / "SharedDropRecordActual.inc").write_text(record)
    binary = build / "shared-drop-transaction-tests"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), "-I", str(root / "Src"),
        str(root / "Tests/CoopSharedDropTransactionTests.cpp"), "-o", str(binary),
    ], check=True)
    sys.exit(subprocess.run([str(binary), *sys.argv[1:]]).returncode)
