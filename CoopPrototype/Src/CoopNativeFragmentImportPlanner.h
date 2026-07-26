#pragma once

#include "CoopNativeFragmentPayload.h"
#include "CoopNativeGameStateFragmentLocator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CoopNativeFragmentImportPlanner
{
struct TargetNodeShape
{
    uint32_t nodeIndex = 0;
    uint32_t nodeId = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    uint32_t groupKind = 0;
    uint32_t entityId = 0;
    uint32_t itemOrdinal = 0xFFFFFFFFu;
    bool readStore = false;
};

struct TargetInventoryFragmentBundle
{
    bool ok = false;
    std::string reason;
    uint64_t targetGameStateSerializerFingerprint = 0;
    uint64_t targetSchemaHash = 0;
    uint64_t targetContentHash = 0;
    uint32_t inventoryNodes = 0;
    uint32_t inventoryRanges = 0;
    uint32_t itemTargets = 0;
    uint32_t inventoryEntityCount = 0;
    uint32_t missingRefs = 0;
    uint32_t observedAttrVectors = 0;
    uint32_t observedChildVectors = 0;
    uint32_t readStoreNodes = 0;
    uint32_t writeStoreNodes = 0;
    uint32_t targetStoreNodes = 0;
    std::vector<TargetNodeShape> nodes;
    std::vector<TargetNodeShape> inventoryRootNodes;
    std::vector<TargetNodeShape> itemRootNodes;
    std::vector<uint32_t> inventoryEntityIds;
    std::vector<uint32_t> resolvedItemEntityIds;
    std::vector<uint32_t> missingItemEntityIds;
    std::vector<uint32_t> knownItemEntityIds;
};

struct StoreRangeRewriteOp
{
    enum class Kind : uint8_t
    {
        ReplaceNodeBlockRange,
        ReplaceOrAppendAttrVector,
        ReplaceOrAppendChildVector,
        RebaseAttrCursor,
        RebaseChildCursor,
        RebaseChildEntryIndex,
        ReplaceItemSubtree,
    };

    Kind kind = Kind::ReplaceNodeBlockRange;
    uint32_t sourceRangeIndex = 0;
    uint32_t sourceGroupKind = 0;
    uint32_t sourceRangeKind = 0;
    uint32_t sourceEntityId = 0;
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t targetNodeIndex = 0;
    uint32_t targetGroupKind = 0;
    uint32_t targetEntityId = 0;
    uint32_t targetItemOrdinal = 0xFFFFFFFFu;
    uint32_t sourceOffset = 0;
    uint32_t byteSize = 0;
    uint32_t targetCursorBefore = 0;
    uint32_t targetCursorAfter = 0;
    bool requiresInsert = false;
    bool requiresCursorRebase = false;
    bool materializerInputOnly = false;
    bool shapeCompatible = false;
    bool supported = false;
};

struct NativeFragmentImportPlan
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    uint64_t sourceSchemaHash = 0;
    uint64_t targetSchemaHash = 0;
    uint32_t rewriteOps = 0;
    uint32_t unsupportedOps = 0;
    uint32_t missingTargets = 0;
    uint32_t requiresInsertOps = 0;
    uint32_t rebaseOps = 0;
    uint32_t replaceExistingOps = 0;
    uint32_t insertNodeOps = 0;
    uint32_t insertBackingOps = 0;
    uint32_t backingVectorOps = 0;
    uint32_t shapeMismatchOps = 0;
    uint32_t materializerInputOps = 0;
    std::vector<StoreRangeRewriteOp> ops;
};

struct StoreRangeImportStep
{
    uint32_t sourceRangeIndex = 0;
    uint32_t sourceGroupKind = 0;
    uint32_t sourceRangeKind = 0;
    uint32_t sourceEntityId = 0;
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t targetNodeIndex = 0;
    uint32_t plannedTargetNodeIndex = 0;
    uint32_t sourceCursor = 0;
    uint32_t sourceCount = 0;
    uint32_t byteSize = 0;
    bool targetExists = false;
    bool insert = false;
    bool insertNodeAllocated = false;
    bool nodeRecordMaterialization = false;
    bool readPoolAllocation = false;
    bool writeVectorTranscode = false;
    bool materializerInputOnly = false;
    bool attrVector = false;
    bool childVector = false;
    bool cursorRebase = false;
};

struct NativeFragmentRebuildPlan
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    uint32_t replaceNodeOps = 0;
    uint32_t insertNodeOps = 0;
    uint32_t attrVectorImports = 0;
    uint32_t childVectorImports = 0;
    uint32_t cursorRemaps = 0;
    uint32_t rawBytes = 0;
    uint32_t targetExistingNodes = 0;
    uint32_t targetPlannedNodeBase = 0;
    uint32_t targetPlannedNodeEnd = 0;
    uint32_t assignedInsertedNodes = 0;
    uint32_t unresolvedInsertBackings = 0;
    uint32_t nodeRecordMaterializations = 0;
    uint32_t readPoolAllocations = 0;
    uint32_t writeVectorTranscodes = 0;
    uint32_t materializerRequired = 0;
    uint32_t allocatorRequired = 0;
    uint32_t blockedOps = 0;
    std::vector<StoreRangeImportStep> steps;
};

TargetInventoryFragmentBundle BuildTargetBundle(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& bundle);
NativeFragmentImportPlan BuildReadOnlyImportPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const TargetInventoryFragmentBundle& target);
NativeFragmentRebuildPlan BuildReadOnlyRebuildPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const TargetInventoryFragmentBundle& target,
    const NativeFragmentImportPlan& importPlan);

std::string BuildTargetStatus(const TargetInventoryFragmentBundle& target);
std::string BuildImportPlanStatus(const NativeFragmentImportPlan& plan);
std::string BuildRebuildPlanStatus(const NativeFragmentRebuildPlan& plan);
std::string BuildPatchStatus(
    const NativeFragmentImportPlan& plan,
    bool mutationEnabled,
    bool selfReplaceEnabled);
}
