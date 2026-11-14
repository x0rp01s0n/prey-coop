#include "CoopProtocol.h"
#include "CoopSerialSequence.h"
#include "CoopSharedDropDelivery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using EntityId = unsigned;
constexpr EntityId INVALID_ENTITYID = 0;
constexpr int ZERO = 0, IDENTITY = 1;
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    explicit Vec3(int) {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
struct Quat {
    float w = 1; Vec3 v;
    Quat() = default;
    explicit Quat(int) {}
    Quat(float a, float b, float c, float d) : w(a), v(b, c, d) {}
};
struct IEntity {
    EntityId id = 0;
    bool hidden = false;
    std::string name = "";
    const char* GetName() const { return name.c_str(); }
    void SetName(const char* value) { name = value; }
    EntityId GetId() const { return id; }
    bool IsHidden() const { return hidden; }
    Vec3 GetWorldPos() const { return {}; }
    Quat GetWorldRotation() const { return {}; }
};
struct IEntityIt;
struct IEntityItPtr : std::shared_ptr<IEntityIt> {
    IEntityItPtr(IEntityIt* value) : std::shared_ptr<IEntityIt>(value) {}
};
struct EntitySystem {
    std::unordered_map<EntityId, IEntity> entities;
    IEntity* GetEntity(EntityId id) {
        auto found = entities.find(id);
        return found == entities.end() ? nullptr : &found->second;
    }
    void RemoveEntity(EntityId id, bool) { entities.erase(id); }
    IEntityIt* GetEntityIterator();
};
struct IEntityIt {
    EntitySystem* system;
    std::vector<EntityId> ids;
    size_t index = 0;
    explicit IEntityIt(EntitySystem* value) : system(value) {}
    void MoveFirst() {
        ids.clear(); index = 0;
        for (const auto& entry : system->entities) ids.push_back(entry.first);
    }
    bool IsEnd() const { return index >= ids.size(); }
    IEntity* Next() { return index < ids.size() ? system->GetEntity(ids[index++]) : nullptr; }
};
IEntityIt* EntitySystem::GetEntityIterator() { return new IEntityIt(this); }
struct ArkInventory {
    std::unordered_set<EntityId> members;
    bool Contains(EntityId id) const { return members.count(id) != 0; }
};
struct Environment { EntitySystem* pEntitySystem = nullptr; };
Environment* gEnv = nullptr;

// Network methods are extracted unchanged from production. Native delivery
// and filesystem persistence below are dependency fakes, not native proof.
struct CArkItem {
    EntityId id = 0;
    bool accept = true;
    unsigned nativeCalls = 0;
    unsigned inventoryCount = 0;
    int count = 1;
    uint64_t archetype = 99;
    static inline std::unordered_map<EntityId, CArkItem*> items;
    static inline std::function<bool(CArkItem*)> beforePickup;
    static CArkItem* GetItemFromEntityId(EntityId id) {
        auto found = items.find(id);
        return found == items.end() ? nullptr : found->second;
    }
    EntityId GetEntityId() const { return id; }
    IEntity* GetEntity() const { return gEnv->pEntitySystem->GetEntity(id); }
    uint64_t GetArchetype() const { return archetype; }
    int GetCount() const { return count; }
    unsigned GetOwnerId() const { return 0; }
    void RemoveEntity() {
        gEnv->pEntitySystem->RemoveEntity(id, true);
        items.erase(id);
    }
    bool PickUp(EntityId, bool) {
        if (beforePickup && beforePickup(this)) return false;
        ++nativeCalls;
        if (accept) ++inventoryCount;
        return accept;
    }
};
struct ArkPlayer {
    EntityId id = 1;
    bool ready = true;
    IEntity entity{1};
    ArkInventory inventory;
    ArkInventory* m_pInventory = &inventory;
    static ArkPlayer instance;
    static ArkPlayer* GetInstancePtr() { return &instance; }
    static ArkPlayer& GetInstance() { return instance; }
    EntityId GetEntityId() const { return id; }
    IEntity* GetEntity() { return ready ? &entity : nullptr; }
};
ArkPlayer ArkPlayer::instance;
namespace CoopRuntimeGuards {
template<class F, class T>
bool TryGuardedCall(const char*, F callback, T& result, std::string*) {
    result = callback();
    return true;
}
template<class F>
bool TryGuardedVoidCall(const char*, F callback, std::string*) {
    callback();
    return true;
}
}
namespace CoopItemClassification {
bool IsRealWeaponInventoryItem(CArkItem*, std::string&) { return false; }
}
bool SetSharedDropWorldPresentation(IEntity& entity, bool visible, std::string&) {
    entity.hidden = !visible;
    return true;
}
enum class CoopNetworkMode { Off, Host, Client };

class ModMain {
public:
    void RecoverSavedSharedDrops();
    void ReplaySharedDrops();
    void ResetSharedDropState(const char*, bool = false);
    unsigned pruneCalls = 0;
    void PruneUnboundSharedDrops() { ++pruneCalls; }
#include "SharedDropRecordActual.inc"
    struct Peer {
        bool sessionReady = true; uint32_t address = 1; uint16_t port = 1;
        uint64_t levelId = 10;
        uint32_t worldEpoch = 1;
        uint32_t sessionFlags = CoopProtocol::kSessionFlagWorldReady;
    };
    using RemotePeerSession = Peer;
    struct Sent { CoopProtocol::SharedDropPacket packet; uint64_t target = 0; };
    CoopNetworkMode m_networkMode = CoopNetworkMode::Client;
    bool m_hasRemoteEndpoint = true;
    uint32_t m_localWorldEpoch = 1;
    uint64_t m_localLevelId = 10;
    uint32_t m_remoteAddress = 1;
    uint16_t m_remotePort = 1;
    uint32_t m_sharedDropSequence = 0;
    unsigned m_sharedDropApplyDepth = 0;
    unsigned m_sharedDropSent = 0, m_sharedDropReceived = 0, m_sharedDropApplied = 0;
    unsigned m_sharedDropDropped = 0, m_sharedDropPickupRequests = 0;
    unsigned m_sharedDropPickupCommits = 0, m_sharedDropPickupSuppressions = 0;
    unsigned m_sharedDropDuplicateCommits = 0;
    unsigned m_sharedDropEntityRetirements = 0;
    uint64_t token = 2, remoteToken = 1;
    uint64_t m_sharedDropHostSessionId = 40;
    bool m_enablePlayerSidecar = true;
    bool m_localInventoryDirty = false;
    std::string m_currentHostSaveStateKey = "save_A";
    std::string m_localInventoryDirtySaveKey;
    uint64_t m_localInventoryDirtyHostAccountToken = 0;
    bool gameplayReady = true;
    bool continuingHostWorld = false;
    bool HasContinuingHostInventoryWorld() const { return continuingHostWorld; }
    bool m_saveLoadGuardActive = false;
    bool m_pendingPostLoadResync = false;
    bool m_arkLevelTransitionLoadActive = false;
    bool m_runtimeTransitionCleanupPrepared = false;
    bool m_sharedDropRecoveryRequired = false;
    std::unordered_map<uint64_t, Peer> m_remotePeers;
    std::unordered_map<uint64_t, SharedDropRecord> m_sharedDrops;
    std::unordered_map<EntityId, uint64_t> m_sharedDropByEntityId;
    std::string m_lastSharedDropEvent;
    std::vector<Sent> sent;
    bool failSends = false;
    bool failStore = false, failSnapshot = false, failJournal = false, failReceipt = false;
    unsigned storeAttempts = 0, snapshotAttempts = 0, journalAttempts = 0, receiptAttempts = 0;
    unsigned commitSendAttempts = 0;
    unsigned deliveryCalls = 0;
    unsigned materializeCalls = 0;
    EntityId materializeEntity = INVALID_ENTITYID;
    bool allowMaterialize = false;
    struct Stored { uint64_t account; CoopSharedDropDelivery::Entry entry; };
    std::vector<Stored> durable;
    std::vector<CoopSharedDropDelivery::Entry> m_localSharedDropDeliveries;
    uint64_t GetLocalAccountToken() const { return token; }
    uint64_t GetRemoteAccountToken() const { return remoteToken; }
    uint64_t GetSessionHostAccountToken() const { return m_networkMode == CoopNetworkMode::Host ? token : remoteToken; }
    uint64_t CurrentHostSaveKeyHash() const { return 20; }
    bool IsSessionGameplayReady() const { return gameplayReady; }
    bool IsSharedDropWorldReady() const;
    bool IsSharedDropServiceReady() const;
    void DetachSharedDropEntities();
    uint64_t BuildSharedDropStableId(uint64_t, const Vec3&, uint32_t) const;
    void OnNativeSharedItemDropping(CArkItem*);
    void OnNativeSharedItemDropped(CArkItem*, int, const char*);
    void RetireLivePropTrackingForSharedDrop(EntityId) {}
    void MarkLocalInventoryDirty(const char*) { m_localInventoryDirty = true; }
    void QueueClientPlayerStateUpload(const char*, const std::string&) {}
    uint64_t EnsureSharedDropHostSessionId() { return m_sharedDropHostSessionId; }
    bool SaveLocalPlayerSidecar(const char*) { ++snapshotAttempts; return !failSnapshot; }
    bool WriteLocalPlayerRecoveryJournal(const char*) { ++journalAttempts; return !failJournal; }
    bool StoreCommittedSharedDropDelivery(uint64_t account, const CoopSharedDropDelivery::Entry& entry) {
        ++storeAttempts;
        const auto& record = m_sharedDrops.at(entry.stableSpawnId);
        if (!record.live || record.pickupCommitted || !record.pickupPending)
            throw std::runtime_error("ownership persistence must precede world removal/commit");
        if (failStore) return false;
        durable.push_back({account, entry});
        return true;
    }
    bool PersistLocalSharedDropDelivery(SharedDropRecord& record) {
        ++receiptAttempts;
        if (!record.localPickupGranted) throw std::runtime_error("receipt before delivery");
        if (failReceipt) return false;
        record.pickupDeliveryStored = true;
        return true;
    }
    bool TryDeliverCommittedSharedDrop(SharedDropRecord& record, bool = true) {
        ++deliveryCalls;
        if (!record.pickupCommitted || record.live)
            throw std::runtime_error("delivery before durable ownership decision");
        if (m_networkMode == CoopNetworkMode::Host &&
            (durable.empty() || (m_hasRemoteEndpoint && gameplayReady && commitSendAttempts == 0)))
            throw std::runtime_error("Host native pickup before durable commit/publication attempt");
        if (record.localPickupGranted)
            return record.pickupDeliveryStored || PersistLocalSharedDropDelivery(record);
        CArkItem* item = CArkItem::GetItemFromEntityId(record.localEntityId);
        if (!item) return false;
        ++m_sharedDropApplyDepth;
        const bool accepted = item->PickUp(ArkPlayer::GetInstance().GetEntityId(), true);
        --m_sharedDropApplyDepth;
        if (!accepted) return false;
        record.localPickupGranted = true;
        return PersistLocalSharedDropDelivery(record);
    }
    void RestoreCommittedSharedDropDeliveries() {}
    void EnsureLocalPlayerWeaponRegistered(EntityId, const char*) {}
    bool MaterializeSharedDrop(const CoopProtocol::SharedDropPacket& packet, SharedDropRecord& record, std::string&) {
        ++materializeCalls;
        if (!allowMaterialize) throw std::runtime_error("unexpected spawn materialization dispatch");
        record.stableSpawnId = packet.stableSpawnId;
        record.areaId = packet.areaId;
        record.archetypeId = packet.archetypeId;
        record.version = packet.objectVersion;
        record.count = packet.count;
        record.localEntityId = materializeEntity;
        record.live = (packet.flags & CoopProtocol::kSharedDropFlagLive) != 0;
        return true;
    }
    bool RemoveSharedDropLocal(SharedDropRecord&, std::string&);
    bool SendReliablePayloadTo(uint16_t, const void* packet, uint16_t size,
        uint32_t, uint16_t, const char*) {
        if (size != sizeof(CoopProtocol::SharedDropPacket)) throw std::runtime_error("packet size");
        if (m_networkMode == CoopNetworkMode::Host &&
            static_cast<CoopProtocol::SharedDropCommand>(static_cast<const CoopProtocol::SharedDropPacket*>(packet)->command) ==
                CoopProtocol::SharedDropCommand::PickupCommit)
            ++commitSendAttempts;
        if (failSends) return false;
        sent.push_back({*static_cast<const CoopProtocol::SharedDropPacket*>(packet), 0});
        return true;
    }
    bool QueueReliablePayloadToEndpoint(uint16_t, const void* packet, uint16_t size,
        uint32_t address, uint16_t, uint64_t, const char*) {
        if (m_networkMode == CoopNetworkMode::Host &&
            static_cast<const CoopProtocol::SharedDropPacket*>(packet)->command ==
                static_cast<uint16_t>(CoopProtocol::SharedDropCommand::PickupCommit))
            ++commitSendAttempts;
        if (failSends) return false;
        if (size != sizeof(CoopProtocol::SharedDropPacket)) throw std::runtime_error("packet size");
        uint64_t recipient = 0;
        for (const auto& entry : m_remotePeers) if (entry.second.address == address) recipient = entry.first;
        sent.push_back({*static_cast<const CoopProtocol::SharedDropPacket*>(packet), recipient});
        return true;
    }
    bool BuildSharedDropPacket(CoopProtocol::SharedDropPacket&, CoopProtocol::SharedDropCommand,
        const SharedDropRecord&, uint64_t = 0) const;
    bool SendSharedDropTo(const CoopProtocol::SharedDropPacket&, uint32_t, uint16_t, const char*);
    bool SendSharedDropToPeer(const CoopProtocol::SharedDropPacket&, uint64_t, const char*);
    bool ShouldDeferNativeSharedItemPickup(CArkItem*, EntityId, const char*);
    bool CommitSharedDropPickup(SharedDropRecord&, uint64_t, uint32_t);
    bool SendSharedDropPickupResult(SharedDropRecord&, bool);
    bool SendSharedDropPickupReady(SharedDropRecord&, bool);
    void TickSharedDropPickupRetries(float);
    void ReleaseSharedDropReservation(SharedDropRecord&, uint64_t, uint32_t, const char*);
    void ReleaseSharedDropReservationsForPeer(uint64_t);
    void HandleSharedDrop(const CoopProtocol::SharedDropPacket&);
};

#include "SharedDropActual.inc"

constexpr uint64_t stable = 77;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
using Command = CoopProtocol::SharedDropCommand;
using Packet = CoopProtocol::SharedDropPacket;
struct Fixture {
    EntitySystem entities;
    Environment environment{&entities};
    ModMain host, first, second;
    CArkItem firstItem{101}, secondItem{102}, hostItem{103};
    Fixture() {
        gEnv = &environment;
        ArkPlayer::instance.ready = true;
        ArkPlayer::instance.inventory.members.clear();
        CArkItem::items.clear();
        CArkItem::beforePickup = {};
        CArkItem::items[101] = &firstItem;
        CArkItem::items[102] = &secondItem;
        CArkItem::items[103] = &hostItem;
        entities.entities.emplace(101, IEntity{101});
        entities.entities.emplace(102, IEntity{102});
        entities.entities.emplace(103, IEntity{103});
        host.m_networkMode = CoopNetworkMode::Host;
        host.token = 1;
        first.token = 2;
        second.token = 3;
        host.m_remotePeers.emplace(2, ModMain::Peer{true, 2, 1});
        host.m_remotePeers.emplace(3, ModMain::Peer{true, 3, 1});
        for (ModMain* peer : {&host, &first, &second}) {
            ModMain::SharedDropRecord record;
            record.stableSpawnId = stable;
            record.areaId = 10;
            record.archetypeId = 99;
            record.ownerPeerHash = 1;
            record.version = 1;
            record.count = 1;
            record.live = true;
            record.localEntityId = peer == &first ? 101 : peer == &second ? 102 : 103;
            peer->m_sharedDrops.emplace(stable, record);
            peer->m_sharedDropByEntityId.emplace(record.localEntityId, stable);
        }
    }
    ModMain::SharedDropRecord& row(ModMain& peer) { return peer.m_sharedDrops.at(stable); }
    static Packet take(ModMain& peer, Command command) {
        auto found = std::find_if(peer.sent.begin(), peer.sent.end(), [command](const auto& sent) {
            return static_cast<Command>(sent.packet.command) == command;
        });
        require(found != peer.sent.end(), "expected production phase packet");
        auto packet = found->packet;
        peer.sent.erase(found);
        return packet;
    }
    static void deliver(ModMain& peer, const Packet& packet) {
        CArkItem::beforePickup = [&peer](CArkItem* item) {
            return peer.ShouldDeferNativeSharedItemPickup(item, 1, "fake native pre-pickup callback");
        };
        peer.remoteToken = packet.sourcePeerHash;
        peer.HandleSharedDrop(packet);
    }
    void request(ModMain& peer, CArkItem& item) {
        require(peer.ShouldDeferNativeSharedItemPickup(&item, 1, "unit interaction"),
            "client pickup must defer");
        deliver(host, take(peer, Command::PickupRequest));
    }
    Packet prepare() {
        request(first, firstItem);
        require(row(host).pickupPending, "host reservation not established");
        auto grant = take(host, Command::PickupGrant);
        deliver(first, grant);
        return grant;
    }
    Packet ready() {
        prepare();
        return take(first, Command::PickupReady);
    }
    Packet commit() {
        deliver(host, ready());
        return take(host, Command::PickupCommit);
    }
    void finish() {
        deliver(first, commit());
        deliver(host, take(first, Command::PickupResult));
    }
    void broadcast() {
        auto packets = host.sent;
        host.sent.clear();
        for (const auto& sent : packets) {
            if (sent.target == 0 || sent.target == first.token) deliver(first, sent.packet);
            if (sent.target == 0 || sent.target == second.token) deliver(second, sent.packet);
        }
    }
    void expire() {
        row(host).pickupReservationDeadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        host.TickSharedDropPickupRetries(0.0f);
    }
};

void prepareHasNoNativeMutation() {
    Fixture f;
    f.prepare();
    require(f.firstItem.nativeCalls == 0 && f.firstItem.inventoryCount == 0,
        "preparation acquired usable native inventory");
    require(f.row(f.host).live && f.row(f.host).pickupPending && f.host.durable.empty(),
        "preparation committed ownership");
    require(f.entities.GetEntity(101)->hidden && f.entities.GetEntity(103)->hidden,
        "prepared world presentation remained pickable");
    require(f.first.snapshotAttempts == 1 && f.first.journalAttempts == 1,
        "prepared baseline did not check both restore snapshots");
}

void successfulPickup() {
    Fixture f;
    auto commit = f.commit();
    require(f.host.durable.size() == 1 && f.host.durable[0].account == f.first.token,
        "host failed to store one committed entitlement");
    require(!f.row(f.host).live && f.row(f.host).pickupCommitted && !f.entities.GetEntity(103),
        "durable commit did not remove host world copy");
    require(f.firstItem.nativeCalls == 0, "native pickup preceded commit delivery");
    Fixture::deliver(f.first, commit);
    require(f.firstItem.nativeCalls == 1 && f.firstItem.inventoryCount == 1,
        "committed grant did not acquire native item once");
    require(f.row(f.first).pickupCommitted &&
        f.row(f.first).pickupDeliveryStored, "delivery callback invalidated transaction");
    Fixture::deliver(f.host, Fixture::take(f.first, Command::PickupResult));
    require(f.row(f.host).pickupResultSucceeded, "delivery acknowledgement not recorded");
    Fixture::deliver(f.first, commit);
    require(f.firstItem.nativeCalls == 1, "duplicate commit acquired twice");
}

void hostLocalPickupUsesDurableCommit() {
    Fixture f;
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host native interaction"),
        "Host tracked pickup bypassed committed delivery");
    require(f.host.durable.size() == 1 && f.host.durable.front().account == f.host.token &&
        f.hostItem.nativeCalls == 1 && f.hostItem.inventoryCount == 1 &&
        f.row(f.host).pickupCommitted && f.row(f.host).pickupDeliveryStored && !f.row(f.host).live,
        "Host-local pickup did not persist and deliver one entitlement");
    f.broadcast();
    require(!f.row(f.first).live && !f.row(f.second).live &&
        f.firstItem.inventoryCount == 0 && f.secondItem.inventoryCount == 0,
        "Host-local decision left observer world copies live");
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host repeat interaction") &&
        f.hostItem.nativeCalls == 1 && f.host.durable.size() == 1,
        "Host repeated interaction duplicated committed delivery");
}

void hostNativeRefusalKeepsAssignedEntitlement() {
    Fixture f;
    f.hostItem.accept = false;
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host native refuses"),
        "Host native refusal bypassed transaction");
    f.broadcast();
    f.host.ReleaseSharedDropReservationsForPeer(f.host.token);
    f.expire();
    require(f.host.durable.size() == 1 && f.hostItem.nativeCalls == 1 &&
        f.hostItem.inventoryCount == 0 && f.row(f.host).pickupCommitted &&
        !f.row(f.host).live && !f.row(f.first).live && !f.row(f.second).live,
        "Host delivery refusal lost assignment or restored shared world ownership");
}

void hostOwnershipStoreFailureCancelsAndRetries() {
    Fixture f;
    f.host.failStore = true;
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host store fails"),
        "Host store failure bypassed transaction");
    f.broadcast();
    require(f.host.durable.empty() && f.hostItem.nativeCalls == 0 && f.row(f.host).live &&
        !f.row(f.host).pickupPending && f.row(f.first).live && f.row(f.second).live,
        "Host failed ownership store mutated inventory or lost live world copy");
    f.host.failStore = false;
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host store retry") &&
        f.host.durable.size() == 1 && f.hostItem.inventoryCount == 1,
        "Host failed-store retry did not deliver exactly once");
}

void hostCommitPublicationFailureDoesNotRevokeOwnership() {
    Fixture f;
    f.host.failSends = true;
    require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host send fails"),
        "Host failed publication bypassed transaction");
    f.host.ReleaseSharedDropReservationsForPeer(f.host.token);
    require(f.host.commitSendAttempts == 2 && f.host.durable.size() == 1 &&
        f.row(f.host).pickupCommitted && !f.row(f.host).live && f.hostItem.inventoryCount == 1,
        "failed terminal publication reverted committed Host ownership");
}

void sessionNotReadyFencesTrackedPickup() {
    Fixture f;
    for (ModMain* peer : {&f.host, &f.first}) {
        peer->gameplayReady = false;
        ArkPlayer::instance.ready = false;
        CArkItem& item = peer == &f.host ? f.hostItem : f.firstItem;
        require(peer->ShouldDeferNativeSharedItemPickup(&item, 1, "session not live") &&
            peer->durable.empty() && peer->sent.empty() && item.nativeCalls == 0 &&
            f.row(*peer).live && !f.row(*peer).pickupPending,
            "temporary readiness loss bypassed tracked shared-drop fence");
    }
}

void sessionNotReadyPreservesUntrackedVanillaPath() {
    Fixture f;
    CArkItem personal{900};
    for (ModMain* peer : {&f.host, &f.first}) {
        peer->gameplayReady = false;
        require(!peer->ShouldDeferNativeSharedItemPickup(&personal, 1, "personal item while unready"),
            "readiness fence intercepted untracked vanilla item");
        peer->m_networkMode = CoopNetworkMode::Off;
        CArkItem& tracked = peer == &f.host ? f.hostItem : f.firstItem;
        require(!peer->ShouldDeferNativeSharedItemPickup(&tracked, 1, "network disabled"),
            "disabled multiplayer intercepted native item");
    }
}

void sessionNotReadyPreservesCommittedReceiptPath() {
    Fixture f;
    f.finish();
    f.first.gameplayReady = false;
    const auto calls = f.first.deliveryCalls;
    require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "owned item while unready") &&
        f.first.deliveryCalls == calls + 1 && f.firstItem.nativeCalls == 1 &&
        f.firstItem.inventoryCount == 1,
        "readiness loss allowed committed item to bypass receipt helper");
}

void sessionNotReadyKeepsPendingReservationFenced() {
    Fixture f;
    f.prepare();
    f.first.gameplayReady = false;
    const auto sent = f.first.sent.size();
    require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "pending item while unready") &&
        f.row(f.first).pickupPending && f.firstItem.nativeCalls == 0 &&
        f.first.sent.size() == sent,
        "readiness loss allowed pending shared drop to enter raw native pickup");
}

void timeoutBeforeGrantDelivery() {
    Fixture f;
    f.request(f.first, f.firstItem);
    auto lateGrant = Fixture::take(f.host, Command::PickupGrant);
    f.expire();
    f.broadcast();
    Fixture::deliver(f.first, lateGrant);
    require(f.row(f.host).live && !f.row(f.host).pickupPending && f.row(f.first).live &&
        !f.row(f.first).pickupPending && f.firstItem.nativeCalls == 0 && f.host.durable.empty(),
        "expired reservation or delayed preparation acquired/lost item");
    require(!f.entities.GetEntity(103)->hidden, "timeout left shared world copy hidden");
}

void timeoutAfterPrepareBeforeReady() {
    Fixture f;
    auto lateReady = f.ready();
    f.expire();
    f.broadcast();
    const auto version = f.row(f.host).version;
    Fixture::deliver(f.host, lateReady);
    require(f.row(f.host).live && !f.row(f.host).pickupPending &&
        f.row(f.host).version == version && f.host.durable.empty() && f.firstItem.nativeCalls == 0,
        "delayed Ready committed cancelled transaction");
    require(!f.entities.GetEntity(101)->hidden && !f.entities.GetEntity(103)->hidden,
        "cancel failed to restore both world presentations");
}

void wallClockReservationDeadline() {
    Fixture f;
    const auto started = std::chrono::steady_clock::now();
    f.prepare();
    const auto prepared = std::chrono::steady_clock::now();
    const auto originalDeadline = f.row(f.host).pickupReservationDeadline;
    require(originalDeadline >= started + std::chrono::seconds(30) &&
        originalDeadline <= prepared + std::chrono::seconds(30),
        "reservation did not establish its thirty-second monotonic deadline");

    f.row(f.host).pickupReservationDeadline = prepared + std::chrono::hours(1);
    f.host.TickSharedDropPickupRetries(1000000.0f);
    require(f.row(f.host).pickupPending && f.host.durable.empty(),
        "frame delta expired a reservation before its real deadline");

    const auto shortDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    f.row(f.host).pickupReservationDeadline = shortDeadline;
    std::this_thread::sleep_until(shortDeadline + std::chrono::milliseconds(1));
    f.host.TickSharedDropPickupRetries(0.0f);
    require(f.row(f.host).live && !f.row(f.host).pickupPending &&
        f.row(f.host).pickupReservationDeadline == std::chrono::steady_clock::time_point{} &&
        f.host.durable.empty() && f.firstItem.nativeCalls == 0,
        "real elapsed deadline did not cancel escrow with zero frame delta");
    f.broadcast();
    require(!f.row(f.first).pickupPending && !f.entities.GetEntity(101)->hidden,
        "real elapsed timeout did not release Client presentation");
}

void disconnectBeforeCommit() {
    Fixture f;
    auto ready = f.ready();
    f.host.m_remotePeers.erase(f.first.token);
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    f.broadcast();
    Fixture::deliver(f.host, ready);
    require(f.row(f.host).live && f.host.durable.empty() && f.firstItem.nativeCalls == 0 &&
        !f.row(f.host).pickupPending && f.row(f.second).live,
        "disconnect did not release world ownership for remaining peers");
}

void lostCommitThenDisconnect() {
    Fixture f;
    auto commit = f.commit();
    (void)commit;
    f.host.m_remotePeers.erase(f.first.token);
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    f.expire();
    require(!f.row(f.host).live && f.row(f.host).pickupCommitted &&
        f.host.durable.size() == 1 && !f.entities.GetEntity(103) &&
        f.firstItem.nativeCalls == 0, "lost commit/disconnect recreated assigned world item");
}

void lostResultAfterAcquisition() {
    Fixture f;
    Fixture::deliver(f.first, f.commit());
    f.first.sent.clear();
    f.host.m_remotePeers.erase(f.first.token);
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    f.expire();
    require(f.firstItem.inventoryCount == 1 && !f.row(f.host).live &&
        f.host.durable.size() == 1 && !f.entities.GetEntity(103),
        "acquired item and unacknowledged commit recreated shared world copy");
}

void storeFailureCancels() {
    Fixture f;
    f.host.failStore = true;
    Fixture::deliver(f.host, f.ready());
    f.broadcast();
    require(f.host.storeAttempts == 1 && f.host.durable.empty() && f.row(f.host).live &&
        !f.row(f.host).pickupPending && f.firstItem.nativeCalls == 0 &&
        !f.row(f.first).pickupPending, "failed durable ownership write granted or lost item");
}

void baselineFailureCancels() {
    Fixture f;
    f.first.failSnapshot = true;
    auto grant = f.prepare();
    auto nack = Fixture::take(f.first, Command::PickupReady);
    require((nack.flags & CoopProtocol::kSharedDropFlagPickupSucceeded) == 0,
        "failed baseline snapshot acknowledged readiness");
    Fixture::deliver(f.first, grant);
    auto duplicate = Fixture::take(f.first, Command::PickupReady);
    require(duplicate.flags == nack.flags, "duplicate Grant upgraded failed readiness");
    f.first.TickSharedDropPickupRetries(0.25f);
    auto retry = Fixture::take(f.first, Command::PickupReady);
    require(retry.flags == nack.flags, "retry upgraded failed readiness");
    Fixture::deliver(f.host, nack);
    f.broadcast();
    require(f.row(f.host).live && f.host.durable.empty() && f.firstItem.nativeCalls == 0,
        "failed baseline preparation committed ownership");
}

void optionalJournalFailureAllowsGrant() {
    Fixture f;
    f.first.failJournal = true;
    const auto grant = f.prepare();
    const auto ready = Fixture::take(f.first, Command::PickupReady);
    require((ready.flags & CoopProtocol::kSharedDropFlagPickupSucceeded) != 0 &&
        f.first.snapshotAttempts == 1 && f.first.journalAttempts == 1,
        "optional journal failure rejected a durable canonical baseline");
    require(f.first.m_localInventoryDirty &&
        f.first.m_localInventoryDirtySaveKey == f.first.m_currentHostSaveStateKey &&
        f.first.m_localInventoryDirtyHostAccountToken == f.host.token,
        "optional journal failure did not retain exact dirty retry scope");
    Fixture::deliver(f.first, grant);
    const auto duplicate = Fixture::take(f.first, Command::PickupReady);
    require(duplicate.flags == ready.flags && f.first.snapshotAttempts == 1 && f.first.journalAttempts == 1,
        "duplicate grant reran preparation or revoked canonical readiness");
    Fixture::deliver(f.host, ready);
    const auto commit = Fixture::take(f.host, Command::PickupCommit);
    Fixture::deliver(f.first, commit);
    Fixture::deliver(f.host, Fixture::take(f.first, Command::PickupResult));
    Fixture::deliver(f.first, commit);
    require(f.firstItem.nativeCalls == 1 && f.firstItem.inventoryCount == 1 &&
        f.row(f.first).pickupDeliveryStored && f.row(f.host).pickupResultSucceeded && !f.row(f.host).live,
        "optional backup failure broke exactly-once committed canonical delivery");
}

void duplicatePhases() {
    Fixture f;
    f.request(f.first, f.firstItem);
    auto grant = Fixture::take(f.host, Command::PickupGrant);
    Packet request;
    require(f.first.BuildSharedDropPacket(request, Command::PickupRequest, f.row(f.first)),
        "build duplicate request");
    request.sequence = grant.sequence;
    Fixture::deliver(f.host, request);
    auto grantReplay = Fixture::take(f.host, Command::PickupGrant);
    require(grant.objectVersion == grantReplay.objectVersion && grant.sequence == grantReplay.sequence,
        "duplicate Request changed reservation identity");
    Fixture::deliver(f.first, grant);
    auto ready = Fixture::take(f.first, Command::PickupReady);
    Fixture::deliver(f.first, grantReplay);
    auto readyReplay = Fixture::take(f.first, Command::PickupReady);
    require(ready.objectVersion == readyReplay.objectVersion && ready.sequence == readyReplay.sequence &&
        f.firstItem.nativeCalls == 0, "duplicate Grant changed preparation or acquired inventory");
    Fixture::deliver(f.host, ready);
    auto commit = Fixture::take(f.host, Command::PickupCommit);
    Fixture::deliver(f.host, readyReplay);
    auto commitReplay = Fixture::take(f.host, Command::PickupCommit);
    require(f.host.durable.size() == 1 && commit.objectVersion == commitReplay.objectVersion,
        "duplicate Ready persisted or versioned ownership twice");
    Fixture::deliver(f.first, commit);
    auto result = Fixture::take(f.first, Command::PickupResult);
    Fixture::deliver(f.first, commitReplay);
    Fixture::deliver(f.host, result);
    Fixture::deliver(f.host, result);
    require(f.firstItem.nativeCalls == 1 && f.firstItem.inventoryCount == 1 &&
        f.row(f.host).pickupResultSucceeded, "duplicate commit/result duplicated native inventory");
}

void nativeDeliveryFailureRetainsEntitlement() {
    Fixture f;
    f.firstItem.accept = false;
    Fixture::deliver(f.first, f.commit());
    require(f.firstItem.nativeCalls == 1 && f.firstItem.inventoryCount == 0 &&
        f.first.sent.empty() && !f.row(f.host).live && f.host.durable.size() == 1 &&
        f.row(f.first).pickupCommitted && !f.row(f.first).localPickupGranted,
        "native delivery refusal lost entitlement or returned it to world");
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    require(!f.row(f.host).live, "delivery refusal/disconnect undid committed ownership");
}

void receiptFailureRetriesWithoutNativeRepeat() {
    Fixture f;
    f.first.failReceipt = true;
    Fixture::deliver(f.first, f.commit());
    require(f.firstItem.nativeCalls == 1 && f.firstItem.inventoryCount == 1 &&
        !f.row(f.first).pickupDeliveryStored && f.first.sent.empty(),
        "failed receipt acknowledged durable delivery");
    f.first.TickSharedDropPickupRetries(0.25f);
    require(f.firstItem.nativeCalls == 1 && f.first.sent.empty(),
        "receipt retry repeated native mutation or acknowledged failure");
    f.first.failReceipt = false;
    f.first.TickSharedDropPickupRetries(0.25f);
    Fixture::deliver(f.host, Fixture::take(f.first, Command::PickupResult));
    require(f.firstItem.nativeCalls == 1 && f.row(f.first).pickupDeliveryStored &&
        f.row(f.host).pickupResultSucceeded, "receipt recovery did not finish without duplication");
}

void readinessMutationRejected(const std::function<void(Packet&)>& mutate, bool direct = false) {
    Fixture f;
    auto ready = f.ready();
    mutate(ready);
    if (direct) f.host.HandleSharedDrop(ready);
    else Fixture::deliver(f.host, ready);
    require(f.host.durable.empty() && f.row(f.host).live && f.row(f.host).pickupPending &&
        f.row(f.host).version == 2 && f.firstItem.nativeCalls == 0,
        "forged/stale readiness changed reservation or acquired inventory");
}

void grantMutationRejected(const std::function<void(Packet&)>& mutate, bool direct = false) {
    Fixture f;
    f.request(f.first, f.firstItem);
    auto grant = Fixture::take(f.host, Command::PickupGrant);
    mutate(grant);
    if (direct) f.first.HandleSharedDrop(grant);
    else Fixture::deliver(f.first, grant);
    require(f.firstItem.nativeCalls == 0 && f.first.snapshotAttempts == 0 &&
        f.row(f.first).version == 1 && !f.row(f.first).pickupGrantReceived,
        "invalid preparation reached native mutation/baseline persistence");
}

void committedFailureCannotUndo() {
    Fixture f;
    auto commit = f.commit();
    Fixture::deliver(f.first, commit);
    auto result = Fixture::take(f.first, Command::PickupResult);
    result.flags = 0;
    Fixture::deliver(f.host, result);
    f.expire();
    require(!f.row(f.host).live && f.row(f.host).pickupCommitted && f.host.durable.size() == 1,
        "failed delivery result undid durable ownership");
}

void contentionExactlyOneWinner() {
    Fixture f;
    f.request(f.first, f.firstItem);
    auto grant = Fixture::take(f.host, Command::PickupGrant);
    f.request(f.second, f.secondItem);
    auto release = Fixture::take(f.host, Command::PickupRelease);
    Fixture::deliver(f.second, release);
    require(!f.row(f.second).pickupPending && f.secondItem.nativeCalls == 0,
        "losing contender remained pending or acquired native item");
    Fixture::deliver(f.first, grant);
    Fixture::deliver(f.host, Fixture::take(f.first, Command::PickupReady));
    f.broadcast();
    require(f.firstItem.inventoryCount == 1 && f.secondItem.inventoryCount == 0 &&
        !f.row(f.second).live && f.host.durable.size() == 1, "contention produced multiple winners");
}

void observerAfterCancelledWinner() {
    Fixture f;
    f.first.failSnapshot = true;
    Fixture::deliver(f.host, f.ready());
    f.broadcast();
    require(f.row(f.second).version == f.row(f.host).version, "cancel omitted observer version refresh");
    f.request(f.second, f.secondItem);
    f.broadcast();
    Fixture::deliver(f.host, Fixture::take(f.second, Command::PickupReady));
    f.broadcast();
    require(f.secondItem.inventoryCount == 1 && f.firstItem.inventoryCount == 0,
        "observer's first interaction failed after cancelled winner");
}

void losingContenderCanCancelAgainAtSameVersion() {
    Fixture f;
    f.request(f.first, f.firstItem);
    const auto heldGrant = Fixture::take(f.host, Command::PickupGrant);
    (void)heldGrant;
    f.request(f.second, f.secondItem);
    Fixture::deliver(f.second, Fixture::take(f.host, Command::PickupRelease));
    require(!f.row(f.second).pickupPending && f.row(f.second).version == f.row(f.host).version,
        "initial losing contender did not refresh reservation version");
    f.request(f.second, f.secondItem);
    Fixture::deliver(f.second, Fixture::take(f.host, Command::PickupRelease));
    require(!f.row(f.second).pickupPending && f.secondItem.nativeCalls == 0,
        "same-version rejection left repeat contender locally pending");
}

void requestSendFailureThenRetry() {
    Fixture f;
    f.first.failSends = true;
    require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "send failure"),
        "failed request bypassed native suppression");
    require(!f.row(f.first).pickupPending && f.first.sent.empty() && f.firstItem.nativeCalls == 0,
        "failed send mutated item or left pending state");
    f.first.failSends = false;
    f.finish();
    require(f.firstItem.inventoryCount == 1, "request failed to recover");
}

void repeatedDisconnectIdempotent() {
    Fixture f;
    f.prepare();
    f.host.m_remotePeers.erase(f.first.token);
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    const auto version = f.row(f.host).version;
    const auto packets = f.host.sent.size();
    f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
    require(f.row(f.host).version == version && f.host.sent.size() == packets,
        "peer removal cancelled reservation twice");
}

void unreservedReadyRejected() {
    Fixture f;
    Packet ready;
    require(f.first.BuildSharedDropPacket(ready, Command::PickupReady, f.row(f.first), f.host.token),
        "build unexpected Ready");
    ready.sequence = 41;
    ready.flags = CoopProtocol::kSharedDropFlagPickupSucceeded;
    Fixture::deliver(f.host, ready);
    require(f.host.durable.empty() && f.row(f.host).live && !f.row(f.host).pickupPending,
        "unreserved Ready created ownership");
}

void personalLootUntouched() {
    Fixture f;
    CArkItem personal{900};
    require(!f.first.ShouldDeferNativeSharedItemPickup(&personal, 1, "personal map loot"),
        "shared-drop transaction intercepted personal map loot");
}

Packet newSpawnPacket(Fixture& f, bool toHost) {
    ModMain& sender = toHost ? f.first : f.host;
    auto record = f.row(sender);
    record.stableSpawnId = stable + 1;
    Packet packet;
    require(sender.BuildSharedDropPacket(packet, Command::Spawn, record), "build Spawn packet");
    return packet;
}

void staleSessionSpawnRejected(bool toHost, bool zero) {
    Fixture f;
    ModMain& recipient = toHost ? f.host : f.first;
    auto packet = newSpawnPacket(f, toHost);
    packet.hostSessionId = zero ? 0 : packet.hostSessionId + 1;
    Fixture::deliver(recipient, packet);
    require(recipient.materializeCalls == 0 && recipient.m_sharedDrops.size() == 1 &&
        recipient.m_sharedDrops.count(packet.stableSpawnId) == 0,
        "stale or missing Host session nonce reached Spawn materialization");
}

void currentSessionSpawnAccepted() {
    for (bool toHost : {true, false}) {
        Fixture f;
        ModMain& recipient = toHost ? f.host : f.first;
        recipient.allowMaterialize = true;
        auto packet = newSpawnPacket(f, toHost);
        Fixture::deliver(recipient, packet);
        require(recipient.materializeCalls == 1 && recipient.m_sharedDrops.count(packet.stableSpawnId) == 1,
            "current-session Spawn did not reach materialization dispatch");
    }
}

void clientWithoutHostSnapshotRejectsSpawn() {
    Fixture f;
    auto packet = newSpawnPacket(f, false);
    f.first.m_sharedDropHostSessionId = 0;
    Fixture::deliver(f.first, packet);
    require(f.first.materializeCalls == 0 && f.first.m_sharedDrops.size() == 1,
        "Client without authoritative Host snapshot accepted Spawn nonce");
}

void reorderedLossyTransportNeverDuplicatesOwnership() {
    struct InFlight { unsigned destination; Packet packet; };
    std::mt19937 random(0x60ec5c);
    for (unsigned scenario = 0; scenario < 160; ++scenario) {
        Fixture f;
        std::vector<InFlight> network;
        auto collect = [&] {
            for (unsigned index = 0; index < 3; ++index) {
                ModMain& source = index == 0 ? f.host : index == 1 ? f.first : f.second;
                auto sent = source.sent;
                source.sent.clear();
                for (const auto& event : sent) {
                    if (index != 0) network.push_back({0, event.packet});
                    else {
                        if (event.target == 0 || event.target == f.first.token)
                            network.push_back({1, event.packet});
                        if (event.target == 0 || event.target == f.second.token)
                            network.push_back({2, event.packet});
                    }
                }
            }
        };
        require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "lossy network"),
            "network scenario bypassed shared pickup");
        for (unsigned tick = 0; tick < 220; ++tick) {
            collect();
            if (!network.empty()) {
                const size_t selected = random() % network.size();
                const auto event = network[selected];
                network.erase(network.begin() + selected);
                const unsigned outcome = random() % 10;
                if (outcome >= 2) {
                    ModMain& destination = event.destination == 0 ? f.host :
                        event.destination == 1 ? f.first : f.second;
                    Fixture::deliver(destination, event.packet);
                    if (outcome == 9) network.push_back(event);
                }
            }
            f.host.TickSharedDropPickupRetries(0.25f);
            f.first.TickSharedDropPickupRetries(0.25f);
            const unsigned assigned = static_cast<unsigned>(f.host.durable.size());
            require(assigned <= 1 && f.firstItem.inventoryCount <= assigned &&
                f.firstItem.nativeCalls <= 1 && !(f.row(f.host).live && assigned != 0),
                "reordering/loss/duplicates produced multiple ownership or uncommitted native pickup");
            require(f.row(f.host).live || assigned == 1,
                "world item vanished without a durable assigned entitlement");
        }
        f.host.m_remotePeers.erase(f.first.token);
        f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
        require(!f.row(f.host).pickupPending &&
            (f.row(f.host).live != !f.host.durable.empty()),
            "disconnect after lossy delivery left unowned escrow or world plus entitlement");
    }
}

void competingLossyTransportNeverDuplicatesOwnership() {
    struct InFlight { unsigned destination; Packet packet; };
    std::mt19937 random(0xc060ec5c);
    for (unsigned scenario = 0; scenario < 192; ++scenario) {
        Fixture f;
        std::vector<InFlight> network;
        auto collect = [&] {
            for (unsigned index = 0; index < 3; ++index) {
                ModMain& source = index == 0 ? f.host : index == 1 ? f.first : f.second;
                auto sent = source.sent;
                source.sent.clear();
                for (const auto& event : sent) {
                    if (index != 0) network.push_back({0, event.packet});
                    else {
                        if (event.target == 0 || event.target == f.first.token)
                            network.push_back({1, event.packet});
                        if (event.target == 0 || event.target == f.second.token)
                            network.push_back({2, event.packet});
                    }
                }
            }
        };
        require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "simultaneous first") &&
            f.second.ShouldDeferNativeSharedItemPickup(&f.secondItem, 1, "simultaneous second"),
            "concurrent request bypassed shared-drop transaction");
        for (unsigned tick = 0; tick < 240; ++tick) {
            collect();
            if (!network.empty()) {
                const size_t selected = random() % network.size();
                const auto event = network[selected];
                network.erase(network.begin() + selected);
                const unsigned outcome = random() % 10;
                if (outcome >= 2) {
                    ModMain& destination = event.destination == 0 ? f.host :
                        event.destination == 1 ? f.first : f.second;
                    Fixture::deliver(destination, event.packet);
                    if (outcome >= 8) network.push_back(event);
                }
            }
            f.host.TickSharedDropPickupRetries(0.25f);
            f.first.TickSharedDropPickupRetries(0.25f);
            f.second.TickSharedDropPickupRetries(0.25f);
            const unsigned assigned = static_cast<unsigned>(f.host.durable.size());
            require(assigned <= 1 &&
                f.firstItem.inventoryCount + f.secondItem.inventoryCount <= assigned &&
                f.firstItem.nativeCalls + f.secondItem.nativeCalls <= 1 &&
                !(f.row(f.host).live && assigned != 0),
                "concurrent lossy contenders acquired two copies or an uncommitted item");
            if (f.firstItem.inventoryCount != 0)
                require(f.host.durable.front().account == f.first.token,
                    "first contender acquired a drop assigned to a different account");
            if (f.secondItem.inventoryCount != 0)
                require(f.host.durable.front().account == f.second.token,
                    "second contender acquired a drop assigned to a different account");
        }
        f.host.m_remotePeers.erase(f.first.token);
        f.host.m_remotePeers.erase(f.second.token);
        f.host.ReleaseSharedDropReservationsForPeer(f.first.token);
        f.host.ReleaseSharedDropReservationsForPeer(f.second.token);
        require(!f.row(f.host).pickupPending &&
            (f.row(f.host).live != !f.host.durable.empty()),
            "concurrent contenders left an unowned reservation or recreated a committed world copy");
    }
}

int main(int argc, char**) {
    if (argc != 1) return 2;
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"prepare_has_no_native_mutation", prepareHasNoNativeMutation},
        {"commit_persists_before_native_pickup", successfulPickup},
        {"host_local_implicit_ready_durable_commit", hostLocalPickupUsesDurableCommit},
        {"split_area_pickup_uses_coordinator_without_same_area_gate", [] {
            Fixture f;
            f.host.m_localLevelId = 20;
            f.host.gameplayReady = f.first.gameplayReady = false;
            f.host.m_remotePeers.at(2).sessionReady = false;
            f.finish();
            require(f.firstItem.inventoryCount == 1 && f.firstItem.nativeCalls == 1 &&
                f.host.durable.size() == 1 && !f.row(f.host).live && f.entities.GetEntity(103),
                "remote-area decision failed or mutated the coordinator's unrelated entity");
        }},
        {"committed_pickup_replayed_to_recipient_after_native_travel", [] {
            Fixture f;
            f.commit(); // The commit is lost while the recipient unloads.
            f.first.DetachSharedDropEntities();
            f.first.m_localLevelId = 20;
            f.host.m_remotePeers.at(2).levelId = 20;
            f.host.remoteToken = f.first.token;
            f.host.sent.clear();
            f.host.ReplaySharedDrops();
            auto commit = Fixture::take(f.host, Command::PickupCommit);
            require(commit.areaId == 10 && commit.targetPeerHash == 2,
                "travel changed source identity or lost the assigned decision");
            Fixture::deliver(f.first, commit);
            require(f.row(f.first).pickupCommitted && !f.row(f.first).live &&
                f.row(f.first).localEntityId == INVALID_ENTITYID && f.firstItem.nativeCalls == 0,
                "travel commit was dropped or touched an unloaded native entity");
            auto ready = commit;
            ready.command = static_cast<uint16_t>(Command::PickupReady);
            ready.sourcePeerHash = 2; ready.targetPeerHash = 1;
            ready.flags = CoopProtocol::kSharedDropFlagPickupSucceeded;
            Fixture::deliver(f.host, ready);
            Fixture::take(f.host, Command::PickupCommit);
            require(f.host.durable.size() == 1, "cross-area retry assigned the item twice");
        }},
        {"new_pickup_cannot_commit_after_recipient_left_source_area", [] {
            Fixture f;
            auto ready = f.ready();
            f.host.m_remotePeers.at(2).levelId = 20;
            Fixture::deliver(f.host, ready);
            require(f.host.durable.empty() && !f.row(f.host).pickupCommitted,
                "cross-area exception admitted a new ownership decision");
            f.expire();
            require(f.row(f.host).live && !f.row(f.host).pickupPending,
                "abandoned preparation did not return its source to the world");
        }},
        {"remote_area_spawn_is_metadata_not_foreign_materialization", [] {
            Fixture f;
            f.host.m_localLevelId = 20;
            auto packet = newSpawnPacket(f, true);
            Fixture::deliver(f.host, packet);
            require(f.host.materializeCalls == 0 && f.host.m_sharedDrops.count(packet.stableSpawnId) == 1 &&
                f.host.m_sharedDrops.at(packet.stableSpawnId).localEntityId == INVALID_ENTITYID,
                "remote publication spawned a native object in the wrong area");
        }},
        {"coordinator_transition_accepts_metadata_without_native_access", [] {
            Fixture f;
            f.host.m_arkLevelTransitionLoadActive = true;
            f.host.m_saveLoadGuardActive = true;
            auto packet = newSpawnPacket(f, true);
            require(f.host.IsSharedDropServiceReady() && !f.host.IsSharedDropWorldReady(),
                "native area loading incorrectly disabled metadata coordination");
            Fixture::deliver(f.host, packet);
            require(f.host.materializeCalls == 0 && f.host.m_sharedDrops.count(packet.stableSpawnId),
                "in-flight publication lost during native travel");
        }},
        {"travel_detaches_bindings_but_keeps_live_and_consumed_identity", [] {
            Fixture f;
            f.row(f.host).pickupCommitted = true;
            f.row(f.host).live = false;
            f.host.m_sharedDrops.emplace(88, f.row(f.host));
            f.host.m_sharedDrops.at(88).live = true;
            f.host.DetachSharedDropEntities();
            require(f.host.m_sharedDrops.size() == 2 && f.host.m_sharedDropByEntityId.empty() &&
                f.row(f.host).localEntityId == INVALID_ENTITYID && f.row(f.host).pickupCommitted &&
                f.host.m_sharedDrops.at(88).live && f.host.m_sharedDropRecoveryRequired && f.entities.GetEntity(103),
                "travel deleted native entities or forgot source lifetimes");
        }},
        {"solo_redrop_starts_new_lifetime_not_old_delivery", [] {
            Fixture f;
            f.row(f.host).live = false;
            f.row(f.host).pickupCommitted = true;
            f.row(f.host).localPickupGranted = true;
            f.row(f.host).pickupWinnerPeerHash = f.host.token;
            f.host.m_hasRemoteEndpoint = false;
            f.host.m_remotePeers.clear();
            f.host.gameplayReady = false;
            f.host.OnNativeSharedItemDropped(&f.hostItem, 1, "native redrop");
            auto next = f.host.m_sharedDropByEntityId.at(103);
            require(next != stable && f.row(f.host).localEntityId == INVALID_ENTITYID &&
                !f.row(f.host).live && f.host.m_sharedDrops.at(next).live,
                "redrop reused or revived its old committed lifetime");
            f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "reacquire redrop");
            f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "repeat");
            require(f.hostItem.nativeCalls == 1 && f.hostItem.inventoryCount == 1,
                "redrop was locked by the old receipt or granted twice");
        }},
        {"return_rebinds_live_source_without_respawning", [] {
            Fixture f;
            f.entities.entities.at(103).name = "CoopSharedDrop_77";
            f.host.DetachSharedDropEntities();
            f.host.RecoverSavedSharedDrops();
            require(f.row(f.host).localEntityId == 103 && f.host.m_sharedDropByEntityId.at(103) == stable &&
                f.host.materializeCalls == 0 && f.entities.GetEntity(103),
                "return duplicated or lost the native saved source");
        }},
        {"continuing_offline_host_redrop_gets_fresh_identity_without_network", [] {
            for (const bool committed : {false, true}) {
                Fixture f;
                f.entities.GetEntity(103)->name = "CoopSharedDrop_77";
                f.row(f.host).live = !committed;
                f.row(f.host).pickupCommitted = committed;
                f.host.m_networkMode = CoopNetworkMode::Off;
                f.host.continuingHostWorld = true;
                f.host.DetachSharedDropEntities();
                f.host.OnNativeSharedItemDropping(&f.hostItem);
                f.host.OnNativeSharedItemDropped(&f.hostItem, 1, "offline native redrop");
                const auto next = f.host.m_sharedDropByEntityId.at(103);
                require(next != stable && !f.row(f.host).live && f.host.m_sharedDrops.at(next).live &&
                    f.row(f.host).localEntityId == INVALID_ENTITYID && f.host.sent.empty(),
                    "offline redrop revived its old receipt, sent packets, or lost its new source");
            }
        }},
        {"saved_private_remainder_is_not_readopted_as_public_loot", [] {
            Fixture f;
            f.entities.GetEntity(103)->name = "CoopSharedDrop_77";
            f.host.m_sharedDrops.clear();
            f.host.m_sharedDropByEntityId.clear();
            f.host.m_localSharedDropDeliveries.push_back({19, stable, f.hostItem.GetArchetype(), 3, 1, 31, 37});
            f.host.m_sharedDropHostSessionId = 19;
            f.host.m_sharedDropRecoveryRequired = true;
            f.host.RecoverSavedSharedDrops();
            require(!f.entities.GetEntity(103) && f.host.m_sharedDrops.empty() &&
                f.host.m_localSharedDropDeliveries.size() == 1 &&
                f.host.m_localSharedDropDeliveries[0].deliveredCount == 1,
                "saved overflow source became public or lost its paired remainder receipt");
        }},
        {"continuing_offline_host_rebinds_only_its_assigned_remainder", [] {
            Fixture f;
            f.hostItem.accept = false;
            f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "prepare owned remainder");
            f.entities.GetEntity(103)->name = "CoopSharedDrop_77";
            f.host.DetachSharedDropEntities();
            f.host.m_networkMode = CoopNetworkMode::Off;
            f.host.continuingHostWorld = true;
            f.hostItem.accept = true;
            require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "offline remainder") &&
                f.hostItem.inventoryCount == 1 && f.row(f.host).localEntityId == 103,
                "offline native pickup bypassed its assigned remainder receipt");
            require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "offline repeat") &&
                f.hostItem.inventoryCount == 1,
                "offline assigned remainder granted twice");
        }},
        {"ordinary_offline_world_does_not_publish_personal_drops", [] {
            Fixture f;
            f.host.m_networkMode = CoopNetworkMode::Off;
            f.host.OnNativeSharedItemDropping(&f.hostItem);
            f.host.OnNativeSharedItemDropped(&f.hostItem, 1, "single player drop");
            require(f.host.m_sharedDrops.size() == 1 && f.host.m_sharedDropByEntityId.at(103) == stable &&
                f.entities.GetEntity(103)->name.empty() && f.host.sent.empty(),
                "offline capture changed ordinary vanilla loot");
        }},
        {"return_deduplicates_saved_entities_with_same_public_source", [] {
            Fixture f;
            CArkItem duplicate{104};
            CArkItem::items.emplace(104, &duplicate);
            f.entities.entities.emplace(104, IEntity{104});
            f.entities.entities.at(103).name = "CoopSharedDrop_77";
            f.entities.entities.at(104).name = "CoopSharedDrop_77";
            f.host.DetachSharedDropEntities();
            f.host.RecoverSavedSharedDrops();
            unsigned copies = 0;
            for (const auto& entity : f.entities.entities)
                if (entity.second.name == "CoopSharedDrop_77") ++copies;
            require(copies == 1 && f.host.m_sharedDropByEntityId.size() == 1,
                "saved duplicate entities survived as multiple physical sources");
        }},
        {"return_removes_consumed_source_from_older_native_area_snapshot", [] {
            Fixture f;
            f.entities.entities.at(103).name = "CoopSharedDrop_77";
            f.row(f.host).live = false;
            f.row(f.host).pickupCommitted = true;
            f.host.DetachSharedDropEntities();
            f.host.RecoverSavedSharedDrops();
            require(!f.entities.GetEntity(103) && !f.row(f.host).live &&
                f.row(f.host).localEntityId == INVALID_ENTITYID && f.host.materializeCalls == 0,
                "return resurrected the saved copy of an already acquired drop");
        }},
        {"native_travel_preserves_catalog_but_rollback_does_not", [] {
            Fixture f;
            f.host.ResetSharedDropState("native travel", true);
            require(f.host.m_sharedDrops.size() == 1 && f.row(f.host).live &&
                f.row(f.host).localEntityId == INVALID_ENTITYID && f.entities.GetEntity(103),
                "travel reset erased the catalog or deleted the native source");
            f.host.ResetSharedDropState("explicit checkpoint rollback");
            require(f.host.m_sharedDrops.empty() && f.host.m_sharedDropByEntityId.empty(),
                "rollback inherited source decisions from the discarded world");
        }},
        {"native_checkpoint_adoption_does_not_claim_inventory_objects", [] {
            Fixture f;
            f.host.m_sharedDrops.clear(); f.host.m_sharedDropByEntityId.clear();
            f.host.m_sharedDropRecoveryRequired = true;
            f.entities.entities.at(103).name = "CoopSharedDrop_77";
            ArkPlayer::instance.inventory.members.insert(103);
            f.host.RecoverSavedSharedDrops();
            require(f.host.m_sharedDrops.empty() && f.entities.GetEntity(103),
                "saved inventory object became public loot");
            ArkPlayer::instance.inventory.members.clear();
            f.host.m_sharedDropRecoveryRequired = true;
            f.host.RecoverSavedSharedDrops();
            require(f.row(f.host).localEntityId == 103 && f.row(f.host).count == 1,
                "saved native public source was not adopted");
        }},
        {"offline_native_collection_is_not_replayed_as_shared_loot_on_rehost", [] {
            Fixture f;
            f.entities.entities.at(103).name = "CoopSharedDrop_77";
            f.host.ResetSharedDropState("continuing world stop", true);
            f.host.m_networkMode = CoopNetworkMode::Off;
            require(!f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "vanilla offline pickup"),
                "offline shared source blocked vanilla");
            ArkPlayer::instance.inventory.members.insert(103);
            f.host.m_networkMode = CoopNetworkMode::Host;
            f.host.ResetSharedDropState("continuing world rehost", true);
            f.host.RecoverSavedSharedDrops();
            require(!f.row(f.host).live && f.host.materializeCalls == 0 &&
                f.entities.GetEntity(103) && ArkPlayer::instance.inventory.Contains(103),
                "rehost replayed or removed the natively collected inventory object");
        }},
        {"return_materializes_client_publication_missing_from_native_area_save", [] {
            Fixture f;
            f.host.m_localLevelId = 20;
            auto packet = newSpawnPacket(f, true);
            Fixture::deliver(f.host, packet);
            f.host.m_localLevelId = 10;
            f.host.m_sharedDropRecoveryRequired = true;
            f.host.allowMaterialize = true;
            f.host.materializeEntity = 104;
            f.host.RecoverSavedSharedDrops();
            require(f.host.materializeCalls == 1 &&
                f.host.m_sharedDrops.at(packet.stableSpawnId).localEntityId == 104,
                "client source published while coordinator away vanished on return");
        }},
        {"unloaded_source_replay_uses_cached_transform", [] {
            Fixture f;
            f.host.m_localLevelId = 20;
            f.row(f.host).position = Vec3(3, 4, 5);
            Packet packet;
            require(f.host.BuildSharedDropPacket(packet, Command::Spawn, f.row(f.host)) &&
                packet.px == 3 && packet.py == 4 && packet.pz == 5 && packet.qw == 1,
                "foreign-area replay used a recycled EntityId or zeroed transform");
        }},
        {"consumed_source_cannot_be_resurrected_by_newer_spawn", [] {
            Fixture f;
            f.finish();
            Packet packet;
            f.first.BuildSharedDropPacket(packet, Command::Spawn, f.row(f.first));
            packet.objectVersion += 20;
            packet.flags = CoopProtocol::kSharedDropFlagLive;
            Fixture::deliver(f.host, packet);
            require(!f.row(f.host).live && f.row(f.host).pickupCommitted && f.host.materializeCalls == 0,
                "delayed publisher overwrote committed ownership");
        }},
        {"source_area_must_match_authenticated_peer", [] {
            Fixture f;
            auto packet = newSpawnPacket(f, true);
            ++packet.areaId;
            Fixture::deliver(f.host, packet);
            require(!f.host.m_sharedDrops.count(packet.stableSpawnId) && f.host.materializeCalls == 0,
                "publisher wrote a source into an unrelated area");
        }},
        {"shared_drop_transport_uses_payload_area_not_coordinator_area", [] {
            require(!IsAreaScopedReliablePayload(static_cast<uint16_t>(CoopProtocol::PacketType::SharedDrop)) &&
                IsAreaScopedReliablePayload(static_cast<uint16_t>(CoopProtocol::PacketType::AreaObjectEvent)),
                "drop coordination was routed away before its area validation or other world traffic lost its fence");
        }},
        {"not_world_ready_source_cannot_request_pickup", [] {
            Fixture f;
            f.host.m_remotePeers.at(2).sessionFlags = 0;
            f.request(f.first, f.firstItem);
            require(!f.row(f.host).pickupPending && f.host.durable.empty() && f.host.sent.empty(),
                "unready peer acquired a reservation");
        }},
        {"host_without_ready_peer_can_pick_up_in_loaded_world", [] {
            for (bool endpoint : {false, true}) {
                Fixture f;
                f.host.gameplayReady = false;
                f.host.m_hasRemoteEndpoint = endpoint;
                for (auto& peer : f.host.m_remotePeers) peer.second.sessionFlags = 0;
                require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "Host alone or peer loading"), "Host bypassed escrow");
                require(f.hostItem.inventoryCount == 1 && f.hostItem.nativeCalls == 1 &&
                    f.host.durable.size() == 1 && !f.row(f.host).live && f.host.sent.empty(),
                    "loaded Host pickup depended on a ready Client");
                f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "repeat");
                require(f.hostItem.inventoryCount == 1 && f.hostItem.nativeCalls == 1, "solo Host duplicate grant");
            }
        }},
        {"local_world_transition_fences_shared_mutation", [] {
            for (int guard = 0; guard < 5; ++guard) {
                Fixture f;
                if (guard == 0) ArkPlayer::instance.ready = false;
                if (guard == 1) f.host.m_saveLoadGuardActive = true;
                if (guard == 2) f.host.m_pendingPostLoadResync = true;
                if (guard == 3) f.host.m_arkLevelTransitionLoadActive = true;
                if (guard == 4) f.host.m_runtimeTransitionCleanupPrepared = true;
                require(f.host.ShouldDeferNativeSharedItemPickup(&f.hostItem, 1, "transition"), "transition bypassed fence");
                require(f.hostItem.nativeCalls == 0 && f.host.durable.empty() && f.host.sent.empty(), "transition mutated shared inventory");
            }
        }},
        {"host_native_refusal_retains_entitlement", hostNativeRefusalKeepsAssignedEntitlement},
        {"host_store_failure_cancels_and_retries", hostOwnershipStoreFailureCancelsAndRetries},
        {"host_publication_failure_keeps_committed_ownership", hostCommitPublicationFailureDoesNotRevokeOwnership},
        {"session_not_ready_fences_tracked_pickup", sessionNotReadyFencesTrackedPickup},
        {"session_not_ready_untracked_and_off_stay_vanilla", sessionNotReadyPreservesUntrackedVanillaPath},
        {"session_not_ready_committed_item_uses_receipt_helper", sessionNotReadyPreservesCommittedReceiptPath},
        {"session_not_ready_pending_reservation_stays_fenced", sessionNotReadyKeepsPendingReservationFenced},
        {"timeout_before_grant_delivery", timeoutBeforeGrantDelivery},
        {"timeout_after_prepare_delayed_ready_rejected", timeoutAfterPrepareBeforeReady},
        {"real_monotonic_deadline_independent_of_frame_delta", wallClockReservationDeadline},
        {"disconnect_precommit_cancels", disconnectBeforeCommit},
        {"lost_commit_disconnect_retains_entitlement", lostCommitThenDisconnect},
        {"lost_result_disconnect_never_recreates_world", lostResultAfterAcquisition},
        {"ownership_store_failure_cancels", storeFailureCancels},
        {"baseline_snapshot_failure_cancels_including_duplicates", baselineFailureCancels},
        {"optional_journal_failure_keeps_canonical_grant_and_dirty_retry", optionalJournalFailureAllowsGrant},
        {"duplicate_request_grant_ready_commit_result", duplicatePhases},
        {"native_delivery_refusal_retains_entitlement", nativeDeliveryFailureRetainsEntitlement},
        {"receipt_failure_retries_without_native_repeat", receiptFailureRetriesWithoutNativeRepeat},
        {"ready_wrong_epoch", [] { readinessMutationRejected([](auto& p) { ++p.worldEpoch; }); }},
        {"ready_wrong_save", [] { readinessMutationRejected([](auto& p) { ++p.hostSaveKeyHash; }); }},
        {"ready_wrong_area", [] { readinessMutationRejected([](auto& p) { ++p.areaId; }); }},
        {"ready_wrong_session", [] { readinessMutationRejected([](auto& p) { ++p.hostSessionId; }); }},
        {"ready_wrong_target", [] { readinessMutationRejected([](auto& p) { ++p.targetPeerHash; }); }},
        {"ready_wrong_source", [] { readinessMutationRejected([](auto& p) { ++p.sourcePeerHash; }, true); }},
        {"ready_wrong_version", [] { readinessMutationRejected([](auto& p) { ++p.objectVersion; }); }},
        {"ready_wrong_transaction", [] { readinessMutationRejected([](auto& p) { ++p.sequence; }); }},
        {"grant_wrong_epoch", [] { grantMutationRejected([](auto& p) { ++p.worldEpoch; }); }},
        {"grant_wrong_session", [] { grantMutationRejected([](auto& p) { ++p.hostSessionId; }); }},
        {"grant_wrong_source", [] { grantMutationRejected([](auto& p) { ++p.sourcePeerHash; }, true); }},
        {"grant_wrong_target", [] { grantMutationRejected([](auto& p) { ++p.targetPeerHash; }); }},
        {"failed_delivery_ack_cannot_undo_commit", committedFailureCannotUndo},
        {"contention_exactly_one_winner", contentionExactlyOneWinner},
        {"observer_first_interaction_after_cancel", observerAfterCancelledWinner},
        {"repeat_losing_contender_same_version_released", losingContenderCanCancelAgainAtSameVersion},
        {"request_send_failure_then_retry", requestSendFailureThenRetry},
        {"repeated_disconnect_idempotent", repeatedDisconnectIdempotent},
        {"unreserved_ready_rejected", unreservedReadyRejected},
        {"personal_map_loot_not_intercepted", personalLootUntouched},
        {"loaded_public_source_fenced_until_bound", [] {
            Fixture f;
            f.first.m_sharedDropByEntityId.clear();
            f.entities.entities.at(101).name = "CoopSharedDrop_123";
            require(f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "loaded"), "unbound public source bypassed escrow");
            f.first.m_networkMode = CoopNetworkMode::Off;
            require(!f.first.ShouldDeferNativeSharedItemPickup(&f.firstItem, 1, "offline"), "offline vanilla pickup blocked");
        }},
        {"snapshot_end_requires_current_host_context", [] {
            Fixture f;
            Packet end = newSpawnPacket(f, false);
            end.command = static_cast<uint16_t>(Command::SnapshotEnd);
            end.stableSpawnId = 0; end.archetypeId = 0;
            ++end.hostSessionId;
            f.first.HandleSharedDrop(end);
            require(f.first.pruneCalls == 0, "stale snapshot pruned current world");
            --end.hostSessionId;
            f.first.HandleSharedDrop(end);
            require(f.first.pruneCalls == 1, "current host snapshot not completed");
            end.sourcePeerHash = 2;
            f.host.HandleSharedDrop(end);
            require(f.host.pruneCalls == 0, "client pruned host world");
        }},
        {"initial_replay_classifier_excludes_pickup_and_malformed_payloads", [] {
            Packet packet = {};
            const auto type = static_cast<uint16_t>(CoopProtocol::PacketType::SharedDrop);
            packet.command = static_cast<uint16_t>(Command::Spawn);
            require(IsSharedDropReplayPayload(type, &packet, sizeof(packet)), "initial spawn not recognized");
            packet.command = static_cast<uint16_t>(Command::SnapshotEnd);
            require(IsSharedDropReplayPayload(type, &packet, sizeof(packet)), "replay end not recognized");
            for (auto command : {Command::PickupRequest, Command::PickupGrant, Command::PickupReady,
                    Command::PickupCommit, Command::PickupResult, Command::PickupRelease, Command::RemoveRequest}) {
                packet.command = static_cast<uint16_t>(command);
                require(!IsSharedDropReplayPayload(type, &packet, sizeof(packet)), "transaction bypassed gameplay gate");
            }
            packet.command = static_cast<uint16_t>(Command::Spawn);
            require(!IsSharedDropReplayPayload(type, nullptr, sizeof(packet)), "null payload accepted");
            require(!IsSharedDropReplayPayload(type, &packet, sizeof(packet)-1), "short payload accepted");
            require(!IsSharedDropReplayPayload(0, &packet, sizeof(packet)), "wrong type accepted");
        }},
        {"spawn_old_session_rejected_on_host", [] { staleSessionSpawnRejected(true, false); }},
        {"spawn_old_session_rejected_on_client", [] { staleSessionSpawnRejected(false, false); }},
        {"spawn_missing_session_rejected_on_host", [] { staleSessionSpawnRejected(true, true); }},
        {"spawn_missing_session_rejected_on_client", [] { staleSessionSpawnRejected(false, true); }},
        {"current_session_spawn_dispatch_accepted", currentSessionSpawnAccepted},
        {"client_without_host_snapshot_rejects_spawn", clientWithoutHostSnapshotRejectsSpawn},
        {"reordered_lossy_duplicate_transport_ownership_invariants", reorderedLossyTransportNeverDuplicatesOwnership},
        {"competing_lossy_duplicate_transport_ownership_invariants", competingLossyTransportNeverDuplicatesOwnership},
    };
    unsigned failures = 0;
    std::cout << "SCOPE actual network transaction methods; native delivery and persistence are faked dependencies\n";
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << "RESULT " << (tests.size() - failures) << "/" << tests.size() << " transaction controls passed\n";
    return failures == 0 ? 0 : 1;
}
