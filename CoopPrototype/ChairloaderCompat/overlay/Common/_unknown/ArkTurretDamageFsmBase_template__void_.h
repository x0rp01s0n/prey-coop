// Opaque ABI placeholder missing from upstream Chairloader at the pinned revision.
#pragma once

#include <cstddef>

template <typename T>
class alignas(8) ArkTurretDamageFsmBase_template_
{
private:
	std::byte m_opaque[24];
};

static_assert(sizeof(ArkTurretDamageFsmBase_template_<void>) == 24);
static_assert(alignof(ArkTurretDamageFsmBase_template_<void>) == 8);
