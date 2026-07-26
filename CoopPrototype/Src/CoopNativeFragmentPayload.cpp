#include "CoopNativeFragmentPayload.h"

#include "CoopNativeSaveStoreApi.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace
{
constexpr uint32_t kFnv1aOffsetBasis = 2166136261u;
constexpr uint32_t kFnv1aPrime = 16777619u;

#pragma pack(push, 1)
struct PayloadHeader
{
    uint32_t magic = CoopNativeFragmentPayload::kPayloadMagic;
    uint16_t version = CoopNativeFragmentPayload::kPayloadVersion;
    uint16_t headerBytes = sizeof(PayloadHeader);
    uint32_t totalBytes = 0;
    uint32_t payloadChecksum = 0;
    uint32_t rawByteChecksum = 0;
    uint32_t rangeRecordCount = 0;
    uint32_t inventoryRangeCount = 0;
    uint32_t itemGroupCount = 0;
    uint32_t itemRangeCount = 0;
    uint32_t rawBytes = 0;
    uint32_t attrNameRecordCount = 0;
    uint32_t attrNameBytes = 0;
    uint64_t runId = 0;
    uint64_t schemaHash = 0;
    uint64_t contentHash = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct PayloadAttrNameTableRecord
{
    uint32_t rangeIndex = 0;
    uint32_t ordinal = 0;
    uint32_t type = 0;
    uint32_t sourceNameIndex = 0;
    uint32_t token = 0;
    uint32_t nameOffset = 0;
    uint32_t nameBytes = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(CoopNativeFragmentPayload::PayloadRangeRecord) == 48);
static_assert(sizeof(CoopNativeFragmentPayload::PayloadWriteNodeOracleRecord) == 32);
static_assert(sizeof(PayloadAttrNameTableRecord) == 32);

uint32_t UpdateFnv1a(uint32_t checksum, const uint8_t* data, size_t size)
{
    if (!data)
        return checksum;

    for (size_t i = 0; i < size; ++i)
    {
        checksum ^= static_cast<uint32_t>(data[i]);
        checksum *= kFnv1aPrime;
    }
    return checksum;
}

template<typename T>
bool ReadValue(const uint8_t* bytes, size_t size, size_t offset, T& out)
{
    if (!bytes || offset > size || sizeof(T) > size - offset)
        return false;

    std::memcpy(&out, bytes + offset, sizeof(T));
    return true;
}

template<typename T>
void AppendValue(std::vector<uint8_t>& bytes, const T& value)
{
    const auto* raw = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

template<typename T>
void PatchValue(std::vector<uint8_t>& bytes, size_t offset, const T& value)
{
    if (offset + sizeof(value) > bytes.size())
        return;

    const auto* raw = reinterpret_cast<const uint8_t*>(&value);
    std::copy(raw, raw + sizeof(value), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::string Hex32(uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string Hex64(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

const CoopNativeGameStateFragmentLocator::FragmentNodeRef* FindFirstNodeForRange(
    const std::vector<CoopNativeGameStateFragmentLocator::FragmentNodeRef>& nodes,
    const CoopNativeGameStateFragmentLocator::RawNodeRange& range)
{
    for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : nodes)
    {
        if (node.valid &&
            node.storePtr == range.storePtr &&
            node.nodeBlockBegin == range.nodeBlockBegin &&
            node.nodeIndex == range.beginIndex)
        {
            return &node;
        }
    }
    return nullptr;
}

bool AppendRangeRecord(
    std::vector<CoopNativeFragmentPayload::PayloadRangeRecord>& records,
    const CoopNativeGameStateFragmentLocator::RawRangeByteCopy& copy,
    const std::vector<CoopNativeGameStateFragmentLocator::FragmentNodeRef>& nodes,
    CoopNativeFragmentPayload::PayloadGroupKind groupKind,
    uint32_t entityId,
    std::string& reason)
{
    if (!copy.ok)
    {
        reason = copy.reason.empty() ? "range_not_captured" : copy.reason;
        return false;
    }

    const CoopNativeGameStateFragmentLocator::FragmentNodeRef* firstNode = FindFirstNodeForRange(nodes, copy.range);
    if (!firstNode)
    {
        reason = "range_first_node_not_found";
        return false;
    }

    CoopNativeFragmentPayload::PayloadRangeRecord record;
    record.groupKind = static_cast<uint32_t>(groupKind);
    record.rangeKind = static_cast<uint32_t>(
        firstNode->readStore
            ? CoopNativeFragmentPayload::PayloadRangeKind::NodeBlock
            : CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock);
    record.entityId = entityId;
    record.beginIndex = copy.range.beginIndex;
    record.nodeCount = copy.range.count;
    record.byteOffset = copy.byteOffset;
    record.byteCount = copy.byteCount;
    record.firstNodeId = firstNode->nodeId;
    record.attrCursor = firstNode->attrCursor;
    record.attrCount = firstNode->attrCount;
    record.childCursor = firstNode->childCursor;
    record.childCount = firstNode->childCount;
    records.push_back(record);
    return true;
}

bool AppendBackingRangeRecord(
    std::vector<CoopNativeFragmentPayload::PayloadRangeRecord>& records,
    const CoopNativeGameStateFragmentLocator::RawBackingRangeByteCopy& copy,
    CoopNativeFragmentPayload::PayloadGroupKind groupKind,
    uint32_t entityId,
    std::string& reason)
{
    if (!copy.ok)
    {
        reason = copy.reason.empty() ? "backing_range_not_captured" : copy.reason;
        return false;
    }

    CoopNativeFragmentPayload::PayloadRangeRecord record;
    record.groupKind = static_cast<uint32_t>(groupKind);
    record.entityId = entityId;
    record.beginIndex = copy.range.ownerNodeIndex;
    record.nodeCount = 1;
    record.byteOffset = copy.byteOffset;
    record.byteCount = copy.byteCount;
    record.firstNodeId = copy.range.ownerNodeId;
    record.attrCursor = copy.range.cursor;
    record.attrCount = copy.range.count;
    if (copy.range.kind == CoopNativeGameStateFragmentLocator::BackingRangeKind::AttrDataPool)
    {
        record.rangeKind = static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::AttrDataPool);
    }
    else if (copy.range.kind == CoopNativeGameStateFragmentLocator::BackingRangeKind::ChildIndexBlock)
    {
        record.rangeKind = static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::ChildIndexBlock);
        record.childCursor = copy.range.cursor;
        record.childCount = copy.range.count;
        record.attrCursor = 0;
        record.attrCount = 0;
    }
    else if (copy.range.kind == CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteAttrVector)
    {
        record.rangeKind = static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
        record.attrCursor = 0;
        record.attrCount = copy.range.count;
    }
    else if (copy.range.kind == CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteChildVector)
    {
        record.rangeKind = static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
        record.childCursor = 0;
        record.childCount = copy.range.count;
        record.attrCursor = 0;
        record.attrCount = 0;
    }
    else
    {
        reason = "unsupported_backing_range_kind";
        return false;
    }

    records.push_back(record);
    return true;
}

bool IsWriteRangeKind(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsAttrPayloadRangeKind(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::AttrDataPool) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
}

bool ValidateItemAttrSignatures(
    const std::vector<CoopNativeFragmentPayload::PayloadRangeRecord>& records,
    const std::vector<PayloadAttrNameTableRecord>& attrNameRecords,
    const std::vector<uint8_t>& nameBytes,
    std::string& reason)
{
    struct RangeSignature
    {
        uint32_t entityId = 0;
        bool itemAttrRange = false;
        bool hasCount = false;
        bool hasArchetype = false;
        bool hasOwner = false;
    };

    std::vector<RangeSignature> rangeSignatures(records.size());
    std::unordered_set<uint32_t> itemEntityIds;
    bool hasItemAttrMetadata = false;

    for (size_t i = 0; i < records.size(); ++i)
    {
        const CoopNativeFragmentPayload::PayloadRangeRecord& record = records[i];
        if (record.groupKind != static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item) ||
            record.entityId == 0)
        {
            continue;
        }

        itemEntityIds.insert(record.entityId);
        if (IsAttrPayloadRangeKind(record.rangeKind))
        {
            rangeSignatures[i].entityId = record.entityId;
            rangeSignatures[i].itemAttrRange = true;
        }
    }

    for (const PayloadAttrNameTableRecord& attr : attrNameRecords)
    {
        if (attr.rangeIndex >= rangeSignatures.size() ||
            !rangeSignatures[attr.rangeIndex].itemAttrRange)
        {
            continue;
        }
        if (attr.nameOffset > nameBytes.size() ||
            attr.nameBytes == 0 ||
            attr.nameBytes > nameBytes.size() - attr.nameOffset)
        {
            reason = "item_attr_signature_name_bounds";
            return false;
        }

        hasItemAttrMetadata = true;
        const char* name = reinterpret_cast<const char*>(nameBytes.data() + attr.nameOffset);
        size_t nameLength = 0;
        while (nameLength < attr.nameBytes && name[nameLength] != '\0')
            ++nameLength;
        const std::string attrName(name, nameLength);
        RangeSignature& signature = rangeSignatures[attr.rangeIndex];
        if (attrName == "m_count")
            signature.hasCount = true;
        else if (attrName == "selectedArchetype")
            signature.hasArchetype = true;
        else if (attrName == "ownerId")
            signature.hasOwner = true;
    }

    if (!hasItemAttrMetadata)
        return true;

    for (uint32_t entityId : itemEntityIds)
    {
        bool hasCompleteSignature = false;
        for (const RangeSignature& signature : rangeSignatures)
        {
            if (signature.entityId == entityId &&
                signature.hasCount &&
                signature.hasArchetype &&
                signature.hasOwner)
            {
                hasCompleteSignature = true;
                break;
            }
        }

        if (!hasCompleteSignature)
        {
            reason = "item_attr_signature_missing_entity_" + std::to_string(entityId);
            return false;
        }
    }

    return true;
}

std::vector<CoopNativeFragmentPayload::PayloadWriteNodeOracleRecord> BuildOracleRecords(
    const std::vector<CoopNativeFragmentPayload::PayloadRangeRecord>& records,
    const std::vector<std::uintptr_t>& recordStores,
    const std::vector<CoopNativeFragmentPayload::PayloadWriteNodeOracleInputRecord>& oracleInputs)
{
    std::vector<CoopNativeFragmentPayload::PayloadWriteNodeOracleRecord> oracleRecords;
    if (records.empty() || recordStores.size() != records.size() || oracleInputs.empty())
        return oracleRecords;

    oracleRecords.reserve(std::min(records.size(), oracleInputs.size()));
    for (uint32_t rangeIndex = 0; rangeIndex < records.size(); ++rangeIndex)
    {
        const CoopNativeFragmentPayload::PayloadRangeRecord& record = records[rangeIndex];
        if (!IsWriteRangeKind(record.rangeKind) || recordStores[rangeIndex] == 0)
            continue;

        const auto matched = std::find_if(
            oracleInputs.begin(),
            oracleInputs.end(),
            [&](const CoopNativeFragmentPayload::PayloadWriteNodeOracleInputRecord& input)
            {
                return input.store == recordStores[rangeIndex] &&
                    input.writeIndex == record.beginIndex;
            });
        if (matched == oracleInputs.end())
            continue;

        CoopNativeFragmentPayload::PayloadWriteNodeOracleRecord oracle;
        oracle.sourceRangeIndex = rangeIndex;
        oracle.writeIndex = matched->writeIndex;
        oracle.finalIndex = matched->finalIndex;
        oracle.nameToken = matched->nameToken;
        oracle.attrRecords = matched->attrRecords;
        oracle.childRecords = matched->childRecords;
        oracle.childCount = matched->childCount;
        oracle.flags = matched->flags;
        oracleRecords.push_back(oracle);
    }
    return oracleRecords;
}

struct WriteAttrRecord
{
    uint32_t type = 0;
    uint32_t nameIndex = 0;
};

bool ReadWriteAttrRecord(const uint8_t* bytes, size_t size, uint32_t ordinal, WriteAttrRecord& outRecord)
{
    outRecord = {};
    constexpr size_t kWriteAttrRecordBytes = 0x28;
    const size_t offset = static_cast<size_t>(ordinal) * kWriteAttrRecordBytes;
    if (!bytes || offset > size || kWriteAttrRecordBytes > size - offset)
        return false;

    std::memcpy(&outRecord.type, bytes + offset, sizeof(outRecord.type));
    std::memcpy(&outRecord.nameIndex, bytes + offset + 0x4, sizeof(outRecord.nameIndex));
    return true;
}

void AppendWriteAttrNameMetadata(
    const CoopNativeGameStateFragmentLocator::RawBackingRangeByteCopy& copy,
    uint32_t rangeIndex,
    const std::vector<uint8_t>& rawBytes,
    std::vector<PayloadAttrNameTableRecord>& tableRecords,
    std::vector<uint8_t>& nameBytes,
    uint32_t& failures)
{
    if (!copy.ok ||
        copy.range.kind != CoopNativeGameStateFragmentLocator::BackingRangeKind::WriteAttrVector ||
        copy.byteCount == 0 ||
        (copy.byteCount % 0x28u) != 0 ||
        copy.byteOffset > rawBytes.size() ||
        copy.byteCount > rawBytes.size() - copy.byteOffset)
    {
        return;
    }

    const uint32_t recordCount = static_cast<uint32_t>(copy.byteCount / 0x28u);
    const uint8_t* localBytes = rawBytes.data() + copy.byteOffset;

    for (uint32_t ordinal = 0; ordinal < recordCount; ++ordinal)
    {
        WriteAttrRecord attr;
        if (!ReadWriteAttrRecord(localBytes, copy.byteCount, ordinal, attr))
        {
            ++failures;
            continue;
        }

        uint16_t token = 0;
        const char* namePtr = nullptr;
        std::string nameReason;
        if (!CoopNativeSaveStoreApi::TryMaterializeWriteAttrToken(
                localBytes + static_cast<size_t>(ordinal) * 0x28u,
                token,
                &nameReason) ||
            !CoopNativeSaveStoreApi::TryResolveWriteStoreName(
                copy.range.storePtr,
                attr.nameIndex,
                namePtr,
                &nameReason))
        {
            ++failures;
            continue;
        }

        const std::string name = CoopRuntimeGuards::ReadRuntimeCString(namePtr, 256);
        if (name.empty() || nameBytes.size() + name.size() + 1 > UINT32_MAX)
        {
            ++failures;
            continue;
        }

        PayloadAttrNameTableRecord record;
        record.rangeIndex = rangeIndex;
        record.ordinal = ordinal;
        record.type = attr.type;
        record.sourceNameIndex = attr.nameIndex;
        record.token = token;
        record.nameOffset = static_cast<uint32_t>(nameBytes.size());
        record.nameBytes = static_cast<uint32_t>(name.size() + 1);
        nameBytes.insert(nameBytes.end(), name.begin(), name.end());
        nameBytes.push_back('\0');
        tableRecords.push_back(record);
    }
}
}

namespace CoopNativeFragmentPayload
{
BuildResult BuildInventoryPayload(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentByteCapture& capture)
{
    const std::vector<PayloadWriteNodeOracleInputRecord> emptyOracleInputs;
    return BuildInventoryPayload(capture, emptyOracleInputs);
}

BuildResult BuildInventoryPayload(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentByteCapture& capture,
    const std::vector<PayloadWriteNodeOracleInputRecord>& oracleInputs)
{
    BuildResult result;
    result.runId = capture.bundle.runId;
    result.schemaHash = capture.bundle.schemaHash;
    result.contentHash = capture.bundle.contentHash;
    result.inventoryRanges = static_cast<uint32_t>(
        capture.inventoryRanges.size() + capture.inventoryBackingRanges.size());
    result.itemGroups = static_cast<uint32_t>(capture.itemRanges.size());
    result.rawBytes = static_cast<uint32_t>(capture.bytes.size());

    if (!capture.ok)
    {
        result.reason = "capture_" + (capture.reason.empty() ? std::string("failed") : capture.reason);
        return result;
    }

    if (capture.bytes.size() > UINT32_MAX)
    {
        result.reason = "raw_bytes_too_large";
        return result;
    }

    std::vector<PayloadRangeRecord> records;
    records.reserve(capture.inventoryRanges.size() + capture.capturedRanges);
    std::vector<std::uintptr_t> recordStores;
    recordStores.reserve(capture.inventoryRanges.size() + capture.capturedRanges);
    std::vector<PayloadAttrNameTableRecord> attrNameRecords;
    std::vector<uint8_t> attrNameBytes;

    for (const CoopNativeGameStateFragmentLocator::RawRangeByteCopy& range : capture.inventoryRanges)
    {
        if (!AppendRangeRecord(
                records,
                range,
                capture.bundle.inventoryObservedNodes,
                PayloadGroupKind::Inventory,
                0,
                result.reason))
        {
            return result;
        }
        recordStores.push_back(range.range.storePtr);
        ++result.nodeRanges;
    }

    for (const CoopNativeGameStateFragmentLocator::RawBackingRangeByteCopy& range : capture.inventoryBackingRanges)
    {
        if (!AppendBackingRangeRecord(
                records,
                range,
                PayloadGroupKind::Inventory,
                0,
                result.reason))
        {
            return result;
        }
        recordStores.push_back(range.range.storePtr);
        ++result.backingRanges;
        AppendWriteAttrNameMetadata(
            range,
            static_cast<uint32_t>(records.size() - 1),
            capture.bytes,
            attrNameRecords,
            attrNameBytes,
            result.attrNameFailures);
    }

    for (size_t itemIndex = 0; itemIndex < capture.itemRanges.size(); ++itemIndex)
    {
        const CoopNativeGameStateFragmentLocator::ItemFragmentByteCopy& itemCopy = capture.itemRanges[itemIndex];
        const auto bundleItem = std::find_if(
            capture.bundle.itemFragments.begin(),
            capture.bundle.itemFragments.end(),
            [&itemCopy](const CoopNativeGameStateFragmentLocator::ItemFragmentRef& item)
            {
                return item.entityId == itemCopy.entityId;
            });
        if (bundleItem == capture.bundle.itemFragments.end())
        {
            result.reason = "item_group_missing_bundle_ref";
            return result;
        }

        for (const CoopNativeGameStateFragmentLocator::RawRangeByteCopy& range : itemCopy.ranges)
        {
            if (!AppendRangeRecord(
                    records,
                    range,
                    bundleItem->observedNodes,
                    PayloadGroupKind::Item,
                    itemCopy.entityId,
                    result.reason))
            {
                return result;
            }
            recordStores.push_back(range.range.storePtr);
            ++result.nodeRanges;
            ++result.itemRanges;
        }

        for (const CoopNativeGameStateFragmentLocator::RawBackingRangeByteCopy& range : itemCopy.backingRanges)
        {
            if (!AppendBackingRangeRecord(
                    records,
                    range,
                    PayloadGroupKind::Item,
                    itemCopy.entityId,
                    result.reason))
            {
                return result;
            }
            recordStores.push_back(range.range.storePtr);
            ++result.backingRanges;
            ++result.itemRanges;
            AppendWriteAttrNameMetadata(
                range,
                static_cast<uint32_t>(records.size() - 1),
                capture.bytes,
                attrNameRecords,
                attrNameBytes,
                result.attrNameFailures);
        }
    }

    if (records.empty())
    {
        result.reason = "no_range_records";
        return result;
    }
    if (recordStores.size() != records.size())
    {
        result.reason = "range_store_mismatch";
        return result;
    }
    if (!ValidateItemAttrSignatures(records, attrNameRecords, attrNameBytes, result.reason))
        return result;

    const std::vector<PayloadWriteNodeOracleRecord> oracleRecords =
        BuildOracleRecords(records, recordStores, oracleInputs);

    PayloadHeader header;
    header.runId = capture.bundle.runId;
    header.schemaHash = capture.bundle.schemaHash;
    header.contentHash = capture.bundle.contentHash;
    header.rangeRecordCount = static_cast<uint32_t>(records.size());
    header.inventoryRangeCount = result.inventoryRanges;
    header.itemGroupCount = result.itemGroups;
    header.itemRangeCount = result.itemRanges;
    header.rawBytes = static_cast<uint32_t>(capture.bytes.size());
    header.attrNameRecordCount = static_cast<uint32_t>(attrNameRecords.size());
    header.attrNameBytes = static_cast<uint32_t>(attrNameBytes.size());
    header.flags = oracleRecords.empty() ? 0 : kPayloadFlagWriteNodeOracle;
    header.reserved = static_cast<uint32_t>(oracleRecords.size());
    header.rawByteChecksum = UpdateFnv1a(kFnv1aOffsetBasis, capture.bytes.data(), capture.bytes.size());
    result.attrNameRecords = header.attrNameRecordCount;
    result.attrNameBytes = header.attrNameBytes;
    result.oracleRecordCount = header.reserved;

    result.bytes.reserve(
        sizeof(PayloadHeader) +
        records.size() * sizeof(PayloadRangeRecord) +
        attrNameRecords.size() * sizeof(PayloadAttrNameTableRecord) +
        oracleRecords.size() * sizeof(PayloadWriteNodeOracleRecord) +
        attrNameBytes.size() +
        capture.bytes.size());
    AppendValue(result.bytes, header);
    for (const PayloadRangeRecord& record : records)
        AppendValue(result.bytes, record);
    for (const PayloadAttrNameTableRecord& record : attrNameRecords)
        AppendValue(result.bytes, record);
    for (const PayloadWriteNodeOracleRecord& record : oracleRecords)
        AppendValue(result.bytes, record);
    result.bytes.insert(result.bytes.end(), attrNameBytes.begin(), attrNameBytes.end());
    result.bytes.insert(result.bytes.end(), capture.bytes.begin(), capture.bytes.end());

    header.totalBytes = static_cast<uint32_t>(result.bytes.size());
    header.payloadChecksum = UpdateFnv1a(
        kFnv1aOffsetBasis,
        result.bytes.data() + sizeof(PayloadHeader),
        result.bytes.size() - sizeof(PayloadHeader));
    PatchValue(result.bytes, offsetof(PayloadHeader, totalBytes), header.totalBytes);
    PatchValue(result.bytes, offsetof(PayloadHeader, payloadChecksum), header.payloadChecksum);
    PatchValue(result.bytes, offsetof(PayloadHeader, rawByteChecksum), header.rawByteChecksum);

    result.payloadChecksum = header.payloadChecksum;
    result.rawByteChecksum = header.rawByteChecksum;
    result.rangeRecords = header.rangeRecordCount;
    result.ok = true;
    result.reason = "ok";
    return result;
}

ParsedPayload ParseInventoryPayload(const uint8_t* bytes, size_t size)
{
    ParsedPayload result;
    if (!bytes || size == 0)
    {
        result.reason = "empty_payload";
        return result;
    }

    PayloadHeader header;
    if (!ReadValue(bytes, size, 0, header))
    {
        result.reason = "header_truncated";
        return result;
    }

    result.version = header.version;
    result.totalBytes = header.totalBytes;
    result.payloadChecksum = header.payloadChecksum;
    result.rawByteChecksum = header.rawByteChecksum;
    result.rangeRecords = header.rangeRecordCount;
    result.inventoryRanges = header.inventoryRangeCount;
    result.itemGroups = header.itemGroupCount;
    result.itemRanges = header.itemRangeCount;
    result.rawBytes = header.rawBytes;
    result.attrNameRecords = header.attrNameRecordCount;
    result.attrNameBytes = header.attrNameBytes;
    result.oracleRecordCount =
        header.version >= 3 && (header.flags & kPayloadFlagWriteNodeOracle) ? header.reserved : 0;
    result.runId = header.runId;
    result.schemaHash = header.schemaHash;
    result.contentHash = header.contentHash;

    if (header.magic != kPayloadMagic)
    {
        result.reason = "bad_magic";
        return result;
    }
    if (header.version < kMinPayloadVersion || header.version > kPayloadVersion)
    {
        result.reason = "bad_version";
        return result;
    }
    if (header.headerBytes != sizeof(PayloadHeader))
    {
        result.reason = "bad_header_size";
        return result;
    }
    if (header.totalBytes != size)
    {
        result.reason = "size_mismatch";
        return result;
    }
    if ((header.flags & ~kPayloadFlagWriteNodeOracle) != 0)
    {
        result.reason = "bad_flags";
        return result;
    }
    if (header.version < 3 && (header.flags != 0 || header.reserved != 0))
    {
        result.reason = "legacy_reserved_nonzero";
        return result;
    }
    if (header.version >= 3 && (header.flags & kPayloadFlagWriteNodeOracle) == 0 && header.reserved != 0)
    {
        result.reason = "oracle_count_without_flag";
        return result;
    }

    const size_t recordBytes = static_cast<size_t>(header.rangeRecordCount) * sizeof(PayloadRangeRecord);
    const size_t attrNameTableOffset = sizeof(PayloadHeader) + recordBytes;
    const size_t attrNameRecordBytes =
        static_cast<size_t>(header.attrNameRecordCount) * sizeof(PayloadAttrNameTableRecord);
    const uint32_t oracleRecordCount =
        header.version >= 3 && (header.flags & kPayloadFlagWriteNodeOracle) ? header.reserved : 0;
    const size_t oracleTableOffset = attrNameTableOffset + attrNameRecordBytes;
    const size_t oracleRecordBytes =
        static_cast<size_t>(oracleRecordCount) * sizeof(PayloadWriteNodeOracleRecord);
    const size_t attrNameBytesOffset = oracleTableOffset + oracleRecordBytes;
    const size_t rawOffset = attrNameBytesOffset + header.attrNameBytes;
    if (rawOffset > size)
    {
        result.reason = "range_table_truncated";
        return result;
    }
    if (header.rawBytes != size - rawOffset)
    {
        result.reason = "raw_size_mismatch";
        return result;
    }
    if (header.inventoryRangeCount > header.rangeRecordCount ||
        header.itemRangeCount > header.rangeRecordCount ||
        header.inventoryRangeCount + header.itemRangeCount != header.rangeRecordCount)
    {
        result.reason = "range_count_mismatch";
        return result;
    }
    if (header.itemGroupCount > header.itemRangeCount)
    {
        result.reason = "item_group_count_mismatch";
        return result;
    }

    for (uint32_t i = 0; i < header.rangeRecordCount; ++i)
    {
        PayloadRangeRecord record;
        if (!ReadValue(bytes, size, sizeof(PayloadHeader) + static_cast<size_t>(i) * sizeof(PayloadRangeRecord), record))
        {
            result.reason = "range_record_truncated";
            return result;
        }
        if (record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::NodeBlock) &&
            record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::WriteNodeBlock) &&
            record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::AttrDataPool) &&
            record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::ChildIndexBlock) &&
            record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::WriteAttrVector) &&
            record.rangeKind != static_cast<uint32_t>(PayloadRangeKind::WriteChildVector))
        {
            result.reason = "unsupported_range_kind";
            return result;
        }
        if (record.groupKind != static_cast<uint32_t>(PayloadGroupKind::Inventory) &&
            record.groupKind != static_cast<uint32_t>(PayloadGroupKind::Item))
        {
            result.reason = "unsupported_group_kind";
            return result;
        }
        if (record.nodeCount == 0 || record.byteCount == 0)
        {
            result.reason = "empty_range";
            return result;
        }
        if (record.byteOffset > header.rawBytes || record.byteCount > header.rawBytes - record.byteOffset)
        {
            result.reason = "range_raw_bounds";
            return result;
        }
        if (record.rangeKind == static_cast<uint32_t>(PayloadRangeKind::NodeBlock) ||
            record.rangeKind == static_cast<uint32_t>(PayloadRangeKind::WriteNodeBlock))
            ++result.nodeRanges;
        else
            ++result.backingRanges;
        result.ranges.push_back(record);
    }

    if (header.attrNameRecordCount != 0)
    {
        if (attrNameBytesOffset > size || header.attrNameBytes > size - attrNameBytesOffset)
        {
            result.reason = "attr_name_table_truncated";
            return result;
        }

        for (uint32_t i = 0; i < header.attrNameRecordCount; ++i)
        {
            PayloadAttrNameTableRecord record;
            if (!ReadValue(
                    bytes,
                    size,
                    attrNameTableOffset + static_cast<size_t>(i) * sizeof(PayloadAttrNameTableRecord),
                    record))
            {
            result.reason = "attr_name_record_truncated";
            return result;
        }
            if (record.rangeIndex >= header.rangeRecordCount ||
                record.nameOffset > header.attrNameBytes ||
                record.nameBytes == 0 ||
                record.nameBytes > header.attrNameBytes - record.nameOffset)
            {
                result.reason = "attr_name_record_bounds";
                return result;
            }

            const size_t nameBegin = attrNameBytesOffset + record.nameOffset;
            const size_t nameEnd = nameBegin + record.nameBytes;
            if (nameEnd > size || bytes[nameEnd - 1] != '\0')
            {
                result.reason = "attr_name_not_terminated";
                return result;
            }

            CoopNativeFragmentPayload::PayloadAttrNameRecord parsed;
            parsed.rangeIndex = record.rangeIndex;
            parsed.ordinal = record.ordinal;
            parsed.type = record.type;
            parsed.sourceNameIndex = record.sourceNameIndex;
            parsed.token = record.token;
            parsed.name.assign(
                reinterpret_cast<const char*>(bytes + nameBegin),
                reinterpret_cast<const char*>(bytes + nameEnd - 1));
            result.attrNames.push_back(std::move(parsed));
        }
    }

    if (oracleRecordCount != 0)
    {
        if (oracleTableOffset > size || oracleRecordBytes > size - oracleTableOffset)
        {
            result.reason = "oracle_table_truncated";
            return result;
        }

        for (uint32_t i = 0; i < oracleRecordCount; ++i)
        {
            PayloadWriteNodeOracleRecord record;
            if (!ReadValue(
                    bytes,
                    size,
                    oracleTableOffset + static_cast<size_t>(i) * sizeof(PayloadWriteNodeOracleRecord),
                    record))
            {
                result.reason = "oracle_record_truncated";
                return result;
            }
            if (record.sourceRangeIndex >= header.rangeRecordCount)
            {
                result.reason = "oracle_range_bounds";
                return result;
            }
            const PayloadRangeRecord& range = result.ranges[record.sourceRangeIndex];
            if (!IsWriteRangeKind(range.rangeKind) || record.writeIndex != range.beginIndex)
            {
                result.reason = "oracle_range_mismatch";
                return result;
            }
            result.oracleRecords.push_back(record);
        }
    }

    const uint32_t expectedPayloadChecksum = UpdateFnv1a(
        kFnv1aOffsetBasis,
        bytes + sizeof(PayloadHeader),
        size - sizeof(PayloadHeader));
    if (expectedPayloadChecksum != header.payloadChecksum)
    {
        result.reason = "payload_checksum_mismatch";
        return result;
    }

    const uint32_t expectedRawChecksum = UpdateFnv1a(
        kFnv1aOffsetBasis,
        bytes + rawOffset,
        size - rawOffset);
    if (expectedRawChecksum != header.rawByteChecksum)
    {
        result.reason = "raw_checksum_mismatch";
        return result;
    }

    result.ok = true;
    result.reason = "ok";
    result.rawDataOffset = static_cast<uint32_t>(rawOffset);
    result.rawData.assign(bytes + rawOffset, bytes + size);
    return result;
}

ParsedPayload ParseInventoryPayload(const std::vector<uint8_t>& bytes)
{
    return ParseInventoryPayload(bytes.data(), bytes.size());
}

std::string BuildStatus(const BuildResult& result)
{
    std::ostringstream out;
    out << (result.ok ? 1 : 0)
        << "/" << (result.reason.empty() ? "-" : result.reason)
        << "/bytes=" << result.bytes.size()
        << "/ranges=" << result.rangeRecords
        << "/invRanges=" << result.inventoryRanges
        << "/itemGroups=" << result.itemGroups
        << "/itemRanges=" << result.itemRanges
        << "/raw=" << result.rawBytes
        << "/nodeRanges=" << result.nodeRanges
        << "/backingRanges=" << result.backingRanges
        << "/attrNames=" << result.attrNameRecords
        << "/attrNameBytes=" << result.attrNameBytes
        << "/attrNameFail=" << result.attrNameFailures
        << "/oracle=" << result.oracleRecordCount
        << "/payloadCrc=" << Hex32(result.payloadChecksum)
        << "/rawCrc=" << Hex32(result.rawByteChecksum)
        << "/schema=" << Hex64(result.schemaHash)
        << "/content=" << Hex64(result.contentHash);
    return out.str();
}

std::string BuildStatus(const ParsedPayload& result)
{
    std::ostringstream out;
    out << (result.ok ? 1 : 0)
        << "/" << (result.reason.empty() ? "-" : result.reason)
        << "/version=" << result.version
        << "/bytes=" << result.totalBytes
        << "/ranges=" << result.rangeRecords
        << "/invRanges=" << result.inventoryRanges
        << "/itemGroups=" << result.itemGroups
        << "/itemRanges=" << result.itemRanges
        << "/raw=" << result.rawBytes
        << "/nodeRanges=" << result.nodeRanges
        << "/backingRanges=" << result.backingRanges
        << "/attrNames=" << result.attrNameRecords
        << "/attrNameBytes=" << result.attrNameBytes
        << "/oracle=" << result.oracleRecordCount
        << "/payloadCrc=" << Hex32(result.payloadChecksum)
        << "/rawCrc=" << Hex32(result.rawByteChecksum)
        << "/schema=" << Hex64(result.schemaHash)
        << "/content=" << Hex64(result.contentHash);
    return out.str();
}

bool WritePayloadFile(const std::filesystem::path& path, const BuildResult& result, std::string* outReason)
{
    if (!result.ok || result.bytes.empty())
    {
        if (outReason)
            *outReason = "payload_not_ok";
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        if (outReason)
            *outReason = "mkdir_failed";
        return false;
    }

    const std::filesystem::path tempPath = path.string() + ".tmp";
    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (outReason)
            *outReason = "open_failed";
        return false;
    }

    output.write(reinterpret_cast<const char*>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));
    output.close();
    if (!output)
    {
        if (outReason)
            *outReason = "write_failed";
        return false;
    }

    error.clear();
    std::filesystem::rename(tempPath, path, error);
    if (!error)
    {
        if (outReason)
            *outReason = "ok";
        return true;
    }

    error.clear();
    std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, error);
    std::error_code removeError;
    std::filesystem::remove(tempPath, removeError);
    if (error)
    {
        if (outReason)
            *outReason = "copy_failed";
        return false;
    }

    if (outReason)
        *outReason = "ok";
    return true;
}
}
