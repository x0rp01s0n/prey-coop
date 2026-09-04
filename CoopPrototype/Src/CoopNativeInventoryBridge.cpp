#include "ModMain.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include "CoopItemClassification.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <boost/variant/get.hpp>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <Chairloader/IChairLogger.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/ark/ArkGame.h>
#include <Prey/GameDll/ark/ArkItemSystem.h>
#include <Prey/GameDll/ark/iface/IArkItem.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerWeaponComponent.h>
#include <Prey/GameDll/ark/player/ArkQuickSelectComponent.h>
#include <Prey/GameDll/ark/player/ArkPsiComponent.h>
#include <Prey/GameDll/ark/player/ability/ArkAbilityComponent.h>
#include <Prey/GameDll/arkitem.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr uint32_t kNativeInventoryTraceLogLimit = 600;

void LogCoop(std::string_view msg)
{
    CoopRuntimeLog::Write(msg);
}

bool TraceInventoryRestoreEnabled()
{
    return CoopRuntimeConfig::Flag("COOP_TRACE_INVENTORY_RESTORE");
}

std::string BoolText(bool value)
{
    return value ? "1" : "0";
}

const char* SerializationTargetName(int target)
{
    switch (static_cast<ESerializationTarget>(target))
    {
    case eST_SaveGame:
        return "save";
    case eST_Network:
        return "network";
    case eST_Script:
        return "script";
    default:
        return "unknown";
    }
}

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c) || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
}

IArkItem* FindArkItemDirect(ArkItemSystem* itemSystem, unsigned itemId)
{
    if (!itemSystem || itemId == 0)
        return nullptr;

    const auto it = itemSystem->m_items.find(itemId);
    return it != itemSystem->m_items.end() ? it->second : nullptr;
}

std::string DescribeItemIdCompactForTrace(unsigned itemId)
{
    if (itemId == 0)
        return "0";

    ArkGame* arkGame = ArkGame::GetArkGame();
    if (!arkGame)
        return std::to_string(itemId) + ":no-system";

    ArkItemSystem& itemSystem = arkGame->GetArkItemSystem();
    IArkItem* item = nullptr;
    std::string reason;
    if (!TryGuardedCall("native inventory bridge item registry lookup", [&]() -> IArkItem* { return FindArkItemDirect(&itemSystem, itemId); }, item, &reason) || !item)
        return std::to_string(itemId) + ":null";

    CArkItem* concreteItem = IsLikelyRuntimeCppObject(item) ? static_cast<CArkItem*>(item) : nullptr;
    if (!concreteItem || !IsLikelyRuntimeCppObject(concreteItem, sizeof(CArkItem)))
        return std::to_string(itemId) + ":unreadable";

    uint64_t archetype = 0;
    int count = -1;
    unsigned ownerId = 0;
    std::string name;
    TryGuardedVoidCall(
        "native inventory bridge compact item fields",
        [&]()
        {
            archetype = concreteItem->m_selectedArchetype;
            count = concreteItem->m_count;
            ownerId = concreteItem->m_ownerId;
            name = concreteItem->m_inventoryName.c_str() ? std::string(concreteItem->m_inventoryName.c_str()) : std::string();
        },
        &reason);

    return std::to_string(itemId) +
        ":arch=" + std::to_string(archetype) +
        ":count=" + std::to_string(count) +
        ":owner=" + std::to_string(ownerId) +
        ":name=" + StatusToken(name.empty() ? "-" : name);
}

std::string DescribeInventoryStoredCellsForTrace(const ArkInventory* inventory)
{
    if (!inventory)
        return {};

    std::string reason;
    std::string details;
    bool builtDetails = false;
    TryGuardedCall(
        "native inventory bridge stored cells",
        [&]() -> bool
        {
            std::ostringstream oss;
            const size_t storedCount = inventory->m_storedItems.size();
            oss << " directSize=" << static_cast<int>(inventory->m_size)
                << " flags=" << BoolText(inventory->m_bSortDirty)
                << BoolText(inventory->m_bSerializeOpen)
                << BoolText(inventory->m_bPreventStorage)
                << BoolText(inventory->m_bTakesTrash)
                << " stored=" << storedCount;

            const size_t limit = std::min<size_t>(storedCount, 8);
            if (limit > 0)
            {
                oss << " cells=[";
                for (size_t i = 0; i < limit; ++i)
                {
                    const ArkInventory::StorageCell& cell = inventory->m_storedItems[i];
                    if (i != 0)
                        oss << ",";
                    oss << DescribeItemIdCompactForTrace(cell.m_entityId)
                        << "@" << cell.m_x << "," << cell.m_y
                        << ":" << cell.m_width << "x" << cell.m_height;
                }
                if (storedCount > limit)
                    oss << ",...";
                oss << "]";
            }

            details = oss.str();
            return true;
        },
        builtDetails,
        &reason);

    if (!reason.empty())
        details += " cellReason=" + StatusToken(reason);
    return details;
}

std::string DescribeInventoryForTrace(const ArkInventory* inventory)
{
    if (!inventory)
        return "inventory=null";

    std::string reason;
    unsigned ownerId = 0;
    int width = -1;
    int height = -1;
    int itemCount = -1;

    TryGuardedCall("native inventory bridge GetOwnerId", [inventory]() -> unsigned { return inventory->GetOwnerId(); }, ownerId, &reason);
    TryGuardedCall("native inventory bridge GetWidth", [inventory]() -> int { return inventory->GetWidth(); }, width, &reason);
    TryGuardedCall("native inventory bridge GetHeight", [inventory]() -> int { return inventory->GetHeight(); }, height, &reason);
    TryGuardedCall("native inventory bridge stored item count", [inventory]() -> int { return static_cast<int>(inventory->m_storedItems.size()); }, itemCount, &reason);

    return "inventory=" + std::to_string(reinterpret_cast<std::uintptr_t>(inventory)) +
        " owner=" + std::to_string(ownerId) +
        " size=" + std::to_string(width) + "x" + std::to_string(height) +
        " ids=" + std::to_string(itemCount) +
        DescribeInventoryStoredCellsForTrace(inventory);
}

std::string DescribeArkItemForTrace(IArkItem* item)
{
    if (!item)
        return "item=null";

    std::string reason;
    uint64_t archetype = 0;
    int count = -1;
    unsigned ownerId = 0;
    std::string name;
    std::string type;
    std::pair<int, int> dimensions = { -1, -1 };
    CArkItem* concreteItem = IsLikelyRuntimeCppObject(item) ? static_cast<CArkItem*>(item) : nullptr;
    if (!concreteItem || !IsLikelyRuntimeCppObject(concreteItem, sizeof(CArkItem)))
        return "item=unreadable";

    TryGuardedVoidCall(
        "native inventory bridge passive CArkItem fields",
        [&]()
        {
            archetype = concreteItem->m_selectedArchetype;
            count = concreteItem->m_count;
            ownerId = concreteItem->m_ownerId;
            dimensions = { concreteItem->m_inventoryWidth, concreteItem->m_inventoryHeight };
            name = concreteItem->m_inventoryName.c_str() ? std::string(concreteItem->m_inventoryName.c_str()) : std::string();
            type = concreteItem->m_type.c_str() ? std::string(concreteItem->m_type.c_str()) : std::string();
        },
        &reason);

    return "arch=" + std::to_string(archetype) +
        " count=" + std::to_string(count) +
        " owner=" + std::to_string(ownerId) +
        " dim=" + std::to_string(dimensions.first) + "x" + std::to_string(dimensions.second) +
        " name=" + StatusToken(name.empty() ? "-" : name) +
        " type=" + StatusToken(type.empty() ? "-" : type);
}

std::string DescribeCArkItemForTrace(CArkItem* item)
{
    if (!item)
        return "item=null";

    return "item=" + std::to_string(reinterpret_cast<std::uintptr_t>(item)) + " " +
        DescribeArkItemForTrace(static_cast<IArkItem*>(item));
}

std::string DescribeItemIdForTrace(unsigned itemId)
{
    if (itemId == 0)
        return "itemId=0";

    ArkGame* arkGame = ArkGame::GetArkGame();
    if (!arkGame)
        return "itemId=" + std::to_string(itemId) + " itemSystem=null";

    ArkItemSystem& itemSystem = arkGame->GetArkItemSystem();
    IArkItem* item = nullptr;
    std::string reason;
    if (!TryGuardedCall("native inventory bridge direct item registry lookup", [&]() -> IArkItem* { return FindArkItemDirect(&itemSystem, itemId); }, item, &reason) || !item)
        return "itemId=" + std::to_string(itemId) + " item=null";

    return "itemId=" + std::to_string(itemId) + " " + DescribeArkItemForTrace(item);
}

CArkItem* FindCArkItemForWeaponRepair(ArkItemSystem* itemSystem, unsigned itemId, std::string& reason)
{
    CArkItem* item = nullptr;
    if (itemSystem && IsLikelyRuntimeCppObject(itemSystem, sizeof(ArkItemSystem)))
    {
        IArkItem* rawItem = FindArkItemDirect(itemSystem, itemId);
        if (rawItem && IsLikelyRuntimeCppObject(rawItem))
            item = static_cast<CArkItem*>(rawItem);
    }

    if (!item)
    {
        TryGuardedCall(
            "coop weapon repair GetItemFromEntityId",
            [itemId]() -> CArkItem*
            {
                return CArkItem::GetItemFromEntityId(itemId);
            },
            item,
            &reason);
    }

    if (!item || !IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return nullptr;

    return item;
}

bool QuickSelectContainsWeapon(ArkQuickSelectComponent* quickSelect, unsigned itemId)
{
    if (!quickSelect || itemId == 0 || !IsLikelyRuntimeCppObject(quickSelect, sizeof(ArkQuickSelectComponent)))
        return false;

    auto containsInSelection = [itemId](const ArkQuickSelectComponent::QuickSelectId& selection) -> bool
    {
        if (selection.m_type != ArkQuickSelectComponent::QuickSelectType::weapon)
            return false;
        const unsigned int* selectedItemId = boost::get<unsigned int>(&selection.m_id);
        return selectedItemId && *selectedItemId == itemId;
    };

    for (const ArkQuickSelectComponent::QuickSelectId& selection : quickSelect->m_keyboardQuickSelects)
    {
        if (containsInSelection(selection))
            return true;
    }
    for (const ArkQuickSelectComponent::QuickSelectId& selection : quickSelect->m_controllerQuickSelects)
    {
        if (containsInSelection(selection))
            return true;
    }

    return false;
}

void RefreshQuickSelectWeapon(ArkQuickSelectComponent* quickSelect, CArkItem* item, unsigned itemId, bool allowAdd, bool& updated, std::string& reason)
{
    if (!quickSelect || !item || !IsLikelyRuntimeCppObject(quickSelect, sizeof(ArkQuickSelectComponent)))
        return;

    bool canQuickSelect = false;
    TryGuardedCall(
        "coop weapon register CanQuickSelect",
        [quickSelect, item]() -> bool
        {
            return quickSelect->CanQuickSelect(*item);
        },
        canQuickSelect,
        &reason);

    if (!canQuickSelect)
        return;

    if (allowAdd && !QuickSelectContainsWeapon(quickSelect, itemId))
    {
        if (TryGuardedVoidCall(
                "coop weapon register AddToQuickSelect",
                [quickSelect, item]()
                {
                    quickSelect->AddToQuickSelect(*item);
                },
                &reason))
        {
            updated = true;
        }
    }

    if (TryGuardedVoidCall(
            "coop weapon register quickselect refresh",
            [quickSelect, item]()
            {
                quickSelect->TryUpdateAmmoCount(*item);
                quickSelect->RefreshFilterFeedback();
            },
            &reason))
    {
        updated = true;
    }
}

} // namespace

bool ModMain::EnsureLocalPlayerWeaponRegistered(unsigned itemId, const char* reason)
{
    if (itemId == 0)
        return false;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !IsLikelyRuntimeCppObject(player, sizeof(ArkPlayer)) || !player->GetEntity())
        return false;

    ArkInventory* inventory = player->m_pInventory;
    if (!inventory || !IsLikelyRuntimeCppObject(inventory, sizeof(ArkInventory)))
        return false;

    std::string guardReason;
    bool contains = false;
    if (!TryGuardedCall(
            "coop weapon register inventory contains",
            [inventory, itemId]() -> bool
            {
                return inventory->Contains(itemId);
            },
            contains,
            &guardReason) ||
        !contains)
    {
        return false;
    }

    ArkGame* arkGame = ArkGame::GetArkGame();
    ArkItemSystem* itemSystem = arkGame ? &arkGame->GetArkItemSystem() : nullptr;
    CArkItem* item = FindCArkItemForWeaponRepair(itemSystem, itemId, guardReason);
    if (!item)
        return false;

    if (!CoopItemClassification::IsRealWeaponInventoryItem(item, guardReason))
        return false;

    ArkPlayerWeaponComponent& weaponComponent = player->m_weaponComponent;
    bool hadWeaponId = std::find(
        weaponComponent.m_weaponEntityIds.begin(),
        weaponComponent.m_weaponEntityIds.end(),
        itemId) != weaponComponent.m_weaponEntityIds.end();
    bool calledOnItemAdded = false;
    if (!hadWeaponId)
    {
        if (TryGuardedVoidCall(
                "coop weapon register OnItemAdded",
                [&weaponComponent, itemId]()
                {
                    weaponComponent.OnItemAdded(itemId, itemId);
                },
                &guardReason))
        {
            calledOnItemAdded = true;
        }
    }

    hadWeaponId = std::find(
        weaponComponent.m_weaponEntityIds.begin(),
        weaponComponent.m_weaponEntityIds.end(),
        itemId) != weaponComponent.m_weaponEntityIds.end();
    if (!hadWeaponId)
        return false;

    if (!m_localPlayerDowned)
    {
        TryGuardedVoidCall(
            "coop weapon register SetCanEquip",
            [&weaponComponent]()
            {
                weaponComponent.SetCanEquip(true);
            },
            &guardReason);
    }

    uint64_t archetypeId = 0;
    TryGuardedCall(
        "coop weapon register GetArchetype",
        [item]() -> uint64_t
        {
            return item->GetArchetype();
        },
        archetypeId,
        &guardReason);

    bool addedType = false;
    if (archetypeId != 0)
    {
        const bool hasType = std::find(
            weaponComponent.m_weaponTypesAcquired.begin(),
            weaponComponent.m_weaponTypesAcquired.end(),
            archetypeId) != weaponComponent.m_weaponTypesAcquired.end();
        if (!hasType)
        {
            if (TryGuardedVoidCall(
                    "coop weapon register AddToAcquiredWeaponTypes",
                    [&weaponComponent, archetypeId]()
                    {
                        weaponComponent.AddToAcquiredWeaponTypes(archetypeId);
                    },
                    &guardReason))
            {
                addedType = true;
            }
            else
            {
                return false;
            }
        }
    }

    bool quickUpdated = false;
    ArkQuickSelectComponent* quickSelect = nullptr;
    if (TryGuardedCall(
            "coop weapon register GetQuickSelectComponent",
            [player]() -> ArkQuickSelectComponent*
            {
                return &player->m_playerComponent.GetQuickSelectComponent();
            },
            quickSelect,
            &guardReason) &&
        quickSelect &&
        IsLikelyRuntimeCppObject(quickSelect, sizeof(ArkQuickSelectComponent)))
    {
        // Vanilla inventory events usually assign quickselect. We only add when absent,
        // otherwise repeated sidecar reconcile passes duplicate the same weapon slot.
        RefreshQuickSelectWeapon(quickSelect, item, itemId, true, quickUpdated, guardReason);
    }

    bool autoEquipAttempted = false;
    bool autoEquipped = false;
    unsigned equippedOrPending = 0;
    const bool equippedStateRead = TryGuardedCall(
        "coop weapon register equipped-or-pending",
        [&weaponComponent]()
        {
            return weaponComponent.GetEquippedOrToEquipWeaponId();
        },
        equippedOrPending,
        &guardReason);
    if (equippedStateRead && equippedOrPending == 0 && !m_localPlayerDowned)
    {
        autoEquipAttempted = true;
        TryGuardedCall(
            "coop weapon register first weapon EquipWeapon",
            [&weaponComponent, itemId]()
            {
                return weaponComponent.EquipWeapon(itemId);
            },
            autoEquipped,
            &guardReason);
    }

    if ((calledOnItemAdded || addedType || quickUpdated || autoEquipAttempted) &&
        TraceInventoryRestoreEnabled())
    {
        LogCoop(
            "local weapon registration repaired item=" + std::to_string(itemId) +
            " arch=" + std::to_string(archetypeId) +
            " onItemAdded=" + BoolText(calledOnItemAdded) +
            " type=" + BoolText(addedType) +
            " quickRefresh=" + BoolText(quickUpdated) +
            " firstEquipAttempted=" + BoolText(autoEquipAttempted) +
            " firstEquipped=" + BoolText(autoEquipped) +
            (reason && reason[0] ? " reason=" + std::string(reason) : std::string()));
    }

    return true;
}

bool ModMain::ResetLocalPlayerWeaponsForInventoryReplacement(const char* reason)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !IsLikelyRuntimeCppObject(player, sizeof(ArkPlayer)) || !player->GetEntity())
        return false;

    std::string guardReason;
    ArkPlayerWeaponComponent& weaponComponent = player->m_weaponComponent;
    if (!TryGuardedVoidCall(
            "coop weapon replacement RemoveWeapons",
            [&weaponComponent]()
            {
                // This must run while the outgoing inventory still owns its
                // weapon entities. Vanilla can then detach/hide the equipped
                // weapon and clear its HUD state through the normal callbacks.
                weaponComponent.RemoveWeapons();
                weaponComponent.m_weaponTypesAcquired.clear();
            },
            &guardReason))
    {
        LogCoop(
            "local weapon replacement reset failed" +
            (reason && reason[0] ? ": " + std::string(reason) : std::string()) +
            (guardReason.empty() ? std::string() : " guard=" + StatusToken(guardReason)));
        return false;
    }

    ArkQuickSelectComponent& quickSelect = player->m_playerComponent.GetQuickSelectComponent();
    if (!TryGuardedVoidCall(
            "coop weapon replacement quickselect reset",
            [&quickSelect]()
            {
                quickSelect.CloseQuickSelect();
                quickSelect.Reset();
                quickSelect.RefreshFilterFeedback();
            },
            &guardReason))
    {
        LogCoop(
            "local weapon replacement quickselect reset failed" +
            (reason && reason[0] ? ": " + std::string(reason) : std::string()) +
            (guardReason.empty() ? std::string() : " guard=" + StatusToken(guardReason)));
        return false;
    }

    LogCoop(
        "local weapon state reset before inventory replacement" +
        (reason && reason[0] ? ": " + std::string(reason) : std::string()));
    return true;
}

uint32_t ModMain::ReconcileLocalPlayerWeaponsFromInventory(const char* reason, bool resetWeaponState)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !IsLikelyRuntimeCppObject(player, sizeof(ArkPlayer)) || !player->GetEntity())
        return 0;

    ArkInventory* inventory = player->m_pInventory;
    if (!inventory || !IsLikelyRuntimeCppObject(inventory, sizeof(ArkInventory)))
        return 0;

    ArkGame* arkGame = ArkGame::GetArkGame();
    ArkItemSystem* itemSystem = arkGame ? &arkGame->GetArkItemSystem() : nullptr;
    if (!itemSystem || !IsLikelyRuntimeCppObject(itemSystem, sizeof(ArkItemSystem)))
        return 0;

    std::string guardReason;
    std::vector<unsigned int> inventoryItemIds;
    bool readCells = false;
    TryGuardedCall(
        "coop weapon reconcile inventory cells",
        [&]() -> bool
        {
            inventoryItemIds.reserve(inventory->m_storedItems.size());
            for (const ArkInventory::StorageCell& cell : inventory->m_storedItems)
            {
                if (cell.m_entityId == 0)
                    continue;
                if (std::find(inventoryItemIds.begin(), inventoryItemIds.end(), cell.m_entityId) == inventoryItemIds.end())
                    inventoryItemIds.push_back(cell.m_entityId);
            }
            return true;
        },
        readCells,
        &guardReason);

    ArkPlayerWeaponComponent& weaponComponent = player->m_weaponComponent;
    ArkQuickSelectComponent* quickSelect = nullptr;
    TryGuardedCall(
        "coop weapon reconcile GetQuickSelectComponent",
        [player]() -> ArkQuickSelectComponent*
        {
            return &player->m_playerComponent.GetQuickSelectComponent();
        },
        quickSelect,
        &guardReason);

    if (resetWeaponState)
    {
        TryGuardedVoidCall(
            "coop weapon reconcile RemoveWeapons",
            [&weaponComponent]()
            {
                weaponComponent.RemoveWeapons();
                // RemoveWeapons owns entity/listener/equip teardown but
                // intentionally preserves campaign acquisition history. At
                // this pre-PostSerialize player replacement boundary that
                // history belongs to the Host, so rebuild it from the Client
                // inventory items restored immediately below.
                weaponComponent.m_weaponTypesAcquired.clear();
            },
            &guardReason);

        if (quickSelect && IsLikelyRuntimeCppObject(quickSelect, sizeof(ArkQuickSelectComponent)))
        {
            TryGuardedVoidCall(
                "coop weapon reconcile quickselect reset",
                [quickSelect]()
                {
                    quickSelect->CloseQuickSelect();
                    quickSelect->Reset();
                    quickSelect->RefreshFilterFeedback();
                },
                &guardReason);
        }
    }

    uint32_t repaired = 0;
    uint32_t weaponCandidates = 0;
    for (const unsigned itemId : inventoryItemIds)
    {
        CArkItem* item = FindCArkItemForWeaponRepair(itemSystem, itemId, guardReason);
        if (!CoopItemClassification::IsRealWeaponInventoryItem(item, guardReason))
            continue;

        ++weaponCandidates;
        if (EnsureLocalPlayerWeaponRegistered(itemId, reason && reason[0] ? reason : "weapon reconcile"))
            ++repaired;
    }

    if (quickSelect && IsLikelyRuntimeCppObject(quickSelect, sizeof(ArkQuickSelectComponent)))
    {
        TryGuardedVoidCall(
            "coop weapon reconcile quickselect refresh",
            [quickSelect]()
            {
                quickSelect->RefreshFilterFeedback();
            },
            &guardReason);
    }

    LogCoop(
        "local weapon state reconciled reset=" + BoolText(resetWeaponState) +
        " cells=" + BoolText(readCells) +
        " inventoryItems=" + std::to_string(inventoryItemIds.size()) +
        " weaponCandidates=" + std::to_string(weaponCandidates) +
        " repaired=" + std::to_string(repaired) +
        (reason && reason[0] ? " reason=" + std::string(reason) : std::string()) +
        (guardReason.empty() ? std::string() : " guard=" + StatusToken(guardReason)));

    return repaired;
}

void ModMain::OnArkInventorySerializeHook(
    ArkInventory* inventory,
    const char* functionName,
    bool reading,
    int target,
    bool ok)
{
    ++m_nativeInventorySerializeCalls;
    if (reading)
        ++m_nativeInventorySerializeReadCalls;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const bool localPlayerInventory = player && inventory && inventory == player->m_pInventory;
    if (!localPlayerInventory)
        return;

    m_lastNativeInventoryLocalSerializeWasRead = reading;
    m_lastNativeInventoryEvent = std::string(functionName ? functionName : "Serialize") +
        (reading ? " read " : " write ") +
        SerializationTargetName(target) +
        (ok ? " ok" : " not-ok") +
        " pending=" + std::to_string(m_playerSidecarInventoryPending) +
        " " + DescribeInventoryForTrace(inventory);

    if (reading || m_playerSidecarInventoryPending > 0)
        LogCoop("native player inventory " + m_lastNativeInventoryEvent);

    if (reading && ok && target == eST_SaveGame)
    {
        const bool pendingClientOverride =
            m_networkMode == CoopNetworkMode::Client &&
            m_saveLoadGuardActive &&
            m_pendingReceivedPlayerStateApply &&
            m_pendingPlayerSidecarInventoryRestore;
        if (pendingClientOverride)
        {
            m_lastNativeInventoryEvent =
                std::string(functionName ? functionName : "FullSerialize") +
                " queued client inventory override for pre-PostSerialize boundary";
            LogCoop("native player inventory " + m_lastNativeInventoryEvent);
        }
        RememberLocalPlayerInventoryItemIds(inventory, functionName);
    }
}

bool ModMain::PreparePendingPlayerAbilitiesForVanillaPostSerialize(
    ArkPlayerComponent* component,
    const char* reason)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (m_networkMode != CoopNetworkMode::Client ||
        !m_saveLoadGuardActive ||
        !m_pendingReceivedPlayerStateApply ||
        !m_pendingNativePlayerStateOverrideValid ||
        !player ||
        component != &player->m_playerComponent ||
        m_receivedPlayerStateAbilitiesPreparedForNativeLoad)
    {
        return false;
    }

    const PlayerSidecarState& state = m_pendingNativePlayerStateOverride;
    if (!state.hasAbilities)
        return false;

    ArkAbilityComponent& abilityComponent = component->GetAbilityComponent();
    std::string guardReason;
    bool applied = false;
    if (!TryGuardedCall(
            "coop native read boundary ability override",
            [&]() -> bool
            {
                abilityComponent.Reset();
                for (const PlayerResearchState& research : state.research)
                {
                    for (ArkResearchTopicData& topic : abilityComponent.m_researchTopics)
                    {
                        if (topic.m_id == research.researchId)
                        {
                            topic.m_scanCount = research.scanCount;
                            break;
                        }
                    }
                }

                for (const PlayerAbilityState& ability : state.abilities)
                {
                    if (ability.seen)
                        abilityComponent.MarkAbilitySeen(ability.abilityId);
                    if (ability.acquired)
                        abilityComponent.GrantAbility(ability.abilityId);
                }

                const bool useNativePlayerCapture =
                    state.hasNativeCapture &&
                    state.nativeCapture.sawPlayerWrite;
                const bool hasPsi = useNativePlayerCapture || state.hasPsi;
                if (hasPsi)
                {
                    const float psiPoints = useNativePlayerCapture
                        ? state.nativeCapture.psiPoints
                        : state.psiPoints;
                    component->GetPsiComponent().SetPoints(std::max(0.0f, psiPoints));
                }
                return true;
            },
            applied,
            &guardReason) ||
        !applied)
    {
        m_lastNativePlayerEvent =
            "client ability override failed before Vanilla PostSerialize guard=" +
            StatusToken(guardReason);
        LogCoop("native player " + m_lastNativePlayerEvent);
        return false;
    }

    m_receivedPlayerStateAbilitiesPreparedForNativeLoad = true;
    m_lastNativePlayerEvent =
        "applied client abilities before Vanilla PostSerialize abilities=" +
        std::to_string(state.abilities.size()) +
        " research=" + std::to_string(state.research.size()) +
        " reason=" + StatusToken(reason && reason[0] ? std::string(reason) : std::string("native read boundary"));
    LogCoop("native player " + m_lastNativePlayerEvent);
    return true;
}

bool ModMain::IsTrackedLocalPlayerInventoryItemId(unsigned entityId) const
{
    return entityId != 0 && m_gameStateLocalPlayerInventoryItemIds.find(entityId) != m_gameStateLocalPlayerInventoryItemIds.end();
}

uint64_t ModMain::EnterNativeInventorySerializeScope(
    ArkInventory* inventory,
    const void* serializerPtr,
    bool localPlayerInventory,
    bool reading,
    int target,
    const std::string& sectionName)
{
    NativeInventorySerializeScopeState state;
    state.depth = static_cast<uint32_t>(m_nativeInventoryScopeStack.size() + 1);
    state.scopeSeq = m_nextNativeInventoryScopeSeq++;
    state.inventoryPtr = reinterpret_cast<std::uintptr_t>(inventory);
    state.serializerPtr = reinterpret_cast<std::uintptr_t>(serializerPtr);
    state.localPlayerInventory = localPlayerInventory;
    state.reading = reading;
    state.target = target;
    state.sectionName = sectionName;

    m_nativeInventoryScope = state;
    m_nativeInventoryScopeStack.push_back(state);
    ++m_nativeInventoryScopeEnters;

    if (localPlayerInventory && target == eST_SaveGame && CoopSaveStateBridge::IsGameStateSection(sectionName))
    {
        NativeInventoryFragmentScopeInfo info;
        info.scopeSeq = state.scopeSeq;
        info.inventoryPtr = state.inventoryPtr;
        info.serializerPtr = state.serializerPtr;
        info.reading = reading;
        info.target = target;
        info.sectionName = sectionName;
        m_nativeFragmentLocator.OnLocalPlayerInventoryScopeEnter(info);

        if (ShouldCaptureNativeFragmentTarget(reading, target, sectionName))
        {
            m_nativeTargetFragmentCapture.BeginInventoryScope(
                info,
                "target local-player inventory read");
        }
    }

    return state.scopeSeq;
}

void ModMain::ExitNativeInventorySerializeScope(uint64_t scopeSeq)
{
    if (!m_nativeInventoryScopeStack.empty())
    {
        if (m_nativeInventoryScopeStack.back().scopeSeq == scopeSeq)
        {
            const NativeInventorySerializeScopeState exiting = m_nativeInventoryScopeStack.back();
            if (exiting.localPlayerInventory && exiting.target == eST_SaveGame && CoopSaveStateBridge::IsGameStateSection(exiting.sectionName))
            {
                m_nativeFragmentLocator.OnLocalPlayerInventoryScopeExit(scopeSeq);
                if (m_nativeTargetFragmentCapture.MatchesInventoryScope(scopeSeq))
                {
                    m_nativeTargetFragmentCapture.ExitInventoryScope(scopeSeq);
                    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle bundle =
                        m_nativeTargetFragmentCapture.BuildBundle();
                    if (bundle.ok)
                    {
                        TryApplyNativeFragmentTargetNoopPatch(bundle, "target local-player inventory complete");
                        m_nativeTargetFragmentCapture.Complete("target local-player inventory complete");
                        m_nativeTargetFragmentCapture.SetLastEvent(
                            "target inventory exit scope=" + std::to_string(scopeSeq) +
                            " complete " + m_nativeTargetFragmentCapture.BuildBundleStatus());
                    }
                    else
                    {
                        m_nativeTargetFragmentCapture.SetLastEvent(
                            "target inventory exit scope=" + std::to_string(scopeSeq) +
                            " waiting item subtrees " + m_nativeTargetFragmentCapture.BuildBundleStatus());
                    }
                }
            }
            m_nativeInventoryScopeStack.pop_back();
        }
        else
        {
            auto it = std::find_if(
                m_nativeInventoryScopeStack.begin(),
                m_nativeInventoryScopeStack.end(),
                [scopeSeq](const NativeInventorySerializeScopeState& state) { return state.scopeSeq == scopeSeq; });
            if (it != m_nativeInventoryScopeStack.end())
                m_nativeInventoryScopeStack.erase(it);
        }
    }

    if (!m_nativeInventoryScopeStack.empty())
        m_nativeInventoryScope = m_nativeInventoryScopeStack.back();
    else
        m_nativeInventoryScope = NativeInventorySerializeScopeState();

    ++m_nativeInventoryScopeExits;
}

bool ModMain::IsNativeLocalPlayerInventorySerializeScopeActive(
    const void* serializerPtr,
    bool reading,
    int target,
    const std::string& sectionName) const
{
    if (m_nativeInventoryScopeStack.empty())
        return false;

    const NativeInventorySerializeScopeState& state = m_nativeInventoryScopeStack.back();
    if (!state.localPlayerInventory || state.target != target || state.reading != reading)
        return false;

    if (!CoopSaveStateBridge::IsGameStateSection(state.sectionName))
        return false;

    const std::uintptr_t queriedSerializer = reinterpret_cast<std::uintptr_t>(serializerPtr);
    if (queriedSerializer != 0 && state.serializerPtr != 0 && queriedSerializer != state.serializerPtr)
    {
        // Serializer wrappers used for trace/probe can change the pointer seen by nested item hooks.
        if (!sectionName.empty() && sectionName != state.sectionName)
            return false;
    }

    if (!sectionName.empty() && !CoopSaveStateBridge::IsGameStateSection(sectionName))
        return false;

    return true;
}

uint64_t ModMain::GetNativeInventorySerializeScopeSeq() const
{
    return m_nativeInventoryScopeStack.empty() ? 0 : m_nativeInventoryScopeStack.back().scopeSeq;
}

std::string ModMain::GetNativeInventorySerializeScopeSection() const
{
    return m_nativeInventoryScopeStack.empty() ? std::string() : m_nativeInventoryScopeStack.back().sectionName;
}

uint64_t ModMain::EnterNativeItemSerializeScope(
    CArkItem* item,
    unsigned itemEntityId,
    const void* serializerPtr,
    bool localPlayerInventoryItem,
    bool reading,
    int target,
    const std::string& sectionName)
{
    if (!localPlayerInventoryItem || target != eST_SaveGame)
        return 0;

    const uint64_t scopeSeq = m_nextNativeItemScopeSeq++;
    ++m_nativeItemScopeEnters;

    NativeItemFragmentScopeInfo info;
    info.scopeSeq = scopeSeq;
    info.inventoryScopeSeq = GetNativeInventorySerializeScopeSeq();
    info.itemPtr = reinterpret_cast<std::uintptr_t>(item);
    info.itemEntityId = itemEntityId;
    info.serializerPtr = reinterpret_cast<std::uintptr_t>(serializerPtr);
    info.reading = reading;
    info.target = target;
    info.sectionName = sectionName.empty() ? GetNativeInventorySerializeScopeSection() : sectionName;
    if (CoopSaveStateBridge::IsGameStateSection(info.sectionName))
    {
        m_nativeFragmentLocator.OnPlayerInventoryItemScopeEnter(info);
        if (m_nativeTargetFragmentCapture.IsActive() &&
            ShouldCaptureNativeFragmentTarget(reading, target, info.sectionName))
        {
            m_nativeTargetFragmentCapture.EnterItemScope(info);
        }
    }

    return scopeSeq;
}

void ModMain::ExitNativeItemSerializeScope(uint64_t scopeSeq)
{
    if (scopeSeq == 0)
        return;

    ++m_nativeItemScopeExits;
    m_nativeFragmentLocator.OnPlayerInventoryItemScopeExit(scopeSeq);
    if (m_nativeTargetFragmentCapture.IsActive())
    {
        m_nativeTargetFragmentCapture.ExitItemScope(scopeSeq);
        const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle bundle =
            m_nativeTargetFragmentCapture.BuildBundle();
        if (bundle.ok)
        {
            TryApplyNativeFragmentTargetNoopPatch(bundle, "target local-player item subtrees complete");
            m_nativeTargetFragmentCapture.Complete("target local-player item subtrees complete");
            m_nativeTargetFragmentCapture.SetLastEvent(
                "target item exit scope=" + std::to_string(scopeSeq) +
                " complete " + m_nativeTargetFragmentCapture.BuildBundleStatus());
        }
        else
        {
            m_nativeTargetFragmentCapture.SetLastEvent(
                "target item exit scope=" + std::to_string(scopeSeq) +
                " pending " + m_nativeTargetFragmentCapture.BuildBundleStatus());
        }
    }
}

bool ModMain::ShouldUseNativeGameStatePlayerOverlay() const
{
    if (!CoopRuntimeConfig::UnsafeFlag("COOP_ENABLE_NATIVE_GAMESTATE_PLAYER_OVERLAY"))
        return false;
    return !CoopRuntimeConfig::Flag("COOP_DISABLE_NATIVE_GAMESTATE_PLAYER_OVERLAY");
}

const NativeSideBlobCaptureState* ModMain::GetPendingNativeGameStatePlayerCapture() const
{
    if (!ShouldUseNativeGameStatePlayerOverlay())
        return nullptr;

    if (m_networkMode != CoopNetworkMode::Client)
        return nullptr;

    if (!m_pendingNativePlayerStateOverrideValid)
        return nullptr;

    const PlayerSidecarState& state = m_pendingNativePlayerStateOverride;
    if (!state.hasNativeCapture || !state.nativeCapture.sawInventoryWrite || state.nativeCapture.items.empty())
        return nullptr;

    return &state.nativeCapture;
}

const NativeCapturedItemState* ModMain::GetPendingNativeGameStateItemForEntity(unsigned entityId) const
{
    const NativeSideBlobCaptureState* capture = GetPendingNativeGameStatePlayerCapture();
    if (!capture)
        return nullptr;

    const auto it = m_gameStateLocalPlayerInventoryItemIndexById.find(entityId);
    if (it == m_gameStateLocalPlayerInventoryItemIndexById.end() || it->second >= capture->items.size())
        return nullptr;

    const NativeCapturedItemState& capturedItem = capture->items[it->second];
    if (capturedItem.itemId != 0 && capturedItem.itemId != entityId)
    {
        ModMain* self = const_cast<ModMain*>(this);
        ++self->m_nativeItemScopeFallbacks;
        self->m_lastNativeInventoryEvent =
            "pending native GameState item index mismatch entity=" + std::to_string(entityId) +
            " captured=" + std::to_string(capturedItem.itemId) +
            " index=" + std::to_string(it->second);
        LogCoop("native player inventory " + self->m_lastNativeInventoryEvent);
        return nullptr;
    }

    return &capturedItem;
}

bool ModMain::IsActiveNativeSideBlobCaptureItem(unsigned entityId) const
{
    if (entityId == 0 || !m_nativeSideBlobCapture.active)
        return false;

    for (const NativeCapturedItemState& item : m_nativeSideBlobCapture.items)
    {
        if (item.itemId == entityId)
            return true;
    }

    return false;
}

bool ModMain::IsActiveNativeSideBlobCapturePlayerOwnedItem(const CArkItem* item, unsigned entityId) const
{
    if (!m_nativeSideBlobCapture.active || !item || entityId == 0)
        return false;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    IEntity* playerEntity = player ? player->GetEntity() : nullptr;
    if (!playerEntity)
        return false;

    const unsigned playerEntityId = playerEntity->GetId();
    if (playerEntityId == 0)
        return false;

    unsigned ownerId = 0;
    std::string reason;
    if (!TryGuardedCall(
            "native sideblob capture item owner probe",
            [item]() -> unsigned
            {
                return item->m_ownerId;
            },
            ownerId,
            &reason))
    {
        return false;
    }

    return ownerId == playerEntityId;
}

unsigned ModMain::GetNativeGameStateOverlayOwnerId() const
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    IEntity* entity = player ? player->GetEntity() : nullptr;
    return entity ? entity->GetId() : 0;
}

void ModMain::RememberLocalPlayerInventoryItemIds(const ArkInventory* inventory, const char* reason)
{
    m_gameStateLocalPlayerInventoryItemIds.clear();
    m_gameStateLocalPlayerInventoryItemIndexById.clear();

    if (!inventory || !IsLikelyRuntimeCppObject(inventory, sizeof(ArkInventory)))
        return;

    std::string guardReason;
    bool captured = false;
    if (!TryGuardedCall(
            "native inventory remember local player item ids",
            [this, inventory]() -> bool
            {
                size_t index = 0;
                for (const ArkInventory::StorageCell& cell : inventory->m_storedItems)
                {
                    if (cell.m_entityId != 0)
                    {
                        m_gameStateLocalPlayerInventoryItemIds.insert(cell.m_entityId);
                        m_gameStateLocalPlayerInventoryItemIndexById.emplace(cell.m_entityId, index);
                    }
                    ++index;
                }
                return true;
            },
            captured,
            &guardReason) ||
        !captured)
    {
        m_lastNativeInventoryEvent =
            std::string("remember local player inventory ids failed") +
            (guardReason.empty() ? std::string() : " reason=" + StatusToken(guardReason));
        LogCoop("native player inventory " + m_lastNativeInventoryEvent);
        return;
    }

    m_lastNativeInventoryEvent =
        std::string("remembered local player inventory item ids from ") +
        (reason && reason[0] ? reason : "FullSerialize") +
        " count=" + std::to_string(m_gameStateLocalPlayerInventoryItemIds.size());
    LogCoop("native player inventory " + m_lastNativeInventoryEvent);
}

void ModMain::ObserveNativeGameStateInventoryReferences(
    ArkInventory* inventory,
    bool localPlayerInventory,
    const void* serializerPtr,
    const char* reason)
{
    const std::uintptr_t serializerKey = reinterpret_cast<std::uintptr_t>(serializerPtr);
    if (serializerKey != 0 && serializerKey != m_gameStateInventoryReferenceSerializer)
    {
        m_gameStateInventoryReferenceSerializer = serializerKey;
        m_gameStateInventoryReferenceCounts.clear();
        m_gameStateInventoryExternalReferenceCounts.clear();
        m_gameStateInventoryLocalReferenceIds.clear();
        ++m_nativeGameStateInventoryReferencePasses;
    }

    ++m_nativeGameStateInventoryReferenceObservations;

    if (!inventory || !IsLikelyRuntimeCppObject(inventory, sizeof(ArkInventory)))
        return;

    std::vector<ArkInventory::StorageCell> cells;
    std::string guardReason;
    if (!TryGuardedCall(
            "native GameState inventory reference copy cells",
            [inventory]() -> std::vector<ArkInventory::StorageCell>
            {
                return inventory->m_storedItems;
            },
            cells,
            &guardReason))
    {
        m_lastNativeGameStateInventoryReferenceEvent =
            "game_state_inventory_refs copy_failed reason=" + StatusToken(guardReason);
        return;
    }

    uint32_t observedIds = 0;
    for (const ArkInventory::StorageCell& cell : cells)
    {
        if (cell.m_entityId == 0)
            continue;

        ++observedIds;
        ++m_gameStateInventoryReferenceCounts[cell.m_entityId];
        if (localPlayerInventory)
            m_gameStateInventoryLocalReferenceIds.insert(cell.m_entityId);
        else
            ++m_gameStateInventoryExternalReferenceCounts[cell.m_entityId];
    }

    uint32_t conflicts = 0;
    for (unsigned itemId : m_gameStateInventoryLocalReferenceIds)
    {
        if (m_gameStateInventoryExternalReferenceCounts.find(itemId) != m_gameStateInventoryExternalReferenceCounts.end())
            ++conflicts;
    }

    m_nativeGameStateInventoryReferenceLocalIds =
        static_cast<uint32_t>(std::min<size_t>(m_gameStateInventoryLocalReferenceIds.size(), UINT32_MAX));
    m_nativeGameStateInventoryReferenceConflicts = conflicts;

    m_lastNativeGameStateInventoryReferenceEvent =
        "game_state_inventory_refs"
        " pass=" + std::to_string(m_nativeGameStateInventoryReferencePasses) +
        " local=" + BoolText(localPlayerInventory) +
        " observed=" + std::to_string(observedIds) +
        " localIds=" + std::to_string(m_nativeGameStateInventoryReferenceLocalIds) +
        " externalInventories=" + std::to_string(m_gameStateInventoryExternalReferenceCounts.size()) +
        " conflicts=" + std::to_string(conflicts) +
        " ser=" + std::to_string(serializerKey) +
        " reason=" + StatusToken(reason && reason[0] ? std::string(reason) : std::string("GameState"));

    if (localPlayerInventory || conflicts != 0 || ShouldLogNativeInventoryTraceDetails())
        LogCoop("native player inventory " + m_lastNativeGameStateInventoryReferenceEvent);
}

void ModMain::OnArkInventorySnapshotHook(
    ArkInventory* inventory,
    const char* functionName,
    bool reading,
    int target)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const bool localPlayerInventory = player && inventory && inventory == player->m_pInventory;
    if (!localPlayerInventory && !ShouldTraceNativeInventoryDetails())
        return;

    m_lastNativeInventoryEvent = std::string(functionName ? functionName : "InventorySnapshot") +
        (reading ? " read " : " write ") +
        SerializationTargetName(target) +
        " local=" + BoolText(localPlayerInventory) +
        " " + DescribeInventoryForTrace(inventory);

    if (localPlayerInventory || (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit))
        LogCoop("native player inventory " + m_lastNativeInventoryEvent);

    CaptureNativeInventorySideBlobSnapshot(inventory, functionName, reading, target, true);
}

void ModMain::OnArkInventoryPostSerializeHook(ArkInventory* inventory, const char* functionName)
{
    ++m_nativeInventoryPostSerializeCalls;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const bool localPlayerInventory = player && inventory && inventory == player->m_pInventory;
    if (!localPlayerInventory)
        return;

    const bool safeRestorePoint = m_lastNativeInventoryLocalSerializeWasRead || m_saveLoadGuardActive;
    m_lastNativeInventoryLocalSerializeWasRead = false;
    if (!safeRestorePoint)
        return;

    m_playerSidecarInventoryNativeRestoreReady = true;
    ++m_nativeInventoryLocalPlayerRestorePoints;
    m_lastNativeInventoryEvent = std::string(functionName ? functionName : "PostSerialize") +
        " local-player restore-point pending=" + std::to_string(m_playerSidecarInventoryPending);
    LogCoop("native player inventory " + m_lastNativeInventoryEvent);

    const bool clientHostLoadInventoryWindow =
        m_networkMode == CoopNetworkMode::Client &&
        (m_clientAwaitingHostPlayerState ||
            m_pendingReceivedPlayerStateApply ||
            m_receivedPlayerStateInventoryPreparedForNativeLoad ||
            m_skipNextHostAuthoritativeInventoryApply);
    if (clientHostLoadInventoryWindow && !m_pendingPlayerSidecarInventoryRestore)
    {
        m_lastNativeInventoryEvent = std::string(functionName ? functionName : "PostSerialize") +
            " local-player restore-point skipped destructive empty fallback pending=" +
            std::to_string(m_playerSidecarInventoryPending);
        LogCoop("native player inventory " + m_lastNativeInventoryEvent);
    }

    if (m_pendingPlayerSidecarInventoryRestore)
    {
        if (m_pendingReceivedPlayerStateApply)
            m_skipNextHostAuthoritativeInventoryApply = true;
        m_playerSidecarInventoryRestoreAccumulator = 0.0f;
        LogCoop("native player inventory restore deferred until post-load interactive");
    }
}

bool ModMain::PreparePendingPlayerInventoryForVanillaPostSerialize(
    ArkInventory* inventory,
    const char* reason)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (m_networkMode != CoopNetworkMode::Client ||
        !m_saveLoadGuardActive ||
        !m_pendingReceivedPlayerStateApply ||
        !m_pendingPlayerSidecarInventoryRestore ||
        !player ||
        inventory != player->m_pInventory)
    {
        return false;
    }

    // FullSerialize still runs before new item entities may be created. At this
    // boundary the load graph exists, while Vanilla has not resolved inventory
    // references through PostSerialize yet.
    const bool restored = TryRestorePendingPlayerSidecarInventory(
        reason && reason[0] ? reason : "native inventory pre-PostSerialize boundary",
        true);
    if (!restored)
        return false;

    m_receivedPlayerStateInventoryPreparedForNativeLoad = true;
    m_skipNextHostAuthoritativeInventoryApply = true;
    m_lastNativeInventoryEvent =
        "applied client inventory override before Vanilla PostSerialize";
    LogCoop("native player inventory " + m_lastNativeInventoryEvent);
    return true;
}

bool ModMain::ShouldTraceNativeInventoryDetails() const
{
    return m_traceNativeInventory ||
        CoopRuntimeConfig::Flag("COOP_TRACE_NATIVE_INVENTORY") ||
        m_saveLoadGuardActive ||
        m_pendingPlayerSidecarInventoryRestore ||
        m_playerSidecarInventoryPending > 0;
}

bool ModMain::ShouldLogNativeInventoryTraceDetails() const
{
    return m_traceNativeInventory || CoopRuntimeConfig::Flag("COOP_TRACE_NATIVE_INVENTORY");
}

void ModMain::SetNativeInventoryTrace(bool enabled)
{
    m_traceNativeInventory = enabled;
    if (enabled)
        m_nativeInventoryTraceLogs = 0;
    m_lastNativeItemEvent = std::string("native inventory trace ") + (enabled ? "enabled" : "disabled");
    LogCoop(m_lastNativeItemEvent);
}

void ModMain::OnArkItemSystemSerializeHook(
    ArkItemSystem* itemSystem,
    const char* functionName,
    bool reading,
    int target,
    bool ok)
{
    ++m_nativeItemSystemSerializeCalls;
    m_lastNativeItemEvent = std::string("ItemSystem ") +
        (functionName ? functionName : "Serialize") +
        (reading ? " read " : " write ") +
        SerializationTargetName(target) +
        (ok ? " ok" : " not-ok") +
        " ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(itemSystem));

    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
        LogCoop("native item trace " + m_lastNativeItemEvent);
}

void ModMain::OnArkItemSerializeHook(
    CArkItem* item,
    const char* functionName,
    bool reading,
    int target,
    bool ok)
{
    ++m_nativeItemSerializeCalls;
    const bool traceDetails = ShouldLogNativeInventoryTraceDetails();
    if (traceDetails && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "Serialize") +
            (reading ? " read " : " write ") +
            SerializationTargetName(target) +
            (ok ? " ok " : " not-ok ") +
            DescribeCArkItemForTrace(item);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else if ((m_nativeItemSerializeCalls % 256) == 0)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "Serialize") +
            (reading ? " read " : " write ") +
            SerializationTargetName(target) +
            (ok ? " ok" : " not-ok") +
            " trace=off count=" + std::to_string(m_nativeItemSerializeCalls);
    }

    CaptureNativeItemSideBlobSnapshot(item, functionName, reading, target, ok);
}

void ModMain::OnArkItemPostSerializeHook(CArkItem* item, const char* functionName)
{
    ++m_nativeItemPostSerializeCalls;
    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "PostSerialize") +
            " " + DescribeCArkItemForTrace(item);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else if ((m_nativeItemPostSerializeCalls % 256) == 0)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "PostSerialize") +
            " trace=off count=" + std::to_string(m_nativeItemPostSerializeCalls);
    }
}

void ModMain::OnArkItemLifecycleHook(CArkItem* item, const char* functionName)
{
    ++m_nativeItemLifecycleCalls;
    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "Lifecycle") +
            " " + DescribeCArkItemForTrace(item);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else if ((m_nativeItemLifecycleCalls % 256) == 0)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "Lifecycle") +
            " trace=off count=" + std::to_string(m_nativeItemLifecycleCalls);
    }
}

void ModMain::OnArkInventoryMutationHook(
    const ArkInventory* inventory,
    const char* functionName,
    unsigned itemId,
    unsigned relatedItemId,
    int x,
    int y,
    bool boolResult,
    unsigned unsignedResult)
{
    ++m_nativeInventoryMutationCalls;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const bool localPlayerInventory = player && inventory && inventory == player->m_pInventory;
    if (localPlayerInventory && boolResult && !ShouldSuppressPlayerSidecarInventoryFeedback())
        MarkLocalInventoryDirty(functionName ? functionName : "inventory mutation");
    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = std::string("Inventory ") +
            (functionName ? functionName : "Mutation") +
            " local=" + BoolText(localPlayerInventory) +
            " result=" + BoolText(boolResult) +
            " ret=" + std::to_string(unsignedResult) +
            " xy=" + std::to_string(x) + "," + std::to_string(y) +
            " related=" + std::to_string(relatedItemId) +
            " " + DescribeInventoryForTrace(inventory) +
            " " + DescribeItemIdForTrace(itemId);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else if ((m_nativeInventoryMutationCalls % 256) == 0)
    {
        m_lastNativeItemEvent = std::string("Inventory ") +
            (functionName ? functionName : "Mutation") +
            " local=" + BoolText(localPlayerInventory) +
            " trace=off count=" + std::to_string(m_nativeInventoryMutationCalls);
    }
}

void ModMain::OnArkItemOwnerMutationHook(
    CArkItem* item,
    const char* functionName,
    unsigned pickerId,
    const IArkInventory* inventory,
    bool result)
{
    ++m_nativeItemOwnerMutationCalls;
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const bool localPlayerInventory =
        player && inventory && inventory == player->m_pInventory;
    const bool localPicker =
        player && pickerId != 0 && player->GetEntity() && player->GetEntity()->GetId() == pickerId;
    if ((localPlayerInventory || localPicker) && result && !ShouldSuppressPlayerSidecarInventoryFeedback())
        MarkLocalInventoryDirty(functionName ? functionName : "item owner mutation");
    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "OwnerMutation") +
            " result=" + BoolText(result) +
            " picker=" + std::to_string(pickerId) +
            " inv=" + std::to_string(reinterpret_cast<std::uintptr_t>(inventory)) +
            " " + DescribeCArkItemForTrace(item);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else if ((m_nativeItemOwnerMutationCalls % 256) == 0)
    {
        m_lastNativeItemEvent = std::string("CArkItem ") +
            (functionName ? functionName : "OwnerMutation") +
            " result=" + BoolText(result) +
            " trace=off count=" + std::to_string(m_nativeItemOwnerMutationCalls);
    }
}

void ModMain::OnArkItemResetCountHook(CArkItem* item, int count, unsigned ownerIdBefore)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    const unsigned localPlayerId = player ? player->GetEntityId() : 0;
    const bool localPlayerOwnedItem =
        ownerIdBefore != 0 && localPlayerId != 0 && ownerIdBefore == localPlayerId;
    const bool inventoryLoadOrRestoreHazard =
        m_saveLoadGuardActive ||
        m_waitingForPostLoadContinue ||
        m_pendingPostLoadResync ||
        m_arkLevelTransitionLoadActive ||
        m_runtimeTransitionCleanupPrepared ||
        m_pendingReceivedPlayerStateApply ||
        m_clientAwaitingHostPlayerState ||
        m_pendingPlayerSidecarInventoryRestore ||
        m_playerSidecarInventoryNativeRestoreActive ||
        m_playerSidecarInventoryPending > 0;
    if (localPlayerOwnedItem &&
        !inventoryLoadOrRestoreHazard &&
        !ShouldSuppressPlayerSidecarInventoryFeedback())
    {
        // ResetCount(0) can detach the item and clear its owner. The hook
        // captures that owner before native ResetCount so stack consumption
        // still arms the recovery journal without marking restore activity.
        MarkLocalInventoryDirty("CArkItem ResetCount");
    }

    if (ShouldLogNativeInventoryTraceDetails() && m_nativeInventoryTraceLogs++ < kNativeInventoryTraceLogLimit)
    {
        m_lastNativeItemEvent = "CArkItem ResetCount count=" +
            std::to_string(count) +
            " ownerBefore=" + std::to_string(ownerIdBefore) +
            " local=" + BoolText(localPlayerOwnedItem) +
            " " + DescribeCArkItemForTrace(item);
        LogCoop("native item trace " + m_lastNativeItemEvent);
    }
    else
    {
        m_lastNativeItemEvent = "CArkItem ResetCount trace=off count=" +
            std::to_string(count) + " ownerBefore=" + std::to_string(ownerIdBefore) +
            " local=" + BoolText(localPlayerOwnedItem);
    }
}
