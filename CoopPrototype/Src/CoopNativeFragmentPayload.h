#pragma once

#include "CoopNativeGameStateFragmentLocator.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace CoopNativeFragmentPayload
{
constexpr uint32_t kPayloadMagic = 0x50464E43; // CNFP
constexpr uint16_t kPayloadVersion = 3;
constexpr uint16_t kMinPayloadVersion = 2;
constexpr uint32_t kPayloadFlagWriteNodeOracle = 0x1;

enum class PayloadGroupKind : uint32_t
{
    Inventory = 1,
    Item = 2,
};

enum class PayloadRangeKind : uint32_t
{
    NodeBlock = 1,
    AttrDataPool = 2,
    ChildIndexBlock = 3,
    WriteAttrVector = 4,
    WriteChildVector = 5,
    WriteNodeBlock = 6,
};

struct PayloadRangeRecord
{
    uint32_t groupKind = 0;
    uint32_t rangeKind = 0;
    uint32_t entityId = 0;
    uint32_t beginIndex = 0;
    uint32_t nodeCount = 0;
    uint32_t byteOffset = 0;
    uint32_t byteCount = 0;
    uint32_t firstNodeId = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
};

struct PayloadAttrNameRecord
{
    uint32_t rangeIndex = 0;
    uint32_t ordinal = 0;
    uint32_t type = 0;
    uint32_t sourceNameIndex = 0;
    uint32_t token = 0;
    std::string name;
};

struct PayloadWriteNodeOracleInputRecord
{
    std::uintptr_t store = 0;
    uint32_t writeIndex = 0;
    uint32_t finalIndex = 0;
    uint32_t nameToken = 0;
    uint32_t attrRecords = 0;
    uint32_t childRecords = 0;
    uint32_t childCount = 0;
    uint32_t flags = 0;
};

struct PayloadWriteNodeOracleRecord
{
    uint32_t sourceRangeIndex = 0;
    uint32_t writeIndex = 0;
    uint32_t finalIndex = 0;
    uint32_t nameToken = 0;
    uint32_t attrRecords = 0;
    uint32_t childRecords = 0;
    uint32_t childCount = 0;
    uint32_t flags = 0;
};

struct BuildResult
{
    bool ok = false;
    std::string reason;
    std::vector<uint8_t> bytes;
    uint32_t payloadChecksum = 0;
    uint32_t rawByteChecksum = 0;
    uint32_t rangeRecords = 0;
    uint32_t inventoryRanges = 0;
    uint32_t itemGroups = 0;
    uint32_t itemRanges = 0;
    uint32_t rawBytes = 0;
    uint32_t nodeRanges = 0;
    uint32_t backingRanges = 0;
    uint32_t attrNameRecords = 0;
    uint32_t attrNameBytes = 0;
    uint32_t attrNameFailures = 0;
    uint32_t oracleRecordCount = 0;
    uint64_t runId = 0;
    uint64_t schemaHash = 0;
    uint64_t contentHash = 0;
};

struct ParsedPayload
{
    bool ok = false;
    std::string reason;
    uint16_t version = 0;
    uint32_t totalBytes = 0;
    uint32_t payloadChecksum = 0;
    uint32_t rawByteChecksum = 0;
    uint32_t rangeRecords = 0;
    uint32_t inventoryRanges = 0;
    uint32_t itemGroups = 0;
    uint32_t itemRanges = 0;
    uint32_t rawBytes = 0;
    uint32_t rawDataOffset = 0;
    uint32_t nodeRanges = 0;
    uint32_t backingRanges = 0;
    uint32_t attrNameRecords = 0;
    uint32_t attrNameBytes = 0;
    uint32_t oracleRecordCount = 0;
    uint64_t runId = 0;
    uint64_t schemaHash = 0;
    uint64_t contentHash = 0;
    std::vector<PayloadRangeRecord> ranges;
    std::vector<PayloadAttrNameRecord> attrNames;
    std::vector<PayloadWriteNodeOracleRecord> oracleRecords;
    std::vector<uint8_t> rawData;
};

BuildResult BuildInventoryPayload(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentByteCapture& capture);
BuildResult BuildInventoryPayload(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentByteCapture& capture,
    const std::vector<PayloadWriteNodeOracleInputRecord>& oracleInputs);
ParsedPayload ParseInventoryPayload(const uint8_t* bytes, size_t size);
ParsedPayload ParseInventoryPayload(const std::vector<uint8_t>& bytes);
std::string BuildStatus(const BuildResult& result);
std::string BuildStatus(const ParsedPayload& result);
bool WritePayloadFile(const std::filesystem::path& path, const BuildResult& result, std::string* outReason = nullptr);
}
