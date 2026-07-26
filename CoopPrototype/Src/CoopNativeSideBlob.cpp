#include "ModMain.h"
#include "CoopItemClassification.h"
#include "CoopRuntimeGuards.h"
#include "CoopRuntimeLog.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <sstream>

#include <Prey/ArkEnums.h>
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryNetwork/ISerialize.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/ark/ArkGame.h>
#include <Prey/GameDll/ark/ArkItemSystem.h>
#include <Prey/GameDll/ark/iface/IArkItem.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPsiComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerWeaponComponent.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerComponent.h>
#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/GameDll/arkitem.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr const char* kNativeSideBlobCapturedPlayerSection = "CoopPrototypeNativePlayerCaptured";
constexpr uint32_t kFnv1aOffsetBasis = 2166136261u;
constexpr uint32_t kFnv1aPrime = 16777619u;

std::string NativeSideBlobHex32(uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

const char* NativeSideBlobBoolText(bool value)
{
    return value ? "1" : "0";
}

std::string NativeSideBlobStatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '/' || ch == '\\')
            ch = '_';
    }

    return value;
}

IArkItem* FindArkItemDirectForSideBlob(ArkItemSystem* itemSystem, unsigned itemId)
{
    if (!itemSystem || itemId == 0)
        return nullptr;

    const auto it = itemSystem->m_items.find(itemId);
    return it != itemSystem->m_items.end() ? it->second : nullptr;
}

template <typename Func>
bool TryWriteNativeSideBlobSection(
    ISaveGame* saveGame,
    const char* sectionName,
    const char* operationName,
    Func writer,
    std::string* outReason)
{
    bool wrote = false;
    if (!TryGuardedCall(
            operationName,
            [&]() -> bool
            {
                alignas(TSerialize) std::byte serializerStorage[sizeof(TSerialize)];
                TSerialize* serializer = reinterpret_cast<TSerialize*>(serializerStorage);
                TSerialize* returned = saveGame->AddSection(serializer, sectionName);
                if (!returned)
                    return false;

                writer(*returned);
                return returned->Ok();
            },
            wrote,
            outReason))
    {
        return false;
    }

    if (!wrote && outReason && outReason->empty())
        *outReason = "serializer not ok";
    return wrote;
}
}

void ModMain::BeginNativeSideBlobCapture(const char* reason)
{
    m_nativeSideBlobCapture = NativeSideBlobCaptureState();
    m_nativeSideBlobCapture.active = true;
    m_nativeSideBlobCapture.reason = reason ? reason : "unknown";
    m_nativeSideBlobCapture.username = GetLocalUsername();
    m_nativeSideBlobCapture.levelName = GetCurrentLevelName();
    m_nativeSideBlobCapture.worldEpoch = m_localWorldEpoch;
    m_nativeSideBlobCapture.checksum = kFnv1aOffsetBasis;
    m_nativeFinalStreamCapturedChunks = 0;
    m_nativeFinalStreamCapturedBytes = 0;
    m_nativeFinalStreamBufferedBytes = 0;
    m_nativeFinalStreamDroppedBytes = 0;
    m_nativeFinalStreamGuards = 0;
    m_nativeFinalStreamChecksum = kFnv1aOffsetBasis;
    m_nativeFinalStreamBuffer.clear();
    m_lastNativeFinalStreamDumpPath = "-";
    m_nativeWriteNodeOracleRecords.clear();
    m_nativeWriteNodeOracleStored = 0;
    m_nativeWriteNodeOracleDropped = 0;
    const uint64_t runId = m_nextNativeFragmentRunId++;
    m_activeNativeFragmentCaptureRunId = runId;
    m_nativeFragmentLocator.BeginRun(runId, reason ? reason : "native sideblob capture");
    m_lastNativeSideBlobCapturedEvent = "capture begin: " + m_nativeSideBlobCapture.reason;
    m_lastNativeFinalStreamEvent = "final_stream reset reason=" + m_nativeSideBlobCapture.reason;
}

void ModMain::CaptureNativePlayerSideBlobSnapshot(ArkPlayer* player, const char* functionName, bool reading, int target, bool ok)
{
    if (!m_nativeSideBlobCapture.active || reading || target != eST_SaveGame || !ok)
        return;
    if (!player || ArkPlayer::GetInstancePtr() != player || !IsLikelyRuntimeCppObject(player, sizeof(ArkPlayer)))
        return;

    std::string reason;
    IEntity* entity = nullptr;
    if (!TryGuardedCall("native sideblob capture player entity", [player]() -> IEntity* { return player->GetEntity(); }, entity, &reason) || !entity)
        return;

    m_nativeSideBlobCapture.sawPlayerWrite = true;
    TryGuardedCall("native sideblob capture player position", [entity]() -> Vec3 { return entity->GetWorldPos(); }, m_nativeSideBlobCapture.position, &reason);
    TryGuardedCall("native sideblob capture player rotation", [entity]() -> Quat { return entity->GetWorldRotation(); }, m_nativeSideBlobCapture.rotation, &reason);
    TryGuardedCall("native sideblob capture player view", [player]() -> Quat { return player->GetViewRotation(); }, m_nativeSideBlobCapture.viewRotation, &reason);
    TryGuardedCall("native sideblob capture player health", [player]() -> float { return player->GetHealth(); }, m_nativeSideBlobCapture.health, &reason);
    TryGuardedCall("native sideblob capture player max health", [player]() -> float { return player->GetMaxHealth(); }, m_nativeSideBlobCapture.maxHealth, &reason);
    TryGuardedCall("native sideblob capture player psi", [player]() -> float { return player->m_playerComponent.GetPsiComponent().GetPoints(); }, m_nativeSideBlobCapture.psiPoints, &reason);
    TryGuardedCall("native sideblob capture player max psi", [player]() -> float { return player->m_playerComponent.GetPsiComponent().GetMaxPoints(); }, m_nativeSideBlobCapture.psiMaxPoints, &reason);
    TryGuardedCall("native sideblob capture player armor", [player]() -> int { return player->GetArmor(); }, m_nativeSideBlobCapture.armor, &reason);
    TryGuardedCall("native sideblob capture player max armor", [player]() -> int { return player->GetMaxArmor(); }, m_nativeSideBlobCapture.maxArmor, &reason);
    EStance stance = EStance::STANCE_NULL;
    if (TryGuardedCall("native sideblob capture player stance", [player]() -> EStance { return player->GetStance(); }, stance, &reason))
        m_nativeSideBlobCapture.stance = static_cast<int>(stance);

    TryGuardedVoidCall(
        "native sideblob capture psi power fields",
        [this, player]()
        {
            const ArkPsiPowerComponent& psiPower = player->GetPsiPowerComponent();
            m_nativeSideBlobCapture.selectedPsiPower = static_cast<int>(psiPower.m_selectedPower);
            m_nativeSideBlobCapture.equippedPsiPower = static_cast<int>(psiPower.m_equippedPower);
        },
        &reason);

    TryGuardedVoidCall(
        "native sideblob capture weapon component fields",
        [this, player]()
        {
            ArkPlayerWeaponComponent& weapon = player->m_weaponComponent;
            m_nativeSideBlobCapture.equippedWeaponId = weapon.m_equippedWeaponId;
            m_nativeSideBlobCapture.lastEquippedWeaponId = weapon.m_lastEquippedWeaponId;
            m_nativeSideBlobCapture.backupWeaponId = weapon.m_backupWeaponId;
            m_nativeSideBlobCapture.toBeEquippedWeaponId = weapon.m_toBeEquippedWeaponId;
            m_nativeSideBlobCapture.weaponCount = static_cast<uint32_t>(weapon.m_weaponEntityIds.size());
            m_nativeSideBlobCapture.specialWeaponCount = static_cast<uint32_t>(weapon.m_specialWeaponIds.size());
        },
        &reason);

    m_lastNativeSideBlobCapturedEvent =
        std::string("captured player from ") + (functionName ? functionName : "serialize");
}

void ModMain::CaptureNativeInventorySideBlobSnapshot(ArkInventory* inventory, const char* functionName, bool reading, int target, bool ok)
{
    if (!m_nativeSideBlobCapture.active || reading || target != eST_SaveGame || !ok)
        return;

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !inventory || inventory != player->m_pInventory || !IsLikelyRuntimeCppObject(inventory, sizeof(ArkInventory)))
        return;

    ArkGame* arkGame = ArkGame::GetArkGame();
    ArkItemSystem* itemSystem = arkGame ? &arkGame->GetArkItemSystem() : nullptr;
    if (!itemSystem || !IsLikelyRuntimeCppObject(itemSystem, sizeof(ArkItemSystem)))
        return;

    std::string reason;
    unsigned ownerId = 0;
    int width = 0;
    int height = 0;
    TryGuardedCall("native sideblob capture inventory owner", [inventory]() -> unsigned { return inventory->GetOwnerId(); }, ownerId, &reason);
    TryGuardedCall("native sideblob capture inventory width", [inventory]() -> int { return inventory->GetWidth(); }, width, &reason);
    TryGuardedCall("native sideblob capture inventory height", [inventory]() -> int { return inventory->GetHeight(); }, height, &reason);

    std::vector<ArkInventory::StorageCell> cells;
    if (!TryGuardedCall(
            "native sideblob capture inventory cells",
            [inventory]() -> std::vector<ArkInventory::StorageCell>
            {
                return inventory->m_storedItems;
            },
            cells,
            &reason))
    {
        return;
    }

    m_nativeSideBlobCapture.sawInventoryWrite = true;
    m_nativeSideBlobCapture.inventoryOwnerId = ownerId;
    m_nativeSideBlobCapture.inventoryWidth = width;
    m_nativeSideBlobCapture.inventoryHeight = height;
    m_nativeSideBlobCapture.items.clear();
    m_nativeSideBlobCapture.items.reserve(cells.size());
    m_nativeSideBlobCapture.checksum = kFnv1aOffsetBasis;
    uint32_t forwardedCellRefs = 0;

    auto mixChecksum = [this](uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            const uint8_t byte = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
            m_nativeSideBlobCapture.checksum ^= byte;
            m_nativeSideBlobCapture.checksum *= kFnv1aPrime;
        }
    };

    for (const ArkInventory::StorageCell& cell : cells)
    {
        if (cell.m_entityId == 0)
            continue;

        IArkItem* rawItem = nullptr;
        std::string itemReason;
        if (!TryGuardedCall(
                "native sideblob capture item lookup",
                [itemSystem, itemId = cell.m_entityId]() -> IArkItem*
                {
                    return FindArkItemDirectForSideBlob(itemSystem, itemId);
                },
                rawItem,
                &itemReason) ||
            !rawItem)
        {
            continue;
        }

        CArkItem* item = IsLikelyRuntimeCppObject(rawItem) ? static_cast<CArkItem*>(rawItem) : nullptr;
        if (!item || !IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
            continue;

        NativeCapturedItemState row;
        row.itemId = cell.m_entityId;
        row.itemPtr = reinterpret_cast<std::uintptr_t>(item);
        row.x = cell.m_x;
        row.y = cell.m_y;
        row.width = cell.m_width;
        row.height = cell.m_height;
        row.hasCell = true;

        TryGuardedVoidCall(
            "native sideblob capture item fields",
            [&row, item, &itemReason]()
            {
                row.archetypeId = item->m_selectedArchetype;
                row.count = item->m_count;
                row.ownerId = item->m_ownerId;
                row.category = static_cast<int>(item->m_category);
                row.flags =
                    (item->m_bFavorite ? 0x1u : 0u) |
                    (item->m_bJunk ? 0x2u : 0u) |
                    (item->m_bStackable ? 0x4u : 0u) |
                    (item->m_bIsGrenade ? 0x8u : 0u) |
                    (item->m_bIsUsable ? 0x10u : 0u) |
                    (item->m_bIsConsumable ? 0x20u : 0u) |
                    (item->m_bIsImportant ? 0x40u : 0u) |
                    (item->m_bPlotCritical ? 0x80u : 0u) |
                    (item->m_bIsUnlimited ? 0x100u : 0u);
                row.isWeapon = CoopItemClassification::IsRealWeaponInventoryItem(item, itemReason);
                if (row.isWeapon && IsLikelyRuntimeCppObject(static_cast<CArkWeapon*>(item), sizeof(CArkWeapon)))
                {
                    CArkWeapon* weapon = static_cast<CArkWeapon*>(item);
                    row.weaponCondition = weapon->m_condition;
                    row.weaponAmmoLoaded = weapon->m_numAmmoLoaded;
                    row.weaponModCount = static_cast<uint32_t>(weapon->m_weaponMods.m_weaponModIds.size());
                    for (const auto& mod : weapon->m_weaponMods.m_weaponModIds)
                    {
                        row.weaponMods.emplace_back(mod.first, mod.second);
                        row.weaponModTotalLevel += static_cast<uint32_t>(std::max(0, mod.second));
                    }
                }
            },
            &itemReason);

        mixChecksum(row.itemId);
        mixChecksum(row.archetypeId);
        mixChecksum(static_cast<uint64_t>(std::max(0, row.count)));
        mixChecksum(static_cast<uint64_t>(static_cast<uint32_t>(row.x)));
        mixChecksum(static_cast<uint64_t>(static_cast<uint32_t>(row.y)));
        mixChecksum(row.flags);
        mixChecksum(row.weaponModTotalLevel);
        for (const auto& mod : row.weaponMods)
        {
            mixChecksum(mod.first);
            mixChecksum(static_cast<uint64_t>(static_cast<uint32_t>(std::max(0, mod.second))));
        }

        m_gameStateLocalPlayerInventoryItemIds.insert(row.itemId);
        m_nativeFragmentLocator.OnInventoryCellEntityId(
            row.itemId,
            "ArkInventory::FullSerialize(local-player)",
            "Inventory/storedItems/i/v/entityId");
        ++forwardedCellRefs;

        m_nativeSideBlobCapture.items.push_back(row);
    }

    m_lastNativeSideBlobCapturedEvent =
        std::string("captured inventory from ") + (functionName ? functionName : "serialize") +
        " items=" + std::to_string(m_nativeSideBlobCapture.items.size()) +
        " refs=" + std::to_string(forwardedCellRefs) +
        " checksum=" + NativeSideBlobHex32(m_nativeSideBlobCapture.checksum);
}

void ModMain::CaptureNativeItemSideBlobSnapshot(CArkItem* item, const char* functionName, bool reading, int target, bool ok)
{
    if (!m_nativeSideBlobCapture.active || reading || target != eST_SaveGame || !ok)
        return;
    if (!item || !IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return;

    unsigned expectedOwner = m_nativeSideBlobCapture.inventoryOwnerId;
    if (expectedOwner == 0 && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity())
        expectedOwner = ArkPlayer::GetInstance().GetEntity()->GetId();

    unsigned ownerId = 0;
    std::string reason;
    if (!TryGuardedCall("native sideblob capture item owner", [item]() -> unsigned { return item->m_ownerId; }, ownerId, &reason))
        return;
    if (expectedOwner != 0 && ownerId != expectedOwner)
        return;

    m_nativeSideBlobCapture.sawItemWrite = true;
    ++m_nativeSideBlobCapture.itemSerializeHits;

    const std::uintptr_t itemPtr = reinterpret_cast<std::uintptr_t>(item);
    for (NativeCapturedItemState& row : m_nativeSideBlobCapture.items)
    {
        if (row.itemPtr != itemPtr)
            continue;

        TryGuardedVoidCall(
            "native sideblob update item from item serialize hook",
            [&row, item]()
            {
                row.count = item->m_count;
                row.ownerId = item->m_ownerId;
                row.flags =
                    (item->m_bFavorite ? 0x1u : 0u) |
                    (item->m_bJunk ? 0x2u : 0u) |
                    (item->m_bStackable ? 0x4u : 0u) |
                    (item->m_bIsGrenade ? 0x8u : 0u) |
                    (item->m_bIsUsable ? 0x10u : 0u) |
                    (item->m_bIsConsumable ? 0x20u : 0u) |
                    (item->m_bIsImportant ? 0x40u : 0u) |
                    (item->m_bPlotCritical ? 0x80u : 0u) |
                    (item->m_bIsUnlimited ? 0x100u : 0u);
            },
            &reason);
        break;
    }

    m_lastNativeSideBlobCapturedEvent =
        std::string("captured item hook from ") + (functionName ? functionName : "serialize") +
        " hits=" + std::to_string(m_nativeSideBlobCapture.itemSerializeHits);
}

void ModMain::FinalizeNativeSideBlobCaptureForPlayerState(const char* reason)
{
    const bool hasObservedCapture =
        m_nativeSideBlobCapture.active ||
        m_nativeSideBlobCapture.sawPlayerWrite ||
        m_nativeSideBlobCapture.sawInventoryWrite ||
        m_nativeSideBlobCapture.sawItemWrite;

    if (!hasObservedCapture && !m_nativeSideBlobCapture.hasNativeFragmentPayload)
        return;

    if (!m_nativeSideBlobCapture.sawPlayerWrite &&
        !m_nativeSideBlobCapture.sawInventoryWrite &&
        !m_nativeSideBlobCapture.hasNativeFragmentPayload)
    {
        m_lastNativeSideBlobCapturedEvent =
            std::string("capture ignored no player/inventory") +
            (reason && reason[0] ? ": " + std::string(reason) : "");
        return;
    }

    m_lastNativePlayerSaveCapture = m_nativeSideBlobCapture;
    m_lastNativePlayerSaveCapture.active = false;
    if (reason && reason[0])
        m_lastNativePlayerSaveCapture.reason = reason;
    ++m_nativePlayerSaveCaptureGeneration;

    m_lastNativeSideBlobCapturedEvent =
        "player-state native capture finalized items=" +
        std::to_string(m_lastNativePlayerSaveCapture.items.size()) +
        " hits=" + std::to_string(m_lastNativePlayerSaveCapture.itemSerializeHits) +
        " fragment=" + std::to_string(m_lastNativePlayerSaveCapture.hasNativeFragmentPayload ? 1 : 0) +
        "/" + std::to_string(m_lastNativePlayerSaveCapture.nativeFragmentPayloadBytes) +
        " checksum=" + NativeSideBlobHex32(m_lastNativePlayerSaveCapture.checksum) +
        " saw=" + NativeSideBlobBoolText(m_lastNativePlayerSaveCapture.sawPlayerWrite) +
        NativeSideBlobBoolText(m_lastNativePlayerSaveCapture.sawInventoryWrite) +
        NativeSideBlobBoolText(m_lastNativePlayerSaveCapture.sawItemWrite) +
        " captureReason=" + NativeSideBlobStatusToken(m_lastNativePlayerSaveCapture.reason) +
        (reason && reason[0] ? " finalizeReason=" + NativeSideBlobStatusToken(reason) : "");
    CoopRuntimeLog::Write(m_lastNativeSideBlobCapturedEvent);

    m_nativeSideBlobCapture = NativeSideBlobCaptureState();
    m_activeNativeFragmentCaptureRunId = 0;
}

bool ModMain::CaptureNativePlayerSnapshotWithoutSave(const char* reason)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player || !IsLikelyRuntimeCppObject(player, sizeof(ArkPlayer)))
    {
        m_lastNativeSideBlobCapturedEvent = "direct native capture failed: no player";
        return false;
    }

    BeginNativeSideBlobCapture(reason && reason[0] ? reason : "direct native player snapshot");
    CaptureNativePlayerSideBlobSnapshot(player, "direct-player-snapshot", false, eST_SaveGame, true);
    CaptureNativeInventorySideBlobSnapshot(player->m_pInventory, "direct-player-snapshot", false, eST_SaveGame, true);
    FinalizeNativeSideBlobCaptureForPlayerState(reason && reason[0] ? reason : "direct native player snapshot");

    const bool usable =
        m_lastNativePlayerSaveCapture.sawPlayerWrite ||
        m_lastNativePlayerSaveCapture.sawInventoryWrite;
    if (!usable)
    {
        m_lastNativeSideBlobCapturedEvent = "direct native capture failed: no player/inventory captured";
        CoopRuntimeLog::Write(m_lastNativeSideBlobCapturedEvent);
        return false;
    }

    CoopRuntimeLog::Write(
        "direct native player snapshot captured items=" +
        std::to_string(m_lastNativePlayerSaveCapture.items.size()) +
        " checksum=" + NativeSideBlobHex32(m_lastNativePlayerSaveCapture.checksum));
    return true;
}

bool ModMain::WriteCapturedNativeSideBlob(ISaveGame* saveGame, std::string& summary)
{
    summary.clear();
    if (!saveGame)
    {
        summary = "captured_write_failed=null_save";
        return false;
    }

    std::string reason;
    if (!TryWriteNativeSideBlobSection(
            saveGame,
            kNativeSideBlobCapturedPlayerSection,
            "coop native captured sideblob write",
            [this](TSerialize& serializer)
            {
                int version = 1;
                int sawPlayer = m_nativeSideBlobCapture.sawPlayerWrite ? 1 : 0;
                int sawInventory = m_nativeSideBlobCapture.sawInventoryWrite ? 1 : 0;
                int sawItem = m_nativeSideBlobCapture.sawItemWrite ? 1 : 0;
                string username = m_nativeSideBlobCapture.username.c_str();
                string level = m_nativeSideBlobCapture.levelName.c_str();
                string reasonText = m_nativeSideBlobCapture.reason.c_str();
                int worldEpoch = static_cast<int>(m_nativeSideBlobCapture.worldEpoch);
                int itemCount = static_cast<int>(std::min<size_t>(m_nativeSideBlobCapture.items.size(), 256));
                int itemHits = static_cast<int>(m_nativeSideBlobCapture.itemSerializeHits);
                uint32_t checksum = m_nativeSideBlobCapture.checksum;

                serializer.Value("version", version);
                serializer.Value("username", username);
                serializer.Value("level", level);
                serializer.Value("reason", reasonText);
                serializer.Value("worldEpoch", worldEpoch);
                serializer.Value("sawPlayer", sawPlayer);
                serializer.Value("sawInventory", sawInventory);
                serializer.Value("sawItem", sawItem);
                serializer.Value("itemCount", itemCount);
                serializer.Value("itemHits", itemHits);
                serializer.Value("checksum", checksum);

                float px = m_nativeSideBlobCapture.position.x;
                float py = m_nativeSideBlobCapture.position.y;
                float pz = m_nativeSideBlobCapture.position.z;
                float rw = m_nativeSideBlobCapture.rotation.w;
                float rx = m_nativeSideBlobCapture.rotation.v.x;
                float ry = m_nativeSideBlobCapture.rotation.v.y;
                float rz = m_nativeSideBlobCapture.rotation.v.z;
                float vw = m_nativeSideBlobCapture.viewRotation.w;
                float vx = m_nativeSideBlobCapture.viewRotation.v.x;
                float vy = m_nativeSideBlobCapture.viewRotation.v.y;
                float vz = m_nativeSideBlobCapture.viewRotation.v.z;
                float health = m_nativeSideBlobCapture.health;
                float maxHealth = m_nativeSideBlobCapture.maxHealth;
                float psi = m_nativeSideBlobCapture.psiPoints;
                float maxPsi = m_nativeSideBlobCapture.psiMaxPoints;
                int armor = m_nativeSideBlobCapture.armor;
                int maxArmor = m_nativeSideBlobCapture.maxArmor;
                int stance = m_nativeSideBlobCapture.stance;
                int selectedPsiPower = m_nativeSideBlobCapture.selectedPsiPower;
                int equippedPsiPower = m_nativeSideBlobCapture.equippedPsiPower;
                unsigned inventoryOwner = m_nativeSideBlobCapture.inventoryOwnerId;
                int inventoryWidth = m_nativeSideBlobCapture.inventoryWidth;
                int inventoryHeight = m_nativeSideBlobCapture.inventoryHeight;
                unsigned equipped = m_nativeSideBlobCapture.equippedWeaponId;
                unsigned lastEquipped = m_nativeSideBlobCapture.lastEquippedWeaponId;
                unsigned backup = m_nativeSideBlobCapture.backupWeaponId;
                unsigned toBeEquipped = m_nativeSideBlobCapture.toBeEquippedWeaponId;
                uint32_t weaponCount = m_nativeSideBlobCapture.weaponCount;
                uint32_t specialWeaponCount = m_nativeSideBlobCapture.specialWeaponCount;

                serializer.Value("px", px);
                serializer.Value("py", py);
                serializer.Value("pz", pz);
                serializer.Value("rw", rw);
                serializer.Value("rx", rx);
                serializer.Value("ry", ry);
                serializer.Value("rz", rz);
                serializer.Value("vw", vw);
                serializer.Value("vx", vx);
                serializer.Value("vy", vy);
                serializer.Value("vz", vz);
                serializer.Value("health", health);
                serializer.Value("maxHealth", maxHealth);
                serializer.Value("psi", psi);
                serializer.Value("maxPsi", maxPsi);
                serializer.Value("armor", armor);
                serializer.Value("maxArmor", maxArmor);
                serializer.Value("stance", stance);
                serializer.Value("selectedPsiPower", selectedPsiPower);
                serializer.Value("equippedPsiPower", equippedPsiPower);
                serializer.Value("inventoryOwner", inventoryOwner);
                serializer.Value("inventoryWidth", inventoryWidth);
                serializer.Value("inventoryHeight", inventoryHeight);
                serializer.Value("equipped", equipped);
                serializer.Value("lastEquipped", lastEquipped);
                serializer.Value("backup", backup);
                serializer.Value("toBeEquipped", toBeEquipped);
                serializer.Value("weaponCount", weaponCount);
                serializer.Value("specialWeaponCount", specialWeaponCount);

                for (int i = 0; i < itemCount; ++i)
                {
                    const NativeCapturedItemState& row = m_nativeSideBlobCapture.items[static_cast<size_t>(i)];
                    const std::string prefix = "item" + std::to_string(i) + "_";
                    unsigned itemId = row.itemId;
                    uint64_t archetype = row.archetypeId;
                    int count = row.count;
                    unsigned owner = row.ownerId;
                    int x = row.x;
                    int y = row.y;
                    int width = row.width;
                    int height = row.height;
                    int category = row.category;
                    uint32_t flags = row.flags;
                    int isWeapon = row.isWeapon ? 1 : 0;
                    float condition = row.weaponCondition;
                    int ammoLoaded = row.weaponAmmoLoaded;
                    uint32_t modCount = row.weaponModCount;
                    uint32_t modTotal = row.weaponModTotalLevel;

                    serializer.Value((prefix + "id").c_str(), itemId);
                    serializer.Value((prefix + "arch").c_str(), archetype);
                    serializer.Value((prefix + "count").c_str(), count);
                    serializer.Value((prefix + "owner").c_str(), owner);
                    serializer.Value((prefix + "x").c_str(), x);
                    serializer.Value((prefix + "y").c_str(), y);
                    serializer.Value((prefix + "w").c_str(), width);
                    serializer.Value((prefix + "h").c_str(), height);
                    serializer.Value((prefix + "category").c_str(), category);
                    serializer.Value((prefix + "flags").c_str(), flags);
                    serializer.Value((prefix + "isWeapon").c_str(), isWeapon);
                    serializer.Value((prefix + "condition").c_str(), condition);
                    serializer.Value((prefix + "ammoLoaded").c_str(), ammoLoaded);
                    serializer.Value((prefix + "modCount").c_str(), modCount);
                    serializer.Value((prefix + "modTotal").c_str(), modTotal);
                }
            },
            &reason))
    {
        summary = "captured_write_failed=" + (reason.empty() ? std::string("serializer_not_ok") : reason);
        return false;
    }

    ++m_nativeSideBlobCapturedWriteSections;
    m_nativeSideBlobCapturedItemWrites += static_cast<uint32_t>(m_nativeSideBlobCapture.items.size());
    summary =
        " capturedSection=1 capturedItems=" + std::to_string(m_nativeSideBlobCapture.items.size()) +
        " capturedHits=" + std::to_string(m_nativeSideBlobCapture.itemSerializeHits) +
        " capturedFragment=" + std::to_string(m_nativeSideBlobCapture.hasNativeFragmentPayload ? 1 : 0) +
        "/" + std::to_string(m_nativeSideBlobCapture.nativeFragmentPayloadBytes) +
        " capturedChecksum=" + NativeSideBlobHex32(m_nativeSideBlobCapture.checksum) +
        " capturedSaw=" + NativeSideBlobBoolText(m_nativeSideBlobCapture.sawPlayerWrite) +
        NativeSideBlobBoolText(m_nativeSideBlobCapture.sawInventoryWrite) +
        NativeSideBlobBoolText(m_nativeSideBlobCapture.sawItemWrite);
    m_lastNativeSideBlobCapturedEvent = summary;
    return true;
}

bool ModMain::ReadCapturedNativeSideBlob(ILoadGame* loadGame, std::string& summary)
{
    summary.clear();
    if (!loadGame)
    {
        summary = "captured_read_failed=null_load";
        return false;
    }

    std::string reason;
    bool haveSection = false;
    if (!TryGuardedCall(
            "coop native captured sideblob HaveSection",
            [&]() -> bool
            {
                return loadGame->HaveSection(kNativeSideBlobCapturedPlayerSection);
            },
            haveSection,
            &reason))
    {
        summary = "captured_read_failed=" + reason;
        return false;
    }

    if (!haveSection)
    {
        summary = " capturedSection=0";
        return true;
    }

    std::unique_ptr<TSerialize> section;
    if (!TryGuardedCall(
            "coop native captured sideblob GetSection",
            [&]() -> std::unique_ptr<TSerialize>
            {
                std::unique_ptr<TSerialize> result;
                loadGame->GetSection(&result, kNativeSideBlobCapturedPlayerSection);
                return result;
            },
            section,
            &reason) ||
        !section)
    {
        summary = "captured_read_failed=" + (reason.empty() ? std::string("null_section") : reason);
        return false;
    }

    bool readOk = false;
    int version = 0;
    int itemCount = 0;
    int itemHits = 0;
    uint32_t checksum = 0;
    int sawPlayer = 0;
    int sawInventory = 0;
    int sawItem = 0;
    if (!TryGuardedCall(
            "coop native captured sideblob read",
            [&]() -> bool
            {
                section->Value("version", version);
                section->Value("sawPlayer", sawPlayer);
                section->Value("sawInventory", sawInventory);
                section->Value("sawItem", sawItem);
                section->Value("itemCount", itemCount);
                section->Value("itemHits", itemHits);
                section->Value("checksum", checksum);
                itemCount = std::clamp(itemCount, 0, 256);
                for (int i = 0; i < itemCount; ++i)
                {
                    const std::string prefix = "item" + std::to_string(i) + "_";
                    unsigned itemId = 0;
                    uint64_t archetype = 0;
                    int count = 0;
                    section->Value((prefix + "id").c_str(), itemId);
                    section->Value((prefix + "arch").c_str(), archetype);
                    section->Value((prefix + "count").c_str(), count);
                }
                return section->Ok();
            },
            readOk,
            &reason) ||
        !readOk)
    {
        summary = "captured_read_failed=" + (reason.empty() ? std::string("serializer_not_ok") : reason);
        return false;
    }

    ++m_nativeSideBlobCapturedReadSections;
    m_nativeSideBlobCapturedItemReads += static_cast<uint32_t>(itemCount);
    summary =
        " capturedSection=1 capturedVersion=" + std::to_string(version) +
        " capturedItems=" + std::to_string(itemCount) +
        " capturedHits=" + std::to_string(itemHits) +
        " capturedChecksum=" + NativeSideBlobHex32(checksum) +
        " capturedSaw=" + std::to_string(sawPlayer) + std::to_string(sawInventory) + std::to_string(sawItem);
    m_lastNativeSideBlobCapturedEvent = summary;
    return true;
}
