#pragma once

#include "CoopSaveStoreDecoder.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace CoopNativeSaveStoreApi
{
enum class StoreAccess : uint32_t
{
    ReadOnly = 1,
    WriteDisabled = 2,
};

struct StoreHandle
{
    std::uintptr_t store = 0;
    std::uintptr_t nodeBlockBegin = 0;
    std::uintptr_t nodeBlockEnd = 0;
    std::uintptr_t attrDataBase = 0;
    std::uintptr_t attrStringBase = 0;
    std::uintptr_t attrNameOffsetTable = 0;
    std::uintptr_t attrTokenContext = 0;
    std::uintptr_t attrTokenIndexBase = 0;
    std::uintptr_t attrTokenBase = 0;
    std::uintptr_t childNameDataBase = 0;
    std::uintptr_t childNameResolverContext = 0;
    std::uintptr_t childNameOffsetTable = 0;
    uint32_t nodeCount = 0;
    bool readStore = false;
    bool valid = false;
};

struct NodeView
{
    std::uintptr_t nodePtr = 0;
    uint32_t nodeIndex = 0;
    uint32_t nodeId = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    std::uintptr_t childIndexBlockBegin = 0;
    std::uintptr_t childIndexBlockEnd = 0;
    bool valid = false;
};

struct CheckResult
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    StoreHandle store;
    NodeView nativeNode;
    NodeView decodedNode;
    uint32_t nameToken = 0;
    std::uintptr_t nativeNamePtr = 0;
    std::uintptr_t computedNamePtr = 0;
    uint32_t nameChecks = 0;
    uint32_t nameMismatches = 0;
    uint32_t attrChecks = 0;
    uint32_t attrMismatches = 0;
    uint32_t attrCursor = 0;
    uint32_t attrTokenCursor = 0;
    uint32_t attrOrdinal = 0;
    uint16_t nativeAttrToken = 0;
    uint16_t computedAttrToken = 0;
    std::uintptr_t nativeAttrNamePtr = 0;
    std::uintptr_t computedAttrNamePtr = 0;
    uint32_t nativeReadValueChecks = 0;
    uint32_t nativeReadValueFailures = 0;
    uint32_t nativeReadValueType = 0;
    uint64_t nativeReadValueBits = 0;
    std::string nativeReadValueReason;
    uint32_t loadSectionChecks = 0;
    uint32_t loadSectionGuards = 0;
    bool gameStateSectionFound = false;
    int32_t gameStateSectionNodeIndex = -1;
    uint32_t gameStateSectionNodeId = 0;
    std::uintptr_t gameStateSectionNodePtr = 0;
    std::string gameStateSectionReason;
    uint32_t mismatches = 0;
    uint32_t guards = 0;
};

struct WriteStoreProbeResult
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    std::uintptr_t storeOwner = 0;
    std::uintptr_t stackStore = 0;
    std::uintptr_t resolvedNode = 0;
    std::uintptr_t nodeByIndex = 0;
    int32_t nodeIndex = -1;
    int32_t generation = 0;
    uint32_t nameToken = 0;
    uint32_t childCount = 0;
    uint32_t attrRecords = 0;
    uint32_t childRecords = 0;
    uint32_t finalizeCursor = 0;
    uint8_t valid = 0;
    uint8_t finalized = 0;
    uint32_t functionChecks = 0;
    uint32_t nativeLookups = 0;
    uint32_t mismatches = 0;
    uint32_t guards = 0;
};

struct WriteAllocatorProbeResult
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    std::uintptr_t storeOwner = 0;
    std::uintptr_t stackStore = 0;
    std::uintptr_t rootNode = 0;
    int32_t nodeIndex = -1;
    int32_t generation = 0;
    std::uintptr_t nodeTableBegin = 0;
    std::uintptr_t nodeTableEnd = 0;
    std::uintptr_t nodeTableCapacity = 0;
    uint32_t nodeTableSlots = 0;
    uint32_t nextFreeNodeIndex = 0;
    uint32_t storeGeneration = 0;
    uint32_t finalizeCursor = 0;
    uint32_t availableNodeSlots = 0;
    uint32_t requestedNodeAllocs = 0;
    uint32_t requestedBackingAllocs = 0;
    uint32_t requestedEntityAllocs = 0;
    uint32_t rootChildCount = 0;
    uint32_t rootAttrRecords = 0;
    uint32_t rootChildRecords = 0;
    uint32_t functionChecks = 0;
    uint32_t functionsReady = 0;
    uint32_t readyCreateNode = 0;
    uint32_t readyCreateChildGroup = 0;
    uint32_t readyAppendAttribute = 0;
    uint32_t readyFinalizeNode = 0;
    uint32_t readySaveStackPush = 0;
    uint32_t readyNodeByIndex = 0;
    uint32_t readyStackEntryToNode = 0;
    uint32_t mismatches = 0;
    uint32_t guards = 0;
};

struct WriteStoreBuilderBackendStatus
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    uint32_t functionChecks = 0;
    uint32_t functionsReady = 0;
    uint32_t readyCreateNode = 0;
    uint32_t readyCreateChildGroup = 0;
    uint32_t readyAppendAttribute = 0;
    uint32_t readyFinalizeNode = 0;
    uint32_t readySaveStackPush = 0;
    uint32_t readyNodeByIndex = 0;
    uint32_t readyStackEntryToNode = 0;
    uint32_t guards = 0;
};

struct ScratchActiveSaveWriterProbeResult
{
    bool attempted = false;
    bool ok = false;
    bool constructed = false;
    bool destroyed = false;
    std::string reason;
    std::uintptr_t activeSave = 0;
    std::uintptr_t vtable = 0;
    std::uintptr_t virtualDtor = 0;
    std::uintptr_t writeStore = 0;
    std::uintptr_t sectionCacheBegin = 0;
    std::uintptr_t sectionCacheEnd = 0;
    std::uintptr_t sectionCacheCapacity = 0;
    uint32_t functionChecks = 0;
    uint32_t functionsReady = 0;
    uint32_t readyFactory = 0;
    uint32_t readyVirtualDtor = 0;
    uint32_t writeStoreReadable = 0;
    uint32_t writeStoreWritable = 0;
    uint32_t sectionCacheEmpty = 0;
    uint32_t guards = 0;
};

struct ScratchActiveSaveSectionProbeResult
{
    bool attempted = false;
    bool ok = false;
    bool constructed = false;
    bool initialized = false;
    bool destroyed = false;
    bool addSectionCalled = false;
    bool sectionAdded = false;
    bool serializerOk = false;
    std::string reason;
    std::uintptr_t activeSave = 0;
    std::uintptr_t vtable = 0;
    std::uintptr_t virtualDtor = 0;
    std::uintptr_t writeStore = 0;
    std::uintptr_t serializerImpl = 0;
    std::uintptr_t sectionCacheBeginBefore = 0;
    std::uintptr_t sectionCacheEndBefore = 0;
    std::uintptr_t sectionCacheCapacityBefore = 0;
    std::uintptr_t sectionCacheBeginAfter = 0;
    std::uintptr_t sectionCacheEndAfter = 0;
    std::uintptr_t sectionCacheCapacityAfter = 0;
    uint32_t sectionCacheEntriesAfter = 0;
    uint32_t rootChildCount = 0;
    uint32_t rootAttrRecords = 0;
    uint32_t rootChildRecords = 0;
    uint32_t functionChecks = 0;
    uint32_t functionsReady = 0;
    uint32_t readyFactory = 0;
    uint32_t readyInit = 0;
    uint32_t readyAddSection = 0;
    uint32_t readyVirtualDtor = 0;
    uint32_t readyCopyRoot = 0;
    uint32_t readyResolveRoot = 0;
    uint32_t guards = 0;
};

struct AttrTokenView
{
    uint32_t attrCursor = 0;
    uint32_t attrOrdinal = 0;
    uint16_t token = 0;
    uint16_t tokenByteOffset = 0;
    std::uintptr_t tokenAddress = 0;
    bool valid = false;
};

struct ReadAttributeView
{
    std::array<uint8_t, 32> nativeValue = {};
    uint32_t attrCursor = 0;
    uint32_t type = 0;
    uint32_t nameIndex = 0;
    uint16_t token = 0;
    bool valid = false;
};

struct ReadAttributeRecord
{
    ReadAttributeView view;
    uint32_t ordinal = 0;
    uint32_t valueCursor = 0;
    uint32_t valueByteCount = 0;
    uint64_t valueBits = 0;
    uint32_t nativeU32 = 0;
    std::string name;
    bool nameResolved = false;
    bool valueReadable = false;
    bool nativeU32Readable = false;
};

StoreHandle MakeReadStoreHandle(const CoopSaveStoreDecoder::StoreMap& map);
bool TryReadNodeByIndex(const StoreHandle& store, uint32_t nodeIndex, NodeView& outNode, std::string* outReason = nullptr);
bool TryFindReadSection(
    const StoreHandle& store,
    const char* sectionName,
    NodeView& outNode,
    std::string* outReason = nullptr);
bool TryEnumerateChildEntryIndices(
    const NodeView& node,
    std::vector<uint32_t>& outChildEntryIndices,
    uint32_t maxEntries = 256,
    std::string* outReason = nullptr);
bool TryReadChildEntryByIndex(
    const StoreHandle& store,
    uint32_t childEntryIndex,
    NodeView& outNode,
    std::string* outReason = nullptr);
bool TryResolveReadNodeName(
    const NodeView& node,
    const char*& outName,
    std::string* outReason = nullptr);
bool TryResolveChildNameToken(const StoreHandle& store, uint32_t nameToken, const char*& outName, std::string* outReason = nullptr);
bool TryFindChildGroup(
    const StoreHandle& store,
    const NodeView& parentNode,
    const char* childName,
    NodeView& outNode,
    std::string* outReason = nullptr);
bool TryReadAttrTokenView(
    const StoreHandle& store,
    uint32_t attrTokenCursor,
    uint32_t attrOrdinal,
    AttrTokenView& outView,
    std::string* outReason = nullptr);
bool TryReadAttrNameToken(const StoreHandle& store, uint32_t attrTokenCursor, uint32_t attrOrdinal, uint16_t& outToken, std::string* outReason = nullptr);
bool TryResolveAttrName(const StoreHandle& store, uint32_t attrValueCursor, uint16_t attrToken, const char*& outName, std::string* outReason = nullptr);
bool TryFindReadAttribute(
    const StoreHandle& store,
    const NodeView& node,
    const char* attrName,
    ReadAttributeView& outAttribute,
    std::string* outReason = nullptr);
bool TryEnumerateReadAttributes(
    const StoreHandle& store,
    const NodeView& node,
    std::vector<ReadAttributeRecord>& outAttributes,
    uint32_t maxAttrs = 64,
    std::string* outReason = nullptr);
bool TryReadU32Attribute(const ReadAttributeView& attribute, uint32_t& outValue, std::string* outReason = nullptr);
bool TryReadI32Attribute(const ReadAttributeView& attribute, int32_t& outValue, std::string* outReason = nullptr);
bool TryReadF32Attribute(const ReadAttributeView& attribute, float& outValue, std::string* outReason = nullptr);
bool TryReadU64Attribute(const ReadAttributeView& attribute, uint64_t& outValue, std::string* outReason = nullptr);
bool TryReadBoolAttribute(const ReadAttributeView& attribute, bool& outValue, std::string* outReason = nullptr);
bool TryRewriteSameAttrNameToken(
    const StoreHandle& store,
    uint32_t attrTokenCursor,
    uint32_t attrOrdinal,
    AttrTokenView* outView = nullptr,
    std::string* outReason = nullptr);
bool TryMaterializeWriteAttrToken(const void* writeAttrRecord, uint16_t& outToken, std::string* outReason = nullptr);
bool TryGetAttrTypePayloadSize(uint32_t attrType, int32_t& outBytes, std::string* outReason = nullptr);
bool TryResolveWriteStoreName(
    std::uintptr_t writeStore,
    uint32_t nameIndex,
    const char*& outName,
    std::string* outReason = nullptr);
CheckResult CheckReadNodeByIndex(const CoopSaveStoreDecoder::StoreMap& map);
std::string BuildCheckStatus(const CheckResult& result);
WriteStoreProbeResult ProbeWriteStoreTrampolines(
    void* storeOwner,
    const std::array<std::uintptr_t, 3>& stackEntry);
std::string BuildWriteStoreProbeStatus(const WriteStoreProbeResult& result);
WriteAllocatorProbeResult ProbeWriteAllocatorReadiness(
    void* storeOwner,
    const std::array<std::uintptr_t, 3>& stackEntry,
    uint32_t requestedNodeAllocs,
    uint32_t requestedBackingAllocs,
    uint32_t requestedEntityAllocs);
std::string BuildWriteAllocatorProbeStatus(const WriteAllocatorProbeResult& result);
WriteStoreBuilderBackendStatus CheckWriteStoreBuilderBackend();
std::string BuildWriteStoreBuilderBackendStatus(const WriteStoreBuilderBackendStatus& result);
bool IsWriteStoreBuilderBackendAvailable();
ScratchActiveSaveWriterProbeResult ProbeScratchActiveSaveWriter();
std::string BuildScratchActiveSaveWriterProbeStatus(const ScratchActiveSaveWriterProbeResult& result);
bool IsScratchActiveSaveWriterProbeEnabled();
ScratchActiveSaveSectionProbeResult ProbeScratchActiveSaveSectionWriter();
std::string BuildScratchActiveSaveSectionProbeStatus(const ScratchActiveSaveSectionProbeResult& result);
bool IsScratchActiveSaveSectionProbeEnabled();

bool IsWriteApiEnabled();
bool IsWriteProbeEnabled();
bool IsWriteAllocatorProbeEnabled();
}
