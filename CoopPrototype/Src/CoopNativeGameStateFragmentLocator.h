#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct NativeInventoryFragmentScopeInfo
{
    uint64_t scopeSeq = 0;
    std::uintptr_t inventoryPtr = 0;
    std::uintptr_t serializerPtr = 0;
    bool reading = false;
    int target = 0;
    std::string sectionName;
};

struct NativeItemFragmentScopeInfo
{
    uint64_t scopeSeq = 0;
    uint64_t inventoryScopeSeq = 0;
    std::uintptr_t itemPtr = 0;
    unsigned itemEntityId = 0;
    std::uintptr_t serializerPtr = 0;
    bool reading = false;
    int target = 0;
    std::string sectionName;
};

struct NativeDecodedStoreNodeInfo
{
    uint32_t nodeIndex = 0;
    uint32_t nodeId = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    std::uintptr_t storePtr = 0;
    std::uintptr_t nodePtr = 0;
    std::uintptr_t nodeBlockBegin = 0;
    std::uintptr_t nodeBlockEnd = 0;
    uint32_t nodeCount = 0;
    std::uintptr_t childIndexBlockBegin = 0;
    std::uintptr_t childIndexBlockEnd = 0;
    std::uintptr_t attrStringBase = 0;
    std::uintptr_t attrNameOffsetTable = 0;
    std::uintptr_t attrTokenContext = 0;
    std::uintptr_t attrTokenIndexBase = 0;
    std::uintptr_t attrTokenBase = 0;
    std::uintptr_t childNameDataBase = 0;
    std::uintptr_t childNameResolverContext = 0;
    std::uintptr_t childNameOffsetTable = 0;
    std::uintptr_t attrVectorBegin = 0;
    std::uintptr_t attrVectorEnd = 0;
    std::uintptr_t childVectorBegin = 0;
    std::uintptr_t childVectorEnd = 0;
    bool readStore = false;
    bool nodeReadable = false;
    bool nodeValid = false;
    bool inventoryNode = false;
    bool itemNode = false;
    std::string source;
    std::string op;
    std::string path;
};

class CoopNativeGameStateFragmentLocator final
{
public:
    struct RawNodeRange
    {
        std::uintptr_t beginPtr = 0;
        std::uintptr_t endPtr = 0;
        std::uintptr_t storePtr = 0;
        std::uintptr_t nodeBlockBegin = 0;
        std::uintptr_t nodeBlockEnd = 0;
        uint32_t beginIndex = 0;
        uint32_t count = 0;
        bool valid = false;
    };

    enum class BackingRangeKind : uint32_t
    {
        AttrDataPool = 2,
        ChildIndexBlock = 3,
        WriteAttrVector = 4,
        WriteChildVector = 5,
    };

    struct RawBackingRange
    {
        BackingRangeKind kind = BackingRangeKind::AttrDataPool;
        std::uintptr_t beginPtr = 0;
        std::uintptr_t endPtr = 0;
        std::uintptr_t storePtr = 0;
        std::uintptr_t basePtr = 0;
        uint32_t ownerNodeIndex = 0;
        uint32_t ownerNodeId = 0;
        uint32_t cursor = 0;
        uint32_t count = 0;
        bool valid = false;
    };

    struct FragmentNodeRef
    {
        uint32_t nodeIndex = 0;
        uint32_t nodeId = 0;
        uint32_t attrCursor = 0;
        uint32_t attrCount = 0;
        uint32_t childCursor = 0;
        uint32_t childCount = 0;
        std::uintptr_t storePtr = 0;
        std::uintptr_t nodePtr = 0;
        std::uintptr_t nodeBlockBegin = 0;
        std::uintptr_t nodeBlockEnd = 0;
        uint32_t nodeCount = 0;
        std::uintptr_t childIndexBlockBegin = 0;
        std::uintptr_t childIndexBlockEnd = 0;
        std::uintptr_t attrStringBase = 0;
        std::uintptr_t attrNameOffsetTable = 0;
        std::uintptr_t attrTokenContext = 0;
        std::uintptr_t attrTokenIndexBase = 0;
        std::uintptr_t attrTokenBase = 0;
        std::uintptr_t childNameDataBase = 0;
        std::uintptr_t childNameResolverContext = 0;
        std::uintptr_t childNameOffsetTable = 0;
        std::uintptr_t attrVectorBegin = 0;
        std::uintptr_t attrVectorEnd = 0;
        std::uintptr_t childVectorBegin = 0;
        std::uintptr_t childVectorEnd = 0;
        bool readStore = false;
        bool valid = false;
    };

    struct ItemFragmentRef
    {
        unsigned entityId = 0;
        unsigned sourceEntityId = 0;
        uint64_t sourceScopeSeq = 0;
        FragmentNodeRef node;
        RawNodeRange nodeRange;
        std::vector<FragmentNodeRef> observedNodes;
        std::vector<RawNodeRange> observedNodeRanges;
    };

    struct NativeInventoryFragmentBundle
    {
        bool ok = false;
        std::string reason;
        uint64_t runId = 0;
        uint64_t schemaHash = 0;
        uint64_t contentHash = 0;
        FragmentNodeRef inventoryCellValueNode;
        RawNodeRange inventoryCellValueRange;
        std::vector<FragmentNodeRef> inventoryObservedNodes;
        std::vector<RawNodeRange> inventoryObservedNodeRanges;
        std::vector<ItemFragmentRef> itemFragments;
        std::vector<unsigned> inventoryEntityIds;
        std::vector<unsigned> resolvedItemEntityIds;
        std::vector<unsigned> missingItemEntityIds;
        std::vector<unsigned> knownItemEntityIds;
        uint32_t inventoryEntityCount = 0;
        uint32_t missingItemReferences = 0;
        uint32_t observedInventoryNodeCount = 0;
        uint32_t observedInventoryRangeCount = 0;
        uint32_t observedItemNodeCount = 0;
        uint32_t observedItemRangeCount = 0;
    };

    struct RawRangeByteCopy
    {
        RawNodeRange range;
        uint32_t byteOffset = 0;
        uint32_t byteCount = 0;
        bool ok = false;
        std::string reason;
    };

    struct RawBackingRangeByteCopy
    {
        RawBackingRange range;
        uint32_t byteOffset = 0;
        uint32_t byteCount = 0;
        bool ok = false;
        std::string reason;
    };

    struct ItemFragmentByteCopy
    {
        unsigned entityId = 0;
        std::vector<RawRangeByteCopy> ranges;
        std::vector<RawBackingRangeByteCopy> backingRanges;
    };

    struct NativeInventoryFragmentByteCapture
    {
        bool ok = false;
        std::string reason;
        NativeInventoryFragmentBundle bundle;
        std::vector<uint8_t> bytes;
        std::vector<RawRangeByteCopy> inventoryRanges;
        std::vector<RawBackingRangeByteCopy> inventoryBackingRanges;
        std::vector<ItemFragmentByteCopy> itemRanges;
        uint32_t capturedRanges = 0;
        uint32_t failedRanges = 0;
        uint32_t capturedBackingRanges = 0;
        uint32_t failedBackingRanges = 0;
    };

    void Reset();
    void BeginRun(uint64_t runId, const char* reason);
    void OnGameStateSerializer(const void* serializerPtr, const std::string& sectionName, bool reading, int target);
    void OnLocalPlayerInventoryScopeEnter(const NativeInventoryFragmentScopeInfo& info);
    void OnLocalPlayerInventoryScopeExit(uint64_t scopeSeq);
    void OnPlayerInventoryItemScopeEnter(const NativeItemFragmentScopeInfo& info);
    void OnPlayerInventoryItemScopeExit(uint64_t scopeSeq);
    void OnDecodedStoreNode(const NativeDecodedStoreNodeInfo& info);
    void OnInventoryCellEntityId(unsigned entityId, const std::string& source, const std::string& path);
    void OnReadStoreItemNodeForEntity(unsigned entityId, const NativeDecodedStoreNodeInfo& info);
    void OnReadStoreItemObservedNodeForEntity(unsigned entityId, const NativeDecodedStoreNodeInfo& info);
    void MergeReadStoreBundle(const NativeInventoryFragmentBundle& bundle, const char* reason);
    void FinalizeRun(const char* reason);

    std::string BuildStatus() const;
    std::string BuildFragmentMapStatus() const;
    std::string BuildFragmentBundleStatus() const;
    NativeInventoryFragmentBundle BuildFragmentBundle() const;
    NativeInventoryFragmentByteCapture CaptureFragmentBytes(uint32_t maxBytes = 4u * 1024u * 1024u) const;
    std::string BuildFragmentByteCaptureStatus(uint32_t maxBytes = 4u * 1024u * 1024u) const;
    std::string GetLastEvent() const { return m_lastEvent; }

private:
    struct FragmentByteSnapshot
    {
        bool ok = false;
        std::string reason;
        std::vector<uint8_t> bytes;
        std::vector<RawRangeByteCopy> ranges;
        std::vector<RawBackingRangeByteCopy> backingRanges;
        uint32_t capturedRanges = 0;
        uint32_t failedRanges = 0;
        uint32_t capturedBackingRanges = 0;
        uint32_t failedBackingRanges = 0;
    };

    void SetLastEvent(const std::string& event);
    void RecomputeReferenceCompleteness();
    static FragmentNodeRef MakeNodeRef(const NativeDecodedStoreNodeInfo& info);
    static RawNodeRange MakeSingleNodeRange(const FragmentNodeRef& node);
    static void AddObservedNode(std::vector<FragmentNodeRef>& nodes, const FragmentNodeRef& node);
    static std::vector<RawNodeRange> BuildContiguousNodeRanges(const std::vector<FragmentNodeRef>& nodes);
    static bool CaptureRangeBytes(
        const RawNodeRange& range,
        std::vector<uint8_t>& bytes,
        uint32_t maxBytes,
        RawRangeByteCopy& outCopy);
    static bool CaptureBackingRangeBytes(
        const RawBackingRange& range,
        std::vector<uint8_t>& bytes,
        uint32_t maxBytes,
        RawBackingRangeByteCopy& outCopy);
    static std::vector<RawBackingRange> BuildBackingRanges(const std::vector<FragmentNodeRef>& nodes);
    static FragmentByteSnapshot CaptureNodeSnapshot(
        const std::vector<FragmentNodeRef>& nodes,
        uint32_t maxBytes);
    void AddItemNodeForEntity(unsigned entityId, const FragmentNodeRef& node);
    void AddAnonymousItemNode(uint64_t scopeSeq, const FragmentNodeRef& node);
    std::vector<ItemFragmentRef> BuildResolvedItemFragments() const;
    bool IsCurrentInventoryScope() const;
    bool IsCurrentItemScope() const;

    uint64_t m_runId = 0;
    uint32_t m_gameStateSerializers = 0;
    uint32_t m_inventoryScopes = 0;
    uint32_t m_itemScopes = 0;
    uint32_t m_inventoryNodes = 0;
    uint32_t m_itemNodes = 0;
    uint32_t m_inventoryCellValueNodes = 0;
    uint32_t m_itemValueNodes = 0;
    uint32_t m_references = 0;
    uint32_t m_missingReferences = 0;
    uint32_t m_inventoryCellEntityIds = 0;
    uint32_t m_distinctInventoryEntityIds = 0;
    uint32_t m_matchedItemEntityScopes = 0;
    uint32_t m_missingItemEntityScopes = 0;
    uint32_t m_schemaOk = 0;
    uint32_t m_lastNodeIndex = 0;
    uint32_t m_lastNodeId = 0;
    uint32_t m_lastAttrCount = 0;
    uint32_t m_lastChildCount = 0;
    uint64_t m_activeInventoryScopeSeq = 0;
    uint64_t m_activeItemScopeSeq = 0;
    unsigned m_activeItemEntityId = 0;
    std::uintptr_t m_lastSerializerPtr = 0;
    FragmentNodeRef m_inventoryCellValueNode;
    std::vector<FragmentNodeRef> m_inventoryObservedNodes;
    std::unordered_map<unsigned, FragmentNodeRef> m_itemNodesByEntityId;
    std::unordered_map<unsigned, int> m_itemRootScoresByEntityId;
    std::unordered_map<unsigned, std::string> m_itemRootSummariesByEntityId;
    std::unordered_map<unsigned, std::vector<FragmentNodeRef>> m_itemObservedNodesByEntityId;
    std::unordered_map<unsigned, std::vector<std::string>> m_itemObservedSummariesByEntityId;
    std::unordered_map<unsigned, FragmentByteSnapshot> m_itemByteSnapshotsByEntityId;
    std::vector<unsigned> m_itemNodeEntityOrder;
    std::unordered_set<unsigned> m_inventoryEntityIds;
    std::vector<unsigned> m_inventoryEntityOrder;
    std::unordered_set<unsigned> m_itemScopeEntityIds;
    std::unordered_map<uint64_t, FragmentNodeRef> m_anonymousItemNodesByScopeSeq;
    std::unordered_map<uint64_t, int> m_anonymousItemRootScoresByScopeSeq;
    std::unordered_map<uint64_t, std::string> m_anonymousItemRootSummariesByScopeSeq;
    std::unordered_map<uint64_t, std::vector<FragmentNodeRef>> m_anonymousItemObservedNodesByScopeSeq;
    std::unordered_map<uint64_t, std::vector<std::string>> m_anonymousItemObservedSummariesByScopeSeq;
    std::unordered_map<uint64_t, FragmentByteSnapshot> m_anonymousItemByteSnapshotsByScopeSeq;
    std::vector<uint64_t> m_anonymousItemScopeOrder;
    std::string m_lastEvent = "-";
};
