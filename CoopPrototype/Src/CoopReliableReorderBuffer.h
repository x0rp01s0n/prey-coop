#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "CoopSerialSequence.h"

namespace CoopReliableReorder
{
constexpr std::size_t kWindowCapacity = 32;

constexpr bool IsWithinForwardWindow(uint32_t frontier, uint32_t sequence)
{
    if (!CoopSerialSequence::IsAfter(sequence, frontier))
        return false;

    const uint32_t distance = frontier == 0
        ? sequence
        : CoopSerialSequence::ForwardDistance(frontier, sequence);
    return distance <= kWindowCapacity;
}

template<typename T>
class Buffer
{
public:
    bool Store(uint32_t frontier, uint32_t sequence, const T& value)
    {
        const uint32_t distance = frontier == 0
            ? sequence
            : CoopSerialSequence::ForwardDistance(frontier, sequence);
        if (distance == 0 || distance > kWindowCapacity ||
            !CoopSerialSequence::IsAfter(sequence, frontier) ||
            m_entries.size() >= kWindowCapacity)
            return false;

        const auto existing = std::find_if(
            m_entries.begin(),
            m_entries.end(),
            [sequence](const Entry& entry) { return entry.sequence == sequence; });
        if (existing != m_entries.end())
            return false;

        m_entries.push_back({sequence, value});
        return true;
    }

    bool TakeNext(uint32_t frontier, T& value)
    {
        const uint32_t sequence = CoopSerialSequence::Next(frontier);
        const auto next = std::find_if(
            m_entries.begin(),
            m_entries.end(),
            [sequence](const Entry& entry) { return entry.sequence == sequence; });
        if (next == m_entries.end())
            return false;

        value = std::move(next->value);
        m_entries.erase(next);
        return true;
    }

    void DiscardAtOrBefore(uint32_t frontier)
    {
        m_entries.erase(
            std::remove_if(
                m_entries.begin(),
                m_entries.end(),
                [frontier](const Entry& entry)
                {
                    return CoopSerialSequence::IsAtOrBefore(entry.sequence, frontier);
                }),
            m_entries.end());
    }

    std::size_t Size() const { return m_entries.size(); }

private:
    struct Entry
    {
        uint32_t sequence = 0;
        T value = {};
    };

    std::vector<Entry> m_entries;
};
}
