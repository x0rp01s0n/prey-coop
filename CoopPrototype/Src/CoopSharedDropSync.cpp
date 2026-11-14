#include "ModMain.h"
#include "CoopItemClassification.h"
#include "CoopSerialSequence.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/arkitem.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
constexpr auto kSharedDropReservationTimeout = std::chrono::seconds(30);

bool IsFiniteTransform(const CoopProtocol::SharedDropPacket& packet)
{
    const float values[] = {
        packet.px, packet.py, packet.pz,
        packet.qw, packet.qx, packet.qy, packet.qz,
    };
    for (float value : values)
    {
        if (!std::isfinite(value))
            return false;
    }

    const float lengthSq =
        packet.qw * packet.qw + packet.qx * packet.qx +
        packet.qy * packet.qy + packet.qz * packet.qz;
    return lengthSq > 0.25f && lengthSq < 2.25f;
}

bool ParsePositiveInteger(const std::string& text, uint64_t& value)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    value = std::strtoull(text.c_str(), &end, 0);
    return end && end != text.c_str() && *end == '\0' && value != 0;
}

int ParseAttemptCount(const std::vector<std::string>& args, size_t index, int fallback)
{
    if (index >= args.size())
        return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(args[index].c_str(), &end, 10);
    return end && end != args[index].c_str() && *end == '\0' && parsed > 0
        ? static_cast<int>(std::min<long>(parsed, 5))
        : fallback;
}

bool SetSharedDropWorldPresentation(IEntity& entity, bool visible, std::string& reason)
{
    return CoopRuntimeGuards::TryGuardedVoidCall(
        visible ? "shared drop restore world entity" : "shared drop hide granted inventory entity",
        [&entity, visible]()
        {
            // PickUp can retain the same entity as the inventory item, while
            // Drop can clone that hidden entity into a new world lifetime.
            // Keep the inventory object but give exactly one side of that
            // lifecycle a physical world presentation.
            entity.Hide(!visible);
            entity.Invisible(!visible);
            entity.EnablePhysics(visible);
        },
        &reason);
}

}

uint64_t ModMain::EnsureSharedDropHostSessionId()
{
    if (m_networkMode == CoopNetworkMode::Host && m_sharedDropHostSessionId == 0)
    {
        const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        m_sharedDropHostSessionId = static_cast<uint64_t>(ticks) ^ GetLocalAccountToken();
        if (m_sharedDropHostSessionId == 0)
            m_sharedDropHostSessionId = 1;
    }
    return m_sharedDropHostSessionId;
}

bool ModMain::IsSharedDropWorldReady() const
{
    return !m_saveLoadGuardActive && !m_pendingPostLoadResync &&
        !m_arkLevelTransitionLoadActive && !m_runtimeTransitionCleanupPrepared &&
        gEnv && gEnv->pEntitySystem && ArkPlayer::GetInstancePtr() &&
        ArkPlayer::GetInstance().GetEntity();
}

bool ModMain::IsSharedDropServiceReady() const
{
    return (IsSharedDropWorldReady() ||
            (m_networkMode == CoopNetworkMode::Host && m_arkLevelTransitionLoadActive)) &&
        (m_networkMode == CoopNetworkMode::Host ||
            (m_networkMode == CoopNetworkMode::Client && m_hasRemoteEndpoint &&
                m_sharedDropHostSessionId != 0 && GetSessionHostAccountToken() != 0));
}

void ModMain::DetachSharedDropEntities()
{
    // Native travel saves/removes the area entities. Keep source lifetimes,
    // but never interpret that teardown (or a reused EntityId) as a pickup.
    for (auto& entry : m_sharedDrops)
    {
        SharedDropRecord& record = entry.second;
        IEntity* entity = gEnv && gEnv->pEntitySystem && record.areaId == m_localLevelId
            ? gEnv->pEntitySystem->GetEntity(record.localEntityId) : nullptr;
        if (entity)
        {
            record.position = entity->GetWorldPos();
            record.rotation = entity->GetWorldRotation();
        }
        record.localEntityId = INVALID_ENTITYID;
    }
    m_sharedDropByEntityId.clear();
    m_sharedDropRecoveryRequired = true;
}

uint64_t ModMain::BuildSharedDropStableId(uint64_t archetypeId, const Vec3& position, uint32_t sequence) const
{
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };

    mix(GetLocalAccountToken());
    mix(m_sharedDropHostSessionId);
    mix(CurrentHostSaveKeyHash());
    mix(m_localWorldEpoch);
    mix(m_localLevelId);
    mix(archetypeId);
    mix(sequence);
    mix(static_cast<uint32_t>(std::lround(position.x * 100.0f)));
    mix(static_cast<uint32_t>(std::lround(position.y * 100.0f)));
    mix(static_cast<uint32_t>(std::lround(position.z * 100.0f)));
    return hash == 0 ? 1 : hash;
}

bool ModMain::BuildSharedDropPacket(
    CoopProtocol::SharedDropPacket& packet,
    CoopProtocol::SharedDropCommand command,
    const SharedDropRecord& record,
    uint64_t targetPeerHash) const
{
    if (record.stableSpawnId == 0 || record.areaId == 0 || record.archetypeId == 0 || record.version == 0)
        return false;

    packet = {};
    packet.sequence = m_sharedDropSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.objectVersion = record.version;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.hostSessionId = m_sharedDropHostSessionId;
    packet.areaId = record.areaId;
    packet.stableSpawnId = record.stableSpawnId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = targetPeerHash;
    packet.archetypeId = record.archetypeId;
    packet.command = static_cast<uint16_t>(command);
    packet.flags = record.live ? CoopProtocol::kSharedDropFlagLive : 0u;
    packet.count = record.count;
    packet.px = record.position.x;
    packet.py = record.position.y;
    packet.pz = record.position.z;
    packet.qw = record.rotation.w;
    packet.qx = record.rotation.v.x;
    packet.qy = record.rotation.v.y;
    packet.qz = record.rotation.v.z;

    IEntity* entity = IsSharedDropWorldReady() && record.areaId == m_localLevelId && record.localEntityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(record.localEntityId)
        : nullptr;
    if (entity)
    {
        const Vec3 position = entity->GetWorldPos();
        const Quat rotation = entity->GetWorldRotation();
        packet.px = position.x;
        packet.py = position.y;
        packet.pz = position.z;
        packet.qw = rotation.w;
        packet.qx = rotation.v.x;
        packet.qy = rotation.v.y;
        packet.qz = rotation.v.z;
    }
    return true;
}

bool ModMain::SendSharedDropTo(
    const CoopProtocol::SharedDropPacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (m_networkMode == CoopNetworkMode::Host)
    {
        bool sent = true;
        for (const auto& entry : m_remotePeers)
        {
            const RemotePeerSession& peer = entry.second;
            if (peer.levelId == packet.areaId && peer.worldEpoch == packet.worldEpoch &&
                (peer.sessionFlags & CoopProtocol::kSessionFlagWorldReady) != 0)
                sent = SendSharedDropToPeer(packet, entry.first, failurePrefix) && sent;
        }
        return sent;
    }
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::SharedDrop),
            &packet,
            sizeof(packet),
            address,
            port,
            failurePrefix))
    {
        return false;
    }
    ++m_sharedDropSent;
    return true;
}

bool ModMain::SendSharedDropToPeer(
    const CoopProtocol::SharedDropPacket& packet,
    uint64_t peerHash,
    const char* failurePrefix)
{
    if (m_networkMode != CoopNetworkMode::Host || peerHash == 0)
        return false;

    const auto peerIt = m_remotePeers.find(peerHash);
    if (peerIt == m_remotePeers.end() || peerIt->second.levelId != packet.areaId ||
        peerIt->second.worldEpoch != packet.worldEpoch ||
        (peerIt->second.sessionFlags & CoopProtocol::kSessionFlagWorldReady) == 0 ||
        peerIt->second.address == 0 || peerIt->second.port == 0)
    {
        return false;
    }

    if (!QueueReliablePayloadToEndpoint(
            static_cast<uint16_t>(CoopProtocol::PacketType::SharedDrop),
            &packet,
            sizeof(packet),
            peerIt->second.address,
            peerIt->second.port,
            GetLocalAccountToken(),
            failurePrefix))
    {
        return false;
    }

    ++m_sharedDropSent;
    return true;
}

void ModMain::OnNativeSharedItemDropped(CArkItem* item, int droppedCount, const char* reason)
{
    if (!item || m_sharedDropApplyDepth != 0 || !IsSharedDropWorldReady() || !IsSharedDropServiceReady())
    {
        return;
    }

    EnsureSharedDropHostSessionId();

    IEntity* entity = nullptr;
    uint64_t archetypeId = 0;
    int count = droppedCount;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall("shared drop GetEntity", [item]() { return item->GetEntity(); }, entity, &guardReason) || !entity ||
        !CoopRuntimeGuards::TryGuardedCall("shared drop GetArchetype", [item]() { return item->GetArchetype(); }, archetypeId, &guardReason) || archetypeId == 0)
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "drop_capture_failed_" + guardReason;
        return;
    }
    if (count <= 0)
        CoopRuntimeGuards::TryGuardedCall("shared drop GetCount", [item]() { return item->GetCount(); }, count, nullptr);
    count = std::max(1, count);

    if (m_networkMode == CoopNetworkMode::Client)
    {
        // Publish only after the durable local pair no longer owns the drop.
        // A crash must not restore the pre-drop inventory beside a world copy.
        MarkLocalInventoryDirty("shared drop publication");
        if (!SaveLocalPlayerSidecar("shared drop publication"))
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "drop_publication_deferred_local_capture_failed";
            return;
        }
        WriteLocalPlayerRecoveryJournal("shared drop publication");
        QueueClientPlayerStateUpload("inventory mutation", m_currentHostSaveStateKey);
    }

    // A SharedDrop picked up on this peer may keep its inventory entity hidden.
    // Vanilla Drop can reuse or clone it without clearing all presentation
    // flags, which leaves the old visible ghost beside the real new drop.
    SetSharedDropWorldPresentation(*entity, true, guardReason);

    SharedDropRecord record;
    record.areaId = m_localLevelId;
    record.archetypeId = archetypeId;
    record.ownerPeerHash = GetLocalAccountToken();
    record.localEntityId = entity->GetId();
    record.version = 1;
    record.count = count;
    record.live = true;
    record.position = entity->GetWorldPos();
    record.rotation = entity->GetWorldRotation();
    const uint32_t sequence = CoopSerialSequence::Advance(m_sharedDropSequence);
    record.stableSpawnId = BuildSharedDropStableId(archetypeId, entity->GetWorldPos(), sequence);
    entity->SetName(("CoopSharedDrop_" + std::to_string(record.stableSpawnId)).c_str());

    // A successfully acquired shared item may retain its native entity while it
    // lerps into (or lives in) the inventory. Dropping that entity starts a new
    // globally unique lifetime, so retire the old tombstone before rebinding it.
    const auto previousBinding = m_sharedDropByEntityId.find(record.localEntityId);
    if (previousBinding != m_sharedDropByEntityId.end())
    {
        const auto previousRecord = m_sharedDrops.find(previousBinding->second);
        if (previousRecord != m_sharedDrops.end() &&
            previousRecord->second.localEntityId == record.localEntityId)
        {
            previousRecord->second.localEntityId = INVALID_ENTITYID;
        }
        m_sharedDropByEntityId.erase(previousBinding);
    }
    m_sharedDrops[record.stableSpawnId] = record;
    m_sharedDropByEntityId[record.localEntityId] = record.stableSpawnId;
    RetireLivePropTrackingForSharedDrop(record.localEntityId);

    CoopProtocol::SharedDropPacket packet;
    if (!BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::Spawn, record) ||
        !SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop spawn failed"))
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "drop_spawn_send_failed";
        return;
    }
    m_lastSharedDropEvent =
        "sent_spawn_id_" + std::to_string(record.stableSpawnId) +
        "_entity_" + std::to_string(record.localEntityId) +
        "_arch_" + std::to_string(archetypeId) +
        "_count_" + std::to_string(count) +
        "_reason_" + (reason ? reason : "-");
}

void ModMain::OnNativeRecyclerIngredientSpawned(EntityId itemEntityId, const char* reason)
{
    if (m_networkMode == CoopNetworkMode::Off || itemEntityId == INVALID_ENTITYID ||
        m_sharedDropApplyDepth != 0 || !IsSharedDropServiceReady())
    {
        return;
    }

    // SpawnNextIngredient can be called more than once while the recycler is
    // finishing its animation. Do not publish the same native entity twice.
    if (m_sharedDropByEntityId.find(itemEntityId) != m_sharedDropByEntityId.end())
        return;

    CArkItem* item = nullptr;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "recycler ingredient item lookup",
            [itemEntityId]() { return CArkItem::GetItemFromEntityId(itemEntityId); },
            item,
            &guardReason) ||
        !item)
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "recycler_output_lookup_failed_" + guardReason;
        return;
    }

    int count = 1;
    CoopRuntimeGuards::TryGuardedCall(
        "recycler ingredient count",
        [item]() { return item->GetCount(); },
        count,
        nullptr);
    OnNativeSharedItemDropped(item, count, reason);
}

bool ModMain::ShouldDeferNativeSharedItemPickup(CArkItem* item, EntityId pickerId, const char* reason)
{
    if (!item || m_sharedDropApplyDepth != 0 ||
        (m_networkMode != CoopNetworkMode::Host && m_networkMode != CoopNetworkMode::Client) ||
        !ArkPlayer::GetInstancePtr() || pickerId != ArkPlayer::GetInstance().GetEntityId())
    {
        return false;
    }

    RecoverSavedSharedDrops();
    const auto found = m_sharedDropByEntityId.find(item->GetEntityId());
    if (found == m_sharedDropByEntityId.end())
    {
        IEntity* entity = gEnv->pEntitySystem->GetEntity(item->GetEntityId());
        const char* name = entity ? entity->GetName() : nullptr;
        // A loaded public source must not fall through to per-player loot
        // while the authoritative replay is still in flight.
        return name && std::strncmp(name, "CoopSharedDrop_", 15) == 0;
    }
    auto recordIt = m_sharedDrops.find(found->second);
    if (recordIt == m_sharedDrops.end())
        return false;

    SharedDropRecord& record = recordIt->second;
    if (!IsSharedDropWorldReady())
        return true;
    if (record.pickupCommitted && record.pickupWinnerPeerHash == GetLocalAccountToken())
    {
        TryDeliverCommittedSharedDrop(record);
        return true;
    }
    if (!IsSharedDropServiceReady())
        return true;
    if (!record.live)
    {
        // Native PickUp can leave its entity interactive while the lerp/removal
        // finishes. Keep the dead binding as a tombstone and never grant it twice.
        ++m_sharedDropPickupSuppressions;
        m_lastSharedDropEvent =
            "suppressed_dead_pickup_id_" + std::to_string(record.stableSpawnId) +
            "_reason_" + (reason ? reason : "-");
        return true;
    }

    if (m_networkMode == CoopNetworkMode::Host)
    {
        if (record.pickupPending)
        {
            ++m_sharedDropPickupSuppressions;
            m_lastSharedDropEvent =
                "suppressed_host_pickup_reserved_id_" + std::to_string(record.stableSpawnId);
            return true;
        }
        // The Host is implicitly ready, but ownership must still be committed
        // before native PickUp can partially transfer a stack or open overflow.
        CoopSerialSequence::Advance(m_sharedDropSequence);
        record.pickupPending = true;
        record.pickupWinnerPeerHash = GetLocalAccountToken();
        record.pickupTransactionSequence = m_sharedDropSequence;
        CommitSharedDropPickup(record, record.pickupWinnerPeerHash, record.pickupTransactionSequence);
        return true;
    }

    if (record.pickupPending)
    {
        ++m_sharedDropPickupSuppressions;
        m_lastSharedDropEvent =
            "suppressed_pending_pickup_id_" + std::to_string(record.stableSpawnId) +
            "_reason_" + (reason ? reason : "-");
        return true;
    }

    CoopSerialSequence::Advance(m_sharedDropSequence);
    CoopProtocol::SharedDropPacket packet;
    if (!BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::PickupRequest, record))
    {
        ++m_sharedDropDropped;
        return true;
    }
    record.pickupPending = true;
    record.pickupGrantReceived = false;
    record.pickupResultSent = false;
    record.pickupResultSucceeded = false;
    record.pickupRetrySecondsRemaining = 0.0f;
    record.pickupCommitted = false;
    record.pickupDeliveryStored = false;
    record.localPickupGranted = false;
    record.pickupWinnerPeerHash = 0;
    record.pickupTransactionSequence = packet.sequence;
    if (!SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop pickup request failed"))
    {
        record.pickupPending = false;
        record.pickupTransactionSequence = 0;
        ++m_sharedDropDropped;
        return true;
    }
    record.pickupRetrySecondsRemaining = 0.25f;
    ++m_sharedDropPickupRequests;
    m_lastSharedDropEvent =
        "sent_pickup_request_id_" + std::to_string(record.stableSpawnId) +
        "_reason_" + (reason ? reason : "-");
    return true;
}

bool ModMain::CommitSharedDropPickup(
    SharedDropRecord& record,
    uint64_t winnerPeerHash,
    uint32_t transactionSequence)
{
    if (m_networkMode != CoopNetworkMode::Host || !record.live || record.pickupCommitted ||
        !record.pickupPending || winnerPeerHash == 0 || transactionSequence == 0 ||
        record.pickupWinnerPeerHash != winnerPeerHash || record.pickupTransactionSequence != transactionSequence)
        return false;

    CoopSharedDropDelivery::Entry delivery;
    delivery.hostSessionId = EnsureSharedDropHostSessionId();
    delivery.stableSpawnId = record.stableSpawnId;
    delivery.archetypeId = record.archetypeId;
    delivery.count = static_cast<uint32_t>(record.count);
    delivery.transactionSequence = transactionSequence;
    delivery.objectVersion = record.version + 1;
    if (!StoreCommittedSharedDropDelivery(winnerPeerHash, delivery))
    {
        ReleaseSharedDropReservation(record, winnerPeerHash, transactionSequence, "ownership_store_failed");
        ++m_sharedDropDropped;
        return false;
    }

    record.version = delivery.objectVersion;
    record.pickupCommitted = true;
    record.live = false;
    record.pickupPending = false;
    record.pickupGrantReceived = false;
    record.pickupResultSent = false;
    record.pickupResultSucceeded = false;
    record.pickupRetrySecondsRemaining = 0.0f;
    record.pickupReservationDeadline = {};
    record.localPickupGranted = false;
    record.pickupWinnerPeerHash = winnerPeerHash;
    record.pickupTransactionSequence = transactionSequence;
    const bool localWinner = winnerPeerHash == GetLocalAccountToken();
    std::string detail;
    if (!localWinner)
        RemoveSharedDropLocal(record, detail);

    if (m_hasRemoteEndpoint)
    {
        CoopProtocol::SharedDropPacket commit;
        if (BuildSharedDropPacket(
                commit,
                CoopProtocol::SharedDropCommand::PickupCommit,
                record,
                winnerPeerHash))
        {
            commit.sequence = transactionSequence;
            if (SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop pickup commit failed"))
            {
                ++m_sharedDropPickupCommits;
                ++m_sharedDropApplied;
            }
            else
                ++m_sharedDropDropped;
        }
    }

    // Publish the terminal world decision before any local native transfer.
    // Failed delivery stays assigned and can be retried without restoring loot.
    if (localWinner)
        TryDeliverCommittedSharedDrop(record);
    m_lastSharedDropEvent =
        "committed_pickup_id_" + std::to_string(record.stableSpawnId) +
        "_transaction_" + std::to_string(transactionSequence);
    return true;
}

bool ModMain::MaterializeSharedDrop(
    const CoopProtocol::SharedDropPacket& packet,
    SharedDropRecord& record,
    std::string& detail)
{
    if (!gEnv || !gEnv->pEntitySystem || !IsFiniteTransform(packet))
    {
        detail = "missing_entity_system_or_bad_transform";
        return false;
    }

    IEntityArchetype* archetype = nullptr;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "shared drop GetEntityArchetype",
            [&packet]() { return gEnv->pEntitySystem->GetEntityArchetype(packet.archetypeId); },
            archetype,
            &reason) || !archetype)
    {
        detail = "missing_archetype_" + std::to_string(packet.archetypeId) + "_" + reason;
        return false;
    }

    SEntitySpawnParams params;
    const std::string name = "CoopSharedDrop_" + std::to_string(packet.stableSpawnId);
    params.sName = name.c_str();
    params.pArchetype = archetype;
    params.pClass = archetype->GetClass();
    params.vPosition = Vec3(packet.px, packet.py, packet.pz);
    params.qRotation = Quat(packet.qw, packet.qx, packet.qy, packet.qz).GetNormalized();
    params.vScale = Vec3(1.0f);

    IEntity* entity = gEnv->pEntitySystem->FindEntityByName(name.c_str());
    if (entity)
    {
        CArkItem* existing = CArkItem::GetItemFromEntityId(entity->GetId());
        ArkInventory* inventory = ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().m_pInventory : nullptr;
        if (!existing || entity->IsHidden() || (inventory && inventory->Contains(entity->GetId())) ||
            existing->GetArchetype() != packet.archetypeId || existing->GetCount() != packet.count)
        {
            detail = "saved_source_conflict";
            return false;
        }
    }
    ++m_sharedDropApplyDepth;
    const bool spawned = entity || CoopRuntimeGuards::TryGuardedCall(
        "shared drop SpawnEntityFromArchetype",
        [&params, archetype]() { return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true); },
        entity,
        &reason);
    --m_sharedDropApplyDepth;
    if (!spawned || !entity)
    {
        detail = "spawn_failed_" + reason;
        return false;
    }

    CArkItem* item = CArkItem::GetItemFromEntityId(entity->GetId());
    if (!item)
    {
        gEnv->pEntitySystem->RemoveEntity(entity->GetId(), true);
        detail = "spawned_entity_not_item";
        return false;
    }

    ++m_sharedDropApplyDepth;
    CoopRuntimeGuards::TryGuardedVoidCall(
        "shared drop ResetCount",
        [item, &packet]() { item->ResetCount(std::max(1, packet.count)); },
        &reason);
    --m_sharedDropApplyDepth;

    record.stableSpawnId = packet.stableSpawnId;
    record.areaId = packet.areaId;
    record.archetypeId = packet.archetypeId;
    record.ownerPeerHash = packet.sourcePeerHash;
    record.localEntityId = entity->GetId();
    record.version = packet.objectVersion;
    record.count = std::max(1, packet.count);
    record.position = params.vPosition;
    record.rotation = params.qRotation;
    record.live = true;
    record.pickupPending = false;
    record.localPickupGranted = false;
    record.nativePickupInProgress = false;
    record.pickupWinnerPeerHash = 0;
    m_sharedDropByEntityId[record.localEntityId] = record.stableSpawnId;
    RetireLivePropTrackingForSharedDrop(record.localEntityId);
    detail = "spawned_entity_" + std::to_string(record.localEntityId);
    return true;
}

bool ModMain::RemoveSharedDropLocal(SharedDropRecord& record, std::string& detail)
{
    IEntity* entity = IsSharedDropWorldReady() && record.areaId == m_localLevelId && record.localEntityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(record.localEntityId)
        : nullptr;
    if (!entity)
    {
        record.live = false;
        record.pickupPending = false;
        record.localEntityId = INVALID_ENTITYID;
        detail = "already_missing";
        return true;
    }

    const EntityId entityId = entity->GetId();
    CArkItem* item = CArkItem::GetItemFromEntityId(entityId);
    bool ok = false;
    std::string reason;
    ++m_sharedDropApplyDepth;
    if (item)
    {
        ok = CoopRuntimeGuards::TryGuardedVoidCall("shared drop RemoveEntity", [item]() { item->RemoveEntity(); }, &reason);
    }
    if (!ok && gEnv && gEnv->pEntitySystem)
    {
        ok = CoopRuntimeGuards::TryGuardedVoidCall(
            "shared drop entity remove fallback",
            [entityId]() { gEnv->pEntitySystem->RemoveEntity(entityId, true); },
            &reason);
    }
    --m_sharedDropApplyDepth;

    record.live = false;
    record.pickupPending = false;
    record.nativePickupInProgress = false;
    m_sharedDropByEntityId.erase(entityId);
    record.localEntityId = INVALID_ENTITYID;
    detail = ok
        ? "removed_entity_" + std::to_string(entityId)
        : "remove_failed_" + reason;
    return ok;
}

bool ModMain::SendSharedDropPickupResult(SharedDropRecord& record, bool success)
{
    if (record.pickupTransactionSequence == 0 || m_networkMode != CoopNetworkMode::Client ||
        !IsSharedDropServiceReady())
    {
        return false;
    }

    CoopProtocol::SharedDropPacket packet;
    if (!BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::PickupResult, record, GetSessionHostAccountToken()))
        return false;
    packet.sequence = record.pickupTransactionSequence;
    packet.flags = success ? CoopProtocol::kSharedDropFlagPickupSucceeded : 0u;
    const bool sent = SendSharedDropTo(
        packet,
        m_remoteAddress,
        m_remotePort,
        "shared drop pickup result failed");
    if (sent)
    {
        record.pickupResultSent = true;
        record.pickupResultSucceeded = success;
        record.pickupRetrySecondsRemaining = 0.0f;
        m_lastSharedDropEvent =
            std::string(success ? "sent_pickup_success_id_" : "sent_pickup_failure_id_") +
            std::to_string(record.stableSpawnId) +
            "_transaction_" + std::to_string(record.pickupTransactionSequence);
    }
    return sent;
}

bool ModMain::SendSharedDropPickupReady(SharedDropRecord& record, bool ready)
{
    CoopProtocol::SharedDropPacket packet;
    if (!BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::PickupReady, record, GetSessionHostAccountToken()))
        return false;
    packet.sequence = record.pickupTransactionSequence;
    packet.flags = ready ? CoopProtocol::kSharedDropFlagPickupSucceeded : 0u;
    return SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop pickup ready failed");
}

void ModMain::RestoreCommittedSharedDropDeliveries()
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !player->GetEntity() || !gEnv || !gEnv->pEntitySystem ||
        m_sharedDropDeliveryRecoveryRequired || m_sharedDropHostSessionId == 0 ||
        m_pendingPlayerSidecarInventoryRestore || m_pendingPlayerSidecarChipsetRestore ||
        m_pendingReceivedPlayerStateApply || m_clientAwaitingHostPlayerState ||
        m_playerSidecarInventoryPending != 0 || m_playerSidecarChipsetsPending != 0)
        return;

    for (const CoopSharedDropDelivery::Entry& delivery : m_localSharedDropDeliveries)
    {
        if (delivery.hostSessionId != m_sharedDropHostSessionId || delivery.deliveredCount == delivery.count)
            continue;
        auto found = m_sharedDrops.find(delivery.stableSpawnId);
        if (found != m_sharedDrops.end() &&
            (!found->second.pickupCommitted || found->second.pickupWinnerPeerHash != GetLocalAccountToken() ||
                found->second.archetypeId != delivery.archetypeId ||
                found->second.pickupTransactionSequence != delivery.transactionSequence ||
                found->second.version != delivery.objectVersion))
            continue;
        if (found == m_sharedDrops.end() ||
            !gEnv->pEntitySystem->GetEntity(found->second.localEntityId))
        {
            // This is the player's assigned remainder, not a shared world drop.
            // Rebuild it only after the snapshot containing its receipt applied.
            CoopProtocol::SharedDropPacket packet;
            packet.stableSpawnId = delivery.stableSpawnId;
            packet.areaId = m_localLevelId;
            packet.archetypeId = delivery.archetypeId;
            packet.objectVersion = delivery.objectVersion;
            packet.count = static_cast<int>(delivery.count - delivery.deliveredCount);
            packet.sourcePeerHash = GetLocalAccountToken();
            const Vec3 position = player->GetEntity()->GetWorldPos();
            packet.px = position.x;
            packet.py = position.y;
            packet.pz = position.z;
            SharedDropRecord record;
            std::string detail;
            if (!MaterializeSharedDrop(packet, record, detail))
                continue;
            record.count = static_cast<int>(delivery.count);
            record.pickupCommitted = true;
            record.pickupPending = true;
            record.pickupGrantReceived = true;
            record.pickupTransactionSequence = delivery.transactionSequence;
            record.pickupWinnerPeerHash = GetLocalAccountToken();
            record.live = false;
            if (found != m_sharedDrops.end() && found->second.localEntityId != record.localEntityId)
                m_sharedDropByEntityId.erase(found->second.localEntityId);
            found = m_sharedDrops.insert_or_assign(delivery.stableSpawnId, record).first;
        }
        if (found->second.pickupCommitted && !found->second.pickupDeliveryAttempted)
            TryDeliverCommittedSharedDrop(found->second, false);
    }
}

void ModMain::TickSharedDropPickupRetries(float frameTime)
{
    RecoverSavedSharedDrops();
    if (!IsSharedDropServiceReady())
        return;

    const float delta = std::isfinite(frameTime) && frameTime > 0.0f
        ? std::min(frameTime, 0.25f)
        : 0.0f;
    if (IsSharedDropWorldReady())
        RestoreCommittedSharedDropDeliveries();
    if (m_networkMode == CoopNetworkMode::Host)
    {
        for (auto& entry : m_sharedDrops)
        {
            SharedDropRecord& record = entry.second;
            if (!record.pickupPending || record.pickupCommitted)
                continue;
            if (std::chrono::steady_clock::now() >= record.pickupReservationDeadline)
                ReleaseSharedDropReservation(record, 0, record.pickupTransactionSequence, "reservation_timeout");
        }
        return;
    }
    if (m_networkMode != CoopNetworkMode::Client)
        return;

    for (auto& entry : m_sharedDrops)
    {
        SharedDropRecord& record = entry.second;
        if (!record.pickupPending || record.pickupTransactionSequence == 0 ||
            record.pickupResultSent)
            continue;

        record.pickupRetrySecondsRemaining = std::max(
            0.0f,
            record.pickupRetrySecondsRemaining - delta);
        if (record.pickupRetrySecondsRemaining > 0.0f)
            continue;

        if (!record.pickupGrantReceived)
        {
            CoopProtocol::SharedDropPacket request;
            if (BuildSharedDropPacket(request, CoopProtocol::SharedDropCommand::PickupRequest, record))
            {
                request.sequence = record.pickupTransactionSequence;
                SendSharedDropTo(request, m_remoteAddress, m_remotePort, "shared drop pickup request retry failed");
            }
            record.pickupRetrySecondsRemaining = 0.25f;
        }
        else
        {
            if (record.pickupCommitted)
            {
                if (record.localPickupGranted && !record.pickupDeliveryStored)
                    PersistLocalSharedDropDelivery(record);
                if (record.localPickupGranted && record.pickupDeliveryStored)
                    SendSharedDropPickupResult(record, true);
            }
            else
                SendSharedDropPickupReady(record, record.pickupResultSucceeded);
            record.pickupRetrySecondsRemaining = 0.25f;
        }
    }
}

void ModMain::ReleaseSharedDropReservation(
    SharedDropRecord& record,
    uint64_t peerHash,
    uint32_t transactionSequence,
    const char* reason)
{
    // A committed entitlement is owned even while its recipient is offline.
    // Only an uncommitted reservation may return to the shared world.
    if (record.pickupCommitted || !record.live)
        return;
    ++record.version;
    record.live = true;
    record.pickupPending = false;
    record.pickupGrantReceived = false;
    record.pickupResultSent = false;
    record.pickupResultSucceeded = false;
    record.pickupRetrySecondsRemaining = 0.0f;
    record.pickupReservationDeadline = {};
    record.localPickupGranted = false;
    record.pickupWinnerPeerHash = 0;
    record.pickupTransactionSequence = 0;

    if (IEntity* entity = IsSharedDropWorldReady() && record.areaId == m_localLevelId
            ? gEnv->pEntitySystem->GetEntity(record.localEntityId) : nullptr)
    {
        std::string detail;
        SetSharedDropWorldPresentation(*entity, true, detail);
    }
    if (m_networkMode != CoopNetworkMode::Host || !m_hasRemoteEndpoint)
        return;

    CoopProtocol::SharedDropPacket release;
    if (BuildSharedDropPacket(release, CoopProtocol::SharedDropCommand::PickupRelease, record, 0))
    {
        release.sequence = transactionSequence;
        const bool sent = SendSharedDropTo(release, m_remoteAddress, m_remotePort, "shared drop pickup release failed");
        if (sent)
            ++m_sharedDropPickupSuppressions;
    }
    (void)peerHash;
    m_lastSharedDropEvent =
        "released_pickup_reservation_id_" + std::to_string(record.stableSpawnId) +
        "_reason_" + (reason ? reason : "-");
}

void ModMain::ReleaseSharedDropReservationsForPeer(uint64_t peerHash)
{
    if (m_networkMode != CoopNetworkMode::Host || peerHash == 0)
        return;

    for (auto& entry : m_sharedDrops)
    {
        SharedDropRecord& record = entry.second;
        if (!record.pickupPending || record.pickupCommitted || record.pickupWinnerPeerHash != peerHash)
            continue;

        const uint32_t transactionSequence = record.pickupTransactionSequence;
        ReleaseSharedDropReservation(record, 0, transactionSequence, "peer_removed");
    }
}

void ModMain::HandleSharedDrop(const CoopProtocol::SharedDropPacket& packet)
{
    ++m_sharedDropReceived;
    const uint64_t remotePeerHash = GetRemoteAccountToken();
    const auto command = static_cast<CoopProtocol::SharedDropCommand>(packet.command);
    const bool roleAllowsCommand =
        (m_networkMode == CoopNetworkMode::Host &&
            (command == CoopProtocol::SharedDropCommand::Spawn ||
                command == CoopProtocol::SharedDropCommand::PickupRequest ||
                command == CoopProtocol::SharedDropCommand::PickupReady ||
                command == CoopProtocol::SharedDropCommand::PickupResult ||
                command == CoopProtocol::SharedDropCommand::RemoveRequest)) ||
        (m_networkMode == CoopNetworkMode::Client &&
            (command == CoopProtocol::SharedDropCommand::Spawn ||
                command == CoopProtocol::SharedDropCommand::PickupCommit ||
                command == CoopProtocol::SharedDropCommand::Remove ||
                command == CoopProtocol::SharedDropCommand::PickupGrant ||
                command == CoopProtocol::SharedDropCommand::PickupRelease ||
                command == CoopProtocol::SharedDropCommand::SnapshotEnd));
    const bool targetAllowsCommand =
        (m_networkMode == CoopNetworkMode::Host &&
            (command == CoopProtocol::SharedDropCommand::PickupResult ||
                command == CoopProtocol::SharedDropCommand::PickupReady) &&
            packet.targetPeerHash == GetLocalAccountToken()) ||
        (m_networkMode == CoopNetworkMode::Client &&
            (command == CoopProtocol::SharedDropCommand::PickupGrant ||
                command == CoopProtocol::SharedDropCommand::PickupRelease) &&
            (packet.targetPeerHash == GetLocalAccountToken() ||
                (command == CoopProtocol::SharedDropCommand::PickupRelease && packet.targetPeerHash == 0))) ||
        (command != CoopProtocol::SharedDropCommand::PickupResult &&
            command != CoopProtocol::SharedDropCommand::PickupReady &&
            command != CoopProtocol::SharedDropCommand::PickupGrant &&
            command != CoopProtocol::SharedDropCommand::PickupRelease);
    const auto sourcePeer = m_remotePeers.find(packet.sourcePeerHash);
    const bool sourceAreaAllowed = m_networkMode == CoopNetworkMode::Host
        ? sourcePeer != m_remotePeers.end() && sourcePeer->second.levelId == packet.areaId &&
            sourcePeer->second.worldEpoch == m_localWorldEpoch &&
            (sourcePeer->second.sessionFlags & CoopProtocol::kSessionFlagWorldReady) != 0
        : packet.sourcePeerHash == GetSessionHostAccountToken() && packet.areaId == m_localLevelId;
    if (packet.worldEpoch != m_localWorldEpoch ||
        packet.hostSaveKeyHash == 0 ||
        packet.hostSaveKeyHash != CurrentHostSaveKeyHash() ||
        packet.areaId == 0 || !sourceAreaAllowed ||
        (command != CoopProtocol::SharedDropCommand::SnapshotEnd && (packet.stableSpawnId == 0 || packet.archetypeId == 0)) ||
        packet.sourcePeerHash == 0 || packet.sourcePeerHash != remotePeerHash || !roleAllowsCommand || !targetAllowsCommand ||
        (command == CoopProtocol::SharedDropCommand::Spawn && !IsFiniteTransform(packet)) ||
        (command == CoopProtocol::SharedDropCommand::Spawn && packet.count <= 0) ||
        packet.hostSessionId == 0 || packet.hostSessionId != EnsureSharedDropHostSessionId())
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "rejected_context_or_source";
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::SnapshotEnd)
    {
        PruneUnboundSharedDrops();
        return;
    }
    auto found = m_sharedDrops.find(packet.stableSpawnId);
    if (command == CoopProtocol::SharedDropCommand::Spawn)
    {
        if (found != m_sharedDrops.end() &&
            (found->second.areaId != packet.areaId || found->second.archetypeId != packet.archetypeId))
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "spawn_identity_conflict";
            return;
        }
        if (found != m_sharedDrops.end() && (m_networkMode == CoopNetworkMode::Host || !found->second.live ||
            (found->second.version >= packet.objectVersion &&
                (found->second.localEntityId != INVALID_ENTITYID || packet.areaId != m_localLevelId))))
        {
            m_lastSharedDropEvent = "duplicate_spawn_id_" + std::to_string(packet.stableSpawnId);
            return;
        }

        SharedDropRecord record;
        std::string detail;
        if (packet.areaId == m_localLevelId && IsSharedDropWorldReady())
        {
            if (!MaterializeSharedDrop(packet, record, detail))
            {
                ++m_sharedDropDropped;
                m_lastSharedDropEvent = "spawn_apply_failed_" + detail;
                return;
            }
        }
        else
        {
            // The coordinator owns this decision, not an entity in its loaded area.
            record.stableSpawnId = packet.stableSpawnId;
            record.areaId = packet.areaId;
            record.archetypeId = packet.archetypeId;
            record.ownerPeerHash = packet.sourcePeerHash;
            record.version = packet.objectVersion;
            record.count = std::max(1, packet.count);
            record.live = true;
            record.position = Vec3(packet.px, packet.py, packet.pz);
            record.rotation = Quat(packet.qw, packet.qx, packet.qy, packet.qz);
        }
        m_sharedDrops[record.stableSpawnId] = record;
        ++m_sharedDropApplied;
        m_lastSharedDropEvent = "applied_spawn_id_" + std::to_string(record.stableSpawnId) + "_" + detail;

        if (m_networkMode == CoopNetworkMode::Host)
        {
            CoopSerialSequence::Advance(m_sharedDropSequence);
            SharedDropRecord& stored = m_sharedDrops[record.stableSpawnId];
            CoopProtocol::SharedDropPacket commit;
            if (BuildSharedDropPacket(commit, CoopProtocol::SharedDropCommand::Spawn, stored))
                SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop spawn echo commit failed");
        }
        return;
    }

    if (found == m_sharedDrops.end())
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "missing_record_id_" + std::to_string(packet.stableSpawnId);
        return;
    }
    SharedDropRecord& record = found->second;
    if (record.areaId != packet.areaId || record.archetypeId != packet.archetypeId)
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "record_identity_conflict";
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupRequest)
    {
        if (m_networkMode != CoopNetworkMode::Host || packet.sequence == 0)
        {
            ++m_sharedDropDropped;
            return;
        }

        if (!record.live && record.pickupWinnerPeerHash != 0)
        {
            // Requests after a terminal decision receive the existing result;
            // never answer a terminal record with a live reservation release.
            CoopProtocol::SharedDropPacket commit;
            if (BuildSharedDropPacket(
                    commit,
                    CoopProtocol::SharedDropCommand::PickupCommit,
                    record,
                    record.pickupWinnerPeerHash))
            {
                commit.sequence = record.pickupTransactionSequence;
                SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop duplicate request commit failed");
            }
            ++m_sharedDropPickupSuppressions;
            m_lastSharedDropEvent = "replayed_pickup_commit_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        if (record.pickupPending)
        {
            if (record.pickupWinnerPeerHash == packet.sourcePeerHash &&
                record.pickupTransactionSequence == packet.sequence)
            {
                CoopProtocol::SharedDropPacket grant;
                if (BuildSharedDropPacket(grant, CoopProtocol::SharedDropCommand::PickupGrant, record, packet.sourcePeerHash))
                {
                    grant.sequence = packet.sequence;
                    SendSharedDropToPeer(grant, packet.sourcePeerHash, "shared drop duplicate grant failed");
                }
                m_lastSharedDropEvent = "replayed_pickup_grant_id_" + std::to_string(record.stableSpawnId);
            }
            else
            {
                CoopProtocol::SharedDropPacket release;
                if (BuildSharedDropPacket(release, CoopProtocol::SharedDropCommand::PickupRelease, record, packet.sourcePeerHash))
                {
                    release.sequence = packet.sequence;
                    SendSharedDropToPeer(release, packet.sourcePeerHash, "shared drop competing request release failed");
                }
                ++m_sharedDropPickupSuppressions;
            }
            return;
        }

        if (!record.live || packet.objectVersion != record.version)
        {
            if (!record.live)
            {
                CoopProtocol::SharedDropPacket removal;
                if (BuildSharedDropPacket(removal, CoopProtocol::SharedDropCommand::Remove, record))
                {
                    removal.sequence = packet.sequence;
                    SendSharedDropTo(removal, m_remoteAddress, m_remotePort, "shared drop terminal request removal failed");
                }
            }
            else
            {
                CoopProtocol::SharedDropPacket release;
                if (BuildSharedDropPacket(release, CoopProtocol::SharedDropCommand::PickupRelease, record, packet.sourcePeerHash))
                {
                    release.sequence = packet.sequence;
                    SendSharedDropToPeer(release, packet.sourcePeerHash, "shared drop rejected pickup release failed");
                }
            }
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "pickup_request_rejected_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        record.version += 1;
        record.pickupPending = true;
        record.pickupGrantReceived = false;
        record.pickupResultSent = false;
        record.pickupResultSucceeded = false;
        record.pickupRetrySecondsRemaining = 0.0f;
        record.pickupReservationDeadline = std::chrono::steady_clock::now() + kSharedDropReservationTimeout;
        record.pickupCommitted = false;
        record.pickupWinnerPeerHash = packet.sourcePeerHash;
        record.pickupTransactionSequence = packet.sequence;

        CoopProtocol::SharedDropPacket grant;
        if (!BuildSharedDropPacket(grant, CoopProtocol::SharedDropCommand::PickupGrant, record, packet.sourcePeerHash))
        {
            ReleaseSharedDropReservation(record, packet.sourcePeerHash, packet.sequence, "grant_build_failed");
            ++m_sharedDropDropped;
            return;
        }
        grant.sequence = packet.sequence;
        if (!SendSharedDropToPeer(grant, packet.sourcePeerHash, "shared drop pickup grant failed"))
        {
            ReleaseSharedDropReservation(record, packet.sourcePeerHash, packet.sequence, "grant_send_failed");
            ++m_sharedDropDropped;
            return;
        }
        if (IEntity* entity = IsSharedDropWorldReady() && record.areaId == m_localLevelId
                ? gEnv->pEntitySystem->GetEntity(record.localEntityId) : nullptr)
        {
            std::string detail;
            SetSharedDropWorldPresentation(*entity, false, detail);
        }
        ++m_sharedDropPickupRequests;
        m_lastSharedDropEvent =
            "reserved_pickup_id_" + std::to_string(record.stableSpawnId) +
            "_transaction_" + std::to_string(packet.sequence);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupReady)
    {
        const bool success = (packet.flags & CoopProtocol::kSharedDropFlagPickupSucceeded) != 0;
        const bool activeReservation =
            record.pickupPending &&
            record.pickupWinnerPeerHash == packet.sourcePeerHash &&
            record.pickupTransactionSequence == packet.sequence &&
            record.version == packet.objectVersion;
        if (!activeReservation)
        {
            // Preparation is side-effect free. A delayed Ready can only replay
            // its already committed decision, never create a new reservation.
            if (record.pickupCommitted &&
                record.pickupWinnerPeerHash == packet.sourcePeerHash &&
                record.pickupTransactionSequence == packet.sequence)
            {
                CoopProtocol::SharedDropPacket commit;
                if (BuildSharedDropPacket(commit, CoopProtocol::SharedDropCommand::PickupCommit, record, packet.sourcePeerHash))
                {
                    commit.sequence = packet.sequence;
                    SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop replayed result commit failed");
                }
            }
            else
            {
                ++m_sharedDropDropped;
                m_lastSharedDropEvent = "pickup_ready_rejected_id_" + std::to_string(record.stableSpawnId);
            }
            return;
        }

        if (!success)
        {
            ReleaseSharedDropReservation(record, packet.sourcePeerHash, packet.sequence, "client_not_ready");
            ++m_sharedDropPickupSuppressions;
            return;
        }

        CommitSharedDropPickup(record, packet.sourcePeerHash, packet.sequence);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupResult)
    {
        // This acknowledges delivery only. Ownership was persisted before the
        // commit, and even a failed/absent acknowledgement cannot undo it.
        if (!record.pickupCommitted || record.pickupWinnerPeerHash != packet.sourcePeerHash ||
            record.pickupTransactionSequence != packet.sequence || record.version != packet.objectVersion)
        {
            ++m_sharedDropDropped;
            return;
        }
        record.pickupResultSucceeded = (packet.flags & CoopProtocol::kSharedDropFlagPickupSucceeded) != 0;
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupGrant)
    {
        if (m_networkMode != CoopNetworkMode::Client ||
            !record.pickupPending || packet.sequence == 0 ||
            packet.sequence != record.pickupTransactionSequence)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "stale_pickup_grant_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        if (record.pickupGrantReceived)
        {
            if (!record.pickupCommitted)
                SendSharedDropPickupReady(record, record.pickupResultSucceeded);
            return;
        }
        if (!record.live || packet.objectVersion != record.version + 1)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "invalid_pickup_grant_version_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        record.version = packet.objectVersion;
        record.pickupGrantReceived = true;
        record.pickupRetrySecondsRemaining = 0.0f;
        record.pickupWinnerPeerHash = GetLocalAccountToken();
        const bool ready = ArkPlayer::GetInstancePtr() && m_enablePlayerSidecar &&
            CArkItem::GetItemFromEntityId(record.localEntityId) &&
            SaveLocalPlayerSidecar("recovery journal: shared drop prepared");
        if (ready && !WriteLocalPlayerRecoveryJournal("shared drop prepared"))
        {
            m_localInventoryDirty = true;
            m_localInventoryDirtySaveKey = m_currentHostSaveStateKey;
            m_localInventoryDirtyHostAccountToken = GetSessionHostAccountToken();
        }
        record.pickupResultSucceeded = ready;
        if (IEntity* entity = gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(record.localEntityId) : nullptr)
        {
            std::string detail;
            SetSharedDropWorldPresentation(*entity, false, detail);
        }
        SendSharedDropPickupReady(record, ready);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupRelease)
    {
        const bool peerRemovalRelease = packet.targetPeerHash == 0;
        if (m_networkMode != CoopNetworkMode::Client || packet.sequence == 0 ||
            (!peerRemovalRelease && packet.sequence != record.pickupTransactionSequence) ||
            packet.objectVersion < record.version ||
            (peerRemovalRelease && packet.objectVersion == record.version) ||
            (packet.flags & CoopProtocol::kSharedDropFlagLive) == 0)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "stale_pickup_release_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        if (record.pickupCommitted || record.localPickupGranted)
        {
            // Preserve a successful local native pickup if its matching
            // reservation release is replayed by the reliable transport.
            SendSharedDropPickupResult(record, true);
            m_lastSharedDropEvent = "pickup_release_after_success_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        record.version = packet.objectVersion;
        record.live = true;
        record.pickupPending = false;
        record.pickupGrantReceived = false;
        record.pickupResultSent = false;
        record.pickupResultSucceeded = false;
        record.pickupRetrySecondsRemaining = 0.0f;
        record.localPickupGranted = false;
        record.pickupWinnerPeerHash = 0;
        record.pickupTransactionSequence = 0;
        if (IEntity* entity = gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(record.localEntityId) : nullptr)
        {
            std::string detail;
            SetSharedDropWorldPresentation(*entity, true, detail);
        }
        m_lastSharedDropEvent = "pickup_released_id_" + std::to_string(record.stableSpawnId);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::RemoveRequest)
    {
        if (m_networkMode != CoopNetworkMode::Host)
        {
            ++m_sharedDropDropped;
            return;
        }

        if (!record.live)
        {
            CoopSerialSequence::Advance(m_sharedDropSequence);
            CoopProtocol::SharedDropPacket commit;
            const bool pickedUp = record.pickupWinnerPeerHash != 0;
            const CoopProtocol::SharedDropCommand terminalCommand = pickedUp
                ? CoopProtocol::SharedDropCommand::PickupCommit
                : CoopProtocol::SharedDropCommand::Remove;
            if (BuildSharedDropPacket(commit, terminalCommand, record, record.pickupWinnerPeerHash))
                SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop duplicate remove request commit failed");
            ++m_sharedDropDuplicateCommits;
            m_lastSharedDropEvent = "replayed_remove_commit_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        if (packet.objectVersion != record.version)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "remove_request_rejected_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        record.version += 1;
        record.live = false;
        record.pickupPending = false;
        record.localPickupGranted = false;
        record.nativePickupInProgress = false;
        record.pickupWinnerPeerHash = 0;
        std::string detail;
        RemoveSharedDropLocal(record, detail);
        CoopSerialSequence::Advance(m_sharedDropSequence);
        CoopProtocol::SharedDropPacket commit;
        if (!BuildSharedDropPacket(commit, CoopProtocol::SharedDropCommand::Remove, record) ||
            !SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop remove request commit failed"))
        {
            ++m_sharedDropDropped;
            return;
        }
        ++m_sharedDropApplied;
        m_lastSharedDropEvent = "committed_remote_remove_id_" + std::to_string(record.stableSpawnId);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupCommit || command == CoopProtocol::SharedDropCommand::Remove)
    {
        if (packet.objectVersion <= record.version)
        {
            ++m_sharedDropDuplicateCommits;
            return;
        }
        const bool grant =
            command == CoopProtocol::SharedDropCommand::PickupCommit &&
            packet.targetPeerHash == GetLocalAccountToken();

        if (grant)
        {
            if (!record.pickupGrantReceived ||
                packet.sequence == 0 || packet.sequence != record.pickupTransactionSequence)
            {
                ++m_sharedDropDropped;
                m_lastSharedDropEvent = "pickup_commit_without_prepare_id_" +
                    std::to_string(record.stableSpawnId);
                return;
            }
            record.version = packet.objectVersion;
            record.live = false;
            record.pickupCommitted = true;
            record.pickupPending = true;
            record.pickupResultSent = false;
            record.pickupResultSucceeded = false;
            record.pickupWinnerPeerHash = packet.targetPeerHash;
            TryDeliverCommittedSharedDrop(record);
            if (record.localPickupGranted && record.pickupDeliveryStored)
                SendSharedDropPickupResult(record, true);
            ++m_sharedDropApplied;
            m_lastSharedDropEvent = "finalized_local_pickup_id_" +
                std::to_string(record.stableSpawnId) +
                "_transaction_" + std::to_string(packet.sequence);
            return;
        }

        record.version = packet.objectVersion;
        record.pickupPending = false;
        record.pickupGrantReceived = false;
        record.pickupResultSent = false;
        record.pickupResultSucceeded = false;
        record.pickupWinnerPeerHash = packet.targetPeerHash;
        record.pickupTransactionSequence = packet.sequence;
        std::string detail;
        record.nativePickupInProgress = true;
        const bool removed = RemoveSharedDropLocal(record, detail);
        record.nativePickupInProgress = false;
        if (!removed)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "commit_remove_failed_" + detail;
            return;
        }
        ++m_sharedDropApplied;
        m_lastSharedDropEvent =
            std::string(grant ? "applied_local_pickup_id_" : "applied_remove_id_") +
            std::to_string(record.stableSpawnId) + "_" + detail;
        return;
    }

    ++m_sharedDropDropped;
    m_lastSharedDropEvent = "unknown_command_" + std::to_string(packet.command);
}

void ModMain::RecoverSavedSharedDrops()
{
    if (!m_sharedDropRecoveryRequired || m_networkMode != CoopNetworkMode::Host ||
        !IsSharedDropWorldReady() || !m_localLevelId || !ArkPlayer::GetInstance().m_pInventory)
        return;
    IEntityItPtr iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
        return;
    ArkInventory* inventory = ArkPlayer::GetInstance().m_pInventory;
    std::vector<EntityId> stale;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        const char* name = entity ? entity->GetName() : nullptr;
        if (!name || std::strncmp(name, "CoopSharedDrop_", 15) != 0)
            continue;
        const std::string idText(name + 15);
        if (idText.empty() || idText.find_first_not_of("0123456789") != std::string::npos)
            continue;
        uint64_t stableId = 0;
        if (!ParsePositiveInteger(idText, stableId))
            continue;
        CArkItem* item = CArkItem::GetItemFromEntityId(entity->GetId());
        if (!item || item->GetCount() <= 0)
            continue;
        auto known = m_sharedDrops.find(stableId);
        if (inventory->Contains(entity->GetId()))
        {
            // During a same-world stop/rehost, vanilla may have collected the
            // source offline. Its native inventory membership wins; do not
            // replay the old live catalog entry as another public copy.
            if (known != m_sharedDrops.end() && known->second.areaId == m_localLevelId &&
                known->second.archetypeId == item->GetArchetype())
            {
                known->second.live = false;
                known->second.pickupPending = false;
                known->second.localEntityId = INVALID_ENTITYID;
                m_sharedDropByEntityId.erase(entity->GetId());
            }
            continue;
        }
        if (known != m_sharedDrops.end())
        {
            SharedDropRecord& record = known->second;
            if (record.areaId != m_localLevelId || record.archetypeId != item->GetArchetype() ||
                (record.live && record.count != item->GetCount()))
                continue;
            if (!record.live || (record.localEntityId != INVALID_ENTITYID &&
                record.localEntityId != entity->GetId() && gEnv->pEntitySystem->GetEntity(record.localEntityId)))
            {
                stale.push_back(entity->GetId());
                continue;
            }
            record.localEntityId = entity->GetId();
            record.position = entity->GetWorldPos();
            record.rotation = entity->GetWorldRotation();
            m_sharedDropByEntityId[entity->GetId()] = stableId;
            RetireLivePropTrackingForSharedDrop(entity->GetId());
            std::string detail;
            SetSharedDropWorldPresentation(*entity, !record.pickupPending, detail);
            continue;
        }
        if (entity->IsHidden())
            continue;
        SharedDropRecord record;
        record.stableSpawnId = stableId;
        record.areaId = m_localLevelId;
        record.archetypeId = item->GetArchetype();
        record.ownerPeerHash = GetLocalAccountToken();
        record.localEntityId = entity->GetId();
        record.version = 1;
        record.count = item->GetCount();
        record.live = true;
        record.position = entity->GetWorldPos();
        record.rotation = entity->GetWorldRotation();
        m_sharedDrops.emplace(stableId, record);
        m_sharedDropByEntityId[entity->GetId()] = stableId;
        RetireLivePropTrackingForSharedDrop(entity->GetId());
    }
    // Do not invalidate the native iterator while examining saved sources.
    ++m_sharedDropApplyDepth;
    for (EntityId id : stale)
    {
        const auto binding = m_sharedDropByEntityId.find(id);
        if (binding != m_sharedDropByEntityId.end())
        {
            auto record = m_sharedDrops.find(binding->second);
            if (record != m_sharedDrops.end() && record->second.localEntityId == id)
                record->second.localEntityId = INVALID_ENTITYID;
            m_sharedDropByEntityId.erase(binding);
        }
        gEnv->pEntitySystem->RemoveEntity(id, true);
    }
    --m_sharedDropApplyDepth;
    // A peer can publish a new source while the coordinator is in another
    // area. On returning, that source has metadata but no native save entity.
    for (auto& entry : m_sharedDrops)
    {
        SharedDropRecord& record = entry.second;
        if (record.areaId != m_localLevelId || !record.live ||
            record.localEntityId != INVALID_ENTITYID)
            continue;
        CoopProtocol::SharedDropPacket packet;
        SharedDropRecord materialized;
        std::string detail;
        if (BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::Spawn, record) &&
            MaterializeSharedDrop(packet, materialized, detail))
            record.localEntityId = materialized.localEntityId;
    }
    m_sharedDropRecoveryRequired = false;
}

void ModMain::PruneUnboundSharedDrops()
{
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr())
        return;
    ArkInventory* inventory = ArkPlayer::GetInstance().m_pInventory;
    if (!inventory)
        return;
    std::vector<EntityId> stale;
    IEntityItPtr iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
        return;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        const char* name = entity ? entity->GetName() : nullptr;
        if (name && std::strncmp(name, "CoopSharedDrop_", 15) == 0 &&
            !inventory->Contains(entity->GetId()) && !m_sharedDropByEntityId.count(entity->GetId()))
            stale.push_back(entity->GetId());
    }
    ++m_sharedDropApplyDepth;
    for (EntityId id : stale)
        gEnv->pEntitySystem->RemoveEntity(id, true);
    --m_sharedDropApplyDepth;
}

void ModMain::ReplaySharedDrops()
{
    RecoverSavedSharedDrops();
    EnsureSharedDropHostSessionId();
    const auto peer = m_remotePeers.find(GetRemoteAccountToken());
    if (peer == m_remotePeers.end() || peer->second.levelId == 0)
        return;
    const uint64_t replayArea = peer->second.levelId;
    auto queue = [this](const CoopProtocol::SharedDropPacket& packet)
    {
        // This peer just reported WorldReady. The aggregate session gate is
        // updated later; do not broadcast or gate this targeted initial replay.
        if (QueueReliablePayloadToEndpoint(static_cast<uint16_t>(CoopProtocol::PacketType::SharedDrop),
                &packet, sizeof(packet), m_remoteAddress, m_remotePort,
                GetLocalAccountToken(), "shared drop replay failed"))
            ++m_sharedDropSent;
    };
    for (const auto& entry : m_sharedDrops)
    {
        const SharedDropRecord& record = entry.second;
        if (!record.live || record.pickupCommitted || record.areaId != replayArea)
            continue;
        CoopProtocol::SharedDropPacket packet;
        if (BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::Spawn, record))
            queue(packet);
    }
    // Same reliable stream: prune stale snapshot copies only after all live
    // sources have been adopted. Inventory-owned objects are never pruned.
    CoopProtocol::SharedDropPacket end = {};
    end.command = static_cast<uint16_t>(CoopProtocol::SharedDropCommand::SnapshotEnd);
    end.worldEpoch = m_localWorldEpoch;
    end.areaId = replayArea;
    end.hostSaveKeyHash = CurrentHostSaveKeyHash();
    end.hostSessionId = m_sharedDropHostSessionId;
    end.sourcePeerHash = GetLocalAccountToken();
    queue(end);
}

void ModMain::ResetSharedDropState(const char* reason, bool preserveAreaDrops)
{
    if (preserveAreaDrops)
    {
        DetachSharedDropEntities();
        return;
    }
    // Visible marked sources belong to the native world, including loaded
    // checkpoints. Keep them for adoption/replay; only hidden orphan objects
    // are disposable. Marker-aware pickup blocks the unbound interval.
    std::unordered_set<EntityId> preservedEntityIds;
    const uint64_t localAccountToken = GetLocalAccountToken();
    for (const auto& entry : m_sharedDrops)
    {
        const SharedDropRecord& record = entry.second;
        if (record.localEntityId == INVALID_ENTITYID)
            continue;

        IEntity* entity = gEnv && gEnv->pEntitySystem
            ? gEnv->pEntitySystem->GetEntity(record.localEntityId)
            : nullptr;
        CArkItem* item = entity ? CArkItem::GetItemFromEntityId(record.localEntityId) : nullptr;
        unsigned ownerId = 0;
        if (item)
            CoopRuntimeGuards::TryGuardedCall("shared drop reset owner", [item]() { return item->GetOwnerId(); }, ownerId, nullptr);
        bool inInventory = false;
        ArkInventory* inventory = ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().m_pInventory : nullptr;
        if (inventory)
            CoopRuntimeGuards::TryGuardedCall("shared drop reset membership", [inventory, &record]()
                { return inventory->Contains(record.localEntityId); }, inInventory, nullptr);
        if (inInventory || (record.live && record.ownerPeerHash == localAccountToken))
            preservedEntityIds.insert(record.localEntityId);
    }

    std::vector<EntityId> staleWorldEntities;
    if (gEnv && gEnv->pEntitySystem)
    {
        IEntityIt* rawIterator = gEnv->pEntitySystem->GetEntityIterator();
        if (rawIterator)
        {
            IEntityItPtr iterator = rawIterator;
            iterator->MoveFirst();
            while (!iterator->IsEnd())
            {
                IEntity* entity = iterator->Next();
                if (!entity || preservedEntityIds.find(entity->GetId()) != preservedEntityIds.end())
                    continue;
                const char* name = entity->GetName();
                if (!name || std::strncmp(name, "CoopSharedDrop_", 15) != 0)
                    continue;

                CArkItem* item = CArkItem::GetItemFromEntityId(entity->GetId());
                unsigned ownerId = 0;
                if (item)
                    CoopRuntimeGuards::TryGuardedCall("shared drop orphan owner", [item]() { return item->GetOwnerId(); }, ownerId, nullptr);
                bool inInventory = false;
                ArkInventory* inventory = ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().m_pInventory : nullptr;
                if (inventory)
                    CoopRuntimeGuards::TryGuardedCall("shared drop orphan membership", [inventory, entity]()
                        { return inventory->Contains(entity->GetId()); }, inInventory, nullptr);
                if (!inInventory && entity->IsHidden())
                    staleWorldEntities.push_back(entity->GetId());
            }
        }
    }

    m_sharedDrops.clear();
    m_sharedDropByEntityId.clear();
    m_sharedDropRecoveryRequired = true;
    m_sharedDropApplyDepth = 1;
    for (const EntityId entityId : staleWorldEntities)
    {
        CArkItem* item = CArkItem::GetItemFromEntityId(entityId);
        std::string removeReason;
        bool removed = item &&
            CoopRuntimeGuards::TryGuardedVoidCall(
                "shared drop reset item remove",
                [item]() { item->RemoveEntity(); },
                &removeReason);
        if (!removed && gEnv && gEnv->pEntitySystem)
        {
            CoopRuntimeGuards::TryGuardedVoidCall(
                "shared drop reset entity remove",
                [entityId]() { gEnv->pEntitySystem->RemoveEntity(entityId, true); },
                &removeReason);
        }
    }
    m_sharedDropApplyDepth = 0;
    m_sharedDropSequence = 0;
    m_sharedDropSent = 0;
    m_sharedDropReceived = 0;
    m_sharedDropApplied = 0;
    m_sharedDropDropped = 0;
    m_sharedDropPickupRequests = 0;
    m_sharedDropPickupCommits = 0;
    m_sharedDropPickupSuppressions = 0;
    m_sharedDropDuplicateCommits = 0;
    m_sharedDropEntityRetirements = 0;
    m_lastSharedDropEvent = reason ? reason : "reset";
}

bool ModMain::DebugSharedDropCommand(
    const std::string& command,
    const std::vector<std::string>& args,
    std::string& detail)
{
    detail.clear();
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    ArkInventory* inventory = player ? player->m_pInventory : nullptr;
    if (m_saveLoadGuardActive || !player || !player->GetEntity() || !inventory || !gEnv || !gEnv->pEntitySystem)
    {
        detail = "game_player_or_inventory_not_ready";
        return false;
    }

    if (command == "coop_shared_drop_drop")
    {
        uint64_t archetypeId = 0;
        if (args.empty() || !ParsePositiveInteger(args[0], archetypeId))
        {
            detail = "usage_archetype_id_optional_count";
            return false;
        }

        CArkItem* selected = nullptr;
        std::string reason;
        for (unsigned itemId : inventory->GetItemIDs())
        {
            CArkItem* item = CArkItem::GetItemFromEntityId(itemId);
            uint64_t itemArchetype = 0;
            bool plotCritical = true;
            if (item &&
                CoopRuntimeGuards::TryGuardedCall("shared drop debug archetype", [item]() { return item->GetArchetype(); }, itemArchetype, &reason) &&
                itemArchetype == archetypeId &&
                CoopRuntimeGuards::TryGuardedCall("shared drop debug plot", [item]() { return item->IsPlotCritical(); }, plotCritical, &reason) &&
                !plotCritical)
            {
                selected = item;
                break;
            }
        }
        if (!selected)
        {
            detail = "non_plot_inventory_item_not_found_arch_" + std::to_string(archetypeId);
            return false;
        }

        int available = 0;
        if (!CoopRuntimeGuards::TryGuardedCall("shared drop debug count", [selected]() { return selected->GetCount(); }, available, &reason) || available <= 0)
        {
            detail = "item_count_unavailable_" + reason;
            return false;
        }

        std::unordered_set<uint64_t> recordsBefore;
        for (const auto& entry : m_sharedDrops)
            recordsBefore.insert(entry.first);
        const int dropCount = std::min(ParseAttemptCount(args, 1, 1), available);
        const EntityId sourceEntityId = selected->GetEntityId();
        selected->Drop(dropCount, nullptr);

        const SharedDropRecord* created = nullptr;
        for (const auto& entry : m_sharedDrops)
        {
            if (recordsBefore.count(entry.first) == 0 && entry.second.live &&
                entry.second.ownerPeerHash == GetLocalAccountToken() && entry.second.archetypeId == archetypeId)
            {
                created = &entry.second;
                break;
            }
        }
        detail =
            "arch_" + std::to_string(archetypeId) +
            "_count_" + std::to_string(dropCount) +
            "_source_" + std::to_string(sourceEntityId) +
            "_entity_" + std::to_string(created ? created->localEntityId : INVALID_ENTITYID) +
            "_stable_" + std::to_string(created ? created->stableSpawnId : 0);
        m_lastSharedDropEvent = "debug_drop_" + detail;
        return created != nullptr;
    }

    uint64_t requestedStableId = 0;
    if (!args.empty() && args[0] != "nearest" && !ParsePositiveInteger(args[0], requestedStableId))
    {
        detail = "usage_optional_stable_id_or_nearest";
        return false;
    }

    SharedDropRecord* selected = nullptr;
    float nearestDistanceSq = std::numeric_limits<float>::max();
    const Vec3 playerPosition = player->GetEntity()->GetWorldPos();
    for (auto& entry : m_sharedDrops)
    {
        SharedDropRecord& candidate = entry.second;
        if (requestedStableId != 0 && candidate.stableSpawnId != requestedStableId)
            continue;
        IEntity* entity = candidate.localEntityId == INVALID_ENTITYID
            ? nullptr
            : gEnv->pEntitySystem->GetEntity(candidate.localEntityId);
        if (!entity)
        {
            if (requestedStableId != 0)
                selected = &candidate;
            continue;
        }
        const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
        if (!selected || requestedStableId != 0 || distanceSq < nearestDistanceSq)
        {
            selected = &candidate;
            nearestDistanceSq = distanceSq;
        }
    }
    if (!selected)
    {
        detail = "shared_drop_record_not_found";
        return false;
    }

    auto describe = [this](const SharedDropRecord& record)
    {
        IEntity* entity = record.localEntityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem
            ? nullptr
            : gEnv->pEntitySystem->GetEntity(record.localEntityId);
        CArkItem* item = entity ? CArkItem::GetItemFromEntityId(entity->GetId()) : nullptr;
        int nativeCount = 0;
        unsigned ownerId = 0;
        bool hidden = false;
        if (item)
        {
            CoopRuntimeGuards::TryGuardedCall("shared drop debug native count", [item]() { return item->GetCount(); }, nativeCount, nullptr);
            CoopRuntimeGuards::TryGuardedCall("shared drop debug owner", [item]() { return item->GetOwnerId(); }, ownerId, nullptr);
        }
        if (entity)
            CoopRuntimeGuards::TryGuardedCall("shared drop debug hidden", [entity]() { return entity->IsHidden(); }, hidden, nullptr);
        const auto delivery = std::find_if(m_localSharedDropDeliveries.begin(), m_localSharedDropDeliveries.end(),
            [this, &record](const CoopSharedDropDelivery::Entry& entry)
            {
                return entry.hostSessionId == m_sharedDropHostSessionId && entry.stableSpawnId == record.stableSpawnId;
            });
        return
            "stable_" + std::to_string(record.stableSpawnId) +
            "_entity_" + std::to_string(record.localEntityId) +
            "_exists_" + std::to_string(entity ? 1 : 0) +
            "_hidden_" + std::to_string(hidden ? 1 : 0) +
            "_live_" + std::to_string(record.live ? 1 : 0) +
            "_pending_" + std::to_string(record.pickupPending ? 1 : 0) +
            "_granted_" + std::to_string(record.localPickupGranted ? 1 : 0) +
            "_committed_" + std::to_string(record.pickupCommitted ? 1 : 0) +
            "_stored_" + std::to_string(record.pickupDeliveryStored ? 1 : 0) +
            "_attempted_" + std::to_string(record.pickupDeliveryAttempted ? 1 : 0) +
            "_received_" + std::to_string(delivery == m_localSharedDropDeliveries.end() ? 0 : delivery->deliveredCount) +
            "_version_" + std::to_string(record.version) +
            "_arch_" + std::to_string(record.archetypeId) +
            "_count_" + std::to_string(record.count) +
            "_nativeCount_" + std::to_string(nativeCount) +
            "_owner_" + std::to_string(ownerId) +
            "_winner_" + std::to_string(record.pickupWinnerPeerHash);
    };

    if (command == "coop_shared_drop_probe")
    {
        detail = describe(*selected);
        m_lastSharedDropEvent = "debug_probe_" + detail;
        return true;
    }

    IEntity* entity = selected->localEntityId == INVALID_ENTITYID
        ? nullptr
        : gEnv->pEntitySystem->GetEntity(selected->localEntityId);
    CArkItem* item = entity ? CArkItem::GetItemFromEntityId(entity->GetId()) : nullptr;
    if (command != "coop_shared_drop_pickup" || !item)
    {
        detail = "pickup_entity_unavailable_" + describe(*selected);
        return false;
    }

    const int attempts = ParseAttemptCount(args, 1, 1);
    int nativeSuccesses = 0;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        entity = selected->localEntityId == INVALID_ENTITYID
            ? nullptr
            : gEnv->pEntitySystem->GetEntity(selected->localEntityId);
        item = entity ? CArkItem::GetItemFromEntityId(entity->GetId()) : nullptr;
        if (!item)
            break;
        nativeSuccesses += item->PickUp(player->GetEntityId(), false) ? 1 : 0;
    }
    detail =
        "attempts_" + std::to_string(attempts) +
        "_nativeSuccesses_" + std::to_string(nativeSuccesses) +
        "_" + describe(*selected);
    m_lastSharedDropEvent = "debug_pickup_" + detail;
    return nativeSuccesses > 0 || selected->pickupPending || !selected->live;
}

void ModMain::OnSharedDropEntityRemoved(EntityId entityId)
{
    const bool locallyRecycled = m_localRecyclerSharedDropTargets.erase(entityId) != 0;
    const auto binding = m_sharedDropByEntityId.find(entityId);
    if (binding == m_sharedDropByEntityId.end())
        return;

    const uint64_t stableSpawnId = binding->second;
    auto record = m_sharedDrops.find(stableSpawnId);
    if (record != m_sharedDrops.end() && record->second.nativePickupInProgress)
        return;

    m_sharedDropByEntityId.erase(binding);
    if (record != m_sharedDrops.end() && record->second.localEntityId == entityId)
        record->second.localEntityId = INVALID_ENTITYID;
    ++m_sharedDropEntityRetirements;

    if (record == m_sharedDrops.end() || !record->second.live)
        return;
    if (m_arkLevelTransitionLoadActive || m_saveLoadGuardActive || m_runtimeTransitionCleanupPrepared)
    {
        m_sharedDropRecoveryRequired = true;
        return;
    }

    // Unexpected native retirement is terminal rather than permission to
    // materialize a replacement. The Host publishes that fail-closed decision
    // so a disappearing replica can never turn into a second grant.
    SharedDropRecord& removedRecord = record->second;
    removedRecord.live = false;
    removedRecord.pickupPending = false;
    removedRecord.localPickupGranted = false;
    removedRecord.nativePickupInProgress = false;
    removedRecord.pickupWinnerPeerHash = 0;
    if (m_networkMode == CoopNetworkMode::Host && IsSharedDropServiceReady())
    {
        removedRecord.version += 1;
        CoopSerialSequence::Advance(m_sharedDropSequence);
        CoopProtocol::SharedDropPacket packet;
        if (BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::Remove, removedRecord) &&
            SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop unexpected remove commit failed"))
        {
            ++m_sharedDropApplied;
        }
    }
    else if (locallyRecycled &&
        m_networkMode == CoopNetworkMode::Client &&
        IsSharedDropServiceReady())
    {
        CoopSerialSequence::Advance(m_sharedDropSequence);
        CoopProtocol::SharedDropPacket packet;
        if (BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::RemoveRequest, removedRecord) &&
            SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop remove request failed"))
        {
            ++m_sharedDropApplied;
        }
    }
    m_lastSharedDropEvent =
        "unexpected_entity_retire_id_" + std::to_string(stableSpawnId) +
        "_entity_" + std::to_string(entityId) +
        "_terminal_" + std::to_string(m_networkMode == CoopNetworkMode::Host ? 1 : 0);
}

void ModMain::MarkRemoteRecyclerGrenadeForHook(CArkProjectileRecyclerGrenade* grenade)
{
    if (grenade)
        m_remoteRecyclerGrenades.insert(grenade);
}

void ModMain::MarkLocalRecyclerSharedDropForHook(
    const CArkProjectileRecyclerGrenade* grenade,
    EntityId targetEntityId)
{
    if (!grenade || targetEntityId == INVALID_ENTITYID ||
        m_remoteRecyclerGrenades.find(grenade) != m_remoteRecyclerGrenades.end() ||
        m_sharedDropByEntityId.find(targetEntityId) == m_sharedDropByEntityId.end())
    {
        return;
    }

    m_localRecyclerSharedDropTargets[targetEntityId] = grenade;
}

void ModMain::UnmarkLocalRecyclerSharedDropForHook(
    const CArkProjectileRecyclerGrenade* grenade,
    EntityId targetEntityId)
{
    const auto target = m_localRecyclerSharedDropTargets.find(targetEntityId);
    if (target != m_localRecyclerSharedDropTargets.end() && target->second == grenade)
        m_localRecyclerSharedDropTargets.erase(target);
}

void ModMain::OnRemoteRecyclerGrenadeDestroyedForHook(CArkProjectileRecyclerGrenade* grenade)
{
    m_remoteRecyclerGrenades.erase(grenade);
    for (auto it = m_localRecyclerSharedDropTargets.begin(); it != m_localRecyclerSharedDropTargets.end();)
    {
        if (it->second == grenade)
            it = m_localRecyclerSharedDropTargets.erase(it);
        else
            ++it;
    }
}

bool ModMain::ShouldSuppressRecyclerForSharedDrop(
    const CArkProjectileRecyclerGrenade* grenade,
    EntityId targetEntityId) const
{
    // Recycler processing continues for several frames after Detonate returns,
    // and its entity can retire before those callbacks. Track the native
    // recycler object until Destroy rather than relying on either short lifetime.
    if (!grenade || targetEntityId == INVALID_ENTITYID)
        return false;
    const auto binding = m_sharedDropByEntityId.find(targetEntityId);
    if (binding == m_sharedDropByEntityId.end())
        return false;
    const auto record = m_sharedDrops.find(binding->second);
    if (record != m_sharedDrops.end() &&
        (record->second.pickupPending || record->second.pickupCommitted))
        return true;
    return m_remoteRecyclerGrenades.find(grenade) != m_remoteRecyclerGrenades.end();
}
