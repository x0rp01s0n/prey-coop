#pragma once

#include "CoopWeaponState.h"
#include <Prey/GameDll/ark/weapons/arkweapon.h>
#include <Prey/Ark/ArkWeaponModifier.h>

namespace CoopWeaponState
{
// Register before Chairloader relocates the SDK function table at mod startup.
inline auto getNativeModifiers = PreyFunction<const ArkWeaponModifiers*()>(0x1668190);

inline bool ApplyNative(CArkWeapon& weapon, const State& state, bool restoreLootState = true)
{
    // The native library accessor used by CArkWeapon::InstallMod.
    const auto* library = getNativeModifiers();
    return library && ApplyNative(weapon, state, *library, restoreLootState);
}
}
