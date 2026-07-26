#include "ModMain.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include "CoopItemClassification.h"
#include "CoopNativeFragmentPayload.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/variant/get.hpp>

#include <Chairloader/IChairLogger.h>
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/IConsole.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ArkInventory.h>
#include <Prey/GameDll/ark/ArkGame.h>
#include <Prey/GameDll/ark/ArkEquipmentMod.h>
#include <Prey/GameDll/ark/ArkItemSystem.h>
#include <Prey/GameDll/ark/iface/IArkItem.h>
#include <Prey/GameDll/arkitem.h>
#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/GameDll/ark/player/ArkPsiComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerRadiationComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerOxygenComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerStatusComponent.h>
#include <Prey/GameDll/ark/player/ArkQuickSelectComponent.h>
#include <Prey/GameDll/ark/player/trauma/ArkTraumaBase.h>
#include <Prey/GameDll/ark/player/ability/ArkAbilityComponent.h>
#include <Prey/GameDll/ark/player/ability/ArkAbilityData.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerComponent.h>
#include <Prey/GameDll/ark/turret/ArkTurret.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;

constexpr float kPlayerSidecarApplyRetrySeconds = 0.25f;
constexpr float kPlayerSidecarInventoryRestoreRetrySeconds = 1.0f;
constexpr float kPlayerSidecarInitialApplyDelaySeconds = 1.5f;
constexpr uint32_t kPlayerSidecarFlagDowned = 1u << 0;
constexpr uint32_t kPlayerInventoryItemFlagFavorite = 1u << 0;
constexpr uint32_t kPlayerInventoryItemFlagJunk = 1u << 1;
constexpr uint32_t kPlayerInventoryItemFlagPlotCritical = 1u << 2;
constexpr uint32_t kPlayerInventoryItemFlagStackable = 1u << 3;
constexpr uint32_t kPlayerInventoryItemFlagWeapon = 1u << 4;
constexpr uint32_t kPlayerInventoryItemFlagUsable = 1u << 5;
constexpr uint32_t kPlayerInventoryItemFlagConsumable = 1u << 6;
constexpr uint32_t kPlayerInventoryItemFlagNativeMeta = 1u << 7;
constexpr float kPlayerSidecarPositionApplyEpsilon = 0.035f;
constexpr float kPlayerSidecarRotationApplyEpsilonRadians = 0.005f;
constexpr float kPlayerSidecarViewRotationApplyEpsilonRadians = 0.0035f;
constexpr std::string_view kPlayerSidecarIntegrityPrefix = "integrityHash=";
constexpr size_t kNativeFragmentPayloadHexBytesPerLine = 384;
constexpr size_t kNativeSnapshotSaveHexBytesPerLine = 512;

void LogCoop(std::string_view msg)
{
    CoopRuntimeLog::Write(msg);
}

bool TraceInventoryRestoreEnabled()
{
    return CoopRuntimeConfig::Flag("COOP_TRACE_INVENTORY_RESTORE");
}

bool IsGameReady()
{
    return gEnv && gEnv->pEntitySystem && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity();
}

uint32_t HashPlayerSidecarPayload(std::string_view payload)
{
    uint32_t hash = 2166136261u;
    for (const char ch : payload)
    {
        hash ^= static_cast<uint8_t>(ch);
        hash *= 16777619u;
    }

    return hash;
}

uint32_t HashPlayerSidecarBytes(const std::vector<uint8_t>& bytes)
{
    uint32_t hash = 2166136261u;
    for (const uint8_t value : bytes)
    {
        hash ^= value;
        hash *= 16777619u;
    }

    return hash;
}

std::string HexPlayerSidecarHash(uint32_t hash)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << hash;
    return output.str();
}

std::string HexPlayerAccountToken(uint64_t token)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::hex << std::nouppercase << std::setw(16) << std::setfill('0') << token;
    return output.str();
}

bool ParsePlayerSidecarHash(std::string_view text, uint32_t& outHash)
{
    if (text.empty() || text.size() > 8)
        return false;

    uint32_t value = 0;
    for (const char ch : text)
    {
        value <<= 4;
        if (ch >= '0' && ch <= '9')
            value |= static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            value |= static_cast<uint32_t>(10 + ch - 'a');
        else if (ch >= 'A' && ch <= 'F')
            value |= static_cast<uint32_t>(10 + ch - 'A');
        else
            return false;
    }

    outHash = value;
    return true;
}

char HexNibble(uint8_t value)
{
    value &= 0x0Fu;
    return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
}

int DecodeHexNibble(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

std::string EncodeHexBytes(const std::vector<uint8_t>& bytes, size_t offset, size_t count)
{
    if (offset >= bytes.size())
        return {};

    count = std::min(count, bytes.size() - offset);
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t value = bytes[offset + i];
        out.push_back(HexNibble(static_cast<uint8_t>(value >> 4)));
        out.push_back(HexNibble(value));
    }
    return out;
}

bool AppendHexBytes(std::string_view text, std::vector<uint8_t>& bytes)
{
    int high = -1;
    for (char ch : text)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
            continue;

        const int value = DecodeHexNibble(ch);
        if (value < 0)
            return false;

        if (high < 0)
        {
            high = value;
        }
        else
        {
            bytes.push_back(static_cast<uint8_t>((high << 4) | value));
            high = -1;
        }
    }

    return high < 0;
}

bool ReadAndValidatePlayerSidecarFile(
    const std::string& pathString,
    std::string& outPayload,
    bool& outHasIntegrityHash,
    std::string& outReason)
{
    outPayload.clear();
    outHasIntegrityHash = false;
    outReason.clear();

    std::ifstream input(pathString, std::ios::binary);
    if (!input)
    {
        outReason = "open failed";
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof())
    {
        outReason = "read failed";
        return false;
    }

    std::string fileText = buffer.str();
    if (fileText.empty())
    {
        outReason = "empty file";
        return false;
    }

    size_t payloadStart = 0;
    size_t firstLineEnd = fileText.find('\n');
    if (firstLineEnd == std::string::npos)
        firstLineEnd = fileText.size();

    std::string_view firstLine(fileText.data(), firstLineEnd);
    if (!firstLine.empty() && firstLine.back() == '\r')
        firstLine.remove_suffix(1);

    if (firstLine.rfind(kPlayerSidecarIntegrityPrefix, 0) == 0)
    {
        outHasIntegrityHash = true;

        uint32_t expectedHash = 0;
        const std::string_view hashText = firstLine.substr(kPlayerSidecarIntegrityPrefix.size());
        if (!ParsePlayerSidecarHash(hashText, expectedHash))
        {
            outReason = "invalid hash header";
            return false;
        }

        payloadStart = firstLineEnd < fileText.size() ? firstLineEnd + 1 : firstLineEnd;
        outPayload = fileText.substr(payloadStart);
        const uint32_t actualHash = HashPlayerSidecarPayload(outPayload);
        if (actualHash != expectedHash)
        {
            outReason =
                "hash mismatch expected=" + HexPlayerSidecarHash(expectedHash) +
                " actual=" + HexPlayerSidecarHash(actualHash);
            return false;
        }

        outReason = "hash ok";
        return true;
    }

    const auto hasField = [&fileText](std::string_view field)
    {
        if (std::string_view(fileText).rfind(field, 0) == 0)
            return true;
        const std::string linePrefix = "\n" + std::string(field);
        return fileText.find(linePrefix) != std::string::npos;
    };
    if (!hasField("version=1") ||
        !hasField("username=") ||
        !hasField("health=") ||
        !hasField("inventoryCount="))
    {
        outReason = "legacy malformed";
        return false;
    }

    outPayload = std::move(fileText);
    outReason = "legacy no hash";
    return true;
}

bool WritePlayerSidecarPayloadWithHash(const std::filesystem::path& path, const std::string& payload)
{
    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.imbue(std::locale::classic());
    output << "integrityHash=" << HexPlayerSidecarHash(HashPlayerSidecarPayload(payload)) << "\n";
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.close();
    if (!output)
        return false;

    std::error_code error;
    std::filesystem::rename(tempPath, path, error);
    if (!error)
        return true;

    error.clear();
    std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, error);
    std::error_code removeError;
    std::filesystem::remove(tempPath, removeError);
    return !error;
}

bool ShouldApplyPlayerSidecarTransform(const IEntity& entity, const Vec3& position, const Quat& rotation)
{
    return !Vec3::IsEquivalent(entity.GetWorldPos(), position, kPlayerSidecarPositionApplyEpsilon) ||
        !Quat::IsEquivalent(entity.GetWorldRotation(), rotation, kPlayerSidecarRotationApplyEpsilonRadians);
}

bool ShouldApplyPlayerSidecarViewRotation(const ArkPlayer& player, const Quat& rotation)
{
    return !Quat::IsEquivalent(player.GetViewRotation(), rotation, kPlayerSidecarViewRotationApplyEpsilonRadians);
}

std::filesystem::path GetPreyProfileRoot()
{
    const char* userProfile = std::getenv("USERPROFILE");
    if (!userProfile || !userProfile[0])
        return {};

    std::filesystem::path root(userProfile);
    root /= "Saved Games";
    root /= "Arkane Studios";
    root /= "Prey";
    return root;
}

std::filesystem::path GetPreySaveGamesRoot()
{
    std::filesystem::path root = GetPreyProfileRoot();
    if (root.empty())
        return {};

    root /= "SaveGames";
    return root;
}

std::filesystem::path GetCoopPlayerStateRoot()
{
    const std::filesystem::path profileRoot = GetPreyProfileRoot();
    if (profileRoot.empty())
        return {};

    return profileRoot / "CoopPrototype" / "PlayerState";
}

std::filesystem::path GetLegacyCoopPlayerStateRoot()
{
    const std::filesystem::path saveRoot = GetPreySaveGamesRoot();
    if (saveRoot.empty())
        return {};

    return saveRoot / "_CoopPrototype";
}

std::string SanitizePathComponent(std::string value)
{
    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '-' && ch != '_')
            ch = '_';
    }

    while (!value.empty() && value.front() == '_')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '_')
        value.pop_back();

    return value.empty() ? std::string("Player") : value;
}

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string AfterPrefix(std::string_view value, std::string_view prefix)
{
    if (!StartsWith(value, prefix))
        return {};
    return std::string(value.substr(prefix.size()));
}

std::istringstream MakeClassicInputStream(std::string_view value)
{
    std::istringstream stream{ std::string(value) };
    stream.imbue(std::locale::classic());
    return stream;
}

ArkInventory* GetLocalArkInventory()
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    if (!player)
        return nullptr;

    return player->m_pInventory;
}

ArkItemSystem* GetArkItemSystemPtr()
{
    ArkGame* arkGame = ArkGame::GetArkGame();
    if (!arkGame)
        return nullptr;

    return &arkGame->GetArkItemSystem();
}

void AppendDetail(std::string& detail, const std::string& part);

IArkItem* FindArkItemDirect(ArkItemSystem* itemSystem, unsigned itemId)
{
    if (!itemSystem || itemId == 0)
        return nullptr;

    const auto it = itemSystem->m_items.find(itemId);
    if (it == itemSystem->m_items.end())
        return nullptr;
    return it->second;
}

struct PassiveInventoryCell
{
    unsigned itemId = 0;
    int x = -1;
    int y = -1;
    int width = -1;
    int height = -1;
};

struct PassiveInventoryItemState
{
    uint64_t archetypeId = 0;
    int count = 0;
    int x = -1;
    int y = -1;
    unsigned itemId = 0;
    int width = -1;
    int height = -1;
    uint32_t flags = 0;
    int category = -1;
    bool isWeapon = false;
    float weaponCondition = 0.0f;
    int weaponAmmoLoaded = 0;
    int weaponAmmoCount = 0;
    uint32_t weaponModCount = 0;
    uint32_t weaponModTotalLevel = 0;
    std::vector<std::pair<uint64_t, int>> weaponMods;
};

bool TryReadInventoryStoredCells(
    ArkInventory& inventory,
    std::vector<PassiveInventoryCell>& outCells,
    std::string& reason)
{
    outCells.clear();
    bool readCells = false;
    if (!TryGuardedCall(
            "coop inventory passive stored cells",
            [&]() -> bool
            {
                outCells.reserve(inventory.m_storedItems.size());
                for (const ArkInventory::StorageCell& cell : inventory.m_storedItems)
                {
                    PassiveInventoryCell row;
                    row.itemId = cell.m_entityId;
                    row.x = cell.m_x;
                    row.y = cell.m_y;
                    row.width = cell.m_width;
                    row.height = cell.m_height;
                    outCells.push_back(row);
                }
                return true;
            },
            readCells,
            &reason))
    {
        return false;
    }

    return readCells;
}

bool TryReadPassiveItemState(
    ArkItemSystem& itemSystem,
    const PassiveInventoryCell& cell,
    PassiveInventoryItemState& outState,
    std::string& reason)
{
    outState = PassiveInventoryItemState();
    if (cell.itemId == 0)
    {
        reason = "empty item id";
        return false;
    }

    IArkItem* item = nullptr;
    if (!TryGuardedCall(
            "coop sidecar direct item registry lookup",
            [&]() -> IArkItem*
            {
                return FindArkItemDirect(&itemSystem, cell.itemId);
            },
            item,
            &reason) ||
        !item)
    {
        if (reason.empty())
            reason = "item registry returned null";
        return false;
    }

    CArkItem* concreteItem = IsLikelyRuntimeCppObject(item) ? static_cast<CArkItem*>(item) : nullptr;
    if (!concreteItem || !IsLikelyRuntimeCppObject(concreteItem, sizeof(CArkItem)))
    {
        reason = "item object unreadable";
        return false;
    }

    bool readItem = false;
    if (!TryGuardedCall(
            "coop sidecar passive CArkItem fields",
            [&]() -> bool
            {
                outState.itemId = cell.itemId;
                outState.archetypeId = concreteItem->m_selectedArchetype;
                outState.count = concreteItem->m_count;
                outState.x = cell.x;
                outState.y = cell.y;
                outState.width = cell.width >= 0 ? cell.width : concreteItem->m_inventoryWidth;
                outState.height = cell.height >= 0 ? cell.height : concreteItem->m_inventoryHeight;
                outState.category = static_cast<int>(concreteItem->m_category);
                outState.flags = 0;
                if (concreteItem->m_bFavorite)
                    outState.flags |= kPlayerInventoryItemFlagFavorite;
                if (concreteItem->m_bJunk)
                    outState.flags |= kPlayerInventoryItemFlagJunk;
                if (concreteItem->m_bPlotCritical)
                    outState.flags |= kPlayerInventoryItemFlagPlotCritical;
                if (concreteItem->m_bStackable)
                    outState.flags |= kPlayerInventoryItemFlagStackable;
                if (CoopItemClassification::IsRealWeaponInventoryItem(concreteItem, reason))
                    outState.flags |= kPlayerInventoryItemFlagWeapon;
                if (concreteItem->m_bIsUsable)
                    outState.flags |= kPlayerInventoryItemFlagUsable;
                if (concreteItem->m_bIsConsumable)
                    outState.flags |= kPlayerInventoryItemFlagConsumable;
                outState.isWeapon = (outState.flags & kPlayerInventoryItemFlagWeapon) != 0;
                if (outState.isWeapon &&
                    IsLikelyRuntimeCppObject(static_cast<CArkWeapon*>(concreteItem), sizeof(CArkWeapon)))
                {
                    CArkWeapon* weapon = static_cast<CArkWeapon*>(concreteItem);
                    outState.weaponCondition = weapon->m_condition;
                    outState.weaponAmmoLoaded = weapon->m_numAmmoLoaded;
                    outState.weaponMods.reserve(weapon->m_weaponMods.m_weaponModIds.size());
                    for (const auto& mod : weapon->m_weaponMods.m_weaponModIds)
                    {
                        outState.weaponMods.emplace_back(mod.first, mod.second);
                        if (mod.second > 0)
                            outState.weaponModTotalLevel += static_cast<uint32_t>(mod.second);
                    }
                    outState.weaponModCount = static_cast<uint32_t>(
                        std::min<size_t>(outState.weaponMods.size(), UINT32_MAX));
                }
                return true;
            },
            readItem,
            &reason) ||
        !readItem)
    {
        if (reason.empty())
            reason = "passive item read failed";
        return false;
    }

    if (outState.archetypeId == 0 || outState.count <= 0)
    {
        reason = "invalid archetype/count";
        return false;
    }

    return true;
}

ModMain::PlayerInventoryItemState BuildInventoryStateFromNativeItem(const NativeCapturedItemState& nativeItem)
{
    ModMain::PlayerInventoryItemState item;
    item.archetypeId = nativeItem.archetypeId;
    item.count = std::max(1, nativeItem.count);
    item.x = nativeItem.x;
    item.y = nativeItem.y;
    item.itemId = nativeItem.itemId;
    item.width = nativeItem.width;
    item.height = nativeItem.height;
    item.category = nativeItem.category;
    item.isWeapon = nativeItem.isWeapon &&
        !CoopItemClassification::IsKnownAmmoPickupArchetype(nativeItem.archetypeId);
    item.weaponCondition = nativeItem.weaponCondition;
    item.weaponAmmoLoaded = nativeItem.weaponAmmoLoaded;
    item.weaponAmmoCount = nativeItem.weaponAmmoCount;
    item.weaponModCount = nativeItem.weaponModCount;
    item.weaponModTotalLevel = nativeItem.weaponModTotalLevel;
    item.weaponMods = nativeItem.weaponMods;
    item.flags = kPlayerInventoryItemFlagNativeMeta;
    if ((nativeItem.flags & 0x1u) != 0)
        item.flags |= kPlayerInventoryItemFlagFavorite;
    if ((nativeItem.flags & 0x2u) != 0)
        item.flags |= kPlayerInventoryItemFlagJunk;
    if ((nativeItem.flags & 0x4u) != 0)
        item.flags |= kPlayerInventoryItemFlagStackable;
    if ((nativeItem.flags & 0x10u) != 0)
        item.flags |= kPlayerInventoryItemFlagUsable;
    if ((nativeItem.flags & 0x20u) != 0)
        item.flags |= kPlayerInventoryItemFlagConsumable;
    if ((nativeItem.flags & 0x80u) != 0)
        item.flags |= kPlayerInventoryItemFlagPlotCritical;
    if (item.isWeapon)
        item.flags |= kPlayerInventoryItemFlagWeapon;
    return item;
}

void NormalizePlayerSidecarInventoryFromNativeCapture(ModMain::PlayerSidecarState& state)
{
    if (!state.hasNativeCapture ||
        !state.nativeCapture.sawInventoryWrite ||
        state.nativeCapture.items.empty())
    {
        return;
    }

    std::vector<ModMain::PlayerInventoryItemState> nativeInventory;
    nativeInventory.reserve(state.nativeCapture.items.size());
    for (const NativeCapturedItemState& nativeItem : state.nativeCapture.items)
    {
        if (nativeItem.archetypeId == 0 || nativeItem.count <= 0)
            continue;
        nativeInventory.push_back(BuildInventoryStateFromNativeItem(nativeItem));
    }

    if (nativeInventory.empty())
        return;

    state.inventory.swap(nativeInventory);
    state.hasInventory = true;
}

bool IsSidecarStackableItem(const ModMain::PlayerInventoryItemState& item)
{
    return (item.flags & kPlayerInventoryItemFlagStackable) != 0 &&
        (item.flags & kPlayerInventoryItemFlagWeapon) == 0;
}

bool IsSidecarDistinctEntityItem(const ModMain::PlayerInventoryItemState& item)
{
    return !IsSidecarStackableItem(item) ||
        (item.flags & kPlayerInventoryItemFlagWeapon) != 0 ||
        item.count <= 1;
}

int GetSidecarRestoreGiveCount(const ModMain::PlayerInventoryItemState& item)
{
    if (IsSidecarDistinctEntityItem(item))
        return 1;
    return std::max(1, item.count);
}

void TryApplySidecarFlagsToRestoredItem(
    ArkItemSystem& itemSystem,
    unsigned itemId,
    const ModMain::PlayerInventoryItemState& sidecarItem,
    std::string& detail,
    std::string& reason)
{
    IArkItem* rawItem = nullptr;
    if (!TryGuardedCall(
            "coop restore flag item lookup",
            [&]() -> IArkItem*
            {
                return FindArkItemDirect(&itemSystem, itemId);
            },
            rawItem,
            &reason) ||
        !rawItem)
    {
        return;
    }

    CArkItem* item = IsLikelyRuntimeCppObject(rawItem) ? static_cast<CArkItem*>(rawItem) : nullptr;
    if (!item || !IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return;

    if (TryGuardedVoidCall(
            "coop restore apply item flags",
            [&]()
            {
                item->m_bFavorite = (sidecarItem.flags & kPlayerInventoryItemFlagFavorite) != 0;
                item->m_bJunk = (sidecarItem.flags & kPlayerInventoryItemFlagJunk) != 0;
                item->m_bPlotCritical = (sidecarItem.flags & kPlayerInventoryItemFlagPlotCritical) != 0;
            },
            &reason))
    {
        AppendDetail(detail, "flagsApplied item=" + std::to_string(itemId));
    }

    if (sidecarItem.isWeapon &&
        IsLikelyRuntimeCppObject(static_cast<CArkWeapon*>(item), sizeof(CArkWeapon)))
    {
        if (TryGuardedVoidCall(
                "coop restore apply weapon fields",
                [&]()
                {
                    CArkWeapon* weapon = static_cast<CArkWeapon*>(item);
                    weapon->m_condition = sidecarItem.weaponCondition;
                    weapon->m_numAmmoLoaded = sidecarItem.weaponAmmoLoaded;
                    weapon->m_weaponMods.Clear();
                    for (const auto& mod : sidecarItem.weaponMods)
                    {
                        const int level = std::clamp(mod.second, 0, 16);
                        for (int i = 0; i < level; ++i)
                            weapon->m_weaponMods.AddMod(mod.first);
                    }
                },
                &reason))
        {
            AppendDetail(detail,
                "weaponFieldsApplied item=" + std::to_string(itemId) +
                " condition=" + std::to_string(sidecarItem.weaponCondition) +
                " ammoLoaded=" + std::to_string(sidecarItem.weaponAmmoLoaded) +
                " mods=" + std::to_string(sidecarItem.weaponMods.size()));
        }
    }
}

void HideInventoryBackedWorldEntity(unsigned itemId, std::string& detail, std::string& reason)
{
    if (!gEnv || !gEnv->pEntitySystem || itemId == 0)
        return;

    CArkItem* item = nullptr;
    TryGuardedCall(
        "coop restore get inventory-backed item",
        [itemId]() -> CArkItem*
        {
            return CArkItem::GetItemFromEntityId(itemId);
        },
        item,
        &reason);
    if (item && CoopItemClassification::IsRealWeaponInventoryItem(item, reason))
    {
        // The inventory entity is also the live first-person weapon. Vanilla
        // owns its select/hide/show lifecycle; forcing entity visibility or
        // physics here leaves a valid weapon record with no usable weapon.
        AppendDetail(detail, "nativeWeaponPresentation=" + std::to_string(itemId));
        return;
    }

    IEntity* entity = nullptr;
    if (!TryGuardedCall(
            "coop restore get inventory-backed entity",
            [itemId]() -> IEntity*
            {
                return gEnv->pEntitySystem->GetEntity(itemId);
            },
            entity,
            &reason) ||
        !entity)
    {
        return;
    }

    bool hidden = false;
    if (TryGuardedCall(
            "coop restore hide inventory-backed entity",
            [&]() -> bool
            {
                entity->Hide(true);
                entity->Invisible(true);
                entity->EnablePhysics(false);
                return true;
            },
            hidden,
            &reason) &&
        hidden)
    {
        AppendDetail(detail, "hidInventoryWorldEntity=" + std::to_string(itemId));
    }
}

bool BuildPassiveInventorySnapshot(
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    std::vector<PassiveInventoryItemState>& outItems,
    std::string& reason)
{
    outItems.clear();

    std::vector<PassiveInventoryCell> cells;
    if (!TryReadInventoryStoredCells(inventory, cells, reason))
        return false;

    outItems.reserve(cells.size());
    std::string detail;
    for (const PassiveInventoryCell& cell : cells)
    {
        PassiveInventoryItemState itemState;
        std::string itemReason;
        if (!TryReadPassiveItemState(itemSystem, cell, itemState, itemReason))
        {
            if (!itemReason.empty())
                AppendDetail(detail, "item " + std::to_string(cell.itemId) + ": " + itemReason);
            continue;
        }

        outItems.push_back(itemState);
    }

    if (!detail.empty())
        reason = detail;
    return true;
}

bool TryReadEquipmentModArchetype(
    unsigned itemId,
    uint64_t& outArchetypeId,
    bool& outIsSuitMod,
    std::string& reason)
{
    outArchetypeId = 0;
    outIsSuitMod = false;
    if (itemId == 0)
    {
        reason = "empty chipset item id";
        return false;
    }

    ArkEquipmentMod* mod = nullptr;
    if (!TryGuardedCall(
            "coop chipset GetEquipmentModFromEntityId",
            [itemId]() -> ArkEquipmentMod*
            {
                return ArkEquipmentMod::GetEquipmentModFromEntityId(itemId);
            },
            mod,
            &reason) ||
        !mod)
    {
        if (reason.empty())
            reason = "chipset entity is not ArkEquipmentMod";
        return false;
    }

    bool read = false;
    if (!TryGuardedCall(
            "coop chipset passive fields",
            [&]() -> bool
            {
                CArkItem* item = static_cast<CArkItem*>(mod);
                if (!IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
                    return false;
                outArchetypeId = item->m_selectedArchetype;
                outIsSuitMod = mod->IsSuitMod();
                return true;
            },
            read,
            &reason) ||
        !read ||
        outArchetypeId == 0)
    {
        if (reason.empty())
            reason = "chipset fields unreadable";
        return false;
    }

    return true;
}

int FindInstalledChipsetSlot(const std::array<unsigned int, ArkEquipmentModComponent::k_maxInstalled>& installed, unsigned itemId)
{
    if (itemId == 0)
        return -1;

    for (int slot = 0; slot < ArkEquipmentModComponent::k_maxInstalled; ++slot)
    {
        if (installed[static_cast<size_t>(slot)] == itemId)
            return slot;
    }

    return -1;
}

bool SnapshotChipsetComponent(
    ArkEquipmentModComponent& component,
    int type,
    std::vector<ModMain::PlayerChipsetState>& outChipsets,
    std::string& reason)
{
    std::vector<unsigned int> owned;
    std::array<unsigned int, ArkEquipmentModComponent::k_maxInstalled> installed = {};
    bool snapshotRead = false;
    if (!TryGuardedCall(
            type == 0 ? "coop sidecar read suit chipsets" : "coop sidecar read scope chipsets",
            [&]() -> bool
            {
                owned = component.m_ownedChipsets;
                installed = component.m_installedChipsets;
                return true;
            },
            snapshotRead,
            &reason))
    {
        return false;
    }
    (void)snapshotRead;

    auto appendChipset = [&](unsigned itemId, int slot, bool installedSlot)
    {
        if (itemId == 0)
            return;

        if (std::find_if(
                outChipsets.begin(),
                outChipsets.end(),
                [type, itemId](const ModMain::PlayerChipsetState& row)
                {
                    return row.type == type && row.itemId == itemId;
                }) != outChipsets.end())
        {
            return;
        }

        uint64_t archetypeId = 0;
        bool isSuitMod = false;
        std::string chipsetReason;
        if (!TryReadEquipmentModArchetype(itemId, archetypeId, isSuitMod, chipsetReason))
        {
            AppendDetail(reason, "chipset " + std::to_string(itemId) + ": " + chipsetReason);
            return;
        }

        if ((type == 0) != isSuitMod)
        {
            AppendDetail(reason, "chipset type mismatch id=" + std::to_string(itemId));
            return;
        }

        ModMain::PlayerChipsetState state;
        state.type = type;
        state.archetypeId = archetypeId;
        state.itemId = itemId;
        state.slot = slot;
        state.installed = installedSlot;
        outChipsets.push_back(state);
    };

    for (const unsigned itemId : owned)
        appendChipset(itemId, FindInstalledChipsetSlot(installed, itemId), FindInstalledChipsetSlot(installed, itemId) >= 0);

    for (int slot = 0; slot < ArkEquipmentModComponent::k_maxInstalled; ++slot)
        appendChipset(installed[static_cast<size_t>(slot)], slot, installed[static_cast<size_t>(slot)] != 0);

    return true;
}

std::vector<ModMain::PlayerChipsetState> BuildPlayerChipsetSnapshot(ArkPlayer& player, std::string& reason)
{
    std::vector<ModMain::PlayerChipsetState> chipsets;
    SnapshotChipsetComponent(player.m_suitChipsetComponent, 0, chipsets, reason);
    SnapshotChipsetComponent(player.m_scopeChipsetComponent, 1, chipsets, reason);
    return chipsets;
}

bool TryRestoreInventoryItem(
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    EntityId playerEntityId,
    const ModMain::PlayerInventoryItemState& sidecarItem,
    std::string& reason);

bool TryFindCurrentChipsetItemByArchetype(
    ArkEquipmentModComponent& component,
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    uint64_t archetypeId,
    int type,
    const std::vector<unsigned int>& usedItemIds,
    unsigned& outItemId,
    std::string& reason)
{
    outItemId = 0;
    if (archetypeId == 0)
        return false;

    auto tryCandidate = [&](unsigned itemId) -> bool
    {
        if (itemId == 0 ||
            std::find(usedItemIds.begin(), usedItemIds.end(), itemId) != usedItemIds.end())
        {
            return false;
        }

        uint64_t chipsetArchetype = 0;
        bool isSuitMod = false;
        std::string itemReason;
        if (!TryReadEquipmentModArchetype(itemId, chipsetArchetype, isSuitMod, itemReason) ||
            chipsetArchetype != archetypeId ||
            ((type == 0) != isSuitMod))
        {
            return false;
        }

        outItemId = itemId;
        return true;
    };

    // Equipment mods leave the normal inventory when collected. Check the
    // native chipset component first so an already materialized item can be
    // reused instead of spawning a duplicate on each restore retry.
    for (const unsigned itemId : component.m_ownedChipsets)
    {
        if (tryCandidate(itemId))
            return true;
    }
    for (const unsigned itemId : component.m_installedChipsets)
    {
        if (tryCandidate(itemId))
            return true;
    }

    std::vector<PassiveInventoryCell> cells;
    if (!TryReadInventoryStoredCells(inventory, cells, reason))
        return false;

    for (const PassiveInventoryCell& cell : cells)
    {
        if (cell.itemId == 0 ||
            std::find(usedItemIds.begin(), usedItemIds.end(), cell.itemId) != usedItemIds.end())
        {
            continue;
        }

        PassiveInventoryItemState itemState;
        std::string itemReason;
        if (!TryReadPassiveItemState(itemSystem, cell, itemState, itemReason) ||
            itemState.archetypeId != archetypeId)
        {
            continue;
        }

        if (tryCandidate(cell.itemId))
            return true;
    }

    reason = "no current chipset item for archetype " + std::to_string(archetypeId);
    return false;
}

bool RestoreChipsetComponent(
    ArkEquipmentModComponent& component,
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    EntityId playerEntityId,
    const std::vector<ModMain::PlayerChipsetState>& desired,
    int type,
    uint32_t& applied,
    std::string& detail,
    std::string& reason)
{
    std::vector<unsigned int> usedItemIds;
    std::vector<std::pair<ModMain::PlayerChipsetState, unsigned int>> resolved;
    const size_t desiredForType = static_cast<size_t>(std::count_if(
        desired.begin(),
        desired.end(),
        [type](const ModMain::PlayerChipsetState& row)
        {
            return row.type == type && row.archetypeId != 0;
        }));
    for (const ModMain::PlayerChipsetState& chipset : desired)
    {
        if (chipset.type != type || chipset.archetypeId == 0)
            continue;

        unsigned currentItemId = 0;
        std::string findReason;
        if (!TryFindCurrentChipsetItemByArchetype(
                component,
                inventory,
                itemSystem,
                chipset.archetypeId,
                type,
                usedItemIds,
                currentItemId,
                findReason))
        {
            ModMain::PlayerInventoryItemState materialized;
            materialized.archetypeId = chipset.archetypeId;
            materialized.count = 1;
            materialized.x = -1;
            materialized.y = -1;

            std::string materializeReason;
            const bool materializedOk = TryRestoreInventoryItem(
                inventory,
                itemSystem,
                playerEntityId,
                materialized,
                materializeReason);
            findReason.clear();
            if (!materializedOk ||
                !TryFindCurrentChipsetItemByArchetype(
                    component,
                    inventory,
                    itemSystem,
                    chipset.archetypeId,
                    type,
                    usedItemIds,
                    currentItemId,
                    findReason))
            {
                AppendDetail(
                    detail,
                    "missingChipset arch=" + std::to_string(chipset.archetypeId) +
                    " materialize=" + (materializedOk ? std::string("ok") : std::string("failed")) +
                    (materializeReason.empty() ? std::string() : " " + materializeReason) +
                    (findReason.empty() ? std::string() : " " + findReason));
                continue;
            }

            AppendDetail(detail, "materializedChipset arch=" + std::to_string(chipset.archetypeId));
        }

        usedItemIds.push_back(currentItemId);
        resolved.emplace_back(chipset, currentItemId);
    }

    if (resolved.size() < desiredForType)
    {
        AppendDetail(detail, std::string(type == 0 ? "suit" : "scope") +
            " waiting resolved=" + std::to_string(resolved.size()) +
            "/" + std::to_string(desiredForType));
        return desiredForType == 0;
    }

    if (!TryGuardedVoidCall(
            type == 0 ? "coop restore reset suit chipsets" : "coop restore reset scope chipsets",
            [&]()
            {
                component.Reset();
            },
            &reason))
    {
        AppendDetail(detail, "reset failed: " + reason);
        return false;
    }

    for (const auto& row : resolved)
    {
        const unsigned itemId = row.second;
        TryGuardedVoidCall(
            type == 0 ? "coop restore add suit chipset" : "coop restore add scope chipset",
            [&]()
            {
                component.AddToInventory(itemId);
            },
            &reason);
    }

    std::vector<std::pair<ModMain::PlayerChipsetState, unsigned int>> installed = resolved;
    installed.erase(
        std::remove_if(
            installed.begin(),
            installed.end(),
            [](const auto& row)
            {
                return !row.first.installed || row.first.slot < 0;
            }),
        installed.end());
    std::sort(
        installed.begin(),
        installed.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.first.slot < rhs.first.slot;
        });

    for (const auto& row : installed)
    {
        ArkEquipmentMod* mod = nullptr;
        if (!TryGuardedCall(
                type == 0 ? "coop restore get suit chipset mod" : "coop restore get scope chipset mod",
                [&]() -> ArkEquipmentMod*
                {
                    return ArkEquipmentMod::GetEquipmentModFromEntityId(row.second);
                },
                mod,
                &reason) ||
            !mod)
        {
            AppendDetail(detail, "install missing id=" + std::to_string(row.second));
            continue;
        }

        if (TryGuardedVoidCall(
                type == 0 ? "coop restore install suit chipset" : "coop restore install scope chipset",
                [&]()
                {
                    component.InstallMod(*mod);
                },
                &reason))
        {
            ++applied;
        }
        else
        {
            AppendDetail(detail, "install failed id=" + std::to_string(row.second) + " " + reason);
        }
    }

    TryGuardedVoidCall(
        type == 0 ? "coop restore exact suit chipset slots" : "coop restore exact scope chipset slots",
        [&]()
        {
            component.m_installedChipsets.fill(0);
            for (const auto& row : installed)
            {
                const int slot = std::clamp(row.first.slot, 0, ArkEquipmentModComponent::k_maxInstalled - 1);
                component.m_installedChipsets[static_cast<size_t>(slot)] = row.second;
            }
        },
        &reason);

    AppendDetail(detail, std::string(type == 0 ? "suit" : "scope") +
        " desired=" + std::to_string(desiredForType) +
        " resolved=" + std::to_string(resolved.size()) +
        " installed=" + std::to_string(installed.size()));
    return true;
}

bool HasUsableNativeCaptureForSidecar(const NativeSideBlobCaptureState& capture)
{
    return capture.sawPlayerWrite || capture.sawInventoryWrite || capture.hasNativeFragmentPayload;
}

void WriteNativeCaptureSidecarSection(std::ostream& output, const NativeSideBlobCaptureState& capture)
{
    const int sawPlayer = capture.sawPlayerWrite ? 1 : 0;
    const int sawInventory = capture.sawInventoryWrite ? 1 : 0;
    const int sawItem = capture.sawItemWrite ? 1 : 0;
    const int itemCount = static_cast<int>(std::min<size_t>(capture.items.size(), 256));

    output << "nativeCaptureVersion=1\n";
    output << "nativeCaptureSaw=" << sawPlayer << ' ' << sawInventory << ' ' << sawItem << "\n";
    output << "nativeCaptureLevel=" << capture.levelName << "\n";
    output << "nativeCaptureWorldEpoch=" << capture.worldEpoch << "\n";
    output << "nativeCapturePosition=" << capture.position.x << ' ' << capture.position.y << ' ' << capture.position.z << "\n";
    output << "nativeCaptureRotation=" << capture.rotation.w << ' ' << capture.rotation.v.x << ' ' << capture.rotation.v.y << ' ' << capture.rotation.v.z << "\n";
    output << "nativeCaptureViewRotation=" << capture.viewRotation.w << ' ' << capture.viewRotation.v.x << ' ' << capture.viewRotation.v.y << ' ' << capture.viewRotation.v.z << "\n";
    output << "nativeCaptureVitals=" << capture.health << ' ' << capture.maxHealth << ' '
        << capture.armor << ' ' << capture.maxArmor << ' ' << capture.stance << "\n";
    output << "nativeCapturePsi=" << capture.psiPoints << ' '
        << capture.psiMaxPoints << ' '
        << capture.selectedPsiPower << ' '
        << capture.equippedPsiPower << "\n";
    output << "nativeCaptureInventory=" << capture.inventoryOwnerId << ' '
        << capture.inventoryWidth << ' '
        << capture.inventoryHeight << ' '
        << capture.checksum << ' '
        << itemCount << ' '
        << capture.itemSerializeHits << "\n";
    output << "nativeCaptureWeapons=" << capture.equippedWeaponId << ' '
        << capture.lastEquippedWeaponId << ' '
        << capture.backupWeaponId << ' '
        << capture.toBeEquippedWeaponId << ' '
        << capture.weaponCount << ' '
        << capture.specialWeaponCount << "\n";

    if (capture.hasNativeFragmentPayload && !capture.nativeFragmentPayload.empty())
    {
        output << "nativePlayerStatePayloadVersion=1\n";
        output << "nativePlayerStatePayloadKind=save-complete-fragment-v1\n";
        output << "legacySidecarDeprecated=1\n";
        output << "nativeFragmentPayload=" << capture.nativeFragmentPayloadVersion << ' '
            << capture.nativeFragmentPayloadBytes << ' '
            << capture.nativeFragmentPayloadChecksum << ' '
            << capture.nativeFragmentPayloadRawChecksum << ' '
            << capture.nativeFragmentPayloadRanges << ' '
            << capture.nativeFragmentPayloadInventoryRanges << ' '
            << capture.nativeFragmentPayloadItemGroups << ' '
            << capture.nativeFragmentPayloadItemRanges << ' '
            << capture.nativeFragmentPayloadRawBytes << ' '
            << capture.nativeFragmentPayloadRunId << ' '
            << capture.nativeFragmentPayloadSchemaHash << ' '
            << capture.nativeFragmentPayloadContentHash << "\n";

        for (size_t offset = 0; offset < capture.nativeFragmentPayload.size(); offset += kNativeFragmentPayloadHexBytesPerLine)
        {
            output << "nativeFragmentPayloadHex="
                << EncodeHexBytes(capture.nativeFragmentPayload, offset, kNativeFragmentPayloadHexBytesPerLine)
                << "\n";
        }
    }

    if (capture.hasNativeSnapshotSave && !capture.nativeSnapshotSave.empty())
    {
        output << "nativeSnapshotSave=" << capture.nativeSnapshotSaveBytes << ' '
            << capture.nativeSnapshotSaveChecksum << "\n";

        for (size_t offset = 0; offset < capture.nativeSnapshotSave.size(); offset += kNativeSnapshotSaveHexBytesPerLine)
        {
            output << "nativeSnapshotSaveHex="
                << EncodeHexBytes(capture.nativeSnapshotSave, offset, kNativeSnapshotSaveHexBytesPerLine)
                << "\n";
        }
    }

    for (int i = 0; i < itemCount; ++i)
    {
        const NativeCapturedItemState& item = capture.items[static_cast<size_t>(i)];
        output << "nativeItem=" << item.itemId << ' '
            << item.archetypeId << ' '
            << item.count << ' '
            << item.ownerId << ' '
            << item.x << ' '
            << item.y << ' '
            << item.width << ' '
            << item.height << ' '
            << item.category << ' '
            << item.flags << ' '
            << (item.isWeapon ? 1 : 0) << ' '
            << item.weaponCondition << ' '
            << item.weaponAmmoLoaded << ' '
            << item.weaponAmmoCount << ' '
            << item.weaponModCount << ' '
            << item.weaponModTotalLevel << "\n";
        for (const auto& mod : item.weaponMods)
        {
            output << "nativeWeaponMod=" << item.itemId << ' '
                << mod.first << ' '
                << mod.second << "\n";
        }
    }
}

bool TryGetInventoryArchetypeCount(ArkInventory& inventory, uint64_t archetypeId, int& outCount, std::string& reason)
{
    outCount = 0;
    if (archetypeId == 0)
        return true;

    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!itemSystem)
    {
        reason = "item system unavailable";
        return false;
    }

    std::vector<PassiveInventoryCell> cells;
    if (!TryReadInventoryStoredCells(inventory, cells, reason))
        return false;

    std::string detail;
    for (const PassiveInventoryCell& cell : cells)
    {
        PassiveInventoryItemState itemState;
        std::string itemReason;
        if (!TryReadPassiveItemState(*itemSystem, cell, itemState, itemReason))
        {
            if (!itemReason.empty())
                AppendDetail(detail, "item " + std::to_string(cell.itemId) + ": " + itemReason);
            continue;
        }

        if (itemState.archetypeId == archetypeId)
            outCount += std::max(0, itemState.count);
    }

    if (!detail.empty())
        reason = detail;
    return true;
}

bool TryGetInventoryItemIds(ArkInventory& inventory, std::vector<unsigned int>& outIds, std::string& reason)
{
    outIds.clear();
    std::vector<PassiveInventoryCell> cells;
    if (!TryReadInventoryStoredCells(inventory, cells, reason))
        return false;

    outIds.reserve(cells.size());
    for (const PassiveInventoryCell& cell : cells)
    {
        if (cell.itemId == 0)
            continue;
        if (std::find(outIds.begin(), outIds.end(), cell.itemId) == outIds.end())
            outIds.push_back(cell.itemId);
    }
    return true;
}

void AppendDetail(std::string& detail, const std::string& part)
{
    if (part.empty())
        return;
    if (!detail.empty())
        detail += " | ";
    detail += part;
}

std::string ArchetypeSummary(IEntityArchetype* archetype)
{
    if (!archetype)
        return "null";

    std::string reason;
    const char* rawName = nullptr;
    uint64_t id = 0;
    IEntityClass* entityClass = nullptr;
    const char* rawClassName = nullptr;

    TryGuardedCall("coop probe archetype GetName", [archetype]() { return archetype->GetName(); }, rawName, &reason);
    TryGuardedCall("coop probe archetype GetId", [archetype]() { return archetype->GetId(); }, id, &reason);
    if (TryGuardedCall("coop probe archetype GetClass", [archetype]() { return archetype->GetClass(); }, entityClass, &reason) && entityClass)
        TryGuardedCall("coop probe class GetName", [entityClass]() { return entityClass->GetName(); }, rawClassName, &reason);

    std::string summary = rawName && rawName[0] ? rawName : "?";
    summary += " id=" + std::to_string(id);
    summary += " class=";
    summary += rawClassName && rawClassName[0] ? rawClassName : "?";
    return summary;
}

bool TryInventoryContains(ArkInventory& inventory, unsigned itemId, bool& contains, std::string& reason)
{
    return TryGuardedCall(
        "coop inventory Contains",
        [&]() -> bool
        {
            return inventory.Contains(itemId);
        },
        contains,
        &reason);
}

std::vector<unsigned int> FindNewInventoryItemIds(
    const std::vector<unsigned int>& beforeIds,
    const std::vector<unsigned int>& afterIds)
{
    std::vector<unsigned int> newIds;
    for (const unsigned itemId : afterIds)
    {
        if (std::find(beforeIds.begin(), beforeIds.end(), itemId) == beforeIds.end())
            newIds.push_back(itemId);
    }
    return newIds;
}

bool TryGetArchetypePointer(uint64_t archetypeId, IEntityArchetype*& outArchetype, std::string& reason)
{
    outArchetype = nullptr;
    if (!gEnv || !gEnv->pEntitySystem)
    {
        reason = "entity system unavailable";
        return false;
    }

    return TryGuardedCall(
        "coop entity GetEntityArchetype",
        [&]() -> IEntityArchetype*
        {
            return gEnv->pEntitySystem->GetEntityArchetype(archetypeId);
        },
        outArchetype,
        &reason) && outArchetype;
}

bool TryBuildArchetypeCommandName(IEntityArchetype& archetype, std::string& outName, std::string& reason)
{
    const char* rawArchetypeName = nullptr;
    if (!TryGuardedCall(
            "coop entity archetype GetName",
            [&]() -> const char*
            {
                return archetype.GetName();
            },
            rawArchetypeName,
            &reason) ||
        !rawArchetypeName ||
        !rawArchetypeName[0])
    {
        return false;
    }

    outName = rawArchetypeName;
    if (outName.rfind("Ark", 0) == 0)
        return true;

    IEntityClass* entityClass = nullptr;
    if (!TryGuardedCall(
            "coop entity archetype GetClass",
            [&]() -> IEntityClass*
            {
                return archetype.GetClass();
            },
            entityClass,
            &reason) ||
        !entityClass)
    {
        return true;
    }

    const char* rawClassName = nullptr;
    if (!TryGuardedCall(
            "coop entity class GetName",
            [&]() -> const char*
            {
                return entityClass->GetName();
            },
            rawClassName,
            &reason) ||
        !rawClassName ||
        !rawClassName[0])
    {
        return true;
    }

    const std::string className = rawClassName;
    if (!StartsWith(outName, className + "."))
        outName = className + "." + outName;
    return true;
}

struct KnownPickupArchetypeInfo
{
    uint64_t id;
    const char* className;
    const char* archetypeName;
};

const KnownPickupArchetypeInfo* LookupKnownPickupArchetypeInfo(uint64_t archetypeId)
{
    static constexpr KnownPickupArchetypeInfo kKnownPickups[] = {
        { 10739735956144611816ULL, "ArkAmmoPickupGooGun", "ArkPickups.Ammo.GooGun" },
        { 10739735956144611817ULL, "ArkAmmoPickupHeavyLaser", "ArkPickups.Ammo.InstaLaser" },
        { 10739735956144611818ULL, "ArkAmmoPickupShells", "ArkPickups.Ammo.ShotgunShells" },
        { 10739735956144611825ULL, "ArkPsiHypo", "ArkPickups.Medical.PsiHypo" },
        { 10739735956144611826ULL, "ArkMedKit", "ArkPickups.Medical.MedKit" },
        { 10739735956144611827ULL, "ArkNeuroMod", "ArkPickups.Player.Neuromod" },
        { 10739735956144611849ULL, "ArkWeaponWrench", "ArkPickups.Weapons.Wrench" },
        { 10739735956144611850ULL, "ArkWeaponGooGun", "ArkPickups.Weapons.GooGun" },
        { 10739735956144611865ULL, "ArkItem", "ArkPickups.Misc.SpareParts" },
        { 10739735956144611866ULL, "ArkWeaponMod", "ArkPickups.Mods.Weapon.WeaponUpgradeKit" },
        { 10739735956144611892ULL, "ArkAmmoPickupBullets", "ArkPickups.Ammo.PistolBullets" },
        { 10739735956144611893ULL, "ArkWeaponPistol", "ArkPickups.Weapons.Pistol" },
        { 10739735956144611951ULL, "ArkWeaponStunGun", "ArkPickups.Weapons.StunGun" },
        { 10739735956144611978ULL, "ArkSuitPatch", "ArkPickups.Player.SuitPatchKit" },
        { 10739735956144611996ULL, "ArkAmmoPickupStunGun", "ArkPickups.Ammo.StunGunAmmo" },
        { 418270515501470348ULL, "ArkRecyclerJunk", "ArkPickups.Crafting.RecyclerJunk.CrumpledPaper" },
        { 418270515501877503ULL, "ArkRecyclerJunk", "ArkPickups.Crafting.RecyclerJunk.EmptyCan" },
    };

    for (const KnownPickupArchetypeInfo& entry : kKnownPickups)
    {
        if (entry.id == archetypeId)
            return &entry;
    }

    return nullptr;
}

std::vector<std::string> BuildArchetypeNameCandidates(const char* archetypeName)
{
    std::vector<std::string> candidates;
    if (!archetypeName || !archetypeName[0])
        return candidates;

    const std::string fullName = archetypeName;
    candidates.push_back(fullName);

    constexpr std::string_view pickupPrefix = "ArkPickups.";
    if (StartsWith(fullName, pickupPrefix))
        candidates.emplace_back(fullName.substr(pickupPrefix.size()));

    const size_t lastDot = fullName.find_last_of('.');
    if (lastDot != std::string::npos && lastDot + 1 < fullName.size())
        candidates.emplace_back(fullName.substr(lastDot + 1));

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

bool TryGetArkItemSystemArchetype(
    ArkItemSystem& itemSystem,
    const KnownPickupArchetypeInfo& info,
    IEntityArchetype*& outArchetype,
    std::string& reason)
{
    outArchetype = nullptr;
    std::string detail;
    for (const std::string& typeName : BuildArchetypeNameCandidates(info.archetypeName))
    {
        reason.clear();
        if (TryGuardedCall(
                "coop item GetArchetype class/name",
                [&]() -> IEntityArchetype*
                {
                    return itemSystem.GetArchetype(info.className, typeName.c_str());
                },
                outArchetype,
                &reason) &&
            outArchetype)
        {
            reason = "matched typeName=" + typeName;
            return true;
        }

        AppendDetail(detail, "GetArchetype " + std::string(info.className) + "/" + typeName +
            (reason.empty() ? " returned-null" : " failed: " + reason));
    }

    std::vector<IEntityArchetype*> classArchetypes;
    reason.clear();
    if (TryGuardedCall(
            "coop item GetArchetypesForClass",
            [&]() -> std::vector<IEntityArchetype*>
            {
                return itemSystem.GetArchetypesForClass(info.className);
            },
            classArchetypes,
            &reason))
    {
        for (IEntityArchetype* candidate : classArchetypes)
        {
            if (!candidate)
                continue;

            uint64_t candidateId = 0;
            std::string candidateReason;
            if (TryGuardedCall(
                    "coop item class candidate GetId",
                    [candidate]() -> uint64_t
                    {
                        return candidate->GetId();
                    },
                    candidateId,
                    &candidateReason) &&
                candidateId == info.id)
            {
                outArchetype = candidate;
                reason = "matched class list count=" + std::to_string(classArchetypes.size());
                return true;
            }
        }

        AppendDetail(detail, "GetArchetypesForClass count=" + std::to_string(classArchetypes.size()) + " no-id-match");
    }
    else
    {
        AppendDetail(detail, "GetArchetypesForClass failed: " + reason);
    }

    reason = detail.empty() ? "returned null" : detail;
    return false;
}

bool TryPlaceOrPackInventoryItems(
    ArkInventory& inventory,
    const std::vector<unsigned int>& itemIds,
    int x,
    int y,
    std::string& reason)
{
    bool allPlaced = true;
    bool placedFirst = false;
    for (const unsigned itemId : itemIds)
    {
        bool placed = false;
        if (!placedFirst && x >= 0 && y >= 0)
        {
            if (!TryGuardedCall(
                    "coop inventory TryPlaceItem",
                    [&]() -> bool
                    {
                        return inventory.TryPlaceItem(itemId, x, y);
                    },
                    placed,
                    &reason))
            {
                allPlaced = false;
                placed = false;
            }
            placedFirst = placed;
        }

        if (!placed &&
            !TryGuardedCall(
                "coop inventory PackItem",
                [&]() -> bool
                {
                    return inventory.PackItem(itemId);
                },
                placed,
                &reason))
        {
            allPlaced = false;
        }
    }

    return allPlaced;
}

bool TryPlaceOrPackInventoryItemsQuietly(
    ArkInventory& inventory,
    const std::vector<unsigned int>& itemIds,
    int,
    int,
    std::string& reason)
{
    bool allPlaced = true;
    for (const unsigned itemId : itemIds)
    {
        bool placed = false;
        // PlaceItemQuietly is not stable in the shipped Epic executable: its
        // generated SDK entry point consistently raises an access violation.
        // PackItem performs the same native cell insertion needed by restore;
        // the surrounding restore scope already suppresses pickup feedback.
        if (!TryGuardedCall(
                "coop inventory PackItem",
                [&]() -> bool
                {
                    return inventory.PackItem(itemId);
                },
                placed,
                &reason))
        {
            allPlaced = false;
        }
        else if (!placed)
        {
            allPlaced = false;
        }
    }

    return allPlaced;
}

bool TryAttachCreatedItemsToInventory(
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    EntityId ownerId,
    const std::vector<unsigned int>& itemIds,
    int requestedCount,
    int x,
    int y,
    std::string& detail,
    std::string& reason)
{
    if (itemIds.empty())
        return false;

    bool anyAttached = false;
    std::vector<unsigned int> packCandidates;
    packCandidates.reserve(itemIds.size());

    for (const unsigned itemId : itemIds)
    {
        std::string itemDetail = "item=" + std::to_string(itemId);

        IArkItem* arkItem = nullptr;
        if (TryGuardedCall(
                "coop restore direct item registry lookup",
                [&]() -> IArkItem*
                {
                    return FindArkItemDirect(&itemSystem, itemId);
                },
                arkItem,
                &reason) &&
            arkItem)
        {
            uint64_t itemArchetype = 0;
            int itemCount = -1;
            unsigned currentOwner = 0;
            CArkItem* readableItem = IsLikelyRuntimeCppObject(arkItem) ? static_cast<CArkItem*>(arkItem) : nullptr;
            if (readableItem && IsLikelyRuntimeCppObject(readableItem, sizeof(CArkItem)))
            {
                TryGuardedVoidCall(
                    "coop restore passive CArkItem fields",
                    [&]()
                    {
                        itemArchetype = readableItem->m_selectedArchetype;
                        itemCount = readableItem->m_count;
                        currentOwner = readableItem->m_ownerId;
                    },
                    &reason);
            }
            itemDetail += " arch=" + std::to_string(itemArchetype) +
                " count=" + std::to_string(itemCount) +
                " owner=" + std::to_string(currentOwner);
        }
        else
        {
            itemDetail += " no-IArkItem";
        }

        CArkItem* concreteItem = nullptr;
        if (TryGuardedCall(
                "coop item CArkItem::GetItemFromEntityId",
                [itemId]() -> CArkItem*
                {
                    return CArkItem::GetItemFromEntityId(itemId);
                },
                concreteItem,
                &reason) &&
            concreteItem)
        {
            bool gaveOwnerInventory = false;
            if (TryGuardedCall(
                    "coop item GiveOwner inventory",
                    [&]() -> bool
                    {
                        return concreteItem->GiveOwner(static_cast<IArkInventory*>(&inventory));
                    },
                    gaveOwnerInventory,
                    &reason))
            {
                itemDetail += std::string(" giveOwnerInv=") + (gaveOwnerInventory ? "1" : "0");
                anyAttached = gaveOwnerInventory || anyAttached;
            }

            int postGiveCount = -1;
            unsigned postGiveOwner = 0;
            TryGuardedVoidCall(
                "coop item post GiveOwner passive fields",
                [&]()
                {
                    postGiveCount = concreteItem->m_count;
                    postGiveOwner = concreteItem->m_ownerId;
                },
                &reason);
            itemDetail += " postCount=" + std::to_string(postGiveCount) + " postOwner=" + std::to_string(postGiveOwner);
        }
        else if (arkItem)
        {
            bool gaveOwner = false;
            if (TryGuardedCall(
                    "coop item GiveOwner picker",
                    [&]() -> bool
                    {
                        return arkItem->GiveOwner(ownerId);
                    },
                    gaveOwner,
                    &reason))
            {
                itemDetail += std::string(" giveOwnerPicker=") + (gaveOwner ? "1" : "0");
                anyAttached = gaveOwner || anyAttached;
            }
        }

        bool contains = false;
        if (TryInventoryContains(inventory, itemId, contains, reason))
            itemDetail += std::string(" containsBeforeAdd=") + (contains ? "1" : "0");

        if (!contains)
        {
            bool added = false;
            if (TryGuardedCall(
                    "coop inventory AddItem",
                    [&]() -> bool
                    {
                        return inventory.AddItem(itemId);
                    },
                    added,
                    &reason))
            {
                itemDetail += std::string(" addItem=") + (added ? "1" : "0");
                anyAttached = added || anyAttached;
            }
        }

        contains = false;
        if (TryInventoryContains(inventory, itemId, contains, reason))
            itemDetail += std::string(" containsAfterAdd=") + (contains ? "1" : "0");
        if (contains)
            packCandidates.push_back(itemId);

        AppendDetail(detail, itemDetail);
    }

    if (!packCandidates.empty())
    {
        const bool placed = TryPlaceOrPackInventoryItemsQuietly(inventory, packCandidates, x, y, reason);
        AppendDetail(detail, std::string("quietPlaceOrPack=") + (placed ? "1" : "0") +
            " requestedCount=" + std::to_string(requestedCount));
        anyAttached = placed || anyAttached;
    }

    return anyAttached;
}

bool TrySpawnAndGiveInventoryItem(
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    EntityId playerEntityId,
    IEntityArchetype& archetype,
    const ModMain::PlayerInventoryItemState& sidecarItem,
    const std::vector<unsigned int>& beforeIds,
    int beforeCount,
    bool haveBeforeCount,
    std::string& detail,
    std::string& reason)
{
    if (!gEnv || !gEnv->pEntitySystem)
    {
        AppendDetail(detail, "spawnGive failed: entity system unavailable");
        return false;
    }

    IEntity* playerEntity = nullptr;
    TryGuardedCall(
        "coop spawnGive get player entity",
        [&]() -> IEntity*
        {
            return gEnv->pEntitySystem->GetEntity(playerEntityId);
        },
        playerEntity,
        &reason);

    Vec3 spawnPosition = ZERO;
    Quat spawnRotation;
    spawnRotation.SetIdentity();
    if (playerEntity)
    {
        spawnPosition = playerEntity->GetWorldPos();
        spawnRotation = playerEntity->GetWorldRotation();
        spawnPosition.z += 0.35f;
    }

    SEntitySpawnParams params;
    params.sName = "CoopInventoryRestoreItem";
    params.pClass = archetype.GetClass();
    params.pArchetype = &archetype;
    params.nFlags = ENTITY_FLAG_NO_SAVE | ENTITY_FLAG_PROCEDURAL;
    params.vPosition = spawnPosition;
    params.qRotation = spawnRotation;

    IEntity* spawnedEntity = nullptr;
    const bool spawnCallOk = TryGuardedCall(
        "coop item SpawnEntityFromArchetype",
        [&]() -> IEntity*
        {
            return gEnv->pEntitySystem->SpawnEntityFromArchetype(&archetype, params, true);
        },
        spawnedEntity,
        &reason);
    if (!spawnCallOk || !spawnedEntity)
    {
        AppendDetail(detail, "spawnGive spawn failed");
        if (!reason.empty())
            AppendDetail(detail, reason);
        return false;
    }

    const EntityId spawnedId = spawnedEntity->GetId();
    AppendDetail(detail, "spawnGive id=" + std::to_string(spawnedId) +
        " arch=" + std::to_string(sidecarItem.archetypeId) +
        " savedCell=" + std::to_string(sidecarItem.x) + "," + std::to_string(sidecarItem.y) +
        " savedSize=" + std::to_string(sidecarItem.width) + "x" + std::to_string(sidecarItem.height) +
        (sidecarItem.isWeapon ? " weapon=1" : ""));

    auto cleanupSpawnIfNotInInventory = [&]()
    {
        bool spawnedInInventory = false;
        TryInventoryContains(inventory, spawnedId, spawnedInInventory, reason);
        if (spawnedInInventory)
        {
            HideInventoryBackedWorldEntity(spawnedId, detail, reason);
            return;
        }

        if (gEnv &&
            gEnv->pEntitySystem &&
            gEnv->pEntitySystem->GetEntity(spawnedId))
        {
            gEnv->pEntitySystem->RemoveEntity(spawnedId, true);
            AppendDetail(detail, "removedLooseSpawn=" + std::to_string(spawnedId));
        }
    };

    CArkItem* item = nullptr;
    if (!TryGuardedCall(
            "coop spawnGive CArkItem::GetItemFromEntityId",
            [spawnedId]() -> CArkItem*
            {
                return CArkItem::GetItemFromEntityId(spawnedId);
            },
            item,
            &reason) ||
        !item)
    {
        AppendDetail(detail, "spawnGive failed: spawned entity is not CArkItem");
        if (!reason.empty())
            AppendDetail(detail, reason);
        gEnv->pEntitySystem->RemoveEntity(spawnedId, true);
        return false;
    }

    bool canCollect = false;
    if (TryGuardedCall(
            "coop spawnGive CanCollect",
            [&]() -> bool
            {
                return item->CanCollect(static_cast<IArkInventory&>(inventory));
            },
            canCollect,
            &reason))
    {
        AppendDetail(detail, std::string("spawnGive canCollect=") + (canCollect ? "1" : "0"));
    }

    const int requestedCount = GetSidecarRestoreGiveCount(sidecarItem);
    if (!TryGuardedVoidCall(
            "coop spawnGive ResetCount",
            [&]()
            {
                item->ResetCount(requestedCount);
            },
            &reason))
    {
        AppendDetail(detail, "spawnGive ResetCount failed");
        if (!reason.empty())
            AppendDetail(detail, reason);
    }

    HideInventoryBackedWorldEntity(spawnedId, detail, reason);

    if (!sidecarItem.isWeapon)
    {
        std::string directAttachDetail;
        std::vector<unsigned int> directIds;
        directIds.push_back(spawnedId);
        const bool directAttached = TryAttachCreatedItemsToInventory(
            inventory,
            itemSystem,
            playerEntityId,
            directIds,
            requestedCount,
            sidecarItem.x,
            sidecarItem.y,
            directAttachDetail,
            reason);
        AppendDetail(detail, std::string("silentAttach=") + (directAttached ? "1" : "0") +
            (directAttachDetail.empty() ? std::string() : " " + directAttachDetail));

        bool containsDirect = false;
        if (directAttached &&
            TryInventoryContains(inventory, spawnedId, containsDirect, reason) &&
            containsDirect)
        {
            TryApplySidecarFlagsToRestoredItem(itemSystem, spawnedId, sidecarItem, detail, reason);
            HideInventoryBackedWorldEntity(spawnedId, detail, reason);
            return true;
        }
    }
    else
    {
        AppendDetail(detail, "silentAttach skipped for weapon registration");
    }

    auto currentCount = [&]() -> int
    {
        int value = -1;
        TryGetInventoryArchetypeCount(inventory, sidecarItem.archetypeId, value, reason);
        return value;
    };

    bool stacked = false;
    const bool distinctEntityRestore = IsSidecarDistinctEntityItem(sidecarItem);
    AppendDetail(detail, "TryGiveStacked skipped for cell rebuild");

    int afterCount = currentCount();
    if (!distinctEntityRestore && haveBeforeCount && afterCount >= beforeCount + requestedCount)
    {
        cleanupSpawnIfNotInInventory();
        return true;
    }

    bool gaveInventory = false;
    if (TryGuardedCall(
            "coop spawnGive TryGiveInventory",
            [&]() -> bool
            {
                return item->TryGiveInventory(static_cast<IArkInventory*>(&inventory));
            },
            gaveInventory,
            &reason))
    {
        AppendDetail(detail, std::string("spawnGive inventory=") + (gaveInventory ? "1" : "0") +
            " count=" + std::to_string(currentCount()));
    }
    else
    {
        AppendDetail(detail, "spawnGive TryGiveInventory failed");
        if (!reason.empty())
            AppendDetail(detail, reason);
    }

    std::vector<unsigned int> afterIds;
    TryGetInventoryItemIds(inventory, afterIds, reason);
    std::vector<unsigned int> newIds = FindNewInventoryItemIds(beforeIds, afterIds);

    bool containsSpawned = false;
    if (TryInventoryContains(inventory, spawnedId, containsSpawned, reason) && containsSpawned)
        newIds.push_back(spawnedId);

    if (!newIds.empty())
    {
        for (unsigned itemId : newIds)
        {
            TryApplySidecarFlagsToRestoredItem(itemSystem, itemId, sidecarItem, detail, reason);
            HideInventoryBackedWorldEntity(itemId, detail, reason);
        }
        TryPlaceOrPackInventoryItems(inventory, newIds, sidecarItem.x, sidecarItem.y, reason);
    }

    afterCount = currentCount();
    if (distinctEntityRestore && !newIds.empty())
    {
        cleanupSpawnIfNotInInventory();
        return true;
    }
    if (!distinctEntityRestore && haveBeforeCount && afterCount >= beforeCount + requestedCount)
    {
        cleanupSpawnIfNotInInventory();
        return true;
    }
    if (!haveBeforeCount && (!newIds.empty() || gaveInventory || stacked))
    {
        cleanupSpawnIfNotInInventory();
        return true;
    }

    cleanupSpawnIfNotInInventory();

    AppendDetail(detail,
        std::string("spawnGive did not restore item after=") + std::to_string(afterCount) +
        " distinct=" + std::to_string(distinctEntityRestore ? 1 : 0) +
        " newIds=" + std::to_string(newIds.size()));
    return false;
}

bool TryGiveInventoryItemWithConsole(
    const std::string& archetypeName,
    int count,
    std::string& reason)
{
    if (!gEnv || !gEnv->pConsole)
    {
        reason = "console unavailable";
        return false;
    }

    const std::string command = "i_giveitem " + archetypeName + " " + std::to_string(std::max(1, count));
    return TryGuardedVoidCall(
        "coop console i_giveitem",
        [&]()
        {
            gEnv->pConsole->ExecuteString(command.c_str(), true, false);
        },
        &reason);
}

bool TryRestoreInventoryItem(
    ArkInventory& inventory,
    ArkItemSystem& itemSystem,
    EntityId playerEntityId,
    const ModMain::PlayerInventoryItemState& sidecarItem,
    std::string& reason)
{
    int beforeCount = 0;
    const bool haveBeforeCount = TryGetInventoryArchetypeCount(inventory, sidecarItem.archetypeId, beforeCount, reason);

    std::vector<unsigned int> beforeIds;
    TryGetInventoryItemIds(inventory, beforeIds, reason);

    unsigned inventoryOwnerId = 0;
    TryGuardedCall(
        "coop inventory GetOwnerId",
        [&]() -> unsigned
        {
            return inventory.GetOwnerId();
        },
        inventoryOwnerId,
        &reason);

    IEntityArchetype* archetype = nullptr;
    std::string lookupReason;
    TryGetArchetypePointer(sidecarItem.archetypeId, archetype, lookupReason);
    const KnownPickupArchetypeInfo* knownInfo = LookupKnownPickupArchetypeInfo(sidecarItem.archetypeId);

    std::string detail;
    AppendDetail(detail, "owner player=" + std::to_string(playerEntityId) +
        " inventory=" + std::to_string(inventoryOwnerId) +
        " beforeCount=" + (haveBeforeCount ? std::to_string(beforeCount) : std::string("?")) +
        " beforeIds=" + std::to_string(beforeIds.size()) +
        " sidecarFlags=0x" + std::to_string(sidecarItem.flags) +
        (IsSidecarDistinctEntityItem(sidecarItem) ? " distinct=1" : " distinct=0"));

    if (archetype)
    {
        const bool spawnGiveOk = TrySpawnAndGiveInventoryItem(
            inventory,
            itemSystem,
            playerEntityId,
            *archetype,
            sidecarItem,
            beforeIds,
            beforeCount,
            haveBeforeCount,
            detail,
            reason);
        int spawnGiveAfterCount = 0;
        if (TryGetInventoryArchetypeCount(inventory, sidecarItem.archetypeId, spawnGiveAfterCount, reason))
            AppendDetail(detail, "spawnGiveAfterCount=" + std::to_string(spawnGiveAfterCount));
        if (spawnGiveOk)
        {
            if (!detail.empty())
                reason = detail;
            return true;
        }
    }
    else if (knownInfo)
    {
        IEntityArchetype* itemSystemArchetype = nullptr;
        std::string itemArchetypeReason;
        if (TryGetArkItemSystemArchetype(itemSystem, *knownInfo, itemSystemArchetype, itemArchetypeReason))
        {
            AppendDetail(detail, std::string("itemSystemArchetype=") + ArchetypeSummary(itemSystemArchetype));
            const bool spawnGiveOk = TrySpawnAndGiveInventoryItem(
                inventory,
                itemSystem,
                playerEntityId,
                *itemSystemArchetype,
                sidecarItem,
                beforeIds,
                beforeCount,
                haveBeforeCount,
                detail,
                reason);
            int spawnGiveAfterCount = 0;
            if (TryGetInventoryArchetypeCount(inventory, sidecarItem.archetypeId, spawnGiveAfterCount, reason))
                AppendDetail(detail, "itemSystemSpawnGiveAfterCount=" + std::to_string(spawnGiveAfterCount));
            if (spawnGiveOk)
            {
                if (!detail.empty())
                    reason = detail;
                return true;
            }
        }
        else
        {
            AppendDetail(detail, std::string("item-system GetArchetype failed ") +
                knownInfo->className + "/" + knownInfo->archetypeName);
            if (!itemArchetypeReason.empty())
                AppendDetail(detail, itemArchetypeReason);
        }
    }
    else
    {
        AppendDetail(detail, "no spawnable archetype");
        if (!lookupReason.empty())
            AppendDetail(detail, lookupReason);
    }

    int afterCount = 0;
    const bool haveAfterCount = TryGetInventoryArchetypeCount(inventory, sidecarItem.archetypeId, afterCount, reason);
    if (haveAfterCount)
        AppendDetail(detail, "afterCount=" + std::to_string(afterCount));
    if (!IsSidecarDistinctEntityItem(sidecarItem) &&
        haveBeforeCount &&
        haveAfterCount &&
        afterCount >= beforeCount + std::max(1, sidecarItem.count))
    {
        if (!detail.empty())
            reason = detail;
        return true;
    }

    if (!detail.empty())
        reason = detail;
    else if (reason.empty())
        reason = "spawn restore did not increase item count";
    AppendDetail(reason, "unsafe GiveArchetype fallback disabled");
    return false;
}

class PlayerSidecarInventoryFeedbackSuppressGuard
{
public:
    PlayerSidecarInventoryFeedbackSuppressGuard(ModMain& mod, const char* reason)
        : m_mod(mod)
        , m_reason(reason && reason[0] ? reason : "sidecar inventory restore")
    {
        m_mod.BeginPlayerSidecarInventoryFeedbackSuppression(m_reason);
    }

    ~PlayerSidecarInventoryFeedbackSuppressGuard()
    {
        m_mod.EndPlayerSidecarInventoryFeedbackSuppression(m_reason);
    }

    PlayerSidecarInventoryFeedbackSuppressGuard(const PlayerSidecarInventoryFeedbackSuppressGuard&) = delete;
    PlayerSidecarInventoryFeedbackSuppressGuard& operator=(const PlayerSidecarInventoryFeedbackSuppressGuard&) = delete;

private:
    ModMain& m_mod;
    const char* m_reason = "";
};

bool ResetTransientPlayerStateForFreshCoopProfile(ArkPlayer& player, std::string& detail)
{
    bool ok = true;
    std::string reason;
    if (!TryGuardedVoidCall(
        "coop reset player radiation",
        [&]()
        {
            player.m_playerComponent.GetRadiationComponent().Reset();
        },
        &reason))
    {
        ok = false;
        detail = reason;
    }

    reason.clear();
    if (!TryGuardedVoidCall(
        "coop clear player statuses",
        [&]()
        {
            player.m_playerComponent.GetStatusComponent().ClearAllStatuses();
        },
        &reason))
    {
        ok = false;
        if (!detail.empty())
            detail += "; ";
        detail += reason;
    }

    return ok;
}
} // namespace

bool ModMain::CaptureSharedStorageInventory(
    ArkInventory& inventory,
    std::vector<PlayerInventoryItemState>& items,
    std::string& detail)
{
    items.clear();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!itemSystem)
    {
        detail = "item_system_unavailable";
        return false;
    }

    std::vector<PassiveInventoryItemState> passiveItems;
    std::string reason;
    if (!BuildPassiveInventorySnapshot(inventory, *itemSystem, passiveItems, reason))
    {
        detail = "capture_failed_" + reason;
        return false;
    }

    items.reserve(passiveItems.size());
    for (const PassiveInventoryItemState& source : passiveItems)
    {
        PlayerInventoryItemState item;
        item.archetypeId = source.archetypeId;
        item.count = source.count;
        item.x = source.x;
        item.y = source.y;
        item.width = source.width;
        item.height = source.height;
        item.flags = source.flags;
        item.category = source.category;
        item.isWeapon = source.isWeapon;
        item.weaponCondition = source.weaponCondition;
        item.weaponAmmoLoaded = source.weaponAmmoLoaded;
        item.weaponAmmoCount = source.weaponAmmoCount;
        item.weaponModCount = source.weaponModCount;
        item.weaponModTotalLevel = source.weaponModTotalLevel;
        item.weaponMods = source.weaponMods;
        items.push_back(std::move(item));
    }

    detail = "captured_" + std::to_string(items.size());
    if (!reason.empty())
        detail += "_warning_" + reason;
    return true;
}

bool ModMain::ReplaceSharedStorageInventory(
    ArkInventory& inventory,
    const std::vector<PlayerInventoryItemState>& items,
    std::string& detail)
{
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!itemSystem)
    {
        detail = "item_system_unavailable";
        return false;
    }

    unsigned ownerId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall("shared storage GetOwnerId", [&inventory]() { return inventory.GetOwnerId(); }, ownerId, &reason) ||
        ownerId == INVALID_ENTITYID)
    {
        detail = "owner_unavailable_" + reason;
        return false;
    }

    if (!TryGuardedVoidCall("shared storage RemoveAllItems", [&inventory]() { inventory.RemoveAllItems(); }, &reason))
    {
        detail = "clear_failed_" + reason;
        return false;
    }

    uint32_t restored = 0;
    for (const PlayerInventoryItemState& item : items)
    {
        if (item.archetypeId == 0 || item.count <= 0)
            continue;
        std::string itemReason;
        if (!TryRestoreInventoryItem(inventory, *itemSystem, ownerId, item, itemReason))
        {
            detail =
                "restore_failed_index_" + std::to_string(restored) +
                "_arch_" + std::to_string(item.archetypeId) +
                "_reason_" + itemReason;
            return false;
        }
        ++restored;
    }

    detail = "restored_" + std::to_string(restored);
    return restored == items.size();
}

bool ModMain::ShouldSuppressPlayerSidecarInventoryFeedback() const
{
    return m_playerSidecarInventoryFeedbackSuppressDepth != 0;
}

void ModMain::BeginPlayerSidecarInventoryFeedbackSuppression(const char* reason)
{
    ++m_playerSidecarInventoryFeedbackSuppressDepth;
    if (m_playerSidecarInventoryFeedbackSuppressDepth == 1)
    {
        LogCoop(
            std::string("sidecar inventory feedback suppressed: ") +
            (reason && reason[0] ? reason : "inventory restore"));
    }
}

void ModMain::EndPlayerSidecarInventoryFeedbackSuppression(const char* reason)
{
    if (m_playerSidecarInventoryFeedbackSuppressDepth == 0)
    {
        LogCoop(
            std::string("sidecar inventory feedback suppression underflow: ") +
            (reason && reason[0] ? reason : "inventory restore"));
        return;
    }

    --m_playerSidecarInventoryFeedbackSuppressDepth;
    if (m_playerSidecarInventoryFeedbackSuppressDepth == 0)
    {
        ++m_playerSidecarInventoryFeedbackSuppressed;
        LogCoop(
            std::string("sidecar inventory feedback restored: ") +
            (reason && reason[0] ? reason : "inventory restore"));
    }
}

std::string ModMain::GetPlayerSidecarPath() const
{
    const std::filesystem::path root = GetCoopPlayerStateRoot();
    if (root.empty())
        return {};

    const std::string fileName = "player_account_" + HexPlayerAccountToken(GetLocalAccountToken()) + ".state";
    return (root / fileName).string();
}

bool ModMain::SaveLocalPlayerSidecar(const char* reason)
{
    if (!m_enablePlayerSidecar)
        return false;

    if (!IsGameReady())
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save skipped: game not ready";
        return false;
    }

    if (m_networkMode == CoopNetworkMode::Client &&
        (m_pendingPlayerSidecarInventoryRestore ||
            m_pendingPlayerSidecarChipsetRestore ||
            m_playerSidecarInventoryPending > 0 ||
            m_playerSidecarChipsetsPending > 0 ||
            m_pendingReceivedPlayerStateApply ||
            m_clientAwaitingHostPlayerState))
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save skipped: host player state restore pending";
        return false;
    }

    const std::string pathString = GetPlayerSidecarPath();
    if (pathString.empty())
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save skipped: no sidecar path";
        return false;
    }

    const std::filesystem::path path(pathString);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save failed: cannot create directory";
        m_lastPlayerSidecarPath = pathString;
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    if (!playerEntity)
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save skipped: no player entity";
        return false;
    }

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(6);
    if (!m_nativeSideBlobCapture.active)
        FinalizeNativeSideBlobCaptureForPlayerState(reason && reason[0] ? reason : "player sidecar save");

    const Vec3 position = playerEntity->GetWorldPos();
    const Quat rotation = playerEntity->GetWorldRotation();
    const Quat viewRotation = player.GetViewRotation();
    const uint32_t flags = m_localPlayerDowned ? kPlayerSidecarFlagDowned : 0u;

    output << "version=1\n";
    output << "username=" << GetLocalUsername() << "\n";
    output << "level=" << GetCurrentLevelName() << "\n";
    output << "worldEpoch=" << m_localWorldEpoch << "\n";
    output << "flags=" << flags << "\n";
    output << "position=" << position.x << ' ' << position.y << ' ' << position.z << "\n";
    output << "rotation=" << rotation.w << ' ' << rotation.v.x << ' ' << rotation.v.y << ' ' << rotation.v.z << "\n";
    output << "viewRotation=" << viewRotation.w << ' ' << viewRotation.v.x << ' ' << viewRotation.v.y << ' ' << viewRotation.v.z << "\n";
    output << "health=" << player.GetHealth() << ' ' << player.GetMaxHealth() << "\n";

    CArkPsiComponent& psiComponent = player.m_playerComponent.GetPsiComponent();
    output << "psi=" << psiComponent.GetPoints() << "\n";

    if (player.m_helmet.m_pOxygenComponent)
    {
        ArkPlayerOxygenComponent& oxygen = *player.m_helmet.m_pOxygenComponent;
        output << "oxygen=" << oxygen.m_oxygen << ' ' << oxygen.GetMaxOxygen() << ' '
            << (oxygen.m_bConsumingOxygen ? 1 : 0) << "\n";
    }

    ArkItemSystem* sidecarItemSystem = GetArkItemSystemPtr();
    auto resolveWeaponArchetype = [sidecarItemSystem](unsigned itemId) -> uint64_t
    {
        if (!sidecarItemSystem || itemId == 0)
            return 0;
        IArkItem* rawItem = FindArkItemDirect(sidecarItemSystem, itemId);
        CArkItem* item = rawItem && IsLikelyRuntimeCppObject(rawItem)
            ? static_cast<CArkItem*>(rawItem)
            : nullptr;
        return item && IsLikelyRuntimeCppObject(item, sizeof(CArkItem))
            ? item->m_selectedArchetype
            : 0;
    };

    const uint64_t equippedWeaponArchetype = resolveWeaponArchetype(
        player.m_weaponComponent.GetEquippedOrToEquipWeaponId());
    output << "equippedWeapon=" << equippedWeaponArchetype << "\n";

    std::vector<PlayerQuickSelectState> quickSelectStates;
    ArkQuickSelectComponent& quickSelect = player.m_playerComponent.GetQuickSelectComponent();
    auto captureQuickSelect = [&](const auto& selections, int bank)
    {
        for (size_t index = 0; index < selections.size(); ++index)
        {
            const ArkQuickSelectComponent::QuickSelectId& selection = selections[index];
            PlayerQuickSelectState row;
            row.bank = bank;
            row.index = static_cast<int>(index);
            row.type = static_cast<int>(selection.m_type);
            if (selection.m_type == ArkQuickSelectComponent::QuickSelectType::weapon)
            {
                const unsigned* itemId = boost::get<unsigned>(&selection.m_id);
                row.stableId = itemId ? resolveWeaponArchetype(*itemId) : 0;
            }
            else if (selection.m_type == ArkQuickSelectComponent::QuickSelectType::power)
            {
                const EArkPsiPowers* power = boost::get<EArkPsiPowers>(&selection.m_id);
                row.stableId = power ? static_cast<uint64_t>(*power) : 0;
            }
            if (row.stableId != 0)
                quickSelectStates.push_back(row);
        }
    };
    captureQuickSelect(quickSelect.m_controllerQuickSelects, 0);
    captureQuickSelect(quickSelect.m_keyboardQuickSelects, 1);
    output << "quickSelectCount=" << quickSelectStates.size() << "\n";
    for (const PlayerQuickSelectState& row : quickSelectStates)
        output << "quickSelect=" << row.bank << ' ' << row.index << ' ' << row.type << ' ' << row.stableId << "\n";

    std::vector<PlayerStatusState> statusStates;
    ArkPlayerStatusComponent& statusComponent = player.m_playerComponent.GetStatusComponent();
    for (const std::unique_ptr<ArkTraumaBase>& trauma : statusComponent.m_statuses)
    {
        if (!trauma || !IsLikelyRuntimeCppObject(trauma.get(), sizeof(ArkTraumaBase)))
            continue;
        if (trauma->m_currentAmount <= 0.0f && trauma->m_currentLevel <= 0)
            continue;
        PlayerStatusState row;
        row.status = static_cast<int>(trauma->m_status);
        row.amount = trauma->m_currentAmount;
        row.level = trauma->m_currentLevel;
        row.suspended = trauma->m_bIsSuspended;
        if (row.status > static_cast<int>(EArkPlayerStatus::Invalid) &&
            row.status < static_cast<int>(EArkPlayerStatus::Last))
        {
            statusStates.push_back(row);
        }
    }
    output << "statusCount=" << statusStates.size() << "\n";
    for (const PlayerStatusState& row : statusStates)
        output << "status=" << row.status << ' ' << row.amount << ' ' << row.level << ' ' << (row.suspended ? 1 : 0) << "\n";

    std::vector<PlayerAbilityState> abilityStates;
    std::vector<PlayerResearchState> researchStates;
    ArkAbilityComponent& abilityComponent = player.m_playerComponent.GetAbilityComponent();
    abilityStates.reserve(abilityComponent.m_abilities.size());
    for (const ArkAbilityData& ability : abilityComponent.m_abilities)
    {
        if (ability.m_id == 0 || (!ability.m_bAcquired && !ability.m_bSeen))
            continue;

        PlayerAbilityState abilityState;
        abilityState.abilityId = ability.m_id;
        abilityState.acquired = ability.m_bAcquired;
        abilityState.seen = ability.m_bSeen;
        abilityStates.push_back(abilityState);
    }

    researchStates.reserve(abilityComponent.m_researchTopics.size());
    for (const ArkResearchTopicData& research : abilityComponent.m_researchTopics)
    {
        if (research.m_id == 0 || research.m_scanCount <= 0)
            continue;

        PlayerResearchState researchState;
        researchState.researchId = research.m_id;
        researchState.scanCount = research.m_scanCount;
        researchStates.push_back(researchState);
    }

    output << "abilityCount=" << abilityStates.size() << "\n";
    for (const PlayerAbilityState& ability : abilityStates)
    {
        output << "ability=" << ability.abilityId << ' '
            << (ability.acquired ? 1 : 0) << ' '
            << (ability.seen ? 1 : 0) << "\n";
    }

    output << "researchCount=" << researchStates.size() << "\n";
    for (const PlayerResearchState& research : researchStates)
        output << "research=" << research.researchId << ' ' << research.scanCount << "\n";

    std::string chipsetReason;
    std::vector<PlayerChipsetState> chipsetStates = BuildPlayerChipsetSnapshot(player, chipsetReason);
    if (!chipsetReason.empty())
        LogCoop("sidecar chipset snapshot warnings: " + chipsetReason);

    output << "chipsetCount=" << chipsetStates.size() << "\n";
    for (const PlayerChipsetState& chipset : chipsetStates)
    {
        output << "chipset=" << chipset.type << ' '
            << chipset.archetypeId << ' '
            << chipset.itemId << ' '
            << chipset.slot << ' '
            << (chipset.installed ? 1 : 0) << "\n";
    }

    uint32_t itemCount = 0;
    std::vector<PlayerInventoryItemState> items;
    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (inventory && itemSystem)
    {
        std::string inventoryReason;
        std::vector<PassiveInventoryItemState> passiveItems;
        if (!BuildPassiveInventorySnapshot(*inventory, *itemSystem, passiveItems, inventoryReason) && !inventoryReason.empty())
        {
            LogCoop("sidecar passive inventory snapshot failed: " + inventoryReason);
        }
        else if (!inventoryReason.empty())
        {
            LogCoop("sidecar passive inventory snapshot warnings: " + inventoryReason);
        }

        items.reserve(passiveItems.size());
        for (const PassiveInventoryItemState& passiveItem : passiveItems)
        {
            PlayerInventoryItemState itemState;
            itemState.archetypeId = passiveItem.archetypeId;
            itemState.count = passiveItem.count;
            itemState.x = passiveItem.x;
            itemState.y = passiveItem.y;
            itemState.itemId = passiveItem.itemId;
            itemState.width = passiveItem.width;
            itemState.height = passiveItem.height;
            itemState.flags = passiveItem.flags;
            itemState.category = passiveItem.category;
            itemState.isWeapon = passiveItem.isWeapon &&
                !CoopItemClassification::IsKnownAmmoPickupArchetype(passiveItem.archetypeId);
            if (!itemState.isWeapon)
                itemState.flags &= ~kPlayerInventoryItemFlagWeapon;
            itemState.weaponCondition = passiveItem.weaponCondition;
            itemState.weaponAmmoLoaded = passiveItem.weaponAmmoLoaded;
            itemState.weaponAmmoCount = passiveItem.weaponAmmoCount;
            itemState.weaponModCount = passiveItem.weaponModCount;
            itemState.weaponModTotalLevel = passiveItem.weaponModTotalLevel;
            itemState.weaponMods = passiveItem.weaponMods;
            items.push_back(itemState);
        }
    }

    output << "inventoryCount=" << items.size() << "\n";
    for (const PlayerInventoryItemState& item : items)
    {
        output << "item=" << item.archetypeId << ' '
            << item.count << ' '
            << item.x << ' '
            << item.y << ' '
            << item.itemId << ' '
            << item.width << ' '
            << item.height << ' '
            << item.flags << ' '
            << item.category << ' '
            << (item.isWeapon ? 1 : 0) << ' '
            << item.weaponCondition << ' '
            << item.weaponAmmoLoaded << ' '
            << item.weaponAmmoCount << ' '
            << item.weaponModCount << ' '
            << item.weaponModTotalLevel << "\n";
        for (const auto& mod : item.weaponMods)
        {
            output << "nativeWeaponMod=" << item.itemId << ' '
                << mod.first << ' '
                << mod.second << "\n";
        }
    }

    const bool nativeCaptureMatchesLevel =
        HasUsableNativeCaptureForSidecar(m_lastNativePlayerSaveCapture) &&
        NormalizeLevelName(m_lastNativePlayerSaveCapture.levelName) == NormalizeLevelName(GetCurrentLevelName());
    if (nativeCaptureMatchesLevel)
        WriteNativeCaptureSidecarSection(output, m_lastNativePlayerSaveCapture);

    const std::string payload = output.str();
    if (payload.empty() || !WritePlayerSidecarPayloadWithHash(path, payload))
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "save failed: write error";
        m_lastPlayerSidecarPath = pathString;
        return false;
    }

    itemCount = static_cast<uint32_t>(std::min<size_t>(items.size(), UINT32_MAX));
    ++m_playerSidecarSaveCount;
    m_playerSidecarInventorySaved = itemCount;
    m_playerSidecarAbilitiesSaved = static_cast<uint32_t>(std::min<size_t>(abilityStates.size(), UINT32_MAX));
    m_playerSidecarChipsetsSaved = static_cast<uint32_t>(std::min<size_t>(chipsetStates.size(), UINT32_MAX));
    m_lastPlayerSidecarPath = pathString;
    m_lastPlayerSidecarEvent =
        std::string("saved player sidecar") +
        (nativeCaptureMatchesLevel ? " nativeCapture=1" : " nativeCapture=0") +
        (nativeCaptureMatchesLevel && m_lastNativePlayerSaveCapture.hasNativeFragmentPayload
            ? " nativeFragment=1/" + std::to_string(m_lastNativePlayerSaveCapture.nativeFragmentPayloadBytes)
            : " nativeFragment=0") +
        (nativeCaptureMatchesLevel && m_lastNativePlayerSaveCapture.hasNativeSnapshotSave
            ? " nativeSnapshot=1/" + std::to_string(m_lastNativePlayerSaveCapture.nativeSnapshotSaveBytes)
            : " nativeSnapshot=0") +
        (reason && reason[0] ? ": " + std::string(reason) : "");
    return true;
}

bool ModMain::LoadLocalPlayerSidecar(PlayerSidecarState& state)
{
    const std::string pathString = GetPlayerSidecarPath();
    m_lastPlayerSidecarPath = pathString.empty() ? "-" : pathString;
    if (pathString.empty())
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "load failed: no sidecar path";
        return false;
    }

    if (LoadPlayerSidecarFromPath(pathString, state))
        return true;

    const std::string previousEvent = m_lastPlayerSidecarEvent;
    const uint32_t previousFailCount = m_playerSidecarFailCount;
    const std::filesystem::path usernamePath =
        GetCoopPlayerStateRoot() / ("player_" + SanitizePathComponent(GetLocalUsername()) + ".state");
    const std::filesystem::path legacyPath =
        GetLegacyCoopPlayerStateRoot() / ("player_" + SanitizePathComponent(GetLocalUsername()) + ".state");
    for (const std::filesystem::path& candidate : { usernamePath, legacyPath })
    {
        if (candidate.empty() || candidate.string() == pathString)
            continue;
        if (!LoadPlayerSidecarFromPath(candidate.string(), state))
            continue;

        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(pathString).parent_path(), error);
        if (!error)
            std::filesystem::copy_file(candidate, pathString, std::filesystem::copy_options::overwrite_existing, error);
        m_lastPlayerSidecarEvent = error
            ? "loaded legacy player sidecar; account migration copy failed"
            : "loaded and migrated legacy player sidecar to account identity";
        m_lastPlayerSidecarPath = pathString;
        return true;
    }

    m_playerSidecarFailCount = previousFailCount;
    m_lastPlayerSidecarEvent = previousEvent;
    m_lastPlayerSidecarPath = pathString;
    return false;
}

bool ModMain::LoadPlayerSidecarFromPath(const std::string& pathString, PlayerSidecarState& state)
{
    state = PlayerSidecarState();
    m_lastPlayerSidecarPath = pathString.empty() ? "-" : pathString;
    if (pathString.empty())
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "load failed: empty sidecar path";
        return false;
    }

    std::string payload;
    bool hasIntegrityHash = false;
    std::string integrityReason;
    if (!ReadAndValidatePlayerSidecarFile(pathString, payload, hasIntegrityHash, integrityReason))
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "load failed: invalid player sidecar integrity: " + integrityReason;
        return false;
    }

    std::istringstream input(payload);
    input.imbue(std::locale::classic());
    std::string line;
    bool nativeFragmentPayloadHexInvalid = false;
    bool nativeSnapshotSaveHexInvalid = false;
    while (std::getline(input, line))
    {
        if (StartsWith(line, "username="))
        {
            state.username = AfterPrefix(line, "username=");
        }
        else if (StartsWith(line, "level="))
        {
            state.levelName = NormalizeLevelName(AfterPrefix(line, "level="));
        }
        else if (StartsWith(line, "worldEpoch="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "worldEpoch="));
            stream >> state.worldEpoch;
        }
        else if (StartsWith(line, "flags="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "flags="));
            stream >> state.flags;
        }
        else if (StartsWith(line, "resetTransientState="))
        {
            int value = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "resetTransientState="));
            if (stream >> value)
                state.resetTransientState = value != 0;
        }
        else if (StartsWith(line, "position="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "position="));
            if (stream >> state.position.x >> state.position.y >> state.position.z)
                state.hasPosition = true;
        }
        else if (StartsWith(line, "rotation="))
        {
            float qw = 1.0f;
            float qx = 0.0f;
            float qy = 0.0f;
            float qz = 0.0f;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "rotation="));
            if (stream >> qw >> qx >> qy >> qz)
            {
                state.rotation = Quat(qw, qx, qy, qz);
                state.hasRotation = true;
            }
        }
        else if (StartsWith(line, "viewRotation="))
        {
            float qw = 1.0f;
            float qx = 0.0f;
            float qy = 0.0f;
            float qz = 0.0f;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "viewRotation="));
            if (stream >> qw >> qx >> qy >> qz)
            {
                state.viewRotation = Quat(qw, qx, qy, qz);
                state.hasViewRotation = true;
            }
        }
        else if (StartsWith(line, "health="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "health="));
            if (stream >> state.health >> state.maxHealth)
                state.hasHealth = true;
        }
        else if (StartsWith(line, "psi="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "psi="));
            if (stream >> state.psiPoints)
                state.hasPsi = true;
        }
        else if (StartsWith(line, "oxygen="))
        {
            int consuming = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "oxygen="));
            if (stream >> state.oxygen >> state.maxOxygen >> consuming &&
                std::isfinite(state.oxygen) && state.oxygen >= 0.0f &&
                std::isfinite(state.maxOxygen) && state.maxOxygen > 0.0f)
            {
                state.oxygen = std::min(state.oxygen, state.maxOxygen);
                state.oxygenConsuming = consuming != 0;
                state.hasOxygen = true;
            }
        }
        else if (StartsWith(line, "equippedWeapon="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "equippedWeapon="));
            stream >> state.equippedWeaponArchetypeId;
        }
        else if (StartsWith(line, "quickSelectCount="))
        {
            state.hasQuickSelect = true;
        }
        else if (StartsWith(line, "quickSelect="))
        {
            PlayerQuickSelectState row;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "quickSelect="));
            if (stream >> row.bank >> row.index >> row.type >> row.stableId)
            {
                const int maxIndex = row.bank == 0 ? 4 : 10;
                if ((row.bank == 0 || row.bank == 1) &&
                    row.index >= 0 && row.index < maxIndex &&
                    (row.type == static_cast<int>(ArkQuickSelectComponent::QuickSelectType::power) ||
                        row.type == static_cast<int>(ArkQuickSelectComponent::QuickSelectType::weapon)) &&
                    row.stableId != 0)
                {
                    state.quickSelect.push_back(row);
                }
                state.hasQuickSelect = true;
            }
        }
        else if (StartsWith(line, "statusCount="))
        {
            state.hasStatuses = true;
        }
        else if (StartsWith(line, "status="))
        {
            PlayerStatusState row;
            int suspended = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "status="));
            if (stream >> row.status >> row.amount >> row.level >> suspended)
            {
                row.suspended = suspended != 0;
                if (row.status > static_cast<int>(EArkPlayerStatus::Invalid) &&
                    row.status < static_cast<int>(EArkPlayerStatus::Last) &&
                    std::isfinite(row.amount) && row.amount >= 0.0f &&
                    row.level >= 0)
                {
                    state.statuses.push_back(row);
                }
                state.hasStatuses = true;
            }
        }
        else if (StartsWith(line, "abilityCount="))
        {
            state.hasAbilities = true;
        }
        else if (StartsWith(line, "ability="))
        {
            PlayerAbilityState ability;
            int acquired = 0;
            int seen = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "ability="));
            if (stream >> ability.abilityId >> acquired >> seen)
            {
                if (ability.abilityId != 0)
                {
                    ability.acquired = acquired != 0;
                    ability.seen = seen != 0;
                    state.abilities.push_back(ability);
                }
                state.hasAbilities = true;
            }
        }
        else if (StartsWith(line, "researchCount="))
        {
            state.hasAbilities = true;
        }
        else if (StartsWith(line, "research="))
        {
            PlayerResearchState research;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "research="));
            if (stream >> research.researchId >> research.scanCount)
            {
                if (research.researchId != 0 && research.scanCount > 0)
                    state.research.push_back(research);
                state.hasAbilities = true;
            }
        }
        else if (StartsWith(line, "chipsetCount="))
        {
            state.hasChipsets = true;
        }
        else if (StartsWith(line, "chipset="))
        {
            PlayerChipsetState chipset;
            int installed = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "chipset="));
            if (stream >> chipset.type
                >> chipset.archetypeId
                >> chipset.itemId
                >> chipset.slot
                >> installed)
            {
                chipset.type = chipset.type == 0 ? 0 : 1;
                chipset.installed = installed != 0;
                if (chipset.archetypeId != 0)
                    state.chipsets.push_back(chipset);
                state.hasChipsets = true;
            }
        }
        else if (StartsWith(line, "inventoryCount="))
        {
            state.hasInventory = true;
        }
        else if (StartsWith(line, "item="))
        {
            PlayerInventoryItemState item;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "item="));
            if (stream >> item.archetypeId >> item.count >> item.x >> item.y)
            {
                stream >> item.itemId >> item.width >> item.height >> item.flags;
                int isWeapon = 0;
                if (stream >> item.category >> isWeapon
                    >> item.weaponCondition
                    >> item.weaponAmmoLoaded
                    >> item.weaponAmmoCount
                    >> item.weaponModCount
                    >> item.weaponModTotalLevel)
                {
                    item.isWeapon = isWeapon != 0 &&
                        !CoopItemClassification::IsKnownAmmoPickupArchetype(item.archetypeId);
                    if (!item.isWeapon)
                        item.flags &= ~kPlayerInventoryItemFlagWeapon;
                    if (item.isWeapon)
                        item.flags |= kPlayerInventoryItemFlagWeapon;
                }
                if (item.archetypeId != 0 && item.count > 0)
                    state.inventory.push_back(item);
                state.hasInventory = true;
            }
        }
        else if (StartsWith(line, "nativeCaptureVersion="))
        {
            int version = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureVersion="));
            if (stream >> version && version == 1)
                state.hasNativeCapture = true;
        }
        else if (StartsWith(line, "nativeCaptureSaw="))
        {
            int sawPlayer = 0;
            int sawInventory = 0;
            int sawItem = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureSaw="));
            if (stream >> sawPlayer >> sawInventory >> sawItem)
            {
                state.nativeCapture.sawPlayerWrite = sawPlayer != 0;
                state.nativeCapture.sawInventoryWrite = sawInventory != 0;
                state.nativeCapture.sawItemWrite = sawItem != 0;
                state.hasNativeCapture = HasUsableNativeCaptureForSidecar(state.nativeCapture);
            }
        }
        else if (StartsWith(line, "nativeCaptureLevel="))
        {
            state.nativeCapture.levelName = NormalizeLevelName(AfterPrefix(line, "nativeCaptureLevel="));
            if (!state.nativeCapture.levelName.empty())
                state.hasNativeCapture = true;
        }
        else if (StartsWith(line, "nativeCaptureWorldEpoch="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureWorldEpoch="));
            stream >> state.nativeCapture.worldEpoch;
        }
        else if (StartsWith(line, "nativeCapturePosition="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCapturePosition="));
            stream >> state.nativeCapture.position.x >> state.nativeCapture.position.y >> state.nativeCapture.position.z;
        }
        else if (StartsWith(line, "nativeCaptureRotation="))
        {
            float qw = 1.0f;
            float qx = 0.0f;
            float qy = 0.0f;
            float qz = 0.0f;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureRotation="));
            if (stream >> qw >> qx >> qy >> qz)
                state.nativeCapture.rotation = Quat(qw, qx, qy, qz);
        }
        else if (StartsWith(line, "nativeCaptureViewRotation="))
        {
            float qw = 1.0f;
            float qx = 0.0f;
            float qy = 0.0f;
            float qz = 0.0f;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureViewRotation="));
            if (stream >> qw >> qx >> qy >> qz)
                state.nativeCapture.viewRotation = Quat(qw, qx, qy, qz);
        }
        else if (StartsWith(line, "nativeCaptureVitals="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureVitals="));
            stream >> state.nativeCapture.health
                >> state.nativeCapture.maxHealth
                >> state.nativeCapture.armor
                >> state.nativeCapture.maxArmor
                >> state.nativeCapture.stance;
        }
        else if (StartsWith(line, "nativeCapturePsi="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCapturePsi="));
            stream >> state.nativeCapture.psiPoints
                >> state.nativeCapture.psiMaxPoints
                >> state.nativeCapture.selectedPsiPower
                >> state.nativeCapture.equippedPsiPower;
            state.hasNativeCapture = true;
        }
        else if (StartsWith(line, "nativeCaptureInventory="))
        {
            int itemCount = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureInventory="));
            if (stream >> state.nativeCapture.inventoryOwnerId
                >> state.nativeCapture.inventoryWidth
                >> state.nativeCapture.inventoryHeight
                >> state.nativeCapture.checksum
                >> itemCount
                >> state.nativeCapture.itemSerializeHits)
            {
                state.nativeCapture.items.reserve(static_cast<size_t>(std::clamp(itemCount, 0, 256)));
                state.hasNativeCapture = true;
            }
        }
        else if (StartsWith(line, "nativeCaptureWeapons="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeCaptureWeapons="));
            stream >> state.nativeCapture.equippedWeaponId
                >> state.nativeCapture.lastEquippedWeaponId
                >> state.nativeCapture.backupWeaponId
                >> state.nativeCapture.toBeEquippedWeaponId
                >> state.nativeCapture.weaponCount
                >> state.nativeCapture.specialWeaponCount;
        }
        else if (StartsWith(line, "nativeFragmentPayload="))
        {
            unsigned version = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeFragmentPayload="));
            if (stream >> version
                >> state.nativeCapture.nativeFragmentPayloadBytes
                >> state.nativeCapture.nativeFragmentPayloadChecksum
                >> state.nativeCapture.nativeFragmentPayloadRawChecksum
                >> state.nativeCapture.nativeFragmentPayloadRanges
                >> state.nativeCapture.nativeFragmentPayloadInventoryRanges
                >> state.nativeCapture.nativeFragmentPayloadItemGroups
                >> state.nativeCapture.nativeFragmentPayloadItemRanges
                >> state.nativeCapture.nativeFragmentPayloadRawBytes
                >> state.nativeCapture.nativeFragmentPayloadRunId
                >> state.nativeCapture.nativeFragmentPayloadSchemaHash
                >> state.nativeCapture.nativeFragmentPayloadContentHash)
            {
                state.nativeCapture.nativeFragmentPayloadVersion = static_cast<uint16_t>(version);
                state.nativeCapture.hasNativeFragmentPayload = true;
                state.hasNativeCapture = true;
            }
        }
        else if (StartsWith(line, "nativeFragmentPayloadHex="))
        {
            if (!AppendHexBytes(AfterPrefix(line, "nativeFragmentPayloadHex="), state.nativeCapture.nativeFragmentPayload))
                nativeFragmentPayloadHexInvalid = true;
        }
        else if (StartsWith(line, "nativeSnapshotSave="))
        {
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeSnapshotSave="));
            if (stream >> state.nativeCapture.nativeSnapshotSaveBytes
                >> state.nativeCapture.nativeSnapshotSaveChecksum)
            {
                state.nativeCapture.hasNativeSnapshotSave = true;
                state.hasNativeCapture = true;
            }
        }
        else if (StartsWith(line, "nativeSnapshotSaveHex="))
        {
            if (!AppendHexBytes(AfterPrefix(line, "nativeSnapshotSaveHex="), state.nativeCapture.nativeSnapshotSave))
                nativeSnapshotSaveHexInvalid = true;
        }
        else if (StartsWith(line, "nativeItem="))
        {
            NativeCapturedItemState item;
            int isWeapon = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeItem="));
            if (stream >> item.itemId
                >> item.archetypeId
                >> item.count
                >> item.ownerId
                >> item.x
                >> item.y
                >> item.width
                >> item.height
                >> item.category
                >> item.flags
                >> isWeapon
                >> item.weaponCondition
                >> item.weaponAmmoLoaded
                >> item.weaponAmmoCount
                >> item.weaponModCount
                >> item.weaponModTotalLevel)
            {
                item.isWeapon = isWeapon != 0 &&
                    !CoopItemClassification::IsKnownAmmoPickupArchetype(item.archetypeId);
                item.hasCell = true;
                if (item.archetypeId != 0 && item.count > 0)
                    state.nativeCapture.items.push_back(item);
                state.hasNativeCapture = true;
            }
        }
        else if (StartsWith(line, "nativeWeaponMod="))
        {
            unsigned itemId = 0;
            uint64_t modId = 0;
            int level = 0;
            auto stream = MakeClassicInputStream(AfterPrefix(line, "nativeWeaponMod="));
            if (stream >> itemId >> modId >> level && itemId != 0 && modId != 0 && level > 0)
            {
                bool attached = false;
                auto inventoryTarget = std::find_if(
                    state.inventory.rbegin(),
                    state.inventory.rend(),
                    [itemId](const PlayerInventoryItemState& item)
                    {
                        return item.itemId == itemId;
                    });
                if (inventoryTarget != state.inventory.rend())
                {
                    inventoryTarget->weaponMods.emplace_back(modId, level);
                    inventoryTarget->weaponModCount = static_cast<uint32_t>(
                        std::min<size_t>(inventoryTarget->weaponMods.size(), UINT32_MAX));
                    uint32_t totalLevel = 0;
                    for (const auto& mod : inventoryTarget->weaponMods)
                        totalLevel += static_cast<uint32_t>(std::max(0, mod.second));
                    inventoryTarget->weaponModTotalLevel = totalLevel;
                    if (!CoopItemClassification::IsKnownAmmoPickupArchetype(inventoryTarget->archetypeId))
                    {
                        inventoryTarget->isWeapon = true;
                        inventoryTarget->flags |= kPlayerInventoryItemFlagWeapon;
                    }
                    state.hasInventory = true;
                    attached = true;
                }

                auto nativeTarget = std::find_if(
                    state.nativeCapture.items.rbegin(),
                    state.nativeCapture.items.rend(),
                    [itemId](const NativeCapturedItemState& item)
                    {
                        return item.itemId == itemId;
                    });
                if (nativeTarget != state.nativeCapture.items.rend())
                {
                    nativeTarget->weaponMods.emplace_back(modId, level);
                    nativeTarget->weaponModCount = static_cast<uint32_t>(
                        std::min<size_t>(nativeTarget->weaponMods.size(), UINT32_MAX));
                    uint32_t totalLevel = 0;
                    for (const auto& mod : nativeTarget->weaponMods)
                        totalLevel += static_cast<uint32_t>(std::max(0, mod.second));
                    nativeTarget->weaponModTotalLevel = totalLevel;
                    if (!CoopItemClassification::IsKnownAmmoPickupArchetype(nativeTarget->archetypeId))
                        nativeTarget->isWeapon = true;
                    state.hasNativeCapture = true;
                    attached = true;
                }
                (void)attached;
            }
        }
    }

    if (nativeFragmentPayloadHexInvalid)
    {
        LogCoop("loaded player sidecar native fragment payload discarded: invalid hex");
        state.nativeCapture.hasNativeFragmentPayload = false;
        state.nativeCapture.nativeFragmentPayload.clear();
    }
    else if (state.nativeCapture.hasNativeFragmentPayload || !state.nativeCapture.nativeFragmentPayload.empty())
    {
        const CoopNativeFragmentPayload::ParsedPayload parsed =
            CoopNativeFragmentPayload::ParseInventoryPayload(state.nativeCapture.nativeFragmentPayload);
        const bool metadataMatches =
            parsed.ok &&
            (state.nativeCapture.nativeFragmentPayloadBytes == 0 ||
                state.nativeCapture.nativeFragmentPayloadBytes == parsed.totalBytes) &&
            (state.nativeCapture.nativeFragmentPayloadChecksum == 0 ||
                state.nativeCapture.nativeFragmentPayloadChecksum == parsed.payloadChecksum) &&
            (state.nativeCapture.nativeFragmentPayloadRawChecksum == 0 ||
                state.nativeCapture.nativeFragmentPayloadRawChecksum == parsed.rawByteChecksum);
        if (!metadataMatches)
        {
            LogCoop(
                "loaded player sidecar native fragment payload discarded: " +
                CoopNativeFragmentPayload::BuildStatus(parsed));
            state.nativeCapture.hasNativeFragmentPayload = false;
            state.nativeCapture.nativeFragmentPayload.clear();
        }
        else
        {
            state.nativeCapture.hasNativeFragmentPayload = true;
            state.nativeCapture.nativeFragmentPayloadVersion = parsed.version;
            state.nativeCapture.nativeFragmentPayloadBytes = parsed.totalBytes;
            state.nativeCapture.nativeFragmentPayloadChecksum = parsed.payloadChecksum;
            state.nativeCapture.nativeFragmentPayloadRawChecksum = parsed.rawByteChecksum;
            state.nativeCapture.nativeFragmentPayloadRanges = parsed.rangeRecords;
            state.nativeCapture.nativeFragmentPayloadInventoryRanges = parsed.inventoryRanges;
            state.nativeCapture.nativeFragmentPayloadItemGroups = parsed.itemGroups;
            state.nativeCapture.nativeFragmentPayloadItemRanges = parsed.itemRanges;
            state.nativeCapture.nativeFragmentPayloadRawBytes = parsed.rawBytes;
            state.nativeCapture.nativeFragmentPayloadRunId = parsed.runId;
            state.nativeCapture.nativeFragmentPayloadSchemaHash = parsed.schemaHash;
            state.nativeCapture.nativeFragmentPayloadContentHash = parsed.contentHash;
            state.hasNativeCapture = true;
        }
    }

    if (nativeSnapshotSaveHexInvalid)
    {
        LogCoop("loaded player sidecar native snapshot save discarded: invalid hex");
        state.nativeCapture.hasNativeSnapshotSave = false;
        state.nativeCapture.nativeSnapshotSave.clear();
        state.nativeCapture.nativeSnapshotSaveBytes = 0;
        state.nativeCapture.nativeSnapshotSaveChecksum = 0;
    }
    else if (state.nativeCapture.hasNativeSnapshotSave || !state.nativeCapture.nativeSnapshotSave.empty())
    {
        const uint32_t actualBytes = static_cast<uint32_t>(
            std::min<size_t>(state.nativeCapture.nativeSnapshotSave.size(), UINT32_MAX));
        const uint32_t actualChecksum = HashPlayerSidecarBytes(state.nativeCapture.nativeSnapshotSave);
        const bool metadataMatches =
            !state.nativeCapture.nativeSnapshotSave.empty() &&
            (state.nativeCapture.nativeSnapshotSaveBytes == 0 ||
                state.nativeCapture.nativeSnapshotSaveBytes == actualBytes) &&
            (state.nativeCapture.nativeSnapshotSaveChecksum == 0 ||
                state.nativeCapture.nativeSnapshotSaveChecksum == actualChecksum);
        if (!metadataMatches)
        {
            LogCoop(
                "loaded player sidecar native snapshot save discarded: bytes=" +
                std::to_string(actualBytes) +
                " expected=" + std::to_string(state.nativeCapture.nativeSnapshotSaveBytes) +
                " checksum=" + std::to_string(actualChecksum) +
                " expectedChecksum=" + std::to_string(state.nativeCapture.nativeSnapshotSaveChecksum));
            state.nativeCapture.hasNativeSnapshotSave = false;
            state.nativeCapture.nativeSnapshotSave.clear();
            state.nativeCapture.nativeSnapshotSaveBytes = 0;
            state.nativeCapture.nativeSnapshotSaveChecksum = 0;
        }
        else
        {
            state.nativeCapture.hasNativeSnapshotSave = true;
            state.nativeCapture.nativeSnapshotSaveBytes = actualBytes;
            state.nativeCapture.nativeSnapshotSaveChecksum = actualChecksum;
            state.hasNativeCapture = true;
        }
    }

    const size_t passiveInventoryItems = state.inventory.size();
    NormalizePlayerSidecarInventoryFromNativeCapture(state);

    ++m_playerSidecarLoadCount;
    m_lastPlayerSidecarEvent =
        "loaded player sidecar nativeCapture=" +
        std::to_string(state.hasNativeCapture ? 1 : 0) +
        " nativeItems=" + std::to_string(state.nativeCapture.items.size()) +
        " inv=" + std::to_string(state.inventory.size()) +
        "/" + std::to_string(passiveInventoryItems) +
        " chipsets=" + std::to_string(state.chipsets.size()) +
        " nativeFragment=" + std::to_string(state.nativeCapture.hasNativeFragmentPayload ? 1 : 0) +
        " nativeSnapshot=" + std::to_string(state.nativeCapture.hasNativeSnapshotSave ? 1 : 0) +
        "/" + std::to_string(state.nativeCapture.nativeSnapshotSaveBytes) +
        (hasIntegrityHash ? " integrity=hash" : " integrity=legacy");
    return true;
}

bool ModMain::ApplyLocalPlayerSidecar(const PlayerSidecarState& state, const char* reason)
{
    if (!m_enablePlayerSidecar || !IsGameReady())
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "apply failed: game not ready";
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    if (!playerEntity)
    {
        ++m_playerSidecarFailCount;
        m_lastPlayerSidecarEvent = "apply failed: no player entity";
        return false;
    }

    bool appliedPosition = false;
    const bool useNativePlayerCapture = state.hasNativeCapture && state.nativeCapture.sawPlayerWrite;
    const Vec3 playerPosition = useNativePlayerCapture ? state.nativeCapture.position : state.position;
    const Quat playerRotation = useNativePlayerCapture ? state.nativeCapture.rotation : state.rotation;
    const Quat playerViewRotation = useNativePlayerCapture ? state.nativeCapture.viewRotation : state.viewRotation;
    const float playerHealth = useNativePlayerCapture ? state.nativeCapture.health : state.health;
    const float playerPsiPoints = useNativePlayerCapture ? state.nativeCapture.psiPoints : state.psiPoints;
    const bool hasPlayerTransform = useNativePlayerCapture || (state.hasPosition && state.hasRotation);
    const bool hasPlayerViewRotation = useNativePlayerCapture || state.hasViewRotation;
    const bool hasPlayerHealth = useNativePlayerCapture || state.hasHealth;
    const bool hasPlayerPsi = useNativePlayerCapture || state.hasPsi;
    const bool applyPosition = m_applyPlayerSidecarPosition || m_forceNextPlayerSidecarFullApply || m_forceNextPlayerSidecarPositionApply;
    const bool applyVitals = m_applyPlayerSidecarVitals || m_forceNextPlayerSidecarFullApply || m_forceNextPlayerSidecarVitalsApply;
    const bool applyAbilities = m_applyPlayerSidecarAbilities || m_forceNextPlayerSidecarFullApply || m_forceNextPlayerSidecarAbilitiesApply;
    // A received Host player-state is the authoritative per-account profile.
    // It must also clear chipsets inherited from the Host world save even when
    // a diagnostic harness disabled ordinary sidecar chipset application.
    const bool applyChipsets =
        m_applyPlayerSidecarChipsets ||
        m_pendingReceivedPlayerStateApply ||
        m_forceNextPlayerSidecarFullApply ||
        m_forceNextPlayerSidecarInventoryApply;
    const bool applyInventory =
        m_enableExperimentalInventoryRestore &&
        !m_suppressNextPlayerSidecarInventoryApply &&
        (m_applyPlayerSidecarInventory || m_forceNextPlayerSidecarFullApply || m_forceNextPlayerSidecarInventoryApply);
    bool resetTransientState = false;

    if (m_forceNextPlayerSidecarResetTransientState)
    {
        std::string detail;
        resetTransientState = ResetTransientPlayerStateForFreshCoopProfile(player, detail);
        if (!resetTransientState && !detail.empty())
            LogCoop("fresh coop player transient reset failed: " + detail);
    }

    if (applyPosition && hasPlayerTransform)
    {
        const std::string currentLevel = NormalizeLevelName(GetCurrentLevelName());
        if (IsKnownSameLevel(state.levelName, currentLevel))
        {
            if (ShouldApplyPlayerSidecarTransform(*playerEntity, playerPosition, playerRotation))
                appliedPosition = TeleportLocalPlayer(playerPosition, playerRotation);
        }
        else
        {
            m_lastPlayerSidecarEvent =
                "position skipped: sidecar level mismatch sidecar=" +
                (state.levelName.empty() ? std::string("unknown") : state.levelName) +
                " current=" + currentLevel;
        }
    }

    if (applyPosition && hasPlayerViewRotation && ShouldApplyPlayerSidecarViewRotation(player, playerViewRotation))
        player.SetViewRotation(playerViewRotation);

    if (applyPosition && useNativePlayerCapture && state.nativeCapture.stance > static_cast<int>(EStance::STANCE_NULL))
        SetLocalPlayerStanceSafe(state.nativeCapture.stance, "apply native player sidecar stance");

    uint32_t appliedAbilities = 0;
    if (applyAbilities && state.hasAbilities)
    {
        ArkAbilityComponent& abilityComponent = player.m_playerComponent.GetAbilityComponent();
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
            {
                abilityComponent.GrantAbility(ability.abilityId);
                ++appliedAbilities;
            }
        }

        abilityComponent.UpdatePlayerMetrics();
        std::string turretRefreshReason;
        if (!TryGuardedVoidCall(
                "refresh turret typhon hostility after sidecar abilities",
                []()
                {
                    ArkTurret::OnNeuromodUsed();
                },
                &turretRefreshReason) &&
            !turretRefreshReason.empty())
        {
            LogCoop("player sidecar turret hostility refresh warning: " + turretRefreshReason);
        }

        if (appliedAbilities == 0)
        {
            ArkPsiPowerComponent& psiPower = player.GetPsiPowerComponent();
            psiPower.Stop();
            psiPower.StopLatentPowers();
            psiPower.ClearUITargets();
            psiPower.ClearSelectedPower();
            psiPower.ClearEquippedPower();
            psiPower.DisableTargetedPowers(false);
            psiPower.m_bPreventWeaponFireOnHold = false;
            psiPower.m_bTargetedPowersDisabled = false;
        }
    }

    if (applyAbilities && hasPlayerPsi)
    {
        CArkPsiComponent& psiComponent = player.m_playerComponent.GetPsiComponent();
        psiComponent.SetPoints(std::max(0.0f, playerPsiPoints));
        if (useNativePlayerCapture)
        {
            ArkPsiPowerComponent& psiPower = player.GetPsiPowerComponent();
            psiPower.m_selectedPower = static_cast<EArkPsiPowers>(state.nativeCapture.selectedPsiPower);
            psiPower.m_equippedPower = static_cast<EArkPsiPowers>(state.nativeCapture.equippedPsiPower);
            psiPower.m_bTargetedPowersDisabled = false;
        }
    }

    if (applyVitals && hasPlayerHealth)
    {
        const float playerMaxHealth = useNativePlayerCapture ? state.nativeCapture.maxHealth : state.maxHealth;
        if (std::isfinite(playerMaxHealth) && playerMaxHealth > 0.0f)
        {
            std::string maxHealthReason;
            TryGuardedVoidCall(
                "apply player sidecar max health",
                [&]() { player.SetMaxHealth(playerMaxHealth); },
                &maxHealthReason);
            if (!maxHealthReason.empty())
                LogCoop("player sidecar max health warning: " + maxHealthReason);
        }
        const bool sidecarDowned = (state.flags & kPlayerSidecarFlagDowned) != 0;
        if (m_downedModeEnabled && sidecarDowned)
        {
            EnterLocalDowned(0, false, false);
            SetLocalPlayerHealthSafe(std::max(1.0f, playerHealth), "apply player sidecar downed health");
        }
        else
        {
            if (m_localPlayerDowned)
                ReviveLocalPlayer(std::max(1.0f, playerHealth), false);
            else
                SetLocalPlayerHealthSafe(std::max(1.0f, playerHealth), "apply player sidecar health");
        }
    }

    if (applyVitals && state.hasOxygen && player.m_helmet.m_pOxygenComponent)
    {
        ArkPlayerOxygenComponent* oxygen = player.m_helmet.m_pOxygenComponent.get();
        std::string oxygenReason;
        TryGuardedVoidCall(
            "apply player sidecar oxygen",
            [oxygen, &state]()
            {
                const float nativeMax = oxygen->GetMaxOxygen();
                const float savedFraction = state.maxOxygen > 0.0f
                    ? std::clamp(state.oxygen / state.maxOxygen, 0.0f, 1.0f)
                    : 0.0f;
                oxygen->SetOxygen(std::max(0.0f, nativeMax * savedFraction));
                oxygen->SetConsumingOxygen(state.oxygenConsuming, false);
                oxygen->PostSerialize();
            },
            &oxygenReason);
        if (!oxygenReason.empty())
            LogCoop("player sidecar oxygen restore warning: " + oxygenReason);
    }

    if (applyVitals && state.hasStatuses)
    {
        ArkPlayerStatusComponent& statusComponent = player.m_playerComponent.GetStatusComponent();
        std::string statusReason;
        TryGuardedVoidCall(
            "apply player sidecar statuses",
            [&]()
            {
                statusComponent.ClearAllStatuses();
                for (const std::unique_ptr<ArkTraumaBase>& trauma : statusComponent.m_statuses)
                {
                    if (trauma)
                        trauma->Reset();
                }
                for (const PlayerStatusState& row : state.statuses)
                {
                    ArkTraumaBase* trauma = statusComponent.GetTraumaForStatus(static_cast<EArkPlayerStatus>(row.status));
                    if (!trauma)
                        continue;
                    if (row.amount > 0.0f)
                        trauma->Accumulate(row.amount);
                    if (row.level > trauma->m_currentLevel)
                        trauma->Activate(row.level);
                    if (row.suspended)
                        trauma->Suspend();
                }
                statusComponent.PostSerialize();
            },
            &statusReason);
        if (!statusReason.empty())
            LogCoop("player sidecar status restore warning: " + statusReason);
    }

    uint32_t queuedInventoryItems = 0;
    uint32_t queuedChipsets = 0;
    if (applyInventory && state.hasInventory)
    {
        m_pendingPlayerSidecarEquippedWeaponArchetypeId = state.equippedWeaponArchetypeId;
        m_pendingPlayerSidecarQuickSelect = state.quickSelect;
        m_pendingPlayerSidecarHasQuickSelect = state.hasQuickSelect;
        queuedInventoryItems = static_cast<uint32_t>(state.inventory.size());
        QueuePlayerSidecarInventoryRestore(state.inventory, true, reason && reason[0] ? reason : "player sidecar apply");
    }
    else if (state.hasInventory && !m_enableExperimentalInventoryRestore)
    {
        LogCoop(
            "sidecar inventory restore skipped by production guard items=" +
            std::to_string(state.inventory.size()));
    }

    if (applyChipsets && state.hasChipsets)
    {
        queuedChipsets = static_cast<uint32_t>(std::min<size_t>(state.chipsets.size(), UINT32_MAX));
        QueuePlayerSidecarChipsetRestore(state.chipsets, reason && reason[0] ? reason : "player sidecar apply");
    }

    if ((!applyInventory || !state.hasInventory) &&
        !m_pendingPlayerSidecarInventoryRestore &&
        (state.equippedWeaponArchetypeId != 0 || state.hasQuickSelect))
    {
        RestorePlayerSidecarEquipment(state, reason && reason[0] ? reason : "player sidecar apply");
    }

    ClearLocalPlayerModalStateAfterSidecarApply(reason && reason[0] ? reason : "player sidecar apply");

    ++m_playerSidecarApplyCount;
    m_playerSidecarAbilitiesApplied = appliedAbilities;
    m_lastPlayerSidecarEvent = std::string("applied player sidecar") +
        (appliedPosition ? " pos" : "") +
        (useNativePlayerCapture ? " native-player" : "") +
        (resetTransientState ? " transient-reset" : "") +
        (applyAbilities && state.hasAbilities ? " abilities" : "") +
        (applyVitals && state.hasOxygen
            ? " oxygen-saved=" + std::to_string(state.oxygen) +
                "/" + std::to_string(state.maxOxygen) +
                "/" + std::to_string(state.oxygenConsuming ? 1 : 0)
            : "") +
        (applyInventory && state.hasInventory ? " inv-queued=" + std::to_string(queuedInventoryItems) : "") +
        (applyChipsets && state.hasChipsets ? " chipsets-queued=" + std::to_string(queuedChipsets) : "") +
        (m_playerSidecarInventoryPending > 0 ? " inv-pending=" + std::to_string(m_playerSidecarInventoryPending) : "") +
        (m_playerSidecarChipsetsPending > 0 ? " chipsets-pending=" + std::to_string(m_playerSidecarChipsetsPending) : "") +
        (reason && reason[0] ? ": " + std::string(reason) : "");
    LogCoop(m_lastPlayerSidecarEvent);
    return true;
}

bool ModMain::RestorePlayerSidecarEquipment(const PlayerSidecarState& state, const char* reason)
{
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!player || !inventory || !itemSystem)
        return false;

    std::string guardReason;
    std::vector<unsigned> itemIds;
    if (!TryGetInventoryItemIds(*inventory, itemIds, guardReason))
        return false;

    auto findWeaponId = [&](uint64_t archetypeId) -> unsigned
    {
        if (archetypeId == 0)
            return 0;
        for (const unsigned itemId : itemIds)
        {
            IArkItem* rawItem = FindArkItemDirect(itemSystem, itemId);
            CArkItem* item = rawItem && IsLikelyRuntimeCppObject(rawItem)
                ? static_cast<CArkItem*>(rawItem)
                : nullptr;
            if (item && IsLikelyRuntimeCppObject(item, sizeof(CArkItem)) &&
                item->m_selectedArchetype == archetypeId &&
                CoopItemClassification::IsRealWeaponInventoryItem(item, guardReason))
            {
                return itemId;
            }
        }
        return 0;
    };

    bool equipped = state.equippedWeaponArchetypeId == 0;
    const unsigned equippedId = findWeaponId(state.equippedWeaponArchetypeId);
    if (equippedId != 0)
    {
        equipped = TryGuardedCall(
            "player sidecar EquipWeapon",
            [&]() { return player->m_weaponComponent.EquipWeapon(equippedId); },
            equipped,
            &guardReason) && equipped;
    }

    bool quickSelectApplied = !state.hasQuickSelect;
    if (state.hasQuickSelect)
    {
        ArkQuickSelectComponent& quickSelect = player->m_playerComponent.GetQuickSelectComponent();
        quickSelectApplied = TryGuardedVoidCall(
            "player sidecar quickselect restore",
            [&]()
            {
                quickSelect.CloseQuickSelect();
                quickSelect.Reset();
                for (const PlayerQuickSelectState& row : state.quickSelect)
                {
                    ArkQuickSelectComponent::QuickSelectId* selection = nullptr;
                    if (row.bank == 0 && row.index >= 0 && row.index < static_cast<int>(quickSelect.m_controllerQuickSelects.size()))
                        selection = &quickSelect.m_controllerQuickSelects[static_cast<size_t>(row.index)];
                    else if (row.bank == 1 && row.index >= 0 && row.index < static_cast<int>(quickSelect.m_keyboardQuickSelects.size()))
                        selection = &quickSelect.m_keyboardQuickSelects[static_cast<size_t>(row.index)];
                    if (!selection)
                        continue;

                    if (row.type == static_cast<int>(ArkQuickSelectComponent::QuickSelectType::weapon))
                    {
                        const unsigned itemId = findWeaponId(row.stableId);
                        if (itemId == 0)
                            continue;
                        selection->m_type = ArkQuickSelectComponent::QuickSelectType::weapon;
                        selection->m_id = itemId;
                    }
                    else if (row.type == static_cast<int>(ArkQuickSelectComponent::QuickSelectType::power))
                    {
                        selection->m_type = ArkQuickSelectComponent::QuickSelectType::power;
                        selection->m_id = static_cast<EArkPsiPowers>(row.stableId);
                    }
                }
                quickSelect.RefreshFilterFeedback();
            },
            &guardReason);
    }

    m_pendingPlayerSidecarEquippedWeaponArchetypeId = 0;
    m_pendingPlayerSidecarQuickSelect.clear();
    m_pendingPlayerSidecarHasQuickSelect = false;
    LogCoop(
        "player sidecar equipment restored equipped=" + std::to_string(equipped ? 1 : 0) +
        " quickSelect=" + std::to_string(quickSelectApplied ? 1 : 0) +
        " rows=" + std::to_string(state.quickSelect.size()) +
        (reason && reason[0] ? " reason=" + std::string(reason) : std::string()) +
        (guardReason.empty() ? std::string() : " guard=" + guardReason));
    return equipped && quickSelectApplied;
}

void ModMain::QueuePlayerSidecarInventoryRestore(
    const std::vector<PlayerInventoryItemState>& items,
    bool clearInventory,
    const char* reason)
{
    m_pendingPlayerSidecarInventoryItems = items;
    m_pendingPlayerSidecarInventoryRestore = !m_pendingPlayerSidecarInventoryItems.empty() || clearInventory;
    m_pendingPlayerSidecarInventoryRestoreNeedsClear = clearInventory;
    m_playerSidecarInventoryPending = static_cast<uint32_t>(m_pendingPlayerSidecarInventoryItems.size());
    m_playerSidecarInventoryRestoreAccumulator = m_playerSidecarInventoryNativeRestoreReady && !clearInventory ?
        kPlayerSidecarInventoryRestoreRetrySeconds :
        0.0f;
    m_playerSidecarInventoryRestoreAttempts = 0;
    m_lastPlayerSidecarEvent = "queued inventory restore items=" +
        std::to_string(m_playerSidecarInventoryPending) +
        (reason && reason[0] ? ": " + std::string(reason) : "");
    LogCoop(m_lastPlayerSidecarEvent);
}

bool ModMain::TryRestorePendingPlayerSidecarInventory(const char* reason, bool nativeRestoreWindow)
{
    if (!m_pendingPlayerSidecarInventoryRestore)
        return true;

    if (!IsGameReady())
    {
        m_lastPlayerSidecarEvent = "inventory restore waiting: game not ready";
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!playerEntity || !inventory || !itemSystem)
    {
        m_lastPlayerSidecarEvent = "inventory restore waiting: native inventory unavailable";
        return false;
    }

    ++m_playerSidecarInventoryRestoreAttempts;
    PlayerSidecarInventoryFeedbackSuppressGuard feedbackSuppress(*this, reason);
    const bool previousNativeRestoreActive = m_playerSidecarInventoryNativeRestoreActive;
    m_playerSidecarInventoryNativeRestoreActive = nativeRestoreWindow;
    struct NativeRestoreActiveGuard
    {
        bool& active;
        bool previous;
        ~NativeRestoreActiveGuard()
        {
            active = previous;
        }
    } nativeRestoreActiveGuard{
        m_playerSidecarInventoryNativeRestoreActive,
        previousNativeRestoreActive};

    std::string guardReason;
    if (m_pendingPlayerSidecarInventoryRestoreNeedsClear)
    {
        if (!nativeRestoreWindow && m_saveLoadGuardActive)
        {
            m_lastPlayerSidecarEvent = "inventory restore waiting: load guard active before clear";
            return false;
        }

        if (!TryGuardedVoidCall(
                "coop inventory restore RemoveAllItems",
                [&]()
                {
                    inventory->RemoveAllItems();
                },
                &guardReason))
        {
            m_lastPlayerSidecarEvent = "inventory restore waiting: remove all failed: " + guardReason;
            LogCoop(m_lastPlayerSidecarEvent);
            return false;
        }

        if (!nativeRestoreWindow)
            LogCoop("inventory restore used guarded runtime clear");

        m_pendingPlayerSidecarInventoryRestoreNeedsClear = false;
        m_playerSidecarInventoryApplied = 0;
        ReconcileLocalPlayerWeaponsFromInventory("sidecar inventory native clear", true);
    }

    const EntityId playerEntityId = playerEntity->GetId();
    std::vector<PlayerInventoryItemState> remaining;
    remaining.reserve(m_pendingPlayerSidecarInventoryItems.size());

    uint32_t restoredThisPass = 0;
    for (const PlayerInventoryItemState& item : m_pendingPlayerSidecarInventoryItems)
    {
        if (item.archetypeId == 0 || item.count <= 0)
            continue;

        guardReason.clear();
        if (TraceInventoryRestoreEnabled())
        {
            LogCoop("inventory restore give arch=" + std::to_string(item.archetypeId) +
                " count=" + std::to_string(item.count) +
                " cell=" + std::to_string(item.x) + "," + std::to_string(item.y) +
                " distinct=" + std::to_string(IsSidecarDistinctEntityItem(item) ? 1 : 0));
        }
        if (TryRestoreInventoryItem(*inventory, *itemSystem, playerEntityId, item, guardReason))
        {
            ++restoredThisPass;
            ++m_playerSidecarInventoryApplied;
            if (TraceInventoryRestoreEnabled())
            {
                LogCoop("inventory restore applied arch=" + std::to_string(item.archetypeId) +
                    " count=" + std::to_string(item.count) +
                    (guardReason.empty() ? std::string() : ": " + guardReason));
            }
            continue;
        }

        remaining.push_back(item);
        LogCoop("inventory restore pending arch=" + std::to_string(item.archetypeId) +
            " count=" + std::to_string(item.count) + ": " + guardReason);
    }

    m_pendingPlayerSidecarInventoryItems.swap(remaining);
    m_playerSidecarInventoryPending = static_cast<uint32_t>(m_pendingPlayerSidecarInventoryItems.size());
    if (m_playerSidecarInventoryPending == 0)
    {
        m_pendingPlayerSidecarInventoryRestore = false;
        m_pendingPlayerSidecarInventoryRestoreNeedsClear = false;
        m_playerSidecarInventoryRestoreAccumulator = 0.0f;
        m_lastPlayerSidecarEvent = "inventory restore complete applied=" +
            std::to_string(m_playerSidecarInventoryApplied) +
            (reason && reason[0] ? ": " + std::string(reason) : "");
        LogCoop(m_lastPlayerSidecarEvent);
        ReconcileLocalPlayerWeaponsFromInventory(reason && reason[0] ? reason : "player sidecar inventory restore", false);
        if (m_pendingPlayerSidecarEquippedWeaponArchetypeId != 0 || m_pendingPlayerSidecarHasQuickSelect)
        {
            PlayerSidecarState equipmentState;
            equipmentState.equippedWeaponArchetypeId = m_pendingPlayerSidecarEquippedWeaponArchetypeId;
            equipmentState.hasQuickSelect = m_pendingPlayerSidecarHasQuickSelect;
            equipmentState.quickSelect = m_pendingPlayerSidecarQuickSelect;
            RestorePlayerSidecarEquipment(
                equipmentState,
                reason && reason[0] ? reason : "player sidecar inventory restore");
        }
        if (!nativeRestoreWindow)
            ClearLocalPlayerModalStateAfterSidecarApply(reason && reason[0] ? reason : "player sidecar inventory restore");
        return true;
    }

    m_lastPlayerSidecarEvent = "inventory restore pending=" +
        std::to_string(m_playerSidecarInventoryPending) +
        " restoredThisPass=" + std::to_string(restoredThisPass) +
        " attempts=" + std::to_string(m_playerSidecarInventoryRestoreAttempts);
    LogCoop(m_lastPlayerSidecarEvent);
    return false;
}

void ModMain::QueuePlayerSidecarChipsetRestore(
    const std::vector<PlayerChipsetState>& chipsets,
    const char* reason)
{
    m_pendingPlayerSidecarChipsets = chipsets;
    m_pendingPlayerSidecarChipsetRestore = true;
    m_playerSidecarChipsetsPending = static_cast<uint32_t>(
        std::min<size_t>(m_pendingPlayerSidecarChipsets.size(), UINT32_MAX));
    m_playerSidecarChipsetsApplied = 0;
    m_lastPlayerSidecarEvent = "queued chipset restore items=" +
        std::to_string(m_playerSidecarChipsetsPending) +
        (reason && reason[0] ? ": " + std::string(reason) : "");
    LogCoop(m_lastPlayerSidecarEvent);
}

bool ModMain::TryRestorePendingPlayerSidecarChipsets(const char* reason)
{
    if (!m_pendingPlayerSidecarChipsetRestore)
        return true;

    if (m_pendingPlayerSidecarInventoryRestore || m_playerSidecarInventoryPending > 0)
    {
        m_lastPlayerSidecarEvent = "chipset restore waiting: inventory restore pending";
        return false;
    }

    if (!IsGameReady())
    {
        m_lastPlayerSidecarEvent = "chipset restore waiting: game not ready";
        return false;
    }

    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!inventory || !itemSystem)
    {
        m_lastPlayerSidecarEvent = "chipset restore waiting: native inventory unavailable";
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    if (!playerEntity)
    {
        m_lastPlayerSidecarEvent = "chipset restore waiting: player entity unavailable";
        return false;
    }
    std::string detail;
    std::string guardReason;
    uint32_t applied = 0;
    const bool suitOk = RestoreChipsetComponent(
        player.m_suitChipsetComponent,
        *inventory,
        *itemSystem,
        playerEntity->GetId(),
        m_pendingPlayerSidecarChipsets,
        0,
        applied,
        detail,
        guardReason);
    const bool scopeOk = RestoreChipsetComponent(
        player.m_scopeChipsetComponent,
        *inventory,
        *itemSystem,
        playerEntity->GetId(),
        m_pendingPlayerSidecarChipsets,
        1,
        applied,
        detail,
        guardReason);

    m_playerSidecarChipsetsApplied = applied;
    if (!suitOk || !scopeOk)
    {
        m_playerSidecarChipsetsPending = static_cast<uint32_t>(
            std::min<size_t>(m_pendingPlayerSidecarChipsets.size(), UINT32_MAX));
        m_lastPlayerSidecarEvent =
            "chipset restore waiting applied=" + std::to_string(applied) +
            (detail.empty() ? std::string() : " " + detail) +
            (reason && reason[0] ? ": " + std::string(reason) : "");
        LogCoop(m_lastPlayerSidecarEvent);
        return false;
    }

    m_pendingPlayerSidecarChipsetRestore = false;
    m_pendingPlayerSidecarChipsets.clear();
    m_playerSidecarChipsetsPending = 0;
    m_lastPlayerSidecarEvent =
        std::string("chipset restore ") +
        (suitOk && scopeOk ? "complete" : "partial") +
        " applied=" + std::to_string(applied) +
        (detail.empty() ? std::string() : " " + detail) +
        (reason && reason[0] ? ": " + std::string(reason) : "");
    LogCoop(m_lastPlayerSidecarEvent);
    return suitOk && scopeOk;
}

bool ModMain::DebugGiveLocalInventoryArchetype(uint64_t archetypeId, int count, std::string& detail)
{
    detail.clear();
    m_debugInventoryArchetypeId = archetypeId;
    m_debugInventoryCount = -1;

    if (archetypeId == 0 || count <= 0)
    {
        detail = "invalid archetype/count";
        m_lastDebugInventoryEvent = "give failed: " + detail;
        return false;
    }

    if (!IsGameReady())
    {
        detail = "game not ready";
        m_lastDebugInventoryEvent = "give failed: " + detail;
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!playerEntity || !inventory || !itemSystem)
    {
        detail = "native inventory unavailable";
        m_lastDebugInventoryEvent = "give failed: " + detail;
        return false;
    }

    std::string guardReason;
    PlayerInventoryItemState item;
    item.archetypeId = archetypeId;
    item.count = count;
    item.x = -1;
    item.y = -1;
    item.flags = kPlayerInventoryItemFlagStackable;
    const bool ok = TryRestoreInventoryItem(
        *inventory,
        *itemSystem,
        playerEntity->GetId(),
        item,
        guardReason);

    int currentCount = 0;
    if (TryGetInventoryArchetypeCount(*inventory, archetypeId, currentCount, guardReason))
        m_debugInventoryCount = currentCount;

    if (!ok)
    {
        detail = guardReason.empty() ? std::string("give failed") : guardReason;
        m_lastDebugInventoryEvent =
            "give failed arch=" + std::to_string(archetypeId) +
            " count=" + std::to_string(count) +
            ": " + detail;
        LogCoop(m_lastDebugInventoryEvent);
        return false;
    }

    ++m_debugInventoryGiveCount;
    m_lastDebugInventoryEvent =
        "gave inventory arch=" + std::to_string(archetypeId) +
        " count=" + std::to_string(count) +
        " now=" + std::to_string(m_debugInventoryCount);
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

bool ModMain::DebugRunConsoleCommand(const std::string& commandLine, bool deferExecution, std::string& detail)
{
    detail.clear();
    if (commandLine.empty())
    {
        detail = "empty command";
        m_lastDebugInventoryEvent = "console failed: " + detail;
        return false;
    }

    if (!gEnv || !gEnv->pConsole)
    {
        detail = "console unavailable";
        m_lastDebugInventoryEvent = "console failed: " + detail;
        return false;
    }

    std::string guardReason;
    const bool ok = TryGuardedVoidCall(
        "coop runtime raw console",
        [&]()
        {
            gEnv->pConsole->ExecuteString(commandLine.c_str(), false, deferExecution);
        },
        &guardReason);

    if (!ok)
    {
        detail = guardReason.empty() ? std::string("ExecuteString failed") : guardReason;
        m_lastDebugInventoryEvent = "console failed: " + detail;
        LogCoop(m_lastDebugInventoryEvent);
        return false;
    }

    detail = std::string("console accepted defer=") + (deferExecution ? "1 " : "0 ") + commandLine;
    m_lastDebugInventoryEvent = detail;
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

bool ModMain::DebugGiveLocalInventoryCommand(const std::string& archetypeName, int count, std::string& detail)
{
    detail.clear();
    if (archetypeName.empty() || count <= 0)
    {
        detail = "invalid archetype/count";
        m_lastDebugInventoryEvent = "give-name failed: " + detail;
        return false;
    }

    if (!IsGameReady())
    {
        detail = "game not ready";
        m_lastDebugInventoryEvent = "give-name failed: " + detail;
        return false;
    }

    std::string guardReason;
    const bool ok = TryGiveInventoryItemWithConsole(archetypeName, count, guardReason);
    if (!ok)
    {
        detail = guardReason.empty() ? std::string("console give failed") : guardReason;
        m_lastDebugInventoryEvent =
            "give-name failed " + archetypeName +
            " count=" + std::to_string(count) +
            ": " + detail;
        LogCoop(m_lastDebugInventoryEvent);
        return false;
    }

    ++m_debugInventoryGiveCount;
    m_lastDebugInventoryEvent =
        "gave inventory name=" + archetypeName +
        " count=" + std::to_string(count);
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

bool ModMain::DebugProbeLocalInventoryArchetype(uint64_t archetypeId, std::string& detail)
{
    detail.clear();
    m_debugInventoryArchetypeId = archetypeId;
    m_debugInventoryCount = -1;

    if (archetypeId == 0)
    {
        detail = "invalid archetype";
        m_lastDebugInventoryEvent = "probe failed: " + detail;
        return false;
    }

    if (!IsGameReady())
    {
        detail = "game not ready";
        m_lastDebugInventoryEvent = "probe failed: " + detail;
        return false;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    ArkInventory* inventory = GetLocalArkInventory();
    ArkItemSystem* itemSystem = GetArkItemSystemPtr();
    if (!playerEntity || !inventory || !itemSystem)
    {
        detail = "native inventory unavailable";
        m_lastDebugInventoryEvent = "probe failed: " + detail;
        return false;
    }

    std::string reason;
    AppendDetail(detail, "playerEntity=" + std::to_string(playerEntity->GetId()));

    unsigned ownerId = 0;
    if (TryGuardedCall(
            "coop probe inventory GetOwnerId",
            [&]() -> unsigned
            {
                return inventory->GetOwnerId();
            },
            ownerId,
            &reason))
    {
        AppendDetail(detail, "inventoryOwner=" + std::to_string(ownerId));
    }
    else
    {
        AppendDetail(detail, "inventoryOwner failed: " + reason);
    }

    int count = -1;
    if (TryGetInventoryArchetypeCount(*inventory, archetypeId, count, reason))
    {
        m_debugInventoryCount = count;
        AppendDetail(detail, "inventoryCount=" + std::to_string(count));
    }
    else
    {
        AppendDetail(detail, "inventoryCount failed: " + reason);
    }

    std::vector<unsigned int> ids;
    if (TryGetInventoryItemIds(*inventory, ids, reason))
        AppendDetail(detail, "inventoryIds=" + std::to_string(ids.size()));

    IEntityArchetype* entityArchetype = nullptr;
    if (TryGetArchetypePointer(archetypeId, entityArchetype, reason))
    {
        AppendDetail(detail, "entitySystem=" + ArchetypeSummary(entityArchetype));
        std::string commandName;
        if (TryBuildArchetypeCommandName(*entityArchetype, commandName, reason))
            AppendDetail(detail, "commandName=" + commandName);
    }
    else
    {
        AppendDetail(detail, "entitySystem failed: " + reason);
    }

    const KnownPickupArchetypeInfo* knownInfo = LookupKnownPickupArchetypeInfo(archetypeId);
    if (knownInfo)
    {
        AppendDetail(detail, std::string("known=") + knownInfo->className + "/" + knownInfo->archetypeName);

        IEntityArchetype* itemSystemArchetype = nullptr;
        reason.clear();
        if (TryGetArkItemSystemArchetype(*itemSystem, *knownInfo, itemSystemArchetype, reason))
            AppendDetail(detail, "itemSystem=" + ArchetypeSummary(itemSystemArchetype));
        else
            AppendDetail(detail, "itemSystem failed: " + reason);

        std::vector<IEntityArchetype*> classArchetypes;
        reason.clear();
        if (TryGuardedCall(
                "coop probe GetArchetypesForClass",
                [&]() -> std::vector<IEntityArchetype*>
                {
                    return itemSystem->GetArchetypesForClass(knownInfo->className);
                },
                classArchetypes,
                &reason))
        {
            bool foundClassMatch = false;
            for (IEntityArchetype* candidate : classArchetypes)
            {
                uint64_t candidateId = 0;
                if (candidate &&
                    TryGuardedCall("coop probe class candidate GetId", [candidate]() -> uint64_t { return candidate->GetId(); }, candidateId, &reason) &&
                    candidateId == archetypeId)
                {
                    foundClassMatch = true;
                    break;
                }
            }
            AppendDetail(detail, "classArchetypes=" + std::to_string(classArchetypes.size()) +
                " found=" + (foundClassMatch ? std::string("1") : std::string("0")));
        }
        else
        {
            AppendDetail(detail, "classArchetypes failed: " + reason);
        }
    }
    else
    {
        AppendDetail(detail, "known=0");
    }

    std::vector<IEntityArchetype*> pickupArchetypes;
    reason.clear();
    if (TryGuardedCall(
            "coop probe GetPickupArchetypes",
            [&]() -> std::vector<IEntityArchetype*>
            {
                return itemSystem->GetPickupArchetypes();
            },
            pickupArchetypes,
            &reason))
    {
        bool foundPickupMatch = false;
        for (IEntityArchetype* candidate : pickupArchetypes)
        {
            uint64_t candidateId = 0;
            if (candidate &&
                TryGuardedCall("coop probe pickup candidate GetId", [candidate]() -> uint64_t { return candidate->GetId(); }, candidateId, &reason) &&
                candidateId == archetypeId)
            {
                foundPickupMatch = true;
                break;
            }
        }
        AppendDetail(detail, "pickupArchetypes=" + std::to_string(pickupArchetypes.size()) +
            " found=" + (foundPickupMatch ? std::string("1") : std::string("0")));
    }
    else
    {
        AppendDetail(detail, "pickupArchetypes failed: " + reason);
    }

    ++m_debugInventoryCountChecks;
    m_lastDebugInventoryEvent =
        "probe inventory arch=" + std::to_string(archetypeId) +
        " count=" + std::to_string(m_debugInventoryCount);
    LogCoop(m_lastDebugInventoryEvent + ": " + detail);
    return true;
}

bool ModMain::DebugClearLocalInventory(std::string& detail)
{
    detail.clear();
    m_debugInventoryCount = -1;

    if (!IsGameReady())
    {
        detail = "game not ready";
        m_lastDebugInventoryEvent = "clear failed: " + detail;
        return false;
    }

    ArkInventory* inventory = GetLocalArkInventory();
    if (!inventory)
    {
        detail = "native inventory unavailable";
        m_lastDebugInventoryEvent = "clear failed: " + detail;
        return false;
    }

    std::string guardReason;
    std::vector<EntityId> storedItemIds;
    size_t storedCount = 0;
    if (TryGuardedCall(
            "coop debug inventory clear stored ids",
            [inventory, &storedItemIds]() -> size_t
            {
                storedItemIds.reserve(inventory->m_storedItems.size());
                for (const ArkInventory::StorageCell& cell : inventory->m_storedItems)
                {
                    const EntityId itemId = static_cast<EntityId>(cell.m_entityId);
                    if (itemId != INVALID_ENTITYID &&
                        std::find(storedItemIds.begin(), storedItemIds.end(), itemId) == storedItemIds.end())
                    {
                        storedItemIds.push_back(itemId);
                    }
                }
                return inventory->m_storedItems.size();
            },
            storedCount,
            &guardReason) &&
        storedCount == 0)
    {
        ++m_debugInventoryClearCount;
        m_lastDebugInventoryEvent = "cleared local inventory already empty";
        LogCoop(m_lastDebugInventoryEvent);
        return true;
    }

    const bool nativeCleared = TryGuardedVoidCall(
            "coop debug inventory RemoveAllItems",
            [&]()
            {
                inventory->RemoveAllItems();
            },
            &guardReason);
    if (!nativeCleared)
    {
        const std::string nativeReason = guardReason.empty() ? std::string("remove all failed") : guardReason;
        guardReason.clear();
        if (!TryGuardedVoidCall(
                "coop debug inventory storage cell clear fallback",
                [&]()
                {
                    inventory->m_storedItems.clear();
                },
                &guardReason))
        {
            detail = nativeReason +
                (guardReason.empty() ? std::string() : " fallback=" + guardReason);
            m_lastDebugInventoryEvent = "clear failed: " + detail;
            LogCoop(m_lastDebugInventoryEvent);
            return false;
        }
        detail = "storage fallback after " + nativeReason;
    }

    // RemoveAllItems detaches storage cells but can leave the old item
    // entities alive until a later lifecycle pass. A recovered test save with
    // dozens of such items then carries their dynamic IDs across multiple
    // levels and can collide with an authored destination entity. This command
    // is explicitly destructive, so finish the requested clear while the
    // current world is stable instead of deferring those orphan entities into
    // a level transition.
    uint32_t removedOrMissing = 0;
    uint32_t removeFailures = 0;
    for (EntityId itemId : storedItemIds)
    {
        if (RemoveCoopEntityGuarded(itemId, true, "debug inventory clear detached item"))
            ++removedOrMissing;
        else
            ++removeFailures;
    }
    if (removeFailures != 0)
    {
        detail =
            "detached item removal failed removedOrMissing=" + std::to_string(removedOrMissing) +
            " failures=" + std::to_string(removeFailures);
        m_lastDebugInventoryEvent = "clear failed: " + detail;
        LogCoop(m_lastDebugInventoryEvent);
        return false;
    }

    ++m_debugInventoryClearCount;
    m_lastDebugInventoryEvent =
        std::string(nativeCleared ? "cleared local inventory" : "cleared local inventory via storage fallback") +
        " detached=" + std::to_string(storedItemIds.size()) +
        " removedOrMissing=" + std::to_string(removedOrMissing) +
        (detail.empty() ? std::string() : " " + detail);
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

bool ModMain::DebugCountLocalInventoryArchetype(uint64_t archetypeId, int& outCount, std::string& detail)
{
    detail.clear();
    outCount = -1;
    m_debugInventoryArchetypeId = archetypeId;
    m_debugInventoryCount = -1;

    if (archetypeId == 0)
    {
        detail = "invalid archetype";
        m_lastDebugInventoryEvent = "count failed: " + detail;
        return false;
    }

    if (!IsGameReady())
    {
        detail = "game not ready";
        m_lastDebugInventoryEvent = "count failed: " + detail;
        return false;
    }

    ArkInventory* inventory = GetLocalArkInventory();
    if (!inventory)
    {
        detail = "native inventory unavailable";
        m_lastDebugInventoryEvent = "count failed: " + detail;
        return false;
    }

    std::string guardReason;
    if (!TryGetInventoryArchetypeCount(*inventory, archetypeId, outCount, guardReason))
    {
        detail = guardReason.empty() ? std::string("count failed") : guardReason;
        m_lastDebugInventoryEvent =
            "count failed arch=" + std::to_string(archetypeId) +
            ": " + detail;
        LogCoop(m_lastDebugInventoryEvent);
        return false;
    }

    ++m_debugInventoryCountChecks;
    m_debugInventoryCount = outCount;
    m_lastDebugInventoryEvent =
        "count inventory arch=" + std::to_string(archetypeId) +
        " count=" + std::to_string(outCount);
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

bool ModMain::DebugQueueInventoryRestoreFixture(uint64_t archetypeId, int count, bool clearInventory, std::string& detail)
{
    detail.clear();
    if (archetypeId == 0 || count <= 0)
    {
        detail = "invalid archetype/count";
        m_lastDebugInventoryEvent = "restore-fixture failed: " + detail;
        return false;
    }

    PlayerInventoryItemState item;
    item.archetypeId = archetypeId;
    item.count = count;
    item.x = -1;
    item.y = -1;
    item.flags = kPlayerInventoryItemFlagStackable;

    QueuePlayerSidecarInventoryRestore({ item }, clearInventory, "runtime inventory restore fixture");
    ++m_debugInventoryRestoreFixtureCount;
    m_debugInventoryArchetypeId = archetypeId;
    m_debugInventoryCount = -1;
    m_lastDebugInventoryEvent =
        "queued inventory restore fixture arch=" + std::to_string(archetypeId) +
        " count=" + std::to_string(count) +
        " clear=" + std::to_string(clearInventory ? 1 : 0);
    LogCoop(m_lastDebugInventoryEvent);
    return true;
}

void ModMain::QueuePlayerSidecarApply(const char* reason)
{
    if (!m_enablePlayerSidecar)
        return;

    m_pendingPlayerSidecarApply = true;
    m_playerSidecarApplyAccumulator = -kPlayerSidecarInitialApplyDelaySeconds;
    m_lastPlayerSidecarEvent = std::string("queued player sidecar apply") + (reason && reason[0] ? ": " + std::string(reason) : "");
}

void ModMain::QueuePlayerSidecarSave(const char* reason)
{
    if (!m_enablePlayerSidecar)
        return;

    m_pendingPlayerSidecarSave = true;
    m_playerSidecarDeferredSaveAccumulator = 0.0f;
    m_pendingPlayerSidecarSaveReason = reason && reason[0] ? reason : "deferred";
    m_lastPlayerSidecarEvent = "queued player sidecar save: " + m_pendingPlayerSidecarSaveReason;
}

bool ModMain::TryApplyPendingPlayerSidecar(const char* reason)
{
    if (!m_pendingPlayerSidecarApply)
        return false;

    if (!IsGameReady())
    {
        m_lastPlayerSidecarEvent = "waiting to apply player sidecar: game not ready";
        return false;
    }

    PlayerSidecarState state;
    if (!LoadLocalPlayerSidecar(state))
    {
        m_pendingPlayerSidecarApply = false;
        return false;
    }

    if (!ApplyLocalPlayerSidecar(state, reason))
        return false;

    m_pendingPlayerSidecarApply = false;
    return true;
}

void ModMain::TickPlayerSidecar(float frameTime)
{
    if (!m_enablePlayerSidecar)
        return;

    if (m_pendingPlayerSidecarInventoryRestore)
    {
        const bool canRestoreOutsideNativeWindow =
            !m_pendingPlayerSidecarInventoryRestoreNeedsClear ||
            (!m_saveLoadGuardActive && IsGameReady());
        if (!m_saveLoadGuardActive &&
            IsGameReady() &&
            canRestoreOutsideNativeWindow &&
            (m_playerSidecarInventoryNativeRestoreReady || IsGameReady()))
        {
            m_playerSidecarInventoryRestoreAccumulator += std::max(0.0f, frameTime);
            if (m_playerSidecarInventoryRestoreAccumulator >= kPlayerSidecarInventoryRestoreRetrySeconds)
            {
                m_playerSidecarInventoryRestoreAccumulator = 0.0f;
                TryRestorePendingPlayerSidecarInventory("inventory restore retry");
            }
        }
        else
        {
            m_playerSidecarInventoryRestoreAccumulator = 0.0f;
        }
    }

    if (m_pendingPlayerSidecarChipsetRestore &&
        !m_pendingPlayerSidecarInventoryRestore &&
        m_playerSidecarInventoryPending == 0)
    {
        if (!m_saveLoadGuardActive && IsGameReady())
            TryRestorePendingPlayerSidecarChipsets("chipset restore retry");
    }

    if (m_pendingPlayerSidecarSave)
    {
        if (!m_saveLoadGuardActive && IsGameReady())
        {
            m_playerSidecarDeferredSaveAccumulator += std::max(0.0f, frameTime);
            if (m_playerSidecarDeferredSaveAccumulator >= kPlayerSidecarApplyRetrySeconds)
            {
                const std::string reason = m_pendingPlayerSidecarSaveReason.empty() ? std::string("deferred") : m_pendingPlayerSidecarSaveReason;
                m_pendingPlayerSidecarSave = false;
                m_playerSidecarDeferredSaveAccumulator = 0.0f;
                SaveLocalPlayerSidecar(reason.c_str());
            }
        }
        else
        {
            m_playerSidecarDeferredSaveAccumulator = 0.0f;
        }
    }

    if (m_pendingPlayerSidecarApply)
    {
        m_playerSidecarApplyAccumulator += std::max(0.0f, frameTime);
        if (m_playerSidecarApplyAccumulator >= kPlayerSidecarApplyRetrySeconds)
        {
            m_playerSidecarApplyAccumulator = 0.0f;
            TryApplyPendingPlayerSidecar("retry");
        }
    }

    // Local sidecars are written from explicit save/load/transition hooks.
    // Polling every few seconds was an early recovery hack and touches inventory
    // state far more often than the multiplayer save model needs.
    m_playerSidecarSaveAccumulator = 0.0f;
}
