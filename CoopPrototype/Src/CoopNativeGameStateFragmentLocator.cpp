#include "CoopNativeGameStateFragmentLocator.h"
#include "CoopNativeSaveStoreApi.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace
{
constexpr std::uintptr_t kReadStoreNodeBytes = 0x50u;
constexpr std::uintptr_t kWriteStoreNodeBytes = 0x80u;

std::string BoolText(bool value)
{
    return value ? "1" : "0";
}

template <typename T>
void AppendIdList(std::ostringstream& out, const char* label, const std::vector<T>& ids, size_t limit = 8)
{
    out << std::dec << "/" << label << "=";
    if (ids.empty())
    {
        out << "-";
        return;
    }

    const size_t count = std::min(ids.size(), limit);
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ",";
        out << ids[i];
    }
    if (ids.size() > count)
        out << ",more" << (ids.size() - count);
}

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
}

bool ContainsPathToken(const std::string& path, const char* token)
{
    return token && token[0] && path.find(token) != std::string::npos;
}

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

struct WriteAttrRecordHeader
{
    uint32_t type = 0;
    uint32_t nameIndex = 0;
};

CoopNativeSaveStoreApi::StoreHandle MakeReadStoreHandle(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    CoopNativeSaveStoreApi::StoreHandle handle;
    handle.store = node.storePtr;
    handle.nodeBlockBegin = node.nodeBlockBegin;
    handle.nodeBlockEnd = node.nodeBlockEnd;
    CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(handle.store + 0x20),
        handle.attrDataBase);
    handle.attrStringBase = node.attrStringBase;
    handle.attrNameOffsetTable = node.attrNameOffsetTable;
    handle.attrTokenContext = node.attrTokenContext;
    handle.attrTokenIndexBase = node.attrTokenIndexBase;
    handle.attrTokenBase = node.attrTokenBase;
    handle.childNameDataBase = node.childNameDataBase;
    handle.childNameResolverContext = node.childNameResolverContext;
    handle.childNameOffsetTable = node.childNameOffsetTable;
    handle.nodeCount = node.nodeCount;
    handle.readStore = node.readStore;
    handle.valid =
        node.valid &&
        node.readStore &&
        node.storePtr != 0 &&
        node.nodeBlockBegin != 0 &&
        node.nodeBlockEnd > node.nodeBlockBegin &&
        node.nodePtr >= node.nodeBlockBegin &&
        node.nodePtr + kReadStoreNodeBytes <= node.nodeBlockEnd;
    return handle;
}

CoopNativeSaveStoreApi::NodeView MakeReadNodeView(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    CoopNativeSaveStoreApi::NodeView view;
    view.nodePtr = node.nodePtr;
    view.nodeIndex = node.nodeIndex;
    view.nodeId = node.nodeId;
    view.childCursor = node.childCursor;
    view.childCount = node.childCount;
    view.attrCursor = node.attrCursor;
    view.attrCount = node.attrCount;
    view.childIndexBlockBegin = node.childIndexBlockBegin;
    view.childIndexBlockEnd = node.childIndexBlockEnd;
    view.valid = node.valid;
    return view;
}

bool CollectWriteStoreAttrNames(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    std::vector<std::string>& outNames)
{
    if (!node.valid || node.readStore || node.storePtr == 0 || node.attrVectorBegin == 0)
        return false;
    if (node.attrVectorEnd <= node.attrVectorBegin)
        return false;

    constexpr std::uintptr_t kWriteAttrRecordBytes = 0x28u;
    const std::uintptr_t byteCount = node.attrVectorEnd - node.attrVectorBegin;
    const uint32_t recordCount = static_cast<uint32_t>(byteCount / kWriteAttrRecordBytes);
    if (recordCount == 0)
        return false;

    const uint32_t count = node.attrCount != 0
        ? std::min<uint32_t>(node.attrCount, recordCount)
        : recordCount;
    const uint32_t limit = std::min<uint32_t>(count, 64u);
    for (uint32_t ordinal = 0; ordinal < limit; ++ordinal)
    {
        const std::uintptr_t recordPtr =
            node.attrVectorBegin + static_cast<std::uintptr_t>(ordinal) * kWriteAttrRecordBytes;
        WriteAttrRecordHeader header;
        if (!CoopRuntimeGuards::TryReadRuntimeValue(
                reinterpret_cast<const WriteAttrRecordHeader*>(recordPtr),
                header))
        {
            continue;
        }

        const char* rawName = nullptr;
        std::string reason;
        if (!CoopNativeSaveStoreApi::TryResolveWriteStoreName(node.storePtr, header.nameIndex, rawName, &reason) ||
            !rawName)
        {
            continue;
        }

        std::string name = CoopRuntimeGuards::ReadRuntimeCString(rawName, 160);
        if (!name.empty())
            outNames.push_back(std::move(name));
    }

    return !outNames.empty();
}

bool CollectReadStoreAttrNames(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    std::vector<std::string>& outNames)
{
    if (!node.valid || !node.readStore || node.attrCount == 0)
        return false;

    const CoopNativeSaveStoreApi::StoreHandle store = MakeReadStoreHandle(node);
    if (!store.valid)
        return false;

    std::vector<CoopNativeSaveStoreApi::ReadAttributeRecord> attrs;
    if (!CoopNativeSaveStoreApi::TryEnumerateReadAttributes(
            store,
            MakeReadNodeView(node),
            attrs,
            std::min<uint32_t>(std::max<uint32_t>(node.attrCount, 1u), 64u),
            nullptr))
    {
        return false;
    }

    for (const CoopNativeSaveStoreApi::ReadAttributeRecord& attr : attrs)
    {
        if (attr.nameResolved && !attr.name.empty())
            outNames.push_back(attr.name);
    }

    return !outNames.empty();
}

bool ContainsName(const std::vector<std::string>& names, const char* needle)
{
    return std::find(names.begin(), names.end(), needle) != names.end();
}

void CollectItemRootCandidateNames(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    std::vector<std::string>& outNames)
{
    if (node.readStore)
        CollectReadStoreAttrNames(node, outNames);
    else
        CollectWriteStoreAttrNames(node, outNames);
}

bool IsLikelyItemRootCandidate(const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    if (!node.valid)
        return false;

    std::vector<std::string> names;
    CollectItemRootCandidateNames(node, names);
    return
        ContainsName(names, "m_count") &&
        ContainsName(names, "selectedArchetype") &&
        ContainsName(names, "ownerId");
}

int ScoreItemRootCandidate(const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    if (!node.valid)
        return std::numeric_limits<int>::min();

    std::vector<std::string> names;
    CollectItemRootCandidateNames(node, names);

    int score = 0;
    score += node.attrCount > 0 ? static_cast<int>(std::min<uint32_t>(node.attrCount, 16u)) : -16;
    score += node.childCount > 0 ? static_cast<int>(std::min<uint32_t>(node.childCount, 8u)) : 0;

    const bool hasCount = ContainsName(names, "m_count");
    const bool hasArchetype = ContainsName(names, "selectedArchetype");
    const bool hasOwner = ContainsName(names, "ownerId");
    if (hasCount)
        score += 160;
    if (hasArchetype)
        score += 180;
    if (hasOwner)
        score += 140;
    if (hasCount && hasArchetype && hasOwner)
        score += 300;

    if (ContainsName(names, "m_aiTreeFilePath"))
        score -= 500;
    if (ContainsName(names, "flags2"))
        score -= 80;
    if (ContainsName(names, "pos"))
        score -= 80;
    if (ContainsName(names, "rot"))
        score -= 80;
    if (ContainsName(names, "trueCount"))
        score -= 80;
    if (ContainsName(names, "falseCount"))
        score -= 80;

    if (!names.empty() && !hasCount && !hasArchetype && !hasOwner)
        score -= 40;

    return score;
}

bool ShouldReplaceItemRootCandidate(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& existing,
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& candidate)
{
    if (!candidate.valid)
        return false;
    if (!existing.valid)
        return true;

    const int existingScore = ScoreItemRootCandidate(existing);
    const int candidateScore = ScoreItemRootCandidate(candidate);
    if (candidateScore != existingScore)
        return candidateScore > existingScore;

    if (candidate.readStore != existing.readStore)
        return candidate.readStore;
    if (candidate.attrCount != existing.attrCount)
        return candidate.attrCount > existing.attrCount;
    if (candidate.childCount != existing.childCount)
        return candidate.childCount > existing.childCount;
    return false;
}

std::string DescribeItemRootCandidate(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    if (!node.valid)
        return "invalid";

    std::vector<std::string> names;
    CollectItemRootCandidateNames(node, names);

    std::ostringstream out;
    out << "n" << node.nodeIndex
        << "s" << ScoreItemRootCandidate(node)
        << "a" << node.attrCount
        << "c" << node.childCount
        << (node.readStore ? "R" : "W");
    if (!names.empty())
    {
        out << "[";
        const size_t limit = std::min<size_t>(names.size(), 5);
        for (size_t i = 0; i < limit; ++i)
        {
            if (i > 0)
                out << ",";
            out << StatusToken(names[i]);
        }
        if (names.size() > limit)
            out << ",more" << (names.size() - limit);
        out << "]";
    }
    return out.str();
}

uint64_t Fnv1aAppend(uint64_t hash, uint64_t value)
{
    constexpr uint64_t kPrime = 1099511628211ull;
    for (int i = 0; i < 8; ++i)
    {
        const uint8_t byte = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
        hash ^= byte;
        hash *= kPrime;
    }
    return hash;
}

uint64_t HashNodeShape(uint64_t hash, const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    hash = Fnv1aAppend(hash, node.valid ? 1u : 0u);
    hash = Fnv1aAppend(hash, node.nodeId);
    hash = Fnv1aAppend(hash, node.attrCount);
    hash = Fnv1aAppend(hash, node.childCount);
    return hash;
}

uint64_t HashNodeContentRef(uint64_t hash, const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    hash = HashNodeShape(hash, node);
    hash = Fnv1aAppend(hash, node.nodeIndex);
    hash = Fnv1aAppend(hash, node.attrCursor);
    hash = Fnv1aAppend(hash, node.childCursor);
    return hash;
}

int32_t ReadStoreAttributeTypeSize(uint32_t type)
{
    // Mirrors PreyDll 0x1804D1AE0: table at 0x182249714, first uint32 per 0x18-byte entry.
    static constexpr std::array<int32_t, 34> kTypeSizes = {
        2, 4, 4, 12, 16, 8, -1, 2, 2, 1, 1, 1, 9, 5, 1, 13, 9, 5, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    if (type >= kTypeSizes.size())
        return -1;
    return kTypeSizes[type];
}

bool TryReadPointerAt(std::uintptr_t owner, size_t offset, std::uintptr_t& out)
{
    out = 0;
    if (owner == 0)
        return false;

    return CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(owner + offset),
        out);
}

bool TryReadU16At(std::uintptr_t address, uint16_t& out)
{
    out = 0;
    if (address == 0)
        return false;

    return CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const uint16_t*>(address),
        out);
}

bool TryReadU32At(std::uintptr_t address, uint32_t& out)
{
    out = 0;
    if (address == 0)
        return false;

    return CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const uint32_t*>(address),
        out);
}

bool TryBuildAttrDataRange(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    CoopNativeGameStateFragmentLocator::RawBackingRange& outRange)
{
    outRange = {};
    if (!node.valid || node.storePtr == 0 || node.attrCount == 0)
        return false;

    std::uintptr_t dataBase = 0;
    std::uintptr_t tokenIndexBase = 0;
    std::uintptr_t tokenBase = 0;
    if (!TryReadPointerAt(node.storePtr, 0x20, dataBase) ||
        !TryReadPointerAt(node.storePtr + 0xB0, 0x8, tokenIndexBase) ||
        !TryReadPointerAt(node.storePtr + 0xB0, 0x28, tokenBase) ||
        dataBase == 0 ||
        tokenIndexBase == 0 ||
        tokenBase == 0)
    {
        return false;
    }

    uint16_t nodeAttrTokenOffset = 0;
    if (!TryReadU16At(tokenIndexBase + static_cast<std::uintptr_t>(node.nodeId) * sizeof(uint16_t), nodeAttrTokenOffset))
        return false;

    uint32_t cursor = node.attrCursor;
    uint32_t maxCursor = cursor;
    bool sawDataBytes = false;
    constexpr uint32_t kMaxFragmentPoolSpan = 4u * 1024u * 1024u;

    for (uint32_t attrIndex = 0; attrIndex < node.attrCount; ++attrIndex)
    {
        uint16_t token = 0;
        const std::uintptr_t tokenAddress =
            tokenBase +
            static_cast<std::uintptr_t>(nodeAttrTokenOffset) +
            static_cast<std::uintptr_t>(attrIndex) * sizeof(uint16_t);
        if (!TryReadU16At(tokenAddress, token))
            return false;

        const uint32_t type = token & 0x3fu;
        uint32_t nextCursor = cursor;
        if (type == 6)
        {
            uint32_t stringBytes = 0;
            if (!TryReadU32At(dataBase + cursor, stringBytes))
                return false;
            if (stringBytes > kMaxFragmentPoolSpan || cursor > std::numeric_limits<uint32_t>::max() - stringBytes - 4u)
                return false;
            nextCursor = cursor + 4u + stringBytes;
        }
        else
        {
            const int32_t typeSize = ReadStoreAttributeTypeSize(type);
            if (typeSize < 0)
                return false;
            if (static_cast<uint32_t>(typeSize) > 0)
            {
                if (cursor > std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(typeSize))
                    return false;
                nextCursor = cursor + static_cast<uint32_t>(typeSize);
            }
        }

        if (nextCursor < cursor || nextCursor - node.attrCursor > kMaxFragmentPoolSpan)
            return false;
        if (nextCursor != cursor)
            sawDataBytes = true;

        maxCursor = std::max(maxCursor, nextCursor);
        cursor = nextCursor;
    }

    if (!sawDataBytes || maxCursor <= node.attrCursor)
        return false;

    outRange.kind = CoopNativeGameStateFragmentLocator::BackingRangeKind::AttrDataPool;
    outRange.beginPtr = dataBase + node.attrCursor;
    outRange.endPtr = dataBase + maxCursor;
    outRange.storePtr = node.storePtr;
    outRange.basePtr = dataBase;
    outRange.ownerNodeIndex = node.nodeIndex;
    outRange.ownerNodeId = node.nodeId;
    outRange.cursor = node.attrCursor;
    outRange.count = node.attrCount;
    outRange.valid = outRange.endPtr > outRange.beginPtr;
    return outRange.valid;
}

bool TryBuildChildIndexBlockRange(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    CoopNativeGameStateFragmentLocator::RawBackingRange& outRange)
{
    outRange = {};
    if (!node.valid ||
        node.childCount == 0 ||
        node.childIndexBlockBegin == 0 ||
        node.childIndexBlockEnd <= node.childIndexBlockBegin)
    {
        return false;
    }

    const std::uintptr_t byteCount = node.childIndexBlockEnd - node.childIndexBlockBegin;
    if ((byteCount % sizeof(uint32_t)) != 0 || byteCount > 1024u * 1024u)
        return false;

    outRange.kind = node.readStore
        ? CoopNativeGameStateFragmentLocator::BackingRangeKind::ChildIndexBlock
        : CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteChildVector;
    outRange.beginPtr = node.childIndexBlockBegin;
    outRange.endPtr = node.childIndexBlockEnd;
    outRange.storePtr = node.storePtr;
    outRange.basePtr = node.childIndexBlockBegin;
    outRange.ownerNodeIndex = node.nodeIndex;
    outRange.ownerNodeId = node.nodeId;
    outRange.cursor = node.childCursor;
    outRange.count = node.childCount;
    outRange.valid = true;
    return true;
}

bool TryBuildWriteAttrVectorRange(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    CoopNativeGameStateFragmentLocator::RawBackingRange& outRange)
{
    outRange = {};
    if (!node.valid ||
        node.readStore ||
        node.attrCount == 0 ||
        node.attrVectorBegin == 0 ||
        node.attrVectorEnd <= node.attrVectorBegin)
    {
        return false;
    }

    const std::uintptr_t byteCount = node.attrVectorEnd - node.attrVectorBegin;
    if ((byteCount % 0x28u) != 0 || byteCount > 1024u * 1024u)
        return false;

    outRange.kind = CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteAttrVector;
    outRange.beginPtr = node.attrVectorBegin;
    outRange.endPtr = node.attrVectorEnd;
    outRange.storePtr = node.storePtr;
    outRange.basePtr = node.attrVectorBegin;
    outRange.ownerNodeIndex = node.nodeIndex;
    outRange.ownerNodeId = node.nodeId;
    outRange.cursor = 0;
    outRange.count = node.attrCount;
    outRange.valid = true;
    return true;
}

bool TryBuildWriteChildVectorRange(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    CoopNativeGameStateFragmentLocator::RawBackingRange& outRange)
{
    outRange = {};
    if (!node.valid ||
        node.readStore ||
        node.childCount == 0 ||
        node.childVectorBegin == 0 ||
        node.childVectorEnd <= node.childVectorBegin)
    {
        return false;
    }

    const std::uintptr_t byteCount = node.childVectorEnd - node.childVectorBegin;
    if ((byteCount % sizeof(uint32_t)) != 0 || byteCount > 1024u * 1024u)
        return false;

    outRange.kind = CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteChildVector;
    outRange.beginPtr = node.childVectorBegin;
    outRange.endPtr = node.childVectorEnd;
    outRange.storePtr = node.storePtr;
    outRange.basePtr = node.childVectorBegin;
    outRange.ownerNodeIndex = node.nodeIndex;
    outRange.ownerNodeId = node.nodeId;
    outRange.cursor = 0;
    outRange.count = node.childCount;
    outRange.valid = true;
    return true;
}

void AddBackingRange(
    std::vector<CoopNativeGameStateFragmentLocator::RawBackingRange>& ranges,
    const CoopNativeGameStateFragmentLocator::RawBackingRange& range)
{
    if (!range.valid || range.beginPtr == 0 || range.endPtr <= range.beginPtr)
        return;

    const auto alreadyKnown = std::any_of(
        ranges.begin(),
        ranges.end(),
        [&range](const CoopNativeGameStateFragmentLocator::RawBackingRange& existing)
        {
            return existing.valid &&
                existing.kind == range.kind &&
                existing.storePtr == range.storePtr &&
                existing.basePtr == range.basePtr &&
                existing.beginPtr == range.beginPtr &&
                existing.endPtr == range.endPtr &&
                existing.ownerNodeIndex == range.ownerNodeIndex;
        });
    if (!alreadyKnown)
        ranges.push_back(range);
}

void AddWriteBackingRangesFromNodeRange(
    std::vector<CoopNativeGameStateFragmentLocator::RawBackingRange>& ranges,
    const CoopNativeGameStateFragmentLocator::RawNodeRange& nodeRange)
{
    if (!nodeRange.valid ||
        nodeRange.beginPtr == 0 ||
        nodeRange.endPtr <= nodeRange.beginPtr ||
        nodeRange.count == 0)
    {
        return;
    }

    const std::uintptr_t byteCount = nodeRange.endPtr - nodeRange.beginPtr;
    if (byteCount != static_cast<std::uintptr_t>(nodeRange.count) * kWriteStoreNodeBytes)
        return;

    for (uint32_t i = 0; i < nodeRange.count; ++i)
    {
        const std::uintptr_t nodePtr =
            nodeRange.beginPtr + static_cast<std::uintptr_t>(i) * kWriteStoreNodeBytes;
        uint32_t nodeId = 0;
        uint32_t childCount = 0;
        uint8_t valid = 0;
        std::uintptr_t attrBegin = 0;
        std::uintptr_t attrEnd = 0;
        std::uintptr_t childBegin = 0;
        std::uintptr_t childEnd = 0;
        if (!TryReadU32At(nodePtr + 0x20, nodeId) ||
            !TryReadU32At(nodePtr + 0x44, childCount) ||
            !CoopRuntimeGuards::TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(nodePtr + 0x41), valid) ||
            !TryReadPointerAt(nodePtr, 0x50, attrBegin) ||
            !TryReadPointerAt(nodePtr, 0x58, attrEnd) ||
            !TryReadPointerAt(nodePtr, 0x68, childBegin) ||
            !TryReadPointerAt(nodePtr, 0x70, childEnd) ||
            valid == 0)
        {
            continue;
        }

        const uint32_t ownerNodeIndex = nodeRange.beginIndex + i;
        if (attrBegin != 0 && attrEnd > attrBegin)
        {
            const std::uintptr_t attrBytes = attrEnd - attrBegin;
            if ((attrBytes % 0x28u) == 0 && attrBytes <= 1024u * 1024u)
            {
                CoopNativeGameStateFragmentLocator::RawBackingRange attrRange;
                attrRange.kind = CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteAttrVector;
                attrRange.beginPtr = attrBegin;
                attrRange.endPtr = attrEnd;
                attrRange.storePtr = nodeRange.storePtr;
                attrRange.basePtr = attrBegin;
                attrRange.ownerNodeIndex = ownerNodeIndex;
                attrRange.ownerNodeId = nodeId;
                attrRange.cursor = 0;
                attrRange.count = static_cast<uint32_t>(attrBytes / 0x28u);
                attrRange.valid = true;
                AddBackingRange(ranges, attrRange);
            }
        }

        if (childBegin != 0 && childEnd > childBegin)
        {
            const std::uintptr_t childBytes = childEnd - childBegin;
            if ((childBytes % sizeof(uint32_t)) == 0 && childBytes <= 1024u * 1024u)
            {
                CoopNativeGameStateFragmentLocator::RawBackingRange childRange;
                childRange.kind = CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteChildVector;
                childRange.beginPtr = childBegin;
                childRange.endPtr = childEnd;
                childRange.storePtr = nodeRange.storePtr;
                childRange.basePtr = childBegin;
                childRange.ownerNodeIndex = ownerNodeIndex;
                childRange.ownerNodeId = nodeId;
                childRange.cursor = 0;
                childRange.count = childCount != 0
                    ? childCount
                    : static_cast<uint32_t>(childBytes / sizeof(uint32_t));
                childRange.valid = true;
                AddBackingRange(ranges, childRange);
            }
        }
    }
}
}

void CoopNativeGameStateFragmentLocator::Reset()
{
    *this = CoopNativeGameStateFragmentLocator();
}

void CoopNativeGameStateFragmentLocator::BeginRun(uint64_t runId, const char* reason)
{
    Reset();
    m_runId = runId;
    SetLastEvent(std::string("begin run=") + std::to_string(runId) + " reason=" + (reason && reason[0] ? reason : "-"));
}

void CoopNativeGameStateFragmentLocator::OnGameStateSerializer(
    const void* serializerPtr,
    const std::string& sectionName,
    bool reading,
    int target)
{
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(serializerPtr);
    if (key != 0 && key != m_lastSerializerPtr)
    {
        m_lastSerializerPtr = key;
        ++m_gameStateSerializers;
    }

    std::ostringstream out;
    out << "serializer ptr=" << key
        << " section=" << (sectionName.empty() ? "-" : sectionName)
        << " reading=" << BoolText(reading)
        << " target=" << target;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnLocalPlayerInventoryScopeEnter(const NativeInventoryFragmentScopeInfo& info)
{
    ++m_inventoryScopes;
    m_activeInventoryScopeSeq = info.scopeSeq;
    OnGameStateSerializer(reinterpret_cast<const void*>(info.serializerPtr), info.sectionName, info.reading, info.target);

    std::ostringstream out;
    out << "inventory enter scope=" << info.scopeSeq
        << " inv=" << info.inventoryPtr
        << " ser=" << info.serializerPtr
        << " section=" << (info.sectionName.empty() ? "-" : info.sectionName)
        << " reading=" << BoolText(info.reading);
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnLocalPlayerInventoryScopeExit(uint64_t scopeSeq)
{
    if (m_activeInventoryScopeSeq == scopeSeq)
        m_activeInventoryScopeSeq = 0;

    std::ostringstream out;
    out << "inventory exit scope=" << scopeSeq;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnPlayerInventoryItemScopeEnter(const NativeItemFragmentScopeInfo& info)
{
    ++m_itemScopes;
    if (info.itemEntityId != 0)
    {
        m_itemScopeEntityIds.insert(info.itemEntityId);
        RecomputeReferenceCompleteness();
    }
    m_activeItemScopeSeq = info.scopeSeq;
    m_activeItemEntityId = info.itemEntityId;
    OnGameStateSerializer(reinterpret_cast<const void*>(info.serializerPtr), info.sectionName, info.reading, info.target);

    std::ostringstream out;
    out << "item enter scope=" << info.scopeSeq
        << " invScope=" << info.inventoryScopeSeq
        << " item=" << info.itemPtr
        << " entity=" << info.itemEntityId
        << " refs=" << m_matchedItemEntityScopes << "/" << m_distinctInventoryEntityIds
        << " ser=" << info.serializerPtr
        << " section=" << (info.sectionName.empty() ? "-" : info.sectionName)
        << " reading=" << BoolText(info.reading);
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnPlayerInventoryItemScopeExit(uint64_t scopeSeq)
{
    if (m_activeItemScopeSeq == scopeSeq)
    {
        m_activeItemScopeSeq = 0;
        m_activeItemEntityId = 0;
    }

    std::ostringstream out;
    out << "item exit scope=" << scopeSeq;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnDecodedStoreNode(const NativeDecodedStoreNodeInfo& info)
{
    if (!info.nodeReadable)
    {
        ++m_missingReferences;
        SetLastEvent(
            "decoded unreadable source=" + StatusToken(info.source) +
            " path=" + StatusToken(info.path));
        return;
    }

    const bool storedItemsValuePath =
        ContainsPathToken(info.path, "Inventory/storedItems/i/v") &&
        info.attrCount == 5;
    const bool storedItemsLeafPath =
        ContainsPathToken(info.path, "Inventory/storedItems/i/v/entityId") ||
        ContainsPathToken(info.path, "Inventory/storedItems/i/v/x") ||
        ContainsPathToken(info.path, "Inventory/storedItems/i/v/y") ||
        ContainsPathToken(info.path, "Inventory/storedItems/i/v/width") ||
        ContainsPathToken(info.path, "Inventory/storedItems/i/v/height");

    const bool localPlayerInventorySource =
        info.source.find("ArkInventory::FullSerialize(local-player)") != std::string::npos;
    const bool inventoryNode = info.inventoryNode || storedItemsValuePath || storedItemsLeafPath;
    const bool itemNode = info.itemNode;
    const FragmentNodeRef nodeRef = MakeNodeRef(info);

    if (inventoryNode)
        ++m_inventoryNodes;
    if (itemNode)
        ++m_itemNodes;
    if (storedItemsValuePath || storedItemsLeafPath)
        ++m_inventoryCellValueNodes;
    if (itemNode && info.attrCount > 0)
        ++m_itemValueNodes;

    if (storedItemsValuePath &&
        (IsCurrentInventoryScope() || localPlayerInventorySource) &&
        info.nodeReadable &&
        info.nodeValid)
    {
        m_inventoryCellValueNode = nodeRef;
    }

    if (inventoryNode && (IsCurrentInventoryScope() || localPlayerInventorySource))
        AddObservedNode(m_inventoryObservedNodes, nodeRef);

    if (itemNode && IsCurrentItemScope() && info.nodeReadable && info.nodeValid)
    {
        if (m_activeItemEntityId != 0)
            AddItemNodeForEntity(m_activeItemEntityId, nodeRef);
        else if (m_activeItemScopeSeq != 0)
            AddAnonymousItemNode(m_activeItemScopeSeq, nodeRef);
    }

    const uint32_t referenceCount =
        info.attrCount +
        info.childCount +
        (info.nodeBlockBegin != 0 ? 1u : 0u) +
        (info.storePtr != 0 ? 1u : 0u);
    m_references += referenceCount;
    if ((inventoryNode || itemNode) && referenceCount == 0)
        ++m_missingReferences;

    m_lastNodeIndex = info.nodeIndex;
    m_lastNodeId = info.nodeId;
    m_lastAttrCount = info.attrCount;
    m_lastChildCount = info.childCount;
    m_schemaOk =
        (m_inventoryScopes > 0 &&
            m_inventoryCellValueNodes > 0 &&
            m_missingReferences == 0 &&
            m_distinctInventoryEntityIds > 0 &&
            m_missingItemEntityScopes == 0) ? 1u : 0u;

    std::ostringstream out;
    out << "decoded source=" << StatusToken(info.source)
        << " op=" << StatusToken(info.op)
        << " path=" << StatusToken(info.path)
        << " idx=" << info.nodeIndex
        << " nodeId=" << info.nodeId
        << " attrs=" << info.attrCount
        << " children=" << info.childCount
        << " inv=" << BoolText(inventoryNode)
        << " item=" << BoolText(itemNode)
        << " cell=" << BoolText(storedItemsValuePath || storedItemsLeafPath);
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnInventoryCellEntityId(
    unsigned entityId,
    const std::string& source,
    const std::string& path)
{
    if (entityId == 0)
        return;

    ++m_inventoryCellEntityIds;
    const auto [_, inserted] = m_inventoryEntityIds.insert(entityId);
    if (inserted)
        m_inventoryEntityOrder.push_back(entityId);
    RecomputeReferenceCompleteness();

    std::ostringstream out;
    out << "inventory cell entity source=" << StatusToken(source)
        << " path=" << StatusToken(path)
        << " entity=" << entityId
        << " refs=" << m_matchedItemEntityScopes << "/" << m_distinctInventoryEntityIds
        << " missingItems=" << m_missingItemEntityScopes;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnReadStoreItemNodeForEntity(
    unsigned entityId,
    const NativeDecodedStoreNodeInfo& info)
{
    if (entityId == 0)
        return;
    if (!info.nodeReadable || !info.nodeValid)
    {
        ++m_missingReferences;
        SetLastEvent(
            "read-store item skipped unreadable entity=" + std::to_string(entityId) +
            " source=" + StatusToken(info.source) +
            " path=" + StatusToken(info.path));
        return;
    }

    NativeDecodedStoreNodeInfo itemInfo = info;
    itemInfo.itemNode = true;
    itemInfo.inventoryNode = false;
    const FragmentNodeRef nodeRef = MakeNodeRef(itemInfo);
    if (!nodeRef.valid)
    {
        ++m_missingReferences;
        SetLastEvent(
            "read-store item skipped invalid entity=" + std::to_string(entityId) +
            " source=" + StatusToken(info.source) +
            " path=" + StatusToken(info.path));
        return;
    }

    ++m_itemNodes;
    if (itemInfo.attrCount > 0)
        ++m_itemValueNodes;

    AddItemNodeForEntity(entityId, nodeRef);

    const uint32_t referenceCount =
        itemInfo.attrCount +
        itemInfo.childCount +
        (itemInfo.nodeBlockBegin != 0 ? 1u : 0u) +
        (itemInfo.storePtr != 0 ? 1u : 0u);
    m_references += referenceCount;
    if (referenceCount == 0)
        ++m_missingReferences;

    m_lastNodeIndex = itemInfo.nodeIndex;
    m_lastNodeId = itemInfo.nodeId;
    m_lastAttrCount = itemInfo.attrCount;
    m_lastChildCount = itemInfo.childCount;

    std::ostringstream out;
    out << "read-store item entity=" << entityId
        << " source=" << StatusToken(itemInfo.source)
        << " path=" << StatusToken(itemInfo.path)
        << " idx=" << itemInfo.nodeIndex
        << " nodeId=" << itemInfo.nodeId
        << " attrs=" << itemInfo.attrCount
        << " children=" << itemInfo.childCount
        << " refs=" << m_matchedItemEntityScopes << "/" << m_distinctInventoryEntityIds
        << " missingItems=" << m_missingItemEntityScopes;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::OnReadStoreItemObservedNodeForEntity(
    unsigned entityId,
    const NativeDecodedStoreNodeInfo& info)
{
    if (entityId == 0)
        return;
    if (!info.nodeReadable || !info.nodeValid)
        return;

    NativeDecodedStoreNodeInfo itemInfo = info;
    itemInfo.itemNode = true;
    itemInfo.inventoryNode = false;
    const FragmentNodeRef nodeRef = MakeNodeRef(itemInfo);
    if (!nodeRef.valid)
        return;

    AddObservedNode(m_itemObservedNodesByEntityId[entityId], nodeRef);

    const uint32_t referenceCount =
        itemInfo.attrCount +
        itemInfo.childCount +
        (itemInfo.nodeBlockBegin != 0 ? 1u : 0u) +
        (itemInfo.storePtr != 0 ? 1u : 0u);
    m_references += referenceCount;

    m_lastNodeIndex = itemInfo.nodeIndex;
    m_lastNodeId = itemInfo.nodeId;
    m_lastAttrCount = itemInfo.attrCount;
    m_lastChildCount = itemInfo.childCount;

    std::ostringstream out;
    out << "read-store item observed entity=" << entityId
        << " source=" << StatusToken(itemInfo.source)
        << " path=" << StatusToken(itemInfo.path)
        << " idx=" << itemInfo.nodeIndex
        << " nodeId=" << itemInfo.nodeId
        << " attrs=" << itemInfo.attrCount
        << " children=" << itemInfo.childCount;
    SetLastEvent(out.str());
}

void CoopNativeGameStateFragmentLocator::MergeReadStoreBundle(
    const NativeInventoryFragmentBundle& bundle,
    const char* reason)
{
    if (m_runId == 0 && bundle.runId != 0)
        m_runId = bundle.runId;

    if (bundle.inventoryCellValueNode.valid)
    {
        m_inventoryCellValueNode = bundle.inventoryCellValueNode;
        ++m_inventoryCellValueNodes;
    }

    for (const unsigned entityId : bundle.inventoryEntityIds)
    {
        if (entityId == 0)
            continue;

        const auto [_, inserted] = m_inventoryEntityIds.insert(entityId);
        if (inserted)
            m_inventoryEntityOrder.push_back(entityId);
        ++m_inventoryCellEntityIds;
    }

    for (const FragmentNodeRef& node : bundle.inventoryObservedNodes)
        AddObservedNode(m_inventoryObservedNodes, node);

    for (const ItemFragmentRef& item : bundle.itemFragments)
    {
        if (item.entityId == 0 || !item.node.valid)
            continue;

        AddItemNodeForEntity(item.entityId, item.node);
        for (const FragmentNodeRef& node : item.observedNodes)
            AddObservedNode(m_itemObservedNodesByEntityId[item.entityId], node);
    }

    RecomputeReferenceCompleteness();
    SetLastEvent(
        std::string("merge read-store bundle reason=") +
        (reason && reason[0] ? reason : "-") +
        " status=" + BuildFragmentBundleStatus());
}

void CoopNativeGameStateFragmentLocator::FinalizeRun(const char* reason)
{
    RecomputeReferenceCompleteness();
    m_schemaOk =
        (m_inventoryScopes > 0 &&
            m_inventoryCellValueNodes > 0 &&
            m_missingReferences == 0 &&
            m_distinctInventoryEntityIds > 0 &&
            m_missingItemEntityScopes == 0) ? 1u : 0u;
    SetLastEvent(std::string("finalize reason=") + (reason && reason[0] ? reason : "-") + " status=" + BuildStatus());
}

std::string CoopNativeGameStateFragmentLocator::BuildStatus() const
{
    std::ostringstream out;
    out << m_gameStateSerializers << "/"
        << m_inventoryScopes << "/"
        << m_itemScopes << "/"
        << m_inventoryNodes << "/"
        << m_itemNodes << "/"
        << m_inventoryCellValueNodes << "/"
        << m_itemValueNodes << "/"
        << m_references << "/"
        << m_missingReferences << "/"
        << m_inventoryCellEntityIds << "/"
        << m_distinctInventoryEntityIds << "/"
        << m_matchedItemEntityScopes << "/"
        << m_missingItemEntityScopes << "/"
        << m_schemaOk << "/"
        << m_lastNodeIndex << "/"
        << m_lastNodeId << "/"
        << m_lastAttrCount << "/"
        << m_lastChildCount;
    return out.str();
}

std::string CoopNativeGameStateFragmentLocator::BuildFragmentMapStatus() const
{
    auto ptrHex = [](std::uintptr_t value)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::uppercase << value;
        return out.str();
    };

    std::ostringstream out;
    out << "inv=" << (m_inventoryCellValueNode.valid ? 1 : 0);
    if (m_inventoryCellValueNode.valid)
    {
        out << ":idx=" << m_inventoryCellValueNode.nodeIndex
            << ":id=" << m_inventoryCellValueNode.nodeId
            << ":attrs=" << m_inventoryCellValueNode.attrCursor << "+" << m_inventoryCellValueNode.attrCount
            << ":children=" << m_inventoryCellValueNode.childCursor << "+" << m_inventoryCellValueNode.childCount
            << ":store=" << ptrHex(m_inventoryCellValueNode.storePtr)
            << ":nodes=" << m_inventoryCellValueNode.nodeCount;
    }

    const std::vector<ItemFragmentRef> resolvedItems = BuildResolvedItemFragments();
    out << "/items=" << resolvedItems.size() << "/" << m_distinctInventoryEntityIds;
    if (!resolvedItems.empty())
    {
        out << ":ids=";
        const size_t limit = std::min<size_t>(resolvedItems.size(), 8);
        for (size_t i = 0; i < limit; ++i)
        {
            if (i > 0)
                out << ",";
            out << resolvedItems[i].entityId;
            if (resolvedItems[i].node.valid)
                out << "@n" << resolvedItems[i].node.nodeIndex;
        }
        if (resolvedItems.size() > limit)
            out << ",more" << (resolvedItems.size() - limit);
    }

    if (!resolvedItems.empty())
    {
        out << "/rootScores=";
        const size_t limit = std::min<size_t>(resolvedItems.size(), 4);
        for (size_t i = 0; i < limit; ++i)
        {
            if (i > 0)
                out << ";";
            out << resolvedItems[i].entityId << "@";
            const auto summaryIt = m_itemRootSummariesByEntityId.find(resolvedItems[i].entityId);
            if (summaryIt != m_itemRootSummariesByEntityId.end())
                out << summaryIt->second;
            else
                out << DescribeItemRootCandidate(resolvedItems[i].node);
        }
        if (resolvedItems.size() > limit)
            out << ";more" << (resolvedItems.size() - limit);
    }

    if (!m_itemObservedSummariesByEntityId.empty())
    {
        out << "/obsScoreSample=";
        size_t emittedItems = 0;
        for (unsigned entityId : m_itemNodeEntityOrder)
        {
            const auto observedIt = m_itemObservedSummariesByEntityId.find(entityId);
            if (observedIt == m_itemObservedSummariesByEntityId.end() || observedIt->second.empty())
                continue;
            if (emittedItems > 0)
                out << ";";
            out << entityId << ":";
            const size_t nodeLimit = std::min<size_t>(observedIt->second.size(), 4);
            for (size_t nodeIndex = 0; nodeIndex < nodeLimit; ++nodeIndex)
            {
                if (nodeIndex > 0)
                    out << ",";
                out << observedIt->second[nodeIndex];
            }
            if (observedIt->second.size() > nodeLimit)
                out << ",more" << (observedIt->second.size() - nodeLimit);
            if (++emittedItems >= 4)
                break;
        }
    }
    AppendIdList(out, "cellIds", m_inventoryEntityOrder);
    AppendIdList(out, "knownIds", m_itemNodeEntityOrder);

    const std::vector<RawNodeRange> invRanges = BuildContiguousNodeRanges(m_inventoryObservedNodes);
    size_t observedItemNodes = 0;
    size_t observedItemRanges = 0;
    for (const ItemFragmentRef& item : resolvedItems)
    {
        observedItemNodes += item.observedNodes.size();
        observedItemRanges += item.observedNodeRanges.size();
    }
    out << "/obsInv=" << m_inventoryObservedNodes.size() << ":r" << invRanges.size()
        << "/obsItems=" << observedItemNodes << ":r" << observedItemRanges;

    out << "/ready="
        << (m_inventoryCellValueNode.valid &&
                m_distinctInventoryEntityIds > 0 &&
                resolvedItems.size() >= m_distinctInventoryEntityIds
            ? 1
            : 0);
    return out.str();
}

CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle
CoopNativeGameStateFragmentLocator::BuildFragmentBundle() const
{
    NativeInventoryFragmentBundle bundle;
    bundle.runId = m_runId;
    bundle.inventoryCellValueNode = m_inventoryCellValueNode;
    bundle.inventoryCellValueRange = MakeSingleNodeRange(m_inventoryCellValueNode);
    bundle.inventoryObservedNodes = m_inventoryObservedNodes;
    bundle.inventoryObservedNodeRanges = BuildContiguousNodeRanges(m_inventoryObservedNodes);
    bundle.inventoryEntityCount = m_distinctInventoryEntityIds;
    bundle.missingItemReferences = m_missingItemEntityScopes;
    bundle.observedInventoryNodeCount = static_cast<uint32_t>(bundle.inventoryObservedNodes.size());
    bundle.observedInventoryRangeCount = static_cast<uint32_t>(bundle.inventoryObservedNodeRanges.size());
    bundle.inventoryEntityIds = m_inventoryEntityOrder;
    bundle.knownItemEntityIds = m_itemNodeEntityOrder;

    bundle.itemFragments = BuildResolvedItemFragments();
    std::unordered_set<unsigned> resolvedItemEntityIds;
    for (const ItemFragmentRef& item : bundle.itemFragments)
    {
        bundle.resolvedItemEntityIds.push_back(item.entityId);
        resolvedItemEntityIds.insert(item.entityId);
        bundle.observedItemNodeCount += static_cast<uint32_t>(item.observedNodes.size());
        bundle.observedItemRangeCount += static_cast<uint32_t>(item.observedNodeRanges.size());
    }
    for (unsigned entityId : m_inventoryEntityOrder)
    {
        if (resolvedItemEntityIds.find(entityId) == resolvedItemEntityIds.end())
            bundle.missingItemEntityIds.push_back(entityId);
    }

    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    uint64_t schemaHash = kFnvOffset;
    schemaHash = HashNodeShape(schemaHash, bundle.inventoryCellValueNode);
    schemaHash = Fnv1aAppend(schemaHash, bundle.inventoryEntityCount);
    schemaHash = Fnv1aAppend(schemaHash, static_cast<uint64_t>(bundle.itemFragments.size()));
    schemaHash = Fnv1aAppend(schemaHash, bundle.observedInventoryNodeCount);
    schemaHash = Fnv1aAppend(schemaHash, bundle.observedInventoryRangeCount);
    for (const ItemFragmentRef& item : bundle.itemFragments)
    {
        schemaHash = HashNodeShape(schemaHash, item.node);
        schemaHash = Fnv1aAppend(schemaHash, static_cast<uint64_t>(item.observedNodes.size()));
        schemaHash = Fnv1aAppend(schemaHash, static_cast<uint64_t>(item.observedNodeRanges.size()));
    }
    bundle.schemaHash = schemaHash;

    uint64_t contentHash = kFnvOffset;
    contentHash = HashNodeContentRef(contentHash, bundle.inventoryCellValueNode);
    for (const FragmentNodeRef& node : bundle.inventoryObservedNodes)
        contentHash = HashNodeContentRef(contentHash, node);
    for (const ItemFragmentRef& item : bundle.itemFragments)
    {
        contentHash = Fnv1aAppend(contentHash, item.entityId);
        contentHash = HashNodeContentRef(contentHash, item.node);
        for (const FragmentNodeRef& node : item.observedNodes)
            contentHash = HashNodeContentRef(contentHash, node);
    }
    bundle.contentHash = contentHash;

    if (!bundle.inventoryCellValueNode.valid)
        bundle.reason = "missing_inventory_cell_value_node";
    else if (bundle.inventoryEntityCount == 0)
        bundle.reason = "missing_inventory_entity_refs";
    else if (bundle.missingItemReferences != 0)
        bundle.reason = "missing_item_subtrees";
    else if (bundle.itemFragments.size() < bundle.inventoryEntityCount)
        bundle.reason = "incomplete_item_fragment_map";
    else
    {
        bundle.ok = true;
        bundle.reason = "ok";
    }

    return bundle;
}

std::string CoopNativeGameStateFragmentLocator::BuildFragmentBundleStatus() const
{
    const NativeInventoryFragmentBundle bundle = BuildFragmentBundle();
    std::ostringstream out;
    out << (bundle.ok ? 1 : 0)
        << "/" << StatusToken(bundle.reason)
        << "/run=" << bundle.runId
        << "/inv=" << (bundle.inventoryCellValueNode.valid ? 1 : 0)
        << "/items=" << bundle.itemFragments.size() << "/" << bundle.inventoryEntityCount
        << "/obsInv=" << bundle.observedInventoryNodeCount << ":r" << bundle.observedInventoryRangeCount
        << "/obsItems=" << bundle.observedItemNodeCount << ":r" << bundle.observedItemRangeCount
        << "/missing=" << bundle.missingItemReferences
        << "/schema=0x" << std::hex << std::uppercase << bundle.schemaHash
        << "/content=0x" << bundle.contentHash;
    AppendIdList(out, "cellIds", bundle.inventoryEntityIds);
    AppendIdList(out, "resolvedIds", bundle.resolvedItemEntityIds);
    AppendIdList(out, "missingIds", bundle.missingItemEntityIds);
    AppendIdList(out, "knownIds", bundle.knownItemEntityIds);
    return out.str();
}

CoopNativeGameStateFragmentLocator::NativeInventoryFragmentByteCapture
CoopNativeGameStateFragmentLocator::CaptureFragmentBytes(uint32_t maxBytes) const
{
    NativeInventoryFragmentByteCapture capture;
    capture.bundle = BuildFragmentBundle();
    if (!capture.bundle.ok)
    {
        capture.reason = "bundle_" + capture.bundle.reason;
        return capture;
    }

    for (const RawNodeRange& range : capture.bundle.inventoryObservedNodeRanges)
    {
        RawRangeByteCopy copy;
        if (CaptureRangeBytes(range, capture.bytes, maxBytes, copy))
            ++capture.capturedRanges;
        else
            ++capture.failedRanges;
        capture.inventoryRanges.push_back(std::move(copy));
    }

    std::vector<RawBackingRange> inventoryBackingRanges = BuildBackingRanges(capture.bundle.inventoryObservedNodes);
    for (const RawNodeRange& range : capture.bundle.inventoryObservedNodeRanges)
        AddWriteBackingRangesFromNodeRange(inventoryBackingRanges, range);
    for (const RawBackingRange& range : inventoryBackingRanges)
    {
        RawBackingRangeByteCopy copy;
        if (CaptureBackingRangeBytes(range, capture.bytes, maxBytes, copy))
            ++capture.capturedBackingRanges;
        else
            ++capture.failedBackingRanges;
        capture.inventoryBackingRanges.push_back(std::move(copy));
    }

    capture.itemRanges.reserve(capture.bundle.itemFragments.size());
    for (const ItemFragmentRef& item : capture.bundle.itemFragments)
    {
        ItemFragmentByteCopy itemCopy;
        itemCopy.entityId = item.entityId;

        const FragmentByteSnapshot* snapshot = nullptr;
        if (item.sourceEntityId != 0)
        {
            const auto snapshotIt = m_itemByteSnapshotsByEntityId.find(item.sourceEntityId);
            if (snapshotIt != m_itemByteSnapshotsByEntityId.end())
                snapshot = &snapshotIt->second;
        }
        if (!snapshot && item.entityId != 0)
        {
            const auto snapshotIt = m_itemByteSnapshotsByEntityId.find(item.entityId);
            if (snapshotIt != m_itemByteSnapshotsByEntityId.end())
                snapshot = &snapshotIt->second;
        }
        if (!snapshot && item.sourceScopeSeq != 0)
        {
            const auto snapshotIt = m_anonymousItemByteSnapshotsByScopeSeq.find(item.sourceScopeSeq);
            if (snapshotIt != m_anonymousItemByteSnapshotsByScopeSeq.end())
                snapshot = &snapshotIt->second;
        }
        if (snapshot && snapshot->ok && !snapshot->bytes.empty())
        {
            if (capture.bytes.size() + snapshot->bytes.size() > maxBytes)
            {
                RawRangeByteCopy failedCopy;
                failedCopy.reason = "snapshot_capture_too_large";
                itemCopy.ranges.push_back(std::move(failedCopy));
                ++capture.failedRanges;
            }
            else
            {
                const uint32_t baseOffset = static_cast<uint32_t>(capture.bytes.size());
                capture.bytes.insert(capture.bytes.end(), snapshot->bytes.begin(), snapshot->bytes.end());

                itemCopy.ranges.reserve(snapshot->ranges.size());
                for (RawRangeByteCopy copy : snapshot->ranges)
                {
                    if (copy.ok)
                    {
                        copy.byteOffset += baseOffset;
                        ++capture.capturedRanges;
                    }
                    else
                    {
                        ++capture.failedRanges;
                    }
                    itemCopy.ranges.push_back(std::move(copy));
                }

                itemCopy.backingRanges.reserve(snapshot->backingRanges.size());
                for (RawBackingRangeByteCopy copy : snapshot->backingRanges)
                {
                    if (copy.ok)
                    {
                        copy.byteOffset += baseOffset;
                        ++capture.capturedBackingRanges;
                    }
                    else
                    {
                        ++capture.failedBackingRanges;
                    }
                    itemCopy.backingRanges.push_back(std::move(copy));
                }

                capture.itemRanges.push_back(std::move(itemCopy));
                continue;
            }
        }

        itemCopy.ranges.reserve(item.observedNodeRanges.size());
        for (const RawNodeRange& range : item.observedNodeRanges)
        {
            RawRangeByteCopy copy;
            if (CaptureRangeBytes(range, capture.bytes, maxBytes, copy))
                ++capture.capturedRanges;
            else
                ++capture.failedRanges;
            itemCopy.ranges.push_back(std::move(copy));
        }
        std::vector<RawBackingRange> itemBackingRanges = BuildBackingRanges(item.observedNodes);
        for (const RawNodeRange& range : item.observedNodeRanges)
            AddWriteBackingRangesFromNodeRange(itemBackingRanges, range);
        for (const RawBackingRange& range : itemBackingRanges)
        {
            RawBackingRangeByteCopy copy;
            if (CaptureBackingRangeBytes(range, capture.bytes, maxBytes, copy))
                ++capture.capturedBackingRanges;
            else
                ++capture.failedBackingRanges;
            itemCopy.backingRanges.push_back(std::move(copy));
        }
        capture.itemRanges.push_back(std::move(itemCopy));
    }

    if (capture.failedRanges != 0)
        capture.reason = "range_capture_failed";
    else if (capture.failedBackingRanges != 0)
        capture.reason = "backing_range_capture_failed";
    else if (capture.capturedRanges == 0)
        capture.reason = "no_ranges";
    else
    {
        capture.ok = true;
        capture.reason = "ok";
    }

    return capture;
}

std::string CoopNativeGameStateFragmentLocator::BuildFragmentByteCaptureStatus(uint32_t maxBytes) const
{
    const NativeInventoryFragmentByteCapture capture = CaptureFragmentBytes(maxBytes);
    std::ostringstream out;
    out << (capture.ok ? 1 : 0)
        << "/" << StatusToken(capture.reason)
        << "/ranges=" << capture.capturedRanges << "+" << capture.failedRanges
        << "/backing=" << capture.capturedBackingRanges << "+" << capture.failedBackingRanges
        << "/bytes=" << capture.bytes.size()
        << "/invRanges=" << capture.inventoryRanges.size()
        << "/invBacking=" << capture.inventoryBackingRanges.size()
        << "/itemGroups=" << capture.itemRanges.size()
        << "/schema=0x" << std::hex << std::uppercase << capture.bundle.schemaHash
        << "/content=0x" << capture.bundle.contentHash;
    if (!capture.ok)
    {
        for (const RawRangeByteCopy& range : capture.inventoryRanges)
        {
            if (!range.ok && !range.reason.empty())
            {
                out << "/fail=" << StatusToken(range.reason);
                return out.str();
            }
        }
        for (const RawBackingRangeByteCopy& range : capture.inventoryBackingRanges)
        {
            if (!range.ok && !range.reason.empty())
            {
                out << "/failBacking=" << StatusToken(range.reason);
                return out.str();
            }
        }
        for (const ItemFragmentByteCopy& item : capture.itemRanges)
        {
            for (const RawRangeByteCopy& range : item.ranges)
            {
                if (!range.ok && !range.reason.empty())
                {
                    out << "/failItem=" << item.entityId << ":" << StatusToken(range.reason);
                    return out.str();
                }
            }
            for (const RawBackingRangeByteCopy& range : item.backingRanges)
            {
                if (!range.ok && !range.reason.empty())
                {
                    out << "/failItemBacking=" << item.entityId << ":" << StatusToken(range.reason);
                    return out.str();
                }
            }
        }
    }
    return out.str();
}

void CoopNativeGameStateFragmentLocator::SetLastEvent(const std::string& event)
{
    m_lastEvent = event.empty() ? "-" : event;
}

void CoopNativeGameStateFragmentLocator::RecomputeReferenceCompleteness()
{
    m_distinctInventoryEntityIds = static_cast<uint32_t>(m_inventoryEntityIds.size());
    const uint32_t resolvedItems = static_cast<uint32_t>(BuildResolvedItemFragments().size());
    m_matchedItemEntityScopes = std::min(m_distinctInventoryEntityIds, resolvedItems);
    m_missingItemEntityScopes =
        m_distinctInventoryEntityIds > m_matchedItemEntityScopes
            ? m_distinctInventoryEntityIds - m_matchedItemEntityScopes
            : 0;

    m_schemaOk =
        (m_inventoryScopes > 0 &&
            m_inventoryCellValueNodes > 0 &&
            m_missingReferences == 0 &&
            m_distinctInventoryEntityIds > 0 &&
            m_missingItemEntityScopes == 0) ? 1u : 0u;
}

CoopNativeGameStateFragmentLocator::FragmentNodeRef CoopNativeGameStateFragmentLocator::MakeNodeRef(
    const NativeDecodedStoreNodeInfo& info)
{
    FragmentNodeRef ref;
    ref.nodeIndex = info.nodeIndex;
    ref.nodeId = info.nodeId;
    ref.attrCursor = info.attrCursor;
    ref.attrCount = info.attrCount;
    ref.childCursor = info.childCursor;
    ref.childCount = info.childCount;
    ref.storePtr = info.storePtr;
    ref.nodePtr = info.nodePtr;
    ref.nodeBlockBegin = info.nodeBlockBegin;
    ref.nodeBlockEnd = info.nodeBlockEnd;
    ref.nodeCount = info.nodeCount;
    ref.childIndexBlockBegin = info.childIndexBlockBegin;
    ref.childIndexBlockEnd = info.childIndexBlockEnd;
    ref.attrStringBase = info.attrStringBase;
    ref.attrNameOffsetTable = info.attrNameOffsetTable;
    ref.attrTokenContext = info.attrTokenContext;
    ref.attrTokenIndexBase = info.attrTokenIndexBase;
    ref.attrTokenBase = info.attrTokenBase;
    ref.childNameDataBase = info.childNameDataBase;
    ref.childNameResolverContext = info.childNameResolverContext;
    ref.childNameOffsetTable = info.childNameOffsetTable;
    ref.attrVectorBegin = info.attrVectorBegin;
    ref.attrVectorEnd = info.attrVectorEnd;
    ref.childVectorBegin = info.childVectorBegin;
    ref.childVectorEnd = info.childVectorEnd;
    ref.readStore = info.readStore;
    ref.valid =
        info.nodeReadable &&
        info.nodeValid &&
        info.storePtr != 0 &&
        info.nodePtr != 0 &&
        info.nodeBlockBegin != 0 &&
        info.nodeBlockEnd > info.nodeBlockBegin;
    return ref;
}

CoopNativeGameStateFragmentLocator::RawNodeRange CoopNativeGameStateFragmentLocator::MakeSingleNodeRange(
    const FragmentNodeRef& node)
{
    RawNodeRange range;
    if (!node.valid || node.nodePtr == 0)
        return range;

    range.beginPtr = node.nodePtr;
    range.endPtr = node.nodePtr + (node.readStore ? kReadStoreNodeBytes : kWriteStoreNodeBytes);
    range.storePtr = node.storePtr;
    range.nodeBlockBegin = node.nodeBlockBegin;
    range.nodeBlockEnd = node.nodeBlockEnd;
    range.beginIndex = node.nodeIndex;
    range.count = 1;
    range.valid = true;
    return range;
}

void CoopNativeGameStateFragmentLocator::AddObservedNode(
    std::vector<FragmentNodeRef>& nodes,
    const FragmentNodeRef& node)
{
    if (!node.valid)
        return;

    const auto alreadyKnown = std::any_of(
        nodes.begin(),
        nodes.end(),
        [&node](const FragmentNodeRef& existing)
        {
            return existing.valid &&
                existing.storePtr == node.storePtr &&
                existing.nodeBlockBegin == node.nodeBlockBegin &&
                existing.nodeIndex == node.nodeIndex;
        });

    if (!alreadyKnown)
        nodes.push_back(node);
}

void CoopNativeGameStateFragmentLocator::AddItemNodeForEntity(unsigned entityId, const FragmentNodeRef& node)
{
    if (entityId == 0 || !node.valid)
        return;

    const int candidateScore = ScoreItemRootCandidate(node);
    const std::string candidateSummary = DescribeItemRootCandidate(node);
    if (!IsLikelyItemRootCandidate(node))
    {
        std::vector<std::string>& observedSummaries = m_itemObservedSummariesByEntityId[entityId];
        if (observedSummaries.size() < 12)
            observedSummaries.push_back("reject:" + candidateSummary);
        SetLastEvent(
            "item root rejected entity=" + std::to_string(entityId) +
            " candidate=" + candidateSummary);
        return;
    }

    bool storeSnapshot = false;
    auto [it, inserted] = m_itemNodesByEntityId.emplace(entityId, node);
    if (!inserted)
    {
        int existingScore = ScoreItemRootCandidate(it->second);
        const auto scoreIt = m_itemRootScoresByEntityId.find(entityId);
        if (scoreIt != m_itemRootScoresByEntityId.end())
            existingScore = scoreIt->second;
        if (candidateScore > existingScore ||
            (candidateScore == existingScore && ShouldReplaceItemRootCandidate(it->second, node)))
        {
            it->second = node;
            m_itemRootScoresByEntityId[entityId] = candidateScore;
            m_itemRootSummariesByEntityId[entityId] = candidateSummary;
            storeSnapshot = true;
        }
    }
    else
    {
        m_itemNodeEntityOrder.push_back(entityId);
        m_itemRootScoresByEntityId[entityId] = candidateScore;
        m_itemRootSummariesByEntityId[entityId] = candidateSummary;
        storeSnapshot = true;
    }

    AddObservedNode(m_itemObservedNodesByEntityId[entityId], node);
    if (storeSnapshot || m_itemByteSnapshotsByEntityId.find(entityId) == m_itemByteSnapshotsByEntityId.end())
    {
        std::vector<FragmentNodeRef> snapshotNodes;
        snapshotNodes.push_back(node);
        m_itemByteSnapshotsByEntityId[entityId] = CaptureNodeSnapshot(snapshotNodes, 512u * 1024u);
    }
    std::vector<std::string>& observedSummaries = m_itemObservedSummariesByEntityId[entityId];
    if (observedSummaries.size() < 12)
        observedSummaries.push_back(candidateSummary);
    RecomputeReferenceCompleteness();
}

void CoopNativeGameStateFragmentLocator::AddAnonymousItemNode(uint64_t scopeSeq, const FragmentNodeRef& node)
{
    if (scopeSeq == 0 || !node.valid)
        return;

    const int candidateScore = ScoreItemRootCandidate(node);
    const std::string candidateSummary = DescribeItemRootCandidate(node);
    if (!IsLikelyItemRootCandidate(node))
    {
        std::vector<std::string>& observedSummaries = m_anonymousItemObservedSummariesByScopeSeq[scopeSeq];
        if (observedSummaries.size() < 12)
            observedSummaries.push_back("reject:" + candidateSummary);
        SetLastEvent(
            "anonymous item root rejected scope=" + std::to_string(scopeSeq) +
            " candidate=" + candidateSummary);
        return;
    }

    bool storeSnapshot = false;
    auto [it, inserted] = m_anonymousItemNodesByScopeSeq.emplace(scopeSeq, node);
    if (!inserted)
    {
        int existingScore = ScoreItemRootCandidate(it->second);
        const auto scoreIt = m_anonymousItemRootScoresByScopeSeq.find(scopeSeq);
        if (scoreIt != m_anonymousItemRootScoresByScopeSeq.end())
            existingScore = scoreIt->second;
        if (candidateScore > existingScore ||
            (candidateScore == existingScore && ShouldReplaceItemRootCandidate(it->second, node)))
        {
            it->second = node;
            m_anonymousItemRootScoresByScopeSeq[scopeSeq] = candidateScore;
            m_anonymousItemRootSummariesByScopeSeq[scopeSeq] = candidateSummary;
            storeSnapshot = true;
        }
    }
    else
    {
        m_anonymousItemScopeOrder.push_back(scopeSeq);
        m_anonymousItemRootScoresByScopeSeq[scopeSeq] = candidateScore;
        m_anonymousItemRootSummariesByScopeSeq[scopeSeq] = candidateSummary;
        storeSnapshot = true;
    }

    AddObservedNode(m_anonymousItemObservedNodesByScopeSeq[scopeSeq], node);
    if (storeSnapshot || m_anonymousItemByteSnapshotsByScopeSeq.find(scopeSeq) == m_anonymousItemByteSnapshotsByScopeSeq.end())
    {
        std::vector<FragmentNodeRef> snapshotNodes;
        snapshotNodes.push_back(node);
        m_anonymousItemByteSnapshotsByScopeSeq[scopeSeq] = CaptureNodeSnapshot(snapshotNodes, 512u * 1024u);
    }
    std::vector<std::string>& observedSummaries = m_anonymousItemObservedSummariesByScopeSeq[scopeSeq];
    if (observedSummaries.size() < 12)
        observedSummaries.push_back(candidateSummary);
    RecomputeReferenceCompleteness();
}

std::vector<CoopNativeGameStateFragmentLocator::ItemFragmentRef>
CoopNativeGameStateFragmentLocator::BuildResolvedItemFragments() const
{
    std::vector<ItemFragmentRef> items;
    items.reserve(std::max(m_itemNodesByEntityId.size(), m_inventoryEntityOrder.size()));

    const bool rootOnlyPayload = EnvFlagEnabled("COOP_NATIVE_FRAGMENT_ROOT_ONLY_PAYLOAD");
    const bool serializeOrderFallback = EnvFlagEnabled("COOP_NATIVE_FRAGMENT_SERIALIZE_ORDER_FALLBACK");
    const bool anonymousFallback =
        serializeOrderFallback ||
        EnvFlagEnabled("COOP_NATIVE_FRAGMENT_ANONYMOUS_ITEM_FALLBACK");
    std::unordered_set<unsigned> consumedKnownEntities;
    std::unordered_set<uint64_t> consumedAnonymousScopes;

    auto appendKnown = [&](unsigned targetEntityId, unsigned sourceEntityId) -> bool
    {
        if (consumedKnownEntities.find(sourceEntityId) != consumedKnownEntities.end())
            return false;

        const auto nodeIt = m_itemNodesByEntityId.find(sourceEntityId);
        if (nodeIt == m_itemNodesByEntityId.end() || !nodeIt->second.valid)
            return false;

        ItemFragmentRef item;
        item.entityId = targetEntityId;
        item.sourceEntityId = sourceEntityId;
        item.node = nodeIt->second;
        item.nodeRange = MakeSingleNodeRange(nodeIt->second);
        if (rootOnlyPayload)
        {
            item.observedNodes.push_back(nodeIt->second);
            item.observedNodeRanges = BuildContiguousNodeRanges(item.observedNodes);
        }
        else
        {
            const auto observedIt = m_itemObservedNodesByEntityId.find(sourceEntityId);
            if (observedIt != m_itemObservedNodesByEntityId.end())
            {
                item.observedNodes = observedIt->second;
                item.observedNodeRanges = BuildContiguousNodeRanges(item.observedNodes);
            }
        }
        items.push_back(std::move(item));
        consumedKnownEntities.insert(sourceEntityId);
        return true;
    };

    auto appendKnownBySerializeOrder = [&](unsigned targetEntityId) -> bool
    {
        for (unsigned sourceEntityId : m_itemNodeEntityOrder)
        {
            if (appendKnown(targetEntityId, sourceEntityId))
                return true;
        }
        return false;
    };

    auto appendAnonymous = [&](unsigned targetEntityId) -> bool
    {
        for (uint64_t scopeSeq : m_anonymousItemScopeOrder)
        {
            if (consumedAnonymousScopes.find(scopeSeq) != consumedAnonymousScopes.end())
                continue;

            const auto nodeIt = m_anonymousItemNodesByScopeSeq.find(scopeSeq);
            if (nodeIt == m_anonymousItemNodesByScopeSeq.end() || !nodeIt->second.valid)
                continue;

            ItemFragmentRef item;
            item.entityId = targetEntityId;
            item.sourceScopeSeq = scopeSeq;
            item.node = nodeIt->second;
            item.nodeRange = MakeSingleNodeRange(nodeIt->second);
            if (rootOnlyPayload)
            {
                item.observedNodes.push_back(nodeIt->second);
                item.observedNodeRanges = BuildContiguousNodeRanges(item.observedNodes);
            }
            else
            {
                const auto observedIt = m_anonymousItemObservedNodesByScopeSeq.find(scopeSeq);
                if (observedIt != m_anonymousItemObservedNodesByScopeSeq.end())
                {
                    item.observedNodes = observedIt->second;
                    item.observedNodeRanges = BuildContiguousNodeRanges(item.observedNodes);
                }
            }
            items.push_back(std::move(item));
            consumedAnonymousScopes.insert(scopeSeq);
            return true;
        }
        return false;
    };

    for (unsigned entityId : m_inventoryEntityOrder)
    {
        if (appendKnown(entityId, entityId))
            continue;
        if (serializeOrderFallback && appendKnownBySerializeOrder(entityId))
            continue;
        if (anonymousFallback && appendAnonymous(entityId))
            continue;
    }

    return items;
}

std::vector<CoopNativeGameStateFragmentLocator::RawNodeRange>
CoopNativeGameStateFragmentLocator::BuildContiguousNodeRanges(const std::vector<FragmentNodeRef>& nodes)
{
    std::vector<FragmentNodeRef> sorted;
    sorted.reserve(nodes.size());
    for (const FragmentNodeRef& node : nodes)
    {
        if (node.valid && node.nodeBlockBegin != 0 && node.nodeBlockEnd > node.nodeBlockBegin)
            sorted.push_back(node);
    }

    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const FragmentNodeRef& lhs, const FragmentNodeRef& rhs)
        {
            if (lhs.storePtr != rhs.storePtr)
                return lhs.storePtr < rhs.storePtr;
            if (lhs.nodeBlockBegin != rhs.nodeBlockBegin)
                return lhs.nodeBlockBegin < rhs.nodeBlockBegin;
            return lhs.nodeIndex < rhs.nodeIndex;
        });

    std::vector<RawNodeRange> ranges;
    for (const FragmentNodeRef& node : sorted)
    {
        const std::uintptr_t beginPtr = node.readStore
            ? node.nodeBlockBegin + static_cast<std::uintptr_t>(node.nodeIndex) * kReadStoreNodeBytes
            : node.nodePtr;
        const std::uintptr_t endPtr = beginPtr + (node.readStore ? kReadStoreNodeBytes : kWriteStoreNodeBytes);
        if (beginPtr == 0 || endPtr <= beginPtr)
            continue;
        if (node.readStore && (beginPtr < node.nodeBlockBegin || endPtr > node.nodeBlockEnd))
            continue;

        if (node.readStore && !ranges.empty())
        {
            RawNodeRange& last = ranges.back();
            const bool sameBlock =
                last.valid &&
                last.storePtr == node.storePtr &&
                last.nodeBlockBegin == node.nodeBlockBegin &&
                last.endPtr == beginPtr;
            const bool consecutiveIndex = last.beginIndex + last.count == node.nodeIndex;
            if (sameBlock && consecutiveIndex)
            {
                last.endPtr = endPtr;
                ++last.count;
                continue;
            }
        }

        RawNodeRange range;
        range.beginPtr = beginPtr;
        range.endPtr = endPtr;
        range.storePtr = node.storePtr;
        range.nodeBlockBegin = node.nodeBlockBegin;
        range.nodeBlockEnd = node.nodeBlockEnd;
        range.beginIndex = node.nodeIndex;
        range.count = 1;
        range.valid = true;
        ranges.push_back(range);
    }

    return ranges;
}

bool CoopNativeGameStateFragmentLocator::CaptureRangeBytes(
    const RawNodeRange& range,
    std::vector<uint8_t>& bytes,
    uint32_t maxBytes,
    RawRangeByteCopy& outCopy)
{
    outCopy.range = range;
    if (!range.valid || range.beginPtr == 0 || range.endPtr <= range.beginPtr)
    {
        outCopy.reason = "invalid_range";
        return false;
    }

    const uint64_t byteCount64 = static_cast<uint64_t>(range.endPtr - range.beginPtr);
    if (byteCount64 > maxBytes)
    {
        outCopy.reason = "range_too_large";
        return false;
    }

    const size_t byteCount = static_cast<size_t>(byteCount64);
    if (bytes.size() + byteCount > maxBytes)
    {
        outCopy.reason = "capture_too_large";
        return false;
    }

    const void* source = reinterpret_cast<const void*>(range.beginPtr);
    std::string reason;
    if (!CoopRuntimeGuards::PreflightRuntimePointer(
            "capture native save fragment bytes",
            source,
            byteCount,
            CoopRuntimeGuards::RuntimeAccess::Read,
            &reason))
    {
        outCopy.reason = reason.empty() ? "preflight_failed" : reason;
        return false;
    }

    const size_t offset = bytes.size();
    bytes.resize(offset + byteCount);
    struct CopyContext
    {
        const void* source = nullptr;
        void* destination = nullptr;
        size_t size = 0;
    } context { source, bytes.data() + offset, byteCount };

    const bool copied = CoopRuntimeGuards::TryRunGuardedCallback(
        "copy native save fragment bytes",
        [](void* rawContext) -> bool
        {
            CopyContext* ctx = static_cast<CopyContext*>(rawContext);
            std::memcpy(ctx->destination, ctx->source, ctx->size);
            return true;
        },
        &context,
        &reason);

    if (!copied)
    {
        bytes.resize(offset);
        outCopy.reason = reason.empty() ? "copy_failed" : reason;
        return false;
    }

    outCopy.byteOffset = static_cast<uint32_t>(offset);
    outCopy.byteCount = static_cast<uint32_t>(byteCount);
    outCopy.ok = true;
    outCopy.reason = "ok";
    return true;
}

bool CoopNativeGameStateFragmentLocator::CaptureBackingRangeBytes(
    const RawBackingRange& range,
    std::vector<uint8_t>& bytes,
    uint32_t maxBytes,
    RawBackingRangeByteCopy& outCopy)
{
    outCopy.range = range;
    if (!range.valid || range.beginPtr == 0 || range.endPtr <= range.beginPtr)
    {
        outCopy.reason = "invalid_backing_range";
        return false;
    }

    const uint64_t byteCount64 = static_cast<uint64_t>(range.endPtr - range.beginPtr);
    if (byteCount64 > maxBytes)
    {
        outCopy.reason = "backing_range_too_large";
        return false;
    }

    const size_t byteCount = static_cast<size_t>(byteCount64);
    if (bytes.size() + byteCount > maxBytes)
    {
        outCopy.reason = "capture_too_large";
        return false;
    }

    const void* source = reinterpret_cast<const void*>(range.beginPtr);
    std::string reason;
    if (!CoopRuntimeGuards::PreflightRuntimePointer(
            "capture native save backing fragment bytes",
            source,
            byteCount,
            CoopRuntimeGuards::RuntimeAccess::Read,
            &reason))
    {
        outCopy.reason = reason.empty() ? "preflight_failed" : reason;
        return false;
    }

    const size_t offset = bytes.size();
    bytes.resize(offset + byteCount);
    struct CopyContext
    {
        const void* source = nullptr;
        void* destination = nullptr;
        size_t size = 0;
    } context { source, bytes.data() + offset, byteCount };

    const bool copied = CoopRuntimeGuards::TryRunGuardedCallback(
        "copy native save backing fragment bytes",
        [](void* rawContext) -> bool
        {
            CopyContext* ctx = static_cast<CopyContext*>(rawContext);
            std::memcpy(ctx->destination, ctx->source, ctx->size);
            return true;
        },
        &context,
        &reason);

    if (!copied)
    {
        bytes.resize(offset);
        outCopy.reason = reason.empty() ? "copy_failed" : reason;
        return false;
    }

    outCopy.byteOffset = static_cast<uint32_t>(offset);
    outCopy.byteCount = static_cast<uint32_t>(byteCount);
    outCopy.ok = true;
    outCopy.reason = "ok";
    return true;
}

CoopNativeGameStateFragmentLocator::FragmentByteSnapshot
CoopNativeGameStateFragmentLocator::CaptureNodeSnapshot(
    const std::vector<FragmentNodeRef>& nodes,
    uint32_t maxBytes)
{
    FragmentByteSnapshot snapshot;
    if (nodes.empty())
    {
        snapshot.reason = "no_nodes";
        return snapshot;
    }

    const std::vector<RawNodeRange> nodeRanges = BuildContiguousNodeRanges(nodes);
    if (nodeRanges.empty())
    {
        snapshot.reason = "no_node_ranges";
        return snapshot;
    }

    for (const RawNodeRange& range : nodeRanges)
    {
        RawRangeByteCopy copy;
        if (CaptureRangeBytes(range, snapshot.bytes, maxBytes, copy))
            ++snapshot.capturedRanges;
        else
            ++snapshot.failedRanges;
        snapshot.ranges.push_back(std::move(copy));
    }

    std::vector<RawBackingRange> backingRanges = BuildBackingRanges(nodes);
    for (const RawNodeRange& range : nodeRanges)
        AddWriteBackingRangesFromNodeRange(backingRanges, range);
    for (const RawBackingRange& range : backingRanges)
    {
        RawBackingRangeByteCopy copy;
        if (CaptureBackingRangeBytes(range, snapshot.bytes, maxBytes, copy))
            ++snapshot.capturedBackingRanges;
        else
            ++snapshot.failedBackingRanges;
        snapshot.backingRanges.push_back(std::move(copy));
    }

    if (snapshot.failedRanges != 0)
        snapshot.reason = "range_capture_failed";
    else if (snapshot.failedBackingRanges != 0)
        snapshot.reason = "backing_range_capture_failed";
    else if (snapshot.capturedRanges == 0)
        snapshot.reason = "no_ranges";
    else
    {
        snapshot.ok = true;
        snapshot.reason = "ok";
    }

    return snapshot;
}

std::vector<CoopNativeGameStateFragmentLocator::RawBackingRange>
CoopNativeGameStateFragmentLocator::BuildBackingRanges(const std::vector<FragmentNodeRef>& nodes)
{
    std::vector<RawBackingRange> ranges;
    for (const FragmentNodeRef& node : nodes)
    {
        RawBackingRange attrRange;
        if (node.readStore && TryBuildAttrDataRange(node, attrRange))
            AddBackingRange(ranges, attrRange);

        RawBackingRange writeAttrRange;
        if (!node.readStore && TryBuildWriteAttrVectorRange(node, writeAttrRange))
            AddBackingRange(ranges, writeAttrRange);

        RawBackingRange childRange;
        if (TryBuildChildIndexBlockRange(node, childRange))
            AddBackingRange(ranges, childRange);

        RawBackingRange writeChildRange;
        if (!node.readStore && TryBuildWriteChildVectorRange(node, writeChildRange))
            AddBackingRange(ranges, writeChildRange);
    }

    std::sort(
        ranges.begin(),
        ranges.end(),
        [](const RawBackingRange& lhs, const RawBackingRange& rhs)
        {
            if (lhs.kind != rhs.kind)
                return static_cast<uint32_t>(lhs.kind) < static_cast<uint32_t>(rhs.kind);
            if (lhs.storePtr != rhs.storePtr)
                return lhs.storePtr < rhs.storePtr;
            if (lhs.basePtr != rhs.basePtr)
                return lhs.basePtr < rhs.basePtr;
            if (lhs.beginPtr != rhs.beginPtr)
                return lhs.beginPtr < rhs.beginPtr;
            return lhs.ownerNodeIndex < rhs.ownerNodeIndex;
        });
    return ranges;
}

bool CoopNativeGameStateFragmentLocator::IsCurrentInventoryScope() const
{
    return m_activeInventoryScopeSeq != 0;
}

bool CoopNativeGameStateFragmentLocator::IsCurrentItemScope() const
{
    return m_activeItemScopeSeq != 0;
}
