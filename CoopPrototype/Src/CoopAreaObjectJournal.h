#pragma once

#include "CoopProtocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>

class CoopAreaObjectJournal
{
public:
    using ApplyCallback = std::function<bool(const CoopProtocol::AreaObjectEventPacket&, std::string&)>;

    struct ReplayStats
    {
        uint32_t rows = 0;
        uint32_t applied = 0;
        uint32_t duplicates = 0;
        uint32_t rejected = 0;
        std::string last = "-";
    };

    void Reset();
    bool Record(const std::string& levelName, const CoopProtocol::AreaObjectEventPacket& packet);
    bool ExportLevelJsonl(const std::string& levelName, std::ostream& output) const;
    bool ReplayLevelJsonl(
        const std::filesystem::path& path,
        const std::string& levelName,
        const ApplyCallback& apply,
        ReplayStats& stats);
    uint32_t GetEntryCount() const { return static_cast<uint32_t>(m_entries.size()); }

private:
    struct Entry
    {
        std::string levelName;
        CoopProtocol::AreaObjectEventPacket packet = {};
    };

    static uint64_t BuildKey(
        uint64_t areaId,
        uint64_t targetGuid,
        uint16_t eventKind,
        int32_t count);
    std::unordered_map<uint64_t, Entry> m_entries;
};
