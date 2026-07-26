#include "CoopAreaObjectJournal.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <ostream>
#include <string_view>
#include <utility>

namespace
{
std::string EscapeJson(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value)
    {
        if (ch == '"' || ch == '\\')
            escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

bool ReadUint64(std::string_view line, std::string_view key, uint64_t& value)
{
    const size_t pos = line.find(key);
    if (pos == std::string_view::npos)
        return false;
    const std::string token(line.substr(pos + key.size(), 32));
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(token.c_str(), &end, 10);
    if (errno != 0 || !end || end == token.c_str())
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool ReadInt32(std::string_view line, std::string_view key, int32_t& value)
{
    const size_t pos = line.find(key);
    if (pos == std::string_view::npos)
        return false;
    const std::string token(line.substr(pos + key.size(), 32));
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(token.c_str(), &end, 10);
    if (errno != 0 || !end || end == token.c_str() ||
        parsed < static_cast<long>(std::numeric_limits<int32_t>::min()) ||
        parsed > static_cast<long>(std::numeric_limits<int32_t>::max()))
    {
        return false;
    }
    value = static_cast<int32_t>(parsed);
    return true;
}

bool ReadString(std::string_view line, std::string_view key, std::string& value)
{
    const size_t pos = line.find(key);
    if (pos == std::string_view::npos)
        return false;
    size_t cursor = pos + key.size();
    if (cursor >= line.size() || line[cursor] != '"')
        return false;
    ++cursor;
    std::string result;
    bool escaped = false;
    for (; cursor < line.size(); ++cursor)
    {
        const char ch = line[cursor];
        if (escaped)
        {
            result.push_back(ch);
            escaped = false;
        }
        else if (ch == '\\')
        {
            escaped = true;
        }
        else if (ch == '"')
        {
            value = std::move(result);
            return true;
        }
        else
        {
            result.push_back(ch);
        }
    }
    return false;
}
} // namespace

uint64_t CoopAreaObjectJournal::BuildKey(
    uint64_t areaId,
    uint64_t targetGuid,
    uint16_t eventKind,
    int32_t count)
{
    uint64_t hash = 14695981039346656037ull;
    const bool buttonScoped =
        eventKind == CoopProtocol::kAreaObjectEventKioskButtonState ||
        eventKind == CoopProtocol::kAreaObjectEventKioskButtonHeader ||
        eventKind == CoopProtocol::kAreaObjectEventKioskButtonBody ||
        eventKind == CoopProtocol::kAreaObjectEventKioskButtonVisible;
    const uint64_t values[] = {
        areaId,
        targetGuid,
        static_cast<uint64_t>(eventKind),
        buttonScoped ? static_cast<uint64_t>(static_cast<uint32_t>(count)) : 0ull,
    };
    for (const uint64_t value : values)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash == 0 ? 1 : hash;
}

void CoopAreaObjectJournal::Reset()
{
    m_entries.clear();
}

bool CoopAreaObjectJournal::Record(const std::string& levelName, const CoopProtocol::AreaObjectEventPacket& packet)
{
    // Button presses are commands, not durable object state. Replaying one on
    // area restore could start the same authored FlowGraph sequence twice.
    if (CoopProtocol::IsTransientAreaObjectEvent(packet.eventKind))
        return true;

    if (levelName.empty() || levelName == "unknown" || packet.levelId == 0 ||
        packet.targetGuid == 0 || packet.eventKind == 0 || packet.eventId == 0)
    {
        return false;
    }

    const uint64_t key = BuildKey(
        packet.levelId,
        packet.targetGuid,
        packet.eventKind,
        packet.count);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.packet.postVersion > packet.postVersion)
        return true;

    m_entries[key] = Entry{ levelName, packet };
    return true;
}

bool CoopAreaObjectJournal::ExportLevelJsonl(const std::string& levelName, std::ostream& output) const
{
    for (const auto& pair : m_entries)
    {
        const Entry& entry = pair.second;
        if (entry.levelName != levelName)
            continue;
        const CoopProtocol::AreaObjectEventPacket& packet = entry.packet;
        output
            << "{\"type\":\"area_object_event\""
            << ",\"level\":\"" << EscapeJson(entry.levelName) << "\""
            << ",\"eventId\":" << packet.eventId
            << ",\"eventKind\":" << packet.eventKind
            << ",\"targetGuid\":" << packet.targetGuid
            << ",\"targetClassHash\":" << packet.targetClassHash
            << ",\"value\":" << packet.value
            << ",\"count\":" << packet.count
            << ",\"textValue\":\"" << EscapeJson(packet.textValue) << "\""
            << ",\"flags\":" << packet.flags
            << ",\"worldEpoch\":" << packet.worldEpoch
            << ",\"levelId\":" << packet.levelId
            << ",\"hostSaveKeyHash\":" << packet.hostSaveKeyHash
            << ",\"sourcePeerHash\":" << packet.sourcePeerHash
            << ",\"preVersion\":" << packet.preVersion
            << ",\"postVersion\":" << packet.postVersion
            << ",\"areaRevision\":" << packet.areaRevision
            << "}\n";
    }
    return static_cast<bool>(output);
}

bool CoopAreaObjectJournal::ReplayLevelJsonl(
    const std::filesystem::path& path,
    const std::string& levelName,
    const ApplyCallback& apply,
    ReplayStats& stats)
{
    stats = {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        stats.last = "open_failed";
        return false;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (line.find("\"type\":\"area_object_event\"") == std::string::npos)
            continue;
        ++stats.rows;

        std::string rowLevel;
        CoopProtocol::AreaObjectEventPacket packet = {};
        uint64_t eventKind = 0;
        uint64_t value = 0;
        uint64_t flags = 0;
        uint64_t worldEpoch = 0;
        uint64_t preVersion = 0;
        uint64_t postVersion = 0;
        uint64_t areaRevision = 0;
        int32_t count = 0;
        std::string textValue;
        if (!ReadString(line, "\"level\":", rowLevel) || rowLevel != levelName ||
            !ReadUint64(line, "\"eventId\":", packet.eventId) ||
            !ReadUint64(line, "\"eventKind\":", eventKind) ||
            !ReadUint64(line, "\"targetGuid\":", packet.targetGuid) ||
            !ReadUint64(line, "\"targetClassHash\":", packet.targetClassHash) ||
            !ReadUint64(line, "\"value\":", value) ||
            !ReadUint64(line, "\"flags\":", flags) ||
            !ReadUint64(line, "\"worldEpoch\":", worldEpoch) ||
            !ReadUint64(line, "\"levelId\":", packet.levelId) ||
            !ReadUint64(line, "\"hostSaveKeyHash\":", packet.hostSaveKeyHash) ||
            !ReadUint64(line, "\"sourcePeerHash\":", packet.sourcePeerHash) ||
            !ReadUint64(line, "\"preVersion\":", preVersion) ||
            !ReadUint64(line, "\"postVersion\":", postVersion) ||
            !ReadUint64(line, "\"areaRevision\":", areaRevision))
        {
            ++stats.rejected;
            stats.last = "parse_rejected";
            continue;
        }

        packet.eventKind = static_cast<uint16_t>(eventKind);
        packet.magic = CoopProtocol::kPacketMagic;
        packet.version = CoopProtocol::kProtocolVersion;
        packet.type = static_cast<uint16_t>(CoopProtocol::PacketType::AreaObjectEvent);
        packet.value = static_cast<uint16_t>(value);
        // Protocol 85 journals did not carry these fields. Keep their replay
        // valid while Protocol 86 writes the typed AreaFact payload.
        ReadInt32(line, "\"count\":", count);
        ReadString(line, "\"textValue\":", textValue);
        packet.count = count;
        if (textValue.size() >= sizeof(packet.textValue))
        {
            ++stats.rejected;
            stats.last = "text_value_too_long";
            continue;
        }
        std::copy(textValue.begin(), textValue.end(), packet.textValue);
        packet.textValue[textValue.size()] = '\0';
        packet.flags = static_cast<uint32_t>(flags);
        packet.worldEpoch = static_cast<uint32_t>(worldEpoch);
        packet.preVersion = static_cast<uint32_t>(preVersion);
        packet.postVersion = static_cast<uint32_t>(postVersion);
        packet.areaRevision = static_cast<uint32_t>(areaRevision);

        std::string detail;
        if (!apply || !apply(packet, detail))
        {
            ++stats.rejected;
            stats.last = detail.empty() ? "apply_rejected" : detail;
            continue;
        }
        ++stats.applied;
        stats.last = detail.empty() ? "applied" : detail;
        Record(rowLevel, packet);
    }
    return input.eof() && stats.rejected == 0;
}
