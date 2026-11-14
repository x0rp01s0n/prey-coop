#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace CoopSharedDropDelivery
{
constexpr size_t kMaxEntries = 4096;
constexpr std::string_view kSessionPrefix = "sharedDropHostSessionId=";
constexpr std::string_view kCountPrefix = "sharedDropDeliveryCount=";
constexpr std::string_view kEntryPrefix = "sharedDropDelivery=";

struct Entry
{
    uint64_t hostSessionId = 0;
    uint64_t stableSpawnId = 0;
    uint64_t archetypeId = 0;
    uint32_t count = 0;
    uint32_t deliveredCount = 0;
    uint32_t transactionSequence = 0;
    uint32_t objectVersion = 0;
};

inline bool IsValid(const Entry& row)
{
    return row.hostSessionId != 0 && row.stableSpawnId != 0 && row.archetypeId != 0 &&
        row.count != 0 && row.count <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
        row.deliveredCount <= row.count && row.transactionSequence != 0 && row.objectVersion != 0;
}

inline bool SameLifetime(const Entry& a, const Entry& b)
{
    return a.hostSessionId == b.hostSessionId && a.stableSpawnId == b.stableSpawnId;
}

inline bool SameCommit(const Entry& a, const Entry& b)
{
    return SameLifetime(a, b) && a.archetypeId == b.archetypeId && a.count == b.count &&
        a.transactionSequence == b.transactionSequence && a.objectVersion == b.objectVersion;
}

inline bool ReadUnsigned(std::string_view& text, uint64_t& value)
{
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos)
        return false;
    text.remove_prefix(start);
    const size_t end = text.find_first_of(" \t");
    const std::string_view token = text.substr(0, end);
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size())
        return false;
    text.remove_prefix(token.size());
    return true;
}

inline bool ParseEntry(std::string_view text, Entry& row)
{
    uint64_t values[7] = {};
    for (uint64_t& value : values)
    {
        if (!ReadUnsigned(text, value))
            return false;
    }
    if (text.find_first_not_of(" \t") != std::string_view::npos ||
        values[3] > UINT32_MAX || values[4] > UINT32_MAX ||
        values[5] > UINT32_MAX || values[6] > UINT32_MAX)
    {
        return false;
    }
    row = {values[0], values[1], values[2], static_cast<uint32_t>(values[3]),
        static_cast<uint32_t>(values[4]), static_cast<uint32_t>(values[5]),
        static_cast<uint32_t>(values[6])};
    return IsValid(row);
}

inline bool ParseSection(
    std::string_view payload,
    std::vector<Entry>& rows,
    bool& present,
    uint64_t* hostSessionId = nullptr)
{
    rows.clear();
    present = false;
    uint64_t expectedCount = 0;
    uint64_t sessionId = 0;
    bool hasSession = false;
    while (!payload.empty())
    {
        const size_t end = payload.find('\n');
        std::string_view line = payload.substr(0, end);
        payload.remove_prefix(end == std::string_view::npos ? payload.size() : end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.substr(0, kSessionPrefix.size()) == kSessionPrefix)
        {
            if (hasSession)
                return false;
            hasSession = true;
            std::string_view text = line.substr(kSessionPrefix.size());
            if (!ReadUnsigned(text, sessionId) || sessionId == 0 ||
                text.find_first_not_of(" \t") != std::string_view::npos)
            {
                return false;
            }
        }
        else if (line.substr(0, kCountPrefix.size()) == kCountPrefix)
        {
            if (present)
                return false;
            present = true;
            std::string_view text = line.substr(kCountPrefix.size());
            if (!ReadUnsigned(text, expectedCount) || expectedCount > kMaxEntries ||
                text.find_first_not_of(" \t") != std::string_view::npos)
            {
                return false;
            }
        }
        else if (line.substr(0, kEntryPrefix.size()) == kEntryPrefix)
        {
            Entry row;
            if (rows.size() == kMaxEntries || !ParseEntry(line.substr(kEntryPrefix.size()), row) ||
                std::any_of(rows.begin(), rows.end(), [&row](const Entry& existing)
                    { return SameLifetime(row, existing); }))
            {
                return false;
            }
            rows.push_back(row);
        }
    }
    if (hostSessionId)
        *hostSessionId = sessionId;
    return present ? hasSession && rows.size() == expectedCount : !hasSession && rows.empty();
}

inline void WriteSection(std::ostream& output, const std::vector<Entry>& rows, uint64_t hostSessionId)
{
    output << kSessionPrefix << hostSessionId << '\n';
    output << kCountPrefix << rows.size() << '\n';
    for (const Entry& row : rows)
    {
        output << kEntryPrefix << row.hostSessionId << ' ' << row.stableSpawnId << ' '
            << row.archetypeId << ' ' << row.count << ' ' << row.deliveredCount << ' '
            << row.transactionSequence << ' ' << row.objectVersion << '\n';
    }
}

inline std::string WithoutSection(std::string_view payload)
{
    std::string result;
    while (!payload.empty())
    {
        const size_t end = payload.find('\n');
        const std::string_view line = payload.substr(0, end);
        if (line.substr(0, kSessionPrefix.size()) != kSessionPrefix &&
            line.substr(0, kCountPrefix.size()) != kCountPrefix &&
            line.substr(0, kEntryPrefix.size()) != kEntryPrefix)
        {
            result.append(line.data(), line.size());
            result.push_back('\n');
        }
        payload.remove_prefix(end == std::string_view::npos ? payload.size() : end + 1);
    }
    return result;
}

inline bool MergeForUpload(
    std::vector<Entry>& incoming,
    const std::vector<Entry>& stored,
    uint64_t currentHostSessionId)
{
    for (const Entry& row : incoming)
    {
        const auto existing = std::find_if(stored.begin(), stored.end(), [&row](const Entry& candidate)
            { return SameLifetime(row, candidate); });
        if (existing == stored.end())
        {
            if (row.hostSessionId == currentHostSessionId)
                return false;
            continue;
        }
        if (!SameCommit(row, *existing) || row.deliveredCount < existing->deliveredCount)
            return false;
    }
    for (const Entry& row : stored)
    {
        const auto uploaded = std::find_if(incoming.begin(), incoming.end(), [&row](const Entry& candidate)
            { return SameLifetime(row, candidate); });
        if (uploaded != incoming.end())
            continue;
        if ((row.hostSessionId == currentHostSessionId && row.deliveredCount != 0) ||
            incoming.size() == kMaxEntries)
        {
            return false;
        }
        incoming.push_back(row);
    }
    return true;
}
}
