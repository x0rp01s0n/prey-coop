#pragma once

#include "CoopRuntimeGuards.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/GameDll/arkitem.h>

namespace CoopItemClassification
{
inline bool IsKnownAmmoPickupArchetype(uint64_t archetypeId)
{
    switch (archetypeId)
    {
    case 10739735956144611816ULL: // GooGun ammo
    case 10739735956144611817ULL: // Q-Beam ammo
    case 10739735956144611818ULL: // Shotgun shells
    case 10739735956144611828ULL: // Nullwave transmitter pickup
    case 10739735956144611862ULL: // Recycler charge pickup
    case 10739735956144611892ULL: // Pistol bullets
    case 10739735956144611952ULL: // Toy gun darts
    case 10739735956144611969ULL: // Typhon lure pickup
    case 10739735956144611970ULL: // EMP charge pickup
    case 10739735956144611980ULL: // Operator lure pickup
    case 10739735956144611996ULL: // Stun gun ammo
        return true;
    default:
        return false;
    }
}

inline bool IsKnownWeaponPickupArchetype(uint64_t archetypeId)
{
    switch (archetypeId)
    {
    case 10739735956144611833ULL: // Q-Beam
    case 10739735956144611834ULL: // Shotgun
    case 10739735956144611849ULL: // Wrench
    case 10739735956144611850ULL: // GLOO gun
    case 10739735956144611893ULL: // Pistol
    case 10739735956144611951ULL: // Stun gun
    case 10739735956144611953ULL: // Toy gun
    case 10739735956144611984ULL: // Golden pistol
    case 3149325216979386322ULL:  // Golden shotgun
        return true;
    default:
        return false;
    }
}

inline std::string LowerAscii(std::string_view value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return lowered;
}

inline bool ContainsToken(std::string_view value, std::string_view token)
{
    return LowerAscii(value).find(LowerAscii(token)) != std::string::npos;
}

inline uint64_t ReadArchetype(CArkItem* item, std::string& reason)
{
    if (!item || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return 0;

    uint64_t archetypeId = 0;
    CoopRuntimeGuards::TryGuardedCall(
        "coop item classify archetype",
        [item]() -> uint64_t
        {
            return item->m_selectedArchetype;
        },
        archetypeId,
        &reason);
    return archetypeId;
}

inline bool IsAmmoLikeItem(CArkItem* item, std::string& reason)
{
    if (!item || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return false;

    uint64_t archetypeId = 0;
    std::string name;
    std::string type;
    bool readFields = false;
    CoopRuntimeGuards::TryGuardedCall(
        "coop item classify ammo fields",
        [&]() -> bool
        {
            archetypeId = item->m_selectedArchetype;
            name = item->m_inventoryName.c_str() ? std::string(item->m_inventoryName.c_str()) : std::string();
            type = item->m_type.c_str() ? std::string(item->m_type.c_str()) : std::string();
            return true;
        },
        readFields,
        &reason);

    if (!readFields)
        return false;

    if (IsKnownAmmoPickupArchetype(archetypeId))
        return true;

    const std::string lowerName = LowerAscii(name);
    const std::string lowerType = LowerAscii(type);
    if (lowerName.rfind("@i_ammo", 0) == 0)
        return true;

    return lowerType.find("ammo") != std::string::npos ||
        lowerType.find("shell") != std::string::npos ||
        lowerType.find("bullet") != std::string::npos ||
        lowerType.find("dart") != std::string::npos;
}

inline bool IsConcreteArkWeapon(CArkItem* item, std::string& reason)
{
    if (!item || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return false;

    CArkWeapon* weapon = static_cast<CArkWeapon*>(item);
    if (!weapon || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(weapon, sizeof(CArkWeapon)))
        return false;

    bool concreteWeapon = false;
    CoopRuntimeGuards::TryGuardedCall(
        "coop item classify CArkWeapon fields",
        [weapon]() -> bool
        {
            const char* className = weapon->m_pWeaponClassName.c_str();
            return className && className[0] && std::string_view(className).find("ArkWeapon") == 0;
        },
        concreteWeapon,
        &reason);
    return concreteWeapon;
}

inline bool IsRealWeaponInventoryItem(CArkItem* item, std::string& reason)
{
    if (!item || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(item, sizeof(CArkItem)))
        return false;

    const uint64_t archetypeId = ReadArchetype(item, reason);
    if (IsKnownAmmoPickupArchetype(archetypeId) || IsAmmoLikeItem(item, reason))
        return false;

    if (IsConcreteArkWeapon(item, reason))
        return true;

    return IsKnownWeaponPickupArchetype(archetypeId);
}
} // namespace CoopItemClassification
