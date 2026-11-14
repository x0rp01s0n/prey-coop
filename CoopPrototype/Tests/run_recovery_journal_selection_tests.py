#!/usr/bin/env python3
"""Compile actual recovery selection/retirement methods against storage dependencies."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / "Src/CoopPlayerSidecar.cpp"
text = source.read_text()
metadata = text[text.index("struct RecoveryJournalMetadata\n"):text.index("uint64_t GetRecoveryJournalDiskMaxRevision(")]
methods = text[text.index("bool ModMain::ClearLocalPlayerRecoveryJournal("):text.index("bool ModMain::IsLocalInventoryDirtyForSaveKey(")]
capture = text[text.index("bool ModMain::SaveLocalPlayerSidecar("):text.index("bool ModMain::StoreCommittedSharedDropDelivery(")]
captured = "m_localInventoryCapturedRevision = m_localInventoryDirtyRevision;"
write = "if (payload.empty() || !WritePlayerSidecarPayloadWithHash(path, payload))"
success = "m_localInventoryJournalRevision = std::max(m_localInventoryJournalRevision, snapshotRevision);"
if capture.count(captured) != 1 or not capture.index(write) < capture.index(success) < capture.index(captured):
    raise RuntimeError("canonical successful capture must publish its exact dirty revision after the checked write")
write_failure = capture[capture.index(write):capture.index(success)]
if "return false;" not in write_failure or captured in write_failure:
    raise RuntimeError("failed canonical write cannot publish a captured dirty revision")
revision_capture = capture[capture.index("    uint64_t snapshotRevision = 0;"):capture.index("    const std::string payload = output.str();")]
offline_start = capture.index("    RecoveryJournalMetadata preservedRecovery;")
offline_guard = capture[offline_start:capture.index("    const std::filesystem::path path =", offline_start)]
tick = text[text.index("void ModMain::TickPlayerSidecar("):]
retry_capture = tick[tick.index("            RecoveryJournalMetadata captured;"):tick.index("            const bool journalWritten = sidecarSaved")]
main = (root / "Src/ModMain.cpp").read_text()
watermark_start = main.index("        bool recoveryAlreadyStored = false;")
watermark = main[watermark_start:main.index("        if (!recoveryAlreadyStored &&", watermark_start)]
stop_start = main.index("    const bool savedNetworkStopSidecar =", main.index("void ModMain::StopNetwork()"))
stop_capture = main[stop_start:main.index('    ClearJoinOverlayState("network stop");', stop_start)]
print("source_sha256=" + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
print("selected_methods_sha256=" + hashlib.sha256((metadata + methods + revision_capture + retry_capture + watermark + stop_capture + offline_guard).encode()).hexdigest(), flush=True)
print("canonical_capture_dirty_revision_source_wiring=after_successful_checked_write", flush=True)
with tempfile.TemporaryDirectory(prefix="prey-journal-selection-") as directory:
    build = Path(directory)
    (build / "RecoveryMetadataActual.inc").write_text(metadata)
    (build / "RecoverySelectionActual.inc").write_text(methods)
    (build / "SnapshotRevisionActual.inc").write_text(revision_capture)
    (build / "SnapshotRetryActual.inc").write_text(retry_capture)
    (build / "CarriedSnapshotWatermarkActual.inc").write_text(watermark)
    (build / "StopNetworkCaptureActual.inc").write_text(stop_capture)
    (build / "OfflineRecoveryGuardActual.inc").write_text(offline_guard)
    binary = build / "recovery-journal-selection-tests"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(build), str(root / "Tests/CoopRecoveryJournalSelectionTests.cpp"), "-o", str(binary),
    ], check=True)
    sys.exit(subprocess.run([str(binary), str(build / "fixtures")]).returncode)
