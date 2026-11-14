#include "CoopSharedDropDelivery.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>

enum class CoopNetworkMode { Off, Host, Client };
namespace CoopFilesystem
{
std::filesystem::path FromUtf8(const std::string& value) { return value; }
}

struct ModMain
{
    struct Store
    {
        bool result = true;
        unsigned calls = 0;
        bool StageCheckpoint(const std::string&, std::string&) { ++calls; return result; }
    };
    std::unique_ptr<Store> m_hostPlayerStateStore = std::make_unique<Store>();
    uint64_t m_currentHostTimelineId = 11;
    bool m_sharedDropDeliveryRecoveryRequired = false;
    bool commitWritable = true;
    std::vector<CoopSharedDropDelivery::Entry> stored;
    CoopNetworkMode m_networkMode = CoopNetworkMode::Host;
    uint64_t m_sharedDropHostSessionId = 29;
    std::string payload;
    bool valid = true;
    bool hashed = true;
    bool writable = true;
    struct Peer { std::string username; };
    std::unordered_map<uint64_t, Peer> m_remotePeers;
    std::string ownPath;
    std::vector<CoopSharedDropDelivery::Entry> m_localSharedDropDeliveries;
    struct PlayerSidecarState { std::vector<CoopSharedDropDelivery::Entry> sharedDropDeliveries; };
    void EnsureSharedDropHostSessionId() {}
    bool ReadAndValidatePlayerSidecarFile(const std::string&, std::string& out, bool& hash, std::string&)
    { out = payload; hash = hashed; return valid; }
    bool WritePlayerSidecarPayloadWithHash(const std::filesystem::path&, const std::string& value)
    {
        if (!writable || !commitWritable) return false;
        payload = value;
        bool section = false;
        return CoopSharedDropDelivery::ParseSection(payload, stored, section);
    }
    bool PrepareSharedDropDeliveriesForHostTransfer(const std::string&);
    bool RestoreHostSharedDropDeliveryReceipts();
    uint64_t GetLocalAccountToken() const { return 7; }
    std::string m_currentHostSaveStateKey = "save_A";
    std::string GetHostPlayerStatePathForAccountAndSave(uint64_t, const std::string&) { return ownPath; }
    bool LoadPlayerSidecarFromPath(const std::string&, PlayerSidecarState& state)
    { bool section = false; return CoopSharedDropDelivery::ParseSection(payload, state.sharedDropDeliveries, section); }
    bool StageHostPlayerCheckpoint();
    bool StoreCommittedSharedDropDelivery(uint64_t, const CoopSharedDropDelivery::Entry&);
    bool StoreCommittedSharedDropDeliveries(uint64_t, const std::vector<CoopSharedDropDelivery::Entry>&);
    bool HasContinuingHostInventoryWorld() const { return m_networkMode == CoopNetworkMode::Off; }
    bool WriteDefaultHostPlayerStateFile(const std::string&, const std::string&) { return false; }
    void LogCoop(const std::string&) {}
};

#include "SharedDropSessionRestoreActual.inc"
#include "HostCheckpointStageActual.inc"
#include "SharedDropCommitStoreActual.inc"

unsigned checks = 0;
void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}

std::vector<CoopSharedDropDelivery::Entry> Rows(const ModMain& mod)
{
    std::vector<CoopSharedDropDelivery::Entry> rows;
    bool section = false;
    Check(CoopSharedDropDelivery::ParseSection(mod.payload, rows, section) && section, "valid rewritten receipt section");
    return rows;
}

int main()
{
    ModMain mod;
    std::ostringstream saved;
    saved << "inventory=baseline\n";
    CoopSharedDropDelivery::WriteSection(saved, {{19, 23, 31, 3, 1, 37, 41}}, 19);
    mod.payload = saved.str();
    Check(mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state"), "prepare selected checkpoint");
    auto rows = Rows(mod);
    Check(rows.size() == 1 && rows[0].hostSessionId == 29 && rows[0].deliveredCount == 1 && rows[0].count == 3 &&
        rows[0].stableSpawnId == 23 && rows[0].archetypeId == 31 && rows[0].transactionSequence == 37 && rows[0].objectVersion == 41,
        "checkpoint load preserves exact assigned remainder across transport sessions");
    Check(CoopSharedDropDelivery::WithoutSection(mod.payload) == "inventory=baseline\n", "inventory bytes remain untouched");
    const auto once = mod.payload;
    Check(mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state") && mod.payload == once,
        "duplicate preparation is idempotent");
    mod.payload = saved.str(); mod.writable = false;
    Check(!mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state") && mod.payload == saved.str(),
        "write failure cannot discard pending ownership");
    mod.writable = true; mod.hashed = false;
    Check(!mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state"), "unhashed receipt cannot be rebased");
    mod.hashed = true; mod.m_networkMode = CoopNetworkMode::Client;
    Check(!mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state"), "Client cannot choose its own checkpoint");
    mod.m_networkMode = CoopNetworkMode::Host;
    std::ostringstream completed;
    CoopSharedDropDelivery::WriteSection(completed, {{19, 23, 31, 3, 3, 37, 41}}, 19);
    mod.payload = completed.str();
    Check(mod.PrepareSharedDropDeliveriesForHostTransfer("checkpoint_player.state") && Rows(mod).empty(),
        "completed old-session receipts are not replayed as new grants");

    const auto path = std::filesystem::temp_directory_path() / ("prey-own-receipt-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    { std::ofstream file(path); file << "dependency fixture"; }
    mod.ownPath = path.string(); mod.payload = saved.str();
    Check(mod.RestoreHostSharedDropDeliveryReceipts() && mod.m_localSharedDropDeliveries.size() == 1 &&
        mod.m_localSharedDropDeliveries[0].deliveredCount == 1 && mod.m_localSharedDropDeliveries[0].hostSessionId == 29,
        "Host restores receipts only while native inventory remains independently owned");
    mod.m_networkMode = CoopNetworkMode::Off;
    Check(mod.StageHostPlayerCheckpoint() && mod.stored.size() == 1 && mod.stored[0].deliveredCount == 1 &&
        mod.m_hostPlayerStateStore->calls == 1, "offline continuing Host freezes its actual local delivery progress");
    mod.commitWritable = false;
    Check(!mod.StageHostPlayerCheckpoint() && mod.m_hostPlayerStateStore->calls == 1,
        "failed receipt persistence prevents publishing a misleading native checkpoint");
    mod.commitWritable = true; mod.m_sharedDropDeliveryRecoveryRequired = true;
    Check(!mod.StageHostPlayerCheckpoint() && mod.m_hostPlayerStateStore->calls == 1,
        "unknown native outcome cannot become a checkpoint");
    mod.m_sharedDropDeliveryRecoveryRequired = false;
    mod.m_networkMode = CoopNetworkMode::Host;
    auto one = mod.m_localSharedDropDeliveries.front();
    auto two = one; two.stableSpawnId = 47;
    Check(mod.StoreCommittedSharedDropDeliveries(7, {one, two}) && Rows(mod).size() == 2,
        "Host checkpoint batches receipt progress in one accepted account file");
    const auto batch = mod.payload;
    auto newer = one; newer.deliveredCount = 2;
    auto older = two; older.deliveredCount = 0;
    Check(!mod.StoreCommittedSharedDropDeliveries(7, {newer, older}) && mod.payload == batch,
        "one rejected progress update cannot partially publish a checkpoint batch");
    mod.m_networkMode = CoopNetworkMode::Client;
    Check(!mod.StoreCommittedSharedDropDelivery(7, newer) && mod.payload == batch,
        "Client cannot commit ownership through Host checkpoint storage");
    std::filesystem::remove(path);
    std::cout << checks << " selected-production session/checkpoint receipt checks passed\n";
}
