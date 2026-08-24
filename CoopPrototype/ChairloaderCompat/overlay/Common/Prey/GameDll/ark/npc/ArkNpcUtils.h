// Minimal ABI definition missing from upstream Chairloader at the pinned revision.
#pragma once

namespace ArkNpcUtils
{
struct Range
{
	float m_min;
	float m_max;
};
} // namespace ArkNpcUtils

static_assert(sizeof(ArkNpcUtils::Range) == 8);
