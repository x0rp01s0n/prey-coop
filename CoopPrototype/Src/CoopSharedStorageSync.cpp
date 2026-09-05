#include "ModMain.h"
#include "CoopRuntimeGuards.h"
#include "CoopSerialSequence.h"

#include <algorithm>
#include <limits>

#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/ark/ui/arkexternalinventoryui.h>

namespace
{
constexpr uint16_t kSharedStorageFlagFinalCommit = 1u << 0;
constexpr float kSharedStorageRemoteLeaseTimeoutSeconds = 120.0f;

bool IsSnapshotCommand(CoopProtocol::SharedStorageCommand command)
{
    return command == CoopProtocol::SharedStorageCommand::SnapshotBegin ||
        command == CoopProtocol::SharedStorageCommand::SnapshotItem ||
        command == CoopProtocol::SharedStorageCommand::SnapshotEnd;
}

bool IsCommitCommand(CoopProtocol::SharedStorageCommand command)
{
    return command == CoopProtocol::SharedStorageCommand::CommitBegin ||
        command == CoopProtocol::SharedStorageCommand::CommitItem ||
        command == CoopProtocol::SharedStorageCommand::CommitEnd;
}
}

ArkInventory* ModMain::ResolveSharedStorageInventory(uint64_t guid, EntityId* outEntityId, std::string& detail) const
{
    if (outEntityId)
        *outEntityId = INVALID_ENTITYID;
    if (guid == 0 || !gEnv || !gEnv->pEntitySystem || !gEnv->pGame)
    {
        detail = "shared_storage_unavailable";
        return nullptr;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    IGameFramework* framework = nullptr;
    IGameObject* gameObject = nullptr;
    IGameObjectExtension* extension = nullptr;
    IGameObjectSystem::ExtensionID extensionId = 0;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "shared storage FindEntityByGuid",
            [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(guid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage GetFramework",
            []() { return gEnv->pGame->GetIGameFramework(); },
            framework,
            &reason) ||
        !framework ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage GetGameObject",
            [framework, entityId]() { return framework->GetGameObject(entityId); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        detail = "shared_storage_resolve_failed_" + reason;
        return nullptr;
    }

    if (!CoopRuntimeGuards::TryGuardedCall(
            "shared storage GetExtensionId",
            [gameObject]() { return gameObject->GetExtensionId("ArkInventory"); },
            extensionId,
            &reason) ||
        extensionId == 0 ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) ||
        !extension)
    {
        extension = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "shared storage QueryExtension name",
                [gameObject]() { return gameObject->QueryExtension("ArkInventory"); },
                extension,
                &reason) ||
            !extension)
        {
            detail = "shared_storage_extension_failed_" + reason;
            return nullptr;
        }
    }

    if (outEntityId)
        *outEntityId = entityId;
    detail = "shared_storage_resolved_entity_" + std::to_string(entityId);
    return static_cast<ArkInventory*>(extension);
}

bool ModMain::IsSharedStorageInventory(const ArkInventory* inventory, uint64_t* outGuid) const
{
    if (outGuid)
        *outGuid = 0;
    if (!inventory || !gEnv || !gEnv->pEntitySystem)
        return false;

    unsigned ownerId = INVALID_ENTITYID;
    IEntity* owner = nullptr;
    uint64_t guid = 0;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "shared storage inventory GetOwnerId",
            [inventory]() { return inventory->GetOwnerId(); },
            ownerId,
            &reason) ||
        ownerId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage inventory GetEntity",
            [ownerId]() { return gEnv->pEntitySystem->GetEntity(ownerId); },
            owner,
            &reason) ||
        !owner ||
        !CoopRuntimeGuards::TryGuardedCall(
            "shared storage inventory GetGuid",
            [owner]() { return owner->GetGuid(); },
            guid,
            &reason) ||
        guid == 0)
    {
        return false;
    }

    if (outGuid)
        *outGuid = guid;
    const auto found = m_sharedStorages.find(guid);
    return found != m_sharedStorages.end() && found->second.registered;
}

bool ModMain::BuildSharedStoragePacket(
    CoopProtocol::SharedStoragePacket& packet,
    CoopProtocol::SharedStorageCommand command,
    const SharedStorageRecord& record,
    uint64_t targetPeerHash) const
{
    if (!record.registered || record.guid == 0 || record.areaId == 0 || record.version == 0)
        return false;

    packet = {};
    packet.sequence = m_sharedStorageSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.storageVersion = record.version;
    packet.leaseEpoch = record.leaseEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = record.areaId;
    packet.storageGuid = record.guid;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = targetPeerHash;
    packet.command = static_cast<uint16_t>(command);
    return true;
}

bool ModMain::SendSharedStorageTo(
    const CoopProtocol::SharedStoragePacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::SharedStorage),
            &packet,
            sizeof(packet),
            address,
            port,
            failurePrefix))
    {
        return false;
    }
    ++m_sharedStorageSent;
    return true;
}

bool ModMain::RegisterSharedStorage(uint64_t guid, bool broadcast, const char* reason)
{
    EntityId entityId = INVALID_ENTITYID;
    std::string detail;
    ArkInventory* inventory = ResolveSharedStorageInventory(guid, &entityId, detail);
    if (!inventory)
    {
        ++m_sharedStorageDropped;
        m_lastSharedStorageEvent = "register_failed_guid_" + std::to_string(guid) + "_" + detail;
        return false;
    }

    SharedStorageRecord& record = m_sharedStorages[guid];
    record.guid = guid;
    record.areaId = m_localLevelId;
    record.localEntityId = entityId;
    record.version = std::max(1u, record.version);
    record.registered = true;

    if (broadcast && m_hasRemoteEndpoint && IsSessionGameplayReady())
    {
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        CoopProtocol::SharedStoragePacket packet;
        if (!BuildSharedStoragePacket(packet, CoopProtocol::SharedStorageCommand::Register, record) ||
            !SendSharedStorageTo(packet, m_remoteAddress, m_remotePort, "shared storage register failed"))
        {
            ++m_sharedStorageDropped;
            return false;
        }
    }

    m_lastSharedStorageEvent =
        "registered_guid_" + std::to_string(guid) +
        "_entity_" + std::to_string(entityId) +
        "_reason_" + (reason && reason[0] ? reason : "-");
    return true;
}

bool ModMain::SendSharedStorageSnapshot(
    SharedStorageRecord& record,
    ArkInventory& inventory,
    bool commit,
    bool releaseAfterCommit,
    uint64_t targetPeerHash,
    const char* reason)
{
    std::vector<PlayerInventoryItemState> items;
    std::string detail;
    if (!CaptureSharedStorageInventory(inventory, items, detail) ||
        items.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
    {
        ++m_sharedStorageDropped;
        m_lastSharedStorageEvent = "snapshot_capture_failed_guid_" + std::to_string(record.guid) + "_" + detail;
        return false;
    }

    if (commit)
        ++record.version;
    const uint32_t transaction = ++m_sharedStorageTransaction;
    const auto beginCommand = commit
        ? CoopProtocol::SharedStorageCommand::CommitBegin
        : CoopProtocol::SharedStorageCommand::SnapshotBegin;
    const auto itemCommand = commit
        ? CoopProtocol::SharedStorageCommand::CommitItem
        : CoopProtocol::SharedStorageCommand::SnapshotItem;
    const auto endCommand = commit
        ? CoopProtocol::SharedStorageCommand::CommitEnd
        : CoopProtocol::SharedStorageCommand::SnapshotEnd;

    auto sendPacket = [&](CoopProtocol::SharedStoragePacket& packet, const char* failure) -> bool
    {
        packet.transactionId = transaction;
        packet.itemTotal = static_cast<uint16_t>(items.size());
        return SendSharedStorageTo(packet, m_remoteAddress, m_remotePort, failure);
    };

    CoopSerialSequence::Advance(m_sharedStorageSequence);
    CoopProtocol::SharedStoragePacket begin;
    if (!BuildSharedStoragePacket(begin, beginCommand, record, targetPeerHash) ||
        !sendPacket(begin, "shared storage begin failed"))
    {
        ++m_sharedStorageDropped;
        return false;
    }

    for (size_t index = 0; index < items.size(); ++index)
    {
        const PlayerInventoryItemState& item = items[index];
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        CoopProtocol::SharedStoragePacket packet;
        if (!BuildSharedStoragePacket(packet, itemCommand, record, targetPeerHash))
        {
            ++m_sharedStorageDropped;
            return false;
        }
        packet.itemIndex = static_cast<uint16_t>(index);
        packet.archetypeId = item.archetypeId;
        packet.itemFlags = item.flags;
        packet.count = item.count;
        packet.x = static_cast<int16_t>(std::clamp(item.x, -1, 32767));
        packet.y = static_cast<int16_t>(std::clamp(item.y, -1, 32767));
        packet.width = static_cast<int16_t>(std::clamp(item.width, -1, 32767));
        packet.height = static_cast<int16_t>(std::clamp(item.height, -1, 32767));
        packet.category = static_cast<int16_t>(std::clamp(item.category, -1, 32767));
        packet.weaponCondition = item.weaponCondition;
        packet.weaponAmmoLoaded = static_cast<int16_t>(std::clamp(item.weaponAmmoLoaded, -32768, 32767));
        packet.weaponAmmoCount = static_cast<int16_t>(std::clamp(item.weaponAmmoCount, -32768, 32767));
        packet.weaponModCount = static_cast<uint16_t>(
            std::min(item.weaponMods.size(), CoopProtocol::kSharedStorageMaxWeaponMods));
        for (size_t modIndex = 0; modIndex < packet.weaponModCount; ++modIndex)
        {
            packet.weaponModIds[modIndex] = item.weaponMods[modIndex].first;
            packet.weaponModLevels[modIndex] = static_cast<int16_t>(
                std::clamp(item.weaponMods[modIndex].second, 0, 16));
        }
        if (!sendPacket(packet, "shared storage item failed"))
        {
            ++m_sharedStorageDropped;
            return false;
        }
    }

    CoopSerialSequence::Advance(m_sharedStorageSequence);
    CoopProtocol::SharedStoragePacket end;
    if (!BuildSharedStoragePacket(end, endCommand, record, targetPeerHash))
        return false;
    if (releaseAfterCommit)
        end.flags |= kSharedStorageFlagFinalCommit;
    if (!sendPacket(end, "shared storage end failed"))
    {
        ++m_sharedStorageDropped;
        return false;
    }

    m_lastSharedStorageEvent =
        std::string(commit ? "sent_commit" : "sent_snapshot") +
        "_guid_" + std::to_string(record.guid) +
        "_tx_" + std::to_string(transaction) +
        "_version_" + std::to_string(record.version) +
        "_items_" + std::to_string(items.size()) +
        "_final_" + std::to_string(releaseAfterCommit ? 1 : 0) +
        "_reason_" + (reason && reason[0] ? reason : "-");
    return true;
}

bool ModMain::ShouldDeferSharedStorageOpen(
    CArkExternalInventoryUI* ui,
    ArkInventory* inventory,
    const char* reason)
{
    uint64_t guid = 0;
    if (!inventory || m_sharedStorageApplyDepth != 0)
        return false;

    bool takesTrash = false;
    std::string inventoryReason;
    if (CoopRuntimeGuards::TryGuardedCall(
            "shared storage inventory GetTakesTrash",
            [inventory]() { return inventory->GetTakesTrash(); },
            takesTrash,
            &inventoryReason) &&
        takesTrash)
    {
        // Recycler input is a PlayerOwned staging inventory. Prey expects its
        // PDA open to finish synchronously; deferring that call behind a
        // SharedStorage lease leaves the machine/PDA transition locked. Its
        // material output is deliberately local per the Recycler contract.
        m_lastSharedStorageEvent = "bypass_player_owned_recycler_input";
        return false;
    }

    const bool registered = IsSharedStorageInventory(inventory, &guid);
    if (!registered)
    {
        if (guid == 0 || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
            return false;
        if (!RegisterSharedStorage(guid, true, "external inventory lazy register"))
        {
            ++m_sharedStorageDenied;
            m_lastSharedStorageEvent =
                "open_denied_registration_failed_guid_" + std::to_string(guid);
            return true;
        }
    }

    SharedStorageRecord& record = m_sharedStorages[guid];
    const uint64_t localPeerHash = GetLocalAccountToken();
    if (m_networkMode == CoopNetworkMode::Host)
    {
        if (record.leaseOwnerHash == 0 || record.leaseOwnerHash == localPeerHash)
        {
            if (record.leaseOwnerHash == 0)
                ++record.leaseEpoch;
            record.leaseOwnerHash = localPeerHash;
            record.leaseSeconds = 0.0f;
            m_activeSharedStorageUi = ui;
            m_activeSharedStorageInventory = inventory;
            m_activeSharedStorageGuid = guid;
            m_lastSharedStorageEvent = "host_open_granted_guid_" + std::to_string(guid);
            return false;
        }

        ++m_sharedStorageDenied;
        m_lastSharedStorageEvent = "host_open_denied_busy_guid_" + std::to_string(guid);
        return true;
    }

    if (m_networkMode != CoopNetworkMode::Client || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
        return false;
    if (record.leaseOwnerHash == localPeerHash && record.localGrantReceived)
    {
        m_activeSharedStorageUi = ui;
        m_activeSharedStorageInventory = inventory;
        m_activeSharedStorageGuid = guid;
        return false;
    }

    m_pendingSharedStorageUi = ui;
    m_pendingSharedStorageInventory = inventory;
    CoopSerialSequence::Advance(m_sharedStorageSequence);
    CoopProtocol::SharedStoragePacket packet;
    if (!BuildSharedStoragePacket(packet, CoopProtocol::SharedStorageCommand::OpenRequest, record) ||
        !SendSharedStorageTo(packet, m_remoteAddress, m_remotePort, "shared storage open request failed"))
    {
        ++m_sharedStorageDropped;
    }
    else
    {
        m_lastSharedStorageEvent =
            "open_requested_guid_" + std::to_string(guid) +
            "_reason_" + (reason && reason[0] ? reason : "-");
    }
    return true;
}

void ModMain::OnSharedStorageTransfer(CArkItem*, IArkInventory* source, IArkInventory* target, const char* reason)
{
    if (m_sharedStorageApplyDepth != 0 || !m_activeSharedStorageInventory ||
        (source != m_activeSharedStorageInventory && target != m_activeSharedStorageInventory))
    {
        return;
    }

    const auto found = m_sharedStorages.find(m_activeSharedStorageGuid);
    if (found == m_sharedStorages.end() ||
        found->second.leaseOwnerHash != GetLocalAccountToken())
    {
        return;
    }

    SharedStorageRecord& record = found->second;
    record.leaseSeconds = 0.0f;
    if (m_networkMode == CoopNetworkMode::Client)
        SendSharedStorageSnapshot(record, *m_activeSharedStorageInventory, true, false, 0, reason);
    else
    {
        ++record.version;
        m_lastSharedStorageEvent = "host_transfer_committed_guid_" + std::to_string(record.guid) +
            "_version_" + std::to_string(record.version);
    }
}

void ModMain::OnSharedStorageClosed(ArkInventory* inventory, const char* reason)
{
    uint64_t guid = 0;
    if (m_sharedStorageApplyDepth != 0 || !IsSharedStorageInventory(inventory, &guid) ||
        guid != m_activeSharedStorageGuid)
    {
        return;
    }

    auto found = m_sharedStorages.find(guid);
    if (found == m_sharedStorages.end() ||
        found->second.leaseOwnerHash != GetLocalAccountToken())
    {
        return;
    }

    SharedStorageRecord& record = found->second;
    if (m_networkMode == CoopNetworkMode::Client)
        SendSharedStorageSnapshot(record, *inventory, true, true, 0, reason);
    else
    {
        ++record.version;
        record.leaseOwnerHash = 0;
        record.leaseSeconds = 0.0f;
    }
    m_activeSharedStorageUi = nullptr;
    m_activeSharedStorageInventory = nullptr;
    m_activeSharedStorageGuid = 0;
}

bool ModMain::TryFinalizeSharedStorageAssembly(uint64_t guid, std::string& detail)
{
    auto assemblyIt = m_sharedStorageAssemblies.find(guid);
    auto recordIt = m_sharedStorages.find(guid);
    if (assemblyIt == m_sharedStorageAssemblies.end() || recordIt == m_sharedStorages.end())
        return false;
    SharedStorageAssembly& assembly = assemblyIt->second;
    SharedStorageRecord& record = recordIt->second;
    if (!assembly.endReceived || assembly.received.size() != assembly.expectedItems ||
        std::find(assembly.received.begin(), assembly.received.end(), false) != assembly.received.end())
    {
        detail = "assembly_waiting";
        return false;
    }

    if (assembly.commit)
    {
        if (m_networkMode != CoopNetworkMode::Host ||
            record.leaseOwnerHash == 0 || record.leaseOwnerHash != assembly.sourcePeerHash ||
            assembly.leaseEpoch != record.leaseEpoch || assembly.storageVersion <= record.version)
        {
            detail = "commit_stale_or_not_owner";
            m_sharedStorageAssemblies.erase(assemblyIt);
            return false;
        }
    }
    else if (m_networkMode != CoopNetworkMode::Client)
    {
        detail = "snapshot_wrong_role";
        m_sharedStorageAssemblies.erase(assemblyIt);
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    ArkInventory* inventory = ResolveSharedStorageInventory(guid, &entityId, detail);
    if (!inventory)
    {
        m_sharedStorageAssemblies.erase(assemblyIt);
        return false;
    }

    ++m_sharedStorageApplyDepth;
    const bool applied = ReplaceSharedStorageInventory(*inventory, assembly.items, detail);
    --m_sharedStorageApplyDepth;
    if (!applied)
    {
        m_sharedStorageAssemblies.erase(assemblyIt);
        return false;
    }

    record.localEntityId = entityId;
    record.version = assembly.storageVersion;
    record.leaseSeconds = 0.0f;
    const uint32_t transaction = assembly.transactionId;
    const uint16_t expectedItems = assembly.expectedItems;
    const bool commit = assembly.commit;
    m_sharedStorageAssemblies.erase(assemblyIt);
    ++m_sharedStorageApplied;

    if (!commit && record.localGrantReceived && record.localGrantTransaction == transaction &&
        m_pendingSharedStorageUi && m_pendingSharedStorageInventory == inventory)
    {
        record.leaseOwnerHash = GetLocalAccountToken();
        m_activeSharedStorageUi = m_pendingSharedStorageUi;
        m_activeSharedStorageInventory = inventory;
        m_activeSharedStorageGuid = guid;
        CArkExternalInventoryUI* pendingUi = m_pendingSharedStorageUi;
        m_pendingSharedStorageUi = nullptr;
        m_pendingSharedStorageInventory = nullptr;
        pendingUi->Open(*inventory);
    }
    detail = std::string(commit ? "commit_applied" : "snapshot_applied") +
        "_guid_" + std::to_string(guid) +
        "_tx_" + std::to_string(transaction) +
        "_items_" + std::to_string(expectedItems);
    return true;
}

void ModMain::HandleSharedStorage(const CoopProtocol::SharedStoragePacket& packet)
{
    ++m_sharedStorageReceived;
    const auto command = static_cast<CoopProtocol::SharedStorageCommand>(packet.command);
    const uint64_t localPeerHash = GetLocalAccountToken();
    if (packet.storageGuid == 0 || packet.areaId != m_localLevelId ||
        packet.worldEpoch != m_localWorldEpoch ||
        packet.hostSaveKeyHash == 0 ||
        packet.hostSaveKeyHash != CurrentHostSaveKeyHash() ||
        (packet.targetPeerHash != 0 && packet.targetPeerHash != localPeerHash))
    {
        ++m_sharedStorageDropped;
        m_lastSharedStorageEvent = "packet_guard_drop_command_" + std::to_string(packet.command);
        return;
    }

    if (command == CoopProtocol::SharedStorageCommand::Register)
    {
        RegisterSharedStorage(packet.storageGuid, false, "network register");
        return;
    }

    auto recordIt = m_sharedStorages.find(packet.storageGuid);
    if (recordIt == m_sharedStorages.end() || !recordIt->second.registered)
    {
        ++m_sharedStorageDropped;
        m_lastSharedStorageEvent = "unknown_storage_guid_" + std::to_string(packet.storageGuid);
        return;
    }
    SharedStorageRecord& record = recordIt->second;

    if (command == CoopProtocol::SharedStorageCommand::OpenRequest)
    {
        if (m_networkMode != CoopNetworkMode::Host)
            return;
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        CoopProtocol::SharedStoragePacket response;
        if (record.leaseOwnerHash != 0 && record.leaseOwnerHash != packet.sourcePeerHash)
        {
            BuildSharedStoragePacket(response, CoopProtocol::SharedStorageCommand::OpenDeny, record, packet.sourcePeerHash);
            SendSharedStorageTo(response, m_remoteAddress, m_remotePort, "shared storage deny failed");
            ++m_sharedStorageDenied;
            return;
        }

        record.leaseOwnerHash = packet.sourcePeerHash;
        record.leaseSeconds = 0.0f;
        ++record.leaseEpoch;
        EntityId entityId = INVALID_ENTITYID;
        std::string detail;
        ArkInventory* inventory = ResolveSharedStorageInventory(record.guid, &entityId, detail);
        if (!inventory || !SendSharedStorageSnapshot(record, *inventory, false, false, packet.sourcePeerHash, "open request"))
        {
            record.leaseOwnerHash = 0;
            ++m_sharedStorageDropped;
            return;
        }
        const uint32_t transaction = m_sharedStorageTransaction;
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        BuildSharedStoragePacket(response, CoopProtocol::SharedStorageCommand::OpenGrant, record, packet.sourcePeerHash);
        response.transactionId = transaction;
        SendSharedStorageTo(response, m_remoteAddress, m_remotePort, "shared storage grant failed");
        return;
    }

    if (command == CoopProtocol::SharedStorageCommand::OpenGrant)
    {
        if (m_networkMode != CoopNetworkMode::Client)
            return;
        record.leaseOwnerHash = localPeerHash;
        record.leaseEpoch = packet.leaseEpoch;
        record.localGrantReceived = true;
        record.localGrantTransaction = packet.transactionId;
        std::string detail;
        TryFinalizeSharedStorageAssembly(record.guid, detail);
        m_lastSharedStorageEvent = "open_grant_guid_" + std::to_string(record.guid) + "_" + detail;
        return;
    }

    if (command == CoopProtocol::SharedStorageCommand::OpenDeny)
    {
        ++m_sharedStorageDenied;
        m_pendingSharedStorageUi = nullptr;
        m_pendingSharedStorageInventory = nullptr;
        m_lastSharedStorageEvent = "open_denied_guid_" + std::to_string(record.guid);
        return;
    }

    if (command == CoopProtocol::SharedStorageCommand::Release)
    {
        record.version = std::max(record.version, packet.storageVersion);
        record.leaseOwnerHash = 0;
        record.localGrantReceived = false;
        record.localGrantTransaction = 0;
        record.leaseSeconds = 0.0f;
        if (m_activeSharedStorageGuid == record.guid)
        {
            m_activeSharedStorageUi = nullptr;
            m_activeSharedStorageInventory = nullptr;
            m_activeSharedStorageGuid = 0;
        }
        m_lastSharedStorageEvent = "released_guid_" + std::to_string(record.guid);
        return;
    }

    if (!IsSnapshotCommand(command) && !IsCommitCommand(command))
    {
        ++m_sharedStorageDropped;
        return;
    }

    const bool commit = IsCommitCommand(command);
    const bool begin = command == CoopProtocol::SharedStorageCommand::SnapshotBegin ||
        command == CoopProtocol::SharedStorageCommand::CommitBegin;
    const bool item = command == CoopProtocol::SharedStorageCommand::SnapshotItem ||
        command == CoopProtocol::SharedStorageCommand::CommitItem;
    const bool end = command == CoopProtocol::SharedStorageCommand::SnapshotEnd ||
        command == CoopProtocol::SharedStorageCommand::CommitEnd;

    if (begin)
    {
        SharedStorageAssembly assembly;
        assembly.guid = packet.storageGuid;
        assembly.sourcePeerHash = packet.sourcePeerHash;
        assembly.transactionId = packet.transactionId;
        assembly.storageVersion = packet.storageVersion;
        assembly.leaseEpoch = packet.leaseEpoch;
        assembly.expectedItems = packet.itemTotal;
        assembly.commit = commit;
        assembly.items.resize(packet.itemTotal);
        assembly.received.assign(packet.itemTotal, false);
        m_sharedStorageAssemblies[packet.storageGuid] = std::move(assembly);
        return;
    }

    auto assemblyIt = m_sharedStorageAssemblies.find(packet.storageGuid);
    if (assemblyIt == m_sharedStorageAssemblies.end() ||
        assemblyIt->second.transactionId != packet.transactionId ||
        assemblyIt->second.commit != commit)
    {
        ++m_sharedStorageDropped;
        return;
    }
    SharedStorageAssembly& assembly = assemblyIt->second;
    if (item)
    {
        if (packet.itemIndex >= assembly.expectedItems || packet.archetypeId == 0 || packet.count <= 0)
        {
            ++m_sharedStorageDropped;
            return;
        }
        PlayerInventoryItemState& state = assembly.items[packet.itemIndex];
        state.archetypeId = packet.archetypeId;
        state.count = packet.count;
        state.x = packet.x;
        state.y = packet.y;
        state.width = packet.width;
        state.height = packet.height;
        state.category = packet.category;
        state.flags = packet.itemFlags;
        state.isWeapon = (packet.itemFlags & (1u << 4)) != 0;
        state.weaponCondition = packet.weaponCondition;
        state.weaponAmmoLoaded = packet.weaponAmmoLoaded;
        state.weaponAmmoCount = packet.weaponAmmoCount;
        for (size_t index = 0; index < std::min<size_t>(packet.weaponModCount, CoopProtocol::kSharedStorageMaxWeaponMods); ++index)
        {
            if (packet.weaponModIds[index] != 0)
                state.weaponMods.emplace_back(packet.weaponModIds[index], packet.weaponModLevels[index]);
        }
        state.weaponModCount = static_cast<uint32_t>(state.weaponMods.size());
        assembly.received[packet.itemIndex] = true;
    }
    if (end)
        assembly.endReceived = true;

    std::string detail;
    if (TryFinalizeSharedStorageAssembly(packet.storageGuid, detail) && commit && m_networkMode == CoopNetworkMode::Host)
    {
        const bool finalCommit = (packet.flags & kSharedStorageFlagFinalCommit) != 0;
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        CoopProtocol::SharedStoragePacket response;
        BuildSharedStoragePacket(
            response,
            finalCommit ? CoopProtocol::SharedStorageCommand::Release : CoopProtocol::SharedStorageCommand::OpenGrant,
            record,
            packet.sourcePeerHash);
        response.transactionId = packet.transactionId;
        SendSharedStorageTo(response, m_remoteAddress, m_remotePort, "shared storage commit ack failed");
        if (finalCommit)
            record.leaseOwnerHash = 0;
    }
    m_lastSharedStorageEvent = detail;
}

void ModMain::TickSharedStorage(float frameTime)
{
    if (m_networkMode != CoopNetworkMode::Host || frameTime <= 0.0f)
        return;
    const uint64_t localPeerHash = GetLocalAccountToken();
    for (auto& pair : m_sharedStorages)
    {
        SharedStorageRecord& record = pair.second;
        if (record.leaseOwnerHash == 0 || record.leaseOwnerHash == localPeerHash)
            continue;
        record.leaseSeconds += frameTime;
        if (record.leaseSeconds < kSharedStorageRemoteLeaseTimeoutSeconds)
            continue;

        const uint64_t expiredOwner = record.leaseOwnerHash;
        record.leaseOwnerHash = 0;
        record.leaseSeconds = 0.0f;
        CoopSerialSequence::Advance(m_sharedStorageSequence);
        CoopProtocol::SharedStoragePacket packet;
        if (BuildSharedStoragePacket(packet, CoopProtocol::SharedStorageCommand::Release, record, expiredOwner))
            SendSharedStorageTo(packet, m_remoteAddress, m_remotePort, "shared storage timeout release failed");
        m_lastSharedStorageEvent = "lease_timeout_guid_" + std::to_string(record.guid);
    }
}

void ModMain::ResetSharedStorageState(const char* reason)
{
    m_sharedStorages.clear();
    m_sharedStorageAssemblies.clear();
    m_pendingSharedStorageUi = nullptr;
    m_pendingSharedStorageInventory = nullptr;
    m_activeSharedStorageUi = nullptr;
    m_activeSharedStorageInventory = nullptr;
    m_activeSharedStorageGuid = 0;
    m_sharedStorageSequence = 0;
    m_sharedStorageTransaction = 0;
    m_sharedStorageSent = 0;
    m_sharedStorageReceived = 0;
    m_sharedStorageApplied = 0;
    m_sharedStorageDropped = 0;
    m_sharedStorageDenied = 0;
    m_sharedStorageApplyDepth = 0;
    m_lastSharedStorageEvent = reason && reason[0] ? reason : "reset";
}
