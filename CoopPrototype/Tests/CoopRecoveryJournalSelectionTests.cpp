#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "../Src/CoopProtocol.h"

namespace CoopFilesystem {
std::filesystem::path FromUtf8(const std::string& value) { return value; }
std::string ToUtf8(const std::filesystem::path& value) { return value.string(); }
}
bool StartsWith(std::string_view value, std::string_view prefix) { return value.substr(0, prefix.size()) == prefix; }
std::string AfterPrefix(std::string_view value, std::string_view prefix) { return std::string(value.substr(prefix.size())); }
std::string HexPlayerAccountToken(uint64_t value) {
    std::ostringstream output; output << std::hex << std::setw(16) << std::setfill('0') << value; return output.str();
}
bool ReadAndValidatePlayerSidecarFile(const std::string& path, std::string& payload, bool& hasHash, std::string&) {
    std::ifstream file(path, std::ios::binary);
    payload.assign(std::istreambuf_iterator<char>(file), {});
    const std::string footer = "fixtureIntegrity=verified\n";
    hasHash = payload.size() >= footer.size() && payload.compare(payload.size() - footer.size(), footer.size(), footer) == 0;
    if (hasHash) payload.resize(payload.size() - footer.size());
    return file.good() || file.eof();
}
std::string failWritePath;
unsigned writeCalls = 0;
bool WritePlayerSidecarPayloadWithHash(const std::filesystem::path& path, const std::string& payload) {
    ++writeCalls;
    if (path.string() == failWritePath) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << payload << "fixtureIntegrity=verified\n";
    return file.good();
}
#include "RecoveryMetadataActual.inc"
using RecoveryStateMetadata = RecoveryJournalMetadata;
bool ReadRecoveryStateMetadata(const std::filesystem::path& path, RecoveryStateMetadata& metadata) {
    return ParseRecoveryJournalMetadata(path.string(), metadata);
}
std::string Hex64(uint64_t value) { return HexPlayerAccountToken(value); }
uint64_t diskRevisionFloor = 0;
uint64_t GetRecoveryJournalDiskMaxRevision(const std::filesystem::path&, uint64_t) { return diskRevisionFloor; }

enum class CoopNetworkMode { Off, Client, Host };
class ModMain {
public:
    struct PlayerSidecarState {};
    CoopNetworkMode m_networkMode = CoopNetworkMode::Client;
    uint64_t m_currentHostTimelineId = 30;
    uint64_t m_localInventoryJournalRevision = 0;
    uint64_t m_localInventoryCapturedRevision = 0, m_localInventoryDirtyRevision = 0;
    unsigned captureCalls = 0, backupCalls = 0;
    bool overwriteCanonicalOnCapture = false;
    bool m_clientRecoveryJournalPending = false;
    bool m_clientRecoveryJournalAwaitingStoredAck = false, m_clientRecoveryJournalStoredAckReceived = false;
    uint64_t m_clientRecoveryJournalTransferId = 0, m_clientRecoveryJournalChecksum = 0, m_clientRecoveryJournalRevision = 0;
    uint64_t m_clientRecoveryJournalHostAccountToken = 0, m_localInventoryDirtyHostAccountToken = 0;
    std::string m_clientRecoveryJournalSourcePath, m_clientRecoveryJournalSaveKey;
    std::string m_localInventoryDirtySaveKey, m_lastPlayerSidecarEvent, m_lastPlayerStateTransferEvent;
    std::string m_currentHostSaveStateKey = "0x11111111_campaign0_0x1234567890ABCDEF_manual0";
    std::filesystem::path root;
    std::string loadedPayload;
    bool dirty = false;
    uint64_t GetLocalAccountToken() const { return 10; }
    uint64_t GetSessionHostAccountToken() const { return 20; }
    std::filesystem::path GetCoopPlayerStateRoot() const { return root; }
    std::string GetPlayerSidecarPath() const { return (root / "player_account_000000000000000a.state").string(); }
    std::string JournalPath(const std::string& key) const {
        return (root / ("player_recovery_account_000000000000000a_host_0000000000000014_" + key + ".state")).string();
    }
    std::string GetPlayerSidecarRecoveryJournalPath() const { return JournalPath(m_currentHostSaveStateKey); }
    bool IsLocalInventoryDirtyForSaveKey(const std::string&) const { return dirty; }
    bool LoadPlayerSidecarFromPath(const std::string& path, PlayerSidecarState&) {
        bool hash = false; std::string reason;
        return ReadAndValidatePlayerSidecarFile(path, loadedPayload, hash, reason) && hash &&
            loadedPayload.find("inventoryCount=1\n") != std::string::npos;
    }
    void LogCoop(const std::string&) {}
    bool SaveLocalPlayerSidecar(const char*) {
        ++captureCalls;
        return !overwriteCanonicalOnCapture || WritePlayerSidecarPayloadWithHash(GetPlayerSidecarPath(), "inventoryCount=1\nfixtureNativeCount=999\n");
    }
    bool WriteLocalPlayerRecoveryJournal(const char*) { ++backupCalls; return true; }
    void StopCaptureSubset(bool preserveClientInventoryRecovery) {
#include "StopNetworkCaptureActual.inc"
    }
    bool CaptureWithOfflineGuard(const char* reason) {
        (void)reason;
        const std::string pathString = GetPlayerSidecarPath();
#include "OfflineRecoveryGuardActual.inc"
        return WritePlayerSidecarPayloadWithHash(pathString, "inventoryCount=1\nfixtureNativeCount=999\n");
    }
    bool CaptureRevisionSection(std::string& serialized) {
        std::ostringstream output;
        const std::string pathString = GetPlayerSidecarPath();
#include "SnapshotRevisionActual.inc"
        serialized = output.str();
        return true;
    }
    bool RetryCurrentCapture() {
        const uint64_t capturedRevision = m_localInventoryDirtyRevision;
        const char* reason = "test optional backup retry";
#include "SnapshotRetryActual.inc"
        return sidecarSaved;
    }
    bool IsStoredUpload(const std::filesystem::path& scopedDest, uint64_t accountToken,
        const RecoveryStateMetadata& incomingRecovery, bool incomingIsRecovery = true) {
#include "CarriedSnapshotWatermarkActual.inc"
        return recoveryAlreadyStored;
    }
    bool RecoverLocalPlayerRecoveryJournal(const char*);
    bool ClearLocalPlayerRecoveryJournal(const std::string&, uint64_t, const std::string&, uint64_t, const char*);
};
#include "RecoverySelectionActual.inc"

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Payload(const ModMain& mod, uint64_t revision, int inventory = 19, int receipt = 1,
    const std::string& key = {}, uint64_t timeline = 30, uint64_t host = 20, uint64_t account = 10) {
    std::ostringstream output;
    output << "inventoryCount=1\nfixtureNativeCount=" << inventory << "\nsharedDropDelivery=" << receipt
        << "\nrecoveryAccount=" << HexPlayerAccountToken(account) << "\nrecoveryHost=" << HexPlayerAccountToken(host)
        << "\nrecoveryTimeline=" << HexPlayerAccountToken(timeline) << "\nrecoverySaveKey="
        << (key.empty() ? mod.m_currentHostSaveStateKey : key) << "\nrecoveryRevision=" << revision << '\n';
    return output.str();
}
void Store(const std::string& path, const std::string& payload) {
    const auto savedFailure = failWritePath; failWritePath.clear();
    Require(WritePlayerSidecarPayloadWithHash(path, payload), "fixture write failed");
    failWritePath = savedFailure;
}
std::string Read(const std::string& path) {
    std::string payload, reason; bool hash = false;
    Require(ReadAndValidatePlayerSidecarFile(path, payload, hash, reason) && hash, "fixture read failed");
    return payload;
}
struct Fixture {
    ModMain mod;
    explicit Fixture(const std::filesystem::path& root) {
        mod.root = root; std::filesystem::remove_all(root); std::filesystem::create_directories(root);
        failWritePath.clear(); writeCalls = 0; diskRevisionFloor = 0;
    }
    void Canonical(uint64_t revision, int count = 19, int receipt = 1) { Store(mod.GetPlayerSidecarPath(), Payload(mod, revision, count, receipt)); }
    void Journal(uint64_t revision, int count = 20, int receipt = 0) { Store(mod.GetPlayerSidecarRecoveryJournalPath(), Payload(mod, revision, count, receipt)); }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root = argv[1];
    unsigned index = 0;
    const std::vector<std::pair<const char*, std::function<void(Fixture&)>>> tests = {
        {"newer_scoped_canonical_beats_stale_backup", [](Fixture& f) {
            f.Canonical(9); f.Journal(8);
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test"), "recovery failed");
            Require(f.mod.m_clientRecoveryJournalSourcePath == f.mod.GetPlayerSidecarPath() &&
                f.mod.m_clientRecoveryJournalRevision == 9 && f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "stale journal replaced newer canonical inventory/receipt pair");
        }},
        {"newer_backup_beats_canonical", [](Fixture& f) {
            f.Canonical(8); f.Journal(9, 18, 2);
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && f.mod.m_clientRecoveryJournalRevision == 9 &&
                f.mod.m_clientRecoveryJournalSourcePath == f.mod.GetPlayerSidecarRecoveryJournalPath(), "highest backup not selected");
        }},
        {"legacy_canonical_never_auto_recovers", [](Fixture& f) {
            Store(f.mod.GetPlayerSidecarPath(), "inventoryCount=1\nfixtureNativeCount=100\n");
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test"), "unscoped legacy canonical accepted");
            f.Journal(1);
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && f.mod.m_clientRecoveryJournalRevision == 1, "valid scoped backup ignored");
        }},
        {"wrong_timeline_host_account_rejected", [](Fixture& f) {
            f.Journal(1);
            for (const auto& scope : std::vector<std::vector<uint64_t>>{{29,20,10},{0,20,10},{30,21,10},{30,20,11}}) {
                Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 99, 99, 1, {}, scope[0], scope[1], scope[2]));
                Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && f.mod.m_clientRecoveryJournalRevision == 1, "foreign canonical scope accepted");
            }
        }},
        {"same_timeline_forward_slot_rebinds_exact_pair", [](Fixture& f) {
            const std::string oldKey = "0x22222222_campaign0_0x1234567890ABCDEF_manual1";
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 8, 19, 1, oldKey));
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test"), "forward snapshot not selected");
            RecoveryJournalMetadata metadata;
            Require(ParseRecoveryJournalMetadata(f.mod.m_clientRecoveryJournalSourcePath, metadata) && metadata.revision == 8 &&
                metadata.saveKey == f.mod.m_currentHostSaveStateKey && f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "rebind changed revision or inventory/receipt");
        }},
        {"optional_backup_failure_uses_scoped_canonical", [](Fixture& f) {
            f.Canonical(7); failWritePath = f.mod.GetPlayerSidecarRecoveryJournalPath(); writeCalls = 0;
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && writeCalls == 0 &&
                f.mod.m_clientRecoveryJournalSourcePath == f.mod.GetPlayerSidecarPath(), "backup write wrongly mandatory");
        }},
        {"forward_rebind_survives_optional_backup_write_failure", [](Fixture& f) {
            const std::string oldKey = "0x22222222_campaign0_0x1234567890ABCDEF_manual1";
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 8, 19, 1, oldKey));
            failWritePath = f.mod.GetPlayerSidecarRecoveryJournalPath();
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && f.mod.m_clientRecoveryJournalSourcePath == f.mod.GetPlayerSidecarPath(),
                "canonical fallback rebind blocked by optional backup failure");
            RecoveryJournalMetadata metadata;
            Require(ParseRecoveryJournalMetadata(f.mod.GetPlayerSidecarPath(), metadata) && metadata.saveKey == f.mod.m_currentHostSaveStateKey &&
                metadata.revision == 8, "canonical rebind lost exact revision");
        }},
        {"equal_revision_conflicting_pair_fails_closed", [](Fixture& f) {
            f.Canonical(8); f.Journal(8);
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test") && !f.mod.m_clientRecoveryJournalPending,
                "divergent same-revision pair was chosen arbitrarily");
        }},
        {"older_conflict_cannot_block_strictly_newer_pair", [](Fixture& f) {
            f.Journal(1); Store(f.mod.JournalPath("0x22222222_campaign0_0x1234567890ABCDEF_manual1"),
                Payload(f.mod, 1, 30, 0, "0x22222222_campaign0_0x1234567890ABCDEF_manual1")); f.Canonical(2);
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && f.mod.m_clientRecoveryJournalRevision == 2, "obsolete conflicting revisions blocked newest");
        }},
        {"different_campaign_timeline_cannot_rebind", [](Fixture& f) {
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 9, 19, 1,
                "0x22222222_campaign1_0x1234567890ABCDEF_manual1", 29));
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test"), "foreign campaign timeline accepted");
        }},
        {"save_label_guid_does_not_override_verified_host_timeline", [](Fixture& f) {
            const std::string oldKey = "0x22222222_campaign1_0xFEDCBA9876543210_manual1";
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 10, 19, 1, oldKey));
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("forward"), "display GUID overrode exact durable lineage");
            Require(f.mod.m_clientRecoveryJournalRevision == 10 &&
                f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "display GUID change lost the newer paired snapshot");
        }},
        {"native_custom_forward_save_recovers_canonical_only19", [](Fixture& f) {
            const std::string oldKey = "0xCFDE2E9B_campaign0_coopcheckpointlivea-v20-20261110";
            f.mod.m_currentHostSaveStateKey = "0xC016BF0B_campaign0_coopofflinedrop-20261110-204131165";
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 16, 19, 1, oldKey));
            failWritePath = f.mod.GetPlayerSidecarRecoveryJournalPath();
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("native warm join"), "custom save label rejected same durable timeline");
            RecoveryJournalMetadata metadata;
            Require(ParseRecoveryJournalMetadata(f.mod.GetPlayerSidecarPath(), metadata) && metadata.revision == 16 &&
                metadata.saveKey == f.mod.m_currentHostSaveStateKey && f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "custom forward save changed paired inventory, receipt, or revision");
        }},
        {"truncated_save_labels_do_not_replace_durable_lineage", [](Fixture& f) {
            const std::string oldKey = "0x11111111_very-long-custom-slot-without-campaign-prefix";
            f.mod.m_currentHostSaveStateKey = "0x22222222_another-long-custom-slot-without-campaign-prefix";
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 9, 19, 1, oldKey));
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("forward"), "truncated labels rejected same scoped timeline");
            Require(f.mod.m_clientRecoveryJournalRevision == 9 && f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "truncated labels altered selected inventory/receipt pair");
        }},
        {"invalid_capture_and_current_save_keys_cannot_rebind", [](Fixture& f) {
            const auto currentKey = f.mod.m_currentHostSaveStateKey;
            for (const std::string& key : {std::string("../escape"), std::string(".."), std::string("unknown_save"),
                     std::string(64, 'x'), std::string("slot with spaces"), std::string("C:drive")}) {
                Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 9, 19, 1, key));
                Require(!f.mod.RecoverLocalPlayerRecoveryJournal("invalid capture"), "invalid snapshot key accepted");
                f.Canonical(9); f.mod.m_currentHostSaveStateKey = key;
                Require(!f.mod.RecoverLocalPlayerRecoveryJournal("invalid current"), "invalid advertised key accepted");
                f.mod.m_currentHostSaveStateKey = currentKey;
            }
        }},
        {"symlink_candidate_rejected", [](Fixture& f) {
            const auto other = f.mod.root / "not-a-candidate"; Store(other.string(), Payload(f.mod, 9));
            std::filesystem::create_symlink(other, f.mod.GetPlayerSidecarPath());
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test"), "canonical symlink accepted");
        }},
        {"previous_pending_timeline_is_reset", [](Fixture& f) {
            const auto previous = f.mod.root / "previous.state"; Store(previous.string(), Payload(f.mod, 8, 20, 0, {}, 29));
            f.mod.m_clientRecoveryJournalPending = true; f.mod.m_clientRecoveryJournalSourcePath = previous.string();
            f.mod.m_clientRecoveryJournalSaveKey = f.mod.m_currentHostSaveStateKey; f.mod.m_clientRecoveryJournalHostAccountToken = 20;
            f.mod.m_clientRecoveryJournalAwaitingStoredAck = true; f.mod.m_clientRecoveryJournalRevision = 8; f.Canonical(9);
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("test") && !f.mod.m_clientRecoveryJournalAwaitingStoredAck &&
                f.mod.m_clientRecoveryJournalRevision == 9 && f.mod.m_clientRecoveryJournalSourcePath == f.mod.GetPlayerSidecarPath(), "stale timeline transport retained");
        }},
        {"canonical_ack_retains_inventory_removes_only_older_backup", [](Fixture& f) {
            f.Canonical(9); f.Journal(8);
            Require(f.mod.ClearLocalPlayerRecoveryJournal(f.mod.GetPlayerSidecarPath(), 10, f.mod.m_currentHostSaveStateKey, 9, "ack") &&
                std::filesystem::exists(f.mod.GetPlayerSidecarPath()) && !std::filesystem::exists(f.mod.GetPlayerSidecarRecoveryJournalPath()),
                "canonical ACK deleted inventory or retained stale backup");
        }},
        {"canonical_ack_cannot_delete_newer_backup", [](Fixture& f) {
            f.Canonical(8); f.Journal(9);
            Require(f.mod.ClearLocalPlayerRecoveryJournal(f.mod.GetPlayerSidecarPath(), 10, f.mod.m_currentHostSaveStateKey, 8, "ack") &&
                std::filesystem::exists(f.mod.GetPlayerSidecarRecoveryJournalPath()), "older ACK deleted newer journal");
        }},
        {"old_revision_ack_cannot_retire_mutated_canonical", [](Fixture& f) {
            f.Canonical(9); f.Journal(8);
            Require(!f.mod.ClearLocalPlayerRecoveryJournal(f.mod.GetPlayerSidecarPath(), 10, f.mod.m_currentHostSaveStateKey, 8, "ack") &&
                std::filesystem::exists(f.mod.GetPlayerSidecarPath()) && std::filesystem::exists(f.mod.GetPlayerSidecarRecoveryJournalPath()), "old ACK deleted current state");
        }},
        {"dirty_mutation_blocks_retirement", [](Fixture& f) {
            f.Canonical(9); f.Journal(9, 19, 1); f.mod.dirty = true;
            Require(!f.mod.ClearLocalPlayerRecoveryJournal(f.mod.GetPlayerSidecarPath(), 10, f.mod.m_currentHostSaveStateKey, 9, "ack") &&
                std::filesystem::exists(f.mod.GetPlayerSidecarRecoveryJournalPath()), "in-flight mutation lost backup");
        }},
        {"arbitrary_scoped_file_is_not_deletion_target", [](Fixture& f) {
            const auto other = f.mod.root / "other.state"; Store(other.string(), Payload(f.mod, 9));
            Require(!f.mod.ClearLocalPlayerRecoveryJournal(other.string(), 10, f.mod.m_currentHostSaveStateKey, 9, "ack") &&
                std::filesystem::exists(other), "unexpected arbitrary source deleted");
        }},
        {"journal_ack_removes_only_exact_backup", [](Fixture& f) {
            f.Canonical(9); f.Journal(9, 19, 1);
            Require(f.mod.ClearLocalPlayerRecoveryJournal(f.mod.GetPlayerSidecarRecoveryJournalPath(), 10, f.mod.m_currentHostSaveStateKey, 9, "ack") &&
                !std::filesystem::exists(f.mod.GetPlayerSidecarRecoveryJournalPath()) && std::filesystem::exists(f.mod.GetPlayerSidecarPath()), "journal ACK affected canonical");
        }},
        {"canonical_requires_integrity_dependency", [](Fixture& f) {
            std::ofstream file(f.mod.GetPlayerSidecarPath()); file << Payload(f.mod, 9); file.close();
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test"), "unverified canonical auto-restored");
        }},
        {"revision_floor_strict_decimal_parser", [](Fixture& f) {
            const std::vector<std::string> invalid = {"", "0", "-1", "+1", "1x", "1 ", "18446744073709551616", "1\nrecoveryRevision=2"};
            for (const auto& value : invalid) {
                Store(f.mod.GetPlayerSidecarPath(), "recoveryRevision=" + value + "\n");
                Require(ReadPlayerSidecarSnapshotRevision(f.mod.GetPlayerSidecarPath()) == 0, "malformed floor accepted");
            }
            Store(f.mod.GetPlayerSidecarPath(), "recoveryRevision=18446744073709551615\r\n");
            Require(ReadPlayerSidecarSnapshotRevision(f.mod.GetPlayerSidecarPath()) == std::numeric_limits<uint64_t>::max(), "valid maximum floor lost");
        }},
        {"offline_capture_preserves_floor_without_recovery_scope", [](Fixture& f) {
            Store(f.mod.GetPlayerSidecarPath(), "inventoryCount=1\nrecoveryRevision=50\n");
            f.mod.m_networkMode = CoopNetworkMode::Off; f.mod.m_currentHostTimelineId = 0;
            std::string section;
            Require(f.mod.CaptureRevisionSection(section) && section == "recoveryRevision=50\n", "offline capture relabeled scope or lost floor");
            Store(f.mod.GetPlayerSidecarPath(), "inventoryCount=1\nfixtureNativeCount=100\n" + section);
            f.mod.m_networkMode = CoopNetworkMode::Client; f.mod.m_currentHostTimelineId = 30;
            Require(!f.mod.RecoverLocalPlayerRecoveryJournal("test"), "offline inventory became restore-eligible");
            Require(f.mod.CaptureRevisionSection(section) && section.find("recoveryRevision=51\n") != std::string::npos,
                "fresh scoped capture reused older accepted revision after offline restart");
        }},
        {"snapshot_floor_uses_highest_durable_or_memory_value", [](Fixture& f) {
            f.Canonical(5); f.mod.m_localInventoryJournalRevision = 8; diskRevisionFloor = 10;
            std::string section;
            Require(f.mod.CaptureRevisionSection(section) && section.find("recoveryRevision=11\n") != std::string::npos, "revision did not exceed every floor");
            diskRevisionFloor = std::numeric_limits<uint64_t>::max();
            Require(!f.mod.CaptureRevisionSection(section), "revision overflow wrapped");
        }},
        {"unchanged_backup_retry_reuses_canonical_revision", [](Fixture& f) {
            f.Canonical(9); f.mod.m_localInventoryJournalRevision = 9;
            f.mod.m_localInventoryCapturedRevision = f.mod.m_localInventoryDirtyRevision = 7;
            for (unsigned retry = 0; retry < 20; ++retry) Require(f.mod.RetryCurrentCapture(), "current capture unavailable");
            Require(f.mod.captureCalls == 0, "optional backup retries advanced canonical revision and starved ACKs");
            ++f.mod.m_localInventoryDirtyRevision;
            Require(f.mod.RetryCurrentCapture() && f.mod.captureCalls == 1, "new native mutation reused stale canonical");
        }},
        {"changed_scope_cannot_reuse_captured_dirty_revision", [](Fixture& f) {
            f.Canonical(9); f.mod.m_localInventoryJournalRevision = 9;
            f.mod.m_localInventoryCapturedRevision = f.mod.m_localInventoryDirtyRevision = 7;
            ++f.mod.m_currentHostTimelineId;
            Require(f.mod.RetryCurrentCapture() && f.mod.captureCalls == 1, "foreign timeline canonical reused for backup retry");
        }},
        {"carried_old_slot_preserves_current_scoped_watermark", [](Fixture& f) {
            const std::string oldKey = "0x22222222_campaign0_0x1234567890ABCDEF_manual1";
            const auto currentScopedDest = f.mod.root / "live_current" / "player_account_000000000000003c.state";
            Store(currentScopedDest.string(), Payload(f.mod, 10, 19, 1, oldKey, 30, 10, 60));
            RecoveryStateMetadata incoming; incoming.revision = 9;
            Require(f.mod.IsStoredUpload(currentScopedDest, 60, incoming), "carried previous-slot label reset accepted revision");
            incoming.revision = 10;
            Require(f.mod.IsStoredUpload(currentScopedDest, 60, incoming), "equal accepted revision was not idempotent");
            incoming.revision = 11;
            Require(!f.mod.IsStoredUpload(currentScopedDest, 60, incoming), "newer revision was incorrectly suppressed");
        }},
        {"carried_watermark_requires_recipient_host_and_timeline", [](Fixture& f) {
            const auto currentScopedDest = f.mod.root / "live_current" / "player_account_000000000000003c.state";
            RecoveryStateMetadata incoming; incoming.revision = 9;
            for (const auto& scope : std::vector<std::vector<uint64_t>>{{29,10,60},{30,11,60},{30,10,61}}) {
                Store(currentScopedDest.string(), Payload(f.mod, 10, 19, 1, {}, scope[0], scope[1], scope[2]));
                Require(!f.mod.IsStoredUpload(currentScopedDest, 60, incoming), "foreign carried watermark suppressed valid upload");
            }
            Require(!f.mod.IsStoredUpload({}, 60, incoming), "missing current scoped path supplied a watermark");
        }},
        {"carried_watermark_is_not_applied_to_unscoped_upload", [](Fixture& f) {
            const auto currentScopedDest = f.mod.root / "live_current" / "player_account_000000000000003c.state";
            Store(currentScopedDest.string(), Payload(f.mod, 10, 19, 1, {}, 30, 10, 60));
            RecoveryStateMetadata incoming; incoming.revision = 9;
            Require(!f.mod.IsStoredUpload(currentScopedDest, 60, incoming, false), "non-recovery upload treated as scoped metadata");
        }},
        {"already_off_stop_preserves_scoped_crash_recovery", [](Fixture& f) {
            f.Canonical(9); const std::string before = Read(f.mod.GetPlayerSidecarPath());
            f.mod.m_networkMode = CoopNetworkMode::Off; f.mod.overwriteCanonicalOnCapture = true;
            f.mod.StopCaptureSubset(false);
            Require(f.mod.captureCalls == 0 && f.mod.backupCalls == 0 && Read(f.mod.GetPlayerSidecarPath()) == before,
                "joining from loaded singleplayer overwrote the only scoped canonical fallback");
        }},
        {"active_client_stop_still_captures_and_backs_up", [](Fixture& f) {
            f.mod.StopCaptureSubset(true);
            Require(f.mod.captureCalls == 1 && f.mod.backupCalls == 1, "active Client stop lost its final inventory capture");
            f.mod.StopCaptureSubset(false);
            Require(f.mod.captureCalls == 2 && f.mod.backupCalls == 1, "ordinary active Client stop capture was incorrectly skipped");
        }},
        {"offline_load_save_and_stop_cannot_replace_scoped_pair", [](Fixture& f) {
            f.Canonical(9, 19, 1); f.Journal(8, 20, 0);
            const std::string canonical = Read(f.mod.GetPlayerSidecarPath());
            const std::string journal = Read(f.mod.GetPlayerSidecarRecoveryJournalPath());
            f.mod.m_networkMode = CoopNetworkMode::Off; writeCalls = 0;
            for (const char* reason : {"before native load", "native save hook", "network stop", "debug button"}) {
                Require(!f.mod.CaptureWithOfflineGuard(reason) && Read(f.mod.GetPlayerSidecarPath()) == canonical &&
                    Read(f.mod.GetPlayerSidecarRecoveryJournalPath()) == journal && writeCalls == 0,
                    "offline entry erased canonical19 or touched stale journal20");
            }
            f.mod.m_networkMode = CoopNetworkMode::Client;
            Require(f.mod.RecoverLocalPlayerRecoveryJournal("reconnect") && f.mod.m_clientRecoveryJournalRevision == 9 &&
                f.mod.loadedPayload.find("fixtureNativeCount=19\nsharedDropDelivery=1") != std::string::npos,
                "preserved canonical-only recovery was not selected on reconnect");
        }},
        {"offline_guard_does_not_protect_invalid_or_unscoped_files", [](Fixture& f) {
            f.mod.m_networkMode = CoopNetworkMode::Off;
            Store(f.mod.GetPlayerSidecarPath(), "inventoryCount=1\nrecoveryRevision=50\n");
            Require(f.mod.CaptureWithOfflineGuard("native save hook"), "unscoped offline sidecar wrongly protected");
            std::ofstream file(f.mod.GetPlayerSidecarPath()); file << Payload(f.mod, 9); file.close();
            Require(f.mod.CaptureWithOfflineGuard("before native load"), "invalid-integrity canonical wrongly protected");
            Store(f.mod.GetPlayerSidecarPath(), Payload(f.mod, 9, 19, 1, {}, 30, 20, 11));
            Require(f.mod.CaptureWithOfflineGuard("native save hook"), "foreign-account canonical wrongly protected");
        }},
        {"offline_guard_does_not_block_active_host_or_client_capture", [](Fixture& f) {
            f.Canonical(9); f.mod.m_networkMode = CoopNetworkMode::Host;
            Require(f.mod.CaptureWithOfflineGuard("native save hook"), "active Host capture was blocked");
            f.Canonical(10); f.mod.m_networkMode = CoopNetworkMode::Client;
            Require(f.mod.CaptureWithOfflineGuard("inventory mutation"), "active Client capture was blocked");
        }}
    };
    try {
        for (const auto& test : tests) {
            Fixture fixture(root / std::to_string(++index)); test.second(fixture); std::cout << "PASS " << test.first << '\n';
        }
    } catch (const std::exception& error) { std::cerr << "FAIL " << tests[index - 1].first << ": " << error.what() << '\n'; return 1; }
    std::cout << tests.size() << "/" << tests.size() << " actual recovery-selector storage-dependency checks passed\n";
}
