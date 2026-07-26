#pragma once

#include <cstdint>
#include <limits>

namespace CoopSerialSequence
{
constexpr uint32_t kFirst = 1u;
constexpr uint32_t kLast = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kHalfRange = kLast / 2u;

constexpr uint32_t Next(uint32_t value)
{
    return value == 0u || value == kLast ? kFirst : value + 1u;
}

constexpr uint32_t Previous(uint32_t value)
{
    return value <= kFirst ? kLast : value - 1u;
}

constexpr uint32_t ForwardDistance(uint32_t from, uint32_t to)
{
    if (from == 0u || to == 0u)
        return 0u;
    return to >= from ? to - from : (kLast - from) + to;
}

constexpr bool IsAfter(uint32_t candidate, uint32_t reference)
{
    if (candidate == 0u)
        return false;
    if (reference == 0u)
        return true;
    if (candidate == reference)
        return false;
    const uint32_t distance = ForwardDistance(reference, candidate);
    return distance != 0u && distance <= kHalfRange;
}

constexpr bool IsAtOrBefore(uint32_t value, uint32_t frontier)
{
    if (value == 0u || frontier == 0u)
        return false;
    return value == frontier || IsAfter(frontier, value);
}

constexpr bool IsStaleOrDuplicate(uint32_t candidate, uint32_t lastAccepted)
{
    return candidate != 0u && lastAccepted != 0u &&
        !IsAfter(candidate, lastAccepted);
}

constexpr uint32_t Newer(uint32_t lhs, uint32_t rhs)
{
    if (lhs == 0u)
        return rhs;
    if (rhs == 0u)
        return lhs;
    return IsAfter(lhs, rhs) ? lhs : rhs;
}

inline void Observe(uint32_t candidate, uint32_t& lastAccepted)
{
    if (candidate != 0u && (lastAccepted == 0u || IsAfter(candidate, lastAccepted)))
        lastAccepted = candidate;
}

inline uint32_t Advance(uint32_t& value)
{
    value = Next(value);
    return value;
}
}

static_assert(CoopSerialSequence::Next(0u) == 1u);
static_assert(CoopSerialSequence::Next(0xffffffffu) == 1u);
static_assert(CoopSerialSequence::Previous(1u) == 0xffffffffu);
static_assert(CoopSerialSequence::IsAfter(1u, 0xffffffffu));
static_assert(!CoopSerialSequence::IsAfter(0xffffffffu, 1u));
static_assert(CoopSerialSequence::IsAtOrBefore(0xffffffffu, 1u));
static_assert(CoopSerialSequence::IsAtOrBefore(1u, 1u));
static_assert(!CoopSerialSequence::IsAtOrBefore(2u, 1u));
static_assert(!CoopSerialSequence::IsStaleOrDuplicate(1u, 0xffffffffu));
static_assert(CoopSerialSequence::IsStaleOrDuplicate(0xffffffffu, 1u));
static_assert(CoopSerialSequence::Newer(1u, 0xffffffffu) == 1u);
static_assert(CoopSerialSequence::Newer(0xffffffffu, 1u) == 1u);
