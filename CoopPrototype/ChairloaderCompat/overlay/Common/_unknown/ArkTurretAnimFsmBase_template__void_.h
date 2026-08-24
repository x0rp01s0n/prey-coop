// Opaque ABI placeholder missing from upstream Chairloader at the pinned revision.
#pragma once

#include <cstddef>

template <typename T>
class alignas(8) ArkTurretAnimFsmBase_template_
{
private:
	std::byte m_opaque[64];
};

static_assert(sizeof(ArkTurretAnimFsmBase_template_<void>) == 64);
static_assert(alignof(ArkTurretAnimFsmBase_template_<void>) == 8);
