#!/usr/bin/env python3
"""Execute selected production timeline barriers with engine dependencies faked."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
main = (root / "Src/ModMain.cpp").read_text()
header = (root / "Src/ModMain.h").read_text()
sidecar = (root / "Src/CoopPlayerSidecar.cpp").read_text()


def region(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError("production timeline boundary missing or duplicated: " + start)
    if text.index(end) <= text.index(start):
        raise RuntimeError("production timeline boundaries reversed: " + start)
    return text[text.index(start):text.index(end)]


def method(text, start):
    if text.count(start) != 1:
        raise RuntimeError("production method missing or duplicated: " + start)
    begin = text.index(start)
    following = re.search(r"^\S[^\n]* ModMain::[A-Za-z0-9_]+\(",
                          text[begin + len(start):], re.MULTILINE)
    if not following:
        raise RuntimeError("production method has no next boundary: " + start)
    return text[begin:begin + len(start) + following.start()]


callback = region(main, "void ModMain::OnCryActionSaveGameReturned(\n",
                  "void ModMain::OnCryActionNotifySaveGame(")
pending = region(main, "static std::string NativeHostCheckpointSaveScope(",
                 "void ModMain::OnCryActionSaveGameRequested(")
pending_state = region(header, "    struct PendingHostCheckpointSave\n",
                       "    bool m_hostInternalSnapshotRetryPending = false;")
host_session_state = region(header, "    enum class HostInventorySession\n",
                            "    std::string m_pendingHostPlayerStateReason;")
host_timeline = method(main, "bool ModMain::OpenHostPlayerStateTimeline(")
host_load_finish = method(main, "void ModMain::FinishHostInventoryWorldLoad(")
native_load_request = region(main, "void ModMain::OnCryActionLoadGameRequested(",
                             "void ModMain::OnCryActionLoadGameFinished(")
host_start = region(main, "void ModMain::StartHost()\n", "void ModMain::StartClient()\n")
host_start_gate = region(host_start,
                         "    if (m_hostInventorySession == HostInventorySession::LoadingCheckpoint ||\n",
                         "    if (m_multiplayerRestartRequired ||")
host_start_timeline = "    OpenHostPlayerStateTimeline(m_lastSaveLoadPath, false);\n"
if host_start.count(host_start_timeline) != 1:
    raise RuntimeError("Host startup must use the central world-continuity policy exactly once")
if host_start.index(host_start_gate) >= host_start.index("    StopNetwork();\n"):
    raise RuntimeError("pending/failed world must be rejected before transport teardown")
client_start = method(main, "void ModMain::StartClient()\n")
client_start_gate = region(client_start, "    if (HasPendingHostCheckpointSave())\n",
                           "    StopNetwork();\n")
if client_start.index(client_start_gate) >= client_start.index("    StopNetwork();\n"):
    raise RuntimeError("pending native checkpoint must block Client startup before transport teardown")
client_entry = ("    m_networkMode = CoopNetworkMode::Client;\n"
                "    m_pendingHostInventoryLoadPath.clear();\n"
                "    if (m_hostInventorySession == HostInventorySession::Continuing)\n"
                "        m_hostInventorySession = HostInventorySession::NeedsCheckpointRestore;\n")
if client_start.count(client_entry) != 1:
    raise RuntimeError("entering a foreign Client world must invalidate earlier Host continuity")
host_stop = method(main, "void ModMain::StopNetwork()\n")
if "m_hostInventorySession" in host_stop:
    raise RuntimeError("transport-only StopNetwork must not reset world continuity")
host_stop_inventory = region(host_stop,
                             "    const bool preserveHostInventoryWorld = HasContinuingHostInventoryWorld();\n",
                             "    ResetChat();\n")
host_stop_reset = '    ResetPlayerStateTransferState("network stopped", !preserveHostInventoryWorld);\n'
if host_stop.count(host_stop_reset) != 1:
    raise RuntimeError("transport-only StopNetwork must preserve only an established Host inventory world")
end_load_guard = method(main, "void ModMain::EndCoopLoadGuard(")
if end_load_guard.count("FinishHostInventoryWorldLoad(loadFailed);") != 1:
    raise RuntimeError("native completion must resolve Host continuity at the load-guard boundary")
if end_load_guard.index("FinishHostInventoryWorldLoad(loadFailed);") >= end_load_guard.index("            StartHost();"):
    raise RuntimeError("Host continuity must settle before pending automatic Host startup")
request = region(main, "    const bool tracksHostCheckpoint =\n",
                 "    m_lastSaveLoadEvent =\n        std::string(internalCoopSnapshot ?")
for required in (
    "    ObserveHostCheckpointSaveWrite(safeSaveName, result);\n",
    "    ProcessSharedDropDeliveryRecoveryFault();\n    TickHostCheckpointSave();\n",
    "    if (resetHostSaveIdentity)\n    {\n        ResetPendingHostCheckpointSave();\n",
    "    if (!m_arkLevelTransitionLoadActive)\n    {\n        m_pendingHostInventoryLoadPath = m_lastSaveLoadPath;\n        ResetPendingHostCheckpointSave();\n        m_hostInventorySession = HostInventorySession::LoadingCheckpoint;\n",
):
    if main.count(required) != 1:
        raise RuntimeError("pending checkpoint production wiring missing or duplicated: " + required)
dirty = region(sidecar, "void ModMain::MarkLocalInventoryDirty(",
               "void ModMain::CaptureLocalPlayerPickupRecovery(")
save_guard = region(sidecar, "bool ModMain::SaveLocalPlayerSidecar(",
                    "    // A native acquisition whose resulting count is not yet known must not\n")
reset = region(main, "void ModMain::ResetPlayerStateTransferState(",
               "void ModMain::ResetAreaJournalTransferState(")
if "m_hostInventorySession" in reset:
    raise RuntimeError("transport/player transfer reset must not reset world continuity")
reset_host_inventory = region(reset, "    if (resetHostSaveIdentity)\n    {\n",
                              "    m_pendingHostSaveLoadBroadcastAfterLoad = false;\n")
carry_forward = method(main, "uint32_t ModMain::CarryForwardHostPlayerStatesForSave(")
carry_forward_guard = carry_forward[:carry_forward.index("    std::string error;\n")]
reset_guard = region(reset,
                     "    if (resetHostSaveIdentity)\n        m_clientHostTimelineRestorePending = false;\n",
                     "    m_hostPlayerStatePreloadSent = false;\n")
timeline_guard = region(main, "    const bool incomingHostState = m_networkMode == CoopNetworkMode::Client &&\n",
                        "    if (command == CoopProtocol::PlayerStateTransferCommand::StoredAck)\n")
hello = region(main, "bool ModMain::HandleSessionHello(", "void ModMain::HandleAreaLease(")
world_change = region(hello, "    if (remoteWorldChanged)\n", "    if (newPeer)\n")
remove_peer = region(main, "void ModMain::RemoveRemotePeer(", "void ModMain::KickRemotePeer(")
peer_removal = region(remove_peer, "    for (auto queueIt = m_reliableSendQueue.begin(); queueIt != m_reliableSendQueue.end();)\n",
                      "    m_hostPlayerStateUploadReceives.erase(accountToken);\n")
old_world = region(main, "    if (oldWorldPayload)\n", "    if (knownAreaMismatch && m_networkMode == CoopNetworkMode::Host)\n")
old_world_guard = region(main, "    const bool oldWorldPayload =\n", "    const bool knownAreaMismatch =\n")
restart = region(hello, "    if (acceptedRuntimeSessionRestart)\n",
                 "    const uint32_t previousWorldEpoch = newPeer ? 0 : existingIt->second.worldEpoch;\n")
if restart.count('RemoveRemotePeer(packet.accountToken, "peer runtime session restarted", false);') != 1:
    raise RuntimeError("runtime restart must retain the exact peer-removal transport reset")
selected = pending_state + pending + request + callback + dirty + save_guard + reset_guard + timeline_guard
selected += world_change + peer_removal + old_world_guard + old_world + restart
selected += host_session_state + host_timeline + host_load_finish + native_load_request
selected += host_start_gate + host_start_timeline
selected += client_entry
selected += client_start_gate
selected += host_stop_inventory + host_stop_reset + reset_host_inventory + carry_forward_guard
print("selected_timeline_controls_sha256=" + hashlib.sha256(selected.encode()).hexdigest(), flush=True)
actual = pending + callback + dirty + save_guard + "    (void)reason;\n    return true;\n}\n"
actual += "void ModMain::RequestCheckpointSave(const char* path, bool forceImmediate, bool internalHostSnapshot)\n{\n    const bool internalCoopSnapshot = internalHostSnapshot;\n" + request + "}\n"
actual += "void ModMain::ResetTimelineBarrier(bool resetHostSaveIdentity)\n{\n" + reset_guard + "}\n"
actual += "bool ModMain::AcceptTimelinePacket(const TimelinePacket& packet, CoopProtocol::PlayerStateTransferCommand command, const std::string& saveKey, bool hostAuthoritative)\n{\n"
actual += timeline_guard.replace("        return;\n", "        return false;\n") + "    return true;\n}\n"
actual += "void TransportHarness::ObservePeerWorldChange(bool remoteWorldChanged, Peer& peer)\n{\n"
actual += "    [[maybe_unused]] const auto fromAddress = peer.address;\n    [[maybe_unused]] const auto fromPort = peer.port;\n"
actual += world_change + "}\n"
actual += "void TransportHarness::RemovePeerTransport(const Peer& peer)\n{\n" + peer_removal + "}\n"
actual += "void TransportHarness::ReceiveWorldPayload(const ReliablePacket& packet, uint32_t fromAddress, uint16_t fromPort)\n{\n"
actual += "    const auto endpointKey = MakeEndpointKey(fromAddress, fromPort);\n"
actual += "    auto& endpointState = m_reliableEndpointStates[endpointKey];\n"
actual += old_world_guard + old_world + "    ++applied;\n}\n"
actual += host_load_finish + host_timeline + native_load_request
actual += "void ModMain::StartTimelineForTest()\n{\n" + host_start_timeline + "}\n"
actual += "bool ModMain::StartHostGateForTest()\n{\n"
actual += host_start_gate.replace("        return;\n", "        return false;\n") + "    return true;\n}\n"
actual += "void ModMain::EnterClientForTest()\n{\n" + client_entry + "}\n"
actual += "bool ModMain::StartClientForTest()\n{\n"
actual += client_start_gate.replace("        return;\n", "        return false;\n")
actual += "    StopNetwork();\n" + client_entry + "    return true;\n}\n"
actual += "void ModMain::ResetHostInventoryContextForTest(bool resetHostSaveIdentity)\n{\n"
actual += reset_host_inventory + "}\n"
actual += "void ModMain::StopNetwork()\n{\n    actions.push_back(\"stop\");\n"
actual += host_stop_inventory
actual += "    ResetHostInventoryContextForTest(!preserveHostInventoryWorld);\n"
actual += "    m_networkMode = CoopNetworkMode::Off;\n}\n"
actual += carry_forward_guard
actual += "    (void)reason;\n    actions.push_back(\"advance\");\n    return 1;\n}\n"

with tempfile.TemporaryDirectory(prefix="prey-timeline-barrier-") as directory:
    build = Path(directory)
    (build / "HostTimelineBarrierActual.inc").write_text(actual)
    (build / "HostPendingCheckpointStateActual.inc").write_text(pending_state)
    (build / "HostInventorySessionStateActual.inc").write_text(host_session_state)
    binary = build / "host-timeline-barrier-tests"
    subprocess.run([
        *shlex.split(os.environ.get("CXX", "c++")),
        "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(build),
        str(root / "Tests/CoopHostTimelineBarrierTests.cpp"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
