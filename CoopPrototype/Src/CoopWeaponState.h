#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopWeaponState
{
constexpr size_t kMaxMods = 16;

struct State
{
    bool present = false;
    float condition = 0.0f;
    int32_t ammoLoaded = 0;
    std::vector<std::pair<uint64_t, int>> mods;
    bool ammoLooted = false; // Vanilla's one-time ammo-clip grant must not restart on a replica.
};

struct Wire
{
    uint32_t present = 0;
    float condition = 0.0f;
    int32_t ammoLoaded = 0;
    uint32_t modCount = 0;
    uint32_t ammoLooted = 0;
    uint64_t modIds[kMaxMods] = {};
    uint32_t modLevels[kMaxMods] = {};
};

inline bool IsValid(const State& state)
{
    if (!state.present)
        return state.condition == 0.0f && state.ammoLoaded == 0 && state.mods.empty() && !state.ammoLooted;
    if (!std::isfinite(state.condition) || state.condition < 0.0f || state.condition > 100.0f ||
        state.ammoLoaded < 0 || state.ammoLoaded > 1000000 || state.mods.size() > kMaxMods)
        return false;
    for (size_t i = 0; i < state.mods.size(); ++i)
    {
        const auto& mod = state.mods[i];
        if (!mod.first || mod.second <= 0 || mod.second > 16)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (state.mods[j].first == mod.first)
                return false;
    }
    return true;
}

inline bool Same(const State& a, const State& b)
{
    if (a.present != b.present || a.ammoLooted != b.ammoLooted || a.condition != b.condition || a.ammoLoaded != b.ammoLoaded ||
        a.mods.size() != b.mods.size())
        return false;
    return std::all_of(a.mods.begin(), a.mods.end(), [&b](const auto& mod)
        { return std::find(b.mods.begin(), b.mods.end(), mod) != b.mods.end(); });
}

inline bool Encode(const State& state, Wire& wire)
{
    wire = {};
    if (!IsValid(state))
        return false;
    wire.present = state.present ? 1u : 0u;
    wire.condition = state.condition;
    wire.ammoLoaded = state.ammoLoaded;
    wire.modCount = static_cast<uint32_t>(state.mods.size());
    wire.ammoLooted = state.ammoLooted ? 1u : 0u;
    for (size_t i = 0; i < state.mods.size(); ++i)
    {
        wire.modIds[i] = state.mods[i].first;
        wire.modLevels[i] = static_cast<uint32_t>(state.mods[i].second);
    }
    return true;
}

inline bool Decode(const Wire& wire, State& state)
{
    state = {};
    if (wire.present > 1 || wire.ammoLooted > 1 || wire.modCount > kMaxMods)
        return false;
    state.present = wire.present != 0;
    state.condition = wire.condition;
    state.ammoLoaded = wire.ammoLoaded;
    state.ammoLooted = wire.ammoLooted != 0;
    for (size_t i = 0; i < kMaxMods; ++i)
    {
        if (i < wire.modCount)
        {
            if (wire.modLevels[i] > 16)
                return false;
            state.mods.emplace_back(wire.modIds[i], static_cast<int>(wire.modLevels[i]));
        }
        else if (wire.modIds[i] || wire.modLevels[i])
            return false;
    }
    return IsValid(state);
}

template<class Weapon>
State CaptureNative(const Weapon& weapon)
{
    State state;
    state.present = true;
    state.condition = weapon.m_condition;
    state.ammoLoaded = weapon.m_numAmmoLoaded;
    state.ammoLooted = weapon.m_bHasAmmoBeenLooted;
    for (const auto& mod : weapon.m_weaponMods.m_weaponModIds)
        state.mods.emplace_back(mod.first, mod.second);
    std::sort(state.mods.begin(), state.mods.end());
    return state;
}

template<class Weapon, class ModLibrary>
bool ApplyNative(Weapon& weapon, const State& state, const ModLibrary& library, bool restoreLootState = true)
{
    if (!IsValid(state) || !state.present)
        return false;
    // Resolve every level before changing the instance.
    for (const auto& mod : state.mods)
    {
        const auto* definition = library.GetWeaponModByID(mod.first);
        if (!definition)
            return false;
        for (int level = 1; level <= mod.second; ++level)
            if (!definition->GetLevelWeaponMod(level))
                return false;
    }
    const auto current = CaptureNative(weapon);
    if (current.mods.size() != state.mods.size() ||
        !std::is_permutation(current.mods.begin(), current.mods.end(), state.mods.begin(), state.mods.end()))
    {
        // AddMod only records levels. Rebuild the native stats too, without
        // InstallMod's upgrade-kit consumption and UI side effects.
        weapon.InitializeStats();
        weapon.m_weaponMods.Clear();
        for (const auto& mod : state.mods)
        {
            const auto* definition = library.GetWeaponModByID(mod.first);
            for (int level = 1; level <= mod.second; ++level)
            {
                for (const auto& stat : definition->GetLevelWeaponMod(level)->m_StatModifiers)
                    weapon.m_statsComponent.AddModifier(stat.m_StatName, stat.m_Modifier, definition->m_Name);
                weapon.m_weaponMods.AddMod(mod.first);
            }
        }
    }
    weapon.m_condition = state.condition;
    weapon.m_numAmmoLoaded = state.ammoLoaded;
    if (restoreLootState)
        weapon.m_bHasAmmoBeenLooted = state.ammoLooted;
    return true;
}

// Exact float bits avoid locale and decimal-rounding changes in receipt identity.
inline void WriteSuffix(std::ostream& out, const State& state)
{
    if (!state.present)
        return;
    uint32_t condition = 0;
    static_assert(sizeof(condition) == sizeof(state.condition));
    std::memcpy(&condition, &state.condition, sizeof(condition));
    out << " w1 " << condition << ' ' << state.ammoLoaded << ' ' << state.mods.size() << ' ' << state.ammoLooted;
    for (const auto& mod : state.mods)
        out << ' ' << mod.first << ' ' << mod.second;
}

inline bool ReadNumber(std::string_view& text, uint64_t& value)
{
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return false;
    text.remove_prefix(start);
    const auto token = text.substr(0, text.find_first_of(" \t"));
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size())
        return false;
    text.remove_prefix(token.size());
    return true;
}

inline bool ReadSuffix(std::string_view text, State& state)
{
    state = {};
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return true; // Older receipts have no weapon state.
    text.remove_prefix(start);
    if (text.substr(0, 3) != "w1 ")
        return false;
    text.remove_prefix(3);
    uint64_t bits = 0, ammo = 0, count = 0, looted = 0;
    if (!ReadNumber(text, bits) || bits > UINT32_MAX || !ReadNumber(text, ammo) ||
        ammo > 1000000 || !ReadNumber(text, count) || count > kMaxMods ||
        !ReadNumber(text, looted) || looted > 1)
        return false;
    state.present = true;
    const uint32_t raw = static_cast<uint32_t>(bits);
    std::memcpy(&state.condition, &raw, sizeof(raw));
    state.ammoLoaded = static_cast<int32_t>(ammo);
    state.ammoLooted = looted != 0;
    for (uint64_t i = 0; i < count; ++i)
    {
        uint64_t id = 0, level = 0;
        if (!ReadNumber(text, id) || !ReadNumber(text, level) || level > 16)
            return false;
        state.mods.emplace_back(id, static_cast<int>(level));
    }
    return text.find_first_not_of(" \t") == std::string_view::npos && IsValid(state);
}
}
