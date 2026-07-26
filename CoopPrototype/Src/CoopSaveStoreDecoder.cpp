#include "CoopSaveStoreDecoder.h"

#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using CoopRuntimeGuards::IsRuntimePointerMappedInCurrentProcess;
using CoopRuntimeGuards::PreflightRuntimePointer;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::RuntimeAccess;
using CoopRuntimeGuards::TryReadRuntimeValue;

constexpr size_t kNodeStride = 0x50;
constexpr size_t kHelperStackEntryStride = 0x18;

enum class SerializerStackLayout
{
    ReadStore,
    WriteStore,
};

struct RuntimeVectorTriple
{
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
};

struct NodeFields
{
    bool readable = false;
    uint32_t nodeId = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    uint32_t attrCount = 0;
    uint32_t attrCursor = 0;
    std::uintptr_t childIndexBlockBegin = 0;
    std::uintptr_t childIndexBlockEnd = 0;
    std::uintptr_t attrVectorBegin = 0;
    std::uintptr_t attrVectorEnd = 0;
    std::uintptr_t childVectorBegin = 0;
    std::uintptr_t childVectorEnd = 0;
    uint8_t valid = 0;
};

struct StackEntry
{
    std::uintptr_t address = 0;
    const void* store = nullptr;
    int32_t nodeIndex = -1;
    int32_t generation = 0;
    const void* node = nullptr;
    NodeFields fields;
};

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

uint32_t EnvUIntClamped(
    const char* primary,
    const char* fallback,
    uint32_t defaultValue,
    uint32_t minValue,
    uint32_t maxValue)
{
    const char* value = std::getenv(primary);
    if ((!value || !value[0]) && fallback && fallback[0])
        value = std::getenv(fallback);
    if (!value || !value[0])
        return std::clamp(defaultValue, minValue, maxValue);

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value)
        return std::clamp(defaultValue, minValue, maxValue);

    return std::clamp(static_cast<uint32_t>(parsed), minValue, maxValue);
}

std::string HexU64(uint64_t value)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string PointerHex(const void* ptr)
{
    return HexU64(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ptr)));
}

std::string PointerHex(std::uintptr_t ptr)
{
    return HexU64(static_cast<uint64_t>(ptr));
}

std::string HexOffset(size_t offset)
{
    std::ostringstream out;
    out << "+0x" << std::hex << std::uppercase << offset;
    return out.str();
}

std::string HexByte(uint8_t value)
{
    char buffer[4] = {};
    std::snprintf(buffer, sizeof(buffer), "%02X", value);
    return buffer;
}

std::string ReadRuntimeBytesHex(const void* ptr, size_t bytes)
{
    if (!ptr || bytes == 0)
        return "-";

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder raw bytes", ptr, bytes, RuntimeAccess::Read, &reason))
        return "unreadable";

    std::string out;
    out.reserve(bytes * 2);
    const auto* raw = reinterpret_cast<const uint8_t*>(ptr);
    for (size_t i = 0; i < bytes; ++i)
    {
        uint8_t value = 0;
        if (!TryReadRuntimeValue(raw + i, value))
            return out.empty() ? std::string("unreadable") : out;
        out += HexByte(value);
    }
    return out;
}

std::string TextProbe(const void* ptr)
{
    const std::string text = ReadRuntimeCString(reinterpret_cast<const char*>(ptr), 96);
    if (text.size() < 2 || text.size() > 80)
        return {};

    size_t printable = 0;
    for (unsigned char ch : text)
    {
        if (std::isprint(ch) && ch != '"' && ch != '\\')
            ++printable;
    }
    if (printable + 2 < text.size())
        return {};

    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text)
    {
        if (std::isspace(ch))
            out.push_back('_');
        else if (std::isprint(ch) && ch != '"' && ch != '\\')
            out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool ReadRuntimeVectorTriple(const void* owner, size_t offset, RuntimeVectorTriple& out)
{
    if (!owner)
        return false;

    const auto* base = reinterpret_cast<const uint8_t*>(owner);
    return
        TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + offset), out.begin) &&
        TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + offset + sizeof(std::uintptr_t)), out.end) &&
        TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + offset + sizeof(std::uintptr_t) * 2), out.capacity);
}

bool IsPlausibleVectorTriple(const RuntimeVectorTriple& triple, uint32_t maxBytes)
{
    if (triple.begin == 0 || triple.end == 0 || triple.capacity == 0)
        return false;
    if (triple.begin > triple.end || triple.end > triple.capacity)
        return false;
    if ((triple.begin % alignof(void*)) != 0 ||
        (triple.end % alignof(void*)) != 0 ||
        (triple.capacity % alignof(void*)) != 0)
    {
        return false;
    }

    const std::uintptr_t usedBytes = triple.end - triple.begin;
    const std::uintptr_t capacityBytes = triple.capacity - triple.begin;
    if (usedBytes == 0 || capacityBytes == 0 || capacityBytes > maxBytes || usedBytes > capacityBytes)
        return false;

    return IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(triple.begin), 1) &&
        IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(triple.end - 1), 1);
}

bool ReadNodeFields(const void* node, NodeFields& out)
{
    out = {};
    if (!node)
        return false;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder node", node, kNodeStride, RuntimeAccess::Read, &reason))
        return false;

    const auto* base = reinterpret_cast<const uint8_t*>(node);
    out.readable = true;
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x10), out.nodeId);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x18), out.childCursor);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x1C), out.childCount);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x20), out.attrCount);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x24), out.attrCursor);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x28), out.childIndexBlockBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x30), out.childIndexBlockEnd);
    TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(base + 0x48), out.valid);
    return true;
}

uint32_t ReadVectorElementCount(std::uintptr_t begin, std::uintptr_t end, size_t stride)
{
    if (begin == 0 || end < begin || stride == 0)
        return 0;

    const std::uintptr_t bytes = end - begin;
    if ((bytes % stride) != 0)
        return 0;

    const std::uintptr_t count = bytes / stride;
    if (count > 100000)
        return 0;

    return static_cast<uint32_t>(count);
}

bool ReadWriteNodeFields(const void* node, NodeFields& out)
{
    out = {};
    if (!node)
        return false;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder write node", node, 0x80, RuntimeAccess::Read, &reason))
        return false;

    const auto* base = reinterpret_cast<const uint8_t*>(node);
    std::uintptr_t attrBegin = 0;
    std::uintptr_t attrEnd = 0;
    std::uintptr_t childBegin = 0;
    std::uintptr_t childEnd = 0;
    uint32_t childCount = 0;
    uint8_t valid = 0;
    uint32_t nameToken = 0;

    out.readable = true;
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x20), nameToken);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x50), attrBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x58), attrEnd);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x68), childBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x70), childEnd);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x44), childCount);
    TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(base + 0x41), valid);

    out.nodeId = nameToken;
    out.attrCursor = 0;
    out.attrCount = ReadVectorElementCount(attrBegin, attrEnd, 0x28);
    out.childCursor = 0;
    out.childCount = childCount;
    out.childIndexBlockBegin = childBegin;
    out.childIndexBlockEnd = childEnd;
    out.attrVectorBegin = attrBegin;
    out.attrVectorEnd = attrEnd;
    out.childVectorBegin = childBegin;
    out.childVectorEnd = childEnd;
    out.valid = valid;
    return true;
}

std::uintptr_t ComputeNodeBlockBegin(const void* node, int32_t nodeIndex)
{
    if (!node || nodeIndex < 0 || nodeIndex > 100000)
        return 0;

    const std::uintptr_t nodeAddress = reinterpret_cast<std::uintptr_t>(node);
    const std::uintptr_t delta = static_cast<std::uintptr_t>(nodeIndex) * kNodeStride;
    if (nodeAddress < delta)
        return 0;
    return nodeAddress - delta;
}

std::uintptr_t ComputeSyntheticWriteNodeBlockBegin(const void* node, int32_t nodeIndex)
{
    if (!node || nodeIndex < 0 || nodeIndex > 100000)
        return 0;

    const std::uintptr_t nodeAddress = reinterpret_cast<std::uintptr_t>(node);
    const std::uintptr_t delta = static_cast<std::uintptr_t>(nodeIndex) * kNodeStride;
    return nodeAddress >= delta ? nodeAddress - delta : nodeAddress;
}

void AppendNodeFields(const NodeFields& fields, std::string& out)
{
    if (!fields.readable)
    {
        out += "/nodeUnreadable";
        return;
    }

    out +=
        "/nodeId=" + std::to_string(fields.nodeId) +
        "/childCursor=" + std::to_string(fields.childCursor) +
        "/childCount=" + std::to_string(fields.childCount) +
        "/attrCursor=" + std::to_string(fields.attrCursor) +
        "/attrCount=" + std::to_string(fields.attrCount) +
        "/childBlocks=" + PointerHex(fields.childIndexBlockBegin) + ".." + PointerHex(fields.childIndexBlockEnd) +
        "/valid=" + std::to_string(fields.valid ? 1 : 0);
    if (fields.attrVectorBegin != 0 || fields.attrVectorEnd != 0)
        out += "/writeAttrVec=" + PointerHex(fields.attrVectorBegin) + ".." + PointerHex(fields.attrVectorEnd);
    if (fields.childVectorBegin != 0 || fields.childVectorEnd != 0)
        out += "/writeChildVec=" + PointerHex(fields.childVectorBegin) + ".." + PointerHex(fields.childVectorEnd);
}

void DecodeChildEntryIndices(
    const NodeFields& fields,
    std::vector<uint32_t>& out,
    uint32_t maxEntries,
    uint32_t& guardCounter);

CoopSaveStoreDecoder::StoreNodeRecord MakeNodeRecord(
    uint32_t nodeIndex,
    std::uintptr_t nodeAddress,
    const NodeFields& fields,
    uint32_t& guardCounter)
{
    CoopSaveStoreDecoder::StoreNodeRecord record;
    record.nodeIndex = nodeIndex;
    record.nodeId = fields.nodeId;
    record.childCursor = fields.childCursor;
    record.childCount = fields.childCount;
    record.attrCursor = fields.attrCursor;
    record.attrCount = fields.attrCount;
    record.nodePtr = nodeAddress;
    record.childIndexBlockBegin = fields.childIndexBlockBegin;
    record.childIndexBlockEnd = fields.childIndexBlockEnd;
    record.attrVectorBegin = fields.attrVectorBegin;
    record.attrVectorEnd = fields.attrVectorEnd;
    record.childVectorBegin = fields.childVectorBegin;
    record.childVectorEnd = fields.childVectorEnd;
    DecodeChildEntryIndices(fields, record.childEntryIndices, 256, guardCounter);
    record.readable = fields.readable;
    record.valid = fields.valid != 0;
    return record;
}

bool TryReadPointerAt(const void* owner, size_t offset, std::uintptr_t& out)
{
    out = 0;
    if (!owner)
        return false;
    const auto* base = reinterpret_cast<const uint8_t*>(owner);
    return TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + offset), out);
}

uint32_t ReadNodeCountFromReadStoreBlock(std::uintptr_t nodeBlockBegin)
{
    if (nodeBlockBegin < sizeof(uint32_t))
        return 0;

    uint32_t taggedCount = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(nodeBlockBegin - sizeof(uint32_t)), taggedCount))
        return 0;

    return taggedCount & 0x7fffffffu;
}

void DecodeChildEntryIndices(
    const NodeFields& fields,
    std::vector<uint32_t>& out,
    uint32_t maxEntries,
    uint32_t& guardCounter)
{
    out.clear();
    if (fields.childCount == 0 ||
        fields.childIndexBlockBegin == 0 ||
        fields.childIndexBlockEnd <= fields.childIndexBlockBegin ||
        ((fields.childIndexBlockEnd - fields.childIndexBlockBegin) % sizeof(uint32_t)) != 0)
    {
        return;
    }

    const std::uintptr_t blockBytes = fields.childIndexBlockEnd - fields.childIndexBlockBegin;
    const uint32_t availableBlocks = static_cast<uint32_t>(std::min<std::uintptr_t>(
        blockBytes / sizeof(uint32_t),
        65536));
    const uint32_t requiredBlocks =
        (fields.childCount >> 6) + ((fields.childCount & 0x3fu) != 0 ? 1u : 0u);
    const uint32_t blocks = std::min(availableBlocks, requiredBlocks);
    if (blocks == 0)
        return;

    std::string reason;
    if (!PreflightRuntimePointer(
            "save store decoder child entry blocks",
            reinterpret_cast<const void*>(fields.childIndexBlockBegin),
            static_cast<size_t>(blocks) * sizeof(uint32_t),
            RuntimeAccess::Read,
            &reason))
    {
        ++guardCounter;
        return;
    }

    out.reserve(std::min(fields.childCount, maxEntries));
    uint32_t remaining = fields.childCount;
    for (uint32_t block = 0; block < blocks && remaining > 0 && out.size() < maxEntries; ++block)
    {
        uint32_t baseEntryIndex = 0;
        if (!TryReadRuntimeValue(
                reinterpret_cast<const uint32_t*>(fields.childIndexBlockBegin + static_cast<std::uintptr_t>(block) * sizeof(uint32_t)),
                baseEntryIndex))
        {
            ++guardCounter;
            return;
        }

        const uint32_t entriesInBlock = std::min<uint32_t>(remaining, 64);
        for (uint32_t i = 0; i < entriesInBlock && out.size() < maxEntries; ++i)
            out.push_back(baseEntryIndex + i);
        remaining -= entriesInBlock;
    }
}

bool PointerInRange(std::uintptr_t ptr, std::uintptr_t begin, std::uintptr_t end, size_t bytes = 1)
{
    if (ptr < begin || ptr >= end)
        return false;
    return bytes <= end - ptr;
}

void AppendCursorHit(
    const char* label,
    size_t ownerOffset,
    std::uintptr_t base,
    uint32_t cursor,
    uint32_t count,
    size_t stride,
    uint32_t rawBytes,
    std::string& out,
    uint32_t& emitted,
    uint32_t maxCandidates,
    std::vector<std::uintptr_t>& emittedTargets)
{
    if (emitted >= maxCandidates || base == 0 || cursor == 0 || count == 0)
        return;
    if (!IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(base), 1))
        return;

    std::uintptr_t byteOffset = cursor;
    if (stride > 0)
    {
        if (cursor > std::numeric_limits<std::uintptr_t>::max() / stride)
            return;
        byteOffset = static_cast<std::uintptr_t>(cursor) * stride;
    }
    if (byteOffset > 128ull * 1024ull * 1024ull)
        return;

    const std::uintptr_t address = base + byteOffset;
    if (address < base)
        return;
    if (std::find(emittedTargets.begin(), emittedTargets.end(), address) != emittedTargets.end())
        return;

    const size_t bytesToRead = std::max<size_t>(1, std::min<size_t>(rawBytes, 96));
    std::string reason;
    if (!PreflightRuntimePointer(
            "save store decoder cursor target",
            reinterpret_cast<const void*>(address),
            bytesToRead,
            RuntimeAccess::Read,
            &reason))
    {
        return;
    }

    if (emitted > 0)
        out += ",";
    emittedTargets.push_back(address);
    out +=
        HexOffset(ownerOffset) +
        ":" + PointerHex(base) +
        ":" + (label && label[0] ? label : "cursor") +
        ":cursor=" + std::to_string(cursor) +
        ":count=" + std::to_string(count);
    if (stride > 0)
        out += ":stride=" + std::to_string(stride);
    else
        out += ":byteOff=1";
    out +=
        ":target=" + PointerHex(address) +
        ":raw=" + ReadRuntimeBytesHex(reinterpret_cast<const void*>(address), bytesToRead);

    if (address >= base + 16 && IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(address - 16), 16))
        out += ":pre16=" + ReadRuntimeBytesHex(reinterpret_cast<const void*>(address - 16), std::min<size_t>(16, bytesToRead));

    const std::string text = TextProbe(reinterpret_cast<const void*>(address));
    if (!text.empty())
        out += ":text=" + text;

    ++emitted;
}

void AppendPoolScanForCursor(
    const char* label,
    const void* store,
    uint32_t cursor,
    uint32_t count,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!store || cursor == 0 || count == 0 || options.maxPoolCandidates == 0)
        return;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder pool owner", store, options.poolBytes, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        return;
    }

    std::string hits;
    uint32_t emitted = 0;
    std::vector<std::uintptr_t> emittedTargets;
    const auto* bytes = reinterpret_cast<const uint8_t*>(store);
    const std::array<size_t, 8> strides = { 0, 2, 4, 8, 12, 16, 24, 32 };
    for (size_t offset = 0; offset + sizeof(std::uintptr_t) <= options.poolBytes && emitted < options.maxPoolCandidates; offset += sizeof(std::uintptr_t))
    {
        std::uintptr_t base = 0;
        ++result.slots;
        if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(bytes + offset), base))
        {
            ++result.guards;
            continue;
        }

        for (size_t stride : strides)
        {
            AppendCursorHit(
                label,
                offset,
                base,
                cursor,
                count,
                stride,
                options.rawBytes,
                hits,
                emitted,
                options.maxPoolCandidates,
                emittedTargets);
            if (emitted >= options.maxPoolCandidates)
                break;
            if (!options.poolStrideScan)
                break;
        }
    }

    if (!hits.empty())
    {
        result.detail += "/" + std::string(label && label[0] ? label : "Cursor") + "PoolScan=[" + hits + "]";
        result.candidates += emitted;
    }
}

void AppendVectorPoolScan(
    const void* store,
    const NodeFields& fields,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!store || !fields.readable)
        return;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder vector owner", store, options.storeBytes, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        return;
    }

    std::string hits;
    uint32_t emitted = 0;
    const std::array<size_t, 8> strides = { 1, 2, 4, 8, 12, 16, 24, 32 };
    for (size_t offset = 0; offset + sizeof(std::uintptr_t) * 3 <= options.storeBytes && emitted < options.maxPoolCandidates; offset += sizeof(std::uintptr_t))
    {
        RuntimeVectorTriple triple;
        ++result.slots;
        if (!ReadRuntimeVectorTriple(store, offset, triple))
        {
            ++result.guards;
            continue;
        }
        if (!IsPlausibleVectorTriple(triple, 128u * 1024u * 1024u))
            continue;

        auto tryCursor = [&](const char* label, uint32_t cursor, uint32_t count)
        {
            if (cursor == 0 || count == 0 || emitted >= options.maxPoolCandidates)
                return;

            const std::uintptr_t usedBytes = triple.end - triple.begin;
            if (cursor < usedBytes)
            {
                if (emitted > 0)
                    hits += ",";
                hits +=
                    HexOffset(offset) +
                    ":" + label +
                    ":vec=" + PointerHex(triple.begin) +
                    ".." + PointerHex(triple.end) +
                    ":cursor=" + std::to_string(cursor) +
                    ":byteOffRaw=" +
                    ReadRuntimeBytesHex(reinterpret_cast<const void*>(triple.begin + cursor), std::min<size_t>(options.rawBytes, usedBytes - cursor));
                ++emitted;
            }

            for (size_t stride : strides)
            {
                if (emitted >= options.maxPoolCandidates)
                    return;
                const std::uintptr_t byteOffset = static_cast<std::uintptr_t>(cursor) * stride;
                if (byteOffset >= usedBytes)
                    continue;

                const size_t bytesToRead = std::min<size_t>(
                    options.rawBytes,
                    std::max<size_t>(1, std::min<std::uintptr_t>(usedBytes - byteOffset, stride * std::max<uint32_t>(1, count))));
                if (emitted > 0)
                    hits += ",";
                hits +=
                    HexOffset(offset) +
                    ":" + label +
                    ":vec=" + PointerHex(triple.begin) +
                    ".." + PointerHex(triple.end) +
                    ":cursor=" + std::to_string(cursor) +
                    ":stride=" + std::to_string(stride) +
                    ":raw=" +
                    ReadRuntimeBytesHex(reinterpret_cast<const void*>(triple.begin + byteOffset), bytesToRead);
                ++emitted;
            }
        };

        tryCursor("attr", fields.attrCursor, fields.attrCount);
        tryCursor("child", fields.childCursor, fields.childCount);
    }

    if (!hits.empty())
    {
        result.detail += "/VectorPoolScan=[" + hits + "]";
        result.candidates += emitted;
    }
}

void BuildReadStoreMap(
    const void* store,
    std::uintptr_t computedNodeBlockBegin,
    int32_t currentNodeIndex,
    const NodeFields& currentFields,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!store || currentNodeIndex < 0)
        return;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder read store header", store, 0x128, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        return;
    }

    CoopSaveStoreDecoder::StoreMap map;
    map.store = reinterpret_cast<std::uintptr_t>(store);
    map.readStore = true;

    std::uintptr_t storeNodeBlock = 0;
    TryReadPointerAt(store, 0x10, storeNodeBlock);
    map.nodeBlockBegin = storeNodeBlock ? storeNodeBlock : computedNodeBlockBegin;
    map.nodeCount = ReadNodeCountFromReadStoreBlock(map.nodeBlockBegin);

    if (map.nodeBlockBegin != 0 && map.nodeCount > 0 && map.nodeCount < 200000)
    {
        map.nodeBlockEnd = map.nodeBlockBegin + static_cast<std::uintptr_t>(map.nodeCount) * kNodeStride;
        map.ok =
            map.nodeBlockEnd > map.nodeBlockBegin &&
            IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(map.nodeBlockBegin), 1) &&
            IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(map.nodeBlockEnd - 1), 1);
    }

    TryReadPointerAt(store, 0x68, map.attrStringBase);
    TryReadPointerAt(store, 0x80, map.attrNameOffsetTable);
    map.attrTokenContext = map.store + 0xB0;
    TryReadPointerAt(reinterpret_cast<const void*>(map.attrTokenContext), 0x8, map.attrTokenIndexBase);
    TryReadPointerAt(reinterpret_cast<const void*>(map.attrTokenContext), 0x28, map.attrTokenBase);

    map.childNameResolverContext = map.store + 0x38;
    TryReadPointerAt(reinterpret_cast<const void*>(map.childNameResolverContext), 0x8, map.childNameDataBase);
    TryReadPointerAt(reinterpret_cast<const void*>(map.childNameResolverContext), 0x20, map.childNameOffsetTable);

    if (map.ok)
    {
        const std::uintptr_t currentNodePtr =
            map.nodeBlockBegin + static_cast<std::uintptr_t>(currentNodeIndex) * kNodeStride;
        map.currentNode = MakeNodeRecord(
            static_cast<uint32_t>(currentNodeIndex),
            currentNodePtr,
            currentFields,
            result.guards);

        std::vector<int32_t> indices;
        indices.push_back(currentNodeIndex);
        if (options.nodeWindow > 0)
        {
            const int32_t before = static_cast<int32_t>(std::min<uint32_t>(
                options.nodeWindow,
                static_cast<uint32_t>(std::max<int32_t>(currentNodeIndex, 0))));
            const int32_t after = static_cast<int32_t>(options.nodeWindow);
            for (int32_t idx = currentNodeIndex - before; idx <= currentNodeIndex + after; ++idx)
            {
                if (idx >= 0 && static_cast<uint32_t>(idx) < map.nodeCount)
                    indices.push_back(idx);
            }
        }
        if (options.fullStoreMap)
        {
            const uint32_t fullCount = std::min<uint32_t>(map.nodeCount, options.maxStoreEntries);
            indices.reserve(indices.size() + fullCount);
            for (uint32_t idx = 0; idx < fullCount; ++idx)
                indices.push_back(static_cast<int32_t>(idx));
        }

        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (int32_t idx : indices)
        {
            const std::uintptr_t nodeAddress = map.nodeBlockBegin + static_cast<std::uintptr_t>(idx) * kNodeStride;
            NodeFields fields;
            ++result.slots;
            if (!ReadNodeFields(reinterpret_cast<const void*>(nodeAddress), fields))
            {
                ++result.guards;
                continue;
            }
            map.sampledNodes.push_back(MakeNodeRecord(static_cast<uint32_t>(idx), nodeAddress, fields, result.guards));
        }
    }

    result.storeMap = std::move(map);
}

void BuildWriteStoreMap(
    const void* store,
    std::uintptr_t syntheticNodeBlockBegin,
    int32_t currentNodeIndex,
    const void* currentNode,
    const NodeFields& currentFields,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!store || !currentNode || currentNodeIndex < 0 || syntheticNodeBlockBegin == 0)
        return;

    CoopSaveStoreDecoder::StoreMap map;
    map.store = reinterpret_cast<std::uintptr_t>(store);
    map.readStore = false;
    map.nodeBlockBegin = syntheticNodeBlockBegin;
    map.nodeBlockEnd = syntheticNodeBlockBegin + kNodeStride;
    map.nodeCount = static_cast<uint32_t>(currentNodeIndex) + 1u;
    map.ok = currentFields.readable;

    RuntimeVectorTriple nodePointerTable;
    const bool hasNodePointerTable =
        ReadRuntimeVectorTriple(store, 0, nodePointerTable) &&
        nodePointerTable.begin != 0 &&
        nodePointerTable.end > nodePointerTable.begin &&
        ((nodePointerTable.end - nodePointerTable.begin) % sizeof(std::uintptr_t)) == 0 &&
        IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(nodePointerTable.begin), 1) &&
        IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(nodePointerTable.end - 1), 1);

    if (hasNodePointerTable)
    {
        map.nodeBlockBegin = nodePointerTable.begin;
        map.nodeBlockEnd = nodePointerTable.end;
        const uint32_t tableSlots = static_cast<uint32_t>(std::min<std::uintptr_t>(
            (nodePointerTable.end - nodePointerTable.begin) / sizeof(std::uintptr_t),
            EnvUIntClamped(
                "COOP_SAVE_STORE_DECODER_WRITE_NODE_SCAN_LIMIT",
                nullptr,
                options.fullStoreMap ? 8192u : 512u,
                16u,
                65536u)));

        uint32_t maxValidNodeIndexPlusOne = 0;
        for (uint32_t idx = 0; idx < tableSlots; ++idx)
        {
            const std::uintptr_t slotAddress =
                nodePointerTable.begin + static_cast<std::uintptr_t>(idx) * sizeof(std::uintptr_t);
            const void* node = nullptr;
            ++result.slots;
            if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(slotAddress), node) || !node)
                continue;

            NodeFields fields;
            if (!ReadWriteNodeFields(node, fields) || !fields.readable)
            {
                ++result.guards;
                continue;
            }

            maxValidNodeIndexPlusOne = idx + 1u;
            const bool inWindow =
                options.nodeWindow > 0 &&
                std::abs(static_cast<int32_t>(idx) - currentNodeIndex) <= static_cast<int32_t>(options.nodeWindow);
            const bool sample =
                idx == 0 ||
                idx == static_cast<uint32_t>(currentNodeIndex) ||
                inWindow ||
                (options.fullStoreMap && map.sampledNodes.size() < options.maxStoreEntries);
            if (sample)
                map.sampledNodes.push_back(MakeNodeRecord(idx, reinterpret_cast<std::uintptr_t>(node), fields, result.guards));
        }

        if (maxValidNodeIndexPlusOne != 0)
            map.nodeCount = maxValidNodeIndexPlusOne;
    }

    if (map.ok)
    {
        map.currentNode = MakeNodeRecord(
            static_cast<uint32_t>(currentNodeIndex),
            reinterpret_cast<std::uintptr_t>(currentNode),
            currentFields,
            result.guards);
    }

    result.storeMap = std::move(map);
}

void AppendStoreMap(
    const void* store,
    std::uintptr_t nodeBlockBegin,
    int32_t currentNodeIndex,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!store || options.maxStoreEntries == 0)
        return;

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder store map", store, options.storeBytes, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        return;
    }

    const std::uintptr_t currentNode = nodeBlockBegin && currentNodeIndex >= 0
        ? nodeBlockBegin + static_cast<std::uintptr_t>(currentNodeIndex) * kNodeStride
        : 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(store);
    uint32_t emitted = 0;
    std::string entries;
    for (size_t offset = 0; offset + sizeof(std::uintptr_t) <= options.storeBytes; offset += sizeof(std::uintptr_t))
    {
        std::uintptr_t word = 0;
        ++result.slots;
        if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(bytes + offset), word))
        {
            ++result.guards;
            continue;
        }

        const bool mapped = word && IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(word), 1);
        const bool nodeBlockBeginHit = nodeBlockBegin && word == nodeBlockBegin;
        const bool nodeHit = currentNode && word == currentNode;
        const bool nodeBlockMember =
            nodeBlockBegin &&
            word >= nodeBlockBegin &&
            ((word - nodeBlockBegin) % kNodeStride) == 0 &&
            (word - nodeBlockBegin) / kNodeStride < 100000;
        const bool knownHeaderOffset = offset < 0x40 || offset == 0x150 || offset == 0x158 || offset == 0x160;
        const bool smallValue = word != 0 && word < 0x1000000;
        const bool interesting =
            options.fullStoreMap ||
            mapped ||
            nodeBlockBeginHit ||
            nodeHit ||
            nodeBlockMember ||
            knownHeaderOffset ||
            (knownHeaderOffset && smallValue);

        if (!interesting)
            continue;
        if (emitted >= options.maxStoreEntries)
            break;

        if (emitted > 0)
            entries += ",";
        entries += HexOffset(offset) + ":" + PointerHex(word);
        if (mapped)
            entries += ":mapped";
        if (nodeBlockBeginHit)
            entries += ":nodeBlockBegin";
        else if (nodeHit)
            entries += ":currentNode";
        else if (nodeBlockMember)
            entries += ":nodeBlockMember";
        if (smallValue)
            entries += ":small=" + std::to_string(static_cast<uint64_t>(word));
        if (knownHeaderOffset || nodeBlockBeginHit || nodeHit || nodeBlockMember)
            entries += ":raw=" + ReadRuntimeBytesHex(bytes + offset, std::min<size_t>(options.rawBytes, options.storeBytes - offset));

        ++emitted;
    }

    if (!entries.empty())
    {
        result.detail +=
            "/StoreMap=ptr:" + PointerHex(store) +
            ":bytes=" + std::to_string(options.storeBytes) +
            ":entries=[" + entries + "]";
        result.candidates += emitted;
    }
}

void AppendNodeMap(
    std::uintptr_t nodeBlockBegin,
    const std::vector<StackEntry>& entries,
    const CoopSaveStoreDecoder::DecodeOptions& options,
    CoopSaveStoreDecoder::DecodeResult& result)
{
    if (!nodeBlockBegin)
        return;

    std::vector<int32_t> indices;
    for (const StackEntry& entry : entries)
    {
        if (entry.nodeIndex >= 0)
            indices.push_back(entry.nodeIndex);
    }
    if (!indices.empty() && options.nodeWindow > 0)
    {
        const int32_t current = indices.front();
        const int32_t before = static_cast<int32_t>(std::min<uint32_t>(options.nodeWindow, static_cast<uint32_t>(std::max<int32_t>(current, 0))));
        const int32_t after = static_cast<int32_t>(options.nodeWindow);
        for (int32_t idx = current - before; idx <= current + after; ++idx)
        {
            if (idx >= 0)
                indices.push_back(idx);
        }
    }

    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    std::string nodes;
    uint32_t emitted = 0;
    for (int32_t idx : indices)
    {
        if (idx < 0 || idx > 100000)
            continue;
        const std::uintptr_t nodeAddress = nodeBlockBegin + static_cast<std::uintptr_t>(idx) * kNodeStride;
        NodeFields fields;
        ++result.slots;
        if (!ReadNodeFields(reinterpret_cast<const void*>(nodeAddress), fields))
        {
            ++result.guards;
            continue;
        }
        if (emitted > 0)
            nodes += ",";
        nodes +=
            "idx=" + std::to_string(idx) +
            ":ptr=" + PointerHex(nodeAddress);
        AppendNodeFields(fields, nodes);
        ++emitted;
    }

    if (!nodes.empty())
    {
        result.detail +=
            "/NodeMap=block:" + PointerHex(nodeBlockBegin) +
            ":stride=0x50:nodes=[" + nodes + "]";
        result.candidates += emitted;
    }
}
}

namespace CoopSaveStoreDecoder
{
DecodeOptions OptionsFromEnvironment()
{
    DecodeOptions options;
    options.fullStoreMap = EnvFlagEnabled("COOP_SAVE_STORE_DECODER_FULL_STORE_MAP");
    options.stackEntries = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_STACK_ENTRIES",
        "COOP_SERIALIZER_NODE_PROBE_STACK_ENTRIES",
        options.stackEntries,
        1,
        16);
    options.storeBytes = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_STORE_BYTES",
        "COOP_SERIALIZER_NODE_PROBE_VECTOR_BYTES",
        options.storeBytes,
        0x40,
        0x4000);
    options.poolBytes = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_POOL_BYTES",
        "COOP_SERIALIZER_NODE_PROBE_POOL_BYTES",
        options.poolBytes,
        0x40,
        0x8000);
    options.rawBytes = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_RAW_BYTES",
        "COOP_SERIALIZER_NODE_PROBE_VECTOR_RAW_BYTES",
        options.rawBytes,
        8,
        128);
    options.maxStoreEntries = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_MAX_STORE_ENTRIES",
        nullptr,
        options.fullStoreMap ? 8192u : options.maxStoreEntries,
        8,
        250000);
    options.maxPoolCandidates = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_MAX_POOL_CANDIDATES",
        "COOP_SERIALIZER_NODE_PROBE_VECTOR_CANDIDATES",
        options.maxPoolCandidates,
        1,
        128);
    options.nodeWindow = EnvUIntClamped(
        "COOP_SAVE_STORE_DECODER_NODE_WINDOW",
        nullptr,
        options.nodeWindow,
        0,
        32);
    options.poolStrideScan = EnvFlagEnabled("COOP_SAVE_STORE_DECODER_POOL_STRIDES");
    return options;
}

std::string BuildStoreMapStatus(const StoreMap& map)
{
    std::ostringstream out;
    out << "StoreMap2="
        << (map.ok ? 1 : 0)
        << ":read=" << (map.readStore ? 1 : 0)
        << ":store=" << PointerHex(map.store)
        << ":nodes=" << map.nodeCount
        << ":block=" << PointerHex(map.nodeBlockBegin) << ".." << PointerHex(map.nodeBlockEnd)
        << ":cur=" << map.currentNode.nodeIndex
        << ":curId=" << map.currentNode.nodeId
        << ":attrs=" << map.currentNode.attrCursor << "+" << map.currentNode.attrCount
        << ":children=" << map.currentNode.childCursor << "+" << map.currentNode.childCount
        << ":childBlocks=" << PointerHex(map.currentNode.childIndexBlockBegin) << ".." << PointerHex(map.currentNode.childIndexBlockEnd)
        << ":childEntries=" << map.currentNode.childEntryIndices.size()
        << ":attrPools=" << PointerHex(map.attrStringBase) << "," << PointerHex(map.attrNameOffsetTable) << "," << PointerHex(map.attrTokenIndexBase) << "," << PointerHex(map.attrTokenBase)
        << ":childPools=" << PointerHex(map.childNameDataBase) << "," << PointerHex(map.childNameResolverContext) << "," << PointerHex(map.childNameOffsetTable)
        << ":sampled=" << map.sampledNodes.size();
    if (!map.currentNode.childEntryIndices.empty())
    {
        out << ":childEntryIds=";
        const size_t limit = std::min<size_t>(map.currentNode.childEntryIndices.size(), 8);
        for (size_t i = 0; i < limit; ++i)
        {
            if (i > 0)
                out << ",";
            out << map.currentNode.childEntryIndices[i];
        }
        if (map.currentNode.childEntryIndices.size() > limit)
            out << ",more" << (map.currentNode.childEntryIndices.size() - limit);
    }
    return out.str();
}

DecodeResult DecodeSerializerStore(const void* serializerImpl, const DecodeOptions& options)
{
    DecodeResult result;
    if (!serializerImpl)
        return result;
    result.serializerImpl = reinterpret_cast<std::uintptr_t>(serializerImpl);

    std::string reason;
    if (!PreflightRuntimePointer("save store decoder wrapper", serializerImpl, sizeof(void*) * 2, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=wrapperUnreadable";
        return result;
    }

    const auto* wrapper = reinterpret_cast<const uint8_t*>(serializerImpl);
    const void* vtable = nullptr;
    const void* helper = nullptr;
    ++result.slots;
    if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(wrapper), vtable))
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=vtableUnreadable";
        return result;
    }
    ++result.slots;
    if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(wrapper + sizeof(void*)), helper) || !helper)
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=helperMissing";
        return result;
    }

    if (!PreflightRuntimePointer("save store decoder helper", helper, 0x70, RuntimeAccess::Read, &reason))
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=helperUnreadable";
        return result;
    }

    const auto* helperBytes = reinterpret_cast<const uint8_t*>(helper);
    uint16_t flags = 0;
    uint32_t skipDepth = 0;
    RuntimeVectorTriple loadStack;
    RuntimeVectorTriple writeStack;
    const void* context = nullptr;
    result.helper = reinterpret_cast<std::uintptr_t>(helper);
    TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(helperBytes), flags);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(helperBytes + 0x4), skipDepth);
    ReadRuntimeVectorTriple(helper, 0x8, loadStack);
    ReadRuntimeVectorTriple(helper, 0x10, writeStack);
    TryReadRuntimeValue(reinterpret_cast<const void* const*>(helperBytes + 0x20), context);
    result.context = reinterpret_cast<std::uintptr_t>(context);

    auto stackLooksValid = [](const RuntimeVectorTriple& stack) -> bool
    {
        return
            stack.begin != 0 &&
            stack.end >= stack.begin &&
            stack.capacity >= stack.end &&
            ((stack.end - stack.begin) % kHelperStackEntryStride) == 0 &&
            ((stack.capacity - stack.begin) % kHelperStackEntryStride) == 0 &&
            ((stack.end - stack.begin) / kHelperStackEntryStride) < 512u &&
            IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(stack.begin), 1) &&
            IsRuntimePointerMappedInCurrentProcess(reinterpret_cast<const void*>(stack.end - 1), 1);
    };

    const bool loadStackValid = stackLooksValid(loadStack);
    const bool writeStackValid = stackLooksValid(writeStack);
    SerializerStackLayout stackLayout = SerializerStackLayout::ReadStore;
    RuntimeVectorTriple activeStack = loadStack;
    if (!loadStackValid && writeStackValid)
    {
        stackLayout = SerializerStackLayout::WriteStore;
        activeStack = writeStack;
        TryReadRuntimeValue(reinterpret_cast<const void* const*>(helperBytes + 0x58), context);
        result.context = reinterpret_cast<std::uintptr_t>(context);
    }
    result.slots += 9;

    result.detail =
        "SaveStoreDecoder=wrapper:" + PointerHex(serializerImpl) +
        "/vt:" + PointerHex(vtable) +
        "/helper:" + PointerHex(helper) +
        "/flags:" + std::to_string(flags) +
        "/skip:" + std::to_string(skipDepth) +
        "/ctx:" + PointerHex(context) +
        "/layout:" + std::string(stackLayout == SerializerStackLayout::WriteStore ? "write" : "read") +
        "/stack:" + PointerHex(activeStack.begin) +
        ".." + PointerHex(activeStack.end) +
        "/" + PointerHex(activeStack.capacity) +
        "/loadStack:" + PointerHex(loadStack.begin) +
        ".." + PointerHex(loadStack.end) +
        "/" + PointerHex(loadStack.capacity) +
        "/writeStack:" + PointerHex(writeStack.begin) +
        ".." + PointerHex(writeStack.end) +
        "/" + PointerHex(writeStack.capacity);

    result.helperStackSeen = true;
    if (!loadStackValid && !writeStackValid)
    {
        result.detail += "/stackInvalid";
        return result;
    }

    const uint32_t depth = static_cast<uint32_t>((activeStack.end - activeStack.begin) / kHelperStackEntryStride);
    const uint32_t entriesToDump = std::min<uint32_t>(depth, options.stackEntries);
    result.detail += "/depth:" + std::to_string(depth);

    std::vector<StackEntry> entries;
    entries.reserve(entriesToDump);
    std::string stackDetail;
    for (uint32_t entryIndex = 0; entryIndex < entriesToDump; ++entryIndex)
    {
        const std::uintptr_t entryAddress = activeStack.end - kHelperStackEntryStride * (entryIndex + 1u);
        if (!PreflightRuntimePointer(
                "save store decoder stack entry",
                reinterpret_cast<const void*>(entryAddress),
                kHelperStackEntryStride,
                RuntimeAccess::Read,
                &reason))
        {
            ++result.guards;
            continue;
        }

        const auto* entryBytes = reinterpret_cast<const uint8_t*>(entryAddress);
        StackEntry entry;
        entry.address = entryAddress;
        TryReadRuntimeValue(reinterpret_cast<const void* const*>(entryBytes), entry.store);
        TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0x8), entry.nodeIndex);
        TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0xC), entry.generation);
        TryReadRuntimeValue(reinterpret_cast<const void* const*>(entryBytes + 0x10), entry.node);
        result.slots += 4;

        if (stackLayout == SerializerStackLayout::WriteStore)
        {
            if (!entry.node && entry.store && entry.nodeIndex >= 0 && entry.nodeIndex < 100000)
            {
                std::uintptr_t nodePointerTable = 0;
                if (TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(entry.store), nodePointerTable) &&
                    nodePointerTable != 0)
                {
                    const std::uintptr_t pointerAddress =
                        nodePointerTable + static_cast<std::uintptr_t>(entry.nodeIndex) * sizeof(std::uintptr_t);
                    const void* resolvedNode = nullptr;
                    if (TryReadRuntimeValue(reinterpret_cast<const void* const*>(pointerAddress), resolvedNode))
                        entry.node = resolvedNode;
                }
            }
            ReadWriteNodeFields(entry.node, entry.fields);
        }
        else
        {
            ReadNodeFields(entry.node, entry.fields);
        }

        if (!stackDetail.empty())
            stackDetail += ",";
        stackDetail +=
            "e" + std::to_string(entryIndex) +
            ":entry=" + PointerHex(entry.address) +
            ":store=" + PointerHex(entry.store) +
            ":idx=" + std::to_string(entry.nodeIndex) +
            ":gen=" + std::to_string(entry.generation) +
            ":node=" + PointerHex(entry.node);
        AppendNodeFields(entry.fields, stackDetail);
        entries.push_back(entry);
    }
    if (!stackDetail.empty())
        result.detail += "/HelperStack=[" + stackDetail + "]";

    if (entries.empty())
        return result;

    const StackEntry& current = entries.front();
    const std::uintptr_t nodeBlockBegin = stackLayout == SerializerStackLayout::WriteStore
        ? ComputeSyntheticWriteNodeBlockBegin(current.node, current.nodeIndex)
        : ComputeNodeBlockBegin(current.node, current.nodeIndex);
    result.store = reinterpret_cast<std::uintptr_t>(current.store);
    result.node = reinterpret_cast<std::uintptr_t>(current.node);
    result.nodeBlockBegin = nodeBlockBegin;
    result.nodeIndex = current.nodeIndex;
    result.nodeId = current.fields.nodeId;
    result.childCursor = current.fields.childCursor;
    result.childCount = current.fields.childCount;
    result.attrCursor = current.fields.attrCursor;
    result.attrCount = current.fields.attrCount;
    result.attrVectorBegin = current.fields.attrVectorBegin;
    result.attrVectorEnd = current.fields.attrVectorEnd;
    result.childVectorBegin = current.fields.childVectorBegin;
    result.childVectorEnd = current.fields.childVectorEnd;
    result.nodeReadable = current.fields.readable;
    result.nodeValid = current.fields.valid != 0;
    if (nodeBlockBegin)
        result.detail += "/NodeBlockBegin=" + PointerHex(nodeBlockBegin);

    if (stackLayout == SerializerStackLayout::WriteStore)
        BuildWriteStoreMap(current.store, nodeBlockBegin, current.nodeIndex, current.node, current.fields, options, result);
    else
        BuildReadStoreMap(current.store, nodeBlockBegin, current.nodeIndex, current.fields, options, result);
    if (result.storeMap.ok)
        result.detail += "/" + BuildStoreMapStatus(result.storeMap);

    if (stackLayout != SerializerStackLayout::WriteStore)
    {
        AppendNodeMap(nodeBlockBegin, entries, options, result);
        AppendStoreMap(current.store, nodeBlockBegin, current.nodeIndex, options, result);
        AppendVectorPoolScan(current.store, current.fields, options, result);
        AppendPoolScanForCursor("Attr", current.store, current.fields.attrCursor, current.fields.attrCount, options, result);
        AppendPoolScanForCursor("Child", current.store, current.fields.childCursor, current.fields.childCount, options, result);
    }

    return result;
}

DecodeResult DecodeReadStackEntry(const void* stackEntry, const void* resolvedNode, const DecodeOptions& options)
{
    DecodeResult result;
    if (!stackEntry)
        return result;

    std::string reason;
    if (!PreflightRuntimePointer(
            "save store decoder read stack entry",
            stackEntry,
            kHelperStackEntryStride,
            RuntimeAccess::Read,
            &reason))
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=readStackEntryUnreadable";
        return result;
    }

    const auto* entryBytes = reinterpret_cast<const uint8_t*>(stackEntry);
    StackEntry entry;
    entry.address = reinterpret_cast<std::uintptr_t>(stackEntry);
    TryReadRuntimeValue(reinterpret_cast<const void* const*>(entryBytes), entry.store);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0x8), entry.nodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0xC), entry.generation);
    TryReadRuntimeValue(reinterpret_cast<const void* const*>(entryBytes + 0x10), entry.node);
    if (resolvedNode)
        entry.node = resolvedNode;
    result.slots += 4;

    ReadNodeFields(entry.node, entry.fields);
    const std::uintptr_t nodeBlockBegin = ComputeNodeBlockBegin(entry.node, entry.nodeIndex);
    result.serializerImpl = reinterpret_cast<std::uintptr_t>(stackEntry);
    result.helperStackSeen = true;
    result.store = reinterpret_cast<std::uintptr_t>(entry.store);
    result.node = reinterpret_cast<std::uintptr_t>(entry.node);
    result.nodeBlockBegin = nodeBlockBegin;
    result.nodeIndex = entry.nodeIndex;
    result.nodeId = entry.fields.nodeId;
    result.childCursor = entry.fields.childCursor;
    result.childCount = entry.fields.childCount;
    result.attrCursor = entry.fields.attrCursor;
    result.attrCount = entry.fields.attrCount;
    result.attrVectorBegin = entry.fields.attrVectorBegin;
    result.attrVectorEnd = entry.fields.attrVectorEnd;
    result.childVectorBegin = entry.fields.childVectorBegin;
    result.childVectorEnd = entry.fields.childVectorEnd;
    result.nodeReadable = entry.fields.readable;
    result.nodeValid = entry.fields.valid != 0;
    result.detail =
        "SaveStoreDecoder=readStackEntry:" + PointerHex(stackEntry) +
        "/store:" + PointerHex(entry.store) +
        "/idx:" + std::to_string(entry.nodeIndex) +
        "/gen:" + std::to_string(entry.generation) +
        "/node:" + PointerHex(entry.node);
    AppendNodeFields(entry.fields, result.detail);
    if (nodeBlockBegin)
        result.detail += "/NodeBlockBegin=" + PointerHex(nodeBlockBegin);

    if (entry.store && entry.node && entry.nodeIndex >= 0)
    {
        BuildReadStoreMap(entry.store, nodeBlockBegin, entry.nodeIndex, entry.fields, options, result);
        if (result.storeMap.ok)
            result.detail += "/" + BuildStoreMapStatus(result.storeMap);
    }

    if (entry.store)
        AppendStoreMap(entry.store, nodeBlockBegin, entry.nodeIndex, options, result);

    return result;
}

DecodeResult DecodeWriteStackEntry(const void* stackEntry, const void* resolvedNode, const DecodeOptions& options)
{
    DecodeResult result;
    if (!stackEntry)
        return result;

    std::string reason;
    if (!PreflightRuntimePointer(
            "save store decoder write stack entry",
            stackEntry,
            kHelperStackEntryStride,
            RuntimeAccess::Read,
            &reason))
    {
        ++result.guards;
        result.detail = "SaveStoreDecoder=writeStackEntryUnreadable";
        return result;
    }

    const auto* entryBytes = reinterpret_cast<const uint8_t*>(stackEntry);
    StackEntry entry;
    entry.address = reinterpret_cast<std::uintptr_t>(stackEntry);
    TryReadRuntimeValue(reinterpret_cast<const void* const*>(entryBytes), entry.store);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0x8), entry.nodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0xC), entry.generation);
    entry.node = resolvedNode;
    result.slots += 4;

    if (!entry.node && entry.store && entry.nodeIndex >= 0 && entry.nodeIndex < 100000)
    {
        std::uintptr_t nodePointerTable = 0;
        if (TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(entry.store), nodePointerTable) &&
            nodePointerTable != 0)
        {
            const std::uintptr_t pointerAddress =
                nodePointerTable + static_cast<std::uintptr_t>(entry.nodeIndex) * sizeof(std::uintptr_t);
            const void* tableNode = nullptr;
            if (TryReadRuntimeValue(reinterpret_cast<const void* const*>(pointerAddress), tableNode))
                entry.node = tableNode;
        }
    }

    ReadWriteNodeFields(entry.node, entry.fields);
    const std::uintptr_t nodeBlockBegin = ComputeSyntheticWriteNodeBlockBegin(entry.node, entry.nodeIndex);
    result.serializerImpl = reinterpret_cast<std::uintptr_t>(stackEntry);
    result.helperStackSeen = true;
    result.store = reinterpret_cast<std::uintptr_t>(entry.store);
    result.node = reinterpret_cast<std::uintptr_t>(entry.node);
    result.nodeBlockBegin = nodeBlockBegin;
    result.nodeIndex = entry.nodeIndex;
    result.nodeId = entry.fields.nodeId;
    result.childCursor = entry.fields.childCursor;
    result.childCount = entry.fields.childCount;
    result.attrCursor = entry.fields.attrCursor;
    result.attrCount = entry.fields.attrCount;
    result.attrVectorBegin = entry.fields.attrVectorBegin;
    result.attrVectorEnd = entry.fields.attrVectorEnd;
    result.childVectorBegin = entry.fields.childVectorBegin;
    result.childVectorEnd = entry.fields.childVectorEnd;
    result.nodeReadable = entry.fields.readable;
    result.nodeValid = entry.fields.valid != 0;
    result.detail =
        "SaveStoreDecoder=writeStackEntry:" + PointerHex(stackEntry) +
        "/store:" + PointerHex(entry.store) +
        "/idx:" + std::to_string(entry.nodeIndex) +
        "/gen:" + std::to_string(entry.generation) +
        "/node:" + PointerHex(entry.node);
    AppendNodeFields(entry.fields, result.detail);
    if (nodeBlockBegin)
        result.detail += "/NodeBlockBegin=" + PointerHex(nodeBlockBegin);

    if (entry.store && entry.node && entry.nodeIndex >= 0)
    {
        BuildWriteStoreMap(entry.store, nodeBlockBegin, entry.nodeIndex, entry.node, entry.fields, options, result);
        if (result.storeMap.ok)
            result.detail += "/" + BuildStoreMapStatus(result.storeMap);
    }

    if (options.fullStoreMap && entry.store)
        AppendStoreMap(entry.store, nodeBlockBegin, entry.nodeIndex, options, result);

    return result;
}
}
