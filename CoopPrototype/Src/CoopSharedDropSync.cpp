#include "ModMain.h"
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
    packet.areaId = record.areaId;
    packet.stableSpawnId = record.stableSpawnId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = targetPeerHash;
    packet.archetypeId = record.archetypeId;
    packet.command = static_cast<uint16_t>(command);
    packet.flags = record.live ? 1u : 0u;
    packet.count = record.count;

    IEntity* entity = gEnv && gEnv->pEntitySystem && record.localEntityId != INVALID_ENTITYID
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

void ModMain::OnNativeSharedItemDropped(CArkItem* item, int droppedCount, const char* reason)
{
    if (!item || m_sharedDropApplyDepth != 0 || m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return;
    }

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

    // A SharedDrop picked up on this peer may keep its inventory entity hidden.
    // Vanilla Drop can reuse or clone it without clearing all presentation
    // flags, which leaves the old visible ghost beside the real new drop.
    SetSharedDropWorldPresentation(*entity, true, guardReason);

    // Terminal records reject delayed reliable packets, but they do not need to
    // grow without bound over a long session once their native entity is gone.
    if (m_sharedDrops.size() >= 4096)
    {
        for (auto it = m_sharedDrops.begin(); it != m_sharedDrops.end() && m_sharedDrops.size() > 3072;)
        {
            if (!it->second.live && it->second.localEntityId == INVALID_ENTITYID)
                it = m_sharedDrops.erase(it);
            else
                ++it;
        }
    }

    SharedDropRecord record;
    record.areaId = m_localLevelId;
    record.archetypeId = archetypeId;
    record.ownerPeerHash = GetLocalAccountToken();
    record.localEntityId = entity->GetId();
    record.version = 1;
    record.count = count;
    record.live = true;
    const uint32_t sequence = CoopSerialSequence::Advance(m_sharedDropSequence);
    record.stableSpawnId = BuildSharedDropStableId(archetypeId, entity->GetWorldPos(), sequence);

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

bool ModMain::ShouldDeferNativeSharedItemPickup(CArkItem* item, EntityId pickerId, const char* reason)
{
    if (!item || m_sharedDropApplyDepth != 0 || m_networkMode == CoopNetworkMode::Off ||
        !ArkPlayer::GetInstancePtr() || pickerId != ArkPlayer::GetInstance().GetEntityId())
    {
        return false;
    }

    const auto found = m_sharedDropByEntityId.find(item->GetEntityId());
    if (found == m_sharedDropByEntityId.end())
        return false;
    auto recordIt = m_sharedDrops.find(found->second);
    if (recordIt == m_sharedDrops.end())
        return false;

    SharedDropRecord& record = recordIt->second;
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

    // The Host is the arbiter, but its own successful native pickup is the
    // authoritative transaction. OnNativeSharedItemPicked commits it afterward.
    if (m_networkMode == CoopNetworkMode::Host)
    {
        record.nativePickupInProgress = true;
        return false;
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
    if (!BuildSharedDropPacket(packet, CoopProtocol::SharedDropCommand::PickupRequest, record) ||
        !SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop pickup request failed"))
    {
        ++m_sharedDropDropped;
        return true;
    }
    record.pickupPending = true;
    ++m_sharedDropPickupRequests;
    m_lastSharedDropEvent =
        "sent_pickup_request_id_" + std::to_string(record.stableSpawnId) +
        "_reason_" + (reason ? reason : "-");
    return true;
}

void ModMain::OnNativeSharedItemPicked(EntityId itemEntityId, EntityId pickerId, bool success, const char* reason)
{
    if (m_sharedDropApplyDepth != 0)
        return;
    const auto byEntity = m_sharedDropByEntityId.find(itemEntityId);
    if (byEntity == m_sharedDropByEntityId.end())
        return;
    auto recordIt = m_sharedDrops.find(byEntity->second);
    if (recordIt == m_sharedDrops.end())
        return;

    SharedDropRecord& record = recordIt->second;
    record.nativePickupInProgress = false;
    if (!success || !record.live)
    {
        if (!gEnv || !gEnv->pEntitySystem || !gEnv->pEntitySystem->GetEntity(itemEntityId))
        {
            m_sharedDropByEntityId.erase(byEntity);
            record.localEntityId = INVALID_ENTITYID;
        }
        return;
    }
    record.live = false;
    record.pickupPending = false;
    record.localPickupGranted = true;
    record.pickupWinnerPeerHash = GetLocalAccountToken();
    record.version += 1;

    IEntity* grantedEntity = gEnv && gEnv->pEntitySystem
        ? gEnv->pEntitySystem->GetEntity(itemEntityId)
        : nullptr;
    if (grantedEntity)
    {
        std::string presentationReason;
        SetSharedDropWorldPresentation(*grantedEntity, false, presentationReason);
    }

    // Do not erase the entity binding here. CArkItem::PickUp may keep the world
    // entity alive during its native lerp, and that exact interval previously
    // allowed the same shared stack to be picked up repeatedly. OnRemove retires
    // the binding, or a later Drop replaces it with a fresh shared lifetime.
    if (!gEnv || !gEnv->pEntitySystem || !gEnv->pEntitySystem->GetEntity(itemEntityId))
    {
        m_sharedDropByEntityId.erase(byEntity);
        record.localEntityId = INVALID_ENTITYID;
    }

    if (m_networkMode == CoopNetworkMode::Host && m_hasRemoteEndpoint && IsSessionGameplayReady())
    {
        CoopSerialSequence::Advance(m_sharedDropSequence);
        CoopProtocol::SharedDropPacket packet;
        if (BuildSharedDropPacket(
                packet,
                CoopProtocol::SharedDropCommand::PickupCommit,
                record,
                GetLocalAccountToken()) &&
            SendSharedDropTo(packet, m_remoteAddress, m_remotePort, "shared drop pickup commit failed"))
        {
            ++m_sharedDropPickupCommits;
        }
    }
    m_lastSharedDropEvent =
        "local_pickup_id_" + std::to_string(record.stableSpawnId) +
        "_picker_" + std::to_string(pickerId) +
        "_reason_" + (reason ? reason : "-");
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

    IEntity* entity = nullptr;
    ++m_sharedDropApplyDepth;
    const bool spawned = CoopRuntimeGuards::TryGuardedCall(
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

bool ModMain::RemoveSharedDropLocal(SharedDropRecord& record, bool grantToLocalPlayer, std::string& detail)
{
    IEntity* entity = gEnv && gEnv->pEntitySystem && record.localEntityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(record.localEntityId)
        : nullptr;
    if (!entity)
    {
        record.live = false;
        record.pickupPending = false;
        record.localEntityId = INVALID_ENTITYID;
        detail = grantToLocalPlayer ? "grant_entity_missing" : "already_missing";
        return !grantToLocalPlayer;
    }

    const EntityId entityId = entity->GetId();
    CArkItem* item = CArkItem::GetItemFromEntityId(entityId);
    bool ok = false;
    std::string reason;
    ++m_sharedDropApplyDepth;
    if (grantToLocalPlayer && item && ArkPlayer::GetInstancePtr())
    {
        ok = CoopRuntimeGuards::TryGuardedCall(
            "shared drop committed PickUp",
            [item]() { return item->PickUp(ArkPlayer::GetInstance().GetEntityId(), false); },
            ok,
            &reason) && ok;
    }
    else if (!grantToLocalPlayer && item)
    {
        ok = CoopRuntimeGuards::TryGuardedVoidCall("shared drop RemoveEntity", [item]() { item->RemoveEntity(); }, &reason);
    }
    if (!grantToLocalPlayer && !ok && gEnv && gEnv->pEntitySystem)
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
    if (grantToLocalPlayer && ok)
    {
        record.localPickupGranted = true;
        record.pickupWinnerPeerHash = GetLocalAccountToken();
        IEntity* grantedEntity = gEnv && gEnv->pEntitySystem
            ? gEnv->pEntitySystem->GetEntity(entityId)
            : nullptr;
        bool presentationHidden = !grantedEntity;
        if (grantedEntity)
            presentationHidden = SetSharedDropWorldPresentation(*grantedEntity, false, reason);
        else
        {
            m_sharedDropByEntityId.erase(entityId);
            record.localEntityId = INVALID_ENTITYID;
        }
        detail =
            "granted_entity_" + std::to_string(entityId) +
            "_nativeImmediate_1" +
            "_entityRetained_" + std::to_string(grantedEntity ? 1 : 0) +
            "_worldHidden_" + std::to_string(presentationHidden ? 1 : 0);
        return true;
    }

    if (!grantToLocalPlayer)
    {
        m_sharedDropByEntityId.erase(entityId);
        record.localEntityId = INVALID_ENTITYID;
    }
    detail = ok
        ? "removed_entity_" + std::to_string(entityId)
        : std::string(grantToLocalPlayer ? "grant_failed_" : "remove_failed_") + reason;
    return ok;
}

void ModMain::HandleSharedDrop(const CoopProtocol::SharedDropPacket& packet)
{
    ++m_sharedDropReceived;
    const uint64_t remotePeerHash = GetRemoteAccountToken();
    const auto command = static_cast<CoopProtocol::SharedDropCommand>(packet.command);
    const bool roleAllowsCommand =
        (m_networkMode == CoopNetworkMode::Host &&
            (command == CoopProtocol::SharedDropCommand::Spawn ||
                command == CoopProtocol::SharedDropCommand::PickupRequest)) ||
        (m_networkMode == CoopNetworkMode::Client &&
            (command == CoopProtocol::SharedDropCommand::Spawn ||
                command == CoopProtocol::SharedDropCommand::PickupCommit ||
                command == CoopProtocol::SharedDropCommand::Remove));
    if (packet.worldEpoch != m_localWorldEpoch ||
        !IsCurrentOrRecentHostSaveKeyHash(packet.hostSaveKeyHash) ||
        packet.areaId != m_localLevelId || packet.stableSpawnId == 0 || packet.archetypeId == 0 ||
        packet.sourcePeerHash == 0 || packet.sourcePeerHash != remotePeerHash || !roleAllowsCommand ||
        (command == CoopProtocol::SharedDropCommand::Spawn && !IsFiniteTransform(packet)))
    {
        ++m_sharedDropDropped;
        m_lastSharedDropEvent = "rejected_context_or_source";
        return;
    }

    auto found = m_sharedDrops.find(packet.stableSpawnId);
    if (command == CoopProtocol::SharedDropCommand::Spawn)
    {
        if (found != m_sharedDrops.end() && found->second.version >= packet.objectVersion)
        {
            m_lastSharedDropEvent = "duplicate_spawn_id_" + std::to_string(packet.stableSpawnId);
            return;
        }

        SharedDropRecord record;
        std::string detail;
        if (!MaterializeSharedDrop(packet, record, detail))
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "spawn_apply_failed_" + detail;
            return;
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

    if (command == CoopProtocol::SharedDropCommand::PickupRequest)
    {
        if (m_networkMode == CoopNetworkMode::Host && !record.live && record.pickupWinnerPeerHash != 0)
        {
            // A repeated request can arrive after the first reliable commit was
            // queued. Re-broadcast the same terminal decision; peers apply it
            // idempotently and only the original winner receives the item.
            CoopSerialSequence::Advance(m_sharedDropSequence);
            CoopProtocol::SharedDropPacket commit;
            if (BuildSharedDropPacket(
                    commit,
                    CoopProtocol::SharedDropCommand::PickupCommit,
                    record,
                    record.pickupWinnerPeerHash))
            {
                SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop duplicate request commit failed");
            }
            ++m_sharedDropPickupSuppressions;
            m_lastSharedDropEvent = "replayed_pickup_commit_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        if (m_networkMode != CoopNetworkMode::Host || !record.live || packet.objectVersion != record.version)
        {
            ++m_sharedDropDropped;
            m_lastSharedDropEvent = "pickup_request_rejected_id_" + std::to_string(record.stableSpawnId);
            return;
        }

        record.version += 1;
        record.live = false;
        record.pickupPending = false;
        record.pickupWinnerPeerHash = packet.sourcePeerHash;
        std::string detail;
        RemoveSharedDropLocal(record, false, detail);
        CoopSerialSequence::Advance(m_sharedDropSequence);
        CoopProtocol::SharedDropPacket commit;
        if (!BuildSharedDropPacket(commit, CoopProtocol::SharedDropCommand::PickupCommit, record, packet.sourcePeerHash) ||
            !SendSharedDropTo(commit, m_remoteAddress, m_remotePort, "shared drop pickup commit failed"))
        {
            ++m_sharedDropDropped;
            return;
        }
        ++m_sharedDropPickupCommits;
        ++m_sharedDropApplied;
        m_lastSharedDropEvent = "committed_remote_pickup_id_" + std::to_string(record.stableSpawnId);
        return;
    }

    if (command == CoopProtocol::SharedDropCommand::PickupCommit || command == CoopProtocol::SharedDropCommand::Remove)
    {
        if (packet.objectVersion <= record.version)
        {
            ++m_sharedDropDuplicateCommits;
            return;
        }
        record.version = packet.objectVersion;
        record.pickupPending = false;
        record.pickupWinnerPeerHash = packet.targetPeerHash;
        const bool grant =
            command == CoopProtocol::SharedDropCommand::PickupCommit &&
            packet.targetPeerHash == GetLocalAccountToken();
        std::string detail;
        if (!RemoveSharedDropLocal(record, grant, detail))
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

void ModMain::ResetSharedDropState(const char* reason)
{
    // A reconnect/reset must not leave network-materialized world replicas
    // behind as ordinary loot. Preserve inventory-owned entities and the
    // locally originated Vanilla drop; remove remote or orphan world copies
    // before forgetting their stable records.
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
        if (ownerId != 0 || (record.live && record.ownerPeerHash == localAccountToken))
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
                if (ownerId == 0)
                    staleWorldEntities.push_back(entity->GetId());
            }
        }
    }

    m_sharedDrops.clear();
    m_sharedDropByEntityId.clear();
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
        return
            "stable_" + std::to_string(record.stableSpawnId) +
            "_entity_" + std::to_string(record.localEntityId) +
            "_exists_" + std::to_string(entity ? 1 : 0) +
            "_hidden_" + std::to_string(hidden ? 1 : 0) +
            "_live_" + std::to_string(record.live ? 1 : 0) +
            "_pending_" + std::to_string(record.pickupPending ? 1 : 0) +
            "_granted_" + std::to_string(record.localPickupGranted ? 1 : 0) +
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

    // Unexpected native retirement is terminal rather than permission to
    // materialize a replacement. The Host publishes that fail-closed decision
    // so a disappearing replica can never turn into a second grant.
    SharedDropRecord& removedRecord = record->second;
    removedRecord.live = false;
    removedRecord.pickupPending = false;
    removedRecord.localPickupGranted = false;
    removedRecord.nativePickupInProgress = false;
    removedRecord.pickupWinnerPeerHash = 0;
    if (m_networkMode == CoopNetworkMode::Host &&
        m_hasRemoteEndpoint &&
        IsSessionGameplayReady())
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
    m_lastSharedDropEvent =
        "unexpected_entity_retire_id_" + std::to_string(stableSpawnId) +
        "_entity_" + std::to_string(entityId) +
        "_terminal_" + std::to_string(m_networkMode == CoopNetworkMode::Host ? 1 : 0);
}
