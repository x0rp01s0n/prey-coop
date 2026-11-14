#include "CoopSharedDropDelivery.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using EntityId = unsigned;
constexpr EntityId INVALID_ENTITYID = 0;
constexpr int ZERO = 0, IDENTITY = 1;
struct Vec3 {
    float x = 1, y = 2, z = 3;
    Vec3() = default;
    explicit Vec3(int) : x(0), y(0), z(0) {}
};
struct Quat { explicit Quat(int) {} };
namespace CoopProtocol {
struct SharedDropPacket {
    uint64_t stableSpawnId = 0, archetypeId = 0, sourcePeerHash = 0;
    uint32_t areaId = 0, objectVersion = 0;
    int count = 0;
    float px = 0, py = 0, pz = 0;
};
}
struct CArkProjectileRecyclerGrenade {};

struct IArkInventory { virtual ~IArkInventory() = default; };
struct ArkInventory : IArkInventory {
    int count = 10;
    bool failRead = false;
    int GetCountOfArchetype(uint64_t) const {
        if (failRead) throw std::runtime_error("inventory unavailable");
        return count;
    }
};
struct IEntity {
    bool hidden = true, invisible = true, physics = false;
    void Hide(bool value) { hidden = value; }
    void Invisible(bool value) { invisible = value; }
    void EnablePhysics(bool value) { physics = value; }
    Vec3 GetWorldPos() const { return {}; }
};
struct EntitySystem {
    std::unordered_map<EntityId, IEntity> entities;
    IEntity* GetEntity(EntityId id) {
        auto found = entities.find(id);
        return found == entities.end() ? nullptr : &found->second;
    }
};
struct Environment { EntitySystem* pEntitySystem = nullptr; };
Environment* gEnv = nullptr;
struct ArkPlayer {
    ArkInventory* m_pInventory = nullptr;
    static inline ArkPlayer* instance = nullptr;
    static ArkPlayer* GetInstancePtr() { return instance; }
    EntityId GetEntityId() const { return 1; }
    IEntity* GetEntity() { return gEnv->pEntitySystem->GetEntity(GetEntityId()); }
};
struct CArkItem {
    EntityId id = 7;
    unsigned nativeCalls = 0;
    int count = 5, acquire = 5;
    bool nativeResult = true, canCollect = true, weapon = false, removeAfterPickup = false;
    std::function<void()> duringPickup;
    static inline std::unordered_map<EntityId, CArkItem*> items;
    static CArkItem* GetItemFromEntityId(EntityId id) {
        auto found = items.find(id);
        return found == items.end() ? nullptr : found->second;
    }
    EntityId GetEntityId() const { return id; }
    IEntity* GetEntity() { return gEnv->pEntitySystem->GetEntity(id); }
    void ResetCount(int value) { count = value; }
    bool CanCollect(const IArkInventory&) const { return canCollect; }
    bool PickUp(EntityId, bool) {
        if (!GetEntity() || GetEntity()->hidden || !GetEntity()->physics)
            throw std::runtime_error("native source must be restored before pickup");
        ++nativeCalls;
        ArkPlayer::instance->m_pInventory->count += std::min(count, acquire);
        if (duringPickup) duringPickup();
        if (removeAfterPickup) {
            gEnv->pEntitySystem->entities.erase(id);
            items.erase(id);
        }
        return nativeResult;
    }
};
namespace CoopRuntimeGuards {
template<class F, class T>
bool TryGuardedCall(const char*, F callback, T& result, std::string*) {
    try { result = callback(); return true; }
    catch (const std::exception&) { return false; }
}
template<class F>
bool TryGuardedVoidCall(const char*, F callback, std::string*) {
    try { callback(); return true; }
    catch (const std::exception&) { return false; }
}
}
namespace CoopItemClassification {
bool IsRealWeaponInventoryItem(CArkItem* item, std::string&) { return item && item->weapon; }
}
enum class CoopNetworkMode { Off, Host, Client };
namespace CoopRuntimeLog { void Write(std::string_view) {} }

class ModMain {
public:
#include "SharedDropRecordActual.inc"
    std::vector<CoopSharedDropDelivery::Entry> m_localSharedDropDeliveries;
    std::unordered_map<uint64_t, SharedDropRecord> m_sharedDrops;
    std::unordered_map<EntityId, uint64_t> m_sharedDropByEntityId;
    std::unordered_set<const CArkProjectileRecyclerGrenade*> m_remoteRecyclerGrenades;
    std::unordered_map<EntityId, CArkItem> materializedItems;
    uint32_t m_localLevelId = 6;
    unsigned materializeCalls = 0;
    bool materializedCanCollect = true;
    uint64_t m_sharedDropHostSessionId = 40;
    bool m_sharedDropDeliveryRecoveryRequired = false;
    CoopNetworkMode m_networkMode = CoopNetworkMode::Client;
    bool m_multiplayerRestartRequired = false;
    unsigned m_sharedDropApplyDepth = 0;
    bool m_enablePlayerSidecar = true;
    bool m_pendingPlayerSidecarInventoryRestore = false, m_pendingPlayerSidecarChipsetRestore = false;
    int m_playerSidecarInventoryPending = 0, m_playerSidecarChipsetsPending = 0;
    bool m_pendingReceivedPlayerStateApply = false, m_clientAwaitingHostPlayerState = false;
    bool m_pendingClientPlayerStateUpload = false;
    std::string m_currentHostSaveStateKey = "fixture_save";
    uint64_t m_localInventoryDirtyRevision = 0, m_localInventoryDirtyHostAccountToken = 0;
    bool m_localInventoryDirty = false;
    std::string m_localInventoryDirtySaveKey, m_lastSharedDropEvent, m_networkStatus, m_sessionStatus;
    float m_localInventoryJournalAccumulator = 0;
    unsigned saves = 0, journals = 0, registrations = 0, uploads = 0;
    unsigned stops = 0;
    unsigned failSaveAt = 0, failJournalAt = 0;
    struct Snapshot { int count = 0; std::vector<CoopSharedDropDelivery::Entry> rows; };
    Snapshot sidecar, recovery;
    uint64_t GetLocalAccountToken() const { return 2; }
    void MarkLocalInventoryDirty(const char*) { m_localInventoryDirty = true; ++m_localInventoryDirtyRevision; }
    bool SaveLocalPlayerSidecar(const char*) {
        ++saves;
        if (m_sharedDropDeliveryRecoveryRequired ||
            std::any_of(m_sharedDrops.begin(), m_sharedDrops.end(), [](const auto& pair)
            { return pair.second.pickupDeliveryInProgress; })) return false;
        if (saves == failSaveAt) return false;
        for (const auto& row : m_localSharedDropDeliveries)
            if (!CoopSharedDropDelivery::IsValid(row)) throw std::runtime_error("invalid receipt capture");
        sidecar = {ArkPlayer::instance->m_pInventory->count, m_localSharedDropDeliveries};
        return true;
    }
    bool WriteLocalPlayerRecoveryJournal(const char*) {
        ++journals;
        if (journals == failJournalAt) return false;
        recovery = sidecar;
        return true;
    }
    void QueueClientPlayerStateUpload(const char* reason, const std::string& saveKey) {
        if (m_networkMode != CoopNetworkMode::Client) return;
        if (std::string_view(reason) != "inventory mutation" || saveKey != m_currentHostSaveStateKey)
            throw std::runtime_error("delivery upload scope mismatch");
        ++uploads;
        m_pendingClientPlayerStateUpload = true;
    }
    bool EnsureLocalPlayerWeaponRegistered(unsigned, const char*) { ++registrations; return true; }
    void StopNetwork() {
        ++stops;
        m_networkMode = CoopNetworkMode::Off;
        m_sharedDrops.clear();
        m_sharedDropByEntityId.clear();
    }
    void DisconnectRemotePeer(const char*) { StopNetwork(); m_multiplayerRestartRequired = false; }
    void QueueCoopHudFeedback(const std::string&, float) {}
    bool PersistLocalSharedDropDelivery(SharedDropRecord&);
    bool TryDeliverCommittedSharedDrop(SharedDropRecord&, bool interactive = true);
    uint64_t BeginCommittedSharedDropInventoryTransfer(CArkItem*, IArkInventory*, int&);
    void EndCommittedSharedDropInventoryTransfer(uint64_t, int, IArkInventory*);
    void ProcessSharedDropDeliveryRecoveryFault();
    void RestoreCommittedSharedDropDeliveries();
    bool ShouldSuppressRecyclerForSharedDrop(const CArkProjectileRecyclerGrenade*, EntityId) const;
    bool MaterializeSharedDrop(const CoopProtocol::SharedDropPacket& packet, SharedDropRecord& record, std::string&) {
        const EntityId id = 100 + ++materializeCalls;
        gEnv->pEntitySystem->entities.emplace(id, IEntity{});
        CArkItem& item = materializedItems[id];
        item.id = id;
        item.count = packet.count;
        item.canCollect = materializedCanCollect;
        CArkItem::items[id] = &item;
        record.stableSpawnId = packet.stableSpawnId;
        record.archetypeId = packet.archetypeId;
        record.count = packet.count;
        record.version = packet.objectVersion;
        record.localEntityId = id;
        m_sharedDropByEntityId[id] = packet.stableSpawnId;
        return true;
    }
};

#include "SharedDropDeliveryActual.inc"
#include "SharedDropDeliverySyncActual.inc"

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    ArkInventory inventory;
    ArkPlayer player;
    EntitySystem entities;
    Environment environment;
    CArkItem item;
    ModMain mod;
    Fixture() {
        player.m_pInventory = &inventory;
        ArkPlayer::instance = &player;
        environment.pEntitySystem = &entities;
        gEnv = &environment;
        entities.entities.emplace(item.id, IEntity{});
        entities.entities.emplace(player.GetEntityId(), IEntity{});
        CArkItem::items[item.id] = &item;
        auto& r = record();
        r.stableSpawnId = 90;
        r.archetypeId = 100;
        r.localEntityId = item.id;
        r.count = 5;
        r.version = 3;
        r.pickupTransactionSequence = 8;
        r.pickupCommitted = true;
        r.pickupPending = true;
        r.pickupWinnerPeerHash = 2;
        mod.m_sharedDropByEntityId[item.id] = r.stableSpawnId;
    }
    ~Fixture() { CArkItem::items.clear(); ArkPlayer::instance = nullptr; gEnv = nullptr; }
    ModMain::SharedDropRecord& record() { return mod.m_sharedDrops[90]; }
    CoopSharedDropDelivery::Entry& receipt() { return mod.m_localSharedDropDeliveries.at(0); }
};

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"full_native_delivery_paired_snapshot", [] {
            Fixture f;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()), "full native delivery failed");
            require(f.item.nativeCalls == 1 && f.inventory.count == 15, "wrong native mutation");
            require(f.receipt().deliveredCount == 5 && f.mod.recovery.count == 15 && f.mod.recovery.rows[0].deliveredCount == 5,
                "inventory and receipt were not saved together");
            require(!f.entities.entities.at(7).physics && f.entities.entities.at(7).hidden, "nonweapon world copy visible");
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1, "repeated grant delivered twice");
        }},
        {"partial_native_false_counts_only_real_delta", [] {
            Fixture f; f.item.acquire = 2; f.item.nativeResult = false;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()), "partial pickup reported complete");
            require(f.receipt().deliveredCount == 2 && f.inventory.count == 12 && f.mod.recovery.rows[0].deliveredCount == 2,
                "false native return lost partial delivery");
            require(!f.record().localPickupGranted && f.item.count == 5, "partial state incorrectly terminal");
            f.item.acquire = 5;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.count == 3 && f.inventory.count == 15,
                "explicit retry did not deliver exact remainder");
        }},
        {"baseline_capture_failure_prevents_native_mutation", [] {
            Fixture f; f.mod.failSaveAt = 1;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 0 && f.inventory.count == 10,
                "native mutation before durable baseline");
            require(!f.record().pickupDeliveryAttempted, "failed baseline consumed native attempt");
            require(!f.mod.m_sharedDropDeliveryRecoveryRequired, "baseline refusal incorrectly disconnected session");
            require(f.mod.uploads == 0, "failed canonical capture queued an ordinary upload");
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()), "baseline write refusal was not retryable");
        }},
        {"known_postcapture_failure_retries_without_regrant_or_disconnect", [] {
            Fixture f; f.mod.failSaveAt = 2;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.inventory.count == 15 && !f.record().pickupDeliveryStored,
                "failed postcapture was treated durable");
            require(!f.mod.m_sharedDropDeliveryRecoveryRequired && f.mod.m_localInventoryDirty && f.receipt().deliveredCount == 5,
                "known receipt was quarantined or lost its pending capture");
            f.mod.ProcessSharedDropDeliveryRecoveryFault();
            require(f.mod.stops == 0 && f.mod.m_networkMode == CoopNetworkMode::Client && !f.mod.m_multiplayerRestartRequired,
                "known write failure disconnected the session");
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1 &&
                f.mod.sidecar.count == 15 && f.mod.sidecar.rows[0].deliveredCount == 5,
                "capture retry repeated native acquisition or lost the paired state");
        }},
        {"optional_recovery_write_failure_keeps_canonical_and_dirty_retry", [] {
            Fixture f; f.mod.failJournalAt = 2;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.record().pickupDeliveryStored &&
                f.mod.sidecar.count == 15 && f.mod.sidecar.rows[0].deliveredCount == 5,
                "optional recovery failure invalidated a complete canonical snapshot");
            require(!f.mod.m_sharedDropDeliveryRecoveryRequired && f.mod.m_localInventoryDirty &&
                f.mod.recovery.count == 10 && f.mod.recovery.rows[0].deliveredCount == 0,
                "failed optional backup lost its retry marker or armed quarantine");
            require(f.mod.uploads == 1 && f.mod.m_pendingClientPlayerStateUpload,
                "optional backup failure blocked the ordinary canonical upload");
            f.mod.ProcessSharedDropDeliveryRecoveryFault();
            require(f.mod.stops == 0 && f.mod.PersistLocalSharedDropDelivery(f.record()) && f.item.nativeCalls == 1 &&
                f.mod.recovery.count == 15 && f.mod.recovery.rows[0].deliveredCount == 5,
                "optional backup retry disconnected or delivered a second item");
            require(f.mod.uploads == 1, "pending ordinary upload was queued repeatedly by backup retries");
        }},
        {"optional_baseline_journal_failure_does_not_block_native_delivery", [] {
            Fixture f; f.mod.failJournalAt = 1;
            f.item.duringPickup = [&f] {
                require(f.mod.sidecar.rows[0].deliveredCount == 0 && f.mod.sidecar.count == 10 &&
                    f.mod.recovery.rows.empty(), "native pickup did not use the canonical-only durable baseline");
            };
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1,
                "optional baseline backup refusal blocked native delivery");
            require(!f.mod.m_sharedDropDeliveryRecoveryRequired, "optional baseline backup refusal triggered fault");
        }},
        {"overflow_acceptance_nested_and_duplicate_callbacks", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()), "overflow unexpectedly completed");
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            require(token == 90 && before == 10 && f.record().pickupDeliveryInProgress, "overflow not observed");
            int nestedBefore = -1;
            require(f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, nestedBefore) == 0,
                "nested transfer observed twice");
            f.inventory.count += 2;
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.receipt().deliveredCount == 2 && !f.record().pickupDeliveryInProgress, "overflow delta missing");
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.receipt().deliveredCount == 2, "duplicate callback applied twice");
            const uint64_t finalToken = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            f.inventory.count += 3;
            f.entities.entities.erase(f.item.id); CArkItem::items.erase(f.item.id);
            f.mod.EndCommittedSharedDropInventoryTransfer(finalToken, before, &f.inventory);
            require(f.record().localPickupGranted && f.record().pickupDeliveryStored && f.receipt().deliveredCount == 5,
                "destroyed overflow source lost delivery");
            require(f.record().localEntityId == INVALID_ENTITYID, "destroyed source binding retained");
        }},
        {"overflow_cancel_has_no_delivery_delta", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.receipt().deliveredCount == 0 && !f.record().localPickupGranted && !f.record().pickupDeliveryInProgress,
                "empty/cancelled transfer acquired entitlement");
        }},
        {"unknown_native_outcome_quarantines_capture_and_retry", [] {
            Fixture f; f.item.duringPickup = [&f] { f.inventory.failRead = true; };
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.record().pickupDeliveryInProgress,
                "unknown native outcome not quarantined");
            require(f.mod.m_sharedDropDeliveryRecoveryRequired, "unknown outcome did not arm session fence");
            require(!f.mod.SaveLocalPlayerSidecar("unexpected save"), "uncertain inventory captured without receipt");
            f.inventory.failRead = false;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1,
                "unknown native outcome delivered twice");
            require(f.mod.recovery.count == 10 && f.mod.recovery.rows[0].deliveredCount == 0,
                "pre-mutation recovery baseline replaced");
        }},
        {"known_overflow_receipt_failure_retries_exact_remaining_count", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            f.mod.failSaveAt = 2;
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            f.inventory.count += 2;
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.receipt().deliveredCount == 2 && !f.mod.m_sharedDropDeliveryRecoveryRequired &&
                f.mod.m_localInventoryDirty && !f.record().pickupDeliveryStored,
                "known overflow receipt was lost or incorrectly quarantined");
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.receipt().deliveredCount == 2, "failed capture allowed a duplicate callback receipt");
            require(f.mod.PersistLocalSharedDropDelivery(f.record()) && f.mod.sidecar.count == 12 &&
                f.mod.sidecar.rows[0].deliveredCount == 2 && f.item.nativeCalls == 1,
                "known overflow capture retry changed native inventory");
            f.item.acquire = 5;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.inventory.count == 15 &&
                f.item.count == 3 && f.item.nativeCalls == 2, "capture retry duplicated the delivered prefix");
        }},
        {"overflow_unknown_delta_arms_fault_and_preserves_baseline", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            f.inventory.count += 2; f.inventory.failRead = true;
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.mod.m_sharedDropDeliveryRecoveryRequired && f.record().pickupDeliveryInProgress &&
                f.mod.recovery.count == 10 && f.mod.recovery.rows[0].deliveredCount == 0,
                "unknown overflow outcome replaced safe baseline");
        }},
        {"overflow_missing_precount_fences_the_native_transfer_and_capture", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            f.inventory.failRead = true;
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            require(token == 0 && f.mod.m_sharedDropDeliveryRecoveryRequired && !f.record().pickupDeliveryStored,
                "unmeasurable owned overflow transfer did not arm quarantine");
            // GiveOwnerInventory's wrapper invokes vanilla even without a token.
            f.inventory.failRead = false;
            f.inventory.count += 2;
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            f.mod.MarkLocalInventoryDirty("native owner mutation after unavailable baseline");
            require(!f.mod.SaveLocalPlayerSidecar("dirty callback") &&
                !f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1 &&
                f.mod.recovery.count == 10 && f.mod.recovery.rows[0].deliveredCount == 0,
                "native transfer escaped quarantine with an unchanged receipt");
            f.mod.ProcessSharedDropDeliveryRecoveryFault();
            require(f.mod.m_networkMode == CoopNetworkMode::Off &&
                f.mod.m_multiplayerRestartRequired && f.mod.m_sharedDropDeliveryRecoveryRequired &&
                !f.mod.SaveLocalPlayerSidecar("offline callback") && f.mod.recovery.count == 10,
                "pre-count fault teardown allowed uncertain inventory to replace the baseline");
        }},
        {"host_unknown_native_outcome_stops_session_once", [] {
            Fixture f; f.mod.m_networkMode = CoopNetworkMode::Host;
            f.item.duringPickup = [&f] { f.inventory.failRead = true; };
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            require(f.mod.m_sharedDropDeliveryRecoveryRequired, "Host unknown native outcome was not fenced");
            f.mod.ProcessSharedDropDeliveryRecoveryFault();
            f.mod.ProcessSharedDropDeliveryRecoveryFault();
            require(f.mod.stops == 1 && f.mod.m_multiplayerRestartRequired && f.mod.m_sharedDropDeliveryRecoveryRequired,
                "Host fault did not retain reload fence after one teardown");
        }},
        {"native_nested_owner_hook_is_not_double_counted", [] {
            Fixture f;
            f.item.duringPickup = [&f] {
                int before = -1;
                require(f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before) == 0,
                    "synchronous native hook counted nested delivery");
            };
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.receipt().deliveredCount == 5,
                "outer delivery lost after nested hook");
        }},
        {"wrong_target_inventory_is_not_a_receipt", [] {
            Fixture f; f.item.acquire = 0; f.mod.TryDeliverCommittedSharedDrop(f.record());
            ArkInventory other; int before = -1;
            require(f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &other, before) == 0,
                "external inventory counted as player delivery");
        }},
        {"restored_receipt_is_authoritative_for_remainder", [] {
            Fixture f;
            f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,3,8,3});
            f.inventory.count = 13;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.count == 2 && f.inventory.count == 15,
                "restored lifetime receipt ignored");
        }},
        {"mismatched_commit_metadata_blocks_native_delivery", [] {
            Fixture f;
            f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,0,9,3});
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 0,
                "different transaction used old lifetime receipt");
        }},
        {"pending_restore_and_noninteractive_capacity_are_gated", [] {
            Fixture f; f.mod.m_clientAwaitingHostPlayerState = true;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 0 && f.mod.saves == 0,
                "native delivery raced authoritative restore");
            f.mod.m_clientAwaitingHostPlayerState = false; f.item.canCollect = false;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record(), false) && f.item.nativeCalls == 0,
                "background delivery opened native overflow");
            require(f.mod.TryDeliverCommittedSharedDrop(f.record(), true), "explicit native pickup improperly capacity gated");
        }},
        {"weapon_keeps_native_presentation", [] {
            Fixture f; f.item.weapon = true;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()), "weapon delivery failed");
            require(!f.entities.entities.at(7).hidden && f.entities.entities.at(7).physics && f.mod.registrations == 1,
                "delivery override broke native weapon presentation");
        }},
        {"no_native_pickup_without_committed_ownership", [] {
            Fixture f; f.record().pickupCommitted = false;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 0 && f.mod.saves == 0,
                "reserved item acquired before commit");
        }},
        {"different_winner_cannot_receive_or_resume_overflow", [] {
            Fixture f; f.record().pickupWinnerPeerHash = 3;
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 0 &&
                f.mod.m_localSharedDropDeliveries.empty(), "nonwinner acquired a committed drop");
            f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,0,8,3});
            int before = -1;
            require(f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before) == 0,
                "nonwinner resumed another account's private overflow source");
        }},
        {"previous_session_receipt_cannot_suppress_current_delivery", [] {
            Fixture f;
            f.mod.m_localSharedDropDeliveries.push_back({39,90,100,5,5,8,3});
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.inventory.count == 15 &&
                f.mod.m_localSharedDropDeliveries.size() == 2 &&
                f.mod.m_localSharedDropDeliveries.back().hostSessionId == 40 &&
                f.mod.m_localSharedDropDeliveries.back().deliveredCount == 5,
                "same stable number from old session deduplicated a new lifetime");
        }},
        {"full_receipt_table_refuses_new_lifetime_without_mutation", [] {
            Fixture f;
            for (size_t index = 0; index != CoopSharedDropDelivery::kMaxEntries; ++index)
                f.mod.m_localSharedDropDeliveries.push_back({40,1000+index,100,1,1,8,3});
            require(!f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.inventory.count == 10 &&
                f.item.nativeCalls == 0 && f.mod.saves == 0 &&
                f.mod.m_localSharedDropDeliveries.size() == CoopSharedDropDelivery::kMaxEntries,
                "receipt capacity overflow granted inventory without a durable slot");
            require(!f.mod.m_sharedDropDeliveryRecoveryRequired,
                "pre-mutation receipt capacity refusal incorrectly became an uncertain-delivery fault");
        }},
        {"full_receipt_table_still_finishes_existing_remainder", [] {
            Fixture f;
            f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,2,8,3});
            for (size_t index = 1; index != CoopSharedDropDelivery::kMaxEntries; ++index)
                f.mod.m_localSharedDropDeliveries.push_back({40,1000+index,100,1,1,8,3});
            f.inventory.count = 12;
            require(f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.inventory.count == 15 &&
                f.item.count == 3 && f.receipt().deliveredCount == 5 &&
                f.mod.m_localSharedDropDeliveries.size() == CoopSharedDropDelivery::kMaxEntries,
                "receipt cap stranded an already assigned partial delivery");
        }},
        {"overflow_identity_change_after_begin_quarantines_uncertain_delta", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            int before = -1;
            const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
            f.inventory.count += 2;
            ++f.record().pickupTransactionSequence;
            f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
            require(f.mod.m_sharedDropDeliveryRecoveryRequired && f.record().pickupDeliveryInProgress &&
                f.mod.recovery.count == 10 && f.mod.recovery.rows[0].deliveredCount == 0,
                "changed commit identity associated uncertain inventory with a different receipt");
        }},
        {"partial_overflow_duplicate_callback_schedule_conserves_count", [] {
            for (uint32_t seed = 1; seed <= 128; ++seed) {
                Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
                f.mod.TryDeliverCommittedSharedDrop(f.record());
                uint32_t random = seed;
                for (unsigned step = 0; step != 64 && f.receipt().deliveredCount < 5; ++step) {
                    random = random * 1664525u + 1013904223u;
                    const uint32_t remaining = 5 - f.receipt().deliveredCount;
                    const uint32_t moved = std::min(remaining, random % 3);
                    int before = -1;
                    const uint64_t token = f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before);
                    require(token == 90, "valid overflow interaction lost its owned source");
                    f.inventory.count += static_cast<int>(moved);
                    f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
                    f.mod.EndCommittedSharedDropInventoryTransfer(token, before, &f.inventory);
                    require(f.inventory.count == 10 + static_cast<int>(f.receipt().deliveredCount) &&
                        f.mod.recovery.count == f.inventory.count &&
                        f.mod.recovery.rows[0].deliveredCount == f.receipt().deliveredCount &&
                        f.item.nativeCalls == 1, "duplicate or zero-delta callback broke paired delivery conservation");
                }
                require(f.receipt().deliveredCount == 5 && f.inventory.count == 15,
                    "bounded repeated partial overflow schedule never completed");
                int before = -1;
                require(f.mod.BeginCommittedSharedDropInventoryTransfer(&f.item, &f.inventory, before) == 0 &&
                    f.mod.TryDeliverCommittedSharedDrop(f.record()) && f.item.nativeCalls == 1,
                    "completed overflow lifetime became acquirable again");
            }
        }},
        {"local_recycler_cannot_consume_reserved_or_committed_source", [] {
            Fixture f; CArkProjectileRecyclerGrenade local;
            f.record().pickupCommitted = false;
            require(f.mod.ShouldSuppressRecyclerForSharedDrop(&local, 7), "reserved source reached local recycler");
            f.record().pickupPending = false; f.record().pickupCommitted = true;
            require(f.mod.ShouldSuppressRecyclerForSharedDrop(&local, 7), "committed source reached local recycler");
        }},
        {"ordinary_local_recycler_and_untracked_targets_remain_native", [] {
            Fixture f; CArkProjectileRecyclerGrenade local;
            f.record().pickupPending = false; f.record().pickupCommitted = false;
            require(!f.mod.ShouldSuppressRecyclerForSharedDrop(&local, 7), "ordinary local recycling was suppressed");
            require(!f.mod.ShouldSuppressRecyclerForSharedDrop(&local, 99) &&
                !f.mod.ShouldSuppressRecyclerForSharedDrop(&local, INVALID_ENTITYID) &&
                !f.mod.ShouldSuppressRecyclerForSharedDrop(nullptr, 7), "untracked or invalid target matched escrow");
        }},
        {"remote_recycler_still_defers_ordinary_shared_sources", [] {
            Fixture f; CArkProjectileRecyclerGrenade remote;
            f.record().pickupPending = false; f.record().pickupCommitted = false;
            f.mod.m_remoteRecyclerGrenades.insert(&remote);
            require(f.mod.ShouldSuppressRecyclerForSharedDrop(&remote, 7) &&
                !f.mod.ShouldSuppressRecyclerForSharedDrop(&remote, 99), "remote shared-source protection regressed");
            f.mod.m_sharedDrops.clear();
            require(f.mod.ShouldSuppressRecyclerForSharedDrop(&remote, 7), "remote binding protection now needs a live record");
        }},
        {"retired_source_rebuilds_only_the_owned_remainder", [] {
            Fixture f; f.item.acquire = 2; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            f.entities.entities.erase(7); CArkItem::items.erase(7);
            f.record().localEntityId = INVALID_ENTITYID;
            f.mod.m_sharedDropByEntityId.erase(7);
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.mod.materializeCalls == 1 && f.receipt().deliveredCount == 5 && f.inventory.count == 15,
                "retired private source lost or duplicated its committed remainder");
            require(f.mod.materializedItems.at(101).count == 3 && f.mod.materializedItems.at(101).nativeCalls == 1,
                "reconstruction delivered original count instead of remainder");
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.mod.materializeCalls == 1 && f.inventory.count == 15, "completed receipt rebuilt again");
        }},
        {"retired_source_waits_for_capacity_without_popup_or_respawn_loop", [] {
            Fixture f; f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,2,8,3});
            f.inventory.count = 12; f.entities.entities.erase(7); CArkItem::items.erase(7);
            f.mod.materializedCanCollect = false;
            f.mod.RestoreCommittedSharedDropDeliveries();
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.mod.materializeCalls == 1 && f.mod.materializedItems.at(101).nativeCalls == 0 &&
                f.mod.m_sharedDropByEntityId.count(7) == 0, "capacity wait respawned source or opened overflow");
            f.mod.materializedItems.at(101).canCollect = true;
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.inventory.count == 15 && f.mod.materializedItems.at(101).nativeCalls == 1,
                "free capacity did not resume exact owned remainder");
        }},
        {"overflow_cancel_is_not_reopened_by_background_restore", [] {
            Fixture f; f.item.acquire = 0; f.item.nativeResult = false;
            f.mod.TryDeliverCommittedSharedDrop(f.record());
            f.mod.RestoreCommittedSharedDropDeliveries();
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.item.nativeCalls == 1 && f.mod.materializeCalls == 0,
                "cancelled overflow reopened without an explicit pickup");
        }},
        {"reconstruction_respects_fault_restore_and_commit_identity", [] {
            Fixture f; f.mod.m_localSharedDropDeliveries.push_back({40,90,100,5,0,8,3});
            f.entities.entities.erase(7); CArkItem::items.erase(7);
            f.mod.m_sharedDropDeliveryRecoveryRequired = true;
            f.mod.RestoreCommittedSharedDropDeliveries();
            f.mod.m_sharedDropDeliveryRecoveryRequired = false;
            f.mod.m_pendingReceivedPlayerStateApply = true;
            f.mod.RestoreCommittedSharedDropDeliveries();
            f.mod.m_pendingReceivedPlayerStateApply = false;
            ++f.record().pickupTransactionSequence;
            f.mod.RestoreCommittedSharedDropDeliveries();
            require(f.mod.materializeCalls == 0, "reconstruction bypassed fault, restore, or transaction identity gate");
        }},
    };
    unsigned failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) { ++failures; std::cout << "FAIL " << test.first << ": " << error.what() << '\n'; }
    }
    std::cout << (tests.size() - failures) << "/" << tests.size() << " native-delivery dependency-fake checks passed\n";
    return failures ? 1 : 0;
}
