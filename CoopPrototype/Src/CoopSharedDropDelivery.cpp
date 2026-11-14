#include "ModMain.h"
#include "CoopItemClassification.h"
#include "CoopRuntimeGuards.h"
#include "CoopRuntimeLog.h"
#include "CoopSharedDropDelivery.h"

#include <algorithm>
#include <iterator>

#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/arkitem.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
bool ReadInventoryCount(ArkInventory& inventory, uint64_t archetypeId, int& count)
{
    return CoopRuntimeGuards::TryGuardedCall(
        "shared drop delivery inventory count",
        [&inventory, archetypeId]() { return inventory.GetCountOfArchetype(archetypeId); },
        count,
        nullptr) && count >= 0;
}

auto FindDelivery(std::vector<CoopSharedDropDelivery::Entry>& deliveries, uint64_t sessionId, uint64_t stableId)
{
    return std::find_if(deliveries.begin(), deliveries.end(),
        [sessionId, stableId](const CoopSharedDropDelivery::Entry& entry)
        {
            return entry.hostSessionId == sessionId && entry.stableSpawnId == stableId;
        });
}

bool FinishDeliveryPresentation(EntityId entityId)
{
    IEntity* entity = gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr;
    if (!entity)
        return false;
    CArkItem* item = CArkItem::GetItemFromEntityId(entityId);
    std::string reason;
    if (!CoopItemClassification::IsRealWeaponInventoryItem(item, reason))
    {
        CoopRuntimeGuards::TryGuardedVoidCall(
            "shared drop delivery hide retained inventory entity",
            [entity]()
            {
                entity->Hide(true);
                entity->Invisible(true);
                entity->EnablePhysics(false);
            },
            &reason);
    }
    return true;
}
}

bool ModMain::PersistLocalSharedDropDelivery(SharedDropRecord& record)
{
    if (m_networkMode == CoopNetworkMode::Off && HasContinuingHostInventoryWorld())
    {
        const auto delivery = FindDelivery(m_localSharedDropDeliveries, m_sharedDropHostSessionId, record.stableSpawnId);
        record.pickupDeliveryStored = delivery != m_localSharedDropDeliveries.end() &&
            StoreCommittedSharedDropDelivery(GetLocalAccountToken(), *delivery);
        return record.pickupDeliveryStored;
    }
    // Receipt and inventory are serialized together. A failed capture leaves the
    // committed entitlement assigned, never released back into the shared world.
    MarkLocalInventoryDirty("shared drop delivery receipt");
    const uint64_t revision = m_localInventoryDirtyRevision;
    constexpr const char* reason = "recovery journal: shared drop delivery receipt";
    record.pickupDeliveryStored = SaveLocalPlayerSidecar(reason);
    const bool journalWritten = record.pickupDeliveryStored &&
        WriteLocalPlayerRecoveryJournal(reason);
    if (record.pickupDeliveryStored && !m_pendingClientPlayerStateUpload)
        QueueClientPlayerStateUpload("inventory mutation", m_currentHostSaveStateKey);
    if (journalWritten && revision == m_localInventoryDirtyRevision)
    {
        m_localInventoryDirty = false;
        m_localInventoryDirtySaveKey.clear();
        m_localInventoryDirtyHostAccountToken = 0;
        m_localInventoryJournalAccumulator = 0.0f;
    }
    return record.pickupDeliveryStored;
}

bool ModMain::TryDeliverCommittedSharedDrop(SharedDropRecord& record, bool interactive)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    ArkInventory* inventory = player ? player->m_pInventory : nullptr;
    if (!IsSharedDropWorldReady() || m_sharedDropDeliveryRecoveryRequired || !record.pickupCommitted || record.pickupWinnerPeerHash != GetLocalAccountToken() ||
        record.pickupDeliveryInProgress || m_sharedDropApplyDepth != 0 ||
        m_sharedDropHostSessionId == 0 || !m_enablePlayerSidecar || !inventory ||
        record.count <= 0 || record.pickupTransactionSequence == 0 || record.version == 0 ||
        m_pendingPlayerSidecarInventoryRestore || m_pendingPlayerSidecarChipsetRestore ||
        m_playerSidecarInventoryPending > 0 || m_playerSidecarChipsetsPending > 0 ||
        m_pendingReceivedPlayerStateApply || m_clientAwaitingHostPlayerState)
    {
        return false;
    }

    auto delivery = FindDelivery(m_localSharedDropDeliveries, m_sharedDropHostSessionId, record.stableSpawnId);
    if (delivery == m_localSharedDropDeliveries.end())
    {
        if (m_localSharedDropDeliveries.size() >= CoopSharedDropDelivery::kMaxEntries)
            return false;
        CoopSharedDropDelivery::Entry entry;
        entry.hostSessionId = m_sharedDropHostSessionId;
        entry.stableSpawnId = record.stableSpawnId;
        entry.archetypeId = record.archetypeId;
        entry.count = static_cast<uint32_t>(std::max(1, record.count));
        entry.transactionSequence = record.pickupTransactionSequence;
        entry.objectVersion = record.version;
        m_localSharedDropDeliveries.push_back(entry);
        delivery = std::prev(m_localSharedDropDeliveries.end());
        record.pickupDeliveryStored = false;
    }
    if (delivery->archetypeId != record.archetypeId ||
        delivery->count != static_cast<uint32_t>(std::max(1, record.count)) ||
        delivery->transactionSequence != record.pickupTransactionSequence || delivery->objectVersion != record.version ||
        delivery->deliveredCount > delivery->count)
    {
        m_lastSharedDropEvent = "delivery_receipt_mismatch_id_" + std::to_string(record.stableSpawnId);
        return false;
    }

    if (delivery->deliveredCount == delivery->count)
    {
        record.localPickupGranted = true;
        record.pickupResultSucceeded = true;
        return record.pickupDeliveryStored || PersistLocalSharedDropDelivery(record);
    }

    // Persist the undelivered intent with its inventory baseline before allowing
    // native mutation. A crash before the next snapshot restores this baseline.
    if (!record.pickupDeliveryStored && !PersistLocalSharedDropDelivery(record))
        return false;

    const EntityId entityId = record.localEntityId;
    CArkItem* item = entityId != INVALID_ENTITYID ? CArkItem::GetItemFromEntityId(entityId) : nullptr;
    int before = 0;
    if (!item || !ReadInventoryCount(*inventory, record.archetypeId, before))
        return false;

    if (!interactive)
    {
        bool canCollect = false;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "shared drop delivery CanCollect",
                [item, inventory]() { return item->CanCollect(*inventory); },
                canCollect,
                nullptr) || !canCollect)
        {
            return false;
        }
    }

    const uint32_t remaining = delivery->count - delivery->deliveredCount;
    bool nativeResult = false;
    std::string reason;
    record.pickupDeliveryInProgress = true;
    record.nativePickupInProgress = true;
    ++m_sharedDropApplyDepth;
    const bool prepared = CoopRuntimeGuards::TryGuardedVoidCall(
        "shared drop delivery remainder",
        [item, remaining]()
        {
            item->ResetCount(static_cast<int>(remaining));
            if (IEntity* entity = item->GetEntity())
            {
                entity->Hide(false);
                entity->Invisible(false);
                entity->EnablePhysics(true);
            }
        },
        &reason);
    if (prepared)
    {
        record.pickupDeliveryAttempted = true;
        CoopRuntimeGuards::TryGuardedCall(
            "shared drop committed native PickUp",
            [item, player]() { return item->PickUp(player->GetEntityId(), false); },
            nativeResult,
            &reason);
    }
    --m_sharedDropApplyDepth;
    record.nativePickupInProgress = false;
    record.pickupDeliveryInProgress = false;

    int after = before;
    if (!ReadInventoryCount(*inventory, record.archetypeId, after))
    {
        record.pickupDeliveryStored = false;
        record.pickupDeliveryInProgress = true;
        m_sharedDropDeliveryRecoveryRequired = true;
        m_lastSharedDropEvent = "delivery_count_unavailable_id_" + std::to_string(record.stableSpawnId);
        return false;
    }
    const uint32_t acquired = static_cast<uint32_t>(std::clamp(after - before, 0, static_cast<int>(remaining)));
    if (acquired != 0)
    {
        // Native overflow can return false after transferring part of a stack.
        // The inventory delta, not that return value, is the delivery receipt.
        delivery->deliveredCount += acquired;
        record.pickupDeliveryStored = false;
        EnsureLocalPlayerWeaponRegistered(entityId, "shared drop committed delivery");
    }

    const bool complete = delivery->deliveredCount == delivery->count;
    record.localPickupGranted = complete;
    record.pickupResultSucceeded = complete;
    record.live = false;
    if (complete)
    {
        if (!FinishDeliveryPresentation(entityId))
        {
            m_sharedDropByEntityId.erase(entityId);
            record.localEntityId = INVALID_ENTITYID;
        }
    }
    // A measured native delta is retained in memory and retried on storage
    // failure. Only an unknown native outcome requires quarantine.
    if (acquired != 0)
        PersistLocalSharedDropDelivery(record);
    m_lastSharedDropEvent = "committed_delivery_id_" + std::to_string(record.stableSpawnId) +
        "_received_" + std::to_string(delivery->deliveredCount) +
        "_count_" + std::to_string(delivery->count) +
        "_stored_" + std::to_string(record.pickupDeliveryStored ? 1 : 0) +
        "_native_" + std::to_string(nativeResult ? 1 : 0);
    return complete && record.pickupDeliveryStored;
}

uint64_t ModMain::BeginCommittedSharedDropInventoryTransfer(CArkItem* item, IArkInventory* target, int& countBefore)
{
    countBefore = -1;
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    ArkInventory* inventory = player ? player->m_pInventory : nullptr;
    if (m_sharedDropDeliveryRecoveryRequired || !item || !inventory ||
        target != static_cast<IArkInventory*>(inventory) || m_sharedDropApplyDepth != 0)
        return 0;

    EntityId entityId = INVALID_ENTITYID;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "shared drop overflow entity id", [item]() { return item->GetEntityId(); }, entityId, nullptr))
        return 0;
    const auto mapping = m_sharedDropByEntityId.find(entityId);
    if (mapping == m_sharedDropByEntityId.end())
        return 0;
    const auto recordIt = m_sharedDrops.find(mapping->second);
    if (recordIt == m_sharedDrops.end())
        return 0;
    SharedDropRecord& record = recordIt->second;
    auto delivery = FindDelivery(m_localSharedDropDeliveries, m_sharedDropHostSessionId, record.stableSpawnId);
    if (!record.pickupCommitted || record.pickupWinnerPeerHash != GetLocalAccountToken() ||
        record.pickupDeliveryInProgress || delivery == m_localSharedDropDeliveries.end() ||
        delivery->archetypeId != record.archetypeId || delivery->transactionSequence != record.pickupTransactionSequence ||
        delivery->objectVersion != record.version ||
        delivery->deliveredCount >= delivery->count)
        return 0;
    if (!ReadInventoryCount(*inventory, record.archetypeId, countBefore))
    {
        // The native UI transfer still runs. Without its baseline, discard this
        // private world rather than saving inventory with an unchanged receipt.
        record.pickupDeliveryStored = false;
        m_sharedDropDeliveryRecoveryRequired = true;
        return 0;
    }
    record.pickupDeliveryInProgress = true;
    record.nativePickupInProgress = true;
    return record.stableSpawnId;
}

void ModMain::EndCommittedSharedDropInventoryTransfer(uint64_t stableSpawnId, int countBefore, IArkInventory* target)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    ArkInventory* inventory = player ? player->m_pInventory : nullptr;
    if (stableSpawnId == 0 || countBefore < 0 || !inventory ||
        target != static_cast<IArkInventory*>(inventory) || m_sharedDropApplyDepth != 0)
        return;
    const auto recordIt = m_sharedDrops.find(stableSpawnId);
    if (recordIt == m_sharedDrops.end())
        return;
    SharedDropRecord& record = recordIt->second;
    if (!record.pickupDeliveryInProgress)
        return;
    auto delivery = FindDelivery(m_localSharedDropDeliveries, m_sharedDropHostSessionId, stableSpawnId);
    int after = countBefore;
    record.nativePickupInProgress = false;
    if (!record.pickupCommitted || record.pickupWinnerPeerHash != GetLocalAccountToken() ||
        delivery == m_localSharedDropDeliveries.end() || delivery->deliveredCount >= delivery->count ||
        delivery->archetypeId != record.archetypeId || delivery->transactionSequence != record.pickupTransactionSequence ||
        delivery->objectVersion != record.version ||
        !ReadInventoryCount(*inventory, record.archetypeId, after))
    {
        record.pickupDeliveryStored = false;
        m_sharedDropDeliveryRecoveryRequired = true;
        return;
    }
    record.pickupDeliveryInProgress = false;
    const uint32_t remaining = delivery->count - delivery->deliveredCount;
    const uint32_t acquired = static_cast<uint32_t>(std::clamp(after - countBefore, 0, static_cast<int>(remaining)));
    if (acquired == 0)
        return;

    delivery->deliveredCount += acquired;
    record.pickupDeliveryStored = false;
    record.localPickupGranted = delivery->deliveredCount == delivery->count;
    record.pickupResultSucceeded = record.localPickupGranted;
    record.live = false;
    EnsureLocalPlayerWeaponRegistered(record.localEntityId, "shared drop overflow delivery");
    if (record.localPickupGranted && !FinishDeliveryPresentation(record.localEntityId))
    {
        m_sharedDropByEntityId.erase(record.localEntityId);
        record.localEntityId = INVALID_ENTITYID;
    }
    PersistLocalSharedDropDelivery(record);
    m_lastSharedDropEvent = "overflow_delivery_id_" + std::to_string(stableSpawnId) +
        "_received_" + std::to_string(delivery->deliveredCount) +
        "_count_" + std::to_string(delivery->count) +
        "_stored_" + std::to_string(record.pickupDeliveryStored ? 1 : 0);
}

void ModMain::ProcessSharedDropDeliveryRecoveryFault()
{
    if (!m_sharedDropDeliveryRecoveryRequired || m_networkMode == CoopNetworkMode::Off)
        return;

    // Run only outside native delivery and packet-handler stacks. The wire
    // fence remains armed until LEVEL_POST_UNLOAD discards this private world.
    constexpr const char* reason = "inventory receipt could not be saved";
    if (m_networkMode == CoopNetworkMode::Client)
        DisconnectRemotePeer(reason);
    else
        StopNetwork();
    m_multiplayerRestartRequired = true;
    m_networkStatus = "inventory storage failed; reload the world before reconnecting";
    m_sessionStatus = "inventory recovery required";
    QueueCoopHudFeedback("INVENTORY STORAGE FAILED - RELOAD BEFORE RECONNECTING", 6.0f);
    CoopRuntimeLog::Write(m_networkStatus);
}
