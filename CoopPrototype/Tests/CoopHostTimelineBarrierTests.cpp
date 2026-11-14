#include <algorithm>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../Src/CoopReliableReorderBuffer.h"
#include "../Src/CoopSerialSequence.h"

static std::string NativeHostCheckpointSaveScope(std::string);
bool campaignKnown = true;
bool TryGetCurrentCampaignIdentity(int& slot, uint64_t& guid)
{ slot = 0; guid = 23; return campaignKnown; }

enum class CoopNetworkMode { Off, Host, Client };
namespace HostStoreFixture
{
bool result = true;
uint64_t timeline = 31;
std::string fingerprint = "native_same_checkpoint";
std::filesystem::path root = "host_store";
std::vector<std::string> calls;
std::vector<std::string> fingerprintPaths;
std::vector<std::string> scopes;
}
namespace CoopPlayerStateStore
{
struct Store
{
    Store(const std::filesystem::path&,
        std::function<bool(const std::filesystem::path&, std::string&)>) {}
    bool RotateTimeline(const std::string& scope, const std::string&, std::string&)
    {
        HostStoreFixture::calls.push_back("rotate");
        HostStoreFixture::scopes.push_back(scope);
        return HostStoreFixture::result;
    }
    bool Open(const std::string& scope, const std::string&, std::string&)
    {
        HostStoreFixture::calls.push_back("open");
        HostStoreFixture::scopes.push_back(scope);
        return HostStoreFixture::result;
    }
    uint64_t Timeline() const { return HostStoreFixture::timeline; }
};
}
std::string NativePlayerCheckpointFingerprint(const std::string& path)
{
    HostStoreFixture::fingerprintPaths.push_back(path);
    return HostStoreFixture::fingerprint;
}
std::filesystem::path GetCoopServerSaveStateRoot() { return HostStoreFixture::root; }
std::string Hex64(uint64_t value) { return std::to_string(value); }
bool ValidatePlayerStateIntegrityFile(const std::filesystem::path&, void*, bool* hash, std::string*)
{ *hash = true; return true; }
std::string ToLowerAscii(std::string value)
{
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
namespace CoopProtocol
{
enum class PlayerStateTransferCommand { Start, HostSaveIdentity, StoredAck };
}

struct TimelinePacket
{
    uint64_t timelineId = 11;
    uint32_t totalBytes = 128;
    uint32_t chunkCount = 1;
};

struct ModMain
{
    CoopNetworkMode m_networkMode = CoopNetworkMode::Client;
    bool m_hostInternalSnapshotSaveActive = false;
    bool m_clientInternalPlayerSnapshotSaveActive = false;
    bool m_clientHostTimelineRestorePending = false;
    bool m_clientAwaitingHostPlayerState = false;
    bool m_enablePlayerSidecar = true;
    bool m_localInventoryDirty = false;
    bool suppressFeedback = false;
    bool captureResult = true;
    bool campaignGuidChanged = false;
    uint64_t hostAccount = 7;
    uint64_t m_currentHostTimelineId = 11;
    uint64_t m_localInventoryDirtyHostAccountToken = 0;
    uint64_t m_localInventoryDirtyRevision = 0;
    float m_localInventoryJournalAccumulator = 1.0f;
    std::string m_currentHostSaveStateKey = "save_A";
    std::string m_localInventoryDirtySaveKey;
    std::string m_lastPlayerSidecarEvent;
    std::string m_lastSaveLoadEvent;
    std::string m_lastSaveLoadPath = "save_A";
    std::string m_lastPlayerStateTransferEvent;
    std::vector<std::string> actions;
    std::vector<std::string> logs;
#include "HostPendingCheckpointStateActual.inc"
#include "HostInventorySessionStateActual.inc"
    std::unique_ptr<CoopPlayerStateStore::Store> m_hostPlayerStateStore;
    unsigned m_loadGameHookCalls = 0;
    std::string m_pendingAutoStartMode;
    bool m_arkLevelTransitionLoadActive = false;
    uint64_t m_sharedDropHostSessionId = 19;
    std::vector<int> m_localSharedDropDeliveries = {1};
    bool m_pendingHostSaveLoadBroadcastAfterLoad = false;
    bool m_hasRemoteEndpoint = false;
    bool m_playerSidecarInventoryNativeRestoreReady = true;
    bool m_playerSidecarInventoryNativeRestoreActive = true;
    bool m_lastNativeInventoryLocalSerializeWasRead = true;
    bool m_saveLoadGuardActive = false;
    bool m_pendingPostLoadResync = false;
    bool gameReady = true;
    std::string m_networkStatus;

    void OnCryActionSaveGameReturned(const char*, bool, bool, int, bool, const char*, bool);
    void OnCryActionLoadGameRequested(const char*, bool, bool);
    bool OpenHostPlayerStateTimeline(const std::string&, bool);
    void FinishHostInventoryWorldLoad(bool);
    void StartTimelineForTest();
    bool StartHostGateForTest();
    bool StartClientForTest();
    void EnterClientForTest();
    bool HasContinuingHostInventoryWorld() const;
    bool HasPendingHostCheckpointSave();
    void RequestCheckpointSave(const char*, bool = true, bool = false);
    void BeginHostCheckpointSave(const std::string&);
    void ObserveHostCheckpointSaveWrite(const std::string&, bool);
    void ResetPendingHostCheckpointSave();
    void TickHostCheckpointSave();
    void MarkLocalInventoryDirty(const char*);
    bool SaveLocalPlayerSidecar(const char*);
    void ResetTimelineBarrier(bool);
    void ResetHostInventoryContextForTest(bool);
    bool AcceptTimelinePacket(const TimelinePacket&, CoopProtocol::PlayerStateTransferCommand,
        const std::string&, bool);
    void CancelClientPlayerStateForTimelineChange(const char*)
    { actions.push_back("cancel"); m_clientHostTimelineRestorePending = true; }
    bool ShouldSuppressPlayerSidecarInventoryFeedback() const { return suppressFeedback; }
    uint64_t GetSessionHostAccountToken() const { return hostAccount; }
    uint64_t GetLocalAccountToken() const { return hostAccount; }
    bool IsGameReady() const { return gameReady; }
    void CancelHostPlayerStateUploadsForTimelineChange(const char*) { actions.push_back("cancel_uploads"); }
    void BeginCoopLoadGuard(const char*) { actions.push_back("load_guard"); }
    void BeginHostManualLoadPreload(const std::string&) { actions.push_back("preload"); }
    void ReconcileLocalInventoryDirtyForSaveKey(const std::string&) {}
    void LogCoop(const std::string& value) { logs.push_back(value); }
    std::string StatusToken(const std::string& value) const { return value; }
    std::string BuildHostSaveStateKey(const std::string& value) const
    {
        std::string scope = NativeHostCheckpointSaveScope(value);
        if (scope.rfind("campaign0/", 0) == 0) scope.erase(0, 10);
        return scope + (campaignGuidChanged ? "_new_guid" : "");
    }
    uint32_t CarryForwardHostPlayerStatesForSave(const std::string&, const std::string&, const char*);
    void SetCurrentHostSaveStateKey(const std::string& value, bool)
    { actions.push_back("set"); m_currentHostSaveStateKey = value; }
    bool CaptureHostPlayerCheckpoint(const std::string&)
    { actions.push_back("capture"); return captureResult; }
    void StopNetwork();
    void QueueCoopHudFeedback(const char*, float) { actions.push_back("hud"); }
    void BroadcastHostSaveIdentity(const char*, bool = false) { actions.push_back("broadcast"); }
    void RequestRemotePlayerStateUpload(const char*) { actions.push_back("request"); }
};

struct TransportHarness
{
    struct Peer
    {
        uint32_t address = 1;
        uint16_t port = 27015;
        uint32_t lastPoseSequence = 20;
    };
    struct ReliablePacket
    {
        uint32_t worldEpoch = 1;
        uint32_t reliableSequence = 5;
        uint16_t payloadType = 9;
    };
    struct Endpoint
    {
        uint32_t sendSequence = 0;
        uint32_t recvSequence = 0;
        uint32_t ackedSequence = 0;
        CoopReliableReorder::Buffer<ReliablePacket> reorderedPackets;
    };
    using ReliableEndpointState = Endpoint;
    struct Pending
    {
        uint32_t address;
        uint16_t port;
    };
    struct Telemetry { void RecordReliableRetired() {} } m_networkTelemetry;
    CoopNetworkMode m_networkMode = CoopNetworkMode::Host;
    bool m_saveLoadGuardActive = false;
    bool m_arkLevelTransitionLoadActive = false;
    uint32_t m_localWorldEpoch = 2;
    uint32_t m_reliableRecvSequence = 4;
    unsigned m_reliableRetiredAreaPackets = 0;
    unsigned applied = 0;
    unsigned acknowledgements = 0;
    unsigned drains = 0;
    unsigned smoothingResets = 0;
    unsigned replicationResets = 0;
    unsigned detachments = 0;
    std::string m_lastReliableEvent;
    std::unordered_map<uint64_t, Endpoint> m_reliableEndpointStates;
    std::vector<Pending> m_reliableSendQueue;
    static uint64_t MakeEndpointKey(uint32_t address, uint16_t port)
    { return (static_cast<uint64_t>(address) << 16) | port; }
    bool IsGameReady() const { return true; }
    void ClearRemotePeerPoseSmoothing(Peer&) { ++smoothingResets; }
    void ResetReplicationSequenceGuardsForWorldChange(const char*) { ++replicationResets; }
    void DetachRuntimeEntitiesForIncomingWorldReset(const char*) { ++detachments; }
    void SendReliableAckTo(uint32_t, uint32_t, uint16_t, const char*) { ++acknowledgements; }
    void DrainReliableReorderBuffer(uint64_t, uint32_t, uint16_t) { ++drains; }
    void ObservePeerWorldChange(bool, Peer&);
    void RemovePeerTransport(const Peer&);
    void ReceiveWorldPayload(const ReliablePacket&, uint32_t, uint16_t);
};

#include "HostTimelineBarrierActual.inc"

namespace
{
unsigned checks = 0;

void Check(bool condition, const char* label)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "FAIL " << label << '\n';
        std::exit(1);
    }
}

void SaveResult(ModMain& mod, bool result = true)
{
    mod.OnCryActionSaveGameReturned("save_B", false, true, 0, false, nullptr, result);
}

void ResetHostStore()
{
    HostStoreFixture::result = true;
    HostStoreFixture::timeline = 31;
    HostStoreFixture::fingerprint = "native_same_checkpoint";
    HostStoreFixture::root = "host_store";
    HostStoreFixture::calls.clear();
    HostStoreFixture::fingerprintPaths.clear();
    HostStoreFixture::scopes.clear();
}

void EstablishContinuingHostWorld(ModMain& mod)
{
    mod.m_hostInventorySession = ModMain::HostInventorySession::Continuing;
    mod.m_hostPlayerStateStore = std::make_unique<CoopPlayerStateStore::Store>(
        HostStoreFixture::root,
        [](const std::filesystem::path&, std::string&) { return true; });
}
}

int main()
{
    using HostSession = ModMain::HostInventorySession;
    ResetHostStore();
    ModMain coldHost;
    coldHost.m_networkMode = CoopNetworkMode::Host;
    Check(coldHost.m_hostInventorySession == HostSession::NeedsCheckpointRestore,
        "new process never inherits continuing-world authority from disk metadata");
    coldHost.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate"}) &&
        coldHost.m_currentHostTimelineId == 31 &&
        coldHost.m_hostInventorySession == HostSession::Continuing,
        "cold Host same native key and fingerprint restores checkpoint instead of future live state");
    coldHost.StopNetwork();
    coldHost.m_networkMode = CoopNetworkMode::Host;
    coldHost.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}) &&
        coldHost.m_hostInventorySession == HostSession::Continuing,
        "same-world network stop and rehost keeps accepted live inventory and timeline");
    coldHost.EnterClientForTest();
    Check(coldHost.m_networkMode == CoopNetworkMode::Client &&
        coldHost.m_hostInventorySession == HostSession::NeedsCheckpointRestore,
        "entering a foreign Client world discards former local Host continuity");
    coldHost.m_networkMode = CoopNetworkMode::Host;
    coldHost.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open", "rotate"}),
        "returning from Client mode to Host restores checkpoint rather than foreign world live state");
    for (const auto state : {HostSession::LoadingCheckpoint, HostSession::CheckpointLoadFailed})
    {
        ModMain fencedJoin;
        fencedJoin.m_hostInventorySession = state;
        fencedJoin.EnterClientForTest();
        Check(fencedJoin.m_networkMode == CoopNetworkMode::Client &&
            fencedJoin.m_hostInventorySession == state,
            "entering Client mode cannot erase pending or failed native-load evidence");
        fencedJoin.StopNetwork();
        Check(fencedJoin.m_hostInventorySession == state && !fencedJoin.StartHostGateForTest(),
            "failed join then transport stop cannot bypass the native-load Host startup fence");
        if (state == HostSession::LoadingCheckpoint)
        {
            fencedJoin.FinishHostInventoryWorldLoad(false);
            Check(fencedJoin.m_hostInventorySession == HostSession::CheckpointLoadFailed,
                "pending Off completion without a retained native load target cannot invent Host continuity");
        }
        else
        {
            fencedJoin.FinishHostInventoryWorldLoad(false);
            Check(fencedJoin.m_hostInventorySession == HostSession::CheckpointLoadFailed,
                "unrelated completion after failed join cannot erase native-load failure");
        }
    }

    ResetHostStore();
    ModMain offLoad;
    offLoad.m_networkMode = CoopNetworkMode::Off;
    offLoad.m_hostInventorySession = HostSession::Continuing;
    offLoad.OnCryActionLoadGameRequested("save_A", false, false);
    Check(offLoad.m_hostInventorySession == HostSession::LoadingCheckpoint &&
        offLoad.m_pendingHostInventoryLoadPath == "save_A" && HostStoreFixture::calls.empty(),
        "native load while network Off invalidates Host continuity without opening a store early");
    offLoad.FinishHostInventoryWorldLoad(false);
    Check(offLoad.m_hostInventorySession == HostSession::Continuing &&
        offLoad.m_pendingHostInventoryLoadPath.empty() &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate"}),
        "completed Off native load restores its frozen checkpoint before offline saves can advance it");
    offLoad.m_networkMode = CoopNetworkMode::Host;
    offLoad.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}),
        "starting Host after a completed Off load resumes its already restored checkpoint without another rollback");

    ResetHostStore();
    ModMain coldOfflineSave;
    coldOfflineSave.m_networkMode = CoopNetworkMode::Off;
    coldOfflineSave.OnCryActionLoadGameRequested("save_A", false, false);
    coldOfflineSave.RequestCheckpointSave("save_B");
    SaveResult(coldOfflineSave);
    Check(coldOfflineSave.m_pendingHostInventoryLoadPath == "save_A" &&
        coldOfflineSave.m_lastSaveLoadPath == "save_B" &&
        !coldOfflineSave.HasPendingHostCheckpointSave() && HostStoreFixture::calls.empty(),
        "a save callback during cold loading cannot replace the independently captured native load target");
    coldOfflineSave.FinishHostInventoryWorldLoad(false);
    Check(coldOfflineSave.m_hostInventorySession == HostSession::Continuing &&
        coldOfflineSave.m_pendingHostInventoryLoadPath.empty() &&
        coldOfflineSave.m_currentHostSaveStateKey == "save_a" &&
        coldOfflineSave.m_lastSaveLoadPath == "save_A" &&
        HostStoreFixture::fingerprintPaths == std::vector<std::string>({"save_A"}) &&
        HostStoreFixture::scopes == std::vector<std::string>({"save_a"}) &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate"}),
        "cold offline completion restores exactly loaded A rather than mutable last-save B or future live state");
    coldOfflineSave.actions.clear();
    coldOfflineSave.RequestCheckpointSave("save_B");
    SaveResult(coldOfflineSave);
    coldOfflineSave.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    coldOfflineSave.TickHostCheckpointSave();
    Check(coldOfflineSave.actions == std::vector<std::string>({"advance", "set", "capture"}) &&
        coldOfflineSave.m_currentHostSaveStateKey == "save_b" &&
        coldOfflineSave.m_currentHostTimelineId == 31,
        "first offline save after cold load advances its restored account set instead of copying newer abandoned live state");
    coldOfflineSave.m_networkMode = CoopNetworkMode::Host;
    coldOfflineSave.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}) &&
        HostStoreFixture::scopes == std::vector<std::string>({"save_a", "save_b"}) &&
        HostStoreFixture::fingerprintPaths.back() == "%USER%/SaveGames/Campaign0/save_B/save.CSF",
        "first Host startup after cold-load A and offline save B reopens the paired B continuation");
    coldOfflineSave.FinishHostInventoryWorldLoad(false);
    Check(coldOfflineSave.m_pendingHostInventoryLoadPath.empty() &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}),
        "an unrelated duplicate completion cannot repeat the consumed cold checkpoint restore");

    ResetHostStore();
    ModMain stopColdLoad;
    stopColdLoad.m_networkMode = CoopNetworkMode::Off;
    stopColdLoad.OnCryActionLoadGameRequested("save_A", false, false);
    stopColdLoad.StopNetwork();
    Check(stopColdLoad.m_hostInventorySession == HostSession::LoadingCheckpoint &&
        stopColdLoad.m_pendingHostInventoryLoadPath == "save_A" &&
        stopColdLoad.m_currentHostTimelineId == 0,
        "generic transport teardown preserves an in-flight native load target even when it resets inventory context");
    stopColdLoad.FinishHostInventoryWorldLoad(false);
    Check(stopColdLoad.m_hostInventorySession == HostSession::Continuing &&
        stopColdLoad.m_pendingHostInventoryLoadPath.empty() &&
        HostStoreFixture::fingerprintPaths == std::vector<std::string>({"save_A"}) &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate"}),
        "cold load completion still restores the exact native checkpoint after transport teardown");

    ResetHostStore();
    ModMain failedColdLoad;
    failedColdLoad.m_networkMode = CoopNetworkMode::Off;
    failedColdLoad.OnCryActionLoadGameRequested("save_A", false, false);
    failedColdLoad.FinishHostInventoryWorldLoad(true);
    Check(failedColdLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
        failedColdLoad.m_pendingHostInventoryLoadPath.empty() &&
        HostStoreFixture::calls.empty() && !failedColdLoad.StartHostGateForTest(),
        "failed cold native load consumes its target and cannot establish an offline Host scope");
    failedColdLoad.FinishHostInventoryWorldLoad(false);
    Check(failedColdLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
        HostStoreFixture::calls.empty(),
        "late unrelated success cannot revive an already failed cold native load");

    for (const bool missingFingerprint : {false, true})
    {
        ResetHostStore();
        HostStoreFixture::result = false;
        HostStoreFixture::timeline = 0;
        if (missingFingerprint)
            HostStoreFixture::fingerprint.clear();
        ModMain failedColdStore;
        failedColdStore.m_networkMode = CoopNetworkMode::Off;
        failedColdStore.OnCryActionLoadGameRequested("save_A", false, false);
        failedColdStore.FinishHostInventoryWorldLoad(false);
        Check(failedColdStore.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
            failedColdStore.m_pendingHostInventoryLoadPath.empty() &&
            failedColdStore.m_currentHostTimelineId == 0 &&
            !failedColdStore.HasContinuingHostInventoryWorld() &&
            !failedColdStore.StartHostGateForTest(),
            "cold checkpoint identity or Store I/O failure cannot authorize an offline save or first Host startup");
    }

    ResetHostStore();
    HostStoreFixture::result = false;
    ModMain emptyColdHistory;
    emptyColdHistory.m_networkMode = CoopNetworkMode::Off;
    emptyColdHistory.OnCryActionLoadGameRequested("unknown_historical_checkpoint", false, false);
    emptyColdHistory.FinishHostInventoryWorldLoad(false);
    Check(emptyColdHistory.m_hostInventorySession == HostSession::Continuing &&
        emptyColdHistory.m_currentHostTimelineId == 31 &&
        emptyColdHistory.m_pendingHostInventoryLoadPath.empty() &&
        emptyColdHistory.HasContinuingHostInventoryWorld() &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate"}),
        "missing historical Client checkpoint with a durable empty Store remains a safe cold offline world");

    for (const bool nativeClient : {false, true})
    {
        ResetHostStore();
        ModMain foreignLoad;
        foreignLoad.m_networkMode = nativeClient ? CoopNetworkMode::Client : CoopNetworkMode::Off;
        if (!nativeClient)
            foreignLoad.m_pendingAutoStartMode = "client";
        foreignLoad.OnCryActionLoadGameRequested("save_A", false, false);
        foreignLoad.FinishHostInventoryWorldLoad(false);
        Check(foreignLoad.m_hostInventorySession == HostSession::NeedsCheckpointRestore &&
            foreignLoad.m_pendingHostInventoryLoadPath.empty() && HostStoreFixture::calls.empty() &&
            !foreignLoad.HasContinuingHostInventoryWorld(),
            "active or pending Client load cannot establish a local Host checkpoint from a foreign world");
    }

    ResetHostStore();
    ModMain adoptedClientLoad;
    adoptedClientLoad.m_networkMode = CoopNetworkMode::Off;
    adoptedClientLoad.OnCryActionLoadGameRequested("save_A", false, false);
    Check(adoptedClientLoad.StartClientForTest() &&
        adoptedClientLoad.m_networkMode == CoopNetworkMode::Client &&
        adoptedClientLoad.m_hostInventorySession == HostSession::LoadingCheckpoint &&
        adoptedClientLoad.m_pendingHostInventoryLoadPath.empty(),
        "explicit Client adoption retires a captured Host load target without erasing the load fence");
    adoptedClientLoad.FinishHostInventoryWorldLoad(false);
    Check(adoptedClientLoad.m_hostInventorySession == HostSession::NeedsCheckpointRestore &&
        HostStoreFixture::calls.empty(),
        "completion after Client adoption cannot rotate the abandoned local Host checkpoint");

    ResetHostStore();
    ModMain missingColdTarget;
    missingColdTarget.m_networkMode = CoopNetworkMode::Off;
    missingColdTarget.OnCryActionLoadGameRequested(nullptr, false, false);
    missingColdTarget.FinishHostInventoryWorldLoad(false);
    Check(missingColdTarget.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
        missingColdTarget.m_pendingHostInventoryLoadPath.empty() && HostStoreFixture::calls.empty(),
        "successful callback without a usable captured native path fails closed rather than guessing a slot");

    ResetHostStore();
    ModMain activeLoad;
    activeLoad.m_networkMode = CoopNetworkMode::Host;
    activeLoad.m_hostInventorySession = HostSession::Continuing;
    activeLoad.OnCryActionLoadGameRequested("save_A", false, false);
    Check(activeLoad.m_hostInventorySession == HostSession::LoadingCheckpoint &&
        HostStoreFixture::calls == std::vector<std::string>({"rotate"}),
        "active Host load rotates checkpoint before preload but leaves native completion pending");
    activeLoad.FinishHostInventoryWorldLoad(false);
    Check(activeLoad.m_hostInventorySession == HostSession::Continuing,
        "successful active native load consumes its already-established inventory restore");
    activeLoad.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}),
        "network-only restart after completed active load does not rotate preloaded Clients twice");

    ResetHostStore();
    HostStoreFixture::timeline = 0;
    HostStoreFixture::result = false;
    ModMain unestablishedLoad;
    unestablishedLoad.m_networkMode = CoopNetworkMode::Host;
    unestablishedLoad.OnCryActionLoadGameRequested("save_A", false, false);
    unestablishedLoad.FinishHostInventoryWorldLoad(false);
    Check(unestablishedLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
        unestablishedLoad.m_currentHostTimelineId == 0,
        "native world success cannot clear failed inventory checkpoint establishment");

    ResetHostStore();
    ModMain areaTravel;
    areaTravel.m_networkMode = CoopNetworkMode::Host;
    areaTravel.m_hostInventorySession = HostSession::Continuing;
    areaTravel.m_arkLevelTransitionLoadActive = true;
    areaTravel.OnCryActionLoadGameRequested("area_transition", false, false);
    areaTravel.FinishHostInventoryWorldLoad(false);
    Check(areaTravel.m_hostInventorySession == HostSession::Continuing &&
        HostStoreFixture::calls.empty(),
        "ordinary area transition preserves the same inventory lineage without a checkpoint rollback");

    ResetHostStore();
    ModMain pendingLoad;
    pendingLoad.m_networkMode = CoopNetworkMode::Host;
    pendingLoad.m_hostInventorySession = HostSession::LoadingCheckpoint;
    Check(pendingLoad.OpenHostPlayerStateTimeline("save_A", false) &&
        pendingLoad.m_hostInventorySession == HostSession::LoadingCheckpoint,
        "store availability alone cannot consume pending native world loading");
    pendingLoad.FinishHostInventoryWorldLoad(true);
    Check(pendingLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed,
        "asynchronous native load failure fences a previously opened checkpoint store");
    pendingLoad.FinishHostInventoryWorldLoad(false);
    Check(pendingLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed,
        "unrelated later load-guard completion cannot clear a failed checkpoint fence");
    pendingLoad.OpenHostPlayerStateTimeline("save_A", false);
    Check(pendingLoad.m_hostInventorySession == HostSession::CheckpointLoadFailed,
        "opening a store is not proof that a failed native world load recovered");
    pendingLoad.OnCryActionLoadGameRequested("save_A", false, false);
    pendingLoad.FinishHostInventoryWorldLoad(false);
    Check(pendingLoad.m_hostInventorySession == HostSession::Continuing,
        "only another successful real native load may recover failed world continuity");

    ResetHostStore();
    HostStoreFixture::result = false;
    HostStoreFixture::timeline = 0;
    ModMain failedOpen;
    failedOpen.m_networkMode = CoopNetworkMode::Host;
    Check(!failedOpen.OpenHostPlayerStateTimeline("save_A", false) &&
        failedOpen.m_currentHostTimelineId == 0 &&
        failedOpen.m_hostInventorySession == HostSession::NeedsCheckpointRestore,
        "unsafe store I/O failure retains checkpoint-rebase requirement and zero lineage");
    HostStoreFixture::result = true;
    HostStoreFixture::timeline = 32;
    failedOpen.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "rotate"}),
        "retry after failed first store opening still rotates rather than resumes stale live state");
    ResetHostStore();
    HostStoreFixture::fingerprint.clear();
    ModMain noFingerprint;
    Check(!noFingerprint.OpenHostPlayerStateTimeline("save_A", false) &&
        noFingerprint.m_currentHostTimelineId == 0 && HostStoreFixture::calls.empty() &&
        noFingerprint.m_hostInventorySession == HostSession::NeedsCheckpointRestore,
        "missing native checkpoint identity cannot establish world continuity");
    ResetHostStore();
    HostStoreFixture::result = false;
    ModMain safeEmpty;
    safeEmpty.m_networkMode = CoopNetworkMode::Host;
    Check(!safeEmpty.OpenHostPlayerStateTimeline("unknown_checkpoint", false) &&
        safeEmpty.m_currentHostTimelineId == 31 &&
        safeEmpty.m_hostInventorySession == HostSession::Continuing,
        "missing historical snapshot with durable empty live state still establishes a safe continuing world");
    safeEmpty.StartTimelineForTest();
    Check(HostStoreFixture::calls == std::vector<std::string>({"rotate", "open"}),
        "safe empty bootstrap cannot erase newly accepted live inventory on each network rehost");
    ResetHostStore();

    ModMain startGate;
    Check(startGate.StartHostGateForTest(),
        "ready cold world can establish a checkpoint-backed Host session");
    for (const auto state : {HostSession::LoadingCheckpoint, HostSession::CheckpointLoadFailed})
    {
        startGate.m_hostInventorySession = state;
        Check(!startGate.StartHostGateForTest(),
            "pending or failed native checkpoint cannot tear down and restart networking");
    }
    startGate.m_hostInventorySession = HostSession::Continuing;
    startGate.m_saveLoadGuardActive = true;
    Check(!startGate.StartHostGateForTest(), "native load guard blocks Host startup");
    startGate.m_saveLoadGuardActive = false;
    startGate.m_pendingPostLoadResync = true;
    Check(!startGate.StartHostGateForTest(), "pending post-load resync blocks Host startup");
    startGate.m_pendingPostLoadResync = false;
    startGate.gameReady = false;
    Check(!startGate.StartHostGateForTest(), "unready native world blocks Host startup");
    startGate.gameReady = true;
    Check(startGate.StartHostGateForTest(), "settled continuing world can resume networking");

    ModMain client;
    client.m_clientHostTimelineRestorePending = true;
    client.MarkLocalInventoryDirty("abandoned timeline mutation");
    Check(!client.m_localInventoryDirty && client.m_localInventoryDirtyRevision == 0,
        "pending Host checkpoint rejects old-world dirty capture");
    Check(!client.SaveLocalPlayerSidecar("ordinary upload"),
        "pending Host checkpoint rejects old-world canonical capture");
    client.ResetTimelineBarrier(false);
    Check(client.m_clientHostTimelineRestorePending && client.m_clientAwaitingHostPlayerState,
        "transport-only Abort/reset preserves pending restore and awaiting state");
    client.MarkLocalInventoryDirty("mutation after transport Abort");
    Check(!client.m_localInventoryDirty && !client.SaveLocalPlayerSidecar("retry"),
        "transport reset cannot relabel abandoned inventory with new lineage");
    client.ResetTimelineBarrier(true);
    Check(!client.m_clientHostTimelineRestorePending && !client.m_clientAwaitingHostPlayerState,
        "full session identity reset clears barrier");
    Check(client.SaveLocalPlayerSidecar("after full reset"),
        "cleared barrier passes the selected save prefix");
    client.MarkLocalInventoryDirty("new valid mutation");
    Check(client.m_localInventoryDirty && client.m_localInventoryDirtyRevision == 1 &&
        client.m_localInventoryDirtySaveKey == "save_A" &&
        client.m_localInventoryDirtyHostAccountToken == 7,
        "ordinary scoped mutation retains native dirty bookkeeping");
    ModMain notPending;
    notPending.m_clientAwaitingHostPlayerState = true;
    notPending.ResetTimelineBarrier(false);
    Check(!notPending.m_clientHostTimelineRestorePending && !notPending.m_clientAwaitingHostPlayerState,
        "ordinary transport reset does not invent a pending checkpoint restore");
    ModMain host;
    host.m_networkMode = CoopNetworkMode::Host;
    host.m_clientHostTimelineRestorePending = true;
    host.MarkLocalInventoryDirty("Host mutation");
    Check(host.m_localInventoryDirty && host.SaveLocalPlayerSidecar("Host capture"),
        "Client-only barrier does not suppress Host canonical capture");
    ModMain disabled;
    disabled.m_enablePlayerSidecar = false;
    Check(!disabled.SaveLocalPlayerSidecar("disabled"), "disabled sidecar remains disabled");

    ModMain offlineSave;
    offlineSave.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(offlineSave);
    offlineSave.RequestCheckpointSave("save_B");
    Check(offlineSave.m_lastSaveLoadPath == "save_A" &&
        offlineSave.m_hostCheckpointSave.path == "save_B",
        "offline save in an established Host world retains completed A until native B is paired");
    Check(!offlineSave.StartHostGateForTest(),
        "rehost cannot race an unfinished offline native checkpoint");
    offlineSave.actions.clear();
    SaveResult(offlineSave);
    offlineSave.TickHostCheckpointSave();
    Check(offlineSave.actions.empty() && offlineSave.m_lastSaveLoadPath == "save_A",
        "offline native success return alone cannot advance the completed checkpoint");
    offlineSave.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    Check(offlineSave.actions.empty(),
        "offline writer callback remains signal-only instead of hashing on the writer thread");
    offlineSave.TickHostCheckpointSave();
    Check(offlineSave.actions == std::vector<std::string>({"advance", "set", "capture"}) &&
        offlineSave.m_currentHostSaveStateKey == "save_b" &&
        offlineSave.m_currentHostTimelineId == 11 &&
        offlineSave.m_hostInventorySession == HostSession::Continuing &&
        offlineSave.m_networkMode == CoopNetworkMode::Off,
        "completed offline save pairs live Client checkpoints without broadcasting or replacing lineage");
    Check(offlineSave.m_lastSaveLoadPath == "%USER%/SaveGames/Campaign0/save_B/save.CSF" &&
        offlineSave.StartHostGateForTest(),
        "rehost resumes only after exact completed offline native path and checkpoint are paired");
    offlineSave.TickHostCheckpointSave();
    offlineSave.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    offlineSave.TickHostCheckpointSave();
    Check(offlineSave.actions.size() == 3,
        "duplicate offline final-write signals cannot recapture an already paired checkpoint");

    for (const unsigned completedSignals : {0u, 1u, 2u})
    {
        ModMain stopDuringSave;
        stopDuringSave.m_networkMode = CoopNetworkMode::Host;
        EstablishContinuingHostWorld(stopDuringSave);
        const auto* store = stopDuringSave.m_hostPlayerStateStore.get();
        stopDuringSave.RequestCheckpointSave("save_B");
        const auto deadline = stopDuringSave.m_hostCheckpointSave.deadline;
        if (completedSignals == 1)
            SaveResult(stopDuringSave);
        else if (completedSignals == 2)
            stopDuringSave.ObserveHostCheckpointSaveWrite(
                "%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
        stopDuringSave.StopNetwork();
        Check(stopDuringSave.m_networkMode == CoopNetworkMode::Off &&
            stopDuringSave.m_hostPlayerStateStore.get() == store &&
            stopDuringSave.m_currentHostTimelineId == 11 &&
            stopDuringSave.m_currentHostSaveStateKey == "save_A" &&
            stopDuringSave.m_hostCheckpointSave.path == "save_B" &&
            stopDuringSave.m_hostCheckpointSave.deadline == deadline,
            "network stop preserves established store identity and unfinished native save at either callback ordering");
        Check(!stopDuringSave.StartHostGateForTest(),
            "unfinished save remains a rehost fence after transport shutdown");
        stopDuringSave.actions.clear();
        if (completedSignals != 1)
            SaveResult(stopDuringSave);
        if (completedSignals != 2)
            stopDuringSave.ObserveHostCheckpointSaveWrite(
                "%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
        stopDuringSave.TickHostCheckpointSave();
        Check(stopDuringSave.actions == std::vector<std::string>({"advance", "set", "capture"}) &&
            stopDuringSave.m_hostPlayerStateStore.get() == store &&
            stopDuringSave.m_currentHostSaveStateKey == "save_b" &&
            stopDuringSave.m_currentHostTimelineId == 11,
            "pending active Host save finishes offline in the same store after transport shutdown");
    }

    for (const unsigned invalidContext : {0u, 1u, 2u, 3u, 4u, 5u, 6u})
    {
        ModMain unestablishedContext;
        unestablishedContext.m_networkMode = CoopNetworkMode::Off;
        EstablishContinuingHostWorld(unestablishedContext);
        switch (invalidContext)
        {
        case 0: unestablishedContext.m_hostPlayerStateStore.reset(); break;
        case 1: unestablishedContext.m_currentHostTimelineId = 0; break;
        case 2: unestablishedContext.m_currentHostSaveStateKey.clear(); break;
        case 3: unestablishedContext.m_currentHostSaveStateKey = "unknown_save"; break;
        case 4: unestablishedContext.m_hostInventorySession = HostSession::NeedsCheckpointRestore; break;
        case 5: unestablishedContext.m_hostInventorySession = HostSession::CheckpointLoadFailed; break;
        case 6: unestablishedContext.m_networkMode = CoopNetworkMode::Client; break;
        }
        Check(!unestablishedContext.HasContinuingHostInventoryWorld(),
            "a state enum alone cannot authorize offline Host checkpoint capture or retention");
        unestablishedContext.RequestCheckpointSave("save_B");
        SaveResult(unestablishedContext);
        unestablishedContext.ObserveHostCheckpointSaveWrite(
            "%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
        unestablishedContext.TickHostCheckpointSave();
        Check(unestablishedContext.actions.empty() && !unestablishedContext.HasPendingHostCheckpointSave(),
            "missing scope or foreign world cannot capture a Host checkpoint through ordinary save callbacks");
    }

    for (const auto mode : {CoopNetworkMode::Off, CoopNetworkMode::Host})
    {
        ModMain pendingJoin;
        pendingJoin.m_networkMode = mode;
        EstablishContinuingHostWorld(pendingJoin);
        const auto* store = pendingJoin.m_hostPlayerStateStore.get();
        pendingJoin.RequestCheckpointSave("save_B");
        Check(!pendingJoin.StartClientForTest() && pendingJoin.m_networkMode == mode &&
            pendingJoin.m_hostInventorySession == HostSession::Continuing &&
            pendingJoin.m_hostPlayerStateStore.get() == store &&
            pendingJoin.m_currentHostSaveStateKey == "save_A" &&
            pendingJoin.m_currentHostTimelineId == 11 &&
            pendingJoin.m_hostCheckpointSave.path == "save_B" &&
            pendingJoin.actions == std::vector<std::string>({"hud"}),
            "joining cannot tear down or abandon an unfinished Host checkpoint in either network mode");
        pendingJoin.actions.clear();
        SaveResult(pendingJoin, false);
        Check(pendingJoin.StartClientForTest() &&
            pendingJoin.m_networkMode == CoopNetworkMode::Client &&
            pendingJoin.m_hostInventorySession == HostSession::NeedsCheckpointRestore &&
            pendingJoin.actions == std::vector<std::string>({"stop"}),
            "initial native save rejection releases the join fence and discards former Host continuity normally");
    }

    ModMain conflictedJoin;
    conflictedJoin.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(conflictedJoin);
    conflictedJoin.m_hostCheckpointSave.conflict = true;
    Check(!conflictedJoin.StartClientForTest() &&
        conflictedJoin.m_hostCheckpointSave.conflict &&
        conflictedJoin.m_networkMode == CoopNetworkMode::Off,
        "an unscoped save conflict blocks Client startup before any transport reset");

    ModMain completedJoin;
    completedJoin.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(completedJoin);
    completedJoin.RequestCheckpointSave("save_B");
    SaveResult(completedJoin);
    completedJoin.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    completedJoin.TickHostCheckpointSave();
    completedJoin.actions.clear();
    Check(completedJoin.StartClientForTest() &&
        completedJoin.m_networkMode == CoopNetworkMode::Client &&
        completedJoin.m_hostInventorySession == HostSession::NeedsCheckpointRestore &&
        completedJoin.actions == std::vector<std::string>({"stop"}),
        "a completed offline paired save allows normal Client startup without preserving Host authority");

    ModMain ordinaryJoin;
    ordinaryJoin.m_networkMode = CoopNetworkMode::Off;
    Check(ordinaryJoin.StartClientForTest() &&
        ordinaryJoin.m_networkMode == CoopNetworkMode::Client &&
        ordinaryJoin.actions == std::vector<std::string>({"stop"}),
        "ordinary Client startup without a pending native checkpoint remains unchanged");

    ModMain offlineDeferred;
    offlineDeferred.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(offlineDeferred);
    offlineDeferred.RequestCheckpointSave("save_B", false);
    offlineDeferred.OnCryActionSaveGameReturned("save_B", false, false, 0, false, nullptr, true);
    offlineDeferred.TickHostCheckpointSave();
    Check(offlineDeferred.actions.empty() && offlineDeferred.m_hostCheckpointSave.path.empty() &&
        offlineDeferred.m_lastSaveLoadPath == "save_A",
        "offline deferred acceptance does not replace A with unfinished B or invent a final write");
    offlineDeferred.RequestCheckpointSave("save_B");
    offlineDeferred.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    offlineDeferred.TickHostCheckpointSave();
    Check(offlineDeferred.actions.empty(),
        "offline writer-before-return ordering still waits for native request acceptance");
    SaveResult(offlineDeferred);
    offlineDeferred.TickHostCheckpointSave();
    Check(offlineDeferred.actions == std::vector<std::string>({"advance", "set", "capture"}),
        "offline deferred replay pairs exactly once after both native completion signals");

    ModMain offlineRejected;
    offlineRejected.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(offlineRejected);
    offlineRejected.RequestCheckpointSave("save_B");
    SaveResult(offlineRejected, false);
    offlineRejected.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    offlineRejected.TickHostCheckpointSave();
    Check(offlineRejected.actions.empty() && offlineRejected.m_lastSaveLoadPath == "save_A" &&
        offlineRejected.m_currentHostSaveStateKey == "save_A" &&
        offlineRejected.m_hostInventorySession == HostSession::Continuing &&
        offlineRejected.StartHostGateForTest(),
        "initial offline SaveGame rejection keeps the previously completed checkpoint usable");

    ModMain offlineTimeout;
    offlineTimeout.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(offlineTimeout);
    offlineTimeout.RequestCheckpointSave("save_B");
    SaveResult(offlineTimeout);
    offlineTimeout.m_hostCheckpointSave.deadline =
        std::chrono::steady_clock::now() - std::chrono::seconds(1);
    offlineTimeout.TickHostCheckpointSave();
    Check(offlineTimeout.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
        !offlineTimeout.StartHostGateForTest() && offlineTimeout.m_lastSaveLoadPath == "save_A",
        "missing offline final-write callback fences stale checkpoint rehosting instead of guessing completion");

    ModMain orphanedConflict;
    orphanedConflict.m_hostCheckpointSave.conflict = true;
    Check(!orphanedConflict.StartHostGateForTest(),
        "an unresolved native save conflict fences hosting even before a scope is available");

    for (const bool captureFails : {false, true})
    {
        ModMain offlineFailure;
        offlineFailure.m_networkMode = CoopNetworkMode::Off;
        EstablishContinuingHostWorld(offlineFailure);
        offlineFailure.captureResult = !captureFails;
        offlineFailure.RequestCheckpointSave("save_B");
        SaveResult(offlineFailure);
        offlineFailure.ObserveHostCheckpointSaveWrite(
            "%USER%/SaveGames/Campaign0/save_B/save.CSF", captureFails);
        offlineFailure.TickHostCheckpointSave();
        Check(offlineFailure.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
            !offlineFailure.StartHostGateForTest() && offlineFailure.m_lastSaveLoadPath == "save_A",
            "accepted offline final-write or checkpoint failure prevents stale-pair rehosting");
        Check(std::find(offlineFailure.actions.begin(), offlineFailure.actions.end(), "broadcast") ==
                offlineFailure.actions.end() &&
            std::find(offlineFailure.actions.begin(), offlineFailure.actions.end(), "request") ==
                offlineFailure.actions.end(),
            "failed offline save cannot publish a new Client identity or request uploads");
        offlineFailure.EnterClientForTest();
        offlineFailure.StopNetwork();
        Check(offlineFailure.m_hostInventorySession == HostSession::CheckpointLoadFailed &&
            !offlineFailure.StartHostGateForTest(),
            "a failed Client join cannot erase the offline save failure fence");
    }

    for (const auto mode : {CoopNetworkMode::Off, CoopNetworkMode::Client})
    {
        ModMain unrelatedSave;
        unrelatedSave.m_networkMode = mode;
        unrelatedSave.RequestCheckpointSave("save_B");
        SaveResult(unrelatedSave);
        unrelatedSave.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
        unrelatedSave.TickHostCheckpointSave();
        Check(unrelatedSave.actions.empty() && unrelatedSave.m_hostCheckpointSave.path.empty() &&
            unrelatedSave.m_hostInventorySession == HostSession::NeedsCheckpointRestore,
            "ordinary single-player or Client save cannot capture a former Host inventory checkpoint");
    }

    ResetHostStore();
    ModMain abandonedOfflineSave;
    abandonedOfflineSave.m_networkMode = CoopNetworkMode::Off;
    EstablishContinuingHostWorld(abandonedOfflineSave);
    abandonedOfflineSave.RequestCheckpointSave("save_B");
    SaveResult(abandonedOfflineSave);
    abandonedOfflineSave.OnCryActionLoadGameRequested("save_A", false, false);
    abandonedOfflineSave.FinishHostInventoryWorldLoad(false);
    abandonedOfflineSave.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    abandonedOfflineSave.TickHostCheckpointSave();
    Check(abandonedOfflineSave.m_hostCheckpointSave.path.empty() &&
        abandonedOfflineSave.m_hostInventorySession == HostSession::Continuing &&
        abandonedOfflineSave.m_pendingHostInventoryLoadPath.empty() &&
        abandonedOfflineSave.m_currentHostSaveStateKey == "save_a" &&
        std::find(abandonedOfflineSave.actions.begin(), abandonedOfflineSave.actions.end(), "capture") ==
            abandonedOfflineSave.actions.end(),
        "native load while Off retires pending old-world save before its late final-write callback");

    ModMain successful;
    successful.m_networkMode = CoopNetworkMode::Host;
    successful.RequestCheckpointSave("save_B");
    Check(successful.m_lastSaveLoadPath == "save_A" && successful.m_hostCheckpointSave.path == "save_B",
        "forced Host request tracks attempted B without replacing the completed A path");
    SaveResult(successful);
    successful.TickHostCheckpointSave();
    Check(successful.actions.empty() && successful.m_currentHostSaveStateKey == "save_A",
        "native successful return alone cannot fingerprint or publish unfinished bytes");
    successful.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    Check(successful.actions.empty(), "native writer callback only signals, never hashes or publishes");
    successful.TickHostCheckpointSave();
    Check(successful.actions == std::vector<std::string>({"advance", "set", "capture", "broadcast", "request"}),
        "successful external native save captures before identity broadcast and live upload request");
    Check(successful.m_currentHostSaveStateKey == "save_b" &&
        successful.m_networkMode == CoopNetworkMode::Host,
        "successful checkpoint retains the active Host session");
    Check(successful.m_lastSaveLoadPath == "%USER%/SaveGames/Campaign0/save_B/save.CSF",
        "only completed durable checkpoint advances the tracked native path for future rehost");
    successful.TickHostCheckpointSave();
    successful.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    successful.TickHostCheckpointSave();
    Check(successful.actions.size() == 5, "completed or duplicate writer signals cannot publish twice");
    ModMain beforeReturn;
    beforeReturn.m_networkMode = CoopNetworkMode::Host;
    beforeReturn.RequestCheckpointSave("save_B");
    beforeReturn.ObserveHostCheckpointSaveWrite("C:\\Profiles\\SaveGames\\Campaign0\\SAVE_B\\save.CSF", true);
    beforeReturn.TickHostCheckpointSave();
    Check(beforeReturn.actions.empty(), "early writer completion waits for the native call result");
    SaveResult(beforeReturn);
    beforeReturn.TickHostCheckpointSave();
    Check(beforeReturn.actions.size() == 5, "writer-before-return ordering publishes once on main Tick");
    ModMain deferred;
    deferred.m_networkMode = CoopNetworkMode::Host;
    deferred.RequestCheckpointSave("save_B", false);
    deferred.OnCryActionSaveGameReturned("save_B", false, false, 0, false, nullptr, true);
    deferred.TickHostCheckpointSave();
    Check(deferred.actions.empty() && deferred.m_currentHostSaveStateKey == "save_A",
        "accepted deferred native request does not hash or publish the previous slot bytes");
    Check(deferred.m_lastSaveLoadPath == "save_A",
        "deferred acceptance cannot poison the native path used by a subsequent Host restart");
    deferred.captureResult = false;
    deferred.OnCryActionSaveGameReturned("save_B", false, false, 0, false, nullptr, true);
    Check(deferred.actions.empty() && deferred.m_networkMode == CoopNetworkMode::Host,
        "deferred request with no new disk file does not stop the Host session");
    deferred.captureResult = true;
    deferred.RequestCheckpointSave("save_B");
    SaveResult(deferred);
    deferred.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    deferred.TickHostCheckpointSave();
    Check(deferred.actions == std::vector<std::string>({"advance", "set", "capture", "broadcast", "request"}),
        "eventual forced native replay captures the completed checkpoint exactly once");
    ModMain failure;
    failure.m_networkMode = CoopNetworkMode::Host;
    failure.captureResult = false;
    failure.RequestCheckpointSave("save_B");
    SaveResult(failure);
    failure.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    failure.TickHostCheckpointSave();
    Check(failure.actions == std::vector<std::string>({"advance", "set", "capture", "stop", "hud"}),
        "checkpoint persistence failure stops session before any identity broadcast or upload request");
    Check(failure.m_networkMode == CoopNetworkMode::Off,
        "failed checkpoint cannot leave zero or stale lineage networking alive");
    Check(failure.m_lastSaveLoadPath == "save_A",
        "co-op capture failure does not label the new native slot as a completed paired checkpoint");
    ModMain nativeFailure;
    nativeFailure.m_networkMode = CoopNetworkMode::Host;
    nativeFailure.RequestCheckpointSave("save_B");
    nativeFailure.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    SaveResult(nativeFailure, false);
    nativeFailure.TickHostCheckpointSave();
    Check(nativeFailure.actions.empty() && nativeFailure.m_currentHostSaveStateKey == "save_A",
        "failed native save neither captures nor advances active checkpoint");
    Check(nativeFailure.m_lastSaveLoadPath == "save_A" &&
        nativeFailure.BuildHostSaveStateKey(nativeFailure.m_lastSaveLoadPath) == "save_a",
        "failed existing-B overwrite leaves rehost fingerprint input bound to A, never B");
    ModMain internal;
    internal.m_networkMode = CoopNetworkMode::Host;
    internal.m_hostInternalSnapshotSaveActive = true;
    internal.RequestCheckpointSave("CoopHostSnapshot", true, true);
    SaveResult(internal);
    internal.TickHostCheckpointSave();
    Check(internal.actions.empty(), "internal Host transfer snapshot is not a native user checkpoint");
    ModMain localClientSave;
    localClientSave.m_clientInternalPlayerSnapshotSaveActive = true;
    localClientSave.RequestCheckpointSave("save_B");
    SaveResult(localClientSave);
    localClientSave.TickHostCheckpointSave();
    Check(localClientSave.actions.empty(), "Client native snapshot cannot publish Host checkpoint identity");
    ModMain fallbackPath;
    fallbackPath.m_networkMode = CoopNetworkMode::Host;
    fallbackPath.m_lastSaveLoadPath = "save_fallback";
    fallbackPath.RequestCheckpointSave(nullptr);
    fallbackPath.OnCryActionSaveGameReturned(nullptr, false, true, 0, false, "checkpoint", true);
    fallbackPath.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_fallback/save.CSF", true);
    fallbackPath.TickHostCheckpointSave();
    Check(fallbackPath.m_currentHostSaveStateKey == "save_fallback",
        "native save return without path uses the tracked native path");
    ModMain wrongSlot;
    wrongSlot.m_networkMode = CoopNetworkMode::Host;
    wrongSlot.RequestCheckpointSave("save_B");
    SaveResult(wrongSlot);
    wrongSlot.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign1/save_B/save.CSF", true);
    wrongSlot.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/CoopHostSnapshot/save.CSF", true);
    wrongSlot.TickHostCheckpointSave();
    Check(wrongSlot.actions.empty() && !wrongSlot.m_hostCheckpointSave.writeComplete,
        "other campaign and internal snapshot completions cannot unlock the pending checkpoint");
    wrongSlot.m_hostCheckpointSave.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    wrongSlot.TickHostCheckpointSave();
    Check(wrongSlot.actions == std::vector<std::string>({"stop", "hud"}),
        "missing exact writer completion fails closed, never guesses success at a timeout");
    ModMain writeFailure;
    writeFailure.m_networkMode = CoopNetworkMode::Host;
    writeFailure.RequestCheckpointSave("save_B");
    SaveResult(writeFailure);
    writeFailure.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", false);
    writeFailure.TickHostCheckpointSave();
    Check(writeFailure.actions == std::vector<std::string>({"stop", "hud"}),
        "native final write failure stops before carrying live data or publishing identity");
    ModMain overlap;
    overlap.m_networkMode = CoopNetworkMode::Host;
    overlap.RequestCheckpointSave("save_B");
    overlap.RequestCheckpointSave("save_C");
    SaveResult(overlap);
    overlap.TickHostCheckpointSave();
    Check(overlap.actions == std::vector<std::string>({"stop", "hud"}),
        "overlapping forced saves cannot silently discard an earlier pending checkpoint");
    ModMain cancelled;
    cancelled.m_networkMode = CoopNetworkMode::Host;
    cancelled.RequestCheckpointSave("save_B");
    SaveResult(cancelled);
    cancelled.ResetPendingHostCheckpointSave();
    cancelled.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    cancelled.TickHostCheckpointSave();
    Check(cancelled.actions.empty() && cancelled.m_hostCheckpointSave.path.empty(),
        "explicit load/session reset fences a late old-world completion");
    ModMain changedCampaign;
    changedCampaign.m_networkMode = CoopNetworkMode::Host;
    changedCampaign.RequestCheckpointSave("save_B");
    SaveResult(changedCampaign);
    changedCampaign.ObserveHostCheckpointSaveWrite("%USER%/SaveGames/Campaign0/save_B/save.CSF", true);
    changedCampaign.campaignGuidChanged = true;
    changedCampaign.TickHostCheckpointSave();
    Check(changedCampaign.actions == std::vector<std::string>({"stop", "hud"}),
        "canonical campaign identity must still match before publishing native completion");
    ModMain unknownCampaign;
    unknownCampaign.m_networkMode = CoopNetworkMode::Host;
    campaignKnown = false;
    unknownCampaign.RequestCheckpointSave("save_B");
    campaignKnown = true;
    unknownCampaign.TickHostCheckpointSave();
    Check(unknownCampaign.actions == std::vector<std::string>({"stop", "hud"}),
        "bare slot with unknown campaign cannot accept a same-named unrelated completion");

    using Command = CoopProtocol::PlayerStateTransferCommand;
    TimelinePacket packet;
    packet.timelineId = 10;
    ModMain staleStart;
    Check(!staleStart.AcceptTimelinePacket(packet, Command::Start, "save_A", true) &&
        staleStart.m_currentHostTimelineId == 11 && staleStart.actions.empty(),
        "old authoritative Start cannot replace an established Host lineage");
    packet.timelineId = 11;
    Check(staleStart.AcceptTimelinePacket(packet, Command::Start, "save_A", true),
        "current authoritative Start remains accepted");
    packet.timelineId = 0;
    Check(!staleStart.AcceptTimelinePacket(packet, Command::Start, "save_A", true),
        "zero-lineage Start is rejected");
    ModMain bootstrap;
    bootstrap.m_currentHostTimelineId = 0;
    bootstrap.m_currentHostSaveStateKey = "unknown_save";
    packet.timelineId = 11;
    packet.totalBytes = 0;
    Check(!bootstrap.AcceptTimelinePacket(packet, Command::Start, "save_A", true) &&
        bootstrap.m_currentHostTimelineId == 0,
        "empty bootstrap Start cannot establish lineage");
    packet.totalBytes = 128;
    Check(!bootstrap.AcceptTimelinePacket(packet, Command::Start, "save_A", false) &&
        bootstrap.m_currentHostTimelineId == 0,
        "non-authoritative bootstrap Start cannot establish lineage");
    Check(bootstrap.AcceptTimelinePacket(packet, Command::Start, "save_A", true) &&
        bootstrap.m_currentHostTimelineId == 11 && bootstrap.actions.empty(),
        "valid first Start establishes unknown Host lineage without inventing a rollback");
    packet.timelineId = 12;
    Check(bootstrap.AcceptTimelinePacket(packet, Command::HostSaveIdentity, "save_A", false) &&
        bootstrap.m_currentHostTimelineId == 12 && bootstrap.m_clientHostTimelineRestorePending &&
        bootstrap.actions == std::vector<std::string>({"cancel"}),
        "new Host identity fences the abandoned lineage before accepting the replacement");

    TransportHarness transport;
    TransportHarness::Peer peer;
    const auto endpoint = TransportHarness::MakeEndpointKey(peer.address, peer.port);
    const auto otherEndpoint = TransportHarness::MakeEndpointKey(2, peer.port);
    transport.m_reliableEndpointStates[endpoint] = {8, 4, 7, {}};
    transport.m_reliableEndpointStates[otherEndpoint] = {};
    transport.m_reliableSendQueue = {{peer.address, peer.port}, {2, peer.port}};
    TransportHarness::ReliablePacket reorderedPayload;
    reorderedPayload.worldEpoch = 2;
    reorderedPayload.reliableSequence = 6;
    Check(transport.m_reliableEndpointStates[endpoint].reorderedPackets.Store(4, 6, reorderedPayload),
        "warm-join fixture retains a real out-of-order current-world packet");
    transport.ObservePeerWorldChange(true, peer);
    Check(peer.lastPoseSequence == 0 && transport.smoothingResets == 1,
        "warm peer world change still resets pose smoothing");
    const auto& frontier = transport.m_reliableEndpointStates.at(endpoint);
    Check(frontier.recvSequence == 4 && frontier.sendSequence == 8 && frontier.ackedSequence == 7,
        "loaded single-player epoch1 to Host epoch2 preserves session transport frontiers");
    Check(frontier.reorderedPackets.Size() == 1 && transport.m_reliableSendQueue.size() == 2,
        "world change retains buffered incoming and unacknowledged outgoing packets");
    Check(CoopSerialSequence::Next(frontier.recvSequence) == 5,
        "recovery upload sequence5 follows the four already-acknowledged warm-join controls");
    TransportHarness::ReliablePacket stalePayload;
    transport.ReceiveWorldPayload(stalePayload, peer.address, peer.port);
    Check(transport.applied == 0 && transport.m_reliableRetiredAreaPackets == 1,
        "old epoch payload is retired without applying stale world state");
    Check(transport.m_reliableEndpointStates.at(endpoint).recvSequence == 5 &&
        transport.m_reliableRecvSequence == 5 && transport.acknowledgements == 1 && transport.drains == 1,
        "stale payload advances and acknowledges the intact stream before draining reorder");
    TransportHarness::ReliablePacket retainedPayload;
    Check(transport.m_reliableEndpointStates[endpoint].reorderedPackets.TakeNext(5, retainedPayload) &&
        retainedPayload.reliableSequence == 6 && retainedPayload.worldEpoch == 2,
        "retained current-world sequence6 remains drainable once stale sequence5 is retired");
    TransportHarness::ReliablePacket currentPayload;
    currentPayload.worldEpoch = 2;
    currentPayload.reliableSequence = 6;
    transport.ReceiveWorldPayload(currentPayload, peer.address, peer.port);
    Check(transport.applied == 1 && transport.m_reliableRetiredAreaPackets == 1,
        "current epoch payload remains eligible for the normal dispatch path");
    transport.RemovePeerTransport(peer);
    Check(transport.m_reliableEndpointStates.count(endpoint) == 0 &&
        transport.m_reliableEndpointStates.count(otherEndpoint) == 1,
        "genuine peer removal clears only the departed incarnation's endpoint");
    Check(transport.m_reliableSendQueue.size() == 1 && transport.m_reliableSendQueue[0].address == 2,
        "genuine peer removal discards only that incarnation's outstanding packets");
    transport.m_reliableEndpointStates[endpoint] = {};
    Check(CoopSerialSequence::Next(transport.m_reliableEndpointStates.at(endpoint).recvSequence) == 1,
        "new process incarnation admits sequence1 after its actual endpoint reset");
    TransportHarness clientTransport;
    clientTransport.m_networkMode = CoopNetworkMode::Client;
    clientTransport.m_reliableEndpointStates[endpoint] = {8, 4, 7, {}};
    clientTransport.ObservePeerWorldChange(true, peer);
    Check(clientTransport.replicationResets == 1 && clientTransport.detachments == 1 &&
        clientTransport.m_reliableEndpointStates.at(endpoint).recvSequence == 4,
        "Client Host-epoch change retains transport while resetting world replication");
    std::cout << checks << " selected-production timeline barrier controls passed\n";
}
